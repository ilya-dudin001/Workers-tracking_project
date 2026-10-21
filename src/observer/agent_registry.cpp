#include "observer/agent_registry.h"

#include <utility>

namespace monitor::observer {

std::optional<std::uint64_t> AgentRegistry::register_agent(
    const protocol::AgentGuid& guid, std::string employee_name,
    std::string hostname, const Clock::time_point now) {
    std::scoped_lock lock(mutex_);
    auto found = entries_.find(guid);
    if (found == entries_.end() && entries_.size() >= kMaxAgents) {
        return std::nullopt;
    }
    Entry& entry = entries_[guid];
    entry.source_name = std::move(employee_name);
    const auto local = local_names_.find(guid);
    entry.local_name = local == local_names_.end() ? std::string{} : local->second;
    entry.hostname = std::move(hostname);
    entry.generation = next_generation_++;
    entry.connected = true;
    entry.last_activity = now;
    entry.frame.reset();
    entry.frame_times.clear();
    return entry.generation;
}

bool AgentRegistry::update_frame(const protocol::AgentGuid& guid,
    const std::uint64_t generation, protocol::Frame frame,
    const Clock::time_point now) {
    std::scoped_lock lock(mutex_);
    const auto found = entries_.find(guid);
    if (found == entries_.end() || found->second.generation != generation ||
        !found->second.connected) {
        return false;
    }
    Entry& entry = found->second;
    entry.frame = std::make_shared<const protocol::Frame>(std::move(frame));
    entry.last_activity = now;
    entry.frame_times.push_back(now);
    while (entry.frame_times.size() > 16U) {
        entry.frame_times.pop_front();
    }
    return true;
}

bool AgentRegistry::heartbeat(const protocol::AgentGuid& guid,
    const std::uint64_t generation, const Clock::time_point now) {
    std::scoped_lock lock(mutex_);
    const auto found = entries_.find(guid);
    if (found == entries_.end() || found->second.generation != generation ||
        !found->second.connected) {
        return false;
    }
    found->second.last_activity = now;
    return true;
}

void AgentRegistry::disconnect(const protocol::AgentGuid& guid,
    const std::uint64_t generation, const Clock::time_point) {
    std::scoped_lock lock(mutex_);
    const auto found = entries_.find(guid);
    if (found != entries_.end() && found->second.generation == generation) {
        found->second.connected = false;
    }
}

void AgentRegistry::set_local_name(const protocol::AgentGuid& guid,
                                   std::string name) {
    std::scoped_lock lock(mutex_);
    local_names_[guid] = name;
    const auto found = entries_.find(guid);
    if (found != entries_.end()) {
        found->second.local_name = std::move(name);
    }
}

std::vector<AgentSnapshot> AgentRegistry::snapshot(
    const Clock::time_point now) const {
    std::scoped_lock lock(mutex_);
    std::vector<AgentSnapshot> result;
    result.reserve(entries_.size());
    for (const auto& [guid, entry] : entries_) {
        AgentStatus status = AgentStatus::Offline;
        if (entry.connected) {
            status = now - entry.last_activity <= std::chrono::seconds(3)
                ? AgentStatus::Online : AgentStatus::Stale;
        }
        double fps = 0.0;
        if (entry.frame_times.size() >= 2U) {
            const auto elapsed = entry.frame_times.back() - entry.frame_times.front();
            const double seconds =
                std::chrono::duration<double>(elapsed).count();
            if (seconds > 0.0) {
                fps = static_cast<double>(entry.frame_times.size() - 1U) / seconds;
            }
        }
        result.push_back(AgentSnapshot{
            guid,
            entry.local_name.empty() ? entry.source_name : entry.local_name,
            entry.hostname,
            status,
            fps,
            entry.generation,
            entry.frame});
    }
    return result;
}

}
