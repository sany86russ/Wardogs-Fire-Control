#pragma once

#include "wardogs/ocr.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

// Frozen 2.12 preprocessing algorithms for exact paired comparison. Reference
// callers supply valid, bounded images; production validation is tested alone.
namespace ocr_preprocessing_reference {

inline void tensor(const wardogs::Image& image, std::vector<float>& output,
                   int& tensor_width, int crop_top, int crop_bottom) {
    constexpr int target_height = 48;
    const int source_height = image.height - crop_top - crop_bottom;
    const double ratio = static_cast<double>(image.width) / source_height;
    tensor_width = std::max(320,
        static_cast<int>(target_height * std::max(320.0 / 48.0, ratio)));
    const int resized_width = std::min(tensor_width,
        static_cast<int>(std::ceil(target_height * ratio)));
    output.assign(static_cast<std::size_t>(3 * target_height * tensor_width), 0.0F);
    const double scale_x = static_cast<double>(image.width) / resized_width;
    const double scale_y = static_cast<double>(source_height) / target_height;
    const std::size_t plane = static_cast<std::size_t>(target_height * tensor_width);
    for (int y = 0; y < target_height; ++y) {
        const double source_y = std::clamp(crop_top + (y + 0.5) * scale_y - 0.5,
            static_cast<double>(crop_top),
            static_cast<double>(image.height - crop_bottom - 1));
        const int y0 = static_cast<int>(std::floor(source_y));
        const int y1 = std::min(y0 + 1, image.height - crop_bottom - 1);
        const double fy = source_y - y0;
        for (int x = 0; x < resized_width; ++x) {
            const double source_x = std::clamp((x + 0.5) * scale_x - 0.5,
                0.0, static_cast<double>(image.width - 1));
            const int x0 = static_cast<int>(std::floor(source_x));
            const int x1 = std::min(x0 + 1, image.width - 1);
            const double fx = source_x - x0;
            for (int channel = 0; channel < 3; ++channel) {
                const auto sample = [&](int sx, int sy) {
                    return image.bgr[static_cast<std::size_t>(
                        (sy * image.width + sx) * 3 + channel)];
                };
                const double top = sample(x0, y0) * (1.0 - fx) + sample(x1, y0) * fx;
                const double bottom = sample(x0, y1) * (1.0 - fx) + sample(x1, y1) * fx;
                const double pixel = top * (1.0 - fy) + bottom * fy;
                output[static_cast<std::size_t>(channel) * plane +
                       static_cast<std::size_t>(y * tensor_width + x)] =
                    static_cast<float>(pixel / 127.5 - 1.0);
            }
        }
    }
}

inline void windows_pixels(const wardogs::Image& image, int scale,
                           bool high_contrast, std::vector<std::uint8_t>& bytes) {
    const int width = image.width * scale, height = image.height * scale;
    bytes.resize(static_cast<std::size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y) {
        const auto source_row = static_cast<std::size_t>((y / scale) * image.width * 3);
        for (int x = 0; x < width; ++x) {
            const auto input = source_row + static_cast<std::size_t>((x / scale) * 3);
            const auto output = static_cast<std::size_t>((y * width + x) * 4);
            if (high_contrast) {
                const int blue = image.bgr[input], green = image.bgr[input + 1], red = image.bgr[input + 2];
                const int chroma = std::max({blue, green, red}) - std::min({blue, green, red});
                const auto value = static_cast<std::uint8_t>(
                    chroma >= 40 && green > red && green > blue ? 0 : 255);
                bytes[output] = bytes[output + 1] = bytes[output + 2] = value;
            } else {
                std::copy_n(image.bgr.data() + input, 3, bytes.data() + output);
            }
            bytes[output + 3] = 255;
        }
    }
}

inline wardogs::Image pixels(int width, int height) {
    wardogs::Image image{width, height, {}};
    image.bgr.resize(static_cast<std::size_t>(width) * height * 3);
    std::uint32_t state = 0x9e3779b9U;
    for (auto& byte : image.bgr) {
        state = state * 1664525U + 1013904223U;
        byte = static_cast<std::uint8_t>(state >> 24);
    }
    return image;
}

}  // namespace ocr_preprocessing_reference
