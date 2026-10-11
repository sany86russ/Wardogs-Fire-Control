#include "wardogs/pinned_placement.hpp"

#include <array>
#include <iostream>
#include <limits>

namespace {

int failures{};

void check(bool condition, const char* description) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << description << '\n';
    }
}

}  // namespace

int main() {
    using namespace wardogs;
    const PinnedCardRect primary{0, 0, 1920, 1040};
    const PinnedCardRect left{-2560, -200, 2560, 1400};
    const std::array screens{PinnedCardScreen{L"DISPLAY1", primary, true},
        PinnedCardScreen{L"DISPLAY2", left, false}};
    const PinnedCardPlacement saved{L"DISPLAY2", left, {-2230, 120, 640, 180}};

    const auto retained = restore_pinned_card(saved, screens, {640, 180}, primary);
    check(retained && *retained == saved,
          "restoring a valid secondary-monitor location preserves every logical coordinate");

    const auto resized = restore_pinned_card(saved, screens, {720, 240}, primary);
    check(resized && resized->rect == PinnedCardRect{-2230, 120, 720, 240},
          "a different mode size keeps the user's anchor while fitting on the same monitor");

    // A smaller available logical area models a DPI/resolution/taskbar change.
    const std::array dpi_screens{PinnedCardScreen{L"DISPLAY2", {-1920, -200, 1920, 1040}, false}};
    const auto logical = restore_pinned_card(saved, dpi_screens, {640, 180}, primary);
    check(logical && logical->rect == PinnedCardRect{-1590, 120, 640, 180},
          "DPI changes preserve logical content size and recover the monitor-local offset");

    const std::array relocated{PinnedCardScreen{L"DISPLAY2", {1920, 0, 2560, 1400}, false}};
    const auto moved = restore_pinned_card(saved, relocated, {640, 180}, primary);
    check(moved && moved->rect == PinnedCardRect{2250, 320, 640, 180},
          "a relocated monitor keeps the previous offset from its available top-left corner");

    const std::array only_primary{screens.front()};
    const auto removed = restore_pinned_card(saved, only_primary, {640, 180}, primary);
    check(removed && removed->screen_id == L"DISPLAY1" &&
              removed->rect == PinnedCardRect{330, 320, 640, 180},
          "disconnecting the old monitor makes the card reachable on the remaining display");

    const PinnedCardPlacement renamed{L"OLD_NAME", left, saved.rect};
    const auto found = restore_pinned_card(renamed, screens, {640, 180}, primary);
    check(found && found->screen_id == L"DISPLAY2" && found->rect == saved.rect,
          "a changed screen name preserves a valid global location by geometric overlap");

    const PinnedCardPlacement edge{L"DISPLAY1", primary, {1800, 990, 640, 180}};
    const auto fit = restore_pinned_card(edge, screens, {640, 180}, left);
    check(fit && fit->rect == PinnedCardRect{1280, 860, 640, 180},
          "partly inaccessible cards are clamped to the selected monitor's usable area");

    const auto oversized = restore_pinned_card(saved, only_primary, {3840, 2160}, primary);
    check(oversized && oversized->rect == primary,
          "a saved large canvas cannot exceed a smaller replacement screen");

    const std::array separated_screens{
        PinnedCardScreen{L"PRIMARY", {0, 0, 1280, 720}, true},
        PinnedCardScreen{L"SECONDARY", {1920, 0, 1280, 720}, false}};
    const PinnedCardPlacement in_gap{L"REMOVED", {1280, 0, 640, 720}, {1450, 100, 320, 144}};
    const auto recovered_gap = restore_pinned_card(in_gap, separated_screens,
        {320, 144}, {0, 0, 900, 600});
    check(recovered_gap && recovered_gap->screen_id == L"PRIMARY" &&
              recovered_gap->rect.x >= 0 && recovered_gap->rect.y >= 0 &&
              recovered_gap->rect.x + recovered_gap->rect.width <= 1280 &&
              recovered_gap->rect.y + recovered_gap->rect.height <= 720,
          "a card wholly in a logical desktop gap is recovered onto a real monitor, never a bounding rectangle");

    const PinnedCardRect anchor{-2200, 100, 900, 600};
    const auto fresh = restore_pinned_card(std::nullopt, screens, {640, 180}, anchor);
    check(fresh && fresh->screen_id == L"DISPLAY2" &&
              fresh->rect == PinnedCardRect{-1940, 100, 640, 180},
          "a new card starts beside the invoking window on its actual monitor");

    check(!restore_pinned_card(saved, {}, {640, 180}, anchor),
          "an unavailable screen inventory does not manufacture a placement");
    check(!restore_pinned_card(saved, screens, {0, 180}, anchor) &&
              !restore_pinned_card(saved, screens, {16385, 180}, anchor),
          "invalid requested sizes are rejected before coordinate arithmetic");

    auto corrupt = saved;
    corrupt.rect.x = std::numeric_limits<int>::min();
    const auto recovered = restore_pinned_card(corrupt, screens, {640, 180}, anchor);
    check(recovered && recovered->rect == fresh->rect,
          "corrupt legacy geometry falls back to a reachable default without integer overflow");
    check(!valid_pinned_card_rect({999999, 0, 640, 180}) &&
              !valid_pinned_card_rect({-1000001, 0, 640, 180}) &&
              !valid_pinned_card_rect({0, 0, 640, -1}),
          "persistence bounds cover coordinate endpoints and positive dimensions");
    corrupt = saved;
    corrupt.screen_id = L"DISPLAY2\nsettings=invalid";
    check(!valid_pinned_card_placement(corrupt),
          "screen identities cannot inject extra lines into an INI file");
    corrupt.screen_id.assign(pinned_card_screen_id_limit + 1, L'X');
    check(!valid_pinned_card_placement(corrupt),
          "screen identities are bounded independently from geometry");
    if (failures) return 1;
    std::cout << "All pinned placement tests passed\n";
    return 0;
}
