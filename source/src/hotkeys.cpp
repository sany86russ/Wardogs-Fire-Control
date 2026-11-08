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

struct NamedKey { std::wstring_view name; UINT code; };
constexpr NamedKey named_keys[]{
    {L"Mouse4", VK_XBUTTON1}, {L"Mouse5", VK_XBUTTON2},
    {L"Backspace", VK_BACK}, {L"CapsLock", VK_CAPITAL}, {L"Pause", VK_PAUSE},
    {L"PrintScreen", VK_SNAPSHOT}, {L"ScrollLock", VK_SCROLL}, {L"NumLock", VK_NUMLOCK},
    {L"Clear", VK_CLEAR}, {L"Apps", VK_APPS}, {L"Sleep", VK_SLEEP},
    {L"Multiply", VK_MULTIPLY}, {L"Add", VK_ADD}, {L"Separator", VK_SEPARATOR},
    {L"Subtract", VK_SUBTRACT}, {L"Decimal", VK_DECIMAL}, {L"Divide", VK_DIVIDE},
    {L"Semicolon", VK_OEM_1}, {L"Plus", VK_OEM_PLUS}, {L"Comma", VK_OEM_COMMA},
    {L"Minus", VK_OEM_MINUS}, {L"Period", VK_OEM_PERIOD}, {L"Slash", VK_OEM_2},
    {L"Backtick", VK_OEM_3}, {L"BracketLeft", VK_OEM_4}, {L"Backslash", VK_OEM_5},
    {L"BracketRight", VK_OEM_6}, {L"Apostrophe", VK_OEM_7}, {L"OEM8", VK_OEM_8},
    {L"OEM102", VK_OEM_102},
    {L"VolumeMute", VK_VOLUME_MUTE}, {L"VolumeDown", VK_VOLUME_DOWN}, {L"VolumeUp", VK_VOLUME_UP},
    {L"MediaNext", VK_MEDIA_NEXT_TRACK}, {L"MediaPrevious", VK_MEDIA_PREV_TRACK},
    {L"MediaStop", VK_MEDIA_STOP}, {L"MediaPlayPause", VK_MEDIA_PLAY_PAUSE},
    {L"BrowserBack", VK_BROWSER_BACK}, {L"BrowserForward", VK_BROWSER_FORWARD},
    {L"BrowserRefresh", VK_BROWSER_REFRESH}, {L"BrowserStop", VK_BROWSER_STOP},
    {L"BrowserSearch", VK_BROWSER_SEARCH}, {L"BrowserFavorites", VK_BROWSER_FAVORITES},
    {L"BrowserHome", VK_BROWSER_HOME}, {L"LaunchMail", VK_LAUNCH_MAIL},
    {L"LaunchMedia", VK_LAUNCH_MEDIA_SELECT}, {L"LaunchApp1", VK_LAUNCH_APP1},
    {L"LaunchApp2", VK_LAUNCH_APP2}
};

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
    if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9)
        return L"Numpad" + std::to_wstring(key - VK_NUMPAD0);
    for (const auto& named : named_keys)
        if (named.code == key) return std::wstring(named.name);
    return L"VK" + std::to_wstring(key);
}

std::wstring canonical_display(UINT modifiers, UINT key) {
    std::wstring result;
    if (modifiers & MOD_WIN) result += L"Win+";
    if (modifiers & MOD_CONTROL) result += L"Ctrl+";
    if (modifiers & MOD_ALT) result += L"Alt+";
    if (modifiers & MOD_SHIFT) result += L"Shift+";
    return result + key_display(key);
}

}  // namespace

Hotkey parse_hotkey(std::wstring_view text) {
    std::wstring normalized = trim(std::wstring(text));
    if (normalized == L"+") normalized = L"Plus";
    else if (normalized.ends_with(L"++")) normalized.replace(normalized.size() - 1, 1, L"Plus");
    text = normalized;
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
    // Qt PortableText spells keypad bindings as Num+1 / Ctrl+Num+1.
    const auto num = std::find(parts.begin(), parts.end(), L"NUM");
    if (num != parts.end()) {
        if (num + 2 != parts.end())
            throw std::invalid_argument("Эта клавиша не поддерживается");
        auto& value = *(num + 1);
        if (value.size() == 1 && value[0] >= L'0' && value[0] <= L'9') value = L"NUMPAD" + value;
        else if (value == L"PLUS") value = L"ADD";
        else if (value == L"-") value = L"SUBTRACT";
        else if (value == L"*") value = L"MULTIPLY";
        else if (value == L"/") value = L"DIVIDE";
        else if (value == L"." || value == L",") value = L"DECIMAL";
        parts.erase(num);
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
        else if (part.starts_with(L"NUMPAD") && part.size() == 7 && part[6] >= L'0' && part[6] <= L'9')
            key = VK_NUMPAD0 + part[6] - L'0';
        else if (part == L"XBUTTON1" || part == L"MB4" || part == L"MOUSEBUTTON4") key = VK_XBUTTON1;
        else if (part == L"XBUTTON2" || part == L"MB5" || part == L"MOUSEBUTTON5") key = VK_XBUTTON2;
        else if (part == L"=" || part == L"EQUAL") key = VK_OEM_PLUS;
        else if (part == L";") key = VK_OEM_1;
        else if (part == L",") key = VK_OEM_COMMA;
        else if (part == L"-") key = VK_OEM_MINUS;
        else if (part == L".") key = VK_OEM_PERIOD;
        else if (part == L"/") key = VK_OEM_2;
        else if (part == L"`" || part == L"~") key = VK_OEM_3;
        else if (part == L"[") key = VK_OEM_4;
        else if (part == L"\\") key = VK_OEM_5;
        else if (part == L"]") key = VK_OEM_6;
        else if (part == L"'") key = VK_OEM_7;
        else if (part == L"BACKTAB") {
            key = VK_TAB;
            if (!(modifiers & MOD_SHIFT)) {
                modifiers |= MOD_SHIFT;
                display_modifiers.push_back(L"Shift");
            }
        }
        else if (part == L"MEDIANEXTTRACK") key = VK_MEDIA_NEXT_TRACK;
        else if (part == L"MEDIAPREVIOUSTRACK" || part == L"MEDIAPREVIOUS") key = VK_MEDIA_PREV_TRACK;
        else if (part == L"TOGGLEMUTE") key = VK_VOLUME_MUTE;
        else if (part == L"MEDIAPLAY" || part == L"MEDIAPAUSE") key = VK_MEDIA_PLAY_PAUSE;
        else {
            for (const auto& named : named_keys)
                if (part == upper(std::wstring(named.name))) { key = named.code; break; }
            if (!key && part.starts_with(L"VK")) {
                try {
                    std::size_t used{};
                    const auto code = std::stoul(part.substr(2), &used,
                        part.substr(2).starts_with(L"0X") ? 16 : 10);
                    if (used == part.size() - 2 && code > 0 && code <= 255) key = static_cast<UINT>(code);
                } catch (const std::exception&) { /* Invalid numeric key is rejected below. */ }
            }
        }
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

Hotkey hotkey_from_native(UINT virtual_key, UINT modifiers) {
    if (!virtual_key || virtual_key > 255)
        throw std::invalid_argument("Неверный код горячей клавиши или модификатора");
    return {modifiers | MOD_NOREPEAT, virtual_key, canonical_display(modifiers, virtual_key)};
}

bool is_mouse_hotkey(const Hotkey& hotkey) noexcept {
    return hotkey.virtual_key == VK_XBUTTON1 || hotkey.virtual_key == VK_XBUTTON2;
}

bool is_physical_hotkey_event(std::uint32_t flags, bool mouse) noexcept {
    return (flags & (mouse ? (LLMHF_INJECTED | LLMHF_LOWER_IL_INJECTED) :
        (LLKHF_INJECTED | LLKHF_LOWER_IL_INJECTED))) == 0;
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
        if ((hotkey.modifiers & MOD_WIN) != 0)
            throw std::invalid_argument("Сочетания с Win зарезервированы Windows. Используйте Alt, Ctrl или Shift");
        if (hotkey.virtual_key == VK_SHIFT || hotkey.virtual_key == VK_CONTROL ||
            hotkey.virtual_key == VK_MENU || (hotkey.virtual_key >= VK_LSHIFT && hotkey.virtual_key <= VK_RMENU) ||
            hotkey.virtual_key == VK_LWIN || hotkey.virtual_key == VK_RWIN)
            throw std::invalid_argument("Сочетание должно содержать одну обычную клавишу");
        if ((hotkey.virtual_key == VK_DELETE &&
                (hotkey.modifiers & (MOD_CONTROL | MOD_ALT)) == (MOD_CONTROL | MOD_ALT)) ||
            (hotkey.virtual_key == VK_ESCAPE &&
                (hotkey.modifiers & (MOD_CONTROL | MOD_SHIFT)) == (MOD_CONTROL | MOD_SHIFT)))
            throw std::invalid_argument("Это системное сочетание Windows. Выберите другое");
    }
}

void validate_reload_input_hotkeys(std::span<const Hotkey> active,
                                   bool reload_enabled) {
    if (!reload_enabled) return;
    constexpr UINT modifier_mask = MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN;
    for (const auto& hotkey : active) {
        const bool arrow = hotkey.virtual_key == VK_UP || hotkey.virtual_key == VK_DOWN ||
            hotkey.virtual_key == VK_LEFT || hotkey.virtual_key == VK_RIGHT;
        if (arrow && (hotkey.modifiers & modifier_mask) == 0)
            throw std::invalid_argument("При включённой автоперезарядке стрелки без модификаторов нельзя назначать горячими клавишами. Добавьте Ctrl, Alt или Shift");
        if (hotkey.virtual_key == L'R' && (hotkey.modifiers & modifier_mask) == 0)
            throw std::invalid_argument("При включённой автоперезарядке R запускает перезарядку. Добавьте Ctrl, Alt или Shift к горячей клавише");
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

std::optional<std::size_t> HotkeyMatcher::handle_hook_event(UINT virtual_key, bool pressed,
    UINT active_modifiers, std::uint32_t flags, bool mouse) {
    if (!is_physical_hotkey_event(flags, mouse)) return std::nullopt;
    return handle_key_event(virtual_key, pressed, active_modifiers);
}

struct GlobalHotkeyListener::Impl {
    static constexpr int first_hotkey_id = 0x4000;
    static constexpr DWORD lifecycle_timeout_ms = 2000;
    static constexpr UINT modifier_mask = MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN;
    static constexpr UINT hook_message = WM_APP + 0x168;

    static bool hook_backed(const Hotkey& hotkey) noexcept {
        return is_mouse_hotkey(hotkey) || hotkey.virtual_key == VK_F12;
    }

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
        std::vector<bool> registered;
        std::unique_ptr<HotkeyMatcher> hook_matcher;
        std::array<bool, 256> hook_keys{};
        HHOOK mouse_hook{};
        HHOOK keyboard_hook{};

        ~State() { if (ready) CloseHandle(ready); }
    };

    std::shared_ptr<State> state;
    HANDLE thread{};
    static inline thread_local State* hook_state{};

    static UINT current_modifiers() noexcept {
        UINT modifiers{};
        if (GetAsyncKeyState(VK_CONTROL) & 0x8000) modifiers |= MOD_CONTROL;
        if (GetAsyncKeyState(VK_MENU) & 0x8000) modifiers |= MOD_ALT;
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000) modifiers |= MOD_SHIFT;
        if ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) modifiers |= MOD_WIN;
        return modifiers;
    }

    static void hook_event(UINT key, bool down, DWORD flags, bool mouse) noexcept {
        const auto state = hook_state;
        if (!state || key >= state->hook_keys.size() || !state->hook_keys[key] ||
            state->stopping.load(std::memory_order_acquire)) return;
        const auto action = state->hook_matcher->handle_hook_event(key, down,
            current_modifiers(), flags, mouse);
        if (action)
            PostThreadMessageW(state->thread_id.load(std::memory_order_relaxed), hook_message,
                static_cast<WPARAM>(*action), reinterpret_cast<LPARAM>(GetForegroundWindow()));
    }

    static LRESULT CALLBACK mouse_callback(int code, WPARAM message, LPARAM data) noexcept {
        if (code >= 0 && (message == WM_XBUTTONDOWN || message == WM_XBUTTONUP)) {
            const auto& event = *reinterpret_cast<const MSLLHOOKSTRUCT*>(data);
            const auto button = HIWORD(event.mouseData);
            if (button == XBUTTON1 || button == XBUTTON2)
                hook_event(button == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2,
                    message == WM_XBUTTONDOWN, event.flags, true);
        }
        return CallNextHookEx(nullptr, code, message, data);
    }

    static LRESULT CALLBACK keyboard_callback(int code, WPARAM message, LPARAM data) noexcept {
        if (code >= 0 && (message == WM_KEYDOWN || message == WM_SYSKEYDOWN ||
                         message == WM_KEYUP || message == WM_SYSKEYUP)) {
            const auto& event = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(data);
            if (event.vkCode == VK_F12)
                hook_event(event.vkCode, message == WM_KEYDOWN || message == WM_SYSKEYDOWN,
                    event.flags, false);
        }
        return CallNextHookEx(nullptr, code, message, data);
    }

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
            state->registered.assign(state->hotkeys.size(), false);
            state->hook_matcher = std::make_unique<HotkeyMatcher>(state->hotkeys);
            for (std::size_t index = 0; index < state->hotkeys.size(); ++index) {
                if (state->stopping.load(std::memory_order_acquire)) break;
                const auto& requested = state->requested_hotkeys[index];
                if (hook_backed(requested)) {
                    state->hook_keys[requested.virtual_key] = true;
                    ++registered_count;
                    continue;
                }
                // Ordinary keyboard combinations retain exclusive Windows
                // reservations and the existing bounded conflict policy.
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
                state->registered[index] = true;
                state->had_registrations.store(true, std::memory_order_release);
            }
            if (registered_count == state->hotkeys.size() && !state->stopping.load(std::memory_order_acquire)) {
                hook_state = state.get();
                if (state->hook_keys[VK_XBUTTON1] || state->hook_keys[VK_XBUTTON2]) {
                    state->mouse_hook = SetWindowsHookExW(WH_MOUSE_LL, mouse_callback,
                        GetModuleHandleW(nullptr), 0);
                    if (!state->mouse_hook) state->startup_error.store(GetLastError());
                }
                if (!state->startup_error.load() && state->hook_keys[VK_F12]) {
                    state->keyboard_hook = SetWindowsHookExW(WH_KEYBOARD_LL, keyboard_callback,
                        GetModuleHandleW(nullptr), 0);
                    if (!state->keyboard_hook) state->startup_error.store(GetLastError());
                }
                if (state->mouse_hook || state->keyboard_hook)
                    state->had_registrations.store(true, std::memory_order_release);
                if (state->startup_error.load()) {
                    for (std::size_t index = 0; index < state->hotkeys.size(); ++index)
                        if (hook_backed(state->hotkeys[index])) { state->failed_index.store(index); break; }
                }
            }
            if (registered_count == state->hotkeys.size() &&
                !state->startup_error.load() &&
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
                if (message.message == hook_message && message.hwnd == nullptr) {
                    const auto index = static_cast<std::size_t>(message.wParam);
                    if (index < state->hotkeys.size() && hook_backed(state->hotkeys[index]) &&
                        reinterpret_cast<HWND>(message.lParam) == GetForegroundWindow() &&
                        !state->stopping.load(std::memory_order_acquire)) {
                        try { state->callback(index); }
                        catch (...) { log_error("Ошибка обработчика горячей клавиши."); }
                    }
                    continue;
                }
                if (message.message != WM_HOTKEY || message.hwnd != nullptr ||
                    message.wParam < first_hotkey_id ||
                    message.wParam >= first_hotkey_id + registered_count ||
                    state->stopping.load(std::memory_order_acquire)) continue;
                const auto index = static_cast<std::size_t>(message.wParam - first_hotkey_id);
                if (!state->registered[index]) continue;
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
        hook_state = nullptr;
        if (state->mouse_hook && !UnhookWindowsHookEx(state->mouse_hook))
            state->unregistered.store(false, std::memory_order_release);
        if (state->keyboard_hook && !UnhookWindowsHookEx(state->keyboard_hook))
            state->unregistered.store(false, std::memory_order_release);
        // Cleanup happens on the same thread that registered the combinations.
        // A partially registered set is rolled back before this thread exits.
        for (std::size_t index = 0; index < registered_count; ++index) {
            if (!state->registered[index]) continue;
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
