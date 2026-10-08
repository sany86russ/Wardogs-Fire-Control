#pragma once

#include <Windows.h>

#include <array>
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
void validate_unique_hotkeys(std::span<const Hotkey> hotkeys);
// Checks documented RegisterHotKey restrictions before any OS registration.
void validate_global_hotkeys(std::span<const Hotkey> hotkeys);

class HotkeyMatcher {
public:
    explicit HotkeyMatcher(std::span<const Hotkey> hotkeys);

    std::optional<std::size_t> handle_key_event(UINT virtual_key, bool pressed,
                                                UINT active_modifiers);

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
