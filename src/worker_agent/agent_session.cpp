#include "worker_agent/agent_session.h"

#include <exception>
#include <stdexcept>
#include <utility>
#include <variant>

namespace monitor::worker_agent {

WinsockAgentSession::WinsockAgentSession(AgentSessionConfig config)
    : config_(std::move(config)) {}

bool WinsockAgentSession::connect() {
    disconnect();
    state_ = AgentConnectionState::Connecting;
    last_error_.clear();
    decoder_ = protocol::StreamDecoder{};
    try {
        socket_ = net::Socket::connect_ipv4(config_.endpoint.ipv4_address,
                                            config_.endpoint.port,
                                            config_.connect_timeout);
        socket_.set_receive_timeout(config_.receive_timeout);
        socket_.set_send_timeout(config_.send_timeout);
        // Online begins only after the identity packet has been fully written.
        socket_.send_all(protocol::encode(protocol::Message{config_.hello}));
        state_ = AgentConnectionState::Online;
        return true;
    } catch (const std::exception& error) {
        fail(error.what());
        return false;
    }
}

bool WinsockAgentSession::send_frame(const protocol::Frame& frame) {
    return send_message(protocol::Message{frame});
}

bool WinsockAgentSession::send_heartbeat(const protocol::Heartbeat& heartbeat) {
    return send_message(protocol::Message{heartbeat});
}

std::vector<protocol::SetProfile> WinsockAgentSession::receive_commands() {
    if (state_ != AgentConnectionState::Online) {
        return {};
    }

    try {
        const net::ReceiveResult received =
            socket_.receive_chunk(net::kMaxReceiveChunkSize);
        if (received.status == net::ReceiveStatus::TimedOut) {
            return {};
        }
        if (received.status == net::ReceiveStatus::Closed) {
            fail("observer closed the connection");
            return {};
        }

        const auto messages = decoder_.feed(received.bytes);
        std::vector<protocol::SetProfile> commands;
        commands.reserve(messages.size());
        for (const auto& message : messages) {
            if (!std::holds_alternative<protocol::SetProfile>(message)) {
                throw protocol::ProtocolError(
                    "agent accepts only SetProfile inbound messages");
            }
            commands.push_back(std::get<protocol::SetProfile>(message));
        }
        return commands;
    } catch (const protocol::ProtocolError& error) {
        fail(error.what());
        throw;
    } catch (const std::exception& error) {
        fail(error.what());
        return {};
    }
}

void WinsockAgentSession::disconnect() noexcept {
    socket_.shutdown();
    socket_.close();
    state_ = AgentConnectionState::Disconnected;
}

AgentConnectionState WinsockAgentSession::state() const noexcept {
    return state_;
}

std::string WinsockAgentSession::last_error() const {
    return last_error_;
}

bool WinsockAgentSession::send_message(const protocol::Message& message) {
    if (state_ != AgentConnectionState::Online) {
        return false;
    }
    try {
        socket_.send_all(protocol::encode(message));
        return true;
    } catch (const std::exception& error) {
        fail(error.what());
        return false;
    }
}

void WinsockAgentSession::fail(std::string error) noexcept {
    socket_.shutdown();
    socket_.close();
    state_ = AgentConnectionState::Disconnected;
    last_error_ = std::move(error);
}

}
