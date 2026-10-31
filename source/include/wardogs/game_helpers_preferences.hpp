#pragma once

#include <stdexcept>
#include <string>

namespace wardogs {

namespace game_helpers_limits {
inline constexpr int minimum_hold_ms = 50;
inline constexpr int maximum_hold_ms = 5000;
inline constexpr int minimum_pause_ms = 20;
inline constexpr int maximum_pause_ms = 10000;
inline constexpr int minimum_interval_ms = 100;
inline constexpr int maximum_interval_ms = 10000;
inline constexpr int maximum_random_ms = 2000;
}  // namespace game_helpers_limits

struct BuildHelperPreferences {
    bool enabled{false};
    std::wstring hotkey{L"F6"};
    int hold_ms{660};
    int pause_ms{100};
    bool random_enabled{false};
    int random_min_ms{0};
    int random_max_ms{30};

    bool operator==(const BuildHelperPreferences&) const = default;
};

struct TeamPikerPreferences {
    bool enabled{false};
    std::wstring hotkey{L"F7"};
    int interval_ms{800};
    bool random_enabled{true};
    int random_min_ms{100};
    int random_max_ms{200};

    bool operator==(const TeamPikerPreferences&) const = default;
};

struct GameHelpersPreferences {
    BuildHelperPreferences build;
    TeamPikerPreferences team;

    bool operator==(const GameHelpersPreferences&) const = default;
};

// These are persistent preferences only. Running state must never be restored
// from a profile; every application launch starts with both helpers stopped.
inline void validate_game_helpers_preferences(const GameHelpersPreferences& value) {
    using namespace game_helpers_limits;
    if (value.build.hold_ms < minimum_hold_ms || value.build.hold_ms > maximum_hold_ms)
        throw std::invalid_argument("Удержание ЛКМ должно быть от 50 до 5000 мс");
    if (value.build.pause_ms < minimum_pause_ms || value.build.pause_ms > maximum_pause_ms)
        throw std::invalid_argument("Пауза Build helper должна быть от 20 до 10000 мс");
    if (value.team.interval_ms < minimum_interval_ms || value.team.interval_ms > maximum_interval_ms)
        throw std::invalid_argument("Интервал Team Piker должен быть от 100 до 10000 мс");
    const auto valid_random = [](int minimum, int maximum) {
        return minimum >= 0 && minimum <= maximum && maximum <= maximum_random_ms;
    };
    if (!valid_random(value.build.random_min_ms, value.build.random_max_ms) ||
        !valid_random(value.team.random_min_ms, value.team.random_max_ms))
        throw std::invalid_argument("Случайная добавка должна быть от 0 до 2000 мс; минимум не больше максимума");
}

}  // namespace wardogs
