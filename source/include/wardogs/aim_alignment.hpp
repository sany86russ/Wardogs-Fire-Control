#pragma once

#include <cstdint>
#include <optional>

namespace wardogs {

// Compatibility of an absolute reading interval with the displayed numeric
// command's rounding cell (0.1 degree / one MIL), never a hit prediction.
enum class AimAlignmentState { unknown, aligned, close, misaligned };

enum class AimAlignmentReason {
    unverified_sight,
    invalid_context,
    invalid_values,
    missing_capture_time,
    future_capture,
    stale_capture,
    missing_uncertainty,
    invalid_uncertainty,
    awaiting_stable_sight,
    unknown_stabilization,
    unstabilized,
    aligned,
    close,
    misaligned
};

struct AimAlignmentInput {
    double desired_bearing_deg{};
    double desired_mil{};
    double actual_bearing_deg{};
    double actual_mil{};
    std::optional<double> bearing_uncertainty_deg;
    std::optional<double> mil_uncertainty;
    std::optional<std::uint64_t> captured_at_ms;
    std::uint64_t now_ms{};
    bool stable_pair{};
    std::optional<bool> stabilized;
    bool current_context_valid{};
    bool actual_sight_verified{};
};

struct AimAlignment {
    AimAlignmentState state{AimAlignmentState::unknown};
    AimAlignmentReason reason{AimAlignmentReason::unverified_sight};
    // Desired minus actual: the numeric adjustment towards the command.
    // Bearing is the shortest circular difference in [-180, 180].
    std::optional<double> bearing_delta_deg;
    std::optional<double> mil_delta;
};

[[nodiscard]] AimAlignment evaluate_aim_alignment(const AimAlignmentInput& input) noexcept;

}  // namespace wardogs
