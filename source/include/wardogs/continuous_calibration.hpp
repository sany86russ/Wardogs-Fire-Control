#pragma once

#include "wardogs/vehicle_ballistics.hpp"

#include <cstddef>
#include <span>
#include <utility>
#include <vector>

namespace wardogs {

// Retain the complete session without allowing indefinite quadratic refits.
inline constexpr std::size_t maximum_continuous_observations = 256;
inline constexpr double local_correction_radius_m = 50.0;
// Explicit project limits for identifying an optional local landing and its
// recorded firing settings. These are neither game dispersion probabilities
// nor fixed angle/MIL caps: the smaller geometric bound applies to each shot.
inline constexpr double maximum_local_impact_miss_m = 400.0;
inline constexpr double maximum_local_impact_miss_fraction = 0.20;
// Allow table nonlinearity while bounding accumulated steering relative to
// the frozen direct solution for every result, including nearby queries.
// This reserve is application policy as well.
inline constexpr double maximum_local_command_displacement_multiplier = 2.0;

enum class ContinuousCorrectionMode {
    platform_refinement,
    // Retain the given baseline and learn only same-arc offsets near the
    // observed target. One landing cannot identify a global platform rotation.
    local_only
};

// Captures the firing settings used for one observed landing. Callers may
// supply actual settings if they differ from the displayed solution.
struct FiringSnapshot {
    Point target;
    Arc arc{Arc::low};
    double bearing_deg{};
    double mil{};
    double target_height_delta_m{};
};

struct ObservationAssessment {
    // Relative, uncalibrated model-consistency score in [0, 1].
    // Local-only mode reports the lesser of the independently weighted
    // bearing/MIL scores; this score does not scale both corrections to zero.
    double confidence{};
    std::size_t observation_count{};
    // Evidence near this requested target, on this arc only. History outside
    // the local kernel is not counted as corroboration for a new target.
    std::size_t local_observation_count{};
    std::size_t accepted_observation_count{};
    bool provisional{true};
    bool scatter_available{};
    // Weighted RMS spread of the retained absolute correction estimates,
    // projected to metres at this range using the sight table's local slope.
    // This describes observed consistency, not a hit radius or probability.
    double empirical_scatter_m{};
};

class ContinuousCalibration {
public:
    ContinuousCalibration(
        Point base, PlatformCalibration baseline,
        ContinuousCorrectionMode mode = ContinuousCorrectionMode::platform_refinement);

    [[nodiscard]] CorrectedSolution solution(
        Point target, Arc arc, double height_delta_m = 0.0) const;
    [[nodiscard]] FiringSnapshot firing_snapshot(
        Point target, Arc arc, double height_delta_m = 0.0) const;
    ObservationAssessment add_landing(
        FiringSnapshot firing, Point impact,
        double impact_height_delta_m = 0.0);
    [[nodiscard]] ObservationAssessment assess(
        Point target, Arc arc, double height_delta_m = 0.0) const;
    void clear();
    [[nodiscard]] std::size_t sample_count() const noexcept;
    [[nodiscard]] const PlatformCalibration& global_calibration() const noexcept;
    [[nodiscard]] double global_rotation_adjustment_deg() const noexcept;

private:
    struct WeightedNeighbor {
        std::size_t index;
        double weight;
    };
    struct Sample {
        Point target;
        Arc arc;
        double target_bearing_deg;
        double target_range_m;
        double target_height_delta_m;
        double bearing_offset_deg;
        double mil_offset;
        double firing_bearing_deg;
        double firing_mil;
        Point impact;
        double impact_height_delta_m;
        Vector3 local_direction;
        Vector3 world_direction;
    };
    struct LocalEstimate {
        std::pair<double, double> correction;
        ObservationAssessment assessment;
    };

    [[nodiscard]] double confidence(
        std::size_t index, std::span<const WeightedNeighbor> neighbors) const;
    [[nodiscard]] std::pair<double, double> local_confidence(
        std::size_t index, std::span<const WeightedNeighbor> neighbors) const;
    [[nodiscard]] double proximity(const Sample& sample, Point target, double bearing_deg,
                                   double range_m, double height_delta_m) const;
    [[nodiscard]] std::pair<double, double> correction(
        Point target, Arc arc, double height_delta_m) const;
    [[nodiscard]] LocalEstimate local_estimate(
        Point target, Arc arc, double height_delta_m) const;
    void refresh_offsets();
    void refit_global_calibration(std::span<const std::size_t> target_groups);

    Point base_;
    PlatformCalibration baseline_;
    PlatformCalibration active_;
    ContinuousCorrectionMode mode_;
    std::vector<Sample> samples_;
    std::vector<double> confidence_scores_;
    std::vector<std::pair<double, double>> local_confidence_scores_;
};

}  // namespace wardogs
