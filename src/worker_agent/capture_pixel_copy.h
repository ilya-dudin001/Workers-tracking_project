#pragma once

#include <cstddef>
#include <cstdint>

namespace monitor::worker_agent {

void copy_bgra_rows(const std::uint8_t* source,
                    std::size_t source_stride,
                    std::uint8_t* destination,
                    std::size_t destination_stride,
                    std::uint32_t height);

[[nodiscard]] bool has_desktop_image_update(std::int64_t last_present_time) noexcept;

[[nodiscard]] bool requires_staging_recreation(std::uint32_t current_width,
                                               std::uint32_t current_height,
                                               std::uint32_t frame_width,
                                               std::uint32_t frame_height) noexcept;

}
