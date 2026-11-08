#pragma once

#include <chrono>
#include <optional>

namespace wardogs {

// Persistent arming is separate from the four-snapshot input worker. Only a
// fresh unmodified R press/release in the eligible game can request a cycle.
class ReloadTrigger {
public:
    using Clock = std::chrono::steady_clock;

    void arm(bool reload_key_down) noexcept {
        armed_ = true;
        was_down_ = reload_key_down;
        clear_pending();
    }
    void disarm() noexcept {
        armed_ = false;
        clear_pending();
    }
    [[nodiscard]] bool armed() const noexcept { return armed_; }

    // No screenshots or input are taken here. Busy cycles and ineligible
    // foregrounds discard R intentions instead of queuing an automatic retry.
    [[nodiscard]] bool update(bool eligible, bool reload_key_down, bool busy,
                              Clock::time_point now) noexcept {
        const bool rising = reload_key_down && !was_down_;
        was_down_ = reload_key_down;
        if (!armed_ || !eligible || busy) {
            clear_pending();
            return false;
        }
        if (rising) {
            pending_ = true;
            released_at_.reset();
        }
        if (!pending_ || reload_key_down) return false;
        if (!released_at_) released_at_ = now;
        // Give the game time to display the first white row after R. The
        // recognizer still uses exactly one frame for each four-key batch.
        if (now - *released_at_ < std::chrono::milliseconds{500}) return false;
        clear_pending();
        return true;
    }

private:
    void clear_pending() noexcept { pending_ = false; released_at_.reset(); }
    bool armed_{};
    bool was_down_{};
    bool pending_{};
    std::optional<Clock::time_point> released_at_;
};

}  // namespace wardogs
