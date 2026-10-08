#include "wardogs/ocr.hpp"
#include "wardogs/windows_ocr.hpp"

#include <winrt/base.h>
#include <Windows.h>
#include <Psapi.h>

#include <chrono>
#include <algorithm>
#include <iomanip>
#include <iostream>
#include <string>

namespace {
std::string utf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0,
                                         nullptr, nullptr);
    if (size <= 0) throw std::runtime_error("OCR text is not valid UTF-16");
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                        static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2 || argc > 3) {
        std::wcerr << L"usage: windows_ocr_probe <image.png> [iterations]\n";
        return 2;
    }
    try {
        const auto image = wardogs::load_image_file(argv[1]);
        wardogs::WindowsOcr ocr;
        const int iterations = argc == 3 ? std::clamp(_wtoi(argv[2]), 1, 10000) : 1;
        const auto begin = std::chrono::steady_clock::now();
        wardogs::OcrResult original;
        for (int index = 0; index < iterations; ++index) original = ocr.recognize(image);
        const auto middle = std::chrono::steady_clock::now();
        wardogs::OcrResult contrasted;
        for (int index = 0; index < iterations; ++index)
            contrasted = ocr.recognize_high_contrast(image);
        const auto end = std::chrono::steady_clock::now();
        std::cout << "original=" << utf8(original.text) << '\n'
                  << "high_contrast=" << utf8(contrasted.text) << '\n';
        PROCESS_MEMORY_COUNTERS_EX memory{sizeof(memory)};
        GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                             sizeof(memory));
        const auto assessment = wardogs::assess_ocr_result(original);
        std::cout << std::fixed << std::setprecision(2)
                  << "original_ms="
                  << std::chrono::duration<double, std::milli>(middle - begin).count() / iterations
                  << '\n' << "high_contrast_ms="
                  << std::chrono::duration<double, std::milli>(end - middle).count() / iterations
                  << '\n' << "private_mib=" << memory.PrivateUsage / 1024.0 / 1024.0
                  << '\n' << "working_set_mib=" << memory.WorkingSetSize / 1024.0 / 1024.0
                  << '\n' << "line_count=" << original.line_count
                  << '\n' << "candidate_count=" << assessment.candidates.size()
                  << '\n' << "has_coordinate=" << assessment.selected.has_value()
                  << '\n' << "selected_x=" << (assessment.selected ? assessment.selected->x : 0)
                  << '\n' << "selected_y=" << (assessment.selected ? assessment.selected->y : 0)
                  << '\n' << "requires_confirmation=" << assessment.requires_confirmation() << '\n';
        return 0;
    } catch (const winrt::hresult_error& error) {
        std::wcerr << L"HRESULT=0x" << std::hex
                   << static_cast<unsigned>(error.code()) << L" message="
                   << error.message().c_str() << L'\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
