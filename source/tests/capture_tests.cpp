#include "wardogs/capture.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>

// This is an implementation detail used before any GDI acquisition. Keeping
// its declaration here avoids expanding the public capture API for tests.
namespace wardogs::detail {
std::size_t checked_capture_pixel_count(std::int64_t width, std::int64_t height);
}

namespace {
int failures = 0;
void check(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}
template <typename Callback>
void rejects(Callback&& callback, const char* message) {
    bool rejected = false;
    try { callback(); }
    catch (const std::exception&) { rejected = true; }
    check(rejected, message);
}
bool same_rect(RECT value, RECT expected) {
    return value.left == expected.left && value.top == expected.top &&
           value.right == expected.right && value.bottom == expected.bottom;
}
bool inside(RECT value, RECT client) {
    return value.left >= client.left && value.top >= client.top &&
           value.right <= client.right && value.bottom <= client.bottom &&
           value.left < value.right && value.top < value.bottom;
}
}  // namespace

int main() {
    using wardogs::detail::checked_capture_pixel_count;
    check(checked_capture_pixel_count(1, 1) == 1 &&
              checked_capture_pixel_count(4000, 4000) == 16'000'000,
          "capture accepts ordinary sizes and the exact 16 MP OCR boundary");
    rejects([] { checked_capture_pixel_count(0, 10); },
            "an empty capture cannot allocate resources");
    rejects([] { checked_capture_pixel_count(-1, 10); },
            "negative capture geometry is rejected");
    rejects([] { checked_capture_pixel_count(4000, 4001); },
            "one row beyond the OCR boundary is rejected");
    rejects([] { checked_capture_pixel_count(16'000'001, 1); },
            "oversized skinny input is rejected before multiplication");
    rejects([] { checked_capture_pixel_count(16385, 1); },
            "capture and OCR use the same maximum physical dimension");
    const auto chat = wardogs::make_chat_search_rect({-1920, -100, 0, 980});
    check(chat.left == -1920 && chat.top == -100 && chat.right == -960 && chat.bottom == 440,
          "chat search follows physical client coordinates on a negative-origin monitor");
    const auto odd_chat = wardogs::make_chat_search_rect({200, 100, 1501, 901});
    check(odd_chat.right == 851 && odd_chat.bottom == 501,
          "odd-size game clients retain the last half pixel without crossing the client");
    rejects([] { (void)wardogs::make_chat_search_rect({4, 4, 2, 2}); },
            "an inverted game client cannot start an automatic screen search");
    rejects([] {
        checked_capture_pixel_count(std::numeric_limits<std::int64_t>::max(),
                                     std::numeric_limits<std::int64_t>::max());
    }, "huge dimensions cannot overflow a signed pixel product");
    rejects([] {
        checked_capture_pixel_count(
            static_cast<std::int64_t>(std::numeric_limits<LONG>::max()) -
                std::numeric_limits<LONG>::min(), 2);
    }, "extreme rectangle subtraction is checked with wide arithmetic");

    const auto map_1080 = wardogs::make_map_coordinate_rects({0, 0, 1920, 1080}, {700, 500});
    check(same_rect(map_1080.x_field, {720, 454, 860, 512}) &&
              same_rect(map_1080.y_field, {688, 380, 840, 448}),
          "1080p map fields preserve the original X and Y source ordering");
    const auto live_map = wardogs::make_map_coordinate_rects({0, 0, 1472, 1149}, {475, 365});
    check(same_rect(live_map.x_field, {495, 319, 635, 377}) &&
              same_rect(live_map.y_field, {463, 245, 615, 313}),
          "real ping screenshot fields include labeled X/Y without reading chat or player names");
    const auto map_1440 = wardogs::make_map_coordinate_rects({0, 0, 2560, 1440}, {1000, 700}, 4.0 / 3.0);
    check(same_rect(map_1440.x_field, {1027, 639, 1213, 716}) &&
              same_rect(map_1440.y_field, {984, 540, 1187, 631}),
          "1440p optional scale rounds cursor offsets to physical pixels");
    const auto map_4k = wardogs::make_map_coordinate_rects({0, 0, 3840, 2160}, {2000, 1000}, 2.0);
    check(same_rect(map_4k.x_field, {2040, 908, 2320, 1024}) &&
              same_rect(map_4k.y_field, {1976, 760, 2280, 896}),
          "4K optional scale doubles both coordinate fields without swapping axes");
    const RECT negative_client{-2000, -1000, -80, 80};
    const auto map_negative = wardogs::make_map_coordinate_rects(negative_client, {-1300, -500});
    check(same_rect(map_negative.x_field, {-1280, -546, -1140, -488}) &&
              same_rect(map_negative.y_field, {-1312, -620, -1160, -552}),
          "map fields use negative screen origins without unsigned conversion");
    for (const POINT cursor : {POINT{-2000, -1000}, POINT{-81, -1000},
                               POINT{-2000, 79}, POINT{-81, 79}}) {
        const auto fields = wardogs::make_map_coordinate_rects(negative_client, cursor);
        check(inside(fields.x_field, negative_client) && inside(fields.y_field, negative_client) &&
                  fields.x_field.right - fields.x_field.left == 140 &&
                  fields.x_field.bottom - fields.x_field.top == 58 &&
                  fields.y_field.right - fields.y_field.left == 152 &&
                  fields.y_field.bottom - fields.y_field.top == 68,
              "edge capture shifts complete fields into the client without exposing another app");
    }
    const auto map_exact = wardogs::make_map_coordinate_rects({40, 30, 192, 98}, {50, 40});
    check(same_rect(map_exact.y_field, {40, 30, 192, 98}) &&
              inside(map_exact.x_field, {40, 30, 192, 98}),
          "a client exactly fitting the larger field remains usable");
    const LONG long_min = std::numeric_limits<LONG>::min();
    const LONG long_max = std::numeric_limits<LONG>::max();
    for (const RECT client : {RECT{long_min, long_min, long_min + 500, long_min + 500},
                              RECT{long_max - 500, long_max - 500, long_max, long_max},
                              RECT{long_min, long_min, long_max, long_max}}) {
        const POINT cursor{client.right - 1, client.bottom - 1};
        const auto fields = wardogs::make_map_coordinate_rects(client, cursor, 2.0);
        check(inside(fields.x_field, client) && inside(fields.y_field, client),
              "extreme signed coordinates cannot overflow cursor offset or client subtraction");
    }
    for (const POINT cursor : {POINT{-1, 50}, POINT{50, -1}, POINT{1920, 50}, POINT{50, 1080}})
        rejects([&] { wardogs::make_map_coordinate_rects({0, 0, 1920, 1080}, cursor); },
                "cursor outside any client edge is rejected before capturing");
    for (const RECT client : {RECT{0, 0, 0, 100}, RECT{100, 0, 20, 100}, RECT{0, 40, 100, 20}})
        rejects([&] { wardogs::make_map_coordinate_rects(client, {10, 10}); },
                "empty or inverted map client geometry is rejected");
    for (const RECT client : {RECT{0, 0, 151, 68}, RECT{0, 0, 152, 67}})
        rejects([&] { wardogs::make_map_coordinate_rects(client, {10, 10}); },
                "both complete map fields must fit without cropping numbers");
    for (const double scale : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                               -std::numeric_limits<double>::infinity(),
                               std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::max(), 0.0001, 200.0})
        rejects([&] { wardogs::make_map_coordinate_rects({long_min, long_min, long_max, long_max},
                                                        {0, 0}, scale); },
                "invalid, empty, overflowed and oversized scaled fields are rejected");

    // This small capture checks native monitor resolution and real GDI cleanup.
    // Pixel contents are neither saved nor printed, and input is never changed.
    const HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!monitor || !GetMonitorInfoW(monitor, &info)) {
        std::cerr << "FAIL: capture integration needs an active Windows monitor\n";
        return 1;
    }
    try {
        const LONG width = std::min<LONG>(16, info.rcMonitor.right - info.rcMonitor.left);
        const LONG height = std::min<LONG>(16, info.rcMonitor.bottom - info.rcMonitor.top);
        const RECT selected{info.rcMonitor.left, info.rcMonitor.top,
                             info.rcMonitor.left + width, info.rcMonitor.top + height};
        const auto region = wardogs::make_capture_region(monitor, selected);
        check(region.monitor_size.cx == info.rcMonitor.right - info.rcMonitor.left &&
                  region.monitor_size.cy == info.rcMonitor.bottom - info.rcMonitor.top,
              "selection records physical monitor dimensions for resolution change checks");
        const RECT resolved = wardogs::resolve_capture_region(region);
        check(resolved.left == selected.left && resolved.top == selected.top &&
                  resolved.right == selected.right && resolved.bottom == selected.bottom,
              "a monitor-relative capture region resolves back to its screen rectangle");
        const auto reversed = wardogs::make_capture_region(
            monitor, RECT{selected.right, selected.bottom, selected.left, selected.top});
        check(reversed.relative.left == region.relative.left &&
                  reversed.relative.right == region.relative.right,
              "reverse dragging keeps the same normalized capture geometry");
        rejects([&] { wardogs::make_capture_region(monitor, selected, -1); },
                "negative padding cannot shrink or invert a selected region");
        const auto padded = wardogs::make_capture_region(
            monitor, selected, std::numeric_limits<int>::max());
        check(padded.relative.left == 0 && padded.relative.top == 0 &&
                  padded.relative.right == info.rcMonitor.right - info.rcMonitor.left &&
                  padded.relative.bottom == info.rcMonitor.bottom - info.rcMonitor.top,
              "extreme padding clamps to the monitor without signed overflow");
        rejects([&] {
            wardogs::capture_screen(wardogs::CaptureRegion{info.szDevice, {4, 4, 2, 2}});
        }, "inverted persisted rectangles are rejected before GDI creation");
        rejects([&] {
            wardogs::capture_screen(wardogs::CaptureRegion{
                info.szDevice, {0, 0, std::numeric_limits<LONG>::max(), 16}});
        }, "persisted rectangles outside the monitor cannot allocate a giant bitmap");
        rejects([] {
            wardogs::capture_screen(wardogs::CaptureRegion{
                L"WARDOGS-NOT-A-CONNECTED-MONITOR", {0, 0, 16, 16}});
        }, "a disconnected monitor produces an explicit capture failure");
        auto changed_resolution = region;
        ++changed_resolution.monitor_size.cx;
        rejects([&] { (void)wardogs::resolve_capture_region(changed_resolution); },
                "a changed monitor resolution cannot silently target an old coordinate area");

        const auto warmup = wardogs::capture_screen(region);
        check(warmup.width == width && warmup.height == height &&
                  warmup.bgr.size() == static_cast<std::size_t>(width) * height * 3,
              "native screen capture returns a tightly packed three-channel image");
        const DWORD before = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        for (int repetition = 0; repetition < 32; ++repetition) {
            const auto image = wardogs::capture_screen(region);
            check(image.bgr.size() == warmup.bgr.size(),
                  "repeated capture retains the exact output dimensions");
        }
        const DWORD after = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        check(after == before,
              "repeated native capture releases all process-owned GDI handles");
    } catch (const std::exception& error) {
        std::cerr << "FAIL: native capture integration: " << error.what() << '\n';
        return 1;
    }
    if (failures) return 1;
    std::cout << "All capture bounds and native GDI lifecycle tests passed\n";
    return 0;
}
