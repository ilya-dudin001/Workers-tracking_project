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

template <typename T>
T round_trip(const T& message) {
    const Bytes packet = monitor::protocol::encode(Message{message});
    return std::get<T>(monitor::protocol::decode_packet(packet));
}

TEST(ProtocolRoundTrip, Hello) {
    EXPECT_EQ(round_trip(Hello{"worker-\xE2\x98\x83"}), (Hello{"worker-\xE2\x98\x83"}));
}

TEST(ProtocolRoundTrip, Frame) {
    const Frame frame{42, 1920, 1080, Bytes{0xFF, 0xD8, 0x01, 0xFF, 0xD9}};
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
        0x57, 0x54, 0x52, 0x4B, 0x00, 0x01, 0x00, 0x03, 0x00, 0x00, 0x00, 0x08};
    EXPECT_TRUE(std::equal(expected.begin(), expected.end(), packet.begin()));
    EXPECT_EQ(std::vector<std::uint8_t>(packet.begin() + monitor::protocol::kHeaderSize, packet.end()),
              (Bytes{1, 2, 3, 4, 5, 6, 7, 8}));
}

TEST(ProtocolValidation, RejectsUnknownVersion) {
    Bytes packet = monitor::protocol::encode(Message{Heartbeat{1}});
    packet[5] = 2;
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
    Bytes header(monitor::protocol::kHeaderSize, 0);
    header[0] = 0x57;
    header[1] = 0x54;
    header[2] = 0x52;
    header[3] = 0x4B;
    header[5] = 1;
    header[7] = 3;
    const std::uint32_t oversized = monitor::protocol::kMaxPayloadSize + 1;
    header[8] = static_cast<std::uint8_t>(oversized >> 24);
    header[9] = static_cast<std::uint8_t>(oversized >> 16);
    header[10] = static_cast<std::uint8_t>(oversized >> 8);
    header[11] = static_cast<std::uint8_t>(oversized);
    EXPECT_THROW(static_cast<void>(monitor::protocol::decode_packet(header)), ProtocolError);
}

TEST(ProtocolValidation, RejectsOversizedControlString) {
    EXPECT_THROW(static_cast<void>(monitor::protocol::encode(
                     Message{Hello{std::string(monitor::protocol::kMaxControlStringBytes + 1, 'a')}})),
                 ProtocolError);
}

TEST(ProtocolValidation, RejectsInvalidUtf8) {
    EXPECT_THROW(static_cast<void>(
                     monitor::protocol::encode(Message{Hello{std::string("\xC3\x28", 2)}})),
                 ProtocolError);
}

TEST(ProtocolValidation, RejectsMalformedFixedPayload) {
    Bytes packet = monitor::protocol::encode(Message{Heartbeat{1}});
    packet[11] = 7;
    packet.pop_back();
    EXPECT_THROW(static_cast<void>(monitor::protocol::decode_packet(packet)), ProtocolError);
}

}
