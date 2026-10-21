#pragma once

#include "common/protocol.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace monitor::worker_agent {

struct AgentConfig {
    protocol::AgentGuid agent_guid{};
    std::string employee_name;
    std::string observer_ipv4;
    std::uint16_t port{45873};
    bool autostart{};
    bool operator==(const AgentConfig&) const = default;
};

[[nodiscard]] bool valid_utf8(std::string_view value) noexcept;
[[nodiscard]] bool valid_guid(const protocol::AgentGuid& value) noexcept;
[[nodiscard]] std::string validate_config(const AgentConfig& config);
[[nodiscard]] protocol::AgentGuid generate_agent_guid();
[[nodiscard]] std::string runtime_hostname_utf8();

enum class ConfigLoadStatus {
    Loaded,
    Missing,
    Corrupt,
};

struct ConfigLoadResult {
    ConfigLoadStatus status;
    std::optional<AgentConfig> config;
    std::string error;
};

class ConfigStore {
public:
    static constexpr std::size_t kMaximumFileSize = 4096;

    explicit ConfigStore(std::filesystem::path directory);
    [[nodiscard]] static ConfigStore for_current_user();
    [[nodiscard]] ConfigLoadResult load() const noexcept;
    [[nodiscard]] std::string save(const AgentConfig& config) const noexcept;
    [[nodiscard]] const std::filesystem::path& path() const noexcept;
    [[nodiscard]] std::filesystem::path temporary_path() const;

private:
    std::filesystem::path directory_;
    std::filesystem::path path_;
};

}  // namespace monitor::worker_agent
