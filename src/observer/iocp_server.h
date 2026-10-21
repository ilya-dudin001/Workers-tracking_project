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
        std::size_t worker_threads = 0;
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
    void set_registration_handler(
        std::function<void(const protocol::AgentGuid&)> handler);
    bool send_profile(const protocol::AgentGuid& guid,
                      const protocol::SetProfile& profile) override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}
