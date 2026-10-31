#include "wardogs/game_helpers.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;
using wardogs::GameHelperMode;
using wardogs::GameHelperRunner;
using wardogs::GameHelperStopReason;
using wardogs::GameHelpersPreferences;

bool wait_for(const std::function<bool()>& predicate,
              std::chrono::milliseconds timeout = 2000ms) {
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= end) return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

struct Event {
    bool pressed;
    std::chrono::steady_clock::time_point time;
};

struct Fixture {
    std::atomic<bool> foreground{true};
    std::atomic<bool> activation_released{true};
    std::atomic<bool> down_accepted{true};
    std::atomic<bool> up_accepted{true};
    std::atomic<bool> throw_on_down{};
    std::mutex mutex;
    std::vector<Event> events;

    wardogs::GameHelperPlatform platform() {
        return {
            [this] { return foreground.load(); },
            [this](bool pressed) {
                {
                    std::lock_guard lock(mutex);
                    events.push_back({pressed, std::chrono::steady_clock::now()});
                }
                if (pressed && throw_on_down.load()) throw std::runtime_error("test failure");
                return pressed ? down_accepted.load() : up_accepted.load();
            },
            [this] { return activation_released.load(); }
        };
    }

    std::vector<Event> snapshot() {
        std::lock_guard lock(mutex);
        return events;
    }

    std::size_t downs() {
        const auto values = snapshot();
        std::size_t result = 0;
        for (const auto& event : values) if (event.pressed) ++result;
        return result;
    }
};

GameHelpersPreferences fast_preferences() {
    GameHelpersPreferences preferences;
    preferences.build.enabled = true;
    preferences.build.hold_ms = 50;
    preferences.build.pause_ms = 20;
    preferences.team.enabled = true;
    preferences.team.interval_ms = 100;
    preferences.team.random_enabled = false;
    return preferences;
}
}  // namespace

int main() {
    int failures = 0;
    int checks = 0;
    const auto check = [&](bool condition, const char* message) {
        ++checks;
        if (!condition) {
            ++failures;
            std::cerr << "FAIL: " << message << '\n';
        }
    };
    const auto rejects = [](const auto& operation) {
        try { operation(); }
        catch (const std::exception&) { return true; }
        return false;
    };

    GameHelpersPreferences defaults;
    const auto build = wardogs::game_helper_timing(GameHelperMode::build, defaults, 20);
    check(build.hold_ms == 660 && build.pause_ms == 100,
          "the requested Build helper preset holds 660ms and releases 100ms without jitter");
    defaults.build.random_enabled = true;
    check(wardogs::game_helper_timing(GameHelperMode::build, defaults, 0).pause_ms == 100 &&
          wardogs::game_helper_timing(GameHelperMode::build, defaults, 30).pause_ms == 130,
          "build jitter includes both configured endpoints and never shortens the hold");
    check(rejects([&] { (void)wardogs::game_helper_timing(GameHelperMode::build, defaults, 31); }),
          "out-of-range jitter is rejected rather than silently altered");
    const auto team_low = wardogs::game_helper_timing(GameHelperMode::team, defaults, 100);
    const auto team_high = wardogs::game_helper_timing(GameHelperMode::team, defaults, 200);
    check(team_low.hold_ms == 30 && team_low.hold_ms + team_low.pause_ms == 900 &&
          team_high.hold_ms + team_high.pause_ms == 1000,
          "Team Piker defaults produce a total click-to-click interval of 900-1000ms");
    check(rejects([&] { (void)wardogs::game_helper_timing(GameHelperMode::none, defaults, 100); }),
          "no-helper mode cannot acquire a mouse timing");

    {
        Fixture fixture;
        GameHelperRunner runner;
        check(!runner.running() && runner.mode() == GameHelperMode::none,
              "construction never starts a remembered cycle");
        check(rejects([&] { runner.start(GameHelperMode::build, defaults, fixture.platform()); }) &&
              fixture.snapshot().empty(), "disabled helper cannot send input");
        check(rejects([&] { runner.start(GameHelperMode::build, fast_preferences(), {}); }),
              "missing foreground/input callbacks fail before starting");
        auto invalid = fast_preferences();
        invalid.build.hold_ms = 0;
        check(rejects([&] { runner.start(GameHelperMode::build, invalid, fixture.platform()); }),
              "invalid preferences fail before the worker starts");
    }
    {
        Fixture fixture;
        fixture.foreground = false;
        GameHelperRunner runner;
        runner.start(GameHelperMode::build, fast_preferences(), fixture.platform());
        check(wait_for([&] { return !runner.running(); }) && fixture.snapshot().empty() &&
              runner.stop_reason() == GameHelperStopReason::focus_lost,
              "an unsuitable foreground never receives a press");
    }
    {
        Fixture fixture;
        GameHelperRunner runner;
        auto preferences = fast_preferences();
        preferences.build.hold_ms = 5000;
        runner.start(GameHelperMode::build, preferences, fixture.platform());
        check(wait_for([&] { return fixture.downs() == 1; }), "long hold starts");
        const auto before_stop = std::chrono::steady_clock::now();
        runner.stop();
        const auto values = fixture.snapshot();
        check(values.size() == 2 && values.front().pressed && !values.back().pressed &&
              !runner.running() && runner.mode() == GameHelperMode::none,
              "STOP immediately releases an owned long press and joins its worker");
        check(std::chrono::steady_clock::now() - before_stop < 250ms,
              "STOP interrupts a five-second hold rather than waiting for its timer");
        runner.stop();
        check(fixture.snapshot().size() == values.size(),
              "repeated STOP does not send unrelated mouse releases");
    }
    {
        Fixture fixture;
        GameHelperRunner runner;
        auto preferences = fast_preferences();
        preferences.build.hold_ms = 5000;
        runner.start(GameHelperMode::build, preferences, fixture.platform());
        check(wait_for([&] { return fixture.downs() == 1; }), "focus-loss hold starts");
        fixture.foreground = false;
        check(wait_for([&] { return !runner.running(); }) &&
              runner.stop_reason() == GameHelperStopReason::focus_lost,
              "focus loss cancels without UI-thread participation");
        const auto values = fixture.snapshot();
        check(values.size() == 2 && !values.back().pressed,
              "focus loss releases the button even though the game is no longer foreground");
        fixture.foreground = true;
        std::this_thread::sleep_for(40ms);
        check(fixture.snapshot().size() == values.size(),
              "restoring focus cannot restart a cancelled cycle");
    }
    {
        Fixture fixture;
        GameHelperRunner runner;
        auto preferences = fast_preferences();
        preferences.build.pause_ms = 5000;
        runner.start(GameHelperMode::build, preferences, fixture.platform());
        check(wait_for([&] { return fixture.snapshot().size() >= 2; }), "released phase starts");
        fixture.foreground = false;
        check(wait_for([&] { return !runner.running(); }) && fixture.downs() == 1,
              "foreground checks also cancel the released phase before another DOWN");
    }
    {
        Fixture fixture;
        GameHelperRunner runner;
        auto preferences = fast_preferences();
        preferences.build.hold_ms = 5000;
        runner.start(GameHelperMode::build, preferences, fixture.platform());
        check(wait_for([&] { return fixture.downs() == 1; }), "first helper starts");
        runner.start(GameHelperMode::team, preferences, fixture.platform());
        check(wait_for([&] { return fixture.downs() >= 2; }), "replacement helper starts");
        runner.stop();
        const auto values = fixture.snapshot();
        check(values.size() >= 4 && values[0].pressed && !values[1].pressed && values[2].pressed &&
              !values.back().pressed,
              "switching helper modes releases the old press before any new one");
    }
    {
        Fixture fixture;
        fixture.activation_released = false;
        GameHelperRunner runner;
        runner.start(GameHelperMode::build, fast_preferences(), fixture.platform());
        std::this_thread::sleep_for(40ms);
        check(fixture.snapshot().empty(), "a held activation shortcut never produces modifier+click");
        fixture.activation_released = true;
        check(wait_for([&] { return fixture.downs() == 1; }),
              "releasing the trigger permits the first click");
        runner.stop();
    }
    {
        Fixture fixture;
        fixture.activation_released = false;
        GameHelperRunner runner;
        runner.start(GameHelperMode::build, fast_preferences(), fixture.platform());
        check(wait_for([&] { return !runner.running(); }, 4000ms) &&
              fixture.snapshot().empty() &&
              runner.stop_reason() == GameHelperStopReason::activation_timeout &&
              !runner.last_error().empty(),
              "an indefinitely held activation key times out with a visible failure and no input");
    }
    {
        Fixture fixture;
        fixture.down_accepted = false;
        GameHelperRunner runner;
        runner.start(GameHelperMode::build, fast_preferences(), fixture.platform());
        check(wait_for([&] { return !runner.running(); }) && fixture.downs() == 1 &&
              runner.stop_reason() == GameHelperStopReason::input_failed &&
              !runner.last_error().empty(), "rejected DOWN stops and exposes the failure");
        const auto values = fixture.snapshot();
        check(values.size() == 2 && !values.back().pressed,
              "a rejected or uncertain DOWN still receives a conservative release");
    }
    {
        Fixture fixture;
        fixture.up_accepted = false;
        GameHelperRunner runner;
        runner.start(GameHelperMode::build, fast_preferences(), fixture.platform());
        check(wait_for([&] { return !runner.running(); }) &&
              runner.stop_reason() == GameHelperStopReason::input_failed &&
              fixture.downs() == 1 && !runner.last_error().empty(),
              "repeated release rejection stops instead of sending additional presses");
        check(rejects([&] { runner.start(GameHelperMode::team, fast_preferences(), fixture.platform()); }) &&
              fixture.downs() == 1,
              "unreleased ownership blocks a replacement run");
        fixture.up_accepted = true;
        runner.stop();
        runner.start(GameHelperMode::team, fast_preferences(), fixture.platform());
        check(wait_for([&] { return fixture.downs() >= 2; }),
              "an explicitly recovered release permits a fresh run");
        runner.stop();
    }
    {
        Fixture fixture;
        fixture.throw_on_down = true;
        GameHelperRunner runner;
        runner.start(GameHelperMode::build, fast_preferences(), fixture.platform());
        check(wait_for([&] { return !runner.running(); }) &&
              runner.stop_reason() == GameHelperStopReason::callback_failed &&
              !runner.last_error().empty(), "platform exceptions stop with an observable failure");
        const auto values = fixture.snapshot();
        check(values.size() == 2 && !values.back().pressed,
              "exception after DOWN cannot skip the final release");
    }
    {
        Fixture fixture;
        {
            GameHelperRunner runner;
            auto preferences = fast_preferences();
            preferences.build.hold_ms = 5000;
            runner.start(GameHelperMode::build, preferences, fixture.platform());
            check(wait_for([&] { return fixture.downs() == 1; }), "destructor test hold starts");
        }
        const auto values = fixture.snapshot();
        check(values.size() == 2 && !values.back().pressed,
              "destruction joins and releases without requiring a separate STOP");
    }
    {
        Fixture fixture;
        GameHelperRunner runner;
        runner.start(GameHelperMode::team, fast_preferences(), fixture.platform());
        check(wait_for([&] { return fixture.downs() >= 3; }), "Team Piker repeats clicks");
        runner.stop();
        const auto values = fixture.snapshot();
        std::vector<Event> downs;
        for (const auto& event : values) if (event.pressed) downs.push_back(event);
        bool intervals = downs.size() >= 3;
        for (std::size_t index = 1; index < downs.size(); ++index)
            intervals = intervals && downs[index].time - downs[index - 1].time >= 95ms;
        check(intervals, "Team Piker's released phase preserves its total configured interval");
    }
    std::cout << checks - failures << '/' << checks << " game-helper checks passed\n";
    return failures == 0 ? 0 : 1;
}
