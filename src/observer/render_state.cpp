#include "observer/render_state.h"

namespace monitor::observer {

BitmapCreateAction classify_bitmap_creation(const RenderResult result) noexcept {
    switch (result) {
        case RenderResult::Ok:
            return BitmapCreateAction::UseBitmap;
        case RenderResult::RecreateTarget:
            return BitmapCreateAction::DiscardResources;
        case RenderResult::Failed:
            return BitmapCreateAction::CacheFrameFailure;
    }
    return BitmapCreateAction::CacheFrameFailure;
}

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
