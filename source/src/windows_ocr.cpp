#include "wardogs/windows_ocr.hpp"
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
    // Scale/mask directly into the WinRT buffer: no full-size temporary BGR
    // image or second upscaled copy, with exactly the previous nearest pixels.
    for (int y = 0; y < height; ++y) {
        if (stop.stop_requested()) throw std::runtime_error("Распознавание отменено");
        const auto source_row = static_cast<std::size_t>((y / scale) * image.width * 3);
        for (int x = 0; x < width; ++x) {
            const auto input = source_row + static_cast<std::size_t>((x / scale) * 3);
            const auto output = static_cast<std::size_t>((y * width + x) * 4);
            if (high_contrast) {
                const int blue = image.bgr[input];
                const int green = image.bgr[input + 1];
                const int red = image.bgr[input + 2];
                const int chroma = std::max({blue, green, red}) - std::min({blue, green, red});
                const byte value = chroma >= 40 && green > red && green > blue ? 0 : 255;
                bytes[output] = bytes[output + 1] = bytes[output + 2] = value;
            } else {
                std::copy_n(image.bgr.data() + input, 3, bytes + output);
            }
            bytes[output + 3] = 255;
        }
    }
    return SoftwareBitmap::CreateCopyFromBuffer(buffer, BitmapPixelFormat::Bgra8,
                                                 width, height,
                                                 BitmapAlphaMode::Ignore);
}

}  // namespace

namespace detail {

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
