#include "wardogs/ghost_reticle.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace wardogs {
namespace {

constexpr double bearing_pixels_per_degree = 111.0 / 15.0;
constexpr double mil_pixels_per_mil = 176.0 / 10.0;
constexpr double mortar_pixels_per_mil = 136.0 / 50.0;

int normalized_bearing(int degrees) {
    const int value = degrees % 360;
    return value < 0 ? value + 360 : value;
}

}  // namespace

std::vector<GhostTick> ghost_bearing_ticks(double target_bearing_degrees) {
    target_bearing_degrees = ghost_game_bearing(target_bearing_degrees, 0.0);
    std::vector<GhostTick> ticks;
    const double visible_degrees =
        (ghost_bearing_right - ghost_bearing_left) / bearing_pixels_per_degree;
    const int first = static_cast<int>(
        std::ceil((target_bearing_degrees - visible_degrees / 2.0) / 3.0));
    const int last = static_cast<int>(
        std::floor((target_bearing_degrees + visible_degrees / 2.0) / 3.0));
    for (int step = first; step <= last; ++step) {
        const int raw_value = step * 3;
        const double position = ghost_center_x +
                                (raw_value - target_bearing_degrees) *
                                    bearing_pixels_per_degree;
        if (position < ghost_bearing_left - 1e-9 ||
            position > ghost_bearing_right + 1e-9)
            continue;
        const int value = normalized_bearing(raw_value);
        ticks.push_back({position, value, value % 15 == 0});
    }
    return ticks;
}

double ghost_game_bearing(double calculated_bearing_degrees,
                          double compensation_degrees) {
    if (!std::isfinite(calculated_bearing_degrees) ||
        !std::isfinite(compensation_degrees))
        throw std::invalid_argument("Азимут и поправка прицела должны быть конечными числами");
    // Normalize before adding, so even two large finite angles cannot overflow
    // and turn an otherwise valid bearing into NaN.
    double value = std::fmod(std::fmod(calculated_bearing_degrees, 360.0) +
                             std::fmod(compensation_degrees, 360.0), 360.0);
    if (value < 0.0) value += 360.0;
    // Rounding a negative subnormal plus 360 can yield exactly 360; north is
    // represented by positive zero throughout the calculated ruler.
    return value == 0.0 || value >= 360.0 ? 0.0 : value;
}

std::vector<GhostTick> ghost_mil_ticks(double target_mil) {
    if (!std::isfinite(target_mil))
        throw std::invalid_argument("MIL прицела должен быть конечным числом");
    std::vector<GhostTick> ticks;
    const double visible_mil =
        (ghost_mil_bottom - ghost_mil_top) / mil_pixels_per_mil;
    const double first_visible = std::max(20.0, target_mil - visible_mil / 2.0);
    const double last_visible = std::min(1400.0, target_mil + visible_mil / 2.0);
    if (first_visible > last_visible) return ticks;
    const int first = static_cast<int>(std::ceil(first_visible / 10.0));
    const int last = static_cast<int>(std::floor(last_visible / 10.0));
    for (int step = first; step <= last; ++step) {
        const int value = step * 10;
        if (value < 20 || value > 1400) continue;
        const double position = ghost_center_y +
                                (value - target_mil) * mil_pixels_per_mil;
        if (position < ghost_mil_top - 1e-9 ||
            position > ghost_mil_bottom + 1e-9)
            continue;
        ticks.push_back({position, value, true});
    }
    return ticks;
}

std::vector<GhostTick> ghost_mortar_mil_ticks(double target_mil) {
    if (!std::isfinite(target_mil))
        throw std::invalid_argument("MIL миномётного прицела должен быть конечным числом");
    std::vector<GhostTick> ticks;
    const double visible_mil =
        (ghost_mil_bottom - ghost_mil_top) / mortar_pixels_per_mil;
    const double first_visible = std::max(150.0, target_mil - visible_mil / 2.0);
    const double last_visible = std::min(850.0, target_mil + visible_mil / 2.0);
    if (first_visible > last_visible) return ticks;
    const int first = static_cast<int>(std::ceil(first_visible / 50.0));
    const int last = static_cast<int>(std::floor(last_visible / 50.0));
    for (int step = first; step <= last; ++step) {
        const int value = step * 50;
        if (value < 150 || value > 850) continue;
        const double position = ghost_center_y +
                                (value - target_mil) *
                                    mortar_pixels_per_mil;
        if (position < ghost_mil_top - 1e-9 ||
            position > ghost_mil_bottom + 1e-9)
            continue;
        ticks.push_back({position, value, true});
    }
    return ticks;
}

std::optional<Arc> effective_ghost_arc(bool low_available,
                                       bool high_available,
                                       Arc preferred) {
    if (low_available && high_available) return preferred;
    if (low_available) return Arc::low;
    if (high_available) return Arc::high;
    return std::nullopt;
}

std::optional<int> ghost_preset_width(int screen_width, int screen_height) {
    if (screen_width <= 0 || screen_height <= 0 ||
        static_cast<long long>(screen_width) * 9 !=
            static_cast<long long>(screen_height) * 16)
        return std::nullopt;
    const long long scaled_width =
        static_cast<long long>(screen_width) * 3;
    if (scaled_width % 8 != 0) return std::nullopt;
    const int reticle_width = static_cast<int>(scaled_width / 8);
    if (reticle_width < GhostReticlePreferences::minimum_width ||
        reticle_width > GhostReticlePreferences::maximum_width)
        return std::nullopt;
    return reticle_width;
}

}  // namespace wardogs
