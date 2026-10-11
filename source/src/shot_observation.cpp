#include "wardogs/shot_observation.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace wardogs {
namespace {

bool finite(Point point) {
    return std::isfinite(point.x) && std::isfinite(point.y);
}

bool valid_context(const ShotEvidenceContext& context) {
    return !context.map_id.empty() && context.map_id.size() <= 128 &&
        finite(context.base) && (context.arc == Arc::low || context.arc == Arc::high);
}

bool height_equal(double left, double right) {
    return std::isfinite(left) && std::isfinite(right) && std::abs(left - right) <= 1e-9;
}

bool valid_uncertainty(std::optional<double> interval) {
    return !interval || (std::isfinite(*interval) && *interval >= 0.0);
}

}  // namespace

ShotObservationSession::ShotObservationSession(ShotObservationPolicy policy)
    : policy_(policy) {
    if (policy.maximum_hud_age_ms == 0 || policy.maximum_hud_age_ms > 60000 ||
        policy.minimum_frame_separation_ms == 0 ||
        policy.minimum_frame_separation_ms > policy.maximum_stable_frame_gap_ms ||
        policy.maximum_stable_frame_gap_ms > 60000 ||
        policy.maximum_intent_confirmation_age_ms < policy.maximum_hud_age_ms ||
        policy.maximum_intent_confirmation_age_ms > 120000 ||
        policy.maximum_pending_age_ms < policy.maximum_hud_age_ms ||
        policy.maximum_pending_age_ms > 600000 ||
        policy.maximum_outstanding_shots == 0 || policy.maximum_outstanding_shots > 64 ||
        !std::isfinite(policy.bearing_tolerance_deg) || policy.bearing_tolerance_deg < 0 ||
        policy.bearing_tolerance_deg > 1 || !std::isfinite(policy.mil_tolerance) ||
        policy.mil_tolerance < 0 || policy.mil_tolerance > 10) {
        throw std::invalid_argument("Invalid shot observation policy");
    }
}

bool ShotObservationSession::advance_time(std::uint64_t now_ms) {
    if (now_ms < last_now_ms_) return false;
    last_now_ms_ = now_ms;
    return true;
}

void ShotObservationSession::clear_inputs() {
    previous_.reset();
    previous_ammunition_.reset();
    previous_camera_.reset();
    stable_.reset();
    discharge_.reset();
    intents_.clear();
    stable_first_frame_id_ = 0;
    candidate_pose_.clear();
    candidate_pose_frame_id_ = 0;
    candidate_pose_time_ms_ = 0;
}

void ShotObservationSession::begin_context(
    ShotEvidenceContext context, std::uint64_t epoch, std::uint64_t now_ms) {
    if (!valid_context(context) || epoch == 0 || epoch <= epoch_ || now_ms < last_now_ms_) {
        throw std::invalid_argument("Shot context requires a new epoch and monotonic time");
    }
    context_ = std::move(context);
    epoch_ = epoch;
    last_now_ms_ = now_ms;
    context_started_at_ms_ = now_ms;
    last_intent_press_ms_.reset();
    last_frame_id_ = last_capture_ms_ = 0;
    active_ = true;
    hud_seen_ = expired_shot_ = false;
    correlation_invalidated_ = false;
    pending_.clear();
    unassociated_discharge_at_ms_.reset();
    confirmed_pose_.clear();
    clear_inputs();
}

void ShotObservationSession::cancel(std::uint64_t now_ms) {
    if (!advance_time(now_ms)) throw std::invalid_argument("Shot cancellation time regressed");
    active_ = false;
    hud_seen_ = expired_shot_ = false;
    correlation_invalidated_ = false;
    pending_.clear();
    unassociated_discharge_at_ms_.reset();
    confirmed_pose_.clear();
    clear_inputs();
}

void ShotObservationSession::invalidate_sight(std::uint64_t now_ms) {
    if (!advance_time(now_ms)) throw std::invalid_argument("Sight invalidation time regressed");
    previous_.reset();
    stable_.reset();
    stable_first_frame_id_ = 0;
}

void ShotObservationSession::invalidate_correlation(
    std::uint64_t now_ms, std::optional<std::uint64_t> physical_pressed_at_ms) {
    if (!advance_time(now_ms)) throw std::invalid_argument("Correlation invalidation time regressed");
    if (!active_ || !hud_seen_) return;
    if (physical_pressed_at_ms &&
        (*physical_pressed_at_ms < context_started_at_ms_ || *physical_pressed_at_ms > now_ms)) return;
    expire(now_ms);
    correlation_invalidated_ = true;
    unassociated_discharge_at_ms_ = now_ms;
    unassociated_reason_ = ShotEvidenceReason::correlation_ambiguous;
    for (auto& pending : pending_) pending.snapshot.correlation_ambiguous = true;
    if (discharge_) discharge_->ambiguous = true;
    invalidate_sight(now_ms);
}

ShotEvidenceProvenance ShotObservationSession::provenance() const noexcept {
    return hud_seen_ ? ShotEvidenceProvenance::hud_only : ShotEvidenceProvenance::unverified_legacy;
}

void ShotObservationSession::expire(std::uint64_t now_ms) {
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (now_ms - it->snapshot.discharged_at_ms > policy_.maximum_pending_age_ms) {
            it = pending_.erase(it);
            expired_shot_ = true;
        } else {
            ++it;
        }
    }
    if (unassociated_discharge_at_ms_ &&
        now_ms - *unassociated_discharge_at_ms_ > policy_.maximum_pending_age_ms) {
        unassociated_discharge_at_ms_.reset();
        expired_shot_ = true;
    }
    if (stable_ && now_ms - stable_->timestamp_ms > policy_.maximum_hud_age_ms) {
        stable_.reset();
        stable_first_frame_id_ = 0;
    }
    while (!intents_.empty() &&
           now_ms - intents_.front().snapshot.pressed_at_ms > policy_.maximum_intent_confirmation_age_ms) {
        unassociated_discharge_at_ms_ = intents_.front().snapshot.pressed_at_ms;
        unassociated_reason_ = ShotEvidenceReason::expired;
        intents_.pop_front();
    }
    if (discharge_ && now_ms - discharge_->intent.pressed_at_ms > policy_.maximum_intent_confirmation_age_ms) {
        // An observed transition without timely corroboration remains an
        // unresolved discharge, preventing a different shot being substituted.
        unassociated_discharge_at_ms_ = discharge_->after.timestamp_ms;
        unassociated_reason_ = ShotEvidenceReason::correlation_ambiguous;
        discharge_.reset();
    }
}

bool ShotObservationSession::same_sight(
    const AcceptedSightFrame& left, const AcceptedSightFrame& right) const {
    return left.epoch == right.epoch && left.context == right.context &&
        left.firing_snapshot.arc == right.firing_snapshot.arc &&
        left.firing_snapshot.target == right.firing_snapshot.target &&
        height_equal(left.firing_snapshot.target_height_delta_m, right.firing_snapshot.target_height_delta_m) &&
        std::abs(std::remainder(left.firing_snapshot.bearing_deg - right.firing_snapshot.bearing_deg, 360.0)) <=
            policy_.bearing_tolerance_deg &&
        std::abs(left.firing_snapshot.mil - right.firing_snapshot.mil) <= policy_.mil_tolerance;
}

ShotEvidenceUpdate ShotObservationSession::observe(
    const AcceptedSightFrame& frame, std::uint64_t now_ms) {
    ShotEvidenceUpdate result{ShotEvidenceReason::accepted_frame, provenance()};
    if (!advance_time(now_ms)) {
        result.reason = ShotEvidenceReason::time_regression;
        return result;
    }
    expire(now_ms);
    if (!active_) {
        result.reason = ShotEvidenceReason::canceled;
        return result;
    }
    if (frame.epoch != epoch_) {
        result.reason = ShotEvidenceReason::stale_epoch;
        return result;
    }
    if (frame.context != context_) {
        result.reason = ShotEvidenceReason::context_changed;
        return result;
    }
    const auto [minimum_mil, maximum_mil] = sph2_mil_bounds(context_.arc);
    if (frame.id == 0 || !frame.foreground_verified || !finite(frame.firing_snapshot.target) ||
        frame.firing_snapshot.target == context_.base || frame.firing_snapshot.arc != context_.arc ||
        !std::isfinite(frame.firing_snapshot.bearing_deg) || frame.firing_snapshot.bearing_deg < 0 ||
        frame.firing_snapshot.bearing_deg > 360 || !std::isfinite(frame.firing_snapshot.mil) ||
        frame.firing_snapshot.mil < minimum_mil || frame.firing_snapshot.mil > maximum_mil ||
        !std::isfinite(frame.firing_snapshot.target_height_delta_m) ||
        (frame.ammo && *frame.ammo < 0) || frame.ammo_channel.size() > 128 ||
        frame.body_attitude_fingerprint.size() > 256 || frame.timestamp_ms > now_ms) {
        invalidate_sight(now_ms);
        result.reason = ShotEvidenceReason::invalid_frame;
        return result;
    }
    if (!valid_uncertainty(frame.bearing_uncertainty_deg) || !valid_uncertainty(frame.mil_uncertainty)) {
        invalidate_sight(now_ms);
        result.reason = ShotEvidenceReason::invalid_frame;
        return result;
    }
    if (now_ms - frame.timestamp_ms > policy_.maximum_hud_age_ms ||
        frame.timestamp_ms < context_started_at_ms_ || frame.timestamp_ms < last_capture_ms_ || frame.id < last_frame_id_) {
        result.reason = ShotEvidenceReason::stale_frame;
        return result;
    }
    if (frame.id == last_frame_id_) {
        result.reason = ShotEvidenceReason::duplicate_frame;
        return result;
    }
    last_frame_id_ = frame.id;
    last_capture_ms_ = frame.timestamp_ms;
    hud_seen_ = true;
    result.provenance = ShotEvidenceProvenance::hud_only;

    if (frame.body_attitude_verified && !frame.body_attitude_fingerprint.empty()) {
        const auto& marker = frame.body_attitude_fingerprint;
        if (marker == confirmed_pose_) {
            candidate_pose_.clear();
            candidate_pose_frame_id_ = 0;
        } else if (marker == candidate_pose_ && frame.id != candidate_pose_frame_id_ &&
                   frame.timestamp_ms - candidate_pose_time_ms_ >= policy_.minimum_frame_separation_ms &&
                   frame.timestamp_ms - candidate_pose_time_ms_ <= policy_.maximum_stable_frame_gap_ms) {
            const bool changed = !confirmed_pose_.empty();
            confirmed_pose_ = marker;
            candidate_pose_.clear();
            candidate_pose_frame_id_ = 0;
            if (changed) {
                clear_inputs();
                pending_.clear();
                unassociated_discharge_at_ms_.reset();
                result.reason = ShotEvidenceReason::body_attitude_changed;
                result.invalidate_learned_prior = true;
                previous_ = frame;
            return result;
            }
        } else {
            candidate_pose_ = marker;
            candidate_pose_frame_id_ = frame.id;
            candidate_pose_time_ms_ = frame.timestamp_ms;
        }
    } else {
        // Missing/unverified pose evidence cannot corroborate a body change.
        candidate_pose_.clear();
        candidate_pose_frame_id_ = 0;
    }

    const DischargeFrameEvidence discharge_frame{frame.id, frame.timestamp_ms, frame.epoch,
        frame.context, frame.foreground_verified, frame.ammo, frame.ammo_channel,
        frame.stabilized, frame.shot_camera};
    if (auto correlated = correlate_discharge(discharge_frame, now_ms)) {
        stable_.reset();
        stable_first_frame_id_ = 0;
        previous_ = frame;
        return *correlated;
    }

    if (previous_ && frame.timestamp_ms - previous_->timestamp_ms <= policy_.maximum_stable_frame_gap_ms &&
        same_sight(*previous_, frame) && previous_->ammo == frame.ammo &&
        previous_->shot_camera == frame.shot_camera && previous_->ammo_channel == frame.ammo_channel) {
        if (frame.timestamp_ms - previous_->timestamp_ms >= policy_.minimum_frame_separation_ms) {
            stable_first_frame_id_ = previous_->id;
            stable_ = frame;
            previous_ = frame;
            result.reason = frame.stabilized == false ? ShotEvidenceReason::stabilization_not_ready :
                ShotEvidenceReason::accepted_frame;
        } else {
            result.reason = ShotEvidenceReason::awaiting_stable_hud;
        }
    } else {
        stable_.reset();
        stable_first_frame_id_ = 0;
        previous_ = frame;
        result.reason = ShotEvidenceReason::awaiting_stable_hud;
    }
    return result;
}

std::optional<ShotEvidenceUpdate> ShotObservationSession::correlate_discharge(
    const DischargeFrameEvidence& frame, std::uint64_t now_ms) {
    ShotEvidenceUpdate result{ShotEvidenceReason::accepted_frame, provenance()};
    const auto previous_ammunition = previous_ammunition_;
    const auto previous_camera = previous_camera_;
    if (frame.ammo) previous_ammunition_ = frame;
    if (frame.shot_camera != ShotCameraPhase::unavailable) previous_camera_ = frame;
    if (previous_ammunition && !previous_ammunition->ammo_channel.empty() &&
        !frame.ammo_channel.empty() && frame.ammo_channel != previous_ammunition->ammo_channel) {
        invalidate_sight(now_ms);
        if (!intents_.empty() || discharge_) {
            unassociated_discharge_at_ms_ = frame.timestamp_ms;
            unassociated_reason_ = ShotEvidenceReason::ammunition_channel_changed;
            intents_.clear();
            discharge_.reset();
        }
        result.reason = ShotEvidenceReason::ammunition_channel_changed;
        return result;
    }

    if (discharge_) {
        const auto candidate = *discharge_;
        const bool ammo_proof = candidate.evidence != ShotDischargeEvidence::camera_transition;
        const bool camera_proof = candidate.evidence != ShotDischargeEvidence::ammunition_decrement;
        const bool corroborates = frame.timestamp_ms - candidate.after.timestamp_ms >= policy_.minimum_frame_separation_ms &&
            (!ammo_proof || (frame.ammo == candidate.after.ammo &&
                            frame.ammo_channel == candidate.after.ammo_channel)) &&
            (!camera_proof || frame.shot_camera == ShotCameraPhase::firing);
        if (corroborates) {
            discharge_.reset();
            if (pending_.size() >= policy_.maximum_outstanding_shots ||
                next_shot_id_ == std::numeric_limits<std::uint64_t>::max()) {
                unassociated_discharge_at_ms_ = candidate.after.timestamp_ms;
                unassociated_reason_ = ShotEvidenceReason::capacity_exhausted;
                result.reason = ShotEvidenceReason::capacity_exhausted;
            } else {
                ObservedShotSnapshot shot;
                shot.shot_id = next_shot_id_++;
                shot.intent_id = candidate.intent.intent_id;
                shot.epoch = epoch_;
                shot.context = context_;
                shot.firing = candidate.before.firing_snapshot;
                shot.command_id = candidate.before.command_id;
                shot.first_sight_frame_id = candidate.first_sight_frame_id;
                shot.last_sight_frame_id = candidate.before.id;
                shot.last_sight_capture_ms = candidate.intent.last_sight_capture_ms;
                shot.discharge_frame_id = candidate.after.id;
                shot.discharged_at_ms = candidate.after.timestamp_ms;
                shot.pressed_at_ms = candidate.intent.pressed_at_ms;
                shot.discharge_evidence = candidate.evidence;
                shot.body_attitude_fingerprint = candidate.before.body_attitude_verified ?
                    candidate.before.body_attitude_fingerprint : std::string{};
                shot.correlation_ambiguous = candidate.ambiguous;
                shot.bearing_uncertainty_deg = candidate.intent.bearing_uncertainty_deg;
                shot.mil_uncertainty = candidate.intent.mil_uncertainty;
                pending_.push_back({shot, 0});
                expired_shot_ = false;
                result.reason = candidate.ambiguous ? ShotEvidenceReason::correlation_ambiguous :
                    ShotEvidenceReason::discharge_observed;
                result.provenance = ShotEvidenceProvenance::hud_discharge_correlated;
                result.shot = shot;
            }
            // Start a fresh post-discharge stable pair; no repeated decrement
            // can be inferred by comparing again with the pre-shot ammo count.
            stable_.reset();
            stable_first_frame_id_ = 0;
            return result;
        }
        if ((ammo_proof && frame.ammo && frame.ammo != candidate.after.ammo) ||
            (camera_proof && frame.shot_camera != ShotCameraPhase::unavailable &&
             frame.shot_camera != ShotCameraPhase::firing)) {
            discharge_.reset();
            unassociated_discharge_at_ms_ = candidate.after.timestamp_ms;
            unassociated_reason_ = ShotEvidenceReason::ammunition_change_ambiguous;
            stable_.reset();
            result.reason = ShotEvidenceReason::ammunition_change_ambiguous;
            return result;
        }
        result.reason = ShotEvidenceReason::awaiting_discharge_confirmation;
        return result;
    }

    // Only an explicit physical pre-press intent supplies a sight snapshot.
    // An ammunition counter may decrement on reload rather than at firing;
    // comparing against the latest post-press aim would fabricate evidence.
    if (!intents_.empty() && frame.timestamp_ms >= intents_.front().snapshot.pressed_at_ms) {
        const auto intent = intents_.front();
        const auto& before = intent.before;
        const bool same_channel = before.ammo_channel == frame.ammo_channel;
        const bool decreased = same_channel && before.ammo && frame.ammo && *frame.ammo < *before.ammo;
        const bool camera_transition = before.shot_camera == ShotCameraPhase::idle &&
            frame.shot_camera == ShotCameraPhase::firing;
        if ((decreased || camera_transition) && intents_.size() > 1) {
            unassociated_discharge_at_ms_ = frame.timestamp_ms;
            unassociated_reason_ = ShotEvidenceReason::multiple_fire_intents;
            intents_.clear();
            invalidate_sight(now_ms);
            result.reason = ShotEvidenceReason::multiple_fire_intents;
            return result;
        }
        const bool multiple_recent_decrements = previous_ammunition && frame.ammo &&
            previous_ammunition->ammo_channel == frame.ammo_channel &&
            *previous_ammunition->ammo - *frame.ammo > 1;
        if (decreased && (*before.ammo - *frame.ammo != 1 || multiple_recent_decrements)) {
            unassociated_discharge_at_ms_ = frame.timestamp_ms;
            unassociated_reason_ = ShotEvidenceReason::ammunition_change_ambiguous;
            intents_.clear();
            invalidate_sight(now_ms);
            result.reason = ShotEvidenceReason::ammunition_change_ambiguous;
            return result;
        }
        if (decreased || camera_transition) {
            const auto kind = decreased && camera_transition ? ShotDischargeEvidence::both :
                decreased ? ShotDischargeEvidence::ammunition_decrement : ShotDischargeEvidence::camera_transition;
            discharge_ = DischargeCandidate{before, intent.snapshot.first_sight_frame_id, frame, kind,
                correlation_invalidated_ || unassociated_discharge_at_ms_.has_value(), intent.snapshot};
            intents_.pop_front();
            result.reason = ShotEvidenceReason::awaiting_discharge_confirmation;
            return result;
        }
    }

    const bool decreased = previous_ammunition && frame.ammo &&
        previous_ammunition->ammo_channel == frame.ammo_channel &&
        *frame.ammo < *previous_ammunition->ammo;
    const bool camera_transition = previous_camera && previous_camera->shot_camera == ShotCameraPhase::idle &&
        frame.shot_camera == ShotCameraPhase::firing;
    if (intents_.empty() && (decreased || camera_transition)) {
        unassociated_discharge_at_ms_ = frame.timestamp_ms;
        unassociated_reason_ = ShotEvidenceReason::correlation_ambiguous;
        invalidate_sight(now_ms);
        result.reason = ShotEvidenceReason::correlation_ambiguous;
        return result;
    }
    if (previous_ammunition && frame.ammo && *frame.ammo > *previous_ammunition->ammo) {
        // A reload invalidates the current sight, not the frozen intent.
        invalidate_sight(now_ms);
        if (!intents_.empty()) {
            // Counter replenishment changes its baseline. A later decrement
            // cannot uniquely establish the original press's discharge.
            unassociated_discharge_at_ms_ = frame.timestamp_ms;
            unassociated_reason_ = ShotEvidenceReason::ammunition_change_ambiguous;
        }
        result.reason = ShotEvidenceReason::reload_observed;
        return result;
    }

    return std::nullopt;
}

ShotEvidenceUpdate ShotObservationSession::observe_discharge(
    const DischargeFrameEvidence& frame, std::uint64_t now_ms) {
    ShotEvidenceUpdate result{ShotEvidenceReason::accepted_frame, provenance()};
    if (!advance_time(now_ms)) {
        result.reason = ShotEvidenceReason::time_regression;
        return result;
    }
    expire(now_ms);
    if (!active_) result.reason = ShotEvidenceReason::canceled;
    else if (frame.epoch != epoch_) result.reason = ShotEvidenceReason::stale_epoch;
    else if (frame.context != context_) result.reason = ShotEvidenceReason::context_changed;
    else if (frame.id == 0 || !frame.foreground_verified || frame.timestamp_ms > now_ms ||
             (frame.ammo && *frame.ammo < 0) || frame.ammo_channel.size() > 128) {
        invalidate_sight(now_ms);
        result.reason = ShotEvidenceReason::invalid_frame;
    } else if (now_ms - frame.timestamp_ms > policy_.maximum_hud_age_ms ||
               frame.timestamp_ms < context_started_at_ms_ || frame.timestamp_ms < last_capture_ms_ ||
               frame.id < last_frame_id_) result.reason = ShotEvidenceReason::stale_frame;
    else if (frame.id == last_frame_id_) result.reason = ShotEvidenceReason::duplicate_frame;
    else {
        last_frame_id_ = frame.id;
        last_capture_ms_ = frame.timestamp_ms;
        // A shell counter alone never activates actual-sight correlation.
        // Preserve the existing unverified correction path until a complete
        // accepted sight has been seen. In particular, pre-sight counter
        // changes cannot create a baseline or retroactive ambiguous shot.
        if (!hud_seen_) return result;
        result.provenance = ShotEvidenceProvenance::hud_only;
        if (auto correlated = correlate_discharge(frame, now_ms)) result = *correlated;
        // A partial reload reading cannot become a fresh aiming frame.
        invalidate_sight(now_ms);
    }
    return result;
}

ShotEvidenceUpdate ShotObservationSession::fire_intent(
    const ShotImpactQuery& query, std::uint64_t pressed_at_ms, std::uint64_t now_ms) {
    ShotEvidenceUpdate result{ShotEvidenceReason::intent_without_discharge, provenance()};
    if (!advance_time(now_ms)) {
        result.reason = ShotEvidenceReason::time_regression;
        return result;
    }
    expire(now_ms);
    if (!active_) result.reason = ShotEvidenceReason::canceled;
    else if (query.epoch != epoch_) result.reason = ShotEvidenceReason::stale_epoch;
    else if (query.context != context_) result.reason = ShotEvidenceReason::context_changed;
    else if (pressed_at_ms > now_ms || pressed_at_ms < context_started_at_ms_ ||
             (last_intent_press_ms_ && pressed_at_ms < *last_intent_press_ms_))
        result.reason = ShotEvidenceReason::stale_frame;
    else if (last_intent_press_ms_ && pressed_at_ms == *last_intent_press_ms_)
        result.reason = ShotEvidenceReason::duplicate_intent;
    else {
        last_intent_press_ms_ = pressed_at_ms;
        if (correlation_invalidated_) result.reason = ShotEvidenceReason::correlation_ambiguous;
        else if (!hud_seen_) result.reason = ShotEvidenceReason::no_hud_evidence;
        else if (!stable_ || stable_first_frame_id_ == 0)
            result.reason = ShotEvidenceReason::awaiting_stable_hud;
        else if (stable_->timestamp_ms > pressed_at_ms ||
                 pressed_at_ms - stable_->timestamp_ms > policy_.maximum_hud_age_ms)
            result.reason = ShotEvidenceReason::stale_frame;
        else if (stable_->stabilized == false)
            result.reason = ShotEvidenceReason::stabilization_not_ready;
        else if (!finite(query.target) || !std::isfinite(query.target_height_delta_m))
            result.reason = ShotEvidenceReason::invalid_frame;
        else if (stable_->firing_snapshot.target != query.target ||
                 !height_equal(stable_->firing_snapshot.target_height_delta_m, query.target_height_delta_m))
            result.reason = ShotEvidenceReason::target_changed;
        else if (intents_.size() >= policy_.maximum_outstanding_shots ||
                 next_intent_id_ == std::numeric_limits<std::uint64_t>::max())
            result.reason = ShotEvidenceReason::capacity_exhausted;
        else if (discharge_) result.reason = ShotEvidenceReason::correlation_ambiguous;
        else {
            ObservedFireIntentSnapshot intent;
            intent.intent_id = next_intent_id_++;
            intent.epoch = epoch_;
            intent.context = context_;
            intent.firing = stable_->firing_snapshot;
            intent.command_id = stable_->command_id;
            intent.first_sight_frame_id = stable_first_frame_id_;
            intent.last_sight_frame_id = stable_->id;
            intent.last_sight_capture_ms = stable_->timestamp_ms;
            intent.pressed_at_ms = pressed_at_ms;
            intent.bearing_uncertainty_deg = stable_->bearing_uncertainty_deg;
            intent.mil_uncertainty = stable_->mil_uncertainty;
            intents_.push_back({intent, *stable_});
            result.intent = intent;
            if (intents_.size() > 1) result.reason = ShotEvidenceReason::multiple_fire_intents;
            return result;
        }
        // A real press without a usable pre-press snapshot cannot be silently
        // ignored while an older shot is reserved by asynchronous impact OCR.
        if (hud_seen_ || !pending_.empty()) {
            unassociated_discharge_at_ms_ = pressed_at_ms;
            unassociated_reason_ = ShotEvidenceReason::correlation_ambiguous;
        }
    }
    return result;
}

ShotEvidenceUpdate ShotObservationSession::fire_intent(std::uint64_t now_ms) {
    const ShotImpactQuery query{context_, epoch_, stable_ ? stable_->firing_snapshot.target : Point{},
        stable_ ? stable_->firing_snapshot.target_height_delta_m : 0.0};
    return fire_intent(query, now_ms, now_ms);
}

ShotImpactAssociation ShotObservationSession::reserve_impact(
    const ShotImpactQuery& query, std::uint64_t now_ms) {
    ShotImpactAssociation result{ShotEvidenceReason::no_observed_discharge, provenance()};
    if (!advance_time(now_ms)) {
        result.reason = ShotEvidenceReason::time_regression;
        return result;
    }
    expire(now_ms);
    if (!active_) result.reason = ShotEvidenceReason::canceled;
    else if (query.epoch != epoch_) result.reason = ShotEvidenceReason::stale_epoch;
    else if (query.context != context_) result.reason = ShotEvidenceReason::context_changed;
    else if (!finite(query.target) || !std::isfinite(query.target_height_delta_m)) result.reason = ShotEvidenceReason::invalid_frame;
    else if (correlation_invalidated_) result.reason = ShotEvidenceReason::correlation_ambiguous;
    else if (unassociated_discharge_at_ms_) result.reason = unassociated_reason_;
    else if (discharge_) result.reason = ShotEvidenceReason::awaiting_discharge_confirmation;
    else if (!intents_.empty()) result.reason = intents_.size() > 1 ?
        ShotEvidenceReason::multiple_fire_intents : ShotEvidenceReason::awaiting_discharge_confirmation;
    else if (pending_.size() > 1) result.reason = ShotEvidenceReason::multiple_outstanding_shots;
    else if (pending_.empty()) result.reason = expired_shot_ ? ShotEvidenceReason::expired :
        hud_seen_ ? ShotEvidenceReason::no_observed_discharge : ShotEvidenceReason::no_hud_evidence;
    else {
        auto& pending = pending_.front();
        if (pending.snapshot.correlation_ambiguous) result.reason = ShotEvidenceReason::correlation_ambiguous;
        else if (pending.snapshot.firing.target != query.target ||
                 !height_equal(pending.snapshot.firing.target_height_delta_m, query.target_height_delta_m))
            result.reason = ShotEvidenceReason::target_changed;
        else if (pending.reservation_id != 0) result.reason = ShotEvidenceReason::already_reserved;
        else if (next_reservation_id_ == std::numeric_limits<std::uint64_t>::max())
            result.reason = ShotEvidenceReason::capacity_exhausted;
        else {
            pending.reservation_id = next_reservation_id_++;
            result.reason = ShotEvidenceReason::unique_association;
            result.provenance = ShotEvidenceProvenance::hud_discharge_correlated;
            result.association_unique = true;
            result.reservation = ShotImpactReservation{pending.reservation_id, pending.snapshot};
        }
    }
    return result;
}

ShotEvidenceReason ShotObservationSession::commit(
    std::uint64_t reservation_id, std::uint64_t now_ms) {
    if (!advance_time(now_ms)) return ShotEvidenceReason::time_regression;
    expire(now_ms);
    if (!active_) return ShotEvidenceReason::canceled;
    if (reservation_id == 0) return ShotEvidenceReason::reservation_not_found;
    const auto found = std::find_if(pending_.begin(), pending_.end(), [&](const PendingShot& shot) {
        return shot.reservation_id == reservation_id;
    });
    if (found == pending_.end()) return expired_shot_ ? ShotEvidenceReason::expired : ShotEvidenceReason::reservation_not_found;
    const ShotImpactQuery query{found->snapshot.context, found->snapshot.epoch,
        found->snapshot.firing.target, found->snapshot.firing.target_height_delta_m};
    const auto validation = validate_reservation(reservation_id, query, now_ms);
    if (validation != ShotEvidenceReason::unique_association) return validation;
    pending_.erase(found);
    return ShotEvidenceReason::committed;
}

ShotEvidenceReason ShotObservationSession::validate_reservation(
    std::uint64_t reservation_id, const ShotImpactQuery& query, std::uint64_t now_ms) {
    if (!advance_time(now_ms)) return ShotEvidenceReason::time_regression;
    expire(now_ms);
    if (!active_) return ShotEvidenceReason::canceled;
    if (query.epoch != epoch_) return ShotEvidenceReason::stale_epoch;
    if (query.context != context_) return ShotEvidenceReason::context_changed;
    if (reservation_id == 0) return ShotEvidenceReason::reservation_not_found;
    const auto found = std::find_if(pending_.begin(), pending_.end(), [&](const PendingShot& shot) {
        return shot.reservation_id == reservation_id;
    });
    if (found == pending_.end()) return expired_shot_ ? ShotEvidenceReason::expired : ShotEvidenceReason::reservation_not_found;
    if (correlation_invalidated_) return ShotEvidenceReason::correlation_ambiguous;
    if (unassociated_discharge_at_ms_) return unassociated_reason_;
    if (discharge_) return ShotEvidenceReason::awaiting_discharge_confirmation;
    if (!intents_.empty()) return intents_.size() > 1 ? ShotEvidenceReason::multiple_fire_intents :
        ShotEvidenceReason::awaiting_discharge_confirmation;
    if (pending_.size() != 1) return ShotEvidenceReason::multiple_outstanding_shots;
    if (found->snapshot.correlation_ambiguous) return ShotEvidenceReason::correlation_ambiguous;
    if (found->snapshot.firing.target != query.target ||
        !height_equal(found->snapshot.firing.target_height_delta_m, query.target_height_delta_m))
        return ShotEvidenceReason::target_changed;
    return ShotEvidenceReason::unique_association;
}

ShotEvidenceReason ShotObservationSession::release(
    std::uint64_t reservation_id, std::uint64_t now_ms) {
    if (!advance_time(now_ms)) return ShotEvidenceReason::time_regression;
    expire(now_ms);
    if (!active_) return ShotEvidenceReason::canceled;
    if (reservation_id == 0) return ShotEvidenceReason::reservation_not_found;
    const auto found = std::find_if(pending_.begin(), pending_.end(), [&](const PendingShot& shot) {
        return shot.reservation_id == reservation_id;
    });
    if (found == pending_.end()) return expired_shot_ ? ShotEvidenceReason::expired : ShotEvidenceReason::reservation_not_found;
    found->reservation_id = 0;
    return ShotEvidenceReason::released;
}

std::size_t ShotObservationSession::outstanding_shots() const noexcept {
    return pending_.size();
}

std::uint64_t ShotObservationSession::epoch() const noexcept {
    return epoch_;
}

bool ShotObservationSession::has_stable_sight() const noexcept {
    return active_ && stable_.has_value() && stable_first_frame_id_ != 0 && !correlation_invalidated_;
}

}  // namespace wardogs
