#include "wardogs/reload_region.hpp"

#include <array>
#include <iostream>
#include <stdexcept>

int main() {
    int failures{};
    const auto check = [&](bool value, const char* message) {
        if (!value) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
    };
    wardogs::ReloadHelperPreferences defaults;
    for (const auto size : {std::array{1280, 720}, std::array{1920, 1080},
                           std::array{2560, 1440}, std::array{3440, 1440},
                           std::array{3840, 2160}, std::array{7680, 2160}}) {
        const auto full = wardogs::reload_search_rect(size[0], size[1], defaults);
        check(full.left == 0 && full.top == 0 && full.right == size[0] && full.bottom == size[1],
              "automatic search covers the whole client regardless of aspect ratio");
    }
    const auto manual = wardogs::select_reload_region(defaults, 1920, 1080, {768, 432, 1152, 648});
    const auto scaled = wardogs::reload_search_rect(3840, 2160, manual);
    check(!manual.automatic_region && scaled.left == 1536 && scaled.top == 864 &&
          scaled.right == 2304 && scaled.bottom == 1296, "manual selection scales with client resolution");
    const auto edge = wardogs::select_reload_region(defaults, 1366, 768, {1, 1, 1365, 767});
    const auto restored = wardogs::reload_search_rect(1366, 768, edge);
    check(restored.left <= 1 && restored.top <= 1 && restored.right >= 1365 && restored.bottom >= 767,
          "normalization rounds outward and preserves all selected pixels");
    for (const auto bad : {wardogs::ImageRect{-1, 0, 30, 30}, {0, 0, 1921, 10}, {40, 20, 30, 40}}) {
        bool rejected{};
        try { (void)wardogs::select_reload_region(defaults, 1920, 1080, bad); }
        catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "selection outside game client or empty selection is rejected");
    }
    bool rejected{};
    try { (void)wardogs::reload_search_rect(0, 1080, defaults); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "minimized or invalid client size is rejected");
    return failures ? 1 : 0;
}
