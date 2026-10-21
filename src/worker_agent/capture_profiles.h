#pragma once

#include <chrono>
#include <cstdint>

namespace monitor::worker_agent {

struct CaptureProfile {
    std::uint32_t max_width;
    std::uint32_t max_height;
    std::uint8_t jpeg_quality;
    std::chrono::milliseconds minimum_interval;
    std::chrono::milliseconds maximum_interval;
    bool operator==(const CaptureProfile&) const = default;
};

[[nodiscard]] CaptureProfile grid_profile() noexcept;
[[nodiscard]] CaptureProfile focus_profile() noexcept;

}
