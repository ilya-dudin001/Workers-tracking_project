#pragma once

#include "observer/agent_registry.h"
#include "observer/focus_router.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace monitor::observer {

class IocpServer final : public ProfileSink {
public:
    struct Config {
        std::string bind_address = "0.0.0.0";
        std::uint16_t port = 45873;
        std::chrono::milliseconds hello_timeout{3000};
        std::chrono::milliseconds monitor_poll_interval{25};
        std::size_t worker_threads = 0;
        std::function<void()> connection_accepted_handler;
    };

    explicit IocpServer(AgentRegistry& registry, Config config = {});
    ~IocpServer();
    IocpServer(const IocpServer&) = delete;
    IocpServer& operator=(const IocpServer&) = delete;

    [[nodiscard]] bool start();
    void stop() noexcept;
    [[nodiscard]] std::uint16_t port() const noexcept;
    [[nodiscard]] std::string last_error() const;
    [[nodiscard]] std::size_t connection_count() const;
    [[nodiscard]] std::size_t in_flight_operation_count() const;
    [[nodiscard]] std::uint64_t timeout_monitor_passes() const noexcept;
    void set_registration_handler(
        std::function<void(const protocol::AgentGuid&)> handler);
    bool send_profile(const protocol::AgentGuid& guid,
                      const protocol::SetProfile& profile) override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}
