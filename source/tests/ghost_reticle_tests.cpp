#include "wardogs/ghost_reticle.hpp"
#include "wardogs/presentation.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

bool near(double left, double right, double tolerance = 1e-9) {
    return std::abs(left - right) <= tolerance;
}

template <typename Operation>
void rejects(Operation operation, const char* message) {
    bool rejected = false;
    try { operation(); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, message);
}

}  // namespace

int main() {
    using wardogs::Arc;

    const auto bearing = wardogs::ghost_bearing_ticks(188.4);
    check(!bearing.empty(), "bearing scale produces visible ticks");
    bool found_195 = false;
    for (const auto& tick : bearing) {
        check(tick.position >= wardogs::ghost_bearing_left &&
                  tick.position <= wardogs::ghost_bearing_right,
              "bearing ticks stay inside the measured 662 px span");
        if (tick.value == 195) {
            found_195 = true;
            check(tick.major, "15 degree labels are major ticks");
            check(near(tick.position,
                       wardogs::ghost_center_x + (195.0 - 188.4) * 7.4),
                  "fractional bearing is represented by sub-tick translation");
        }
    }
    check(found_195, "bearing ruler includes the next standard label");

    const auto wrapped = wardogs::ghost_bearing_ticks(358.5);
    bool found_zero = false;
    for (const auto& tick : wrapped) {
        if (tick.value == 0) {
            found_zero = true;
            check(near(tick.position,
                       wardogs::ghost_center_x + 1.5 * 7.4),
                  "bearing ruler wraps through north without a jump");
        }
    }
    check(found_zero, "wrapped bearing ruler labels north as zero");
    check(near(wardogs::ghost_game_bearing(236.0, 0.0), 236.0),
          "ghost bearing compensation defaults to no offset");
    check(near(wardogs::ghost_game_bearing(236.0, -0.35), 235.65),
          "user bearing compensation retains sub-degree precision");
    check(near(wardogs::ghost_game_bearing(359.5, 1.0), 0.5),
          "user bearing compensation wraps cleanly through north");
    for (const double bearing_value : {-1.5, 718.5, -721.5}) {
        const auto equivalent = wardogs::ghost_bearing_ticks(bearing_value);
        check(equivalent.size() == wrapped.size(),
              "equivalent rotations keep the same visible bearing labels");
        for (std::size_t index = 0; index < equivalent.size(); ++index)
            check(equivalent[index].value == wrapped[index].value &&
                      near(equivalent[index].position, wrapped[index].position),
                  "negative and multi-turn bearings normalize before integer indexing");
    }
    const double large_angle = std::numeric_limits<double>::max();
    check(near(wardogs::ghost_game_bearing(large_angle, large_angle), 256.0),
          "adding two maximum finite angles preserves their modular bearing without overflow");
    check(!wardogs::ghost_bearing_ticks(large_angle).empty(),
          "maximum finite bearings normalize into a bounded visible ruler");
    const auto tiny_north = wardogs::ghost_game_bearing(
        -std::numeric_limits<double>::denorm_min(), 0);
    check(tiny_north == 0.0 && !std::signbit(tiny_north) &&
              !std::signbit(wardogs::ghost_game_bearing(-0.0, -0.0)),
          "north is positive zero even after negative subnormal rounding or signed zeros");
    for (const double invalid : {std::numeric_limits<double>::quiet_NaN(),
                                 std::numeric_limits<double>::infinity(),
                                 -std::numeric_limits<double>::infinity()}) {
        rejects([&] { (void)wardogs::ghost_bearing_ticks(invalid); },
                "nonfinite bearings cannot reach integer tick indexing");
        rejects([&] { (void)wardogs::ghost_game_bearing(0, invalid); },
                "nonfinite bearing compensation is rejected");
        rejects([&] { (void)wardogs::ghost_mil_ticks(invalid); },
                "nonfinite MIL cannot reach integer tick indexing");
        rejects([&] { (void)wardogs::ghost_mortar_mil_ticks(invalid); },
                "nonfinite mortar MIL cannot reach integer tick indexing");
    }
    for (const double outside : {-large_angle, large_angle})
        check(wardogs::ghost_mil_ticks(outside).empty() &&
                  wardogs::ghost_mortar_mil_ticks(outside).empty(),
              "large finite offscale MIL values produce no ticks without integer overflow");

    const auto mil = wardogs::ghost_mil_ticks(403.25);
    check(!mil.empty(), "MIL scale produces visible marks");
    const wardogs::GhostTick* mark_400 = nullptr;
    const wardogs::GhostTick* mark_410 = nullptr;
    for (const auto& tick : mil) {
        check(tick.position >= wardogs::ghost_mil_top &&
                  tick.position <= wardogs::ghost_mil_bottom,
              "MIL marks are clipped 93 px from both reticle borders");
        if (tick.value == 400) mark_400 = &tick;
        if (tick.value == 410) mark_410 = &tick;
    }
    check(mark_400 && mark_410, "MIL scale includes neighbouring labels");
    check(near(mark_410->position - mark_400->position, 176.0),
          "10 mil labels retain the measured 176 px spacing");
    check(near(mark_400->position,
               wardogs::ghost_center_y + (400.0 - 403.25) * 17.6),
          "fractional MIL is represented by continuous ruler translation");

    const auto low_command = wardogs::displayed_firing_command(359.96, 399.49, Arc::low);
    bool commanded_north_centered = false;
    for (const auto& tick : wardogs::ghost_bearing_ticks(low_command.bearing_deg)) {
        if (tick.value == 0)
            commanded_north_centered = near(tick.position, wardogs::ghost_center_x);
    }
    check(commanded_north_centered,
          "rounded north command places the zero azimuth mark exactly at the ghost center");
    bool commanded_400_found = false;
    for (const auto& tick : wardogs::ghost_mil_ticks(low_command.mil)) {
        if (tick.value == 400)
            commanded_400_found = near(tick.position, wardogs::ghost_center_y + 17.6);
    }
    check(commanded_400_found,
          "399 MIL card command places the 400 MIL ruler mark one integer step from the center");
    const auto high_command = wardogs::displayed_firing_command(36.86, 1149.51, Arc::high);
    bool commanded_1150_centered = false;
    for (const auto& tick : wardogs::ghost_mil_ticks(high_command.mil)) {
        if (tick.value == 1150)
            commanded_1150_centered = near(tick.position, wardogs::ghost_center_y);
    }
    check(commanded_1150_centered,
          "rounded high-arc command places its labelled integer MIL at the ruler center");

    const auto mortar_mil = wardogs::ghost_mortar_mil_ticks(725.0);
    const wardogs::GhostTick* mortar_700 = nullptr;
    const wardogs::GhostTick* mortar_750 = nullptr;
    for (const auto& tick : mortar_mil) {
        check(tick.position >= wardogs::ghost_mil_top &&
                  tick.position <= wardogs::ghost_mil_bottom,
              "mortar MIL marks use the measured 93 px vertical margins");
        if (tick.value == 700) mortar_700 = &tick;
        if (tick.value == 750) mortar_750 = &tick;
    }
    check(mortar_700 && mortar_750,
          "mortar ruler includes adjacent 50 MIL labels");
    check(near(mortar_750->position - mortar_700->position, 136.0),
          "mortar 50 MIL labels retain the measured 136 px spacing");
    check(near(mortar_700->position,
               wardogs::ghost_center_y + (700.0 - 725.0) * 2.72),
          "mortar ruler scrolls continuously to fractional MIL solutions");

    check(wardogs::effective_ghost_arc(true, true, Arc::high) == Arc::high,
          "both valid solutions use the persisted preference");
    check(wardogs::effective_ghost_arc(true, false, Arc::high) == Arc::low,
          "a lone low solution temporarily overrides a high preference");
    check(wardogs::effective_ghost_arc(false, true, Arc::low) == Arc::high,
          "a lone high solution temporarily overrides a low preference");
    check(!wardogs::effective_ghost_arc(false, false, Arc::low),
          "no valid solution produces no ghost reticle result");
    check(wardogs::ghost_preset_width(1280, 720) == 480,
          "the 720p preset keeps the measured reticle-to-screen ratio");
    check(wardogs::ghost_preset_width(1600, 900) == 600,
          "the 900p preset keeps the measured reticle-to-screen ratio");
    check(wardogs::ghost_preset_width(1920, 1080) == 720,
          "the 1080p preset keeps the measured reticle-to-screen ratio");
    check(wardogs::ghost_preset_width(2560, 1440) == 960,
          "the 1440p preset restores the measured 960 px width");
    check(!wardogs::ghost_preset_width(1920, 1200),
          "non-16:9 resolutions are not offered without measurements");

    std::cout << "All ghost reticle tests passed\n";
    return 0;
}
