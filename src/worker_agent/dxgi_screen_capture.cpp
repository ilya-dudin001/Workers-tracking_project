#include "worker_agent/dxgi_screen_capture.h"

#include "worker_agent/capture_pixel_copy.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace monitor::worker_agent {
namespace {

using Microsoft::WRL::ComPtr;

void require_success(const HRESULT result, const char* operation) {
    if (FAILED(result)) {
        throw std::runtime_error(std::string(operation) + " failed");
    }
}

MONITORINFO primary_monitor_info() {
    MONITORINFO info{sizeof(info)};
    const HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    if (monitor == nullptr || GetMonitorInfoW(monitor, &info) == FALSE) {
        throw std::runtime_error("primary monitor lookup failed");
    }
    return info;
}

bool equal_rect(const RECT& left, const RECT& right) noexcept {
    return left.left == right.left && left.top == right.top && left.right == right.right &&
           left.bottom == right.bottom;
}

class AcquiredFrame {
public:
    explicit AcquiredFrame(IDXGIOutputDuplication* duplication) : duplication_(duplication) {}
    ~AcquiredFrame() {
        if (duplication_ != nullptr) {
            duplication_->ReleaseFrame();
        }
    }

    AcquiredFrame(const AcquiredFrame&) = delete;
    AcquiredFrame& operator=(const AcquiredFrame&) = delete;

private:
    IDXGIOutputDuplication* duplication_;
};

class MappedTexture {
public:
    MappedTexture(ID3D11DeviceContext* context, ID3D11Texture2D* texture)
        : context_(context), texture_(texture) {}
    ~MappedTexture() {
        if (mapped_) {
            context_->Unmap(texture_, 0);
        }
    }

    D3D11_MAPPED_SUBRESOURCE& value() noexcept {
        return value_;
    }

    void mark_mapped() noexcept {
        mapped_ = true;
    }

    MappedTexture(const MappedTexture&) = delete;
    MappedTexture& operator=(const MappedTexture&) = delete;

private:
    ID3D11DeviceContext* context_;
    ID3D11Texture2D* texture_;
    D3D11_MAPPED_SUBRESOURCE value_{};
    bool mapped_{};
};

}

class DxgiScreenCapture::Impl {
public:
    Impl() {
        initialize();
    }

    CaptureResult capture(const std::chrono::milliseconds timeout) {
        if (!duplication_) {
            try {
                initialize();
            } catch (const std::runtime_error&) {
                return {CaptureStatus::AccessLost, std::nullopt};
            }
        }
        try {
            const auto current_monitor = primary_monitor_info();
            if (!equal_rect(current_monitor.rcMonitor, output_rect_)) {
                initialize();
            }
        } catch (const std::runtime_error&) {
            return {CaptureStatus::AccessLost, std::nullopt};
        }

        const auto bounded_timeout = std::clamp<std::int64_t>(
            timeout.count(), 0, static_cast<std::int64_t>(std::numeric_limits<DWORD>::max()));
        DXGI_OUTDUPL_FRAME_INFO frame_info{};
        ComPtr<IDXGIResource> desktop_resource;
        const HRESULT acquired = duplication_->AcquireNextFrame(
            static_cast<DWORD>(bounded_timeout), &frame_info, &desktop_resource);
        if (acquired == DXGI_ERROR_WAIT_TIMEOUT) {
            return {CaptureStatus::Timeout, std::nullopt};
        }
        if (acquired == DXGI_ERROR_ACCESS_LOST) {
            recreate_after_loss();
            return {CaptureStatus::AccessLost, std::nullopt};
        }
        if (FAILED(acquired)) {
            return {CaptureStatus::Fatal, std::nullopt};
        }

        // Desktop Duplication requires exactly one ReleaseFrame after every successful acquire.
        const AcquiredFrame release_frame(duplication_.Get());
        if (!has_desktop_image_update(frame_info.LastPresentTime.QuadPart)) {
            return {CaptureStatus::Timeout, std::nullopt};
        }
        ComPtr<ID3D11Texture2D> desktop_texture;
        if (FAILED(desktop_resource.As(&desktop_texture))) {
            return {CaptureStatus::Fatal, std::nullopt};
        }

        D3D11_TEXTURE2D_DESC description{};
        desktop_texture->GetDesc(&description);
        if (description.Format != DXGI_FORMAT_B8G8R8A8_UNORM || description.Width == 0 ||
            description.Height == 0 ||
            description.Width > std::numeric_limits<std::uint32_t>::max() / 4U) {
            return {CaptureStatus::Fatal, std::nullopt};
        }
        if (!staging_ ||
            requires_staging_recreation(staging_width_,
                                        staging_height_,
                                        description.Width,
                                        description.Height)) {
            D3D11_TEXTURE2D_DESC staging_description = description;
            staging_description.Usage = D3D11_USAGE_STAGING;
            staging_description.BindFlags = 0;
            staging_description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            staging_description.MiscFlags = 0;
            staging_description.MipLevels = 1;
            staging_description.ArraySize = 1;
            staging_description.SampleDesc = {1, 0};
            staging_.Reset();
            if (FAILED(device_->CreateTexture2D(&staging_description, nullptr, &staging_))) {
                return {CaptureStatus::Fatal, std::nullopt};
            }
            staging_width_ = description.Width;
            staging_height_ = description.Height;
        }

        context_->CopyResource(staging_.Get(), desktop_texture.Get());
        MappedTexture mapped(context_.Get(), staging_.Get());
        const HRESULT map_result =
            context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped.value());
        if (FAILED(map_result)) {
            return {map_result == DXGI_ERROR_DEVICE_REMOVED ? CaptureStatus::AccessLost
                                                            : CaptureStatus::Fatal,
                    std::nullopt};
        }
        mapped.mark_mapped();

        const std::uint32_t stride = description.Width * 4U;
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(stride) *
                                         description.Height);
        // RowPitch can exceed width*4; copy into a tight owned buffer before unmapping.
        copy_bgra_rows(static_cast<const std::uint8_t*>(mapped.value().pData),
                       mapped.value().RowPitch,
                       pixels.data(),
                       stride,
                       description.Height);

        const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count();
        BgraFrame result{description.Width,
                         description.Height,
                         stride,
                         std::move(pixels),
                         ++sequence_,
                         static_cast<std::uint64_t>(timestamp)};
        return {CaptureStatus::Frame, std::move(result)};
    }

private:
    void initialize() {
        duplication_.Reset();
        staging_.Reset();
        context_.Reset();
        device_.Reset();
        staging_width_ = 0;
        staging_height_ = 0;

        const MONITORINFO monitor_info = primary_monitor_info();
        const HMONITOR primary =
            MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);

        ComPtr<IDXGIFactory1> factory;
        require_success(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI factory creation");

        ComPtr<IDXGIAdapter1> selected_adapter;
        ComPtr<IDXGIOutput1> selected_output;
        DXGI_OUTPUT_DESC selected_description{};
        for (UINT adapter_index = 0; selected_output == nullptr; ++adapter_index) {
            ComPtr<IDXGIAdapter1> adapter;
            const HRESULT adapter_result = factory->EnumAdapters1(adapter_index, &adapter);
            if (adapter_result == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            require_success(adapter_result, "DXGI adapter enumeration");
            for (UINT output_index = 0;; ++output_index) {
                ComPtr<IDXGIOutput> output;
                const HRESULT output_result = adapter->EnumOutputs(output_index, &output);
                if (output_result == DXGI_ERROR_NOT_FOUND) {
                    break;
                }
                require_success(output_result, "DXGI output enumeration");
                DXGI_OUTPUT_DESC description{};
                require_success(output->GetDesc(&description), "DXGI output description");
                if (description.Monitor == primary) {
                    require_success(output.As(&selected_output), "DXGI output upgrade");
                    selected_adapter = adapter;
                    selected_description = description;
                    break;
                }
            }
        }
        if (!selected_output || !selected_adapter) {
            throw std::runtime_error("primary desktop output is unavailable");
        }

        constexpr D3D_FEATURE_LEVEL feature_levels[]{
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
        D3D_FEATURE_LEVEL selected_level{};
        require_success(D3D11CreateDevice(selected_adapter.Get(),
                                          D3D_DRIVER_TYPE_UNKNOWN,
                                          nullptr,
                                          D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                          feature_levels,
                                          static_cast<UINT>(std::size(feature_levels)),
                                          D3D11_SDK_VERSION,
                                          &device_,
                                          &selected_level,
                                          &context_),
                        "D3D11 device creation");
        require_success(selected_output->DuplicateOutput(device_.Get(), &duplication_),
                        "desktop duplication creation");
        output_rect_ = selected_description.DesktopCoordinates;
        if (!equal_rect(output_rect_, monitor_info.rcMonitor)) {
            throw std::runtime_error("primary output coordinates changed");
        }
    }

    void recreate_after_loss() noexcept {
        try {
            initialize();
        } catch (const std::runtime_error&) {
            duplication_.Reset();
        }
    }

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGIOutputDuplication> duplication_;
    ComPtr<ID3D11Texture2D> staging_;
    RECT output_rect_{};
    UINT staging_width_{};
    UINT staging_height_{};
    std::uint64_t sequence_{};
};

DxgiScreenCapture::DxgiScreenCapture() : impl_(std::make_unique<Impl>()) {}

DxgiScreenCapture::~DxgiScreenCapture() = default;

CaptureResult DxgiScreenCapture::capture(const std::chrono::milliseconds timeout) {
    return impl_->capture(timeout);
}

}
