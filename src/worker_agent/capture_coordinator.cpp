#include "worker_agent/capture_coordinator.h"

#include <optional>
#include <stdexcept>

namespace monitor::worker_agent {
namespace {

std::chrono::milliseconds retry_deadline(const std::chrono::milliseconds now,
                                         const std::chrono::milliseconds delay) noexcept {
    const auto maximum = std::chrono::milliseconds::max();
    return now > maximum - delay ? maximum : now + delay;
}

}

CaptureCoordinator::CaptureCoordinator(ICaptureFactory& factory,
                                       const std::chrono::milliseconds dxgi_retry_delay)
    : factory_(factory), retry_delay_(dxgi_retry_delay) {
    if (retry_delay_ <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("DXGI retry delay must be positive");
    }
}

CaptureResult CaptureCoordinator::capture(const std::chrono::milliseconds now,
                                          const std::chrono::milliseconds timeout) {
    const bool retry_disabled =
        next_dxgi_attempt_ == std::chrono::milliseconds::max();
    if (backend_ == Backend::None &&
        (retry_disabled || now < next_dxgi_attempt_)) {
        return {CaptureStatus::Unavailable, std::nullopt};
    }
    if (!retry_disabled &&
        ((backend_ == Backend::None && now >= next_dxgi_attempt_) ||
         (backend_ == Backend::Gdi && now >= next_dxgi_attempt_))) {
        try_dxgi(now);
    }
    if (backend_ == Backend::None && !ensure_gdi()) {
        next_dxgi_attempt_ = retry_deadline(now, retry_delay_);
        return {CaptureStatus::Unavailable, std::nullopt};
    }

    auto result = source_->capture(timeout);
    if (backend_ == Backend::Dxgi &&
        (result.status == CaptureStatus::AccessLost ||
         result.status == CaptureStatus::Unavailable ||
         result.status == CaptureStatus::Fatal)) {
        source_.reset();
        backend_ = Backend::None;
        next_dxgi_attempt_ = retry_deadline(now, retry_delay_);
        if (!ensure_gdi()) {
            return result;
        }
        return source_->capture(timeout);
    }
    return result;
}

void CaptureCoordinator::try_dxgi(const std::chrono::milliseconds now) {
    auto dxgi = factory_.create_dxgi();
    if (dxgi) {
        source_ = std::move(dxgi);
        backend_ = Backend::Dxgi;
        return;
    }
    next_dxgi_attempt_ = retry_deadline(now, retry_delay_);
}

bool CaptureCoordinator::ensure_gdi() {
    if (backend_ == Backend::Gdi) {
        return true;
    }
    auto gdi = factory_.create_gdi();
    if (!gdi) {
        return false;
    }
    source_ = std::move(gdi);
    backend_ = Backend::Gdi;
    return true;
}

}
