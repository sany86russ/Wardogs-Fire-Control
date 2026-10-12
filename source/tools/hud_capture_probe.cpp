#include "wardogs/capture.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>
#include <TlHelp32.h>

namespace {
bool game_window(HWND window) {
    DWORD pid{};
    if (!window || !GetWindowThreadProcessId(window, &pid)) return false;
    // Anti-cheat can restrict process handles. The OS process inventory still
    // supplies identity without reading or altering game process memory.
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    bool found{};
    if (Process32FirstW(snapshot, &entry)) do {
        if (entry.th32ProcessID == pid) {
            found = _wcsicmp(entry.szExeFile, L"WardogsClient-Win64-Shipping.exe") == 0;
            break;
        }
    } while (Process32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    return found;
}

void save_bmp(const wardogs::Image& image, const std::filesystem::path& path) {
    if (std::filesystem::exists(path)) throw std::runtime_error("Capture file already exists");
    const auto stride = (static_cast<std::uint32_t>(image.width) * 3 + 3) & ~3U;
    BITMAPFILEHEADER file{};
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info); info.biWidth = image.width; info.biHeight = -image.height;
    info.biPlanes = 1; info.biBitCount = 24; info.biCompression = BI_RGB;
    info.biSizeImage = stride * static_cast<std::uint32_t>(image.height);
    file.bfType = 0x4D42; file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + info.biSizeImage;
    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(&file), sizeof(file));
    stream.write(reinterpret_cast<const char*>(&info), sizeof(info));
    const char padding[3]{};
    for (int y = 0; y < image.height; ++y) {
        const auto row_size = static_cast<std::uint32_t>(image.width) * 3;
        stream.write(reinterpret_cast<const char*>(image.bgr.data() +
            static_cast<std::size_t>(y) * row_size), row_size);
        stream.write(padding, stride - row_size);
    }
    if (!stream) throw std::runtime_error("Cannot save game-client capture");
}
}

int wmain(int argc, wchar_t** argv) {
    const bool screen = argc == 3 && std::wstring_view(argv[2]) == L"--screen";
    if (argc != 2 && !screen) { std::wcerr << L"usage: hud_capture_probe <new-output-directory> [--screen]\n"; return 2; }
    try {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        SetPriorityClass(GetCurrentProcess(), IDLE_PRIORITY_CLASS);
        const std::filesystem::path directory(argv[1]);
        if (std::filesystem::exists(directory)) throw std::runtime_error("Output directory already exists");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        std::size_t captured{}, refused{};
        while (captured < 6 && std::chrono::steady_clock::now() < deadline) {
            const auto window = GetForegroundWindow();
            if (!game_window(window)) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); continue; }
            auto stamp = GetTickCount64();
            const auto capture_started = stamp;
            std::uint64_t source_qpc{};
            std::string_view backend = "screen-gdi-diagnostic";
            wardogs::Image image;
            if (screen) {
                RECT client{}; POINT origin{};
                if (!GetClientRect(window, &client) || !ClientToScreen(window, &origin))
                    throw std::runtime_error("Cannot locate foreground game client");
                const RECT physical{origin.x, origin.y, origin.x + client.right, origin.y + client.bottom};
                image = wardogs::capture_screen(wardogs::make_capture_region(
                    MonitorFromWindow(window, MONITOR_DEFAULTTONULL), physical));
                stamp = GetTickCount64();
            } else {
                try {
                    auto capture = wardogs::capture_game_client_frame(window);
                    stamp = capture.captured_at_ms;
                    source_qpc = capture.system_relative_time_100ns;
                    backend = capture.backend;
                    image = std::move(capture.image);
                } catch (const std::runtime_error& error) {
                    // Production preserves a healthy device after a bounded
                    // cold-start timeout. Exercise that same retry in this
                    // process; never substitute an older client-DC image.
                    if (std::string_view(error.what()).find("exceeded 250 ms") == std::string_view::npos ||
                        ++refused > 2) throw;
                    std::cerr << "capture_attempt_refused=" << refused << " reason=" << error.what() << '\n';
                    continue;
                }
            }
            if (GetForegroundWindow() != window || !game_window(window)) continue;
            if (!captured) std::filesystem::create_directories(directory);
            save_bmp(image, directory / (L"frame-" + std::to_wstring(captured + 1) + L".bmp"));
            std::cout << "frame=" << ++captured << " capture_tick_ms=" << stamp
                      << " acquisition_started_at_ms=" << capture_started
                      << " system_relative_time_100ns=" << source_qpc << " backend=" << backend
                      << " width=" << image.width << " height=" << image.height << '\n';
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (captured != 6) { std::cerr << "Incomplete capture: WARDOGS must remain foreground\n"; return 1; }
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
