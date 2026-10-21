#include "worker_agent/agent_orchestrator.h"

#include <chrono>
#include <cstdint>
#include <utility>

namespace monitor::worker_agent {

AgentOrchestrator::AgentOrchestrator(AgentTransport& transport,
                                     const SteadyTime start,
                                     ProfileCallback profile_callback,
                                     Clock clock)
    : transport_(transport),
      profile_callback_(std::move(profile_callback)),
      clock_(std::move(clock)),
      reconnect_deadline_(start),
      capture_deadline_(start + rate_controller_.current_profile().minimum_interval) {
    heartbeat_scheduler_.reset(start);
}

void AgentOrchestrator::submit_frame(protocol::Frame frame) {
    frame_replaced_ = pending_frame_.has_value();
    pending_frame_ = std::move(frame);
}

void AgentOrchestrator::tick(const SteadyTime now) {
    if (transport_.state() == AgentConnectionState::Disconnected) {
        if (now < reconnect_deadline_) {
            return;
        }
        if (!transport_.connect()) {
            schedule_reconnect(now);
            return;
        }
        reconnect_scheduler_.reset();
        heartbeat_scheduler_.reset(now);
    }

    for (const protocol::SetProfile& command : transport_.receive_commands()) {
        rate_controller_.apply(command);
        capture_deadline_ = now + rate_controller_.current_profile().minimum_interval;
        if (profile_callback_) {
            profile_callback_(rate_controller_.current_profile());
        }
    }
    if (transport_.state() != AgentConnectionState::Online) {
        schedule_reconnect(now);
        return;
    }

    // Control traffic is emitted before the disposable frame handoff.
    if (heartbeat_scheduler_.due(now)) {
        const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch());
        if (!transport_.send_heartbeat(
                protocol::Heartbeat{static_cast<std::uint64_t>(timestamp.count())})) {
            schedule_reconnect(now);
            return;
        }
        heartbeat_scheduler_.mark_sent(now);
    }

    if (pending_frame_) {
        const CaptureProfile previous_profile = rate_controller_.current_profile();
        const SteadyTime send_start = clock_();
        if (!transport_.send_frame(*pending_frame_)) {
            rate_controller_.observe(
                {rate_controller_.current_profile().minimum_interval,
                 frame_replaced_,
                 true});
            if (rate_controller_.current_profile() != previous_profile &&
                profile_callback_) {
                profile_callback_(rate_controller_.current_profile());
            }
            schedule_reconnect(now);
            return;
        }
        const auto send_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            clock_() - send_start);
        rate_controller_.observe({send_duration, frame_replaced_, false});
        if (rate_controller_.current_profile() != previous_profile &&
            profile_callback_) {
            profile_callback_(rate_controller_.current_profile());
        }
        pending_frame_.reset();
        frame_replaced_ = false;
    }

    if (now >= capture_deadline_) {
        capture_deadline_ = now + rate_controller_.current_profile().minimum_interval;
    }
}

const CaptureProfile& AgentOrchestrator::current_profile() const noexcept {
    return rate_controller_.current_profile();
}

SteadyTime AgentOrchestrator::next_reconnect_deadline() const noexcept {
    return reconnect_deadline_;
}

SteadyTime AgentOrchestrator::next_capture_deadline() const noexcept {
    return capture_deadline_;
}

SteadyTime AgentOrchestrator::next_heartbeat_deadline() const noexcept {
    return heartbeat_scheduler_.next_deadline();
}

AgentConnectionState AgentOrchestrator::state() const noexcept {
    return transport_.state();
}

std::string AgentOrchestrator::last_error() const {
    return transport_.last_error();
}

void AgentOrchestrator::schedule_reconnect(const SteadyTime now) noexcept {
    transport_.disconnect();
    reconnect_deadline_ = now + reconnect_scheduler_.next_delay();
}

}
