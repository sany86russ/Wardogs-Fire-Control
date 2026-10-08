#include "wardogs/capture.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace wardogs {
namespace detail {

// OCR rejects inputs above the same 16 MP limit. Check before multiplying,
// allocating the output buffer, or acquiring any scarce GDI resources.
std::size_t checked_capture_pixel_count(std::int64_t width, std::int64_t height) {
    constexpr std::int64_t maximum_pixels = 16'000'000;
    if (width <= 0 || height <= 0)
        throw std::invalid_argument("Область захвата пуста");
    if (width > 16384 || height > 16384 ||
        width > maximum_pixels / height)
        throw std::invalid_argument("Область захвата слишком велика: до 16 млн пикселей и 16384 по каждой стороне.");
    return static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
}

}  // namespace detail
namespace {

MONITORINFOEXW monitor_info(HMONITOR monitor) {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
        throw std::runtime_error("Не удалось прочитать параметры монитора");
    }
    return info;
}

struct SearchContext {
    const std::wstring* device;
    HMONITOR result{};
    bool enumeration_failed{};
};

BOOL CALLBACK find_monitor(HMONITOR monitor, HDC, LPRECT, LPARAM data) noexcept {
    auto& context = *reinterpret_cast<SearchContext*>(data);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    // A C++ exception must never escape a callback called by Win32.
    if (!GetMonitorInfoW(monitor, &info)) {
        context.enumeration_failed = true;
        return FALSE;
    }
    if (*context.device == info.szDevice) {
        context.result = monitor;
        return FALSE;
    }
    return TRUE;
}

struct CaptureGdiResources {
    HDC screen{};
    HDC memory{};
    HBITMAP bitmap{};
    HGDIOBJ previous{};

    CaptureGdiResources() = default;
    CaptureGdiResources(const CaptureGdiResources&) = delete;
    CaptureGdiResources& operator=(const CaptureGdiResources&) = delete;
    ~CaptureGdiResources() {
        if (memory && previous) SelectObject(memory, previous);
        if (bitmap) DeleteObject(bitmap);
        if (memory) DeleteDC(memory);
        if (screen) ReleaseDC(nullptr, screen);
    }
};

}  // namespace

CaptureRegion make_capture_region(HMONITOR monitor, RECT virtual_rect, int padding) {
    if (padding < 0)
        throw std::invalid_argument("Отступ области захвата не может быть отрицательным.");
    const auto info = monitor_info(monitor);
    RECT selected{
        std::min(virtual_rect.left, virtual_rect.right),
        std::min(virtual_rect.top, virtual_rect.bottom),
        std::max(virtual_rect.left, virtual_rect.right),
        std::max(virtual_rect.top, virtual_rect.bottom),
    };
    selected.left = static_cast<LONG>(std::max<std::int64_t>(
        info.rcMonitor.left, static_cast<std::int64_t>(selected.left) - padding));
    selected.top = static_cast<LONG>(std::max<std::int64_t>(
        info.rcMonitor.top, static_cast<std::int64_t>(selected.top) - padding));
    selected.right = static_cast<LONG>(std::min<std::int64_t>(
        info.rcMonitor.right, static_cast<std::int64_t>(selected.right) + padding));
    selected.bottom = static_cast<LONG>(std::min<std::int64_t>(
        info.rcMonitor.bottom, static_cast<std::int64_t>(selected.bottom) + padding));
    if (selected.right <= selected.left || selected.bottom <= selected.top) {
        throw std::invalid_argument("Область захвата пуста");
    }
    return {info.szDevice,
            {selected.left - info.rcMonitor.left, selected.top - info.rcMonitor.top,
             selected.right - info.rcMonitor.left,
             selected.bottom - info.rcMonitor.top},
            {info.rcMonitor.right - info.rcMonitor.left,
             info.rcMonitor.bottom - info.rcMonitor.top}};
}

RECT make_chat_search_rect(RECT client_rect) {
    const auto width = static_cast<std::int64_t>(client_rect.right) - client_rect.left;
    const auto height = static_cast<std::int64_t>(client_rect.bottom) - client_rect.top;
    detail::checked_capture_pixel_count((width + 1) / 2, (height + 1) / 2);
    if (width <= 0 || height <= 0)
        throw std::invalid_argument("Окно игры не имеет видимой области для поиска координат.");
    return {client_rect.left, client_rect.top,
            static_cast<LONG>(static_cast<std::int64_t>(client_rect.left) + (width + 1) / 2),
            static_cast<LONG>(static_cast<std::int64_t>(client_rect.top) + (height + 1) / 2)};
}

MapCoordinateRects make_map_coordinate_rects(RECT client_rect, POINT cursor,
                                             double scale) {
    const auto client_width = static_cast<std::int64_t>(client_rect.right) - client_rect.left;
    const auto client_height = static_cast<std::int64_t>(client_rect.bottom) - client_rect.top;
    if (client_width <= 0 || client_height <= 0)
        throw std::invalid_argument("Окно игры не имеет области для чтения координат карты.");
    if (cursor.x < client_rect.left || cursor.x >= client_rect.right ||
        cursor.y < client_rect.top || cursor.y >= client_rect.bottom)
        throw std::invalid_argument("Точка карты находится за пределами окна игры.");
    if (!std::isfinite(scale) || scale <= 0.0)
        throw std::invalid_argument("Масштаб подписей карты должен быть положительным и конечным.");

    const auto offset = [scale](int value) -> std::int64_t {
        const auto scaled = std::round(static_cast<long double>(value) * scale);
        // Bound before converting floating point to an integer. Offsets larger
        // than a LONG cannot describe a supported OCR field anyway.
        if (!std::isfinite(scaled) ||
            scaled < std::numeric_limits<LONG>::min() ||
            scaled > std::numeric_limits<LONG>::max())
            throw std::invalid_argument("Масштаб области координат карты слишком велик.");
        return static_cast<std::int64_t>(scaled);
    };
    const auto field = [&](int left, int top, int right, int bottom) -> RECT {
        const auto dx = offset(left);
        const auto dy = offset(top);
        const auto width = offset(right) - dx;
        const auto height = offset(bottom) - dy;
        detail::checked_capture_pixel_count(width, height);
        if (width > client_width || height > client_height)
            throw std::invalid_argument("Окно игры слишком мало для полной подписи координат карты.");
        const auto x = std::clamp(static_cast<std::int64_t>(cursor.x) + dx,
                                 static_cast<std::int64_t>(client_rect.left),
                                 static_cast<std::int64_t>(client_rect.right) - width);
        const auto y = std::clamp(static_cast<std::int64_t>(cursor.y) + dy,
                                 static_cast<std::int64_t>(client_rect.top),
                                 static_cast<std::int64_t>(client_rect.bottom) - height);
        return {static_cast<LONG>(x), static_cast<LONG>(y),
                static_cast<LONG>(x + width), static_cast<LONG>(y + height)};
    };
    // The original application reads X from the lower/right field, Y from
    // the upper field. Never infer the axis from an OCR result's number order.
    return {field(20, -46, 160, 12), field(-12, -120, 140, -52)};
}

RECT resolve_capture_region(const CaptureRegion& region) {
    SearchContext context{&region.monitor_device, nullptr, false};
    const BOOL enumerated = EnumDisplayMonitors(nullptr, nullptr, find_monitor,
                                                reinterpret_cast<LPARAM>(&context));
    if (context.enumeration_failed || (!enumerated && !context.result))
        throw std::runtime_error("Не удалось получить список мониторов.");
    if (!context.result) {
        throw std::runtime_error("Выбранный монитор отключён");
    }
    const auto info = monitor_info(context.result);
    const RECT& value = region.relative;
    const auto monitor_width = static_cast<std::int64_t>(info.rcMonitor.right) - info.rcMonitor.left;
    const auto monitor_height = static_cast<std::int64_t>(info.rcMonitor.bottom) - info.rcMonitor.top;
    if ((region.monitor_size.cx != 0 || region.monitor_size.cy != 0) &&
        (region.monitor_size.cx != monitor_width || region.monitor_size.cy != monitor_height))
        throw std::runtime_error("Разрешение выбранного монитора изменилось. Выберите область заново или используйте поиск в чате.");
    if (value.left < 0 || value.top < 0 || value.right > monitor_width ||
        value.bottom > monitor_height ||
        value.right <= value.left || value.bottom <= value.top) {
        throw std::runtime_error("Область больше не помещается на мониторе. Выберите её заново.");
    }
    return {info.rcMonitor.left + value.left, info.rcMonitor.top + value.top,
            info.rcMonitor.left + value.right, info.rcMonitor.top + value.bottom};
}

Image capture_screen(const CaptureRegion& region) {
    const RECT rect = resolve_capture_region(region);
    const auto wide_width = static_cast<std::int64_t>(rect.right) - rect.left;
    const auto wide_height = static_cast<std::int64_t>(rect.bottom) - rect.top;
    const auto pixel_count = detail::checked_capture_pixel_count(wide_width, wide_height);
    const auto width = static_cast<int>(wide_width);
    const auto height = static_cast<int>(wide_height);
    Image image{width, height, std::vector<std::uint8_t>(pixel_count * 3)};
    CaptureGdiResources resources;
    resources.screen = GetDC(nullptr);
    if (!resources.screen)
        throw std::runtime_error("Не удалось открыть экран для захвата.");
    resources.memory = CreateCompatibleDC(resources.screen);
    if (!resources.memory)
        throw std::runtime_error("Не удалось создать контекст захвата.");
    BITMAPINFO bitmap_info{};
    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth = width;
    bitmap_info.bmiHeader.biHeight = -height;
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    resources.bitmap = CreateDIBSection(resources.screen, &bitmap_info, DIB_RGB_COLORS,
                                        &pixels, nullptr, 0);
    if (!resources.bitmap || !pixels) {
        throw std::runtime_error("Не удалось создать изображение для захвата");
    }
    const HGDIOBJ previous = SelectObject(resources.memory, resources.bitmap);
    if (!previous || previous == HGDI_ERROR)
        throw std::runtime_error("Не удалось подготовить буфер захвата.");
    resources.previous = previous;
    if (!BitBlt(resources.memory, 0, 0, width, height, resources.screen,
                rect.left, rect.top, SRCCOPY | CAPTUREBLT)) {
        throw std::runtime_error("Windows не смогла захватить область экрана");
    }
    // Microsoft requires synchronization before accessing DIB pixels directly.
    if (!GdiFlush())
        throw std::runtime_error("Windows не смогла завершить захват экрана.");
    const auto* bgra = static_cast<const std::uint8_t*>(pixels);
    for (std::size_t i = 0, j = 0; j < image.bgr.size(); i += 4, j += 3) {
        image.bgr[j] = bgra[i];
        image.bgr[j + 1] = bgra[i + 1];
        image.bgr[j + 2] = bgra[i + 2];
    }
    return image;
}

}  // namespace wardogs
