#include "wardogs/core.hpp"

#include <cmath>
#include <algorithm>
#include <numbers>
#include <array>
#include <limits>
#include <stdexcept>
#include <utility>

namespace wardogs {
namespace {

using MortarTableEntry = std::pair<double, double>;

// L81 flat-ground firing table from
// https://github.com/apollyon-sys/wardogs-calculator/blob/main/data/weapons.json
// retrieved 2026-10-03. The weapon's declared operating limits are
// 132..684 m and 850..150 mil; out-of-limit extension rows are not imported.
constexpr std::array<MortarTableEntry, 71> mortar_table{{
    {132, 850}, {140, 840}, {151, 830}, {163, 820}, {175, 810},
    {187, 800}, {198, 790}, {208, 780}, {219, 770}, {229, 760},
    {239, 750}, {250, 740}, {260, 730}, {270, 720}, {280, 710},
    {290, 700}, {300, 690}, {310, 680}, {319, 670}, {329, 660},
    {339, 650}, {348, 640}, {358, 630}, {367, 620}, {376, 610},
    {385, 600}, {394, 590}, {403, 580}, {412, 570}, {420, 560},
    {429, 550}, {437, 540}, {446, 530}, {454, 520}, {462, 510},
    {470, 500}, {478, 490}, {486, 480}, {494, 470}, {501, 460},
    {509, 450}, {516, 440}, {524, 430}, {531, 420}, {538, 410},
    {545, 400}, {552, 390}, {559, 380}, {565, 370}, {572, 360},
    {578, 350}, {585, 340}, {591, 330}, {597, 320}, {603, 310},
    {609, 300}, {615, 290}, {620, 280}, {626, 270}, {631, 260},
    {636, 250}, {641, 240}, {646, 230}, {651, 220}, {656, 210},
    {661, 200}, {666, 190}, {670, 180}, {675, 170}, {680, 160},
    {684, 150},
}};

}  // namespace

Shot calculate_shot(Point base, Point target) {
    if (!std::isfinite(base.x) || !std::isfinite(base.y) ||
        !std::isfinite(target.x) || !std::isfinite(target.y))
        throw std::invalid_argument("Координаты орудия и цели должны быть конечными числами");
    const double dx = target.x - base.x;
    const double dy = target.y - base.y;
    const double distance = std::hypot(dx, dy);
    if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(distance) ||
        !std::isfinite(distance * 100.0))
        throw std::invalid_argument("Координаты слишком велики для расчёта расстояния");
    double angle = distance == 0.0 ? 0.0
        : std::atan2(dx, dy) * 180.0 / std::numbers::pi;
    if (angle < 0) {
        angle += 360.0;
    }
    // A tiny negative atan2 result can round to 360 when wrapped. Bearings
    // use the half-open [0, 360) interval, with canonical unsigned north.
    if (angle == 0.0 || angle >= 360.0) angle = 0.0;
    return {base, target, dx, dy, distance, angle};
}

double mortar_mil_for_distance(double distance_m) {
    if (!std::isfinite(distance_m)) {
        throw std::invalid_argument("Таблица миномёта L81 поддерживает дальность от 132 до 684 м");
    }
    // Decimal map coordinates are represented in binary. Subtracting two
    // coordinates and converting to metres can put an exact 132/684-metre
    // shot a few ULPs outside the table. Accommodate only that roundoff; this
    // allowance is far smaller than the game's coordinate resolution.
    constexpr double endpoint_roundoff_m = 32.0 *
        std::numeric_limits<double>::epsilon() * mortar_table.back().first;
    for (const double endpoint : {mortar_table.front().first, mortar_table.back().first}) {
        if (std::abs(distance_m - endpoint) <= endpoint_roundoff_m) {
            distance_m = endpoint;
            break;
        }
    }
    if (distance_m < mortar_table.front().first ||
        distance_m > mortar_table.back().first) {
        throw std::invalid_argument("Таблица миномёта L81 поддерживает дальность от 132 до 684 м");
    }
    const auto right = std::lower_bound(mortar_table.begin(), mortar_table.end(), distance_m,
        [](const MortarTableEntry& entry, double distance) { return entry.first < distance; });
    if (right == mortar_table.begin()) return right->second;
    const auto [left_distance, left_mil] = *std::prev(right);
    const auto [right_distance, right_mil] = *right;
    const double fraction = (distance_m - left_distance) / (right_distance - left_distance);
    return left_mil + fraction * (right_mil - left_mil);
}

}  // namespace wardogs
