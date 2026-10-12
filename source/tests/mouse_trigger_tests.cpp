#include "wardogs/mouse_trigger.hpp"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string_view>

int main(int argc, char** argv) {
    const bool policy_only = argc == 2 && std::string_view(argv[1]) == "--policy-only";
    if (argc != 1 && !policy_only) {
        std::cerr << "Expected no arguments or --policy-only\n";
        return 2;
    }
    using wardogs::is_middle_mouse_press;
    using wardogs::is_fire_mouse_press;
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
    check(is_fire_mouse_press(WM_LBUTTONDOWN, 0),
          "a physical left-button press records a fire intention");
    check(!is_fire_mouse_press(WM_LBUTTONUP, 0) &&
              !is_fire_mouse_press(WM_MBUTTONDOWN, 0) &&
              !is_fire_mouse_press(WM_MOUSEMOVE, 0) &&
              !is_fire_mouse_press(WM_RBUTTONDOWN, 0) &&
              !is_fire_mouse_press(WM_MOUSEWHEEL, 0),
          "fire intentions exclude releases, movements, other buttons, and the wheel");
    check(!is_fire_mouse_press(WM_LBUTTONDOWN, LLMHF_INJECTED) &&
              !is_fire_mouse_press(WM_LBUTTONDOWN, LLMHF_LOWER_IL_INJECTED) &&
              !is_fire_mouse_press(WM_LBUTTONDOWN,
                                    LLMHF_INJECTED | LLMHF_LOWER_IL_INJECTED),
          "injected left-button presses never record a fire intention");
    check(!is_middle_mouse_press(WM_LBUTTONDOWN, 0) &&
              !is_fire_mouse_press(WM_MBUTTONDOWN, 0),
          "map capture and fire intention policies remain independent");

    wardogs::FireIntentEvent pending;
    const wardogs::FireIntentEvent first{12, 23, 42, 300};
    wardogs::coalesce_fire_intent(pending, first, false);
    check(!pending.multiple_presses && !pending.foreground_changed &&
              pending.timestamp_ms == 300 && pending.last_timestamp_ms == 300 &&
              pending.foreground_window == 42,
          "the first pending physical press retains its capture-time identity");
    wardogs::coalesce_fire_intent(pending, {88, 99, 42, 305}, true);
    check(pending.multiple_presses && !pending.foreground_changed &&
              pending.timestamp_ms == 300 && pending.last_timestamp_ms == 305 &&
              pending.foreground_window == 42 && pending.x == 12 && pending.y == 23,
          "same-window queued presses mark ambiguity without replacing the first snapshot");
    wardogs::coalesce_fire_intent(pending, {90, 91, 45, 310}, true);
    check(pending.multiple_presses && pending.foreground_changed &&
              pending.timestamp_ms == 300 && pending.last_timestamp_ms == 310 &&
              pending.foreground_window == 42,
          "a different foreground window is exposed while retaining the first press identity");
    wardogs::coalesce_fire_intent(pending, {92, 93, 42, 315}, true);
    check(pending.multiple_presses && pending.foreground_changed &&
              pending.last_timestamp_ms == 315,
          "returning to the first window cannot clear cross-window ambiguity");
    wardogs::coalesce_fire_intent(pending, {7, 8, 46, 400}, false);
    check(!pending.multiple_presses && !pending.foreground_changed &&
              pending.timestamp_ms == 400 && pending.last_timestamp_ms == 400 &&
              pending.foreground_window == 46 && pending.x == 7 && pending.y == 8,
          "a new dispatch batch has its own first press and clears the old coalescing marker");
    wardogs::coalesce_fire_intent(pending, {10, 11, 46, 100}, false);
    wardogs::coalesce_fire_intent(pending, {20, 21, 46, 5000}, true);
    check(pending.multiple_presses && pending.timestamp_ms == 100 &&
              pending.last_timestamp_ms == 5000 && pending.x == 10,
          "a coalesced press entering a newer HUD context exposes its latest time without replacing the first snapshot");
    wardogs::coalesce_fire_intent(pending, {22, 23, 46, 4000}, true);
    check(pending.last_timestamp_ms == 5000,
          "out-of-order press timestamps cannot move the batch end backward");
    if (policy_only) return failures == 0 ? 0 : 1;

    wardogs::GlobalMouseListener listener;
    check(!listener.active(), "a new listener is inactive");
    listener.stop();
    listener.stop();
    bool empty_rejected = false;
    try { listener.start({}, {}); }
    catch (const std::invalid_argument&) { empty_rejected = true; }
    check(empty_rejected && !listener.active(),
          "two absent callbacks cannot install an unusable hook");
    std::atomic<int> callbacks{};
    std::atomic<int> fire_callbacks{};
    // Actual hook installation validates the Win32 lifecycle. No artificial
    // input is generated and no cursor/button state is changed by these tests.
    for (int attempt = 0; attempt < 12; ++attempt) {
        try {
            const auto middle = [&](wardogs::MiddleMouseEvent) { ++callbacks; };
            const auto fire = [&](wardogs::FireIntentEvent) { ++fire_callbacks; };
            if (attempt % 3 == 0) listener.start(middle);
            else if (attempt % 3 == 1) listener.start({}, fire);
            else listener.start(middle, fire);
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
