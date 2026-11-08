#pragma once

#include "wardogs/game_helpers_preferences.hpp"
#include "wardogs/reload_vision.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace wardogs {

enum class ReloadHelperState { stopped, searching, verifying, completed, error };

struct ReloadHelperPlatform {
    // All callbacks run on the worker and must return promptly. The observer
    // takes a NEW screenshot on every call; returning a cached frame is unsafe.
    // Callbacks must not invoke this runner's start/stop methods.
    std::function<bool()> foreground_matches;
    std::function<bool()> activation_released;
    std::function<std::optional<ReloadObservation>()> observe;
    // false means no event was delivered. A throwing DOWN may have been
    // delivered, so the runner still owes a matching UP.
    std::function<bool(ReloadDirection, bool pressed)> send_key;
    // Optional inclusive sampler for isolated tests or custom platforms.
    // Production uses an independently sampled uniform addition for each wait.
    std::function<int(int minimum, int maximum)> random_between;
};

// Runtime cycle timings. Defaults preserve the fixed four-stage cadence;
// short timings remain available for isolated tests.
struct ReloadHelperTiming {
    int hold_ms{50};
    int key_gap_ms{90};
    int stage_gap_ms{750};
    int activation_timeout_ms{3000};
    int overall_timeout_ms{15000};
    bool key_random_enabled{false};
    int key_random_min_ms{0};
    int key_random_max_ms{30};
    bool stage_random_enabled{false};
    int stage_random_min_ms{0};
    int stage_random_max_ms{200};
};

// Copies validated preferences and budgets for their worst-case cycle length.
// Key additions apply independently to hold and key gap; stage additions apply
// only to the pause between stages. Disabled randomness never samples a value.
[[nodiscard]] ReloadHelperTiming reload_helper_timing(const ReloadHelperPreferences& preferences);

// Arrow keys only, through ordinary Windows SendInput. Each DOWN verifies the
// exact foreground HWND and active input desktop, and refuses held arrows or
// modifiers. Owed UP remains possible after focus loss. No alternate input
// method, privilege escalation, game-process access, or concealment is used.
[[nodiscard]] bool send_reload_key(std::uintptr_t expected_window,
                                   ReloadDirection direction, bool pressed) noexcept;

class ReloadHelperRunner {
public:
    ReloadHelperRunner();
    ~ReloadHelperRunner();
    ReloadHelperRunner(const ReloadHelperRunner&) = delete;
    ReloadHelperRunner& operator=(const ReloadHelperRunner&) = delete;

    // Lifecycle calls belong to the owning thread. Construction and stop never
    // arm a run. A failed release blocks restart until that release succeeds.
    void start(ReloadHelperPlatform platform, ReloadHelperTiming timing = {});
    void stop() noexcept;
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] ReloadHelperState state() const noexcept;
    [[nodiscard]] std::string last_error() const;
    // Number of THIS RUN's successfully completed DOWN/UP pairs. This is
    // delivery progress, not proof that the game accepted any input.
    [[nodiscard]] int sent_count() const noexcept;
    // Cross-helper coordinator must refuse mouse/keyboard runs while any
    // previous synthetic key still needs its UP, even if running() is false.
    [[nodiscard]] bool release_pending() const noexcept;

    // completed means exactly four batches of four DOWN/UP pairs were sent.
    // Each batch uses one fresh all-white four-arrow snapshot, with configured
    // spacing between keys and stages. No animation or acceptance is observed
    // between keys; completion does not establish that the vehicle is reloaded.
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace wardogs
