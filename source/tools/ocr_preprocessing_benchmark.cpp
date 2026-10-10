#include "wardogs/ocr_preprocessing.hpp"
#include "../tests/ocr_preprocessing_reference.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
template<class Work>
double time_batch(Work&& work, int iterations) {
    const auto begin = std::chrono::steady_clock::now();
    for (int i = 0; i < iterations; ++i) work();
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - begin).count() / iterations;
}

template<class Before, class After, class Equal>
void paired(const char* name, int iterations, Before&& before, After&& after, Equal&& equal) {
    before(); after();
    if (!equal()) throw std::runtime_error("Preprocessing pixels differ from the 2.12 reference");
    std::vector<double> baseline, candidate;
    for (int pair = 0; pair < 7; ++pair) {
        if (pair % 2 == 0) {
            baseline.push_back(time_batch(before, iterations));
            candidate.push_back(time_batch(after, iterations));
        } else {
            candidate.push_back(time_batch(after, iterations));
            baseline.push_back(time_batch(before, iterations));
        }
        if (!equal()) throw std::runtime_error("Repeated preprocessing changed reference pixels");
    }
    std::cout << "{\"case\":\"" << name << "\",\"iterations_per_batch\":" << iterations
              << ",\"reference_samples_ms\":[";
    for (std::size_t i = 0; i < baseline.size(); ++i)
        std::cout << (i ? "," : "") << baseline[i];
    std::cout << "],\"optimized_samples_ms\":[";
    for (std::size_t i = 0; i < candidate.size(); ++i)
        std::cout << (i ? "," : "") << candidate[i];
    std::sort(baseline.begin(), baseline.end());
    std::sort(candidate.begin(), candidate.end());
    std::cout << "],\"reference_median_ms\":" << baseline[3]
              << ",\"optimized_median_ms\":" << candidate[3]
              << ",\"latency_change_percent\":" << (candidate[3] / baseline[3] - 1.0) * 100
              << ",\"bit_identical\":true}";
}

void tensor_case(const char* name, const wardogs::Image& image, int iterations) {
    std::vector<float> before, after;
    int width_before = 0, width_after = 0;
    paired(name, iterations,
        [&] { ocr_preprocessing_reference::tensor(image, before, width_before, 0, 0); },
        [&] { wardogs::detail::prepare_ocr_tensor(image, after, width_after, 0, 0, {}); },
        [&] { return width_before == width_after && before.size() == after.size() &&
                     std::memcmp(before.data(), after.data(), before.size() * sizeof(float)) == 0; });
}

void bitmap_case(const char* name, const wardogs::Image& image, bool high_contrast, int iterations) {
    const int scale = image.height >= 96 ? 1 : (96 + image.height - 1) / image.height;
    std::vector<std::uint8_t> before, after(
        static_cast<std::size_t>(image.width) * image.height * scale * scale * 4);
    paired(name, iterations,
        [&] { ocr_preprocessing_reference::windows_pixels(image, scale, high_contrast, before); },
        [&] { wardogs::detail::prepare_windows_ocr_pixels(image, scale, high_contrast, after, {}); },
        [&] { return before == after; });
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3 || argc > 4) {
        std::cerr << "usage: ocr_preprocessing_benchmark <coordinate.png> <native-map-x.png> [iterations]\n";
        return 2;
    }
    try {
        const auto coordinate = wardogs::load_image_file(argv[1]);
        const auto map = wardogs::load_image_file(argv[2]);
        const int iterations = argc == 4 ? std::clamp(_wtoi(argv[3]), 1, 1000) : 100;
        const auto wide = ocr_preprocessing_reference::pixels(4096, 48);
        const auto large = ocr_preprocessing_reference::pixels(1600, 900);
        std::cout << std::fixed << std::setprecision(6)
                  << "{\"scope\":\"pure preprocessing paired reference; no capture, GUI or model inference\",\"cases\":[";
        tensor_case("tensor_retained_coordinate", coordinate, iterations);
        std::cout << ','; tensor_case("tensor_retained_map", map, iterations);
        std::cout << ','; tensor_case("tensor_maximum_width_synthetic", wide, iterations);
        std::cout << ','; bitmap_case("bitmap_retained_coordinate_color", coordinate, false, iterations);
        std::cout << ','; bitmap_case("bitmap_retained_coordinate_mask", coordinate, true, iterations);
        std::cout << ','; bitmap_case("bitmap_retained_map_color", map, false, iterations);
        std::cout << ','; bitmap_case("bitmap_retained_map_mask", map, true, iterations);
        std::cout << ','; bitmap_case("bitmap_large_synthetic_color", large, false, std::max(1, iterations / 10));
        std::cout << ','; bitmap_case("bitmap_large_synthetic_mask", large, true, std::max(1, iterations / 10));
        std::cout << "]}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Preprocessing benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
