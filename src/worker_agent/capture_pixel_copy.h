#pragma once

#include <cstddef>
#include <cstdint>

namespace monitor::worker_agent {

void copy_bgra_rows(const std::uint8_t* source,
                    std::size_t source_stride,
                    std::uint8_t* destination,
                    std::size_t destination_stride,
                    std::uint32_t height);

}
