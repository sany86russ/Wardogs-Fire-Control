#pragma once

#include <Windows.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wardogs {

struct Hotkey {
    UINT modifiers{};
    UINT virtual_key{};
    std::wstring display;

    bool operator==(const Hotkey&) const = default;
};

Hotkey parse_hotkey(std::wstring_view text);
// Canonical, layout-independent names for native virtual keys (including
// Mouse4/Mouse5). Useful when a settings field records a physical shortcut.
Hotkey hotkey_from_native(UINT virtual_key, UINT modifiers);
[[nodiscard]] bool is_mouse_hotkey(const Hotkey& hotkey) noexcept;
// Hook events never synthesize or consume input. Injected events are ignored.
[[nodiscard]] bool is_physical_hotkey_event(std::uint32_t flags, bool mouse) noexcept;
void validate_unique_hotkeys(std::span<const Hotkey> hotkeys);
// Checks documented RegisterHotKey restrictions before any OS registration.
void validate_global_hotkeys(std::span<const Hotkey> hotkeys);
// Reload synthesizes unmodified arrow keys. An enabled action registered on
// one of those keys could consume the input or trigger another helper. Only
// active assignments are supplied; disabled stored shortcuts stay editable.
void validate_reload_input_hotkeys(std::span<const Hotkey> active,
                                  bool reload_enabled);

class HotkeyMatcher {
public:
    explicit HotkeyMatcher(std::span<const Hotkey> hotkeys);

    std::optional<std::size_t> handle_key_event(UINT virtual_key, bool pressed,
                                                UINT active_modifiers);
    std::optional<std::size_t> handle_hook_event(UINT virtual_key, bool pressed,
        UINT active_modifiers, std::uint32_t flags, bool mouse);

private:
    std::vector<Hotkey> hotkeys_;
    std::array<bool, 256> pressed_keys_{};
};

class GlobalHotkeyListener {
public:
    using Callback = std::function<void(std::size_t)>;

    GlobalHotkeyListener();
    ~GlobalHotkeyListener();
    GlobalHotkeyListener(const GlobalHotkeyListener&) = delete;
    GlobalHotkeyListener& operator=(const GlobalHotkeyListener&) = delete;

    // Registration is all-or-nothing. Callbacks run on the listener's own
    // message thread; UI callers must marshal them to their UI thread.
    void start(std::span<const Hotkey> hotkeys, Callback callback);
    // Only an occupied combination permits a bounded alternative on the same
    // key, adding Ctrl and/or Shift. Returns the complete actual registered set;
    // callers must use it for displayed shortcuts and subsequent persistence.
    [[nodiscard]] std::vector<Hotkey> start_with_conflict_fallback(
        std::span<const Hotkey> hotkeys, Callback callback);
    void stop() noexcept;
    [[nodiscard]] bool active() const noexcept;

private:
    std::vector<Hotkey> start_impl(std::span<const Hotkey> hotkeys,
                                  Callback callback, bool conflict_fallback);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace wardogs
