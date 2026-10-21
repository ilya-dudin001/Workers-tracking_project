#include "worker_agent/image_processor.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <objidl.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace monitor::worker_agent {
namespace {

using Microsoft::WRL::ComPtr;

class ComApartment {
public:
    ComApartment() {
        const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(result)) {
            initialized_ = true;
        } else if (result != RPC_E_CHANGED_MODE) {
            throw std::runtime_error("COM initialization failed");
        }
    }

    ~ComApartment() {
        if (initialized_) {
            CoUninitialize();
        }
    }

    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

private:
    bool initialized_{};
};

void require_success(const HRESULT result, const char* operation) {
    if (FAILED(result)) {
        throw std::runtime_error(std::string(operation) + " failed");
    }
}

std::pair<std::uint32_t, std::uint32_t> fitted_size(const BgraFrame& frame,
                                                    const std::uint32_t max_width,
                                                    const std::uint32_t max_height) {
    if (frame.width <= max_width && frame.height <= max_height) {
        return {frame.width, frame.height};
    }
    if (static_cast<std::uint64_t>(max_width) * frame.height <=
        static_cast<std::uint64_t>(max_height) * frame.width) {
        const auto height = std::max<std::uint64_t>(
            1, static_cast<std::uint64_t>(frame.height) * max_width / frame.width);
        return {max_width, static_cast<std::uint32_t>(height)};
    }
    const auto width = std::max<std::uint64_t>(
        1, static_cast<std::uint64_t>(frame.width) * max_height / frame.height);
    return {static_cast<std::uint32_t>(width), max_height};
}

}

EncodedImage WicImageProcessor::encode_jpeg(const BgraFrame& frame,
                                            const std::uint32_t max_width,
                                            const std::uint32_t max_height,
                                            const std::uint32_t quality) const {
    if (!frame.is_valid() || max_width == 0 || max_height == 0) {
        throw std::invalid_argument("valid frame and nonzero image bounds required");
    }
    if (frame.pixels.size() > std::numeric_limits<UINT>::max()) {
        throw std::invalid_argument("frame is too large for WIC");
    }

    const auto [output_width, output_height] = fitted_size(frame, max_width, max_height);
    const ComApartment apartment;

    ComPtr<IWICImagingFactory> factory;
    require_success(CoCreateInstance(CLSID_WICImagingFactory,
                                     nullptr,
                                     CLSCTX_INPROC_SERVER,
                                     IID_PPV_ARGS(&factory)),
                    "WIC factory creation");

    ComPtr<IWICBitmap> bitmap;
    require_success(factory->CreateBitmapFromMemory(frame.width,
                                                    frame.height,
                                                    GUID_WICPixelFormat32bppBGRA,
                                                    frame.stride,
                                                    static_cast<UINT>(frame.pixels.size()),
                                                    const_cast<BYTE*>(frame.pixels.data()),
                                                    &bitmap),
                    "WIC bitmap creation");

    ComPtr<IWICBitmapSource> source = bitmap;
    ComPtr<IWICBitmapScaler> scaler;
    if (output_width != frame.width || output_height != frame.height) {
        require_success(factory->CreateBitmapScaler(&scaler), "WIC scaler creation");
        require_success(scaler->Initialize(bitmap.Get(),
                                           output_width,
                                           output_height,
                                           WICBitmapInterpolationModeFant),
                        "WIC scaling");
        source = scaler;
    }

    ComPtr<IStream> stream;
    require_success(CreateStreamOnHGlobal(nullptr, TRUE, &stream), "JPEG stream creation");

    ComPtr<IWICBitmapEncoder> encoder;
    require_success(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder),
                    "JPEG encoder creation");
    require_success(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache),
                    "JPEG encoder initialization");

    ComPtr<IWICBitmapFrameEncode> encoded_frame;
    ComPtr<IPropertyBag2> properties;
    require_success(encoder->CreateNewFrame(&encoded_frame, &properties),
                    "JPEG frame creation");

    PROPBAG2 option{};
    option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
    VARIANT value;
    VariantInit(&value);
    value.vt = VT_R4;
    const auto clamped_quality =
        std::clamp(quality, kMinimumJpegQuality, kMaximumJpegQuality);
    value.fltVal = static_cast<float>(clamped_quality) / 100.0F;
    require_success(properties->Write(1, &option, &value), "JPEG quality configuration");

    require_success(encoded_frame->Initialize(properties.Get()), "JPEG frame initialization");
    require_success(encoded_frame->SetSize(output_width, output_height), "JPEG size setup");
    WICPixelFormatGUID pixel_format = GUID_WICPixelFormat24bppBGR;
    require_success(encoded_frame->SetPixelFormat(&pixel_format), "JPEG pixel format setup");
    require_success(encoded_frame->WriteSource(source.Get(), nullptr), "JPEG pixel encoding");
    require_success(encoded_frame->Commit(), "JPEG frame commit");
    require_success(encoder->Commit(), "JPEG encoder commit");

    STATSTG statistics{};
    require_success(stream->Stat(&statistics, STATFLAG_NONAME), "JPEG stream sizing");
    if (statistics.cbSize.QuadPart <= 0 ||
        static_cast<ULONGLONG>(statistics.cbSize.QuadPart) >
            std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error("encoded JPEG size is invalid");
    }
    LARGE_INTEGER beginning{};
    require_success(stream->Seek(beginning, STREAM_SEEK_SET, nullptr), "JPEG stream rewind");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(statistics.cbSize.QuadPart));
    ULONG bytes_read = 0;
    if (bytes.size() > std::numeric_limits<ULONG>::max()) {
        throw std::runtime_error("encoded JPEG is too large");
    }
    require_success(stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &bytes_read),
                    "JPEG stream read");
    if (bytes_read != bytes.size()) {
        throw std::runtime_error("encoded JPEG stream was truncated");
    }
    return {output_width, output_height, std::move(bytes)};
}

}
