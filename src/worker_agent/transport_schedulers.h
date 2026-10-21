#pragma once

#include <chrono>

namespace monitor::worker_agent {

using SteadyTime = std::chrono::steady_clock::time_point;

class ReconnectScheduler {
public:
    [[nodiscard]] std::chrono::milliseconds next_delay() noexcept;
    void reset() noexcept;

private:
    std::chrono::milliseconds next_{250};
};

class HeartbeatScheduler {
public:
    static constexpr std::chrono::milliseconds kInterval{2000};

    void reset(SteadyTime now) noexcept;
    [[nodiscard]] bool due(SteadyTime now) const noexcept;
    void mark_sent(SteadyTime now) noexcept;
    [[nodiscard]] SteadyTime next_deadline() const noexcept;

private:
    SteadyTime next_{kInterval};
};

}
