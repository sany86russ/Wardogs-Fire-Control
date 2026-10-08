#include "wardogs/firing_analysis.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace wardogs {
namespace {

bool known_weapon(AnalysisWeapon weapon) {
    return weapon == AnalysisWeapon::l81 || weapon == AnalysisWeapon::sph2;
}

bool known_arc(Arc arc) {
    return arc == Arc::low || arc == Arc::high;
}

bool has_text(std::string_view value) {
    return value.find_first_not_of(" \t\r\n") != std::string_view::npos;
}

void require_finite(double value, const char* message) {
    if (!std::isfinite(value)) throw std::invalid_argument(message);
}

void require_positive(double value, const char* message) {
    if (!std::isfinite(value) || value <= 0.0)
        throw std::invalid_argument(message);
}

void require_nonnegative(double value, const char* message) {
    if (!std::isfinite(value) || value < 0.0)
        throw std::invalid_argument(message);
}

std::optional<double> terrain_value(const HeightLookup& lookup, Point point) {
    const auto value = lookup(point);
    if (value)
        require_finite(*value, "Данные рельефа содержат неконечную высоту");
    return value;
}

void validate_request(const FiringAnalysisRequest& request) {
    if (!known_weapon(request.weapon) || !known_arc(request.arc))
        throw std::invalid_argument("Неизвестное орудие или траектория анализа");
    if (request.height_delta_m)
        require_finite(*request.height_delta_m, "Разница высот должна быть конечной");
    require_nonnegative(request.muzzle_height_above_ground_m,
                        "Высота ствола над землёй должна быть конечной и неотрицательной");
    require_nonnegative(request.target_height_above_ground_m,
                        "Высота цели над землёй должна быть конечной и неотрицательной");
    require_positive(request.sample_step_m,
                     "Шаг проверки траектории должен быть конечным и положительным");
    require_nonnegative(request.clearance_margin_m,
                        "Запас над рельефом должен быть конечным и неотрицательным");
    if (request.gravity_mps2) {
        if (request.weapon != AnalysisWeapon::sph2)
            throw std::invalid_argument("Параметр g для SPH-2 нельзя применить к L81");
        require_positive(*request.gravity_mps2,
                         "Предполагаемая гравитация должна быть конечной и положительной");
    }
    if (request.vacuum_profile) {
        if (request.weapon != AnalysisWeapon::l81)
            throw std::invalid_argument("Профиль скорости L81 нельзя применить к SPH-2");
        require_positive(request.vacuum_profile->speed_mps,
                         "Предполагаемая скорость должна быть конечной и положительной");
        require_positive(request.vacuum_profile->gravity_mps2,
                         "Предполагаемая гравитация должна быть конечной и положительной");
        if (!has_text(request.vacuum_profile->source))
            throw std::invalid_argument("Укажите источник предположений скорости и гравитации");
    }
    if (request.flight_profile) validate_flight_time_profile(*request.flight_profile);
}

std::optional<FlightTimeEstimate> measured_time(
    const FiringAnalysisRequest& request, FiringAnalysis& result) {
    if (!request.flight_profile) return std::nullopt;
    const auto& profile = *request.flight_profile;
    if (profile.weapon != request.weapon || profile.arc != request.arc ||
        profile.ammunition_id != request.ammunition_id ||
        profile.game_version != request.game_version) {
        result.flight_profile_status = FlightProfileStatus::identity_mismatch;
        return std::nullopt;
    }
    if (!result.height_delta_m ||
        std::abs(*result.height_delta_m - profile.height_delta_m) >
            flight_profile_height_tolerance_m) {
        result.flight_profile_status = FlightProfileStatus::height_mismatch;
        return std::nullopt;
    }
    double distance = result.distance_m;
    // Accommodate binary subtraction of identical decimal map coordinates,
    // without admitting a material extrapolation outside measured coverage.
    for (const auto& sample : profile.samples) {
        const double roundoff = 64.0 * std::numeric_limits<double>::epsilon() *
                                std::max(distance, sample.distance_m);
        if (std::abs(distance - sample.distance_m) <= roundoff) {
            result.flight_profile_status = FlightProfileStatus::matched;
            return FlightTimeEstimate{sample.seconds, sample.uncertainty_s,
                {EstimateBasis::user_measurement, profile.source, profile.game_version}};
        }
    }
    if (distance < profile.samples.front().distance_m ||
        distance > profile.samples.back().distance_m) {
        result.flight_profile_status = FlightProfileStatus::out_of_coverage;
        return std::nullopt;
    }
    const auto right = std::lower_bound(profile.samples.begin(), profile.samples.end(),
        distance, [](const FlightTimeSample& sample, double value) {
            return sample.distance_m < value;
        });
    const auto& left = *std::prev(right);
    const double fraction = (distance - left.distance_m) /
                            (right->distance_m - left.distance_m);
    std::optional<double> uncertainty;
    // Propagate supplied absolute bounds, not an invented statistical CI.
    // A missing bound at either endpoint leaves the interpolated bound unknown.
    if (left.uncertainty_s && right->uncertainty_s)
        uncertainty = std::lerp(*left.uncertainty_s, *right->uncertainty_s, fraction);
    result.flight_profile_status = FlightProfileStatus::matched;
    return FlightTimeEstimate{std::lerp(left.seconds, right->seconds, fraction), uncertainty,
        {EstimateBasis::interpolated_user_measurement, profile.source, profile.game_version}};
}

std::optional<double> vacuum_elevation(double distance, double height,
                                       double maximum_range, Arc arc) {
    const double scaled_distance = distance / maximum_range;
    const double scaled_height = height / maximum_range;
    const double discriminant = 1.0 - scaled_distance * scaled_distance - 2.0 * scaled_height;
    if (!std::isfinite(discriminant) || discriminant < 0.0) return std::nullopt;
    const double root = std::sqrt(discriminant);
    const double tangent = arc == Arc::low
        ? (scaled_distance + 2.0 * scaled_height / scaled_distance) / (1.0 + root)
        : (1.0 + root) / scaled_distance;
    if (!std::isfinite(tangent) || tangent <= 0.0) return std::nullopt;
    return std::atan(tangent);
}

void make_trajectory(const FiringAnalysisRequest& request, FiringAnalysis& result,
                     double elevation, EstimateSource source,
                     std::optional<double> speed, std::optional<double> gravity) {
    const double distance = result.distance_m;
    const double delta = *result.height_delta_m;
    const double coefficient = distance * std::tan(elevation) - delta;
    require_positive(coefficient, "Параметры модели не дают конечную траекторию");
    const double interval_count = std::ceil(distance / request.sample_step_m);
    // Reserve one additional analytic apex sample alongside both endpoints.
    if (!std::isfinite(interval_count) || interval_count > maximum_trajectory_samples - 2)
        throw std::invalid_argument("Шаг проверки создаёт слишком много точек траектории");
    const auto intervals = static_cast<std::size_t>(std::max(1.0, interval_count));
    const double apex_fraction = std::clamp(0.5 + 0.5 * delta / coefficient, 0.0, 1.0);
    const auto height_at = [&](double fraction) {
        // Endpoint-anchored form of z=x*tan(theta)-g*x*x/(2*v*v*cos²(theta)).
        // It preserves the requested endpoints without cancellation there.
        return std::fma(fraction, delta, coefficient * fraction * (1.0 - fraction));
    };
    TrajectoryEstimate trajectory;
    trajectory.source = std::move(source);
    trajectory.elevation_rad = elevation;
    trajectory.apex_distance_m = distance * apex_fraction;
    trajectory.apex_height_above_muzzle_m = height_at(apex_fraction);
    require_finite(trajectory.apex_height_above_muzzle_m,
                   "Параметры модели не дают конечную траекторию");
    trajectory.assumed_speed_mps = speed;
    trajectory.assumed_gravity_mps2 = gravity;
    if (speed) {
        trajectory.model_flight_time_s = distance / (*speed * std::cos(elevation));
        require_positive(*trajectory.model_flight_time_s,
                         "Параметры модели не дают конечное время полёта");
    }
    std::vector<double> fractions;
    fractions.reserve(intervals + 2);
    for (std::size_t index = 0; index <= intervals; ++index)
        fractions.push_back(static_cast<double>(index) / static_cast<double>(intervals));
    if (apex_fraction > 0.0 && apex_fraction < 1.0)
        fractions.push_back(apex_fraction);
    std::sort(fractions.begin(), fractions.end());
    fractions.erase(std::unique(fractions.begin(), fractions.end()), fractions.end());
    trajectory.samples.reserve(fractions.size());
    for (const double fraction : fractions) {
        TrajectorySample sample;
        sample.position = {std::lerp(request.base.x, request.target.x, fraction),
                           std::lerp(request.base.y, request.target.y, fraction)};
        sample.distance_m = distance * fraction;
        sample.height_above_muzzle_m = height_at(fraction);
        require_finite(sample.height_above_muzzle_m,
                       "Параметры модели не дают конечную траекторию");
        if (trajectory.model_flight_time_s)
            sample.model_time_s = *trajectory.model_flight_time_s * fraction;
        trajectory.samples.push_back(sample);
    }
    result.clearance.actual_sample_step_m = distance / static_cast<double>(intervals);
    result.trajectory_status = TrajectoryStatus::estimated;
    result.trajectory = std::move(trajectory);
}

void check_terrain(const FiringAnalysisRequest& request, FiringAnalysis& result,
                   std::optional<double> base_ground, std::optional<double> target_ground) {
    if (!request.terrain) return;
    if (!result.trajectory) {
        result.clearance.status = TerrainClearanceStatus::model_unavailable;
        return;
    }
    auto& clearance = result.clearance;
    auto& samples = result.trajectory->samples;
    for (std::size_t index = 0; index < samples.size(); ++index) {
        auto& sample = samples[index];
        const auto ground = index == 0 ? base_ground
            : index + 1 == samples.size() ? target_ground
            : terrain_value(request.terrain, sample.position);
        if (!ground || !base_ground) {
            ++clearance.samples_missing;
            continue;
        }
        sample.terrain_height_above_muzzle_m =
            *ground - *base_ground - request.muzzle_height_above_ground_m;
        require_finite(*sample.terrain_height_above_muzzle_m,
                       "Высоты рельефа слишком велики для проверки траектории");
        sample.clearance_m = sample.height_above_muzzle_m - *sample.terrain_height_above_muzzle_m;
        require_finite(*sample.clearance_m,
                       "Высоты рельефа слишком велики для проверки траектории");
        ++clearance.samples_checked;
        const bool endpoint = index == 0 || index + 1 == samples.size();
        const double endpoint_roundoff = 64.0 * std::numeric_limits<double>::epsilon() *
            std::max({1.0, std::abs(sample.height_above_muzzle_m),
                      std::abs(*sample.terrain_height_above_muzzle_m)});
        const bool endpoint_below_ground = endpoint && *sample.clearance_m < -endpoint_roundoff;
        if (endpoint && !endpoint_below_ground) continue;
        if (!clearance.minimum_clearance_m || *sample.clearance_m < *clearance.minimum_clearance_m)
            clearance.minimum_clearance_m = sample.clearance_m;
        if ((endpoint_below_ground || *sample.clearance_m <= request.clearance_margin_m) &&
            !clearance.first_blocked_distance_m) {
            clearance.first_blocked_distance_m = sample.distance_m;
            clearance.first_blocked_position = sample.position;
        }
    }
    clearance.status = clearance.first_blocked_distance_m ? TerrainClearanceStatus::blocked
        : clearance.samples_missing ? TerrainClearanceStatus::incomplete
        : clearance.minimum_clearance_m ? TerrainClearanceStatus::clear_at_samples
        : TerrainClearanceStatus::incomplete;
}

}  // namespace

void validate_flight_time_profile(const FlightTimeProfile& profile) {
    if (!known_weapon(profile.weapon) || !known_arc(profile.arc))
        throw std::invalid_argument("Неизвестное орудие или траектория профиля времени");
    require_finite(profile.height_delta_m, "Разница высот профиля должна быть конечной");
    if (!has_text(profile.ammunition_id) || !has_text(profile.game_version) || !has_text(profile.source))
        throw std::invalid_argument("Профиль времени требует боеприпас, версию игры и источник измерений");
    if (profile.samples.empty() || profile.samples.size() > maximum_flight_time_samples)
        throw std::invalid_argument("Профиль времени должен содержать от 1 до 256 измерений");
    double previous_distance{};
    for (const auto& sample : profile.samples) {
        require_positive(sample.distance_m, "Дальность измерения должна быть конечной и положительной");
        require_positive(sample.seconds, "Время измерения должно быть конечным и положительным");
        if (sample.distance_m <= previous_distance)
            throw std::invalid_argument("Дальности профиля должны строго возрастать без повторений");
        if (sample.uncertainty_s) {
            require_nonnegative(*sample.uncertainty_s,
                                "Погрешность времени должна быть конечной и неотрицательной");
            if (*sample.uncertainty_s > sample.seconds)
                throw std::invalid_argument("Погрешность времени не должна превышать само время");
        }
        previous_distance = sample.distance_m;
    }
}

FiringAnalysis analyze_firing(const FiringAnalysisRequest& request) {
    validate_request(request);
    const auto shot = calculate_shot(request.base, request.target);
    FiringAnalysis result;
    result.distance_m = shot.distance * 100.0;
    result.bearing_deg = shot.angle;
    std::optional<double> base_ground;
    std::optional<double> target_ground;
    if (request.terrain) {
        base_ground = terrain_value(request.terrain, request.base);
        target_ground = terrain_value(request.terrain, request.target);
    }
    if (request.height_delta_m) {
        result.height_source = HeightSource::explicit_delta;
        result.height_delta_m = *request.height_delta_m;
    } else if (request.terrain) {
        if (base_ground && target_ground) {
            result.height_source = HeightSource::terrain;
            result.height_delta_m = (*target_ground + request.target_height_above_ground_m) -
                                    (*base_ground + request.muzzle_height_above_ground_m);
        } else {
            result.height_source = HeightSource::unavailable;
        }
    } else {
        result.height_source = HeightSource::assumed_level;
        result.height_delta_m = request.target_height_above_ground_m - request.muzzle_height_above_ground_m;
    }
    if (result.height_delta_m)
        require_finite(*result.height_delta_m, "Разница высот слишком велика для расчёта");
    if (request.weapon == AnalysisWeapon::sph2 && !result.height_delta_m) {
        result.status = FiringAnalysisStatus::height_unavailable;
        result.trajectory_status = TrajectoryStatus::height_unavailable;
        (void)measured_time(request, result);
        check_terrain(request, result, base_ground, target_ground);
        return result;
    }
    try {
        result.nominal_mil = request.weapon == AnalysisWeapon::l81
            ? mortar_mil_for_distance(result.distance_m)
            : sph2_mil_for_trajectory(result.distance_m, *result.height_delta_m, request.arc);
    } catch (const std::invalid_argument&) {
        result.status = request.weapon == AnalysisWeapon::l81 || *result.height_delta_m == 0.0
            ? FiringAnalysisStatus::unsupported_range : FiringAnalysisStatus::unreachable;
        result.trajectory_status = result.status == FiringAnalysisStatus::unreachable
            ? TrajectoryStatus::unreachable : TrajectoryStatus::unavailable;
        check_terrain(request, result, base_ground, target_ground);
        return result;
    }
    if (request.weapon == AnalysisWeapon::sph2) {
        // Reuse the retained geometry rather than treating game MIL as radians.
        const auto direction = impact_direction(request.base, request.target, request.arc,
                                                *result.height_delta_m);
        const double elevation = std::atan2(direction[2], std::hypot(direction[0], direction[1]));
        std::optional<double> speed;
        EstimateSource source{EstimateBasis::retained_geometric_model,
            "Сохранённая геометрическая модель SPH-2: R = 2629 м", request.game_version};
        if (request.gravity_mps2) {
            // R=v²/g fixes the spatial parabola, but not its time scale.
            // x=U0*t and z=V0*t-g*t²/2: NASA Glenn ballistic flight equations.
            // https://www1.grc.nasa.gov/beginners-guide-to-aeronautics/ballistic-flight-equations/
            speed = std::sqrt(sph2_maximum_range_m) * std::sqrt(*request.gravity_mps2);
            require_positive(*speed, "Параметры модели не дают конечную скорость");
            source.basis = EstimateBasis::assumed_vacuum_model;
            source.source = "Предположение пользователя: SPH-2, v²/g = 2629 м";
        }
        make_trajectory(request, result, elevation, std::move(source), speed, request.gravity_mps2);
    } else if (request.vacuum_profile) {
        if (!result.height_delta_m) {
            result.trajectory_status = TrajectoryStatus::height_unavailable;
        } else {
            const auto& profile = *request.vacuum_profile;
            const long double maximum_range = static_cast<long double>(profile.speed_mps) *
                profile.speed_mps / profile.gravity_mps2;
            const double range = static_cast<double>(maximum_range);
            require_positive(range, "Параметры модели не дают конечную дальность");
            const auto elevation = vacuum_elevation(result.distance_m, *result.height_delta_m,
                                                     range, request.arc);
            if (!elevation) {
                result.trajectory_status = TrajectoryStatus::unreachable;
            } else {
                make_trajectory(request, result, *elevation,
                    {EstimateBasis::assumed_vacuum_model, profile.source, request.game_version},
                    profile.speed_mps, profile.gravity_mps2);
            }
        }
    }
    // Prefer declared observations for the total time. A separate model time
    // remains in the trajectory; measurements do not validate its geometry.
    result.flight_time = measured_time(request, result);
    if (!result.flight_time && result.trajectory && result.trajectory->model_flight_time_s)
        result.flight_time = FlightTimeEstimate{*result.trajectory->model_flight_time_s,
            std::nullopt, result.trajectory->source};
    check_terrain(request, result, base_ground, target_ground);
    return result;
}

Point offset_target(Point base, Point target, double lateral_m, double longitudinal_m) {
    require_finite(lateral_m, "Боковая поправка должна быть конечной");
    require_finite(longitudinal_m, "Поправка дальности должна быть конечной");
    const auto shot = calculate_shot(base, target);
    require_positive(shot.distance, "Для поправки задайте цель отдельно от орудия");
    const double along_x = shot.dx / shot.distance;
    const double along_y = shot.dy / shot.distance;
    const double lateral = lateral_m / 100.0;
    const double longitudinal = longitudinal_m / 100.0;
    const Point adjusted{
        std::fma(along_x, longitudinal, std::fma(along_y, lateral, target.x)),
        std::fma(along_y, longitudinal, std::fma(-along_x, lateral, target.y))};
    if (!std::isfinite(adjusted.x) || !std::isfinite(adjusted.y))
        throw std::invalid_argument("Поправки слишком велики для координат цели");
    return adjusted;
}

}  // namespace wardogs
