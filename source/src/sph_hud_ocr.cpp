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
    const auto recognize_words = [&](ImageRect crop_rect) {
        if (stop.stop_requested()) throw std::runtime_error("HUD recognition canceled");
        const auto cropped = instrument_crop(image, crop_rect, true);
        const auto raw = ocr.recognize(cropped, stop);
        std::vector<SphHudToken> words;
        if (!raw.word_geometry_verified) return words;
        const auto scale = static_cast<double>(cropped.width) / (crop_rect.right - crop_rect.left);
        for (const auto& word : raw.words) {
            if (word.text.empty()) continue;
            const ImageRect bounds{
                crop_rect.left + static_cast<int>(std::floor(word.bounds.left / scale)),
                crop_rect.top + static_cast<int>(std::floor(word.bounds.top / scale)),
                crop_rect.left + static_cast<int>(std::ceil(word.bounds.right / scale)),
                crop_rect.top + static_cast<int>(std::ceil(word.bounds.bottom / scale))};
            if (bounds.left < rect.left || bounds.top < rect.top || bounds.right > rect.right ||
                bounds.bottom > rect.bottom || bounds.right <= bounds.left || bounds.bottom <= bounds.top)
                continue;
            OcrResult reading;
            reading.text = word.text;
            reading.line_count = 1;
            reading.word_geometry_verified = true;
            reading.text_angle_deg = raw.text_angle_deg;
            words.push_back({std::move(reading), bounds, true, SphHudTokenBoundsSource::ocr_word});
        }
        return words;
    };
    auto words = recognize_words(rect);
    if (!words.empty()) {
        for (auto& word : words) frame.tokens.push_back(std::move(word));
        return;
    }
    // A failed full-strip detector can retry independently located ink groups.
    // Neither a ruler tick nor the calculator command determines these crops.
    // Two padding variants must agree on both text and original-pixel geometry.
    for (const auto ink : extract_sph_hud_compass_word_regions(image, rect)) {
        const auto padded = [&](int padding) {
            return ImageRect{std::max(rect.left, ink.left - padding), std::max(rect.top, ink.top - padding),
                             std::min(rect.right, ink.right + padding), std::min(rect.bottom, ink.bottom + padding)};
        };
        const auto height = ink.bottom - ink.top;
        const auto first_padding = std::max(2, static_cast<int>(std::lround(height * 8.0 / 9.0)));
        const auto second_padding = std::max(first_padding + 1, static_cast<int>(std::lround(height * 10.0 / 9.0)));
        auto first = recognize_words(padded(first_padding));
        auto second = recognize_words(padded(second_padding));
        if (first.size() != 1 || second.size() != 1 || first[0].reading.text != second[0].reading.text) continue;
        const auto& a = first[0].bounds;
        const auto& b = second[0].bounds;
        if (std::abs(a.left - b.left) > 1 || std::abs(a.top - b.top) > 1 ||
            std::abs(a.right - b.right) > 1 || std::abs(a.bottom - b.bottom) > 1) continue;
        // Keep the second independent reading, without supplying a score.
        first[0].reading.alternate_text = second[0].reading.text;
        frame.tokens.push_back(std::move(first[0]));
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
