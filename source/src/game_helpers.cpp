#include "wardogs/game_helpers.hpp"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <random>
#include <stdexcept>
#include <thread>
#include <utility>

namespace wardogs {
namespace {
constexpr int team_click_ms = 30;
constexpr auto focus_poll_interval = std::chrono::milliseconds{10};

int checked_random_addition(bool enabled, int minimum, int maximum, int addition) {
    if (!enabled) return 0;
    if (addition < minimum || addition > maximum)
        throw std::invalid_argument("Случайная задержка помощника вне заданного диапазона");
    return addition;
}
}  // namespace

GameHelperTiming game_helper_timing(GameHelperMode mode,
                                   const GameHelpersPreferences& preferences,
                                   int random_addition_ms) {
    validate_game_helpers_preferences(preferences);
    if (mode == GameHelperMode::build) {
        const auto& build = preferences.build;
        return {build.hold_ms, build.pause_ms + checked_random_addition(
            build.random_enabled, build.random_min_ms, build.random_max_ms,
            random_addition_ms)};
    }
    if (mode == GameHelperMode::team) {
        const auto& team = preferences.team;
        return {team_click_ms, team.interval_ms - team_click_ms + checked_random_addition(
            team.random_enabled, team.random_min_ms, team.random_max_ms,
            random_addition_ms)};
    }
    throw std::invalid_argument("Выберите помощника перед запуском");
}

bool helper_window_allows_input(std::uintptr_t expected_window) noexcept {
    const auto window = reinterpret_cast<HWND>(expected_window);
    if (!window || GetForegroundWindow() != window || !IsWindowVisible(window) || IsIconic(window))
        return false;
    // This is a borrowed handle. Never close it, switch desktops, or attach
    // the worker to another desktop in order to make synthetic input succeed.
    const HDESK desktop = GetThreadDesktop(GetCurrentThreadId());
    BOOL receives_input = FALSE;
    DWORD required{};
    if (!desktop || !GetUserObjectInformationW(desktop, UOI_IO, &receives_input,
                                               sizeof(receives_input), &required) ||
        receives_input == FALSE)
        return false;
    return GetForegroundWindow() == window;
}

bool send_helper_left_button(std::uintptr_t expected_window, bool pressed) noexcept {
    if (pressed && !helper_window_allows_input(expected_window)) return false;
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = pressed ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    return SendInput(1, &input, sizeof(input)) == 1;
}

struct GameHelperRunner::Impl {
    std::thread worker;
    std::atomic<bool> stopping{};
    std::atomic<bool> running{};
    std::atomic<bool> owns_button{};
    std::atomic<GameHelperMode> mode{GameHelperMode::none};
    std::atomic<GameHelperStopReason> reason{GameHelperStopReason::none};
    mutable std::mutex error_mutex;
    std::string error;
    std::mutex wait_mutex;
    std::condition_variable wake;
    GameHelperPlatform platform;

    void fail(GameHelperStopReason value, const char* message, bool replace = false) noexcept {
        reason.store(value, std::memory_order_release);
        // Keep the first failure unless releasing the button itself fails.
        try {
            std::lock_guard lock(error_mutex);
            if (replace || error.empty()) error = message;
        } catch (...) {
            // The enum still exposes the failure even if allocation fails.
        }
    }

    bool foreground_ok() {
        if (stopping.load(std::memory_order_acquire)) return false;
        if (!platform.foreground_matches()) {
            reason.store(GameHelperStopReason::focus_lost, std::memory_order_release);
            return false;
        }
        return !stopping.load(std::memory_order_acquire);
    }

    bool wait_until(std::chrono::steady_clock::time_point deadline) {
        for (;;) {
            if (!foreground_ok()) return false;
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return true;
            std::unique_lock lock(wait_mutex);
            if (wake.wait_until(lock, std::min(deadline, now + focus_poll_interval),
                                [this] { return stopping.load(std::memory_order_acquire); }))
                return false;
        }
    }

    bool release_button() noexcept {
        if (!owns_button.load(std::memory_order_acquire)) return true;
        // Releasing after focus loss is deliberate: never leave the global
        // button latched. Do not send any new DOWN after losing foreground.
        for (int attempt = 0; attempt < 3; ++attempt) {
            try {
                if (platform.send_left_button(false)) {
                    owns_button.store(false, std::memory_order_release);
                    return true;
                }
            } catch (...) {
                fail(GameHelperStopReason::callback_failed,
                     "Ошибка обработчика отпускания ЛКМ");
            }
        }
        fail(GameHelperStopReason::input_failed,
             "Windows не приняла отпускание ЛКМ. Остановите помощник и отпустите кнопку мыши", true);
        return false;
    }

    void run(GameHelperMode selected, GameHelpersPreferences preferences) noexcept {
        try {
            if (platform.activation_released) {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
                while (foreground_ok() && !platform.activation_released()) {
                    if (std::chrono::steady_clock::now() >= deadline) {
                        fail(GameHelperStopReason::activation_timeout,
                             "Отпустите горячую клавишу и запустите помощник заново");
                        break;
                    }
                    if (!wait_until(std::min(deadline,
                            std::chrono::steady_clock::now() + focus_poll_interval))) break;
                }
                if (!foreground_ok() ||
                    reason.load(std::memory_order_acquire) != GameHelperStopReason::none) {
                    if (reason.load(std::memory_order_acquire) == GameHelperStopReason::none)
                        reason.store(GameHelperStopReason::requested, std::memory_order_release);
                    mode.store(GameHelperMode::none, std::memory_order_release);
                    running.store(false, std::memory_order_release);
                    return;
                }
            }
            const bool random_enabled = selected == GameHelperMode::build
                ? preferences.build.random_enabled : preferences.team.random_enabled;
            const int random_minimum = selected == GameHelperMode::build
                ? preferences.build.random_min_ms : preferences.team.random_min_ms;
            const int random_maximum = selected == GameHelperMode::build
                ? preferences.build.random_max_ms : preferences.team.random_max_ms;
            // No randomness is used for concealment; it only varies the user's
            // explicitly configured released-button timing.
            std::mt19937 random;
            if (random_enabled) random.seed(std::random_device{}());
            std::uniform_int_distribution<int> distribution(random_minimum, random_maximum);
            while (foreground_ok()) {
                const auto timing = game_helper_timing(selected, preferences,
                    random_enabled ? distribution(random) : 0);
                if (!foreground_ok()) break;
                const auto down_time = std::chrono::steady_clock::now();
                // A throwing callback may have sent DOWN before throwing.
                // Explicit false, however, means no input was accepted.
                owns_button.store(true, std::memory_order_release);
                if (!platform.send_left_button(true)) {
                    owns_button.store(false, std::memory_order_release);
                    fail(GameHelperStopReason::input_failed,
                         "Windows не приняла нажатие ЛКМ");
                    break;
                }
                if (!wait_until(down_time + std::chrono::milliseconds{timing.hold_ms})) break;
                if (!release_button()) break;
                const auto pause_end = selected == GameHelperMode::team
                    ? down_time + std::chrono::milliseconds{timing.hold_ms + timing.pause_ms}
                    : std::chrono::steady_clock::now() + std::chrono::milliseconds{timing.pause_ms};
                if (!wait_until(pause_end)) break;
            }
        } catch (...) {
            fail(GameHelperStopReason::callback_failed,
                 "Ошибка помощника. Цикл остановлен");
        }
        release_button();
        if (reason.load(std::memory_order_acquire) == GameHelperStopReason::none)
            reason.store(GameHelperStopReason::requested, std::memory_order_release);
        mode.store(GameHelperMode::none, std::memory_order_release);
        running.store(false, std::memory_order_release);
    }
};

GameHelperRunner::GameHelperRunner() : impl_(std::make_unique<Impl>()) {}
GameHelperRunner::~GameHelperRunner() { stop(); }

void GameHelperRunner::start(GameHelperMode selected,
                             const GameHelpersPreferences& preferences,
                             GameHelperPlatform platform) {
    stop();
    if (impl_->owns_button.load(std::memory_order_acquire))
        throw std::runtime_error("Не удалось отпустить ЛКМ предыдущего цикла");
    validate_game_helpers_preferences(preferences);
    if (selected != GameHelperMode::build && selected != GameHelperMode::team)
        throw std::invalid_argument("Выберите помощника перед запуском");
    if ((selected == GameHelperMode::build && !preferences.build.enabled) ||
        (selected == GameHelperMode::team && !preferences.team.enabled))
        throw std::invalid_argument("Выбранный помощник отключён в настройках");
    if (!platform.foreground_matches || !platform.send_left_button)
        throw std::invalid_argument("Не настроена проверка окна или отправка ЛКМ");
    impl_->platform = std::move(platform);
    {
        std::lock_guard lock(impl_->error_mutex);
        impl_->error.clear();
    }
    impl_->reason.store(GameHelperStopReason::none, std::memory_order_release);
    impl_->stopping.store(false, std::memory_order_release);
    impl_->mode.store(selected, std::memory_order_release);
    impl_->running.store(true, std::memory_order_release);
    try {
        impl_->worker = std::thread([state = impl_.get(), selected, preferences] {
            state->run(selected, preferences);
        });
    } catch (...) {
        impl_->running.store(false, std::memory_order_release);
        impl_->mode.store(GameHelperMode::none, std::memory_order_release);
        throw;
    }
}

void GameHelperRunner::stop() noexcept {
    impl_->stopping.store(true, std::memory_order_release);
    impl_->wake.notify_all();
    if (impl_->worker.joinable()) impl_->worker.join();
    impl_->release_button();
}

bool GameHelperRunner::running() const noexcept {
    return impl_->running.load(std::memory_order_acquire);
}

GameHelperMode GameHelperRunner::mode() const noexcept {
    return impl_->mode.load(std::memory_order_acquire);
}

GameHelperStopReason GameHelperRunner::stop_reason() const noexcept {
    return impl_->reason.load(std::memory_order_acquire);
}

std::string GameHelperRunner::last_error() const {
    std::lock_guard lock(impl_->error_mutex);
    return impl_->error;
}

}  // namespace wardogs
