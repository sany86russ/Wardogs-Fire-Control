#pragma once

#include "wardogs/ocr.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace wardogs {

enum class SphHudTokenBoundsSource {
    unverified,
    recognition_region,
    ocr_word,
};

// Word bounds and every layout point use the original captured image pixels.
// Resizing for OCR must be reversed by the provider before building a frame.
// A recognition crop is not an observed glyph box. Compass labels require
// actual unrotated OCR word bounds to verify their measured tick association.
struct SphHudToken {
    OcrResult reading;
    ImageRect bounds{};
    bool geometry_verified{};
    SphHudTokenBoundsSource bounds_source{SphHudTokenBoundsSource::unverified};
};

struct SphHudTick {
    Point position;
};

enum class SphHudLayoutSource {
    unverified,
    optic_pixel_geometry,
    supplemental_icon_geometry,
};

enum class SphHudAmmoKind {
    unverified,
    single_shell,
};

struct SphHudScaleLayout {
    ImageRect labels{};
    std::vector<SphHudTick> ticks;
    ImageRect header{};
    std::vector<ImageRect> label_regions;
};

struct SphHudLayout {
    SphHudLayoutSource source{SphHudLayoutSource::unverified};
    Point reticle;
    double reticle_uncertainty_px{};
    double tick_uncertainty_px{};
    SphHudScaleLayout mil_scale;
    SphHudScaleLayout range_scale;
    SphHudScaleLayout compass;
    Point compass_pointer;
    double compass_pointer_uncertainty_px{};
    // Optional fields are read only inside independently identified HUD boxes.
    // Missing boxes and absent labels never become default metadata values.
    std::optional<ImageRect> asl;
    std::optional<ImageRect> stabilization;
    std::optional<ImageRect> ammunition;
    SphHudAmmoKind ammunition_kind{SphHudAmmoKind::unverified};
    std::optional<ImageRect> weapon;
};

struct SphHudFrame {
    int client_width{};
    int client_height{};
    ImageRect captured_client_region{};
    std::uint64_t frame_id{};
    std::string provider;
    std::vector<SphHudToken> tokens;
    SphHudLayout layout;
};

enum class SphHudRefusal {
    none,
    supplemental_only,
    invalid_frame,
    unverified_layout,
    invalid_layout,
    missing_scale_headers,
    wrong_weapon,
    missing_mil_labels,
    ambiguous_mil_labels,
    low_read_confidence,
    inconsistent_mil_scale,
    reticle_outside_labels,
    missing_range_labels,
    ambiguous_range_labels,
    scale_row_disagreement,
    inconsistent_range_scale,
    missing_compass_labels,
    ambiguous_compass_labels,
    inconsistent_compass_scale,
    compass_pointer_outside_labels,
    excessive_uncertainty,
};

struct SphHudLabelEvidence {
    double value{};
    ImageRect bounds{};
    Point tick;
    std::wstring text;
    double anchor_offset_px{};
};

struct SphHudScaleReading {
    double value{};
    double uncertainty{};
    double units_per_pixel{};
    std::vector<SphHudLabelEvidence> labels;
};

struct SphHudPose {
    // These are the observed sight readings. No bore angle is inferred.
    double sight_mil{};
    double bearing_deg{};
    double mil_uncertainty{};
    double bearing_uncertainty_deg{};
    Point normalized_reticle;
};

struct SphHudAmmo {
    unsigned count{};
    SphHudAmmoKind kind{SphHudAmmoKind::unverified};
    ImageRect bounds{};
};

struct SphHudAsl {
    double displayed_value{};
    std::wstring unit;
    ImageRect bounds{};
};

struct SphHudProvenance {
    std::uint64_t frame_id{};
    std::string provider;
    ImageRect captured_client_region{};
    SphHudLayoutSource layout_source{SphHudLayoutSource::unverified};
    bool ocr_confidence_available{};
    // Raw OCR scores describe readability, never a hit probability.
    float minimum_read_score{};
};

struct SphHudReading {
    SphHudRefusal refusal{SphHudRefusal::invalid_frame};
    std::optional<SphHudPose> pose;
    std::optional<SphHudScaleReading> mil_scale;
    std::optional<SphHudScaleReading> range_scale;
    std::optional<SphHudScaleReading> compass;
    std::optional<SphHudAsl> asl;
    std::optional<bool> stabilized;
    std::optional<SphHudAmmo> ammunition;
    SphHudProvenance provenance;

    [[nodiscard]] bool accepted() const noexcept {
        return refusal == SphHudRefusal::none && pose.has_value();
    }
};

// Interpolates visible numeric labels at independently detected tick anchors.
// The real optic has RNG at the left and MIL at the right. Their independently
// detected tick rows must agree. RNG is reported as displayed HUD range, never
// converted through a ballistic table to manufacture a second MIL reading.
// There is no last-command, screen-centre or physical-angle fallback.
SphHudReading read_sph_hud(const SphHudFrame& frame);

// Finds the actual reticle strokes, paired side ticks and inner compass ruler.
// The returned label crops allow an OCR provider to read each numeric row at
// useful resolution. Occlusion or an ambiguous pixel pattern returns no layout.
std::optional<SphHudLayout> extract_sph_hud_layout(const Image& image);

// Bounded numeric-word candidates from separate dark-neutral pixel components
// inside the supplied compass strip. Bounds are exact original-pixel unions,
// without OCR padding, inferred values or tick anchoring. These candidates do
// not verify a word: the provider must still supply actual unrotated OCR bounds.
// More than twelve candidates or an invalid/oversized strip returns no regions.
std::vector<ImageRect> extract_sph_hud_compass_word_regions(const Image& image, ImageRect strip);

// Reload animation may hide the reticle while leaving the single-shell icon
// and its counter intact. This path observes that separate channel only.
std::optional<SphHudLayout> extract_sph_hud_supplemental_layout(const Image& image);
SphHudReading read_sph_hud_supplemental(const SphHudFrame& frame);

}  // namespace wardogs
