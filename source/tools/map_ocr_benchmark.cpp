#include "wardogs/ocr.hpp"

#include <Windows.h>
#include <Psapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
void paste(wardogs::Image& target, const wardogs::Image& source, int left, int top) {
    if (left < 0 || top < 0 || left > target.width - source.width || top > target.height - source.height)
        throw std::invalid_argument("Source pixels do not fit the composed neighborhood");
    for (int y = 0; y < source.height; ++y)
        std::copy_n(source.bgr.data() + static_cast<std::size_t>(y) * source.width * 3,
                    static_cast<std::size_t>(source.width) * 3,
                    target.bgr.data() + (static_cast<std::size_t>(top + y) * target.width + left) * 3);
}
bool trusted(const wardogs::OcrResult& result) {
    const auto assessment = wardogs::assess_ocr_result(result);
    return assessment.selected && assessment.confidence_available && !assessment.requires_confirmation() &&
           result.map_axes_labeled && result.isolated_coordinate_pair && result.coordinate_passes_agree &&
           result.coordinate_glyph_count_matches && !result.coordinate_boundary_clipped;
}
template <class Read>
std::vector<double> measure(Read read, int count, wardogs::OcrResult& result) {
    std::vector<double> timings;
    read(); // Warm up the same retained session, excluded from timed iterations.
    for (int i = 0; i < count; ++i) {
        const auto begin = std::chrono::steady_clock::now();
        result = read();
        timings.push_back(std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - begin).count());
    }
    std::sort(timings.begin(), timings.end());
    return timings;
}
void report(const char* name, const std::vector<double>& timings, const wardogs::OcrResult& result) {
    const auto assessment = wardogs::assess_ocr_result(result);
    const auto p95 = static_cast<std::size_t>(std::ceil(timings.size() * 0.95)) - 1;
    std::cout << name << "_p50_ms=" << timings[timings.size() / 2] << '\n'
              << name << "_p95_ms=" << timings[p95] << '\n'
              << name << "_trusted=" << trusted(result) << '\n'
              << name << "_minimum_confidence=" << result.minimum_confidence << '\n'
              << name << "_x=" << (assessment.selected ? assessment.selected->x : 0) << '\n'
              << name << "_y=" << (assessment.selected ? assessment.selected->y : 0) << '\n';
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    if ((argc != 5 && argc != 8) || (argc == 8 && std::wstring_view(argv[5]) != L"--displace")) {
        std::cerr << "usage: map_ocr_benchmark <model.onnx> <x.png> <y.png> <iterations> [--displace dx dy]\n";
        return 2;
    }
    try {
        const auto x = wardogs::load_image_file(argv[2]), y = wardogs::load_image_file(argv[3]);
        const double scale = x.width / 140.0;
        const auto pixels = [scale](int value) { return static_cast<int>(std::lround(value * scale)); };
        wardogs::Image image{pixels(480), pixels(352), {}};
        image.bgr.assign(static_cast<std::size_t>(image.width) * image.height * 3, 20);
        const int xx = pixels(212), xy = pixels(178), yx = pixels(180), yy = pixels(104);
        const wardogs::MapOcrSearchLayout layout{{xx, xy, xx + x.width, xy + x.height},
            {yx, yy, yx + y.width, yy + y.height}, pixels(192), pixels(224), scale};
        const int dx = argc == 8 ? _wtoi(argv[6]) : 0, dy = argc == 8 ? _wtoi(argv[7]) : 0;
        paste(image, x, xx + dx, xy + dy); paste(image, y, yx + dx, yy + dy);
        const int iterations = std::clamp(_wtoi(argv[4]), 1, 100);
        wardogs::RapidOcr ocr(argv[1]);
        wardogs::OcrResult legacy, neighborhood;
        const auto legacy_times = measure([&] { return ocr.recognize_map_coordinates(x, y); }, iterations, legacy);
        const auto search_times = measure([&] { return ocr.recognize_map_neighborhood(image, layout); }, iterations, neighborhood);
        std::cout << std::fixed << std::setprecision(3)
                  << "fixture_kind=composed_retained_native_crop_pixels\n"
                  << "iterations=" << iterations << '\n'
                  << "search_width=" << image.width << '\n' << "search_height=" << image.height << '\n';
        report("legacy", legacy_times, legacy); report("neighborhood", search_times, neighborhood);
        PROCESS_MEMORY_COUNTERS_EX memory{};
        memory.cb = sizeof(memory);
        if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)))
            throw std::runtime_error("Cannot query benchmark process memory");
        std::cout << "private_mib=" << memory.PrivateUsage / 1024.0 / 1024.0 << '\n';
        const bool equal = wardogs::assess_ocr_result(legacy).selected == wardogs::assess_ocr_result(neighborhood).selected;
        return trusted(legacy) && trusted(neighborhood) && equal ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "Map benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
