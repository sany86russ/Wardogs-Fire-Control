#include "wardogs/firing_analysis.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>

namespace {

int failures{};

void check(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void close(double actual, double expected, const char* message, double tolerance = 1e-8) {
    check(std::isfinite(actual) && std::abs(actual - expected) <= tolerance, message);
}

void rejects(const std::function<void()>& action, const char* message) {
    try {
        action();
        check(false, message);
    } catch (const std::invalid_argument&) {
    }
}

wardogs::FiringAnalysisRequest request(wardogs::AnalysisWeapon weapon, double distance,
                                       wardogs::Arc arc = wardogs::Arc::low) {
    wardogs::FiringAnalysisRequest value;
    value.weapon = weapon;
    value.arc = arc;
    value.base = {0.0, 0.0};
    value.target = {distance / 100.0, 0.0};
    value.ammunition_id = weapon == wardogs::AnalysisWeapon::sph2 ? "sph2-standard" : "l81-standard";
    value.game_version = "test-game-version";
    return value;
}

wardogs::FlightTimeProfile profile(wardogs::AnalysisWeapon weapon = wardogs::AnalysisWeapon::sph2,
                                   wardogs::Arc arc = wardogs::Arc::low) {
    // Synthetic observations test interpolation/provenance, not game physics.
    return {weapon, arc, 0.0,
        weapon == wardogs::AnalysisWeapon::sph2 ? "sph2-standard" : "l81-standard",
        "test-game-version", "Synthetic test observations",
        {{1500.0, 12.0, 0.2}, {2000.0, 16.0, 0.4}}};
}

void default_models() {
    using namespace wardogs;
    const auto low = analyze_firing(request(AnalysisWeapon::sph2, 1800.0));
    const auto high = analyze_firing(request(AnalysisWeapon::sph2, 1800.0, Arc::high));
    check(low.status == FiringAnalysisStatus::available && low.nominal_mil && low.trajectory,
          "default SPH-2 exposes nominal guidance and retained geometry");
    check(high.status == FiringAnalysisStatus::available && high.nominal_mil && high.trajectory,
          "both SPH-2 branches have geometry within retained table coverage");
    check(!low.flight_time && !high.flight_time && !low.trajectory->model_flight_time_s,
          "a range/MIL table alone never invents flight seconds");
    check(!low.trajectory->assumed_speed_mps && !low.trajectory->assumed_gravity_mps2,
          "default retained geometry does not invent speed or game gravity");
    check(low.height_source == HeightSource::assumed_level &&
          low.clearance.status == TerrainClearanceStatus::not_checked,
          "level-ground assumption and absence of terrain checks are explicit");
    close(*low.nominal_mil, sph2_mil_for_distance(1800.0, Arc::low),
          "nominal low MIL remains the retained game table");
    close(*high.nominal_mil, sph2_mil_for_distance(1800.0, Arc::high),
          "nominal high MIL remains the retained game table");
    const double low_angle = 0.5 * std::asin(1800.0 / 2629.0);
    close(low.trajectory->elevation_rad, low_angle,
          "low elevation agrees with an independent flat parabola solution");
    close(high.trajectory->elevation_rad, std::numbers::pi / 2.0 - low_angle,
          "high elevation agrees with the independent complementary solution");
    close(low.trajectory->apex_distance_m, 900.0, "flat trajectory apex is halfway to the target");
    check(high.trajectory->apex_height_above_muzzle_m > low.trajectory->apex_height_above_muzzle_m,
          "high arc apex exceeds low arc apex");
    check(low.trajectory->samples.front().position == Point{} &&
          low.trajectory->samples.back().position == Point{18.0, 0.0},
          "trajectory contains exact gun and target endpoints");
    close(low.trajectory->samples.front().height_above_muzzle_m, 0.0,
          "trajectory starts at the muzzle");
    close(low.trajectory->samples.back().height_above_muzzle_m, 0.0,
          "level trajectory ends at the target height");
    for (const auto& sample : low.trajectory->samples) {
        const double expected = sample.distance_m * std::tan(low_angle) -
            sample.distance_m * sample.distance_m / (2.0 * 2629.0 * std::pow(std::cos(low_angle), 2));
        close(sample.height_above_muzzle_m, expected,
              "sampled geometry agrees with the independent ballistic equation");
        check(!sample.model_time_s, "default geometric samples carry no fabricated timestamps");
    }
    const auto mortar = analyze_firing(request(AnalysisWeapon::l81, 300.0, Arc::high));
    close(*mortar.nominal_mil, 690.0, "default L81 retains the 300-metre/690-MIL table row");
    check(!mortar.trajectory && !mortar.flight_time && mortar.trajectory_status == TrajectoryStatus::unavailable,
          "default L81 does not derive physical elevation or flight time from MIL");
    check(analyze_firing(request(AnalysisWeapon::l81, 80.0)).status == FiringAnalysisStatus::unsupported_range,
          "external L81 extension rows do not expand the supported minimum to 80 metres");
    check(analyze_firing(request(AnalysisWeapon::l81, 684.1)).status == FiringAnalysisStatus::unsupported_range,
          "L81 remains strictly bounded above 684 metres");
    check(analyze_firing(request(AnalysisWeapon::sph2, 1000.0)).status == FiringAnalysisStatus::unsupported_range,
          "low SPH-2 table coverage is not expanded to the high arc minimum");
    const auto maximum = analyze_firing(request(AnalysisWeapon::sph2, 2629.0, Arc::high));
    close(*maximum.nominal_mil, 610.0, "duplicate high maximum preserves the original first table row");
    close(maximum.trajectory->apex_height_above_muzzle_m, 2629.0 / 4.0,
          "maximum-range geometric apex is independently R/4");
}

void assumed_physics() {
    using namespace wardogs;
    auto low_request = request(AnalysisWeapon::sph2, 2000.0);
    low_request.gravity_mps2 = 9.80665;  // Deliberate test assumption, not game data.
    auto high_request = low_request;
    high_request.arc = Arc::high;
    const auto low = analyze_firing(low_request);
    const auto high = analyze_firing(high_request);
    close(low.flight_time->seconds, 13.717419388977392,
          "assumed SPH-2 low time agrees with an independent vacuum calculation", 1e-7);
    close(high.flight_time->seconds, 29.734928533199753,
          "assumed SPH-2 high time agrees with an independent vacuum calculation", 1e-7);
    check(low.flight_time->source.basis == EstimateBasis::assumed_vacuum_model &&
          !low.flight_time->uncertainty_s,
          "assumed physics has explicit provenance and no invented error bound");
    close(*low.trajectory->assumed_speed_mps, std::sqrt(2629.0 * 9.80665),
          "SPH-2 speed is an explicit consequence of R and assumed gravity");
    close(*low.trajectory->samples.back().model_time_s, low.flight_time->seconds,
          "model timestamp at the target agrees with its own total time");
    auto doubled_g = low_request;
    doubled_g.gravity_mps2 = 4.0 * *low_request.gravity_mps2;
    const auto faster = analyze_firing(doubled_g);
    close(faster.flight_time->seconds, low.flight_time->seconds / 2.0,
          "changing gravity scales time even when the same R fixes all spatial geometry");
    close(faster.trajectory->apex_height_above_muzzle_m, low.trajectory->apex_height_above_muzzle_m,
          "assumed gravity does not silently change retained SPH-2 geometric shape");
    close(*faster.nominal_mil, *low.nominal_mil,
          "assumed flight clock never changes authoritative sight guidance");

    auto elevated_request = low_request;
    elevated_request.height_delta_m = 50.0;
    const auto elevated = analyze_firing(elevated_request);
    const double root = std::sqrt(2629.0 * 2629.0 - 2000.0 * 2000.0 - 2.0 * 2629.0 * 50.0);
    const double angle = std::atan((2629.0 - root) / 2000.0);
    close(elevated.trajectory->elevation_rad, angle,
          "explicit target height participates in the independent elevation solution");
    close(elevated.trajectory->samples.back().height_above_muzzle_m, 50.0,
          "elevated trajectory exactly reaches the specified target height");
    close(elevated.flight_time->seconds, 2000.0 / (std::sqrt(2629.0 * 9.80665) * std::cos(angle)),
          "height-aware time uses the correct horizontal velocity");
    close(*elevated.nominal_mil, sph2_mil_for_trajectory(2000.0, 50.0, Arc::low),
          "height-aware sight guidance still uses the existing retained calculation");
    auto impossible = request(AnalysisWeapon::sph2, 2600.0, Arc::high);
    impossible.height_delta_m = 500.0;
    check(analyze_firing(impossible).status == FiringAnalysisStatus::unreachable,
          "an unreachable elevated target cannot receive a made-up path or guidance");

    auto mortar_request = request(AnalysisWeapon::l81, 300.0, Arc::high);
    mortar_request.vacuum_profile = VacuumFlightProfile{100.0, 10.0, "Synthetic user assumption"};
    const auto mortar = analyze_firing(mortar_request);
    const double mortar_angle = (std::numbers::pi - std::asin(0.3)) / 2.0;
    close(mortar.trajectory->elevation_rad, mortar_angle,
          "L81 user vacuum profile solves its own elevation without converting MIL to radians");
    close(mortar.flight_time->seconds, 300.0 / (100.0 * std::cos(mortar_angle)),
          "explicit L81 speed and gravity give a separately labelled model time");
    close(*mortar.nominal_mil, 690.0, "L81 model preserves the retained nominal MIL");
    check(mortar.flight_time->source.source == "Synthetic user assumption",
          "L81 assumption provenance survives in the flight result");
    mortar_request.vacuum_profile->speed_mps = 10.0;
    const auto unsupported_model = analyze_firing(mortar_request);
    check(unsupported_model.nominal_mil && !unsupported_model.trajectory && !unsupported_model.flight_time &&
          unsupported_model.trajectory_status == TrajectoryStatus::unreachable,
          "an unsuitable L81 assumed model cannot invalidate the valid retained sight table");
}

void terrain_checks() {
    using namespace wardogs;
    auto flat_request = request(AnalysisWeapon::sph2, 1800.0);
    flat_request.terrain = [](Point) -> std::optional<double> { return 100.0; };
    const auto flat = analyze_firing(flat_request);
    check(flat.height_source == HeightSource::terrain &&
          flat.clearance.status == TerrainClearanceStatus::clear_at_samples,
          "available ground data enables sampled clearance, with an explicit limited claim");
    close(*flat.height_delta_m, 0.0, "terrain endpoint subtraction supplies actual model height delta");
    check(flat.clearance.samples_missing == 0 && flat.clearance.minimum_clearance_m &&
          *flat.clearance.minimum_clearance_m > 0.0 && !flat.clearance.first_blocked_position,
          "expected ground contact at endpoints is excluded from interior obstruction checks");
    check(flat.clearance.samples_checked == flat.trajectory->samples.size() &&
          flat.clearance.actual_sample_step_m <= 2.0,
          "all available samples are checked at the requested spatial resolution");

    auto ridge_request = flat_request;
    ridge_request.terrain = [](Point point) -> std::optional<double> {
        return point.x >= 8.9 && point.x <= 9.1 ? 600.0 : 100.0;
    };
    const auto blocked_low = analyze_firing(ridge_request);
    ridge_request.arc = Arc::high;
    const auto clear_high = analyze_firing(ridge_request);
    check(blocked_low.clearance.status == TerrainClearanceStatus::blocked &&
          blocked_low.clearance.first_blocked_position && *blocked_low.clearance.minimum_clearance_m < 0.0,
          "a sampled ridge intersects the low trajectory");
    check(clear_high.clearance.status == TerrainClearanceStatus::clear_at_samples,
          "the same ridge lies below the high trajectory");
    check(*blocked_low.clearance.first_blocked_distance_m >= 890.0 &&
          *blocked_low.clearance.first_blocked_distance_m <= 910.0,
          "first obstruction is located in the actual ridge region");

    auto missing_request = flat_request;
    missing_request.terrain = [](Point point) -> std::optional<double> {
        if (point.x >= 5.0 && point.x <= 6.0) return std::nullopt;
        return 100.0;
    };
    const auto missing = analyze_firing(missing_request);
    check(missing.trajectory && missing.clearance.status == TerrainClearanceStatus::incomplete &&
          missing.clearance.samples_missing > 0,
          "interior coverage gaps do not receive zero heights or a clear status");
    check(std::any_of(missing.trajectory->samples.begin(), missing.trajectory->samples.end(),
        [](const TrajectorySample& sample) { return !sample.clearance_m; }),
        "missing ground remains absent in the plotted sample data");
    missing_request.terrain = [](Point point) -> std::optional<double> {
        if (point.x >= 5.0 && point.x <= 6.0) return std::nullopt;
        return point.x >= 8.9 && point.x <= 9.1 ? 600.0 : 100.0;
    };
    const auto missing_and_blocked = analyze_firing(missing_request);
    check(missing_and_blocked.clearance.status == TerrainClearanceStatus::blocked &&
          missing_and_blocked.clearance.samples_missing > 0,
          "known obstruction is retained even when other path sections lack terrain");

    auto missing_endpoint = flat_request;
    missing_endpoint.terrain = [](Point point) -> std::optional<double> {
        return point.x == 18.0 ? std::nullopt : std::optional<double>{100.0};
    };
    const auto unknown_height = analyze_firing(missing_endpoint);
    check(unknown_height.status == FiringAnalysisStatus::height_unavailable &&
          !unknown_height.nominal_mil && !unknown_height.trajectory && !unknown_height.height_delta_m,
          "missing target height cannot silently produce level-ground SPH-2 guidance");
    missing_endpoint.height_delta_m = 0.0;
    const auto explicit_height = analyze_firing(missing_endpoint);
    check(explicit_height.trajectory && explicit_height.clearance.status == TerrainClearanceStatus::incomplete,
          "an explicit height permits geometry while honestly preserving missing terrain coverage");

    auto sloped = flat_request;
    sloped.terrain = [](Point point) -> std::optional<double> { return 100.0 + point.x * 2.0; };
    sloped.muzzle_height_above_ground_m = 2.0;
    sloped.target_height_above_ground_m = 1.0;
    const auto slope = analyze_firing(sloped);
    close(*slope.height_delta_m, 35.0, "derived delta includes distinct muzzle and target offsets");
    close(*slope.trajectory->samples.front().clearance_m, 2.0,
          "launch endpoint uses muzzle height rather than assuming a ground-level barrel");
    close(*slope.trajectory->samples.back().clearance_m, 1.0,
          "target endpoint retains its stated height above the ground");
    auto buried_target = flat_request;
    buried_target.sample_step_m = 5000.0;
    buried_target.height_delta_m = -100.0;
    const auto buried = analyze_firing(buried_target);
    check(buried.clearance.status == TerrainClearanceStatus::blocked &&
          buried.clearance.first_blocked_distance_m && *buried.clearance.first_blocked_distance_m == 1800.0,
          "an explicitly buried target endpoint is not mistaken for expected ground contact");
    flat_request.clearance_margin_m = 1000.0;
    check(analyze_firing(flat_request).clearance.status == TerrainClearanceStatus::blocked,
          "explicit positive clearance margin participates in obstruction assessment");
    auto mortar = request(AnalysisWeapon::l81, 300.0);
    mortar.terrain = [](Point) -> std::optional<double> { return 50.0; };
    check(analyze_firing(mortar).clearance.status == TerrainClearanceStatus::model_unavailable,
          "L81 cannot claim terrain clearance without an explicit trajectory model");
}

void observed_times() {
    using namespace wardogs;
    auto exact_request = request(AnalysisWeapon::sph2, 1500.0);
    exact_request.flight_profile = profile();
    const auto exact = analyze_firing(exact_request);
    check(exact.flight_profile_status == FlightProfileStatus::matched && exact.flight_time &&
          exact.flight_time->source.basis == EstimateBasis::user_measurement,
          "an exact observation is identified as supplied measurement");
    close(exact.flight_time->seconds, 12.0, "exact flight observation retains its measured time");
    close(*exact.flight_time->uncertainty_s, 0.2, "exact observation retains the supplied uncertainty bound");
    check(exact.flight_time->source.game_version == "test-game-version" &&
          exact.flight_time->source.source == "Synthetic test observations" &&
          !exact.trajectory->samples.back().model_time_s,
          "observation provenance survives, without inventing intermediate timestamps");
    auto between_request = exact_request;
    between_request.target = {17.5, 0.0};
    const auto between = analyze_firing(between_request);
    close(between.flight_time->seconds, 14.0, "covered flight time is interpolated between declared observations");
    close(*between.flight_time->uncertainty_s, 0.3, "known absolute time bounds interpolate without inventing confidence");
    check(between.flight_time->source.basis == EstimateBasis::interpolated_user_measurement,
          "an interpolated observation is distinguished from an exact measurement");
    between_request.flight_profile->samples.back().uncertainty_s.reset();
    check(!analyze_firing(between_request).flight_time->uncertainty_s,
          "a missing bound at either endpoint does not become an invented zero bound");

    auto outside = exact_request;
    outside.target = {22.0, 0.0};
    const auto no_extrapolation = analyze_firing(outside);
    check(!no_extrapolation.flight_time && no_extrapolation.flight_profile_status == FlightProfileStatus::out_of_coverage,
          "a valid sight solution outside flight profile coverage gets no extrapolated seconds");
    auto different_height = exact_request;
    different_height.height_delta_m = 10.0;
    check(analyze_firing(different_height).flight_profile_status == FlightProfileStatus::height_mismatch &&
          !analyze_firing(different_height).flight_time,
          "flat observations cannot claim measured flight time for an elevated target");
    for (const int identity_change : {0, 1, 2, 3}) {
        auto mismatch = exact_request;
        if (identity_change == 0) mismatch.arc = Arc::high;
        if (identity_change == 1) mismatch.ammunition_id = "different-ammo";
        if (identity_change == 2) mismatch.game_version = "different-patch";
        if (identity_change == 3) mismatch.flight_profile->weapon = AnalysisWeapon::l81;
        const auto changed = analyze_firing(mismatch);
        check(!changed.flight_time && changed.flight_profile_status == FlightProfileStatus::identity_mismatch,
              "observations cannot transfer between arcs, ammunition, patches or weapons");
    }
    auto both_sources = exact_request;
    both_sources.gravity_mps2 = 9.80665;
    const auto observed_and_model = analyze_firing(both_sources);
    close(observed_and_model.flight_time->seconds, 12.0,
          "a matching observation takes priority over separately assumed model seconds");
    check(observed_and_model.flight_time->source.basis == EstimateBasis::user_measurement &&
          observed_and_model.trajectory->model_flight_time_s &&
          observed_and_model.trajectory->source.basis == EstimateBasis::assumed_vacuum_model,
          "measured total time does not certify the separate assumed geometry or model clock");
    auto single = exact_request;
    single.flight_profile->samples.resize(1);
    check(analyze_firing(single).flight_time.has_value(), "one observation applies exactly at its measured distance");
    single.target = {15.01, 0.0};
    check(!analyze_firing(single).flight_time, "one observation does not extrapolate over nearby targets");
    auto long_profile = exact_request;
    long_profile.flight_profile->samples.back().distance_m = 1e308;
    long_profile.target = {16.0, 0.0};
    check(analyze_firing(long_profile).flight_time->source.basis == EstimateBasis::interpolated_user_measurement,
          "a distant profile endpoint cannot inflate roundoff into a false exact measurement");
    auto mortar = request(AnalysisWeapon::l81, 300.0, Arc::high);
    mortar.flight_profile = profile(AnalysisWeapon::l81, Arc::high);
    mortar.flight_profile->samples = {{300.0, 8.0, std::nullopt}};
    const auto measured_mortar = analyze_firing(mortar);
    check(measured_mortar.flight_time && !measured_mortar.trajectory &&
          measured_mortar.flight_time->source.basis == EstimateBasis::user_measurement,
          "L81 can retain an observed total time without inventing a physical path");
}

void invalid_inputs() {
    using namespace wardogs;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    auto valid = request(AnalysisWeapon::sph2, 1800.0);
    for (const double bad : {0.0, -1.0, nan, infinity}) {
        auto bad_gravity = valid;
        bad_gravity.gravity_mps2 = bad;
        rejects([&] { (void)analyze_firing(bad_gravity); }, "invalid assumed gravity is rejected before analysis");
        auto bad_step = valid;
        bad_step.sample_step_m = bad;
        rejects([&] { (void)analyze_firing(bad_step); }, "invalid spatial step is rejected before sampling");
    }
    auto too_many_samples = valid;
    too_many_samples.sample_step_m = 0.001;
    rejects([&] { (void)analyze_firing(too_many_samples); }, "sampling budget cannot allocate unbounded trajectory data");
    auto bad_weapon = valid;
    bad_weapon.weapon = static_cast<AnalysisWeapon>(99);
    rejects([&] { (void)analyze_firing(bad_weapon); }, "unknown weapon cannot silently select SPH-2");
    auto bad_arc = valid;
    bad_arc.arc = static_cast<Arc>(99);
    rejects([&] { (void)analyze_firing(bad_arc); }, "unknown trajectory cannot silently select a branch");
    auto bad_height = valid;
    bad_height.height_delta_m = nan;
    rejects([&] { (void)analyze_firing(bad_height); }, "NaN height cannot enter trajectory geometry");
    auto bad_terrain = valid;
    bad_terrain.terrain = [=](Point) -> std::optional<double> { return nan; };
    rejects([&] { (void)analyze_firing(bad_terrain); }, "NaN terrain is an explicit data error rather than zero height");
    auto bad_point = valid;
    bad_point.base.x = infinity;
    rejects([&] { (void)analyze_firing(bad_point); }, "non-finite point is rejected by retained coordinate validation");
    auto wrong_model = valid;
    wrong_model.vacuum_profile = VacuumFlightProfile{100.0, 10.0, "Explicit assumption"};
    rejects([&] { (void)analyze_firing(wrong_model); }, "L81 speed profile cannot override SPH-2 retained geometry");
    auto wrong_gravity = request(AnalysisWeapon::l81, 300.0);
    wrong_gravity.gravity_mps2 = 9.80665;
    rejects([&] { (void)analyze_firing(wrong_gravity); }, "SPH-2 gravity parameter cannot invent L81 velocity");
    auto missing_provenance = request(AnalysisWeapon::l81, 300.0);
    missing_provenance.vacuum_profile = VacuumFlightProfile{100.0, 10.0, "   "};
    rejects([&] { (void)analyze_firing(missing_provenance); }, "assumed physics must retain its provenance");
    auto bad_margin = valid;
    bad_margin.clearance_margin_m = -1.0;
    rejects([&] { (void)analyze_firing(bad_margin); }, "a negative terrain margin cannot weaken obstruction checks");

    for (int scenario = 0; scenario < 12; ++scenario) {
        auto bad_profile = profile();
        switch (scenario) {
        case 0: bad_profile.samples.clear(); break;
        case 1: bad_profile.samples[1].distance_m = bad_profile.samples[0].distance_m; break;
        case 2: std::swap(bad_profile.samples[0], bad_profile.samples[1]); break;
        case 3: bad_profile.samples[0].seconds = 0.0; break;
        case 4: bad_profile.samples[0].distance_m = infinity; break;
        case 5: bad_profile.height_delta_m = nan; break;
        case 6: bad_profile.samples[0].uncertainty_s = -1.0; break;
        case 7: bad_profile.samples[0].uncertainty_s = 20.0; break;
        case 8: bad_profile.game_version.clear(); break;
        case 9: bad_profile.source = " \t\n"; break;
        case 10: bad_profile.ammunition_id.clear(); break;
        case 11: bad_profile.samples.resize(maximum_flight_time_samples + 1); break;
        }
        rejects([&] { validate_flight_time_profile(bad_profile); },
                "malformed observation profiles are rejected transactionally before use");
    }
}

void target_offsets() {
    using namespace wardogs;
    const auto north = offset_target({0.0, 0.0}, {0.0, 10.0}, 100.0, 200.0);
    close(north.x, 1.0, "right from north adds east by the correct metre-to-coordinate scale");
    close(north.y, 12.0, "add from north increases target distance northward");
    const auto east = offset_target({0.0, 0.0}, {10.0, 0.0}, 100.0, 200.0);
    close(east.x, 12.0, "add from east increases target distance eastward");
    close(east.y, -1.0, "right from east points south");
    const auto southwest = offset_target({3.0, 4.0}, {2.0, 3.0}, std::sqrt(2.0) * 100.0, 0.0);
    close(southwest.x, 1.0, "non-cardinal lateral offset follows the local right-hand frame");
    close(southwest.y, 4.0, "non-cardinal lateral offset does not alter longitudinal distance");
    const auto drop_left = offset_target({0.0, 0.0}, {0.0, 10.0}, -50.0, -100.0);
    close(drop_left.x, -0.5, "negative lateral offset means left");
    close(drop_left.y, 9.0, "negative longitudinal offset means drop/near");
    const Point unchanged{12.75, -6.5};
    check(offset_target({1.0, 2.0}, unchanged, 0.0, 0.0) == unchanged,
          "zero offsets preserve exact accepted coordinates");
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    rejects([] { (void)offset_target({1.0, 2.0}, {1.0, 2.0}, 1.0, 2.0); },
            "coincident gun and target cannot define a correction frame");
    rejects([&] { (void)offset_target({0.0, 0.0}, {1.0, 2.0}, nan, 0.0); },
            "NaN lateral correction cannot corrupt target coordinates");
    rejects([&] { (void)offset_target({0.0, 0.0}, {1.0, 2.0}, 0.0, infinity); },
            "infinite longitudinal correction cannot corrupt target coordinates");
    rejects([] { (void)offset_target({1.797e308, 1.0}, {1.797e308, 2.0}, 1e308, 0.0); },
            "target correction rejects overflow even for initially finite coordinates");
}

}  // namespace

int main() {
    try {
        default_models();
        assumed_physics();
        terrain_checks();
        observed_times();
        invalid_inputs();
        target_offsets();
    } catch (const std::exception& error) {
        std::cerr << "Unexpected failure: " << error.what() << '\n';
        return 1;
    }
    if (failures) {
        std::cerr << failures << " firing analysis tests failed\n";
        return 1;
    }
    std::cout << "All firing analysis tests passed\n";
    return 0;
}
