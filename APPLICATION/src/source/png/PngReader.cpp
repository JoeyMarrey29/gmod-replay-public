#include "source/png/PngReader.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl.h>

#include <stdexcept>

using Microsoft::WRL::ComPtr;

namespace gmod::source::png {

PngReader::PngReader(std::filesystem::path path)
    : path_(std::move(path)) {}

vtf::Pixels PngReader::read() const {
    ComPtr<IWICImagingFactory> factory;
    auto result = CoCreateInstance(
        CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory));
    if (FAILED(result)) {
        throw std::runtime_error("Could not create WIC imaging factory.");
    }
    ComPtr<IWICBitmapDecoder> decoder;
    result = factory->CreateDecoderFromFilename(
        path_.c_str(), nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(result)) {
        throw std::runtime_error("Could not decode PNG: " + path_.string());
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    result = decoder->GetFrame(0, &frame);
    if (FAILED(result)) {
        throw std::runtime_error("Could not read PNG frame: " + path_.string());
    }
    UINT width = 0;
    UINT height = 0;
    frame->GetSize(&width, &height);
    ComPtr<IWICFormatConverter> converter;
    result = factory->CreateFormatConverter(&converter);
    if (FAILED(result)) {
        throw std::runtime_error("Could not create PNG format converter.");
    }
    result = converter->Initialize(
        frame.Get(), GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone, nullptr, 0.0,
        WICBitmapPaletteTypeCustom);
    if (FAILED(result)) {
        throw std::runtime_error("Could not convert PNG pixels.");
    }
    vtf::Pixels pixels{
        static_cast<int>(width),
        static_cast<int>(height),
        std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4)
    };
    result = converter->CopyPixels(
        nullptr, width * 4, static_cast<UINT>(pixels.rgba.size()),
        pixels.rgba.data());
    if (FAILED(result)) {
        throw std::runtime_error("Could not copy PNG pixels.");
    }
    return pixels;
}

} // namespace gmod::source::png
