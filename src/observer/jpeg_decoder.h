#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace monitor::observer {

struct DecodedImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels;
};

class JpegDecoder {
public:
    [[nodiscard]] bool accept_dimensions(std::uint32_t width,
                                         std::uint32_t height) const noexcept;
    [[nodiscard]] std::optional<DecodedImage> decode(
        std::span<const std::uint8_t> jpeg) const noexcept;
};

}
