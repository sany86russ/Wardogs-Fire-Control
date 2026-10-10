#include "wardogs/pinned_placement.hpp"

#include <algorithm>
#include <cstdint>

namespace wardogs {
namespace {

using Coordinate = std::int64_t;

bool valid_screen_id(const std::wstring& value) noexcept {
    return !value.empty() && value.size() <= pinned_card_screen_id_limit &&
        value.front() != L' ' && value.back() != L' ' &&
        !(value.size() >= 2 && ((value.front() == L'"' && value.back() == L'"') ||
                               (value.front() == L'\'' && value.back() == L'\''))) &&
        std::none_of(value.begin(), value.end(), [](wchar_t character) {
            return character < 32 || character == 127;
        });
}

Coordinate right(PinnedCardRect rect) noexcept {
    return static_cast<Coordinate>(rect.x) + rect.width;
}

Coordinate bottom(PinnedCardRect rect) noexcept {
    return static_cast<Coordinate>(rect.y) + rect.height;
}

bool contains(PinnedCardRect outer, PinnedCardRect inner) noexcept {
    return inner.x >= outer.x && inner.y >= outer.y &&
        right(inner) <= right(outer) && bottom(inner) <= bottom(outer);
}

Coordinate intersection_area(PinnedCardRect first, PinnedCardRect second) noexcept {
    const auto width = std::max<Coordinate>(0,
        std::min(right(first), right(second)) - std::max(first.x, second.x));
    const auto height = std::max<Coordinate>(0,
        std::min(bottom(first), bottom(second)) - std::max(first.y, second.y));
    return width * height;
}

bool usable_screen(const PinnedCardScreen& screen) noexcept {
    return valid_screen_id(screen.screen_id) && valid_pinned_card_rect(screen.available);
}

const PinnedCardScreen* overlapping_screen(std::span<const PinnedCardScreen> screens,
                                          PinnedCardRect rect) noexcept {
    const PinnedCardScreen* selected{};
    Coordinate largest{};
    for (const auto& screen : screens) {
        if (!usable_screen(screen)) continue;
        const auto area = intersection_area(screen.available, rect);
        if (area > largest || (area != 0 && area == largest && screen.primary)) {
            selected = &screen;
            largest = area;
        }
    }
    return selected;
}

const PinnedCardScreen* fallback_screen(std::span<const PinnedCardScreen> screens,
                                       PinnedCardRect anchor) noexcept {
    if (valid_pinned_card_rect(anchor))
        if (const auto* selected = overlapping_screen(screens, anchor)) return selected;
    const PinnedCardScreen* first{};
    for (const auto& screen : screens) {
        if (!usable_screen(screen)) continue;
        if (screen.primary) return &screen;
        if (!first) first = &screen;
    }
    return first;
}

}  // namespace

bool valid_pinned_card_size(PinnedCardSize size) noexcept {
    return size.width > 0 && size.height > 0 &&
        size.width <= pinned_card_dimension_limit && size.height <= pinned_card_dimension_limit;
}

bool valid_pinned_card_rect(PinnedCardRect rect) noexcept {
    return valid_pinned_card_size({rect.width, rect.height}) &&
        rect.x >= -pinned_card_coordinate_limit && rect.y >= -pinned_card_coordinate_limit &&
        right(rect) <= pinned_card_coordinate_limit && bottom(rect) <= pinned_card_coordinate_limit;
}

bool valid_pinned_card_placement(const PinnedCardPlacement& placement) noexcept {
    return valid_screen_id(placement.screen_id) && valid_pinned_card_rect(placement.available) &&
        valid_pinned_card_rect(placement.rect);
}

std::optional<PinnedCardPlacement> restore_pinned_card(
    const std::optional<PinnedCardPlacement>& saved,
    std::span<const PinnedCardScreen> screens,
    PinnedCardSize requested_size,
    PinnedCardRect anchor) {
    if (!valid_pinned_card_size(requested_size)) return std::nullopt;
    const bool have_saved = saved && valid_pinned_card_placement(*saved);
    const PinnedCardScreen* selected{};
    if (have_saved) {
        for (const auto& screen : screens) {
            if (usable_screen(screen) && screen.screen_id == saved->screen_id) {
                selected = &screen;
                break;
            }
        }
        if (!selected) selected = overlapping_screen(screens, saved->rect);
    }
    if (!selected) selected = fallback_screen(screens, anchor);
    if (!selected) return std::nullopt;

    PinnedCardRect restored{
        selected->available.x, selected->available.y,
        std::min(requested_size.width, selected->available.width),
        std::min(requested_size.height, selected->available.height)};
    Coordinate x = restored.x;
    Coordinate y = restored.y;
    if (have_saved) {
        x = saved->rect.x;
        y = saved->rect.y;
        const PinnedCardRect requested{saved->rect.x, saved->rect.y, restored.width, restored.height};
        if (!contains(selected->available, requested)) {
            // Preserve the local offset when a monitor moved or was removed.
            // Logical dimensions also make a DPI change require only a fit,
            // rather than multiplying previously logical window coordinates.
            x = static_cast<Coordinate>(selected->available.x) + saved->rect.x - saved->available.x;
            y = static_cast<Coordinate>(selected->available.y) + saved->rect.y - saved->available.y;
        }
    } else if (valid_pinned_card_rect(anchor)) {
        x = right(anchor) - restored.width;
        y = anchor.y;
    } else {
        x = right(selected->available) - restored.width;
    }
    restored.x = static_cast<int>(std::clamp(x, static_cast<Coordinate>(selected->available.x),
        right(selected->available) - restored.width));
    restored.y = static_cast<int>(std::clamp(y, static_cast<Coordinate>(selected->available.y),
        bottom(selected->available) - restored.height));
    return PinnedCardPlacement{selected->screen_id, selected->available, restored};
}

}  // namespace wardogs
