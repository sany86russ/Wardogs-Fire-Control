#include "wardogs/core.hpp"
#include "wardogs/ocr.hpp"
#include "wardogs/windows_ocr.hpp"

#include <Windows.h>
#include <winrt/base.h>

#include <exception>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>

// Test the same process-wide admission gate used immediately before bitmap
// allocation. Opaque leases model late cancelled completions without hanging
// an actual system provider or depending on timing of its private cleanup.
namespace wardogs::detail {
std::shared_ptr<void> acquire_windows_ocr_provider_slot(std::stop_token stop);
}

namespace {
int failures = 0;
void check(bool value, const char* message) {
    if (!value) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}
}  // namespace

int run_tests() {
    try {
        const auto image = wardogs::load_image_file(WARDOGS_TEST_IMAGE);
        const auto caller_sta = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        check(SUCCEEDED(caller_sta),
              "loading a fixture does not leave a hidden COM reference before engine creation");
        if (SUCCEEDED(caller_sta)) CoUninitialize();
        std::unique_ptr<wardogs::WindowsOcr> ocr;
        try {
            ocr = std::make_unique<wardogs::WindowsOcr>();
        } catch (const std::runtime_error& error) {
            if (std::string_view(error.what()) ==
                "В Windows не установлен английский пакет OCR (en-US)") {
                if (failures) return 1;
                std::cout << "SKIP: Windows English OCR language is not installed\n";
                return 77;
            }
            throw;
        }
        // MainWindow retains its OCR engine but starts a fresh worker for every
        // capture. Reproduce that lifetime rather than testing only one thread.
        for (int capture = 0; capture < 2; ++capture) {
            wardogs::OcrResult result;
            std::exception_ptr failure;
            std::jthread worker([&] {
                try {
                    result = ocr->recognize(image);
                } catch (...) {
                    failure = std::current_exception();
                }
            });
            worker.join();
            if (failure) std::rethrow_exception(failure);
            check(wardogs::parse_ocr_coordinate(result.text) ==
                      wardogs::Point{114.51, 191.81},
                  "retained Windows OCR engine reads the golden image from each new worker");
            check(result.confidence == 0.0F,
                  "Windows OCR does not invent confidence unsupported by its API");
            check(result.minimum_confidence == 0.0F && result.line_count == 1,
                  "Windows OCR exposes real line count without invented weakest-glyph confidence");
        }
        std::stop_source cancellation;
        cancellation.request_stop();
        try {
            (void)ocr->recognize(image, cancellation.get_token());
            check(false, "Windows OCR respects a pre-cancelled capture");
        } catch (const std::runtime_error& error) {
            check(std::string_view(error.what()) == "Распознавание отменено",
                  "pre-cancellation reports a clear cancellation reason");
        }
        try {
            (void)ocr->recognize({226, 47, {}});
            check(false, "invalid BGR cannot reach the Windows buffer conversion");
        } catch (const std::invalid_argument&) {
        }
        try {
            (void)ocr->recognize_high_contrast({1, 1, {255}});
            check(false, "high-contrast preprocessing rejects incomplete pixels");
        } catch (const std::invalid_argument&) {
        }
        try {
            (void)ocr->recognize({4096, 1, std::vector<std::uint8_t>(4096 * 3)});
            check(false, "upscaling beyond the engine limit is rejected before buffer allocation");
        } catch (const std::invalid_argument&) {
        }
        {
            auto first_pending = wardogs::detail::acquire_windows_ocr_provider_slot({});
            auto second_pending = wardogs::detail::acquire_windows_ocr_provider_slot({});
            const auto refused_at = std::chrono::steady_clock::now();
            try {
                (void)ocr->recognize(image);
                check(false, "a third unfinished provider cannot allocate another OCR bitmap");
            } catch (const std::runtime_error& error) {
                check(std::string_view(error.what()) ==
                          "Предыдущее Windows OCR ещё завершается. Повторите захват через несколько секунд.",
                      "provider saturation reports a clear bounded retry failure");
            }
            check(std::chrono::steady_clock::now() - refused_at < std::chrono::seconds(1),
                  "provider saturation never blocks the caller indefinitely");
            std::stop_source waiting_stop;
            std::exception_ptr waiting_error;
            std::jthread waiting([&] {
                try { (void)ocr->recognize(image, waiting_stop.get_token()); }
                catch (...) { waiting_error = std::current_exception(); }
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            waiting_stop.request_stop();
            waiting.join();
            check(waiting_error != nullptr,
                  "a caller waiting for provider capacity honours stop requests");
            if (waiting_error) {
                try { std::rethrow_exception(waiting_error); }
                catch (const std::runtime_error& error) {
                    check(std::string_view(error.what()) == "Распознавание отменено",
                          "provider-capacity cancellation preserves the normal cancellation reason");
                }
            }
            first_pending.reset();
            check(wardogs::parse_ocr_coordinate(ocr->recognize(image).text) ==
                      wardogs::Point{114.51, 191.81},
                  "releasing one late completion permits a fresh exact recognition");
        }

        // Serialized calls from unrelated workers retain the same engine and
        // yield exact coordinates; cancellation of one caller cannot poison it.
        std::exception_ptr concurrent_failure[2];
        wardogs::OcrResult concurrent_results[2];
        std::jthread concurrent_first([&] {
            try { concurrent_results[0] = ocr->recognize(image); }
            catch (...) { concurrent_failure[0] = std::current_exception(); }
        });
        std::jthread concurrent_second([&] {
            try { concurrent_results[1] = ocr->recognize(image); }
            catch (...) { concurrent_failure[1] = std::current_exception(); }
        });
        concurrent_first.join();
        concurrent_second.join();
        for (int index = 0; index < 2; ++index) {
            if (concurrent_failure[index]) std::rethrow_exception(concurrent_failure[index]);
            check(wardogs::parse_ocr_coordinate(concurrent_results[index].text) ==
                      wardogs::Point{114.51, 191.81},
                  "concurrent workers cannot corrupt a retained Windows OCR engine");
        }
        const wardogs::Image large_blank{4096, 1024,
            std::vector<std::uint8_t>(4096 * 1024 * 3, 255)};
        std::stop_source active_cancellation;
        std::promise<void> started;
        std::exception_ptr cancellation_failure;
        std::jthread interrupted([&] {
            started.set_value();
            try {
                (void)ocr->recognize(large_blank, active_cancellation.get_token());
            } catch (...) {
                cancellation_failure = std::current_exception();
            }
        });
        started.get_future().wait();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        const auto cancel_started = std::chrono::steady_clock::now();
        active_cancellation.request_stop();
        interrupted.join();
        check(std::chrono::steady_clock::now() - cancel_started < std::chrono::seconds(1),
              "cancelling a large native OCR capture returns within one second");
        // Completion may race a stop request; either natural completion or an
        // explicit cancellation is valid, but arbitrary provider errors are not.
        if (cancellation_failure) {
            try { std::rethrow_exception(cancellation_failure); }
            catch (const std::runtime_error& error) {
                check(std::string_view(error.what()) == "Распознавание отменено",
                      "an interrupted provider reports explicit cancellation");
            }
        }
        std::cout << "Windows active cancellation: "
                  << (cancellation_failure ? "cancelled" : "completed before stop") << '\n';
        check(wardogs::parse_ocr_coordinate(ocr->recognize(image).text) ==
                  wardogs::Point{114.51, 191.81},
              "engine reuse immediately after active cancellation still reads exact coordinates");
        if (failures) return 1;
        std::cout << "All Windows OCR tests passed\n";
        return 0;
    } catch (const winrt::hresult_error& error) {
        std::wcerr << L"FAIL: Windows OCR HRESULT 0x" << std::hex
                   << static_cast<unsigned>(error.code()) << L" "
                   << error.message().c_str() << L'\n';
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
    }
    return 1;
}

int main() {
    int status = 1;
    std::jthread isolated{[&] {
        APTTYPE type{};
        APTTYPEQUALIFIER qualifier{};
        check(CoGetApartmentType(&type, &qualifier) == CO_E_NOTINITIALIZED,
              "Windows OCR lifecycle starts on a fresh thread without COM initialization");
        if (failures) return;
        // Recreate the engine after every temporary caller apartment has ended.
        // Its explicit runtime lease must support both worker reuse and teardown.
        for (int lifetime = 0; lifetime < 2; ++lifetime) {
            status = run_tests();
            if (status != 0) break;
        }
    }};
    isolated.join();
    return status;
}
