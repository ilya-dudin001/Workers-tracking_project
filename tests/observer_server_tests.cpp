#include "common/winsock_socket.h"
#include "observer/iocp_server.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using monitor::net::ReceiveStatus;
using monitor::net::Socket;
using monitor::observer::AgentRegistry;
using monitor::observer::IocpServer;
using monitor::protocol::AgentGuid;
using monitor::protocol::Frame;
using monitor::protocol::Heartbeat;
using monitor::protocol::Hello;
using monitor::protocol::Message;

AgentGuid server_guid(const std::uint8_t value) {
    AgentGuid result{};
    result.fill(value);
    return result;
}

bool wait_for(const auto& predicate, const std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(10ms);
    }
    return predicate();
}

Socket connect_to(const IocpServer& server) {
    Socket socket = Socket::connect_ipv4("127.0.0.1", server.port(), 1s);
    socket.set_receive_timeout(200ms);
    socket.set_send_timeout(1s);
    return socket;
}

TEST(ObserverServerIntegration, RequiresHelloAndRejectsWrongDirectionPackets) {
    monitor::net::WinsockRuntime runtime;
    AgentRegistry registry;
    IocpServer server(registry, {.bind_address = "127.0.0.1", .port = 0,
                                 .hello_timeout = 250ms});
    ASSERT_TRUE(server.start());
    Socket before_hello = connect_to(server);
    before_hello.send_all(monitor::protocol::encode(Message{Heartbeat{1}}));
    EXPECT_TRUE(wait_for([&] {
        return before_hello.receive_chunk(64).status == ReceiveStatus::Closed;
    }));

    Socket malformed = connect_to(server);
    const std::array<std::uint8_t, monitor::protocol::kHeaderSize> invalid{};
    malformed.send_all(invalid);
    EXPECT_TRUE(wait_for([&] {
        return malformed.receive_chunk(64).status == ReceiveStatus::Closed;
    }));

    Socket wrong_direction = connect_to(server);
    wrong_direction.send_all(monitor::protocol::encode(
        Message{Hello{server_guid(1), "Ada", "desk"}}));
    ASSERT_TRUE(wait_for([&] { return registry.snapshot(AgentRegistry::Clock::now()).size() == 1; }));
    wrong_direction.send_all(monitor::protocol::encode(
        Message{monitor::protocol::SetProfile{250, 75, 1280, 720}}));
    EXPECT_TRUE(wait_for([&] {
        return wrong_direction.receive_chunk(64).status == ReceiveStatus::Closed;
    }));
    server.stop();
}

TEST(ObserverServerIntegration, FrameHeartbeatAndNewestDuplicateUpdateRegistry) {
    monitor::net::WinsockRuntime runtime;
    AgentRegistry registry;
    IocpServer server(registry, {.bind_address = "127.0.0.1", .port = 0});
    ASSERT_TRUE(server.start());
    Socket first = connect_to(server);
    first.send_all(monitor::protocol::encode(
        Message{Hello{server_guid(2), "Old", "host-a"}}));
    first.send_all(monitor::protocol::encode(
        Message{Frame{9, 16, 9, {0xff, 0xd8, 0xff, 0xd9}}}));
    first.send_all(monitor::protocol::encode(Message{Heartbeat{42}}));
    ASSERT_TRUE(wait_for([&] {
        const auto items = registry.snapshot(AgentRegistry::Clock::now());
        return items.size() == 1 && items[0].frame && items[0].frame->sequence == 9;
    }));

    Socket newest = connect_to(server);
    newest.send_all(monitor::protocol::encode(
        Message{Hello{server_guid(2), "New", "host-b"}}));
    ASSERT_TRUE(wait_for([&] {
        const auto items = registry.snapshot(AgentRegistry::Clock::now());
        return items.size() == 1 && items[0].employee_name == "New" && !items[0].frame;
    }));
    EXPECT_TRUE(wait_for([&] {
        return first.receive_chunk(64).status == ReceiveStatus::Closed;
    }));
    server.stop();
}

TEST(ObserverServerIntegration, RejectsFortyFirstDistinctAgentAndStopsCleanly) {
    monitor::net::WinsockRuntime runtime;
    AgentRegistry registry;
    IocpServer server(registry, {.bind_address = "127.0.0.1", .port = 0});
    ASSERT_TRUE(server.start());
    std::vector<Socket> sockets;
    for (std::uint8_t index = 0; index < 40; ++index) {
        sockets.push_back(connect_to(server));
        sockets.back().send_all(monitor::protocol::encode(
            Message{Hello{server_guid(index), "worker", "host"}}));
    }
    ASSERT_TRUE(wait_for([&] {
        return registry.snapshot(AgentRegistry::Clock::now()).size() == 40;
    }, 4s));
    Socket overflow = connect_to(server);
    overflow.send_all(monitor::protocol::encode(
        Message{Hello{server_guid(99), "overflow", "host"}}));
    EXPECT_TRUE(wait_for([&] {
        return overflow.receive_chunk(64).status == ReceiveStatus::Closed;
    }));
    const auto start = std::chrono::steady_clock::now();
    server.stop();
    EXPECT_LT(std::chrono::steady_clock::now() - start, 2s);
}

TEST(ObserverServerIntegration, HelloTimeoutIsFinite) {
    monitor::net::WinsockRuntime runtime;
    AgentRegistry registry;
    IocpServer server(registry, {.bind_address = "127.0.0.1", .port = 0,
                                 .hello_timeout = 100ms});
    ASSERT_TRUE(server.start());
    Socket silent = connect_to(server);
    EXPECT_TRUE(wait_for([&] {
        return silent.receive_chunk(64).status == ReceiveStatus::Closed;
    }, 1s));
    server.stop();
}

}
