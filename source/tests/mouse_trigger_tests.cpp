#include "wardogs/mouse_trigger.hpp"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>

int main() {
    using wardogs::is_middle_mouse_press;
    int failures = 0;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) {
            ++failures;
            std::cerr << "FAIL: " << message << '\n';
        }
    };
    check(is_middle_mouse_press(WM_MBUTTONDOWN, 0),
          "a physical middle-button press reads the existing map hover labels");
    check(!is_middle_mouse_press(WM_MBUTTONUP, 0) &&
              !is_middle_mouse_press(WM_MOUSEMOVE, 0) &&
              !is_middle_mouse_press(WM_LBUTTONUP, 0) &&
              !is_middle_mouse_press(WM_MOUSEWHEEL, 0),
          "releases, movements, other buttons, and wheel events never trigger OCR");
    check(!is_middle_mouse_press(WM_MBUTTONDOWN, LLMHF_INJECTED) &&
              !is_middle_mouse_press(WM_MBUTTONDOWN, LLMHF_LOWER_IL_INJECTED) &&
              !is_middle_mouse_press(WM_MBUTTONDOWN,
                                       LLMHF_INJECTED | LLMHF_LOWER_IL_INJECTED),
          "all injected-event variants are excluded from capture");

    wardogs::GlobalMouseListener listener;
    check(!listener.active(), "a new listener is inactive");
    listener.stop();
    listener.stop();
    bool empty_rejected = false;
    try { listener.start({}); }
    catch (const std::invalid_argument&) { empty_rejected = true; }
    check(empty_rejected && !listener.active(),
          "an absent callback cannot install an unusable hook");
    std::atomic<int> callbacks{};
    // Actual hook installation validates the Win32 lifecycle. No artificial
    // input is generated and no cursor/button state is changed by these tests.
    for (int attempt = 0; attempt < 12; ++attempt) {
        try {
            listener.start([&](wardogs::MiddleMouseEvent) { ++callbacks; });
            check(listener.active(), "the dedicated message thread installs its mouse hook");
            const auto begin = std::chrono::steady_clock::now();
            listener.stop();
            const auto elapsed = std::chrono::steady_clock::now() - begin;
            check(!listener.active() && elapsed < std::chrono::seconds{3},
                  "stopping joins the idle hook thread within its bounded timeout");
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL: hook lifecycle: " << error.what() << '\n';
            break;
        }
    }
    if (failures) return 1;
    std::cout << "All mouse trigger policy and Win32 lifecycle tests passed\n";
    return 0;
}
