#include "reload_tracking.hpp"

#include <iostream>

int main() {
    using namespace wardogs_ui;
    int checks{}, failures{};
    const auto check = [&](bool result, const char* name) {
        ++checks;
        if (!result) { ++failures; std::cerr << "FAIL: " << name << '\n'; }
    };
    ReloadRegionTracker tracker;
    const ReloadRegionKey key{123, 2560, 1440, {0, 0, 2560, 1440}};
    const wardogs::ImageRect strip{1100, 900, 1390, 1030};
    check(!tracker.lookup(key), "first reload starts with discovery");
    check(tracker.remember(key, strip), "confirmed physical strip is retained");
    for (int cycle = 0; cycle < 4; ++cycle) {
        const auto region = tracker.lookup(key);
        check(region && region->left == 1100 && region->top == 900 && region->right == 1390 &&
            region->bottom == 1030, "later reloads reuse position without caching directions");
    }
    auto changed = key;
    changed.window = 124;
    check(!tracker.lookup(changed), "a different window requires discovery");
    changed = key; changed.width = 3840; changed.height = 2160;
    check(!tracker.lookup(changed), "resolution change requires discovery");
    changed = key; changed.search = {1000, 800, 1500, 1100};
    check(!tracker.lookup(changed), "a different manual search does not reuse automatic calibration");
    check(tracker.lookup(key).has_value(), "a mismatched query does not corrupt valid calibration");
    for (const auto invalid : {wardogs::ImageRect{-1, 0, 10, 10}, {0, 0, 2561, 10},
                              {10, 10, 10, 20}, {0, 0, 10, 1441}}) {
        check(!tracker.remember(key, invalid) && !tracker.lookup(key), "invalid bounds clear cached position");
        (void)tracker.remember(key, strip);
    }
    changed = key; changed.window = 0;
    check(!tracker.remember(changed, strip), "unidentified window cannot be calibrated");
    changed = key; changed.search = {-1, 0, 2560, 1440};
    check(!tracker.remember(changed, strip), "search outside client cannot be calibrated");
    changed = key; changed.width = 0;
    check(!tracker.remember(changed, strip), "minimized client cannot be calibrated");
    (void)tracker.remember(key, strip);
    tracker.clear();
    check(!tracker.lookup(key), "recognition failure or settings reset can discard calibration");
    std::cout << checks << " reload tracking checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
