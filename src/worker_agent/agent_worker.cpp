#include "worker_agent/agent_worker.h"

#include "worker_agent/capture_coordinator.h"
#include "worker_agent/change_detector.h"
#include "worker_agent/image_processor.h"
#include "worker_agent/windows_capture_factory.h"

#include <algorithm>
#include <condition_variable>
#include <exception>

namespace monitor::worker_agent {
namespace {

using namespace std::chrono_literals;

template <typename T>
std::unique_ptr<T> require_dependency(std::unique_ptr<T> value,
                                      const char* message) {
    if (!value) {
        throw std::invalid_argument(message);
    }
    return value;
}

class WindowsWorkerPipeline final : public IWorkerPipeline {
public:
    WindowsWorkerPipeline() : coordinator_(factory_, 2s), profile_(grid_profile()) {}

    bool capture_changed_frame(protocol::Frame& result) override {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_);
        const CaptureResult captured = coordinator_.capture(elapsed, 50ms);
        if (captured.status == CaptureStatus::Timeout) {
            return false;
        }
        if (captured.status != CaptureStatus::Frame || !captured.frame) {
            throw std::runtime_error("Primary display capture is temporarily unavailable");
        }
        if (!detector_.has_changed(*captured.frame)) {
            return false;
        }
        const EncodedImage encoded =
            processor_.encode_jpeg(*captured.frame, profile_.max_width, profile_.max_height,
                                   profile_.jpeg_quality);
        result = protocol::Frame{++sequence_, encoded.width, encoded.height, encoded.jpeg,
                                 captured.frame->captured_at_ms};
        return true;
    }

    void apply_profile(const CaptureProfile& profile) override {
        profile_ = profile;
    }

private:
    WindowsCaptureFactory factory_;
    CaptureCoordinator coordinator_;
    ChangeDetector detector_;
    WicImageProcessor processor_;
    CaptureProfile profile_;
    std::uint64_t sequence_{};
    std::chrono::steady_clock::time_point start_{std::chrono::steady_clock::now()};
};

WorkerState public_state(const AgentConnectionState state) {
    switch (state) {
        case AgentConnectionState::Connecting:
            return WorkerState::Connecting;
        case AgentConnectionState::Online:
            return WorkerState::Connected;
        case AgentConnectionState::Disconnected:
            return WorkerState::Disconnected;
    }
    return WorkerState::Disconnected;
}

}  // namespace

AgentWorkerCore::AgentWorkerCore(std::unique_ptr<AgentTransport> transport,
                                 std::unique_ptr<IWorkerPipeline> pipeline,
                                 WorkerStateCallback callback)
    : transport_(require_dependency(std::move(transport),
                                    "Worker transport is required")),
      pipeline_(require_dependency(std::move(pipeline),
                                   "Worker pipeline is required")),
      callback_(std::move(callback)),
      orchestrator_(*transport_, start_,
                    [this](const CaptureProfile& profile) {
                        pipeline_->apply_profile(profile);
                    }) {
    pipeline_->apply_profile(orchestrator_.current_profile());
}

AgentWorkerCore::~AgentWorkerCore() {
    transport_->disconnect();
}

void AgentWorkerCore::step(const std::chrono::milliseconds elapsed) {
    const SteadyTime now = start_ + elapsed;
    if (now >= orchestrator_.next_capture_deadline()) {
        try {
            protocol::Frame frame{};
            if (pipeline_->capture_changed_frame(frame)) {
                orchestrator_.submit_frame(std::move(frame));
            }
            if (capture_unavailable_) {
                capture_unavailable_ = false;
                if (callback_) {
                    callback_(public_state(orchestrator_.state()), {});
                }
            }
        } catch (const std::exception& error) {
            capture_unavailable_ = true;
            if (callback_) {
                callback_(WorkerState::CaptureUnavailable, error.what());
            }
        }
    }
    orchestrator_.tick(now);
    publish_state();
}

SteadyTime AgentWorkerCore::next_deadline() const noexcept {
    if (orchestrator_.state() == AgentConnectionState::Disconnected) {
        return orchestrator_.next_reconnect_deadline();
    }
    return std::min(orchestrator_.next_capture_deadline(),
                    orchestrator_.next_heartbeat_deadline());
}

void AgentWorkerCore::publish_state() {
    const AgentConnectionState current = orchestrator_.state();
    if (!state_published_ || current != published_state_) {
        state_published_ = true;
        published_state_ = current;
        if (callback_) {
            callback_(public_state(current), orchestrator_.last_error());
        }
    }
}

AgentWorkerController::AgentWorkerController(WorkerRunner runner)
    : runner_(std::move(runner)) {
    if (!runner_) {
        throw std::invalid_argument("Worker runner is required");
    }
}

AgentWorkerController::~AgentWorkerController() {
    stop();
}

void AgentWorkerController::start(const AgentConfig& config, WorkerStateCallback callback) {
    stop();
    config_ = config;
    callback_ = std::move(callback);
    const AgentConfig snapshot = config_;
    const WorkerStateCallback state_callback = callback_;
    thread_ = std::jthread([this, snapshot, state_callback](const std::stop_token stop_token) {
        runner_(snapshot, stop_token, state_callback);
    });
}

void AgentWorkerController::apply(const AgentConfig& config) {
    const WorkerStateCallback state_callback = callback_;
    start(config, state_callback);
}

void AgentWorkerController::stop() noexcept {
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
}

bool AgentWorkerController::running() const noexcept {
    return thread_.joinable();
}

void run_agent_worker(const AgentConfig& config,
                      const std::stop_token stop,
                      const WorkerStateCallback& callback) {
    try {
        AgentSessionConfig session_config{
            {config.observer_ipv4, config.port},
            {config.agent_guid, config.employee_name, runtime_hostname_utf8()},
            1000ms,
            50ms,
            1000ms,
            stop};
        auto transport = std::make_unique<WinsockAgentSession>(std::move(session_config));
        auto pipeline = std::make_unique<WindowsWorkerPipeline>();
        AgentWorkerCore worker(std::move(transport), std::move(pipeline), callback);
        const auto started = std::chrono::steady_clock::now();
        std::mutex wait_mutex;
        std::condition_variable_any wake;
        while (!stop.stop_requested()) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started);
            worker.step(elapsed);
            std::unique_lock lock(wait_mutex);
            const auto next =
                started + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                              worker.next_deadline().time_since_epoch());
            wake.wait_until(lock, stop, next, [] { return false; });
        }
    } catch (const std::exception& error) {
        if (callback) {
            callback(WorkerState::Disconnected, error.what());
        }
    }
    if (callback) {
        callback(WorkerState::Stopped, {});
    }
}

}  // namespace monitor::worker_agent
