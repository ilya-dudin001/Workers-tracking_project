#include "common/winsock_socket.h"

#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <string>

namespace monitor::net {
namespace {

[[nodiscard]] SOCKET as_socket(const std::uintptr_t value) noexcept {
    return static_cast<SOCKET>(value);
}

[[noreturn]] void throw_socket_error(const char* operation) {
    throw std::runtime_error(
        std::string(operation) + " failed with Winsock error " +
        std::to_string(WSAGetLastError()));
}

void require_positive_timeout(const std::chrono::milliseconds timeout) {
    if (timeout <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("socket timeout must be positive");
    }
}

timeval to_timeval(const std::chrono::milliseconds timeout) {
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(timeout);
    const auto remainder =
        std::chrono::duration_cast<std::chrono::microseconds>(timeout - seconds);
    timeval value{};
    value.tv_sec = static_cast<long>(seconds.count());
    value.tv_usec = static_cast<long>(remainder.count());
    return value;
}

}

WinsockRuntime::WinsockRuntime() {
    WSADATA data{};
    const int result = WSAStartup(MAKEWORD(2, 2), &data);
    if (result != 0) {
        throw std::runtime_error(
            "WSAStartup failed with Winsock error " + std::to_string(result));
    }
    if (LOBYTE(data.wVersion) != 2 || HIBYTE(data.wVersion) != 2) {
        WSACleanup();
        throw std::runtime_error("Winsock 2.2 is unavailable");
    }
}

WinsockRuntime::~WinsockRuntime() {
    WSACleanup();
}

Socket::Socket(const std::uintptr_t native) noexcept : native_(native) {}

Socket::~Socket() {
    close();
}

Socket::Socket(Socket&& other) noexcept : native_(other.native_) {
    other.native_ = kInvalid;
}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close();
        native_ = other.native_;
        other.native_ = kInvalid;
    }
    return *this;
}

Socket Socket::tcp() {
    const SOCKET value = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (value == INVALID_SOCKET) {
        throw_socket_error("socket");
    }
    return Socket{static_cast<std::uintptr_t>(value)};
}

Socket Socket::connect_ipv4(const std::string_view address,
                            const std::uint16_t port,
                            const std::chrono::milliseconds timeout,
                            const std::stop_token stop_token) {
    require_positive_timeout(timeout);
    if (port == 0 || address.empty() || address.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("invalid IPv4 endpoint");
    }

    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(port);
    const std::string address_copy(address);
    if (inet_pton(AF_INET, address_copy.c_str(), &endpoint.sin_addr) != 1) {
        throw std::invalid_argument("invalid IPv4 address");
    }

    Socket result = tcp();
    u_long nonblocking = 1;
    if (ioctlsocket(as_socket(result.native_), FIONBIO, &nonblocking) == SOCKET_ERROR) {
        throw_socket_error("ioctlsocket");
    }

    const int connect_result =
        ::connect(as_socket(result.native_),
                  reinterpret_cast<const sockaddr*>(&endpoint),
                  static_cast<int>(sizeof(endpoint)));
    if (connect_result == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
        throw_socket_error("connect");
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (connect_result == SOCKET_ERROR) {
        if (stop_token.stop_requested()) {
            throw std::runtime_error("connect cancelled");
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            throw std::runtime_error("connect timed out");
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const auto slice = std::min(remaining, std::chrono::milliseconds{50});
        timeval wait = to_timeval(std::max(slice, std::chrono::milliseconds{1}));
        fd_set writable;
        FD_ZERO(&writable);
        FD_SET(as_socket(result.native_), &writable);
        fd_set exceptional;
        FD_ZERO(&exceptional);
        FD_SET(as_socket(result.native_), &exceptional);
        const int selected =
            select(0, nullptr, &writable, &exceptional, &wait);
        if (selected == SOCKET_ERROR) {
            throw_socket_error("select");
        }
        if (selected == 0) {
            continue;
        }
        int socket_error = 0;
        int length = static_cast<int>(sizeof(socket_error));
        if (getsockopt(as_socket(result.native_),
                       SOL_SOCKET,
                       SO_ERROR,
                       reinterpret_cast<char*>(&socket_error),
                       &length) == SOCKET_ERROR) {
            throw_socket_error("getsockopt");
        }
        if (socket_error != 0) {
            WSASetLastError(socket_error);
            throw_socket_error("connect");
        }
        break;
    }

    nonblocking = 0;
    if (ioctlsocket(as_socket(result.native_), FIONBIO, &nonblocking) == SOCKET_ERROR) {
        throw_socket_error("ioctlsocket");
    }
    return result;
}

void Socket::bind_loopback(const std::uint16_t port) {
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(port);
    endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(as_socket(native()),
               reinterpret_cast<const sockaddr*>(&endpoint),
               static_cast<int>(sizeof(endpoint))) == SOCKET_ERROR) {
        throw_socket_error("bind");
    }
}

void Socket::listen(const int backlog) {
    if (backlog <= 0) {
        throw std::invalid_argument("listen backlog must be positive");
    }
    if (::listen(as_socket(native()), backlog) == SOCKET_ERROR) {
        throw_socket_error("listen");
    }
}

Socket Socket::accept(const std::chrono::milliseconds timeout) {
    require_positive_timeout(timeout);
    timeval wait = to_timeval(timeout);
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(as_socket(native()), &readable);
    const int selected = select(0, &readable, nullptr, nullptr, &wait);
    if (selected == SOCKET_ERROR) {
        throw_socket_error("select");
    }
    if (selected == 0) {
        throw std::runtime_error("accept timed out");
    }
    const SOCKET accepted = ::accept(as_socket(native()), nullptr, nullptr);
    if (accepted == INVALID_SOCKET) {
        throw_socket_error("accept");
    }
    return Socket{static_cast<std::uintptr_t>(accepted)};
}

std::uint16_t Socket::local_port() const {
    sockaddr_in endpoint{};
    int length = static_cast<int>(sizeof(endpoint));
    if (getsockname(as_socket(native_),
                    reinterpret_cast<sockaddr*>(&endpoint),
                    &length) == SOCKET_ERROR) {
        throw_socket_error("getsockname");
    }
    return ntohs(endpoint.sin_port);
}

void Socket::set_receive_timeout(const std::chrono::milliseconds timeout) {
    require_positive_timeout(timeout);
    const auto capped =
        std::min<std::int64_t>(timeout.count(), std::numeric_limits<DWORD>::max());
    const DWORD milliseconds = static_cast<DWORD>(capped);
    if (setsockopt(as_socket(native()),
                   SOL_SOCKET,
                   SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&milliseconds),
                   static_cast<int>(sizeof(milliseconds))) == SOCKET_ERROR) {
        throw_socket_error("setsockopt");
    }
}

void Socket::set_send_timeout(const std::chrono::milliseconds timeout) {
    require_positive_timeout(timeout);
    const auto capped =
        std::min<std::int64_t>(timeout.count(), std::numeric_limits<DWORD>::max());
    const DWORD milliseconds = static_cast<DWORD>(capped);
    if (setsockopt(as_socket(native()),
                   SOL_SOCKET,
                   SO_SNDTIMEO,
                   reinterpret_cast<const char*>(&milliseconds),
                   static_cast<int>(sizeof(milliseconds))) == SOCKET_ERROR) {
        throw_socket_error("setsockopt");
    }
}

void Socket::send_all(const std::span<const std::uint8_t> bytes) {
    send_all(bytes, std::chrono::seconds(30), {});
}

void Socket::send_all(const std::span<const std::uint8_t> bytes,
                      const std::chrono::milliseconds timeout,
                      const std::stop_token stop_token) {
    require_positive_timeout(timeout);
    const SOCKET socket = as_socket(native());
    u_long nonblocking = 1;
    if (ioctlsocket(socket, FIONBIO, &nonblocking) == SOCKET_ERROR) {
        throw_socket_error("ioctlsocket");
    }
    struct RestoreBlocking {
        SOCKET socket;
        ~RestoreBlocking() {
            u_long blocking = 0;
            static_cast<void>(ioctlsocket(socket, FIONBIO, &blocking));
        }
    } restore{socket};

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        if (stop_token.stop_requested()) {
            throw std::runtime_error("send cancelled");
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            throw std::runtime_error("send timed out");
        }
        const std::size_t remaining = bytes.size() - sent;
        const int chunk_size = static_cast<int>(
            std::min<std::size_t>(remaining, static_cast<std::size_t>(INT_MAX)));
        const int result =
            ::send(socket,
                   reinterpret_cast<const char*>(bytes.data() + sent),
                   chunk_size,
                   0);
        if (result == SOCKET_ERROR) {
            const int error = WSAGetLastError();
            if (error != WSAEWOULDBLOCK) {
                throw_socket_error("send");
            }
            const auto remaining_time =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
            const auto slice =
                std::clamp(remaining_time, std::chrono::milliseconds{1},
                           std::chrono::milliseconds{50});
            timeval wait = to_timeval(slice);
            fd_set writable;
            FD_ZERO(&writable);
            FD_SET(socket, &writable);
            const int selected = select(0, nullptr, &writable, nullptr, &wait);
            if (selected == SOCKET_ERROR) {
                throw_socket_error("select");
            }
            continue;
        }
        if (result == 0) {
            throw std::runtime_error("send returned zero");
        }
        sent += static_cast<std::size_t>(result);
    }
}

ReceiveResult Socket::receive_chunk(const std::size_t maximum_bytes) {
    if (maximum_bytes == 0 || maximum_bytes > kMaxReceiveChunkSize) {
        throw std::invalid_argument("receive chunk size is out of bounds");
    }
    std::vector<std::uint8_t> bytes(maximum_bytes);
    const int result =
        recv(as_socket(native()),
             reinterpret_cast<char*>(bytes.data()),
             static_cast<int>(bytes.size()),
             0);
    if (result > 0) {
        bytes.resize(static_cast<std::size_t>(result));
        return {ReceiveStatus::Data, std::move(bytes)};
    }
    if (result == 0) {
        return {ReceiveStatus::Closed, {}};
    }
    const int error = WSAGetLastError();
    if (error == WSAETIMEDOUT || error == WSAEWOULDBLOCK) {
        return {ReceiveStatus::TimedOut, {}};
    }
    throw_socket_error("recv");
}

void Socket::shutdown() noexcept {
    if (valid()) {
        static_cast<void>(::shutdown(as_socket(native_), SD_BOTH));
    }
}

void Socket::close() noexcept {
    if (valid()) {
        closesocket(as_socket(native_));
        native_ = kInvalid;
    }
}

bool Socket::valid() const noexcept {
    return native_ != kInvalid;
}

std::uintptr_t Socket::native() const {
    if (!valid()) {
        throw std::logic_error("socket is closed");
    }
    return native_;
}

}
