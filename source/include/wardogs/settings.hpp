#pragma once

#include "wardogs/capture.hpp"
#include "wardogs/core.hpp"
#include "wardogs/ghost_reticle.hpp"
#include "wardogs/game_map.hpp"
#include "wardogs/language.hpp"
#include "wardogs/pinned_preferences.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace wardogs {

enum class OcrBackend { rapid, windows };

struct AppSettings {
    static constexpr int minimum_mouse_capture_delay_ms = 0;
    static constexpr int maximum_mouse_capture_delay_ms = 2000;
    static constexpr int current_quick_workflow_version = 1;

    UiLanguage language{UiLanguage::russian};
    bool check_updates_on_start{true};
    std::wstring region_hotkey{L"Alt+R"};
    std::wstring base_hotkey{L"Alt+X"};
    std::wstring target_hotkey{L"Alt+T"};
    std::wstring quick_target_hotkey{L"Alt+V"};
    std::wstring impact_hotkey{L"Alt+I"};
    std::wstring ghost_arc_hotkey{L"F4"};
    std::wstring exit_game_mode_hotkey{L"Alt+C"};
    // Versioned preferences distinguish the requested quick workflow from an
    // explicit standalone choice saved after its introduction.
    int quick_workflow_version{current_quick_workflow_version};
    bool quick_workflow_migrated{false};  // Transient; never written to the INI.
    bool game_integration_enabled{true};
    bool middle_mouse_enabled{true};
    int mouse_capture_delay_ms{250};
    OcrBackend backend{OcrBackend::rapid};
    // Manual capture remains an optional advanced fallback.
    bool automatic_chat_region{true};
    // A remembered choice must still be confirmed for a new game session.
    GameMap last_game_map{GameMap::unselected};
    std::wstring coordinate_pattern{default_ocr_coordinate_pattern};
    std::optional<CaptureRegion> capture_region;
    PinnedCardPreferences pinned_card;
    GhostReticlePreferences ghost_reticle;
};

std::filesystem::path settings_path();
// The optional legacy file is read only when the destination does not exist.
// Saving may copy its unknown keys to a new profile, but never writes to it.
AppSettings load_settings_from(
    const std::filesystem::path& path,
    const std::filesystem::path& legacy_path = {});
void save_settings_to(const std::filesystem::path& path,
                      const AppSettings& settings,
                      const std::filesystem::path& legacy_path = {});
AppSettings load_settings();
void save_settings(const AppSettings& settings);

}  // namespace wardogs
