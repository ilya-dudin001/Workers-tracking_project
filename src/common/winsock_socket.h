#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stop_token>
#include <string_view>
#include <vector>

namespace monitor::net {

inline constexpr std::size_t kMaxReceiveChunkSize = 64U * 1024U;

class WinsockRuntime {
public:
    WinsockRuntime();
    ~WinsockRuntime();

    WinsockRuntime(const WinsockRuntime&) = delete;
    WinsockRuntime& operator=(const WinsockRuntime&) = delete;
};

enum class ReceiveStatus {
    Data,
    Closed,
    TimedOut,
};

struct ReceiveResult {
    ReceiveStatus status;
    std::vector<std::uint8_t> bytes;
};

// Socket is move-only and single-owner. Callers serialize operations on one instance.
class Socket {
public:
    Socket() noexcept = default;
    ~Socket();

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    [[nodiscard]] static Socket tcp();
    [[nodiscard]] static Socket connect_ipv4(
        std::string_view address,
        std::uint16_t port,
        std::chrono::milliseconds timeout,
        std::stop_token stop_token = {});

    void bind_loopback(std::uint16_t port);
    void listen(int backlog = 1);
    [[nodiscard]] Socket accept(std::chrono::milliseconds timeout);
    [[nodiscard]] std::uint16_t local_port() const;

    void set_receive_timeout(std::chrono::milliseconds timeout);
    void set_send_timeout(std::chrono::milliseconds timeout);
    void send_all(std::span<const std::uint8_t> bytes);
    [[nodiscard]] ReceiveResult receive_chunk(std::size_t maximum_bytes);
    void shutdown() noexcept;
    void close() noexcept;
    [[nodiscard]] bool valid() const noexcept;

private:
    explicit Socket(std::uintptr_t native) noexcept;
    [[nodiscard]] std::uintptr_t native() const;

    static constexpr std::uintptr_t kInvalid = ~std::uintptr_t{0};
    std::uintptr_t native_ = kInvalid;
};

}
