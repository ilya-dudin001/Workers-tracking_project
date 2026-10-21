#include "worker_agent/image_processor.h"

#include <gtest/gtest.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;
using monitor::worker_agent::BgraFrame;
using monitor::worker_agent::EncodedImage;
using monitor::worker_agent::WicImageProcessor;

class TestComApartment {
public:
    TestComApartment() : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~TestComApartment() {
        if (SUCCEEDED(result_)) {
            CoUninitialize();
        }
    }

    TestComApartment(const TestComApartment&) = delete;
    TestComApartment& operator=(const TestComApartment&) = delete;

private:
    HRESULT result_;
};

BgraFrame synthetic_frame(const std::uint32_t width, const std::uint32_t height) {
    const auto stride = width * 4U;
    BgraFrame result{width,
                     height,
                     stride,
                     std::vector<std::uint8_t>(static_cast<std::size_t>(stride) * height),
                     1,
                     1};
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto offset = static_cast<std::size_t>(y) * stride + x * 4U;
            result.pixels[offset] = static_cast<std::uint8_t>(x);
            result.pixels[offset + 1] = static_cast<std::uint8_t>(y);
            result.pixels[offset + 2] = static_cast<std::uint8_t>(x + y);
            result.pixels[offset + 3] = 0xFF;
        }
    }
    return result;
}

std::pair<UINT, UINT> decoded_dimensions(const std::vector<std::uint8_t>& jpeg) {
    const TestComApartment apartment;

    ComPtr<IWICImagingFactory> factory;
    HRESULT result = CoCreateInstance(CLSID_WICImagingFactory,
                                      nullptr,
                                      CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&factory));
    if (SUCCEEDED(result)) {
        ComPtr<IWICStream> stream;
        result = factory->CreateStream(&stream);
        if (SUCCEEDED(result)) {
            result = stream->InitializeFromMemory(
                const_cast<BYTE*>(jpeg.data()), static_cast<DWORD>(jpeg.size()));
        }
        ComPtr<IWICBitmapDecoder> decoder;
        if (SUCCEEDED(result)) {
            result = factory->CreateDecoderFromStream(
                stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
        }
        ComPtr<IWICBitmapFrameDecode> frame;
        if (SUCCEEDED(result)) {
            result = decoder->GetFrame(0, &frame);
        }
        UINT width = 0;
        UINT height = 0;
        if (SUCCEEDED(result)) {
            result = frame->GetSize(&width, &height);
        }
        if (FAILED(result)) {
            throw std::runtime_error("WIC could not decode encoded JPEG");
        }
        return {width, height};
    }

    throw std::runtime_error("WIC factory creation failed");
}

TEST(WicImageProcessor, ScalesToFitPreservesAspectAndProducesDecodableJpeg) {
    WicImageProcessor processor;
    const EncodedImage image = processor.encode_jpeg(synthetic_frame(800, 600), 480, 270, 75);

    EXPECT_EQ(image.width, 360U);
    EXPECT_EQ(image.height, 270U);
    EXPECT_GT(image.jpeg.size(), 4U);
    EXPECT_EQ(image.jpeg.front(), 0xFFU);
    EXPECT_EQ(image.jpeg[1], 0xD8U);
    EXPECT_EQ(decoded_dimensions(image.jpeg), std::make_pair(360U, 270U));
}

TEST(WicImageProcessor, NeverUpscalesAndClampsQualityToDocumentedRange) {
    WicImageProcessor processor;
    const auto low = processor.encode_jpeg(synthetic_frame(100, 50), 480, 270, 0);
    const auto high = processor.encode_jpeg(synthetic_frame(100, 50), 480, 270, 500);

    EXPECT_EQ(low.width, 100U);
    EXPECT_EQ(low.height, 50U);
    EXPECT_EQ(decoded_dimensions(low.jpeg), std::make_pair(100U, 50U));
    EXPECT_EQ(decoded_dimensions(high.jpeg), std::make_pair(100U, 50U));
}

TEST(WicImageProcessor, RejectsInvalidFramesAndZeroBounds) {
    WicImageProcessor processor;
    auto invalid = synthetic_frame(2, 2);
    invalid.pixels.pop_back();

    EXPECT_THROW(static_cast<void>(processor.encode_jpeg(invalid, 2, 2, 75)),
                 std::invalid_argument);
    EXPECT_THROW(static_cast<void>(processor.encode_jpeg(synthetic_frame(2, 2), 0, 2, 75)),
                 std::invalid_argument);
}

}
