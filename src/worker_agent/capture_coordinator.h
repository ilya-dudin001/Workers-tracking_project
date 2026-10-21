#pragma once

#include "worker_agent/screen_capture.h"

#include <chrono>
#include <memory>

namespace monitor::worker_agent {

class CaptureCoordinator {
public:
    CaptureCoordinator(ICaptureFactory& factory, std::chrono::milliseconds dxgi_retry_delay);

    CaptureResult capture(std::chrono::milliseconds now,
                          std::chrono::milliseconds timeout);

private:
    enum class Backend {
        None,
        Dxgi,
        Gdi,
    };

    void try_dxgi(std::chrono::milliseconds now);
    bool ensure_gdi();

    ICaptureFactory& factory_;
    std::chrono::milliseconds retry_delay_;
    std::chrono::milliseconds next_dxgi_attempt_{};
    std::unique_ptr<IScreenCapture> source_;
    Backend backend_{Backend::None};
};

}
