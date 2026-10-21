#include "worker_agent/transport_schedulers.h"

#include <algorithm>

namespace monitor::worker_agent {

std::chrono::milliseconds ReconnectScheduler::next_delay() noexcept {
    const auto result = next_;
    // Clamp before doubling so the backoff cannot overflow.
    next_ = std::min(next_ * 2, std::chrono::milliseconds{1500});
    return result;
}

void ReconnectScheduler::reset() noexcept {
    next_ = std::chrono::milliseconds{250};
}

void HeartbeatScheduler::reset(const SteadyTime now) noexcept {
    next_ = now + kInterval;
}

bool HeartbeatScheduler::due(const SteadyTime now) const noexcept {
    return now >= next_;
}

void HeartbeatScheduler::mark_sent(const SteadyTime now) noexcept {
    next_ = now + kInterval;
}

SteadyTime HeartbeatScheduler::next_deadline() const noexcept {
    return next_;
}

}
