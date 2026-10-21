#include "common/stream_decoder.h"

#include <algorithm>
#include <cstddef>

namespace monitor::protocol {

std::vector<Message> StreamDecoder::feed(const std::span<const std::uint8_t> chunk) {
    if (poisoned_) {
        throw ProtocolError("stream decoder is poisoned");
    }

    std::vector<Message> messages;
    std::size_t read_position = 0;
    try {
        while (read_position < chunk.size()) {
            const std::span<const std::uint8_t> remaining = chunk.subspan(read_position);
            if (buffer_.empty() && remaining.size() >= kHeaderSize) {
                const PacketHeader header = decode_header(remaining.first(kHeaderSize));
                const std::size_t packet_size = kHeaderSize + header.payload_size;
                if (remaining.size() >= packet_size) {
                    messages.push_back(decode_packet(remaining.first(packet_size)));
                    read_position += packet_size;
                    continue;
                }
                buffer_.reserve(packet_size);
                buffer_.insert(buffer_.end(), remaining.begin(), remaining.end());
                break;
            }

            if (buffer_.size() < kHeaderSize) {
                const std::size_t bytes_to_header =
                    std::min(kHeaderSize - buffer_.size(), remaining.size());
                buffer_.insert(buffer_.end(), remaining.begin(),
                               remaining.begin() + static_cast<std::ptrdiff_t>(bytes_to_header));
                read_position += bytes_to_header;
                if (buffer_.size() < kHeaderSize) {
                    break;
                }
            }

            const PacketHeader header =
                decode_header(std::span<const std::uint8_t>(buffer_).first(kHeaderSize));
            const std::size_t packet_size = kHeaderSize + header.payload_size;
            buffer_.reserve(packet_size);
            const std::size_t bytes_to_packet =
                std::min(packet_size - buffer_.size(), chunk.size() - read_position);
            buffer_.insert(
                buffer_.end(), chunk.begin() + static_cast<std::ptrdiff_t>(read_position),
                chunk.begin() + static_cast<std::ptrdiff_t>(read_position + bytes_to_packet));
            read_position += bytes_to_packet;
            if (buffer_.size() < packet_size) {
                break;
            }
            messages.push_back(decode_packet(buffer_));
            buffer_.clear();
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
