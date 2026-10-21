#pragma once

#include "worker_agent/frame.h"

#include <mutex>
#include <optional>

namespace monitor::worker_agent {

class LatestFrameQueue {
public:
    void push(BgraFrame frame) {
        std::scoped_lock lock(mutex_);
        pending_ = std::move(frame);
    }

    [[nodiscard]] std::optional<BgraFrame> try_pop() {
        std::scoped_lock lock(mutex_);
        auto result = std::move(pending_);
        pending_.reset();
        return result;
    }

    [[nodiscard]] bool empty() const {
        std::scoped_lock lock(mutex_);
        return !pending_.has_value();
    }

private:
    mutable std::mutex mutex_;
    std::optional<BgraFrame> pending_;
};

}
