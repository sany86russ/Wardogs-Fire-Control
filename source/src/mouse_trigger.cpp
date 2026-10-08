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

struct GlobalMouseListener::Impl {
    static constexpr UINT capture_message = WM_APP + 0x57;
    static constexpr DWORD lifecycle_timeout_ms = 2000;

    struct State {
        Callback callback;
        HANDLE ready{};
        HHOOK hook{};
        std::atomic<DWORD> thread_id{};
        std::atomic<DWORD> startup_error{};
        std::atomic<bool> stopping{};
        std::atomic<bool> installed{};
        std::atomic<bool> hook_was_installed{};
        std::atomic<bool> hook_uninstalled{};
        std::atomic<bool> capture_pending{};
        // The hook and message callback run on this same dedicated thread.
        MiddleMouseEvent pending_event;

        ~State() { if (ready) CloseHandle(ready); }
    };

    static inline thread_local State* current_state{};
    std::shared_ptr<State> state;
    HANDLE thread{};

    static LRESULT CALLBACK hook_proc(int code, WPARAM message,
                                      LPARAM data) noexcept {
        State* state = current_state;
        // Mouse movement takes this single comparison path; no allocation,
        // logging, timestamps, UI, OCR, or callback execution inside the hook.
        if (code == HC_ACTION && state && message == WM_MBUTTONDOWN &&
            !state->stopping.load(std::memory_order_acquire)) {
            const auto* event = reinterpret_cast<const MSLLHOOKSTRUCT*>(data);
            if (event && is_middle_mouse_press(message, event->flags) &&
                !state->capture_pending.exchange(true, std::memory_order_acq_rel)) {
                state->pending_event = {event->pt.x, event->pt.y,
                    reinterpret_cast<std::uintptr_t>(GetForegroundWindow())};
                if (!PostThreadMessageW(state->thread_id.load(), capture_message, 0, 0))
                    state->capture_pending.store(false, std::memory_order_release);
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
                state->capture_pending.store(false, std::memory_order_release);
                if (!state->stopping.load(std::memory_order_acquire)) {
                    try {
                        state->callback(state->pending_event);
                    } catch (...) {
                        log_error("Ошибка обработки средней кнопки мыши.");
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

void GlobalMouseListener::start(Callback callback) {
    if (!callback)
        throw std::invalid_argument("Не указан обработчик средней кнопки мыши.");
    stop();
    auto state = std::make_shared<Impl::State>();
    state->callback = std::move(callback);
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
        throw std::runtime_error("Не удалось включить среднюю кнопку мыши (код Windows " +
                                 std::to_string(error) + ").");
    }
    log_info("Обработчик средней кнопки мыши включён.");
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
