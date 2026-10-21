#include "worker_agent/autostart.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <vector>

namespace monitor::worker_agent {
namespace {

std::string registry_error(const char* operation, const LSTATUS status) {
    return std::string(operation) + " failed (" + std::to_string(status) + ")";
}

}  // namespace

std::wstring quote_executable_path(const std::wstring_view executable) {
    if (executable.empty() || executable.find(L'"') != std::wstring_view::npos) {
        return {};
    }
    return L"\"" + std::wstring(executable) + L"\"";
}

bool autostart_command_matches(const std::wstring_view command,
                               const std::wstring_view executable) {
    const std::wstring expected = quote_executable_path(executable);
    return !expected.empty() && command.size() == expected.size() &&
           std::equal(command.begin(), command.end(), expected.begin(),
                      [](const wchar_t left, const wchar_t right) {
                          return std::towlower(left) == std::towlower(right);
                      });
}

AutostartRegistry::AutostartRegistry(std::wstring subkey, std::wstring value_name)
    : subkey_(std::move(subkey)), value_name_(std::move(value_name)) {}

bool AutostartRegistry::is_enabled(const std::wstring_view executable) const noexcept {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, subkey_.c_str(), 0, KEY_QUERY_VALUE, &key) !=
        ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0;
    DWORD bytes = 0;
    LSTATUS status =
        RegQueryValueExW(key, value_name_.c_str(), nullptr, &type, nullptr, &bytes);
    std::vector<wchar_t> value(bytes / sizeof(wchar_t) + 1, L'\0');
    if (status == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ) &&
        bytes >= sizeof(wchar_t)) {
        status = RegQueryValueExW(key, value_name_.c_str(), nullptr, &type,
                                  reinterpret_cast<BYTE*>(value.data()), &bytes);
    }
    RegCloseKey(key);
    if (status != ERROR_SUCCESS || value.empty()) {
        return false;
    }
    const std::wstring_view command(value.data());
    return autostart_command_matches(command, executable);
}

std::string AutostartRegistry::set_enabled(const bool enabled,
                                           const std::wstring_view executable) const noexcept {
    HKEY key = nullptr;
    DWORD disposition = 0;
    const LSTATUS open_status =
        RegCreateKeyExW(HKEY_CURRENT_USER, subkey_.c_str(), 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key,
                        &disposition);
    if (open_status != ERROR_SUCCESS) {
        return registry_error("Opening autostart registry key", open_status);
    }
    LSTATUS status = ERROR_SUCCESS;
    if (enabled) {
        const std::wstring command = quote_executable_path(executable);
        if (command.empty()) {
            RegCloseKey(key);
            return "Executable path cannot be quoted";
        }
        status = RegSetValueExW(
            key, value_name_.c_str(), 0, REG_SZ,
            reinterpret_cast<const BYTE*>(command.c_str()),
            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        status = RegDeleteValueW(key, value_name_.c_str());
        if (status == ERROR_FILE_NOT_FOUND) {
            status = ERROR_SUCCESS;
        }
    }
    RegCloseKey(key);
    return status == ERROR_SUCCESS ? std::string{} : registry_error("Updating autostart", status);
}

void AutostartRegistry::delete_test_key() const noexcept {
    if (subkey_.starts_with(L"Software\\WorkersTracking\\Tests\\")) {
        RegDeleteTreeW(HKEY_CURRENT_USER, subkey_.c_str());
    }
}

}  // namespace monitor::worker_agent
