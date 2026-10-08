#pragma once

#include "wardogs/core.hpp"
#include "wardogs/game_map.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace wardogs {

enum class FireMissionKind { firing_position, target };
enum class FireMissionWeapon { l81, sph2 };

struct SavedFireMission {
    std::string id;
    std::wstring name;
    GameMap map{GameMap::unselected};
    FireMissionWeapon weapon{FireMissionWeapon::l81};
    FireMissionKind kind{FireMissionKind::firing_position};
    Point point;

    bool operator==(const SavedFireMission&) const = default;
};

inline constexpr int fire_missions_schema_version = 1;
inline constexpr std::size_t maximum_saved_fire_missions = 500;
// UTF-16 code units, matching the Windows/Qt name editor.
inline constexpr std::size_t maximum_fire_mission_name_length = 120;
inline constexpr std::size_t maximum_fire_missions_file_bytes = 1024 * 1024;

// The persistent file is outside the portable application and updater's manifest:
// %LOCALAPPDATA%/WardogsFireControl/fire-missions.json.
[[nodiscard]] std::filesystem::path fire_missions_path();
void validate_fire_mission(const SavedFireMission& mission);

// Requires an explicit already-confirmed map and matching weapon. This only
// returns the exact stored point: it never confirms a map, installs terrain,
// changes a weapon, restores calibration or mutates any UI state.
[[nodiscard]] Point restore_fire_mission(const SavedFireMission& mission,
                                        GameMap confirmed_map,
                                        FireMissionWeapon confirmed_weapon,
                                        bool map_is_confirmed);

class FireMissionRepository {
public:
    explicit FireMissionRepository(std::filesystem::path path = fire_missions_path());

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    // Missing files mean an empty collection. Invalid, inaccessible, oversized
    // and unsupported files raise errors and are never silently replaced.
    [[nodiscard]] std::vector<SavedFireMission> load() const;
    // Every mutation locks, reloads the current collection, validates all data
    // and atomically replaces the file. Unknown/future schema keys are rejected
    // rather than discarded. Names are trimmed/NFC-normalized on user input;
    // duplicate names within one map/weapon/kind are rejected case-insensitively.
    [[nodiscard]] SavedFireMission create(GameMap map, FireMissionWeapon weapon,
                                          FireMissionKind kind, std::wstring name,
                                          Point point) const;
    // The identity, map, weapon and point kind remain unchanged.
    [[nodiscard]] SavedFireMission update(std::string_view id, std::wstring name,
                                          Point point) const;
    // false means that a valid identifier is absent; malformed data still fails.
    [[nodiscard]] bool erase(std::string_view id) const;

private:
    std::filesystem::path path_;
};

}  // namespace wardogs
