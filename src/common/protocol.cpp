#include "common/protocol.h"

#include <algorithm>
#include <string_view>
#include <type_traits>

namespace monitor::protocol {
namespace {

constexpr std::size_t kFrameMetadataSize = 20;

void append_u16(Bytes& output, const std::uint16_t value) {
    output.push_back(static_cast<std::uint8_t>(value >> 8));
    output.push_back(static_cast<std::uint8_t>(value));
}

void append_u32(Bytes& output, const std::uint32_t value) {
    output.push_back(static_cast<std::uint8_t>(value >> 24));
    output.push_back(static_cast<std::uint8_t>(value >> 16));
    output.push_back(static_cast<std::uint8_t>(value >> 8));
    output.push_back(static_cast<std::uint8_t>(value));
}

void append_u64(Bytes& output, const std::uint64_t value) {
    append_u32(output, static_cast<std::uint32_t>(value >> 32));
    append_u32(output, static_cast<std::uint32_t>(value));
}

bool is_valid_utf8(const std::string_view text) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
    std::size_t index = 0;
    while (index < text.size()) {
        const unsigned char first = bytes[index];
        std::size_t length = 0;
        if (first <= 0x7F) {
            length = 1;
        } else if (first >= 0xC2 && first <= 0xDF) {
            length = 2;
        } else if (first >= 0xE0 && first <= 0xEF) {
            length = 3;
        } else if (first >= 0xF0 && first <= 0xF4) {
            length = 4;
        } else {
            return false;
        }
        if (index + length > text.size()) {
            return false;
        }
        for (std::size_t offset = 1; offset < length; ++offset) {
            if ((bytes[index + offset] & 0xC0) != 0x80) {
                return false;
            }
        }
        if (length == 3 &&
            ((first == 0xE0 && bytes[index + 1] < 0xA0) ||
             (first == 0xED && bytes[index + 1] >= 0xA0))) {
            return false;
        }
        if (length == 4 &&
            ((first == 0xF0 && bytes[index + 1] < 0x90) ||
             (first == 0xF4 && bytes[index + 1] >= 0x90))) {
            return false;
        }
        index += length;
    }
    return true;
}

void append_control_string(Bytes& output, const std::string_view text) {
    if (text.size() > kMaxControlStringBytes || !is_valid_utf8(text)) {
        throw ProtocolError("invalid control string");
    }
    append_u16(output, static_cast<std::uint16_t>(text.size()));
    output.insert(output.end(), text.begin(), text.end());
}

class Reader {
public:
    explicit Reader(const std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

    std::uint8_t u8() {
        require(1);
        return bytes_[position_++];
    }

    std::uint16_t u16() {
        require(2);
        const std::uint16_t value =
            static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes_[position_]) << 8) |
                                       bytes_[position_ + 1]);
        position_ += 2;
        return value;
    }

    std::uint32_t u32() {
        require(4);
        std::uint32_t value = 0;
        for (int count = 0; count < 4; ++count) {
            value = (value << 8) | bytes_[position_++];
        }
        return value;
    }

    std::uint64_t u64() {
        const std::uint64_t high = u32();
        const std::uint64_t low = u32();
        return (high << 32) | low;
    }

    Bytes bytes(const std::size_t size) {
        require(size);
        Bytes value(bytes_.begin() + static_cast<std::ptrdiff_t>(position_),
                    bytes_.begin() + static_cast<std::ptrdiff_t>(position_ + size));
        position_ += size;
        return value;
    }

    std::string control_string() {
        const std::size_t size = u16();
        if (size > kMaxControlStringBytes) {
            throw ProtocolError("control string exceeds limit");
        }
        const Bytes raw = bytes(size);
        const std::string value(raw.begin(), raw.end());
        if (!is_valid_utf8(value)) {
            throw ProtocolError("control string is not UTF-8");
        }
        return value;
    }

    void require_finished() const {
        if (position_ != bytes_.size()) {
            throw ProtocolError("unexpected payload bytes");
        }
    }

private:
    void require(const std::size_t size) const {
        if (size > bytes_.size() - position_) {
            throw ProtocolError("truncated payload");
        }
    }

    std::span<const std::uint8_t> bytes_;
    std::size_t position_ = 0;
};

MessageType message_type(const Message& message) {
    return std::visit(
        [](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, Hello>) {
                return MessageType::Hello;
            } else if constexpr (std::is_same_v<T, Frame>) {
                return MessageType::Frame;
            } else if constexpr (std::is_same_v<T, Heartbeat>) {
                return MessageType::Heartbeat;
            } else if constexpr (std::is_same_v<T, SetProfile>) {
                return MessageType::SetProfile;
            } else {
                return MessageType::RenameAck;
            }
        },
        message);
}

Bytes encode_payload(const Message& message) {
    Bytes payload;
    std::visit(
        [&payload](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, Hello>) {
                append_control_string(payload, value.name);
            } else if constexpr (std::is_same_v<T, Frame>) {
                if (value.encoded_image.size() > kMaxPayloadSize - kFrameMetadataSize) {
                    throw ProtocolError("frame is too large");
                }
                payload.reserve(kFrameMetadataSize + value.encoded_image.size());
                append_u64(payload, value.sequence);
                append_u32(payload, value.width);
                append_u32(payload, value.height);
                append_u32(payload, static_cast<std::uint32_t>(value.encoded_image.size()));
                payload.insert(payload.end(), value.encoded_image.begin(), value.encoded_image.end());
            } else if constexpr (std::is_same_v<T, Heartbeat>) {
                append_u64(payload, value.timestamp_ms);
            } else if constexpr (std::is_same_v<T, SetProfile>) {
                append_u32(payload, value.frame_interval_ms);
                append_u16(payload, value.jpeg_quality);
                append_u16(payload, value.max_width);
                append_u16(payload, value.max_height);
            } else {
                payload.push_back(value.accepted ? 1 : 0);
                append_control_string(payload, value.name);
            }
        },
        message);
    return payload;
}

}

PacketHeader decode_header(const std::span<const std::uint8_t> header) {
    if (header.size() < kHeaderSize) {
        throw ProtocolError("truncated header");
    }
    Reader reader(header.first(kHeaderSize));
    if (reader.u32() != kMagic) {
        throw ProtocolError("invalid packet magic");
    }
    if (reader.u16() != kVersion) {
        throw ProtocolError("unsupported protocol version");
    }
    const std::uint16_t raw_type = reader.u16();
    if (raw_type < static_cast<std::uint16_t>(MessageType::Hello) ||
        raw_type > static_cast<std::uint16_t>(MessageType::RenameAck)) {
        throw ProtocolError("unknown message type");
    }
    const auto type = static_cast<MessageType>(raw_type);
    const std::uint32_t payload_size = reader.u32();
    if (payload_size > kMaxPayloadSize) {
        throw ProtocolError("payload exceeds packet limit");
    }
    if (type != MessageType::Frame && payload_size > kMaxControlPayloadSize) {
        throw ProtocolError("control payload exceeds limit");
    }
    return PacketHeader{type, payload_size};
}

Bytes encode(const Message& message) {
    const MessageType type = message_type(message);
    Bytes payload = encode_payload(message);
    if (payload.size() > kMaxPayloadSize ||
        (type != MessageType::Frame && payload.size() > kMaxControlPayloadSize)) {
        throw ProtocolError("payload exceeds limit");
    }

    Bytes packet;
    packet.reserve(kHeaderSize + payload.size());
    append_u32(packet, kMagic);
    append_u16(packet, kVersion);
    append_u16(packet, static_cast<std::uint16_t>(type));
    append_u32(packet, static_cast<std::uint32_t>(payload.size()));
    packet.insert(packet.end(), payload.begin(), payload.end());
    return packet;
}

Message decode_packet(const std::span<const std::uint8_t> packet) {
    const PacketHeader header = decode_header(packet);
    const std::size_t expected_size = kHeaderSize + header.payload_size;
    if (packet.size() != expected_size) {
        throw ProtocolError(packet.size() < expected_size ? "truncated packet" : "trailing packet bytes");
    }

    Reader reader(packet.subspan(kHeaderSize));
    Message message;
    switch (header.type) {
        case MessageType::Hello:
            message = Hello{reader.control_string()};
            break;
        case MessageType::Frame: {
            const std::uint64_t sequence = reader.u64();
            const std::uint32_t width = reader.u32();
            const std::uint32_t height = reader.u32();
            const std::uint32_t image_size = reader.u32();
            message = Frame{sequence, width, height, reader.bytes(image_size)};
            break;
        }
        case MessageType::Heartbeat:
            message = Heartbeat{reader.u64()};
            break;
        case MessageType::SetProfile:
            message = SetProfile{reader.u32(), reader.u16(), reader.u16(), reader.u16()};
            break;
        case MessageType::RenameAck: {
            const std::uint8_t accepted = reader.u8();
            if (accepted > 1) {
                throw ProtocolError("invalid rename acknowledgement flag");
            }
            message = RenameAck{accepted == 1, reader.control_string()};
            break;
        }
    }
    reader.require_finished();
    return message;
}

}
