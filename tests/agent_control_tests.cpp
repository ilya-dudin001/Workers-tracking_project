#include "worker_agent/adaptive_rate_controller.h"
#include "worker_agent/agent_orchestrator.h"
#include "worker_agent/transport_schedulers.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using monitor::protocol::Frame;
using monitor::protocol::Heartbeat;
using monitor::protocol::SetProfile;
using monitor::worker_agent::AdaptiveRateController;
using monitor::worker_agent::AgentConnectionState;
using monitor::worker_agent::AgentOrchestrator;
using monitor::worker_agent::AgentTransport;
using monitor::worker_agent::HeartbeatScheduler;
using monitor::worker_agent::ReconnectScheduler;
using monitor::worker_agent::SteadyTime;

TEST(ReconnectScheduler, UsesExactCappedSequenceAndResets) {
    ReconnectScheduler scheduler;

    EXPECT_EQ(scheduler.next_delay(), 250ms);
    EXPECT_EQ(scheduler.next_delay(), 500ms);
    EXPECT_EQ(scheduler.next_delay(), 1000ms);
    EXPECT_EQ(scheduler.next_delay(), 1500ms);
    EXPECT_EQ(scheduler.next_delay(), 1500ms);

    scheduler.reset();
    EXPECT_EQ(scheduler.next_delay(), 250ms);
}

TEST(HeartbeatScheduler, UsesDeterministicTwoSecondDeadlines) {
    const SteadyTime start{};
    HeartbeatScheduler scheduler;
    scheduler.reset(start);

    EXPECT_EQ(scheduler.next_deadline(), start + 2s);
    EXPECT_FALSE(scheduler.due(start + 1999ms));
    EXPECT_TRUE(scheduler.due(start + 2s));

    scheduler.mark_sent(start + 2500ms);
    EXPECT_EQ(scheduler.next_deadline(), start + 4500ms);
}

TEST(AdaptiveRateController, OverloadBacksOffWithinGridBounds) {
    AdaptiveRateController controller;
    for (int sample = 0; sample < 20; ++sample) {
        controller.observe({220ms, true, true});
    }

    const auto profile = controller.current_profile();
    EXPECT_EQ(profile.minimum_interval, 500ms);
    EXPECT_EQ(profile.maximum_interval, 500ms);
    EXPECT_EQ(profile.jpeg_quality, 45U);
    EXPECT_LE(profile.max_width, 1280U);
    EXPECT_LE(profile.max_height, 720U);
}

TEST(AdaptiveRateController, RecoveryRequiresSustainedHealthySamples) {
    AdaptiveRateController controller;
    controller.observe({220ms, true, true});
    const auto overloaded = controller.current_profile();

    for (int sample = 0; sample < 3; ++sample) {
        controller.observe({20ms, false, false});
    }
    EXPECT_EQ(controller.current_profile(), overloaded);

    controller.observe({20ms, false, false});
    EXPECT_LT(controller.current_profile().minimum_interval, overloaded.minimum_interval);
}

TEST(AdaptiveRateController, GridNeverLeavesDocumentedBounds) {
    AdaptiveRateController controller;
    for (int sample = 0; sample < 100; ++sample) {
        controller.observe({0ms, false, false});
    }

    const auto profile = controller.current_profile();
    EXPECT_GE(profile.minimum_interval, 250ms);
    EXPECT_LE(profile.minimum_interval, 500ms);
    EXPECT_GE(profile.jpeg_quality, 45U);
    EXPECT_LE(profile.jpeg_quality, 65U);
}

TEST(AdaptiveRateController, FocusIsFixedUntilExplicitSafeProfile) {
    AdaptiveRateController controller;
    controller.use_focus();
    for (int sample = 0; sample < 20; ++sample) {
        controller.observe({900ms, true, true});
    }

    auto profile = controller.current_profile();
    EXPECT_EQ(profile.minimum_interval, 250ms);
    EXPECT_EQ(profile.max_width, 1280U);
    EXPECT_EQ(profile.max_height, 720U);
    EXPECT_EQ(profile.jpeg_quality, 75U);

    controller.apply(SetProfile{1, 100, 9000, 0});
    profile = controller.current_profile();
    EXPECT_EQ(profile.minimum_interval, 250ms);
    EXPECT_EQ(profile.jpeg_quality, 75U);
    EXPECT_EQ(profile.max_width, 1280U);
    EXPECT_EQ(profile.max_height, 1U);
}

class FakeTransport final : public AgentTransport {
public:
    bool connect() override {
        ++connect_calls;
        state_value = connect_result ? AgentConnectionState::Online
                                     : AgentConnectionState::Disconnected;
        return connect_result;
    }

    bool send_frame(const Frame& frame) override {
        sent_frames.push_back(frame.sequence);
        return send_result;
    }

    bool send_heartbeat(const Heartbeat& heartbeat) override {
        sent_heartbeats.push_back(heartbeat.timestamp_ms);
        return send_result;
    }

    std::vector<SetProfile> receive_commands() override {
        return std::exchange(commands, {});
    }

    void disconnect() noexcept override {
        state_value = AgentConnectionState::Disconnected;
    }

    [[nodiscard]] AgentConnectionState state() const noexcept override {
        return state_value;
    }

    [[nodiscard]] std::string last_error() const override {
        return {};
    }

    bool connect_result = true;
    bool send_result = true;
    int connect_calls = 0;
    AgentConnectionState state_value = AgentConnectionState::Disconnected;
    std::vector<std::uint64_t> sent_frames;
    std::vector<std::uint64_t> sent_heartbeats;
    std::vector<SetProfile> commands;
};

TEST(AgentOrchestrator, KeepsOnlyNewestFrameAndSendsControlIndependently) {
    FakeTransport transport;
    std::vector<monitor::worker_agent::CaptureProfile> applied;
    AgentOrchestrator orchestrator(
        transport, SteadyTime{}, [&applied](const auto& profile) {
            applied.push_back(profile);
        });

    orchestrator.submit_frame(Frame{1, 10, 10, {1}});
    orchestrator.submit_frame(Frame{2, 10, 10, {2}});
    orchestrator.tick(SteadyTime{});

    ASSERT_EQ(transport.sent_frames.size(), 1U);
    EXPECT_EQ(transport.sent_frames.front(), 2U);
    EXPECT_TRUE(transport.sent_heartbeats.empty());

    transport.commands.push_back(SetProfile{400, 55, 480, 270});
    orchestrator.tick(SteadyTime{} + 1ms);
    ASSERT_EQ(applied.size(), 1U);
    EXPECT_EQ(applied.front(), orchestrator.current_profile());
    EXPECT_EQ(orchestrator.current_profile().minimum_interval, 400ms);
}

TEST(AgentOrchestrator, ExposesReconnectCaptureAndHeartbeatDeadlines) {
    FakeTransport transport;
    transport.connect_result = false;
    const SteadyTime start{};
    AgentOrchestrator orchestrator(transport, start, {});

    orchestrator.tick(start);

    EXPECT_EQ(orchestrator.next_reconnect_deadline(), start + 250ms);
    EXPECT_EQ(orchestrator.next_capture_deadline(), start + 250ms);
    EXPECT_EQ(orchestrator.next_heartbeat_deadline(), start + 2s);
}

}
