#include "observer/render_state.h"

namespace monitor::observer {

void DeviceResourceState::creation_completed(const bool succeeded) noexcept {
    ready_ = succeeded;
}

bool DeviceResourceState::apply(const RenderResult result) noexcept {
    if (result == RenderResult::Ok) {
        return false;
    }
    ready_ = false;
    return true;
}

bool DeviceResourceState::ready() const noexcept {
    return ready_;
}

}
