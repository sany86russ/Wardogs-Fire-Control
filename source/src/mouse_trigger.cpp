#include "wardogs/mouse_trigger.hpp"
#include "wardogs/logger.hpp"

#include <Windows.h>
#include <process.h>

#include <atomic>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace wardogs {

bool is_middle_mouse_press(std::uintptr_t message, std::uint32_t flags) noexcept {
    return message == WM_MBUTTONDOWN &&
           (flags & (LLMHF_INJECTED | LLMHF_LOWER_IL_INJECTED)) == 0;
}

bool is_fire_mouse_press(std::uintptr_t message, std::uint32_t flags) noexcept {
    return message == WM_LBUTTONDOWN &&
           (flags & (LLMHF_INJECTED | LLMHF_LOWER_IL_INJECTED)) == 0;
}

std::optional<std::uint64_t> expand_mouse_message_time(
    std::uint32_t message_time_ms, std::uint64_t now_ms) noexcept {
    // Unsigned subtraction recovers elapsed time across the native 32-bit wrap.
    // The small admission window rejects future/native-clock mismatches without
    // guessing another epoch or granting a delayed hook a fresh press time.
    const auto elapsed = static_cast<std::uint32_t>(
        static_cast<std::uint32_t>(now_ms) - message_time_ms);
    if (elapsed > 1000 || now_ms < elapsed) return std::nullopt;
    return now_ms - elapsed;
}

void coalesce_fire_intent(FireIntentEvent& pending, const FireIntentEvent& press,
                          bool already_pending) noexcept {
    if (already_pending) {
        pending.multiple_presses = true;
        pending.timestamp_verified = pending.timestamp_verified && press.timestamp_verified;
        pending.foreground_changed = pending.foreground_changed ||
            press.foreground_window != pending.foreground_window;
        if (press.timestamp_ms > pending.last_timestamp_ms)
            pending.last_timestamp_ms = press.timestamp_ms;
    } else {
        pending = press;
        pending.last_timestamp_ms = press.timestamp_ms;
    }
}

struct GlobalMouseListener::Impl {
    static constexpr UINT capture_message = WM_APP + 0x57;
    static constexpr UINT fire_message = WM_APP + 0x58;
    static constexpr DWORD lifecycle_timeout_ms = 2000;

    struct State {
        Callback callback;
        FireCallback fire_callback;
        HANDLE ready{};
        HHOOK hook{};
        std::atomic<DWORD> thread_id{};
        std::atomic<DWORD> startup_error{};
        std::atomic<bool> stopping{};
        std::atomic<bool> installed{};
        std::atomic<bool> hook_was_installed{};
        std::atomic<bool> hook_uninstalled{};
        std::atomic<bool> capture_pending{};
        std::atomic<bool> fire_pending{};
        // The hook and message callback run on this same dedicated thread.
        MiddleMouseEvent pending_event;
        FireIntentEvent pending_fire_event;

        ~State() { if (ready) CloseHandle(ready); }
    };

    static inline thread_local State* current_state{};
    std::shared_ptr<State> state;
    HANDLE thread{};

    static LRESULT CALLBACK hook_proc(int code, WPARAM message,
                                      LPARAM data) noexcept {
        State* state = current_state;
        // Mouse movement takes this single comparison path; no allocation,
        // logging, UI, OCR, or callback execution inside the hook. Timestamp
        // collection is restricted to an accepted physical fire intention.
        if (code == HC_ACTION && state &&
            (message == WM_MBUTTONDOWN || message == WM_LBUTTONDOWN) &&
            !state->stopping.load(std::memory_order_acquire)) {
            const auto* event = reinterpret_cast<const MSLLHOOKSTRUCT*>(data);
            if (event && state->callback &&
                is_middle_mouse_press(message, event->flags) &&
                !state->capture_pending.exchange(true, std::memory_order_acq_rel)) {
                state->pending_event = {event->pt.x, event->pt.y,
                    reinterpret_cast<std::uintptr_t>(GetForegroundWindow())};
                if (!PostThreadMessageW(state->thread_id.load(), capture_message, 0, 0))
                    state->capture_pending.store(false, std::memory_order_release);
            } else if (event && state->fire_callback &&
                       is_fire_mouse_press(message, event->flags)) {
                const bool already_pending = state->fire_pending.exchange(
                    true, std::memory_order_acq_rel);
                const auto timestamp = expand_mouse_message_time(event->time, GetTickCount64());
                FireIntentEvent press{event->pt.x, event->pt.y,
                    reinterpret_cast<std::uintptr_t>(GetForegroundWindow()),
                    timestamp.value_or(0)};
                press.timestamp_verified = timestamp.has_value();
                coalesce_fire_intent(state->pending_fire_event, press, already_pending);
                if (!already_pending &&
                    !PostThreadMessageW(state->thread_id.load(), fire_message, 0, 0))
                    state->fire_pending.store(false, std::memory_order_release);
            }
        }
        return CallNextHookEx(state ? state->hook : nullptr, code, message, data);
    }

    static unsigned __stdcall run(void* parameter) noexcept {
        std::unique_ptr<std::shared_ptr<State>> owner{
            static_cast<std::shared_ptr<State>*>(parameter)};
        const auto state = *owner;
        owner.reset();
        state->thread_id.store(GetCurrentThreadId(), std::memory_order_release);
        MSG message{};
        // Ensure PostThreadMessage can terminate startup and the idle loop.
        PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
        current_state = state.get();
        if (!state->stopping.load(std::memory_order_acquire)) {
            state->hook = SetWindowsHookExW(WH_MOUSE_LL, hook_proc,
                                            GetModuleHandleW(nullptr), 0);
            if (!state->hook) state->startup_error.store(GetLastError());
            else {
                state->hook_was_installed.store(true, std::memory_order_release);
                state->installed.store(true, std::memory_order_release);
            }
        }
        SetEvent(state->ready);
        while (state->hook && !state->stopping.load(std::memory_order_acquire)) {
            const BOOL received = GetMessageW(&message, nullptr, 0, 0);
            if (received <= 0) {
                if (received < 0) log_error("Ошибка ожидания событий мыши.");
                break;
            }
            if (message.message == capture_message) {
                const auto event = state->pending_event;
                state->capture_pending.store(false, std::memory_order_release);
                if (!state->stopping.load(std::memory_order_acquire) && state->callback) {
                    try {
                        state->callback(event);
                    } catch (...) {
                        log_error("Ошибка обработки средней кнопки мыши.");
                    }
                }
            } else if (message.message == fire_message) {
                const auto event = state->pending_fire_event;
                state->fire_pending.store(false, std::memory_order_release);
                if (!state->stopping.load(std::memory_order_acquire) && state->fire_callback) {
                    try {
                        state->fire_callback(event);
                    } catch (...) {
                        log_error("Ошибка обработки намерения выстрела.");
                    }
                }
            } else {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        state->installed.store(false, std::memory_order_release);
        if (state->hook) {
            if (UnhookWindowsHookEx(state->hook))
                state->hook_uninstalled.store(true, std::memory_order_release);
            else log_warning("Не удалось отключить обработчик мыши.");
        }
        state->hook = nullptr;
        current_state = nullptr;
        return 0;
    }
};

GlobalMouseListener::GlobalMouseListener() : impl_(std::make_unique<Impl>()) {}
GlobalMouseListener::~GlobalMouseListener() { stop(); }

void GlobalMouseListener::start(Callback callback, FireCallback fire_callback) {
    if (!callback && !fire_callback)
        throw std::invalid_argument("Не указан обработчик мыши.");
    stop();
    auto state = std::make_shared<Impl::State>();
    state->callback = std::move(callback);
    state->fire_callback = std::move(fire_callback);
    state->ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!state->ready)
        throw std::runtime_error("Не удалось подготовить обработчик мыши.");
    auto thread_owner = std::make_unique<std::shared_ptr<Impl::State>>(state);
    const auto handle = _beginthreadex(nullptr, 0, Impl::run, thread_owner.get(), 0,
                                       nullptr);
    if (!handle)
        throw std::runtime_error("Не удалось запустить поток обработки мыши.");
    thread_owner.release(); // The worker now owns the shared-state reference.
    impl_->state = std::move(state);
    impl_->thread = reinterpret_cast<HANDLE>(handle);
    const DWORD ready = WaitForSingleObject(impl_->state->ready,
                                            Impl::lifecycle_timeout_ms);
    const DWORD error = impl_->state->startup_error.load();
    if (ready != WAIT_OBJECT_0 || !impl_->state->installed.load()) {
        stop();
        if (ready == WAIT_TIMEOUT)
            throw std::runtime_error("Обработчик мыши не ответил вовремя.");
        throw std::runtime_error("Не удалось включить обработчик мыши (код Windows " +
                                 std::to_string(error) + ").");
    }
    log_info("Обработчик мыши включён.");
}

void GlobalMouseListener::stop() noexcept {
    if (!impl_->state) return;
    impl_->state->stopping.store(true, std::memory_order_release);
    const DWORD thread_id = impl_->state->thread_id.load(std::memory_order_acquire);
    if (thread_id) PostThreadMessageW(thread_id, WM_QUIT, 0, 0);
    // Stopping from a callback must not wait for its own thread.
    if (impl_->thread && thread_id != GetCurrentThreadId()) {
        const DWORD stopped = WaitForSingleObject(impl_->thread,
                                                  Impl::lifecycle_timeout_ms);
        if (stopped == WAIT_OBJECT_0 && impl_->state->hook_was_installed.load() &&
            impl_->state->hook_uninstalled.load())
            log_info("mouse.hook_stopped success=1");
        else if (stopped != WAIT_OBJECT_0)
            log_warning("Поток мыши завершит текущий обработчик самостоятельно.");
    }
    if (impl_->thread) CloseHandle(impl_->thread);
    impl_->thread = nullptr;
    // On timeout the worker keeps its own shared_ptr, so its hook and callback
    // never refer to a destroyed listener or to freed implementation storage.
    impl_->state.reset();
}

bool GlobalMouseListener::active() const noexcept {
    return impl_->state && !impl_->state->stopping.load(std::memory_order_acquire) &&
           impl_->state->installed.load(std::memory_order_acquire);
}

}  // namespace wardogs
