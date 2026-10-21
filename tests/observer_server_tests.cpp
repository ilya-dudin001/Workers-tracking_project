#include "common/winsock_socket.h"
#include "common/stream_decoder.h"
#include "observer/focus_router.h"
#include "observer/iocp_server.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <thread>
#include <variant>
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

Socket connect_to(const IocpServer& server,
                  const std::chrono::milliseconds timeout = 1s) {
    Socket socket = Socket::connect_ipv4("127.0.0.1", server.port(), timeout);
    socket.set_receive_timeout(200ms);
    socket.set_send_timeout(1s);
    return socket;
}

std::optional<monitor::protocol::SetProfile> receive_profile(
    Socket& socket, const std::chrono::milliseconds timeout = 2s) {
    monitor::protocol::StreamDecoder decoder;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto received = socket.receive_chunk(256);
        if (received.status == ReceiveStatus::Closed) return std::nullopt;
        if (received.status != ReceiveStatus::Data) continue;
        for (const auto& message : decoder.feed(received.bytes)) {
            if (const auto* profile =
                    std::get_if<monitor::protocol::SetProfile>(&message)) {
                return *profile;
            }
        }
    }
    return std::nullopt;
}

bool drain_until_closed(Socket& socket,
                        const std::chrono::milliseconds timeout = 2s) {
    return wait_for([&] {
        try {
            return socket.receive_chunk(256).status == ReceiveStatus::Closed;
        } catch (const std::runtime_error&) {
            return true;
        }
    }, timeout);
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

TEST(ObserverServerIntegration, RejectsHelloProcessedAfterDeadline) {
    monitor::net::WinsockRuntime runtime;
    AgentRegistry registry;
    IocpServer server(registry, {.bind_address = "127.0.0.1", .port = 0,
                                 .hello_timeout = 50ms,
                                 .monitor_poll_interval = 5s});
    ASSERT_TRUE(server.start());
    Socket late = connect_to(server);
    std::this_thread::sleep_for(80ms);
    try {
        late.send_all(monitor::protocol::encode(
            Message{Hello{server_guid(8), "late", "host"}}));
    } catch (const std::runtime_error&) {
    }
    EXPECT_TRUE(drain_until_closed(late, 1s));
    EXPECT_TRUE(registry.snapshot(AgentRegistry::Clock::now()).empty());
    EXPECT_EQ(server.timeout_monitor_passes(), 0U);
    server.stop();
}

TEST(ObserverServerIntegration, ConcurrentDuplicateHelloHasSingleNewestOwner) {
    monitor::net::WinsockRuntime runtime;
    AgentRegistry registry;
    IocpServer server(registry, {.bind_address = "127.0.0.1", .port = 0});
    ASSERT_TRUE(server.start());
    Socket first = connect_to(server);
    Socket second = connect_to(server);
    std::barrier start(3);
    auto send_hello = [&](Socket& socket, const char* name) {
        start.arrive_and_wait();
        socket.send_all(monitor::protocol::encode(
            Message{Hello{server_guid(9), name, "host"}}));
    };
    auto one = std::async(std::launch::async, send_hello,
                          std::ref(first), "one");
    auto two = std::async(std::launch::async, send_hello,
                          std::ref(second), "two");
    start.arrive_and_wait();
    one.get();
    two.get();
    ASSERT_TRUE(wait_for([&] {
        const auto items = registry.snapshot(AgentRegistry::Clock::now());
        return items.size() == 1 && items[0].generation >= 2U &&
               items[0].status == monitor::observer::AgentStatus::Online;
    }));
    const auto owner_name =
        registry.snapshot(AgentRegistry::Clock::now())[0].employee_name;
    Socket* owner = owner_name == "one" ? &first : &second;
    Socket* replaced = owner_name == "one" ? &second : &first;
    ASSERT_TRUE(drain_until_closed(*replaced));
    owner->send_all(monitor::protocol::encode(
        Message{Frame{77, 16, 9, {1, 2, 3}}}));
    ASSERT_TRUE(wait_for([&] {
        const auto items = registry.snapshot(AgentRegistry::Clock::now());
        return items[0].frame && items[0].frame->sequence == 77 &&
               items[0].status == monitor::observer::AgentStatus::Online;
    }));
    server.stop();
}

TEST(ObserverServerIntegration, FocusedReconnectGetsFocusAndOldCannotUpdate) {
    monitor::net::WinsockRuntime runtime;
    AgentRegistry registry;
    IocpServer server(registry, {.bind_address = "127.0.0.1", .port = 0});
    auto router = std::make_shared<monitor::observer::FocusRouter>(server);
    router->select(server_guid(10));
    const std::weak_ptr<monitor::observer::FocusRouter> weak_router = router;
    server.set_registration_handler(
        [weak_router](const AgentGuid& id) {
            if (const auto current = weak_router.lock()) {
                current->agent_registered(id);
            }
        });
    ASSERT_TRUE(server.start());
    Socket old = connect_to(server);
    old.send_all(monitor::protocol::encode(
        Message{Hello{server_guid(10), "old", "host"}}));
    ASSERT_EQ(receive_profile(old), monitor::observer::FocusRouter::focus_profile());

    Socket replacement = connect_to(server);
    replacement.send_all(monitor::protocol::encode(
        Message{Hello{server_guid(10), "new", "host"}}));
    ASSERT_EQ(receive_profile(replacement),
              monitor::observer::FocusRouter::focus_profile());
    try {
        old.send_all(monitor::protocol::encode(
            Message{Frame{87, 16, 9, {1, 2, 3}}}));
    } catch (const std::runtime_error&) {
    }
    ASSERT_TRUE(drain_until_closed(old));
    EXPECT_FALSE(registry.snapshot(AgentRegistry::Clock::now())[0].frame);
    replacement.send_all(monitor::protocol::encode(
        Message{Frame{88, 16, 9, {1, 2, 3}}}));
    ASSERT_TRUE(wait_for([&] {
        const auto items = registry.snapshot(AgentRegistry::Clock::now());
        return items.size() == 1 && items[0].employee_name == "new" &&
               items[0].frame && items[0].frame->sequence == 88;
    }));
    server.stop();
}

TEST(ObserverServerIntegration, StopRacingAcceptLeavesNoConnectionsOnline) {
    monitor::net::WinsockRuntime runtime;
    for (int repetition = 0; repetition < 100; ++repetition) {
        AgentRegistry registry;
        std::atomic<int> accepted{0};
        std::promise<void> second_accepted_promise;
        auto second_accepted = second_accepted_promise.get_future();
        std::promise<void> release_promise;
        auto release = release_promise.get_future().share();
        IocpServer server(registry, {
            .bind_address = "127.0.0.1",
            .port = 0,
            .connection_accepted_handler = [&] {
                if (++accepted == 2) {
                    second_accepted_promise.set_value();
                    release.wait();
                }
            }});
        ASSERT_TRUE(server.start());
        Socket established = connect_to(server);
        ASSERT_TRUE(wait_for([&] { return accepted.load() >= 1; }));

        std::jthread racing_connector([&] {
            Socket racing = connect_to(server);
            static_cast<void>(racing);
        });
        if (second_accepted.wait_for(1s) != std::future_status::ready) {
            release_promise.set_value();
            racing_connector.join();
            server.stop();
            ADD_FAILURE() << "second accept did not reach completion seam";
            continue;
        }
        EXPECT_GE(server.connection_count(), 2U);
        EXPECT_GT(server.in_flight_operation_count(), 0U);
        const auto started = std::chrono::steady_clock::now();
        auto stopping = std::async(std::launch::async, [&] { server.stop(); });
        EXPECT_EQ(stopping.wait_for(20ms), std::future_status::timeout);
        release_promise.set_value();
        ASSERT_EQ(stopping.wait_for(2s), std::future_status::ready);
        stopping.get();
        racing_connector.join();
        EXPECT_LT(std::chrono::steady_clock::now() - started, 2s);
        EXPECT_EQ(server.connection_count(), 0U);
        EXPECT_EQ(server.in_flight_operation_count(), 0U);
        for (const auto& item : registry.snapshot(AgentRegistry::Clock::now())) {
            EXPECT_NE(item.status, monitor::observer::AgentStatus::Online);
        }
    }
}

}
