#pragma once

#include "common/protocol.h"
#include "worker_agent/capture_profiles.h"

#include <chrono>

namespace monitor::worker_agent {

struct RateSample {
    std::chrono::milliseconds send_duration;
    bool frame_replaced;
    bool congested;
};

class AdaptiveRateController {
public:
    AdaptiveRateController() noexcept;

    void use_grid() noexcept;
    void use_focus() noexcept;
    void apply(const protocol::SetProfile& profile) noexcept;
    void observe(const RateSample& sample) noexcept;
    [[nodiscard]] const CaptureProfile& current_profile() const noexcept;

private:
    CaptureProfile profile_;
    bool adaptive_grid_ = true;
    unsigned healthy_samples_ = 0;
};

}
