#include "observer/jpeg_decoder.h"

#define NOMINMAX
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <limits>

namespace monitor::observer {

using Microsoft::WRL::ComPtr;

bool JpegDecoder::accept_dimensions(const std::uint32_t width,
                                    const std::uint32_t height) const noexcept {
    constexpr std::uint64_t kMaxPixels = 3840ULL * 2160ULL;
    return width > 0 && height > 0 && width <= 3840 && height <= 2160 &&
           static_cast<std::uint64_t>(width) * height <= kMaxPixels;
}

std::optional<DecodedImage> JpegDecoder::decode(
    const std::span<const std::uint8_t> jpeg) const noexcept {
    if (jpeg.empty() || jpeg.size() > 8U * 1024U * 1024U ||
        jpeg.size() > static_cast<std::size_t>(std::numeric_limits<DWORD>::max())) {
        return std::nullopt;
    }
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
        return std::nullopt;
    }
    struct ComScope {
        bool active;
        ~ComScope() {
            if (active) {
                CoUninitialize();
            }
        }
    };
    const ComScope com_scope{SUCCEEDED(initialized)};
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
        return std::nullopt;
    }
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    HRESULT result = factory->CreateStream(&stream);
    if (SUCCEEDED(result)) {
        result = stream->InitializeFromMemory(
            const_cast<BYTE*>(reinterpret_cast<const BYTE*>(jpeg.data())),
            static_cast<DWORD>(jpeg.size()));
    }
    if (SUCCEEDED(result)) {
        result = factory->CreateDecoderFromStream(
            stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
    }
    if (SUCCEEDED(result)) result = decoder->GetFrame(0, &frame);
    UINT width = 0;
    UINT height = 0;
    if (SUCCEEDED(result)) result = frame->GetSize(&width, &height);
    if (FAILED(result) || !accept_dimensions(width, height)) {
        return std::nullopt;
    }
    if (SUCCEEDED(result)) result = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(result)) {
        result = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    }
    const std::uint64_t stride64 = static_cast<std::uint64_t>(width) * 4U;
    const std::uint64_t size64 = stride64 * height;
    if (FAILED(result) || size64 > std::numeric_limits<UINT>::max()) {
        return std::nullopt;
    }
    DecodedImage image{width, height,
        std::vector<std::uint8_t>(static_cast<std::size_t>(size64))};
    result = converter->CopyPixels(nullptr, static_cast<UINT>(stride64),
        static_cast<UINT>(size64), image.pixels.data());
    if (FAILED(result)) {
        return std::nullopt;
    }
    return image;
}

}
