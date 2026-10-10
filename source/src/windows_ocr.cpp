#include "wardogs/windows_ocr.hpp"
#include "wardogs/ocr_preprocessing.hpp"
#include "wardogs/logger.hpp"

#include <Windows.h>
#include <robuffer.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace wardogs {
namespace {

class MtaRuntime final {
public:
    MtaRuntime() {
        if (FAILED(CoIncrementMTAUsage(&cookie_)))
            throw std::runtime_error("Не удалось сохранить среду COM для Windows OCR");
    }
    ~MtaRuntime() {
        if (FAILED(CoDecrementMTAUsage(cookie_)))
            log_error("windows_ocr.mta_usage_release_failed");
    }
    MtaRuntime(const MtaRuntime&) = delete;
    MtaRuntime& operator=(const MtaRuntime&) = delete;

private:
    CO_MTA_USAGE_COOKIE cookie_{};
};

struct MtaApartment {
    MtaApartment() { winrt::init_apartment(winrt::apartment_type::multi_threaded); }
    ~MtaApartment() { winrt::uninit_apartment(); }
    MtaApartment(const MtaApartment&) = delete;
    MtaApartment& operator=(const MtaApartment&) = delete;
};

struct ProviderCapacity {
    std::mutex mutex;
    std::condition_variable_any changed;
    unsigned active{};
};

struct ProviderSlot {
    std::shared_ptr<ProviderCapacity> capacity;
    explicit ProviderSlot(std::shared_ptr<ProviderCapacity> value)
        : capacity(std::move(value)) {}
    ~ProviderSlot() {
        {
            std::lock_guard lock(capacity->mutex);
            --capacity->active;
        }
        capacity->changed.notify_all();
    }
};

void validate_image(const Image& image) {
    if (image.width <= 0 || image.height <= 0 || image.width > 16384 ||
        image.height > 16384 ||
        static_cast<std::size_t>(image.width) * image.height > 16'000'000 ||
        image.bgr.size() != static_cast<std::size_t>(image.width) * image.height * 3)
        throw std::invalid_argument("Windows OCR получил неверные размеры или пиксели изображения");
}

winrt::Windows::Graphics::Imaging::SoftwareBitmap software_bitmap(
    const Image& image, int scale, bool high_contrast, std::stop_token stop) {
    using namespace winrt::Windows::Graphics::Imaging;
    const int width = image.width * scale;
    const int height = image.height * scale;
    const auto byte_count = static_cast<std::uint32_t>(width * height * 4);
    winrt::Windows::Storage::Streams::Buffer buffer(byte_count);
    buffer.Length(byte_count);
    byte* bytes = nullptr;
    winrt::check_hresult(
        buffer.as<::Windows::Storage::Streams::IBufferByteAccess>()->Buffer(&bytes));
    detail::prepare_windows_ocr_pixels(image, scale, high_contrast, {bytes, byte_count}, stop);
    return SoftwareBitmap::CreateCopyFromBuffer(buffer, BitmapPixelFormat::Bgra8,
                                                 width, height,
                                                 BitmapAlphaMode::Ignore);
}

}  // namespace

namespace detail {

void prepare_windows_ocr_pixels(const Image& image, int scale, bool high_contrast,
                               std::span<std::uint8_t> bgra, std::stop_token stop) {
    validate_image(image);
    // The provider chooses ceil(96 / height), hence 1..96. Validate this pure
    // boundary independently so a bad scale cannot overflow or overrun a span.
    if (scale < 1 || scale > 96 ||
        static_cast<std::size_t>(image.width) * image.height * scale * scale > 16'000'000)
        throw std::invalid_argument("Неверный масштаб изображения Windows OCR");
    const auto stride = static_cast<std::size_t>(image.width) * scale * 4;
    if (bgra.size() != stride * image.height * scale)
        throw std::invalid_argument("Неверный размер буфера изображения Windows OCR");
    // Convert/mask each source pixel only once and copy the repeated nearest-
    // neighbour rows. No temporary image and no extra provider recognition.
    for (int y = 0; y < image.height; ++y) {
        if (stop.stop_requested()) throw std::runtime_error("Распознавание отменено");
        const auto* source = image.bgr.data() + static_cast<std::size_t>(y) * image.width * 3;
        auto* row = bgra.data() + static_cast<std::size_t>(y) * scale * stride;
        for (int x = 0; x < image.width; ++x) {
            auto* output = row + static_cast<std::size_t>(x) * scale * 4;
            if (high_contrast) {
                const int blue = source[x * 3], green = source[x * 3 + 1], red = source[x * 3 + 2];
                const int chroma = std::max({blue, green, red}) - std::min({blue, green, red});
                const auto value = static_cast<std::uint8_t>(chroma >= 40 && green > red && green > blue ? 0 : 255);
                output[0] = output[1] = output[2] = value;
            } else {
                std::copy_n(source + x * 3, 3, output);
            }
            output[3] = 255;
            for (int repeat = 1; repeat < scale; ++repeat)
                std::copy_n(output, 4, output + repeat * 4);
        }
        for (int repeat = 1; repeat < scale; ++repeat) {
            if (stop.stop_requested()) throw std::runtime_error("Распознавание отменено");
            std::copy_n(row, stride, row + repeat * stride);
        }
    }
}

// Process-wide, including reconstructed WindowsOcr instances. A cancelled
// provider may retain its callback/bitmap for an arbitrarily long time; its
// completion owns this slot until those resources are actually released.
std::shared_ptr<void> acquire_windows_ocr_provider_slot(std::stop_token stop) {
    static const auto capacity = std::make_shared<ProviderCapacity>();
    std::unique_lock lock(capacity->mutex);
    const bool available = capacity->changed.wait_until(
        lock, stop, std::chrono::steady_clock::now() + std::chrono::milliseconds(100),
        [&] { return capacity->active < 2; });
    if (stop.stop_requested()) throw std::runtime_error("Распознавание отменено");
    if (!available)
        throw std::runtime_error("Предыдущее Windows OCR ещё завершается. Повторите захват через несколько секунд.");
    const auto slot = std::make_shared<ProviderSlot>(capacity);
    ++capacity->active;
    return slot;
}

}  // namespace detail

struct WindowsOcr::Impl {
    // Engine objects outlive their creating worker. Keep the MTA alive without
    // leaking that worker's CoInitialize count or tying cleanup to its thread.
    std::shared_ptr<MtaRuntime> runtime = std::make_shared<MtaRuntime>();
    winrt::Windows::Media::Ocr::OcrEngine engine{nullptr};
    std::timed_mutex recognition_mutex;

    Impl() {
        const MtaApartment apartment;
        ensure_engine();
    }

    void ensure_engine() {
        if (engine) return;
        engine = winrt::Windows::Media::Ocr::OcrEngine::TryCreateFromLanguage(
            winrt::Windows::Globalization::Language(L"en-US"));
        if (!engine) {
            throw std::runtime_error("В Windows не установлен английский пакет OCR (en-US)");
        }
    }

    OcrResult run(const Image& image, bool high_contrast, std::stop_token stop) {
        if (stop.stop_requested()) throw std::runtime_error("Распознавание отменено");
        validate_image(image);
        std::unique_lock recognition_lock(recognition_mutex, std::defer_lock);
        while (!recognition_lock.try_lock_for(std::chrono::milliseconds(10)))
            if (stop.stop_requested()) throw std::runtime_error("Распознавание отменено");
        // Consecutive captures may run on different std::jthread instances.
        // Each calling worker must enter (and leave) its own WinRT apartment.
        const MtaApartment apartment;
        if (stop.stop_requested()) throw std::runtime_error("Распознавание отменено");
        ensure_engine();
        const int scale = image.height >= 96 ? 1 : (96 + image.height - 1) / image.height;
        const int maximum = static_cast<int>(winrt::Windows::Media::Ocr::OcrEngine::MaxImageDimension());
        if (image.width > maximum / scale || image.height > maximum / scale ||
            static_cast<std::size_t>(image.width) * image.height * scale * scale > 16'000'000)
            throw std::invalid_argument("Область распознавания превышает предел Windows OCR: выделите строку координат");
        const auto provider_slot = detail::acquire_windows_ocr_provider_slot(stop);
        const auto bitmap = software_bitmap(image, scale, high_contrast, stop);
        const auto operation = engine.RecognizeAsync(bitmap);
        struct Completion {
            // Declaration order releases the bitmap before the last MTA lease.
            std::shared_ptr<MtaRuntime> runtime;
            std::shared_ptr<void> provider_slot;
            winrt::Windows::Media::Ocr::OcrEngine engine{nullptr};
            winrt::Windows::Graphics::Imaging::SoftwareBitmap bitmap{nullptr};
            std::mutex mutex;
            std::condition_variable_any changed;
            bool finished{};
        };
        // The shared completion state outlives this call when a cancelled
        // WinRT provider completes late. It never captures a window or Impl.
        const auto completion = std::make_shared<Completion>();
        completion->runtime = runtime;
        completion->provider_slot = provider_slot;
        completion->engine = engine;
        completion->bitmap = bitmap;
        operation.Completed([completion](auto const&, auto) {
            {
                std::lock_guard lock(completion->mutex);
                completion->finished = true;
            }
            completion->changed.notify_all();
        });
        std::unique_lock lock(completion->mutex);
        const bool finished = completion->changed.wait_until(
            lock, stop, std::chrono::steady_clock::now() + std::chrono::seconds(3),
            [&] { return completion->finished; });
        lock.unlock();
        if (!finished || stop.stop_requested()) {
            // Cancel is asynchronous: this provider may still reject another
            // RecognizeAsync as "already running". Retire it immediately for
            // the next capture; completion owns the old engine until it ends.
            engine = nullptr;
            operation.Cancel();
            throw std::runtime_error(stop.stop_requested()
                ? "Распознавание отменено"
                : "Windows OCR не ответил за 3 секунды. Уменьшите область или повторите захват");
        }
        const auto result = operation.GetResults();
        OcrResult recognized;
        recognized.text = result.Text().c_str();
        recognized.line_count = result.Lines().Size();
        return recognized;
    }
};

WindowsOcr::WindowsOcr() : impl_(std::make_unique<Impl>()) {}
WindowsOcr::~WindowsOcr() = default;
WindowsOcr::WindowsOcr(WindowsOcr&&) noexcept = default;
WindowsOcr& WindowsOcr::operator=(WindowsOcr&&) noexcept = default;

OcrResult WindowsOcr::recognize(const Image& image, std::stop_token stop) const {
    if (stop.stop_requested()) throw std::runtime_error("Распознавание отменено");
    return impl_->run(image, false, stop);
}

OcrResult WindowsOcr::recognize_high_contrast(const Image& image, std::stop_token stop) const {
    if (stop.stop_requested()) throw std::runtime_error("Распознавание отменено");
    return impl_->run(image, true, stop);
}

}  // namespace wardogs
