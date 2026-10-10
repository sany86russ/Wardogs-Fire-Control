#pragma once

#include "wardogs/vehicle_ballistics.hpp"

namespace wardogs {

// The numeric azimuth shown to a player, in [0, 360). Rounding across north
// gives positive 0.0, never 360.0 or negative zero. Rejects non-finite input.
[[nodiscard]] double displayed_bearing_deg(double bearing_deg);

// Exact settings shown by the SPH-2 firing cards. Retain the full solution
// internally, but use these settings when recording a displayed command or
// comparing the next command with the one used for an observed landing.
struct DisplayedFiringCommand {
    Arc arc{Arc::low};
    double bearing_deg{};
    double mil{};
    double table_distance_m{};  // Sight-table range at the integer MIL setting.

    bool operator==(const DisplayedFiringCommand&) const = default;
};

// Azimuth rounds to 0.1 degree; game MIL rounds to an integer using std::round.
// Validates both the original and rounded MIL against the selected table so
// rounding cannot silently make an unsupported solution appear valid.
[[nodiscard]] DisplayedFiringCommand displayed_firing_command(
    double bearing_deg, double mil, Arc arc);
[[nodiscard]] DisplayedFiringCommand displayed_firing_command(
    const CorrectedSolution& solution);

}  // namespace wardogs
