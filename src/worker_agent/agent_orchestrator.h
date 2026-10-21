#pragma once

#include "worker_agent/adaptive_rate_controller.h"
#include "worker_agent/agent_session.h"
#include "worker_agent/transport_schedulers.h"

#include <functional>
#include <optional>

namespace monitor::worker_agent {

class AgentOrchestrator {
public:
    using ProfileCallback = std::function<void(const CaptureProfile&)>;
    using Clock = std::function<SteadyTime()>;

    AgentOrchestrator(AgentTransport& transport,
                      SteadyTime start,
                      ProfileCallback profile_callback,
                      Clock clock = std::chrono::steady_clock::now);

    // Replacement is intentional: encoded frames are disposable telemetry, not a backlog.
    void submit_frame(protocol::Frame frame);
    void tick(SteadyTime now);

    [[nodiscard]] const CaptureProfile& current_profile() const noexcept;
    [[nodiscard]] SteadyTime next_reconnect_deadline() const noexcept;
    [[nodiscard]] SteadyTime next_capture_deadline() const noexcept;
    [[nodiscard]] SteadyTime next_heartbeat_deadline() const noexcept;
    [[nodiscard]] AgentConnectionState state() const noexcept;
    [[nodiscard]] std::string last_error() const;

private:
    void schedule_reconnect(SteadyTime now) noexcept;

    AgentTransport& transport_;
    ProfileCallback profile_callback_;
    Clock clock_;
    AdaptiveRateController rate_controller_;
    ReconnectScheduler reconnect_scheduler_;
    HeartbeatScheduler heartbeat_scheduler_;
    std::optional<protocol::Frame> pending_frame_;
    SteadyTime reconnect_deadline_;
    SteadyTime capture_deadline_;
    bool frame_replaced_ = false;
};

}
