#include "wardogs/capture.hpp"
#include "wardogs/reload_helper.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using namespace wardogs;
constexpr UINT paint_prefix = WM_APP + 0x41;
constexpr wchar_t fixture_class[] = L"WardogsReloadCaptureFixture";
constexpr std::array directions{ReloadDirection::up, ReloadDirection::right,
                                 ReloadDirection::down, ReloadDirection::down};
int checks{};
int failures{};
void check(bool value, const char* message) {
    ++checks;
    if (!value) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}

// A real screen capture is permitted only after switching to this new,
// otherwise empty desktop. Never falls back to the user's input desktop.
class IsolatedDesktop {
public:
    IsolatedDesktop() {
        original_thread_ = GetThreadDesktop(GetCurrentThreadId());
        original_input_ = OpenInputDesktop(0, FALSE, DESKTOP_SWITCHDESKTOP);
        if (!original_thread_ || !original_input_) {
            close();
            throw std::runtime_error("Native reload capture requires an interactive Windows desktop");
        }
        const auto name = L"WardogsReload-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                          std::to_wstring(GetTickCount64());
        constexpr ACCESS_MASK access = DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS |
            DESKTOP_CREATEWINDOW | DESKTOP_CREATEMENU | DESKTOP_ENUMERATE | DESKTOP_SWITCHDESKTOP;
        test_ = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, access, nullptr);
        if (!test_ || !SetThreadDesktop(test_)) {
            close();
            throw std::runtime_error("Cannot create an isolated reload capture desktop");
        }
        attached_ = true;
        if (!SwitchDesktop(test_)) {
            restore(); close();
            throw std::runtime_error("Cannot activate the isolated reload capture desktop");
        }
        switched_ = true;
    }
    ~IsolatedDesktop() {
        if (!restore()) std::cerr << "FAIL: original desktop restoration failed\n";
        close();
    }
    IsolatedDesktop(const IsolatedDesktop&) = delete;
    IsolatedDesktop& operator=(const IsolatedDesktop&) = delete;
    HDESK handle() const noexcept { return test_; }
    void restore_checked() {
        if (!restore()) throw std::runtime_error("Cannot restore the original Windows desktop");
    }
private:
    bool restore() noexcept {
        bool result = true;
        if (switched_) {
            if (SwitchDesktop(original_input_)) switched_ = false;
            else result = false;
        }
        if (attached_) {
            if (SetThreadDesktop(original_thread_)) attached_ = false;
            else result = false;
        }
        return result;
    }
    void close() noexcept {
        if (test_) { CloseDesktop(test_); test_ = nullptr; }
        if (original_input_) { CloseDesktop(original_input_); original_input_ = nullptr; }
    }
    HDESK original_thread_{}; // Borrowed.
    HDESK original_input_{};
    HDESK test_{};
    bool attached_{};
    bool switched_{};
};

struct Fixture {
    int width{}, height{};
    std::array<std::vector<std::uint8_t>, 6> frames;
    unsigned prefix{};
};

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* fixture = reinterpret_cast<Fixture*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (fixture && message == paint_prefix) {
        if (wparam > 5) return FALSE;
        fixture->prefix = static_cast<unsigned>(wparam);
        InvalidateRect(window, nullptr, FALSE);
        return UpdateWindow(window);
    }
    if (fixture && message == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        BITMAPINFO bitmap{};
        bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmap.bmiHeader.biWidth = fixture->width;
        bitmap.bmiHeader.biHeight = -fixture->height;
        bitmap.bmiHeader.biPlanes = 1;
        bitmap.bmiHeader.biBitCount = 32;
        bitmap.bmiHeader.biCompression = BI_RGB;
        StretchDIBits(dc, 0, 0, fixture->width, fixture->height, 0, 0,
            fixture->width, fixture->height, fixture->frames[fixture->prefix].data(),
            &bitmap, DIB_RGB_COLORS, SRCCOPY);
        EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

struct OwnedWindow {
    HWND value{};
    ~OwnedWindow() { if (value) DestroyWindow(value); }
};

Fixture make_fixture(const Image& original, int width, int height, double scale) {
    Fixture result{width, height};
    // These four boxes annotate the supplied screenshot, not detector output.
    // Recolouring its original pixels gives independent progress evidence.
    constexpr std::array<ImageRect, 4> glyphs{{{127, 183, 145, 205}, {160, 184, 182, 201},
                                             {199, 181, 216, 202}, {234, 181, 251, 203}}};
    const int scaled_width = static_cast<int>(original.width * scale);
    const int scaled_height = static_cast<int>(original.height * scale);
    const int left = (width - scaled_width) / 2;
    const int top = (height - scaled_height) / 2;
    if (left < 0 || top < 0) throw std::runtime_error("Reload fixture does not fit its native window");
    for (unsigned prefix = 0; prefix < result.frames.size(); ++prefix) {
        auto& pixels = result.frames[prefix];
        pixels.assign(static_cast<std::size_t>(width) * height * 4, 235);
        if (prefix == 5) continue; // Mini-game disappeared after confirmed input.
        for (int y = 0; y < scaled_height; ++y) for (int x = 0; x < scaled_width; ++x) {
            const int sx = std::min(original.width - 1, static_cast<int>(x / scale));
            const int sy = std::min(original.height - 1, static_cast<int>(y / scale));
            const auto input = (static_cast<std::size_t>(sy) * original.width + sx) * 3;
            std::array<std::uint8_t, 3> colour{original.bgr[input], original.bgr[input + 1], original.bgr[input + 2]};
            for (unsigned slot = 0; slot < glyphs.size(); ++slot) {
                const auto box = glyphs[slot];
                if (sx < box.left || sx >= box.right || sy < box.top || sy >= box.bottom) continue;
                const auto [minimum, maximum] = std::minmax_element(colour.begin(), colour.end());
                const bool neutral = *minimum >= 130 && *maximum - *minimum <= 35;
                const bool green = colour[1] >= 80 && colour[1] > colour[0] * 1.3 && colour[1] > colour[2] * 1.2;
                if (slot < 2 ? green : neutral) colour = slot < prefix
                    ? std::array<std::uint8_t, 3>{35, 200, 105}
                    : std::array<std::uint8_t, 3>{215, 215, 215};
            }
            const auto output = (static_cast<std::size_t>(top + y) * width + left + x) * 4;
            std::copy(colour.begin(), colour.end(), pixels.begin() + static_cast<std::ptrdiff_t>(output));
        }
    }
    return result;
}

void scenario(HDESK desktop, HMONITOR monitor, RECT monitor_bounds, const Image& crop,
              int width, int height, double scale, bool acknowledge) {
    auto fixture = make_fixture(crop, width, height, scale);
    OwnedWindow window{CreateWindowExW(WS_EX_TOPMOST, fixture_class, L"Synthetic reload capture",
        WS_POPUP, monitor_bounds.left + 8, monitor_bounds.top + 8, width, height,
        nullptr, nullptr, GetModuleHandleW(nullptr), &fixture)};
    if (!window.value) throw std::runtime_error("Cannot create isolated reload HUD window");
    ShowWindow(window.value, SW_SHOWNORMAL);
    SetForegroundWindow(window.value);
    UpdateWindow(window.value);
    POINT origin{};
    if (!ClientToScreen(window.value, &origin)) throw std::runtime_error("Cannot resolve fixture coordinates");
    const RECT bounds{origin.x, origin.y, origin.x + width, origin.y + height};
    const auto region = make_capture_region(monitor, bounds);
    // A human starts F8 after the white arrows are visible. WM_PAINT alone
    // does not guarantee that a newly created native window was presented.
    // Synchronize the fixture before starting the single-snapshot runner.
    unsigned presentation_misses{};
    const auto presentation_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(750);
    for (;;) {
        GdiFlush();
        const auto visible = capture_reload_screen(region);
        bool matches = visible.width == width && visible.height == height;
        for (std::size_t pixel = 0; matches && pixel < static_cast<std::size_t>(width) * height; ++pixel)
            for (std::size_t channel = 0; channel < 3; ++channel)
                if (visible.bgr[pixel * 3 + channel] != fixture.frames[0][pixel * 4 + channel]) {
                    matches = false;
                    break;
                }
        if (matches) break;
        if (std::chrono::steady_clock::now() >= presentation_deadline)
            throw std::runtime_error("Native arrow fixture was not presented before the single-snapshot run");
        ++presentation_misses;
        MSG pending{};
        while (PeekMessageW(&pending, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&pending); DispatchMessageW(&pending);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::vector<std::pair<ReloadDirection, bool>> input;
    unsigned pressed{};
    unsigned frames{};
    bool attached{};
    ReloadHelperRunner runner;
    ReloadHelperTiming timing;
    timing.hold_ms = 10;
    timing.key_gap_ms = 5;
    timing.stage_gap_ms = 30;
    timing.activation_timeout_ms = 500;
    timing.overall_timeout_ms = 4000;
    runner.start({
        [&] {
            if (!attached) {
                if (!SetThreadDesktop(desktop)) throw std::runtime_error("Cannot attach capture worker to isolated desktop");
                attached = true;
            }
            return IsWindow(window.value) != FALSE;
        },
        [] { return true; },
        [&]() -> std::optional<ReloadObservation> {
            ++frames;
            const auto frame = capture_reload_screen(region);
            return detect_reload_sequence(frame);
        },
        [&](ReloadDirection direction, bool down) {
            // Intentionally a fake transport: never calls SendInput or sends a
            // keyboard message. Only our fixture's painting state is updated.
            input.emplace_back(direction, down);
            if (down) {
                if (pressed >= directions.size() * 4 || direction != directions[pressed % directions.size()]) return false;
                ++pressed;
            } else if (!acknowledge && pressed == 4) {
                // The next stage is absent. No acknowledgement captures are
                // performed during the first four-key batch.
                DWORD_PTR result{};
                return SendMessageTimeoutW(window.value, paint_prefix, 5, 0,
                    SMTO_BLOCK | SMTO_ABORTIFHUNG, 500, &result) != 0;
            }
            return true;
        }}, timing);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while (runner.running() && std::chrono::steady_clock::now() < deadline) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const bool finished = !runner.running();
    const auto state = runner.state();
    const auto sent = runner.sent_count();
    const auto error = runner.last_error();
    runner.stop(); // Join before reading worker-owned input/frame counters.
    check(finished, "native capture workflow finishes within its bounded budget");
    if (acknowledge) {
        check(frames == 4, "four stages use exactly four fresh GDI captures");
        check(state == ReloadHelperState::completed && sent == 16 && error.empty(),
              "GDI capture and single-frame recognition send four batches of four inputs");
        check(input.size() == 32 && pressed == 16, "successful workflow generates exactly sixteen fake DOWN/UP pairs");
        for (std::size_t i = 0; i < std::min(input.size(), directions.size() * 8); ++i)
            check(input[i].first == directions[(i / 2) % directions.size()] && input[i].second == (i % 2 == 0),
                  "captured sequence preserves ordering and separate repeated DOWN directions");
    } else {
        check(frames == 2 && state == ReloadHelperState::error && sent == 4 && !error.empty(),
              "missing next-stage arrows stop after the second snapshot");
        check(input.size() == 8 && pressed == 4 && !input.back().second,
              "missing next stage sends no additional keys and leaves no key held");
    }
    std::cout << "native " << width << 'x' << height << " HUD scale " << scale
              << " available=" << acknowledge << " presentation_misses=" << presentation_misses << " frames=" << frames
              << " sent=" << sent << " input_events=" << input.size()
              << " error=" << error << '\n';
}
} // namespace

int main(int argc, char** argv) {
    if (argc > 2) { std::cerr << "usage: reload_capture_tests [evidence-directory]\n"; return 2; }
    try {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        IsolatedDesktop desktop;
        const auto crop = load_image_file(std::filesystem::path(WARDOGS_TEST_SOURCE_DIR) /
            L"tests/fixtures/reload/user-sph2-arrow-crop.png");
        if (crop.width != 344 || crop.height != 323)
            throw std::runtime_error("The annotated reload screenshot fixture changed");
        WNDCLASSW type{};
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = fixture_class;
        type.lpfnWndProc = window_proc;
        if (!RegisterClassW(&type)) throw std::runtime_error("Cannot register reload capture fixture");
        const HMONITOR monitor = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO info{sizeof(info)};
        if (!monitor || !GetMonitorInfoW(monitor, &info))
            throw std::runtime_error("Native reload capture requires a monitor");
        const int width = static_cast<int>(info.rcMonitor.right - info.rcMonitor.left) - 16;
        const int height = static_cast<int>(info.rcMonitor.bottom - info.rcMonitor.top) - 16;
        if (width < 640 || height < 480) throw std::runtime_error("Native reload fixture requires at least 656x496 pixels");
        scenario(desktop.handle(), monitor, info.rcMonitor, crop, 640, 480, 0.75, true);
        scenario(desktop.handle(), monitor, info.rcMonitor, crop,
                 std::min(width, 1024), std::min(height, 720), 1.4, true);
        scenario(desktop.handle(), monitor, info.rcMonitor, crop, 640, 480, 1.0, false);
        if (!UnregisterClassW(fixture_class, GetModuleHandleW(nullptr)))
            throw std::runtime_error("Cannot unregister reload fixture window class");
        desktop.restore_checked();
        if (argc == 2) {
            const auto output = std::filesystem::absolute(argv[1]);
            std::filesystem::create_directories(output);
            std::ofstream report(output / "reload-capture-summary.json");
            report.exceptions(std::ios::failbit | std::ios::badbit);
            report << "{\"checks\":" << checks << ",\"failures\":" << failures
                   << ",\"isolated_desktop\":true,\"real_input_events\":0}\n";
        }
        std::cout << checks << " checks; " << failures << " failures; original desktop restored; no real input sent\n";
        return failures ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
