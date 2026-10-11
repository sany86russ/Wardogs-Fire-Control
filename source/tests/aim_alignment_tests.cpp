#include "wardogs/aim_alignment.hpp"

#include <cmath>
#include <iostream>
#include <limits>

namespace {

int failures = 0;

void check(bool good, const char* name) {
    if (!good) { ++failures; std::cerr << "FAIL: " << name << '\n'; }
}

wardogs::AimAlignmentInput readable() {
    wardogs::AimAlignmentInput input;
    input.desired_bearing_deg = 0.0;
    input.actual_bearing_deg = 0.0;
    input.desired_mil = 900.0;
    input.actual_mil = 900.0;
    input.bearing_uncertainty_deg = 0.05;
    input.mil_uncertainty = 0.25;
    input.captured_at_ms = 100;
    input.now_ms = 200;
    input.stable_pair = true;
    input.stabilized = true;
    input.current_context_valid = true;
    input.actual_sight_verified = true;
    return input;
}

}  // namespace

int main() {
    using State = wardogs::AimAlignmentState;
    using Reason = wardogs::AimAlignmentReason;
    const auto evaluate = wardogs::evaluate_aim_alignment;
    auto input = readable();
    check(evaluate(input).state == State::aligned, "fresh stable verified exact command is aligned");
    input.actual_bearing_deg = 359.9;
    auto result = evaluate(input);
    check(result.state == State::aligned && result.bearing_delta_deg &&
          std::abs(*result.bearing_delta_deg - 0.1) < 1e-10, "359.9 to north takes positive shortest adjustment");
    input.desired_bearing_deg = 359.9;
    input.actual_bearing_deg = 0;
    result = evaluate(input);
    check(result.bearing_delta_deg && std::abs(*result.bearing_delta_deg + 0.1) < 1e-10,
          "north to 359.9 takes negative shortest adjustment");
    input = readable();
    input.desired_bearing_deg = 360;
    check(evaluate(input).state == State::aligned && evaluate(input).bearing_delta_deg == 0.0,
          "360 and zero are the same circular bearing");
    input.desired_mil = 895;
    check(evaluate(input).mil_delta == -5.0 && evaluate(input).state == State::close,
          "signed MIL adjustment is desired minus actual");

    input = readable();
    input.bearing_uncertainty_deg = 0.2;
    input.mil_uncertainty = 1.0;
    check(evaluate(input).state == State::aligned, "green includes exact worst-error bounds");
    input.bearing_uncertainty_deg = 0.2001;
    check(evaluate(input).state == State::close, "uncertainty alone can prevent green despite matching central values");
    input = readable();
    input.actual_mil = 900.75;
    check(evaluate(input).state == State::aligned, "MIL deviation plus interval exactly one remains green");
    input.mil_uncertainty = 0.251;
    check(evaluate(input).state == State::close, "green uses the whole MIL interval rather than its centre");

    input = readable();
    input.actual_bearing_deg = 2.0;
    input.bearing_uncertainty_deg = 1.0;
    check(evaluate(input).state == State::close, "minimum possible AZ error exactly one is not red");
    input.bearing_uncertainty_deg = 0.999;
    check(evaluate(input).state == State::misaligned, "red requires minimum possible AZ error above one");
    input = readable();
    input.actual_mil = 911;
    input.mil_uncertainty = 1.0;
    check(evaluate(input).state == State::close, "minimum possible MIL error exactly ten is not red");
    input.mil_uncertainty = 0.99;
    check(evaluate(input).state == State::misaligned, "red requires minimum possible MIL error above ten");
    input = readable();
    input.bearing_uncertainty_deg = 1.01;
    check(evaluate(input).state == State::unknown && evaluate(input).reason == Reason::invalid_uncertainty,
          "bearing interval beyond reader bound cannot claim orange agreement");
    input = readable();
    input.mil_uncertainty = 1.01;
    check(evaluate(input).state == State::unknown && evaluate(input).reason == Reason::invalid_uncertainty,
          "MIL interval beyond reader bound stays neutral");
    input = readable();
    input.actual_bearing_deg = 0.5;
    check(evaluate(input).state == State::close, "readable intermediate disagreement is orange");

    input = readable();
    input.stabilized = false;
    check(evaluate(input).state == State::close && evaluate(input).reason == Reason::unstabilized,
          "known unstabilized matching sight stays orange");
    input.actual_bearing_deg = 4;
    check(evaluate(input).state == State::misaligned, "far numeric disagreement is red even when unstabilized");
    input.stable_pair = false;
    result = evaluate(input);
    check(result.state == State::unknown && result.reason == Reason::awaiting_stable_sight && result.bearing_delta_deg,
          "a single known frame stays neutral while its numeric adjustment remains available");
    input.stable_pair = true;
    input.stabilized.reset();
    check(evaluate(input).state == State::unknown && evaluate(input).reason == Reason::unknown_stabilization,
          "unknown stabilization remains neutral even for far readings");

    input = readable();
    input.now_ms = 1100;
    check(evaluate(input).state == State::aligned, "capture age exactly one second is fresh");
    input.now_ms = 1101;
    check(evaluate(input).state == State::unknown && evaluate(input).reason == Reason::stale_capture,
          "capture beyond one second is neutral");
    input.now_ms = 99;
    check(evaluate(input).state == State::unknown && evaluate(input).reason == Reason::future_capture,
          "future capture is rejected without unsigned age underflow");
    input = readable();
    input.captured_at_ms.reset();
    check(evaluate(input).state == State::unknown, "missing capture time stays neutral");
    input = readable();
    input.actual_sight_verified = false;
    check(evaluate(input).state == State::unknown && !evaluate(input).bearing_delta_deg,
          "unverified input cannot imply measured agreement");
    input = readable();
    input.current_context_valid = false;
    check(evaluate(input).state == State::unknown, "changed gun or target context stays neutral");
    input = readable();
    input.bearing_uncertainty_deg.reset();
    check(evaluate(input).state == State::unknown, "missing absolute bearing uncertainty stays neutral");
    input = readable();
    input.mil_uncertainty.reset();
    check(evaluate(input).state == State::unknown, "missing absolute MIL uncertainty stays neutral");
    input = readable();
    input.mil_uncertainty = -0.1;
    check(evaluate(input).state == State::unknown, "negative interval stays neutral");
    input = readable();
    input.bearing_uncertainty_deg = std::numeric_limits<double>::infinity();
    check(evaluate(input).state == State::unknown, "infinite interval stays neutral");
    input = readable();
    input.actual_bearing_deg = std::numeric_limits<double>::quiet_NaN();
    check(evaluate(input).state == State::unknown, "NaN actual sight stays neutral");
    input = readable();
    input.actual_bearing_deg = 360.1;
    check(evaluate(input).state == State::unknown, "invalid bearing is not silently wrapped");
    input = readable();
    input.actual_mil = -1.0;
    check(evaluate(input).state == State::unknown, "negative sight MIL is invalid");
    input = readable();
    input.desired_mil = std::numeric_limits<double>::max();
    input.mil_uncertainty = std::numeric_limits<double>::max();
    check(evaluate(input).state == State::unknown, "overflowing error interval stays neutral");
    if (failures) return 1;
    std::cout << "Aim alignment tests passed\n";
    return 0;
}
