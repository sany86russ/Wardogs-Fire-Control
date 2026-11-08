#include "wardogs/reload_helper.hpp"
#include "wardogs/game_helpers.hpp"
#include "wardogs/logger.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace wardogs {
namespace {
using Clock = std::chrono::steady_clock;
using Milliseconds = std::chrono::milliseconds;
constexpr Milliseconds focus_interval{10};
constexpr int stage_count = 4;

WORD virtual_key(ReloadDirection direction) noexcept {
    switch (direction) {
    case ReloadDirection::up: return VK_UP;
    case ReloadDirection::right: return VK_RIGHT;
    case ReloadDirection::down: return VK_DOWN;
    case ReloadDirection::left: return VK_LEFT;
    }
    return 0;
}

void validate_timing(const ReloadHelperTiming& timing) {
    const std::array values{timing.hold_ms, timing.key_gap_ms, timing.stage_gap_ms,
        timing.activation_timeout_ms};
    const auto valid_random = [](int minimum, int maximum) {
        return minimum >= 0 && minimum <= maximum &&
            maximum <= game_helpers_limits::maximum_random_ms;
    };
    if (std::any_of(values.begin(), values.end(), [](int value) {
            return value < 1 || value > 120000;
        }) || timing.hold_ms > 1000 || timing.overall_timeout_ms < 1 ||
        timing.overall_timeout_ms > 300000 ||
        !valid_random(timing.key_random_min_ms, timing.key_random_max_ms) ||
        !valid_random(timing.stage_random_min_ms, timing.stage_random_max_ms))
        throw std::invalid_argument("Некорректные интервалы помощника перезарядки");
}

bool valid_snapshot(const ReloadObservation& observation) noexcept {
    if (!std::isfinite(observation.confidence) || observation.confidence < 0.7 ||
        observation.confidence > 1.0 || observation.bounds.left < 0 ||
        observation.bounds.top < 0 || observation.bounds.right <= observation.bounds.left ||
        observation.bounds.bottom <= observation.bounds.top ||
        observation.bounds.right > 32768 || observation.bounds.bottom > 32768)
        return false;
    return std::all_of(observation.arrows.begin(), observation.arrows.end(),
        [](const ReloadArrow& arrow) { return virtual_key(arrow.direction) != 0; });
}

char direction_name(ReloadDirection direction) noexcept {
    switch (direction) {
    case ReloadDirection::up: return 'U';
    case ReloadDirection::right: return 'R';
    case ReloadDirection::down: return 'D';
    case ReloadDirection::left: return 'L';
    }
    return '?';
}

void log_input(const char* action, ReloadDirection direction, int stage, int key) noexcept {
    try {
        std::ostringstream output;
        output << "reload.input action=" << action << " direction=" << direction_name(direction)
            << " stage=" << stage + 1 << " key=" << key + 1;
        log_info(output.str());
    } catch (...) {
        // Diagnostic formatting cannot alter input ownership or delivery.
    }
}

void log_sent(int count, int stage, int key) noexcept {
    try {
        std::ostringstream output;
        output << "reload.sent count=" << count << " stage=" << stage + 1 << " key=" << key + 1;
        log_info(output.str());
    } catch (...) {
        // A failed diagnostic cannot change the number of delivered pairs.
    }
}
}  // namespace

ReloadHelperTiming reload_helper_timing(const ReloadHelperPreferences& preferences) {
    validate_reload_helper_timing_preferences(preferences);
    ReloadHelperTiming timing;
    timing.hold_ms = preferences.hold_ms;
    timing.key_gap_ms = preferences.key_gap_ms;
    timing.stage_gap_ms = preferences.stage_gap_ms;
    timing.key_random_enabled = preferences.key_random_enabled;
    timing.key_random_min_ms = preferences.key_random_min_ms;
    timing.key_random_max_ms = preferences.key_random_max_ms;
    timing.stage_random_enabled = preferences.stage_random_enabled;
    timing.stage_random_min_ms = preferences.stage_random_min_ms;
    timing.stage_random_max_ms = preferences.stage_random_max_ms;
    const int key_addition = timing.key_random_enabled ? timing.key_random_max_ms : 0;
    const int stage_addition = timing.stage_random_enabled ? timing.stage_random_max_ms : 0;
    const int worst_case = 16 * (timing.hold_ms + key_addition) +
        12 * (timing.key_gap_ms + key_addition) + 3 * (timing.stage_gap_ms + stage_addition);
    timing.overall_timeout_ms = std::max(timing.overall_timeout_ms, worst_case + 8000);
    validate_timing(timing);
    return timing;
}

bool send_reload_key(std::uintptr_t expected_window, ReloadDirection direction,
                     bool pressed) noexcept {
    const WORD key = virtual_key(direction);
    if (!key) return false;
    if (pressed) {
        if (!helper_window_allows_input(expected_window)) return false;
        for (const int held_key : {VK_SHIFT, VK_CONTROL, VK_MENU, VK_LWIN, VK_RWIN,
                                  VK_UP, VK_RIGHT, VK_DOWN, VK_LEFT})
            if ((GetAsyncKeyState(held_key) & 0x8000) != 0) return false;
        if (!helper_window_allows_input(expected_window)) return false;
    }
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = key;
    input.ki.dwFlags = KEYEVENTF_EXTENDEDKEY | (pressed ? 0 : KEYEVENTF_KEYUP);
    return SendInput(1, &input, sizeof(input)) == 1;
}

struct ReloadHelperRunner::Impl {
    std::thread worker;
    std::atomic<bool> stopping{};
    std::atomic<bool> running{};
    std::atomic<bool> owns_key{};
    ReloadDirection owned_direction{ReloadDirection::up};
    int owned_stage{};
    int owned_index{};
    std::atomic<ReloadHelperState> state{ReloadHelperState::stopped};
    std::atomic<int> sent{};
    mutable std::mutex error_mutex;
    std::string error;
    std::mutex wait_mutex;
    std::condition_variable wake;
    ReloadHelperPlatform platform;
    ReloadHelperTiming timing;

    void fail(const char* message, bool replace = false) noexcept {
        state.store(ReloadHelperState::error, std::memory_order_release);
        try {
            std::lock_guard lock(error_mutex);
            if (replace || error.empty()) error = message;
        } catch (...) {
            // The error state remains observable even on allocation failure.
        }
    }

    bool foreground_ok() {
        if (stopping.load(std::memory_order_acquire)) return false;
        if (!platform.foreground_matches()) {
            fail("Окно игры потеряло фокус. Перезарядка остановлена");
            return false;
        }
        return !stopping.load(std::memory_order_acquire);
    }

    bool within_budget(Clock::time_point deadline) {
        if (Clock::now() < deadline) return true;
        fail("Достигнут предел времени помощника перезарядки");
        return false;
    }

    bool wait_until(Clock::time_point deadline) {
        for (;;) {
            if (!foreground_ok()) return false;
            const auto now = Clock::now();
            if (now >= deadline) return true;
            std::unique_lock lock(wait_mutex);
            if (wake.wait_until(lock, std::min(deadline, now + focus_interval),
                                [this] { return stopping.load(std::memory_order_acquire); }))
                return false;
        }
    }

    bool wait_gap(int milliseconds, Clock::time_point overall_deadline) {
        return wait_until(std::min(overall_deadline, Clock::now() + Milliseconds{milliseconds})) &&
            within_budget(overall_deadline);
    }

    int randomized_delay(int base, bool enabled, int minimum, int maximum) {
        if (!enabled) return base;
        const int addition = [&] {
            if (platform.random_between) return platform.random_between(minimum, maximum);
            thread_local std::mt19937 random{std::random_device{}()};
            return std::uniform_int_distribution<int>{minimum, maximum}(random);
        }();
        if (addition < minimum || addition > maximum)
            throw std::invalid_argument("Случайная задержка помощника вне заданного диапазона");
        return base + addition;
    }

    bool release_key() noexcept {
        if (!owns_key.load(std::memory_order_acquire)) return true;
        for (int attempt = 0; attempt < 3; ++attempt) {
            try {
                if (platform.send_key(owned_direction, false)) {
                    owns_key.store(false, std::memory_order_release);
                    log_input("up", owned_direction, owned_stage, owned_index);
                    return true;
                }
            } catch (...) {
                fail("Ошибка обработчика отпускания клавиши перезарядки");
            }
        }
        fail("Windows не приняла отпускание клавиши перезарядки. Запуск заблокирован до её освобождения", true);
        return false;
    }

    bool press(ReloadDirection direction, int stage, int index, Clock::time_point overall_deadline) {
        if (!foreground_ok() || !within_budget(overall_deadline)) return false;
        if (platform.activation_released && !platform.activation_released()) {
            fail("Отпустите горячую клавишу и модификаторы. Перезарядка остановлена");
            return false;
        }
        const int hold_ms = randomized_delay(timing.hold_ms, timing.key_random_enabled,
            timing.key_random_min_ms, timing.key_random_max_ms);
        owned_direction = direction;
        owned_stage = stage;
        owned_index = index;
        log_input("down", direction, stage, index);
        if (!foreground_ok() || !within_budget(overall_deadline)) return false;
        owns_key.store(true, std::memory_order_release);
        if (!platform.send_key(direction, true)) {
            owns_key.store(false, std::memory_order_release);
            fail("Windows не приняла нажатие клавиши перезарядки");
            return false;
        }
        const bool held = wait_until(std::min(overall_deadline,
            Clock::now() + Milliseconds{hold_ms}));
        const bool released = release_key();
        // Retry an uncertain UP to free the owned key, but never continue the
        // batch after its handler already failed, even if a retry succeeds.
        const bool succeeded = held && released &&
            state.load(std::memory_order_acquire) != ReloadHelperState::error && foreground_ok();
        return within_budget(overall_deadline) && succeeded;
    }

    void batches(Clock::time_point overall_deadline) {
        for (int stage = 0; stage < stage_count; ++stage) {
            if (!foreground_ok() || !within_budget(overall_deadline)) return;
            state.store(ReloadHelperState::searching, std::memory_order_release);
            // One fresh snapshot supplies all four directions for this stage.
            // There is no capture, polling or animation check between keys.
            const auto observation = platform.observe();
            if (!foreground_ok() || !within_budget(overall_deadline)) return;
            if (!observation) {
                fail("Не найдены четыре стрелки. Перезарядка остановлена");
                return;
            }
            if (!valid_snapshot(*observation)) {
                fail("Некорректная или неоднозначная полоса стрелок. Перезарядка остановлена");
                return;
            }
            if (std::any_of(observation->arrows.begin(), observation->arrows.end(),
                    [](const ReloadArrow& arrow) { return arrow.accepted; })) {
                fail("Нужны четыре белые стрелки. Запустите помощник с новой перезарядки");
                return;
            }
            state.store(ReloadHelperState::verifying, std::memory_order_release);
            for (int key = 0; key < 4; ++key) {
                if (!press(observation->arrows[static_cast<std::size_t>(key)].direction,
                           stage, key, overall_deadline)) return;
                const int count = sent.fetch_add(1, std::memory_order_acq_rel) + 1;
                log_sent(count, stage, key);
                if (key < 3 && !wait_gap(randomized_delay(timing.key_gap_ms, timing.key_random_enabled,
                        timing.key_random_min_ms, timing.key_random_max_ms), overall_deadline)) return;
            }
            if (stage < stage_count - 1 &&
                !wait_gap(randomized_delay(timing.stage_gap_ms, timing.stage_random_enabled,
                    timing.stage_random_min_ms, timing.stage_random_max_ms), overall_deadline)) return;
        }
        if (foreground_ok() && within_budget(overall_deadline))
            state.store(ReloadHelperState::completed, std::memory_order_release);
    }

    void run() noexcept {
        try {
            const auto activated = Clock::now();
            const auto overall_deadline = activated + Milliseconds{timing.overall_timeout_ms};
            const auto activation_deadline = activated + Milliseconds{timing.activation_timeout_ms};
            while (foreground_ok() && platform.activation_released && !platform.activation_released()) {
                if (!within_budget(overall_deadline)) break;
                if (Clock::now() >= activation_deadline) {
                    fail("Отпустите горячую клавишу и запустите перезарядку заново");
                    break;
                }
                if (!wait_until(std::min({activation_deadline, overall_deadline,
                                         Clock::now() + focus_interval}))) break;
            }
            if (foreground_ok() && state.load(std::memory_order_acquire) != ReloadHelperState::error)
                batches(overall_deadline);
        } catch (const std::exception& exception) {
            fail(exception.what());
        } catch (...) {
            fail("Ошибка захвата, распознавания или ввода. Перезарядка остановлена");
        }
        release_key();
        if (state.load(std::memory_order_acquire) != ReloadHelperState::error &&
            state.load(std::memory_order_acquire) != ReloadHelperState::completed)
            state.store(ReloadHelperState::stopped, std::memory_order_release);
        running.store(false, std::memory_order_release);
    }
};

ReloadHelperRunner::ReloadHelperRunner() : impl_(std::make_unique<Impl>()) {}
ReloadHelperRunner::~ReloadHelperRunner() { stop(); }

void ReloadHelperRunner::start(ReloadHelperPlatform platform, ReloadHelperTiming timing) {
    stop();
    if (impl_->owns_key.load(std::memory_order_acquire))
        throw std::runtime_error("Не удалось отпустить клавишу предыдущей перезарядки");
    if (!platform.foreground_matches || !platform.observe || !platform.send_key)
        throw std::invalid_argument("Не настроен захват, проверка окна или ввод перезарядки");
    validate_timing(timing);
    impl_->platform = std::move(platform);
    impl_->timing = timing;
    {
        std::lock_guard lock(impl_->error_mutex);
        impl_->error.clear();
    }
    impl_->sent.store(0, std::memory_order_release);
    impl_->stopping.store(false, std::memory_order_release);
    impl_->state.store(ReloadHelperState::searching, std::memory_order_release);
    impl_->running.store(true, std::memory_order_release);
    try {
        impl_->worker = std::thread([state = impl_.get()] { state->run(); });
    } catch (...) {
        impl_->running.store(false, std::memory_order_release);
        impl_->state.store(ReloadHelperState::stopped, std::memory_order_release);
        throw;
    }
}

void ReloadHelperRunner::stop() noexcept {
    impl_->stopping.store(true, std::memory_order_release);
    impl_->wake.notify_all();
    if (impl_->worker.joinable()) impl_->worker.join();
    impl_->release_key();
    if (impl_->state.load(std::memory_order_acquire) != ReloadHelperState::error &&
        impl_->state.load(std::memory_order_acquire) != ReloadHelperState::completed)
        impl_->state.store(ReloadHelperState::stopped, std::memory_order_release);
    impl_->running.store(false, std::memory_order_release);
}

bool ReloadHelperRunner::running() const noexcept {
    return impl_->running.load(std::memory_order_acquire);
}

ReloadHelperState ReloadHelperRunner::state() const noexcept {
    return impl_->state.load(std::memory_order_acquire);
}

std::string ReloadHelperRunner::last_error() const {
    std::lock_guard lock(impl_->error_mutex);
    return impl_->error;
}

int ReloadHelperRunner::sent_count() const noexcept {
    return impl_->sent.load(std::memory_order_acquire);
}

bool ReloadHelperRunner::release_pending() const noexcept {
    return impl_->owns_key.load(std::memory_order_acquire);
}

}  // namespace wardogs
