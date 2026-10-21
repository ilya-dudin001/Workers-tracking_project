#pragma once

#include "worker_agent/agent_config.h"
#include "worker_agent/agent_orchestrator.h"

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>

namespace monitor::worker_agent {

enum class WorkerState {
    Connecting,
    Connected,
    Disconnected,
    CaptureUnavailable,
    Stopped,
};

using WorkerStateCallback = std::function<void(WorkerState, const std::string&)>;

class IWorkerPipeline {
public:
    virtual ~IWorkerPipeline() = default;
    virtual bool capture_changed_frame(protocol::Frame& frame) = 0;
    virtual void apply_profile(const CaptureProfile& profile) = 0;
};

class AgentWorkerCore {
public:
    AgentWorkerCore(std::unique_ptr<AgentTransport> transport,
                    std::unique_ptr<IWorkerPipeline> pipeline,
                    WorkerStateCallback callback);
    ~AgentWorkerCore();

    AgentWorkerCore(const AgentWorkerCore&) = delete;
    AgentWorkerCore& operator=(const AgentWorkerCore&) = delete;

    void step(std::chrono::milliseconds elapsed);
    [[nodiscard]] SteadyTime next_deadline() const noexcept;

private:
    void publish_state();

    std::unique_ptr<AgentTransport> transport_;
    std::unique_ptr<IWorkerPipeline> pipeline_;
    WorkerStateCallback callback_;
    SteadyTime start_{};
    AgentOrchestrator orchestrator_;
    AgentConnectionState published_state_{AgentConnectionState::Disconnected};
    bool state_published_{};
    bool capture_unavailable_{};
};

using WorkerRunner =
    std::function<void(const AgentConfig&, std::stop_token, const WorkerStateCallback&)>;

class AgentWorkerController {
public:
    explicit AgentWorkerController(WorkerRunner runner);
    ~AgentWorkerController();

    void start(const AgentConfig& config, WorkerStateCallback callback);
    void apply(const AgentConfig& config);
    void stop() noexcept;
    [[nodiscard]] bool running() const noexcept;

private:
    WorkerRunner runner_;
    AgentConfig config_;
    WorkerStateCallback callback_;
    std::jthread thread_;
};

void run_agent_worker(const AgentConfig& config,
                      std::stop_token stop,
                      const WorkerStateCallback& callback);

}  // namespace monitor::worker_agent
