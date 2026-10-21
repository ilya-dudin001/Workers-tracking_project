#pragma once

#include <filesystem>
#include <mutex>
#include <string_view>

namespace monitor::worker_agent {

class BoundedLog {
public:
    explicit BoundedLog(std::filesystem::path path);
    void write(std::string_view message) noexcept;

private:
    void rotate_if_needed();

    std::filesystem::path path_;
    std::mutex mutex_;
};

}  // namespace monitor::worker_agent
