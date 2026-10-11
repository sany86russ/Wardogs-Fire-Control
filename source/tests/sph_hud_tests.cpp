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
    frame.tokens.push_back({std::move(reading), bounds, true});
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

}  // namespace

int main() {
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
    if (!failures) std::cout << "SPH HUD tests passed\n";
    return failures ? 1 : 0;
}
