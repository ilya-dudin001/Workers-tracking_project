#include "worker_agent/capture_pixel_copy.h"
#include "worker_agent/windows_capture_factory.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

TEST(CapturePixelCopy, RemovesSourceRowPadding) {
    const std::vector<std::uint8_t> source{
        1, 2, 3, 4, 5, 6, 7, 8, 99, 99, 99, 99,
        9, 10, 11, 12, 13, 14, 15, 16, 99, 99, 99, 99};
    std::vector<std::uint8_t> destination(16);

    monitor::worker_agent::copy_bgra_rows(
        source.data(), 12, destination.data(), 8, 2);

    EXPECT_EQ(destination,
              (std::vector<std::uint8_t>{
                  1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}));
}

TEST(DxgiFrameMetadata, DistinguishesPointerOnlyUpdatesFromDesktopImages) {
    EXPECT_FALSE(monitor::worker_agent::has_desktop_image_update(0));
    EXPECT_TRUE(monitor::worker_agent::has_desktop_image_update(1));
}

TEST(DxgiStagingTexture, RecreatesOnlyWhenFrameDimensionsChange) {
    EXPECT_FALSE(monitor::worker_agent::requires_staging_recreation(1920, 1080, 1920, 1080));
    EXPECT_TRUE(monitor::worker_agent::requires_staging_recreation(1920, 1080, 1280, 720));
    EXPECT_TRUE(monitor::worker_agent::requires_staging_recreation(0, 0, 1920, 1080));
}

TEST(WindowsCaptureFactory, ConstructionDoesNotTouchThePhysicalDesktop) {
    monitor::worker_agent::WindowsCaptureFactory factory;
    EXPECT_NE(&factory, nullptr);
}

}
