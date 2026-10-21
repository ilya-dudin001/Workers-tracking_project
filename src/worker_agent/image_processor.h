#pragma once

#include "worker_agent/frame.h"

#include <cstdint>
#include <vector>

namespace monitor::worker_agent {

inline constexpr std::uint32_t kMinimumJpegQuality = 1;
inline constexpr std::uint32_t kMaximumJpegQuality = 100;

struct EncodedImage {
    std::uint32_t width;
    std::uint32_t height;
    std::vector<std::uint8_t> jpeg;
};

class WicImageProcessor {
public:
    [[nodiscard]] EncodedImage encode_jpeg(const BgraFrame& frame,
                                           std::uint32_t max_width,
                                           std::uint32_t max_height,
                                           std::uint32_t quality) const;
};

}
