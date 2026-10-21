#pragma once

#include "common/protocol.h"
#include "common/stream_decoder.h"
#include "common/winsock_socket.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace monitor::worker_agent {

enum class AgentConnectionState {
    Disconnected,
    Connecting,
    Online,
};

struct AgentEndpoint {
    std::string ipv4_address;
    std::uint16_t port;
};

struct AgentSessionConfig {
    AgentEndpoint endpoint;
    protocol::Hello hello;
    std::chrono::milliseconds connect_timeout{1000};
    std::chrono::milliseconds receive_timeout{50};
    std::chrono::milliseconds send_timeout{1000};
};

// The later tray loop owns and calls a session from one thread; no hidden worker exists.
// Operational socket/protocol failures disconnect, set last_error, and do not escape.
class AgentTransport {
public:
    virtual ~AgentTransport() = default;
    virtual bool connect() = 0;
    virtual bool send_frame(const protocol::Frame& frame) = 0;
    virtual bool send_heartbeat(const protocol::Heartbeat& heartbeat) = 0;
    [[nodiscard]] virtual std::vector<protocol::SetProfile> receive_commands() = 0;
    virtual void disconnect() noexcept = 0;
    [[nodiscard]] virtual AgentConnectionState state() const noexcept = 0;
    [[nodiscard]] virtual std::string last_error() const = 0;
};

class WinsockAgentSession final : public AgentTransport {
public:
    explicit WinsockAgentSession(AgentSessionConfig config);

    bool connect() override;
    bool send_frame(const protocol::Frame& frame) override;
    bool send_heartbeat(const protocol::Heartbeat& heartbeat) override;
    [[nodiscard]] std::vector<protocol::SetProfile> receive_commands() override;
    void disconnect() noexcept override;
    [[nodiscard]] AgentConnectionState state() const noexcept override;
    [[nodiscard]] std::string last_error() const override;

private:
    bool send_message(const protocol::Message& message);
    void fail(std::string error) noexcept;

    AgentSessionConfig config_;
    net::WinsockRuntime runtime_;
    net::Socket socket_;
    protocol::StreamDecoder decoder_;
    AgentConnectionState state_ = AgentConnectionState::Disconnected;
    std::string last_error_;
};

}
