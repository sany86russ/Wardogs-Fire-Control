#include "wardogs/reload_region.hpp"

#include <cstdint>
#include <stdexcept>

namespace wardogs {
namespace {
void validate_size(int width, int height) {
    if (width <= 0 || height <= 0 || width > 16384 || height > 8192 ||
        static_cast<std::int64_t>(width) * height > 40'000'000)
        throw std::invalid_argument("Размер окна для поиска стрелок не поддерживается");
}
int scaled(int value, int extent, bool round_up = false) {
    return static_cast<int>((static_cast<std::int64_t>(value) * extent +
                             (round_up ? 9999 : 0)) / 10000);
}
}  // namespace

ImageRect reload_search_rect(int width, int height,
                             const ReloadHelperPreferences& preferences) {
    validate_size(width, height);
    if (preferences.automatic_region) return {0, 0, width, height};
    if (preferences.left < 0 || preferences.top < 0 ||
        preferences.right > 10000 || preferences.bottom > 10000 ||
        preferences.left >= preferences.right || preferences.top >= preferences.bottom)
        throw std::invalid_argument("Выберите область стрелок внутри окна WARDOGS");
    return {scaled(preferences.left, width), scaled(preferences.top, height),
            scaled(preferences.right, width, true), scaled(preferences.bottom, height, true)};
}

ReloadHelperPreferences select_reload_region(ReloadHelperPreferences preferences,
    int width, int height, ImageRect selection) {
    validate_size(width, height);
    if (selection.left < 0 || selection.top < 0 || selection.right > width ||
        selection.bottom > height || selection.left >= selection.right ||
        selection.top >= selection.bottom)
        throw std::invalid_argument("Выберите область стрелок внутри окна WARDOGS");
    const auto normalize = [](int value, int extent, bool up) {
        return static_cast<int>((static_cast<std::int64_t>(value) * 10000 +
                                 (up ? extent - 1 : 0)) / extent);
    };
    preferences.left = normalize(selection.left, width, false);
    preferences.top = normalize(selection.top, height, false);
    preferences.right = normalize(selection.right, width, true);
    preferences.bottom = normalize(selection.bottom, height, true);
    preferences.automatic_region = false;
    return preferences;
}
}  // namespace wardogs
