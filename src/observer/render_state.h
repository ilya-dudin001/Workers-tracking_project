#pragma once

namespace monitor::observer {

enum class RenderResult { Ok, RecreateTarget, Failed };
enum class BitmapCreateAction { UseBitmap, DiscardResources, CacheFrameFailure };

[[nodiscard]] BitmapCreateAction classify_bitmap_creation(
    RenderResult result) noexcept;

class DeviceResourceState {
public:
    void creation_completed(bool succeeded) noexcept;
    [[nodiscard]] bool apply(RenderResult result) noexcept;
    [[nodiscard]] bool ready() const noexcept;

private:
    bool ready_ = false;
};

}
