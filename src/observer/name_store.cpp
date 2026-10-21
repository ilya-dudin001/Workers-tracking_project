#include "observer/name_store.h"

#include <windows.h>

#include <array>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <system_error>

namespace monitor::observer {
namespace {

constexpr std::size_t kMaxNameBytes = 255;

bool valid_utf8(const std::string& value) {
    if (value.empty() || value.size() > kMaxNameBytes) {
        return false;
    }
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                               static_cast<int>(value.size()), nullptr, 0) > 0;
}

std::string to_hex(const protocol::AgentGuid& guid) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(32);
    for (const auto byte : guid) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0f]);
    }
    return result;
}

bool from_hex(const std::string& text, protocol::AgentGuid& guid) {
    if (text.size() != 32U) {
        return false;
    }
    const auto nibble = [](const char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        return -1;
    };
    for (std::size_t index = 0; index < guid.size(); ++index) {
        const int high = nibble(text[index * 2U]);
        const int low = nibble(text[index * 2U + 1U]);
        if (high < 0 || low < 0) {
            return false;
        }
        guid[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return true;
}

}

NameStore::NameStore(std::filesystem::path directory)
    : directory_(std::move(directory)) {}

NameStore NameStore::local_app_data() {
    wchar_t* path = nullptr;
    std::size_t length = 0;
    if (_wdupenv_s(&path, &length, L"LOCALAPPDATA") != 0 || path == nullptr) {
        return NameStore(std::filesystem::temp_directory_path() /
                         "WorkersTracking" / "observer_names");
    }
    const std::filesystem::path root(path);
    std::free(path);
    return NameStore(root / "WorkersTracking" / "observer_names");
}

bool NameStore::save(const protocol::AgentGuid& guid,
                     const std::string& utf8_name) const {
    if (!valid_utf8(utf8_name)) {
        return false;
    }
    std::error_code error;
    std::filesystem::create_directories(directory_, error);
    if (error) {
        return false;
    }
    const auto target = directory_ / (to_hex(guid) + ".name");
    const auto temporary = target.wstring() + L".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(utf8_name.data(), static_cast<std::streamsize>(utf8_name.size()));
        if (!output) {
            return false;
        }
    }
    if (!MoveFileExW(temporary.c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

std::map<protocol::AgentGuid, std::string> NameStore::load() const {
    std::map<protocol::AgentGuid, std::string> result;
    std::error_code error;
    if (!std::filesystem::exists(directory_, error)) {
        return result;
    }
    for (std::filesystem::directory_iterator iterator(directory_, error), end;
         !error && iterator != end; iterator.increment(error)) {
        if (!iterator->is_regular_file(error) || iterator->path().extension() != ".name") {
            continue;
        }
        protocol::AgentGuid guid{};
        const std::string stem = iterator->path().stem().string();
        if (!from_hex(stem, guid)) {
            continue;
        }
        std::ifstream input(iterator->path(), std::ios::binary);
        std::string value{std::istreambuf_iterator<char>(input),
                          std::istreambuf_iterator<char>()};
        if (valid_utf8(value)) {
            result.emplace(guid, std::move(value));
        }
    }
    return result;
}

}
