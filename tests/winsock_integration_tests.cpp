#include "common/stream_decoder.h"
#include "common/winsock_socket.h"
#include "worker_agent/agent_orchestrator.h"
#include "worker_agent/agent_session.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
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
using monitor::worker_agent::AgentOrchestrator;
using monitor::worker_agent::AgentSessionConfig;
using monitor::worker_agent::SteadyTime;
using monitor::worker_agent::WinsockAgentSession;

static_assert(!std::is_copy_constructible_v<Socket>);
static_assert(!std::is_copy_assignable_v<Socket>);
static_assert(std::is_nothrow_move_constructible_v<Socket>);

Hello test_hello() {
    return Hello{{0x10, 0x32, 0x54, 0x76}, "Ada", "desk-01"};
}

template <typename T>
std::optional<T> finish_server(std::future<T>& result, std::jthread& server) {
    if (result.wait_for(3s) != std::future_status::ready) {
        ADD_FAILURE() << "loopback server missed its three-second deadline";
        server.join();
        return std::nullopt;
    }
    try {
        T value = result.get();
        server.join();
        return value;
    } catch (const std::exception& error) {
        server.join();
        ADD_FAILURE() << "loopback server failed: " << error.what();
        return std::nullopt;
    } catch (...) {
        server.join();
        ADD_FAILURE() << "loopback server failed with a non-standard exception";
        return std::nullopt;
    }
}

bool finish_server(std::future<void>& result, std::jthread& server) {
    if (result.wait_for(3s) != std::future_status::ready) {
        ADD_FAILURE() << "loopback server missed its three-second deadline";
        server.join();
        return false;
    }
    try {
        result.get();
        server.join();
        return true;
    } catch (const std::exception& error) {
        server.join();
        ADD_FAILURE() << "loopback server failed: " << error.what();
        return false;
    } catch (...) {
        server.join();
        ADD_FAILURE() << "loopback server failed with a non-standard exception";
        return false;
    }
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
    std::promise<void> send_remainder_promise;
    auto send_remainder = send_remainder_promise.get_future();
    std::jthread server([listener = std::move(listener),
                         promise = std::move(received_promise),
                         send_remainder = std::move(send_remainder)]() mutable {
        try {
            Socket peer = listener.accept(2s);
            peer.set_receive_timeout(100ms);
            peer.set_send_timeout(500ms);
            StreamDecoder decoder;
            std::vector<Message> received;
            const auto receive_deadline = std::chrono::steady_clock::now() + 2s;
            while (received.size() < 3U &&
                   std::chrono::steady_clock::now() < receive_deadline) {
                auto chunk = peer.receive_chunk(64U);
                if (chunk.status == ReceiveStatus::Closed) {
                    break;
                }
                if (chunk.status == ReceiveStatus::TimedOut) {
                    continue;
                }
                auto messages = decoder.feed(chunk.bytes);
                received.insert(received.end(), messages.begin(), messages.end());
            }

            const Bytes first =
                monitor::protocol::encode(Message{SetProfile{300, 60, 640, 360}});
            const Bytes second =
                monitor::protocol::encode(Message{SetProfile{450, 50, 480, 270}});
            peer.send_all(std::span(first).first(5));
            if (send_remainder.wait_for(2s) != std::future_status::ready) {
                throw std::runtime_error("client did not request command remainder");
            }
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
    const bool connected = session.connect();
    EXPECT_TRUE(connected);
    if (!connected) {
        const auto ignored = finish_server(received_future, server);
        return;
    }
    EXPECT_EQ(session.state(), AgentConnectionState::Online);
    EXPECT_TRUE(session.send_frame(Frame{7, 320, 180, {0xFF, 0xD8, 0xFF, 0xD9}}));
    EXPECT_TRUE(session.send_heartbeat(Heartbeat{1234}));

    std::vector<SetProfile> commands = session.receive_commands();
    EXPECT_TRUE(commands.empty());
    send_remainder_promise.set_value();
    for (int attempt = 0; attempt < 4 && commands.size() < 2U; ++attempt) {
        auto batch = session.receive_commands();
        commands.insert(commands.end(), batch.begin(), batch.end());
    }

    EXPECT_EQ(commands.size(), 2U);
    if (commands.size() == 2U) {
        EXPECT_EQ(commands[0], (SetProfile{300, 60, 640, 360}));
        EXPECT_EQ(commands[1], (SetProfile{450, 50, 480, 270}));
    }

    const auto received = finish_server(received_future, server);
    ASSERT_TRUE(received.has_value());
    ASSERT_EQ(received->size(), 3U);
    EXPECT_TRUE(std::holds_alternative<Hello>((*received)[0]));
    EXPECT_TRUE(std::holds_alternative<Frame>((*received)[1]));
    EXPECT_TRUE(std::holds_alternative<Heartbeat>((*received)[2]));
    EXPECT_EQ(std::get<Hello>((*received)[0]), test_hello());
}

TEST(WinsockIntegration, DetectsOrderlyServerCloseWithFiniteTimeout) {
    WinsockRuntime runtime;
    Socket listener = Socket::tcp();
    listener.bind_loopback(0);
    listener.listen();
    const std::uint16_t port = listener.local_port();

    std::promise<void> server_promise;
    auto server_result = server_promise.get_future();
    std::jthread server([listener = std::move(listener),
                         promise = std::move(server_promise)]() mutable {
        try {
            Socket peer = listener.accept(2s);
            peer.set_receive_timeout(500ms);
            const auto hello = peer.receive_chunk(256U);
            if (hello.status != ReceiveStatus::Data) {
                throw std::runtime_error("server did not receive Hello");
            }
            promise.set_value();
        } catch (...) {
            promise.set_exception(std::current_exception());
        }
    });

    WinsockAgentSession session(AgentSessionConfig{
        AgentEndpoint{"127.0.0.1", port}, test_hello(), 500ms, 200ms});
    const bool connected = session.connect();
    EXPECT_TRUE(connected);
    EXPECT_TRUE(finish_server(server_result, server));
    if (!connected) {
        return;
    }

    static_cast<void>(session.receive_commands());
    EXPECT_EQ(session.state(), AgentConnectionState::Disconnected);
    EXPECT_FALSE(session.last_error().empty());
}

TEST(WinsockIntegration, UnexpectedInboundPacketDoesNotEscapeOrchestratorTick) {
    WinsockRuntime runtime;
    Socket listener = Socket::tcp();
    listener.bind_loopback(0);
    listener.listen();
    const std::uint16_t port = listener.local_port();

    std::promise<void> server_promise;
    auto server_result = server_promise.get_future();
    std::jthread server([listener = std::move(listener),
                         promise = std::move(server_promise)]() mutable {
        try {
            Socket peer = listener.accept(2s);
            peer.set_receive_timeout(500ms);
            peer.set_send_timeout(500ms);
            const auto hello = peer.receive_chunk(256U);
            if (hello.status != ReceiveStatus::Data) {
                throw std::runtime_error("server did not receive Hello");
            }
            peer.send_all(monitor::protocol::encode(Message{Heartbeat{8}}));
            promise.set_value();
        } catch (...) {
            promise.set_exception(std::current_exception());
        }
    });

    WinsockAgentSession session(AgentSessionConfig{
        AgentEndpoint{"127.0.0.1", port}, test_hello(), 500ms, 200ms});
    const SteadyTime start{};
    AgentOrchestrator orchestrator(session, start, {});

    EXPECT_NO_THROW(orchestrator.tick(start));
    EXPECT_TRUE(finish_server(server_result, server));
    EXPECT_EQ(orchestrator.state(), AgentConnectionState::Disconnected);
    EXPECT_NE(orchestrator.last_error().find("SetProfile"), std::string::npos);
    EXPECT_EQ(orchestrator.next_reconnect_deadline(), start + 250ms);
}

}
