#include "common/stream_decoder.h"

#include <cstddef>

namespace monitor::protocol {

std::vector<Message> StreamDecoder::feed(const std::span<const std::uint8_t> chunk) {
    if (poisoned_) {
        throw ProtocolError("stream decoder is poisoned");
    }

    buffer_.insert(buffer_.end(), chunk.begin(), chunk.end());
    std::vector<Message> messages;
    try {
        while (buffer_.size() >= kHeaderSize) {
            const PacketHeader header =
                decode_header(std::span<const std::uint8_t>(buffer_).first(kHeaderSize));
            const std::size_t packet_size = kHeaderSize + header.payload_size;
            if (buffer_.size() < packet_size) {
                break;
            }
            messages.push_back(
                decode_packet(std::span<const std::uint8_t>(buffer_).first(packet_size)));
            buffer_.erase(buffer_.begin(),
                          buffer_.begin() + static_cast<std::ptrdiff_t>(packet_size));
        }
    } catch (const ProtocolError&) {
        poisoned_ = true;
        buffer_.clear();
        throw;
    }
    return messages;
}

std::size_t StreamDecoder::buffered_size() const noexcept {
    return buffer_.size();
}

bool StreamDecoder::poisoned() const noexcept {
    return poisoned_;
}

}
