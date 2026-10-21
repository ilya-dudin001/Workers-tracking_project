#include "worker_agent/frame.h"

#include <limits>

namespace monitor::worker_agent {

bool BgraFrame::is_valid() const noexcept {
    if (width == 0 || height == 0) {
        return false;
    }

    // Check each multiplication before using it so malformed capture metadata cannot wrap.
    if (width > std::numeric_limits<std::uint32_t>::max() / 4U) {
        return false;
    }
    const auto minimum_stride = width * 4U;
    if (stride < minimum_stride ||
        static_cast<std::size_t>(height) >
            std::numeric_limits<std::size_t>::max() / static_cast<std::size_t>(stride)) {
        return false;
    }
    return pixels.size() == static_cast<std::size_t>(stride) * height;
}

}
