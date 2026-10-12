#pragma once

#include "wardogs/ocr.hpp"

#include <Windows.h>

#include <string>
#include <string_view>

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
struct GameClientCapture {
    Image image;
    // Availability time conservatively orders pixels after intervening input.
    // The source's actual QPC timestamp is retained separately for diagnostics.
    std::uint64_t captured_at_ms{};
    std::uint64_t acquisition_started_at_ms{};
    std::uint64_t system_relative_time_100ns{};
    std::string_view backend{"Windows.Graphics.Capture"};
};
// Fresh HWND-targeted compositor capture. Other application overlays never
// become sight evidence. Refuses unavailable graphics capture, stale frames,
// hidden/minimized or changing clients; never falls back to a cached client DC.
Image capture_game_client(HWND window, std::stop_token stop = {});
GameClientCapture capture_game_client_frame(HWND window, std::stop_token stop = {});

}  // namespace wardogs
