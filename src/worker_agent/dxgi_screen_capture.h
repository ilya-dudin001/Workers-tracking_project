#pragma once

#include "worker_agent/screen_capture.h"

#include <memory>

namespace monitor::worker_agent {

class DxgiScreenCapture final : public IScreenCapture {
public:
    DxgiScreenCapture();
    ~DxgiScreenCapture() override;

    DxgiScreenCapture(const DxgiScreenCapture&) = delete;
    DxgiScreenCapture& operator=(const DxgiScreenCapture&) = delete;

    CaptureResult capture(std::chrono::milliseconds timeout) override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}
