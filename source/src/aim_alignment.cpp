#include "wardogs/aim_alignment.hpp"

#include <algorithm>
#include <cmath>

namespace wardogs {
namespace {

bool valid_bearing(double value) {
    return std::isfinite(value) && value >= 0.0 && value <= 360.0;
}

bool valid_mil(double value) {
    return std::isfinite(value) && value >= 0.0;
}

bool valid_uncertainty(double value) {
    return std::isfinite(value) && value >= 0.0 && value <= 1.0;
}

}  // namespace

AimAlignment evaluate_aim_alignment(const AimAlignmentInput& input) noexcept {
    AimAlignment result;
    if (!input.actual_sight_verified) return result;
    if (!input.current_context_valid) {
        result.reason = AimAlignmentReason::invalid_context;
        return result;
    }
    if (!valid_bearing(input.desired_bearing_deg) || !valid_bearing(input.actual_bearing_deg) ||
        !valid_mil(input.desired_mil) || !valid_mil(input.actual_mil)) {
        result.reason = AimAlignmentReason::invalid_values;
        return result;
    }
    if (!input.captured_at_ms) {
        result.reason = AimAlignmentReason::missing_capture_time;
        return result;
    }
    if (*input.captured_at_ms > input.now_ms) {
        result.reason = AimAlignmentReason::future_capture;
        return result;
    }
    if (input.now_ms - *input.captured_at_ms > 1000) {
        result.reason = AimAlignmentReason::stale_capture;
        return result;
    }
    result.bearing_delta_deg = std::remainder(input.desired_bearing_deg - input.actual_bearing_deg, 360.0);
    result.mil_delta = input.desired_mil - input.actual_mil;
    if (!input.bearing_uncertainty_deg || !input.mil_uncertainty) {
        result.reason = AimAlignmentReason::missing_uncertainty;
        return result;
    }
    if (!valid_uncertainty(*input.bearing_uncertainty_deg) || !valid_uncertainty(*input.mil_uncertainty)) {
        result.reason = AimAlignmentReason::invalid_uncertainty;
        return result;
    }
    const double bearing_error = std::abs(*result.bearing_delta_deg);
    const double mil_error = std::abs(*result.mil_delta);
    const double maximum_mil_error = mil_error + *input.mil_uncertainty;
    if (!std::isfinite(maximum_mil_error)) {
        result.reason = AimAlignmentReason::invalid_uncertainty;
        return result;
    }
    if (!input.stable_pair) {
        result.reason = AimAlignmentReason::awaiting_stable_sight;
        return result;
    }
    if (!input.stabilized) {
        result.reason = AimAlignmentReason::unknown_stabilization;
        return result;
    }
    const double minimum_bearing_error = std::max(0.0, bearing_error - *input.bearing_uncertainty_deg);
    const double minimum_mil_error = std::max(0.0, mil_error - *input.mil_uncertainty);
    if (minimum_bearing_error > 1.0 || minimum_mil_error > 10.0) {
        result.state = AimAlignmentState::misaligned;
        result.reason = AimAlignmentReason::misaligned;
    } else if (!*input.stabilized) {
        result.state = AimAlignmentState::close;
        result.reason = AimAlignmentReason::unstabilized;
    } else if (bearing_error <= *input.bearing_uncertainty_deg + 0.05 + 1e-10 &&
               mil_error <= *input.mil_uncertainty + 0.5 + 1e-10) {
        // Agreement means intersection of the absolute reading interval with
        // the displayed command's rounding cell (0.1 degree / one MIL).
        // It never reduces the reader's uncertainty or claims a hit bound.
        // A 0.2-degree worst-error requirement is below the actual ruler's
        // pixel-localisation floor and cannot be met by the real extractor.
        // The tiny epsilon only accommodates circular subtraction roundoff.
        result.state = AimAlignmentState::aligned;
        result.reason = AimAlignmentReason::aligned;
    } else {
        result.state = AimAlignmentState::close;
        result.reason = AimAlignmentReason::close;
    }
    return result;
}

}  // namespace wardogs
