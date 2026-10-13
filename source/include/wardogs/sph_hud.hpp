#pragma once

#include "wardogs/ocr.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wardogs {

enum class SphHudTokenBoundsSource {
    unverified,
    recognition_region,
    ocr_word,
    pixel_glyph_union,
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
    std::uint32_t observed_glyph_count{};
};

struct SphHudInkPixel { int x; int y; };

struct SphHudCompassWord {
    ImageRect bounds{};
    std::uint32_t glyph_count{};
    std::vector<SphHudInkPixel> ink_pixels;
};

struct SphHudTick {
    Point position;
};

enum class SphHudLayoutSource {
    unverified,
    optic_pixel_geometry,
    supplemental_icon_geometry,
};

enum class SphHudRecognitionStage {
    not_started,
    invalid_image,
    layout_not_found,
    optic_layout,
    supplemental_layout,
    instrument_ocr,
    completed,
};

enum class SphHudLayoutStage {
    not_started,
    invalid_image,
    reticle,
    left_ticks,
    right_ticks,
    paired_rows,
    compass_baseline,
    compass_ticks,
    ready,
};

struct SphHudLayoutDiagnostics {
    SphHudLayoutStage stage{SphHudLayoutStage::not_started};
    std::uint32_t reticle_candidates{};
    std::uint32_t left_ticks{};
    std::uint32_t right_ticks{};
    std::uint32_t compass_strong_columns{};
    std::uint32_t compass_faint_columns{};
    std::uint32_t compass_grid_ticks{};
    double reticle_uncertainty_px{};
    double compass_baseline_contrast{};
    double compass_tick_uncertainty_px{};
    bool compass_faint_rescue{};
    bool compass_weak_baseline{};
    bool compass_grid_ambiguous{};
};

// Fixed-size counters preserve failure progress without retaining game pixels
// or raw OCR text. A token count is evidence volume, never acceptance/confidence.
struct SphHudInstrumentDiagnostics {
    std::uint32_t attempted_regions{};
    std::uint32_t tokens{};
    std::uint32_t geometry_rejections{};
    std::uint32_t admission_rejections{};
    std::uint32_t unanchored_tokens{};
    bool budget_exhausted{};
};

struct SphHudRecognitionDiagnostics {
    SphHudRecognitionStage stage{SphHudRecognitionStage::not_started};
    SphHudLayoutSource layout_source{SphHudLayoutSource::unverified};
    SphHudLayoutDiagnostics geometry;
    SphHudInstrumentDiagnostics mil;
    SphHudInstrumentDiagnostics range;
    SphHudInstrumentDiagnostics compass;
    SphHudInstrumentDiagnostics asl;
    SphHudInstrumentDiagnostics stabilization;
    SphHudInstrumentDiagnostics ammunition;
    SphHudInstrumentDiagnostics weapon;
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
    // Zero means legacy caller geometry; extracted column clouds retain their
    // measured localisation interval and cannot silently use a narrower one.
    double tick_uncertainty_px{};
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
    // Complete observations outside the independently measured compass ruler
    // are diagnostic evidence only, never interpolation anchors.
    std::vector<SphHudToken> unanchored_compass_tokens;
    SphHudLayout layout;
    SphHudRecognitionDiagnostics diagnostics;
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

[[nodiscard]] constexpr std::string_view sph_hud_refusal_name(SphHudRefusal refusal) noexcept {
    switch (refusal) {
        case SphHudRefusal::none: return "none";
        case SphHudRefusal::supplemental_only: return "supplemental_only";
        case SphHudRefusal::invalid_frame: return "invalid_frame";
        case SphHudRefusal::unverified_layout: return "unverified_layout";
        case SphHudRefusal::invalid_layout: return "invalid_layout";
        case SphHudRefusal::missing_scale_headers: return "missing_scale_headers";
        case SphHudRefusal::wrong_weapon: return "wrong_weapon";
        case SphHudRefusal::missing_mil_labels: return "missing_mil_labels";
        case SphHudRefusal::ambiguous_mil_labels: return "ambiguous_mil_labels";
        case SphHudRefusal::low_read_confidence: return "low_read_confidence";
        case SphHudRefusal::inconsistent_mil_scale: return "inconsistent_mil_scale";
        case SphHudRefusal::reticle_outside_labels: return "reticle_outside_labels";
        case SphHudRefusal::missing_range_labels: return "missing_range_labels";
        case SphHudRefusal::ambiguous_range_labels: return "ambiguous_range_labels";
        case SphHudRefusal::scale_row_disagreement: return "scale_row_disagreement";
        case SphHudRefusal::inconsistent_range_scale: return "inconsistent_range_scale";
        case SphHudRefusal::missing_compass_labels: return "missing_compass_labels";
        case SphHudRefusal::ambiguous_compass_labels: return "ambiguous_compass_labels";
        case SphHudRefusal::inconsistent_compass_scale: return "inconsistent_compass_scale";
        case SphHudRefusal::compass_pointer_outside_labels: return "compass_pointer_outside_labels";
        case SphHudRefusal::excessive_uncertainty: return "excessive_uncertainty";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view sph_hud_layout_source_name(SphHudLayoutSource source) noexcept {
    switch (source) {
        case SphHudLayoutSource::unverified: return "unverified";
        case SphHudLayoutSource::optic_pixel_geometry: return "optic_pixel_geometry";
        case SphHudLayoutSource::supplemental_icon_geometry: return "supplemental_icon_geometry";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view sph_hud_recognition_stage_name(SphHudRecognitionStage stage) noexcept {
    switch (stage) {
        case SphHudRecognitionStage::not_started: return "not_started";
        case SphHudRecognitionStage::invalid_image: return "invalid_image";
        case SphHudRecognitionStage::layout_not_found: return "layout_not_found";
        case SphHudRecognitionStage::optic_layout: return "optic_layout";
        case SphHudRecognitionStage::supplemental_layout: return "supplemental_layout";
        case SphHudRecognitionStage::instrument_ocr: return "instrument_ocr";
        case SphHudRecognitionStage::completed: return "completed";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view sph_hud_layout_stage_name(SphHudLayoutStage stage) noexcept {
    switch (stage) {
        case SphHudLayoutStage::not_started: return "not_started";
        case SphHudLayoutStage::invalid_image: return "invalid_image";
        case SphHudLayoutStage::reticle: return "reticle";
        case SphHudLayoutStage::left_ticks: return "left_ticks";
        case SphHudLayoutStage::right_ticks: return "right_ticks";
        case SphHudLayoutStage::paired_rows: return "paired_rows";
        case SphHudLayoutStage::compass_baseline: return "compass_baseline";
        case SphHudLayoutStage::compass_ticks: return "compass_ticks";
        case SphHudLayoutStage::ready: return "ready";
    }
    return "unknown";
}

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
std::optional<SphHudLayout> extract_sph_hud_layout(const Image& image,
                                                SphHudLayoutDiagnostics* diagnostics = nullptr);

// Bounded numeric-word candidates from separate dark-neutral pixel components
// inside the supplied compass strip. Bounds are exact original-pixel unions,
// without OCR padding, inferred values or tick anchoring. These candidates do
// not verify a word: the provider must still supply actual unrotated OCR bounds.
// More than twelve candidates or an invalid/oversized strip returns no regions.
std::vector<ImageRect> extract_sph_hud_compass_word_regions(const Image& image, ImageRect strip);
std::vector<SphHudCompassWord> extract_sph_hud_compass_words(const Image& image, ImageRect strip);

// Isolates one independently located side-scale text row using two bright-
// neutral masks. Returns only an original-pixel union, never a numeric value.
// Clipped, competing, merged or unstable component geometry stays unavailable.
std::optional<ImageRect> extract_sph_hud_numeric_word(const Image& image, ImageRect region);

// Admission checks only supplied model scores and line count. Complete text,
// units, pass disagreements and scale consistency remain parser decisions.
[[nodiscard]] bool sph_hud_numeric_scores_qualified(const OcrResult& reading) noexcept;

// Reload animation may hide the reticle while leaving the single-shell icon
// and its counter intact. This path observes that separate channel only.
std::optional<SphHudLayout> extract_sph_hud_supplemental_layout(const Image& image);
SphHudReading read_sph_hud_supplemental(const SphHudFrame& frame);

// Delay measured from OCR completion. A valid sight budgets the current
// capture age and the next recognition against the one-second visual limit,
// with a 100-ms reserve where possible. A fresh accepted read that cannot
// sustain continuous indication retries after a bounded 150-ms idle interval.
// Refused or stale readings retain the slower bounded retry; no delay is zero.
[[nodiscard]] int sph_hud_poll_delay_ms(std::uint64_t processing_ms,
                                      std::uint64_t capture_age_ms,
                                      bool full_sight_accepted) noexcept;

}  // namespace wardogs
