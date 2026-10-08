#include "wardogs/ocr.hpp"

#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>
#include <limits>
#include <stdexcept>

namespace wardogs {
namespace {

class ComApartment final {
public:
    ComApartment() : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {
        if (FAILED(result_) && result_ != RPC_E_CHANGED_MODE)
            throw std::runtime_error("Не удалось инициализировать COM для загрузки изображения");
    }
    ~ComApartment() {
        // S_FALSE still increments the COM initialization count. An existing
        // STA returns RPC_E_CHANGED_MODE and must remain owned by its caller.
        if (SUCCEEDED(result_)) CoUninitialize();
    }
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

private:
    HRESULT result_;
};

}  // namespace

Image load_image_file(const std::filesystem::path& path) {
    const ComApartment apartment;

    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    HRESULT result = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                      CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(result)) {
        throw std::runtime_error("Не удалось создать загрузчик изображений Windows");
    }
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    result = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(result)) {
        throw std::runtime_error("Не удалось открыть изображение: проверьте файл и его формат");
    }
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    result = decoder->GetFrame(0, &frame);
    if (FAILED(result)) {
        throw std::runtime_error("Не удалось прочитать кадр изображения");
    }
    UINT width = 0;
    UINT height = 0;
    result = frame->GetSize(&width, &height);
    if (FAILED(result))
        throw std::runtime_error("Не удалось определить размеры изображения");
    constexpr std::uint64_t maximum_pixels = 16'000'000;
    constexpr UINT maximum_dimension = 16384;
    const auto pixels = static_cast<std::uint64_t>(width) * height;
    if (width == 0 || height == 0 || width > maximum_dimension ||
        height > maximum_dimension || pixels > maximum_pixels)
        throw std::invalid_argument("Изображение слишком большое: допустимо до 16 млн пикселей и 16384 пикселей по каждой стороне");
    const auto stride = static_cast<std::uint64_t>(width) * 3;
    const auto byte_count = stride * height;
    if (stride > std::numeric_limits<UINT>::max() ||
        byte_count > std::numeric_limits<UINT>::max() ||
        byte_count > std::numeric_limits<std::size_t>::max())
        throw std::invalid_argument("Размер пикселей изображения не поддерживается загрузчиком Windows");

    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    result = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(result)) {
        result = converter->Initialize(frame.Get(), GUID_WICPixelFormat24bppBGR,
                                       WICBitmapDitherTypeNone, nullptr, 0,
                                       WICBitmapPaletteTypeCustom);
    }
    if (FAILED(result)) {
        throw std::runtime_error("Не удалось преобразовать изображение в BGR");
    }

    Image image{static_cast<int>(width), static_cast<int>(height), {}};
    image.bgr.resize(static_cast<std::size_t>(byte_count));
    result = converter->CopyPixels(nullptr, static_cast<UINT>(stride),
                                   static_cast<UINT>(image.bgr.size()), image.bgr.data());
    if (FAILED(result)) {
        throw std::runtime_error("Не удалось прочитать пиксели изображения");
    }
    return image;
}

}  // namespace wardogs
