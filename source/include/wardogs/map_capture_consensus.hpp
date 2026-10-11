#pragma once

#include "wardogs/core.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <vector>

namespace wardogs {

enum class MapCaptureDecision { retry, accept, review, failed };

struct MapCaptureAnchor {
    // X and Y ink bounds in the guarded neighborhood screenshot.
    std::array<int, 8> bounds{};
    double scale{};
    bool operator==(const MapCaptureAnchor&) const = default;
};

class MapCaptureFrameEvidence {
public:
    void observe(std::optional<MapCaptureAnchor> anchor, bool trustworthy) {
        if (!trustworthy) return;
        ++verified_frames_;
        if (!anchor || !std::isfinite(anchor->scale) || anchor->scale <= 0.0 ||
            anchor->bounds[2] <= anchor->bounds[0] || anchor->bounds[3] <= anchor->bounds[1] ||
            anchor->bounds[6] <= anchor->bounds[4] || anchor->bounds[7] <= anchor->bounds[5]) {
            anchors_agree_ = false;
            return;
        }
        if (!anchor_) anchor_ = anchor;
        else if (*anchor_ != *anchor) anchors_agree_ = false;
    }

    [[nodiscard]] unsigned verified_frames() const noexcept { return verified_frames_; }
    [[nodiscard]] bool anchors_agree() const noexcept { return anchor_.has_value() && anchors_agree_; }
    [[nodiscard]] bool frozen_pair_verified() const noexcept {
        return verified_frames_ >= 2 && anchors_agree();
    }

private:
    unsigned verified_frames_{};
    bool anchors_agree_{true};
    std::optional<MapCaptureAnchor> anchor_;
};

// Moving the pointer after both guarded screenshots were frozen cannot change
// their pixels. It still prevents another capture. Focus/client changes always
// invalidate the request; a single readable frame remains explicit review.
inline MapCaptureDecision complete_map_capture(MapCaptureDecision decision,
                                               bool window_client_unchanged,
                                               bool cursor_unchanged,
                                               bool guarded_candidate_available,
                                               bool frozen_pair_verified) noexcept {
    if (!window_client_unchanged) return MapCaptureDecision::failed;
    if (cursor_unchanged) return decision;
    if (decision == MapCaptureDecision::accept && frozen_pair_verified)
        return MapCaptureDecision::accept;
    return guarded_candidate_available ? MapCaptureDecision::review : MapCaptureDecision::failed;
}

// Each observation must come from a fresh screenshot of the same stationary
// cursor and unchanged game client. Reprocessing one frame is not a vote.
class MapCaptureConsensus {
public:
    static constexpr unsigned maximum_frames = 4;
    static constexpr unsigned required_agreement = 2;

    MapCaptureDecision observe(std::optional<Point> point, bool trustworthy) {
        if (frames_ >= maximum_frames) return terminal_decision();
        ++frames_;
        if (point && (!std::isfinite(point->x) || !std::isfinite(point->y))) point.reset();
        if (point && std::find(candidates_.begin(), candidates_.end(), *point) == candidates_.end())
            candidates_.push_back(*point);
        if (point && trustworthy) {
            if (!trusted_) { trusted_ = point; agreeing_frames_ = 1; }
            else if (*trusted_ == *point) ++agreeing_frames_;
            else conflict_ = true;
        }
        if (!conflict_ && agreeing_frames_ >= required_agreement)
            return MapCaptureDecision::accept;
        return frames_ < maximum_frames ? MapCaptureDecision::retry : terminal_decision();
    }

    [[nodiscard]] unsigned frames() const noexcept { return frames_; }
    [[nodiscard]] unsigned agreeing_frames() const noexcept { return agreeing_frames_; }
    [[nodiscard]] bool conflict() const noexcept { return conflict_; }
    [[nodiscard]] const std::vector<Point>& candidates() const noexcept { return candidates_; }

private:
    MapCaptureDecision terminal_decision() const noexcept {
        return candidates_.empty() ? MapCaptureDecision::failed : MapCaptureDecision::review;
    }
    unsigned frames_{};
    unsigned agreeing_frames_{};
    bool conflict_{};
    std::optional<Point> trusted_;
    std::vector<Point> candidates_;
};

} // namespace wardogs
