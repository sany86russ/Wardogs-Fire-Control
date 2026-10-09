#pragma once

#include "wardogs/continuous_calibration.hpp"
#include <cmath>
#include <stdexcept>

namespace wardogs {

// An observed miss is a displacement in metres. Aim changes are separate:
// clockwise azimuth degrees, game MIL, and the selected table's range delta.
// Neither an observation nor Alt+I establishes when a shot was fired.
struct ImpactFeedback {
    Point target;
    Arc arc;
    double miss_m;
    double right_m;
    double far_m;
    double bearing_change_deg;
    double mil_change;
    double table_range_change_m;
};

inline ImpactFeedback impact_feedback(Point base, const FiringSnapshot& firing,
                                      Point impact, const CorrectedSolution& next) {
    const auto shot = calculate_shot(base, firing.target);
    const double range = shot.distance;
    if (!(range > 0.0) || next.arc != firing.arc || !std::isfinite(firing.bearing_deg) ||
        !std::isfinite(firing.mil) || !std::isfinite(next.bearing_deg) ||
        !std::isfinite(next.mil) || !std::isfinite(impact.x) || !std::isfinite(impact.y))
        throw std::invalid_argument("Некорректные данные пристрелки");
    const double ux = (firing.target.x - base.x) / range;
    const double uy = (firing.target.y - base.y) / range;
    const double dx = (impact.x - firing.target.x) * 100.0;
    const double dy = (impact.y - firing.target.y) * 100.0;
    ImpactFeedback result{firing.target, firing.arc, std::hypot(dx, dy),
        dx * uy - dy * ux, dx * ux + dy * uy,
        std::remainder(next.bearing_deg - firing.bearing_deg, 360.0),
        next.mil - firing.mil,
        sph2_distance_for_mil(next.mil, firing.arc) -
            sph2_distance_for_mil(firing.mil, firing.arc)};
    if (!std::isfinite(result.miss_m) || !std::isfinite(result.right_m) ||
        !std::isfinite(result.far_m) || !std::isfinite(result.bearing_change_deg) ||
        !std::isfinite(result.mil_change) || !std::isfinite(result.table_range_change_m))
        throw std::invalid_argument("Некорректные данные пристрелки");
    return result;
}

} // namespace wardogs
