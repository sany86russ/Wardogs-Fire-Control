#include "wardogs/capture.hpp"
#include "wardogs/reload_vision.hpp"

#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <QImage>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

struct Options {
    std::filesystem::path image;
    std::filesystem::path output;
    std::optional<wardogs::ImageRect> crop;
    bool capture_game{};
    bool wait_for_arrows{};
    bool help{};
    int frames{1};
    int interval_ms{100};
};

void output_json(const QJsonObject& value) {
    const auto bytes = QJsonDocument(value).toJson(QJsonDocument::Compact);
    std::cout.write(bytes.constData(), bytes.size());
    std::cout << '\n' << std::flush;
}

int number(std::wstring_view text, int minimum, int maximum) {
    std::size_t used{};
    const auto value = std::stoll(std::wstring(text), &used);
    if (used != text.size() || value < minimum || value > maximum)
        throw std::invalid_argument("Numeric argument is outside its allowed range");
    return static_cast<int>(value);
}

wardogs::ImageRect parse_crop(std::wstring_view value) {
    std::array<int, 4> coordinates{};
    for (std::size_t index = 0; index < coordinates.size(); ++index) {
        const auto separator = value.find(L',');
        if ((index < 3) != (separator != std::wstring_view::npos))
            throw std::invalid_argument("--crop requires exactly left,top,right,bottom");
        coordinates[index] = number(value.substr(0, separator), 0, 16384);
        if (separator != std::wstring_view::npos) value.remove_prefix(separator + 1);
    }
    return {coordinates[0], coordinates[1], coordinates[2], coordinates[3]};
}

Options parse(int argc, wchar_t** argv) {
    Options result;
    bool frames_set{}, interval_set{};
    for (int index = 1; index < argc; ++index) {
        const std::wstring_view argument{argv[index]};
        const auto next = [&]() -> std::wstring_view {
            if (++index == argc) throw std::invalid_argument("Missing option value");
            return argv[index];
        };
        if (argument == L"--help") result.help = true;
        else if (argument == L"--capture-game") {
            if (result.capture_game) throw std::invalid_argument("Duplicate capture mode");
            result.capture_game = true;
        } else if (argument == L"--wait-for-arrows") {
            if (result.wait_for_arrows) throw std::invalid_argument("Duplicate arrow wait option");
            result.wait_for_arrows = true;
        } else if (argument == L"--image") {
            if (!result.image.empty()) throw std::invalid_argument("Duplicate image option");
            result.image = next();
        } else if (argument == L"--output") {
            if (!result.output.empty()) throw std::invalid_argument("Duplicate output option");
            result.output = next();
        } else if (argument == L"--crop") {
            if (result.crop) throw std::invalid_argument("Duplicate crop option");
            result.crop = parse_crop(next());
        } else if (argument == L"--frames") {
            if (frames_set) throw std::invalid_argument("Duplicate frames option");
            frames_set = true; result.frames = number(next(), 1, 500);
        } else if (argument == L"--interval-ms") {
            if (interval_set) throw std::invalid_argument("Duplicate interval option");
            interval_set = true; result.interval_ms = number(next(), 10, 10000);
        } else throw std::invalid_argument("Unknown option; use --help");
    }
    if (result.help) return result;
    if (result.capture_game == !result.image.empty())
        throw std::invalid_argument("Choose exactly one mode: --image or --capture-game");
    if (result.capture_game && result.output.empty())
        throw std::invalid_argument("--capture-game requires an explicit --output directory");
    if (!result.capture_game && (!result.output.empty() || frames_set || interval_set || result.wait_for_arrows))
        throw std::invalid_argument("Output, frames, interval and arrow wait are only valid for --capture-game");
    if (result.capture_game && !result.crop && (result.frames > 100 || result.interval_ms < 50))
        throw std::invalid_argument("More than 100 frames or intervals below 50 ms require an explicit --crop");
    return result;
}

void validate_crop(wardogs::ImageRect area, int width, int height) {
    if (area.left < 0 || area.top < 0 || area.right > width || area.bottom > height ||
        area.right <= area.left || area.bottom <= area.top)
        throw std::invalid_argument("Crop must be a nonempty rectangle inside the source image");
}

wardogs::Image cropped(const wardogs::Image& source, wardogs::ImageRect area) {
    validate_crop(area, source.width, source.height);
    wardogs::Image result{area.right - area.left, area.bottom - area.top, {}};
    const auto stride = static_cast<std::size_t>(result.width) * 3;
    result.bgr.resize(stride * result.height);
    for (int y = 0; y < result.height; ++y) {
        const auto input = (static_cast<std::size_t>(area.top + y) * source.width + area.left) * 3;
        std::memcpy(result.bgr.data() + static_cast<std::size_t>(y) * stride,
                    source.bgr.data() + input, stride);
    }
    return result;
}

QJsonArray rectangle(wardogs::ImageRect value) {
    return {value.left, value.top, value.right, value.bottom};
}

wardogs::ImageRect translated(wardogs::ImageRect value, int x, int y) {
    return {value.left + x, value.top + y, value.right + x, value.bottom + y};
}

wardogs::Image load_probe_image(const std::filesystem::path& path) {
    // The text OCR loader keeps a 16 MP budget. This diagnostic tool must also
    // reopen 40 MP reload captures. WIC inspects the content, including JPEG
    // screenshots whose temporary filename happens to end in .png.
    struct Apartment {
        HRESULT result{CoInitializeEx(nullptr, COINIT_MULTITHREADED)};
        ~Apartment() { if (SUCCEEDED(result)) CoUninitialize(); }
    } apartment;
    if (FAILED(apartment.result) && apartment.result != RPC_E_CHANGED_MODE)
        throw std::runtime_error("Cannot initialize the image decoder apartment");
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory))))
        throw std::runtime_error("Cannot initialize the Windows image decoder");
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnLoad, &decoder)))
        throw std::runtime_error("Cannot open the input image");
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) throw std::runtime_error("Cannot read the input image frame");
    UINT width{}, height{};
    if (FAILED(frame->GetSize(&width, &height))) throw std::runtime_error("Cannot read the input image size");
    if (!width || !height || width > 16384 || height > 8192 ||
        static_cast<std::uint64_t>(width) * height > 40'000'000)
        throw std::invalid_argument("Input image must fit the 40 MP, 16384 by 8192 reload budget");
    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat24bppBGR,
                                      WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
        throw std::runtime_error("Cannot decode the input image as BGR");
    wardogs::Image image{static_cast<int>(width), static_cast<int>(height), {}};
    const auto stride = static_cast<std::size_t>(image.width) * 3;
    image.bgr.resize(stride * image.height);
    if (FAILED(converter->CopyPixels(nullptr, static_cast<UINT>(stride),
                                     static_cast<UINT>(image.bgr.size()), image.bgr.data())))
        throw std::runtime_error("Cannot read the input image pixels");
    return image;
}

QJsonObject analyze(const wardogs::Image& original, const std::optional<wardogs::ImageRect>& crop) {
    const auto start = Clock::now();
    const auto image = crop ? cropped(original, *crop) : original;
    const auto observation = wardogs::detect_reload_sequence(image);
    const int offset_x = crop ? crop->left : 0, offset_y = crop ? crop->top : 0;
    QJsonObject output{{"event", "analysis"},
        {"originalCaptureSize", QJsonArray{original.width, original.height}},
        {"analyzedSize", QJsonArray{image.width, image.height}},
        {"crop", crop ? QJsonValue(rectangle(*crop)) : QJsonValue(QJsonValue::Null)},
        {"rawDetected", observation.has_value()}};
    if (observation) {
        output.insert("bounds", rectangle(translated(observation->bounds, offset_x, offset_y)));
        output.insert("boundsKind", "glyphrow");
        output.insert("confidence", observation->confidence);
        QJsonArray arrows;
        for (const auto& arrow : observation->arrows) {
            QString direction;
            switch (arrow.direction) {
            case wardogs::ReloadDirection::up: direction = "up"; break;
            case wardogs::ReloadDirection::right: direction = "right"; break;
            case wardogs::ReloadDirection::down: direction = "down"; break;
            case wardogs::ReloadDirection::left: direction = "left"; break;
            }
            arrows.append(QJsonObject{{"direction", direction}});
        }
        output.insert("arrows", arrows);
    }
    output.insert("analysisMs", std::chrono::duration<double, std::milli>(Clock::now() - start).count());
    return output;
}

QJsonObject analyze_capture(const wardogs::Image& image, wardogs::ImageRect saved_region,
                           int client_width, int client_height, bool crop_requested) {
    auto report = analyze(image, std::nullopt);
    report.insert("originalCaptureSize", QJsonArray{client_width, client_height});
    report.insert("savedRegion", rectangle(saved_region));
    report.insert("crop", crop_requested ? QJsonValue(rectangle(saved_region)) : QJsonValue(QJsonValue::Null));
    const auto translated_json = [&](const QJsonArray& local) {
        return QJsonArray{local.at(0).toInt() + saved_region.left,
            local.at(1).toInt() + saved_region.top, local.at(2).toInt() + saved_region.left,
            local.at(3).toInt() + saved_region.top};
    };
    if (report.contains("bounds")) report.insert("bounds", translated_json(report.value("bounds").toArray()));
    return report;
}

bool game_window(HWND window) {
    DWORD process{};
    if (!GetWindowThreadProcessId(window, &process) || process == GetCurrentProcessId() ||
        !IsWindowVisible(window) || IsIconic(window)) return false;
    std::array<wchar_t, 128> window_class{};
    std::array<wchar_t, 512> title{};
    if (!GetClassNameW(window, window_class.data(), static_cast<int>(window_class.size())) ||
        std::wstring_view(window_class.data()) != L"UnrealWindow" ||
        !GetWindowTextW(window, title.data(), static_cast<int>(title.size()))) return false;
    std::wstring name{title.data()};
    while (!name.empty() && std::iswspace(name.back())) name.pop_back();
    const auto first = std::find_if_not(name.begin(), name.end(), [](wchar_t ch) { return std::iswspace(ch); });
    name.erase(name.begin(), first);
    std::transform(name.begin(), name.end(), name.begin(), [](wchar_t ch) { return std::towlower(ch); });
    return name == L"wardogs" || name == L"wardogsclient" || name == L"wardogsclient-win64-shipping";
}

HWND find_game() {
    struct Search { HWND window{}; unsigned count{}; bool failed{}; } result;
    EnumWindows([](HWND window, LPARAM value) noexcept -> BOOL {
        auto& search = *reinterpret_cast<Search*>(value);
        try {
            if (game_window(window)) { search.window = window; ++search.count; }
        } catch (...) { search.failed = true; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&result));
    if (result.failed || result.count != 1)
        throw std::runtime_error("Exactly one visible WARDOGS UnrealWindow is required");
    return result.window;
}

RECT client_bounds(HWND window) {
    RECT client{};
    POINT origin{};
    if (!game_window(window) || !GetClientRect(window, &client) ||
        !ClientToScreen(window, &origin) || client.right <= 0 || client.bottom <= 0)
        throw std::runtime_error("Cannot resolve the visible game client");
    return {origin.x, origin.y, origin.x + client.right, origin.y + client.bottom};
}

void capture(const Options& options) {
    const HWND window = find_game();
    const RECT original = client_bounds(window);
    if (options.crop) validate_crop(*options.crop, original.right - original.left,
                                    original.bottom - original.top);
    const auto saved_region = options.crop.value_or(wardogs::ImageRect{0, 0,
        static_cast<int>(original.right - original.left), static_cast<int>(original.bottom - original.top)});
    const RECT capture_bounds{original.left + saved_region.left, original.top + saved_region.top,
                              original.left + saved_region.right, original.top + saved_region.bottom};
    const auto monitor = MonitorFromPoint({capture_bounds.left, capture_bounds.top}, MONITOR_DEFAULTTONULL);
    MONITORINFO info{sizeof(info)};
    if (!monitor || !GetMonitorInfoW(monitor, &info) || capture_bounds.left < info.rcMonitor.left ||
        capture_bounds.top < info.rcMonitor.top || capture_bounds.right > info.rcMonitor.right ||
        capture_bounds.bottom > info.rcMonitor.bottom)
        throw std::runtime_error("The requested capture region must fit inside one monitor");
    const auto region = wardogs::make_capture_region(monitor, capture_bounds);
    const auto require_geometry = [&] {
        const auto current = client_bounds(window);
        if (!EqualRect(&original, &current)) throw std::runtime_error("Game client geometry changed");
    };
    output_json({{"event", "waitingForGameForeground"}, {"timeoutMs", 30000},
          {"readOnly", true}, {"frames", options.frames}, {"waitForArrows", options.wait_for_arrows}});
    const auto deadline = Clock::now() + 30s;
    while (GetForegroundWindow() != window) {
        require_geometry();
        if (Clock::now() >= deadline) throw std::runtime_error("Timed out waiting for the game foreground");
        std::this_thread::sleep_for(50ms);
    }
    std::filesystem::create_directories(options.output);
    const auto started = Clock::now();
    const auto trigger_deadline = started + 30s;
    bool triggered = !options.wait_for_arrows;
    double trigger_wait_ms{};
    if (!triggered) output_json({{"event", "waitingForArrows"}, {"timeoutMs", 30000}, {"readOnly", true}});
    for (int frame = 0; frame < options.frames;) {
        require_geometry();
        if (GetForegroundWindow() != window) throw std::runtime_error("Game foreground lost; capture stopped");
        if (!triggered && Clock::now() >= trigger_deadline)
            throw std::runtime_error("Timed out waiting for a visible arrow strip");
        const auto before_capture = Clock::now();
        const auto image = wardogs::capture_reload_screen(region);
        const auto capture_ms = std::chrono::duration<double, std::milli>(Clock::now() - before_capture).count();
        require_geometry();
        if (GetForegroundWindow() != window) throw std::runtime_error("Game foreground changed during capture");
        auto report = analyze_capture(image, saved_region, static_cast<int>(original.right - original.left),
                                      static_cast<int>(original.bottom - original.top), options.crop.has_value());
        if (!triggered) {
            if (Clock::now() >= trigger_deadline)
                throw std::runtime_error("Timed out waiting for a visible arrow strip");
            if (!report.value("rawDetected").toBool()) {
                // These observations are intentionally neither saved nor
                // counted. Frame zero is the exact first recognized capture.
                std::this_thread::sleep_until(std::min(trigger_deadline,
                    Clock::now() + std::chrono::milliseconds(options.interval_ms)));
                continue;
            }
            triggered = true;
            trigger_wait_ms = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
            output_json({{"event", "arrowTrigger"}, {"triggerWaitMs", trigger_wait_ms}, {"readOnly", true}});
        }
        std::wostringstream filename;
        filename << L"frame-" << std::setw(3) << std::setfill(L'0') << frame << L".png";
        const auto path = options.output / filename.str();
        QFile destination(QString::fromStdWString(path.wstring()));
        if (!destination.open(QIODevice::WriteOnly | QIODevice::NewOnly))
            throw std::runtime_error("Cannot create capture file; existing files are never overwritten");
        const QImage bitmap(image.bgr.data(), image.width, image.height, image.width * 3, QImage::Format_BGR888);
        if (!bitmap.save(&destination, "PNG") || !destination.flush())
            throw std::runtime_error("Cannot save the explicit capture file");
        destination.close();
        report.insert("event", "capturedFrame"); report.insert("frame", frame);
        report.insert("triggerWaitMs", trigger_wait_ms);
        report.insert("captureBackend", "GDI_SRCCOPY_CAPTUREBLT");
        report.insert("includesDesktopOverlays", true);
        report.insert("clientScreenRect", QJsonArray{static_cast<int>(original.left), static_cast<int>(original.top),
                                                     static_cast<int>(original.right), static_cast<int>(original.bottom)});
        report.insert("captureMs", capture_ms);
        report.insert("elapsedMs", std::chrono::duration<double, std::milli>(Clock::now() - started).count());
        report.insert("saved", QString::fromStdWString(std::filesystem::absolute(path).wstring()));
        report.insert("foregroundVerified", true); output_json(report);
        ++frame;
        if (frame < options.frames) std::this_thread::sleep_for(std::chrono::milliseconds(options.interval_ms));
    }
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        const auto options = parse(argc, argv);
        if (options.help) {
            std::cout << "Read-only SPH-2 reload vision diagnostics; never sends input or activates windows.\n"
                "  --image PATH [--crop left,top,right,bottom]\n"
                "  --capture-game --output DIRECTORY [--frames 1..500] [--interval-ms 10..10000]\n"
                "                 [--crop left,top,right,bottom] [--wait-for-arrows]\n"
                "Capture waits up to 30 seconds for the existing game to receive focus.\n"
                "Capture saves only the selected client crop, or the complete client when --crop is absent.\n"
                "PNGs are written only to the explicit output directory.\n"
                "Without an explicit --crop, capture is limited to 100 frames and intervals of at least 50 ms.\n"
                "--wait-for-arrows waits up to 30 seconds after foreground for raw recognition,\n"
                "without saving or counting earlier frames.\n"
                "JSON bounds are physical pixels in the original source image.\n";
            return 0;
        }
        if (options.capture_game) {
            SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            capture(options);
        } else output_json(analyze(load_probe_image(options.image), options.crop));
        return 0;
    } catch (const std::exception& error) {
        output_json({{"event", "error"}, {"message", QString::fromUtf8(error.what())}, {"readOnly", true}});
        return 1;
    }
}
