#include "wardogs/sph_hud_ocr.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace wardogs {
namespace {

Image instrument_crop(const Image& image, ImageRect rect, bool numeric_recognition) {
    if (rect.left < 0 || rect.top < 0 || rect.right > image.width || rect.bottom > image.height ||
        rect.right <= rect.left || rect.bottom <= rect.top)
        throw std::invalid_argument("Invalid HUD instrument crop");
    const int width = rect.right - rect.left, height = rect.bottom - rect.top;
    // Small, isolated labels need magnification; cap the provider input rather
    // than sending the complete game frame through its text detector.
    const int scale = numeric_recognition ? std::clamp((96 + height - 1) / height, 1, 8) : 3;
    const int output_height = height * scale;
    const auto count = static_cast<std::uint64_t>(width) * scale * output_height;
    if (count > 1'000'000) throw std::invalid_argument("HUD label is too large");
    Image result{width * scale, output_height, std::vector<std::uint8_t>(count * 3)};
    for (int y = 0; y < result.height; ++y)
        for (int x = 0; x < result.width; ++x) {
            const auto source_y = y / scale;
            const auto source = (static_cast<std::size_t>(rect.top + source_y) * image.width + rect.left + x / scale) * 3;
            const auto destination = (static_cast<std::size_t>(y) * result.width + x) * 3;
            std::copy_n(image.bgr.begin() + source, 3, result.bgr.begin() + destination);
        }
    return result;
}

void read_region(SphHudFrame& frame, const Image& image, WindowsOcr& ocr,
                 ImageRect rect, std::stop_token stop, RapidOcr* recognition = nullptr,
                 bool numeric = false) {
    if (stop.stop_requested()) throw std::runtime_error("HUD recognition canceled");
    if (rect.right <= rect.left || rect.bottom <= rect.top) return;
    const auto cropped = instrument_crop(image, rect, recognition != nullptr || numeric);
    auto raw = recognition ? recognition->recognize(cropped, stop) : ocr.recognize(cropped, stop);
    // The geometry is the independently detected isolated label crop, not a
    // rotated Windows word rectangle. Keep disagreement for the strict parser.
    // The generic high-contrast provider extracts green map/chat text. HUD
    // labels are neutral white/gray, so that mask erases them and doubles OCR
    // work. Preserve the ordinary provider result for these isolated crops.
    if (!raw.text.empty()) frame.tokens.push_back({std::move(raw), rect, true,
                                                 SphHudTokenBoundsSource::recognition_region});
}

void read_compass_words(SphHudFrame& frame, const Image& image, WindowsOcr& ocr,
                        ImageRect rect, std::stop_token stop) {
    if (stop.stop_requested()) throw std::runtime_error("HUD recognition canceled");
    const auto cropped = instrument_crop(image, rect, true);
    const auto raw = ocr.recognize(cropped, stop);
    // A recognition-only model locates no glyphs. A crop centred on a ruler
    // stroke must never stand in for the position of the printed number: real
    // HUD frames show different phases between labels and long strokes.
    if (!raw.word_geometry_verified || raw.words.empty()) {
        if (!raw.text.empty()) frame.tokens.push_back({raw, rect, false,
                                                      SphHudTokenBoundsSource::recognition_region});
        return;
    }
    const auto scale = static_cast<double>(cropped.width) / (rect.right - rect.left);
    for (const auto& word : raw.words) {
        if (word.text.empty()) continue;
        const ImageRect bounds{
            rect.left + static_cast<int>(std::floor(word.bounds.left / scale)),
            rect.top + static_cast<int>(std::floor(word.bounds.top / scale)),
            rect.left + static_cast<int>(std::ceil(word.bounds.right / scale)),
            rect.top + static_cast<int>(std::ceil(word.bounds.bottom / scale))};
        if (bounds.left < rect.left || bounds.top < rect.top || bounds.right > rect.right ||
            bounds.bottom > rect.bottom || bounds.right <= bounds.left || bounds.bottom <= bounds.top)
            continue;
        OcrResult reading;
        reading.text = word.text;
        reading.line_count = 1;
        reading.word_geometry_verified = true;
        reading.text_angle_deg = raw.text_angle_deg;
        frame.tokens.push_back({std::move(reading), bounds, true, SphHudTokenBoundsSource::ocr_word});
    }
}

}  // namespace

SphHudFrame recognize_sph_hud_frame(const Image& image, WindowsOcr& ocr,
                                  std::uint64_t frame_id, std::stop_token stop, RapidOcr* recognition) {
    SphHudFrame frame;
    frame.client_width = image.width; frame.client_height = image.height;
    frame.captured_client_region = {0, 0, image.width, image.height};
    frame.frame_id = frame_id; frame.provider = recognition
        ? "RapidOcr/PP-OCRv6 + Windows.Media.Ocr/en-US isolated HUD labels"
        : "Windows.Media.Ocr/en-US isolated HUD labels";
    if (image.width < 640 || image.height < 360 || image.width > 7680 || image.height > 4320 ||
        static_cast<std::uint64_t>(image.width) * image.height > 16'000'000 ||
        image.bgr.size() != static_cast<std::uint64_t>(image.width) * image.height * 3)
        return frame;
    auto layout = extract_sph_hud_layout(image);
    if (!layout) layout = extract_sph_hud_supplemental_layout(image);
    if (!layout) return frame;
    frame.layout = *layout;
    for (const auto* scale : {&layout->mil_scale, &layout->range_scale}) {
        read_region(frame, image, ocr, scale->header, stop);
        for (const auto region : scale->label_regions) read_region(frame, image, ocr, region, stop, recognition);
    }
    if (layout->source == SphHudLayoutSource::optic_pixel_geometry)
        read_compass_words(frame, image, ocr, layout->compass.labels, stop);
    for (const auto region : {layout->asl, layout->stabilization, layout->weapon})
        if (region) read_region(frame, image, ocr, *region, stop);
    if (layout->ammunition) read_region(frame, image, ocr, *layout->ammunition, stop, nullptr, true);
    return frame;
}

}  // namespace wardogs
