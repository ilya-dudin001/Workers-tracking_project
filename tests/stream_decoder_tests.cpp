#include "common/stream_decoder.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>
#include <vector>

namespace {

using monitor::protocol::Bytes;
using monitor::protocol::Heartbeat;
using monitor::protocol::Hello;
using monitor::protocol::Message;
using monitor::protocol::ProtocolError;
using monitor::protocol::StreamDecoder;

TEST(StreamDecoder, AcceptsPacketByteByByte) {
    const Hello hello{{1}, "worker", "host"};
    const Bytes packet = monitor::protocol::encode(Message{hello});
    StreamDecoder decoder;
    std::vector<Message> decoded;

    for (const std::uint8_t byte : packet) {
        auto messages = decoder.feed(std::span<const std::uint8_t>(&byte, 1));
        decoded.insert(decoded.end(), messages.begin(), messages.end());
    }

    ASSERT_EQ(decoded.size(), 1U);
    EXPECT_EQ(std::get<Hello>(decoded.front()), hello);
    EXPECT_EQ(decoder.buffered_size(), 0U);
}

TEST(StreamDecoder, PreservesPartialPacketBetweenFeeds) {
    const Bytes packet = monitor::protocol::encode(Message{Heartbeat{44}});
    StreamDecoder decoder;
    const std::size_t split = monitor::protocol::kHeaderSize + 2;

    EXPECT_TRUE(decoder.feed(std::span(packet).first(split)).empty());
    EXPECT_EQ(decoder.buffered_size(), split);
    const auto decoded = decoder.feed(std::span(packet).subspan(split));

    ASSERT_EQ(decoded.size(), 1U);
    EXPECT_EQ(std::get<Heartbeat>(decoded.front()), (Heartbeat{44}));
    EXPECT_EQ(decoder.buffered_size(), 0U);
}

TEST(StreamDecoder, ProducesMultipleMessagesFromOneFeed) {
    const Bytes first = monitor::protocol::encode(Message{Heartbeat{1}});
    const Bytes second = monitor::protocol::encode(Message{Heartbeat{2}});
    Bytes combined = first;
    combined.insert(combined.end(), second.begin(), second.end());
    StreamDecoder decoder;

    const auto decoded = decoder.feed(combined);

    ASSERT_EQ(decoded.size(), 2U);
    EXPECT_EQ(std::get<Heartbeat>(decoded[0]).timestamp_ms, 1U);
    EXPECT_EQ(std::get<Heartbeat>(decoded[1]).timestamp_ms, 2U);
}

TEST(StreamDecoder, ProducesCompleteMessageAndPreservesNextPartialMessage) {
    const Bytes first = monitor::protocol::encode(Message{Heartbeat{1}});
    const Bytes second = monitor::protocol::encode(Message{Hello{{2}, "next", "host"}});
    Bytes chunk = first;
    chunk.insert(chunk.end(), second.begin(), second.begin() + 5);
    StreamDecoder decoder;

    const auto decoded = decoder.feed(chunk);

    ASSERT_EQ(decoded.size(), 1U);
    EXPECT_EQ(decoder.buffered_size(), 5U);
    const auto remainder = decoder.feed(std::span(second).subspan(5));
    ASSERT_EQ(remainder.size(), 1U);
    EXPECT_EQ(std::get<Hello>(remainder.front()).employee_name, "next");
}

TEST(StreamDecoder, MalformedHeaderPoisonsDecoder) {
    Bytes packet = monitor::protocol::encode(Message{Heartbeat{1}});
    packet[0] = 0;
    StreamDecoder decoder;

    EXPECT_THROW(static_cast<void>(decoder.feed(packet)), ProtocolError);
    EXPECT_TRUE(decoder.poisoned());
    EXPECT_THROW(static_cast<void>(decoder.feed(Bytes{})), ProtocolError);
}

TEST(StreamDecoder, OversizedHeaderPoisonsDecoderBeforePayloadArrives) {
    Bytes packet = monitor::protocol::encode(Message{Heartbeat{1}});
    const std::uint32_t size =
        static_cast<std::uint32_t>(monitor::protocol::kMaxPacketSize -
                                   monitor::protocol::kHeaderSize + 1);
    packet.resize(monitor::protocol::kHeaderSize);
    packet[8] = static_cast<std::uint8_t>(size >> 24);
    packet[9] = static_cast<std::uint8_t>(size >> 16);
    packet[10] = static_cast<std::uint8_t>(size >> 8);
    packet[11] = static_cast<std::uint8_t>(size);
    StreamDecoder decoder;

    EXPECT_THROW(static_cast<void>(decoder.feed(packet)), ProtocolError);
    EXPECT_TRUE(decoder.poisoned());
}

TEST(StreamDecoder, BuffersAtMostOneIncompleteMaximumPacket) {
    constexpr std::size_t frame_metadata_size = 20;
    const std::size_t image_size =
        monitor::protocol::kMaxPacketSize - monitor::protocol::kHeaderSize -
        frame_metadata_size;
    const Bytes packet =
        monitor::protocol::encode(Message{monitor::protocol::Frame{7, 1, 1, Bytes(image_size)}});
    ASSERT_EQ(packet.size(), monitor::protocol::kMaxPacketSize);
    StreamDecoder decoder;

    EXPECT_TRUE(decoder.feed(std::span(packet).first(packet.size() - 1)).empty());
    EXPECT_EQ(decoder.buffered_size(), monitor::protocol::kMaxPacketSize - 1);
    const auto decoded = decoder.feed(std::span(packet).last(1));

    ASSERT_EQ(decoded.size(), 1U);
    EXPECT_EQ(std::get<monitor::protocol::Frame>(decoded.front()).sequence, 7U);
    EXPECT_EQ(decoder.buffered_size(), 0U);
}

TEST(StreamDecoder, AcceptsManyCompletePacketsInChunkLargerThanPacketLimit) {
    constexpr std::size_t image_size = 1024U * 1024U;
    constexpr std::size_t packet_count = 9;
    const Bytes packet =
        monitor::protocol::encode(Message{monitor::protocol::Frame{3, 1, 1, Bytes(image_size)}});
    Bytes chunk;
    chunk.reserve(packet.size() * packet_count);
    for (std::size_t count = 0; count < packet_count; ++count) {
        chunk.insert(chunk.end(), packet.begin(), packet.end());
    }
    ASSERT_GT(chunk.size(), monitor::protocol::kMaxPacketSize);
    StreamDecoder decoder;

    const auto decoded = decoder.feed(chunk);

    EXPECT_EQ(decoded.size(), packet_count);
    EXPECT_EQ(decoder.buffered_size(), 0U);
}

}
