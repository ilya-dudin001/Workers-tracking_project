#pragma once

#include "common/protocol.h"

#include <mutex>
#include <optional>

namespace monitor::observer {

class ProfileSink {
public:
    virtual ~ProfileSink() = default;
    virtual bool send_profile(const protocol::AgentGuid& guid,
                              const protocol::SetProfile& profile) = 0;
};

class FocusRouter {
public:
    explicit FocusRouter(ProfileSink& sink) : sink_(sink) {}
    void agent_registered(const protocol::AgentGuid& guid);
    void select(const protocol::AgentGuid& guid);
    void clear();
    [[nodiscard]] std::optional<protocol::AgentGuid> selection() const;
    [[nodiscard]] static protocol::SetProfile grid_profile() noexcept;
    [[nodiscard]] static protocol::SetProfile focus_profile() noexcept;

private:
    ProfileSink& sink_;
    mutable std::mutex mutex_;
    std::optional<protocol::AgentGuid> selection_;
};

}
