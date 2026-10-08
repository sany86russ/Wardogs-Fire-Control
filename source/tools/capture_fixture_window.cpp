#include "wardogs/ocr.hpp"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// Only the diagnostic child uses this handshake. The fixture receives focus
// before exercising the application's unchanged foreground safety gate.
constexpr UINT prepare_capture_message = WM_APP + 0x331;

class FixtureCursorRestore {
public:
    void remember_original() {
        if (!saved_ && !GetCursorPos(&original_))
            throw std::runtime_error("Cannot save the original test cursor position");
        saved_ = true;
    }
    bool move_to(HWND own_window, POINT client_point) noexcept {
        DWORD pid{};
        if (!saved_ || !GetWindowThreadProcessId(own_window, &pid) ||
            pid != GetCurrentProcessId() || GetForegroundWindow() != own_window ||
            !ClientToScreen(own_window, &client_point)) return false;
        if (!SetCursorPos(client_point.x, client_point.y)) return false;
        moved_ = true;
        POINT actual{};
        return GetCursorPos(&actual) && actual.x == client_point.x && actual.y == client_point.y;
    }
    void restore_checked() {
        if (!restore()) throw std::runtime_error("Cannot restore the original test cursor position");
    }
    ~FixtureCursorRestore() {
        if (!restore()) std::cerr << "FAIL: cannot restore the original test cursor position\n";
    }
private:
    bool restore() noexcept {
        if (!moved_) return true;
        if (!SetCursorPos(original_.x, original_.y)) return false;
        POINT actual{};
        if (!GetCursorPos(&actual) || actual.x != original_.x || actual.y != original_.y) return false;
        moved_ = false;
        return true;
    }
    POINT original_{};
    bool saved_{};
    bool moved_{};
};

struct Fixture {
    int width{};
    int height{};
    std::vector<std::uint8_t> bgra;
    bool map_fields_painted{};
    int draft_width{};
    int draft_height{};
    std::vector<std::uint8_t> draft_bgra;
    bool show_draft{};
    bool draft_painted{};
    FixtureCursorRestore* cursor{};
};

bool paint_map_coordinate_fields(HDC dc) {
    // Synthetic map fixture, independent of the chat image. The diagnostic
    // app uses ClientToScreen({700,500}) as its click source; no mouse input is
    // generated. Each number falls within the original EXE's own axis field.
    const HFONT font = CreateFontW(-20, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                   CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY,
                                   DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    if (!font) return false;
    const HGDIOBJ previous = SelectObject(dc, font);
    if (!previous || previous == HGDI_ERROR) {
        DeleteObject(font);
        return false;
    }
    const int previous_mode = SetBkMode(dc, TRANSPARENT);
    const COLORREF previous_color = SetTextColor(dc, RGB(255, 255, 255));
    const BOOL x_drawn = TextOutW(dc, 725, 470, L"x99.51", 6);
    // 211 m from the real gun draft (98.74,111.85), inside L81's range.
    const BOOL y_drawn = TextOutW(dc, 700, 405, L"y113.81", 7);
    SetTextColor(dc, previous_color);
    SetBkMode(dc, previous_mode);
    SelectObject(dc, previous);
    DeleteObject(font);
    return x_drawn && y_drawn;
}

LRESULT CALLBACK fixture_window_proc(HWND window, UINT message, WPARAM wparam,
                                    LPARAM lparam) {
    if (message == prepare_capture_message) {
        auto* fixture = reinterpret_cast<Fixture*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (!fixture || wparam > 1 || (wparam == 1 && fixture->draft_bgra.empty())) return FALSE;
        fixture->show_draft = wparam == 1;
        fixture->draft_painted = false;
        InvalidateRect(window, nullptr, FALSE);
        UpdateWindow(window);
        if (fixture->show_draft && !fixture->draft_painted) return FALSE;
        const BOOL activated = GetForegroundWindow() == window ? TRUE : SetForegroundWindow(window);
        const bool foreground = GetForegroundWindow() == window;
        std::cout << "Native fixture requested focus: accepted=" << (activated ? 1 : 0)
                  << " foreground=" << (foreground ? 1 : 0) << '\n';
        if (!foreground) return FALSE;
        if (wparam == 1 && (!fixture->cursor || !fixture->cursor->move_to(window, POINT{700, 500})))
            return FALSE;
        return TRUE;
    }
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA,
                         reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(window, &paint);
        FillRect(dc, &paint.rcPaint, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        auto* fixture = reinterpret_cast<Fixture*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (fixture) {
            BITMAPINFO bitmap{};
            bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bitmap.bmiHeader.biWidth = fixture->show_draft ? fixture->draft_width : fixture->width;
            bitmap.bmiHeader.biHeight = -(fixture->show_draft ? fixture->draft_height : fixture->height);
            bitmap.bmiHeader.biPlanes = 1;
            bitmap.bmiHeader.biBitCount = 32;
            bitmap.bmiHeader.biCompression = BI_RGB;
            if (fixture->show_draft) {
                fixture->draft_painted = SetDIBitsToDevice(
                    dc, 24, 16, fixture->draft_width, fixture->draft_height,
                    0, 0, 0, fixture->draft_height, fixture->draft_bgra.data(),
                    &bitmap, DIB_RGB_COLORS) == fixture->draft_height;
            } else {
                for (int row = 0; row < 2; ++row)
                    SetDIBitsToDevice(dc, 24, 16 + row * 60, fixture->width, fixture->height,
                                     0, 0, 0, fixture->height, fixture->bgra.data(),
                                     &bitmap, DIB_RGB_COLORS);
            }
            fixture->map_fields_painted = paint_map_coordinate_fields(dc);
        }
        EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

struct OwnedWindow {
    HWND value{};
    ~OwnedWindow() { if (value) DestroyWindow(value); }
};

struct ChildProcess {
    PROCESS_INFORMATION process{};
    bool finished{};
    ~ChildProcess() {
        if (process.hProcess) {
            // This is exclusively the child created by this local test.
            if (!finished) {
                TerminateProcess(process.hProcess, 124);
                WaitForSingleObject(process.hProcess, 1000);
            }
            CloseHandle(process.hProcess);
        }
        if (process.hThread) CloseHandle(process.hThread);
    }
};

class TestDesktop {
public:
    TestDesktop() {
        original_thread_ = GetThreadDesktop(GetCurrentThreadId());
        original_input_ = OpenInputDesktop(0, FALSE, DESKTOP_SWITCHDESKTOP);
        try {
            if (!original_thread_ || !original_input_)
                throw std::runtime_error("Native capture test needs an interactive desktop");
            const auto name = L"WardogsCapture-" + std::to_wstring(GetCurrentProcessId()) +
                              L"-" + std::to_wstring(GetTickCount64());
            constexpr ACCESS_MASK access = DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS |
                DESKTOP_CREATEWINDOW | DESKTOP_CREATEMENU | DESKTOP_ENUMERATE | DESKTOP_SWITCHDESKTOP;
            test_ = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, access, nullptr);
            if (!test_ || !SetThreadDesktop(test_))
                throw std::runtime_error("Cannot create an isolated native test desktop");
            attached_ = true;
            DWORD bytes{};
            GetUserObjectInformationW(GetProcessWindowStation(), UOI_NAME, nullptr, 0, &bytes);
            if (bytes == 0 || bytes > 1024)
                throw std::runtime_error("Cannot resolve the native test window station");
            std::wstring station(bytes / sizeof(wchar_t), L'\0');
            if (!GetUserObjectInformationW(GetProcessWindowStation(), UOI_NAME,
                                           station.data(), bytes, &bytes))
                throw std::runtime_error("Cannot read the native test window station");
            station.resize(std::char_traits<wchar_t>::length(station.c_str()));
            child_desktop_ = station + L"\\" + name;
            if (!SwitchDesktop(test_)) throw std::runtime_error("Cannot activate the native test desktop");
            switched_ = true;
        } catch (...) {
            if (!restore()) std::cerr << "FAIL: cannot restore the original desktop after setup failure\n";
            close_handles();
            throw;
        }
    }
    ~TestDesktop() {
        const bool needed_restore = switched_ || attached_;
        if (!restore()) std::cerr << "FAIL: cannot restore the original desktop\n";
        else if (needed_restore && !restore_reported_)
            std::cout << "Native test restored the original desktop during failure cleanup\n";
        close_handles();
    }
    TestDesktop(const TestDesktop&) = delete;
    TestDesktop& operator=(const TestDesktop&) = delete;

    wchar_t* child_desktop() noexcept { return child_desktop_.data(); }

    void restore_checked() {
        if (!restore()) throw std::runtime_error("Cannot restore the original desktop after native capture");
        restore_reported_ = true;
        std::cout << "Native test restored the original desktop\n";
    }

private:
    bool restore() noexcept {
        bool restored = true;
        if (switched_) {
            if (SwitchDesktop(original_input_)) switched_ = false;
            else restored = false;
        }
        if (attached_) {
            if (SetThreadDesktop(original_thread_)) attached_ = false;
            else restored = false;
        }
        return restored;
    }
    void close_handles() noexcept {
        if (test_) { CloseDesktop(test_); test_ = nullptr; }
        if (original_input_) { CloseDesktop(original_input_); original_input_ = nullptr; }
    }

    HDESK original_thread_{};  // Borrowed, never closed.
    HDESK original_input_{};
    HDESK test_{};
    bool switched_{};
    bool attached_{};
    bool restore_reported_{};
    std::wstring child_desktop_;
};

std::wstring quoted_path(const std::filesystem::path& path) {
    const auto value = std::filesystem::absolute(path).wstring();
    if (value.find(L'"') != std::wstring::npos)
        throw std::invalid_argument("The test path contains a quote");
    return L"\"" + value + L"\"";
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 4 && argc != 5) {
        std::wcerr << L"usage: capture_fixture_window <app.exe> <fixture.png> <report.json> [active-draft.png]\n";
        return 2;
    }
    try {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        // Cursor restoration outlives desktop restoration, including failure
        // cleanup. Mode zero never changes the pointer. No input is sent to a
        // game; only this process's foreground test fixture may position it.
        FixtureCursorRestore cursor;
        if (argc == 5) cursor.remember_original();
        // Own desktop avoids dependence on another application's current
        // foreground rights and never attaches to its input queue. The screen
        // still passes through the application's real GDI/focus safety path.
        TestDesktop desktop;
        const auto image = wardogs::load_image_file(argv[2]);
        Fixture fixture{image.width, image.height,
                        std::vector<std::uint8_t>(
                            static_cast<std::size_t>(image.width) * image.height * 4, 255)};
        fixture.cursor = &cursor;
        for (std::size_t input = 0, output = 0; input < image.bgr.size(); input += 3, output += 4)
            std::copy_n(image.bgr.data() + input, 3, fixture.bgra.data() + output);
        if (argc == 5) {
            const auto draft = wardogs::load_image_file(argv[4]);
            fixture.draft_width = draft.width;
            fixture.draft_height = draft.height;
            fixture.draft_bgra.assign(static_cast<std::size_t>(draft.width) * draft.height * 4, 255);
            for (std::size_t input = 0, output = 0; input < draft.bgr.size(); input += 3, output += 4)
                std::copy_n(draft.bgr.data() + input, 3, fixture.draft_bgra.data() + output);
        }
        const auto instance = GetModuleHandleW(nullptr);
        WNDCLASSW window_class{};
        window_class.lpfnWndProc = fixture_window_proc;
        window_class.hInstance = instance;
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.lpszClassName = L"UnrealWindow";
        if (!RegisterClassW(&window_class)) throw std::runtime_error("Cannot register the fixture window");
        RECT work{};
        if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0))
            throw std::runtime_error("Cannot resolve the test monitor");
        RECT bounds{0, 0, 1280, 540};
        if (!AdjustWindowRectEx(&bounds, WS_OVERLAPPEDWINDOW, FALSE, 0))
            throw std::runtime_error("Cannot resolve the fixture window bounds");
        OwnedWindow window{CreateWindowExW(
            // Match the observed game caption, including two trailing spaces.
            WS_EX_TOPMOST, L"UnrealWindow", L"Wardogs  ",
            WS_OVERLAPPEDWINDOW, work.left + 8, work.top + 8,
            bounds.right - bounds.left, bounds.bottom - bounds.top,
            nullptr, nullptr, instance, &fixture)};
        if (!window.value) throw std::runtime_error("Cannot create the fixture window");
        ShowWindow(window.value, SW_SHOWNORMAL);
        SetForegroundWindow(window.value);
        UpdateWindow(window.value);
        if (!fixture.map_fields_painted)
            throw std::runtime_error("Cannot render synthetic map coordinate fields");

        const auto app = std::filesystem::absolute(argv[1]);
        const auto report = std::filesystem::absolute(argv[3]);
        std::filesystem::create_directories(report.parent_path());
        std::wstring command = quoted_path(app) + L" \"--self-test=" + report.wstring() +
                               L"\" --test-capture-window --capture-fixture-pid=" +
                               std::to_wstring(GetCurrentProcessId());
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.lpDesktop = desktop.child_desktop();
        ChildProcess child;
        if (!CreateProcessW(app.c_str(), command.data(), nullptr, nullptr, FALSE, 0,
                            nullptr, app.parent_path().c_str(), &startup, &child.process))
            throw std::runtime_error("Cannot start the application capture workflow");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(25);
        while (std::chrono::steady_clock::now() < deadline && IsWindow(window.value)) {
            const DWORD ready = MsgWaitForMultipleObjects(1, &child.process.hProcess,
                                                          FALSE, 100, QS_ALLINPUT);
            if (ready == WAIT_OBJECT_0) {
                DWORD exit_code{};
                if (!GetExitCodeProcess(child.process.hProcess, &exit_code))
                    throw std::runtime_error("Cannot read the application test result");
                child.finished = true;
                DestroyWindow(window.value);
                window.value = nullptr;
                desktop.restore_checked();
                cursor.restore_checked();
                return static_cast<int>(exit_code);
            }
            if (ready == WAIT_FAILED) throw std::runtime_error("Native fixture message wait failed");
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        throw std::runtime_error("Application capture workflow did not finish within 25 seconds");
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
