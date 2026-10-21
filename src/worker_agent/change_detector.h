#pragma once

#include "worker_agent/frame.h"

#include <cstdint>
#include <optional>

namespace monitor::worker_agent {

class ChangeDetector {
public:
    [[nodiscard]] bool has_changed(const BgraFrame& frame);
    void reset() noexcept;

private:
    struct Signature {
        std::uint32_t width;
        std::uint32_t height;
        std::uint32_t stride;
        std::uint64_t hash;

        bool operator==(const Signature&) const = default;
    };

    std::optional<Signature> previous_;
};

}
