#include "worker_agent/bounded_log.h"

#include <chrono>
#include <fstream>
#include <string>

namespace monitor::worker_agent {
namespace {

constexpr std::uintmax_t kMaximumLogSize = 1024U * 1024U;
constexpr std::size_t kMaximumEntrySize = 4096;

}  // namespace

BoundedLog::BoundedLog(std::filesystem::path path) : path_(std::move(path)) {}

void BoundedLog::write(const std::string_view message) noexcept {
    try {
        std::scoped_lock lock(mutex_);
        std::filesystem::create_directories(path_.parent_path());
        const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch());
        std::string entry = std::to_string(milliseconds.count()) + " ";
        entry.append(message.substr(0, kMaximumEntrySize));
        entry.push_back('\n');
        if (!prepare_for_write(entry.size())) {
            return;
        }
        std::ofstream output(path_, std::ios::binary | std::ios::app);
        output.write(entry.data(), static_cast<std::streamsize>(entry.size()));
        output.flush();
    } catch (...) {
        // Logging is diagnostic and must never terminate monitoring.
    }
}

bool BoundedLog::prepare_for_write(const std::uintmax_t entry_size) noexcept {
    std::error_code error;
    const bool exists = std::filesystem::exists(path_, error);
    if (error) {
        return false;
    }
    const std::uintmax_t current_size =
        exists ? std::filesystem::file_size(path_, error) : 0;
    if (error) {
        return false;
    }
    if (current_size <= kMaximumLogSize &&
        entry_size <= kMaximumLogSize - current_size) {
        return true;
    }
    const auto backup = path_.wstring() + L".1";
    std::filesystem::remove(backup, error);
    if (error) {
        error.clear();
        std::ofstream truncate(path_, std::ios::binary | std::ios::trunc);
        return static_cast<bool>(truncate);
    }
    error.clear();
    std::filesystem::rename(path_, backup, error);
    if (!error) {
        return true;
    }
    // If rotation cannot rename (for example an antivirus holds the file),
    // truncate instead of allowing an unbounded diagnostic file.
    std::ofstream truncate(path_, std::ios::binary | std::ios::trunc);
    return static_cast<bool>(truncate);
}

}  // namespace monitor::worker_agent
