#pragma once

#include "wardogs/ocr.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace wardogs {

inline constexpr char reload_vision_revision[] = "contour-recovery-v2";

enum class ReloadDirection { up, right, down, left };

struct ReloadArrow {
    ReloadDirection direction{};
    bool accepted{};
    bool operator==(const ReloadArrow&) const = default;
};

struct ReloadObservation {
    // Physical pixels in the supplied image, with a small surrounding margin.
    ImageRect bounds;
    std::array<ReloadArrow, 4> arrows;
    // Shape similarity, not a calibrated probability of correct gameplay.
    double confidence{};
};

enum class ReloadDetectionFailure {
    none, invalid_image, glyph_limit, not_enough_glyphs, no_row, ambiguous_rows
};

struct ReloadVisionDiagnostics {
    ReloadDetectionFailure failure{};
    std::size_t components{}, glyphs{}, rows{}, joined_recovery_rois{};
    bool recovery_budget_exhausted{};
    std::size_t contour_glyphs{}, contour_passes{};
    // At most 512 classified silhouettes, in supplied-image coordinates.
    std::vector<ImageRect> glyph_bounds;
};

const char* reload_detection_failure_name(ReloadDetectionFailure failure) noexcept;

// The detector's exact colour thresholds: 0 background, 1 white, 2 green.
// bgr points to three readable bytes. Used by offline diagnostic masks too.
std::uint8_t reload_pixel_class(const std::uint8_t* bgr) noexcept;

// Reads exactly four directional arrow shapes in one evenly spaced row.
// No rectangle, circular timer, weapon silhouette or background is required.
// Ambiguous, clipped and non-directional glyphs are rejected. This function
// reads a single image; key scheduling and four-stage timing are separate.
std::optional<ReloadObservation> detect_reload_sequence(
    const Image& image, ReloadVisionDiagnostics* diagnostics = nullptr);

}  // namespace wardogs
