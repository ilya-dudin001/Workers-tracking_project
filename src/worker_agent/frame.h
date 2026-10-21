#pragma once

#include <cstdint>
#include <vector>

namespace monitor::worker_agent {

struct BgraFrame {
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t stride{};
    std::vector<std::uint8_t> pixels;
    std::uint64_t sequence{};
    std::uint64_t captured_at_ms{};

    [[nodiscard]] bool is_valid() const noexcept;
};

}
