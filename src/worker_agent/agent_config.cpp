#include "worker_agent/agent_config.h"

#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <system_error>
#include <vector>

namespace monitor::worker_agent {
namespace {

constexpr std::array<std::uint8_t, 4> kMagic{'W', 'T', 'A', 'C'};
constexpr std::uint16_t kFormatVersion = 1;
constexpr std::size_t kChecksumSize = 4;
constexpr std::size_t kFixedSize =
    4 + 2 + protocol::kAgentGuidSize + 2 + 1 + 2 + 2 + kChecksumSize;

bool is_trim_character(const unsigned char value) noexcept {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

std::string trimmed(std::string_view value) {
    while (!value.empty() && is_trim_character(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() && is_trim_character(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return std::string(value);
}

void append_u16(std::vector<std::uint8_t>& bytes, const std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void append_u32(std::vector<std::uint8_t>& bytes, const std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>(value >> 24U));
}

std::uint32_t checksum(const std::vector<std::uint8_t>& bytes) {
    std::uint32_t value = 2166136261U;
    for (const std::uint8_t byte : bytes) {
        value ^= byte;
        value *= 16777619U;
    }
    return value;
}

bool take_u16(const std::vector<std::uint8_t>& bytes,
              std::size_t& offset,
              std::uint16_t& result) {
    if (offset > bytes.size() || bytes.size() - offset < 2) {
        return false;
    }
    result = static_cast<std::uint16_t>(bytes[offset]) |
             static_cast<std::uint16_t>(bytes[offset + 1]) << 8U;
    offset += 2;
    return true;
}

std::string system_error_text(const char* operation) {
    return std::string(operation) + " failed (" + std::to_string(GetLastError()) + ")";
}

}  // namespace

bool valid_utf8(const std::string_view value) noexcept {
    if (value.empty() || value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                               static_cast<int>(value.size()), nullptr, 0) > 0;
}

bool valid_guid(const protocol::AgentGuid& value) noexcept {
    return std::any_of(value.begin(), value.end(), [](const std::uint8_t byte) {
        return byte != 0;
    });
}

std::string validate_config(const AgentConfig& config) {
    const std::string name = trimmed(config.employee_name);
    if (name.empty()) {
        return "Employee name is required";
    }
    if (name != config.employee_name) {
        return "Employee name must not have leading or trailing whitespace";
    }
    if (name.size() > protocol::kMaxIdentityStringBytes || !valid_utf8(name)) {
        return "Employee name must be valid UTF-8 and at most 255 bytes";
    }
    if (config.observer_ipv4.empty() ||
        config.observer_ipv4.find('\0') != std::string::npos) {
        return "Observer IPv4 address is required";
    }
    IN_ADDR address{};
    if (InetPtonA(AF_INET, config.observer_ipv4.c_str(), &address) != 1) {
        return "Observer address must be a valid IPv4 address";
    }
    if (config.port == 0) {
        return "Port must be between 1 and 65535";
    }
    if (!valid_guid(config.agent_guid)) {
        return "Agent identity is invalid";
    }
    return {};
}

protocol::AgentGuid generate_agent_guid() {
    GUID generated{};
    if (FAILED(CoCreateGuid(&generated))) {
        throw std::runtime_error("Could not generate agent identity");
    }
    protocol::AgentGuid result{};
    static_assert(sizeof(generated) == result.size());
    std::memcpy(result.data(), &generated, result.size());
    if (!valid_guid(result)) {
        throw std::runtime_error("Generated agent identity is invalid");
    }
    return result;
}

std::string runtime_hostname_utf8() {
    std::array<wchar_t, 256> buffer{};
    DWORD size = static_cast<DWORD>(buffer.size());
    if (!GetComputerNameExW(ComputerNameDnsHostname, buffer.data(), &size) || size == 0) {
        throw std::runtime_error("Could not read computer name");
    }
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, buffer.data(),
                                             static_cast<int>(size), nullptr, 0, nullptr, nullptr);
    if (required <= 0 ||
        static_cast<std::size_t>(required) > protocol::kMaxIdentityStringBytes) {
        throw std::runtime_error("Computer name is not valid bounded UTF-8");
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, buffer.data(),
                            static_cast<int>(size), result.data(), required, nullptr, nullptr) !=
        required) {
        throw std::runtime_error("Could not convert computer name");
    }
    return result;
}

ConfigStore::ConfigStore(std::filesystem::path directory)
    : directory_(std::move(directory)), path_(directory_ / L"agent_config.bin") {}

ConfigStore ConfigStore::for_current_user() {
    PWSTR local_app_data = nullptr;
    const HRESULT result = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr,
                                                &local_app_data);
    if (FAILED(result) || local_app_data == nullptr) {
        throw std::runtime_error("LOCALAPPDATA is unavailable");
    }
    const std::filesystem::path directory =
        std::filesystem::path(local_app_data) / L"WorkersTracking" / L"agent";
    CoTaskMemFree(local_app_data);
    return ConfigStore(directory);
}

ConfigLoadResult ConfigStore::load() const noexcept {
    try {
        std::error_code error;
        if (!std::filesystem::exists(path_, error)) {
            return {ConfigLoadStatus::Missing, std::nullopt, {}};
        }
        const auto size = std::filesystem::file_size(path_, error);
        if (error || size < kFixedSize || size > kMaximumFileSize) {
            return {ConfigLoadStatus::Corrupt, std::nullopt, "Configuration size is invalid"};
        }
        std::ifstream input(path_, std::ios::binary);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        if (!input || input.peek() != std::ifstream::traits_type::eof()) {
            return {ConfigLoadStatus::Corrupt, std::nullopt, "Configuration read failed"};
        }
        const std::size_t checksum_offset = bytes.size() - kChecksumSize;
        const std::uint32_t expected =
            static_cast<std::uint32_t>(bytes[checksum_offset]) |
            static_cast<std::uint32_t>(bytes[checksum_offset + 1]) << 8U |
            static_cast<std::uint32_t>(bytes[checksum_offset + 2]) << 16U |
            static_cast<std::uint32_t>(bytes[checksum_offset + 3]) << 24U;
        bytes.resize(checksum_offset);
        if (checksum(bytes) != expected) {
            return {ConfigLoadStatus::Corrupt, std::nullopt,
                    "Configuration checksum is invalid"};
        }
        std::size_t offset = 0;
        if (!std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) {
            return {ConfigLoadStatus::Corrupt, std::nullopt, "Configuration signature is invalid"};
        }
        offset += kMagic.size();
        std::uint16_t version = 0;
        if (!take_u16(bytes, offset, version) || version != kFormatVersion) {
            return {ConfigLoadStatus::Corrupt, std::nullopt, "Configuration version is unsupported"};
        }
        AgentConfig config{};
        if (bytes.size() - offset < config.agent_guid.size()) {
            return {ConfigLoadStatus::Corrupt, std::nullopt, "Configuration is truncated"};
        }
        std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                    config.agent_guid.size(), config.agent_guid.begin());
        offset += config.agent_guid.size();
        std::uint16_t name_size = 0;
        std::uint16_t address_size = 0;
        if (!take_u16(bytes, offset, config.port) || offset >= bytes.size()) {
            return {ConfigLoadStatus::Corrupt, std::nullopt, "Configuration is truncated"};
        }
        config.autostart = bytes[offset++] != 0;
        if (!take_u16(bytes, offset, name_size) || !take_u16(bytes, offset, address_size) ||
            offset > bytes.size() ||
            static_cast<std::size_t>(name_size) + address_size != bytes.size() - offset) {
            return {ConfigLoadStatus::Corrupt, std::nullopt, "Configuration lengths are invalid"};
        }
        config.employee_name.assign(
            reinterpret_cast<const char*>(bytes.data() + offset), name_size);
        offset += name_size;
        config.observer_ipv4.assign(
            reinterpret_cast<const char*>(bytes.data() + offset), address_size);
        const std::string validation = validate_config(config);
        if (!validation.empty()) {
            return {ConfigLoadStatus::Corrupt, std::nullopt, validation};
        }
        return {ConfigLoadStatus::Loaded, std::move(config), {}};
    } catch (const std::exception& error) {
        return {ConfigLoadStatus::Corrupt, std::nullopt, error.what()};
    }
}

std::string ConfigStore::save(const AgentConfig& config) const noexcept {
    try {
        const std::string validation = validate_config(config);
        if (!validation.empty()) {
            return validation;
        }
        if (config.employee_name.size() > std::numeric_limits<std::uint16_t>::max() ||
            config.observer_ipv4.size() > std::numeric_limits<std::uint16_t>::max()) {
            return "Configuration strings are too large";
        }
        std::vector<std::uint8_t> bytes(kMagic.begin(), kMagic.end());
        append_u16(bytes, kFormatVersion);
        bytes.insert(bytes.end(), config.agent_guid.begin(), config.agent_guid.end());
        append_u16(bytes, config.port);
        bytes.push_back(config.autostart ? 1U : 0U);
        append_u16(bytes, static_cast<std::uint16_t>(config.employee_name.size()));
        append_u16(bytes, static_cast<std::uint16_t>(config.observer_ipv4.size()));
        bytes.insert(bytes.end(), config.employee_name.begin(), config.employee_name.end());
        bytes.insert(bytes.end(), config.observer_ipv4.begin(), config.observer_ipv4.end());
        append_u32(bytes, checksum(bytes));
        if (bytes.size() > kMaximumFileSize) {
            return "Configuration is too large";
        }
        std::filesystem::create_directories(directory_);
        const auto temporary = temporary_path();
        {
            HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
            if (file == INVALID_HANDLE_VALUE) {
                return system_error_text("CreateFile");
            }
            DWORD written = 0;
            const BOOL write_ok =
                WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
            const BOOL flush_ok = write_ok ? FlushFileBuffers(file) : FALSE;
            CloseHandle(file);
            if (!write_ok || !flush_ok || written != bytes.size()) {
                DeleteFileW(temporary.c_str());
                return system_error_text("Configuration write");
            }
        }
        const bool destination_exists = std::filesystem::exists(path_);
        const BOOL replaced =
            destination_exists
                ? ReplaceFileW(path_.c_str(), temporary.c_str(), nullptr,
                               REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)
                : MoveFileExW(temporary.c_str(), path_.c_str(),
                              MOVEFILE_WRITE_THROUGH | MOVEFILE_REPLACE_EXISTING);
        if (!replaced) {
            DeleteFileW(temporary.c_str());
            return system_error_text("Configuration replace");
        }
        return {};
    } catch (const std::exception& error) {
        return error.what();
    }
}

const std::filesystem::path& ConfigStore::path() const noexcept {
    return path_;
}

std::filesystem::path ConfigStore::temporary_path() const {
    return path_.wstring() + L".tmp";
}

}  // namespace monitor::worker_agent
