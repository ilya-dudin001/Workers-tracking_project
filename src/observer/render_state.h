#pragma once

namespace monitor::observer {

enum class RenderResult { Ok, RecreateTarget, Failed };

class DeviceResourceState {
public:
    void creation_completed(bool succeeded) noexcept;
    [[nodiscard]] bool apply(RenderResult result) noexcept;
    [[nodiscard]] bool ready() const noexcept;

private:
    bool ready_ = false;
};

}
