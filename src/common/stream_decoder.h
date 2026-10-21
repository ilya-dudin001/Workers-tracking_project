#pragma once

#include "common/protocol.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace monitor::protocol {

class StreamDecoder {
public:
    [[nodiscard]] std::vector<Message> feed(std::span<const std::uint8_t> chunk);
    [[nodiscard]] std::size_t buffered_size() const noexcept;
    [[nodiscard]] bool poisoned() const noexcept;

private:
    Bytes buffer_;
    bool poisoned_ = false;
};

}
