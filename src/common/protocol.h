#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace monitor::protocol {

using Bytes = std::vector<std::uint8_t>;

inline constexpr std::uint32_t kMagic = 0x5754524B;
inline constexpr std::uint16_t kVersion = 1;
inline constexpr std::size_t kHeaderSize = 12;
inline constexpr std::size_t kMaxPacketSize = 8U * 1024U * 1024U;
inline constexpr std::uint32_t kMaxPayloadSize =
    static_cast<std::uint32_t>(kMaxPacketSize - kHeaderSize);
inline constexpr std::uint32_t kMaxControlPayloadSize = 2048;
inline constexpr std::size_t kMaxControlStringBytes = 1024;
inline constexpr std::size_t kAgentGuidSize = 16;
inline constexpr std::size_t kMaxIdentityStringBytes = 255;

using AgentGuid = std::array<std::uint8_t, kAgentGuidSize>;

enum class MessageType : std::uint16_t {
    Hello = 1,
    Frame = 2,
    Heartbeat = 3,
    SetProfile = 4,
    RenameAck = 5,
};

struct Hello {
    AgentGuid agent_guid{};
    std::string employee_name;
    std::string hostname;
    bool operator==(const Hello&) const = default;
};

struct Frame {
    std::uint64_t sequence;
    std::uint32_t width;
    std::uint32_t height;
    Bytes encoded_image;
    bool operator==(const Frame&) const = default;
};

struct Heartbeat {
    std::uint64_t timestamp_ms;
    bool operator==(const Heartbeat&) const = default;
};

struct SetProfile {
    std::uint32_t frame_interval_ms;
    std::uint16_t jpeg_quality;
    std::uint16_t max_width;
    std::uint16_t max_height;
    bool operator==(const SetProfile&) const = default;
};

struct RenameAck {
    bool accepted;
    std::string name;
    bool operator==(const RenameAck&) const = default;
};

using Message = std::variant<Hello, Frame, Heartbeat, SetProfile, RenameAck>;

class ProtocolError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct PacketHeader {
    MessageType type;
    std::uint32_t payload_size;
};

[[nodiscard]] PacketHeader decode_header(std::span<const std::uint8_t> header);
[[nodiscard]] Bytes encode(const Message& message);
[[nodiscard]] Message decode_packet(std::span<const std::uint8_t> packet);

}
