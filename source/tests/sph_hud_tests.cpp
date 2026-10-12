#include "wardogs/sph_hud.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>

namespace {

using namespace wardogs;
unsigned failures{};

void check(bool value, const char* name) {
    if (!value) { ++failures; std::cerr << "FAIL: " << name << '\n'; }
}

bool close(double first, double second, double tolerance = 0.001) {
    return std::abs(first - second) <= tolerance;
}

void token(SphHudFrame& frame, std::wstring text, ImageRect bounds, float score = 0.0F) {
    OcrResult reading;
    reading.text = std::move(text);
    reading.confidence = score;
    reading.minimum_confidence = score;
    reading.line_count = 1;
    frame.tokens.push_back({std::move(reading), bounds, true, SphHudTokenBoundsSource::ocr_word});
}

SphHudFrame fixture() {
    SphHudFrame frame;
    frame.client_width = 1280; frame.client_height = 720;
    frame.captured_client_region = {0, 0, 1280, 720};
    frame.frame_id = 41; frame.provider = "fixture";
    auto& layout = frame.layout;
    layout.source = SphHudLayoutSource::optic_pixel_geometry;
    layout.reticle = {639.5, 359.5};
    layout.reticle_uncertainty_px = layout.tick_uncertainty_px = 0.75;
    layout.compass_pointer = {639.5, 213.0};
    layout.compass_pointer_uncertainty_px = 0.75;
    layout.mil_scale.labels = {826, 277, 909, 479};
    layout.range_scale.labels = {381, 277, 459, 479};
    for (const auto y : {289.5, 377.5, 465.5}) {
        layout.mil_scale.ticks.push_back({{820.0, y}});
        layout.range_scale.ticks.push_back({{463.0, y}});
    }
    token(frame, L"MIL", {869, 354, 890, 365});
    token(frame, L"RNG", {388, 354, 410, 365});
    token(frame, L"990 mils", {828, 285, 874, 295});
    token(frame, L"1,000 mil", {828, 373, 880, 383});
    token(frame, L"1,010 mils", {828, 461, 884, 471});
    token(frame, L"2,170m", {421, 285, 457, 295});
    token(frame, L"2,147m", {421, 373, 457, 383});
    token(frame, L"2,122m", {421, 461, 457, 471});
    layout.compass.labels = {483, 177, 808, 199};
    unsigned index{};
    for (const auto x : {507.0, 563.0, 618.0, 673.0, 729.0, 785.0}) {
        layout.compass.ticks.push_back({{x, 213.0}});
        token(frame, std::to_wstring(210 + index * 15),
            {static_cast<int>(x) - 9, 188, static_cast<int>(x) + 9, 196});
        ++index;
    }
    layout.asl = ImageRect{510, 613, 585, 645};
    token(frame, L"ASL", {516, 625, 537, 636});
    token(frame, L"82", {552, 625, 565, 636});
    layout.stabilization = ImageRect{610, 570, 674, 590};
    token(frame, L"Stabilized", {617, 573, 665, 585});
    token(frame, L"TRANSITION", {694, 412, 790, 429});
    token(frame, L"Unstabilized", {824, 412, 921, 429});
    layout.ammunition = ImageRect{1218, 668, 1251, 688};
    layout.ammunition_kind = SphHudAmmoKind::single_shell;
    token(frame, L"013", {1227, 673, 1243, 682});
    token(frame, L"317", {1170, 653, 1205, 675});
    layout.weapon = ImageRect{1180, 596, 1265, 624};
    token(frame, L"SPH-2", {1228, 603, 1259, 613});
    return frame;
}

SphHudToken& named(SphHudFrame& frame, std::wstring_view text) {
    return *std::find_if(frame.tokens.begin(), frame.tokens.end(), [&](const auto& value) {
        return value.reading.text == text;
    });
}

void remove(SphHudFrame& frame, std::wstring_view text) {
    std::erase_if(frame.tokens, [&](const auto& value) { return value.reading.text == text; });
}

ImageRect scaled(ImageRect value, double factor) {
    return {static_cast<int>(std::floor(value.left * factor)), static_cast<int>(std::floor(value.top * factor)),
            static_cast<int>(std::ceil(value.right * factor)), static_cast<int>(std::ceil(value.bottom * factor))};
}

SphHudFrame scaled(SphHudFrame frame, double factor) {
    frame.client_width = static_cast<int>(frame.client_width * factor);
    frame.client_height = static_cast<int>(frame.client_height * factor);
    frame.captured_client_region = scaled(frame.captured_client_region, factor);
    for (auto& value : frame.tokens) value.bounds = scaled(value.bounds, factor);
    auto& layout = frame.layout;
    layout.reticle.x *= factor; layout.reticle.y *= factor;
    layout.compass_pointer.x *= factor; layout.compass_pointer.y *= factor;
    layout.reticle_uncertainty_px *= factor; layout.tick_uncertainty_px *= factor;
    layout.compass_pointer_uncertainty_px *= factor;
    for (auto* value : {&layout.mil_scale, &layout.range_scale, &layout.compass}) {
        value->tick_uncertainty_px *= factor;
        value->labels = scaled(value->labels, factor);
        for (auto& tick : value->ticks) { tick.position.x *= factor; tick.position.y *= factor; }
    }
    for (auto* roi : {&layout.asl, &layout.stabilization, &layout.ammunition, &layout.weapon})
        if (*roi) **roi = scaled(**roi, factor);
    return frame;
}

void rect(Image& image, int left, int top, int right, int bottom, std::uint8_t color) {
    for (int y = std::max(0, top); y < std::min(image.height, bottom); ++y)
        for (int x = std::max(0, left); x < std::min(image.width, right); ++x)
            for (unsigned channel = 0; channel < 3; ++channel)
                image.bgr[(static_cast<std::size_t>(y) * image.width + x) * 3 + channel] = color;
}

Image optic(bool sight = true, bool scales = true) {
    Image image{1280, 720, std::vector<std::uint8_t>(1280 * 720 * 3, 20)};
    if (sight) {
        rect(image, 627, 319, 653, 321, 130);
        rect(image, 639, 291, 641, 320, 130);
        rect(image, 627, 399, 653, 401, 130);
        rect(image, 639, 400, 641, 429, 130);
    }
    if (scales) {
        for (const int y : {289, 377, 465}) {
            rect(image, 820, y - 7, 822, y + 8, 250);
            rect(image, 808, y, 822, y + 2, 250);
            rect(image, 463, y - 7, 465, y + 8, 250);
            rect(image, 463, y, 477, y + 2, 250);
        }
        rect(image, 474, 213, 806, 215, 250);
        for (const int x : {507, 563, 618, 673, 729, 785})
            rect(image, x, 199, x + 2, 214, 250);
        rect(image, 1234, 645, 1235, 647, 250);
        rect(image, 1233, 647, 1235, 649, 250);
        rect(image, 1233, 649, 1236, 650, 250);
        rect(image, 1232, 650, 1236, 652, 250);
        rect(image, 1232, 652, 1237, 656, 250);
        rect(image, 1232, 657, 1237, 661, 250);
    }
    return image;
}

void compass_character(Image& image, int left, int top, int factor = 1, std::uint8_t color = 60) {
    // Separate connected ink shapes test localization, not OCR text or a
    // fabricated accepted HUD frame. No numeric value is assigned to them.
    constexpr const char* rows[]{"01110", "10001", "10001", "10001", "10001", "10001", "01110"};
    for (int y = 0; y < 7; ++y)
        for (int x = 0; x < 5; ++x)
            if (rows[y][x] == '1')
                rect(image, left + x * factor, top + y * factor,
                     left + (x + 1) * factor, top + (y + 1) * factor, color);
}

bool same_rect(ImageRect first, ImageRect second) {
    return first.left == second.left && first.top == second.top &&
           first.right == second.right && first.bottom == second.bottom;
}

bool overlaps(ImageRect first, ImageRect second) {
    return first.left < second.right && second.left < first.right &&
           first.top < second.bottom && second.top < first.bottom;
}

bool encloses(ImageRect outer, ImageRect inner) {
    return inner.left >= outer.left && inner.top >= outer.top &&
           inner.right <= outer.right && inner.bottom <= outer.bottom;
}

void rounded_instrument_roi_containment() {
    auto image = optic();
    // Measured row spines can jitter by a source pixel. The aggregate parser
    // ROI must cover each floor/ceil-rounded recognition crop independently.
    rect(image, 807, 370, 824, 385, 20);
    rect(image, 819, 370, 821, 385, 250);
    rect(image, 807, 377, 821, 379, 250);
    rect(image, 462, 370, 479, 385, 20);
    rect(image, 464, 370, 466, 385, 250);
    rect(image, 464, 377, 478, 379, 250);
    const auto layout = extract_sph_hud_layout(image);
    check(layout.has_value(), "slight mirrored side-tick row jitter preserves the measured optic");
    if (!layout) return;
    for (const auto* scale : {&layout->mil_scale, &layout->range_scale}) {
        check(encloses(scale->labels, scale->header), "aggregate instrument bounds contain the measured header crop");
        for (const auto crop : scale->label_regions)
            check(encloses(scale->labels, crop),
                  "aggregate instrument bounds contain every independently rounded numeric crop");
    }
    auto frame = fixture();
    frame.layout = *layout;
    named(frame, L"MIL").bounds = layout->mil_scale.header;
    named(frame, L"RNG").bounds = layout->range_scale.header;
    constexpr std::wstring_view mil_labels[]{L"990 mils", L"1,000 mil", L"1,010 mils"};
    constexpr std::wstring_view range_labels[]{L"2,170m", L"2,147m", L"2,122m"};
    for (std::size_t index = 0; index < 3; ++index) {
        auto& mil = named(frame, mil_labels[index]);
        auto& range = named(frame, range_labels[index]);
        mil.bounds = layout->mil_scale.label_regions[index];
        range.bounds = layout->range_scale.label_regions[index];
        mil.bounds_source = range.bounds_source = SphHudTokenBoundsSource::recognition_region;
    }
    check(read_sph_hud(frame).accepted(),
          "verified numeric tokens in jittered actual crops are retained through aggregate containment");
}

Image pixel_scaled(const Image& original, int factor) {
    Image image{original.width * factor, original.height * factor,
                std::vector<std::uint8_t>(static_cast<std::size_t>(original.width) * original.height * factor * factor * 3)};
    for (int y = 0; y < image.height; ++y)
        for (int x = 0; x < image.width; ++x) {
            const auto source = (static_cast<std::size_t>(y / factor) * original.width + x / factor) * 3;
            const auto destination = (static_cast<std::size_t>(y) * image.width + x) * 3;
            std::copy_n(original.bgr.begin() + source, 3, image.bgr.begin() + destination);
        }
    return image;
}

void reticle_localization() {
    auto image = pixel_scaled(optic(), 2);
    // A seven-pixel rasterized stem leaves one connected interval of equally
    // credible locations. Its original-pixel uncertainty must be retained.
    rect(image, 1264, 570, 1296, 638, 20);
    rect(image, 1264, 802, 1296, 874, 20);
    rect(image, 1276, 582, 1283, 638, 130);
    rect(image, 1276, 802, 1283, 858, 130);
    const auto thick = extract_sph_hud_layout(image);
    check(thick && thick->reticle_uncertainty_px >= 0.75 && thick->reticle_uncertainty_px <= 4.0 &&
          std::abs(thick->reticle.x - 1279.0) <= thick->reticle_uncertainty_px &&
          std::abs(thick->reticle.y - 719.5) <= thick->reticle_uncertainty_px,
          "a thick 1440p reticle interval contains its actually rendered stem/bar centre");
    if (thick) {
        for (const auto* scale : {&thick->mil_scale, &thick->range_scale})
            for (const auto crop : scale->label_regions)
                check(!overlaps(crop, scale->header),
                      "measured 1440p numeric crops remain isolated from scale headers");
    }

    auto competing = image;
    rect(competing, 1264, 570, 1296, 638, 20);
    rect(competing, 1264, 802, 1296, 874, 20);
    for (const int left : {1268, 1286}) {
        rect(competing, left, 582, left + 5, 638, 130);
        rect(competing, left, 802, left + 5, 858, 130);
    }
    check(!extract_sph_hud_layout(competing),
          "two disconnected credible reticle stem locations cannot be averaged into a sight centre");
    auto broad = image;
    rect(broad, 1254, 638, 1306, 642, 50);
    rect(broad, 1254, 798, 1306, 802, 50);
    rect(broad, 1264, 570, 1296, 638, 20);
    rect(broad, 1264, 802, 1296, 874, 20);
    rect(broad, 1274, 582, 1285, 638, 130);
    rect(broad, 1274, 802, 1285, 858, 130);
    check(!extract_sph_hud_layout(broad),
          "a reticle localization interval wider than the uncertainty ceiling stays refused");
    check(!extract_sph_hud_layout(pixel_scaled(optic(false), 2)),
          "1440p reload geometry cannot become a reticle through localization tolerance");
}

void compass_geometry_uncertainty() {
    auto frame = fixture();
    check(read_sph_hud(frame).accepted(), "zero per-scale uncertainty retains the legacy frame contract");
    frame.layout.compass.tick_uncertainty_px = 1.5;
    check(read_sph_hud(frame).refusal == SphHudRefusal::invalid_layout,
          "a global tick interval narrower than measured compass geometry is refused");
    frame.layout.tick_uncertainty_px = 1.5;
    check(read_sph_hud(frame).accepted(),
          "a measured compass interval is retained when the global uncertainty covers it");
    for (const double invalid : {-0.1, 4.1, std::numeric_limits<double>::quiet_NaN(),
                                 std::numeric_limits<double>::infinity()}) {
        frame.layout.compass.tick_uncertainty_px = invalid;
        check(read_sph_hud(frame).refusal == SphHudRefusal::invalid_layout,
              "negative, excessive or nonfinite measured tick uncertainty never reaches interpolation");
    }

    auto sky = optic();
    for (const int x : {563, 618, 673, 729}) rect(sky, x, 199, x + 2, 213, 60);
    const auto faint = extract_sph_hud_layout(sky);
    check(faint && faint->compass.ticks.size() == 6 && faint->compass.tick_uncertainty_px > 0.0 &&
          faint->compass.tick_uncertainty_px <= 4.0 &&
          faint->tick_uncertainty_px >= faint->compass.tick_uncertainty_px,
          "faint measured major ticks require independent strong anchors and retain their pixel interval");
    if (faint) {
        check(close(faint->compass.ticks.front().position.x, 507.0, 1.0) &&
              close(faint->compass.ticks.back().position.x, 785.0, 1.0),
              "a rescued grid uses the actual faint and strong stem locations without filling new ticks");
    }
    auto uncorroborated = sky;
    rect(uncorroborated, 507, 199, 509, 213, 60);
    check(!extract_sph_hud_layout(uncorroborated),
          "one strong major anchor cannot corroborate a faint compass grid");
    rect(uncorroborated, 785, 199, 787, 213, 60);
    check(!extract_sph_hud_layout(uncorroborated),
          "an entirely faint scene cannot become a compass through a low contrast threshold");
    auto incomplete = optic();
    rect(incomplete, 618, 199, 620, 213, 60);
    for (const int x : {673, 729, 785}) rect(incomplete, x, 199, x + 2, 213, 20);
    check(!extract_sph_hud_layout(incomplete),
          "three faint/strong stems cannot invent the required longer observed grid");
    auto competing = sky;
    for (const int x : {507, 563, 618, 673, 729, 785}) {
        rect(competing, x, 199, x + 2, 213, 250);
        rect(competing, x + 10, 199, x + 12, 213, 250);
    }
    check(!extract_sph_hud_layout(competing),
          "ambiguous strong major chains stay refused before any faint-grid rescue");

    auto weak_line = optic();
    rect(weak_line, 474, 213, 806, 215, 60);
    for (const int x : {563, 618, 673, 729, 785})
        rect(weak_line, x, 199, x + 2, 213, 60);
    SphHudLayoutDiagnostics weak_diagnostics;
    const auto weak = extract_sph_hud_layout(weak_line, &weak_diagnostics);
    check(weak && weak->compass.ticks.size() == 6 && weak_diagnostics.compass_weak_baseline &&
          weak->compass.tick_uncertainty_px >= 0.75 && weak->compass.tick_uncertainty_px <= 4.0,
          "a weak baseline is supported by six independently measured stems and a strong anchor");
    auto bare_line = weak_line;
    for (const int x : {507, 563, 618, 673, 729, 785})
        rect(bare_line, x, 199, x + 2, 213, 20);
    check(!extract_sph_hud_layout(bare_line),
          "a weak horizontal line alone cannot invent measured compass stems");
    auto four_stems = weak_line;
    for (const int x : {729, 785}) rect(four_stems, x, 199, x + 2, 213, 20);
    check(!extract_sph_hud_layout(four_stems),
          "four measured stems do not corroborate a weak baseline");
    auto no_strong_stem = weak_line;
    rect(no_strong_stem, 507, 199, 509, 213, 60);
    check(!extract_sph_hud_layout(no_strong_stem),
          "a weak baseline and weak stems cannot supply their own independent strong anchor");
    auto truncated_stem = weak_line;
    rect(truncated_stem, 785, 199, 787, 213, 20);
    rect(truncated_stem, 729, 204, 731, 213, 20);
    check(!extract_sph_hud_layout(truncated_stem),
          "a truncated fifth stem cannot complete the required weak-baseline proof");
    auto inconsistent_stem = pixel_scaled(weak_line, 2);
    rect(inconsistent_stem, 1452, 398, 1468, 426, 20);
    rect(inconsistent_stem, 1456, 398, 1463, 411, 58);
    rect(inconsistent_stem, 1455, 411, 1464, 426, 58);
    check(!extract_sph_hud_layout(inconsistent_stem),
          "a weak grid whose lower stem has an overwide measured ink interval remains refused");
}

void instrument_isolation() {
    auto frame = fixture();
    const auto compass_roi = frame.layout.compass.labels;
    std::erase_if(frame.tokens, [&](const auto& value) { return overlaps(value.bounds, compass_roi); });
    token(frame, L"240", {600, 10, 627, 25});
    token(frame, L"255", {650, 10, 677, 25});
    token(frame, L"245SW", {621, 38, 659, 55});
    check(read_sph_hud(frame).refusal == SphHudRefusal::missing_compass_labels,
          "navigation compass and boxed heading cannot supply absent inner compass labels");

    frame = fixture();
    named(frame, L"1,000 mil").reading.text = L"190 mils MIL";
    check(!read_sph_hud(frame).accepted(),
          "a MIL header merged with a numeric crop is refused rather than deleted from OCR text");
    frame = fixture();
    named(frame, L"2,147m").reading.text = L"G 1,942m";
    remove(frame, L"2,170m"); remove(frame, L"2,122m");
    check(read_sph_hud(frame).refusal == SphHudRefusal::missing_range_labels,
          "a clipped RNG header cannot become a repaired range label");

    frame = fixture();
    remove(frame, L"SPH-2");
    token(frame, L"SPH-2", {600, 300, 670, 320});
    check(read_sph_hud(frame).refusal == SphHudRefusal::wrong_weapon,
          "the model name elsewhere in a scene cannot verify the weapon panel");
    frame = fixture();
    remove(frame, L"Stabilized");
    token(frame, L"Stabilized", {680, 300, 750, 320});
    const auto unrelated_state = read_sph_hud(frame);
    check(unrelated_state.accepted() && !unrelated_state.stabilized,
          "an unrelated stabilization/action caption cannot supply bottom HUD state");
    frame = fixture();
    named(frame, L"Stabilized").reading.text = L"СТАБИЛИЗИРОВАНО";
    const auto russian_state = read_sph_hud(frame);
    check(russian_state.accepted() && russian_state.stabilized && *russian_state.stabilized,
          "the observed Russian stabilization caption is recognized independently of the C locale");
    token(frame, L"Unstabilized", {612, 571, 666, 585});
    check(!read_sph_hud(frame).stabilized,
          "conflicting Russian and English state captions cannot produce verified stabilization");
    frame = fixture();
    named(frame, L"Stabilized").reading.text = L"Стабилизирован";
    check(!read_sph_hud(frame).stabilized,
          "a similar but unverified Russian word cannot repair the exact observed state caption");
    frame = fixture();
    named(frame, L"Stabilized").reading.text = L"Не стабилизировано";
    const auto russian_false = read_sph_hud(frame);
    check(russian_false.stabilized && !*russian_false.stabilized,
          "the observed spaced Russian negative caption preserves explicit unstabilized state");
    named(frame, L"Не стабилизировано").reading.text = L"НЕ СТАБИЛИЗИРОВАНО";
    const auto uppercase_false = read_sph_hud(frame);
    check(uppercase_false.stabilized && !*uppercase_false.stabilized,
          "the observed Russian negative caption is recognized with independent uppercase folding");
    token(frame, L"Stabilized", {612, 571, 666, 585});
    check(!read_sph_hud(frame).stabilized,
          "contradictory verified negative Russian and positive English states remain unknown");
    frame = fixture();
    named(frame, L"Stabilized").reading.text = L"Нестабилизировано";
    const auto joined_false = read_sph_hud(frame);
    check(joined_false.stabilized && !*joined_false.stabilized,
          "the independently observed joined Russian negative caption preserves its negative state");
    token(frame, L"Stabilized", {612, 571, 666, 585});
    check(!read_sph_hud(frame).stabilized,
          "the observed joined negative caption cannot override a conflicting positive caption");
    frame = fixture();
    named(frame, L"Stabilized").reading.text = L"Нестабилизирован";
    check(!read_sph_hud(frame).stabilized,
          "an unsupported similar negative spelling is not supplied by a guess");
    frame = fixture();
    remove(frame, L"013");
    token(frame, L"013", {1170, 653, 1205, 675});
    const auto unrelated_ammo = read_sph_hud(frame);
    check(unrelated_ammo.accepted() && !unrelated_ammo.ammunition,
          "an MG-area number cannot supply a missing single-shell counter");

    const auto layout = extract_sph_hud_layout(optic());
    check(layout.has_value(), "the scale isolation fixture must expose a complete optic layout");
    if (layout) {
        for (const auto* scale : {&layout->mil_scale, &layout->range_scale}) {
            check(!scale->label_regions.empty(), "a measured scale must expose isolated numeric crops");
            for (const auto region : scale->label_regions)
                check(!overlaps(region, scale->header),
                      "each numeric and unit crop excludes its neighbouring MIL/RNG header pixels");
        }
    }
}

void diagnostic_semantics() {
    check(sph_hud_recognition_stage_name(SphHudRecognitionStage::invalid_image) == "invalid_image" &&
          sph_hud_recognition_stage_name(SphHudRecognitionStage::layout_not_found) == "layout_not_found" &&
          sph_hud_recognition_stage_name(SphHudRecognitionStage::completed) == "completed",
          "frame validation, missing geometry and completed OCR have distinct diagnostic names");
    check(sph_hud_refusal_name(SphHudRefusal::invalid_frame) == "invalid_frame" &&
          sph_hud_refusal_name(SphHudRefusal::missing_compass_labels) == "missing_compass_labels" &&
          sph_hud_refusal_name(SphHudRefusal::low_read_confidence) == "low_read_confidence",
          "stable textual refusal names preserve distinct geometry and OCR failures");
    check(sph_hud_layout_source_name(SphHudLayoutSource::unverified) == "unverified" &&
          sph_hud_layout_source_name(SphHudLayoutSource::supplemental_icon_geometry) == "supplemental_icon_geometry",
          "supplemental shell evidence is labelled separately from an unverified optic");
    check(sph_hud_recognition_stage_name(static_cast<SphHudRecognitionStage>(99)) == "unknown" &&
          sph_hud_refusal_name(static_cast<SphHudRefusal>(99)) == "unknown" &&
          sph_hud_layout_source_name(static_cast<SphHudLayoutSource>(99)) == "unknown",
          "unknown diagnostic enums do not masquerade as a successful pipeline stage");

    auto frame = fixture();
    frame.diagnostics.stage = SphHudRecognitionStage::completed;
    frame.diagnostics.layout_source = SphHudLayoutSource::optic_pixel_geometry;
    frame.diagnostics.compass.tokens = 100;
    std::erase_if(frame.tokens, [&](const auto& value) { return overlaps(value.bounds, frame.layout.compass.labels); });
    check(!read_sph_hud(frame).accepted(),
          "completed diagnostics and high token counters cannot replace actual compass evidence");
}

void pixel_glyph_provenance() {
    auto frame = fixture();
    auto& observed = named(frame, L"270");
    observed.bounds_source = SphHudTokenBoundsSource::pixel_glyph_union;
    observed.observed_glyph_count = 3;
    observed.reading.alternate_text = L"270";
    observed.reading.confidence = 0.999F;
    observed.reading.minimum_confidence = 0.998F;
    check(read_sph_hud(frame).accepted(),
          "matching scored OCR digits may use an independently measured complete glyph union");
    auto invalid = frame;
    named(invalid, L"270").observed_glyph_count = 2;
    check(read_sph_hud(invalid).refusal == SphHudRefusal::invalid_frame,
          "digit count cannot exceed the observed independent glyph components");
    invalid = frame;
    auto& truncated = named(invalid, L"270");
    truncated.reading.text = truncated.reading.alternate_text = L"30";
    check(read_sph_hud(invalid).refusal == SphHudRefusal::invalid_frame,
          "a truncated valid-grid integer cannot hide a third observed glyph");
    invalid = frame;
    named(invalid, L"270").observed_glyph_count = 0;
    check(read_sph_hud(invalid).refusal == SphHudRefusal::invalid_frame,
          "unknown glyph count cannot authorize an inferred numeric word footprint");
    invalid = frame;
    named(invalid, L"270").reading.minimum_confidence = 0.0F;
    check(read_sph_hud(invalid).refusal == SphHudRefusal::invalid_frame,
          "a glyph-union OCR fallback must provide its actual minimum character score");
    invalid = frame;
    named(invalid, L"270").reading.confidence = 0.0F;
    check(read_sph_hud(invalid).refusal == SphHudRefusal::invalid_frame,
          "a glyph-union OCR fallback cannot borrow the unknown-score Windows contract");
    invalid = frame;
    named(invalid, L"270").reading.confidence = 0.80F;
    check(read_sph_hud(invalid).refusal == SphHudRefusal::low_read_confidence,
          "new glyph evidence does not relax the existing OCR readability threshold");
    invalid = frame;
    named(invalid, L"270").reading.alternate_text = L"285";
    check(read_sph_hud(invalid).refusal == SphHudRefusal::invalid_frame,
          "two conflicting numeric views cannot become a complete observed compass word");
    invalid = frame;
    named(invalid, L"270").reading.alternate_text = L"２７０";
    check(read_sph_hud(invalid).refusal == SphHudRefusal::invalid_frame,
          "a scored glyph fallback requires exact ASCII digits in both views");
    invalid = frame;
    named(invalid, L"270").bounds_source = SphHudTokenBoundsSource::recognition_region;
    check(read_sph_hud(invalid).refusal == SphHudRefusal::invalid_frame,
          "glyph count and high scores never turn a recognition crop into measured word bounds");
}

void compass_absolute_phase() {
    for (const auto source : {SphHudTokenBoundsSource::ocr_word,
                              SphHudTokenBoundsSource::pixel_glyph_union}) {
        auto frame = fixture();
        for (unsigned i = 0; i < 6; ++i) {
            auto& word = named(frame, std::to_wstring(210 + i * 15));
            word.bounds_source = source;
            word.observed_glyph_count = 3;
            word.reading.alternate_text = word.reading.text;
            word.reading.confidence = 0.999F;
            word.reading.minimum_confidence = 0.998F;
        }
        check(read_sph_hud(frame).accepted(),
              "complete matching compass words preserve their printed 15-degree phase");
        for (unsigned i = 0; i < 6; ++i) {
            auto& word = named(frame, std::to_wstring(210 + i * 15));
            word.reading.text = word.reading.alternate_text = std::to_wstring(211 + i * 15);
        }
        check(read_sph_hud(frame).refusal == SphHudRefusal::inconsistent_compass_scale,
              source == SphHudTokenBoundsSource::ocr_word
                  ? "coherently shifted OCR words cannot bypass the absolute compass grid phase"
                  : "coherently shifted scored glyph unions cannot bypass the absolute compass grid phase");
    }
}

void compass_word_regions() {
    const ImageRect strip{200, 180, 420, 205};
    const auto blank = [] { return Image{1280, 720, std::vector<std::uint8_t>(1280 * 720 * 3, 190)}; };
    auto image = blank();
    check(extract_sph_hud_compass_word_regions(image, strip).empty(), "empty compass pixels invent no words");
    compass_character(image, 220, 188);
    check(extract_sph_hud_compass_word_regions(image, strip).empty(), "one component cannot invent a neighbouring character");
    compass_character(image, 227, 188); compass_character(image, 234, 188);
    compass_character(image, 300, 188); compass_character(image, 307, 188);
    compass_character(image, 500, 400); compass_character(image, 507, 400);
    const auto regions = extract_sph_hud_compass_word_regions(image, strip);
    check(regions.size() == 2 && same_rect(regions[0], {220, 188, 239, 195}) &&
          same_rect(regions[1], {300, 188, 312, 195}),
          "word candidates are exact ink unions in left-to-right order without padding or outside-strip pixels");
    const auto evidence = extract_sph_hud_compass_words(image, strip);
    check(evidence.size() == 2 && evidence[0].glyph_count == 3 && evidence[1].glyph_count == 2 &&
          evidence[0].ink_pixels.size() == 48 && evidence[1].ink_pixels.size() == 32,
          "glyph evidence retains exactly the independently observed component count and ink pixels");
    for (const auto& word : evidence)
        for (const auto pixel : word.ink_pixels) {
            const auto at = (static_cast<std::size_t>(pixel.y) * image.width + pixel.x) * 3;
            check(pixel.x >= word.bounds.left && pixel.x < word.bounds.right &&
                  pixel.y >= word.bounds.top && pixel.y < word.bounds.bottom && image.bgr[at] == 60,
                  "retained glyph pixels are real source ink inside the measured union");
        }
    auto large = Image{2560, 1440, std::vector<std::uint8_t>(2560 * 1440 * 3, 190)};
    for (const int x : {220, 227, 234, 300, 307}) compass_character(large, x * 2, 376, 2);
    const auto large_regions = extract_sph_hud_compass_word_regions(large, scaled(strip, 2));
    check(large_regions.size() == 2 && same_rect(large_regions[0], {440, 376, 478, 390}) &&
          same_rect(large_regions[1], {600, 376, 624, 390}),
          "scaled source pixels preserve exact physical unions for separately scaled OCR padding");
    check(extract_sph_hud_compass_word_regions(image, {224, 180, 260, 205}).empty(),
          "a clipped leading character cannot become a shorter complete word");

    image = blank(); compass_character(image, 220, 188); compass_character(image, 240, 188);
    check(extract_sph_hud_compass_word_regions(image, strip).empty(), "distant characters do not manufacture a word across empty pixels");
    image = blank(); compass_character(image, 220, 188); compass_character(image, 227, 192);
    check(extract_sph_hud_compass_word_regions(image, strip).empty(), "sloping character baselines are not a horizontal compass word");
    image = blank(); compass_character(image, 220, 185); compass_character(image, 220, 197);
    check(extract_sph_hud_compass_word_regions(image, strip).empty(), "vertical character arrangement is not a compass word");
    image = blank(); compass_character(image, 220, 188); compass_character(image, 227, 188);
    rect(image, 224, 191, 228, 192, 60);
    check(extract_sph_hud_compass_word_regions(image, strip).empty(),
          "merged ink is not split into invented independent character components");
    image = blank(); rect(image, 220, 188, 250, 195, 60); compass_character(image, 252, 188);
    check(extract_sph_hud_compass_word_regions(image, strip).empty(), "wide scenery fragment cannot become a numeral component");
    image = blank(); compass_character(image, 220, 188, 1, 121); compass_character(image, 227, 188, 1, 121);
    check(extract_sph_hud_compass_word_regions(image, strip).empty(), "bright neutral pixels exceed the observed dark-ink bound");
    image = blank(); compass_character(image, 220, 188); compass_character(image, 227, 188);
    for (std::size_t at = 0; at < image.bgr.size(); at += 3) {
        if (image.bgr[at] != 60) continue;
        image.bgr[at] = 100; image.bgr[at + 1] = 112; image.bgr[at + 2] = 124;
    }
    check(extract_sph_hud_compass_word_regions(image, strip).size() == 1,
          "both independent dark-neutral masks include their strict mean and chroma boundary pixels");
    for (std::size_t at = 0; at < image.bgr.size(); at += 3) {
        if (image.bgr[at] == 100) { image.bgr[at] = 99; image.bgr[at + 2] = 124; }
    }
    check(extract_sph_hud_compass_word_regions(image, strip).empty(), "coloured foliage cannot use neutral glyph geometry");
    image = blank(); compass_character(image, 220, 188); compass_character(image, 227, 188);
    compass_character(image, 234, 188, 1, 116);
    check(extract_sph_hud_compass_words(image, strip).empty(),
          "independent threshold masks that disagree on a third glyph refuse the whole word");

    image = blank();
    for (int i = 0; i < 12; ++i) {
        compass_character(image, 210 + i * 29, 188); compass_character(image, 217 + i * 29, 188);
    }
    const ImageRect crowded{200, 180, 630, 205};
    const auto twelve = extract_sph_hud_compass_word_regions(image, crowded);
    check(twelve.size() == 12 && same_rect(twelve.front(), {210, 188, 222, 195}) &&
          same_rect(twelve.back(), {529, 188, 541, 195}), "twelve candidates retain deterministic bounded order");
    compass_character(image, 558, 188); compass_character(image, 565, 188);
    check(extract_sph_hud_compass_word_regions(image, crowded).empty(),
          "candidate overflow refuses the strip instead of selecting a convenient subset");
    check(extract_sph_hud_compass_word_regions(Image{1280, 720, {}}, strip).empty() &&
          extract_sph_hud_compass_word_regions(image, {-1, 180, 420, 205}).empty() &&
          extract_sph_hud_compass_word_regions(image, {200, 180, 200, 205}).empty() &&
          extract_sph_hud_compass_word_regions(image, {0, 0, 1280, 720}).empty(),
          "invalid storage, bounds and unbounded full-frame scans are refused");
}

void numeric_score_admission() {
    OcrResult reading;
    reading.text = L"990 mils";
    reading.alternate_text = L"990 mil";
    reading.confidence = 0.90F;
    reading.minimum_confidence = 0.70F;
    check(sph_hud_numeric_scores_qualified(reading),
          "the unchanged numeric score thresholds admit actual boundary scores");
    reading.line_count = 1;
    check(sph_hud_numeric_scores_qualified(reading), "one measured OCR line retains score admission");
    reading.line_count = 2;
    check(!sph_hud_numeric_scores_qualified(reading), "multiple OCR lines cannot enter one numeric row");
    reading.line_count = 0;
    reading.alternate_text = L"980 mils";
    check(sph_hud_numeric_scores_qualified(reading),
          "high-score alternate disagreement stays visible to the strict parser");
    reading.text = reading.alternate_text = L"990m";
    check(sph_hud_numeric_scores_qualified(reading),
          "admission never discards a high-score reading because its unit is incompatible with MIL");
    reading.text.clear();
    check(!sph_hud_numeric_scores_qualified(reading), "empty OCR has no numeric evidence to admit");
    reading.text = reading.alternate_text = L"270";
    for (const float invalid : {0.0F, -0.1F, 0.899F, 1.001F,
                                std::numeric_limits<float>::quiet_NaN(),
                                std::numeric_limits<float>::infinity()}) {
        reading.confidence = invalid; reading.minimum_confidence = 0.85F;
        check(!sph_hud_numeric_scores_qualified(reading),
              "missing, weak, excessive and nonfinite average scores never enter numeric evidence");
    }
    for (const float invalid : {0.0F, -0.1F, 0.699F, 1.001F,
                                std::numeric_limits<float>::quiet_NaN(),
                                std::numeric_limits<float>::infinity()}) {
        reading.confidence = 0.95F; reading.minimum_confidence = invalid;
        check(!sph_hud_numeric_scores_qualified(reading),
              "missing, weak, excessive and nonfinite character scores never enter numeric evidence");
    }
    reading.confidence = reading.minimum_confidence = 0.0F;
    check(!sph_hud_numeric_scores_qualified(reading),
          "a zero-score glyph CTC result cannot borrow Windows-style missing-score semantics");

    const auto ctc_fixture = [] {
        auto frame = fixture();
        for (const auto label : {L"990 mils", L"1,000 mil", L"1,010 mils",
                                 L"2,170m", L"2,147m", L"2,122m"}) {
            auto& value = named(frame, label);
            value.bounds_source = SphHudTokenBoundsSource::recognition_region;
            value.reading.confidence = 0.95F;
            value.reading.minimum_confidence = 0.85F;
            value.reading.alternate_text = value.reading.text;
        }
        return frame;
    };
    const auto admit = [](SphHudFrame frame) {
        std::erase_if(frame.tokens, [&](const auto& value) {
            if (value.bounds_source != SphHudTokenBoundsSource::recognition_region) return false;
            auto* instrument = encloses(frame.layout.mil_scale.labels, value.bounds)
                ? &frame.diagnostics.mil : encloses(frame.layout.range_scale.labels, value.bounds)
                ? &frame.diagnostics.range : nullptr;
            if (!instrument || sph_hud_numeric_scores_qualified(value.reading)) return false;
            ++instrument->admission_rejections;
            return true;
        });
        return frame;
    };
    const auto weaken = [](SphHudToken& value) {
        value.reading.confidence = 0.89F;
        value.reading.minimum_confidence = 0.40F;
        value.reading.text = L"unobserved";
        value.reading.alternate_text = L"different unobserved pixels";
    };
    auto frame = ctc_fixture();
    const auto original = read_sph_hud(frame);
    weaken(named(frame, L"1,010 mils"));
    weaken(named(frame, L"2,122m"));
    const auto surplus = admit(frame);
    const auto observed = read_sph_hud(surplus);
    check(observed.accepted() && original.accepted() && observed.mil_scale->labels.size() == 2 &&
          observed.range_scale->labels.size() == 2 &&
          close(observed.pose->sight_mil, original.pose->sight_mil) &&
          surplus.diagnostics.mil.admission_rejections == 1 &&
          surplus.diagnostics.range.admission_rejections == 1,
          "two real paired labels bracket the reticle after only weak surplus observations are omitted");

    frame = ctc_fixture();
    weaken(named(frame, L"990 mils"));
    check(read_sph_hud(admit(frame)).refusal == SphHudRefusal::reticle_outside_labels,
          "omitting a weak required upper MIL anchor cannot extrapolate from lower labels");
    weaken(named(frame, L"1,000 mil"));
    check(read_sph_hud(admit(frame)).refusal == SphHudRefusal::missing_mil_labels,
          "one remaining real MIL label cannot be completed by score admission");
    frame = ctc_fixture();
    weaken(named(frame, L"2,170m"));
    check(read_sph_hud(admit(frame)).refusal == SphHudRefusal::reticle_outside_labels,
          "omitting a weak required upper range anchor cannot extrapolate distance");
    weaken(named(frame, L"2,147m"));
    check(read_sph_hud(admit(frame)).refusal == SphHudRefusal::missing_range_labels,
          "one remaining real range label cannot be completed by score admission");

    frame = ctc_fixture();
    auto& wrong_grid = named(frame, L"990 mils");
    wrong_grid.reading.text = wrong_grid.reading.alternate_text = L"980 mils";
    const auto admitted_grid = admit(frame);
    check(admitted_grid.diagnostics.mil.admission_rejections == 0 &&
          read_sph_hud(admitted_grid).refusal == SphHudRefusal::inconsistent_mil_scale,
          "high-score numbers conflicting with the actual major grid remain a parser refusal");
    frame = ctc_fixture();
    named(frame, L"990 mils").reading.alternate_text = L"980 mils";
    const auto admitted_conflict = admit(frame);
    check(admitted_conflict.diagnostics.mil.admission_rejections == 0 &&
          read_sph_hud(admitted_conflict).refusal == SphHudRefusal::low_read_confidence,
          "high-score conflicting internal readings are retained and refused without selecting a value");
    frame = ctc_fixture();
    auto& wrong_unit = named(frame, L"990 mils");
    wrong_unit.reading.text = wrong_unit.reading.alternate_text = L"990m";
    check(read_sph_hud(admit(frame)).refusal == SphHudRefusal::ambiguous_mil_labels,
          "score admission cannot hide a high-score invalid unit from the parser");
}

void numeric_source_word_bounds() {
    const ImageRect region{210, 180, 280, 205};
    auto image = Image{1280, 720, std::vector<std::uint8_t>(1280 * 720 * 3, 20)};
    // Three tall source glyphs plus four suffix glyphs, separated by an actual
    // word gap. Localization has no assigned number or expected unit string.
    for (const int x : {220, 227, 234, 244, 251, 258, 265})
        compass_character(image, x, 188, 1, 200);
    const auto word = extract_sph_hud_numeric_word(image, region);
    check(word && same_rect(*word, {220, 188, 270, 195}),
          "the measured numeric crop retains all original leading glyphs and the complete separated suffix");
    const auto enlarged = extract_sph_hud_numeric_word(pixel_scaled(image, 2), scaled(region, 2));
    check(enlarged && same_rect(*enlarged, {440, 376, 540, 390}),
          "numeric ink localization preserves actual source-pixel geometry across DPI scaling");
    check(!extract_sph_hud_numeric_word(image, {222, 180, 280, 205}),
          "a clipped leading numeric glyph never produces a shorter tight crop");
    auto prefix = image;
    rect(prefix, 213, 191, 217, 193, 200);
    check(!extract_sph_hud_numeric_word(prefix, region),
          "significant separate prefix punctuation stays in the broad original crop");
    auto disagreement = image;
    rect(disagreement, 265, 188, 270, 195, 20);
    compass_character(disagreement, 265, 188, 1, 170);
    check(!extract_sph_hud_numeric_word(disagreement, region),
          "independent bright masks that lose a suffix glyph cannot certify a complete numeric crop");
    auto scenery = image;
    rect(scenery, 212, 183, 232, 185, 200);
    check(!extract_sph_hud_numeric_word(scenery, region),
          "an oversized significant scenery component cannot be silently excluded from the measured word");
    auto split_line = image;
    rect(split_line, 265, 188, 270, 195, 20);
    compass_character(split_line, 265, 191, 1, 200);
    check(!extract_sph_hud_numeric_word(split_line, region),
          "numeric and suffix components on incompatible baselines cannot be joined into one crop");
    auto interior_dot = image;
    rect(interior_dot, 222, 191, 223, 193, 200);
    const auto dotted = extract_sph_hud_numeric_word(interior_dot, region);
    check(dotted && same_rect(*dotted, {220, 188, 270, 195}),
          "significant detached ink inside the full source union remains available in the original RGB crop");
    auto drift = image;
    rect(drift, 218, 189, 220, 194, 170);
    check(!extract_sph_hud_numeric_word(drift, region),
          "the same glyph count cannot hide a two-pixel source edge disagreement between masks");
}

void compass_word_completeness() {
    const ImageRect strip{400, 360, 840, 410};
    auto image = Image{2560, 1440, std::vector<std::uint8_t>(2560 * 1440 * 3, 200)};
    compass_character(image, 440, 376, 2);
    compass_character(image, 454, 376, 2);
    const auto clean = extract_sph_hud_compass_words(image, strip);
    check(clean.size() == 1 && clean.front().glyph_count == 2 &&
          same_rect(clean.front().bounds, {440, 376, 464, 390}),
          "a complete two-glyph numeric word retains its measured ink without requiring a third digit");
    const auto sparse = [](Image& target, int left) {
        for (int y = 0; y < 14; ++y)
            rect(target, left + std::min(y, 11), 376 + y,
                 left + std::min(y, 11) + 1, 377 + y, 60);
    };
    auto leading = image;
    sparse(leading, 427);
    check(extract_sph_hud_compass_words(leading, strip).empty(),
          "a rejected sparse leading component prevents promotion of its surviving two-glyph suffix");
    auto trailing = image;
    sparse(trailing, 466);
    check(extract_sph_hud_compass_words(trailing, strip).empty(),
          "a rejected sparse trailing component prevents promotion of an incomplete shorter word");
    auto clipped = image;
    rect(clipped, 400, 376, 438, 377, 60);
    rect(clipped, 437, 376, 438, 390, 60);
    check(extract_sph_hud_compass_words(clipped, strip).empty(),
          "a clipped significant source component adjacent to the word cannot be dropped as scenery");
    auto distant = image;
    sparse(distant, 550);
    const auto unaffected = extract_sph_hud_compass_words(distant, strip);
    check(unaffected.size() == 1 && same_rect(unaffected.front().bounds, {440, 376, 464, 390}),
          "a distant rejected scenery component does not globally erase a complete independent word");
}

void unanchored_compass_evidence() {
    auto frame = fixture();
    for (const auto text : {L"210", L"225", L"270", L"285"}) remove(frame, text);
    const auto baseline = read_sph_hud(frame);
    check(baseline.accepted() && baseline.compass->labels.size() == 2,
          "two actual inner compass labels independently bracket the pointer");
    OcrResult unknown;
    unknown.text = unknown.alternate_text = L"300";
    unknown.confidence = unknown.minimum_confidence = 0.99F;
    // This whole observed word is outside the first measured tick's physical
    // anchoring corridor. Preserve it as raw unknown evidence, never as a tick.
    frame.unanchored_compass_tokens.push_back({unknown, {484, 188, 501, 196}, true,
                                               SphHudTokenBoundsSource::ocr_word});
    frame.diagnostics.compass.unanchored_tokens = 1;
    const auto retained = read_sph_hud(frame);
    check(retained.accepted() && close(retained.pose->bearing_deg, baseline.pose->bearing_deg) &&
          retained.compass->labels.size() == 2 && frame.unanchored_compass_tokens.size() == 1 &&
          frame.unanchored_compass_tokens.front().reading.text == L"300",
          "raw words beyond the actual measured grid remain unknown without changing genuine interpolation");
    auto unfiltered = frame;
    unfiltered.tokens.push_back(frame.unanchored_compass_tokens.front());
    check(read_sph_hud(unfiltered).refusal == SphHudRefusal::ambiguous_compass_labels,
          "the core parser still refuses an unanchored word incorrectly supplied as numeric evidence");
    auto conflict = frame;
    auto& value = named(conflict, L"240");
    value.reading = unknown;
    check(read_sph_hud(conflict).refusal == SphHudRefusal::inconsistent_compass_scale,
          "a high-score conflict overlapping the actual tick grid cannot be moved into unknown evidence");
    auto missing = frame;
    remove(missing, L"255");
    check(read_sph_hud(missing).refusal == SphHudRefusal::missing_compass_labels,
          "a retained unanchored numeral cannot replace a missing required compass anchor");
    auto outside = fixture();
    for (const auto text : {L"210", L"255", L"270", L"285"}) remove(outside, text);
    outside.unanchored_compass_tokens = frame.unanchored_compass_tokens;
    check(read_sph_hud(outside).refusal == SphHudRefusal::compass_pointer_outside_labels,
          "unknown words do not permit compass extrapolation beyond the two actual labels");
}

}  // namespace

int main() {
    compass_word_regions();
    rounded_instrument_roi_containment();
    reticle_localization();
    compass_geometry_uncertainty();
    instrument_isolation();
    diagnostic_semantics();
    pixel_glyph_provenance();
    compass_absolute_phase();
    numeric_score_admission();
    numeric_source_word_bounds();
    compass_word_completeness();
    unanchored_compass_evidence();
    auto frame = fixture();
    const auto first = read_sph_hud(frame);
    check(first.accepted() && close(first.pose->sight_mil, 997.954545, 0.001) &&
          close(first.pose->bearing_deg, 245.863636, 0.001),
          "actual sight is interpolated from labelled ticks, independent of command and ballistics");
    check(first.accepted() && first.provenance.frame_id == 41 && !first.provenance.ocr_confidence_available &&
          close(first.pose->normalized_reticle.x, 639.5 / 1280) && first.pose->mil_uncertainty > 0.0,
          "Windows-style missing model scores do not become a fabricated probability");
    check(first.asl && first.asl->displayed_value == 82 && first.asl->unit.empty() &&
          first.stabilized == true && first.ammunition && first.ammunition->count == 13,
          "raw ASL, actual bottom state and single-shell counter exclude transition and MG counter");
    auto large = read_sph_hud(scaled(frame, 1.25));
    check(large.accepted() && close(large.pose->sight_mil, first.pose->sight_mil) &&
          close(large.pose->bearing_deg, first.pose->bearing_deg) &&
          close(large.pose->normalized_reticle.x, first.pose->normalized_reticle.x),
          "source-image scaling preserves measured pose and normalized reticle");
    auto missing = frame; remove(missing, L"1,000 mil");
    check(read_sph_hud(missing).accepted(), "two complete labels may bracket three independently verified major tick rows");
    remove(missing, L"990 mils");
    check(read_sph_hud(missing).refusal == SphHudRefusal::missing_mil_labels,
          "one readable MIL label never supplies an actual sight reading");
    auto mismatch = frame; mismatch.layout.range_scale.ticks[1].position.y += 8;
    check(read_sph_hud(mismatch).refusal == SphHudRefusal::scale_row_disagreement,
          "RNG and MIL anchors from incompatible rows are refused");
    auto wrong_unit = frame; named(wrong_unit, L"1,000 mil").reading.text = L"1,000m";
    check(read_sph_hud(wrong_unit).refusal == SphHudRefusal::ambiguous_mil_labels,
          "metres are not accepted as a MIL reading");
    auto noise = frame; named(noise, L"1,000 mil").reading.text = L"1.OOO mil";
    check(!read_sph_hud(noise).accepted(), "OCR letter substitutions are not invented digits");
    noise = frame; named(noise, L"1,000 mil").reading.text = L"1.000 mil";
    check(read_sph_hud(noise).refusal == SphHudRefusal::inconsistent_mil_scale,
          "ambiguous thousands punctuation cannot silently change 1000 to 1 MIL");
    auto duplicate = frame; token(duplicate, L"1000", {831, 373, 862, 383});
    check(read_sph_hud(duplicate).refusal == SphHudRefusal::ambiguous_mil_labels,
          "multiple numeric readings attached to one tick remain ambiguous");
    auto low = frame; named(low, L"990 mils").reading.confidence = 0.6F;
    check(read_sph_hud(low).refusal == SphHudRefusal::low_read_confidence, "weak OCR is refused even if numerically plausible");
    low = frame; named(low, L"990 mils").reading.minimum_confidence = std::numeric_limits<float>::quiet_NaN();
    check(read_sph_hud(low).refusal == SphHudRefusal::low_read_confidence, "nonfinite readability scores cannot be accepted");
    auto alternate = frame; named(alternate, L"990 mils").reading.alternate_text = L"980 mils";
    check(!read_sph_hud(alternate).accepted(), "disagreeing OCR passes never fabricate a complete reading");
    alternate = frame; named(alternate, L"990 mils").reading.alternate_text = L"990 mil";
    check(read_sph_hud(alternate).accepted(), "exact singular and plural MIL units describe the same OCR evidence");
    auto rotated = frame; named(rotated, L"990 mils").geometry_verified = false;
    check(read_sph_hud(rotated).refusal == SphHudRefusal::invalid_frame,
          "untransformed rotated OCR word bounds cannot contribute to interpolation");
    auto crop_bounds = frame;
    named(crop_bounds, L"240").bounds_source = SphHudTokenBoundsSource::recognition_region;
    check(read_sph_hud(crop_bounds).refusal == SphHudRefusal::invalid_frame,
          "a compass recognition crop is not an independently observed word box");
    crop_bounds = frame;
    named(crop_bounds, L"240").bounds_source = SphHudTokenBoundsSource::unverified;
    check(read_sph_hud(crop_bounds).refusal == SphHudRefusal::invalid_frame,
          "legacy unspecified compass box provenance cannot establish an absolute anchor");
    auto shifted_phase = frame;
    shifted_phase.layout.compass.labels.right += 21;
    for (auto& value : shifted_phase.tokens) {
        if (value.bounds.top != 188) continue;
        value.bounds.left += 21; value.bounds.right += 21;
    }
    check(read_sph_hud(shifted_phase).refusal == SphHudRefusal::ambiguous_compass_labels,
          "Steam-style labels shifted about 20.7 native pixels cannot inherit major-tick values");
    check(read_sph_hud(scaled(shifted_phase, 1.5)).refusal == SphHudRefusal::ambiguous_compass_labels,
          "the compass phase refusal preserves its physical bound at 1920x1080");
    auto wide_box = frame;
    wide_box.layout.compass.labels.right = 820;
    named(wide_box, L"285").bounds = {769, 188, 809, 196};
    check(read_sph_hud(wide_box).refusal == SphHudRefusal::ambiguous_compass_labels,
          "wide numeric boxes cannot relax the compass word-to-tick bound");
    auto small_offset = frame;
    for (auto& value : small_offset.tokens) {
        if (value.bounds.top != 188) continue;
        ++value.bounds.left; ++value.bounds.right;
    }
    const auto offset_reading = read_sph_hud(small_offset);
    check(offset_reading.accepted() && offset_reading.compass &&
          close(offset_reading.compass->labels.front().anchor_offset_px, 1.0) &&
          close(offset_reading.pose->bearing_deg, first.pose->bearing_deg) &&
          close(offset_reading.pose->bearing_uncertainty_deg,
                first.pose->bearing_uncertainty_deg + std::abs(first.compass->units_per_pixel)),
          "a small observed label offset remains evidence and enlarges absolute bearing uncertainty");
    auto wrap = frame;
    for (unsigned i = 0; i < 6; ++i)
        named(wrap, std::to_wstring(210 + i * 15)).reading.text = std::to_wstring((330 + i * 15) % 360);
    const auto wrapped = read_sph_hud(wrap);
    check(wrapped.accepted() && close(wrapped.pose->bearing_deg, 5.863636, 0.001),
          "inner compass interpolation crosses north without averaging to south");
    check(wrapped.compass && wrapped.compass->labels[2].value == 0.0 &&
          wrapped.compass->labels[3].value == 15.0 && wrapped.compass->labels[2].text == L"0",
          "circular interpolation retains raw printed numeric labels in evidence");
    auto outside = frame; outside.layout.reticle.y = 500;
    check(read_sph_hud(outside).refusal == SphHudRefusal::reticle_outside_labels,
          "no sight estimate extrapolates beyond visible labels");
    auto nav = frame; nav.layout.compass.labels.top = 0; nav.layout.compass.labels.bottom = 40;
    check(read_sph_hud(nav).refusal == SphHudRefusal::invalid_layout,
          "the boxed navigation heading cannot replace verified inner optic geometry");
    auto wrong_weapon = frame; named(wrong_weapon, L"SPH-2").reading.text = L"BOBCAT";
    check(read_sph_hud(wrong_weapon).refusal == SphHudRefusal::wrong_weapon,
          "another weapon HUD cannot become an SPH sight frame");
    auto spaced_weapon = frame; named(spaced_weapon, L"SPH-2").reading.text = L"S P H \u2014 2";
    named(spaced_weapon, L"S P H \u2014 2").reading.alternate_text = L"SPH-2";
    check(read_sph_hud(spaced_weapon).accepted(),
          "exact weapon characters permit only OCR spacing and equivalent dash glyphs");
    named(spaced_weapon, L"S P H \u2014 2").reading.text = L"s pH";
    check(!read_sph_hud(spaced_weapon).accepted(),
          "weapon character canonicalization never supplies a missing model digit");
    auto state = frame; token(state, L"TRANSITION", {612, 571, 666, 585});
    check(read_sph_hud(state).accepted() && !read_sph_hud(state).stabilized,
          "ambiguous state/action boxes leave optional stabilization unknown");
    auto ammo = frame; ammo.layout.ammunition_kind = SphHudAmmoKind::unverified;
    check(read_sph_hud(ammo).accepted() && !read_sph_hud(ammo).ammunition,
          "unverified ammunition icons cannot turn a readable MG counter into shells");
    auto negative_asl = frame; named(negative_asl, L"82").reading.text = L"-12.5";
    check(read_sph_hud(negative_asl).asl && read_sph_hud(negative_asl).asl->displayed_value == -12.5,
          "negative displayed ASL remains raw without an unverified world-height conversion");
    auto combined_asl = frame; remove(combined_asl, L"ASL"); remove(combined_asl, L"82");
    token(combined_asl, L"ASL -12,5", {516, 621, 577, 638});
    named(combined_asl, L"ASL -12,5").reading.alternate_text = L"ASL -12.5";
    check(read_sph_hud(combined_asl).asl && read_sph_hud(combined_asl).asl->displayed_value == -12.5,
          "an explicit combined ASL prefix preserves its raw decimal reading");
    named(combined_asl, L"ASL -12,5").reading.alternate_text = L"ASL -13.5";
    check(!read_sph_hud(combined_asl).asl,
          "combined ASL OCR disagreements leave height metadata unknown");
    combined_asl = frame; remove(combined_asl, L"ASL");
    check(!read_sph_hud(combined_asl).asl,
          "a bare numeric field does not become ASL without an explicit label");
    auto unverified = frame; unverified.layout.source = SphHudLayoutSource::unverified;
    check(!read_sph_hud(unverified).accepted(), "readable numbers alone do not establish a reticle");
    const auto layout = extract_sph_hud_layout(optic());
    check(layout && close(layout->reticle.x, 639.5, 1.0) && close(layout->reticle.y, 359.5, 1.0) &&
          layout->mil_scale.ticks.size() == 3 && layout->range_scale.ticks.size() == 3 &&
          layout->compass.ticks.size() == 6 && layout->mil_scale.label_regions.size() == 3,
          "pixel extraction measures actual reticle, paired rows and inner compass numeric crops");
    if (layout) {
        frame.layout = *layout;
        check(read_sph_hud(frame).accepted(), "pixel geometry and OCR evidence form one complete observed sight frame");
    }
    check(!extract_sph_hud_layout(optic(false)), "a reload/occluded reticle is refused despite intact numeric scales");
    check(!extract_sph_hud_layout(optic(true, false)), "an unrelated crosshair without paired optic scales is refused");
    const auto supplemental = extract_sph_hud_supplemental_layout(optic(false));
    check(supplemental && supplemental->ammunition &&
          supplemental->ammunition_kind == SphHudAmmoKind::single_shell && supplemental->mil_scale.ticks.empty(),
          "a reload-hidden reticle still exposes its independently verified shell channel");
    if (supplemental) {
        frame = fixture(); frame.layout = *supplemental;
        const auto reload = read_sph_hud_supplemental(frame);
        check(!reload.accepted() && !reload.pose && reload.ammunition && reload.ammunition->count == 13 &&
              reload.refusal == SphHudRefusal::supplemental_only,
              "a supplemental ammo reading never manufactures an actual firing pose");
        named(frame, L"SPH-2").reading.text = L"BOBCAT";
        check(!read_sph_hud_supplemental(frame).ammunition,
              "a single-shell icon on another weapon cannot enter the SPH ammo channel");
    }
    auto rectangle = optic(false);
    rect(rectangle, 1227, 643, 1243, 673, 20);
    rect(rectangle, 1232, 645, 1237, 661, 250);
    check(!extract_sph_hud_supplemental_layout(rectangle),
          "a bright rectangular UI stroke is not a tapered shell plus cartridge base");
    auto eroded = optic(false);
    rect(eroded, 1232, 655, 1237, 656, 20);
    rect(eroded, 1234, 655, 1235, 656, 250);
    check(extract_sph_hud_supplemental_layout(eroded).has_value(),
          "an eroded terminal tip row retains the real tapered shell and separate cartridge base");
    auto no_base = eroded;
    rect(no_base, 1232, 657, 1237, 661, 20);
    check(!extract_sph_hud_supplemental_layout(no_base),
          "a tapered stroke without its aligned cartridge base is not a shell channel");
    check(!extract_sph_hud_layout(Image{1280, 720, {}}), "invalid image storage never reaches pixel sampling");
    check(sph_hud_poll_delay_ms(300, 300, true) == 300,
          "300-ms full reading replaces the prior frame by 900 ms instead of 1200 ms");
    check(sph_hud_poll_delay_ms(300, 380, true) == 220,
          "queued completion latency is counted in the next sight budget");
    check(sph_hud_poll_delay_ms(400, 400, true) == 150,
          "a feasible slow reading retains a nonzero delay within freshness");
    check(sph_hud_poll_delay_ms(500, 500, true) == 150 &&
          sph_hud_poll_delay_ms(600, 600, true) == 150 &&
          sph_hud_poll_delay_ms(850, 850, true) == 150,
          "fresh accepted slow readers resume after bounded idle without the refusal backoff");
    check(sph_hud_poll_delay_ms(10, 10, true) == 700,
          "a fast reader is throttled while reserving time before expiry");
    check(sph_hud_poll_delay_ms(300, 300, false) == 600 &&
          sph_hud_poll_delay_ms(10, 10, false) == 300 &&
          sph_hud_poll_delay_ms(600, 600, false) == 700,
          "partial and rejected readings keep the prior retry cadence");
    check(sph_hud_poll_delay_ms(300, 1000, true) == 600 &&
          sph_hud_poll_delay_ms(850, 1000, true) == 700 &&
          sph_hud_poll_delay_ms(1000, 1000, true) == 700 &&
          sph_hud_poll_delay_ms(UINT64_MAX, UINT64_MAX, true) == 700,
          "stale and overflowing duration inputs cannot underflow the delay");
    if (!failures) std::cout << "SPH HUD tests passed\n";
    return failures ? 1 : 0;
}
