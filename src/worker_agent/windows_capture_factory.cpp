#include "worker_agent/windows_capture_factory.h"

#include "worker_agent/dxgi_screen_capture.h"
#include "worker_agent/gdi_screen_capture.h"

#include <memory>
#include <stdexcept>

namespace monitor::worker_agent {

std::unique_ptr<IScreenCapture> WindowsCaptureFactory::create_dxgi() {
    try {
        return std::make_unique<DxgiScreenCapture>();
    } catch (const std::runtime_error&) {
        return nullptr;
    }
}

std::unique_ptr<IScreenCapture> WindowsCaptureFactory::create_gdi() {
    return std::make_unique<GdiScreenCapture>();
}

}
