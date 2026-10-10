#include "wardogs/continuous_calibration.hpp"
#include "wardogs/presentation.hpp"

#include <array>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void close(double actual, double expected, const char* message) {
    check(std::abs(actual - expected) < 1e-8, message);
}

void rejects(const std::function<void()>& action, const char* message) {
    try {
        action();
        check(false, message);
    } catch (const std::invalid_argument&) {
    }
}

wardogs::Point point_for(double range_m, double bearing_deg) {
    const double bearing = bearing_deg * std::numbers::pi / 180.0;
    return {range_m * std::sin(bearing) / 100.0,
            range_m * std::cos(bearing) / 100.0};
}

void no_fictitious_correction(wardogs::Arc arc, double mil, double bearing_deg) {
    const wardogs::Point base{};
    wardogs::ContinuousCalibration calibration(
        base, {wardogs::identity_rotation(), 0.0},
        wardogs::ContinuousCorrectionMode::local_only);
    const auto target = point_for(wardogs::sph2_distance_for_mil(mil, arc), bearing_deg);
    const auto initial_solution = calibration.solution(target, arc);
    const auto initial_command = wardogs::displayed_firing_command(initial_solution);
    const auto ideal_landing = point_for(initial_command.table_distance_m,
                                         initial_command.bearing_deg);
    check(wardogs::calculate_shot(target, ideal_landing).distance > 1e-6,
          "fixture has a real geometric miss caused only by command rounding");
    calibration.add_landing(
        {target, arc, initial_command.bearing_deg, initial_command.mil, 0.0},
        ideal_landing);
    const auto next_solution = calibration.solution(target, arc);
    const auto next_command = wardogs::displayed_firing_command(next_solution);
    close(next_solution.bearing_deg, initial_solution.bearing_deg,
          "an ideal quantized landing does not teach a fictitious azimuth offset");
    close(next_solution.mil, initial_solution.mil,
          "an ideal quantized landing does not teach a fictitious MIL offset");
    close(next_command.bearing_deg, initial_command.bearing_deg,
          "quantized landing preserves the next displayed azimuth");
    close(next_command.mil, initial_command.mil,
          "quantized landing preserves the next displayed integer MIL");
}

}  // namespace

int main() {
    close(wardogs::displayed_bearing_deg(36.86989764584402), 36.9,
          "command azimuth uses the firing card precision");
    close(wardogs::displayed_bearing_deg(10.05), 10.1,
          "positive half-tenth rounds upward");
    close(wardogs::displayed_bearing_deg(-10.05), 350.0,
          "negative azimuth normalizes before command rounding");
    close(wardogs::displayed_bearing_deg(359.949), 359.9,
          "azimuth below north rounding threshold stays in the last tenth");
    close(wardogs::displayed_bearing_deg(359.95), 0.0,
          "north half-tenth becomes zero rather than 360");
    close(wardogs::displayed_bearing_deg(-0.01), 0.0,
          "negative small azimuth wraps to canonical north");
    check(!std::signbit(wardogs::displayed_bearing_deg(-0.0)),
          "command north never retains negative zero");
    close(wardogs::displayed_bearing_deg(720.0), 0.0,
          "whole rotations use canonical north");
    check(std::isfinite(wardogs::displayed_bearing_deg(
              std::numeric_limits<double>::max())),
          "normalization cannot overflow for a finite azimuth");

    for (int tenth = 0; tenth < 3600; ++tenth) {
        const double angle = tenth / 10.0;
        const double commanded = wardogs::displayed_bearing_deg(angle);
        check(commanded >= 0.0 && commanded < 360.0,
              "every command remains within the half-open compass circle");
        close(wardogs::displayed_bearing_deg(commanded), commanded,
              "azimuth command is idempotent over the full circle");
        close(wardogs::displayed_bearing_deg(angle - 360.0), commanded,
              "negative equivalent azimuth produces the same command");
        check(wardogs::format_bearing(angle) == wardogs::format_bearing(commanded),
              "formatted bearing and numeric command use the same quantization");
    }

    for (const auto arc : {wardogs::Arc::low, wardogs::Arc::high}) {
        const double mil = arc == wardogs::Arc::low ? 399.5 : 1149.5;
        const auto command = wardogs::displayed_firing_command(359.96, mil, arc);
        check(command.arc == arc, "command retains the selected trajectory");
        close(command.bearing_deg, 0.0, "command wraps north for either trajectory");
        close(command.mil, std::round(mil), "half-MIL follows the integer card rounding");
        close(command.table_distance_m, wardogs::sph2_distance_for_mil(command.mil, arc),
              "sight-table range describes the actual integer MIL command");
        check(wardogs::displayed_firing_command(command.bearing_deg, command.mil, arc) == command,
              "the complete command is idempotent for either trajectory");
        const auto below_half = wardogs::displayed_firing_command(0.0, mil - 0.001, arc);
        close(below_half.mil, mil - 0.5, "MIL below the half step rounds downward");
        const wardogs::CorrectedSolution source{arc, 36.86989764584402, 1234.567, mil};
        const auto from_solution = wardogs::displayed_firing_command(source);
        close(from_solution.bearing_deg, 36.9, "solution overload quantizes azimuth");
        close(from_solution.mil, command.mil, "solution overload quantizes MIL");
        close(source.mil, mil, "command creation preserves the precise source solution");
        close(source.reticle_distance_m, 1234.567,
              "command creation does not rewrite the source range");
    }

    for (const auto endpoint : std::array<std::pair<wardogs::Arc, double>, 4>{{
             {wardogs::Arc::low, 20.0}, {wardogs::Arc::low, 600.0},
             {wardogs::Arc::high, 610.0}, {wardogs::Arc::high, 1400.0}}}) {
        const auto command = wardogs::displayed_firing_command(0.0, endpoint.second,
                                                               endpoint.first);
        close(command.mil, endpoint.second, "exact table endpoints remain valid commands");
    }
    rejects([] { (void)wardogs::displayed_firing_command(0.0, 19.9, wardogs::Arc::low); },
            "rounding cannot disguise a low MIL below the table");
    rejects([] { (void)wardogs::displayed_firing_command(0.0, 600.1, wardogs::Arc::low); },
            "rounding cannot disguise a low MIL above the table");
    rejects([] { (void)wardogs::displayed_firing_command(0.0, 609.9, wardogs::Arc::high); },
            "rounding cannot disguise a high MIL below the table");
    rejects([] { (void)wardogs::displayed_firing_command(0.0, 1400.1, wardogs::Arc::high); },
            "rounding cannot disguise a high MIL above the table");
    rejects([] { (void)wardogs::displayed_firing_command(0.0, -399.5, wardogs::Arc::low); },
            "negative MIL cannot become a firing command");
    rejects([] { (void)wardogs::displayed_firing_command(0.0, 400.0,
                                                        static_cast<wardogs::Arc>(99)); },
            "an unsupported trajectory cannot produce a command");
    for (const double invalid : {std::numeric_limits<double>::quiet_NaN(),
                                 std::numeric_limits<double>::infinity(),
                                 -std::numeric_limits<double>::infinity()}) {
        rejects([=] { (void)wardogs::displayed_bearing_deg(invalid); },
                "non-finite azimuth cannot become a numeric command");
        rejects([=] { (void)wardogs::displayed_firing_command(invalid, 400.0,
                                                            wardogs::Arc::low); },
                "non-finite command azimuth is rejected");
        rejects([=] { (void)wardogs::displayed_firing_command(0.0, invalid,
                                                            wardogs::Arc::low); },
                "non-finite command MIL is rejected");
        check(wardogs::format_bearing(invalid) == L"—",
              "non-finite formatted azimuth keeps the unavailable marker");
    }

    no_fictitious_correction(wardogs::Arc::low, 399.49, 359.96);
    no_fictitious_correction(wardogs::Arc::high, 1149.49, 359.96);
    no_fictitious_correction(wardogs::Arc::low, 399.51, 36.86);
    no_fictitious_correction(wardogs::Arc::high, 1149.51, 36.86);

    if (failures) return 1;
    std::cout << "Presentation command tests passed\n";
    return 0;
}
