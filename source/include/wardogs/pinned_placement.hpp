#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>

namespace wardogs {

// All coordinates and sizes are Qt logical pixels, never physical pixels.
struct PinnedCardSize {
    int width{};
    int height{};
    friend bool operator==(const PinnedCardSize&, const PinnedCardSize&) = default;
};

struct PinnedCardRect {
    int x{};
    int y{};
    int width{};
    int height{};
    friend bool operator==(const PinnedCardRect&, const PinnedCardRect&) = default;
};

struct PinnedCardPlacement {
    std::wstring screen_id;
    PinnedCardRect available;
    PinnedCardRect rect;
    friend bool operator==(const PinnedCardPlacement&, const PinnedCardPlacement&) = default;
};

struct PinnedCardScreen {
    std::wstring screen_id;
    PinnedCardRect available;
    bool primary{};
};

inline constexpr int pinned_card_coordinate_limit = 1'000'000;
inline constexpr int pinned_card_dimension_limit = 16'384;
inline constexpr std::size_t pinned_card_screen_id_limit = 512;

[[nodiscard]] bool valid_pinned_card_size(PinnedCardSize size) noexcept;
[[nodiscard]] bool valid_pinned_card_rect(PinnedCardRect rect) noexcept;
[[nodiscard]] bool valid_pinned_card_placement(const PinnedCardPlacement& placement) noexcept;

// Preserve a valid saved location, recover a displaced/removed screen, and fit
// the requested content size within the selected screen's available area.
// Without a saved location, anchor's upper right corner supplies the position.
// No usable screens (or an invalid requested size) returns nullopt.
[[nodiscard]] std::optional<PinnedCardPlacement> restore_pinned_card(
    const std::optional<PinnedCardPlacement>& saved,
    std::span<const PinnedCardScreen> screens,
    PinnedCardSize requested_size,
    PinnedCardRect anchor);

}  // namespace wardogs
