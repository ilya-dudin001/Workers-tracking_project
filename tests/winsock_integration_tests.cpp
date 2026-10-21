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
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <variant>
#include <vector>

namespace {

using namespace std::chrono_literals;
using monitor::net::ReceiveResult;
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

class LoopbackTestServer {
public:
    template <typename Handler>
    explicit LoopbackTestServer(Handler handler) {
        listener_ = Socket::tcp();
        listener_.bind_loopback(0);
        listener_.listen();
        port_ = listener_.local_port();

        std::promise<std::vector<Message>> promise;
        result_ = promise.get_future();
        worker_ = std::jthread(
            [this, handler = std::move(handler), promise = std::move(promise)](
                const std::stop_token stop_token) mutable {
                try {
                    Socket peer = accept_until_stopped(stop_token);
                    ActivePeerRegistration registration(*this, peer);
                    peer.set_receive_timeout(25ms);
                    peer.set_send_timeout(100ms);
                    promise.set_value(handler(peer, stop_token));
                } catch (...) {
                    promise.set_exception(std::current_exception());
                }
            });
    }

    ~LoopbackTestServer() {
        stop_and_join();
    }

    LoopbackTestServer(const LoopbackTestServer&) = delete;
    LoopbackTestServer& operator=(const LoopbackTestServer&) = delete;

    [[nodiscard]] std::uint16_t port() const noexcept {
        return port_;
    }

    [[nodiscard]] std::optional<std::vector<Message>> finish(
        const std::chrono::milliseconds timeout = 3s) {
        if (result_.wait_for(timeout) != std::future_status::ready) {
            ADD_FAILURE() << "loopback server missed its deadline";
            stop_and_join();
            return std::nullopt;
        }
        try {
            auto value = result_.get();
            stop_and_join();
            return value;
        } catch (const std::exception& error) {
            stop_and_join();
            ADD_FAILURE() << "loopback server failed: " << error.what();
            return std::nullopt;
        } catch (...) {
            stop_and_join();
            ADD_FAILURE() << "loopback server failed with a non-standard exception";
            return std::nullopt;
        }
    }

private:
    class ActivePeerRegistration {
    public:
        ActivePeerRegistration(LoopbackTestServer& server, Socket& peer)
            : server_(server) {
            std::scoped_lock lock(server_.socket_mutex_);
            server_.active_peer_ = &peer;
        }

        ~ActivePeerRegistration() {
            std::scoped_lock lock(server_.socket_mutex_);
            server_.active_peer_ = nullptr;
        }

    private:
        LoopbackTestServer& server_;
    };

    Socket accept_until_stopped(const std::stop_token stop_token) {
        while (!stop_token.stop_requested()) {
            try {
                return listener_.accept(25ms);
            } catch (const std::runtime_error& error) {
                if (std::string_view(error.what()) != "accept timed out") {
                    throw;
                }
            }
        }
        throw std::runtime_error("loopback server stopped before accept");
    }

    void stop_and_join() noexcept {
        if (!worker_.joinable()) {
            return;
        }
        worker_.request_stop();
        listener_.shutdown();
        {
            std::scoped_lock lock(socket_mutex_);
            if (active_peer_ != nullptr) {
                active_peer_->shutdown();
            }
        }
        // All worker waits are capped at 25 ms; sends are capped at 100 ms.
        worker_.join();
    }

    Socket listener_;
    std::uint16_t port_ = 0;
    std::mutex socket_mutex_;
    Socket* active_peer_ = nullptr;
    std::future<std::vector<Message>> result_;
    std::jthread worker_;
};

ReceiveResult receive_until_data(Socket& peer,
                                 const std::stop_token stop_token,
                                 const std::chrono::steady_clock::time_point deadline) {
    while (!stop_token.stop_requested() &&
           std::chrono::steady_clock::now() < deadline) {
        ReceiveResult received = peer.receive_chunk(256U);
        if (received.status != ReceiveStatus::TimedOut) {
            return received;
        }
    }
    throw std::runtime_error("timed out waiting for client data");
}

bool wait_for_signal(std::future<void>& signal,
                     const std::stop_token stop_token,
                     const std::chrono::steady_clock::time_point deadline) {
    while (!stop_token.stop_requested() &&
           std::chrono::steady_clock::now() < deadline) {
        if (signal.wait_for(25ms) == std::future_status::ready) {
            return true;
        }
    }
    return false;
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
    std::promise<void> send_remainder_promise;
    auto send_remainder = send_remainder_promise.get_future();
    LoopbackTestServer server(
        [send_remainder = std::move(send_remainder)](
            Socket& peer, const std::stop_token stop_token) mutable {
            StreamDecoder decoder;
            std::vector<Message> received;
            const auto receive_deadline = std::chrono::steady_clock::now() + 2s;
            while (!stop_token.stop_requested() && received.size() < 3U &&
                   std::chrono::steady_clock::now() < receive_deadline) {
                const auto chunk = peer.receive_chunk(64U);
                if (chunk.status == ReceiveStatus::Closed) {
                    break;
                }
                if (chunk.status == ReceiveStatus::Data) {
                    auto messages = decoder.feed(chunk.bytes);
                    received.insert(received.end(), messages.begin(), messages.end());
                }
            }
            if (received.size() != 3U) {
                throw std::runtime_error("server did not receive three packets");
            }
            const Bytes first =
                monitor::protocol::encode(Message{SetProfile{300, 60, 640, 360}});
            const Bytes second =
                monitor::protocol::encode(Message{SetProfile{450, 50, 480, 270}});
            if (stop_token.stop_requested()) {
                throw std::runtime_error("server stopped before command send");
            }
            peer.send_all(std::span(first).first(5));
            if (!wait_for_signal(send_remainder,
                                 stop_token,
                                 std::chrono::steady_clock::now() + 2s)) {
                throw std::runtime_error("client did not request command remainder");
            }
            Bytes combined(first.begin() + 5, first.end());
            combined.insert(combined.end(), second.begin(), second.end());
            if (stop_token.stop_requested()) {
                throw std::runtime_error("server stopped before remainder send");
            }
            peer.send_all(combined);
            return received;
        });

    WinsockAgentSession session(AgentSessionConfig{
        AgentEndpoint{"127.0.0.1", server.port()}, test_hello(), 500ms, 200ms});
    const bool connected = session.connect();
    EXPECT_TRUE(connected);
    if (!connected) {
        static_cast<void>(server.finish());
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

    const auto received = server.finish();
    ASSERT_TRUE(received.has_value());
    ASSERT_EQ(received->size(), 3U);
    EXPECT_TRUE(std::holds_alternative<Hello>((*received)[0]));
    EXPECT_TRUE(std::holds_alternative<Frame>((*received)[1]));
    EXPECT_TRUE(std::holds_alternative<Heartbeat>((*received)[2]));
    EXPECT_EQ(std::get<Hello>((*received)[0]), test_hello());
}

TEST(WinsockIntegration, DetectsOrderlyServerCloseWithFiniteTimeout) {
    WinsockRuntime runtime;
    LoopbackTestServer server(
        [](Socket& peer, const std::stop_token stop_token) {
            const auto hello = receive_until_data(
                peer, stop_token, std::chrono::steady_clock::now() + 2s);
            if (hello.status != ReceiveStatus::Data) {
                throw std::runtime_error("server did not receive Hello");
            }
            return std::vector<Message>{};
        });

    WinsockAgentSession session(AgentSessionConfig{
        AgentEndpoint{"127.0.0.1", server.port()}, test_hello(), 500ms, 200ms});
    const bool connected = session.connect();
    EXPECT_TRUE(connected);
    const auto server_result = server.finish();
    EXPECT_TRUE(server_result.has_value());
    if (!connected) {
        return;
    }

    static_cast<void>(session.receive_commands());
    EXPECT_EQ(session.state(), AgentConnectionState::Disconnected);
    EXPECT_FALSE(session.last_error().empty());
}

TEST(WinsockIntegration, UnexpectedInboundPacketDoesNotEscapeOrchestratorTick) {
    WinsockRuntime runtime;
    LoopbackTestServer server(
        [](Socket& peer, const std::stop_token stop_token) {
            const auto hello = receive_until_data(
                peer, stop_token, std::chrono::steady_clock::now() + 2s);
            if (hello.status != ReceiveStatus::Data) {
                throw std::runtime_error("server did not receive Hello");
            }
            if (stop_token.stop_requested()) {
                throw std::runtime_error("server stopped before malformed send");
            }
            peer.send_all(monitor::protocol::encode(Message{Heartbeat{8}}));
            return std::vector<Message>{};
        });

    WinsockAgentSession session(AgentSessionConfig{
        AgentEndpoint{"127.0.0.1", server.port()}, test_hello(), 500ms, 25ms});
    const bool connected = session.connect();
    EXPECT_TRUE(connected);
    const auto server_result = server.finish();
    EXPECT_TRUE(server_result.has_value());
    if (!connected || !server_result.has_value()) {
        return;
    }

    const SteadyTime start{};
    AgentOrchestrator orchestrator(session, start, {});

    EXPECT_NO_THROW(orchestrator.tick(start));
    EXPECT_EQ(orchestrator.state(), AgentConnectionState::Disconnected);
    EXPECT_NE(orchestrator.last_error().find("SetProfile"), std::string::npos);
    EXPECT_EQ(orchestrator.next_reconnect_deadline(), start + 250ms);
}

}
