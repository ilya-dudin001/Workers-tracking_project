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

bool has_desktop_image_update(const std::int64_t last_present_time) noexcept {
    // Desktop Duplication may acquire a frame for pointer movement only.
    return last_present_time != 0;
}

bool requires_staging_recreation(const std::uint32_t current_width,
                                 const std::uint32_t current_height,
                                 const std::uint32_t frame_width,
                                 const std::uint32_t frame_height) noexcept {
    return current_width != frame_width || current_height != frame_height;
}

}
