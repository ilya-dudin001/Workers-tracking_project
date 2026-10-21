#include "observer/agent_registry.h"
#include "observer/frame_cache.h"
#include "observer/focus_router.h"
#include "observer/jpeg_decoder.h"
#include "observer/layout.h"
#include "observer/name_store.h"
#include "observer/render_state.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace {

using namespace std::chrono_literals;
using monitor::observer::AgentRegistry;
using monitor::observer::AgentStatus;
using monitor::observer::FocusRouter;
using monitor::observer::LayoutEngine;
using monitor::protocol::AgentGuid;
using monitor::protocol::Frame;
using monitor::protocol::SetProfile;

AgentGuid guid(const std::uint8_t value) {
    AgentGuid result{};
    result.fill(value);
    return result;
}

TEST(AgentRegistry, GenerationProtectsReplacementAndKeepsLatestFrame) {
    AgentRegistry registry;
    const auto now = AgentRegistry::Clock::time_point{};
    const auto first = registry.register_agent(guid(1), "Ada", "desk-a", now);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(registry.update_frame(guid(1), *first, Frame{1, 10, 10, {1}}, now + 100ms));
    ASSERT_TRUE(registry.update_frame(guid(1), *first, Frame{2, 10, 10, {2}}, now + 200ms));
    const auto latest = registry.snapshot(now + 200ms);
    ASSERT_TRUE(latest[0].frame);
    EXPECT_EQ(latest[0].frame->sequence, 2U);

    const auto replacement = registry.register_agent(guid(1), "Grace", "desk-b", now + 300ms);
    ASSERT_TRUE(replacement.has_value());
    EXPECT_NE(*first, *replacement);
    registry.disconnect(guid(1), *first, now + 400ms);

    const auto snapshot = registry.snapshot(now + 400ms);
    ASSERT_EQ(snapshot.size(), 1U);
    EXPECT_EQ(snapshot[0].employee_name, "Grace");
    EXPECT_EQ(snapshot[0].status, AgentStatus::Online);
    EXPECT_FALSE(snapshot[0].frame);
}

TEST(AgentRegistry, EnforcesCapAndComputesStatusAndFps) {
    AgentRegistry registry;
    const auto now = AgentRegistry::Clock::time_point{};
    for (std::uint8_t index = 0; index < 40; ++index) {
        ASSERT_TRUE(registry.register_agent(guid(index), "worker", "host", now));
    }
    EXPECT_FALSE(registry.register_agent(guid(41), "overflow", "host", now));

    AgentRegistry one;
    const auto generation = one.register_agent(guid(7), "worker", "host", now);
    ASSERT_TRUE(generation);
    for (std::uint64_t sequence = 1; sequence <= 5; ++sequence) {
        ASSERT_TRUE(one.update_frame(guid(7), *generation,
            Frame{sequence, 16, 9, {1}}, now + std::chrono::milliseconds(sequence * 250)));
    }
    auto snapshot = one.snapshot(now + 1500ms);
    ASSERT_EQ(snapshot.size(), 1U);
    EXPECT_NEAR(snapshot[0].fps, 4.0, 0.01);
    EXPECT_EQ(snapshot[0].status, AgentStatus::Online);
    snapshot = one.snapshot(now + 6s);
    EXPECT_EQ(snapshot[0].status, AgentStatus::Stale);
    one.disconnect(guid(7), *generation, now + 7s);
    EXPECT_EQ(one.snapshot(now + 7s)[0].status, AgentStatus::Offline);
}

TEST(LayoutEngine, IsDeterministicNonOverlappingAndHitTestable) {
    EXPECT_TRUE(LayoutEngine::compute(0, {0, 0, 1920, 1080}).empty());
    for (const std::size_t count : {1U, 32U, 40U}) {
        const auto tiles = LayoutEngine::compute(count, {0, 0, 1920, 1080});
        ASSERT_EQ(tiles.size(), count);
        for (std::size_t left = 0; left < tiles.size(); ++left) {
            EXPECT_EQ(LayoutEngine::hit_test(tiles, tiles[left].bounds.left + 1,
                                             tiles[left].bounds.top + 1), left);
            for (std::size_t right = left + 1; right < tiles.size(); ++right) {
                EXPECT_FALSE(tiles[left].bounds.intersects(tiles[right].bounds));
            }
        }
    }
    const auto thirty_two = LayoutEngine::compute(32, {0, 0, 1920, 1080});
    EXPECT_EQ(thirty_two.back().row, 3U);
    EXPECT_EQ(thirty_two.back().column, 7U);
    EXPECT_EQ(LayoutEngine::compute(40, {0, 0, 7, 5}).size(), 40U);
    EXPECT_NE(LayoutEngine::compute(1, {0, 0, 100, 100})[0].bounds.right,
              LayoutEngine::compute(1, {0, 0, 200, 100})[0].bounds.right);
}

TEST(LayoutEngine, UsesDeviceIndependentCoordinatesAtCommonDpis) {
    const auto at_96 = monitor::observer::DpiMetrics::from_dpi(96);
    const auto at_144 = monitor::observer::DpiMetrics::from_dpi(144);
    const auto at_192 = monitor::observer::DpiMetrics::from_dpi(192);
    EXPECT_EQ(at_96.pixels_to_dips({0, 0, 1920, 1080}), (monitor::observer::Rect{0, 0, 1920, 1080}));
    EXPECT_EQ(at_144.pixels_to_dips({0, 0, 2880, 1620}), (monitor::observer::Rect{0, 0, 1920, 1080}));
    EXPECT_EQ(at_192.pixels_to_dips({0, 0, 3840, 2160}), (monitor::observer::Rect{0, 0, 1920, 1080}));
    EXPECT_EQ(at_144.pixel_point_to_dips(150, 300), std::make_pair(100, 200));
}

class FakeProfileSink final : public monitor::observer::ProfileSink {
public:
    bool send_profile(const AgentGuid& id, const SetProfile& profile) override {
        sent.emplace_back(id, profile);
        return connected;
    }
    bool connected = true;
    std::vector<std::pair<AgentGuid, SetProfile>> sent;
};

TEST(FocusRouter, SendsGridFocusTransitionsAndHandlesDisconnectedSelection) {
    FakeProfileSink sink;
    FocusRouter router(sink);
    router.agent_registered(guid(1));
    ASSERT_EQ(sink.sent.size(), 1U);
    EXPECT_EQ(sink.sent.back().second, FocusRouter::grid_profile());

    router.select(guid(1));
    EXPECT_EQ(sink.sent.back().second, FocusRouter::focus_profile());
    router.select(guid(2));
    ASSERT_EQ(sink.sent.size(), 4U);
    EXPECT_EQ(sink.sent[2], std::make_pair(guid(1), FocusRouter::grid_profile()));
    EXPECT_EQ(sink.sent[3], std::make_pair(guid(2), FocusRouter::focus_profile()));
    sink.connected = false;
    EXPECT_NO_THROW(router.clear());
    EXPECT_FALSE(router.selection().has_value());
}

TEST(NameStore, RoundTripsAtomicallyAndIgnoresCorruptOrOversizedRecords) {
    const auto root = std::filesystem::temp_directory_path() / "workers-observer-name-test";
    std::filesystem::remove_all(root);
    monitor::observer::NameStore store(root);
    EXPECT_TRUE(store.save(guid(3), "Renamed Worker"));
    EXPECT_TRUE(store.save(guid(3), "Replaced Name"));
    EXPECT_EQ(store.load()[guid(3)], "Replaced Name");
    EXPECT_FALSE(store.save(guid(4), ""));
    EXPECT_FALSE(store.save(guid(4), std::string(300, 'x')));

    const auto corrupt = root / "ffffffffffffffffffffffffffffffff.name";
    std::ofstream(corrupt, std::ios::binary) << "\xff\xfe";
    const auto huge = root / "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee.name";
    {
        std::ofstream output(huge, std::ios::binary);
        const std::string block(1024U * 1024U, 'x');
        output.write(block.data(), static_cast<std::streamsize>(block.size()));
    }
    const auto invalid_name = root / "not-a-guid.name";
    std::ofstream(invalid_name, std::ios::binary) << "ignored";
    const auto loaded = store.load();
    EXPECT_FALSE(loaded.contains(guid(255)));
    EXPECT_FALSE(loaded.contains(guid(238)));
    std::filesystem::remove_all(root);
}

TEST(FrameDecodeCache, CachesSuccessAndFailureUntilFrameIdentityChanges) {
    monitor::observer::FrameDecodeCache cache;
    const monitor::observer::FrameIdentity first{guid(1), 7, 10};
    EXPECT_TRUE(cache.should_decode(first));
    cache.record_attempt(first, false);
    EXPECT_FALSE(cache.should_decode(first));
    EXPECT_EQ(cache.outcome(first), monitor::observer::DecodeOutcome::Failed);
    const monitor::observer::FrameIdentity next_sequence{guid(1), 7, 11};
    EXPECT_TRUE(cache.should_decode(next_sequence));
    cache.record_attempt(next_sequence, true);
    EXPECT_FALSE(cache.should_decode(next_sequence));
    const monitor::observer::FrameIdentity next_generation{guid(1), 8, 11};
    EXPECT_TRUE(cache.should_decode(next_generation));
}

TEST(DeviceResourceState, DiscardsOnResizeOrEndDrawDeviceLoss) {
    monitor::observer::DeviceResourceState state;
    EXPECT_FALSE(state.ready());
    state.creation_completed(true);
    EXPECT_TRUE(state.ready());
    EXPECT_FALSE(state.apply(monitor::observer::RenderResult::Ok));
    EXPECT_TRUE(state.apply(monitor::observer::RenderResult::RecreateTarget));
    EXPECT_FALSE(state.ready());
    state.creation_completed(false);
    EXPECT_FALSE(state.ready());
    state.creation_completed(true);
    EXPECT_TRUE(state.apply(monitor::observer::RenderResult::Failed));
    EXPECT_FALSE(state.ready());
}

TEST(JpegDecoder, DecodesValidImageAndDropsMalformedOrOversizedMetadata) {
    const std::vector<std::uint8_t> one_pixel_jpeg{
        0xff,0xd8,0xff,0xdb,0x00,0x43,0x00,0x08,0x06,0x06,0x07,0x06,0x05,0x08,
        0x07,0x07,0x07,0x09,0x09,0x08,0x0a,0x0c,0x14,0x0d,0x0c,0x0b,0x0b,0x0c,
        0x19,0x12,0x13,0x0f,0x14,0x1d,0x1a,0x1f,0x1e,0x1d,0x1a,0x1c,0x1c,0x20,
        0x24,0x2e,0x27,0x20,0x22,0x2c,0x23,0x1c,0x1c,0x28,0x37,0x29,0x2c,0x30,
        0x31,0x34,0x34,0x34,0x1f,0x27,0x39,0x3d,0x38,0x32,0x3c,0x2e,0x33,0x34,
        0x32,0xff,0xc0,0x00,0x0b,0x08,0x00,0x01,0x00,0x01,0x01,0x01,0x11,0x00,
        0xff,0xc4,0x00,0x14,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xff,0xc4,0x00,0x14,0x10,0x01,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0xff,0xda,0x00,0x08,0x01,0x01,0x00,0x00,0x3f,0x00,0x00,0xff,0xd9};
    monitor::observer::JpegDecoder decoder;
    const auto decoded = decoder.decode(one_pixel_jpeg);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->width, 1U);
    EXPECT_EQ(decoded->height, 1U);
    EXPECT_EQ(decoded->pixels.size(), 4U);
    const std::vector<std::uint8_t> malformed{1, 2, 3};
    EXPECT_FALSE(decoder.decode(malformed).has_value());
    EXPECT_FALSE(decoder.accept_dimensions(100000, 100000));
}

}
