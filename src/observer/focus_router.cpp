#include "observer/focus_router.h"

namespace monitor::observer {

protocol::SetProfile FocusRouter::grid_profile() noexcept {
    return {500, 55, 480, 270};
}

protocol::SetProfile FocusRouter::focus_profile() noexcept {
    return {250, 75, 1280, 720};
}

void FocusRouter::agent_registered(const protocol::AgentGuid& guid) {
    static_cast<void>(sink_.send_profile(
        guid, selection_ && *selection_ == guid ? focus_profile() : grid_profile()));
}

void FocusRouter::select(const protocol::AgentGuid& guid) {
    if (selection_ && *selection_ == guid) {
        return;
    }
    if (selection_) {
        static_cast<void>(sink_.send_profile(*selection_, grid_profile()));
    }
    selection_ = guid;
    static_cast<void>(sink_.send_profile(guid, focus_profile()));
}

void FocusRouter::clear() {
    if (selection_) {
        static_cast<void>(sink_.send_profile(*selection_, grid_profile()));
    }
    selection_.reset();
}

std::optional<protocol::AgentGuid> FocusRouter::selection() const {
    return selection_;
}

}
