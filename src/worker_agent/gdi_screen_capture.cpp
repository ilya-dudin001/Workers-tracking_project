#include "worker_agent/gdi_screen_capture.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

namespace monitor::worker_agent {
namespace {

struct ReleaseScreenDc {
    void operator()(HDC__* dc) const noexcept {
        if (dc != nullptr) {
            ReleaseDC(nullptr, dc);
        }
    }
};

struct DeleteMemoryDc {
    void operator()(HDC__* dc) const noexcept {
        if (dc != nullptr) {
            DeleteDC(dc);
        }
    }
};

struct DeleteBitmap {
    void operator()(HBITMAP__* bitmap) const noexcept {
        if (bitmap != nullptr) {
            DeleteObject(bitmap);
        }
    }
};

using UniqueScreenDc = std::unique_ptr<HDC__, ReleaseScreenDc>;
using UniqueMemoryDc = std::unique_ptr<HDC__, DeleteMemoryDc>;
using UniqueBitmap = std::unique_ptr<HBITMAP__, DeleteBitmap>;

class SelectedBitmap {
public:
    SelectedBitmap(HDC dc, HGDIOBJ prior) : dc_(dc), prior_(prior) {}
    ~SelectedBitmap() {
        if (prior_ != nullptr && prior_ != HGDI_ERROR) {
            SelectObject(dc_, prior_);
        }
    }

    SelectedBitmap(const SelectedBitmap&) = delete;
    SelectedBitmap& operator=(const SelectedBitmap&) = delete;

private:
    HDC dc_;
    HGDIOBJ prior_;
};

}

CaptureResult GdiScreenCapture::capture(const std::chrono::milliseconds) {
    const HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO monitor_info{sizeof(monitor_info)};
    if (monitor == nullptr || GetMonitorInfoW(monitor, &monitor_info) == FALSE) {
        return {CaptureStatus::Unavailable, std::nullopt};
    }

    const LONG raw_width = monitor_info.rcMonitor.right - monitor_info.rcMonitor.left;
    const LONG raw_height = monitor_info.rcMonitor.bottom - monitor_info.rcMonitor.top;
    if (raw_width <= 0 || raw_height <= 0 ||
        static_cast<unsigned long>(raw_width) >
            std::numeric_limits<std::uint32_t>::max() / 4U) {
        return {CaptureStatus::Fatal, std::nullopt};
    }
    const auto width = static_cast<std::uint32_t>(raw_width);
    const auto height = static_cast<std::uint32_t>(raw_height);
    const auto stride = width * 4U;
    if (static_cast<std::size_t>(height) >
        std::numeric_limits<std::size_t>::max() / stride) {
        return {CaptureStatus::Fatal, std::nullopt};
    }

    UniqueScreenDc screen(GetDC(nullptr));
    if (!screen) {
        return {CaptureStatus::Unavailable, std::nullopt};
    }
    UniqueMemoryDc memory(CreateCompatibleDC(screen.get()));
    if (!memory) {
        return {CaptureStatus::Unavailable, std::nullopt};
    }

    BITMAPINFO bitmap_info{};
    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth = raw_width;
    bitmap_info.bmiHeader.biHeight = -raw_height;
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;
    void* dib_pixels = nullptr;
    UniqueBitmap bitmap(CreateDIBSection(
        screen.get(), &bitmap_info, DIB_RGB_COLORS, &dib_pixels, nullptr, 0));
    if (!bitmap || dib_pixels == nullptr) {
        return {CaptureStatus::Unavailable, std::nullopt};
    }
    HGDIOBJ prior = SelectObject(memory.get(), bitmap.get());
    if (prior == nullptr || prior == HGDI_ERROR) {
        return {CaptureStatus::Fatal, std::nullopt};
    }
    const SelectedBitmap restore_selection(memory.get(), prior);

    if (BitBlt(memory.get(),
               0,
               0,
               raw_width,
               raw_height,
               screen.get(),
               monitor_info.rcMonitor.left,
               monitor_info.rcMonitor.top,
               SRCCOPY | CAPTUREBLT) == FALSE) {
        return {CaptureStatus::Unavailable, std::nullopt};
    }

    const auto byte_count = static_cast<std::size_t>(stride) * height;
    const auto* first = static_cast<const std::uint8_t*>(dib_pixels);
    std::vector<std::uint8_t> pixels(first, first + byte_count);
    const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
    return {CaptureStatus::Frame,
            BgraFrame{width,
                      height,
                      stride,
                      std::move(pixels),
                      ++sequence_,
                      static_cast<std::uint64_t>(timestamp)}};
}

}
