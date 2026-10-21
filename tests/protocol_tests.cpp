#include "common/protocol.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace {

using monitor::protocol::Bytes;
using monitor::protocol::Frame;
using monitor::protocol::Heartbeat;
using monitor::protocol::Hello;
using monitor::protocol::Message;
using monitor::protocol::ProtocolError;
using monitor::protocol::RenameAck;
using monitor::protocol::SetProfile;

static_assert(monitor::protocol::kMaxPacketSize == 8U * 1024U * 1024U);
static_assert(monitor::protocol::kMaxPayloadSize + monitor::protocol::kHeaderSize ==
              monitor::protocol::kMaxPacketSize);

void write_u32(Bytes& bytes, const std::size_t offset, const std::uint32_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value >> 24);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 16);
    bytes[offset + 2] = static_cast<std::uint8_t>(value >> 8);
    bytes[offset + 3] = static_cast<std::uint8_t>(value);
}

Bytes header_for(const std::uint16_t type, const std::uint32_t payload_size) {
    Bytes header(monitor::protocol::kHeaderSize, 0);
    write_u32(header, 0, monitor::protocol::kMagic);
    header[4] = static_cast<std::uint8_t>(monitor::protocol::kVersion >> 8);
    header[5] = static_cast<std::uint8_t>(monitor::protocol::kVersion);
    header[6] = static_cast<std::uint8_t>(type >> 8);
    header[7] = static_cast<std::uint8_t>(type);
    write_u32(header, 8, payload_size);
    return header;
}

template <typename T>
T round_trip(const T& message) {
    const Bytes packet = monitor::protocol::encode(Message{message});
    return std::get<T>(monitor::protocol::decode_packet(packet));
}

TEST(ProtocolRoundTrip, Hello) {
    const Hello hello{
        {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
         0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF},
        "worker-\xE2\x98\x83",
        "host-\xE6\x9D\xB1"};
    EXPECT_EQ(round_trip(hello), hello);
}

TEST(ProtocolWireFormat, HelloStartsWithFixedGuidThenLengthPrefixedStrings) {
    const Hello hello{
        {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
         0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F},
        "Ada",
        "desk"};
    const Bytes packet = monitor::protocol::encode(Message{hello});
    const auto payload = std::span(packet).subspan(monitor::protocol::kHeaderSize);

    ASSERT_EQ(payload.size(), 16U + 2U + 3U + 2U + 4U);
    EXPECT_TRUE(std::equal(hello.agent_guid.begin(), hello.agent_guid.end(), payload.begin()));
    EXPECT_EQ(payload[16], 0U);
    EXPECT_EQ(payload[17], 3U);
    EXPECT_EQ(payload[21], 0U);
    EXPECT_EQ(payload[22], 4U);
}

TEST(ProtocolRoundTrip, Frame) {
    const Frame frame{42, 1920, 1080, Bytes{0xFF, 0xD8, 0x01, 0xFF, 0xD9},
                      9'876'543'210ULL};
    EXPECT_EQ(round_trip(frame), frame);
}

TEST(ProtocolRoundTrip, Heartbeat) {
    EXPECT_EQ(round_trip(Heartbeat{9'876'543'210ULL}), (Heartbeat{9'876'543'210ULL}));
}

TEST(ProtocolRoundTrip, SetProfile) {
    const SetProfile profile{250, 75, 1280, 720};
    EXPECT_EQ(round_trip(profile), profile);
}

TEST(ProtocolRoundTrip, RenameAck) {
    EXPECT_EQ(round_trip(RenameAck{true, "desk-\xE6\x9D\xB1"}), (RenameAck{true, "desk-\xE6\x9D\xB1"}));
}

TEST(ProtocolWireFormat, HeaderUsesNetworkByteOrder) {
    const Bytes packet = monitor::protocol::encode(Message{Heartbeat{0x0102030405060708ULL}});
    const std::array<std::uint8_t, monitor::protocol::kHeaderSize> expected{
        0x57, 0x54, 0x52, 0x4B, 0x00, 0x02, 0x00, 0x03, 0x00, 0x00, 0x00, 0x08};
    EXPECT_TRUE(std::equal(expected.begin(), expected.end(), packet.begin()));
    EXPECT_EQ(std::vector<std::uint8_t>(packet.begin() + monitor::protocol::kHeaderSize, packet.end()),
              (Bytes{1, 2, 3, 4, 5, 6, 7, 8}));
}

TEST(ProtocolWireFormat, DecodesKnownNonSymmetricUint64InNetworkOrder) {
    Bytes packet = header_for(3, 8);
    packet.insert(packet.end(), {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF});

    const auto decoded = monitor::protocol::decode_packet(packet);

    EXPECT_EQ(std::get<Heartbeat>(decoded).timestamp_ms, 0x0123456789ABCDEFULL);
}

TEST(ProtocolValidation, RejectsUnknownVersion) {
    Bytes packet = monitor::protocol::encode(Message{Heartbeat{1}});
    packet[5] = 0x7F;
    EXPECT_THROW(static_cast<void>(monitor::protocol::decode_packet(packet)), ProtocolError);
}

TEST(ProtocolValidation, RejectsUnknownType) {
    Bytes packet = monitor::protocol::encode(Message{Heartbeat{1}});
    packet[6] = 0x7F;
    packet[7] = 0xFF;
    EXPECT_THROW(static_cast<void>(monitor::protocol::decode_packet(packet)), ProtocolError);
}

TEST(ProtocolValidation, RejectsTruncatedHeader) {
    EXPECT_THROW(static_cast<void>(
                     monitor::protocol::decode_packet(
                         Bytes(monitor::protocol::kHeaderSize - 1, 0))),
                 ProtocolError);
}

TEST(ProtocolValidation, RejectsTruncatedPayload) {
    Bytes packet = monitor::protocol::encode(Message{Heartbeat{1}});
    packet.pop_back();
    EXPECT_THROW(static_cast<void>(monitor::protocol::decode_packet(packet)), ProtocolError);
}

TEST(ProtocolValidation, RejectsTrailingBytes) {
    Bytes packet = monitor::protocol::encode(Message{Heartbeat{1}});
    packet.push_back(0);
    EXPECT_THROW(static_cast<void>(monitor::protocol::decode_packet(packet)), ProtocolError);
}

TEST(ProtocolValidation, RejectsGlobalPacketLimitInHeader) {
    const auto oversized_payload =
        static_cast<std::uint32_t>(monitor::protocol::kMaxPacketSize -
                                   monitor::protocol::kHeaderSize + 1);
    const Bytes header = header_for(2, oversized_payload);
    EXPECT_THROW(static_cast<void>(monitor::protocol::decode_header(header)), ProtocolError);
}

TEST(ProtocolValidation, RejectsFrameWhoseTotalPacketExceedsLimit) {
    constexpr std::size_t frame_metadata_size = 28;
    const std::size_t oversized_image_size =
        monitor::protocol::kMaxPacketSize - monitor::protocol::kHeaderSize -
        frame_metadata_size + 1;
    const Frame frame{1, 1, 1, Bytes(oversized_image_size, 0x5A)};

    EXPECT_THROW(static_cast<void>(monitor::protocol::encode(Message{frame})), ProtocolError);
    EXPECT_EQ(frame.encoded_image.size(), oversized_image_size);
}

TEST(ProtocolValidation, RejectsOversizedControlString) {
    EXPECT_THROW(static_cast<void>(monitor::protocol::encode(
                     Message{Hello{{}, std::string(
                                               monitor::protocol::kMaxControlStringBytes + 1, 'a'),
                                         "host"}})),
                 ProtocolError);
}

TEST(ProtocolValidation, RejectsInvalidUtf8) {
    EXPECT_THROW(static_cast<void>(
                     monitor::protocol::encode(
                         Message{Hello{{}, std::string("\xC3\x28", 2), "host"}})),
                 ProtocolError);
}

TEST(ProtocolValidation, RejectsInvalidUtf8FromWire) {
    Bytes packet = monitor::protocol::encode(Message{Hello{{}, "ok", "host"}});
    packet[monitor::protocol::kHeaderSize + 18] = 0xC3;
    packet[monitor::protocol::kHeaderSize + 19] = 0x28;

    EXPECT_THROW(static_cast<void>(monitor::protocol::decode_packet(packet)), ProtocolError);
}

TEST(ProtocolValidation, RejectsOversizedControlStringLengthFromWire) {
    Bytes packet = header_for(1, 2);
    const std::size_t oversized = monitor::protocol::kMaxControlStringBytes + 1;
    packet.push_back(static_cast<std::uint8_t>(oversized >> 8));
    packet.push_back(static_cast<std::uint8_t>(oversized));

    EXPECT_THROW(static_cast<void>(monitor::protocol::decode_packet(packet)), ProtocolError);
}

TEST(ProtocolValidation, RejectsOversizedControlPayloadFromWire) {
    const Bytes header =
        header_for(1, monitor::protocol::kMaxControlPayloadSize + 1);

    EXPECT_THROW(static_cast<void>(monitor::protocol::decode_header(header)), ProtocolError);
}

TEST(ProtocolValidation, RejectsMalformedFixedPayload) {
    Bytes packet = monitor::protocol::encode(Message{Heartbeat{1}});
    packet[11] = 7;
    packet.pop_back();
    EXPECT_THROW(static_cast<void>(monitor::protocol::decode_packet(packet)), ProtocolError);
}

}
