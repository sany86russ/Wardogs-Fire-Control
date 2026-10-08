#include "wardogs/settings.hpp"
#include "wardogs/hotkeys.hpp"
#include "wardogs/logger.hpp"

#include <Windows.h>
#include <ShlObj.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace wardogs {
namespace {

constexpr LONGLONG maximum_ini_bytes = 8 * 1024 * 1024;

void assign_quick_workflow_hotkeys(AppSettings& settings) {
    const std::array preserved{
        parse_hotkey(settings.region_hotkey), parse_hotkey(settings.target_hotkey),
        parse_hotkey(settings.quick_target_hotkey), parse_hotkey(settings.impact_hotkey),
        parse_hotkey(settings.ghost_arc_hotkey), parse_hotkey(settings.pinned_card.unlock_hotkey)};
    constexpr UINT modifier_mask = MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN;
    const auto available = [&](wchar_t key) {
        for (std::wstring_view prefix : {L"Alt+", L"Ctrl+Alt+", L"Alt+Shift+", L"Ctrl+Alt+Shift+"}) {
            const auto candidate = parse_hotkey(std::wstring{prefix} + key);
            const bool occupied = std::any_of(preserved.begin(), preserved.end(), [&](const Hotkey& existing) {
                return existing.virtual_key == candidate.virtual_key &&
                    (existing.modifiers & modifier_mask) == (candidate.modifiers & modifier_mask);
            });
            if (!occupied) return candidate.display;
        }
        throw std::invalid_argument("Нет свободного сочетания для нового сценария: измените дополнительные горячие клавиши");
    };
    // These two defaults are generated during migration. Other user actions
    // keep their assignments; duplicate validation remains strict elsewhere.
    settings.base_hotkey = available(L'X');
    settings.exit_game_mode_hotkey = available(L'C');
}

std::runtime_error settings_error(const char* message, DWORD error = GetLastError()) {
    return std::runtime_error(std::string{message} + " (код Windows " +
                              std::to_string(error) + ")");
}

std::wstring normalized_exit_hotkey(std::wstring_view value) {
    try { return parse_hotkey(value).display; }
    catch (const std::exception&) {
        throw std::invalid_argument("Некорректная горячая клавиша выхода из игрового режима.");
    }
}

std::filesystem::path local_app_data() {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr,
                                  &raw))) {
        throw std::runtime_error("Не удалось найти папку настроек пользователя.");
    }
    const std::filesystem::path result{raw};
    CoTaskMemFree(raw);
    return result;
}

std::filesystem::path legacy_settings_path() {
    return local_app_data() / L"WarDogsDistanceCalculatorCpp" / L"settings.ini";
}

std::filesystem::path existing_source(const std::filesystem::path& path,
                                       const std::filesystem::path& legacy) {
    if (std::filesystem::exists(path) || legacy.empty() ||
        !std::filesystem::is_regular_file(legacy)) {
        return path;
    }
    return std::filesystem::absolute(legacy);
}

class TemporaryIni {
public:
    explicit TemporaryIni(const std::filesystem::path& destination) {
        static std::atomic<unsigned long> sequence{};
        for (unsigned attempt = 0; attempt < 32; ++attempt) {
            path_ = destination;
            path_ += L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                     std::to_wstring(GetTickCount64()) + L"-" +
                     std::to_wstring(sequence.fetch_add(1));
            HANDLE file = CreateFileW(path_.c_str(), GENERIC_WRITE, 0, nullptr,
                                      CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) {
                if (GetLastError() == ERROR_FILE_EXISTS ||
                    GetLastError() == ERROR_ALREADY_EXISTS) continue;
                throw settings_error("Не удалось подготовить файл настроек");
            }
            constexpr unsigned char utf16_bom[]{0xff, 0xfe};
            DWORD written = 0;
            const bool initialized = WriteFile(file, utf16_bom, sizeof(utf16_bom),
                                                &written, nullptr) != FALSE &&
                                     written == sizeof(utf16_bom);
            const DWORD error = GetLastError();
            CloseHandle(file);
            if (!initialized) {
                DeleteFileW(path_.c_str());
                throw settings_error("Не удалось создать файл настроек", error);
            }
            return;
        }
        throw std::runtime_error("Не удалось создать временный файл настроек.");
    }
    ~TemporaryIni() { if (!path_.empty()) DeleteFileW(path_.c_str()); }
    TemporaryIni(const TemporaryIni&) = delete;
    TemporaryIni& operator=(const TemporaryIni&) = delete;
    const std::filesystem::path& path() const noexcept { return path_; }
    void committed() noexcept { path_.clear(); }
private:
    std::filesystem::path path_;
};

void ensure_unicode_ini(const std::filesystem::path& path) {
    constexpr unsigned char bom[]{0xff, 0xfe};
    struct FileHandle {
        HANDLE value;
        ~FileHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    } file{CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (file.value == INVALID_HANDLE_VALUE)
        throw settings_error("Не удалось подготовить кодировку настроек");
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.value, &size))
        throw settings_error("Не удалось определить размер настроек");
    if (size.QuadPart < 0 || size.QuadPart > maximum_ini_bytes)
        throw std::invalid_argument("Файл настроек превышает безопасный предел 8 МиБ");
    std::vector<char> encoded(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    if (!encoded.empty() && (!ReadFile(file.value, encoded.data(),
            static_cast<DWORD>(encoded.size()), &read, nullptr) || read != encoded.size()))
        throw settings_error("Не удалось прочитать копию настроек", ERROR_READ_FAULT);
    const auto begins = [&](std::initializer_list<unsigned char> prefix) {
        if (encoded.size() < prefix.size()) return false;
        return std::equal(prefix.begin(), prefix.end(), encoded.begin(),
            [](unsigned char expected, char actual) {
                return expected == static_cast<unsigned char>(actual);
            });
    };
    if (begins({0xef, 0xbb, 0xbf}) || begins({0xfe, 0xff}) ||
        begins({0xff, 0xfe, 0, 0}) || begins({0, 0, 0xfe, 0xff}))
        throw std::invalid_argument("Кодировка файла настроек не поддерживается: требуется ANSI или UTF-16LE");
    if (begins({0xff, 0xfe})) {
        if (encoded.size() % sizeof(wchar_t) != 0)
            throw std::invalid_argument("Файл настроек содержит неполную строку UTF-16LE");
        return;
    }
    if (std::find(encoded.begin(), encoded.end(), '\0') != encoded.end())
        throw std::invalid_argument("Файл настроек содержит недопустимый нулевой байт");
    std::vector<wchar_t> decoded;
    if (!encoded.empty()) {
        const int characters = MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS,
            encoded.data(), static_cast<int>(encoded.size()), nullptr, 0);
        if (characters == 0)
            throw settings_error("Копия настроек содержит недопустимый текст ANSI");
        if (sizeof(bom) + static_cast<std::size_t>(characters) * sizeof(wchar_t) >
            static_cast<std::size_t>(maximum_ini_bytes))
            throw std::invalid_argument("Настройки после преобразования в Unicode превышают безопасный предел 8 МиБ");
        decoded.resize(static_cast<std::size_t>(characters));
        if (MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, encoded.data(),
                static_cast<int>(encoded.size()), decoded.data(), characters) != characters)
            throw settings_error("Не удалось преобразовать копию настроек в Unicode");
    }
    if (!SetFilePointerEx(file.value, {}, nullptr, FILE_BEGIN))
        throw settings_error("Не удалось подготовить запись настроек Unicode");
    DWORD written = 0;
    if (!WriteFile(file.value, bom, sizeof(bom), &written, nullptr) || written != sizeof(bom))
        throw settings_error("Не удалось записать кодировку настроек", ERROR_WRITE_FAULT);
    const DWORD decoded_bytes = static_cast<DWORD>(decoded.size() * sizeof(wchar_t));
    if (decoded_bytes != 0 && (!WriteFile(file.value, decoded.data(), decoded_bytes,
            &written, nullptr) || written != decoded_bytes))
        throw settings_error("Не удалось записать настройки Unicode", ERROR_WRITE_FAULT);
    if (!SetEndOfFile(file.value))
        throw settings_error("Не удалось завершить преобразование настроек Unicode");
}

void flush_ini(const std::filesystem::path& path) {
    // The Win32 documentation explicitly permits a zero return for a cache
    // flush. Every individual write is checked above; durable file I/O below
    // supplies the meaningful failure check for the completed temporary INI.
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        throw settings_error("Не удалось открыть сохранённые настройки");
    const bool flushed = FlushFileBuffers(file) != FALSE;
    const DWORD error = GetLastError();
    CloseHandle(file);
    if (!flushed) throw settings_error("Не удалось записать настройки на диск", error);
}

std::wstring read_value(const std::filesystem::path& path, const wchar_t* key,
                        std::wstring_view fallback) {
    std::array<wchar_t, 8192> buffer{};
    GetPrivateProfileStringW(L"settings", key, std::wstring{fallback}.c_str(),
                             buffer.data(), static_cast<DWORD>(buffer.size()),
                             path.c_str());
    return buffer.data();
}

void write_value(const std::filesystem::path& path, const wchar_t* key,
                 const std::wstring& value) {
    if (!WritePrivateProfileStringW(L"settings", key, value.c_str(), path.c_str())) {
        throw settings_error("Не удалось сохранить настройки");
    }
}

std::optional<long> read_integer(const std::filesystem::path& path,
                                 const wchar_t* key) {
    const std::wstring text = read_value(path, key, L"");
    if (text.empty()) return std::nullopt;
    try {
        std::size_t consumed = 0;
        const long value = std::stol(text, &consumed);
        if (consumed != text.size()) return std::nullopt;
        return value;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<double> read_double(const std::filesystem::path& path,
                                  const wchar_t* key) {
    const std::wstring text = read_value(path, key, L"");
    if (text.empty()) return std::nullopt;
    const auto invalid = []() -> std::optional<double> {
        log_warning("Некорректная числовая настройка: использовано значение по умолчанию.");
        return {};
    };
    const auto ascii_space = [](wchar_t value) {
        return value == L' ' || value == L'\t' || value == L'\r' ||
            value == L'\n' || value == L'\v' || value == L'\f';
    };
    auto first = text.begin();
    auto last = text.end();
    while (first != last && ascii_space(*first)) ++first;
    while (first != last && ascii_space(*std::prev(last))) --last;
    std::string normalized;
    normalized.reserve(static_cast<std::size_t>(std::distance(first, last)));
    for (auto current = first; current != last; ++current) {
        if (*current < 0 || *current > 127) return invalid();
        normalized.push_back(static_cast<char>(*current));
    }
    // Old std::to_wstring profiles used the current Windows decimal separator.
    // Accept one legacy decimal comma, while refusing mixed/grouped notation.
    if (normalized.find(',') != std::string::npos) {
        if (normalized.find('.') != std::string::npos ||
            std::count(normalized.begin(), normalized.end(), ',') != 1)
            return invalid();
        std::replace(normalized.begin(), normalized.end(), ',', '.');
    }
    const char* begin = normalized.data();
    const char* end = begin + normalized.size();
    if (begin != end && *begin == '+') ++begin;
    double value{};
    const auto parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end || !std::isfinite(value))
        return invalid();
    return value;
}

std::wstring format_double(double value) {
    if (!std::isfinite(value))
        throw std::invalid_argument("Поправка прицела должна быть конечным числом");
    std::array<char, 64> buffer{};
    const auto formatted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (formatted.ec != std::errc{})
        throw std::runtime_error("Не удалось сохранить числовую настройку");
    return {buffer.data(), formatted.ptr};
}

void remove_value(const std::filesystem::path& path, const wchar_t* key) {
    if (!WritePrivateProfileStringW(L"settings", key, nullptr, path.c_str())) {
        throw settings_error("Не удалось удалить старое значение настройки");
    }
}

}  // namespace

std::filesystem::path settings_path() {
    return local_app_data() / L"WardogsFireControl" / L"settings.ini";
}

AppSettings load_settings_from(const std::filesystem::path& requested_path,
                               const std::filesystem::path& legacy_path) {
    const auto path = existing_source(std::filesystem::absolute(requested_path),
                                      legacy_path);
    AppSettings settings;
    settings.language = read_value(path, L"ui_language", L"ru") == L"en"
        ? UiLanguage::english : UiLanguage::russian;
    if (const auto enabled = read_integer(path, L"check_updates_on_start")) {
        if (*enabled == 0 || *enabled == 1)
            settings.check_updates_on_start = *enabled == 1;
    }
    const auto map_key = read_value(path, L"last_game_map", L"");
    settings.last_game_map = game_map_from_key(map_key);
    if (!map_key.empty() && settings.last_game_map == GameMap::unselected)
        log_warning("settings.invalid_game_map; a new explicit map choice is required");
    settings.region_hotkey = read_value(path, L"region_hotkey", settings.region_hotkey);
    settings.base_hotkey = read_value(path, L"base_hotkey", settings.base_hotkey);
    settings.target_hotkey = read_value(path, L"target_hotkey", settings.target_hotkey);
    settings.quick_target_hotkey =
        read_value(path, L"quick_target_hotkey", settings.quick_target_hotkey);
    settings.impact_hotkey = read_value(path, L"impact_hotkey", settings.impact_hotkey);
    settings.ghost_arc_hotkey =
        read_value(path, L"ghost_arc_hotkey", settings.ghost_arc_hotkey);
    try {
        settings.exit_game_mode_hotkey = normalized_exit_hotkey(read_value(
            path, L"exit_game_mode_hotkey", settings.exit_game_mode_hotkey));
    } catch (const std::exception&) {
        log_warning("Некорректная клавиша выхода из игрового режима: использовано Alt+C.");
    }
    if (const auto enabled = read_integer(path, L"game_integration_enabled")) {
        if (*enabled == 0 || *enabled == 1)
            settings.game_integration_enabled = *enabled == 1;
    }
    if (const auto enabled = read_integer(path, L"middle_mouse_enabled")) {
        if (*enabled == 0 || *enabled == 1)
            settings.middle_mouse_enabled = *enabled == 1;
    }
    if (const auto delay = read_integer(path, L"mouse_capture_delay_ms")) {
        settings.mouse_capture_delay_ms = static_cast<int>(std::clamp(
            *delay, static_cast<long>(AppSettings::minimum_mouse_capture_delay_ms),
            static_cast<long>(AppSettings::maximum_mouse_capture_delay_ms)));
    }
    settings.coordinate_pattern =
        normalize_ocr_coordinate_pattern(read_value(path, L"coordinate_pattern", settings.coordinate_pattern));
    settings.backend = read_value(path, L"ocr_backend", L"rapid") == L"windows"
                           ? OcrBackend::windows
                           : OcrBackend::rapid;
    settings.pinned_card.locked =
        read_value(path, L"pinned_card_locked", L"0") == L"1";
    settings.pinned_card.unlock_hotkey = read_value(
        path, L"pinned_card_unlock_hotkey",
        settings.pinned_card.unlock_hotkey);
    if (const auto opacity = read_integer(path, L"pinned_card_opacity_percent")) {
        settings.pinned_card.opacity_percent = static_cast<int>(std::clamp(
            *opacity,
            static_cast<long>(PinnedCardPreferences::minimum_opacity_percent),
            static_cast<long>(PinnedCardPreferences::maximum_opacity_percent)));
    }
    if (const auto opacity = read_integer(path, L"ghost_reticle_opacity_percent")) {
        settings.ghost_reticle.opacity_percent = static_cast<int>(std::clamp(
            *opacity,
            static_cast<long>(GhostReticlePreferences::minimum_opacity_percent),
            static_cast<long>(GhostReticlePreferences::maximum_opacity_percent)));
    }
    if (const auto width = read_integer(path, L"ghost_reticle_width")) {
        settings.ghost_reticle.width = static_cast<int>(std::clamp(
            *width, static_cast<long>(GhostReticlePreferences::minimum_width),
            static_cast<long>(GhostReticlePreferences::maximum_width)));
    }
    if (const auto compensation = read_double(
            path, L"ghost_reticle_bearing_compensation_deg")) {
        settings.ghost_reticle.bearing_compensation_deg = std::clamp(
            *compensation,
            GhostReticlePreferences::minimum_bearing_compensation_deg,
            GhostReticlePreferences::maximum_bearing_compensation_deg);
    }
    if (const auto width = read_integer(path, L"ghost_preset_screen_width"))
        settings.ghost_reticle.preset_screen_width = static_cast<int>(*width);
    if (const auto height = read_integer(path, L"ghost_preset_screen_height"))
        settings.ghost_reticle.preset_screen_height = static_cast<int>(*height);
    settings.ghost_reticle.preferred_arc =
        read_value(path, L"ghost_reticle_preferred_arc", L"low") == L"high"
            ? Arc::high : Arc::low;
    const std::wstring monitor = read_value(path, L"capture_monitor", L"");
    const auto left = read_integer(path, L"capture_left");
    const auto top = read_integer(path, L"capture_top");
    const auto right = read_integer(path, L"capture_right");
    const auto bottom = read_integer(path, L"capture_bottom");
    if (!monitor.empty() && left && top && right && bottom && *left >= 0 &&
        *top >= 0 && *right > *left && *bottom > *top) {
        settings.capture_region = CaptureRegion{
            monitor, {static_cast<LONG>(*left), static_cast<LONG>(*top),
                      static_cast<LONG>(*right), static_cast<LONG>(*bottom)}};
        const auto width = read_integer(path, L"capture_monitor_width");
        const auto height = read_integer(path, L"capture_monitor_height");
        if (width && height && *width > 0 && *height > 0)
            settings.capture_region->monitor_size = {static_cast<LONG>(*width), static_cast<LONG>(*height)};
    }
    settings.automatic_chat_region = !settings.capture_region.has_value();
    if (const auto automatic = read_integer(path, L"automatic_chat_region")) {
        if (*automatic == 0 || *automatic == 1)
            settings.automatic_chat_region = *automatic == 1;
    }
    if (const auto version = read_integer(path, L"quick_workflow_version");
        version && *version >= AppSettings::current_quick_workflow_version) {
        settings.quick_workflow_version = static_cast<int>(*version);
    } else {
        assign_quick_workflow_hotkeys(settings);
        settings.game_integration_enabled = true;
        settings.middle_mouse_enabled = true;
        settings.automatic_chat_region = true;
        settings.quick_workflow_migrated = std::filesystem::is_regular_file(path);
        if (settings.quick_workflow_migrated)
            log_info("settings.quick_workflow_migrated version=1");
    }
    return settings;
}

void save_settings_to(const std::filesystem::path& requested_path,
                      const AppSettings& settings,
                      const std::filesystem::path& legacy_path) {
    const auto destination = std::filesystem::absolute(requested_path);
    std::filesystem::create_directories(destination.parent_path());
    TemporaryIni temporary{destination};
    const auto& path = temporary.path();
    const auto source = existing_source(destination, legacy_path);
    if (std::filesystem::exists(source)) {
        if (std::filesystem::file_size(source) > maximum_ini_bytes)
            throw std::invalid_argument("Файл настроек превышает безопасный предел 8 МиБ");
        if (!CopyFileW(source.c_str(), path.c_str(), FALSE))
            throw settings_error("Не удалось сохранить копию прежних настроек");
        // CopyFile carries the read-only attribute; the private temporary file
        // must remain writable and removable even if replacing the source fails.
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            !SetFileAttributesW(path.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY))
            throw settings_error("Не удалось подготовить копию настроек");
    }
    // CopyFile overwrites the initial BOM. Win32 writes Unicode values to an
    // ANSI INI with lossy best-fit conversion unless the staged copy is Unicode.
    // Convert the whole text so unknown keys, sections and comments survive.
    ensure_unicode_ini(path);
    write_value(path, L"ui_language", settings.language == UiLanguage::english ? L"en" : L"ru");
    write_value(path, L"check_updates_on_start", settings.check_updates_on_start ? L"1" : L"0");
    write_value(path, L"quick_workflow_version", std::to_wstring(
        std::max(settings.quick_workflow_version, AppSettings::current_quick_workflow_version)));
    write_value(path, L"last_game_map", std::wstring{game_map_key(settings.last_game_map)});
    write_value(path, L"region_hotkey", settings.region_hotkey);
    write_value(path, L"base_hotkey", settings.base_hotkey);
    write_value(path, L"target_hotkey", settings.target_hotkey);
    write_value(path, L"quick_target_hotkey", settings.quick_target_hotkey);
    write_value(path, L"impact_hotkey", settings.impact_hotkey);
    write_value(path, L"ghost_arc_hotkey", settings.ghost_arc_hotkey);
    write_value(path, L"exit_game_mode_hotkey",
                normalized_exit_hotkey(settings.exit_game_mode_hotkey));
    write_value(path, L"game_integration_enabled",
                settings.game_integration_enabled ? L"1" : L"0");
    write_value(path, L"middle_mouse_enabled",
                settings.middle_mouse_enabled ? L"1" : L"0");
    write_value(path, L"mouse_capture_delay_ms", std::to_wstring(std::clamp(
        settings.mouse_capture_delay_ms, AppSettings::minimum_mouse_capture_delay_ms,
        AppSettings::maximum_mouse_capture_delay_ms)));
    write_value(path, L"coordinate_pattern", normalize_ocr_coordinate_pattern(settings.coordinate_pattern));
    write_value(path, L"ocr_backend",
                settings.backend == OcrBackend::rapid ? L"rapid" : L"windows");
    write_value(path, L"automatic_chat_region", settings.automatic_chat_region ? L"1" : L"0");
    write_value(path, L"pinned_card_locked",
                settings.pinned_card.locked ? L"1" : L"0");
    write_value(path, L"pinned_card_unlock_hotkey",
                settings.pinned_card.unlock_hotkey);
    write_value(path, L"pinned_card_opacity_percent",
                std::to_wstring(std::clamp(
                    settings.pinned_card.opacity_percent,
                    PinnedCardPreferences::minimum_opacity_percent,
                    PinnedCardPreferences::maximum_opacity_percent)));
    write_value(path, L"ghost_reticle_opacity_percent",
                std::to_wstring(std::clamp(
                    settings.ghost_reticle.opacity_percent,
                    GhostReticlePreferences::minimum_opacity_percent,
                    GhostReticlePreferences::maximum_opacity_percent)));
    write_value(path, L"ghost_reticle_width",
                std::to_wstring(std::clamp(
                    settings.ghost_reticle.width,
                    GhostReticlePreferences::minimum_width,
                    GhostReticlePreferences::maximum_width)));
    if (!std::isfinite(settings.ghost_reticle.bearing_compensation_deg))
        throw std::invalid_argument("Поправка прицела должна быть конечным числом");
    write_value(
        path, L"ghost_reticle_bearing_compensation_deg",
        format_double(std::clamp(
            settings.ghost_reticle.bearing_compensation_deg,
            GhostReticlePreferences::minimum_bearing_compensation_deg,
            GhostReticlePreferences::maximum_bearing_compensation_deg)));
    write_value(path, L"ghost_preset_screen_width",
                std::to_wstring(settings.ghost_reticle.preset_screen_width));
    write_value(path, L"ghost_preset_screen_height",
                std::to_wstring(settings.ghost_reticle.preset_screen_height));
    write_value(path, L"ghost_reticle_preferred_arc",
                settings.ghost_reticle.preferred_arc == Arc::high
                    ? L"high" : L"low");
    if (settings.capture_region) {
        const auto& region = *settings.capture_region;
        write_value(path, L"capture_monitor", region.monitor_device);
        write_value(path, L"capture_left", std::to_wstring(region.relative.left));
        write_value(path, L"capture_top", std::to_wstring(region.relative.top));
        write_value(path, L"capture_right", std::to_wstring(region.relative.right));
        write_value(path, L"capture_bottom", std::to_wstring(region.relative.bottom));
        write_value(path, L"capture_monitor_width", std::to_wstring(region.monitor_size.cx));
        write_value(path, L"capture_monitor_height", std::to_wstring(region.monitor_size.cy));
    } else {
        for (const wchar_t* key : {L"capture_monitor", L"capture_left", L"capture_top",
                                   L"capture_right", L"capture_bottom", L"capture_monitor_width", L"capture_monitor_height"}) {
            remove_value(path, key);
        }
    }
    flush_ini(path);
    if (std::filesystem::file_size(path) > maximum_ini_bytes)
        throw std::invalid_argument("Сохранённые настройки Unicode превышают безопасный предел 8 МиБ");
    // The old file is never opened for writing. On a sharing/permission failure,
    // MoveFileEx leaves it intact and TemporaryIni removes the partial new file.
    if (!MoveFileExW(path.c_str(), destination.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw settings_error("Не удалось заменить файл настроек; прежние настройки сохранены");
    }
    temporary.committed();
}

AppSettings load_settings() {
    return load_settings_from(settings_path(), legacy_settings_path());
}

void save_settings(const AppSettings& settings) {
    save_settings_to(settings_path(), settings, legacy_settings_path());
}

}  // namespace wardogs
