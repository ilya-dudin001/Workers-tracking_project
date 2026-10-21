#pragma once

#include <string>
#include <string_view>

namespace monitor::worker_agent {

[[nodiscard]] std::wstring quote_executable_path(std::wstring_view executable);
[[nodiscard]] bool autostart_command_matches(std::wstring_view command,
                                             std::wstring_view executable);

class AutostartRegistry {
public:
    AutostartRegistry(
        std::wstring subkey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        std::wstring value_name = L"WorkerAgent");

    [[nodiscard]] bool is_enabled(std::wstring_view executable) const noexcept;
    [[nodiscard]] std::string set_enabled(bool enabled,
                                          std::wstring_view executable) const noexcept;
    void delete_test_key() const noexcept;

private:
    std::wstring subkey_;
    std::wstring value_name_;
};

}  // namespace monitor::worker_agent
