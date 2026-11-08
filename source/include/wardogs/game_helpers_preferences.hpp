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
inline constexpr int minimum_reload_hold_ms = 20;
inline constexpr int maximum_reload_hold_ms = 1000;
inline constexpr int minimum_reload_key_gap_ms = 20;
inline constexpr int maximum_reload_key_gap_ms = 2000;
inline constexpr int minimum_reload_stage_gap_ms = 100;
inline constexpr int maximum_reload_stage_gap_ms = 10000;
}  // namespace game_helpers_limits

struct BuildHelperPreferences {
    bool enabled{false};
    std::wstring hotkey{L"F6"};
    int hold_ms{514};
    int pause_ms{33};
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

struct ReloadHelperPreferences {
    bool enabled{false};
    std::wstring hotkey{L"F8"};
    bool automatic_region{true};
    // Ten-thousandths of the game client size, independent of screen DPI.
    // An empty rectangle is allowed only while automatic search is selected.
    int left{};
    int top{};
    int right{};
    int bottom{};

    int hold_ms{50};
    int key_gap_ms{90};
    int stage_gap_ms{750};
    bool key_random_enabled{false};
    int key_random_min_ms{0};
    int key_random_max_ms{30};
    bool stage_random_enabled{false};
    int stage_random_min_ms{0};
    int stage_random_max_ms{200};

    bool operator==(const ReloadHelperPreferences&) const = default;
};

inline bool valid_reload_region(const ReloadHelperPreferences& value) noexcept {
    return value.left >= 0 && value.top >= 0 && value.right <= 10000 &&
        value.bottom <= 10000 && value.left < value.right && value.top < value.bottom;
}

inline void validate_reload_helper_timing_preferences(const ReloadHelperPreferences& value) {
    using namespace game_helpers_limits;
    if (value.hold_ms < minimum_reload_hold_ms || value.hold_ms > maximum_reload_hold_ms)
        throw std::invalid_argument("Удержание клавиши перезарядки должно быть от 20 до 1000 мс");
    if (value.key_gap_ms < minimum_reload_key_gap_ms || value.key_gap_ms > maximum_reload_key_gap_ms)
        throw std::invalid_argument("Пауза между клавишами перезарядки должна быть от 20 до 2000 мс");
    if (value.stage_gap_ms < minimum_reload_stage_gap_ms || value.stage_gap_ms > maximum_reload_stage_gap_ms)
        throw std::invalid_argument("Пауза между комбинациями должна быть от 100 до 10000 мс");
    const auto valid_random = [](int minimum, int maximum) {
        return minimum >= 0 && minimum <= maximum && maximum <= maximum_random_ms;
    };
    if (!valid_random(value.key_random_min_ms, value.key_random_max_ms) ||
        !valid_random(value.stage_random_min_ms, value.stage_random_max_ms))
        throw std::invalid_argument("Случайная добавка должна быть от 0 до 2000 мс; минимум не больше максимума");
}

struct GameHelpersPreferences {
    BuildHelperPreferences build;
    TeamPikerPreferences team;
    ReloadHelperPreferences reload;

    bool operator==(const GameHelpersPreferences&) const = default;
};

// These are persistent preferences only. Running state must never be restored
// from a profile; every application launch starts with all helpers stopped.
inline void validate_game_helpers_preferences(const GameHelpersPreferences& value) {
    using namespace game_helpers_limits;
    validate_reload_helper_timing_preferences(value.reload);
    const bool empty_reload_region = value.reload.left == 0 && value.reload.top == 0 &&
        value.reload.right == 0 && value.reload.bottom == 0;
    if (!valid_reload_region(value.reload) &&
        (!value.reload.automatic_region || !empty_reload_region))
        throw std::invalid_argument("Выберите область стрелок внутри окна WARDOGS или включите автоматический поиск");
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
