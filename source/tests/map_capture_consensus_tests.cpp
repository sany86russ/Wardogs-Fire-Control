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
    const wardogs::MapCaptureAnchor anchor{{298, 276, 357, 291, 265, 202, 324, 221}, 4.0 / 3.0};
    wardogs::MapCaptureFrameEvidence guarded;
    MapCaptureConsensus frozen;
    guarded.observe(anchor, true);
    const auto first_frozen = frozen.observe(point, true);
    check(wardogs::complete_map_capture(first_frozen, true, false, true,
                                        guarded.frozen_pair_verified()) == MapCaptureDecision::review &&
          frozen.frames() == 1 && frozen.agreeing_frames() == 1 && guarded.verified_frames() == 1,
          "moving after one guarded frame permits review but no retry or automatic target");
    check(wardogs::complete_map_capture(MapCaptureDecision::retry, true, false, true, false) ==
              MapCaptureDecision::review && frozen.frames() == 1 && frozen.agreeing_frames() == 1,
          "reviewing a saved candidate after a retry guard never adds another frame or vote");
    guarded.observe(anchor, true);
    const auto second_frozen = frozen.observe(point, true);
    check(wardogs::complete_map_capture(second_frozen, true, false, true,
                                        guarded.frozen_pair_verified()) == MapCaptureDecision::accept,
          "two independent guarded agreeing frames with stable anchors survive later cursor movement");
    check(wardogs::complete_map_capture(second_frozen, false, true, true, true) == MapCaptureDecision::failed &&
          wardogs::complete_map_capture(second_frozen, false, false, true, true) == MapCaptureDecision::failed,
          "changed game focus or client invalidates even a frozen agreeing pair");
    check(wardogs::complete_map_capture(first_frozen, true, true, true, false) == MapCaptureDecision::retry &&
          wardogs::complete_map_capture(first_frozen, true, false, false, false) == MapCaptureDecision::failed,
          "fresh screenshots require unchanged cursor and unreadable moved sources fail");
    wardogs::MapCaptureFrameEvidence unverified;
    MapCaptureConsensus unverified_pair;
    unverified.observe(anchor, false);
    unverified_pair.observe(point, false);
    unverified.observe(anchor, true);
    const auto one_verified = unverified_pair.observe(point, true);
    check(one_verified == MapCaptureDecision::retry && !unverified.frozen_pair_verified() &&
          wardogs::complete_map_capture(one_verified, true, false, true, false) == MapCaptureDecision::review,
          "an earlier unverified frame cannot vote with the latest guarded screenshot");
    auto shifted_anchor = anchor; ++shifted_anchor.bounds[0];
    wardogs::MapCaptureFrameEvidence displaced;
    displaced.observe(anchor, true); displaced.observe(shifted_anchor, true);
    check(wardogs::complete_map_capture(second_frozen, true, false, true,
                                        displaced.frozen_pair_verified()) == MapCaptureDecision::review,
          "equal coordinates from shifted axis bounds require review after cursor release");
    auto changed_scale = anchor; changed_scale.scale = 1.0;
    wardogs::MapCaptureFrameEvidence scaled;
    scaled.observe(anchor, true); scaled.observe(changed_scale, true);
    check(!scaled.frozen_pair_verified(), "changed screenshot scale cannot establish a frozen pair");
    wardogs::MapCaptureFrameEvidence missing_anchor;
    missing_anchor.observe(anchor, true); missing_anchor.observe(std::nullopt, true);
    check(!missing_anchor.frozen_pair_verified(), "missing axis receipts cannot establish a frozen pair");
    check(wardogs::complete_map_capture(MapCaptureDecision::review, true, false, true, true) == MapCaptureDecision::review,
          "weak or conflicting coordinates never become automatic after cursor movement");
    guarded = {};
    check(!guarded.frozen_pair_verified() && guarded.verified_frames() == 0,
          "a new request resets all frame and anchor receipts");
    std::cout << "Map temporal consensus: " << failures << " failures\n";
    return failures ? 1 : 0;
}
