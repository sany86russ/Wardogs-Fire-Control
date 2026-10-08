#include "selection_overlay.hpp"
#include "localization.hpp"

#include <QImage>

#include <windowsx.h>

#include <algorithm>
#include <cstdlib>
#include <exception>

namespace {

struct PreviewSurface {
    HDC dc{};
    HBITMAP bitmap{};
    HGDIOBJ previous_bitmap{};

    ~PreviewSurface() {
        if (dc && previous_bitmap && previous_bitmap != HGDI_ERROR)
            SelectObject(dc, previous_bitmap);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
    }
};

QString error_text(const std::exception& error) {
    return wardogs::i18n::text(QString::fromUtf8(error.what()));
}

}  // namespace

SelectionOverlay::~SelectionOverlay() {
    callback_ = {};
    cancel();
}

bool SelectionOverlay::begin(Callback callback) {
    if (window_) return false;
    selecting_ = false;
    start_ = {};
    end_ = {};
    monitor_ = nullptr;
    callback_ = std::move(callback);
    ensure_class();
    const int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    const auto title = wardogs::i18n::text(QStringLiteral("Выбор области координат")).toStdWString();
    window_ = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED, class_name,
        title.c_str(), WS_POPUP, left, top, width, height, nullptr, nullptr,
        GetModuleHandleW(nullptr), this);
    if (!window_) {
        callback_ = {};
        return false;
    }
    SetLayeredWindowAttributes(window_, 0, 108, LWA_ALPHA);
    ShowWindow(window_, SW_SHOW);
    SetForegroundWindow(window_);
    SetCursor(LoadCursorW(nullptr, IDC_CROSS));
    SetCapture(window_);
    return true;
}

void SelectionOverlay::cancel() {
    if (!window_) return;
    finish(std::nullopt);
}

bool SelectionOverlay::export_preview(const QString& path) const {
    if (!window_ || path.isEmpty()) return false;
    RECT client{};
    if (!GetClientRect(window_, &client)) return false;
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) return false;

    PreviewSurface surface;
    surface.dc = CreateCompatibleDC(nullptr);
    if (!surface.dc) return false;
    BITMAPINFO format{};
    format.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    format.bmiHeader.biWidth = width;
    format.bmiHeader.biHeight = -height;
    format.bmiHeader.biPlanes = 1;
    format.bmiHeader.biBitCount = 32;
    format.bmiHeader.biCompression = BI_RGB;
    void* pixels{};
    surface.bitmap = CreateDIBSection(surface.dc, &format, DIB_RGB_COLORS,
                                      &pixels, nullptr, 0);
    if (!surface.bitmap || !pixels) return false;
    surface.previous_bitmap = SelectObject(surface.dc, surface.bitmap);
    if (!surface.previous_bitmap || surface.previous_bitmap == HGDI_ERROR)
        return false;

    paint_to(surface.dc, client);
    GdiFlush();
    const QImage rendered(static_cast<const uchar*>(pixels), width, height,
                          width * 4, QImage::Format_RGB32);
    // RGB888 explicitly produces an opaque PNG regardless of the reserved
    // high byte written by native GDI into the 32-bit DIB.
    return rendered.convertToFormat(QImage::Format_RGB888).save(path, "PNG");
}

void SelectionOverlay::paint_to(HDC dc, const RECT& paint_rect) const {
    FillRect(dc, &paint_rect, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    POINT cursor{};
    GetCursorPos(&cursor);
    MONITORINFO active_monitor{sizeof(active_monitor)};
    if (GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST),
                       &active_monitor)) {
        POINT origin{};
        ClientToScreen(window_, &origin);
        RECT hint = active_monitor.rcMonitor;
        OffsetRect(&hint, -origin.x, -origin.y);
        hint.left += 24;
        hint.right -= 24;
        hint.top += 26;
        hint.bottom = hint.top + 80;
        const HFONT font = CreateFontW(
            -MulDiv(16, GetDpiForWindow(window_), 96), 0, 0, 0,
            FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        const auto old_font = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(232, 238, 247));
        const auto instruction = wardogs::i18n::text(QStringLiteral(
            "Выделите текст координат X / Y\nEsc или правая кнопка — отмена")).toStdWString();
        DrawTextW(dc, instruction.c_str(),
                  -1, &hint, DT_CENTER | DT_TOP | DT_WORDBREAK);
        SelectObject(dc, old_font);
        DeleteObject(font);
    }
    if (selecting_) {
        RECT rect{start_.x, start_.y, end_.x, end_.y};
        POINT origin{};
        ClientToScreen(window_, &origin);
        OffsetRect(&rect, -origin.x, -origin.y);
        HPEN pen = CreatePen(PS_SOLID, 3, RGB(99, 216, 197));
        HGDIOBJ old_pen = SelectObject(dc, pen);
        HGDIOBJ old_brush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);
        SelectObject(dc, old_brush);
        SelectObject(dc, old_pen);
        DeleteObject(pen);
    }
}

void SelectionOverlay::ensure_class() {
    static const ATOM atom = [] {
        WNDCLASSEXW value{sizeof(value)};
        value.style = CS_HREDRAW | CS_VREDRAW;
        value.lpfnWndProc = window_proc;
        value.hInstance = GetModuleHandleW(nullptr);
        value.hCursor = LoadCursorW(nullptr, IDC_CROSS);
        value.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        value.lpszClassName = class_name;
        return RegisterClassExW(&value);
    }();
    (void)atom;
}

void SelectionOverlay::finish(std::optional<wardogs::CaptureRegion> region,
                              QString error) {
    auto callback = std::move(callback_);
    callback_ = {};
    selecting_ = false;
    start_ = {};
    end_ = {};
    monitor_ = nullptr;
    if (window_ && GetCapture() == window_) ReleaseCapture();
    if (window_) DestroyWindow(window_);
    if (callback) callback(std::move(region), std::move(error));
}

LRESULT SelectionOverlay::handle(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_NCHITTEST:
            return HTCLIENT;
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, IDC_CROSS));
            return TRUE;
        case WM_LBUTTONDOWN:
            selecting_ = true;
            start_ = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ClientToScreen(window_, &start_);
            end_ = start_;
            monitor_ = MonitorFromPoint(start_, MONITOR_DEFAULTTONEAREST);
            SetCapture(window_);
            return 0;
        case WM_MOUSEMOVE:
            if (selecting_) {
                POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                ClientToScreen(window_, &point);
                MONITORINFO info{sizeof(info)};
                GetMonitorInfoW(monitor_, &info);
                point.x = std::clamp(point.x, info.rcMonitor.left,
                                     info.rcMonitor.right);
                point.y = std::clamp(point.y, info.rcMonitor.top,
                                     info.rcMonitor.bottom);
                end_ = point;
                InvalidateRect(window_, nullptr, TRUE);
            }
            return 0;
        case WM_LBUTTONUP:
            if (selecting_) {
                selecting_ = false;
                ReleaseCapture();
                RECT selected{start_.x, start_.y, end_.x, end_.y};
                if (std::abs(selected.right - selected.left) < 8 ||
                    std::abs(selected.bottom - selected.top) < 8) {
                    finish(std::nullopt,
                           wardogs::i18n::text(QStringLiteral("Область слишком мала. Выберите текст координат целиком.")));
                    return 0;
                }
                try {
                    finish(wardogs::make_capture_region(monitor_, selected));
                } catch (const std::exception& error) {
                    finish(std::nullopt, error_text(error));
                }
            }
            return 0;
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE) {
                cancel();
            }
            return 0;
        case WM_RBUTTONDOWN:
            cancel();
            return 0;
        case WM_CLOSE:
            cancel();
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window_, &paint);
            paint_to(dc, paint.rcPaint);
            EndPaint(window_, &paint);
            return 0;
        }
        case WM_DESTROY:
            window_ = nullptr;
            return 0;
    }
    return DefWindowProcW(window_, message, wparam, lparam);
}

LRESULT CALLBACK SelectionOverlay::window_proc(HWND window, UINT message,
                                                WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<SelectionOverlay*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<SelectionOverlay*>(create->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->handle(message, wparam, lparam)
                : DefWindowProcW(window, message, wparam, lparam);
}
