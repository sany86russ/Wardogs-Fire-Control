#include "wardogs/hotkeys.hpp"
#include "wardogs/logger.hpp"

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <memory>
#include <optional>
#include <process.h>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace wardogs {
namespace {

std::wstring trim(std::wstring value) {
    const auto first = std::find_if_not(value.begin(), value.end(), std::iswspace);
    const auto last = std::find_if_not(value.rbegin(), value.rend(), std::iswspace).base();
    return first < last ? std::wstring(first, last) : std::wstring{};
}

std::wstring upper(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), std::towupper);
    return value;
}

std::string utf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int length = static_cast<int>(value.size());
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                             value.data(), length, nullptr, 0, nullptr, nullptr);
    if (required <= 0) return {};
    std::string result(static_cast<std::size_t>(required), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length,
                             result.data(), required, nullptr, nullptr)) return {};
    return result;
}

std::wstring key_display(UINT key) {
    if (key >= VK_F1 && key <= VK_F24)
        return L"F" + std::to_wstring(key - VK_F1 + 1);
    if (key == VK_SPACE) return L"Space";
    if (key == VK_TAB) return L"Tab";
    if (key == VK_RETURN) return L"Enter";
    if (key == VK_ESCAPE) return L"Esc";
    if (key == VK_INSERT) return L"Insert";
    if (key == VK_DELETE) return L"Delete";
    if (key == VK_HOME) return L"Home";
    if (key == VK_END) return L"End";
    if (key == VK_PRIOR) return L"PageUp";
    if (key == VK_NEXT) return L"PageDown";
    if (key == VK_UP) return L"Up";
    if (key == VK_DOWN) return L"Down";
    if (key == VK_LEFT) return L"Left";
    if (key == VK_RIGHT) return L"Right";
    if ((key >= L'A' && key <= L'Z') || (key >= L'0' && key <= L'9'))
        return std::wstring{static_cast<wchar_t>(key)};
    const UINT scan_code = MapVirtualKeyW(key, MAPVK_VK_TO_VSC);
    wchar_t label[64]{};
    if (scan_code && GetKeyNameTextW(static_cast<LONG>(scan_code << 16), label,
                                   static_cast<int>(std::size(label))) > 0)
        return label;
    return L"VK" + std::to_wstring(key);
}

std::wstring canonical_display(UINT modifiers, UINT key) {
    std::wstring result;
    if (modifiers & MOD_CONTROL) result += L"Ctrl+";
    if (modifiers & MOD_ALT) result += L"Alt+";
    if (modifiers & MOD_SHIFT) result += L"Shift+";
    return result + key_display(key);
}

}  // namespace

Hotkey parse_hotkey(std::wstring_view text) {
    if (trim(std::wstring{text}).empty())
        throw std::invalid_argument("Горячая клавиша не может быть пустой");
    std::vector<std::wstring> parts;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const auto end = text.find(L'+', begin);
        auto part = trim(std::wstring{text.substr(begin, end == std::wstring_view::npos
                                                          ? text.size() - begin
                                                          : end - begin)});
        if (part.empty())
            throw std::invalid_argument("Горячая клавиша содержит пустую часть сочетания");
        parts.push_back(upper(std::move(part)));
        if (end == std::wstring_view::npos) break;
        begin = end + 1;
    }
    if (parts.empty()) {
        throw std::invalid_argument("Горячая клавиша не может быть пустой");
    }

    UINT modifiers = MOD_NOREPEAT;
    UINT key = 0;
    std::vector<std::wstring> display_modifiers;
    for (const auto& part : parts) {
        UINT modifier = 0;
        std::wstring label;
        if (part == L"CTRL" || part == L"CONTROL") {
            modifier = MOD_CONTROL; label = L"Ctrl";
        } else if (part == L"ALT") {
            modifier = MOD_ALT; label = L"Alt";
        } else if (part == L"SHIFT") {
            modifier = MOD_SHIFT; label = L"Shift";
        } else if (part == L"WIN" || part == L"META") {
            modifier = MOD_WIN; label = L"Win";
        }
        if (modifier) {
            if (modifiers & modifier || key) {
                throw std::invalid_argument("Модификаторы должны стоять перед обычной клавишей и не повторяться");
            }
            modifiers |= modifier;
            display_modifiers.push_back(std::move(label));
            continue;
        }
        if (key) {
            throw std::invalid_argument("Сочетание должно содержать одну обычную клавишу");
        }
        if (part.size() == 1 && ((part[0] >= L'A' && part[0] <= L'Z') ||
                                 (part[0] >= L'0' && part[0] <= L'9'))) {
            key = static_cast<UINT>(part[0]);
        } else if (part.size() >= 2 && part.size() <= 3 && part[0] == L'F') {
            unsigned number = 0;
            for (std::size_t index = 1; index < part.size(); ++index) {
                if (part[index] < L'0' || part[index] > L'9') {
                    number = 0;
                    break;
                }
                number = number * 10 + static_cast<unsigned>(part[index] - L'0');
            }
            if (number >= 1 && number <= 24) key = VK_F1 + number - 1;
        } else if (part == L"SPACE") key = VK_SPACE;
        else if (part == L"TAB") key = VK_TAB;
        else if (part == L"ENTER" || part == L"RETURN") key = VK_RETURN;
        else if (part == L"ESC" || part == L"ESCAPE") key = VK_ESCAPE;
        else if (part == L"INSERT") key = VK_INSERT;
        else if (part == L"DELETE") key = VK_DELETE;
        else if (part == L"HOME") key = VK_HOME;
        else if (part == L"END") key = VK_END;
        else if (part == L"PAGEUP") key = VK_PRIOR;
        else if (part == L"PAGEDOWN") key = VK_NEXT;
        else if (part == L"UP") key = VK_UP;
        else if (part == L"DOWN") key = VK_DOWN;
        else if (part == L"LEFT") key = VK_LEFT;
        else if (part == L"RIGHT") key = VK_RIGHT;
        if (!key) {
            throw std::invalid_argument("Эта клавиша не поддерживается");
        }
    }
    if (!key) {
        throw std::invalid_argument("Добавьте обычную клавишу после модификатора");
    }
    std::wstring display;
    for (const auto& item : display_modifiers) {
        if (!display.empty()) display += L'+';
        display += item;
    }
    if (!display.empty()) display += L'+';
    display += key_display(key);
    return {modifiers, key, display};
}

void validate_unique_hotkeys(std::span<const Hotkey> hotkeys) {
    constexpr UINT modifier_mask = MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN;
    for (std::size_t i = 0; i < hotkeys.size(); ++i) {
        for (std::size_t j = i + 1; j < hotkeys.size(); ++j) {
            if ((hotkeys[i].modifiers & modifier_mask) ==
                    (hotkeys[j].modifiers & modifier_mask) &&
                hotkeys[i].virtual_key == hotkeys[j].virtual_key) {
                throw std::invalid_argument("Горячие клавиши не должны повторяться");
            }
        }
    }
}

void validate_global_hotkeys(std::span<const Hotkey> hotkeys) {
    validate_unique_hotkeys(hotkeys);
    if (hotkeys.empty() || hotkeys.size() > 0x8000)
        throw std::invalid_argument("Укажите от 1 до 32768 горячих клавиш");
    constexpr UINT allowed_modifiers =
        MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN | MOD_NOREPEAT;
    for (const auto& hotkey : hotkeys) {
        if (hotkey.virtual_key == 0 || hotkey.virtual_key > 0xFF ||
            (hotkey.modifiers & ~allowed_modifiers) != 0)
            throw std::invalid_argument("Неверный код горячей клавиши или модификатора");
        if (hotkey.virtual_key == VK_F12)
            throw std::invalid_argument("F12 зарезервирована Windows для отладчика. Выберите другую клавишу");
        if ((hotkey.modifiers & MOD_WIN) != 0)
            throw std::invalid_argument("Сочетания с Win зарезервированы Windows. Используйте Alt, Ctrl или Shift");
    }
}

HotkeyMatcher::HotkeyMatcher(std::span<const Hotkey> hotkeys)
    : hotkeys_(hotkeys.begin(), hotkeys.end()) {
    validate_unique_hotkeys(hotkeys_);
}

std::optional<std::size_t> HotkeyMatcher::handle_key_event(
    UINT virtual_key, bool pressed, UINT active_modifiers) {
    if (virtual_key >= pressed_keys_.size()) return std::nullopt;
    if (!pressed) {
        pressed_keys_[virtual_key] = false;
        return std::nullopt;
    }
    if (pressed_keys_[virtual_key]) return std::nullopt;
    pressed_keys_[virtual_key] = true;

    constexpr UINT modifier_mask = MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN;
    const UINT current_modifiers = active_modifiers & modifier_mask;
    for (std::size_t index = 0; index < hotkeys_.size(); ++index) {
        const auto& hotkey = hotkeys_[index];
        if (hotkey.virtual_key == virtual_key &&
            (hotkey.modifiers & modifier_mask) == current_modifiers) {
            return index;
        }
    }
    return std::nullopt;
}

struct GlobalHotkeyListener::Impl {
    static constexpr int first_hotkey_id = 0x4000;
    static constexpr DWORD lifecycle_timeout_ms = 2000;
    static constexpr UINT modifier_mask = MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN;

    struct State {
        std::vector<Hotkey> requested_hotkeys;
        std::vector<Hotkey> hotkeys;
        bool conflict_fallback{};
        Callback callback;
        HANDLE ready{};
        std::atomic<DWORD> thread_id{};
        std::atomic<DWORD> startup_error{};
        std::atomic<std::size_t> failed_index{};
        std::atomic<bool> stopping{};
        std::atomic<bool> installed{};
        std::atomic<bool> had_registrations{};
        std::atomic<bool> unregistered{true};

        ~State() { if (ready) CloseHandle(ready); }
    };

    std::shared_ptr<State> state;
    HANDLE thread{};

    static unsigned __stdcall run(void* parameter) noexcept {
        std::unique_ptr<std::shared_ptr<State>> owner{
            static_cast<std::shared_ptr<State>*>(parameter)};
        const auto state = *owner;
        owner.reset();
        state->thread_id.store(GetCurrentThreadId(), std::memory_order_release);
        MSG message{};
        PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
        std::size_t registered_count = 0;
        try {
            for (std::size_t index = 0; index < state->hotkeys.size(); ++index) {
                if (state->stopping.load(std::memory_order_acquire)) break;
                const auto& requested = state->requested_hotkeys[index];
                // Only these explicit combinations reach this process. No
                // keyboard hook, async-key polling, or raw input is requested.
                bool registered = RegisterHotKey(nullptr,
                    first_hotkey_id + static_cast<int>(index),
                    requested.modifiers | MOD_NOREPEAT, requested.virtual_key) != FALSE;
                DWORD error = registered ? ERROR_SUCCESS : GetLastError();
                if (!registered && state->conflict_fallback &&
                    error == ERROR_HOTKEY_ALREADY_REGISTERED) {
                    constexpr std::array additions{MOD_CONTROL, MOD_SHIFT,
                                                   MOD_CONTROL | MOD_SHIFT};
                    std::array<UINT, additions.size()> attempted{};
                    std::size_t attempted_count = 0;
                    for (const UINT added : additions) {
                        if (state->stopping.load(std::memory_order_acquire)) break;
                        const UINT modifiers = requested.modifiers | added | MOD_NOREPEAT;
                        const UINT physical_modifiers = modifiers & modifier_mask;
                        if (physical_modifiers == (requested.modifiers & modifier_mask) ||
                            std::find(attempted.begin(), attempted.begin() + attempted_count,
                                      physical_modifiers) != attempted.begin() + attempted_count)
                            continue;
                        attempted[attempted_count++] = physical_modifiers;
                        const auto matches = [&](const Hotkey& existing) {
                            return existing.virtual_key == requested.virtual_key &&
                                   (existing.modifiers & modifier_mask) == physical_modifiers;
                        };
                        // An alternative must not steal a later requested
                        // action, nor duplicate an earlier chosen alternative.
                        if (std::any_of(state->requested_hotkeys.begin(),
                                        state->requested_hotkeys.end(), matches) ||
                            std::any_of(state->hotkeys.begin(),
                                        state->hotkeys.begin() + index, matches))
                            continue;
                        Hotkey candidate{modifiers, requested.virtual_key,
                                         canonical_display(modifiers, requested.virtual_key)};
                        registered = RegisterHotKey(nullptr,
                            first_hotkey_id + static_cast<int>(index),
                            candidate.modifiers, candidate.virtual_key) != FALSE;
                        error = registered ? ERROR_SUCCESS : GetLastError();
                        if (registered) {
                            state->hotkeys[index] = std::move(candidate);
                            break;
                        }
                        // Access, resource and other OS failures do not permit
                        // trying an unrelated shortcut to hide the error.
                        if (error != ERROR_HOTKEY_ALREADY_REGISTERED) break;
                    }
                }
                if (!registered) {
                    state->failed_index.store(index);
                    state->startup_error.store(error);
                    break;
                }
                ++registered_count;
                state->had_registrations.store(true, std::memory_order_release);
            }
            if (registered_count == state->hotkeys.size() &&
                !state->stopping.load(std::memory_order_acquire))
                state->installed.store(true, std::memory_order_release);
            SetEvent(state->ready);
            while (state->installed.load(std::memory_order_acquire) &&
                   !state->stopping.load(std::memory_order_acquire)) {
                const BOOL received = GetMessageW(&message, nullptr, 0, 0);
                if (received <= 0) {
                    if (received < 0) log_error("Ошибка ожидания горячих клавиш Windows.");
                    break;
                }
                if (message.message != WM_HOTKEY || message.hwnd != nullptr ||
                    message.wParam < first_hotkey_id ||
                    message.wParam >= first_hotkey_id + registered_count ||
                    state->stopping.load(std::memory_order_acquire)) continue;
                const auto index = static_cast<std::size_t>(message.wParam - first_hotkey_id);
                const auto& hotkey = state->hotkeys[index];
                if (static_cast<UINT>(HIWORD(message.lParam)) != hotkey.virtual_key ||
                    (static_cast<UINT>(LOWORD(message.lParam)) & modifier_mask) !=
                        (hotkey.modifiers & modifier_mask)) continue;
                try {
                    log_info("hotkey.dispatch index=" + std::to_string(index));
                    state->callback(index);
                } catch (...) {
                    log_error("Ошибка обработчика горячей клавиши.");
                }
            }
        } catch (...) {
            state->startup_error.store(ERROR_NOT_ENOUGH_MEMORY);
            SetEvent(state->ready);
            log_error("Не удалось обработать горячие клавиши.");
        }
        state->installed.store(false, std::memory_order_release);
        // Cleanup happens on the same thread that registered the combinations.
        // A partially registered set is rolled back before this thread exits.
        for (std::size_t index = 0; index < registered_count; ++index) {
            if (!UnregisterHotKey(nullptr, first_hotkey_id + static_cast<int>(index))) {
                state->unregistered.store(false, std::memory_order_release);
                log_warning("Windows не смогла освободить горячую клавишу.");
            }
        }
        return 0;
    }
};

GlobalHotkeyListener::GlobalHotkeyListener() : impl_(std::make_unique<Impl>()) {}

GlobalHotkeyListener::~GlobalHotkeyListener() { stop(); }

void GlobalHotkeyListener::start(std::span<const Hotkey> hotkeys, Callback callback) {
    (void)start_impl(hotkeys, std::move(callback), false);
}

std::vector<Hotkey> GlobalHotkeyListener::start_with_conflict_fallback(
    std::span<const Hotkey> hotkeys, Callback callback) {
    return start_impl(hotkeys, std::move(callback), true);
}

std::vector<Hotkey> GlobalHotkeyListener::start_impl(
    std::span<const Hotkey> hotkeys, Callback callback, bool conflict_fallback) {
    validate_global_hotkeys(hotkeys);
    if (!callback)
        throw std::invalid_argument("Не указан обработчик горячих клавиш");
    stop();
    auto state = std::make_shared<Impl::State>();
    state->requested_hotkeys.assign(hotkeys.begin(), hotkeys.end());
    state->hotkeys.assign(hotkeys.begin(), hotkeys.end());
    state->conflict_fallback = conflict_fallback;
    state->callback = std::move(callback);
    state->ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!state->ready)
        throw std::runtime_error("Не удалось подготовить горячие клавиши Windows");
    auto thread_owner = std::make_unique<std::shared_ptr<Impl::State>>(state);
    const auto handle = _beginthreadex(nullptr, 0, Impl::run, thread_owner.get(), 0, nullptr);
    if (!handle)
        throw std::runtime_error("Не удалось запустить поток горячих клавиш Windows");
    thread_owner.release();
    impl_->state = state;
    impl_->thread = reinterpret_cast<HANDLE>(handle);
    const DWORD ready = WaitForSingleObject(state->ready, Impl::lifecycle_timeout_ms);
    const DWORD wait_error = ready == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
    const DWORD error = state->startup_error.load();
    const std::size_t failed_index = state->failed_index.load();
    if (ready != WAIT_OBJECT_0 || !state->installed.load(std::memory_order_acquire)) {
        // The worker owns its state even if it takes longer than our timeout.
        // In ordinary registration failure stop() joins the completed rollback.
        stop();
        if (ready == WAIT_TIMEOUT)
            throw std::runtime_error("Горячие клавиши Windows не ответили вовремя");
        if (ready == WAIT_FAILED)
            throw std::runtime_error("Не удалось дождаться горячих клавиш Windows: код " +
                                     std::to_string(wait_error));
        const auto& display = hotkeys[failed_index].display;
        std::string name = utf8(display);
        if (name.empty()) name = "№" + std::to_string(failed_index + 1);
        log_error("hotkey.registration_failed index=" + std::to_string(failed_index) +
                  " windows_error=" + std::to_string(error));
        throw std::runtime_error("Не удалось зарегистрировать " + name +
            (error == ERROR_HOTKEY_ALREADY_REGISTERED
                ? ": сочетание занято другой программой. Выберите другое"
                : ": ошибка Windows " + std::to_string(error)));
    }
    try {
        log_info("hotkey.registration_started count=" + std::to_string(hotkeys.size()));
        return state->hotkeys;
    } catch (...) {
        stop();
        throw;
    }
}

void GlobalHotkeyListener::stop() noexcept {
    if (!impl_->state) return;
    impl_->state->stopping.store(true, std::memory_order_release);
    const DWORD thread_id = impl_->state->thread_id.load(std::memory_order_acquire);
    if (thread_id) PostThreadMessageW(thread_id, WM_QUIT, 0, 0);
    if (impl_->thread && thread_id != GetCurrentThreadId()) {
        const DWORD stopped = WaitForSingleObject(impl_->thread, Impl::lifecycle_timeout_ms);
        if (stopped == WAIT_OBJECT_0 && impl_->state->had_registrations.load() &&
            impl_->state->unregistered.load())
            log_info("hotkey.registration_stopped success=1");
        else if (stopped != WAIT_OBJECT_0)
            log_warning("Поток горячих клавиш завершит текущий обработчик самостоятельно.");
    }
    if (impl_->thread) CloseHandle(impl_->thread);
    impl_->thread = nullptr;
    impl_->state.reset();
}

bool GlobalHotkeyListener::active() const noexcept {
    return impl_->state && !impl_->state->stopping.load(std::memory_order_acquire) &&
           impl_->state->installed.load(std::memory_order_acquire);
}

}  // namespace wardogs
