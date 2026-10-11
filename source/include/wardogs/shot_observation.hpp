#pragma once

#include "wardogs/continuous_calibration.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>

namespace wardogs {

// These are bounded capture/association policies, not measured flight times.
struct ShotObservationPolicy {
    std::uint64_t maximum_hud_age_ms{1000};
    std::uint64_t minimum_frame_separation_ms{40};
    std::uint64_t maximum_stable_frame_gap_ms{2500};
    std::uint64_t maximum_intent_confirmation_age_ms{15000};
    std::uint64_t maximum_pending_age_ms{120000};
    std::size_t maximum_outstanding_shots{8};
    double bearing_tolerance_deg{0.05};
    double mil_tolerance{0.5};
};

struct ShotEvidenceContext {
    std::string map_id;
    Point base;
    Arc arc{Arc::low};

    bool operator==(const ShotEvidenceContext&) const = default;
};

enum class ShotCameraPhase { unavailable, idle, firing };

struct AcceptedSightFrame {
    std::uint64_t id{};
    // Monotonic capture time, BEFORE OCR; completion time is not capture time.
    std::uint64_t timestamp_ms{};
    std::uint64_t epoch{};
    FiringSnapshot firing_snapshot;
    std::uint64_t command_id{};
    ShotEvidenceContext context;
    // The adapter sets this only for a validated foreground/client capture.
    bool foreground_verified{};
    std::optional<int> ammo;
    std::string ammo_channel;
    std::optional<bool> stabilized;
    ShotCameraPhase shot_camera{ShotCameraPhase::unavailable};
    // Aim AZ/MIL/reticle MUST NOT be used as a body-attitude fingerprint.
    std::string body_attitude_fingerprint;
    bool body_attitude_verified{};
    // Absolute reading intervals, copied verbatim. Pair agreement does not
    // reduce them, and missing intervals remain unknown rather than zero.
    std::optional<double> bearing_uncertainty_deg;
    std::optional<double> mil_uncertainty;
};

// Reload/shot-camera screens can hide the reticle. This evidence carries no
// aiming values and can only corroborate an already frozen pre-press intent.
struct DischargeFrameEvidence {
    std::uint64_t id{};
    std::uint64_t timestamp_ms{};
    std::uint64_t epoch{};
    ShotEvidenceContext context;
    bool foreground_verified{};
    std::optional<int> ammo;
    std::string ammo_channel;
    std::optional<bool> stabilized;
    ShotCameraPhase shot_camera{ShotCameraPhase::unavailable};
};

enum class ShotEvidenceReason {
    accepted_frame,
    awaiting_stable_hud,
    awaiting_discharge_confirmation,
    discharge_observed,
    intent_without_discharge,
    duplicate_intent,
    multiple_fire_intents,
    no_hud_evidence,
    no_observed_discharge,
    invalid_frame,
    stale_frame,
    duplicate_frame,
    stale_epoch,
    context_changed,
    target_changed,
    stabilization_not_ready,
    ammunition_channel_changed,
    reload_observed,
    ammunition_change_ambiguous,
    sight_changed_during_discharge,
    body_attitude_changed,
    multiple_outstanding_shots,
    correlation_ambiguous,
    already_reserved,
    reservation_not_found,
    expired,
    capacity_exhausted,
    canceled,
    time_regression,
    unique_association,
    committed,
    released
};

enum class ShotEvidenceProvenance {
    unverified_legacy,
    hud_only,
    hud_discharge_correlated
};

enum class ShotDischargeEvidence { ammunition_decrement, camera_transition, both };

struct ObservedFireIntentSnapshot {
    std::uint64_t intent_id{};
    std::uint64_t epoch{};
    ShotEvidenceContext context;
    FiringSnapshot firing;
    std::uint64_t command_id{};
    std::uint64_t first_sight_frame_id{};
    std::uint64_t last_sight_frame_id{};
    std::uint64_t last_sight_capture_ms{};
    std::uint64_t pressed_at_ms{};
    std::optional<double> bearing_uncertainty_deg;
    std::optional<double> mil_uncertainty;
};

struct ObservedShotSnapshot {
    std::uint64_t shot_id{};
    std::uint64_t intent_id{};
    std::uint64_t epoch{};
    ShotEvidenceContext context;
    FiringSnapshot firing;
    std::uint64_t command_id{};
    std::uint64_t first_sight_frame_id{};
    std::uint64_t last_sight_frame_id{};
    std::uint64_t last_sight_capture_ms{};
    std::uint64_t discharge_frame_id{};
    std::uint64_t discharged_at_ms{};
    std::uint64_t pressed_at_ms{};
    ShotDischargeEvidence discharge_evidence{ShotDischargeEvidence::ammunition_decrement};
    std::string body_attitude_fingerprint;
    bool hud_verified{true};
    bool discharge_observed{true};
    bool correlation_ambiguous{};
    // HUD/ammo/camera correlation does not verify a physical shot or impact.
    bool physical_shot_verified{false};
    std::optional<double> bearing_uncertainty_deg;
    std::optional<double> mil_uncertainty;
};

struct ShotEvidenceUpdate {
    ShotEvidenceReason reason{ShotEvidenceReason::accepted_frame};
    ShotEvidenceProvenance provenance{ShotEvidenceProvenance::unverified_legacy};
    bool invalidate_learned_prior{};
    std::optional<ObservedShotSnapshot> shot;
    std::optional<ObservedFireIntentSnapshot> intent;
};

struct ShotImpactQuery {
    ShotEvidenceContext context;
    std::uint64_t epoch{};
    Point target;
    double target_height_delta_m{};
};

struct ShotImpactReservation {
    std::uint64_t reservation_id{};
    ObservedShotSnapshot shot;
};

struct ShotImpactAssociation {
    ShotEvidenceReason reason{ShotEvidenceReason::no_hud_evidence};
    ShotEvidenceProvenance provenance{ShotEvidenceProvenance::unverified_legacy};
    bool association_unique{};
    std::optional<ShotImpactReservation> reservation;
};

// A serialized/UI-thread session. Never reads input devices or the screen.
// Target/height/command ID come from the capture-time context; bearing/MIL
// MUST be actual parsed HUD values, never a displayed-command fallback.
class ShotObservationSession {
public:
    explicit ShotObservationSession(ShotObservationPolicy policy = {});

    // Caller increments epoch on pause/reset/base/map/arc changes. Reusing an
    // old epoch is rejected, preventing already queued OCR from re-entering.
    void begin_context(ShotEvidenceContext context, std::uint64_t epoch,
                       std::uint64_t now_ms);
    void cancel(std::uint64_t now_ms);
    // Missing/invalid/foreground-lost HUD forbids new intents while retaining
    // already frozen pre-press intentions and outstanding observed shots.
    void invalidate_sight(std::uint64_t now_ms);
    // A lost/coalesced input makes the current shot association unknowable.
    // Keep frozen evidence, but refuse association until explicit context reset.
    // This does not fabricate a press, sight reading, or physical discharge.
    // A supplied physical timestamp must belong to this context and cannot be
    // in the future. Old listener callbacks cannot poison a newer generation.
    void invalidate_correlation(
        std::uint64_t now_ms, std::optional<std::uint64_t> physical_pressed_at_ms = std::nullopt);
    [[nodiscard]] ShotEvidenceUpdate observe(const AcceptedSightFrame& frame,
                                             std::uint64_t now_ms);
    [[nodiscard]] ShotEvidenceUpdate observe_discharge(
        const DischargeFrameEvidence& frame, std::uint64_t now_ms);
    // Intent freezes PRE-PRESS actual sight but cannot enqueue a shot. The
    // adapter supplies real physical button-down time, not callback time.
    [[nodiscard]] ShotEvidenceUpdate fire_intent(
        const ShotImpactQuery& query, std::uint64_t pressed_at_ms, std::uint64_t now_ms);
    // Convenience for synchronous producers/tests with the current target.
    [[nodiscard]] ShotEvidenceUpdate fire_intent(std::uint64_t now_ms);
    [[nodiscard]] ShotImpactAssociation reserve_impact(
        const ShotImpactQuery& query, std::uint64_t now_ms);
    [[nodiscard]] ShotEvidenceReason commit(std::uint64_t reservation_id,
                                            std::uint64_t now_ms);
    // Call immediately before mutating calibration. A new unresolved intent
    // or discharge during asynchronous OCR makes the old reservation invalid.
    [[nodiscard]] ShotEvidenceReason validate_reservation(
        std::uint64_t reservation_id, const ShotImpactQuery& query, std::uint64_t now_ms);
    [[nodiscard]] ShotEvidenceReason release(std::uint64_t reservation_id,
                                             std::uint64_t now_ms);
    [[nodiscard]] std::size_t outstanding_shots() const noexcept;
    [[nodiscard]] std::uint64_t epoch() const noexcept;
    // Readiness still requires the caller to check capture age and context.
    [[nodiscard]] bool has_stable_sight() const noexcept;

private:
    struct PendingShot {
        ObservedShotSnapshot snapshot;
        std::uint64_t reservation_id{};
    };
    struct DischargeCandidate {
        AcceptedSightFrame before;
        std::uint64_t first_sight_frame_id{};
        DischargeFrameEvidence after;
        ShotDischargeEvidence evidence;
        bool ambiguous{};
        ObservedFireIntentSnapshot intent;
    };
    struct FireIntent {
        ObservedFireIntentSnapshot snapshot;
        AcceptedSightFrame before;
    };

    [[nodiscard]] bool advance_time(std::uint64_t now_ms);
    void expire(std::uint64_t now_ms);
    void clear_inputs();
    [[nodiscard]] bool same_sight(const AcceptedSightFrame& left,
                                  const AcceptedSightFrame& right) const;
    [[nodiscard]] ShotEvidenceProvenance provenance() const noexcept;
    [[nodiscard]] std::optional<ShotEvidenceUpdate> correlate_discharge(
        const DischargeFrameEvidence& frame, std::uint64_t now_ms);

    ShotObservationPolicy policy_;
    ShotEvidenceContext context_;
    std::uint64_t epoch_{};
    std::uint64_t last_now_ms_{};
    std::uint64_t last_frame_id_{};
    std::uint64_t last_capture_ms_{};
    std::uint64_t next_shot_id_{1};
    std::uint64_t next_intent_id_{1};
    std::uint64_t next_reservation_id_{1};
    std::uint64_t context_started_at_ms_{};
    std::optional<std::uint64_t> last_intent_press_ms_;
    bool active_{};
    bool hud_seen_{};
    bool correlation_invalidated_{};
    bool expired_shot_{};
    std::optional<std::uint64_t> unassociated_discharge_at_ms_;
    ShotEvidenceReason unassociated_reason_{ShotEvidenceReason::correlation_ambiguous};
    std::optional<AcceptedSightFrame> previous_;
    std::optional<DischargeFrameEvidence> previous_ammunition_;
    std::optional<DischargeFrameEvidence> previous_camera_;
    std::optional<AcceptedSightFrame> stable_;
    std::uint64_t stable_first_frame_id_{};
    std::optional<DischargeCandidate> discharge_;
    std::deque<FireIntent> intents_;
    std::string confirmed_pose_;
    std::string candidate_pose_;
    std::uint64_t candidate_pose_frame_id_{};
    std::uint64_t candidate_pose_time_ms_{};
    std::deque<PendingShot> pending_;
};

}  // namespace wardogs
