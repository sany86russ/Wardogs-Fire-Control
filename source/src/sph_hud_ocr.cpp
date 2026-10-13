#include "wardogs/sph_hud_ocr.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <limits>
#include <stdexcept>

namespace wardogs {
namespace {

std::string language_name(std::wstring_view tag) {
    std::string result;
    result.reserve(tag.size());
    for (const auto ch : tag) {
        if (ch < 0 || ch > 127) throw std::runtime_error("OCR language tag is not ASCII BCP-47");
        result.push_back(static_cast<char>(ch));
    }
    return result;
}

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

Image isolated_instrument_word(const Image& image, ImageRect ink, ImageRect projection,
                               std::uint8_t background = 255) {
    const int width = projection.right - projection.left, height = projection.bottom - projection.top;
    if (ink.left < 0 || ink.top < 0 || ink.right > image.width || ink.bottom > image.height ||
        ink.right <= ink.left || ink.bottom <= ink.top || width <= 0 || height <= 0)
        throw std::invalid_argument("Invalid isolated HUD glyph geometry");
    // Identical magnification in both padding views avoids introducing a new
    // resampling phase merely because the surrounding margin changed.
    constexpr int scale = 4;
    const auto count = static_cast<std::uint64_t>(width) * height * scale * scale;
    if (count > 1'000'000) throw std::invalid_argument("HUD glyph input is too large");
    Image result{width * scale, height * scale, std::vector<std::uint8_t>(count * 3, background)};
    // Keep the complete observed word rectangle unchanged. Synthetic white
    // padding outside it prevents a neighbouring window/branch from becoming
    // OCR geometry; no glyph pixel is filled, repaired or inferred.
    for (int y = ink.top; y < ink.bottom; ++y)
        for (int x = ink.left; x < ink.right; ++x) {
            const auto source = (static_cast<std::size_t>(y) * image.width + x) * 3;
            for (int dy = 0; dy < scale; ++dy)
                for (int dx = 0; dx < scale; ++dx) {
                    const auto target = (static_cast<std::size_t>((y - projection.top) * scale + dy) * result.width +
                                         (x - projection.left) * scale + dx) * 3;
                    std::copy_n(image.bgr.begin() + source, 3, result.bgr.begin() + target);
                }
        }
    return result;
}

void read_region(SphHudFrame& frame, const Image& image, WindowsOcr& ocr,
                 ImageRect rect, std::stop_token stop, RapidOcr* recognition,
                 bool numeric, SphHudInstrumentDiagnostics* diagnostics);

void read_numeric_region(SphHudFrame& frame, const Image& image, WindowsOcr& ocr,
                         ImageRect rect, std::stop_token stop, RapidOcr* recognition,
                         SphHudInstrumentDiagnostics& diagnostics,
                         std::chrono::steady_clock::time_point retry_deadline,
                         bool allow_retries = true) {
    if (rect.right <= rect.left || rect.bottom <= rect.top) return;
    if (!recognition) {
        read_region(frame, image, ocr, rect, stop, nullptr, true, &diagnostics);
        return;
    }
    const auto admit = [&](const Image& input) {
        if (stop.stop_requested()) throw std::runtime_error("HUD recognition canceled");
        ++diagnostics.attempted_regions;
        auto reading = recognition->recognize_hud_line(input, stop);
        if (!sph_hud_numeric_scores_qualified(reading)) {
            ++diagnostics.admission_rejections;
            return false;
        }
        // The source ROI was isolated by the side ladder. Weak surplus labels
        // remain unknown; every strong complete/conflicting read is retained
        // for the parser, without choosing values from neighbouring ticks.
        frame.tokens.push_back({std::move(reading), rect, true, SphHudTokenBoundsSource::recognition_region});
        ++diagnostics.tokens;
        return true;
    };
    if (std::chrono::steady_clock::now() >= retry_deadline) {
        diagnostics.budget_exhausted = true;
        return;
    }
    if (const auto ink = extract_sph_hud_numeric_word(image, rect)) {
        const auto height = ink->bottom - ink->top;
        const int first = std::max(2, static_cast<int>(std::lround(height * 0.4)));
        const int second = std::max(first + 1, static_cast<int>(std::lround(height * 8.0 / 15.0)));
        for (const auto padding : {first, second}) {
            if (std::chrono::steady_clock::now() >= retry_deadline) {
                diagnostics.budget_exhausted = true;
                return;
            }
            const ImageRect projection{ink->left - padding, ink->top - padding,
                                       ink->right + padding, ink->bottom + padding};
            if (admit(isolated_instrument_word(image, *ink, projection, 0)) || !allow_retries) return;
        }
    }
    if (std::chrono::steady_clock::now() >= retry_deadline) {
        diagnostics.budget_exhausted = true;
        return;
    }
    // A one-pixel crop origin can change the model's sampling phase even
    // though the entire label is present. A small blank border preserves
    // every original pixel and avoids clipping or repositioning a digit.
    const int padding = std::max(1, static_cast<int>(std::lround(image.height / 720.0)));
    const ImageRect projection{rect.left - padding, rect.top - padding,
                               rect.right + padding, rect.bottom + padding};
    if (admit(isolated_instrument_word(image, rect, projection, 0)) || !allow_retries) return;
    if (std::chrono::steady_clock::now() >= retry_deadline) {
        diagnostics.budget_exhausted = true;
        return;
    }
    (void)admit(instrument_crop(image, rect, true));
}

void read_region(SphHudFrame& frame, const Image& image, WindowsOcr& ocr,
                 ImageRect rect, std::stop_token stop, RapidOcr* recognition = nullptr,
                 bool numeric = false, SphHudInstrumentDiagnostics* diagnostics = nullptr) {
    if (stop.stop_requested()) throw std::runtime_error("HUD recognition canceled");
    if (rect.right <= rect.left || rect.bottom <= rect.top) return;
    if (diagnostics) ++diagnostics->attempted_regions;
    const auto cropped = instrument_crop(image, rect, recognition != nullptr || numeric);
    auto raw = recognition ? recognition->recognize_hud_line(cropped, stop) : ocr.recognize(cropped, stop);
    // The geometry is the independently detected isolated label crop, not a
    // rotated Windows word rectangle. Keep disagreement for the strict parser.
    // The generic high-contrast provider extracts green map/chat text. HUD
    // labels are neutral white/gray, so that mask erases them and doubles OCR
    // work. Preserve the ordinary provider result for these isolated crops.
    if (!raw.text.empty()) {
        frame.tokens.push_back({std::move(raw), rect, true, SphHudTokenBoundsSource::recognition_region});
        if (diagnostics) ++diagnostics->tokens;
    }
}

std::optional<bool> stabilization_label(std::wstring_view raw) {
    while (!raw.empty() && std::iswspace(raw.front())) raw.remove_prefix(1);
    while (!raw.empty() && std::iswspace(raw.back())) raw.remove_suffix(1);
    std::wstring text(raw);
    for (auto& ch : text) {
        if (ch >= L'A' && ch <= L'Z') ch += L'a' - L'A';
        else if (ch >= L'\u0410' && ch <= L'\u042f') ch += L'\u0430' - L'\u0410';
    }
    if (text == L"stabilized" || text == L"стабилизировано") return true;
    if (text == L"unstabilized" || text == L"не стабилизировано" || text == L"нестабилизировано") return false;
    return std::nullopt;
}

void read_stabilization(SphHudFrame& frame, const Image& image, WindowsOcr& ocr,
                        ImageRect rect, std::stop_token stop, WindowsOcr* localized) {
    auto& diagnostics = frame.diagnostics.stabilization;
    SphHudFrame candidates;
    read_region(candidates, image, ocr, rect, stop, nullptr, false, &diagnostics);
    if (!localized || localized->language_tag() == ocr.language_tag()) {
        for (auto& token : candidates.tokens) frame.tokens.push_back(std::move(token));
        return;
    }
    read_region(candidates, image, *localized, rect, stop, nullptr, false, &diagnostics);
    // Each provider observes the same bounded caption. An unreadable foreign
    // alphabet is unavailable evidence, not a contradictory status. Preserve
    // every contradictory exact state, so the parser will refuse it.
    std::optional<bool> kept;
    std::vector<SphHudToken> recognized;
    for (auto& token : candidates.tokens) {
        const auto state = stabilization_label(token.reading.text);
        if (!state) continue;
        if (!kept || *kept != *state) {
            kept = *state;
            recognized.push_back(std::move(token));
        }
    }
    if (recognized.empty()) recognized = std::move(candidates.tokens);
    diagnostics.tokens = static_cast<std::uint32_t>(recognized.size());
    for (auto& token : recognized) frame.tokens.push_back(std::move(token));
}

void read_header(SphHudFrame& frame, const Image& image, WindowsOcr& ocr,
                 ImageRect rect, std::wstring_view expected, std::stop_token stop,
                 RapidOcr* recognition, SphHudInstrumentDiagnostics& diagnostics,
                 std::chrono::steady_clock::time_point retry_deadline) {
    if (rect.right <= rect.left || rect.bottom <= rect.top) return;
    SphHudFrame candidates;
    read_region(candidates, image, ocr, rect, stop, nullptr, false, &diagnostics);
    const auto matches_literal = [&](const SphHudToken& token, std::wstring_view literal) {
        auto text = token.reading.text;
        while (!text.empty() && std::iswspace(text.front())) text.erase(text.begin());
        while (!text.empty() && std::iswspace(text.back())) text.pop_back();
        for (auto& ch : text) if (ch >= L'a' && ch <= L'z') ch -= L'a' - L'A';
        return text == literal;
    };
    if (candidates.tokens.size() == 1 && matches_literal(candidates.tokens.front(), expected)) {
        frame.tokens.push_back(std::move(candidates.tokens.front()));
        return;
    }
    if (std::any_of(candidates.tokens.begin(), candidates.tokens.end(), [&](const auto& token) {
        return matches_literal(token, L"MIL") || matches_literal(token, L"RNG");
    })) {
        for (auto& token : candidates.tokens) frame.tokens.push_back(std::move(token));
        return; // A different recognized instrument header cannot be replaced.
    }
    // The Latin header can suffer the same background/resampling problem as
    // numeric rows. A verified complete RGB word is tried before the broad
    // region; every qualified contradiction stays subject to the parser.
    if (recognition) {
        const auto admit = [&](const Image& input) {
            if (stop.stop_requested()) throw std::runtime_error("HUD recognition canceled");
            ++diagnostics.attempted_regions;
            // Short Latin instrument names retain the qualified legacy-width
            // path: reducing its context made a real MIL header unreadable.
            auto reading = recognition->recognize(input, stop);
            if (!sph_hud_numeric_scores_qualified(reading)) {
                ++diagnostics.admission_rejections;
                return false;
            }
            frame.tokens.push_back({std::move(reading), rect, true, SphHudTokenBoundsSource::recognition_region});
            ++diagnostics.tokens;
            return true;
        };
        if (const auto ink = extract_sph_hud_numeric_word(image, rect)) {
            const int first = std::max(2, static_cast<int>(std::lround(image.height / 240.0)));
            const int second = std::max(first + 1, static_cast<int>(std::lround(image.height / 180.0)));
            for (const auto padding : {first, second}) {
                if (std::chrono::steady_clock::now() >= retry_deadline) {
                    diagnostics.budget_exhausted = true;
                    break;
                }
                const ImageRect projection{ink->left - padding, ink->top - padding,
                                           ink->right + padding, ink->bottom + padding};
                if (admit(isolated_instrument_word(image, *ink, projection, 0))) return;
            }
        }
        if (std::chrono::steady_clock::now() < retry_deadline) {
            if (admit(instrument_crop(image, rect, true))) return;
        } else diagnostics.budget_exhausted = true;
    }
    for (auto& token : candidates.tokens) frame.tokens.push_back(std::move(token));
}

void read_compass_words(SphHudFrame& frame, const Image& image, WindowsOcr& ocr,
                        const SphHudScaleLayout& layout, Point pointer, std::stop_token stop,
                        SphHudInstrumentDiagnostics& diagnostics, RapidOcr* recognition,
                        std::chrono::steady_clock::time_point retry_deadline) {
    const auto rect = layout.labels;
    const auto observed_words = extract_sph_hud_compass_words(image, rect);
    const auto tick_extent = std::minmax_element(layout.ticks.begin(), layout.ticks.end(),
        [](const auto& a, const auto& b) { return a.position.x < b.position.x; });
    const auto anchor_margin = 3.0 * image.height / 720.0;
    const auto wholly_unanchored = [&](ImageRect bounds) {
        return bounds.right < tick_extent.first->position.x - anchor_margin ||
               bounds.left > tick_extent.second->position.x + anchor_margin;
    };
    const auto complete_digits = [](std::wstring_view text, const SphHudCompassWord& word) {
        return text.size() == word.glyph_count && text.size() >= 2 && text.size() <= 4 &&
            std::all_of(text.begin(), text.end(), [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; });
    };
    const auto recognize_words = [&](ImageRect crop_rect, std::optional<ImageRect> ink = std::nullopt) {
        if (stop.stop_requested()) throw std::runtime_error("HUD recognition canceled");
        const auto cropped = ink ? isolated_instrument_word(image, *ink, crop_rect)
                                 : instrument_crop(image, crop_rect, true);
        ++diagnostics.attempted_regions;
        const auto raw = ocr.recognize(cropped, stop);
        std::vector<SphHudToken> words;
        if (!raw.word_geometry_verified) { ++diagnostics.geometry_rejections; return words; }
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
                { ++diagnostics.geometry_rejections; continue; }
            if (ink && (std::abs(bounds.left - ink->left) > 1 || std::abs(bounds.right - ink->right) > 1 ||
                        std::abs(bounds.top - ink->top) > 1 || std::abs(bounds.bottom - ink->bottom) > 1)) {
                ++diagnostics.geometry_rejections;
                continue; // Agreement on a truncated word is not complete ink evidence.
            }
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
    std::erase_if(words, [&](const auto& token) {
        const auto matches = std::count_if(observed_words.begin(), observed_words.end(), [&](const auto& observed) {
            const auto& a = token.bounds;
            const auto& b = observed.bounds;
            return complete_digits(token.reading.text, observed) &&
                std::abs(a.left - b.left) <= 1 && std::abs(a.right - b.right) <= 1 &&
                std::abs(a.top - b.top) <= 1 && std::abs(a.bottom - b.bottom) <= 1;
        });
        if (matches != 1) ++diagnostics.geometry_rejections;
        return matches != 1;
    });
    for (auto it = words.begin(); it != words.end();) {
        if (!wholly_unanchored(it->bounds)) { ++it; continue; }
        // Its glyphs were observed, but its ruler stem was not. Retain the
        // actual text in diagnostic receipts without supplying a missing tick.
        frame.unanchored_compass_tokens.push_back(std::move(*it));
        it = words.erase(it);
        ++diagnostics.unanchored_tokens;
        ++diagnostics.geometry_rejections;
    }
    // A full-strip detector can truncate 270 into 27 or 300 into 30. Its word
    // rectangles alone do not prove complete compass labels. Prefer that pass
    // only when its independently anchored numeric grid actually brackets the
    // pointer; otherwise retry isolated observed ink, without repairing text.
    const auto brackets_pointer = [&](const std::vector<SphHudToken>& candidates) {
        std::vector<std::pair<std::size_t, unsigned>> anchors;
        bool invalid_numeric{};
        const auto maximum_offset = 3.0 * image.height / 720.0;
        for (const auto& word : candidates) {
            const auto& text = word.reading.text;
            if (text.empty() || !std::all_of(text.begin(), text.end(), [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; }))
                continue;
            if (text.size() > 3) { invalid_numeric = true; continue; }
            unsigned value{};
            for (const auto ch : text) value = value * 10U + static_cast<unsigned>(ch - L'0');
            if (value > 360 || value % 15 != 0) { invalid_numeric = true; continue; }
            const auto centre = (word.bounds.left + word.bounds.right) / 2.0;
            std::size_t chosen{};
            double closest = std::numeric_limits<double>::infinity();
            for (std::size_t i = 0; i < layout.ticks.size(); ++i) {
                const auto distance = std::abs(layout.ticks[i].position.x - centre);
                if (distance < closest) { closest = distance; chosen = i; }
            }
            if (closest > maximum_offset) { invalid_numeric = true; continue; }
            anchors.emplace_back(chosen, value);
        }
        std::sort(anchors.begin(), anchors.end());
        bool grid = !invalid_numeric && anchors.size() >= 2;
        for (std::size_t i = 1; grid && i < anchors.size(); ++i) {
            const auto steps = anchors[i].first - anchors[i - 1].first;
            const auto difference = (anchors[i].second + 360U - anchors[i - 1].second) % 360U;
            grid = steps > 0 && difference == 15U * steps;
        }
        return grid && layout.ticks[anchors.front().first].position.x <= pointer.x &&
            layout.ticks[anchors.back().first].position.x >= pointer.x;
    };
    if (brackets_pointer(words)) {
        diagnostics.tokens += static_cast<std::uint32_t>(words.size());
        for (auto& word : words) frame.tokens.push_back(std::move(word));
        return;
    }
    // A failed full-strip detector can retry independently located ink groups.
    // Neither a ruler tick nor the calculator command determines these crops.
    // Two padding variants must agree on both text and original-pixel geometry.
    // Retain every complete strip observation. A retry may add missing words,
    // but cannot silently replace a disagreeing complete reading at that ink.
    std::vector<SphHudToken> isolated = std::move(words);
    std::vector<SphHudCompassWord> unread;
    for (const auto& observed : observed_words) {
        const auto ink = observed.bounds;
        if (wholly_unanchored(ink)) {
            if (std::none_of(frame.unanchored_compass_tokens.begin(), frame.unanchored_compass_tokens.end(),
                [&](const auto& token) { return std::abs(token.bounds.left - ink.left) <= 1 &&
                    std::abs(token.bounds.right - ink.right) <= 1; })) {
                frame.unanchored_compass_tokens.push_back({{}, ink, false,
                    SphHudTokenBoundsSource::pixel_glyph_union, observed.glyph_count});
                ++diagnostics.unanchored_tokens;
                ++diagnostics.geometry_rejections;
            }
            continue;
        }
        if (std::any_of(isolated.begin(), isolated.end(), [&](const auto& token) {
            return std::abs(token.bounds.left - ink.left) <= 1 && std::abs(token.bounds.right - ink.right) <= 1 &&
                std::abs(token.bounds.top - ink.top) <= 1 && std::abs(token.bounds.bottom - ink.bottom) <= 1;
        })) continue;
        if (std::chrono::steady_clock::now() >= retry_deadline) {
            diagnostics.budget_exhausted = true;
            break;
        }
        const auto padded = [&](int padding) {
            return ImageRect{ink.left - padding, ink.top - padding, ink.right + padding, ink.bottom + padding};
        };
        const auto height = ink.bottom - ink.top;
        const auto first_padding = std::max(2, static_cast<int>(std::lround(height * 8.0 / 11.0)));
        const auto second_padding = std::max(first_padding + 1, static_cast<int>(std::lround(height * 12.0 / 11.0)));
        auto first = recognize_words(padded(first_padding), ink);
        if (std::chrono::steady_clock::now() >= retry_deadline) {
            diagnostics.budget_exhausted = true;
            break;
        }
        auto second = recognize_words(padded(second_padding), ink);
        if (first.size() != 1 || second.size() != 1 ||
            !complete_digits(first[0].reading.text, observed) ||
            !complete_digits(second[0].reading.text, observed)) {
            unread.push_back(observed);
            continue;
        }
        const auto& a = first[0].bounds;
        const auto& b = second[0].bounds;
        if (std::abs(a.left - b.left) > 1 || std::abs(a.top - b.top) > 1 ||
            std::abs(a.right - b.right) > 1 || std::abs(a.bottom - b.bottom) > 1) {
            unread.push_back(observed);
            continue;
        }
        // Keep the second independent reading, including complete conflicts,
        // without supplying a score or replacing them through another engine.
        first[0].reading.alternate_text = second[0].reading.text;
        isolated.push_back(std::move(first[0]));
    }
    // Missing Windows words may use the independently measured source ink.
    // It is a different evidence type: neither a recognition ROI nor a model
    // result supplies glyph positions or repairs an incomplete digit.
    std::stable_sort(unread.begin(), unread.end(), [&](const auto& a, const auto& b) {
        return std::abs((a.bounds.left + a.bounds.right) / 2.0 - pointer.x) <
               std::abs((b.bounds.left + b.bounds.right) / 2.0 - pointer.x);
    });
    for (const auto& observed : unread) {
        if (!recognition || brackets_pointer(isolated)) break;
        if (std::chrono::steady_clock::now() >= retry_deadline) {
            diagnostics.budget_exhausted = true;
            break;
        }
        const auto masked = [&](int padding) {
            const auto ink = observed.bounds;
            const int width = ink.right - ink.left + 2 * padding;
            const int height = ink.bottom - ink.top + 2 * padding;
            constexpr int scale = 4;
            const auto count = static_cast<std::uint64_t>(width) * height * scale * scale;
            if (count > 1'000'000) throw std::invalid_argument("HUD glyph mask is too large");
            Image result{width * scale, height * scale, std::vector<std::uint8_t>(count * 3, 255)};
            for (const auto pixel : observed.ink_pixels) {
                if (pixel.x < ink.left || pixel.x >= ink.right || pixel.y < ink.top || pixel.y >= ink.bottom)
                    throw std::invalid_argument("HUD source ink is outside observed word");
                for (int dy = 0; dy < scale; ++dy)
                    for (int dx = 0; dx < scale; ++dx) {
                        const auto index = (static_cast<std::size_t>((pixel.y - ink.top + padding) * scale + dy) *
                            result.width + (pixel.x - ink.left + padding) * scale + dx) * 3;
                        std::fill_n(result.bgr.begin() + index, 3, std::uint8_t{0});
                    }
            }
            return result;
        };
        const int height = observed.bounds.bottom - observed.bounds.top;
        const int first_padding = std::max(2, static_cast<int>(std::lround(height * 8.0 / 11.0)));
        const int second_padding = std::max(first_padding + 1, static_cast<int>(std::lround(height * 12.0 / 11.0)));
        const auto scored = [&](const OcrResult& reading) {
            return complete_digits(reading.text, observed) && complete_digits(reading.alternate_text, observed) &&
                sph_hud_numeric_scores_qualified(reading);
        };
        ++diagnostics.attempted_regions;
        auto first = recognition->recognize_hud_line(masked(first_padding), stop);
        if (!scored(first)) { ++diagnostics.admission_rejections; continue; }
        if (first.text != first.alternate_text) {
            // Two qualified complete values disagree; preserve that conflict
            // for the parser even if other labels would form a pleasant grid.
            isolated.push_back({std::move(first), observed.bounds, true,
                SphHudTokenBoundsSource::pixel_glyph_union, observed.glyph_count});
            continue;
        }
        if (std::chrono::steady_clock::now() >= retry_deadline) {
            diagnostics.budget_exhausted = true;
            break;
        }
        ++diagnostics.attempted_regions;
        const auto second = recognition->recognize_hud_line(masked(second_padding), stop);
        if (!scored(second)) { ++diagnostics.admission_rejections; continue; }
        first.confidence = std::min(first.confidence, second.confidence);
        first.minimum_confidence = std::min(first.minimum_confidence, second.minimum_confidence);
        // Retain either an internal or inter-view qualified conflict.
        first.alternate_text = first.text != second.text ? second.text : second.alternate_text;
        isolated.push_back({std::move(first), observed.bounds, true,
                            SphHudTokenBoundsSource::pixel_glyph_union, observed.glyph_count});
    }
    diagnostics.tokens += static_cast<std::uint32_t>(isolated.size());
    for (auto& token : isolated) frame.tokens.push_back(std::move(token));
}

}  // namespace

SphHudFrame recognize_sph_hud_frame(const Image& image, WindowsOcr& ocr,
                                  std::uint64_t frame_id, std::stop_token stop, RapidOcr* recognition,
                                  SphHudRecognitionDiagnostics* diagnostics, WindowsOcr* localized_metadata) {
    SphHudFrame frame;
    const auto retry_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
    struct Progress {
        SphHudRecognitionDiagnostics* output;
        const SphHudRecognitionDiagnostics& current;
        ~Progress() { if (output) *output = current; }
    } progress{diagnostics, frame.diagnostics};
    frame.client_width = image.width; frame.client_height = image.height;
    frame.captured_client_region = {0, 0, image.width, image.height};
    frame.frame_id = frame_id;
    const auto language = ocr.language_tag();
    frame.provider = (recognition ? "RapidOcr/PP-OCRv6 + " : std::string{}) +
        "Windows.Media.Ocr/" + language_name(language) + " isolated HUD labels";
    if (localized_metadata) {
        const auto localized = localized_metadata->language_tag();
        frame.provider += " + metadata/" + language_name(localized);
    }
    if (image.width < 640 || image.height < 360 || image.width > 7680 || image.height > 4320 ||
        static_cast<std::uint64_t>(image.width) * image.height > 16'000'000 ||
        image.bgr.size() != static_cast<std::uint64_t>(image.width) * image.height * 3)
        { frame.diagnostics.stage = SphHudRecognitionStage::invalid_image; return frame; }
    auto layout = extract_sph_hud_layout(image, &frame.diagnostics.geometry);
    if (!layout) layout = extract_sph_hud_supplemental_layout(image);
    if (!layout) { frame.diagnostics.stage = SphHudRecognitionStage::layout_not_found; return frame; }
    frame.layout = *layout;
    frame.diagnostics.layout_source = layout->source;
    frame.diagnostics.stage = layout->source == SphHudLayoutSource::optic_pixel_geometry
        ? SphHudRecognitionStage::optic_layout : SphHudRecognitionStage::supplemental_layout;
    frame.diagnostics.stage = SphHudRecognitionStage::instrument_ocr;
    const auto read_label = [&](const SphHudScaleLayout* scale, ImageRect region, bool allow_retries = true) {
        auto& instrument = scale == &layout->mil_scale ? frame.diagnostics.mil : frame.diagnostics.range;
        read_numeric_region(frame, image, ocr, region, stop, recognition, instrument, retry_deadline, allow_retries);
    };
    for (const auto* scale : {&layout->mil_scale, &layout->range_scale}) {
        auto& instrument = scale == &layout->mil_scale ? frame.diagnostics.mil : frame.diagnostics.range;
        read_header(frame, image, ocr, scale->header,
                    scale == &layout->mil_scale ? L"MIL" : L"RNG", stop, recognition, instrument, retry_deadline);
    }
    std::vector<std::pair<const SphHudScaleLayout*, ImageRect>> surplus;
    const auto row_order = [](const SphHudScaleLayout& scale) {
        std::vector<std::size_t> order;
        for (std::size_t i = 0; i < scale.ticks.size(); ++i) order.push_back(i);
        std::sort(order.begin(), order.end(), [&](auto a, auto b) {
            return scale.ticks[a].position.y < scale.ticks[b].position.y;
        });
        return order;
    };
    const auto bracketing_rows = [&](const SphHudScaleLayout& scale,
                                     const std::vector<std::size_t>& order) {
        const auto upper = std::lower_bound(order.begin(), order.end(), layout->reticle.y,
            [&](std::size_t index, double y) { return scale.ticks[index].position.y < y; });
        const auto high = std::clamp(static_cast<std::size_t>(upper - order.begin()), std::size_t{1}, order.size() - 1);
        return std::pair{high - 1, high};
    };
    const auto mil_order = row_order(layout->mil_scale), range_order = row_order(layout->range_scale);
    const double row_tolerance = layout->tick_uncertainty_px * 2.0 + 0.5;
    bool paired = mil_order.size() >= 2 && mil_order.size() == range_order.size() &&
        layout->mil_scale.label_regions.size() == mil_order.size() &&
        layout->range_scale.label_regions.size() == range_order.size();
    for (std::size_t i = 0; paired && i < mil_order.size(); ++i)
        paired = std::abs(layout->mil_scale.ticks[mil_order[i]].position.y -
                          layout->range_scale.ticks[range_order[i]].position.y) <= row_tolerance;
    if (paired) {
        const auto mil_bracket = bracketing_rows(layout->mil_scale, mil_order);
        const auto range_bracket = bracketing_rows(layout->range_scale, range_order);
        std::vector<std::size_t> required{mil_bracket.first, mil_bracket.second,
                                        range_bracket.first, range_bracket.second};
        std::sort(required.begin(), required.end());
        required.erase(std::unique(required.begin(), required.end()), required.end());
        // Near an actual row, tolerated left/right localisation jitter can
        // select opposite adjacent brackets. Read their union on BOTH scales;
        // the parser still decides bracketing and uncertainty from the pixels.
        for (const auto rank : required) {
            read_label(&layout->mil_scale, layout->mil_scale.label_regions[mil_order[rank]]);
            read_label(&layout->range_scale, layout->range_scale.label_regions[range_order[rank]]);
        }
        for (std::size_t rank = 0; rank < mil_order.size(); ++rank) {
            if (std::binary_search(required.begin(), required.end(), rank)) continue;
            surplus.emplace_back(&layout->mil_scale, layout->mil_scale.label_regions[mil_order[rank]]);
            surplus.emplace_back(&layout->range_scale, layout->range_scale.label_regions[range_order[rank]]);
        }
    } else for (const auto* scale : {&layout->mil_scale, &layout->range_scale}) {
        // Scheduling depends only on independently measured rows. Give BOTH
        // ladders their actual bracketing anchors before spending the bounded
        // budget on a weak surplus label; no numeric reading chooses a crop.
        const auto order = row_order(*scale);
        if (order.size() < 2 || scale->label_regions.size() != scale->ticks.size()) {
            for (const auto region : scale->label_regions) read_label(scale, region);
            continue;
        }
        const auto bracket = bracketing_rows(*scale, order);
        const auto low_index = order[bracket.first], high_index = order[bracket.second];
        read_label(scale, scale->label_regions[low_index]);
        read_label(scale, scale->label_regions[high_index]);
        for (const auto index : order)
            if (index != low_index && index != high_index) surplus.emplace_back(scale, scale->label_regions[index]);
    }
    if (layout->source == SphHudLayoutSource::optic_pixel_geometry)
        read_compass_words(frame, image, ocr, layout->compass, layout->compass_pointer, stop,
                           frame.diagnostics.compass, recognition, retry_deadline);
    for (auto pair : {std::pair{layout->asl, &frame.diagnostics.asl},
                      std::pair{layout->weapon, &frame.diagnostics.weapon}})
        if (pair.first) read_region(frame, image, ocr, *pair.first, stop, nullptr, false, pair.second);
    if (layout->stabilization)
        read_stabilization(frame, image, ocr, *layout->stabilization, stop, localized_metadata);
    if (layout->ammunition)
        read_region(frame, image, ocr, *layout->ammunition, stop, nullptr, true, &frame.diagnostics.ammunition);
    for (const auto& [scale, region] : surplus) read_label(scale, region, false);
    frame.diagnostics.stage = SphHudRecognitionStage::completed;
    return frame;
}

}  // namespace wardogs
