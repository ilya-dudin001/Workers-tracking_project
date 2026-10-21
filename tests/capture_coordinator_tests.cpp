#include "worker_agent/capture_coordinator.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace {

using monitor::worker_agent::BgraFrame;
using monitor::worker_agent::CaptureCoordinator;
using monitor::worker_agent::CaptureResult;
using monitor::worker_agent::CaptureStatus;
using monitor::worker_agent::ICaptureFactory;
using monitor::worker_agent::IScreenCapture;

BgraFrame frame(const std::uint32_t width,
                const std::uint32_t height,
                const std::uint64_t sequence) {
    const auto stride = width * 4U;
    return {width,
            height,
            stride,
            std::vector<std::uint8_t>(static_cast<std::size_t>(stride) * height),
            sequence,
            sequence};
}

struct SourceState {
    std::deque<CaptureResult> results;
    std::size_t calls{};
};

class FakeCapture final : public IScreenCapture {
public:
    explicit FakeCapture(std::shared_ptr<SourceState> state) : state_(std::move(state)) {}

    CaptureResult capture(std::chrono::milliseconds) override {
        ++state_->calls;
        if (state_->results.empty()) {
            return {CaptureStatus::Timeout, std::nullopt};
        }
        auto result = std::move(state_->results.front());
        state_->results.pop_front();
        return result;
    }

private:
    std::shared_ptr<SourceState> state_;
};

class FakeFactory final : public ICaptureFactory {
public:
    std::deque<std::shared_ptr<SourceState>> dxgi;
    std::deque<std::shared_ptr<SourceState>> gdi;
    std::size_t dxgi_attempts{};
    std::size_t gdi_attempts{};

    std::unique_ptr<IScreenCapture> create_dxgi() override {
        ++dxgi_attempts;
        return pop(dxgi);
    }

    std::unique_ptr<IScreenCapture> create_gdi() override {
        ++gdi_attempts;
        return pop(gdi);
    }

private:
    static std::unique_ptr<IScreenCapture> pop(
        std::deque<std::shared_ptr<SourceState>>& sources) {
        if (sources.empty()) {
            return nullptr;
        }
        auto state = std::move(sources.front());
        sources.pop_front();
        if (!state) {
            return nullptr;
        }
        return std::make_unique<FakeCapture>(std::move(state));
    }
};

std::shared_ptr<SourceState> source(std::initializer_list<CaptureResult> results) {
    auto state = std::make_shared<SourceState>();
    state->results = results;
    return state;
}

TEST(CaptureResult, StatusesAreExplicitAndFrameCarriesPixels) {
    EXPECT_NE(CaptureStatus::Frame, CaptureStatus::Timeout);
    EXPECT_NE(CaptureStatus::AccessLost, CaptureStatus::Unavailable);
    const CaptureResult result{CaptureStatus::Frame, frame(2, 1, 9)};
    ASSERT_TRUE(result.frame.has_value());
    EXPECT_EQ(result.frame->sequence, 9U);
}

TEST(CaptureCoordinator, FallsBackWhenDxgiInitializationIsUnavailable) {
    FakeFactory factory;
    factory.dxgi.push_back(nullptr);
    factory.gdi.push_back(source({{CaptureStatus::Frame, frame(2, 2, 1)}}));
    CaptureCoordinator coordinator(factory, std::chrono::seconds(2));

    const auto result = coordinator.capture({}, std::chrono::milliseconds(10));

    EXPECT_EQ(result.status, CaptureStatus::Frame);
    EXPECT_EQ(factory.dxgi_attempts, 1U);
    EXPECT_EQ(factory.gdi_attempts, 1U);
}

TEST(CaptureCoordinator, DoesNotRetryUnavailableBackendsBeforeDelay) {
    using namespace std::chrono_literals;
    FakeFactory factory;
    factory.dxgi.push_back(nullptr);
    factory.dxgi.push_back(source({{CaptureStatus::Frame, frame(2, 2, 1)}}));
    factory.gdi.push_back(nullptr);
    CaptureCoordinator coordinator(factory, 2s);

    EXPECT_EQ(coordinator.capture(0ms, 10ms).status, CaptureStatus::Unavailable);
    EXPECT_EQ(coordinator.capture(1s, 10ms).status, CaptureStatus::Unavailable);
    EXPECT_EQ(factory.dxgi_attempts, 1U);
    EXPECT_EQ(factory.gdi_attempts, 1U);

    EXPECT_EQ(coordinator.capture(2s, 10ms).status, CaptureStatus::Frame);
    EXPECT_EQ(factory.dxgi_attempts, 2U);
}

TEST(CaptureCoordinator, PropagatesTimeoutWithoutRecreatingHealthyDxgi) {
    FakeFactory factory;
    factory.dxgi.push_back(source({{CaptureStatus::Timeout, std::nullopt},
                                   {CaptureStatus::Frame, frame(3, 2, 2)}}));
    CaptureCoordinator coordinator(factory, std::chrono::seconds(2));

    EXPECT_EQ(coordinator.capture({}, std::chrono::milliseconds(10)).status,
              CaptureStatus::Timeout);
    EXPECT_EQ(coordinator.capture(std::chrono::milliseconds(1),
                                  std::chrono::milliseconds(10))
                  .status,
              CaptureStatus::Frame);
    EXPECT_EQ(factory.dxgi_attempts, 1U);
}

TEST(CaptureCoordinator, UsesGdiAfterAccessLossAndReturnsToDxgiAfterDelay) {
    using namespace std::chrono_literals;
    FakeFactory factory;
    factory.dxgi.push_back(source({{CaptureStatus::AccessLost, std::nullopt}}));
    factory.dxgi.push_back(source({{CaptureStatus::Frame, frame(8, 6, 3)}}));
    factory.gdi.push_back(source({{CaptureStatus::Frame, frame(4, 3, 2)},
                                  {CaptureStatus::Frame, frame(4, 3, 4)}}));
    CaptureCoordinator coordinator(factory, 2s);

    EXPECT_EQ(coordinator.capture(0ms, 10ms).status, CaptureStatus::Frame);
    EXPECT_EQ(factory.gdi_attempts, 1U);
    EXPECT_EQ(coordinator.capture(1s, 10ms).frame->width, 4U);
    EXPECT_EQ(factory.dxgi_attempts, 1U);
    EXPECT_EQ(coordinator.capture(2s, 10ms).frame->width, 8U);
    EXPECT_EQ(factory.dxgi_attempts, 2U);
}

TEST(CaptureCoordinator, PreservesResolutionChangesFromSource) {
    using namespace std::chrono_literals;
    FakeFactory factory;
    factory.dxgi.push_back(source({{CaptureStatus::Frame, frame(10, 5, 1)},
                                   {CaptureStatus::Frame, frame(20, 10, 2)}}));
    CaptureCoordinator coordinator(factory, 2s);

    EXPECT_EQ(coordinator.capture(0ms, 10ms).frame->width, 10U);
    EXPECT_EQ(coordinator.capture(1ms, 10ms).frame->width, 20U);
}

}
