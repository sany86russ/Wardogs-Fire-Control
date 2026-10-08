#pragma once

#include <cstdint>
#include <functional>
#include <memory>

namespace wardogs {

// Kept independent of desktop state so the physical-input policy is testable.
[[nodiscard]] bool is_middle_mouse_press(std::uintptr_t message,
                                        std::uint32_t flags) noexcept;

struct MiddleMouseEvent {
    int x{};
    int y{};
    std::uintptr_t foreground_window{};
};

class GlobalMouseListener {
public:
    using Callback = std::function<void(MiddleMouseEvent)>;

    GlobalMouseListener();
    ~GlobalMouseListener();
    GlobalMouseListener(const GlobalMouseListener&) = delete;
    GlobalMouseListener& operator=(const GlobalMouseListener&) = delete;

    // Call lifecycle methods from the owning thread. The callback runs on the
    // listener's message thread, outside the hook; queue UI work to its owner.
    void start(Callback callback);
    // New callbacks are disabled immediately. An already running callback may
    // finish after return if it does not respond within the two-second limit.
    void stop() noexcept;
    [[nodiscard]] bool active() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace wardogs
