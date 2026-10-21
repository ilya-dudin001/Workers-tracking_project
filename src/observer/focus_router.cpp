#include "observer/focus_router.h"

namespace monitor::observer {

protocol::SetProfile FocusRouter::grid_profile() noexcept {
    return {500, 55, 480, 270};
}

protocol::SetProfile FocusRouter::focus_profile() noexcept {
    return {250, 75, 1280, 720};
}

void FocusRouter::agent_registered(const protocol::AgentGuid& guid) {
    bool focused = false;
    {
        std::scoped_lock lock(mutex_);
        focused = selection_ && *selection_ == guid;
    }
    static_cast<void>(sink_.send_profile(
        guid, focused ? focus_profile() : grid_profile()));
}

void FocusRouter::select(const protocol::AgentGuid& guid) {
    std::optional<protocol::AgentGuid> previous;
    {
        std::scoped_lock lock(mutex_);
        if (selection_ && *selection_ == guid) {
            return;
        }
        previous = selection_;
        selection_ = guid;
    }
    if (previous) {
        static_cast<void>(sink_.send_profile(*previous, grid_profile()));
    }
    static_cast<void>(sink_.send_profile(guid, focus_profile()));
}

void FocusRouter::clear() {
    std::optional<protocol::AgentGuid> previous;
    {
        std::scoped_lock lock(mutex_);
        previous = selection_;
        selection_.reset();
    }
    if (previous) {
        static_cast<void>(sink_.send_profile(*previous, grid_profile()));
    }
}

std::optional<protocol::AgentGuid> FocusRouter::selection() const {
    std::scoped_lock lock(mutex_);
    return selection_;
}

}
