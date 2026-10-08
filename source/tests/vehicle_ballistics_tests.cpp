#include "wardogs/vehicle_ballistics.hpp"

#include <array>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>

namespace {

int failures = 0;
std::string test_context = "input validation";

void check(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void close(double actual, double expected, const char* message,
           double tolerance = 1e-8) {
    check(std::abs(actual - expected) <= tolerance, message);
}

void rejects(const std::function<void()>& action, const char* message) {
    try {
        action();
        check(false, message);
    } catch (const std::invalid_argument&) {
    }
}

wardogs::Matrix3 rotation_x(double degrees) {
    const double angle = degrees * std::numbers::pi / 180.0;
    return {{{1, 0, 0},
             {0, std::cos(angle), -std::sin(angle)},
             {0, std::sin(angle), std::cos(angle)}}};
}

double trajectory_elevation(double distance_m, double height_delta_m,
                            wardogs::Arc arc) {
    const double maximum = wardogs::sph2_maximum_range_m;
    const double root = std::sqrt(maximum * maximum - distance_m * distance_m -
                                  2.0 * maximum * height_delta_m);
    return std::atan((maximum + (arc == wardogs::Arc::low ? -root : root)) /
                     distance_m);
}

double flat_trajectory_elevation(double distance_m, wardogs::Arc arc) {
    return trajectory_elevation(distance_m, 0.0, arc);
}

double landing_range_for_elevation(double elevation_rad,
                                   double height_delta_m) {
    const long double cosine = std::cos(static_cast<long double>(elevation_rad));
    const long double center = wardogs::sph2_maximum_range_m * cosine * cosine *
                               std::tan(static_cast<long double>(elevation_rad));
    return static_cast<double>(center + std::sqrt(
        center * center - 2.0L * wardogs::sph2_maximum_range_m *
                              cosine * cosine * height_delta_m));
}

wardogs::Vector3 direction_from_bearing_and_elevation(double bearing_deg,
                                                       double elevation_rad) {
    const double bearing = bearing_deg * std::numbers::pi / 180.0;
    const double horizontal = std::cos(elevation_rad);
    return {horizontal * std::sin(bearing),
            horizontal * std::cos(bearing), std::sin(elevation_rad)};
}

wardogs::Point impact_for_rotation(wardogs::Point base, wardogs::Point aim,
                                   wardogs::Arc arc,
                                   const wardogs::Matrix3& rotation,
                                   double aim_height_delta_m = 0.0,
                                   double impact_height_delta_m = 0.0) {
    const double dx = aim.x - base.x;
    const double dy = aim.y - base.y;
    const double distance = std::hypot(dx, dy) * 100.0;
    double bearing = std::atan2(dx, dy) * 180.0 / std::numbers::pi;
    if (bearing < 0) bearing += 360.0;
    const auto direction = direction_from_bearing_and_elevation(
        bearing, trajectory_elevation(distance, aim_height_delta_m, arc));
    wardogs::Vector3 world{};
    for (std::size_t row = 0; row < 3; ++row)
        for (std::size_t column = 0; column < 3; ++column)
            world[row] += rotation[row][column] * direction[column];
    double actual_bearing =
        std::atan2(world[0], world[1]) * 180.0 / std::numbers::pi;
    if (actual_bearing < 0) actual_bearing += 360.0;
    const double actual_elevation =
        std::atan2(world[2], std::hypot(world[0], world[1]));
    const double cosine = std::cos(actual_elevation);
    const double tangent = std::tan(actual_elevation);
    const double center = wardogs::sph2_maximum_range_m * cosine * cosine *
                          tangent;
    const double root = std::sqrt(
        center * center - 2.0 * wardogs::sph2_maximum_range_m * cosine *
                              cosine * impact_height_delta_m);
    const double actual_distance = center + root;
    return {base.x + std::sin(actual_bearing * std::numbers::pi / 180.0) *
                         actual_distance / 100.0,
            base.y + std::cos(actual_bearing * std::numbers::pi / 180.0) *
                         actual_distance / 100.0};
}

}  // namespace

int run_tests() {
    using wardogs::Arc;
    using wardogs::CalibrationShot;
    using wardogs::Point;

    rejects([] { (void)wardogs::sph2_mil_for_distance(
        std::numeric_limits<double>::quiet_NaN(), Arc::low); },
        "NaN cannot return an apparently valid low-arc table endpoint");
    rejects([] { (void)wardogs::sph2_mil_for_distance(
        std::numeric_limits<double>::infinity(), Arc::high); },
        "infinite range is rejected");
    rejects([] { (void)wardogs::sph2_distance_for_mil(
        std::numeric_limits<double>::quiet_NaN(), Arc::high); },
        "NaN sight setting cannot return a table endpoint");
    rejects([] { (void)wardogs::sph2_mil_for_trajectory(
        1800, std::numeric_limits<double>::quiet_NaN(), Arc::low); },
        "NaN height cannot produce a trajectory solution");
    rejects([] { (void)wardogs::impact_direction(
        {0, 0}, {std::numeric_limits<double>::infinity(), 1}, Arc::low); },
        "non-finite impact cannot enter a calibration basis");
    test_context = "table samples and interpolation roundtrips";
    for (int mil = 20; mil <= 600; mil += 10) {
        close(wardogs::sph2_mil_for_distance(
            wardogs::sph2_distance_for_mil(mil, Arc::low), Arc::low), mil,
            "every low-arc sample round-trips through the unchanged firing table");
    }
    for (int mil = 630; mil <= 1400; mil += 10) {
        close(wardogs::sph2_mil_for_distance(
            wardogs::sph2_distance_for_mil(mil, Arc::high), Arc::high), mil,
            "every non-duplicate high-arc sample round-trips through the unchanged firing table");
    }
    rejects([] { (void)wardogs::sph2_mil_for_trajectory(1e308, -1e308, Arc::low, true); },
            "finite huge range and negative height cannot hide a NaN discriminant");
    rejects([] { (void)wardogs::sph2_world_mil_for_distance(1800, static_cast<Arc>(-1)); },
            "an unknown trajectory cannot silently use the high table");
    for (double distance = 1181; distance <= 2629; distance += 0.5) {
        close(wardogs::sph2_distance_for_mil(
                  wardogs::sph2_mil_for_distance(distance, Arc::low), Arc::low),
              distance, "low interpolation round-trips between every pair of samples", 1e-9);
    }
    double previous_high_distance = wardogs::sph2_distance_for_mil(610, Arc::high);
    for (double mil = 610; mil <= 1400; mil += 0.5) {
        const double distance = wardogs::sph2_distance_for_mil(mil, Arc::high);
        check(distance <= previous_high_distance,
              "high sight range is monotone, including the retained maximum plateau");
        previous_high_distance = distance;
        const double recovered = wardogs::sph2_distance_for_mil(
            wardogs::sph2_mil_for_distance(distance, Arc::high), Arc::high);
        check(std::abs(recovered - distance) <= 1.0 + 1e-9,
              "high round-trip ambiguity stays within the original one-metre plateau");
    }
    for (Arc arc : {Arc::low, Arc::high}) {
        for (double distance : {1500.0, 1800.0, 2200.0}) {
            for (double height : {-50.0, 0.0, 100.0}) {
                test_context = std::string{"independent height reference arc="} +
                    (arc == Arc::low ? "low" : "high") + " distance=" +
                    std::to_string(distance) + " height=" + std::to_string(height);
                // An independent long-double evaluation of the retained game
                // approximation checks height signs, arc selection and units.
                const long double maximum = wardogs::sph2_maximum_range_m;
                const long double root = std::sqrt(maximum * maximum -
                    distance * distance - 2 * maximum * height);
                const long double tangent =
                    (maximum + (arc == Arc::low ? -root : root)) / distance;
                const double equivalent = static_cast<double>(maximum *
                    std::sin(2 * std::atan(tangent)));
                close(wardogs::sph2_mil_for_trajectory(distance, height, arc),
                      wardogs::sph2_mil_for_distance(equivalent, arc),
                      "height trajectory matches independent retained-model reference", 1e-8);
            }
        }
    }
    const wardogs::PlatformCalibration terrain_identity{
        wardogs::identity_rotation(), 0.0};
    for (Arc arc : {Arc::low, Arc::high}) {
        for (double x : {-0.0, -1e-15}) {
            test_context = std::string{"canonical north arc="} +
                (arc == Arc::low ? "low" : "high");
            const auto north = wardogs::corrected_solution(
                {0, 0}, {x, 18}, terrain_identity, arc);
            check(north.bearing_deg >= 0.0 && north.bearing_deg < 360.0 &&
                      !std::signbit(north.bearing_deg),
                  "SPH-2 north remains canonical even when wrapping rounds to 360 degrees");
        }
    }
    for (Arc arc : {Arc::low, Arc::high}) {
        for (double distance : {1500.0, 1800.0, 2200.0}) {
            for (double height : {-100.0, -50.0, 0.0, 50.0, 100.0}) {
                test_context = std::string{"height-aware firing roundtrip arc="} +
                    (arc == Arc::low ? "low" : "high") + " distance=" +
                    std::to_string(distance) + " height=" + std::to_string(height);
                const double bearing = 57.0;
                const double radians = bearing * std::numbers::pi / 180.0;
                const Point target{distance * std::sin(radians) / 100.0,
                                   distance * std::cos(radians) / 100.0};
                // Reachability in the physical approximation does not imply
                // that the game's sight can request this command. Determine
                // that domain independently, before testing the supported
                // firing roundtrip. For example 1500 m at -100 m on the low
                // arc needs an equivalent 1167.7 m, below the 1181 m / 20 MIL
                // table endpoint. Preserve the rejection instead of treating
                // it as an unexpectedly valid firing fixture.
                const double reference_equivalent_range = wardogs::sph2_maximum_range_m *
                    std::sin(2.0 * trajectory_elevation(distance, height, arc));
                const double minimum_sight_range = arc == Arc::low ? 1181.0 : 735.0;
                if (reference_equivalent_range < minimum_sight_range) {
                    const auto observed_direction = wardogs::required_firing_angles(
                        {0, 0}, target, terrain_identity, arc, height);
                    check(std::isfinite(observed_direction.mil) &&
                              (arc == Arc::low ? observed_direction.mil < 20.0
                                               : observed_direction.mil > 1400.0),
                          "the physical observation domain can extend past supported sight settings");
                    close(observed_direction.mil,
                          wardogs::sph2_mil_for_trajectory(distance, height, arc, true),
                          "extended observation APIs agree below the sight's supported range");
                    rejects([&] {
                        (void)wardogs::sph2_mil_for_trajectory(distance, height, arc);
                    }, "a physically reachable target cannot extend the game sight table");
                    rejects([&] {
                        (void)wardogs::corrected_solution(
                            {0, 0}, target, terrain_identity, arc, height);
                    }, "a firing command below the supported sight range remains rejected");
                    continue;
                }
                const auto solution = wardogs::corrected_solution(
                    {0, 0}, target, terrain_identity, arc, height);
                close(solution.bearing_deg, bearing,
                      "height alone cannot rotate the target's horizontal bearing");
                close(solution.mil,
                      wardogs::sph2_mil_for_trajectory(distance, height, arc),
                      "map height and direct trajectory lookup produce the same sight setting");
                // Decode through the sight table, not MIL/1000: game sight MIL
                // is not the physical launch angle used by the retained model.
                const auto launch = wardogs::firing_direction(
                    solution.bearing_deg, solution.mil, arc);
                const long double horizontal = std::hypot(
                    static_cast<long double>(launch[0]),
                    static_cast<long double>(launch[1]));
                const long double tangent = launch[2] / horizontal;
                const long double reconstructed_height = distance * tangent -
                    distance * distance /
                        (2.0L * wardogs::sph2_maximum_range_m * horizontal * horizontal);
                close(static_cast<double>(reconstructed_height), height,
                      "terrain-aware command reaches the requested vertical plane in the retained model",
                      1e-8);
            }
        }
        test_context = std::string{"downhill extended range arc="} +
            (arc == Arc::low ? "low" : "high");
        rejects([&] { (void)wardogs::sph2_mil_for_trajectory(2600, 100, arc); },
                "an elevated target beyond the physical envelope is rejected on both arcs");
        const auto downhill_beyond_flat = wardogs::corrected_solution(
            {0, 0}, {0, 26.75}, terrain_identity, arc, -100);
        check(std::isfinite(downhill_beyond_flat.mil) &&
              downhill_beyond_flat.reticle_distance_m <= wardogs::sph2_maximum_range_m,
              "lower terrain can extend horizontal reach without extending the sight table");

        const double minimum_range = arc == Arc::low ? 1181.0 : 735.0;
        const double endpoint_mil = arc == Arc::low ? 20.0 : 1400.0;
        const double elevation = flat_trajectory_elevation(minimum_range, arc);
        for (double height : {-100.0, -50.0, 20.0}) {
            test_context = std::string{"height-aware minimum endpoint arc="} +
                (arc == Arc::low ? "low" : "high") + " height=" + std::to_string(height);
            const double horizontal_range = landing_range_for_elevation(elevation, height);
            close(wardogs::sph2_mil_for_trajectory(horizontal_range, height, arc),
                  endpoint_mil,
                  "height conversion cannot reject an exact supported sight endpoint from roundoff");
            for (double bearing : {0.0, 37.0, 90.0, 180.0, 270.0}) {
                test_context = std::string{"coordinate minimum endpoint arc="} +
                    (arc == Arc::low ? "low" : "high") + " height=" +
                    std::to_string(height) + " bearing=" + std::to_string(bearing);
                const double radians = bearing * std::numbers::pi / 180.0;
                const Point target{horizontal_range * std::sin(radians) / 100.0,
                                   horizontal_range * std::cos(radians) / 100.0};
                const auto endpoint = wardogs::corrected_solution(
                    {0, 0}, target, terrain_identity, arc, height);
                close(endpoint.mil, endpoint_mil,
                      "terrain-aware supported endpoint survives coordinate and direction roundtrips");
            }
        }
        rejects([&] { (void)wardogs::sph2_mil_for_distance(minimum_range - 1e-8, arc); },
                "internal roundoff snapping does not relax raw user range validation");
    }
    test_context = "height signs, tiny range and huge bearing";
    check(wardogs::sph2_mil_for_trajectory(1800, 50, Arc::low) >
              wardogs::sph2_mil_for_trajectory(1800, 0, Arc::low) &&
          wardogs::sph2_mil_for_trajectory(1800, -50, Arc::low) <
              wardogs::sph2_mil_for_trajectory(1800, 0, Arc::low),
          "higher and lower terrain change the low-arc sight in the expected direction");
    check(wardogs::sph2_mil_for_trajectory(1800, 50, Arc::high) <
              wardogs::sph2_mil_for_trajectory(1800, 0, Arc::high) &&
          wardogs::sph2_mil_for_trajectory(1800, -50, Arc::high) >
              wardogs::sph2_mil_for_trajectory(1800, 0, Arc::high),
          "higher and lower terrain change the high-arc sight in the expected direction");
    const auto tiny = wardogs::required_firing_angles(
        {0, 0}, {0, 1e-7}, {wardogs::identity_rotation(), 0}, Arc::low);
    close(tiny.mil, 1e-5 * 20.0 / 1181.0,
          "tiny reachable low direction avoids subtractive cancellation", 1e-15);
    const double huge_bearing = std::numeric_limits<double>::max();
    const auto huge_direction = wardogs::firing_direction(huge_bearing, 200, Arc::low);
    const auto wrapped_direction = wardogs::firing_direction(std::fmod(huge_bearing, 360.0), 200, Arc::low);
    for (std::size_t index = 0; index < 3; ++index)
        close(huge_direction[index], wrapped_direction[index],
              "finite huge bearings wrap before converting degrees to radians");

    close(wardogs::sph2_mil_for_distance(1181, Arc::low), 20,
          "low table endpoint");
    close(wardogs::sph2_mil_for_distance(1206.5, Arc::low), 25,
          "low table interpolation");
    close(wardogs::sph2_distance_for_mil(900, Arc::high), 2360,
          "high inverse table");
    close(wardogs::sph2_mil_for_distance(2629, Arc::high), 610,
          "duplicate maximum range keeps the first high-arc sight value");
    check(wardogs::sph2_world_mil_for_distance(500, Arc::low) > 0,
          "observed low impact extends below sight range");
    check(wardogs::sph2_world_mil_for_distance(553.399, Arc::high) > 1400,
          "observed high impact extends toward vertical");
    rejects([] { (void)wardogs::sph2_mil_for_distance(700, Arc::low); },
            "unsupported low range is rejected");

    test_context = "two-shot synthetic platform rotation";
    const Point base{50, 50};
    const auto rotation = rotation_x(5);
    const Point first_aim{50, 68};
    const Point second_aim{68, 50};
    const auto calibration = wardogs::calibrate_platform(
        base,
        CalibrationShot{first_aim,
                        impact_for_rotation(base, first_aim, Arc::low, rotation),
                        Arc::low},
        CalibrationShot{
            second_aim,
            impact_for_rotation(base, second_aim, Arc::high, rotation),
            Arc::high});
    const auto probe = wardogs::direction_from_bearing_and_mil(42, 500);
    const auto recovered = calibration.local_to_world(probe);
    for (std::size_t row = 0; row < 3; ++row) {
        double expected = 0;
        for (std::size_t column = 0; column < 3; ++column)
            expected += rotation[row][column] * probe[column];
        close(recovered[row], expected, "two shots recover platform rotation",
              1e-9);
    }
    close(calibration.pair_angle_residual_deg, 0,
          "synthetic calibration residual", 1e-9);

    // This recorded pair gave a 6.5537-degree discrepancy and a plausible
    // looking rotation, but its own predicted impacts missed the observations
    // by about 240/286 m under the retained flat-ground model.
    rejects([] {
        (void)wardogs::calibrate_platform(
            {94.16, 110.46},
            {{83.37, 90.3}, {87.25, 99.98}, Arc::high},
            {{104.91, 92.88}, {104.54, 96.14}, Arc::high});
    }, "incompatible recorded high-arc impacts cannot publish an initial calibration");

    // Independently prescribe the angle between the observed directions.
    // Small observation error remains useful; the policy boundary is inclusive
    // on either trajectory and cannot admit a materially greater discrepancy.
    for (Arc arc : {Arc::low, Arc::high}) {
        test_context = std::string{"initial pair residual acceptance arc="} +
            (arc == Arc::low ? "low" : "high");
        const double elevation = flat_trajectory_elevation(1800, arc);
        const double vertical = std::sin(elevation);
        const double horizontal = std::cos(elevation);
        const double nominal_angle = std::acos(vertical * vertical);
        const auto observed_second = [&](double residual_deg) {
            const double observed_angle = nominal_angle +
                residual_deg * std::numbers::pi / 180.0;
            const double bearing = std::acos(
                (std::cos(observed_angle) - vertical * vertical) /
                (horizontal * horizontal));
            return Point{std::sin(bearing) * 18.0,
                         std::cos(bearing) * 18.0};
        };
        for (double residual : {0.5,
                                wardogs::maximum_initial_calibration_residual_deg - 1e-7,
                                wardogs::maximum_initial_calibration_residual_deg}) {
            try {
                const auto accepted = wardogs::calibrate_platform(
                    {0, 0}, {{0, 18}, {0, 18}, arc},
                    {{18, 0}, observed_second(residual), arc});
                close(accepted.pair_angle_residual_deg, residual,
                      "compatible noisy initial pairs retain their measured residual", 1e-9);
            } catch (...) {
                check(false, "the inclusive initial quality boundary accepts both arcs");
            }
        }
        for (double residual : {
                 wardogs::maximum_initial_calibration_residual_deg + 1e-7, 6.5}) {
            rejects([&] {
                (void)wardogs::calibrate_platform(
                    {0, 0}, {{0, 18}, {0, 18}, arc},
                    {{18, 0}, observed_second(residual), arc});
            }, "valid bearings do not admit an incompatible initial pair on either arc");
        }
    }

    test_context = "height-aware two-shot platform rotation";
    const Point raised_first_aim{50, 68};
    const Point lowered_second_aim{70, 50};
    constexpr double first_aim_height = 80.0;
    constexpr double first_impact_height = 25.0;
    constexpr double second_aim_height = -35.0;
    constexpr double second_impact_height = -10.0;
    const Point raised_first_impact = impact_for_rotation(
        base, raised_first_aim, Arc::low, rotation, first_aim_height,
        first_impact_height);
    const Point lowered_second_impact = impact_for_rotation(
        base, lowered_second_aim, Arc::high, rotation, second_aim_height,
        second_impact_height);
    const auto varied_height = [&](Point point) -> std::optional<double> {
        if (point == base) return 0.0;
        if (point == raised_first_aim) return first_aim_height;
        if (point == raised_first_impact) return first_impact_height;
        if (point == lowered_second_aim) return second_aim_height;
        if (point == lowered_second_impact) return second_impact_height;
        return std::nullopt;
    };
    const auto height_calibration = wardogs::calibrate_platform(
        base,
        {raised_first_aim, raised_first_impact, Arc::low},
        {lowered_second_aim, lowered_second_impact, Arc::high},
        varied_height);
    close(height_calibration.pair_angle_residual_deg, 0,
          "height-aware synthetic calibration closes in physical angle space",
          1e-8);
    const auto height_recovered = height_calibration.local_to_world(probe);
    for (std::size_t row = 0; row < 3; ++row) {
        double expected = 0;
        for (std::size_t column = 0; column < 3; ++column)
            expected += rotation[row][column] * probe[column];
        close(height_recovered[row], expected,
              "height-aware calibration recovers platform rotation", 1e-8);
    }

    test_context = "platform validity, transforms and tilt";
    const auto identity = wardogs::PlatformCalibration{
        wardogs::identity_rotation(), 0};
    rejects([&] { (void)wardogs::required_firing_angles(
        base, first_aim, {}, Arc::low); },
        "a zero calibration matrix cannot produce a plausible zero sight setting");
    rejects([&] { auto scaled = identity; scaled.rotation[0][0] = 2;
        (void)wardogs::refine_platform_calibration(scaled, {}); },
        "a scaled prior cannot masquerade as a platform rotation");
    rejects([&] { auto reflected = identity; reflected.rotation[0][0] = -1;
        (void)reflected.local_to_world(probe); },
        "a reflected coordinate system is not a valid platform rotation");
    rejects([&] { auto invalid = identity;
        invalid.pair_angle_residual_deg = std::numeric_limits<double>::quiet_NaN();
        (void)invalid.world_to_local(probe); },
        "non-finite calibration quality cannot reach a firing solution");
    rejects([&] { (void)identity.local_to_world({0, 0, std::numeric_limits<double>::infinity()}); },
        "non-finite direction components are rejected at the transform boundary");
    for (double tilt : {-20.0, -5.0, 0.0, 8.0, 20.0}) {
        const wardogs::PlatformCalibration rigid{rotation_x(tilt), 0};
        const auto world = rigid.local_to_world(probe);
        const auto local = rigid.world_to_local(world);
        for (std::size_t index = 0; index < 3; ++index)
            close(local[index], probe[index], "proper platform rotations invert by transpose", 1e-12);
        close(std::hypot(world[0], world[1], world[2]), 1,
              "platform rotation preserves direction length", 1e-12);
    }
    const auto flat = wardogs::corrected_solution(
        {20, 20}, {20, 38}, identity, Arc::low);
    const auto uphill = wardogs::corrected_solution(
        {20, 20}, {20, 38}, identity, Arc::low, 100);
    close(flat.reticle_distance_m, 1800, "flat table remains unchanged");
    check(uphill.reticle_distance_m > flat.reticle_distance_m,
          "uphill low solution increases sight distance");

    const auto tilted_solution = wardogs::corrected_solution(
        {20, 20}, {20, 38}, {rotation_x(5), 0.0}, Arc::low);
    const double desired_world_elevation =
        flat_trajectory_elevation(1800.0, Arc::low);
    const double required_local_elevation =
        desired_world_elevation - 5.0 * std::numbers::pi / 180.0;
    const double required_equivalent_range = wardogs::sph2_maximum_range_m *
        std::sin(2.0 * required_local_elevation);
    close(tilted_solution.mil,
          wardogs::sph2_mil_for_distance(required_equivalent_range, Arc::low),
          "platform tilt rotates physical launch elevation before sight lookup",
          1e-8);

    test_context = "platform refinement and extreme direction scales";
    std::vector<wardogs::DirectionObservation> observations;
    for (double bearing : {0.0, 45.0, 90.0, 180.0, 270.0}) {
        const auto local = direction_from_bearing_and_elevation(
            bearing, 35.0 * std::numbers::pi / 180.0);
        wardogs::Vector3 world{};
        for (std::size_t row = 0; row < 3; ++row)
            for (std::size_t column = 0; column < 3; ++column)
                world[row] += rotation[row][column] * local[column];
        observations.push_back({local, world, 1.0});
    }
    observations.push_back({
        direction_from_bearing_and_elevation(225.0, 0.6),
        direction_from_bearing_and_elevation(20.0, 1.2), 0.05});
    const wardogs::PlatformCalibration refined =
        wardogs::refine_platform_calibration(
            {wardogs::identity_rotation(), 0.0}, observations, 1.5);
    auto large_observations = observations;
    auto small_observations = observations;
    for (auto& observation : large_observations) {
        observation.weight *= 1e300;
        for (auto* direction : {&observation.local_direction, &observation.world_direction})
            for (double& value : *direction) value *= 1e300;
    }
    for (auto& observation : small_observations)
        for (auto* direction : {&observation.local_direction, &observation.world_direction})
            for (double& value : *direction) value *= 1e-300;
    const auto scaled_refined = wardogs::refine_platform_calibration(identity, large_observations, 1.5e300);
    const auto tiny_refined = wardogs::refine_platform_calibration(identity, small_observations, 1.5);
    for (std::size_t row = 0; row < 3; ++row)
        for (std::size_t column = 0; column < 3; ++column) {
            close(scaled_refined.rotation[row][column], refined.rotation[row][column],
                  "common weight/direction scaling preserves calibration", 1e-10);
            close(tiny_refined.rotation[row][column], refined.rotation[row][column],
                  "tiny nonzero observation magnitudes preserve calibration", 1e-10);
        }
    for (double weight_scale : {1e-20, 1e-80, 1e-300, 1e20, 1e80}) {
        auto scaled = observations;
        for (auto& observation : scaled) observation.weight *= weight_scale;
        try {
            const auto normalized = wardogs::refine_platform_calibration(
                identity, scaled, 1.5 * weight_scale);
            for (std::size_t row = 0; row < 3; ++row)
                for (std::size_t column = 0; column < 3; ++column)
                    close(normalized.rotation[row][column], refined.rotation[row][column],
                          "all common positive weight scales preserve the same refinement", 1e-10);
        } catch (...) {
            check(false, "small finite weights cannot falsely make a stable refinement singular");
        }
    }
    const auto refined_probe = refined.local_to_world(probe);
    double refined_error = 0.0;
    double prior_error = 0.0;
    for (std::size_t row = 0; row < 3; ++row) {
        double expected = 0.0;
        for (std::size_t column = 0; column < 3; ++column)
            expected += rotation[row][column] * probe[column];
        refined_error += std::abs(refined_probe[row] - expected);
        prior_error += std::abs(probe[row] - expected);
    }
    check(refined_error < prior_error * 0.65,
          "weighted direction observations refine the global platform rotation");

    test_context = "calibration geometry and supported nominal settings";
    const Point near_base{96.08, 108.81};
    try {
        const auto near = wardogs::calibrate_platform(
            near_base,
            {{80.87, 108.82}, {82.01, 119.01}, Arc::high},
            {{95.06, 93.16}, {96.29, 103.28}, Arc::high});
        check(std::isfinite(near.pair_angle_residual_deg),
              "near-vehicle high impacts calibrate");
    } catch (...) {
        check(false, "near-vehicle high impacts calibrate");
    }

    rejects(
        [] {
            const Point origin{0, 0};
            (void)wardogs::calibrate_platform(
                origin, {{0, 18}, {0, 18}, Arc::low},
                {{std::sin(29.9 * std::numbers::pi / 180.0) * 18,
                  std::cos(29.9 * std::numbers::pi / 180.0) * 18},
                 {18, 0}, Arc::low});
        },
        "calibration requires 30 degree separation");
    const auto calibration_point = [](double bearing, double range_m) {
        const double radians = bearing * std::numbers::pi / 180.0;
        return Point{std::sin(radians) * range_m / 100.0,
                     std::cos(radians) * range_m / 100.0};
    };
    close(wardogs::calibration_aim_separation_deg({0, 0}, calibration_point(350, 1800),
                                                calibration_point(20, 1800)),
          30.0, "calibration eligibility uses the shortest angle across north");
    close(wardogs::calibration_aim_separation_deg({0, 0}, {0, 18}, {0, 20}),
          0.0, "targets along the same direction cannot form an eligible pair");
    check(!wardogs::valid_calibration_aim_separation(0.0) &&
          !wardogs::valid_calibration_aim_separation(180.0) &&
          !wardogs::valid_calibration_aim_separation(std::numeric_limits<double>::quiet_NaN()) &&
          !wardogs::valid_calibration_aim_separation(std::numeric_limits<double>::infinity()),
          "invalid calibration separation never advertises an eligible target");
    rejects([] { (void)wardogs::calibration_aim_separation_deg({0, 0}, {0, 0}, {18, 0}); },
            "eligibility rejects a first target at the gun position");
    rejects([] { (void)wardogs::calibration_aim_separation_deg({0, 0}, {0, 18}, {0, 0}); },
            "eligibility rejects a second target at the gun position");
    for (Arc arc : {Arc::low, Arc::high}) {
        rejects([&] {
            (void)wardogs::calibrate_platform(
                {0, 0}, {{0, 1}, {0, 1}, arc}, {{18, 0}, {18, 0}, arc});
        }, "a nominal calibration shot below the sight minimum is rejected");
        rejects([&] {
            (void)wardogs::calibrate_platform(
                {0, 0}, {{0, 18}, {0, 18}, arc}, {{1, 0}, {1, 0}, arc});
        }, "the second nominal calibration shot also requires a supported sight setting");
        const double aim_range = arc == Arc::low ? 1200.0 : 800.0;
        const double impact_range = arc == Arc::low ? 1150.0 : 700.0;
        try {
            const auto undershoot = wardogs::calibrate_platform(
                {0, 0}, {{0, aim_range / 100.0}, {0, impact_range / 100.0}, arc},
                {{18, 0}, {18, 0}, arc});
            check(std::isfinite(undershoot.pair_angle_residual_deg),
                  "a real undershoot below the sight minimum remains calibration evidence");
        } catch (...) {
            check(false, "valid sight aim and physical undershoot must calibrate on both arcs");
        }
        const Point elevated_aim{0, arc == Arc::low ? 11.0 : 7.0};
        const double elevation = arc == Arc::low ? 100.0 : 500.0;
        const auto lookup = [&](Point point) -> std::optional<double> {
            return point == elevated_aim ? elevation : 0.0;
        };
        try {
            const auto elevated = wardogs::calibrate_platform(
                {0, 0}, {elevated_aim, elevated_aim, arc},
                {{18, 0}, {18, 0}, arc}, lookup);
            close(elevated.pair_angle_residual_deg, 0,
                  "height-aware nominal shot checks equivalent sight range, not flat distance");
            const auto expected = wardogs::identity_rotation();
            for (std::size_t row = 0; row < 3; ++row)
                for (std::size_t column = 0; column < 3; ++column)
                    close(elevated.rotation[row][column], expected[row][column],
                          "height-aware exact nominal hits preserve the identity platform", 1e-12);
        } catch (...) {
            check(false, "height can make an otherwise short nominal sight aim reachable");
        }
        for (double separation : {30.0, 150.0}) {
            for (int bearing = 0; bearing < 360; ++bearing) {
                const auto first_point = calibration_point(bearing, 1800);
                const auto second_point = calibration_point(bearing + separation, 1800);
                check(wardogs::valid_calibration_aim_separation(
                          wardogs::calibration_aim_separation_deg({0, 0}, first_point, second_point)),
                      "eligibility and solver share inclusive separation endpoints across the compass");
                try {
                    const auto boundary = wardogs::calibrate_platform(
                        {0, 0}, {first_point, first_point, arc},
                        {second_point, second_point, arc});
                    close(boundary.pair_angle_residual_deg, 0,
                          "inclusive calibration separation endpoints work across the compass");
                } catch (...) {
                    check(false, "roundoff cannot reject an exact 30/150-degree calibration pair");
                }
            }
        }
    }
    for (double separation : {29.9999995, 150.0000005}) {
        rejects([&] {
            const auto first_point = calibration_point(0, 1800);
            const auto second_point = calibration_point(separation, 1800);
            check(!wardogs::valid_calibration_aim_separation(
                      wardogs::calibration_aim_separation_deg({0, 0}, first_point, second_point)),
                  "eligibility rejects genuinely out-of-range separation just like the solver");
            (void)wardogs::calibrate_platform(
                {0, 0}, {first_point, first_point, Arc::low},
                {second_point, second_point, Arc::low});
        }, "numerical endpoint allowance cannot accept a genuinely out-of-range separation");
    }
    rejects([] { (void)wardogs::calibrate_platform(
        {0, 0}, {{0, 18}, {0, 18}, Arc::low}, {{18, 0}, {0, 18}, Arc::low}); },
        "coincident observed directions cannot determine a platform rotation");

    if (failures) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All vehicle ballistics tests passed\n";
    return 0;
}

int main() {
    try {
        return run_tests();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: unexpected exception phase=" << test_context
                  << " error=" << error.what() << '\n';
        return 1;
    }
}
