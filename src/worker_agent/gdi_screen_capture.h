#pragma once

#include "worker_agent/screen_capture.h"

namespace monitor::worker_agent {

class GdiScreenCapture final : public IScreenCapture {
public:
    CaptureResult capture(std::chrono::milliseconds timeout) override;

private:
    std::uint64_t sequence_{};
};

}
