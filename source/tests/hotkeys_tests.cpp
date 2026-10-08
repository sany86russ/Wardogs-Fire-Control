#include "wardogs/hotkeys.hpp"

#include <Windows.h>
#include <TlHelp32.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

// Inventory only this test's threads. No other application's windows, memory,
// names or input are inspected; WM_HOTKEY below stays on our own message queue.
std::vector<DWORD> own_threads() {
    std::vector<DWORD> result;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) throw std::runtime_error("thread snapshot");
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID == GetCurrentProcessId())
                result.push_back(entry.th32ThreadID);
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

template<typename Condition>
bool wait_for(Condition condition) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    return condition();
}

class OwnedReservation {
public:
    OwnedReservation(int id, const wardogs::Hotkey& hotkey) : id_(id) {
        if (!RegisterHotKey(nullptr, id_, hotkey.modifiers, hotkey.virtual_key))
            throw std::runtime_error("test reservation unavailable: " +
                                     std::to_string(GetLastError()));
        active_ = true;
    }
    ~OwnedReservation() { if (active_) UnregisterHotKey(nullptr, id_); }
    bool release() {
        if (!active_) return false;
        const bool released = UnregisterHotKey(nullptr, id_) != FALSE;
        if (released) active_ = false;
        return released;
    }
    OwnedReservation(const OwnedReservation&) = delete;
    OwnedReservation& operator=(const OwnedReservation&) = delete;

private:
    int id_;
    bool active_{};
};

}  // namespace

int main() {
    int failures = 0;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
    };
    const auto rejects = [&](auto operation, const char* message) {
        bool rejected = false;
        try { operation(); }
        catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, message);
    };
    for (const auto* malformed : {L"F8x", L"F-1", L"F99999999999999999999999",
                                 L"Ctrl++Q", L"+Alt+R", L"Alt+R+"})
        rejects([&] { wardogs::parse_hotkey(malformed); },
                "malformed input cannot silently reserve another key");
    const auto chord = wardogs::parse_hotkey(L" Control + Alt + q ");
    check(chord.virtual_key == L'Q' && chord.modifiers ==
              (MOD_CONTROL | MOD_ALT | MOD_NOREPEAT), "normal modifier aliases remain compatible");
    auto duplicate = chord;
    duplicate.modifiers &= ~MOD_NOREPEAT;
    const std::array aliases{chord, duplicate};
    rejects([&] { wardogs::validate_unique_hotkeys(aliases); },
            "different repeat flags do not hide the same global combination");
    const std::array windows_reserved{wardogs::parse_hotkey(L"Win+R")};
    const std::array debugger_reserved{wardogs::parse_hotkey(L"Alt+F12")};
    rejects([&] { wardogs::validate_global_hotkeys(windows_reserved); },
            "Windows-logo combinations stay with the OS");
    rejects([&] { wardogs::validate_global_hotkeys(debugger_reserved); },
            "F12 stays reserved for Windows debuggers");
    rejects([&] { wardogs::validate_global_hotkeys({}); }, "an empty registration set is rejected");

    wardogs::GlobalHotkeyListener listener;
    check(!listener.active(), "a new listener has no OS registrations");
    listener.stop();
    listener.stop();
    // Function-key combinations uncommon in normal apps reduce interference.
    // Never synthesize a keyboard/mouse event or touch another application's UI.
    const std::array hotkeys{wardogs::parse_hotkey(L"Ctrl+Alt+Shift+F23"),
                             wardogs::parse_hotkey(L"Ctrl+Alt+Shift+F24")};
    rejects([&] { listener.start(hotkeys, {}); }, "an absent callback cannot install registrations");
    MSG message{};
    PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    constexpr int test_registration_id = 0x2100;
    const auto& blocked = hotkeys[1];
    if (!RegisterHotKey(nullptr, test_registration_id, blocked.modifiers, blocked.virtual_key)) {
        check(false, "test combination must be available before conflict testing");
        return 1;
    }
    bool conflict_reported = false;
    try { listener.start(hotkeys, [](std::size_t) {}); }
    catch (const std::runtime_error& error) {
        const std::string text = error.what();
        conflict_reported = text.find("F24") != std::string::npos &&
                            text.find("занято") != std::string::npos;
    }
    check(conflict_reported && !listener.active(),
          "an externally reserved key fails clearly and leaves the listener inactive");
    const bool partial_set_released = RegisterHotKey(nullptr, test_registration_id + 1,
                                                    hotkeys[0].modifiers, hotkeys[0].virtual_key) != FALSE;
    check(partial_set_released, "registration failure rolls back earlier combinations");
    if (partial_set_released) UnregisterHotKey(nullptr, test_registration_id + 1);
    check(UnregisterHotKey(nullptr, test_registration_id) != FALSE,
          "the unrelated pre-existing registration remains owned by its original thread");

    try {
        const auto requested = wardogs::parse_hotkey(L"Alt+F22");
        OwnedReservation original_owner{test_registration_id, requested};
        const std::array requested_set{requested, wardogs::parse_hotkey(L"Ctrl+Alt+Shift+F21")};
        std::atomic<int> actual_callbacks{};
        std::atomic<std::size_t> actual_index{static_cast<std::size_t>(-1)};
        const auto before = own_threads();
        const auto actual = listener.start_with_conflict_fallback(
            requested_set, [&](std::size_t index) {
                actual_index.store(index);
                actual_callbacks.fetch_add(1);
            });
        const auto expected = wardogs::parse_hotkey(L"Ctrl+Alt+F22");
        check(listener.active() && actual.size() == 2 && actual[0] == expected &&
                  actual[1] == requested_set[1],
              "an occupied chord selects Ctrl first and preserves each conflict-free requested chord");
        DWORD worker = 0;
        for (const DWORD candidate : own_threads())
            if (std::find(before.begin(), before.end(), candidate) == before.end())
                worker = candidate;
        check(worker != 0, "fallback registration still uses its own message thread");
        if (worker) {
            PostThreadMessageW(worker, WM_HOTKEY, 0x4000,
                               MAKELPARAM(MOD_ALT, VK_F22));
            PostThreadMessageW(worker, WM_HOTKEY, 0x4000,
                               MAKELPARAM(MOD_ALT | MOD_SHIFT, VK_F22));
            // A second valid action is a FIFO barrier: observing it proves both
            // preceding invalid notifications have already been handled.
            PostThreadMessageW(worker, WM_HOTKEY, 0x4001,
                               MAKELPARAM(MOD_CONTROL | MOD_ALT | MOD_SHIFT, VK_F21));
            check(wait_for([&] { return actual_index.load() == 1; }) &&
                      actual_callbacks.load() == 1,
                  "an occupied original and unused alternative are rejected before a valid queue barrier");
            PostThreadMessageW(worker, WM_HOTKEY, 0x4000,
                               MAKELPARAM(MOD_CONTROL | MOD_ALT, VK_F22));
            check(wait_for([&] { return actual_callbacks.load() == 2; }) &&
                      actual_index.load() == 0,
                  "dispatch validates the actual chord and rejects the occupied original and unused alternative");
        }
        listener.stop();
        OwnedReservation reclaimed{test_registration_id + 1, expected};
        check(reclaimed.release(), "stopping fallback releases the actual selected chord");
        check(original_owner.release(), "fallback preserves the original registration owner");

        OwnedReservation occupied_first{test_registration_id, requested};
        const std::array requested_reserved{requested, expected};
        const auto actual_reserved = listener.start_with_conflict_fallback(
            requested_reserved, [](std::size_t) {});
        check(actual_reserved.size() == 2 &&
                  actual_reserved[0] == wardogs::parse_hotkey(L"Alt+Shift+F22") &&
                  actual_reserved[1] == expected,
              "fallback skips another action's requested chord before trying Shift");
        wardogs::validate_unique_hotkeys(actual_reserved);
        listener.stop();
        check(occupied_first.release(), "requested-chord exclusion preserves the external owner");

        const auto occupied_a = wardogs::parse_hotkey(L"Alt+F20");
        const auto occupied_b = wardogs::parse_hotkey(L"Alt+Shift+F20");
        OwnedReservation owner_a{test_registration_id, occupied_a};
        OwnedReservation owner_b{test_registration_id + 1, occupied_b};
        const std::array requested_colliding{occupied_a, occupied_b};
        const auto actual_colliding = listener.start_with_conflict_fallback(
            requested_colliding, [](std::size_t) {});
        check(actual_colliding.size() == 2 &&
                  actual_colliding[0] == wardogs::parse_hotkey(L"Ctrl+Alt+F20") &&
                  actual_colliding[1] == wardogs::parse_hotkey(L"Ctrl+Alt+Shift+F20"),
              "selected alternatives remain distinct without removing any requested modifiers");
        wardogs::validate_unique_hotkeys(actual_colliding);
        listener.stop();
        check(owner_a.release() && owner_b.release(),
              "multiple fallback actions preserve each original registration owner");

        OwnedReservation selected_a{test_registration_id, occupied_a};
        OwnedReservation selected_b{test_registration_id + 1, occupied_b};
        OwnedReservation selected_c{test_registration_id + 2,
                                    wardogs::parse_hotkey(L"Ctrl+Alt+F20")};
        bool selected_collision = false;
        try { (void)listener.start_with_conflict_fallback(requested_colliding, [](std::size_t) {}); }
        catch (const std::runtime_error& error) {
            selected_collision = std::string{error.what()}.find("F20") != std::string::npos;
        }
        check(selected_collision && !listener.active(),
              "an earlier chosen alternative cannot be reused by a later action");
        OwnedReservation selected_rollback{test_registration_id + 3,
                                           wardogs::parse_hotkey(L"Ctrl+Alt+Shift+F20")};
        check(selected_rollback.release(), "a colliding selected alternative is rolled back as part of the full set");
        check(selected_a.release() && selected_b.release() && selected_c.release(),
              "selected-alternative exhaustion preserves all external owners");

        // All additive modifiers are already present: no silent reassignment to
        // another key or removal of a modifier is permitted.
        OwnedReservation exhausted_owner{test_registration_id, hotkeys[1]};
        bool exhausted = false;
        try { (void)listener.start_with_conflict_fallback(hotkeys, [](std::size_t) {}); }
        catch (const std::runtime_error& error) {
            const std::string text = error.what();
            exhausted = text.find("F24") != std::string::npos &&
                        text.find("занято") != std::string::npos;
        }
        check(exhausted && !listener.active(),
              "an exhausted additive chord fails clearly without changing its key or removing modifiers");
        OwnedReservation rollback_probe{test_registration_id + 1, hotkeys[0]};
        check(rollback_probe.release(), "fallback exhaustion rolls back an earlier successful registration");
        check(exhausted_owner.release(), "exhaustion preserves the occupied chord's original owner");

        const std::array saturated{wardogs::parse_hotkey(L"Alt+F19"),
                                  wardogs::parse_hotkey(L"Ctrl+Alt+F19"),
                                  wardogs::parse_hotkey(L"Alt+Shift+F19"),
                                  wardogs::parse_hotkey(L"Ctrl+Alt+Shift+F19")};
        OwnedReservation saturated_a{test_registration_id, saturated[0]};
        OwnedReservation saturated_b{test_registration_id + 1, saturated[1]};
        OwnedReservation saturated_c{test_registration_id + 2, saturated[2]};
        OwnedReservation saturated_d{test_registration_id + 3, saturated[3]};
        const std::array saturated_request{wardogs::parse_hotkey(L"Ctrl+Alt+Shift+F18"), saturated[0]};
        bool saturation_reported = false;
        try { (void)listener.start_with_conflict_fallback(saturated_request, [](std::size_t) {}); }
        catch (const std::runtime_error& error) {
            saturation_reported = std::string{error.what()}.find("F19") != std::string::npos;
        }
        check(saturation_reported && !listener.active(),
              "three occupied alternatives terminate with the exact requested key error");
        OwnedReservation saturated_rollback{test_registration_id + 4, saturated_request[0]};
        check(saturated_rollback.release(), "saturation rolls back the whole partial set");
        check(saturated_a.release() && saturated_b.release() && saturated_c.release() && saturated_d.release(),
              "all occupied alternatives remain owned by their original thread");
    } catch (const std::exception& error) {
        ++failures;
        listener.stop();
        std::cerr << "FAIL: conflict fallback: " << error.what() << '\n';
    }

    std::atomic<int> callbacks{};
    std::atomic<DWORD> callback_thread{};
    std::atomic<std::size_t> action_index{static_cast<std::size_t>(-1)};
    const auto previous_threads = own_threads();
    try {
        listener.start(hotkeys, [&](std::size_t index) {
            callback_thread.store(GetCurrentThreadId());
            action_index.store(index);
            callbacks.fetch_add(1);
        });
        check(listener.active(), "the complete combination set becomes active");
        const auto current_threads = own_threads();
        DWORD worker = 0;
        for (const DWORD candidate : current_threads)
            if (std::find(previous_threads.begin(), previous_threads.end(), candidate) == previous_threads.end())
                worker = candidate;
        check(worker != 0, "registration uses a dedicated owned message thread");
        if (worker) {
            // Queue notifications only: these do not generate an OS input event,
            // move the cursor, or send a key to the user/game/another process.
            PostThreadMessageW(worker, WM_HOTKEY, 0x4001,
                               MAKELPARAM(MOD_ALT, VK_F24));
            PostThreadMessageW(worker, WM_HOTKEY, 0x5FFF,
                               MAKELPARAM(MOD_CONTROL | MOD_ALT | MOD_SHIFT, VK_F24));
            PostThreadMessageW(worker, WM_HOTKEY, 0x4001,
                               MAKELPARAM(MOD_CONTROL | MOD_ALT | MOD_SHIFT, VK_F24));
            check(wait_for([&] { return callbacks.load() == 1; }) && action_index.load() == 1 &&
                      callback_thread.load() == worker && worker != GetCurrentThreadId(),
                  "only the valid registered notification dispatches on its own thread");
        }
        const auto begin = std::chrono::steady_clock::now();
        listener.stop();
        check(!listener.active() && std::chrono::steady_clock::now() - begin < std::chrono::seconds{3},
              "idle registration shutdown is bounded");
        for (int attempt = 0; attempt < 12; ++attempt) {
            listener.start(hotkeys, [](std::size_t) {});
            check(listener.active(), "registered hotkeys can restart without leaked reservations");
            listener.stop();
        }
        const bool reservations_released = RegisterHotKey(nullptr, test_registration_id,
                                                          hotkeys[0].modifiers, hotkeys[0].virtual_key) != FALSE;
        check(reservations_released, "stop releases registrations to the OS");
        if (reservations_released) UnregisterHotKey(nullptr, test_registration_id);

        callbacks.store(0);
        std::atomic<bool> self_stopped{};
        const auto before_self_stop = own_threads();
        listener.start(hotkeys, [&](std::size_t) {
            callbacks.fetch_add(1);
            listener.stop();
            self_stopped.store(true, std::memory_order_release);
        });
        DWORD self_stop_worker = 0;
        for (const DWORD candidate : own_threads())
            if (std::find(before_self_stop.begin(), before_self_stop.end(), candidate) == before_self_stop.end())
                self_stop_worker = candidate;
        check(self_stop_worker != 0, "a restarted listener owns a message thread");
        if (self_stop_worker) {
            PostThreadMessageW(self_stop_worker, WM_HOTKEY, 0x4000,
                               MAKELPARAM(MOD_CONTROL | MOD_ALT | MOD_SHIFT, VK_F23));
            // Completion is observed through an OS reservation, never by
            // concurrently reading the listener's owner-thread implementation.
            bool reclaimed = false;
            const bool self_stop_released = wait_for([&] {
                if (!self_stopped.load(std::memory_order_acquire)) return false;
                reclaimed = RegisterHotKey(nullptr, test_registration_id,
                                            hotkeys[0].modifiers, hotkeys[0].virtual_key) != FALSE;
                return reclaimed;
            });
            if (!self_stop_released)
                std::cerr << "self-stop diagnostic callbacks=" << callbacks.load()
                          << " thread=" << self_stop_worker << " error=" << GetLastError() << '\n';
            check(self_stop_released, "a callback can stop its own listener without a self-join or leaked registration");
            if (reclaimed) UnregisterHotKey(nullptr, test_registration_id);
            check(!listener.active(), "a self-stopped listener stays inactive");
        }
    } catch (const std::exception& error) {
        ++failures;
        std::cerr << "FAIL: registration lifecycle: " << error.what() << '\n';
    }
    listener.stop();

    // Exercise the complete eight-action settings set on a real Windows
    // registration thread. These uncommon chords belong only to this test;
    // posted notifications never become keyboard input in the user/game.
    try {
        std::array<wardogs::Hotkey, 8> all_actions;
        for (std::size_t index = 0; index < all_actions.size(); ++index) {
            all_actions[index] = wardogs::parse_hotkey(
                L"Ctrl+Alt+Shift+F" + std::to_wstring(13 + index));
            check((all_actions[index].modifiers & MOD_NOREPEAT) != 0,
                  "each of the eight action chords requests Windows no-repeat registration");
        }
        wardogs::validate_global_hotkeys(all_actions);
        std::array<std::atomic<int>, 8> action_callbacks{};
        std::atomic<int> total_callbacks{};
        std::atomic<int> unexpected_callbacks{};
        std::atomic<DWORD> dispatch_thread{};
        // Destroy/join the worker before its referenced counters on exception.
        wardogs::GlobalHotkeyListener all_actions_listener;
        const auto before_all_actions = own_threads();
        all_actions_listener.start(all_actions, [&](std::size_t index) {
            dispatch_thread.store(GetCurrentThreadId());
            if (index < action_callbacks.size()) action_callbacks[index].fetch_add(1);
            else unexpected_callbacks.fetch_add(1);
            total_callbacks.fetch_add(1);
        });
        check(all_actions_listener.active(), "all eight action chords become active together");
        DWORD all_actions_worker = 0;
        for (const DWORD candidate : own_threads())
            if (std::find(before_all_actions.begin(), before_all_actions.end(), candidate) ==
                before_all_actions.end())
                all_actions_worker = candidate;
        check(all_actions_worker != 0,
              "the eight-action listener owns a dedicated message queue");
        for (std::size_t index = 0; index < all_actions.size(); ++index) {
            const auto& hotkey = all_actions[index];
            const int probe_id = test_registration_id + 16 + static_cast<int>(index);
            const bool duplicate_reserved = RegisterHotKey(
                nullptr, probe_id, hotkey.modifiers, hotkey.virtual_key) != FALSE;
            const DWORD reserve_error = duplicate_reserved ? ERROR_SUCCESS : GetLastError();
            if (duplicate_reserved) UnregisterHotKey(nullptr, probe_id);
            check(!duplicate_reserved && reserve_error == ERROR_HOTKEY_ALREADY_REGISTERED,
                  "each of the eight chords is actually reserved by Windows");
            if (!all_actions_worker) continue;
            const WPARAM action_id = 0x4000 + index;
            const UINT exact_modifiers = MOD_CONTROL | MOD_ALT | MOD_SHIFT;
            check(PostThreadMessageW(all_actions_worker, WM_HOTKEY, action_id,
                                     MAKELPARAM(MOD_CONTROL | MOD_ALT, hotkey.virtual_key)) != FALSE,
                  "the owned queue accepts the missing-modifier rejection probe");
            check(PostThreadMessageW(all_actions_worker, WM_HOTKEY, action_id,
                                     MAKELPARAM(exact_modifiers,
                                                all_actions[(index + 1) % all_actions.size()].virtual_key)) != FALSE,
                  "the owned queue accepts a cross-wired action/key rejection probe");
            check(PostThreadMessageW(all_actions_worker, WM_HOTKEY, 0x4000 + all_actions.size(),
                                     MAKELPARAM(exact_modifiers, hotkey.virtual_key)) != FALSE,
                  "the owned queue accepts the out-of-set action rejection probe");
            // The exact notification is a FIFO barrier for all preceding
            // invalid notifications; each index must dispatch exactly once.
            check(PostThreadMessageW(all_actions_worker, WM_HOTKEY, action_id,
                                     MAKELPARAM(exact_modifiers, hotkey.virtual_key)) != FALSE,
                  "the owned queue accepts each exact registered action chord");
            check(wait_for([&] { return total_callbacks.load() >= static_cast<int>(index + 1); }) &&
                      total_callbacks.load() == static_cast<int>(index + 1) &&
                      action_callbacks[index].load() == 1 &&
                      unexpected_callbacks.load() == 0 &&
                      dispatch_thread.load() == all_actions_worker &&
                      all_actions_worker != GetCurrentThreadId(),
                  "each of eight actions dispatches once for its exact chord on its own thread");
        }
        all_actions_listener.stop();
        check(!all_actions_listener.active() && total_callbacks.load() == 8,
              "stopping the complete action set leaves exactly eight valid dispatches");
        for (std::size_t index = 0; index < all_actions.size(); ++index) {
            check(action_callbacks[index].load() == 1,
                  "all eight action indices are retained without aliasing or extra callbacks");
            OwnedReservation released_action{
                test_registration_id + 16 + static_cast<int>(index), all_actions[index]};
            check(released_action.release(),
                  "stop releases every chord in the eight-action set to Windows");
        }
    } catch (const std::exception& error) {
        ++failures;
        listener.stop();
        std::cerr << "FAIL: eight-action registration and dispatch: " << error.what() << '\n';
    }
    listener.stop();
    if (failures) return 1;
    std::cout << "All hotkey parser, registration, strict conflict, additive fallback, dispatch and lifecycle tests passed\n";
    return 0;
}
