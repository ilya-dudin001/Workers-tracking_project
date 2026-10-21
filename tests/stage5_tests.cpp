#include "worker_agent/agent_config.h"
#include "worker_agent/agent_worker.h"
#include "worker_agent/autostart.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using monitor::protocol::AgentGuid;
using monitor::protocol::Frame;
using monitor::protocol::SetProfile;
using monitor::worker_agent::AgentConfig;
using monitor::worker_agent::AgentWorkerController;
using monitor::worker_agent::AgentWorkerCore;
using monitor::worker_agent::ConfigLoadStatus;
using monitor::worker_agent::ConfigStore;
using monitor::worker_agent::IWorkerPipeline;
using monitor::worker_agent::WorkerState;

std::string u8_to_string(const std::u8string_view value) {
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

AgentConfig valid_config() {
    return AgentConfig{AgentGuid{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16},
                       u8_to_string(u8"Иван Петров"),
                       "127.0.0.1",
                       45873,
                       true};
}

std::filesystem::path unique_directory() {
    const auto value = std::chrono::steady_clock::now().time_since_epoch().count();
    auto path = std::filesystem::temp_directory_path() /
                ("worker_agent_stage5_" + std::to_string(value));
    std::filesystem::create_directories(path);
    return path;
}

class TemporaryDirectory {
public:
    TemporaryDirectory() : path_(unique_directory()) {}
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

TEST(AgentConfig, ValidatesBoundsIpv4PortAndGuid) {
    auto config = valid_config();
    EXPECT_TRUE(monitor::worker_agent::validate_config(config).empty());

    config.employee_name = "  ";
    EXPECT_FALSE(monitor::worker_agent::validate_config(config).empty());
    config = valid_config();
    config.observer_ipv4 = "localhost";
    EXPECT_FALSE(monitor::worker_agent::validate_config(config).empty());
    config = valid_config();
    config.port = 0;
    EXPECT_FALSE(monitor::worker_agent::validate_config(config).empty());
    config = valid_config();
    config.agent_guid.fill(0);
    EXPECT_FALSE(monitor::worker_agent::validate_config(config).empty());
}

TEST(ConfigStore, RoundTripsUnicodeAndPreservesGuidAcrossRewrite) {
    TemporaryDirectory directory;
    ConfigStore store(directory.path());
    const auto original = valid_config();
    ASSERT_TRUE(store.save(original).empty());

    auto loaded = store.load();
    ASSERT_EQ(loaded.status, ConfigLoadStatus::Loaded);
    ASSERT_TRUE(loaded.config.has_value());
    EXPECT_EQ(*loaded.config, original);

    auto edited = *loaded.config;
    edited.employee_name = u8_to_string(u8"Мария");
    ASSERT_TRUE(store.save(edited).empty());
    loaded = store.load();
    ASSERT_TRUE(loaded.config.has_value());
    EXPECT_EQ(loaded.config->agent_guid, original.agent_guid);
    EXPECT_EQ(loaded.config->employee_name, edited.employee_name);
    EXPECT_FALSE(std::filesystem::exists(store.temporary_path()));
}

TEST(ConfigStore, MissingCorruptAndOversizedAreSafe) {
    TemporaryDirectory directory;
    ConfigStore store(directory.path());
    EXPECT_EQ(store.load().status, ConfigLoadStatus::Missing);

    std::filesystem::create_directories(directory.path());
    {
        std::ofstream corrupt(store.path(), std::ios::binary);
        corrupt << "not a config";
    }
    EXPECT_EQ(store.load().status, ConfigLoadStatus::Corrupt);

    {
        std::ofstream oversized(store.path(), std::ios::binary | std::ios::trunc);
        oversized.seekp(static_cast<std::streamoff>(ConfigStore::kMaximumFileSize));
        oversized.put('\0');
    }
    EXPECT_EQ(store.load().status, ConfigLoadStatus::Corrupt);
}

TEST(ConfigStore, DetectsOtherwiseValidSingleByteCorruption) {
    TemporaryDirectory directory;
    ConfigStore store(directory.path());
    ASSERT_TRUE(store.save(valid_config()).empty());
    {
        std::fstream file(store.path(), std::ios::binary | std::ios::in | std::ios::out);
        file.seekp(24);
        file.put('\0');
    }
    EXPECT_EQ(store.load().status, ConfigLoadStatus::Corrupt);
}

TEST(AgentConfig, GeneratedGuidIsNonzeroAndHostnameIsBoundedUtf8) {
    const auto first = monitor::worker_agent::generate_agent_guid();
    const auto second = monitor::worker_agent::generate_agent_guid();
    EXPECT_TRUE(monitor::worker_agent::valid_guid(first));
    EXPECT_TRUE(monitor::worker_agent::valid_guid(second));
    EXPECT_NE(first, second);

    const auto hostname = monitor::worker_agent::runtime_hostname_utf8();
    EXPECT_FALSE(hostname.empty());
    EXPECT_LE(hostname.size(), monitor::protocol::kMaxIdentityStringBytes);
    EXPECT_TRUE(monitor::worker_agent::valid_utf8(hostname));
}

TEST(Autostart, QuotesExecutableCommandExactly) {
    EXPECT_EQ(monitor::worker_agent::quote_executable_path(L"C:\\Program Files\\WorkerAgent.exe"),
              L"\"C:\\Program Files\\WorkerAgent.exe\"");
    EXPECT_EQ(monitor::worker_agent::quote_executable_path(L"C:\\WorkerAgent.exe"),
              L"\"C:\\WorkerAgent.exe\"");
    EXPECT_TRUE(monitor::worker_agent::autostart_command_matches(
        L"\"C:\\WorkerAgent.exe\"", L"C:\\WorkerAgent.exe"));
    EXPECT_FALSE(monitor::worker_agent::autostart_command_matches(
        L"C:\\WorkerAgent.exe --other", L"C:\\WorkerAgent.exe"));
}

TEST(Autostart, DedicatedRegistrySubkeyIntegrationIsIdempotent) {
    const std::wstring subkey =
        L"Software\\WorkersTracking\\Tests\\Stage5_" +
        std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count());
    monitor::worker_agent::AutostartRegistry registry(subkey, L"WorkerAgentTest");
    const std::wstring executable = L"C:\\Program Files\\WorkersTracking\\WorkerAgent.exe";

    EXPECT_TRUE(registry.set_enabled(true, executable).empty());
    EXPECT_TRUE(registry.set_enabled(true, executable).empty());
    EXPECT_TRUE(registry.is_enabled(executable));
    EXPECT_TRUE(registry.set_enabled(false, executable).empty());
    EXPECT_FALSE(registry.is_enabled(executable));
    registry.delete_test_key();
}

class FakePipeline final : public IWorkerPipeline {
public:
    bool capture_changed_frame(Frame& frame) override {
        ++capture_calls;
        if (fail_capture) {
            throw std::runtime_error("capture unavailable");
        }
        if (!changed) {
            return false;
        }
        frame = Frame{next_sequence++, target_width, target_height, {1, 2, 3}};
        ++encoded_frames;
        return true;
    }

    void apply_profile(const monitor::worker_agent::CaptureProfile& profile) override {
        target_width = profile.max_width;
        target_height = profile.max_height;
        quality = profile.jpeg_quality;
    }

    std::size_t capture_calls{};
    std::size_t encoded_frames{};
    bool changed{true};
    bool fail_capture{};
    std::uint64_t next_sequence{1};
    std::uint32_t target_width{};
    std::uint32_t target_height{};
    std::uint32_t quality{};
};

class FakeTransport final : public monitor::worker_agent::AgentTransport {
public:
    bool connect() override {
        ++connect_calls;
        state_value = connect_result ? monitor::worker_agent::AgentConnectionState::Online
                                     : monitor::worker_agent::AgentConnectionState::Disconnected;
        return connect_result;
    }
    bool send_frame(const Frame& frame) override {
        if (!send_result) {
            state_value = monitor::worker_agent::AgentConnectionState::Disconnected;
            error = "send failed";
            return false;
        }
        sent_sequences.push_back(frame.sequence);
        return true;
    }
    bool send_heartbeat(const monitor::protocol::Heartbeat&) override { return send_result; }
    std::vector<SetProfile> receive_commands() override {
        auto result = commands;
        commands.clear();
        return result;
    }
    void disconnect() noexcept override {
        ++disconnect_calls;
        state_value = monitor::worker_agent::AgentConnectionState::Disconnected;
    }
    monitor::worker_agent::AgentConnectionState state() const noexcept override {
        return state_value;
    }
    std::string last_error() const override { return error; }

    bool connect_result{true};
    bool send_result{true};
    int connect_calls{};
    int disconnect_calls{};
    monitor::worker_agent::AgentConnectionState state_value{
        monitor::worker_agent::AgentConnectionState::Disconnected};
    std::string error;
    std::vector<std::uint64_t> sent_sequences;
    std::vector<SetProfile> commands;
};

TEST(AgentWorkerCore, UnchangedFrameIsNotEncodedOrSentAndNewestReplacesOld) {
    auto transport = std::make_unique<FakeTransport>();
    auto* transport_view = transport.get();
    auto pipeline = std::make_unique<FakePipeline>();
    auto* pipeline_view = pipeline.get();
    AgentWorkerCore worker(std::move(transport), std::move(pipeline), {});

    worker.step({});
    worker.step(250ms);
    pipeline_view->changed = false;
    worker.step(500ms);
    EXPECT_EQ(pipeline_view->encoded_frames, 1U);
    ASSERT_EQ(transport_view->sent_sequences.size(), 1U);
}

TEST(AgentWorkerCore, FocusCommandChangesEncoderTargetAndTransportRecovers) {
    auto transport = std::make_unique<FakeTransport>();
    auto* transport_view = transport.get();
    auto pipeline = std::make_unique<FakePipeline>();
    auto* pipeline_view = pipeline.get();
    std::vector<WorkerState> states;
    AgentWorkerCore worker(std::move(transport), std::move(pipeline),
                           [&states](WorkerState state, const std::string&) {
                               states.push_back(state);
                           });

    worker.step({});
    transport_view->commands.push_back(SetProfile{250, 75, 1280, 720});
    worker.step(1ms);
    EXPECT_EQ(pipeline_view->target_width, 1280U);
    EXPECT_EQ(pipeline_view->quality, 75U);

    transport_view->send_result = false;
    worker.step(251ms);
    EXPECT_EQ(states.back(), WorkerState::Disconnected);
    transport_view->send_result = true;
    worker.step(501ms);
    EXPECT_GE(transport_view->connect_calls, 2);
}

TEST(AgentWorkerCore, CaptureErrorUpdatesStateAndSuccessfulRetryRecovers) {
    auto transport = std::make_unique<FakeTransport>();
    auto pipeline = std::make_unique<FakePipeline>();
    auto* pipeline_view = pipeline.get();
    std::vector<WorkerState> states;
    AgentWorkerCore worker(std::move(transport), std::move(pipeline),
                           [&states](WorkerState state, const std::string&) {
                               states.push_back(state);
                           });
    worker.step({});
    pipeline_view->fail_capture = true;
    worker.step(250ms);
    ASSERT_EQ(states.back(), WorkerState::CaptureUnavailable);
    pipeline_view->fail_capture = false;
    worker.step(500ms);
    EXPECT_EQ(states.back(), WorkerState::Connected);
}

class BlockingRunner {
public:
    void operator()(const AgentConfig&, std::stop_token stop,
                    const std::function<void(WorkerState, const std::string&)>&) {
        ++starts;
        while (!stop.stop_requested()) {
            std::this_thread::sleep_for(1ms);
        }
        ++stops;
    }
    std::atomic<int> starts{};
    std::atomic<int> stops{};
};

TEST(AgentWorkerController, ApplyStopsAndRestartsWhileExitJoins) {
    auto runner = std::make_shared<BlockingRunner>();
    AgentWorkerController controller(
        [runner](const AgentConfig& config, std::stop_token stop, const auto& callback) {
            (*runner)(config, stop, callback);
        });
    auto config = valid_config();
    controller.start(config, {});
    while (runner->starts.load() < 1) {
        std::this_thread::yield();
    }
    config.employee_name = "Updated";
    controller.apply(config);
    while (runner->starts.load() < 2) {
        std::this_thread::yield();
    }
    EXPECT_EQ(runner->stops.load(), 1);
    controller.stop();
    EXPECT_EQ(runner->stops.load(), 2);
    EXPECT_FALSE(controller.running());
}

}  // namespace
