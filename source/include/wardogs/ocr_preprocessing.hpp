#pragma once

#include "wardogs/ocr.hpp"

#include <span>

// Pure preprocessing boundaries shared by the providers, regression tests and
// console benchmarks. They neither capture a screen nor start an OCR provider.
namespace wardogs::detail {

void prepare_ocr_tensor(const Image& image, std::vector<float>& tensor, int& tensor_width,
                        int crop_top, int crop_bottom, std::stop_token stop);

void prepare_windows_ocr_pixels(const Image& image, int scale, bool high_contrast,
                               std::span<std::uint8_t> bgra, std::stop_token stop);

}  // namespace wardogs::detail
