#include "worker_agent/change_detector.h"

#include <cstddef>

namespace monitor::worker_agent {

bool ChangeDetector::has_changed(const BgraFrame& frame) {
    if (!frame.is_valid()) {
        return false;
    }

    // Hash visible BGRA bytes only so row padding cannot create false visual changes.
    std::uint64_t hash = 14'695'981'039'346'656'037ULL;
    const auto row_bytes = static_cast<std::size_t>(frame.width) * 4U;
    for (std::uint32_t row = 0; row < frame.height; ++row) {
        const auto row_offset = static_cast<std::size_t>(row) * frame.stride;
        for (std::size_t column = 0; column < row_bytes; ++column) {
            hash ^= frame.pixels[row_offset + column];
            hash *= 1'099'511'628'211ULL;
        }
    }

    const Signature current{frame.width, frame.height, frame.stride, hash};
    const bool changed = !previous_.has_value() || *previous_ != current;
    previous_ = current;
    return changed;
}

void ChangeDetector::reset() noexcept {
    previous_.reset();
}

}
