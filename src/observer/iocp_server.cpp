#include "observer/iocp_server.h"

#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>

#include "common/stream_decoder.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace monitor::observer {
namespace {

constexpr std::size_t kReceiveBytes = 64U * 1024U;
constexpr std::size_t kControlQueueCapacity = 8;

std::string socket_error(const char* operation) {
    return std::string(operation) + " failed: " + std::to_string(WSAGetLastError());
}

}

class IocpServer::Impl {
public:
    enum class Lifecycle { Stopped, Starting, Running, Stopping };
    struct Connection;
    enum class OperationKind { Accept, Receive, Send };
    struct Operation {
        OVERLAPPED overlapped{};
        OperationKind kind;
        std::shared_ptr<Connection> connection;
        std::array<std::uint8_t, kReceiveBytes> receive_buffer{};
        std::array<std::uint8_t, (sizeof(sockaddr_in) + 16U) * 2U> accept_buffer{};
        SOCKET accepted_socket = INVALID_SOCKET;
        protocol::Bytes send_buffer;
        std::size_t send_offset = 0;

        Operation(OperationKind value, std::shared_ptr<Connection> owner)
            : kind(value), connection(std::move(owner)) {}
    };

    struct Connection : std::enable_shared_from_this<Connection> {
        Impl* server;
        SOCKET socket;
        protocol::StreamDecoder decoder;
        std::mutex send_mutex;
        std::deque<protocol::Bytes> control_queue;
        bool send_active = false;
        std::atomic<bool> closed{false};
        const AgentRegistry::Clock::time_point hello_deadline;
        // Identity fields are read and written only while state_mutex_ is held.
        std::optional<protocol::AgentGuid> guid;
        std::uint64_t generation = 0;

        Connection(Impl& owner, const SOCKET value)
            : server(&owner), socket(value),
              hello_deadline(AgentRegistry::Clock::now() +
                             owner.config_.hello_timeout) {}
    };

    Impl(AgentRegistry& registry, Config config)
        : registry_(registry), config_(std::move(config)) {}

    ~Impl() {
        stop();
    }

    bool start() {
        {
            std::scoped_lock lock(state_mutex_);
            if (lifecycle_ == Lifecycle::Running) return true;
            if (lifecycle_ != Lifecycle::Stopped) return false;
            lifecycle_ = Lifecycle::Starting;
        }
        iocp_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
        if (iocp_ == nullptr) {
            return fail("CreateIoCompletionPort failed");
        }
        listener_ = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0,
                               WSA_FLAG_OVERLAPPED);
        if (listener_ == INVALID_SOCKET) {
            return fail(socket_error("WSASocket"));
        }
        BOOL reuse = TRUE;
        static_cast<void>(setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR,
            reinterpret_cast<const char*>(&reuse), sizeof(reuse)));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(config_.port);
        if (InetPtonA(AF_INET, config_.bind_address.c_str(),
                      &address.sin_addr) != 1) {
            return fail("invalid IPv4 bind address");
        }
        if (bind(listener_, reinterpret_cast<const sockaddr*>(&address),
                 sizeof(address)) == SOCKET_ERROR ||
            listen(listener_, SOMAXCONN) == SOCKET_ERROR) {
            return fail(socket_error("bind/listen"));
        }
        sockaddr_in actual{};
        int actual_size = sizeof(actual);
        if (getsockname(listener_, reinterpret_cast<sockaddr*>(&actual),
                        &actual_size) == SOCKET_ERROR) {
            return fail(socket_error("getsockname"));
        }
        port_ = ntohs(actual.sin_port);
        if (CreateIoCompletionPort(reinterpret_cast<HANDLE>(listener_),
                                   iocp_, 0, 0) == nullptr) {
            return fail("listener IOCP association failed");
        }
        GUID accept_ex_guid = WSAID_ACCEPTEX;
        DWORD function_bytes = 0;
        if (WSAIoctl(listener_, SIO_GET_EXTENSION_FUNCTION_POINTER,
            &accept_ex_guid, sizeof(accept_ex_guid), &accept_ex_,
            sizeof(accept_ex_), &function_bytes, nullptr, nullptr) == SOCKET_ERROR) {
            return fail(socket_error("AcceptEx discovery"));
        }
        const std::size_t count = config_.worker_threads == 0
            ? std::clamp<std::size_t>(std::thread::hardware_concurrency(), 2, 8)
            : std::clamp<std::size_t>(config_.worker_threads, 1, 16);
        try {
            workers_.reserve(count);
            for (std::size_t index = 0; index < count; ++index) {
                workers_.emplace_back([this] { worker_loop(); });
            }
            for (std::size_t index = 0; index < 4U; ++index) {
                post_accept();
            }
            monitor_thread_ = std::thread([this] { monitor_loop(); });
            {
                std::scoped_lock lock(state_mutex_);
                lifecycle_ = Lifecycle::Running;
            }
            lifecycle_cv_.notify_all();
        } catch (...) {
            {
                std::scoped_lock lock(state_mutex_);
                lifecycle_ = Lifecycle::Running;
            }
            lifecycle_cv_.notify_all();
            stop();
            throw;
        }
        return true;
    }

    void stop() noexcept {
        SOCKET listener = INVALID_SOCKET;
        std::vector<std::shared_ptr<Connection>> connections;
        {
            std::unique_lock lock(state_mutex_);
            lifecycle_cv_.wait(lock, [this] {
                return lifecycle_ != Lifecycle::Starting;
            });
            if (lifecycle_ == Lifecycle::Stopped) return;
            if (lifecycle_ == Lifecycle::Stopping) {
                lifecycle_cv_.wait(lock, [this] {
                    return lifecycle_ == Lifecycle::Stopped;
                });
                return;
            }
            lifecycle_ = Lifecycle::Stopping;
            listener = std::exchange(listener_, INVALID_SOCKET);
            connections.reserve(connections_.size());
            for (const auto& [socket, connection] : connections_) {
                static_cast<void>(socket);
                connections.push_back(connection);
            }
        }
        if (listener != INVALID_SOCKET) {
            closesocket(listener);
        }
        if (monitor_thread_.joinable()) {
            monitor_thread_.join();
        }
        for (const auto& connection : connections) {
            close_connection(connection);
        }
        {
            std::unique_lock lock(operation_mutex_);
            operation_cv_.wait(lock,
                               [this] { return active_operations_ == 0; });
        }
        if (iocp_ != nullptr) {
            for (std::size_t index = 0; index < workers_.size(); ++index) {
                static_cast<void>(PostQueuedCompletionStatus(iocp_, 0, 0, nullptr));
            }
        }
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        workers_.clear();
        if (iocp_ != nullptr) {
            CloseHandle(iocp_);
            iocp_ = nullptr;
        }
        {
            std::scoped_lock lock(state_mutex_);
            lifecycle_ = Lifecycle::Stopped;
        }
        lifecycle_cv_.notify_all();
    }

    bool send_profile(const protocol::AgentGuid& guid,
                      const protocol::SetProfile& profile) {
        std::shared_ptr<Connection> connection;
        {
            std::scoped_lock lock(state_mutex_);
            if (lifecycle_ != Lifecycle::Running) return false;
            const auto found = by_guid_.find(guid);
            if (found == by_guid_.end()) {
                return false;
            }
            connection = found->second;
        }
        if (!connection || connection->closed) {
            return false;
        }
        protocol::Bytes packet;
        try {
            packet = protocol::encode(protocol::Message{profile});
        } catch (...) {
            return false;
        }
        bool begin_send = false;
        {
            std::scoped_lock lock(connection->send_mutex);
            if (connection->closed ||
                connection->control_queue.size() >= kControlQueueCapacity) {
                return false;
            }
            connection->control_queue.push_back(std::move(packet));
            if (!connection->send_active) {
                connection->send_active = true;
                begin_send = true;
            }
        }
        if (begin_send) {
            post_next_send(connection);
        }
        return true;
    }

    std::uint16_t port() const noexcept {
        return port_;
    }

    std::string last_error() const {
        std::scoped_lock lock(error_mutex_);
        return last_error_;
    }

    std::size_t connection_count() const {
        std::scoped_lock lock(state_mutex_);
        return connections_.size();
    }

    void set_registration_handler(
        std::function<void(const protocol::AgentGuid&)> handler) {
        std::scoped_lock lock(state_mutex_);
        registration_handler_ = std::move(handler);
    }

private:
    bool fail(std::string message) {
        {
            std::scoped_lock lock(error_mutex_);
            last_error_ = std::move(message);
        }
        SOCKET listener = INVALID_SOCKET;
        {
            std::scoped_lock lock(state_mutex_);
            lifecycle_ = Lifecycle::Stopped;
            listener = std::exchange(listener_, INVALID_SOCKET);
        }
        lifecycle_cv_.notify_all();
        if (listener != INVALID_SOCKET) closesocket(listener);
        if (iocp_ != nullptr) {
            CloseHandle(iocp_);
            iocp_ = nullptr;
        }
        return false;
    }

    void monitor_loop() {
        for (;;) {
            {
                std::unique_lock lock(state_mutex_);
                lifecycle_cv_.wait(lock, [this] {
                    return lifecycle_ != Lifecycle::Starting;
                });
                if (lifecycle_ != Lifecycle::Running) return;
            }
            expire_hello_deadlines();
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
    }

    void post_accept() {
        auto operation = std::make_unique<Operation>(
            OperationKind::Accept, std::shared_ptr<Connection>{});
        operation->accepted_socket = WSASocketW(
            AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
        if (operation->accepted_socket == INVALID_SOCKET) return;
        DWORD received = 0;
        BOOL result = FALSE;
        {
            std::scoped_lock lock(state_mutex_);
            if ((lifecycle_ != Lifecycle::Running &&
                 lifecycle_ != Lifecycle::Starting) ||
                listener_ == INVALID_SOCKET) {
                closesocket(operation->accepted_socket);
                return;
            }
            add_operation();
            result = accept_ex_(
                listener_, operation->accepted_socket,
                operation->accept_buffer.data(), 0,
                sizeof(sockaddr_in) + 16U, sizeof(sockaddr_in) + 16U,
                &received, &operation->overlapped);
        }
        if (!result && WSAGetLastError() != ERROR_IO_PENDING) {
            finish_operation();
            closesocket(operation->accepted_socket);
            return;
        }
        static_cast<void>(operation.release());
    }

    void accept_connection(const SOCKET socket) {
        std::shared_ptr<Connection> connection;
        {
            std::scoped_lock lock(state_mutex_);
            if (lifecycle_ != Lifecycle::Running ||
                listener_ == INVALID_SOCKET) {
                closesocket(socket);
                return;
            }
            if (setsockopt(socket, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
                           reinterpret_cast<const char*>(&listener_),
                           sizeof(listener_)) == SOCKET_ERROR ||
                CreateIoCompletionPort(reinterpret_cast<HANDLE>(socket),
                                       iocp_, 0, 0) == nullptr) {
                closesocket(socket);
                return;
            }
            connection = std::make_shared<Connection>(*this, socket);
            const auto [iterator, inserted] =
                connections_.emplace(socket, connection);
            static_cast<void>(iterator);
            if (!inserted) {
                closesocket(socket);
                return;
            }
        }
        post_receive(connection);
    }

    void expire_hello_deadlines() {
        std::vector<std::shared_ptr<Connection>> expired;
        const auto now = AgentRegistry::Clock::now();
        {
            std::scoped_lock lock(state_mutex_);
            for (const auto& [socket, connection] : connections_) {
                static_cast<void>(socket);
                if (!connection->guid && now > connection->hello_deadline) {
                    expired.push_back(connection);
                }
            }
        }
        for (const auto& connection : expired) {
            close_connection(connection);
        }
    }

    void add_operation() {
        std::scoped_lock lock(operation_mutex_);
        ++active_operations_;
    }

    void finish_operation() {
        std::scoped_lock lock(operation_mutex_);
        if (--active_operations_ == 0) {
            operation_cv_.notify_all();
        }
    }

    void post_receive(const std::shared_ptr<Connection>& connection) {
        auto operation = std::make_unique<Operation>(
            OperationKind::Receive, connection);
        WSABUF buffer{
            static_cast<ULONG>(operation->receive_buffer.size()),
            reinterpret_cast<char*>(operation->receive_buffer.data())};
        DWORD flags = 0;
        DWORD received = 0;
        int result = SOCKET_ERROR;
        {
            std::scoped_lock lock(state_mutex_);
            if (lifecycle_ != Lifecycle::Running || connection->closed) {
                return;
            }
            add_operation();
            result = WSARecv(connection->socket, &buffer, 1, &received,
                             &flags, &operation->overlapped, nullptr);
        }
        if (result == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
            finish_operation();
            close_connection(connection);
            return;
        }
        static_cast<void>(operation.release());
    }

    void post_next_send(const std::shared_ptr<Connection>& connection) {
        auto operation = std::make_unique<Operation>(
            OperationKind::Send, connection);
        {
            std::scoped_lock lock(connection->send_mutex);
            if (connection->closed || connection->control_queue.empty()) {
                connection->send_active = false;
                return;
            }
            operation->send_buffer = std::move(connection->control_queue.front());
            connection->control_queue.pop_front();
        }
        issue_send(std::move(operation));
    }

    void issue_send(std::unique_ptr<Operation> operation) {
        WSABUF buffer{
            static_cast<ULONG>(operation->send_buffer.size() -
                               operation->send_offset),
            reinterpret_cast<char*>(operation->send_buffer.data() +
                                    operation->send_offset)};
        DWORD sent = 0;
        int result = SOCKET_ERROR;
        {
            std::scoped_lock lock(state_mutex_);
            if (lifecycle_ != Lifecycle::Running ||
                operation->connection->closed) {
                return;
            }
            add_operation();
            result = WSASend(operation->connection->socket, &buffer, 1,
                             &sent, 0, &operation->overlapped, nullptr);
        }
        if (result == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
            finish_operation();
            close_connection(operation->connection);
            return;
        }
        static_cast<void>(operation.release());
    }

    void worker_loop() {
        for (;;) {
            DWORD bytes = 0;
            ULONG_PTR key = 0;
            OVERLAPPED* raw = nullptr;
            const BOOL succeeded = GetQueuedCompletionStatus(
                iocp_, &bytes, &key, &raw, INFINITE);
            static_cast<void>(key);
            if (raw == nullptr) {
                return;
            }
            std::unique_ptr<Operation> operation(
                CONTAINING_RECORD(raw, Operation, overlapped));
            if (operation->kind == OperationKind::Accept) {
                const SOCKET accepted = operation->accepted_socket;
                operation->accepted_socket = INVALID_SOCKET;
                if (succeeded) {
                    accept_connection(accepted);
                } else {
                    closesocket(accepted);
                }
                post_accept();
                finish_operation();
                continue;
            }
            const auto connection = operation->connection;
            if (!succeeded || bytes == 0 || connection->closed) {
                close_connection(connection);
                finish_operation();
                continue;
            }
            if (operation->kind == OperationKind::Receive) {
                handle_receive(connection,
                    {operation->receive_buffer.data(), bytes});
                if (!connection->closed) {
                    post_receive(connection);
                }
                finish_operation();
            } else {
                operation->send_offset += bytes;
                if (operation->send_offset < operation->send_buffer.size()) {
                    ZeroMemory(&operation->overlapped, sizeof(OVERLAPPED));
                    issue_send(std::move(operation));
                } else {
                    post_next_send(connection);
                }
                finish_operation();
            }
        }
    }

    std::optional<std::pair<protocol::AgentGuid, std::uint64_t>>
    connection_identity(const std::shared_ptr<Connection>& connection) const {
        std::scoped_lock lock(state_mutex_);
        if (!connection->guid || connection->closed) {
            return std::nullopt;
        }
        return std::make_pair(*connection->guid, connection->generation);
    }

    void handle_receive(const std::shared_ptr<Connection>& connection,
                        const std::span<const std::uint8_t> bytes) {
        try {
            const auto messages = connection->decoder.feed(bytes);
            for (const auto& message : messages) {
                const auto identity = connection_identity(connection);
                if (!identity) {
                    const auto* hello = std::get_if<protocol::Hello>(&message);
                    if (hello == nullptr || !handle_hello(connection, *hello)) {
                        close_connection(connection);
                        return;
                    }
                    continue;
                }
                if (const auto* frame = std::get_if<protocol::Frame>(&message)) {
                    if (frame->width == 0 || frame->height == 0 ||
                        frame->width > 3840 || frame->height > 2160 ||
                        frame->encoded_image.empty()) {
                        close_connection(connection);
                        return;
                    }
                    static_cast<void>(registry_.update_frame(
                        identity->first, identity->second, *frame,
                        AgentRegistry::Clock::now()));
                } else if (std::holds_alternative<protocol::Heartbeat>(message)) {
                    static_cast<void>(registry_.heartbeat(
                        identity->first, identity->second,
                        AgentRegistry::Clock::now()));
                } else {
                    close_connection(connection);
                    return;
                }
            }
        } catch (...) {
            close_connection(connection);
        }
    }

    bool handle_hello(const std::shared_ptr<Connection>& connection,
                      const protocol::Hello& hello) {
        std::shared_ptr<Connection> replaced;
        std::function<void(const protocol::AgentGuid&)> handler;
        {
            std::scoped_lock lock(state_mutex_);
            const auto now = AgentRegistry::Clock::now();
            if (lifecycle_ != Lifecycle::Running || connection->closed ||
                connection->guid || now > connection->hello_deadline) {
                return false;
            }
            const auto generation = registry_.register_agent(
                hello.agent_guid, hello.employee_name, hello.hostname, now);
            if (!generation) {
                return false;
            }
            connection->guid = hello.agent_guid;
            connection->generation = *generation;
            const auto found = by_guid_.find(hello.agent_guid);
            if (found != by_guid_.end()) {
                replaced = found->second;
            }
            by_guid_[hello.agent_guid] = connection;
            handler = registration_handler_;
        }
        if (replaced && replaced != connection) {
            close_connection(replaced);
        }
        if (handler) {
            handler(hello.agent_guid);
        } else {
            static_cast<void>(send_profile(hello.agent_guid,
                                           FocusRouter::grid_profile()));
        }
        return true;
    }

    void close_connection(const std::shared_ptr<Connection>& connection) noexcept {
        if (connection->closed.exchange(true)) {
            return;
        }
        std::optional<std::pair<protocol::AgentGuid, std::uint64_t>> identity;
        {
            std::scoped_lock lock(state_mutex_);
            if (connection->guid) {
                identity = std::make_pair(*connection->guid,
                                          connection->generation);
                const auto found = by_guid_.find(*connection->guid);
                if (found != by_guid_.end() &&
                    found->second == connection) {
                    by_guid_.erase(found);
                }
            }
            const auto found = connections_.find(connection->socket);
            if (found != connections_.end() && found->second == connection) {
                connections_.erase(found);
            }
        }
        static_cast<void>(shutdown(connection->socket, SD_BOTH));
        static_cast<void>(CancelIoEx(
            reinterpret_cast<HANDLE>(connection->socket), nullptr));
        closesocket(connection->socket);
        if (identity) {
            registry_.disconnect(identity->first, identity->second,
                                 AgentRegistry::Clock::now());
        }
    }

    AgentRegistry& registry_;
    Config config_;
    mutable std::mutex state_mutex_;
    std::condition_variable lifecycle_cv_;
    Lifecycle lifecycle_ = Lifecycle::Stopped;
    SOCKET listener_ = INVALID_SOCKET;
    HANDLE iocp_ = nullptr;
    std::uint16_t port_ = 0;
    LPFN_ACCEPTEX accept_ex_ = nullptr;
    std::thread monitor_thread_;
    std::vector<std::thread> workers_;
    mutable std::mutex error_mutex_;
    std::string last_error_;
    std::unordered_map<SOCKET, std::shared_ptr<Connection>> connections_;
    std::map<protocol::AgentGuid, std::shared_ptr<Connection>> by_guid_;
    std::function<void(const protocol::AgentGuid&)> registration_handler_;
    std::mutex operation_mutex_;
    std::condition_variable operation_cv_;
    std::size_t active_operations_ = 0;
};

IocpServer::IocpServer(AgentRegistry& registry, Config config)
    : impl_(std::make_unique<Impl>(registry, std::move(config))) {}

IocpServer::~IocpServer() = default;

bool IocpServer::start() {
    return impl_->start();
}

void IocpServer::stop() noexcept {
    impl_->stop();
}

std::uint16_t IocpServer::port() const noexcept {
    return impl_->port();
}

std::string IocpServer::last_error() const {
    return impl_->last_error();
}

std::size_t IocpServer::connection_count() const {
    return impl_->connection_count();
}

void IocpServer::set_registration_handler(
    std::function<void(const protocol::AgentGuid&)> handler) {
    impl_->set_registration_handler(std::move(handler));
}

bool IocpServer::send_profile(const protocol::AgentGuid& guid,
                              const protocol::SetProfile& profile) {
    return impl_->send_profile(guid, profile);
}

}
