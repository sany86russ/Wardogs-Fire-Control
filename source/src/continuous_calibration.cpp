#include "wardogs/continuous_calibration.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <numbers>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace wardogs {
namespace {

// Preserve the older shared platform-refinement policy independently of the
// optional local-only geometric acceptance policy.
constexpr double maximum_bearing_correction_deg = 3.0;
constexpr double maximum_mil_correction = 50.0;
constexpr double bearing_noise_deg = 0.45;
constexpr double mil_noise = 10.0;
constexpr double target_group_radius_m = 50.0;
constexpr double target_group_evidence_limit = 2.0;

bool same_target_group(Point first, Point second) {
    return std::hypot(first.x - second.x, first.y - second.y) * 100.0 <
        target_group_radius_m;
}

double wrapped_difference(double first, double second) {
    return std::remainder(first - second, 360.0);
}

double clamp_correction(double value, double limit) {
    return std::clamp(value, -limit, limit);
}

void require_finite(double value, const char* label) {
    if (!std::isfinite(value)) throw std::invalid_argument(label);
}

std::pair<double, double> bearing_and_range(Point base, Point point) {
    const double dx = point.x - base.x;
    const double dy = point.y - base.y;
    const double range = std::hypot(dx, dy) * 100.0;
    if (!std::isfinite(range) || range <= 0.0)
        throw std::invalid_argument("Цель должна отличаться от позиции орудия");
    double bearing = std::atan2(dx, dy) * 180.0 / std::numbers::pi;
    if (bearing < 0.0) bearing += 360.0;
    if (bearing == 0.0 || bearing >= 360.0) bearing = 0.0;
    return {bearing, range};
}

double firing_command_separation_m(const FiringSnapshot& firing,
                                  const CorrectedSolution& expected) {
    const double fired_range = sph2_distance_for_mil(firing.mil, firing.arc);
    const double expected_range = sph2_distance_for_mil(expected.mil, firing.arc);
    const double relative_bearing = wrapped_difference(
        firing.bearing_deg, expected.bearing_deg) * std::numbers::pi / 180.0;
    // Measure two sight-equivalent horizontal command vectors, without
    // assuming an unavailable terrain height or measured dispersion model.
    return std::hypot(fired_range * std::sin(relative_bearing),
                      fired_range * std::cos(relative_bearing) - expected_range);
}

double weighted_median(std::vector<std::pair<double, double>> values) {
    std::sort(values.begin(), values.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });
    double total = 0.0;
    for (const auto& value : values) total += value.second;
    double accumulated = 0.0;
    for (const auto& value : values) {
        accumulated += value.second;
        if (accumulated >= total * 0.5) return value.first;
    }
    return values.back().first;
}

double midpoint_weighted_median(std::vector<std::pair<double, double>> values) {
    std::sort(values.begin(), values.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });
    double total = 0.0;
    for (const auto& value : values) total += value.second;
    double accumulated = 0.0;
    for (std::size_t index = 0; index < values.size(); ++index) {
        accumulated += values[index].second;
        // Keep a balanced two-sided series centred between its middle
        // observations; choosing its lower median would bias the robust fit.
        if (index + 1 < values.size() &&
            std::abs(accumulated - total * 0.5) <= total * 1e-12)
            return (values[index].first + values[index + 1].first) / 2.0;
        if (accumulated > total * 0.5) return values[index].first;
    }
    return values.back().first;
}

double effective_observations(double weight_sum, double squared_weight_sum) {
    return squared_weight_sum > 0.0
        ? weight_sum * weight_sum / squared_weight_sum : 0.0;
}

double local_metres_per_mil(const CorrectedSolution& direct) {
    const double peak_mil = sph2_mil_for_distance(sph2_maximum_range_m, direct.arc);
    const double gap = peak_mil - direct.mil;
    // Probe toward the supported peak, or one MIL away when already there.
    // The one-sided difference remains supported at either table boundary.
    const double probe_mil = std::abs(gap) > 1e-10
        ? direct.mil + std::copysign(std::min(1.0, std::abs(gap)), gap)
        : direct.mil + (direct.arc == Arc::low ? -1.0 : 1.0);
    return (sph2_distance_for_mil(probe_mil, direct.arc) -
            direct.reticle_distance_m) / (probe_mil - direct.mil);
}

}  // namespace

ContinuousCalibration::ContinuousCalibration(Point base,
                                             PlatformCalibration baseline,
                                             ContinuousCorrectionMode mode)
    : base_(base), baseline_(baseline), active_(baseline), mode_(mode) {
    require_finite(base.x, "Координаты орудия должны быть конечными числами");
    require_finite(base.y, "Координаты орудия должны быть конечными числами");
    validate_platform_calibration(baseline);
    if (mode_ != ContinuousCorrectionMode::platform_refinement &&
        mode_ != ContinuousCorrectionMode::local_only)
        throw std::invalid_argument("Выберите поддерживаемый режим поправок");
}

std::size_t ContinuousCalibration::sample_count() const noexcept {
    return samples_.size();
}

FiringSnapshot ContinuousCalibration::firing_snapshot(
    Point target, Arc arc, double height_delta_m) const {
    const auto current = solution(target, arc, height_delta_m);
    return {target, arc, current.bearing_deg, current.mil, height_delta_m};
}

void ContinuousCalibration::clear() {
    samples_.clear();
    confidence_scores_.clear();
    local_confidence_scores_.clear();
    active_ = baseline_;
}

const PlatformCalibration& ContinuousCalibration::global_calibration() const noexcept {
    return active_;
}

double ContinuousCalibration::global_rotation_adjustment_deg() const noexcept {
    if (mode_ == ContinuousCorrectionMode::local_only) return 0.0;
    double trace = 0.0;
    for (std::size_t row = 0; row < 3; ++row)
        for (std::size_t column = 0; column < 3; ++column)
            trace += active_.rotation[row][column] *
                     baseline_.rotation[row][column];
    const double cosine = std::clamp((trace - 1.0) / 2.0, -1.0, 1.0);
    return std::acos(cosine) * 180.0 / std::numbers::pi;
}

double ContinuousCalibration::proximity(const Sample& sample, Point target,
                                        double bearing_deg, double range_m,
                                        double height_delta_m) const {
    const double height =
        (sample.target_height_delta_m - height_delta_m) / 120.0;
    if (mode_ == ContinuousCorrectionMode::local_only) {
        const double distance_m = std::hypot(sample.target.x - target.x,
                                             sample.target.y - target.y) * 100.0;
        if (distance_m >= local_correction_radius_m) return 0.0;
        // The compact smoothstep kernel has exact zero influence outside the
        // local target area and fades continuously to that boundary.
        const double remaining = 1.0 - distance_m / local_correction_radius_m;
        return remaining * remaining * (3.0 - 2.0 * remaining) *
            std::exp(-0.5 * height * height);
    }
    const double angle = wrapped_difference(sample.target_bearing_deg,
                                            bearing_deg) / 22.0;
    const double range = (sample.target_range_m - range_m) / 400.0;
    return std::exp(-0.5 * (angle * angle + range * range + height * height));
}

std::pair<double, double> ContinuousCalibration::local_confidence(
    std::size_t index, std::span<const WeightedNeighbor> neighbors) const {
    const auto& current = samples_.at(index);
    // Agreement in one component must not discard useful evidence in the
    // other. These relative consistency weights are not hit probabilities.
    constexpr double isolated_quality = 0.6;
    double support = 0.0;
    double bearing_compatibility_sum = 0.0;
    double mil_compatibility_sum = 0.0;
    const auto compatibility = [](double difference) {
        return 0.08 + 0.92 /
            (1.0 + difference * difference * difference * difference);
    };
    for (const auto& neighbor : neighbors) {
        const auto& peer = samples_[neighbor.index];
        support += neighbor.weight;
        bearing_compatibility_sum += neighbor.weight * compatibility(
            (current.bearing_offset_deg - peer.bearing_offset_deg) /
            (2.0 * bearing_noise_deg));
        mil_compatibility_sum += neighbor.weight * compatibility(
            (current.mil_offset - peer.mil_offset) / (2.0 * mil_noise));
    }
    if (support <= 0.0) return {isolated_quality, isolated_quality};
    // A peer approaching the spatial boundary changes either score smoothly.
    const auto score = [&](double sum) {
        return isolated_quality + std::min(1.0, support) *
            (sum / support - isolated_quality);
    };
    return {score(bearing_compatibility_sum), score(mil_compatibility_sum)};
}

double ContinuousCalibration::confidence(
    std::size_t index, std::span<const WeightedNeighbor> neighbors) const {
    const auto& current = samples_.at(index);
    const double magnitude = std::hypot(
        current.bearing_offset_deg / maximum_bearing_correction_deg,
        current.mil_offset / maximum_mil_correction);
    const double excess = std::max(0.0, magnitude - 1.0);
    const double isolated_quality = 0.6 /
        (1.0 + excess * excess * excess * excess);
    std::vector<std::pair<double, double>> bearings;
    std::vector<std::pair<double, double>> mils;
    bearings.reserve(neighbors.size());
    mils.reserve(neighbors.size());
    for (const auto& neighbor : neighbors) {
        bearings.emplace_back(samples_[neighbor.index].bearing_offset_deg, neighbor.weight);
        mils.emplace_back(samples_[neighbor.index].mil_offset, neighbor.weight);
    }

    // An isolated observation is useful, but a very large correction cannot
    // safely drive the model until another shot corroborates it.
    if (bearings.empty()) {
        return isolated_quality;
    }
    const double bearing_center = weighted_median(std::move(bearings));
    const double mil_center = weighted_median(std::move(mils));
    const double normalized_error = std::hypot(
        (current.bearing_offset_deg - bearing_center) / bearing_noise_deg,
        (current.mil_offset - mil_center) / mil_noise);
    const double scaled = normalized_error / 2.0;
    return 0.08 + 0.92 / (1.0 + scaled * scaled * scaled * scaled);
}

ObservationAssessment ContinuousCalibration::add_landing(
    FiringSnapshot firing, Point impact, double impact_height_delta_m) {
    if (samples_.size() >= maximum_continuous_observations)
        throw std::invalid_argument(
            mode_ == ContinuousCorrectionMode::local_only
                ? "Сохранены 256 попаданий — предел этого сеанса. Сбросьте поправки, чтобы начать новый сеанс; прямой расчёт останется доступен."
                : "Сохранены 256 попаданий — предел этого сеанса. Сбросьте поправки, чтобы продолжить пристрелку; исходная калибровка сохранится");
    for (double coordinate : {base_.x, base_.y, firing.target.x,
                              firing.target.y, impact.x, impact.y})
        require_finite(coordinate, "Координаты должны быть конечными числами");
    require_finite(firing.bearing_deg, "Азимут выстрела должен быть конечным числом");
    require_finite(firing.mil, "Значение MIL должно быть конечным числом");
    require_finite(firing.target_height_delta_m, "Высота цели должна быть конечным числом");
    require_finite(impact_height_delta_m, "Высота попадания должна быть конечным числом");
    firing.bearing_deg = std::fmod(firing.bearing_deg, 360.0);
    if (firing.bearing_deg < 0.0) firing.bearing_deg += 360.0;
    if (firing.bearing_deg == 0.0 || firing.bearing_deg >= 360.0)
        firing.bearing_deg = 0.0;

    // Validate the recorded firing settings and both geometries before
    // changing state. In local-only mode active_ remains the frozen baseline:
    // each corrected firing snapshot therefore estimates the same absolute
    // offset rather than adding the previous correction a second time.
    (void)sph2_distance_for_mil(firing.mil, firing.arc);
    (void)corrected_solution(base_, firing.target, active_, firing.arc,
                             firing.target_height_delta_m);
    const auto [bearing, range] = bearing_and_range(base_, firing.target);
    if (mode_ == ContinuousCorrectionMode::local_only) {
        constexpr double acceptance_roundoff_m = 1e-7;
        const double allowed_miss_m = std::min(
            maximum_local_impact_miss_m, range * maximum_local_impact_miss_fraction);
        const double miss_m = std::hypot(firing.target.x - impact.x,
                                         firing.target.y - impact.y) * 100.0;
        if (!std::isfinite(miss_m) || miss_m > allowed_miss_m + acceptance_roundoff_m) {
            std::ostringstream message;
            message << std::fixed << std::setprecision(1)
                    << "Попадание слишком далеко от выбранной цели: " << miss_m
                    << " м. Для этой цели допустимо до " << allowed_miss_m
                    << " м (не более 400 м и 20% дальности). Проверьте цель и точку попадания.";
            throw std::invalid_argument(message.str());
        }
        const auto expected_firing = solution(
            firing.target, firing.arc, firing.target_height_delta_m);
        const double command_separation_m = firing_command_separation_m(
            firing, expected_firing);
        if (!std::isfinite(command_separation_m) ||
            command_separation_m > allowed_miss_m + acceptance_roundoff_m) {
            std::ostringstream message;
            message << std::fixed << std::setprecision(1)
                    << "Записанные азимут и MIL отличаются от текущего расчёта на "
                    << command_separation_m << " м по вектору прицела. Для этой цели допустимо до "
                    << allowed_miss_m << " м. Проверьте настройки выполненного выстрела.";
            throw std::invalid_argument(message.str());
        }
    }
    const auto impact_solution = required_firing_angles(
        base_, impact, active_, firing.arc, impact_height_delta_m);
    const Sample sample{firing.target, firing.arc, bearing, range,
                        firing.target_height_delta_m,
                        wrapped_difference(firing.bearing_deg,
                                           impact_solution.bearing_deg),
                        firing.mil - impact_solution.mil,
                        firing.bearing_deg, firing.mil, impact,
                        impact_height_delta_m,
                        firing_direction(firing.bearing_deg, firing.mil,
                                         firing.arc),
                        impact_direction(base_, impact, firing.arc,
                                         impact_height_delta_m)};
    // Updating the rotation may invalidate an older boundary observation.
    // Commit the observation, offsets and confidence together, after every
    // refit succeeds; a rejected landing cannot change the live model.
    auto updated = *this;
    updated.samples_.push_back(sample);
    updated.confidence_scores_.resize(updated.samples_.size());
    if (mode_ == ContinuousCorrectionMode::local_only)
        updated.local_confidence_scores_.resize(updated.samples_.size());
    // Target geometry does not change between the confidence/refit passes.
    // Calculate each symmetric proximity once rather than eight times.
    std::vector<std::vector<WeightedNeighbor>> neighborhoods(updated.samples_.size());
    std::vector<std::size_t> target_groups(updated.samples_.size());
    std::vector<std::size_t> group_representatives;
    for (std::size_t index = 0; index < updated.samples_.size(); ++index) {
        const auto& current = updated.samples_[index];
        for (std::size_t other = 0; other < index; ++other) {
            if (updated.samples_[other].arc != current.arc) continue;
            const double weight = updated.proximity(
                updated.samples_[other], current.target, current.target_bearing_deg,
                current.target_range_m, current.target_height_delta_m);
            if (mode_ == ContinuousCorrectionMode::local_only ? weight <= 0.0 : weight < 0.05)
                continue;
            neighborhoods[index].push_back({other, weight});
            neighborhoods[other].push_back({index, weight});
        }
        const auto group = std::find_if(group_representatives.begin(),
            group_representatives.end(), [&](std::size_t representative) {
                const auto& previous = updated.samples_[representative];
                return previous.arc == current.arc &&
                    same_target_group(previous.target, current.target);
            });
        if (group == group_representatives.end()) {
            target_groups[index] = group_representatives.size();
            group_representatives.push_back(index);
        } else {
            target_groups[index] = static_cast<std::size_t>(
                std::distance(group_representatives.begin(), group));
        }
    }
    for (std::size_t index = 0; index < updated.samples_.size(); ++index) {
        if (mode_ == ContinuousCorrectionMode::local_only) {
            const auto scores = updated.local_confidence(index, neighborhoods[index]);
            updated.local_confidence_scores_[index] = scores;
            updated.confidence_scores_[index] = std::min(scores.first, scores.second);
        } else {
            updated.confidence_scores_[index] = updated.confidence(index, neighborhoods[index]);
        }
    }
    if (mode_ == ContinuousCorrectionMode::platform_refinement) {
        for (int pass = 0; pass < 3; ++pass) {
            updated.refit_global_calibration(target_groups);
            for (std::size_t index = 0; index < updated.samples_.size(); ++index)
                updated.confidence_scores_[index] = updated.confidence(index, neighborhoods[index]);
        }
    }
    // A valid impact is not enough: the refit or local compensation may push
    // this shot's target outside the supported sight settings. Reject before
    // committing so the displayed target keeps a usable firing solution.
    (void)updated.solution(firing.target, firing.arc, firing.target_height_delta_m);
    auto assessment = updated.assess(
        firing.target, firing.arc, firing.target_height_delta_m);
    assessment.confidence = updated.confidence_scores_.back();
    active_ = updated.active_;
    samples_.swap(updated.samples_);
    confidence_scores_.swap(updated.confidence_scores_);
    local_confidence_scores_.swap(updated.local_confidence_scores_);
    return assessment;
}

ObservationAssessment ContinuousCalibration::assess(
    Point target, Arc arc, double height_delta_m) const {
    // Match the solution's direct geometry validation even for empty history.
    (void)corrected_solution(base_, target, active_, arc, height_delta_m);
    if (mode_ == ContinuousCorrectionMode::local_only)
        return local_estimate(target, arc, height_delta_m).assessment;
    return {confidence_scores_.empty() ? 0.0 : confidence_scores_.back(),
            samples_.size()};
}

ContinuousCalibration::LocalEstimate ContinuousCalibration::local_estimate(
    Point target, Arc arc, double height_delta_m) const {
    LocalEstimate estimate{{0.0, 0.0}, {}};
    estimate.assessment.observation_count = samples_.size();
    const auto [bearing, range] = bearing_and_range(base_, target);
    std::vector<WeightedNeighbor> local;
    std::vector<std::pair<double, double>> bearings, mils;
    double proximity_sum = 0.0, squared_proximity_sum = 0.0;
    for (std::size_t index = 0; index < samples_.size(); ++index) {
        const auto& sample = samples_[index];
        if (sample.arc != arc) continue;
        const double near = proximity(sample, target, bearing, range, height_delta_m);
        if (near <= 0.0) continue;
        local.push_back({index, near});
        bearings.emplace_back(sample.bearing_offset_deg, near);
        mils.emplace_back(sample.mil_offset, near);
        proximity_sum += near;
        squared_proximity_sum += near * near;
    }
    estimate.assessment.local_observation_count = local.size();
    if (local.empty()) return estimate;

    const double bearing_center = midpoint_weighted_median(bearings);
    const double mil_center = midpoint_weighted_median(mils);
    for (auto& value : bearings) value.first = std::abs(value.first - bearing_center);
    for (auto& value : mils) value.first = std::abs(value.first - mil_center);
    // MAD supplies the observed scale. The existing application consistency
    // scales are lower bounds only; they are not measured game dispersion.
    constexpr double median_absolute_deviation_scale = 1.4826;
    const double bearing_scale = std::max(bearing_noise_deg,
        median_absolute_deviation_scale * midpoint_weighted_median(bearings));
    const double mil_scale = std::max(mil_noise,
        median_absolute_deviation_scale * midpoint_weighted_median(mils));
    // Robust suppression needs three effective observations. A peer whose
    // spatial weight vanishes cannot suddenly change a two-shot estimate.
    const double robust_strength = std::clamp(
        effective_observations(proximity_sum, squared_proximity_sum) - 2.0,
        0.0, 1.0);
    const auto robust_weight = [robust_strength](double difference, double scale) {
        // Retain ordinary scatter fully, then fade continuously to zero at
        // three observed scales. A lone outlier can change the median of a
        // balanced even series; it must not reweight its ordinary two sides.
        const double normalized = std::clamp(
            (std::abs(difference) / scale - 1.0) / 2.0, 0.0, 1.0);
        const double remaining = 1.0 - normalized * normalized;
        const double retained = remaining * remaining;
        return 1.0 + robust_strength * (retained - 1.0);
    };

    double bearing_sum = 0.0, mil_sum = 0.0;
    double bearing_weight_sum = 0.0, mil_weight_sum = 0.0;
    double retained_weight_sum = 0.0, squared_retained_weight_sum = 0.0;
    double retained_bearing_sum = 0.0, retained_mil_sum = 0.0;
    double confidence_sum = 0.0;
    std::vector<double> retained_weights;
    retained_weights.reserve(local.size());
    for (const auto& item : local) {
        const auto& sample = samples_[item.index];
        const auto [bearing_quality, mil_quality] = local_confidence_scores_[item.index];
        const double bearing_retained = robust_weight(
            sample.bearing_offset_deg - bearing_center, bearing_scale);
        const double mil_retained = robust_weight(sample.mil_offset - mil_center, mil_scale);
        // Once robust series evidence is available it supplies the relative
        // weights directly. Otherwise a rejected peer would keep changing the
        // retained shots through their legacy pairwise compatibility scores.
        const double bearing_weight = (bearing_quality + robust_strength *
            (1.0 - bearing_quality)) * item.weight * bearing_retained;
        const double mil_weight = (mil_quality + robust_strength *
            (1.0 - mil_quality)) * item.weight * mil_retained;
        bearing_sum += bearing_weight * item.weight * sample.bearing_offset_deg;
        mil_sum += mil_weight * item.weight * sample.mil_offset;
        bearing_weight_sum += bearing_weight;
        mil_weight_sum += mil_weight;
        // Joint evidence statistics exclude a shot rejected in either
        // component; the valid component still contributes to its own fit.
        const double retained = item.weight * std::min(bearing_retained, mil_retained);
        retained_weights.push_back(retained);
        if (retained > 0.0) ++estimate.assessment.accepted_observation_count;
        retained_weight_sum += retained;
        squared_retained_weight_sum += retained * retained;
        retained_bearing_sum += retained * sample.bearing_offset_deg;
        retained_mil_sum += retained * sample.mil_offset;
        confidence_sum += item.weight * confidence_scores_[item.index];
    }
    const double spatial_strength = std::min(1.0, proximity_sum);
    // Normalize components independently: disagreement must never turn a
    // measured same-sign bias into an unsupported correction toward zero.
    estimate.correction = {
        spatial_strength * bearing_sum / bearing_weight_sum,
        spatial_strength * mil_sum / mil_weight_sum};
    estimate.assessment.confidence = confidence_sum / proximity_sum;
    const double retained_count = effective_observations(
        retained_weight_sum, squared_retained_weight_sum);
    estimate.assessment.provisional = retained_count < 3.0 - 1e-9;
    estimate.assessment.scatter_available = retained_count >= 2.0 - 1e-9;
    if (!estimate.assessment.scatter_available) return estimate;

    const double retained_bearing = retained_bearing_sum / retained_weight_sum;
    const double retained_mil = retained_mil_sum / retained_weight_sum;
    const auto direct = corrected_solution(base_, target, active_, arc, height_delta_m);
    const double metres_per_mil = local_metres_per_mil(direct);
    double squared_spread_sum = 0.0;
    for (std::size_t index = 0; index < local.size(); ++index) {
        const auto& sample = samples_[local[index].index];
        const double lateral = range * std::sin(wrapped_difference(
            sample.bearing_offset_deg, retained_bearing) * std::numbers::pi / 180.0);
        const double longitudinal = metres_per_mil * (sample.mil_offset - retained_mil);
        squared_spread_sum += retained_weights[index] *
            (lateral * lateral + longitudinal * longitudinal);
    }
    estimate.assessment.empirical_scatter_m = std::sqrt(
        squared_spread_sum / retained_weight_sum);
    return estimate;
}

void ContinuousCalibration::refresh_offsets() {
    for (auto& sample : samples_) {
        const auto impact_solution = required_firing_angles(
            base_, sample.impact, active_, sample.arc,
            sample.impact_height_delta_m);
        sample.bearing_offset_deg = wrapped_difference(
            sample.firing_bearing_deg, impact_solution.bearing_deg);
        sample.mil_offset = sample.firing_mil - impact_solution.mil;
    }
}

void ContinuousCalibration::refit_global_calibration(
    std::span<const std::size_t> target_groups) {
    std::vector<DirectionObservation> observations;
    observations.reserve(samples_.size());
    std::vector<double> group_weights(samples_.size());
    for (std::size_t index = 0; index < samples_.size(); ++index)
        group_weights[target_groups[index]] += confidence_scores_[index];
    for (std::size_t index = 0; index < samples_.size(); ++index) {
        // Repeated local evidence still belongs in the history, but it cannot
        // gain unlimited global influence over different firing directions.
        const double group_scale = std::min(1.0,
            target_group_evidence_limit / group_weights[target_groups[index]]);
        observations.push_back({samples_[index].local_direction,
                                samples_[index].world_direction,
                                confidence_scores_[index] * group_scale});
    }
    active_ = refine_platform_calibration(baseline_, observations, 1.5);
    refresh_offsets();
}

std::pair<double, double> ContinuousCalibration::correction(
    Point target, Arc arc, double height_delta_m) const {
    if (samples_.empty()) return {0.0, 0.0};
    if (mode_ == ContinuousCorrectionMode::local_only)
        return local_estimate(target, arc, height_delta_m).correction;
    const auto [bearing, range] = bearing_and_range(base_, target);

    struct Group {
        Point target;
        double bearing_sum{};
        double mil_sum{};
        double weight_sum{};
    };
    std::vector<Group> groups;
    double local_bearing_sum = 0.0;
    double local_mil_sum = 0.0;
    double local_weight_sum = 0.0;
    for (std::size_t index = 0; index < samples_.size(); ++index) {
        const auto& sample = samples_[index];
        if (sample.arc != arc) continue;
        const double quality = confidence_scores_[index];
        const double near = proximity(sample, target, bearing, range, height_delta_m);
        const double local_weight = quality * near;
        local_bearing_sum += local_weight * sample.bearing_offset_deg;
        local_mil_sum += local_weight * sample.mil_offset;
        local_weight_sum += local_weight;

        auto group = std::find_if(groups.begin(), groups.end(), [&](const Group& item) {
            return same_target_group(sample.target, item.target);
        });
        if (group == groups.end()) {
            groups.push_back({sample.target});
            group = std::prev(groups.end());
        }
        group->bearing_sum += quality * sample.bearing_offset_deg;
        group->mil_sum += quality * sample.mil_offset;
        group->weight_sum += quality;
    }
    if (groups.empty()) return {0.0, 0.0};

    // One heavily sampled target contributes one group, not one vote per shot.
    double shared_bearing_sum = 0.0;
    double shared_mil_sum = 0.0;
    double shared_weight_sum = 0.0;
    std::vector<std::pair<double, double>> group_bearings;
    std::vector<std::pair<double, double>> group_mils;
    for (const auto& group : groups) {
        const double group_weight = std::min(1.0,
            group.weight_sum / target_group_evidence_limit);
        group_bearings.emplace_back(group.bearing_sum / group.weight_sum,
                                    group_weight);
        group_mils.emplace_back(group.mil_sum / group.weight_sum,
                                group_weight);
    }
    const double median_bearing = weighted_median(group_bearings);
    const double median_mil = weighted_median(group_mils);
    for (std::size_t index = 0; index < groups.size(); ++index) {
        const double disagreement = std::hypot(
            (group_bearings[index].first - median_bearing) / 0.8,
            (group_mils[index].first - median_mil) / 20.0);
        const double scaled = disagreement / 2.0;
        const double robust_weight = 1.0 /
            (1.0 + scaled * scaled * scaled * scaled);
        const double weight = group_bearings[index].second * robust_weight;
        shared_bearing_sum += weight * group_bearings[index].first;
        shared_mil_sum += weight * group_mils[index].first;
        shared_weight_sum += weight;
    }
    const double shared_strength = std::min(0.55, 0.3 * shared_weight_sum);
    const double shared_bearing = shared_strength *
                                  shared_bearing_sum / shared_weight_sum;
    const double shared_mil = shared_strength * shared_mil_sum / shared_weight_sum;

    // Precise reticle entry makes a plausible observation useful immediately.
    // Confidence still suppresses isolated extreme misses, while a second
    // consistent observation reaches full local strength. Cross-target shared
    // correction above remains deliberately slower.
    constexpr double local_evidence_for_full_strength = 0.8;
    const double local_strength = std::min(
        1.0, local_weight_sum / local_evidence_for_full_strength);
    const double local_bearing = local_weight_sum > 0.0
        ? local_bearing_sum / local_weight_sum : shared_bearing;
    const double local_mil = local_weight_sum > 0.0
        ? local_mil_sum / local_weight_sum : shared_mil;
    return {
        clamp_correction(shared_bearing + local_strength *
                             (local_bearing - shared_bearing),
                         maximum_bearing_correction_deg),
        clamp_correction(shared_mil + local_strength *
                             (local_mil - shared_mil),
                         maximum_mil_correction)};
}

CorrectedSolution ContinuousCalibration::solution(
    Point target, Arc arc, double height_delta_m) const {
    auto result = corrected_solution(base_, target, active_, arc,
                                     height_delta_m);
    const auto [bearing, mil] = correction(target, arc, height_delta_m);
    if (bearing == 0.0 && mil == 0.0) return result;
    const auto direct = result;
    result.bearing_deg = std::fmod(result.bearing_deg + bearing + 360.0, 360.0);
    result.mil += mil;
    result.reticle_distance_m = sph2_distance_for_mil(result.mil, arc);
    if (mode_ == ContinuousCorrectionMode::local_only) {
        constexpr double acceptance_roundoff_m = 1e-7;
        const auto [target_bearing, range] = bearing_and_range(base_, target);
        (void)target_bearing;
        const FiringSnapshot proposed_firing{target, arc,
            result.bearing_deg, result.mil, height_delta_m};
        const double displacement_m = firing_command_separation_m(proposed_firing, direct);
        const double allowed_displacement_m = maximum_local_command_displacement_multiplier *
            std::min(maximum_local_impact_miss_m,
                     range * maximum_local_impact_miss_fraction);
        // A nearby query may cross the geometric budget even when its source
        // observation passed. Validate each result, not only new landings.
        if (!std::isfinite(displacement_m) ||
            displacement_m > allowed_displacement_m + acceptance_roundoff_m) {
            std::ostringstream message;
            message << std::fixed << std::setprecision(1)
                    << "Накопленная поправка отклоняет вектор прицела от прямого расчёта на "
                    << displacement_m << " м. Для этой цели допустимо до "
                    << allowed_displacement_m
                    << " м. Проверьте попадания или сбросьте поправки.";
            throw std::invalid_argument(message.str());
        }
    }
    return result;
}

}  // namespace wardogs
