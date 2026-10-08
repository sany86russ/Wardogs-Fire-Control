#include "wardogs/continuous_calibration.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void close(double actual, double expected, const char* message, double tolerance) {
    check(std::abs(actual - expected) <= tolerance, message);
}

void rejects(const std::function<void()>& action, const char* message) {
    try {
        action();
        check(false, message);
    } catch (const std::invalid_argument&) {
    }
}

wardogs::Point impact_with_required_offset(
    wardogs::Point base, wardogs::Point target, wardogs::Arc arc,
    double bearing_offset_deg, double mil_offset,
    double fired_bearing_adjustment_deg = 0.0) {
    const auto base_solution = wardogs::corrected_solution(
        base, target, {wardogs::identity_rotation(), 0.0}, arc);
    const double bearing = (base_solution.bearing_deg +
                            fired_bearing_adjustment_deg - bearing_offset_deg) *
                           std::numbers::pi / 180.0;
    const double range = wardogs::sph2_distance_for_mil(
        base_solution.mil - mil_offset, arc);
    return {base.x + std::sin(bearing) * range / 100.0,
            base.y + std::cos(bearing) * range / 100.0};
}

double flat_trajectory_elevation(double distance_m, wardogs::Arc arc) {
    const double principal =
        std::asin(distance_m / wardogs::sph2_maximum_range_m);
    return arc == wardogs::Arc::low ? principal / 2.0
                                     : (std::numbers::pi - principal) / 2.0;
}

wardogs::Vector3 direction_from_bearing_and_elevation(double bearing_deg,
                                                       double elevation_rad) {
    const double bearing = bearing_deg * std::numbers::pi / 180.0;
    const double horizontal = std::cos(elevation_rad);
    return {horizontal * std::sin(bearing),
            horizontal * std::cos(bearing), std::sin(elevation_rad)};
}

wardogs::FiringSnapshot snapshot(wardogs::Point base, wardogs::Point target,
                                 wardogs::Arc arc,
                                 double bearing_adjustment_deg = 0.0) {
    const auto solution = wardogs::corrected_solution(
        base, target, {wardogs::identity_rotation(), 0.0}, arc);
    return {target, arc, solution.bearing_deg + bearing_adjustment_deg,
            solution.mil};
}

wardogs::Point impact_with_rotated_baseline(
    wardogs::Point base, wardogs::Point target, wardogs::Arc arc,
    const wardogs::PlatformCalibration& calibration,
    double bearing_offset_deg, double mil_offset) {
    const auto firing = wardogs::corrected_solution(
        base, target, calibration, arc);
    const double local_flat_range = wardogs::sph2_distance_for_mil(
        firing.mil - mil_offset, arc);
    const auto world = calibration.local_to_world(
        direction_from_bearing_and_elevation(
            firing.bearing_deg - bearing_offset_deg,
            flat_trajectory_elevation(local_flat_range, arc)));
    const double bearing = std::atan2(world[0], world[1]);
    const double world_elevation =
        std::atan2(world[2], std::hypot(world[0], world[1]));
    const double range = wardogs::sph2_maximum_range_m *
                         std::sin(2.0 * world_elevation);
    return {base.x + std::sin(bearing) * range / 100.0,
            base.y + std::cos(bearing) * range / 100.0};
}

wardogs::Point impact_from_firing(
    wardogs::Point base, const wardogs::FiringSnapshot& firing,
    const wardogs::PlatformCalibration& actual_platform) {
    const double flat_range = wardogs::sph2_distance_for_mil(
        firing.mil, firing.arc);
    const auto local = direction_from_bearing_and_elevation(
        firing.bearing_deg,
        flat_trajectory_elevation(flat_range, firing.arc));
    const auto world = actual_platform.local_to_world(local);
    const double bearing = std::atan2(world[0], world[1]);
    const double elevation =
        std::atan2(world[2], std::hypot(world[0], world[1]));
    const double range = wardogs::sph2_maximum_range_m *
                         std::sin(2.0 * elevation);
    return {base.x + std::sin(bearing) * range / 100.0,
            base.y + std::cos(bearing) * range / 100.0};
}

wardogs::Point impact_with_firing_bias(
    wardogs::Point base, const wardogs::FiringSnapshot& firing,
    double bearing_bias_deg, double mil_bias) {
    const double bearing = (firing.bearing_deg - bearing_bias_deg) *
                           std::numbers::pi / 180.0;
    const double range = wardogs::sph2_distance_for_mil(
        firing.mil - mil_bias, firing.arc);
    return {base.x + std::sin(bearing) * range / 100.0,
            base.y + std::cos(bearing) * range / 100.0};
}

double miss_distance_m(wardogs::Point target, wardogs::Point impact) {
    return std::hypot(target.x - impact.x, target.y - impact.y) * 100.0;
}

wardogs::Point impact_with_firing_bias_on_height(
    wardogs::Point base, const wardogs::FiringSnapshot& firing,
    double bearing_bias_deg, double mil_bias, double height_m) {
    // Forward intersection with an elevated plane independently checks the
    // height-aware inverse used to assess an observed landing.
    const long double flat_range = wardogs::sph2_distance_for_mil(
        firing.mil - mil_bias, firing.arc);
    const long double maximum = wardogs::sph2_maximum_range_m;
    const long double principal = std::asin(flat_range / maximum);
    const long double elevation = firing.arc == wardogs::Arc::low
        ? principal / 2 : (std::numbers::pi_v<long double> - principal) / 2;
    const long double cosine = std::cos(elevation);
    const long double center = maximum * cosine * cosine * std::tan(elevation);
    const double range = static_cast<double>(center + std::sqrt(
        center * center - 2 * maximum * cosine * cosine * height_m));
    const double bearing = (firing.bearing_deg - bearing_bias_deg) *
                           std::numbers::pi / 180.0;
    return {base.x + std::sin(bearing) * range / 100.0,
            base.y + std::cos(bearing) * range / 100.0};
}

}  // namespace

int main() {
    using namespace wardogs;
    const Point base{50, 50};
    const PlatformCalibration baseline{identity_rotation(), 0.0};
    ContinuousCalibration model(base, baseline);
    const Point north{50, 68};

    for (Arc arc : {Arc::low, Arc::high}) {
        ContinuousCalibration local(base, baseline, ContinuousCorrectionMode::local_only);
        const auto direct = corrected_solution(base, north, baseline, arc);
        const auto first_firing = local.firing_snapshot(north, arc);
        close(first_firing.bearing_deg, direct.bearing_deg,
              "local-only first shot requires no calibration landings", 1e-10);
        close(first_firing.mil, direct.mil,
              "local-only first shot retains the direct sight table", 1e-10);
        const double mil_bias = arc == Arc::low ? 10.0 : -10.0;
        const auto first_impact = impact_with_firing_bias(base, first_firing, 1.0, mil_bias);
        const auto first_assessment = local.add_landing(first_firing, first_impact);
        const auto first_correction = local.solution(north, arc);
        close(first_correction.bearing_deg, direct.bearing_deg + 1.0,
              "one plausible local landing applies the complete measured bearing offset", 1e-8);
        close(first_correction.mil, direct.mil + mil_bias,
              "one local undershoot has the correct MIL sign on either arc", 1e-8);
        check(first_assessment.observation_count == 1 &&
                  first_assessment.confidence > 0.35 && first_assessment.confidence <= 1.0,
              "a local correction is useful after one optional landing");
        const auto corrected_impact = impact_with_firing_bias(
            base, local.firing_snapshot(north, arc), 1.0, mil_bias);
        check(miss_distance_m(north, corrected_impact) <
                  miss_distance_m(north, first_impact) * 0.35,
              "one local landing reduces a synthetic constant-bias miss");
        check(miss_distance_m(north, corrected_impact) < 1e-6,
              "one optional same-target landing fully removes a modeled constant firing bias on either arc");
        const Point first_nearby{50.25, 68};
        const auto first_nearby_direct = corrected_solution(base, first_nearby, baseline, arc);
        const auto first_nearby_corrected = local.solution(first_nearby, arc);
        close(first_nearby_corrected.bearing_deg, first_nearby_direct.bearing_deg + 0.25,
              "a single landing still fades to quarter-strength 25 metres from its target", 1e-8);
        close(first_nearby_corrected.mil, first_nearby_direct.mil + 0.25 * mil_bias,
              "one full-strength same-target correction does not transfer full MIL influence to a nearby target", 1e-8);

        for (int shot = 0; shot < 12; ++shot) {
            const auto firing = local.firing_snapshot(north, arc);
            local.add_landing(firing, impact_with_firing_bias(base, firing, 1.0, mil_bias));
        }
        const auto converged = local.solution(north, arc);
        close(converged.bearing_deg, direct.bearing_deg + 1.0,
              "repeated corrected local shots cannot accumulate the prior bearing offset", 1e-8);
        close(converged.mil, direct.mil + mil_bias,
              "repeated corrected local shots cannot double-apply the prior MIL offset", 1e-8);
        check(miss_distance_m(north, impact_with_firing_bias(
                  base, local.firing_snapshot(north, arc), 1.0, mil_bias)) < 1e-6,
              "consistent local evidence converges in the synthetic direct-table model");
        check(local.global_calibration().rotation == baseline.rotation &&
                  local.global_rotation_adjustment_deg() == 0.0,
              "local landings never invent a global platform rotation");

        for (Point far : {Point{68, 50}, Point{50.5, 68}, Point{50.500001, 68}}) {
            const auto expected = corrected_solution(base, far, baseline, arc);
            const auto actual = local.solution(far, arc);
            close(actual.bearing_deg, expected.bearing_deg,
                  "a local landing has exact zero bearing influence at and beyond 50 metres", 1e-10);
            close(actual.mil, expected.mil,
                  "a local landing has exact zero MIL influence at and beyond 50 metres", 1e-10);
        }
        const Point nearby{50.25, 68};
        const auto nearby_direct = corrected_solution(base, nearby, baseline, arc);
        const auto nearby_local = local.solution(nearby, arc);
        close(nearby_local.bearing_deg, nearby_direct.bearing_deg + 0.5,
              "repeated evidence preserves the 25-metre spatial fade", 1e-8);
        close(nearby_local.mil, nearby_direct.mil + 0.5 * mil_bias,
              "repeated evidence cannot spread full local MIL compensation across its radius", 1e-8);

        const auto correction_with_peer = [&](double separation_m) {
            ContinuousCalibration neighborhood(base, baseline, ContinuousCorrectionMode::local_only);
            const auto firing = neighborhood.firing_snapshot(north, arc);
            neighborhood.add_landing(firing, impact_with_firing_bias(base, firing, 1.0, 10.0));
            const Point peer{north.x + separation_m / 100.0, north.y};
            const auto peer_firing = neighborhood.firing_snapshot(peer, arc);
            neighborhood.add_landing(peer_firing, impact_with_firing_bias(
                base, peer_firing, -1.0, -10.0));
            const auto corrected = neighborhood.solution(north, arc);
            return std::pair{std::remainder(corrected.bearing_deg - firing.bearing_deg, 360.0),
                             corrected.mil - firing.mil};
        };
        for (double distance : {local_correction_radius_m - 0.001,
                                local_correction_radius_m,
                                local_correction_radius_m + 0.001}) {
            const auto correction = correction_with_peer(distance);
            close(correction.first, 1.0,
                  "a contradictory vanishing local peer cannot abruptly dilute the existing bearing correction", 1e-7);
            close(correction.second, 10.0,
                  "a contradictory vanishing local peer cannot abruptly dilute the existing MIL correction", 1e-6);
        }
        double cutoff_left = 0.0, cutoff_right = local_correction_radius_m;
        for (int iteration = 0; iteration < 60; ++iteration) {
            const double distance = (cutoff_left + cutoff_right) / 2.0;
            const double remaining = 1.0 - distance / local_correction_radius_m;
            const double weight = remaining * remaining * (3.0 - 2.0 * remaining);
            if (weight > 0.05) cutoff_left = distance;
            else cutoff_right = distance;
        }
        const auto before_old_cutoff = correction_with_peer(cutoff_left - 0.001);
        const auto after_old_cutoff = correction_with_peer(cutoff_right + 0.001);
        check(std::abs(before_old_cutoff.first - after_old_cutoff.first) < 1e-4 &&
                  std::abs(before_old_cutoff.second - after_old_cutoff.second) < 1e-3,
              "local peer compatibility has no legacy 0.05-weight discontinuity");
        const Arc other_arc = arc == Arc::low ? Arc::high : Arc::low;
        const auto other_direct = corrected_solution(base, north, baseline, other_arc);
        const auto other_local = local.solution(north, other_arc);
        close(other_local.bearing_deg, other_direct.bearing_deg,
              "a local landing never changes the other trajectory bearing", 1e-10);
        close(other_local.mil, other_direct.mil,
              "a local landing never changes the other trajectory MIL", 1e-10);

        const Point noncardinal_far{66, 64};
        for (Arc untouched_arc : {arc, other_arc}) {
            const auto expected = corrected_solution(base, noncardinal_far, baseline, untouched_arc);
            const auto actual = local.solution(noncardinal_far, untouched_arc);
            check(actual.bearing_deg == expected.bearing_deg && actual.mil == expected.mil &&
                      actual.reticle_distance_m == expected.reticle_distance_m,
                  "an untouched non-cardinal local target retains its exact original guidance");
        }
        ContinuousCalibration noncardinal(base, baseline, ContinuousCorrectionMode::local_only);
        const auto noncardinal_firing = noncardinal.firing_snapshot(noncardinal_far, arc);
        noncardinal.add_landing(noncardinal_firing, impact_with_firing_bias(
            base, noncardinal_firing, 1.0, 10.0));
        const auto untouched_direct = corrected_solution(base, noncardinal_far, baseline, other_arc);
        const auto untouched_other_arc = noncardinal.solution(noncardinal_far, other_arc);
        check(untouched_other_arc.bearing_deg == untouched_direct.bearing_deg &&
                  untouched_other_arc.mil == untouched_direct.mil &&
                  untouched_other_arc.reticle_distance_m == untouched_direct.reticle_distance_m,
              "a non-cardinal target's opposite trajectory remains exactly unchanged by a local landing");

        const auto retained_count = local.sample_count();
        const auto retained = local.solution(north, arc);
        rejects([&] { local.add_landing(local.firing_snapshot(north, arc), {50, 80}); },
                "an impossible local landing is rejected before publication");
        auto invalid_firing = local.firing_snapshot(north, arc);
        invalid_firing.mil = std::numeric_limits<double>::quiet_NaN();
        rejects([&] { local.add_landing(invalid_firing, north); },
                "local correction rejects non-finite captured firing settings");
        const auto after_failure = local.solution(north, arc);
        check(local.sample_count() == retained_count &&
                  after_failure.bearing_deg == retained.bearing_deg &&
                  after_failure.mil == retained.mil &&
                  local.global_calibration().rotation == baseline.rotation,
              "a failed local observation preserves exact history and firing guidance");
        local.clear();
        const auto cleared = local.solution(north, arc);
        check(local.sample_count() == 0 && cleared.bearing_deg == direct.bearing_deg &&
                  cleared.mil == direct.mil,
              "clearing local corrections restores direct guidance without a calibration workflow");
    }

    for (Arc arc : {Arc::low, Arc::high}) {
        for (double bearing : {359.0, 1.0}) {
            for (double bias : {-2.0, 2.0}) {
                const double radians = bearing * std::numbers::pi / 180.0;
                const Point target{base.x + std::sin(radians) * 18.0,
                                   base.y + std::cos(radians) * 18.0};
                ContinuousCalibration wrapped(base, baseline, ContinuousCorrectionMode::local_only);
                const auto firing = wrapped.firing_snapshot(target, arc);
                wrapped.add_landing(firing, impact_with_firing_bias(base, firing, bias, 10.0));
                const auto corrected = wrapped.solution(target, arc);
                close(std::remainder(corrected.bearing_deg - firing.bearing_deg, 360.0),
                      bias, "local bearing corrections wrap correctly across north", 1e-8);
                check(corrected.bearing_deg >= 0.0 && corrected.bearing_deg < 360.0,
                      "local guidance retains a normalized bearing after crossing north");
            }
        }
        for (double height : {-50.0, 100.0}) {
            ContinuousCalibration elevated(base, baseline, ContinuousCorrectionMode::local_only);
            const auto firing = elevated.firing_snapshot(north, arc, height);
            elevated.add_landing(firing, impact_with_firing_bias_on_height(
                base, firing, 1.0, 10.0, height), height);
            const auto corrected = elevated.solution(north, arc, height);
            close(std::remainder(corrected.bearing_deg - firing.bearing_deg, 360.0), 1.0,
                  "a biased elevated landing preserves the local bearing correction sign", 1e-8);
            close(corrected.mil - firing.mil, 10.0,
                  "forward ballistic elevated intersections invert to the expected optional local MIL correction", 1e-8);
            check(elevated.global_calibration().rotation == baseline.rotation,
                  "height-aware local observations cannot create a global platform rotation");
        }
    }

    ContinuousCalibration local_extreme(base, baseline, ContinuousCorrectionMode::local_only);
    const auto extreme_firing = local_extreme.firing_snapshot(north, Arc::low);
    const auto extreme_landing = impact_with_firing_bias(base, extreme_firing, -10.0, -100.0);
    const auto extreme_direct = corrected_solution(base, north, baseline, Arc::low);
    rejects([&] { local_extreme.add_landing(extreme_firing, extreme_landing); },
            "an extreme one-hit correction cannot masquerade as an applied local adjustment");
    const auto extreme_local = local_extreme.solution(north, Arc::low);
    check(local_extreme.sample_count() == 0 &&
              extreme_local.bearing_deg == extreme_direct.bearing_deg &&
              extreme_local.mil == extreme_direct.mil &&
              local_extreme.global_calibration().rotation == baseline.rotation,
          "a rejected extreme local miss leaves exact direct guidance and empty history");
    // Geometric limits replace fixed angle/MIL acceptance caps. Test both
    // the relative range limit and the absolute cap, with exact boundaries.
    for (Arc arc : {Arc::low, Arc::high}) {
        for (double range_m : {1800.0, 2200.0}) {
            const Point target{base.x, base.y + range_m / 100.0};
            const double limit_m = std::min(maximum_local_impact_miss_m,
                range_m * maximum_local_impact_miss_fraction);
            for (double sign : {-1.0, 1.0}) {
                ContinuousCalibration accepted(base, baseline, ContinuousCorrectionMode::local_only);
                const auto firing = accepted.firing_snapshot(target, arc);
                const Point at_limit{target.x + sign * limit_m / 100.0, target.y};
                const auto assessment = accepted.add_landing(firing, at_limit);
                check(accepted.sample_count() == 1 && assessment.confidence == 0.6,
                      "the signed geometric impact boundaries are inclusive on both arcs");
                const auto preserved = accepted.solution(target, arc);
                const auto preserved_count = accepted.sample_count();
                const Point outside{target.x + sign * (limit_m + 0.001) / 100.0, target.y};
                rejects([&] { accepted.add_landing(accepted.firing_snapshot(target, arc), outside); },
                        "the geometric impact limit rejects genuinely larger signed misses");
                const auto after = accepted.solution(target, arc);
                check(accepted.sample_count() == preserved_count &&
                          after.bearing_deg == preserved.bearing_deg && after.mil == preserved.mil,
                      "a geometric impact rejection preserves exact history and guidance");
            }
            ContinuousCalibration mismatched(base, baseline, ContinuousCorrectionMode::local_only);
            auto unrelated = mismatched.firing_snapshot(target, arc);
            unrelated.bearing_deg += 180.0;
            rejects([&] { mismatched.add_landing(unrelated, target); },
                    "finite unrelated firing settings cannot learn a large correction from a near-target impact");
            check(mismatched.sample_count() == 0,
                  "an unrelated finite firing snapshot cannot enter the local observation history");
            for (double extra_m : {0.0, 0.001}) {
                ContinuousCalibration settings(base, baseline, ContinuousCorrectionMode::local_only);
                auto supplied = settings.firing_snapshot(target, arc);
                supplied.bearing_deg += 2.0 * std::asin((limit_m + extra_m) / (2.0 * range_m)) *
                    180.0 / std::numbers::pi;
                if (extra_m == 0.0) {
                    settings.add_landing(supplied, target);
                    check(settings.sample_count() == 1,
                          "the supplied command-vector geometric acceptance boundary is inclusive");
                } else {
                    rejects([&] { settings.add_landing(supplied, target); },
                            "a supplied firing command outside the geometric limit is rejected");
                    check(settings.sample_count() == 0,
                          "an out-of-policy supplied command leaves no local observation");
                }
            }
        }
    }

    // Reproduce SoNiX's accepted OCR coordinates and displayed trajectory.
    // These numbers were independently computed from the sight table and
    // target/impact geometry, rather than chosen to match a clamped result.
    const Point field_base{94.18, 110.34};
    const Point field_target{84.56, 90.44};
    const Point field_impact{83.57, 92.59};
    ContinuousCalibration field(field_base, baseline, ContinuousCorrectionMode::local_only);
    const auto field_firing = field.firing_snapshot(field_target, Arc::high);
    close(field_firing.bearing_deg, 205.799916, "field replay retains independently computed nominal bearing", 1e-6);
    close(field_firing.mil, 972.901447, "field replay retains independently computed nominal MIL", 1e-6);
    close(miss_distance_m(field_target, field_impact), 236.698120,
          "field replay measures the actual 237-metre local miss", 1e-6);
    const auto field_assessment = field.add_landing(field_firing, field_impact);
    const auto field_corrected = field.solution(field_target, Arc::high);
    check(field_assessment.observation_count == 1 && field_assessment.confidence == 0.6,
          "the field miss is accepted after one optional observation without an angular quality penalty");
    close(field_corrected.bearing_deg, 200.731110,
          "the field correction applies its complete measured bearing offset beyond the old 3-degree cap", 1e-6);
    close(field_corrected.mil, 914.238580,
          "the field correction applies its complete measured MIL offset beyond the old 50-MIL cap", 1e-6);
    constexpr double field_bearing_bias = -5.06880663359049;
    constexpr double field_mil_bias = -58.6628666066354;
    const auto field_next_impact = impact_with_firing_bias(
        field_base, field.firing_snapshot(field_target, Arc::high),
        field_bearing_bias, field_mil_bias);
    check(miss_distance_m(field_target, field_next_impact) < 1e-5,
          "the field correction removes an independently modeled constant angular and MIL bias at the same target");
    for (int repeat = 0; repeat < 8; ++repeat) {
        const auto firing = field.firing_snapshot(field_target, Arc::high);
        field.add_landing(firing, impact_with_firing_bias(
            field_base, firing, field_bearing_bias, field_mil_bias));
    }
    const auto field_repeated = field.solution(field_target, Arc::high);
    close(field_repeated.bearing_deg, field_corrected.bearing_deg,
          "repeated already-corrected field shots do not accumulate the bearing correction", 1e-6);
    close(field_repeated.mil, field_corrected.mil,
          "repeated already-corrected field shots do not accumulate the MIL correction", 1e-6);
    check(field.global_calibration().rotation == baseline.rotation,
          "the field observation cannot alter the global direct baseline");
    ContinuousCalibration rounded_field(field_base, baseline, ContinuousCorrectionMode::local_only);
    auto rounded_firing = rounded_field.firing_snapshot(field_target, Arc::high);
    rounded_firing.bearing_deg = 205.8;
    rounded_firing.mil = 973.0;
    rounded_field.add_landing(rounded_firing, field_impact);
    const auto rounded_corrected = rounded_field.solution(field_target, Arc::high);
    close(rounded_corrected.bearing_deg, 200.731193,
          "the physically displayed rounded bearing is accepted and retained in the field correction", 1e-6);
    close(rounded_corrected.mil, 914.337133,
          "the physically displayed rounded MIL is accepted and retained in the field correction", 1e-6);
    check(rounded_field.sample_count() == 1,
          "display rounding cannot fail the supplied-command geometry guard");

    // Replay externally recorded commands and landings, rather than generate
    // impacts from the implementation being tested. This replay checks the
    // estimate, not a counterfactual promise about where real shells would hit.
    const Point latest_base{94.23, 110.39};
    const Point latest_target{84.56, 90.44};
    const std::array<Point, 4> latest_impacts{{
        {83.40, 92.61}, {85.28, 90.23}, {83.84, 91.73}, {85.14, 90.30}}};
    const std::array<std::pair<double, double>, 4> latest_commands{{
        {205.860, 969.997}, {200.374, 913.790},
        {204.031, 946.296}, {200.789, 912.218}}};
    const std::array<double, 4> latest_misses{{
        246.058936, 75.000000, 147.732867, 59.665736}};
    ContinuousCalibration latest(latest_base, baseline, ContinuousCorrectionMode::local_only);
    const auto latest_direct = latest.solution(latest_target, Arc::high);
    double minimum_bearing_offset = std::numeric_limits<double>::infinity();
    double maximum_bearing_offset = -std::numeric_limits<double>::infinity();
    double minimum_mil_offset = std::numeric_limits<double>::infinity();
    double maximum_mil_offset = -std::numeric_limits<double>::infinity();
    double first_corrected_mil = 0.0;
    for (std::size_t shot = 0; shot < latest_impacts.size(); ++shot) {
        const auto [fired_bearing, fired_mil] = latest_commands[shot];
        const auto required = required_firing_angles(
            latest_base, latest_impacts[shot], baseline, Arc::high);
        const double bearing_offset = std::remainder(
            fired_bearing - required.bearing_deg, 360.0);
        const double mil_offset = fired_mil - required.mil;
        minimum_bearing_offset = std::min(minimum_bearing_offset, bearing_offset);
        maximum_bearing_offset = std::max(maximum_bearing_offset, bearing_offset);
        minimum_mil_offset = std::min(minimum_mil_offset, mil_offset);
        maximum_mil_offset = std::max(maximum_mil_offset, mil_offset);
        close(miss_distance_m(latest_target, latest_impacts[shot]), latest_misses[shot],
              "latest field replay retains independently recorded target/impact distances", 1e-6);
        latest.add_landing({latest_target, Arc::high, fired_bearing, fired_mil},
                           latest_impacts[shot]);
        const auto corrected = latest.solution(latest_target, Arc::high);
        const double estimated_bearing_offset = std::remainder(
            corrected.bearing_deg - latest_direct.bearing_deg, 360.0);
        const double estimated_mil_offset = corrected.mil - latest_direct.mil;
        check(estimated_bearing_offset >= minimum_bearing_offset - 1e-8 &&
                  estimated_bearing_offset <= maximum_bearing_offset + 1e-8 &&
                  estimated_mil_offset >= minimum_mil_offset - 1e-8 &&
                  estimated_mil_offset <= maximum_mil_offset + 1e-8,
              "disagreement in same-sign field measurements cannot pull either estimate outside the observed offsets toward zero");
        if (shot == 0) first_corrected_mil = corrected.mil;
        if (shot == 1) {
            check(corrected.mil < first_corrected_mil &&
                      estimated_bearing_offset < -3.5 && estimated_mil_offset < -56.0,
                  "the second field impact retains the established bias instead of undoing it through a low total confidence");
        }
    }
    check(latest.sample_count() == latest_impacts.size() &&
              latest.global_calibration().rotation == baseline.rotation,
          "all four geometrically valid field observations retain the frozen direct baseline");
    const auto latest_other_arc = latest.solution(latest_target, Arc::low);
    const auto latest_other_direct = corrected_solution(latest_base, latest_target, baseline, Arc::low);
    check(latest_other_arc.bearing_deg == latest_other_direct.bearing_deg &&
              latest_other_arc.mil == latest_other_direct.mil,
          "the field replay cannot leak high-arc adjustments into the low arc");

    for (Arc arc : {Arc::low, Arc::high}) {
        ContinuousCalibration noisy(base, baseline, ContinuousCorrectionMode::local_only);
        const auto direct = noisy.solution(north, arc);
        for (int shot = 0; shot < 20; ++shot) {
            const auto firing = noisy.firing_snapshot(north, arc);
            const bool positive = shot % 2 != 0;
            noisy.add_landing(firing, impact_with_firing_bias(
                base, firing, positive ? 1.4 : 0.6, positive ? 15.0 : 5.0));
            const auto corrected = noisy.solution(north, arc);
            const double bearing_offset = std::remainder(
                corrected.bearing_deg - direct.bearing_deg, 360.0);
            const double mil_offset = corrected.mil - direct.mil;
            check(bearing_offset >= 0.6 - 1e-8 && bearing_offset <= 1.4 + 1e-8 &&
                      mil_offset >= 5.0 - 1e-8 && mil_offset <= 15.0 + 1e-8,
                  "bounded noisy same-sign evidence never creates an unsupported zeroward rollback");
        }
        const auto noisy_solution = noisy.solution(north, arc);
        close(std::remainder(noisy_solution.bearing_deg - direct.bearing_deg, 360.0), 1.0,
              "balanced bearing noise retains the measured central offset", 1e-8);
        close(noisy_solution.mil - direct.mil, 10.0,
              "balanced MIL noise retains the measured central offset", 1e-8);

        for (bool bearing_outlier : {false, true}) {
            ContinuousCalibration independent(base, baseline, ContinuousCorrectionMode::local_only);
            for (int shot = 0; shot < 6; ++shot) {
                const auto firing = independent.firing_snapshot(north, arc);
                independent.add_landing(firing, impact_with_firing_bias(base, firing, 1.0, 10.0));
            }
            const auto before = independent.solution(north, arc);
            const auto firing = independent.firing_snapshot(north, arc);
            const auto assessment = independent.add_landing(firing, impact_with_firing_bias(
                base, firing, bearing_outlier ? -4.0 : 1.0, bearing_outlier ? 10.0 : -40.0));
            const auto after = independent.solution(north, arc);
            check(assessment.confidence < 0.3,
                  "a single-component outlier retains a low reported consistency score");
            if (bearing_outlier) {
                close(after.mil, before.mil,
                      "bearing disagreement cannot weaken independently corroborated MIL guidance", 1e-8);
                check(std::abs(std::remainder(after.bearing_deg - before.bearing_deg, 360.0)) < 0.3,
                      "one bearing outlier cannot overturn the supported bearing history");
            } else {
                close(after.bearing_deg, before.bearing_deg,
                      "MIL disagreement cannot weaken independently corroborated bearing guidance", 1e-8);
                check(std::abs(after.mil - before.mil) < 3.0,
                      "one MIL outlier cannot overturn the supported MIL history");
            }
        }
    }

    for (Arc arc : {Arc::low, Arc::high}) {
        ContinuousCalibration inconsistent(base, baseline, ContinuousCorrectionMode::local_only);
        const auto direct = inconsistent.firing_snapshot(north, arc);
        for (int repeat = 0; repeat < 6; ++repeat) {
            const auto firing = inconsistent.firing_snapshot(north, arc);
            inconsistent.add_landing(firing, impact_with_firing_bias(base, firing, 1.0, 10.0));
        }
        const auto learned = inconsistent.solution(north, arc);
        const auto firing = inconsistent.firing_snapshot(north, arc);
        const auto conflicting = inconsistent.add_landing(firing,
            impact_with_firing_bias(base, firing, -4.0, -40.0));
        const auto after = inconsistent.solution(north, arc);
        check(conflicting.confidence < 0.3 &&
                  std::abs(std::remainder(after.bearing_deg - learned.bearing_deg, 360.0)) < 0.3 &&
                  std::abs(after.mil - learned.mil) < 3.0,
              "one geometrically valid contradictory observation cannot overturn consistent local evidence");
        check(inconsistent.global_calibration().rotation == baseline.rotation,
              "contradictory local evidence cannot create a global platform adjustment");
        const auto preserved_count = inconsistent.sample_count();
        const auto preserved_solution = inconsistent.solution(north, arc);
        auto mismatched_firing = inconsistent.firing_snapshot(north, arc);
        mismatched_firing.bearing_deg += 180.0;
        rejects([&] { inconsistent.add_landing(mismatched_firing, north); },
                "an unrelated finite firing snapshot is rejected after local correction exists");
        const auto after_mismatch = inconsistent.solution(north, arc);
        check(inconsistent.sample_count() == preserved_count &&
                  after_mismatch.bearing_deg == preserved_solution.bearing_deg &&
                  after_mismatch.mil == preserved_solution.mil,
              "rejected mismatched firing settings preserve existing local evidence and guidance");

        const Point maximum_target{base.x, base.y + sph2_maximum_range_m / 100.0};
        ContinuousCalibration unsupported(base, baseline, ContinuousCorrectionMode::local_only);
        const auto maximum_firing = unsupported.firing_snapshot(maximum_target, arc);
        const Point undershoot{base.x, maximum_target.y - 1.0};
        rejects([&] { unsupported.add_landing(maximum_firing, undershoot); },
                "a plausible local undershoot still cannot publish an unsupported compensated sight setting");
        const auto retained = unsupported.solution(maximum_target, arc);
        check(unsupported.sample_count() == 0 && retained.mil == maximum_firing.mil &&
                  retained.bearing_deg == maximum_firing.bearing_deg,
              "unsupported output rejection preserves the exact usable direct solution");

        // Deliberately feed a fixed incorrect impact while taking each newly
        // corrected snapshot. This is inconsistent feedback, not constant
        // firing bias; it must never steer indefinitely away from baseline.
        ContinuousCalibration drift(base, baseline, ContinuousCorrectionMode::local_only);
        const auto wrong_point = impact_with_firing_bias(base, direct, -5.0, 0.0);
        bool rejected_drift = false;
        for (int repeat = 0; repeat < 200 && !rejected_drift; ++repeat) {
            const auto before = drift.solution(north, arc);
            const auto count = drift.sample_count();
            try {
                drift.add_landing(drift.firing_snapshot(north, arc), wrong_point);
            } catch (const std::invalid_argument& error) {
                rejected_drift = true;
                const auto retained_after = drift.solution(north, arc);
                check(drift.sample_count() == count && before.bearing_deg == retained_after.bearing_deg &&
                          before.mil == retained_after.mil,
                      "excessive accumulated steering is rejected transactionally");
                check(std::string(error.what()).find("Накопленная поправка") != std::string::npos,
                      "progressive contradictory feedback reaches the geometric accumulated-steering guard");
            }
        }
        check(rejected_drift,
              "progressive inconsistent feedback cannot accumulate unbounded local steering");
    }
    // A stored target can fit just within the steering budget while a nearby
    // query's range changes enough to exceed it. Check every returned result,
    // including firing snapshots, without mutating accepted observations.
    const Point query_base{0.0, 0.0};
    const Point query_target{0.0, 25.0};
    const Point query_impact{-3.9507533623805506, 24.374262139839075};
    constexpr double query_supplied_bearing = 9.206835708267924;
    constexpr double query_supplied_high_mil = 833.4021851272083;
    const double query_supplied_range = sph2_distance_for_mil(
        query_supplied_high_mil, Arc::high);
    for (Arc arc : {Arc::low, Arc::high}) {
        ContinuousCalibration query_model(query_base, baseline, ContinuousCorrectionMode::local_only);
        auto firing = query_model.firing_snapshot(query_target, arc);
        firing.bearing_deg = query_supplied_bearing;
        firing.mil = arc == Arc::high ? query_supplied_high_mil
            : sph2_mil_for_distance(query_supplied_range, Arc::low);
        query_model.add_landing(firing, query_impact);
        const auto retained = query_model.solution(query_target, arc);
        const double retained_bearing = retained.bearing_deg * std::numbers::pi / 180.0;
        close(std::hypot(retained.reticle_distance_m * std::sin(retained_bearing),
                         retained.reticle_distance_m * std::cos(retained_bearing) - 2500.0),
              799.994787,
              "the retained local target remains just within its geometric steering budget on both arcs", 1e-6);
        const auto inside = query_model.solution({0.0, 24.999}, arc);
        check(std::isfinite(inside.bearing_deg) && std::isfinite(inside.mil),
              "a nearby query within the geometric steering budget keeps usable guidance");
        const auto retained_count = query_model.sample_count();
        rejects([&] { (void)query_model.solution({0.0, 25.001}, arc); },
                "a nearby query exceeding the geometric steering budget cannot return unsafe guidance");
        rejects([&] { (void)query_model.firing_snapshot({0.0, 25.001}, arc); },
                "a nearby out-of-budget query cannot capture unsafe firing settings");
        const auto after_query_failure = query_model.solution(query_target, arc);
        check(query_model.sample_count() == retained_count &&
                  after_query_failure.bearing_deg == retained.bearing_deg &&
                  after_query_failure.mil == retained.mil &&
                  query_model.global_calibration().rotation == baseline.rotation,
              "rejected result queries preserve exact accepted history and guidance");
        const Point far{0.0, 24.5};
        const auto far_direct = corrected_solution(query_base, far, baseline, arc);
        const auto far_actual = query_model.solution(far, arc);
        check(far_actual.bearing_deg == far_direct.bearing_deg && far_actual.mil == far_direct.mil,
              "the query-level steering guard retains exact zero local influence at the 50-metre border");
    }
    rejects([&] { ContinuousCalibration invalid(
        base, baseline, static_cast<ContinuousCorrectionMode>(-1)); },
        "an unknown continuous correction mode cannot silently enable global transfer");
    const auto initial = model.solution(north, Arc::low);
    const auto plain = corrected_solution(base, north, baseline, Arc::low);
    const auto direct_first = model.firing_snapshot(north, Arc::low);
    check(direct_first.target == north && direct_first.arc == Arc::low &&
              std::abs(direct_first.bearing_deg - initial.bearing_deg) < 1e-9 &&
              std::abs(direct_first.mil - initial.mil) < 1e-9,
          "one-step impact entry captures the current target and firing solution");
    check(std::abs(initial.bearing_deg - plain.bearing_deg) < 1e-9 &&
              std::abs(initial.mil - plain.mil) < 1e-9,
          "empty online model preserves the two-shot baseline");

    // A plausible first shot should remove most of the miss now that the
    // reticle overlay makes precise entry practical. Outliers remain gated by
    // their confidence score below.
    const auto first = model.add_landing(
        snapshot(base, north, Arc::low),
        impact_with_required_offset(base, north, Arc::low, 1.0, 10.0));
    check(first.confidence > 0.35 && first.confidence <= 1.0,
          "a first shot is provisional, not automatically unreliable");
    const auto after_first = model.solution(north, Arc::low);
    check(after_first.bearing_deg > plain.bearing_deg + 0.6 &&
              after_first.bearing_deg < plain.bearing_deg + 1.1 &&
              after_first.mil > plain.mil + 6.0 &&
              after_first.mil < plain.mil + 12.0,
          "one plausible shot applies most of the local correction immediately");

    model.add_landing(snapshot(base, north, Arc::low),
                      impact_with_required_offset(base, north, Arc::low, 1.05, 9.0));
    model.add_landing(snapshot(base, north, Arc::low),
                      impact_with_required_offset(base, north, Arc::low, 0.95, 11.0));
    const auto learned = model.solution(north, Arc::low);
    const auto direct_next = model.firing_snapshot(north, Arc::low, 12.0);
    const auto raised = model.solution(north, Arc::low, 12.0);
    check(direct_next.target == north && direct_next.arc == Arc::low &&
              direct_next.target_height_delta_m == 12.0 &&
              std::abs(direct_next.bearing_deg - raised.bearing_deg) < 1e-9 &&
              std::abs(direct_next.mil - raised.mil) < 1e-9,
          "later impact entry uses the latest compensation and target height");
    check(learned.bearing_deg > plain.bearing_deg + 0.65 &&
              learned.bearing_deg < plain.bearing_deg + 1.2,
          "consistent shots learn the bearing correction");
    check(learned.mil > plain.mil + 6.0 && learned.mil < plain.mil + 14.0,
          "consistent shots learn the reticle correction");

    const auto bad = model.add_landing(
        snapshot(base, north, Arc::low),
        impact_with_required_offset(base, north, Arc::low, -2.0, -25.0));
    const auto after_bad = model.solution(north, Arc::low);
    check(bad.confidence < 0.3,
          "a lone incompatible impact has low confidence");
    check(std::abs(after_bad.bearing_deg - learned.bearing_deg) < 0.35 &&
              std::abs(after_bad.mil - learned.mil) < 4.0,
          "one bad impact cannot overturn consistent history");

    const Point east{68, 50};
    const auto before_switch = model.solution(east, Arc::low);
    const auto east_plain = corrected_solution(base, east, baseline, Arc::low);
    check(before_switch.bearing_deg > east_plain.bearing_deg,
          "switching targets retains a bounded shared correction");
    const auto east_first = model.add_landing(
        snapshot(base, east, Arc::low),
        impact_with_required_offset(base, east, Arc::low, 1.0, 10.0));
    check(east_first.confidence > 0.35,
          "a new target does not automatically lower shot confidence");
    model.add_landing(snapshot(base, east, Arc::low),
                      impact_with_required_offset(base, east, Arc::low, 1.0, 10.0));
    check(model.solution(east, Arc::low).bearing_deg >
              before_switch.bearing_deg,
          "new-target impacts continue updating the same model");

    ContinuousCalibration isolated(base, baseline);
    const Point south{50, 32};
    const auto extreme_first = isolated.add_landing(
        snapshot(base, south, Arc::low),
        impact_with_required_offset(base, south, Arc::low, -10.0, -100.0));
    const auto south_plain = corrected_solution(base, south, baseline, Arc::low);
    const auto provisional = isolated.solution(south, Arc::low);
    check(extreme_first.confidence < 0.2,
          "a single extreme unexplained impact starts at low confidence");
    check(std::abs(provisional.bearing_deg - south_plain.bearing_deg) < 1.0 &&
              std::abs(provisional.mil - south_plain.mil) < 15.0,
          "one extreme shot has a strict immediate influence limit");
    isolated.add_landing(snapshot(base, south, Arc::low),
                         impact_with_required_offset(base, south, Arc::low,
                                                     -10.0, -100.0));
    check(isolated.solution(south, Arc::low).bearing_deg <
              south_plain.bearing_deg - 1.0,
          "repeated extreme but coherent evidence can regain influence");

    const Point northeast{63, 63};
    const auto transfer_before = model.solution(northeast, Arc::low);
    const auto south_first = model.add_landing(
        snapshot(base, south, Arc::low),
        impact_with_required_offset(base, south, Arc::low, -10.0, -100.0));
    check(south_first.confidence < 0.2,
          "an unexplained extreme impact is provisional for its size, not its target");
    model.add_landing(snapshot(base, south, Arc::low),
                      impact_with_required_offset(base, south, Arc::low,
                                                  -10.0, -100.0));
    const auto transfer_after = model.solution(northeast, Arc::low);
    check(std::abs(transfer_after.bearing_deg - transfer_before.bearing_deg) < 0.25 &&
              std::abs(transfer_after.mil - transfer_before.mil) < 5.0,
          "a conflicting target group does not rewrite shared correction");
    const auto south_adjusted = model.solution(south, Arc::low);
    check(south_adjusted.bearing_deg < south_plain.bearing_deg - 1.0,
          "a coherent target-specific shift remains useful near that target");

    const auto high_plain = corrected_solution(base, north, baseline, Arc::high);
    const auto high_prediction = model.solution(north, Arc::high);
    check(std::abs(high_prediction.bearing_deg - high_plain.bearing_deg) > 1e-5 ||
              std::abs(high_prediction.mil - high_plain.mil) > 1e-5,
          "low-arc observations transfer global platform information to high arc");
    const auto low_before_high = model.solution(north, Arc::low);
    model.add_landing(snapshot(base, north, Arc::high),
                      impact_with_required_offset(base, north, Arc::high,
                                                  0.7, 12.0));
    model.add_landing(snapshot(base, north, Arc::high),
                      impact_with_required_offset(base, north, Arc::high,
                                                  0.7, 12.0));
    check(model.solution(north, Arc::high).bearing_deg >
              high_plain.bearing_deg + 0.35,
          "high-arc observations update the high-arc solution");
    check(std::abs(model.solution(north, Arc::low).bearing_deg -
                   low_before_high.bearing_deg) > 1e-5,
          "high-arc observations can refine the shared global platform model");

    ContinuousCalibration repeated_target(base, baseline);
    for (Point independent_target : {east, south})
        for (int landing = 0; landing < 3; ++landing)
            repeated_target.add_landing(snapshot(base, independent_target, Arc::low),
                                         independent_target);
    const auto repeated_impact = impact_with_required_offset(
        base, north, Arc::low, 2.0, 20.0);
    for (int landing = 0; landing < 3; ++landing)
        repeated_target.add_landing(snapshot(base, north, Arc::low), repeated_impact);
    const auto balanced_rotation = repeated_target.global_calibration().rotation;
    for (int landing = 0; landing < 30; ++landing)
        repeated_target.add_landing(snapshot(base, north, Arc::low), repeated_impact);
    double repeated_rotation_change = 0.0;
    for (std::size_t row = 0; row < 3; ++row)
        for (std::size_t column = 0; column < 3; ++column)
            repeated_rotation_change += std::abs(
                repeated_target.global_calibration().rotation[row][column] -
                balanced_rotation[row][column]);
    check(repeated_rotation_change < 1e-7,
          "repeating one corroborated target cannot outvote independent global directions");
    check(repeated_target.sample_count() == 39,
          "limiting one target's global vote retains every valid observation");

    model.clear();
    check(model.sample_count() == 0,
          "clear removes active online observations");
    const auto restored = model.solution(north, Arc::low);
    check(std::abs(restored.bearing_deg - plain.bearing_deg) < 1e-9 &&
              std::abs(restored.mil - plain.mil) < 1e-9,
          "clear restores the untouched two-shot model");

    ContinuousCalibration global_model(base, baseline);
    const double global_tilt = 4.0 * std::numbers::pi / 180.0;
    const PlatformCalibration actual_platform{{{{1, 0, 0},
                                                 {0, std::cos(global_tilt),
                                                  -std::sin(global_tilt)},
                                                 {0, std::sin(global_tilt),
                                                  std::cos(global_tilt)}}},
                                               0.0};
    for (const Point training_target :
         {Point{50, 68}, Point{68, 50}, Point{38, 62}}) {
        for (int shot = 0; shot < 3; ++shot) {
            const auto firing = global_model.firing_snapshot(
                training_target, Arc::low);
            global_model.add_landing(
                firing, impact_from_firing(base, firing, actual_platform));
        }
    }
    const auto& learned_platform = global_model.global_calibration();
    const auto learned_probe = learned_platform.local_to_world(
        direction_from_bearing_and_elevation(32.0, 0.4));
    const auto actual_probe = actual_platform.local_to_world(
        direction_from_bearing_and_elevation(32.0, 0.4));
    const auto baseline_probe = baseline.local_to_world(
        direction_from_bearing_and_elevation(32.0, 0.4));
    double learned_error = 0.0;
    double baseline_error = 0.0;
    for (std::size_t index = 0; index < 3; ++index) {
        learned_error += std::abs(learned_probe[index] - actual_probe[index]);
        baseline_error += std::abs(baseline_probe[index] - actual_probe[index]);
    }
    check(learned_error < baseline_error * 0.55,
          "consistent continuous shots refine the global platform model");
    check(global_model.global_rotation_adjustment_deg() > 1.0,
          "global refinement exposes its accumulated rotation adjustment");
    global_model.clear();
    const auto reset_probe = global_model.global_calibration().local_to_world(
        direction_from_bearing_and_elevation(32.0, 0.4));
    check(std::abs(reset_probe[0] - baseline_probe[0]) < 1e-9 &&
              std::abs(reset_probe[1] - baseline_probe[1]) < 1e-9 &&
              std::abs(reset_probe[2] - baseline_probe[2]) < 1e-9,
          "clearing continuous compensation restores the original global model");
    check(global_model.global_rotation_adjustment_deg() < 1e-9,
          "clearing continuous compensation resets the global adjustment");

    const auto edited_shot = snapshot(base, north, Arc::low, 0.4);
    model.add_landing(
        edited_shot,
        impact_with_required_offset(base, north, Arc::low, 1.0, 10.0, 0.4));
    check(model.solution(north, Arc::low).bearing_deg > plain.bearing_deg,
          "recorded firing settings account for manual bearing adjustment");

    const double tilt = 2.0 * std::numbers::pi / 180.0;
    const PlatformCalibration tilted{{{{1, 0, 0},
                                       {0, std::cos(tilt), -std::sin(tilt)},
                                       {0, std::sin(tilt), std::cos(tilt)}}}, 0.0};
    ContinuousCalibration rotated(base, tilted);
    const auto tilted_plain = corrected_solution(base, north, tilted, Arc::low);
    for (int shot = 0; shot < 3; ++shot) {
        rotated.add_landing(
            {north, Arc::low, tilted_plain.bearing_deg, tilted_plain.mil},
            impact_with_rotated_baseline(base, north, Arc::low, tilted,
                                         0.8, 8.0));
    }
    const auto tilted_online = rotated.solution(north, Arc::low);
    check(tilted_online.bearing_deg > tilted_plain.bearing_deg + 0.5 &&
              tilted_online.mil > tilted_plain.mil + 5.0,
          "online correction composes with a non-identity two-shot rotation");

    ContinuousCalibration edge(base, baseline);
    try {
        edge.add_landing(snapshot(base, {50, 62}, Arc::low),
                         {50, 61.5});
        check(edge.sample_count() == 1,
              "a physically reachable impact below the sight-table minimum is kept");
    } catch (...) {
        check(false,
              "an undershoot just below the sight-table minimum is a valid observation");
    }

    rejects([&] {
        auto invalid = snapshot(base, north, Arc::low);
        invalid.mil = std::numeric_limits<double>::quiet_NaN();
        model.add_landing(invalid, north);
    }, "non-finite firing settings are rejected");
    rejects([&] {
        model.add_landing(snapshot(base, north, Arc::low), base);
    }, "an impact at the gun position is rejected");

    ContinuousCalibration transactional({0, 0}, baseline);
    const Point maximum_target{0, 26.29};
    ContinuousCalibration boundary_target({0, 0}, baseline);
    const auto maximum_firing = boundary_target.firing_snapshot(maximum_target, Arc::low);
    rejects([&] {
        boundary_target.add_landing(maximum_firing, {0, 25});
    }, "a boundary-target undershoot cannot commit an unusable target solution");
    check(boundary_target.sample_count() == 0 &&
              boundary_target.global_calibration().rotation == baseline.rotation,
          "an unusable refitted target leaves the observation history and rotation untouched");
    const auto boundary_after = boundary_target.solution(maximum_target, Arc::low);
    close(boundary_after.mil, maximum_firing.mil,
          "rejected target compensation preserves the previous valid sight setting", 1e-8);
    const auto boundary_firing = transactional.firing_snapshot(maximum_target, Arc::low);
    transactional.add_landing(boundary_firing, maximum_target);
    const auto retained_rotation = transactional.global_calibration().rotation;
    const auto retained_solution = transactional.solution(maximum_target, Arc::low);
    rejects([&] {
        transactional.add_landing(transactional.firing_snapshot({0, 18}, Arc::low), {0, 17});
    }, "a refit invalidating a retained boundary impact is rejected");
    check(transactional.sample_count() == 1,
          "failed refit cannot commit a new observation");
    check(transactional.global_calibration().rotation == retained_rotation,
          "failed refit preserves the exact prior active rotation");
    const auto after_rejection = transactional.solution(maximum_target, Arc::low);
    check(after_rejection.bearing_deg == retained_solution.bearing_deg &&
              after_rejection.mil == retained_solution.mil,
          "failed refit preserves the firing solution and observation confidence");
    transactional.add_landing(boundary_firing, maximum_target);
    check(transactional.sample_count() == 2,
          "the model remains usable after rejecting an incompatible landing");
    rejects([&] { ContinuousCalibration invalid(base, {}); },
            "continuous calibration validates its frozen baseline rotation");
    rejects([&] { ContinuousCalibration invalid(
        {std::numeric_limits<double>::infinity(), 0}, baseline); },
            "continuous calibration validates the base position before storing it");
    for (Arc arc : {Arc::low, Arc::high}) {
        for (double height : {-50.0, 100.0}) {
            ContinuousCalibration height_model(base, baseline);
            for (int landing = 0; landing < 3; ++landing)
                height_model.add_landing(height_model.firing_snapshot(north, arc, height),
                                         north, height);
            close(height_model.global_rotation_adjustment_deg(), 0,
                  "exact elevated hits cannot create a false platform tilt", 1e-6);
            const auto expected = corrected_solution(base, north, baseline, arc, height);
            const auto actual = height_model.solution(north, arc, height);
            close(actual.mil, expected.mil,
                  "height-aware frozen firing settings remain consistent after exact hits", 1e-8);
            close(actual.bearing_deg, expected.bearing_deg,
                  "height-aware exact hits preserve the firing bearing", 1e-8);
        }
    }

    ContinuousCalibration bounded(base, baseline);
    const auto exact_firing = bounded.firing_snapshot(north, Arc::low);
    for (std::size_t landing = 0; landing < maximum_continuous_observations; ++landing)
        bounded.add_landing(exact_firing, north);
    check(bounded.sample_count() == maximum_continuous_observations,
          "a complete bounded session retains all observations up to its explicit limit");
    const auto full_rotation = bounded.global_calibration().rotation;
    const auto full_solution = bounded.solution(north, Arc::low);
    rejects([&] { bounded.add_landing(exact_firing, north); },
            "a full session rejects additional work before unbounded history growth");
    check(bounded.sample_count() == maximum_continuous_observations &&
              bounded.global_calibration().rotation == full_rotation,
          "the explicit history limit cannot discard evidence or alter the calibration");
    const auto after_limit = bounded.solution(north, Arc::low);
    check(after_limit.bearing_deg == full_solution.bearing_deg &&
              after_limit.mil == full_solution.mil,
          "reaching the history limit leaves the current firing solution usable");
    bounded.clear();
    bounded.add_landing(exact_firing, north);
    check(bounded.sample_count() == 1 &&
              bounded.global_calibration().rotation == baseline.rotation,
          "clearing a full session makes room while preserving the original baseline");

    ContinuousCalibration local_bounded(base, baseline, ContinuousCorrectionMode::local_only);
    const auto local_exact_firing = local_bounded.firing_snapshot(north, Arc::low);
    for (std::size_t landing = 0; landing < maximum_continuous_observations; ++landing)
        local_bounded.add_landing(local_exact_firing, north);
    const auto local_full_solution = local_bounded.solution(north, Arc::low);
    rejects([&] { local_bounded.add_landing(local_exact_firing, north); },
            "the local-only mode retains the bounded session limit");
    const auto local_after_limit = local_bounded.solution(north, Arc::low);
    check(local_bounded.sample_count() == maximum_continuous_observations &&
              local_bounded.global_calibration().rotation == baseline.rotation &&
              local_after_limit.bearing_deg == local_full_solution.bearing_deg &&
              local_after_limit.mil == local_full_solution.mil,
          "a full local-only session preserves exact usable guidance and evidence");
    local_bounded.clear();
    local_bounded.add_landing(local_exact_firing, north);
    check(local_bounded.sample_count() == 1,
          "clearing the local-only session frees capacity without requiring two new shots");

    if (failures) return 1;
    std::cout << "All continuous calibration tests passed\n";
}
