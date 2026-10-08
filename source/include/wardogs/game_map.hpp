#pragma once

#include <initializer_list>
#include <stdexcept>
#include <string_view>

namespace wardogs {

enum class GameMap { unselected, bakurani, ozeti, zestafona, training, other };

constexpr std::wstring_view game_map_key(GameMap map) {
    switch (map) {
    case GameMap::unselected: return L"";
    case GameMap::bakurani: return L"bakurani";
    case GameMap::ozeti: return L"ozeti";
    case GameMap::zestafona: return L"zestafona";
    case GameMap::training: return L"training";
    case GameMap::other: return L"other";
    }
    throw std::invalid_argument("Неизвестная карта игры");
}

constexpr GameMap game_map_from_key(std::wstring_view key) {
    for (const auto map : {GameMap::bakurani, GameMap::ozeti, GameMap::zestafona,
                           GameMap::training, GameMap::other})
        if (game_map_key(map) == key) return map;
    return GameMap::unselected;
}

constexpr bool game_map_has_terrain(GameMap map) {
    return map == GameMap::bakurani || map == GameMap::ozeti || map == GameMap::zestafona;
}

}  // namespace wardogs
