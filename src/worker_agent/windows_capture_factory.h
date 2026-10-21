#pragma once

#include "worker_agent/screen_capture.h"

namespace monitor::worker_agent {

class WindowsCaptureFactory final : public ICaptureFactory {
public:
    std::unique_ptr<IScreenCapture> create_dxgi() override;
    std::unique_ptr<IScreenCapture> create_gdi() override;
};

}
