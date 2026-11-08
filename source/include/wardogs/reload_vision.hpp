#pragma once

#include "wardogs/ocr.hpp"

#include <array>
#include <optional>

namespace wardogs {

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

// Reads exactly four directional arrow shapes in one evenly spaced row.
// No rectangle, circular timer, weapon silhouette or background is required.
// Ambiguous, clipped and non-directional glyphs are rejected. This function
// reads a single image; key scheduling and four-stage timing are separate.
std::optional<ReloadObservation> detect_reload_sequence(const Image& image);

}  // namespace wardogs
