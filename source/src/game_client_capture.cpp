#include "wardogs/capture.hpp"
#include "wardogs/logger.hpp"

#include <d3d11_4.h>
#include <dwmapi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <chrono>
#include <atomic>
#include <memory>
#include <mutex>
#include <limits>
#include <stdexcept>
#include <utility>

namespace wardogs {
namespace detail {
std::size_t checked_capture_pixel_count(std::int64_t width, std::int64_t height);
}
namespace {

using namespace winrt::Windows::Graphics::Capture;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
using winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;
using winrt::Windows::Foundation::Metadata::ApiInformation;
using DxgiAccess = ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess;

struct CaptureAttempt {
    std::chrono::steady_clock::time_point started{std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point stage_started{started};
    std::string_view stage{"identity"};
    std::uint32_t frames_received{}, stale_frames{}, gpu_waits{};
    std::int64_t requested_qpc{}, source_qpc{};

    void enter(std::string_view next) {
        stage = next; stage_started = std::chrono::steady_clock::now();
    }

    std::string describe() const {
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - started).count();
        const auto stage_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - stage_started).count();
        return " stage=" + std::string(stage) + " elapsed_ms=" + std::to_string(elapsed) +
            " stage_elapsed_ms=" + std::to_string(stage_elapsed) +
            " frames_received=" + std::to_string(frames_received) +
            " stale_frames=" + std::to_string(stale_frames) + " gpu_waits=" + std::to_string(gpu_waits) +
            " request_qpc_100ns=" + std::to_string(requested_qpc) +
            " source_qpc_100ns=" + std::to_string(source_qpc);
    }
};

struct Apartment {
    Apartment() { winrt::init_apartment(winrt::apartment_type::multi_threaded); }
    ~Apartment() { winrt::uninit_apartment(); }
};

struct RuntimeLease {
    CO_MTA_USAGE_COOKIE cookie{};
    RuntimeLease() { winrt::check_hresult(CoIncrementMTAUsage(&cookie)); }
    ~RuntimeLease() {
        if (FAILED(CoDecrementMTAUsage(cookie))) log_error("capture.mta_usage_release_failed");
    }
};

struct PhysicalPixels {
    DPI_AWARENESS_CONTEXT previous{};
    PhysicalPixels() {
        previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        if (!previous) throw std::runtime_error("Cannot establish physical-pixel game capture coordinates");
    }
    ~PhysicalPixels() {
        if (previous && !SetThreadDpiAwarenessContext(previous)) log_error("capture.dpi_context_restore_failed");
    }
};

struct WindowGeometry {
    HWND window{};
    DWORD process{}, thread{};
    RECT client{}, outer{}, extended{};
    POINT origin{};
};

WindowGeometry geometry(HWND window) {
    WindowGeometry result;
    result.window = window;
    DWORD cloaked{};
    if (!window || !IsWindow(window) || !IsWindowVisible(window) || IsIconic(window) ||
        !GetClientRect(window, &result.client) || !ClientToScreen(window, &result.origin) ||
        !GetWindowRect(window, &result.outer) ||
        FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS,
                                     &result.extended, sizeof(result.extended))) ||
        FAILED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) || cloaked)
        throw std::runtime_error("Game client is not available for fresh compositor capture");
    result.thread = GetWindowThreadProcessId(window, &result.process);
    if (!result.thread || !result.process || result.client.left != 0 || result.client.top != 0)
        throw std::runtime_error("Game client identity or geometry is invalid");
    (void)detail::checked_capture_pixel_count(result.client.right, result.client.bottom);
    return result;
}

bool same_geometry(const WindowGeometry& first, const WindowGeometry& second) {
    return first.window == second.window && first.process == second.process && first.thread == second.thread &&
        EqualRect(&first.client, &second.client) && EqualRect(&first.outer, &second.outer) &&
        EqualRect(&first.extended, &second.extended) &&
        first.origin.x == second.origin.x && first.origin.y == second.origin.y;
}

RECT client_crop(const WindowGeometry& geometry, winrt::Windows::Graphics::SizeInt32 size) {
    (void)detail::checked_capture_pixel_count(size.Width, size.Height);
    std::optional<RECT> crop;
    // Win32 window captures include non-client decoration. Require an exact
    // match to observed OS bounds, then retain only the full physical client.
    const auto client_right = static_cast<std::int64_t>(geometry.origin.x) + geometry.client.right;
    const auto client_bottom = static_cast<std::int64_t>(geometry.origin.y) + geometry.client.bottom;
    if (client_right > (std::numeric_limits<LONG>::max)() ||
        client_bottom > (std::numeric_limits<LONG>::max)())
        throw std::runtime_error("Game client screen bounds overflow physical coordinates");
    const RECT client_screen{geometry.origin.x, geometry.origin.y,
        static_cast<LONG>(client_right), static_cast<LONG>(client_bottom)};
    for (const auto bounds : {geometry.extended, geometry.outer, client_screen}) {
        if (static_cast<std::int64_t>(bounds.right) - bounds.left != size.Width ||
            static_cast<std::int64_t>(bounds.bottom) - bounds.top != size.Height) continue;
        const auto left = static_cast<std::int64_t>(geometry.origin.x) - bounds.left;
        const auto top = static_cast<std::int64_t>(geometry.origin.y) - bounds.top;
        const auto right = left + geometry.client.right, bottom = top + geometry.client.bottom;
        if (left < 0 || top < 0 || right > size.Width || bottom > size.Height)
            continue;
        const RECT candidate{static_cast<LONG>(left), static_cast<LONG>(top),
                             static_cast<LONG>(right), static_cast<LONG>(bottom)};
        if (crop && !EqualRect(&*crop, &candidate))
            throw std::runtime_error("Graphics capture bounds have an ambiguous client origin");
        crop = candidate;
    }
    if (!crop) throw std::runtime_error("Graphics capture bounds do not match the complete game client");
    return *crop;
}

std::int64_t qpc_100ns() {
    LARGE_INTEGER counter{}, frequency{};
    if (!QueryPerformanceCounter(&counter) || !QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
        throw std::runtime_error("Cannot verify compositor frame freshness");
    return static_cast<std::int64_t>(static_cast<long double>(counter.QuadPart) * 10'000'000 / frequency.QuadPart);
}

void check_request(std::stop_token stop, std::chrono::steady_clock::time_point deadline,
                   const CaptureAttempt& attempt) {
    if (stop.stop_requested()) throw std::runtime_error("HUD capture canceled");
    if (std::chrono::steady_clock::now() >= deadline)
        throw std::runtime_error("Fresh game compositor capture exceeded 250 ms" + attempt.describe());
}

struct CaptureResources {
    // Devices/items can survive the calling worker, but no session remains
    // active between requests. Pausing/mortar mode does not acquire frames.
    RuntimeLease runtime;
    WindowGeometry source;
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    IDirect3DDevice graphics_device{nullptr};
    GraphicsCaptureItem item{nullptr};
    std::shared_ptr<std::atomic_bool> closed = std::make_shared<std::atomic_bool>(false);
    GraphicsCaptureItem::Closed_revoker closed_subscription;
    winrt::com_ptr<ID3D11Texture2D> staging;
    winrt::Windows::Graphics::SizeInt32 size{};
    RECT crop{};

    explicit CaptureResources(WindowGeometry geometry) : source(geometry) {
        if (!ApiInformation::IsMethodPresent(
                L"Windows.Graphics.Capture.Direct3D11CaptureFramePool", L"CreateFreeThreaded") ||
            !GraphicsCaptureSession::IsSupported())
            throw std::runtime_error("Windows Graphics Capture is unavailable; fresh HUD evidence cannot be captured");
        winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            device.put(), nullptr, context.put()));
        context.as<ID3D11Multithread>()->SetMultithreadProtected(TRUE);
        const auto dxgi = device.as<IDXGIDevice>();
        winrt::com_ptr<IInspectable> inspectable;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()));
        graphics_device = inspectable.as<IDirect3DDevice>();
        const auto factory = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        winrt::check_hresult(factory->CreateForWindow(source.window, winrt::guid_of<GraphicsCaptureItem>(),
                                                     winrt::put_abi(item)));
        // Some games replace the captured window without an apparent visual
        // close. The callback owns only a flag; it never dereferences this
        // cache, whose lifetime can end on another capture worker.
        closed_subscription = item.Closed(winrt::auto_revoke,
            [state = closed](const auto&, const auto&) noexcept { state->store(true, std::memory_order_release); });
        size = item.Size();
        crop = client_crop(source, size);
        D3D11_TEXTURE2D_DESC descriptor{};
        descriptor.Width = static_cast<UINT>(source.client.right);
        descriptor.Height = static_cast<UINT>(source.client.bottom);
        descriptor.MipLevels = 1; descriptor.ArraySize = 1;
        descriptor.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        descriptor.SampleDesc.Count = 1;
        descriptor.Usage = D3D11_USAGE_STAGING; descriptor.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        winrt::check_hresult(device->CreateTexture2D(&descriptor, nullptr, staging.put()));
    }
};

struct RequestSession {
    Direct3D11CaptureFramePool pool{nullptr};
    GraphicsCaptureSession session{nullptr};
    ~RequestSession() {
        try { if (session) session.Close(); }
        catch (...) { log_error("capture.session_close_failed"); }
        try { if (pool) pool.Close(); }
        catch (...) { log_error("capture.frame_pool_close_failed"); }
    }
};

Image read_client_pixels(CaptureResources& resources, const Direct3D11CaptureFrame& frame,
                        std::stop_token stop, std::chrono::steady_clock::time_point deadline,
                        CaptureAttempt& attempt) {
    attempt.enter("readback");
    const auto content = frame.ContentSize();
    if (content.Width != resources.size.Width || content.Height != resources.size.Height)
        throw std::runtime_error("Game client content size changed during compositor capture");
    winrt::com_ptr<ID3D11Texture2D> source;
    winrt::check_hresult(frame.Surface().as<DxgiAccess>()->GetInterface(__uuidof(ID3D11Texture2D), source.put_void()));
    D3D11_TEXTURE2D_DESC descriptor{};
    source->GetDesc(&descriptor);
    if (descriptor.Format != DXGI_FORMAT_B8G8R8A8_UNORM || descriptor.SampleDesc.Count != 1 ||
        descriptor.Width < static_cast<UINT>(content.Width) || descriptor.Height < static_cast<UINT>(content.Height))
        throw std::runtime_error("Unsupported or clipped game compositor surface");
    const auto& crop = resources.crop;
    const D3D11_BOX box{static_cast<UINT>(crop.left), static_cast<UINT>(crop.top), 0,
        static_cast<UINT>(crop.right), static_cast<UINT>(crop.bottom), 1};
    resources.context->CopySubresourceRegion(resources.staging.get(), 0, 0, 0, 0, source.get(), 0, &box);
    resources.context->Flush();
    D3D11_MAPPED_SUBRESOURCE mapped{};
    for (;;) {
        check_request(stop, deadline, attempt);
        const auto result = resources.context->Map(resources.staging.get(), 0, D3D11_MAP_READ,
                                                   D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (result == DXGI_ERROR_WAS_STILL_DRAWING) { ++attempt.gpu_waits; Sleep(1); continue; }
        winrt::check_hresult(result);
        break;
    }
    struct Mapped {
        CaptureResources& resources;
        ~Mapped() { resources.context->Unmap(resources.staging.get(), 0); }
    } unmap{resources};
    const int width = resources.source.client.right, height = resources.source.client.bottom;
    if (!mapped.pData || mapped.RowPitch < static_cast<std::uint64_t>(width) * 4)
        throw std::runtime_error("Invalid compositor readback stride");
    Image image{width, height, std::vector<std::uint8_t>(detail::checked_capture_pixel_count(width, height) * 3)};
    attempt.enter("pixels");
    for (int y = 0; y < height; ++y) {
        check_request(stop, deadline, attempt);
        const auto* row = static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch;
        auto* destination = image.bgr.data() + static_cast<std::size_t>(y) * width * 3;
        for (int x = 0; x < width; ++x) {
            destination[x * 3] = row[x * 4];
            destination[x * 3 + 1] = row[x * 4 + 1];
            destination[x * 3 + 2] = row[x * 4 + 2];
        }
    }
    return image;
}

}  // namespace

GameClientCapture capture_game_client_frame(HWND window, std::stop_token stop) {
    const auto started = GetTickCount64();
    CaptureAttempt attempt;
    const auto deadline = attempt.started + std::chrono::milliseconds(250);
    check_request(stop, deadline, attempt);
    const PhysicalPixels physical_pixels;
    const auto before = geometry(window);
    const Apartment apartment;
    static std::timed_mutex mutex;
    static std::unique_ptr<CaptureResources> resources;
    std::unique_lock lock(mutex, std::defer_lock);
    attempt.enter("admission");
    while (!lock.try_lock_for(std::chrono::milliseconds(2))) check_request(stop, deadline, attempt);
    try {
        attempt.enter("init");
        if (!resources || resources->closed->load(std::memory_order_acquire) ||
            !same_geometry(resources->source, before))
            resources = std::make_unique<CaptureResources>(before);
        check_request(stop, deadline, attempt);
        if (resources->closed->load(std::memory_order_acquire))
            throw std::runtime_error("Game capture item closed during initialization" + attempt.describe());
        RequestSession request;
        attempt.enter("pool");
        request.pool = Direct3D11CaptureFramePool::CreateFreeThreaded(resources->graphics_device,
            DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, resources->size);
        check_request(stop, deadline, attempt);
        request.session = request.pool.CreateCaptureSession(resources->item);
        if (ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsCursorCaptureEnabled"))
            request.session.IsCursorCaptureEnabled(false);
        attempt.enter("sourceclock");
        const auto requested_qpc = qpc_100ns();
        attempt.requested_qpc = requested_qpc;
        attempt.enter("start_capture");
        request.session.StartCapture();
        check_request(stop, deadline, attempt);
        attempt.enter("frame");
        for (;;) {
            check_request(stop, deadline, attempt);
            if (resources->closed->load(std::memory_order_acquire))
                throw std::runtime_error("Game capture item closed while awaiting a fresh frame" + attempt.describe());
            const auto frame = request.pool.TryGetNextFrame();
            if (!frame) { Sleep(2); continue; }
            ++attempt.frames_received;
            attempt.enter("sourceclock");
            const auto source_qpc = frame.SystemRelativeTime().count();
            attempt.source_qpc = source_qpc;
            const auto acquired_qpc = qpc_100ns();
            if (source_qpc < requested_qpc) {
                ++attempt.stale_frames; frame.Close(); attempt.enter("frame"); continue;
            }
            if (source_qpc <= 0 || source_qpc > acquired_qpc)
                throw std::runtime_error("Invalid compositor frame clock; freshness cannot be verified" + attempt.describe());
            auto image = read_client_pixels(*resources, frame, stop, deadline, attempt);
            frame.Close();
            attempt.enter("geometry_verify");
            if (resources->closed->load(std::memory_order_acquire))
                throw std::runtime_error("Game capture item closed during readback" + attempt.describe());
            if (!same_geometry(before, geometry(window)))
                throw std::runtime_error("Game client geometry changed during fresh HUD capture");
            check_request(stop, deadline, attempt);
            return {std::move(image), GetTickCount64(), started, static_cast<std::uint64_t>(source_qpc)};
        }
    } catch (const winrt::hresult_error& error) {
        resources.reset();
        throw std::runtime_error("Windows Graphics Capture failed; HRESULT=" +
                                 std::to_string(static_cast<std::int32_t>(error.code())) + attempt.describe());
    }
}

Image capture_game_client(HWND window, std::stop_token stop) {
    return capture_game_client_frame(window, stop).image;
}

}  // namespace wardogs
