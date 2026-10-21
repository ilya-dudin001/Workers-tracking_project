#include "worker_agent/capture_profiles.h"
#include "worker_agent/change_detector.h"
#include "worker_agent/frame.h"
#include "worker_agent/latest_frame_queue.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <thread>
#include <utility>
#include <vector>

namespace {

using monitor::worker_agent::BgraFrame;
using monitor::worker_agent::ChangeDetector;
using monitor::worker_agent::LatestFrameQueue;

BgraFrame frame(const std::uint32_t width,
                const std::uint32_t height,
                const std::uint64_t sequence,
                const std::uint8_t fill = 0) {
    const auto stride = width * 4U;
    return BgraFrame{
        width, height, stride, std::vector<std::uint8_t>(
                                   static_cast<std::size_t>(stride) * height, fill),
        sequence, sequence * 10U};
}

TEST(BgraFrame, ValidatesStructureAndRejectsOverflowShapes) {
    EXPECT_TRUE(frame(3, 2, 1).is_valid());

    auto short_pixels = frame(3, 2, 1);
    short_pixels.pixels.pop_back();
    EXPECT_FALSE(short_pixels.is_valid());

    BgraFrame overflowing{std::numeric_limits<std::uint32_t>::max(),
                          std::numeric_limits<std::uint32_t>::max(),
                          std::numeric_limits<std::uint32_t>::max(),
                          {},
                          1,
                          1};
    EXPECT_FALSE(overflowing.is_valid());
}

TEST(ChangeDetector, DetectsFirstFrameIdentityPixelChangesShapeChangesAndReset) {
    ChangeDetector detector;
    auto original = frame(8, 4, 1);

    EXPECT_TRUE(detector.has_changed(original));
    EXPECT_FALSE(detector.has_changed(original));

    auto changed = original;
    changed.pixels[17] ^= 0x80U;
    EXPECT_TRUE(detector.has_changed(changed));

    EXPECT_TRUE(detector.has_changed(frame(4, 8, 2)));
    EXPECT_FALSE(detector.has_changed(frame(4, 8, 3)));

    detector.reset();
    EXPECT_TRUE(detector.has_changed(frame(4, 8, 4)));
}

TEST(ChangeDetector, IgnoresNonPixelRowPadding) {
    ChangeDetector detector;
    BgraFrame first{1, 2, 8, std::vector<std::uint8_t>(16, 0), 1, 1};
    auto padding_changed = first;
    padding_changed.pixels[4] = 0xFF;
    padding_changed.pixels[15] = 0xFF;

    EXPECT_TRUE(detector.has_changed(first));
    EXPECT_FALSE(detector.has_changed(padding_changed));
}

TEST(LatestFrameQueue, EmptyPopAndReplacementKeepOnlyNewestFrame) {
    LatestFrameQueue queue;
    EXPECT_FALSE(queue.try_pop().has_value());

    queue.push(frame(1, 1, 1));
    queue.push(frame(1, 1, 2));

    const auto newest = queue.try_pop();
    ASSERT_TRUE(newest.has_value());
    EXPECT_EQ(newest->sequence, 2U);
    EXPECT_FALSE(queue.try_pop().has_value());
}

TEST(LatestFrameQueue, ProducerAndConsumerAreThreadSafe) {
    LatestFrameQueue queue;
    std::atomic<bool> producer_done{false};
    std::atomic<std::uint64_t> greatest_seen{0};

    std::thread producer([&] {
        for (std::uint64_t sequence = 1; sequence <= 500; ++sequence) {
            queue.push(frame(1, 1, sequence));
        }
        producer_done = true;
    });
    std::thread consumer([&] {
        while (!producer_done || !queue.empty()) {
            if (auto value = queue.try_pop()) {
                auto prior = greatest_seen.load();
                while (prior < value->sequence &&
                       !greatest_seen.compare_exchange_weak(prior, value->sequence)) {
                }
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();
    EXPECT_EQ(greatest_seen, 500U);
}

TEST(CaptureProfiles, GridAndFocusMatchStageTwoContract) {
    using namespace std::chrono_literals;
    const auto grid = monitor::worker_agent::grid_profile();
    EXPECT_EQ(grid.max_width, 480U);
    EXPECT_EQ(grid.jpeg_quality, 60U);
    EXPECT_EQ(grid.minimum_interval, 250ms);
    EXPECT_EQ(grid.maximum_interval, 500ms);

    const auto focus = monitor::worker_agent::focus_profile();
    EXPECT_EQ(focus.max_width, 1280U);
    EXPECT_EQ(focus.max_height, 720U);
    EXPECT_EQ(focus.jpeg_quality, 75U);
    EXPECT_EQ(focus.minimum_interval, 250ms);
    EXPECT_EQ(focus.maximum_interval, 250ms);
}

}
