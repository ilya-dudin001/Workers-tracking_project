#include "worker_agent/capture_pixel_copy.h"

#include <cstring>
#include <stdexcept>

namespace monitor::worker_agent {

void copy_bgra_rows(const std::uint8_t* const source,
                    const std::size_t source_stride,
                    std::uint8_t* const destination,
                    const std::size_t destination_stride,
                    const std::uint32_t height) {
    if (source == nullptr || destination == nullptr || source_stride < destination_stride) {
        throw std::invalid_argument("valid BGRA row buffers required");
    }
    for (std::uint32_t row = 0; row < height; ++row) {
        std::memcpy(destination + static_cast<std::size_t>(row) * destination_stride,
                    source + static_cast<std::size_t>(row) * source_stride,
                    destination_stride);
    }
}

}
