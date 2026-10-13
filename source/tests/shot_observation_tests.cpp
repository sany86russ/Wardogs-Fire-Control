#include "wardogs/shot_observation.hpp"

#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void rejects(const std::function<void()>& action, const char* message) {
    try {
        action();
        check(false, message);
    } catch (const std::invalid_argument&) {
    }
}

using Reason = wardogs::ShotEvidenceReason;
using Provenance = wardogs::ShotEvidenceProvenance;

struct Fixture {
    wardogs::ShotEvidenceContext context{"synthetic-map", {10.0, 20.0}, wardogs::Arc::high};
    wardogs::Point target{12.0, 30.0};
    wardogs::ShotObservationSession session;
    std::uint64_t frame_id{};

    explicit Fixture(wardogs::ShotObservationPolicy policy = {}) : session(policy) {
        session.begin_context(context, 1, 0);
    }

    wardogs::ShotImpactQuery query() const { return {context, session.epoch(), target, 5.0}; }

    wardogs::AcceptedSightFrame frame(std::uint64_t time, std::optional<int> ammo = 5,
                                     double bearing = 10.0, double mil = 900.0) {
        wardogs::AcceptedSightFrame value;
        value.id = ++frame_id;
        value.timestamp_ms = time;
        value.epoch = session.epoch();
        value.firing_snapshot = {target, context.arc, bearing, mil, 5.0};
        value.command_id = 17;
        value.context = context;
        value.foreground_verified = true;
        value.ammo = ammo;
        value.ammo_channel = "synthetic-shell-counter";
        return value;
    }

    wardogs::ShotEvidenceUpdate read(std::uint64_t time, std::optional<int> ammo = 5,
                                    double bearing = 10.0, double mil = 900.0) {
        return session.observe(frame(time, ammo, bearing, mil), time);
    }

    void stable(std::uint64_t first = 100, std::optional<int> ammo = 5) {
        (void)read(first, ammo);
        check(read(first + 100, ammo).reason == Reason::accepted_frame, "two distinct frames establish stable HUD");
    }

    wardogs::ShotEvidenceUpdate shot(std::uint64_t start = 100, int ammo = 5) {
        stable(start, ammo);
        const auto intent = session.fire_intent(query(), start + 150, start + 150);
        check(intent.intent.has_value(), "physical intent freezes a stable pre-press sight");
        check(session.outstanding_shots() == 0, "physical intent alone does not enqueue a shot");
        check(read(start + 200, ammo - 1).reason == Reason::awaiting_discharge_confirmation,
              "first counter decrease needs a distinct corroborating frame");
        return read(start + 300, ammo - 1);
    }
};

std::uint64_t reservation_id(const wardogs::ShotImpactAssociation& association) {
    return association.reservation ? association.reservation->reservation_id : 0;
}

void capture_press_ordering() {
    Fixture same_tick;
    same_tick.stable();
    const auto ambiguous = same_tick.session.fire_intent(same_tick.query(), 200, 250);
    check(ambiguous.reason == Reason::stale_frame && !ambiguous.intent,
          "a capture acquired in the press millisecond has no proven pre-press order");
    (void)same_tick.read(300, 4);
    const auto ambiguous_discharge = same_tick.read(400, 4);
    check(!ambiguous_discharge.shot && same_tick.session.outstanding_shots() == 0,
          "a later ammunition decrement cannot retroactively validate a same-tick aim snapshot");

    Fixture one_tick_before;
    (void)one_tick_before.read(100);
    (void)one_tick_before.session.observe(one_tick_before.frame(200), 250);
    const auto ordered = one_tick_before.session.fire_intent(one_tick_before.query(), 201, 251);
    check(ordered.intent && ordered.intent->last_sight_capture_ms == 200 && ordered.intent->pressed_at_ms == 201,
          "a capture acquired one millisecond before the press stays valid despite later OCR completion");
    (void)one_tick_before.read(300, 4);
    const auto ordered_discharge = one_tick_before.read(400, 4);
    check(ordered_discharge.shot && ordered_discharge.shot->last_sight_capture_ms == 200 &&
          ordered_discharge.shot->pressed_at_ms == 201,
          "confirmed discharge preserves strictly pre-press acquisition provenance");

    Fixture one_tick_after;
    one_tick_after.stable(101);
    const auto post_press = one_tick_after.session.fire_intent(one_tick_after.query(), 200, 250);
    check(post_press.reason == Reason::stale_frame && !post_press.intent,
          "a capture acquired one millisecond after the press is never frozen as the executed aim");
}

void bounded_source_capture_times() {
    Fixture old_pixels;
    auto late_receipt = old_pixels.frame(1200);
    late_receipt.source_started_at_ms = 100;
    check(old_pixels.session.observe(late_receipt, 1250).reason == Reason::stale_frame,
          "a recent acquisition receipt cannot make possibly old source pixels fresh");
    late_receipt = old_pixels.frame(1300);
    late_receipt.source_started_at_ms = 1301;
    check(old_pixels.session.observe(late_receipt, 1300).reason == Reason::invalid_frame,
          "source acquisition cannot begin after its receipt timestamp");

    Fixture expiring;
    auto first = expiring.frame(100);
    first.source_started_at_ms = 50;
    (void)expiring.session.observe(first, 100);
    auto second = expiring.frame(200);
    second.source_started_at_ms = 150;
    check(expiring.session.observe(second, 200).reason == Reason::accepted_frame,
          "disjoint bounded captures can still establish a stable actual sight");
    const auto expired = expiring.session.fire_intent(expiring.query(), 1200, 1200);
    check(expired.reason == Reason::awaiting_stable_hud && !expired.intent,
          "pre-press freshness expires from the earliest source time rather than the later receipt");

    Fixture overlapping;
    first = overlapping.frame(200);
    first.source_started_at_ms = 100;
    (void)overlapping.session.observe(first, 200);
    second = overlapping.frame(250);
    second.source_started_at_ms = 150;
    check(overlapping.session.observe(second, 250).reason == Reason::awaiting_stable_hud,
          "receipt separation cannot establish a stable pair when source acquisition intervals overlap");
    auto third = overlapping.frame(350);
    third.source_started_at_ms = 300;
    check(overlapping.session.observe(third, 350).reason == Reason::accepted_frame &&
          overlapping.session.fire_intent(overlapping.query(), 351, 351).intent.has_value(),
          "a later source interval with proven separation restores stable pre-press observation");

    Fixture partial;
    wardogs::DischargeFrameEvidence evidence;
    evidence.id = 1; evidence.timestamp_ms = 1200; evidence.source_started_at_ms = 100;
    evidence.epoch = partial.session.epoch(); evidence.context = partial.context;
    evidence.foreground_verified = true; evidence.ammo = 4; evidence.ammo_channel = "synthetic-shell-counter";
    check(partial.session.observe_discharge(evidence, 1250).reason == Reason::stale_frame,
          "reticle-hidden discharge metadata applies the same conservative source freshness bound");
    evidence.id = 2; evidence.timestamp_ms = 1300; evidence.source_started_at_ms = 1301;
    check(partial.session.observe_discharge(evidence, 1300).reason == Reason::invalid_frame,
          "a partial frame also rejects an inverted acquisition interval");

    Fixture ambiguous;
    ambiguous.stable();
    (void)ambiguous.session.fire_intent(ambiguous.query(), 250, 250);
    auto decrement = ambiguous.frame(300, 4);
    decrement.source_started_at_ms = 240;
    check(ambiguous.session.observe(decrement, 300).reason == Reason::correlation_ambiguous,
          "a first decrement whose source interval overlaps the press cannot prove a post-press discharge");
    check(!ambiguous.read(400, 4).shot && ambiguous.session.outstanding_shots() == 0,
          "a later unchanged counter cannot repair the original ambiguous discharge ordering");

    Fixture corroboration;
    corroboration.stable();
    (void)corroboration.session.fire_intent(corroboration.query(), 250, 250);
    decrement = corroboration.frame(300, 4);
    decrement.source_started_at_ms = 290;
    check(corroboration.session.observe(decrement, 300).reason == Reason::awaiting_discharge_confirmation,
          "a proven post-press source interval starts bounded discharge confirmation");
    auto too_close = corroboration.frame(400, 4);
    too_close.source_started_at_ms = 310;
    check(!corroboration.session.observe(too_close, 400).shot,
          "receipt latency cannot inflate the minimum separation of discharge corroboration frames");
    auto separated = corroboration.frame(500, 4);
    separated.source_started_at_ms = 410;
    check(corroboration.session.observe(separated, 500).shot.has_value(),
          "a genuinely separated later source interval corroborates the immutable pre-press shot");
}

void pre_press_and_retry() {
    Fixture fixture;
    fixture.stable();
    const auto intent = fixture.session.fire_intent(fixture.query(), 250, 250);
    check(intent.intent && intent.intent->firing.bearing_deg == 10.0 && intent.intent->firing.mil == 900.0 &&
          intent.intent->first_sight_frame_id == 1 && intent.intent->last_sight_frame_id == 2 &&
          intent.intent->pressed_at_ms == 250, "intent retains actual pre-press sight and capture provenance");
    check(fixture.session.fire_intent(fixture.query(), 250, 251).reason == Reason::duplicate_intent,
          "duplicate physical press cannot create another intent");
    // The counter can decrease on reload after the operator has changed aim.
    (void)fixture.read(300, 4, 80.0, 1000.0);
    const auto observed = fixture.read(400, 4, 80.0, 1000.0);
    check(observed.reason == Reason::discharge_observed && observed.shot.has_value(),
          "corroborated transition after a press yields HUD-correlated evidence");
    if (observed.shot) {
        const auto& shot = *observed.shot;
        check(shot.firing.bearing_deg == 10.0 && shot.firing.mil == 900.0 && shot.command_id == 17 &&
              shot.firing.target == fixture.target && intent.intent && shot.intent_id == intent.intent->intent_id,
              "reload-time aim never overwrites frozen pre-press aim");
        check(shot.hud_verified && shot.discharge_observed && !shot.physical_shot_verified &&
              !shot.correlation_ambiguous && shot.discharge_frame_id == 3 && shot.pressed_at_ms == 250,
              "HUD correlation stays separate from physical-shot verification");
    }
    (void)fixture.read(500, 4, 150.0, 1100.0);
    (void)fixture.read(600, 4, 150.0, 1100.0);
    const auto reserved = fixture.session.reserve_impact(fixture.query(), 650);
    const auto id = reservation_id(reserved);
    check(reserved.association_unique && reserved.provenance == Provenance::hud_discharge_correlated && id != 0,
          "one frozen observed shot has a unique reservation");
    check(reserved.reservation && reserved.reservation->shot.firing.bearing_deg == 10.0,
          "Alt-I cannot substitute the currently changed sight");
    check(fixture.session.reserve_impact(fixture.query(), 651).reason == Reason::already_reserved,
          "duplicate Alt-I cannot lease one shot twice");
    auto wrong_target = fixture.query();
    wrong_target.target.x += 1.0;
    check(fixture.session.validate_reservation(id, wrong_target, 652) == Reason::target_changed,
          "reservation cannot be recorded against a retaken target");
    check(fixture.session.release(id, 653) == Reason::released && fixture.session.outstanding_shots() == 1,
          "failed impact recording releases without consuming the shot");
    const auto retry = fixture.session.reserve_impact(fixture.query(), 654);
    const auto retry_id = reservation_id(retry);
    check(retry_id != 0 && retry_id != id && retry.reservation && reserved.reservation &&
          retry.reservation->shot.shot_id == reserved.reservation->shot.shot_id,
          "retry creates a new lease for exactly the original immutable shot");
    check(fixture.session.validate_reservation(retry_id, fixture.query(), 655) == Reason::unique_association,
          "retry is validated immediately before calibration mutation");
    check(fixture.session.commit(retry_id, 656) == Reason::committed && fixture.session.outstanding_shots() == 0,
          "successful recording consumes the shot once");
    check(fixture.session.commit(retry_id, 657) == Reason::reservation_not_found,
          "duplicate completion cannot commit twice");
}

void intents_are_not_shots() {
    Fixture no_hud;
    const auto legacy = no_hud.session.reserve_impact(no_hud.query(), 50);
    check(legacy.reason == Reason::no_hud_evidence && legacy.provenance == Provenance::unverified_legacy &&
          !legacy.association_unique && !legacy.reservation, "empty HUD history explicitly remains unverified legacy");
    check(no_hud.session.fire_intent(no_hud.query(), 60, 60).reason == Reason::no_hud_evidence,
          "mouse press with no actual HUD does not invent a sight");
    check(no_hud.session.outstanding_shots() == 0, "mouse press alone never verifies firing");

    Fixture unchanged;
    unchanged.stable();
    (void)unchanged.session.fire_intent(unchanged.query(), 250, 250);
    (void)unchanged.read(300, 5);
    (void)unchanged.read(400, 5);
    check(unchanged.session.outstanding_shots() == 0 &&
          unchanged.session.reserve_impact(unchanged.query(), 450).reason == Reason::awaiting_discharge_confirmation,
          "holding fire without a counter/camera change does not invent a discharge");
    check(unchanged.session.reserve_impact(unchanged.query(), 15251).reason == Reason::expired,
          "intent confirmation timeout is explicit rather than a fabricated shot");

    Fixture passive;
    passive.stable();
    check(passive.read(300, 4).reason == Reason::correlation_ambiguous &&
          !passive.read(400, 4).shot && passive.session.outstanding_shots() == 0,
          "passive reserve-counter decrement cannot bind the latest aim to a shot");
    check(passive.session.reserve_impact(passive.query(), 450).reason == Reason::correlation_ambiguous,
          "unassociated counter transition blocks silent legacy fallback");

    Fixture holding;
    (void)holding.shot();
    check(holding.read(500, 3).reason == Reason::correlation_ambiguous && holding.session.outstanding_shots() == 1,
          "another decrement while holding fire does not fabricate another pre-press snapshot");
    check(holding.session.reserve_impact(holding.query(), 550).reason == Reason::correlation_ambiguous,
          "untracked subsequent discharge prevents using the earlier shot");
}

void asynchronous_ambiguity() {
    Fixture invalid_press;
    (void)invalid_press.shot();
    const auto reserved = invalid_press.session.reserve_impact(invalid_press.query(), 450);
    const auto id = reservation_id(reserved);
    invalid_press.session.invalidate_sight(460);
    check(invalid_press.session.fire_intent(invalid_press.query(), 470, 470).reason == Reason::awaiting_stable_hud,
          "new physical press while HUD is missing is explicitly unverified");
    check(invalid_press.session.validate_reservation(id, invalid_press.query(), 480) == Reason::correlation_ambiguous &&
          invalid_press.session.commit(id, 481) == Reason::correlation_ambiguous,
          "invalid new press during impact OCR cannot silently accept the old shot");
    check(invalid_press.session.release(id, 482) == Reason::released,
          "ambiguous asynchronous completion can still release its old lease");

    Fixture fresh_press;
    (void)fresh_press.shot();
    const auto old = fresh_press.session.reserve_impact(fresh_press.query(), 450);
    (void)fresh_press.read(500, 4);
    check(fresh_press.session.fire_intent(fresh_press.query(), 550, 550).intent.has_value(),
          "a fresh subsequent press freezes its own sight");
    check(fresh_press.session.validate_reservation(reservation_id(old), fresh_press.query(), 560) ==
          Reason::awaiting_discharge_confirmation, "new unresolved intent invalidates a previous impact lease");
    (void)fresh_press.read(600, 3);
    (void)fresh_press.read(700, 3);
    check(fresh_press.session.outstanding_shots() == 2 &&
          fresh_press.session.validate_reservation(reservation_id(old), fresh_press.query(), 750) ==
          Reason::multiple_outstanding_shots, "two outstanding observed shots cannot be collapsed into latest");
    check(fresh_press.session.release(reservation_id(old), 751) == Reason::released &&
          fresh_press.session.reserve_impact(fresh_press.query(), 752).reason == Reason::multiple_outstanding_shots,
          "release does not resolve physical ambiguity between multiple shots");

    Fixture two_intents;
    two_intents.stable();
    (void)two_intents.session.fire_intent(two_intents.query(), 250, 250);
    check(two_intents.session.fire_intent(two_intents.query(), 260, 260).reason == Reason::multiple_fire_intents,
          "two distinct presses awaiting one transition are explicitly ambiguous");
    check(two_intents.read(300, 4).reason == Reason::multiple_fire_intents &&
          two_intents.session.outstanding_shots() == 0, "one counter decrement cannot choose between two pre-press intents");
}

void lost_input_quarantines_context() {
    Fixture fixture;
    (void)fixture.shot();
    const auto original = fixture.session.reserve_impact(fixture.query(), 450);
    const auto id = reservation_id(original);
    fixture.session.invalidate_correlation(460);
    check(fixture.session.outstanding_shots() == 1,
          "lost input preserves frozen evidence rather than replacing it with empty legacy history");
    check(fixture.session.validate_reservation(id, fixture.query(), 470) == Reason::correlation_ambiguous &&
          fixture.session.commit(id, 471) == Reason::correlation_ambiguous,
          "lost input invalidates a previously issued impact lease before mutation");
    check(fixture.session.release(id, 472) == Reason::released,
          "quarantined impact lease can still be released after failure");
    const auto retry = fixture.session.reserve_impact(fixture.query(), 473);
    check(retry.reason == Reason::correlation_ambiguous && !retry.association_unique && !retry.reservation &&
          retry.provenance == Provenance::hud_only,
          "fresh Alt-I cannot silently enter legacy fallback after loss of input correlation");
    (void)fixture.read(500, 4);
    (void)fixture.read(600, 4);
    check(!fixture.session.fire_intent(fixture.query(), 650, 650).intent,
          "new stable HUD cannot silently clear a quarantined gun context");
    check(fixture.session.reserve_impact(fixture.query(), 120651).reason == Reason::correlation_ambiguous &&
          fixture.session.outstanding_shots() == 0,
          "normal pending expiry does not weaken the lost-input quarantine");
    fixture.session.begin_context(fixture.context, 2, 120700);
    check(fixture.session.reserve_impact(fixture.query(), 120701).reason == Reason::no_hud_evidence,
          "explicit new context clears quarantine and all previous identities");
    const auto fresh = fixture.shot(120800);
    check(fresh.shot && fixture.session.reserve_impact(fixture.query(), 121150).association_unique,
          "a newly confirmed shot can be associated after explicit context reset");

    Fixture monotonic;
    (void)monotonic.shot();
    const auto current = monotonic.session.reserve_impact(monotonic.query(), 450);
    rejects([&] { monotonic.session.invalidate_correlation(449); },
            "late correlation invalidation rejects regressed time without underflow");
    check(monotonic.session.validate_reservation(reservation_id(current), monotonic.query(), 451) ==
          Reason::unique_association, "rejected stale invalidation does not mutate a valid current lease");

    Fixture generations;
    (void)generations.shot();
    generations.session.begin_context(generations.context, 2, 500);
    const auto new_shot = generations.shot(1000);
    check(new_shot.shot.has_value(), "new context has independently confirmed evidence");
    const auto new_lease = generations.session.reserve_impact(generations.query(), 1350);
    generations.session.invalidate_correlation(1360, 250);
    check(generations.session.validate_reservation(reservation_id(new_lease), generations.query(), 1370) ==
          Reason::unique_association, "old listener event before context start cannot poison a newer shot lease");
    generations.session.invalidate_correlation(1380, 1400);
    check(generations.session.validate_reservation(reservation_id(new_lease), generations.query(), 1390) ==
          Reason::unique_association, "future physical timestamp cannot quarantine a valid current lease");
    generations.session.invalidate_correlation(1400, 1250);
    check(generations.session.validate_reservation(reservation_id(new_lease), generations.query(), 1410) ==
          Reason::correlation_ambiguous && generations.session.outstanding_shots() == 1,
          "valid delayed same-context press quarantines association without inventing a second shot");
}

void invalidation_and_context() {
    Fixture frozen;
    frozen.stable();
    (void)frozen.session.fire_intent(frozen.query(), 250, 250);
    frozen.session.invalidate_sight(260);
    (void)frozen.read(300, 4, 70.0, 1000.0);
    const auto shot = frozen.read(400, 4, 70.0, 1000.0);
    check(shot.shot && shot.shot->firing.bearing_deg == 10.0,
          "foreground/map-overlay HUD invalidation preserves a frozen pre-press sight");
    frozen.session.invalidate_sight(450);
    check(frozen.session.reserve_impact(frozen.query(), 460).association_unique,
          "HUD invalidation preserves an already observed shot");

    Fixture reload;
    reload.stable();
    (void)reload.session.fire_intent(reload.query(), 250, 250);
    check(reload.read(300, 6, 80.0, 1000.0).reason == Reason::reload_observed,
          "counter increase invalidates current sight without claiming firing");
    check(reload.session.reserve_impact(reload.query(), 350).reason == Reason::ammunition_change_ambiguous,
          "counter replenishment cannot establish a discharge for the original press");
    const auto reloaded = reload.read(400, 4, 80.0, 1000.0);
    check(reloaded.reason == Reason::ammunition_change_ambiguous && !reloaded.shot &&
          reload.session.outstanding_shots() == 0,
          "frozen 5-to-4 difference cannot hide the actual latest 6-to-4 multiple decrement");

    Fixture changed;
    (void)changed.shot();
    auto bad_query = changed.query();
    bad_query.context.base.x += 1.0;
    check(changed.session.reserve_impact(bad_query, 450).reason == Reason::context_changed,
          "different base cannot bind an outstanding shot");
    bad_query = changed.query();
    bad_query.context.map_id = "other-synthetic-map";
    check(changed.session.reserve_impact(bad_query, 451).reason == Reason::context_changed,
          "different map cannot bind an outstanding shot");
    bad_query = changed.query();
    bad_query.context.arc = wardogs::Arc::low;
    check(changed.session.reserve_impact(bad_query, 452).reason == Reason::context_changed,
          "different arc cannot bind an outstanding shot");
    bad_query = changed.query();
    ++bad_query.epoch;
    check(changed.session.reserve_impact(bad_query, 453).reason == Reason::stale_epoch,
          "impact epoch must be the HUD evidence generation");
    changed.session.cancel(460);
    check(changed.session.outstanding_shots() == 0 &&
          changed.session.reserve_impact(changed.query(), 461).reason == Reason::canceled,
          "pause/reset cancels pending shots and leases");
    rejects([&] { changed.session.begin_context(changed.context, 1, 470); }, "resume must not reuse old HUD epoch");
    changed.session.begin_context(changed.context, 2, 480);
    auto late = changed.frame(490);
    late.epoch = 1;
    check(changed.session.observe(late, 490).reason == Reason::stale_epoch,
          "queued old OCR cannot enter the resumed evidence session");
    check(changed.session.reserve_impact(changed.query(), 500).provenance == Provenance::unverified_legacy,
          "new context does not inherit the previous actual sight");
}

void frame_quality_and_timing() {
    Fixture duplicate;
    (void)duplicate.read(100);
    const auto second = duplicate.frame(200);
    (void)duplicate.session.observe(second, 200);
    (void)duplicate.session.fire_intent(duplicate.query(), 250, 250);
    const auto decrement = duplicate.frame(300, 4);
    (void)duplicate.session.observe(decrement, 300);
    check(duplicate.session.observe(decrement, 400).reason == Reason::duplicate_frame &&
          duplicate.session.outstanding_shots() == 0, "same frame ID cannot corroborate a decrement twice");
    check(duplicate.read(500, 4).shot.has_value(), "a fresh distinct frame can corroborate the original transition");

    Fixture fast;
    (void)fast.read(100);
    (void)fast.read(116);
    (void)fast.read(132);
    check(fast.read(148).reason == Reason::accepted_frame,
          "frequent frames preserve the pair anchor until minimum separation is reached");

    Fixture slower_capture;
    (void)slower_capture.read(100);
    check(slower_capture.read(1400).reason == Reason::accepted_frame &&
          slower_capture.session.fire_intent(slower_capture.query(), 1450, 1450).intent.has_value(),
          "1300ms stable-pair cadence is independent of latest capture freshness");
    Fixture excessive_gap;
    (void)excessive_gap.read(100);
    check(excessive_gap.read(2601).reason == Reason::awaiting_stable_hud,
          "stable pair still has its own explicit maximum separation");

    Fixture stale;
    stale.stable();
    check(stale.session.fire_intent(stale.query(), 1300, 1300).reason == Reason::awaiting_stable_hud,
          "stale HUD is not reused as a fresh pre-press sight");
    check(stale.session.reserve_impact(stale.query(), 1310).reason == Reason::correlation_ambiguous,
          "a physical press with stale HUD remains explicitly unresolved");

    Fixture post_press;
    post_press.stable();
    check(post_press.session.fire_intent(post_press.query(), 150, 250).reason == Reason::stale_frame,
          "post-press OCR frame cannot masquerade as a pre-press sight");

    Fixture quality;
    quality.stable();
    auto invalid = quality.frame(300);
    invalid.foreground_verified = false;
    check(quality.session.observe(invalid, 300).reason == Reason::invalid_frame &&
          quality.session.fire_intent(quality.query(), 350, 350).reason == Reason::awaiting_stable_hud,
          "foreground-lost frame invalidates fresh intent eligibility");
    invalid = quality.frame(400);
    invalid.firing_snapshot.bearing_deg = std::numeric_limits<double>::quiet_NaN();
    check(quality.session.observe(invalid, 400).reason == Reason::invalid_frame, "NaN bearing is rejected");
    invalid = quality.frame(500);
    invalid.firing_snapshot.mil = 600;
    check(quality.session.observe(invalid, 500).reason == Reason::invalid_frame, "MIL outside selected arc is rejected");
    invalid = quality.frame(600);
    invalid.timestamp_ms = 601;
    check(quality.session.observe(invalid, 600).reason == Reason::invalid_frame, "future capture time is rejected");
    check(quality.read(550).reason == Reason::time_regression, "completion time must remain monotonic");

    Fixture old_capture;
    auto old = old_capture.frame(100);
    check(old_capture.session.observe(old, 1201).reason == Reason::stale_frame,
          "OCR completion latency cannot make an old capture fresh");
    old_capture.session.begin_context(old_capture.context, 2, 1300);
    old = old_capture.frame(1299);
    check(old_capture.session.observe(old, 1400).reason == Reason::stale_frame,
          "capture must occur after the current context began");
}

void absolute_reading_intervals() {
    Fixture fixture;
    auto first = fixture.frame(100);
    first.bearing_uncertainty_deg = 0.54;
    first.mil_uncertainty = 0.4;
    (void)fixture.session.observe(first, 100);
    auto second = fixture.frame(200);
    second.bearing_uncertainty_deg = 0.54;
    second.mil_uncertainty = 0.4;
    (void)fixture.session.observe(second, 200);
    const auto intent = fixture.session.fire_intent(fixture.query(), 250, 250);
    check(intent.intent && intent.intent->bearing_uncertainty_deg == 0.54 &&
          intent.intent->mil_uncertainty == 0.4 && intent.intent->last_sight_capture_ms == 200,
          "matching HUD frames retain absolute intervals instead of narrowing them");
    auto drop = fixture.frame(300, 4, 80.0, 1000.0);
    drop.bearing_uncertainty_deg = 0.01;
    drop.mil_uncertainty = 0.01;
    (void)fixture.session.observe(drop, 300);
    auto confirm = fixture.frame(400, 4, 80.0, 1000.0);
    confirm.bearing_uncertainty_deg = 0.01;
    confirm.mil_uncertainty = 0.01;
    const auto shot = fixture.session.observe(confirm, 400);
    check(shot.shot && shot.shot->bearing_uncertainty_deg == 0.54 && shot.shot->mil_uncertainty == 0.4 &&
          shot.shot->last_sight_capture_ms == 200,
          "final shot retains pre-press capture time and intervals, never post-press reading precision");
    const auto association = fixture.session.reserve_impact(fixture.query(), 450);
    check(association.reservation && association.reservation->shot.bearing_uncertainty_deg == 0.54,
          "impact reservation preserves the actual sight uncertainty verbatim");

    Fixture unknown;
    const auto unspecified = unknown.shot();
    check(unspecified.shot && !unspecified.shot->bearing_uncertainty_deg && !unspecified.shot->mil_uncertainty,
          "missing reading intervals remain unknown instead of falsely exact zero");
    Fixture invalid;
    auto bad = invalid.frame(100);
    bad.bearing_uncertainty_deg = std::numeric_limits<double>::quiet_NaN();
    check(invalid.session.observe(bad, 100).reason == Reason::invalid_frame, "NaN reading interval is rejected");
    bad = invalid.frame(200);
    bad.mil_uncertainty = -0.1;
    check(invalid.session.observe(bad, 200).reason == Reason::invalid_frame, "negative reading interval is rejected");
}

void uncertain_counter_and_camera() {
    Fixture missing;
    missing.stable(100, std::nullopt);
    (void)missing.session.fire_intent(missing.query(), 250, 250);
    (void)missing.read(300, 4);
    (void)missing.read(400, 4);
    check(missing.session.outstanding_shots() == 0, "missing pre-press ammunition cannot confirm a decrement");

    Fixture jump;
    jump.stable();
    (void)jump.session.fire_intent(jump.query(), 250, 250);
    check(jump.read(300, 3).reason == Reason::ammunition_change_ambiguous && jump.session.outstanding_shots() == 0,
          "counter jump of two cannot be assigned to one physical press");

    Fixture transient;
    transient.stable();
    (void)transient.session.fire_intent(transient.query(), 250, 250);
    (void)transient.read(300, 4);
    check(transient.read(400, 5).reason == Reason::ammunition_change_ambiguous &&
          transient.session.outstanding_shots() == 0, "uncorroborated transient OCR decrease creates no observed shot");

    Fixture missing_between;
    missing_between.stable();
    (void)missing_between.session.fire_intent(missing_between.query(), 250, 250);
    (void)missing_between.read(300, 4);
    auto unavailable = missing_between.frame(400, std::nullopt);
    unavailable.ammo_channel.clear();
    check(missing_between.session.observe(unavailable, 400).reason == Reason::awaiting_discharge_confirmation,
          "missing ammo/channel is unknown evidence rather than a contradictory counter reading");
    check(missing_between.read(500, 4).shot.has_value(),
          "known unchanged count after unreadable ammo can corroborate the frozen intent");
    unavailable = missing_between.frame(600, std::nullopt);
    unavailable.ammo_channel.clear();
    (void)missing_between.session.observe(unavailable, 600);
    check(missing_between.read(700, 3).reason == Reason::correlation_ambiguous &&
          missing_between.session.outstanding_shots() == 1,
          "unknown intervening ammo cannot hide another untracked decrement");

    Fixture camera;
    auto first = camera.frame(100, std::nullopt);
    first.shot_camera = wardogs::ShotCameraPhase::idle;
    (void)camera.session.observe(first, 100);
    auto second = camera.frame(200, std::nullopt);
    second.shot_camera = wardogs::ShotCameraPhase::idle;
    (void)camera.session.observe(second, 200);
    (void)camera.session.fire_intent(camera.query(), 250, 250);
    auto firing = camera.frame(300, std::nullopt, 80.0, 1000.0);
    firing.shot_camera = wardogs::ShotCameraPhase::firing;
    (void)camera.session.observe(firing, 300);
    auto corroborated = camera.frame(400, std::nullopt, 80.0, 1000.0);
    corroborated.shot_camera = wardogs::ShotCameraPhase::firing;
    const auto result = camera.session.observe(corroborated, 400);
    check(result.shot && result.shot->discharge_evidence == wardogs::ShotDischargeEvidence::camera_transition &&
          result.shot->firing.bearing_deg == 10.0 && !result.shot->physical_shot_verified,
          "typed corroborated camera transition retains pre-press sight without physical-shot claims");
    auto held = camera.frame(500, std::nullopt);
    held.shot_camera = wardogs::ShotCameraPhase::firing;
    (void)camera.session.observe(held, 500);
    check(camera.session.outstanding_shots() == 1, "unchanged firing camera does not duplicate a held shot");
}

void reload_without_aiming_pose() {
    Fixture reload;
    reload.stable();
    (void)reload.session.fire_intent(reload.query(), 250, 250);
    reload.session.invalidate_sight(300);
    wardogs::DischargeFrameEvidence first;
    first.id = 3;
    first.timestamp_ms = 6000;
    first.epoch = reload.session.epoch();
    first.context = reload.context;
    first.foreground_verified = true;
    first.ammo = 4;
    first.ammo_channel = "synthetic-shell-counter";
    check(reload.session.observe_discharge(first, 6000).reason == Reason::awaiting_discharge_confirmation,
          "reticle-hidden reload can corroborate an existing pre-press intent without fake aim");
    check(reload.session.observe_discharge(first, 6010).reason == Reason::duplicate_frame &&
          reload.session.outstanding_shots() == 0, "partial reload frame cannot count twice");
    auto second = first;
    second.id = 4;
    second.timestamp_ms = 6100;
    const auto result = reload.session.observe_discharge(second, 6100);
    check(result.shot && result.shot->firing.bearing_deg == 10.0 && result.shot->firing.mil == 900.0 &&
          result.shot->last_sight_frame_id == 2 && result.shot->discharge_frame_id == 3 &&
          !result.shot->physical_shot_verified,
          "partial reload retains immutable actual pre-press sight and explicit limited provenance");
    const auto association = reload.session.reserve_impact(reload.query(), 6200);
    check(association.association_unique, "two partial counter frames allow unique frozen-shot association");
    check(reload.session.release(reservation_id(association), 6201) == Reason::released,
          "partial reload association supports retry transaction");
    check(reload.session.fire_intent(reload.query(), 6250, 6250).reason == Reason::awaiting_stable_hud,
          "partial counter evidence never supplies a fresh aiming pose for another press");

    Fixture passive;
    passive.stable();
    first.id = 3;
    first.timestamp_ms = 300;
    first.context = passive.context;
    first.epoch = passive.session.epoch();
    check(passive.session.observe_discharge(first, 300).reason == Reason::correlation_ambiguous &&
          passive.session.outstanding_shots() == 0,
          "reticle-hidden decrement without pre-press intent cannot invent a sight snapshot");

    Fixture partial_only;
    first.id = 1;
    first.timestamp_ms = 100;
    (void)partial_only.session.observe_discharge(first, 100);
    second = first;
    second.id = 2;
    second.timestamp_ms = 200;
    (void)partial_only.session.observe_discharge(second, 200);
    check(partial_only.session.fire_intent(partial_only.query(), 250, 250).reason == Reason::no_hud_evidence &&
          partial_only.session.reserve_impact(partial_only.query(), 260).provenance == Provenance::unverified_legacy,
          "two ammo-only frames cannot arm actual-sight correlation or disable unverified legacy correction");
}

void partial_hud_before_first_full_sight() {
    Fixture fixture;
    const auto partial = [&](std::uint64_t time, int ammo, wardogs::ShotCameraPhase camera) {
        wardogs::DischargeFrameEvidence value;
        value.id = ++fixture.frame_id;
        value.timestamp_ms = time;
        value.epoch = fixture.session.epoch();
        value.context = fixture.context;
        value.foreground_verified = true;
        value.ammo = ammo;
        value.ammo_channel = "synthetic-shell-counter";
        value.shot_camera = camera;
        return value;
    };
    const auto first = partial(100, 5, wardogs::ShotCameraPhase::idle);
    const auto metadata = fixture.session.observe_discharge(first, 100);
    check(metadata.reason == Reason::accepted_frame && metadata.provenance == Provenance::unverified_legacy &&
          !metadata.intent && !metadata.shot && !fixture.session.has_stable_sight(),
          "a readable shell counter without any full sight remains metadata with unverified legacy provenance");
    check(fixture.session.observe_discharge(first, 110).reason == Reason::duplicate_frame,
          "partial-only metadata still retains distinct capture identity");
    const auto unarmed_press = fixture.session.fire_intent(fixture.query(), 150, 150);
    check(unarmed_press.reason == Reason::no_hud_evidence && !unarmed_press.intent,
          "a physical press before any full sight cannot freeze a counterfeit firing snapshot");
    const auto decrement = fixture.session.observe_discharge(partial(200, 4, wardogs::ShotCameraPhase::firing), 200);
    check(decrement.reason == Reason::accepted_frame && decrement.provenance == Provenance::unverified_legacy &&
          !decrement.shot && fixture.session.outstanding_shots() == 0,
          "pre-sight ammunition and camera changes do not create an unassociated tracked discharge");
    fixture.session.invalidate_correlation(250, 150);
    fixture.session.invalidate_sight(260);
    const auto legacy = fixture.session.reserve_impact(fixture.query(), 270);
    check(legacy.reason == Reason::no_hud_evidence && legacy.provenance == Provenance::unverified_legacy &&
          !legacy.association_unique && !legacy.reservation,
          "lost input and missing sight before arming preserve the existing unverified impact workflow");
    const auto late_legacy = fixture.session.reserve_impact(fixture.query(), 120501);
    check(late_legacy.reason == Reason::no_hud_evidence && late_legacy.provenance == Provenance::unverified_legacy,
          "expiry of pre-sight metadata cannot become a tracked expired shot or quarantine");

    check(fixture.read(120600, 3).reason == Reason::awaiting_stable_hud && !fixture.session.has_stable_sight(),
          "the first full sight starts a fresh baseline rather than comparing against an earlier partial count");
    const auto armed = fixture.session.reserve_impact(fixture.query(), 120610);
    check(armed.reason == Reason::no_observed_discharge && armed.provenance == Provenance::hud_only &&
          !armed.reservation,
          "one accepted full sight already makes actual-shot association strict without inventing a stable pair");
    check(fixture.read(120700, 3).reason == Reason::accepted_frame && fixture.session.has_stable_sight(),
          "a second distinct full sight supplies a fresh usable pre-press pair after partial-only metadata");
    const auto intent = fixture.session.fire_intent(fixture.query(), 120750, 120750);
    check(intent.intent.has_value(), "a new press can freeze the newly established full sight");
    check(fixture.session.observe_discharge(partial(120800, 2, wardogs::ShotCameraPhase::unavailable), 120800).reason ==
          Reason::awaiting_discharge_confirmation,
          "a later reload-hidden decrement confirms only the newly frozen full-sight intent");
    const auto confirmed = fixture.session.observe_discharge(
        partial(120900, 2, wardogs::ShotCameraPhase::unavailable), 120900);
    check(confirmed.shot && confirmed.shot->first_sight_frame_id == 3 &&
          confirmed.shot->last_sight_frame_id == 4 && confirmed.shot->pressed_at_ms == 120750 &&
          confirmed.shot->firing.bearing_deg == 10.0 && confirmed.shot->firing.mil == 900.0 &&
          !confirmed.shot->physical_shot_verified,
          "the accepted shot retains only post-arming actual sight with explicitly limited discharge proof");
    const auto association = fixture.session.reserve_impact(fixture.query(), 120950);
    check(association.association_unique && association.reservation,
          "pre-sight metadata and lost input cannot poison a later genuinely associated shot");
    fixture.session.invalidate_sight(120960);
    check(!fixture.session.has_stable_sight() &&
          fixture.session.validate_reservation(reservation_id(association), fixture.query(), 120970) ==
              Reason::unique_association,
          "loss of live sight disables new readiness while preserving a frozen actual-shot lease");
}

void first_full_sight_keeps_strict_correlation() {
    Fixture fixture;
    (void)fixture.read(100, 5);
    fixture.session.invalidate_sight(110);
    wardogs::DischargeFrameEvidence partial;
    partial.id = ++fixture.frame_id;
    partial.timestamp_ms = 200;
    partial.epoch = fixture.session.epoch();
    partial.context = fixture.context;
    partial.foreground_verified = true;
    partial.ammo = 4;
    partial.ammo_channel = "synthetic-shell-counter";
    check(fixture.session.observe_discharge(partial, 200).reason == Reason::correlation_ambiguous,
          "a passive decrement after even one accepted full sight remains strict unassociated evidence");
    fixture.session.invalidate_correlation(210, 150);
    const auto refused = fixture.session.reserve_impact(fixture.query(), 220);
    check(refused.reason == Reason::correlation_ambiguous && refused.provenance == Provenance::hud_only &&
          !refused.association_unique && !refused.reservation && !fixture.session.has_stable_sight(),
          "partial loss and lost input after arming cannot silently restore legacy correction");
    fixture.session.begin_context(fixture.context, 2, 230);
    const auto reset = fixture.session.reserve_impact(fixture.query(), 240);
    check(reset.reason == Reason::no_hud_evidence && reset.provenance == Provenance::unverified_legacy,
          "only an explicit new context resets full-sight arming and its quarantine");
}

void body_markers_and_bounds() {
    Fixture body;
    auto first = body.frame(100);
    first.body_attitude_verified = true;
    first.body_attitude_fingerprint = "synthetic-body-a";
    (void)body.session.observe(first, 100);
    auto second = body.frame(200);
    second.body_attitude_verified = true;
    second.body_attitude_fingerprint = "synthetic-body-a";
    check(!body.session.observe(second, 200).invalidate_learned_prior,
          "initial confirmed body marker does not invent a body change");
    (void)body.session.fire_intent(body.query(), 250, 250);
    auto drop = body.frame(300, 4);
    drop.body_attitude_verified = true;
    drop.body_attitude_fingerprint = "synthetic-body-a";
    (void)body.session.observe(drop, 300);
    auto confirm = body.frame(400, 4);
    confirm.body_attitude_verified = true;
    confirm.body_attitude_fingerprint = "synthetic-body-a";
    (void)body.session.observe(confirm, 400);
    auto noise = body.frame(500, 4, 70.0, 1000.0);
    noise.body_attitude_fingerprint = "unverified-aim-or-reticle";
    check(!body.session.observe(noise, 500).invalidate_learned_prior && body.session.outstanding_shots() == 1,
          "unverified pose/aim changes cannot invalidate a learned body context");
    auto changed = body.frame(600, 4);
    changed.body_attitude_verified = true;
    changed.body_attitude_fingerprint = "synthetic-body-b";
    check(!body.session.observe(changed, 600).invalidate_learned_prior,
          "one changed marker is insufficient to confirm body movement");
    changed = body.frame(700, 4);
    changed.body_attitude_verified = true;
    changed.body_attitude_fingerprint = "synthetic-body-b";
    const auto update = body.session.observe(changed, 700);
    check(update.reason == Reason::body_attitude_changed && update.invalidate_learned_prior &&
          body.session.outstanding_shots() == 0, "two confirmed changed body markers cancel old shot context and learned prior");

    Fixture expired;
    (void)expired.shot();
    const auto lease = expired.session.reserve_impact(expired.query(), 450);
    check(expired.session.validate_reservation(reservation_id(lease), expired.query(), 120301) == Reason::expired &&
          expired.session.outstanding_shots() == 0, "pending timeout invalidates a lease explicitly");

    wardogs::ShotObservationPolicy bounded;
    bounded.maximum_outstanding_shots = 1;
    Fixture capacity(bounded);
    (void)capacity.shot();
    (void)capacity.read(500, 4);
    (void)capacity.session.fire_intent(capacity.query(), 550, 550);
    (void)capacity.read(600, 3);
    check(capacity.read(700, 3).reason == Reason::capacity_exhausted && capacity.session.outstanding_shots() == 1 &&
          capacity.session.reserve_impact(capacity.query(), 750).reason == Reason::capacity_exhausted,
          "capacity exhaustion preserves old snapshot and blocks ambiguous substitution");

    Fixture unstabilized;
    auto one = unstabilized.frame(100);
    one.stabilized = false;
    (void)unstabilized.session.observe(one, 100);
    auto two = unstabilized.frame(200);
    two.stabilized = false;
    (void)unstabilized.session.observe(two, 200);
    check(unstabilized.session.fire_intent(unstabilized.query(), 250, 250).reason == Reason::stabilization_not_ready &&
          unstabilized.session.outstanding_shots() == 0, "explicit unstable HUD cannot freeze an eligible firing sight");
    bounded.maximum_outstanding_shots = 0;
    rejects([&] { wardogs::ShotObservationSession invalid(bounded); }, "zero-capacity policy is rejected");
    bounded.maximum_outstanding_shots = 8;
    bounded.maximum_stable_frame_gap_ms = bounded.minimum_frame_separation_ms - 1;
    rejects([&] { wardogs::ShotObservationSession invalid(bounded); }, "inverted stable-frame policy is rejected");
}

}  // namespace

int main() {
    capture_press_ordering();
    bounded_source_capture_times();
    pre_press_and_retry();
    intents_are_not_shots();
    asynchronous_ambiguity();
    lost_input_quarantines_context();
    invalidation_and_context();
    frame_quality_and_timing();
    absolute_reading_intervals();
    uncertain_counter_and_camera();
    reload_without_aiming_pose();
    partial_hud_before_first_full_sight();
    first_full_sight_keeps_strict_correlation();
    body_markers_and_bounds();
    if (failures != 0) return 1;
    std::cout << "Shot observation tests passed\n";
    return 0;
}
