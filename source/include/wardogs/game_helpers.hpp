#pragma once

#include "wardogs/game_helpers_preferences.hpp"

#include <functional>
#include <memory>
#include <string>

namespace wardogs {

enum class GameHelperMode { none, build, team };
enum class GameHelperStopReason {
    none, requested, focus_lost, input_failed, callback_failed, activation_timeout
};

struct GameHelperPlatform {
    // Called on the worker thread. Match the exact foreground window captured
    // at START, not merely any game window. Callbacks must return promptly and
    // must not call the runner's lifecycle methods.
    std::function<bool()> foreground_matches;
    std::function<bool(bool pressed)> send_left_button;
    // Optional initial trigger/modifier release check. A held shortcut never
    // becomes Ctrl+click/Alt+click; timeout after 3 seconds fails closed.
    std::function<bool()> activation_released;
};

struct GameHelperTiming {
    int hold_ms{};
    int pause_ms{};
};

// The random addition is inclusive and applies only to the released phase.
// Team Piker's interval is measured down-to-down, including its 30 ms click.
[[nodiscard]] GameHelperTiming game_helper_timing(
    GameHelperMode mode, const GameHelpersPreferences& preferences,
    int random_addition_ms);

// Ordinary, visible Win32 input. No game-process access, hooks, or bypasses.
// Returns false when Windows accepts no input, including UIPI restrictions.
[[nodiscard]] bool send_helper_left_button(bool pressed) noexcept;

class GameHelperRunner {
public:
    GameHelperRunner();
    ~GameHelperRunner();
    GameHelperRunner(const GameHelperRunner&) = delete;
    GameHelperRunner& operator=(const GameHelperRunner&) = delete;

    // Lifecycle calls belong to the owning thread. A new start first stops and
    // releases the previous run; no two helper modes run simultaneously.
    // Settings are copied, and an unfinished button release blocks a new run.
    void start(GameHelperMode mode, const GameHelpersPreferences& preferences,
               GameHelperPlatform platform);
    // Interrupts either phase, joins the worker, and retries any owned release.
    // Its normal wakeup is immediate; foreground is checked at least every
    // 10 ms while running. Neither stop nor focus restoration auto-starts.
    void stop() noexcept;
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] GameHelperMode mode() const noexcept;
    [[nodiscard]] GameHelperStopReason stop_reason() const noexcept;
    [[nodiscard]] std::string last_error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace wardogs
