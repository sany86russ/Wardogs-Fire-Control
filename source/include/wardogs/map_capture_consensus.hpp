#pragma once

#include "wardogs/core.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace wardogs {

enum class MapCaptureDecision { retry, accept, review, failed };

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
