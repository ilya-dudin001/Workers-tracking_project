#pragma once

#include "worker_agent/frame.h"

#include <chrono>
#include <memory>
#include <optional>

namespace monitor::worker_agent {

enum class CaptureStatus {
    Frame,
    Timeout,
    AccessLost,
    Unavailable,
    Fatal,
};

struct CaptureResult {
    CaptureStatus status;
    std::optional<BgraFrame> frame;
};

class IScreenCapture {
public:
    virtual ~IScreenCapture() = default;
    virtual CaptureResult capture(std::chrono::milliseconds timeout) = 0;
};

class ICaptureFactory {
public:
    virtual ~ICaptureFactory() = default;
    virtual std::unique_ptr<IScreenCapture> create_dxgi() = 0;
    virtual std::unique_ptr<IScreenCapture> create_gdi() = 0;
};

}
