#pragma once

#include "wardogs/pinned_placement.hpp"

#include <array>
#include <string>

namespace wardogs {

struct PinnedCardPreferences {
    static constexpr int minimum_opacity_percent = 35;
    static constexpr int maximum_opacity_percent = 100;

    bool locked{};
    int opacity_percent{maximum_opacity_percent};
    std::wstring unlock_hotkey{L"Ctrl+Alt+Q"};
    bool always_on_top{true};
    std::optional<PinnedCardPlacement> placement;
    // Mortar (0) and SPH-2 (1) content sizes, excluding transient status rows.
    std::array<std::optional<PinnedCardSize>, 2> mode_sizes;
};

}  // namespace wardogs
