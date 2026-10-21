#include "worker_agent/adaptive_rate_controller.h"

#include <algorithm>
#include <cstdint>

namespace monitor::worker_agent {
namespace {

constexpr std::uint32_t kMaximumWidth = 1280;
constexpr std::uint32_t kMaximumHeight = 720;
constexpr std::uint8_t kMinimumQuality = 45;
constexpr std::uint8_t kGridMaximumQuality = 65;
constexpr std::uint8_t kMaximumQuality = 75;
constexpr std::chrono::milliseconds kMinimumInterval{250};
constexpr std::chrono::milliseconds kMaximumInterval{500};
constexpr unsigned kHealthySamplesForRecovery = 4;

}

AdaptiveRateController::AdaptiveRateController() noexcept : profile_(grid_profile()) {}

void AdaptiveRateController::use_grid() noexcept {
    profile_ = grid_profile();
    adaptive_grid_ = true;
    healthy_samples_ = 0;
}

void AdaptiveRateController::use_focus() noexcept {
    profile_ = focus_profile();
    adaptive_grid_ = false;
    healthy_samples_ = 0;
}

void AdaptiveRateController::apply(const protocol::SetProfile& profile) noexcept {
    profile_.minimum_interval =
        std::chrono::milliseconds{std::clamp<std::uint32_t>(
            profile.frame_interval_ms,
            static_cast<std::uint32_t>(kMinimumInterval.count()),
            static_cast<std::uint32_t>(kMaximumInterval.count()))};
    profile_.maximum_interval = profile_.minimum_interval;
    profile_.jpeg_quality = static_cast<std::uint8_t>(
        std::clamp<std::uint16_t>(profile.jpeg_quality,
                                  kMinimumQuality,
                                  kMaximumQuality));
    profile_.max_width =
        std::clamp<std::uint32_t>(profile.max_width, 1U, kMaximumWidth);
    profile_.max_height =
        std::clamp<std::uint32_t>(profile.max_height, 1U, kMaximumHeight);
    adaptive_grid_ = false;
    healthy_samples_ = 0;
}

void AdaptiveRateController::observe(const RateSample& sample) noexcept {
    if (!adaptive_grid_) {
        return;
    }

    const bool overloaded =
        sample.frame_replaced || sample.congested ||
        sample.send_duration >= profile_.minimum_interval * 3 / 4;
    if (overloaded) {
        healthy_samples_ = 0;
        profile_.minimum_interval =
            std::min(profile_.minimum_interval + std::chrono::milliseconds{50},
                     kMaximumInterval);
        profile_.maximum_interval = profile_.minimum_interval;
        const auto reduced = static_cast<int>(profile_.jpeg_quality) - 3;
        profile_.jpeg_quality = static_cast<std::uint8_t>(
            std::max(reduced, static_cast<int>(kMinimumQuality)));
        return;
    }

    ++healthy_samples_;
    if (healthy_samples_ < kHealthySamplesForRecovery) {
        return;
    }
    healthy_samples_ = 0;
    profile_.minimum_interval =
        std::max(profile_.minimum_interval - std::chrono::milliseconds{50},
                 kMinimumInterval);
    profile_.maximum_interval = profile_.minimum_interval;
    const auto improved = static_cast<unsigned>(profile_.jpeg_quality) + 2U;
    profile_.jpeg_quality = static_cast<std::uint8_t>(
        std::min(improved, static_cast<unsigned>(kGridMaximumQuality)));
}

const CaptureProfile& AdaptiveRateController::current_profile() const noexcept {
    return profile_;
}

}
