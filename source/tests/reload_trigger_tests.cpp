#include "wardogs/reload_trigger.hpp"

#include <chrono>
#include <iostream>

namespace {
using namespace std::chrono_literals;
using wardogs::ReloadTrigger;
int checks{}, failures{};
void check(bool value, const char* message) {
    ++checks;
    if (!value) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
}

int main() {
    const auto zero = ReloadTrigger::Clock::time_point{};
    ReloadTrigger gate;
    check(!gate.update(true, true, false, zero), "stopped mode never requests input");
    gate.arm(true);
    check(gate.armed() && !gate.update(true, false, false, zero + 1s) &&
          !gate.update(true, false, false, zero + 2s), "R held while arming is not a fresh trigger");
    check(!gate.update(true, true, false, zero + 3s), "R DOWN starts no immediate capture");
    check(!gate.update(true, true, false, zero + 5s), "held R never repeats");
    check(!gate.update(true, false, false, zero + 6s) &&
          !gate.update(true, false, false, zero + 6499ms), "delay starts after release");
    check(gate.update(true, false, false, zero + 6500ms) &&
          !gate.update(true, false, false, zero + 8s), "one fresh press requests exactly one cycle");
    check(gate.armed(), "completed cycle leaves persistent mode armed");
    (void)gate.update(true, true, false, zero + 9s);
    (void)gate.update(true, false, false, zero + 10s);
    (void)gate.update(false, false, false, zero + 10100ms);
    check(!gate.update(true, false, false, zero + 12s) && gate.armed(),
          "focus/modifier loss discards a pending request while preserving arming");
    (void)gate.update(false, true, false, zero + 13s);
    check(!gate.update(true, true, false, zero + 14s) &&
          !gate.update(true, false, false, zero + 15s) &&
          !gate.update(true, false, false, zero + 16s), "R outside the game cannot leak through focus restoration");
    (void)gate.update(true, true, true, zero + 17s);
    (void)gate.update(true, false, true, zero + 18s);
    check(!gate.update(true, false, false, zero + 20s), "R during a busy cycle does not queue another run");
    (void)gate.update(true, true, false, zero + 21s);
    (void)gate.update(true, false, false, zero + 22s);
    check(gate.update(true, false, false, zero + 23s), "a new R after completion starts another cycle");
    (void)gate.update(true, true, false, zero + 24s);
    (void)gate.update(true, false, false, zero + 25s);
    gate.disarm();
    check(!gate.armed() && !gate.update(true, false, false, zero + 26s), "STOP cancels delayed work");
    std::cout << checks << " trigger checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
