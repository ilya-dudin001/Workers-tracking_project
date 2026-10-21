#pragma once

#include "common/protocol.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace monitor::observer {

enum class AgentStatus { Online, Stale, Offline };

struct AgentSnapshot {
    protocol::AgentGuid guid{};
    std::string employee_name;
    std::string hostname;
    AgentStatus status = AgentStatus::Offline;
    double fps = 0.0;
    std::uint64_t generation = 0;
    std::shared_ptr<const protocol::Frame> frame;
};

class AgentRegistry {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::size_t kMaxAgents = 40;

    [[nodiscard]] std::optional<std::uint64_t> register_agent(
        const protocol::AgentGuid& guid, std::string employee_name,
        std::string hostname, Clock::time_point now);
    [[nodiscard]] bool update_frame(const protocol::AgentGuid& guid,
        std::uint64_t generation, protocol::Frame frame, Clock::time_point now);
    [[nodiscard]] bool heartbeat(const protocol::AgentGuid& guid,
        std::uint64_t generation, Clock::time_point now);
    void disconnect(const protocol::AgentGuid& guid, std::uint64_t generation,
                    Clock::time_point now);
    void set_local_name(const protocol::AgentGuid& guid, std::string name);
    [[nodiscard]] std::vector<AgentSnapshot> snapshot(Clock::time_point now) const;

private:
    struct Entry {
        std::string source_name;
        std::string local_name;
        std::string hostname;
        std::uint64_t generation = 0;
        bool connected = false;
        Clock::time_point last_activity{};
        std::shared_ptr<const protocol::Frame> frame;
        std::deque<Clock::time_point> frame_times;
    };

    mutable std::mutex mutex_;
    std::map<protocol::AgentGuid, Entry> entries_;
    std::map<protocol::AgentGuid, std::string> local_names_;
    std::uint64_t next_generation_ = 1;
};

}
