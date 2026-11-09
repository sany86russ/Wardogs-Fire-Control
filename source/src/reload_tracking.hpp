#pragma once

#include "wardogs/ocr.hpp"

#include <cstdint>
#include <optional>

namespace wardogs_ui {

// Physical client coordinates. Window movement does not move its HUD within
// the client; a different window, client size or selected search region does.
struct ReloadRegionKey {
    std::uintptr_t window{};
    int width{}, height{};
    wardogs::ImageRect search{};
};

class ReloadRegionTracker {
public:
    [[nodiscard]] std::optional<wardogs::ImageRect> lookup(const ReloadRegionKey& key) const noexcept {
        if (!record_ || record_->key.window != key.window || record_->key.width != key.width ||
            record_->key.height != key.height || !same(record_->key.search, key.search)) return std::nullopt;
        return record_->region;
    }

    // Cache position only, never arrow directions or a previous recognition.
    // Invalid geometry clears the cache rather than narrowing future captures.
    bool remember(const ReloadRegionKey& key, wardogs::ImageRect region) noexcept {
        if (!key.window || key.width <= 0 || key.height <= 0 ||
            !inside(key.search, {0, 0, key.width, key.height}) || !inside(region, key.search)) {
            clear();
            return false;
        }
        record_ = Record{key, region};
        return true;
    }

    void clear() noexcept { record_.reset(); }

private:
    struct Record { ReloadRegionKey key; wardogs::ImageRect region; };
    std::optional<Record> record_;

    static bool same(wardogs::ImageRect a, wardogs::ImageRect b) noexcept {
        return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
    }
    static bool inside(wardogs::ImageRect a, wardogs::ImageRect b) noexcept {
        return a.left >= b.left && a.top >= b.top && a.right <= b.right && a.bottom <= b.bottom &&
            a.left < a.right && a.top < a.bottom;
    }
};

}  // namespace wardogs_ui
