#include "worker_agent/bounded_log.h"

#include <chrono>
#include <fstream>

namespace monitor::worker_agent {
namespace {

constexpr std::uintmax_t kMaximumLogSize = 1024U * 1024U;

}  // namespace

BoundedLog::BoundedLog(std::filesystem::path path) : path_(std::move(path)) {}

void BoundedLog::write(const std::string_view message) noexcept {
    try {
        std::scoped_lock lock(mutex_);
        std::filesystem::create_directories(path_.parent_path());
        rotate_if_needed();
        std::ofstream output(path_, std::ios::app);
        const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch());
        output << milliseconds.count() << " " << message << '\n';
    } catch (...) {
        // Logging is diagnostic and must never terminate monitoring.
    }
}

void BoundedLog::rotate_if_needed() {
    std::error_code error;
    if (!std::filesystem::exists(path_, error) ||
        std::filesystem::file_size(path_, error) < kMaximumLogSize) {
        return;
    }
    const auto backup = path_.wstring() + L".1";
    std::filesystem::remove(backup, error);
    error.clear();
    std::filesystem::rename(path_, backup, error);
}

}  // namespace monitor::worker_agent
