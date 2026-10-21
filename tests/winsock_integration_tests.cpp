#include "common/stream_decoder.h"
#include "common/winsock_socket.h"
#include "worker_agent/agent_session.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <span>
#include <thread>
#include <type_traits>
#include <variant>
#include <vector>

namespace {

using namespace std::chrono_literals;
using monitor::net::ReceiveStatus;
using monitor::net::Socket;
using monitor::net::WinsockRuntime;
using monitor::protocol::Bytes;
using monitor::protocol::Frame;
using monitor::protocol::Heartbeat;
using monitor::protocol::Hello;
using monitor::protocol::Message;
using monitor::protocol::SetProfile;
using monitor::protocol::StreamDecoder;
using monitor::worker_agent::AgentConnectionState;
using monitor::worker_agent::AgentEndpoint;
using monitor::worker_agent::AgentSessionConfig;
using monitor::worker_agent::WinsockAgentSession;

static_assert(!std::is_copy_constructible_v<Socket>);
static_assert(!std::is_copy_assignable_v<Socket>);
static_assert(std::is_nothrow_move_constructible_v<Socket>);

Hello test_hello() {
    return Hello{{0x10, 0x32, 0x54, 0x76}, "Ada", "desk-01"};
}

TEST(WinsockSocket, RejectsInvalidIpv4Endpoints) {
    WinsockRuntime runtime;
    EXPECT_THROW(static_cast<void>(
                     Socket::connect_ipv4("not-an-address", 5000, 50ms)),
                 std::invalid_argument);
    EXPECT_THROW(static_cast<void>(
                     Socket::connect_ipv4("127.0.0.1", 0, 50ms)),
                 std::invalid_argument);
}

TEST(WinsockIntegration, HelloFrameHeartbeatAndSplitCoalescedCommands) {
    WinsockRuntime runtime;
    Socket listener = Socket::tcp();
    listener.bind_loopback(0);
    listener.listen();
    const std::uint16_t port = listener.local_port();

    std::promise<std::vector<Message>> received_promise;
    auto received_future = received_promise.get_future();
    std::thread server([listener = std::move(listener),
                        promise = std::move(received_promise)]() mutable {
        try {
            Socket peer = listener.accept(2s);
            peer.set_receive_timeout(500ms);
            StreamDecoder decoder;
            std::vector<Message> received;
            while (received.size() < 3U) {
                auto chunk = peer.receive_chunk(64U);
                if (chunk.status != ReceiveStatus::Data) {
                    break;
                }
                auto messages = decoder.feed(chunk.bytes);
                received.insert(received.end(), messages.begin(), messages.end());
            }

            const Bytes first =
                monitor::protocol::encode(Message{SetProfile{300, 60, 640, 360}});
            const Bytes second =
                monitor::protocol::encode(Message{SetProfile{450, 50, 480, 270}});
            peer.send_all(std::span(first).first(5));
            std::this_thread::sleep_for(10ms);
            Bytes combined(first.begin() + 5, first.end());
            combined.insert(combined.end(), second.begin(), second.end());
            peer.send_all(combined);
            promise.set_value(std::move(received));
        } catch (...) {
            promise.set_exception(std::current_exception());
        }
    });

    WinsockAgentSession session(AgentSessionConfig{
        AgentEndpoint{"127.0.0.1", port}, test_hello(), 500ms, 200ms});
    ASSERT_TRUE(session.connect());
    EXPECT_EQ(session.state(), AgentConnectionState::Online);
    ASSERT_TRUE(session.send_frame(Frame{7, 320, 180, {0xFF, 0xD8, 0xFF, 0xD9}}));
    ASSERT_TRUE(session.send_heartbeat(Heartbeat{1234}));

    std::vector<SetProfile> commands;
    for (int attempt = 0; attempt < 4 && commands.size() < 2U; ++attempt) {
        auto batch = session.receive_commands();
        commands.insert(commands.end(), batch.begin(), batch.end());
    }

    ASSERT_EQ(commands.size(), 2U);
    EXPECT_EQ(commands[0], (SetProfile{300, 60, 640, 360}));
    EXPECT_EQ(commands[1], (SetProfile{450, 50, 480, 270}));

    const auto received = received_future.get();
    server.join();
    ASSERT_EQ(received.size(), 3U);
    EXPECT_TRUE(std::holds_alternative<Hello>(received[0]));
    EXPECT_TRUE(std::holds_alternative<Frame>(received[1]));
    EXPECT_TRUE(std::holds_alternative<Heartbeat>(received[2]));
    EXPECT_EQ(std::get<Hello>(received[0]), test_hello());
}

TEST(WinsockIntegration, DetectsOrderlyServerCloseWithFiniteTimeout) {
    WinsockRuntime runtime;
    Socket listener = Socket::tcp();
    listener.bind_loopback(0);
    listener.listen();
    const std::uint16_t port = listener.local_port();

    std::thread server([listener = std::move(listener)]() mutable {
        Socket peer = listener.accept(2s);
        peer.set_receive_timeout(500ms);
        const auto hello = peer.receive_chunk(256U);
        EXPECT_EQ(hello.status, ReceiveStatus::Data);
    });

    WinsockAgentSession session(AgentSessionConfig{
        AgentEndpoint{"127.0.0.1", port}, test_hello(), 500ms, 200ms});
    ASSERT_TRUE(session.connect());
    server.join();

    static_cast<void>(session.receive_commands());
    EXPECT_EQ(session.state(), AgentConnectionState::Disconnected);
    EXPECT_FALSE(session.last_error().empty());
}

TEST(WinsockIntegration, RejectsInboundTypesOtherThanSetProfile) {
    WinsockRuntime runtime;
    Socket listener = Socket::tcp();
    listener.bind_loopback(0);
    listener.listen();
    const std::uint16_t port = listener.local_port();

    std::thread server([listener = std::move(listener)]() mutable {
        Socket peer = listener.accept(2s);
        peer.set_receive_timeout(500ms);
        static_cast<void>(peer.receive_chunk(256U));
        peer.send_all(monitor::protocol::encode(Message{Heartbeat{8}}));
    });

    WinsockAgentSession session(AgentSessionConfig{
        AgentEndpoint{"127.0.0.1", port}, test_hello(), 500ms, 200ms});
    ASSERT_TRUE(session.connect());
    EXPECT_THROW(static_cast<void>(session.receive_commands()), monitor::protocol::ProtocolError);
    EXPECT_EQ(session.state(), AgentConnectionState::Disconnected);
    server.join();
}

}
