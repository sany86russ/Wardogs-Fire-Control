#pragma once

#include "wardogs/ocr.hpp"

#include <Windows.h>

#include <string>

namespace wardogs {

struct CaptureRegion {
    std::wstring monitor_device;
    RECT relative{};
    // Physical monitor size at selection time. Zero keeps legacy profiles
    // readable; new selections fail explicitly after resolution changes.
    SIZE monitor_size{};
};

CaptureRegion make_capture_region(HMONITOR monitor, RECT virtual_rect, int padding = 0);
// Physical client coordinates; bounded upper-left chat/draft search area.
RECT make_chat_search_rect(RECT client_rect);
struct MapCoordinateRects {
    RECT x_field{};
    RECT y_field{};
};
// Physical screen coordinates. Preserves both original cursor fields while
// keeping the complete capture strictly inside the game's client rectangle.
MapCoordinateRects make_map_coordinate_rects(RECT client_rect, POINT cursor,
                                             double scale = 1.0);
struct MapCoordinateSearch {
    RECT search{};
    MapCoordinateRects preferred{};
};
// One bounded physical screenshot contains both original fields and nearby
// displaced labels. Preferred fields are hints, never independent captures.
MapCoordinateSearch make_map_coordinate_search_rects(RECT client_rect, POINT cursor,
                                                     double scale = 1.0);
RECT resolve_capture_region(const CaptureRegion& region);
Image capture_screen(const CaptureRegion& region);

}  // namespace wardogs
