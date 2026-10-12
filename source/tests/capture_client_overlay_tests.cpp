#include "wardogs/capture.hpp"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {

int failures{};
constexpr COLORREF left_color = RGB(34, 145, 210);
constexpr COLORREF right_color = RGB(195, 72, 43);
constexpr COLORREF overlay_color = RGB(250, 35, 180);

void check(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

template <typename Callback>
void rejects(Callback&& callback, const char* message) {
    bool rejected{};
    try { callback(); }
    catch (const std::exception&) { rejected = true; }
    check(rejected, message);
}

class TestWindow {
public:
    explicit TestWindow(HWND handle) : handle_(handle) {
        if (!handle_) throw std::runtime_error("Cannot create controlled capture fixture");
    }
    ~TestWindow() { if (handle_) DestroyWindow(handle_); }
    TestWindow(const TestWindow&) = delete;
    TestWindow& operator=(const TestWindow&) = delete;
    HWND get() const noexcept { return handle_; }
    HWND destroy() noexcept {
        const auto old = std::exchange(handle_, nullptr);
        if (old) DestroyWindow(old);
        return old;
    }
private:
    HWND handle_{};
};

LRESULT CALLBACK fixture_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA,
                         reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        const auto dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        const bool overlay = GetWindowLongPtrW(window, GWLP_USERDATA) != 0;
        const auto first = CreateSolidBrush(overlay ? overlay_color : left_color);
        if (first) {
            FillRect(dc, &client, first);
            DeleteObject(first);
        }
        if (!overlay) {
            const auto second = CreateSolidBrush(right_color);
            RECT right{client.right / 2, 0, client.right, client.bottom};
            if (second) {
                FillRect(dc, &right, second);
                DeleteObject(second);
            }
        }
        EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

class FixtureClass {
public:
    FixtureClass() {
        WNDCLASSW value{};
        value.lpfnWndProc = fixture_proc;
        value.hInstance = GetModuleHandleW(nullptr);
        value.lpszClassName = name;
        if (!RegisterClassW(&value)) throw std::runtime_error("Cannot register capture fixture class");
    }
    ~FixtureClass() { UnregisterClassW(name, GetModuleHandleW(nullptr)); }
    static constexpr const wchar_t* name = L"WardogsClientCaptureOverlayFixture";
};

class CountingBrush {
public:
    CountingBrush() : handle_(CreateSolidBrush(RGB(1, 2, 3))) {
        if (!handle_) throw std::runtime_error("Cannot acquire GDI counting sentinel");
    }
    ~CountingBrush() { DeleteObject(handle_); }
    CountingBrush(const CountingBrush&) = delete;
    CountingBrush& operator=(const CountingBrush&) = delete;
private:
    HBRUSH handle_{};
};

void pump_messages() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

bool screen_pixel_matches(POINT point, COLORREF expected) {
    const auto dc = GetDC(nullptr);
    if (!dc) return false;
    const auto actual = GetPixel(dc, point.x, point.y);
    ReleaseDC(nullptr, dc);
    return actual != CLR_INVALID &&
        std::abs(static_cast<int>(GetRValue(actual)) - GetRValue(expected)) <= 2 &&
        std::abs(static_cast<int>(GetGValue(actual)) - GetGValue(expected)) <= 2 &&
        std::abs(static_cast<int>(GetBValue(actual)) - GetBValue(expected)) <= 2;
}

bool image_pixel_matches(const wardogs::Image& image, int x, int y, COLORREF expected) {
    if (x < 0 || y < 0 || x >= image.width || y >= image.height ||
        image.bgr.size() != static_cast<std::size_t>(image.width) * image.height * 3) return false;
    const auto index = (static_cast<std::size_t>(y) * image.width + x) * 3;
    return image.bgr[index] == GetBValue(expected) &&
        image.bgr[index + 1] == GetGValue(expected) &&
        image.bgr[index + 2] == GetRValue(expected);
}

std::uint64_t current_qpc_100ns() {
    LARGE_INTEGER counter{}, frequency{};
    if (!QueryPerformanceCounter(&counter) || !QueryPerformanceFrequency(&frequency) ||
        counter.QuadPart <= 0 || frequency.QuadPart <= 0)
        throw std::runtime_error("Cannot verify the returned fixture frame clock");
    return static_cast<std::uint64_t>(static_cast<long double>(counter.QuadPart) *
                                      10'000'000 / frequency.QuadPart);
}

wardogs::GameClientCapture capture_client(HWND window) {
    // Cold initialization can consume the budget or leave too little time for
    // the first frame. Production polling retries with the same healthy cache.
    // Each attempt must still prove fresh pixels; never retry stale frames,
    // invalid clocks, parser/geometry errors or a readback failure.
    for (int attempt = 1; attempt <= 3; ++attempt) {
        try {
            auto captured = wardogs::capture_game_client_frame(window);
            check(captured.system_relative_time_100ns > 0 &&
                  captured.system_relative_time_100ns <= current_qpc_100ns(),
                  "every fixture frame source clock has been reached before its return");
            check(captured.acquisition_started_at_ms <= captured.captured_at_ms &&
                  captured.captured_at_ms <= GetTickCount64(),
                  "fixture acquisition and availability stamps retain their real chronological bounds");
            return captured;
        }
        catch (const std::runtime_error& error) {
            const std::string_view reason{error.what()};
            const bool cold_init = reason.starts_with(
                "Fresh game compositor capture exceeded 250 ms stage=init ");
            const bool cold_frame = reason.starts_with(
                "Fresh game compositor capture exceeded 250 ms stage=frame ") &&
                reason.find(" frames_received=0 stale_frames=0 ") != std::string_view::npos;
            if (attempt == 3 || (!cold_init && !cold_frame))
                throw;
            std::cerr << "capture_warmup_refused attempt=" << attempt << " reason=" << error.what() << '\n';
        }
    }
    throw std::runtime_error("Capture initialization did not produce a fresh fixture frame");
}

void check_client_pattern(const wardogs::Image& image, RECT client) {
    check(image.width == client.right && image.height == client.bottom,
          "capture returns the exact full physical client dimensions");
    for (const auto point : {POINT{client.right / 4, client.bottom / 4},
                             POINT{client.right / 4, client.bottom * 3 / 4}})
        check(image_pixel_matches(image, point.x, point.y, left_color),
              "left game-client pixels exclude the covering layered overlay");
    for (const auto point : {POINT{client.right * 3 / 4, client.bottom / 4},
                             POINT{client.right * 3 / 4, client.bottom * 3 / 4}})
        check(image_pixel_matches(image, point.x, point.y, right_color),
              "right game-client pixels retain their own source pattern");
}

}  // namespace

int main() {
    // This creates only isolated native CI fixtures. Never run this test on
    // the owner's desktop when GUI/game interaction is prohibited.
    try {
        FixtureClass registered;
        CountingBrush counting_sentinel;
        RECT work{};
        if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0))
            throw std::runtime_error("Cannot obtain the isolated CI work area");
        TestWindow game(CreateWindowExW(0, FixtureClass::name, L"Capture source fixture",
            WS_POPUP, work.left + 20, work.top + 20, 800, 480,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr));
        ShowWindow(game.get(), SW_SHOWNORMAL);
        UpdateWindow(game.get());
        SetForegroundWindow(game.get());
        pump_messages();
        check(GetForegroundWindow() == game.get(), "controlled source fixture owns foreground");
        RECT client{};
        if (!GetClientRect(game.get(), &client)) throw std::runtime_error("Missing fixture client");
        check_client_pattern(capture_client(game.get()).image, client);

        POINT origin{};
        if (!ClientToScreen(game.get(), &origin)) throw std::runtime_error("Missing fixture origin");
        TestWindow overlay(CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST |
            WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, FixtureClass::name, L"Calculated ruler fixture",
            WS_POPUP, origin.x, origin.y, client.right, client.bottom,
            nullptr, nullptr, GetModuleHandleW(nullptr), reinterpret_cast<void*>(1)));
        if (!SetLayeredWindowAttributes(overlay.get(), 0, 255, LWA_ALPHA))
            throw std::runtime_error("Cannot configure the covering layered fixture");
        ShowWindow(overlay.get(), SW_SHOWNOACTIVATE);
        UpdateWindow(overlay.get());
        SetWindowPos(overlay.get(), HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        const POINT covered{origin.x + client.right / 4, origin.y + client.bottom / 4};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!screen_pixel_matches(covered, overlay_color) &&
               std::chrono::steady_clock::now() < deadline) {
            pump_messages();
            Sleep(10);
        }
        check(screen_pixel_matches(covered, overlay_color),
              "desktop really contains the opaque overlay at the tested client pixel");
        check(GetForegroundWindow() == game.get(), "passive overlay does not steal source foreground");
        const auto covered_frame = capture_client(game.get());
        check_client_pattern(covered_frame.image, client);
        const auto gdi_before = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        auto previous_source_qpc = covered_frame.system_relative_time_100ns;
        for (int iteration = 0; iteration < 32; ++iteration) {
            const auto repeated_frame = capture_client(game.get());
            check(repeated_frame.system_relative_time_100ns > previous_source_qpc,
                  "successive same-window fixture captures have strictly increasing compositor source clocks");
            previous_source_qpc = repeated_frame.system_relative_time_100ns;
            check_client_pattern(repeated_frame.image, client);
        }
        const auto gdi_after = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        check(gdi_before != 0 && gdi_after <= gdi_before,
              "repeated client captures release their GDI buffers and contexts");

        overlay.destroy();
        ShowWindow(game.get(), SW_MINIMIZE);
        pump_messages();
        check(IsIconic(game.get()) != 0, "controlled source is actually minimized");
        rejects([&] { (void)wardogs::capture_game_client(game.get()); },
                "minimized game clients cannot produce HUD evidence");
        ShowWindow(game.get(), SW_RESTORE);
        SetWindowPos(game.get(), nullptr, 0, 0, 720, 400,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        SetForegroundWindow(game.get());
        UpdateWindow(game.get());
        pump_messages();
        GetClientRect(game.get(), &client);
        check_client_pattern(capture_client(game.get()).image, client);
        ShowWindow(game.get(), SW_HIDE);
        rejects([&] { (void)wardogs::capture_game_client(game.get()); },
                "hidden source windows cannot supply HUD evidence");
        const auto destroyed = game.destroy();
        rejects([&] { (void)wardogs::capture_game_client(destroyed); },
                "destroyed source HWND cannot reuse an earlier captured client");
        rejects([] { (void)wardogs::capture_game_client(nullptr); },
                "null source HWND is refused before GDI acquisition");
        // Replacing the source must bind a new capture item even when process,
        // thread, size and position match the previous window. Different ink
        // proves that a cached previous source surface was not reused.
        TestWindow replacement(CreateWindowExW(0, FixtureClass::name, L"Recreated capture source",
            WS_POPUP, work.left + 20, work.top + 20, 720, 400,
            nullptr, nullptr, GetModuleHandleW(nullptr), reinterpret_cast<void*>(1)));
        ShowWindow(replacement.get(), SW_SHOWNORMAL);
        UpdateWindow(replacement.get());
        pump_messages();
        const auto replacement_frame = capture_client(replacement.get());
        check(replacement_frame.image.width == 720 && replacement_frame.image.height == 400 &&
              image_pixel_matches(replacement_frame.image, 360, 200, overlay_color),
              "a replaced source window returns its own fresh content, never the cached previous item");
    } catch (const std::exception& error) {
        ++failures;
        std::cerr << "FAIL: " << error.what() << '\n';
    }
    if (failures == 0) std::cout << "Client overlay capture tests passed\n";
    return failures == 0 ? 0 : 1;
}
