#pragma once

#include <cstdint>
#include <functional>
#include <memory>

namespace wardogs {

// Kept independent of desktop state so the physical-input policy is testable.
[[nodiscard]] bool is_middle_mouse_press(std::uintptr_t message,
                                        std::uint32_t flags) noexcept;
[[nodiscard]] bool is_fire_mouse_press(std::uintptr_t message,
                                      std::uint32_t flags) noexcept;

struct MiddleMouseEvent {
    int x{};
    int y{};
    std::uintptr_t foreground_window{};
};

// An input intention only. Confirm discharge separately from the observed HUD.
struct FireIntentEvent {
    int x{};
    int y{};
    std::uintptr_t foreground_window{};
    std::uint64_t timestamp_ms{};
    // A coalesced message must never masquerade as one physical press.
    bool multiple_presses{};
    // Additional presses may belong to another foreground window, including
    // the game even when the retained first press came from another app.
    bool foreground_changed{};
    // The last press can fall inside a newer HUD context than the first one.
    std::uint64_t last_timestamp_ms{};
};

// Retain the first pre-press identity while exposing any additional physical
// presses awaiting dispatch. No device state or allocation is involved.
void coalesce_fire_intent(FireIntentEvent& pending, const FireIntentEvent& press,
                          bool already_pending) noexcept;

class GlobalMouseListener {
public:
    using Callback = std::function<void(MiddleMouseEvent)>;
    using FireCallback = std::function<void(FireIntentEvent)>;

    GlobalMouseListener();
    ~GlobalMouseListener();
    GlobalMouseListener(const GlobalMouseListener&) = delete;
    GlobalMouseListener& operator=(const GlobalMouseListener&) = delete;

    // Call lifecycle methods from the owning thread. The callback runs on the
    // listener's message thread, outside the hook; queue UI work to its owner.
    // Either callback may be empty; at least one must be supplied.
    void start(Callback callback, FireCallback fire_callback = {});
    // New callbacks are disabled immediately. An already running callback may
    // finish after return if it does not respond within the two-second limit.
    void stop() noexcept;
    [[nodiscard]] bool active() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace wardogs
