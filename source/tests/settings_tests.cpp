#include "wardogs/settings.hpp"
#include "wardogs/hotkeys.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <cmath>
#include <array>
#include <iterator>
#include <clocale>
#include <limits>
#include <algorithm>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

std::string bytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

std::string utf16_bytes(std::wstring_view text) {
    return {reinterpret_cast<const char*>(text.data()), text.size() * sizeof(wchar_t)};
}

std::wstring ini_value(const std::filesystem::path& path, const wchar_t* section,
                       const wchar_t* key) {
    std::array<wchar_t, 1024> result{};
    GetPrivateProfileStringW(section, key, L"", result.data(),
                             static_cast<DWORD>(result.size()), path.c_str());
    return result.data();
}

}  // namespace

int run_settings_tests() {
    namespace fs = std::filesystem;
    using wardogs::AppSettings;
    using wardogs::CaptureRegion;

    const fs::path directory = fs::temp_directory_path() /
        (L"wardogs-fire-control-settings-test-" +
         std::to_wstring(GetCurrentProcessId()));
    const fs::path path = directory / L"settings.ini";
    fs::remove_all(directory);

    AppSettings saved;
    check(saved.language == wardogs::UiLanguage::russian,
          "the default interface language is Russian");
    check(saved.last_game_map == wardogs::GameMap::unselected,
          "a new profile cannot imply a current game map");
    check(saved.region_hotkey == L"Alt+R" && saved.base_hotkey == L"Alt+X" &&
              saved.target_hotkey == L"Alt+T" &&
              saved.quick_target_hotkey == L"Alt+V" &&
              saved.impact_hotkey == L"Alt+I" &&
              saved.exit_game_mode_hotkey == L"Alt+C",
          "fresh shortcuts support Alt+X base capture with a distinct Alt+C return action");
    check(saved.game_integration_enabled && saved.middle_mouse_enabled && saved.automatic_chat_region &&
              saved.mouse_capture_delay_ms == 250 && saved.quick_workflow_version == 1 &&
              !saved.quick_workflow_migrated,
          "fresh profiles are ready for the requested automatic workflow without setup");
    const auto fresh = wardogs::load_settings_from(path);
    check(fresh.game_integration_enabled && fresh.middle_mouse_enabled && fresh.automatic_chat_region &&
              fresh.language == wardogs::UiLanguage::russian &&
              !fresh.quick_workflow_migrated && !fs::exists(path),
          "loading a missing profile prepares the quick workflow without writing a file");
    check(saved.ghost_arc_hotkey == L"F4",
          "the ghost trajectory shortcut defaults to the unused F4 key");
    check(saved.ghost_reticle.bearing_compensation_deg == 0.0,
          "ghost bearing compensation defaults to zero degrees");
    saved.region_hotkey = L"Ctrl+F8";
    saved.language = wardogs::UiLanguage::english;
    saved.last_game_map = wardogs::GameMap::ozeti;
    saved.impact_hotkey = L"Ctrl+Shift+F11";
    saved.game_integration_enabled = true;
    saved.ghost_arc_hotkey = L"Ctrl+Shift+G";
    saved.exit_game_mode_hotkey = L"ctrl+shift+x";
    saved.middle_mouse_enabled = false;
    saved.mouse_capture_delay_ms = 420;
    saved.ghost_reticle.opacity_percent = 72;
    saved.ghost_reticle.width = 1120;
    saved.ghost_reticle.bearing_compensation_deg = -0.35;
    saved.ghost_reticle.preferred_arc = wardogs::Arc::high;
    saved.pinned_card.locked = true;
    saved.pinned_card.opacity_percent = 63;
    saved.pinned_card.unlock_hotkey = L"Ctrl+Shift+U";
    saved.capture_region = CaptureRegion{L"\\\\.\\DISPLAY2", {13, 27, 413, 81}};
    saved.capture_region->monitor_size = {2560, 1440};
    saved.automatic_chat_region = false;
    wardogs::save_settings_to(path, saved);

    const AppSettings loaded = wardogs::load_settings_from(path);
    check(loaded.language == wardogs::UiLanguage::english &&
              ini_value(path, L"settings", L"ui_language") == L"en",
          "English survives the profile round trip with a stable INI identifier");
    const fs::path language_path = directory / L"language.ini";
    wardogs::save_settings_to(language_path, loaded);
    WritePrivateProfileStringW(L"extension", L"keep_language", L"unchanged", language_path.c_str());
    for (const wchar_t* identifier : {L"unexpected", L"", L"EN", L"ru"}) {
        WritePrivateProfileStringW(L"settings", L"ui_language", identifier, language_path.c_str());
        const auto original_bytes = bytes(language_path);
        const auto russian = wardogs::load_settings_from(language_path);
        check(russian.language == wardogs::UiLanguage::russian &&
                  bytes(language_path) == original_bytes,
              "unknown, empty or Russian language identifiers load Russian without changing the profile");
    }
    WritePrivateProfileStringW(L"settings", L"ui_language", nullptr, language_path.c_str());
    auto russian = wardogs::load_settings_from(language_path);
    check(russian.language == wardogs::UiLanguage::russian,
          "a legacy profile without a language key uses Russian");
    wardogs::save_settings_to(language_path, russian);
    const auto retained_language = wardogs::load_settings_from(language_path);
    check(retained_language.language == wardogs::UiLanguage::russian &&
              ini_value(language_path, L"settings", L"ui_language") == L"ru" &&
              ini_value(language_path, L"extension", L"keep_language") == L"unchanged" &&
              retained_language.region_hotkey == loaded.region_hotkey &&
              retained_language.capture_region && loaded.capture_region &&
              retained_language.capture_region->relative.left ==
                  loaded.capture_region->relative.left,
          "Russian persists while custom shortcuts, capture geometry and unrelated keys are retained");
    const fs::path legacy_language_path = directory / L"legacy-language.ini";
    const fs::path migrated_language_path = directory / L"migrated-language.ini";
    wardogs::save_settings_to(legacy_language_path, loaded);
    WritePrivateProfileStringW(L"extension", L"keep_language", L"legacy", legacy_language_path.c_str());
    const auto legacy_language_bytes = bytes(legacy_language_path);
    const auto migrated_language = wardogs::load_settings_from(migrated_language_path, legacy_language_path);
    check(migrated_language.language == wardogs::UiLanguage::english &&
              !fs::exists(migrated_language_path) && bytes(legacy_language_path) == legacy_language_bytes,
          "an existing legacy language choice loads without changing either profile");
    wardogs::save_settings_to(migrated_language_path, migrated_language, legacy_language_path);
    check(wardogs::load_settings_from(migrated_language_path).language == wardogs::UiLanguage::english &&
              ini_value(migrated_language_path, L"extension", L"keep_language") == L"legacy" &&
              bytes(legacy_language_path) == legacy_language_bytes,
          "language migration preserves unrelated values and never writes to the legacy profile");
    check(loaded.last_game_map == wardogs::GameMap::ozeti,
          "the last explicit game map survives the profile round trip");
    for (const auto map : {wardogs::GameMap::unselected, wardogs::GameMap::bakurani,
                           wardogs::GameMap::ozeti, wardogs::GameMap::zestafona,
                           wardogs::GameMap::training, wardogs::GameMap::other}) {
        check(wardogs::game_map_from_key(wardogs::game_map_key(map)) == map,
              "all supported map choices have stable settings identifiers");
    }
    check(wardogs::game_map_from_key(L"unexpected-map") == wardogs::GameMap::unselected &&
              !wardogs::game_map_has_terrain(wardogs::GameMap::training),
          "unknown map identifiers never select another map's terrain");
    check(!loaded.automatic_chat_region && loaded.capture_region &&
              loaded.capture_region->monitor_size.cx == 2560 &&
              loaded.capture_region->monitor_size.cy == 1440,
          "custom search mode and reference physical monitor dimensions persist");
    auto automatic = saved;
    automatic.automatic_chat_region = true;
    wardogs::save_settings_to(directory / L"automatic.ini", automatic);
    check(wardogs::load_settings_from(directory / L"automatic.ini").automatic_chat_region,
          "automatic search can retain a custom region without selecting it");
    check(loaded.region_hotkey == saved.region_hotkey,
          "ordinary settings survive the explicit-path round trip");
    check(loaded.game_integration_enabled && loaded.quick_workflow_version == 1 && !loaded.quick_workflow_migrated,
          "the workflow version and explicit interaction preference survive the round trip");
    check(loaded.impact_hotkey == saved.impact_hotkey,
          "the impact OCR shortcut survives the explicit-path round trip");
    check(loaded.ghost_arc_hotkey == saved.ghost_arc_hotkey,
          "the ghost trajectory shortcut survives the explicit-path round trip");
    check(loaded.exit_game_mode_hotkey == L"Ctrl+Shift+X" &&
              !loaded.middle_mouse_enabled && loaded.mouse_capture_delay_ms == 420,
          "middle-click settings persist and the exit shortcut is normalized");
    check(loaded.ghost_reticle.opacity_percent == 72 &&
              loaded.ghost_reticle.width == 1120 &&
              std::abs(loaded.ghost_reticle.bearing_compensation_deg + 0.35) <
                  1e-9 &&
              loaded.ghost_reticle.preferred_arc == wardogs::Arc::high,
          "ghost reticle size, opacity, bearing offset, and trajectory persist");
    check(loaded.pinned_card.locked,
          "the pinned card lock state survives the explicit-path round trip");
    check(loaded.pinned_card.opacity_percent == 63,
          "the pinned card opacity survives the explicit-path round trip");
    check(loaded.pinned_card.unlock_hotkey == L"Ctrl+Shift+U",
          "the pinned-card unlock hotkey survives the explicit-path round trip");
    check(loaded.capture_region.has_value(),
          "a configured OCR capture region is restored");
    if (loaded.capture_region) {
        check(loaded.capture_region->monitor_device == saved.capture_region->monitor_device,
              "the persisted region keeps its monitor device");
        check(loaded.capture_region->relative.left == 13 &&
                  loaded.capture_region->relative.top == 27 &&
                  loaded.capture_region->relative.right == 413 &&
                  loaded.capture_region->relative.bottom == 81,
              "the persisted region keeps its monitor-relative rectangle");
    }
    try {
        check(wardogs::parse_ocr_coordinate(L"x99.67, y11.06",
                                             loaded.coordinate_pattern) ==
                  wardogs::Point{99.67, 11.06},
              "a persisted default regex parses an ordinary coordinate pair");
    } catch (const std::invalid_argument&) {
        check(false, "a persisted default regex remains usable after INI round trip");
    }

    // Windows conflict recovery returns the actual complete registration set.
    // Persist those assignments without changing unrelated actions or preferences.
    const fs::path recovered_path = directory / L"recovered-hotkeys.ini";
    auto recovered = saved;
    recovered.region_hotkey = L"Ctrl+Alt+R";
    recovered.pinned_card.unlock_hotkey = L"Ctrl+Alt+F10";
    recovered.exit_game_mode_hotkey = L"Ctrl+Alt+F11";
    wardogs::save_settings_to(recovered_path, recovered);
    check(ini_value(recovered_path, L"settings", L"region_hotkey") == L"Ctrl+Alt+R" &&
              ini_value(recovered_path, L"settings", L"pinned_card_unlock_hotkey") == L"Ctrl+Alt+F10" &&
              ini_value(recovered_path, L"settings", L"exit_game_mode_hotkey") == L"Ctrl+Alt+F11",
          "recovered region, unlock and exit shortcuts serialize to their own fields");
    const auto recovered_loaded = wardogs::load_settings_from(recovered_path);
    check(recovered_loaded.region_hotkey == recovered.region_hotkey &&
              recovered_loaded.pinned_card.unlock_hotkey == recovered.pinned_card.unlock_hotkey &&
              recovered_loaded.exit_game_mode_hotkey == recovered.exit_game_mode_hotkey &&
              recovered_loaded.base_hotkey == saved.base_hotkey &&
              recovered_loaded.target_hotkey == saved.target_hotkey &&
              recovered_loaded.game_integration_enabled == saved.game_integration_enabled &&
              recovered_loaded.middle_mouse_enabled == saved.middle_mouse_enabled &&
              recovered_loaded.mouse_capture_delay_ms == saved.mouse_capture_delay_ms,
          "recovered shortcuts round-trip without changing base, target or interaction preferences");

    // Profiles move between Windows language settings. Floating settings must
    // keep their decimal separator and precision independently of LC_NUMERIC.
    const std::string original_numeric_locale = std::setlocale(LC_NUMERIC, nullptr);
    struct RestoreNumericLocale {
        std::string original;
        ~RestoreNumericLocale() { std::setlocale(LC_NUMERIC, original.c_str()); }
    } restore_numeric_locale{original_numeric_locale};
    const fs::path numeric_path = directory / L"numeric.ini";
    auto numeric = saved;
    numeric.ghost_reticle.bearing_compensation_deg = -0.35;
    std::setlocale(LC_NUMERIC, "C");
    wardogs::save_settings_to(numeric_path, numeric);
    const bool comma_locale = std::setlocale(LC_NUMERIC, "Russian_Russia.1251") != nullptr ||
        std::setlocale(LC_NUMERIC, "ru-RU") != nullptr;
    check(comma_locale, "a comma-decimal locale is available for the settings regression");
    if (comma_locale) {
        check(wardogs::load_settings_from(numeric_path).ghost_reticle.bearing_compensation_deg == -0.35,
              "a dot-decimal profile loads unchanged under a comma-decimal locale");
        numeric.ghost_reticle.bearing_compensation_deg = 0.123456789012345;
        wardogs::save_settings_to(numeric_path, numeric);
        const auto serialized = ini_value(numeric_path, L"settings", L"ghost_reticle_bearing_compensation_deg");
        check(serialized.find(L'.') != std::wstring::npos && serialized.find(L',') == std::wstring::npos,
              "floating settings always serialize an invariant decimal dot");
        std::setlocale(LC_NUMERIC, "C");
        check(wardogs::load_settings_from(numeric_path).ghost_reticle.bearing_compensation_deg ==
                  numeric.ghost_reticle.bearing_compensation_deg,
              "a floating setting round-trips across locales without six-digit truncation");
    }
    std::setlocale(LC_NUMERIC, "C");
    WritePrivateProfileStringW(L"settings", L"ghost_reticle_bearing_compensation_deg", L"-0,350000", numeric_path.c_str());
    check(wardogs::load_settings_from(numeric_path).ghost_reticle.bearing_compensation_deg == -0.35,
          "a legacy comma-decimal setting is accepted without depending on the current locale");
    for (const wchar_t* invalid : {L"1,2.3", L"1,2,3", L"1.5x", L"nan", L"inf", L"１２.５"}) {
        WritePrivateProfileStringW(L"settings", L"ghost_reticle_bearing_compensation_deg", invalid, numeric_path.c_str());
        check(wardogs::load_settings_from(numeric_path).ghost_reticle.bearing_compensation_deg == 0.0,
              "mixed separators, trailing text and non-finite or Unicode floating input are rejected");
    }
    WritePrivateProfileStringW(L"settings", L"ghost_reticle_bearing_compensation_deg", L"+1.25e-1", numeric_path.c_str());
    check(wardogs::load_settings_from(numeric_path).ghost_reticle.bearing_compensation_deg == 0.125,
          "legacy finite signed exponent notation remains supported");
    const auto numeric_before_failure = bytes(numeric_path);
    numeric.ghost_reticle.bearing_compensation_deg = std::numeric_limits<double>::quiet_NaN();
    bool nonfinite_rejected = false;
    try { wardogs::save_settings_to(numeric_path, numeric); }
    catch (const std::invalid_argument&) { nonfinite_rejected = true; }
    check(nonfinite_rejected && bytes(numeric_path) == numeric_before_failure,
          "saving NaN reports failure and preserves the previous complete profile");
    std::setlocale(LC_NUMERIC, original_numeric_locale.c_str());

    {
        std::wofstream invalid(path, std::ios::trunc);
        invalid << L"[settings]\n"
                   L"capture_monitor=\\\\.\\DISPLAY2\n"
                   L"capture_left=50\n"
                   L"capture_top=40\n"
                   L"capture_right=20\n"
                   L"capture_bottom=10\n";
    }
    check(!wardogs::load_settings_from(path).capture_region,
          "an empty or inverted persisted rectangle is ignored");

    {
        std::wofstream invalid_opacity(path, std::ios::trunc);
        invalid_opacity << L"[settings]\n"
                           L"pinned_card_opacity_percent=2\n";
    }
    check(wardogs::load_settings_from(path).pinned_card.opacity_percent == 35,
          "persisted card opacity is clamped to the visible minimum");

    {
        std::wofstream invalid_ghost(path, std::ios::trunc);
        invalid_ghost << L"[settings]\n"
                         L"ghost_reticle_opacity_percent=1\n"
                         L"ghost_reticle_width=12\n";
    }
    const auto clamped_ghost = wardogs::load_settings_from(path).ghost_reticle;
    check(clamped_ghost.opacity_percent == 20 && clamped_ghost.width == 480,
          "invalid ghost opacity and size are clamped to usable minimums");

    saved.capture_region.reset();
    wardogs::save_settings_to(path, saved);
    check(!wardogs::load_settings_from(path).capture_region,
          "saving an empty region removes an earlier persisted region");

    // Imports never rewrite the old profile. Unknown native settings and other
    // sections must survive the first save of the independent new profile.
    const fs::path legacy = directory / L"legacy.ini";
    const fs::path migrated_path = directory / L"new-profile.ini";
    {
        std::ofstream file(legacy, std::ios::binary);
        file << "; keep legacy comment\n[settings]\nbase_hotkey=Ctrl+F9\nexit_game_mode_hotkey= alt+x \n"
                "region_hotkey=Ctrl+F8\ntarget_hotkey=Alt+Y\ngame_integration_enabled=0\n"
                "middle_mouse_enabled=0\nautomatic_chat_region=0\n"
                "capture_monitor=\\\\.\\DISPLAY2\ncapture_left=13\ncapture_top=27\n"
                "capture_right=413\ncapture_bottom=81\n"
                "coordinate_pattern=A\\s+([\\d.]+)\\s+B\\s+([\\d.]+)\n"
                "overlay_duration_seconds=11\nmouse_capture_delay_ms=350\n"
                "[extension]\nkeep_this=external-value\n";
    }
    WritePrivateProfileStringW(L"extension", L"legacy_unicode", L"Старые данные", legacy.c_str());
    const auto legacy_unicode = ini_value(legacy, L"extension", L"legacy_unicode");
    const auto legacy_bytes = bytes(legacy);
    const auto migrated = wardogs::load_settings_from(migrated_path, legacy);
    check(migrated.language == wardogs::UiLanguage::russian &&
              migrated.base_hotkey == L"Alt+X" && migrated.exit_game_mode_hotkey == L"Alt+C" &&
              migrated.mouse_capture_delay_ms == 350,
          "an older profile adopts the newly requested base and return shortcuts once");
    check(migrated.game_integration_enabled && migrated.middle_mouse_enabled && migrated.automatic_chat_region &&
              migrated.quick_workflow_version == 1 && migrated.quick_workflow_migrated &&
              migrated.region_hotkey == L"Ctrl+F8" && migrated.target_hotkey == L"Alt+Y" &&
              migrated.capture_region && migrated.capture_region->relative.left == 13 &&
              migrated.capture_region->relative.right == 413 &&
              migrated.coordinate_pattern == LR"(A\s+([\d.]+)\s+B\s+([\d.]+))",
          "legacy migration adopts automation while preserving other shortcuts and a safe custom pattern");
    check(!fs::exists(migrated_path) && bytes(legacy) == legacy_bytes,
          "loading a legacy profile is strictly read-only");
    auto failed_migration = migrated;
    failed_migration.exit_game_mode_hotkey = L"unsupported";
    bool migration_rejected = false;
    try { wardogs::save_settings_to(migrated_path, failed_migration, legacy); }
    catch (const std::exception&) { migration_rejected = true; }
    check(migration_rejected && !fs::exists(migrated_path) && bytes(legacy) == legacy_bytes,
          "failure on the first migrated save leaves the legacy bytes intact and creates no partial profile");
    wardogs::save_settings_to(migrated_path, migrated, legacy);
    check(bytes(legacy) == legacy_bytes,
          "saving a migrated profile never modifies the legacy INI");
    check(ini_value(migrated_path, L"settings", L"overlay_duration_seconds") == L"11" &&
              ini_value(migrated_path, L"extension", L"keep_this") == L"external-value" &&
              ini_value(migrated_path, L"extension", L"legacy_unicode") == legacy_unicode &&
              bytes(migrated_path).find(utf16_bytes(L"; keep legacy comment")) != std::string::npos,
          "unknown native fields and unrelated sections survive migration");
    check(wardogs::load_settings_from(migrated_path).coordinate_pattern == migrated.coordinate_pattern &&
              bytes(migrated_path).starts_with("\xff\xfe") &&
              ini_value(migrated_path, L"settings", L"quick_workflow_version") == L"1" &&
              !wardogs::load_settings_from(migrated_path).quick_workflow_migrated,
          "a migrated Unicode profile retains its pattern and does not migrate again after save");
    const fs::path standalone_path = directory / L"standalone.ini";
    auto standalone = migrated;
    standalone.game_integration_enabled = false;
    standalone.middle_mouse_enabled = false;
    standalone.automatic_chat_region = false;
    standalone.base_hotkey = L"Ctrl+F20";
    standalone.exit_game_mode_hotkey = L"Ctrl+F21";
    wardogs::save_settings_to(standalone_path, standalone);
    const auto standalone_loaded = wardogs::load_settings_from(standalone_path);
    check(!standalone_loaded.game_integration_enabled && !standalone_loaded.middle_mouse_enabled &&
              !standalone_loaded.automatic_chat_region && !standalone_loaded.quick_workflow_migrated &&
              standalone_loaded.base_hotkey == standalone.base_hotkey &&
              standalone_loaded.exit_game_mode_hotkey == standalone.exit_game_mode_hotkey,
          "a versioned explicit standalone preference and custom shortcuts persist without forced automation");
    WritePrivateProfileStringW(L"settings", L"quick_workflow_version", L"2", standalone_path.c_str());
    const auto future = wardogs::load_settings_from(standalone_path);
    wardogs::save_settings_to(standalone_path, future);
    check(!future.game_integration_enabled && !future.quick_workflow_migrated && future.quick_workflow_version == 2 &&
              ini_value(standalone_path, L"settings", L"quick_workflow_version") == L"2",
          "future workflow markers retain explicit preferences and are not downgraded during save");

    const fs::path shortcut_migration_path = directory / L"shortcut-migration.ini";
    {
        std::ofstream file(shortcut_migration_path, std::ios::binary);
        file << "[settings]\nbase_hotkey=Ctrl+F9\nexit_game_mode_hotkey=Ctrl+F10\n"
                "target_hotkey=alt+x\nquick_target_hotkey=ctrl+alt+x\n"
                "region_hotkey=alt+c\nghost_arc_hotkey=ctrl+alt+c\n";
    }
    const auto shortcut_migration_bytes = bytes(shortcut_migration_path);
    const auto assigned = wardogs::load_settings_from(shortcut_migration_path);
    check(assigned.quick_workflow_migrated && assigned.base_hotkey == L"Alt+Shift+X" &&
              assigned.exit_game_mode_hotkey == L"Alt+Shift+C" && assigned.target_hotkey == L"alt+x" &&
              assigned.quick_target_hotkey == L"ctrl+alt+x" && assigned.region_hotkey == L"alt+c" &&
              assigned.ghost_arc_hotkey == L"ctrl+alt+c" && bytes(shortcut_migration_path) == shortcut_migration_bytes,
          "generated migration shortcuts avoid semantic duplicates without changing preserved user actions");
    const std::array assigned_keys{
        wardogs::parse_hotkey(assigned.region_hotkey), wardogs::parse_hotkey(assigned.base_hotkey),
        wardogs::parse_hotkey(assigned.target_hotkey), wardogs::parse_hotkey(assigned.quick_target_hotkey),
        wardogs::parse_hotkey(assigned.impact_hotkey), wardogs::parse_hotkey(assigned.ghost_arc_hotkey),
        wardogs::parse_hotkey(assigned.pinned_card.unlock_hotkey), wardogs::parse_hotkey(assigned.exit_game_mode_hotkey)};
    wardogs::validate_global_hotkeys(assigned_keys);
    {
        std::ofstream file(shortcut_migration_path, std::ios::binary | std::ios::trunc);
        file << "[settings]\nbase_hotkey=Ctrl+F9\nexit_game_mode_hotkey=Ctrl+F10\n"
                "target_hotkey=Alt+X\nquick_target_hotkey=Ctrl+Alt+X\n"
                "impact_hotkey=Alt+Shift+X\nghost_arc_hotkey=Ctrl+Alt+Shift+X\n";
    }
    const auto exhausted_migration_bytes = bytes(shortcut_migration_path);
    bool exhausted_migration_rejected = false;
    try { (void)wardogs::load_settings_from(shortcut_migration_path); }
    catch (const std::invalid_argument&) { exhausted_migration_rejected = true; }
    check(exhausted_migration_rejected && bytes(shortcut_migration_path) == exhausted_migration_bytes,
          "exhausting all generated migration alternatives reports failure without changing the profile");
    auto changed = migrated;
    changed.mouse_capture_delay_ms = 650;
    wardogs::save_settings_to(migrated_path, changed, legacy);
    check(wardogs::load_settings_from(migrated_path, legacy).mouse_capture_delay_ms == 650,
          "an existing new profile takes precedence over legacy data");
    check(ini_value(migrated_path, L"extension", L"keep_this") == L"external-value",
          "subsequent atomic writes preserve unrelated keys");

    // Copying an ANSI profile must not overwrite the temporary UTF-16 BOM:
    // Win32 otherwise best-fit maps the default's full-width punctuation.
    const fs::path ansi_path = directory / L"ansi-profile.ini";
    {
        std::ofstream file(ansi_path, std::ios::binary);
        file << "; retain this comment\r\n[settings]\r\noverlay_duration_seconds=13\r\n"
                "[extension]\r\nkeep_this=ascii-value\r\n";
    }
    check(WritePrivateProfileStringW(L"extension", L"ansi_text", L"Старое значение", ansi_path.c_str()) != FALSE,
          "the ANSI extension fixture uses the same active code page as Windows INI");
    const auto ansi_extension = ini_value(ansi_path, L"extension", L"ansi_text");
    wardogs::save_settings_to(ansi_path, AppSettings{});
    check(bytes(ansi_path).starts_with("\xff\xfe") &&
              wardogs::load_settings_from(ansi_path).coordinate_pattern == wardogs::default_ocr_coordinate_pattern &&
              ini_value(ansi_path, L"settings", L"overlay_duration_seconds") == L"13" &&
              ini_value(ansi_path, L"extension", L"keep_this") == L"ascii-value" &&
              ini_value(ansi_path, L"extension", L"ansi_text") == ansi_extension &&
              bytes(ansi_path).find(utf16_bytes(L"; retain this comment")) != std::string::npos,
          "an existing ANSI destination becomes Unicode without losing unknown fields or comments");
    const std::wstring unicode_extension = L"Сохранено，без потерь";
    check(WritePrivateProfileStringW(L"extension", L"unicode", unicode_extension.c_str(), ansi_path.c_str()) != FALSE,
          "the Unicode extension fixture is written");
    auto unicode_saved = wardogs::load_settings_from(ansi_path);
    unicode_saved.mouse_capture_delay_ms = 780;
    wardogs::save_settings_to(ansi_path, unicode_saved);
    check(ini_value(ansi_path, L"extension", L"unicode") == unicode_extension &&
              bytes(ansi_path).find(utf16_bytes(L"; retain this comment")) != std::string::npos &&
              wardogs::load_settings_from(ansi_path).coordinate_pattern == wardogs::default_ocr_coordinate_pattern,
          "subsequent writes preserve Unicode extension values and comments exactly");

    auto legacy_default = std::wstring{wardogs::default_ocr_coordinate_pattern};
    std::replace(legacy_default.begin(), legacy_default.end(), L'，', L',');
    std::replace(legacy_default.begin(), legacy_default.end(), L'；', L';');
    WritePrivateProfileStringW(L"settings", L"coordinate_pattern", legacy_default.c_str(), ansi_path.c_str());
    check(wardogs::load_settings_from(ansi_path).coordinate_pattern == wardogs::default_ocr_coordinate_pattern,
          "the exact ANSI-damaged historical default is restored on load");
    const std::wstring custom_pattern = LR"(x=([0-9]+),y=([0-9]+))";
    WritePrivateProfileStringW(L"settings", L"coordinate_pattern", custom_pattern.c_str(), ansi_path.c_str());
    auto custom_saved = wardogs::load_settings_from(ansi_path);
    wardogs::save_settings_to(ansi_path, custom_saved);
    check(custom_saved.coordinate_pattern == custom_pattern &&
              wardogs::load_settings_from(ansi_path).coordinate_pattern == custom_pattern,
          "a genuine custom pattern is preserved by Unicode migration and normalization");

    const std::array unsupported_encodings{
        std::string{"\xef\xbb\xbf[settings]\r\nbase_hotkey=Alt+C\r\n"},
        std::string{"\xfe\xff[settings]\r\n"},
        std::string{"\xff\xfe[", 3},
        std::string{"[settings]\r\nx="} + '\0' + "tail"};
    for (std::size_t index = 0; index < unsupported_encodings.size(); ++index) {
        const fs::path unsupported_path = directory / (L"unsupported-encoding-" + std::to_wstring(index) + L".ini");
        {
            std::ofstream file(unsupported_path, std::ios::binary);
            file << unsupported_encodings[index];
        }
        bool unsupported_rejected = false;
        try { wardogs::save_settings_to(unsupported_path, AppSettings{}); }
        catch (const std::exception&) { unsupported_rejected = true; }
        check(unsupported_rejected && bytes(unsupported_path) == unsupported_encodings[index],
              "an unsupported BOM, partial UTF-16 unit or ANSI NUL fails without changing the profile");
    }

    const fs::path oversized_path = directory / L"oversized-profile.ini";
    {
        std::ofstream file(oversized_path, std::ios::binary);
        file << "[settings]\r\n";
        file.seekp(8 * 1024 * 1024);
        file.put('\n');
    }
    const auto oversized_bytes = bytes(oversized_path);
    bool oversized_rejected = false;
    try { wardogs::save_settings_to(oversized_path, AppSettings{}); }
    catch (const std::exception&) { oversized_rejected = true; }
    check(oversized_rejected && bytes(oversized_path) == oversized_bytes,
          "an oversized profile fails before conversion and preserves every original byte");

    for (const std::size_t ansi_size : {5 * 1024 * 1024, 4 * 1024 * 1024 - 1}) {
        const fs::path expanding_path = directory / (L"expanding-ansi-" + std::to_wstring(ansi_size) + L".ini");
        const std::string expanding_bytes(ansi_size, ';');
        {
            std::ofstream file(expanding_path, std::ios::binary);
            file << expanding_bytes;
        }
        bool expansion_rejected = false;
        try { wardogs::save_settings_to(expanding_path, AppSettings{}); }
        catch (const std::exception&) { expansion_rejected = true; }
        check(expansion_rejected && bytes(expanding_path) == expanding_bytes,
              "ANSI conversion or new Unicode fields exceeding the size limit preserve the original profile");
    }

    const fs::path growing_unicode_path = directory / L"growing-unicode-profile.ini";
    const auto growing_unicode_bytes = std::string{"\xff\xfe", 2} +
        utf16_bytes(std::wstring(4 * 1024 * 1024 - 2, L';'));
    {
        std::ofstream file(growing_unicode_path, std::ios::binary);
        file << growing_unicode_bytes;
    }
    bool unicode_growth_rejected = false;
    try { wardogs::save_settings_to(growing_unicode_path, AppSettings{}); }
    catch (const std::exception&) { unicode_growth_rejected = true; }
    check(unicode_growth_rejected && bytes(growing_unicode_path) == growing_unicode_bytes,
          "adding fields beyond the complete UTF-16 file limit preserves the previous Unicode profile");

    for (const int delay : {-100, 2300}) {
        changed.mouse_capture_delay_ms = delay;
        wardogs::save_settings_to(migrated_path, changed);
        check(wardogs::load_settings_from(migrated_path).mouse_capture_delay_ms ==
                  (delay < 0 ? 0 : 2000),
              "saving clamps the marker delay to supported limits");
    }
    {
        std::ofstream invalid(directory / L"invalid.ini");
        invalid << "[settings]\nmouse_capture_delay_ms=-9\n"
                   "middle_mouse_enabled=2\nexit_game_mode_hotkey=bogus\n";
    }
    const auto invalid_added = wardogs::load_settings_from(directory / L"invalid.ini");
    check(invalid_added.mouse_capture_delay_ms == 0 &&
              invalid_added.game_integration_enabled && invalid_added.middle_mouse_enabled &&
              invalid_added.exit_game_mode_hotkey == L"Alt+C",
          "invalid legacy fields retain the current workflow defaults and delay bounds");
    {
        std::ofstream invalid(directory / L"invalid.ini", std::ios::trunc);
        invalid << "[settings]\nmouse_capture_delay_ms=99999\n";
    }
    check(wardogs::load_settings_from(directory / L"invalid.ini").mouse_capture_delay_ms == 2000,
          "the upper delay limit also applies to imported files");

    // Refuse replacement under an actual Windows sharing lock. This exercises
    // failure after all new keys were serialized, not just a synthetic error.
    const auto before_failure = bytes(migrated_path);
    const auto settings_before_failure = wardogs::load_settings_from(migrated_path);
    HANDLE locked = CreateFileW(migrated_path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(locked != INVALID_HANDLE_VALUE, "the replacement-failure fixture is locked");
    if (locked != INVALID_HANDLE_VALUE) {
        bool rejected = false;
        try {
            changed.region_hotkey = recovered.region_hotkey;
            changed.pinned_card.unlock_hotkey = recovered.pinned_card.unlock_hotkey;
            changed.exit_game_mode_hotkey = recovered.exit_game_mode_hotkey;
            wardogs::save_settings_to(migrated_path, changed);
        } catch (const std::exception&) { rejected = true; }
        check(rejected && bytes(migrated_path) == before_failure,
              "a failed atomic replacement preserves every old byte");
        CloseHandle(locked);
        const auto retained = wardogs::load_settings_from(migrated_path);
        check(retained.region_hotkey == settings_before_failure.region_hotkey &&
                  retained.pinned_card.unlock_hotkey == settings_before_failure.pinned_card.unlock_hotkey &&
                  retained.exit_game_mode_hotkey == settings_before_failure.exit_game_mode_hotkey &&
                  retained.base_hotkey == settings_before_failure.base_hotkey &&
                  retained.target_hotkey == settings_before_failure.target_hotkey &&
                  retained.game_integration_enabled == settings_before_failure.game_integration_enabled &&
                  retained.middle_mouse_enabled == settings_before_failure.middle_mouse_enabled,
              "failed persistence of recovered shortcuts retains the complete previous action profile");
    }
    changed.exit_game_mode_hotkey = L"unsupported";
    bool invalid_rejected = false;
    try { wardogs::save_settings_to(migrated_path, changed); }
    catch (const std::exception&) { invalid_rejected = true; }
    check(invalid_rejected && bytes(migrated_path) == before_failure,
          "validation failure during serialization cannot partially change a profile");
    bool no_temporary_files = true;
    for (const auto& entry : fs::directory_iterator(directory)) {
        if (entry.path().filename().wstring().find(L".tmp-") != std::wstring::npos)
            no_temporary_files = false;
    }
    check(no_temporary_files, "both atomic failure paths remove their temporary files");
    check(wardogs::settings_path().parent_path().filename() == L"WardogsFireControl",
          "the new application owns an independent user-profile folder");

    fs::remove_all(directory);
    if (failures) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All settings tests passed\n";
    return 0;
}

int main() {
    try { return run_settings_tests(); }
    catch (const std::exception& error) {
        std::cerr << "FAIL: settings fixture: " << error.what() << '\n';
        return 1;
    }
}
