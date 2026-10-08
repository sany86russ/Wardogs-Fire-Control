#include "wardogs/ocr.hpp"

#include <Windows.h>
#include <Psapi.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
std::string utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0,
                                         nullptr, nullptr);
    if (size <= 0) throw std::runtime_error("OCR text is not valid UTF-16");
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), result.data(), size,
                            nullptr, nullptr) != size)
        throw std::runtime_error("Cannot encode OCR text as UTF-8");
    return result;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3 || argc > 5 || (argc == 5 && std::wstring_view(argv[4]) != L"--details")) {
        std::wcerr << L"usage: ocr_benchmark <model.onnx> <image.png> [iterations] [--details]\n";
        return 2;
    }
    const int iterations = argc >= 4 ? std::clamp(_wtoi(argv[3]), 1, 10000) : 20;
    try {
        const auto image = wardogs::load_image_file(argv[2]);
        const auto start_load = std::chrono::steady_clock::now();
        wardogs::RapidOcr ocr(argv[1]);
        const auto end_load = std::chrono::steady_clock::now();
        wardogs::OcrResult result;
        std::vector<double> timings;
        timings.reserve(static_cast<std::size_t>(iterations));
        const auto start_runs = std::chrono::steady_clock::now();
        for (int i = 0; i < iterations; ++i) {
            const auto begin = std::chrono::steady_clock::now();
            result = ocr.recognize(image);
            timings.push_back(std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - begin).count());
        }
        const auto end_runs = std::chrono::steady_clock::now();
        PROCESS_MEMORY_COUNTERS_EX memory{};
        memory.cb = sizeof(memory);
        GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                             sizeof(memory));
        const auto load_ms = std::chrono::duration<double, std::milli>(end_load - start_load);
        const auto run_ms = std::chrono::duration<double, std::milli>(end_runs - start_runs);
        std::cout << "text=" << utf8(result.text) << '\n'
                  << "confidence=" << std::fixed << std::setprecision(5)
                  << result.confidence << '\n'
                  << "model_load_ms=" << load_ms.count() << '\n'
                  << "average_ocr_ms=" << run_ms.count() / iterations << '\n'
                  << "working_set_mib="
                  << memory.WorkingSetSize / 1024.0 / 1024.0 << '\n'
                  << "private_mib="
                  << memory.PrivateUsage / 1024.0 / 1024.0 << '\n';
        if (argc == 5) {
            std::sort(timings.begin(), timings.end());
            const auto assessment = wardogs::assess_ocr_result(result);
            std::cout << "minimum_confidence=" << result.minimum_confidence << '\n'
                      << "alternate_text=" << utf8(result.alternate_text) << '\n'
                      << "p50_ocr_ms=" << timings[timings.size() / 2] << '\n'
                      << "p95_ocr_ms=" << timings[(timings.size() - 1) * 95 / 100] << '\n'
                      << "match_count=" << assessment.match_count << '\n'
                      << "candidate_count=" << assessment.candidates.size() << '\n'
                      << "has_coordinate=" << assessment.selected.has_value() << '\n'
                      << "selected_x=" << (assessment.selected ? assessment.selected->x : 0) << '\n'
                      << "selected_y=" << (assessment.selected ? assessment.selected->y : 0) << '\n'
                      << "low_confidence=" << assessment.low_confidence << '\n'
                      << "ambiguous=" << assessment.ambiguous << '\n'
                      << "multiple_lines=" << assessment.multiple_lines << '\n'
                      << "pass_disagreement=" << assessment.pass_disagreement << '\n'
                      << "requires_confirmation=" << assessment.requires_confirmation() << '\n';
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
