#include "wardogs/continuous_calibration.hpp"

#include <array>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string_view>

namespace {
using namespace wardogs;
int checks{}, failures{};

void check(bool good, const char* message) {
    ++checks;
    if (!good) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}

void close(double actual, double expected, const char* message, double tolerance = 1e-8) {
    check(std::isfinite(actual) && std::abs(actual - expected) <= tolerance, message);
}

void rejects(const std::function<void()>& action, const char* message) {
    bool rejected = false;
    try { action(); } catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, message);
}

constexpr auto empirical = Sph2HeightPolicy::empirical_flat_low;
constexpr auto retained = Sph2HeightPolicy::retained_approximation;
const Point base{};
const PlatformCalibration identity{identity_rotation(), 0};

Point north(double metres) { return {0, metres / 100.0}; }

// Independently specified portions of the retained observed sight table.
// Synthetic impacts below need only these four intervals, not a production
// inverse that could conceal a shared error in the calibration implementation.
double observed_test_range(double mil) {
    struct Interval { double min_mil, max_mil, min_range, max_range; };
    constexpr std::array<Interval, 4> intervals{{
        {110, 120, 1622, 1666}, {160, 170, 1832, 1870},
        {200, 210, 1979, 2014}, {230, 240, 2079, 2110}}};
    for (const auto& item : intervals) {
        if (mil >= item.min_mil - 1e-8 && mil <= item.max_mil + 1e-8)
            return item.min_range + (mil - item.min_mil) *
                (item.max_range - item.min_range) / (item.max_mil - item.min_mil);
    }
    throw std::logic_error("Synthetic impact lies outside its independent fixture intervals");
}

Point landing_with_bias(const FiringSnapshot& firing, double bias) {
    close(firing.bearing_deg, 0, "northward fixture has no concealed bearing correction");
    return north(observed_test_range(firing.mil - bias));
}

void primary_policy() {
    // Anonymized regression: only distance and actual target-minus-gun height
    // are retained. These constants prove the software policy change, not a hit.
    constexpr double distance = 2384.190638;
    constexpr double height = -63.45;
    const auto target = north(distance);
    const auto old = corrected_solution(base, target, identity, Arc::low, height, retained);
    const auto first = corrected_solution(base, target, identity, Arc::low, height, empirical);
    close(old.mil, 302.7662207154182, "retained model preserves its downhill regression value", 1e-7);
    // The observed table brackets this distance with (2383,350), (2403,360).
    close(first.mil, 350.595319, "first low shot uses observed distance interpolation", 1e-7);
    close(first.mil - old.mil, 47.8290982845818,
          "no universal fifty-MIL offset is introduced", 1e-7);
    close(first.reticle_distance_m, distance, "empirical first shot retains horizontal table distance");
    close(first.bearing_deg, old.bearing_deg, "height policy leaves target azimuth unchanged");

    for (const double d : {1400., 1800., 2000., 2300., 2500.}) {
        for (const double h : {-100., -20., 0., 20., 100.}) {
            const auto low = corrected_solution(base, north(d), identity, Arc::low, h, empirical);
            close(low.mil, sph2_mil_for_distance(d, Arc::low),
                  "all first-shot low heights use the observed flat table");
            const auto zero = corrected_solution(base, north(d), identity, Arc::low, 0, retained);
            close(low.mil, zero.mil, "zero-height low legacy result is unchanged");
        }
    }
    for (const double h : {-100., -20., 0., 20., 100.}) {
        const auto before = corrected_solution(base, north(2000), identity, Arc::high, h, retained);
        const auto after = corrected_solution(base, north(2000), identity, Arc::high, h, empirical);
        close(after.mil, before.mil, "high-arc height calculations remain unchanged");
        close(after.bearing_deg, before.bearing_deg, "high-arc azimuth remains unchanged");
    }
    const auto implicit = corrected_solution(base, target, identity, Arc::low, height);
    close(implicit.mil, old.mil, "existing mathematical API callers retain legacy defaults");
    const auto angles = required_firing_angles(base, target, identity, Arc::low, height, empirical);
    close(angles.mil, first.mil, "explicit firing-angle API uses the same first-shot policy");
    const auto world = impact_direction(base, target, Arc::low, height, empirical);
    const auto level = impact_direction(base, target, Arc::low, 0, retained);
    for (std::size_t i = 0; i < world.size(); ++i)
        close(world[i], level[i], "impact geometry shares the empirical low policy");
    check(sph2_table_profile(Arc::low, empirical) != sph2_table_profile(Arc::low, retained),
          "diagnostics distinguish an empirical low command from retained height modelling");
    check(sph2_table_profile(Arc::high, empirical) == sph2_table_profile(Arc::high, retained),
          "unchanged high arc retains its existing model profile");
}

void local_learning() {
    ContinuousCalibration calibration(base, identity, ContinuousCorrectionMode::local_only,
                                      false, false, empirical);
    const auto target = north(2000);
    constexpr double height = -63.45;
    const auto first = calibration.firing_snapshot(target, Arc::low, height);
    close(first.mil, 206, "uncalibrated session starts on the empirical table");
    close(first.target_height_delta_m, height, "snapshot retains actual height despite flat command policy");
    check(calibration.height_policy() == empirical, "session exposes the selected calculation policy");
    const auto impact = landing_with_bias(first, 40);
    calibration.add_landing(first, impact, -70);
    close(calibration.solution(target, Arc::low, height).mil, 246,
          "first impact learns the full absolute correction against the displayed baseline");
    for (int shot = 0; shot < 3; ++shot) {
        const auto firing = calibration.firing_snapshot(target, Arc::low, height);
        close(firing.mil, 246, "corrected shot does not reintroduce the hidden height model");
        close(firing.target_height_delta_m, height, "corrected snapshots keep real terrain metadata");
        const auto hit = landing_with_bias(firing, 40);
        close(hit.y, target.y, "independent synthetic corrected shot reaches the requested point");
        calibration.add_landing(firing, hit, height);
        close(calibration.solution(target, Arc::low, height).mil, 246,
              "repeated hits do not apply an already learned correction twice");
    }
    close(calibration.global_rotation_adjustment_deg(), 0,
          "empirical local learning cannot fit an unobserved global platform rotation");
    const auto near = calibration.solution(north(2010), Arc::low, height);
    const auto near_base = corrected_solution(base, north(2010), identity, Arc::low, height, empirical);
    check(near.mil > near_base.mil && near.mil < near_base.mil + 40,
          "nearby targets receive a fading local correction");
    const auto far = calibration.solution(north(2100), Arc::low, height);
    close(far.mil, sph2_mil_for_distance(2100, Arc::low),
          "far targets retain their own empirical baseline");
    const auto high = calibration.solution(target, Arc::high, height);
    close(high.mil, corrected_solution(base, target, identity, Arc::high, height, retained).mil,
          "low-arc correction does not contaminate high-arc height handling");
    const auto other_height = calibration.solution(target, Arc::low, height + 600);
    close(other_height.mil, 206, "real height still limits local evidence transfer", 0.001);
    calibration.clear();
    close(calibration.solution(target, Arc::low, height).mil, 206,
          "clear restores empirical baseline instead of legacy height compensation");
    close(calibration.firing_snapshot(target, Arc::low, height).target_height_delta_m, height,
          "clear never replaces real terrain metadata with effective zero height");
}

void range_prediction_height_bounds() {
    ContinuousCalibration calibration(base, identity, ContinuousCorrectionMode::local_only,
                                      false, true, empirical);
    constexpr double height = -63;
    for (const double range : {1800., 2000., 2200.}) {
        const auto firing = calibration.firing_snapshot(north(range), Arc::low, height);
        calibration.add_landing(firing, landing_with_bias(firing, 40), height - 2);
    }
    const auto target = north(1900);
    const auto same = calibration.assess(target, Arc::low, height);
    check(same.range_prediction_applied, "independent same-height targets can support provisional MIL transfer");
    close(calibration.solution(target, Arc::low, height).mil,
          sph2_mil_for_distance(1900, Arc::low) + 40,
          "supported provisional correction is applied once to the empirical baseline", 1e-6);
    const auto changed_height = calibration.assess(target, Arc::low, height + 50);
    check(!changed_height.range_prediction_applied,
          "flat low commands do not erase actual-height support limits for prediction");
    close(calibration.solution(target, Arc::low, height + 50).mil,
          sph2_mil_for_distance(1900, Arc::low),
          "unsupported-height target receives no transferred MIL correction");
    check(!calibration.assess(target, Arc::high, height).range_prediction_applied,
          "provisional transfer remains arc-specific");
}

void invalid_and_legacy() {
    const auto target = north(2000);
    const auto invalid = static_cast<Sph2HeightPolicy>(77);
    for (const auto policy : {retained, empirical}) {
        for (const double height : {std::numeric_limits<double>::quiet_NaN(),
                                    std::numeric_limits<double>::infinity()}) {
            rejects([&] { (void)corrected_solution(base, target, identity, Arc::low, height, policy); },
                    "unused or used non-finite height is always rejected");
            rejects([&] { (void)impact_direction(base, target, Arc::low, height, policy); },
                    "observation geometry rejects non-finite actual height");
        }
    }
    rejects([&] { (void)required_firing_angles(base, target, identity, Arc::low, 0, invalid); },
            "unknown first-shot policy cannot silently choose a model");
    rejects([&] { (void)corrected_solution(base, target, identity, Arc::high, 0, invalid); },
            "high-arc compatibility does not hide invalid policy values");
    rejects([&] { (void)impact_direction(base, target, Arc::low, 0, invalid); },
            "impact observations reject unknown policies");
    rejects([&] { ContinuousCalibration value(base, identity, ContinuousCorrectionMode::local_only,
                                             false, false, invalid); },
            "sessions reject unknown policies before retaining observations");
    rejects([&] { ContinuousCalibration value(base, identity, ContinuousCorrectionMode::platform_refinement,
                                             false, false, empirical); },
            "empirical policy does not enable an unvalidated global platform fit");
    const PlatformCalibration rotated{{{{0, -1, 0}, {1, 0, 0}, {0, 0, 1}}}, 0};
    rejects([&] { ContinuousCalibration value(base, rotated, ContinuousCorrectionMode::local_only,
                                             false, false, empirical); },
            "empirical sessions reject nonidentity platform baselines");
    ContinuousCalibration legacy(base, identity);
    check(legacy.height_policy() == retained, "existing session constructors retain their legacy policy");
    close(legacy.solution(target, Arc::low, -40).mil,
          corrected_solution(base, target, identity, Arc::low, -40, retained).mil,
          "existing session calculations remain compatible");
}

void bearing_prediction_height_bounds() {
    ContinuousCalibration calibration(base, identity, ContinuousCorrectionMode::local_only,
                                      true, false, empirical);
    constexpr double height = -63;
    constexpr double bearing_bias = 1;
    const double radians = -bearing_bias * std::numbers::pi / 180.0;
    for (const double range : {1800., 2000., 2200.}) {
        const auto firing = calibration.firing_snapshot(north(range), Arc::low, height);
        const Point impact{std::sin(radians) * range / 100.0, std::cos(radians) * range / 100.0};
        calibration.add_landing(firing, impact, height - 2);
    }
    const auto target = north(1900);
    check(calibration.assess(target, Arc::low, height).bearing_prediction_applied,
          "same-height independent targets support provisional bearing transfer");
    close(calibration.solution(target, Arc::low, height).bearing_deg, bearing_bias,
          "supported empirical bearing transfer is applied once", 1e-6);
    check(!calibration.assess(target, Arc::low, height + 50).bearing_prediction_applied,
          "empirical low does not erase actual-height bounds for bearing prediction");
    close(calibration.solution(target, Arc::low, height + 50).bearing_deg, 0,
          "unsupported-height target receives no transferred azimuth");
}
}  // namespace

int main() {
    try {
        primary_policy();
        local_learning();
        range_prediction_height_bounds();
        bearing_prediction_height_bounds();
        invalid_and_legacy();
    } catch (const std::exception& error) {
        ++failures;
        std::cerr << "EXCEPTION: " << error.what() << '\n';
    }
    std::cout << checks << " SPH-2 first-shot checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
