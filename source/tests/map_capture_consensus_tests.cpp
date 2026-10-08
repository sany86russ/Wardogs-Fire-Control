#include "wardogs/map_capture_consensus.hpp"

#include <iostream>
#include <limits>

int main() {
    unsigned failures{};
    const auto check = [&](bool value, const char* name) {
        if (!value) { ++failures; std::cerr << "FAIL: " << name << '\n'; }
    };
    using wardogs::MapCaptureConsensus;
    using wardogs::MapCaptureDecision;
    const wardogs::Point point{99.51, 113.81}, other{99.51, 113.82};
    MapCaptureConsensus stable;
    check(stable.observe(point, true) == MapCaptureDecision::retry,
          "one fully readable frame never establishes freshness");
    check(stable.observe(point, true) == MapCaptureDecision::accept && stable.agreeing_frames() == 2,
          "two separate complete agreeing frames establish a target");
    MapCaptureConsensus late;
    check(late.observe(std::nullopt, false) == MapCaptureDecision::retry &&
          late.observe(point, false) == MapCaptureDecision::retry &&
          late.observe(point, true) == MapCaptureDecision::retry &&
          late.observe(point, true) == MapCaptureDecision::accept,
          "animation and an uncertain digit can recover within the four-frame budget");
    MapCaptureConsensus uncertain;
    for (unsigned index = 0; index < 3; ++index)
        check(uncertain.observe(point, false) == MapCaptureDecision::retry,
              "repeating weak evidence cannot become an automatic coordinate");
    check(uncertain.observe(point, false) == MapCaptureDecision::review,
          "weak coordinates remain reviewable after bounded capture");
    MapCaptureConsensus missing;
    for (unsigned index = 0; index < 3; ++index)
        check(missing.observe(std::nullopt, false) == MapCaptureDecision::retry,
              "a temporarily obscured coordinate may be retried");
    check(missing.observe(std::nullopt, false) == MapCaptureDecision::failed &&
          missing.observe(point, true) == MapCaptureDecision::failed && missing.frames() == 4,
          "missing coordinates exhaust the budget without fabricated recovery");
    MapCaptureConsensus conflict;
    conflict.observe(point, true);
    conflict.observe(other, true);
    conflict.observe(point, true);
    check(conflict.observe(point, true) == MapCaptureDecision::review && conflict.conflict() &&
          conflict.candidates().size() == 2,
          "a contradictory complete frame permanently prevents automatic acceptance");
    MapCaptureConsensus new_request;
    check(new_request.frames() == 0 && new_request.observe(other, true) == MapCaptureDecision::retry,
          "a new target does not inherit prior frames or agreement");
    MapCaptureConsensus invalid;
    for (unsigned index = 0; index < 4; ++index)
        invalid.observe(wardogs::Point{std::numeric_limits<double>::infinity(), 1.0}, true);
    check(invalid.candidates().empty() && invalid.agreeing_frames() == 0,
          "non-finite observations cannot contribute evidence or review candidates");
    std::cout << "Map temporal consensus: " << failures << " failures\n";
    return failures ? 1 : 0;
}
