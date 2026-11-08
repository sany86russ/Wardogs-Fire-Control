#pragma once

#include "wardogs/game_helpers_preferences.hpp"
#include "wardogs/ocr.hpp"

namespace wardogs {

// Normalized client coordinates survive a change of resolution. The runtime
// still freezes physical client geometry for the duration of an armed run.
ImageRect reload_search_rect(int client_width, int client_height,
                             const ReloadHelperPreferences& preferences);
ReloadHelperPreferences select_reload_region(ReloadHelperPreferences preferences,
    int client_width, int client_height, ImageRect selection);

}  // namespace wardogs
