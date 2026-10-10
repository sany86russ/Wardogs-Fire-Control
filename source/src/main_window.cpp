#include "main_window.hpp"
#include "localization.hpp"
#include "direction_plot.hpp"
#include "wardogs/mouse_trigger.hpp"
#include "app_icon.hpp"
#include "selection_overlay.hpp"
#include "settings_dialog.hpp"
#include "planning_dialog.hpp"
#include "update_ui.hpp"
#include "ghost_reticle_window.hpp"
#include "window_title_bar.hpp"
#include "windows_taskbar.hpp"

#include "wardogs/capture.hpp"
#include "wardogs/continuous_calibration.hpp"
#include "wardogs/impact_feedback.hpp"
#include "wardogs/core.hpp"
#include "wardogs/hotkeys.hpp"
#include "wardogs/logger.hpp"
#include "wardogs/map_capture_consensus.hpp"
#include "wardogs/ocr.hpp"
#include "wardogs/settings.hpp"
#include "wardogs/terrain_package.hpp"
#include "wardogs/vehicle_ballistics.hpp"
#include "wardogs/windows_ocr.hpp"

#include "pinned_result_window.hpp"
#include "vehicle_solution_widget.hpp"

#include <Windows.h>
#include <dwmapi.h>
#include <windowsx.h>
#include <winrt/base.h>

#include <QApplication>
#include <QBoxLayout>
#include <QClipboard>
#include <QChar>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QFileDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMenu>
#include <QMimeData>
#include <QResizeEvent>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSystemTrayIcon>
#include <QTabWidget>
#include <QTextBrowser>
#include <QToolButton>
#include <QWinEventNotifier>
#include <QCloseEvent>
#include <QContextMenuEvent>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QGroupBox>
#include <QGridLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMetaObject>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QScrollBar>
#include <QStringList>
#include <QStyleFactory>
#include <QTimer>
#include <QTemporaryDir>
#include <QListWidget>
#include <QTreeView>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <numbers>
#include <chrono>
#include <deque>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <regex>
#include <sstream>
#include <locale>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>

namespace {

QString qtext(const std::wstring& value) { return QString::fromStdWString(value); }

QString apply_registered_hotkeys(wardogs::AppSettings& settings,
                                 std::span<const wardogs::Hotkey> actual) {
    if (actual.size() != 8)
        throw std::invalid_argument("Windows вернула неполный набор горячих клавиш");
    wardogs::validate_global_hotkeys(actual);
    auto updated = settings;
    const std::array fields{&updated.region_hotkey, &updated.base_hotkey,
        &updated.target_hotkey, &updated.quick_target_hotkey,
        &updated.impact_hotkey, &updated.ghost_arc_hotkey,
        &updated.pinned_card.unlock_hotkey, &updated.exit_game_mode_hotkey};
    const std::array labels{wardogs::i18n::text(QStringLiteral("область")), wardogs::i18n::text(QStringLiteral("орудие")),
        wardogs::i18n::text(QStringLiteral("цель")), wardogs::i18n::text(QStringLiteral("разовая область")),
        wardogs::i18n::text(QStringLiteral("попадание")), wardogs::i18n::text(QStringLiteral("траектория")),
        wardogs::i18n::text(QStringLiteral("разблокировка карточки")), wardogs::i18n::text(QStringLiteral("выход из игры"))};
    QStringList changes;
    for (std::size_t index = 0; index < fields.size(); ++index) {
        const auto requested = wardogs::parse_hotkey(*fields[index]);
        if (requested.modifiers == actual[index].modifiers &&
            requested.virtual_key == actual[index].virtual_key) continue;
        changes.append(labels[index] + QStringLiteral(": ") + qtext(*fields[index]) +
                       QStringLiteral(" → ") + qtext(actual[index].display));
        *fields[index] = actual[index].display;
    }
    settings = std::move(updated);
    return changes.join(QStringLiteral("; "));
}
QString error_text(const std::exception& error) { return wardogs::i18n::text(QString::fromUtf8(error.what())); }
std::string utf8(const QString& value) { return value.toUtf8().toStdString(); }
std::string one_line_utf8(QString value) {
    value.replace(QLatin1Char('\r'), QStringLiteral("\\r"));
    value.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    return utf8(value);
}

std::filesystem::path executable_directory() {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                            static_cast<DWORD>(buffer.size()));
    buffer.resize(length);
    return std::filesystem::path{buffer}.parent_path();
}

QWidget* result_card(const QString& caption, const QString& color, QLabel*& value,
                     QLabel** secondary = nullptr) {
    auto* card = new QFrame;
    card->setObjectName(QStringLiteral("resultCard"));
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(10, 9, 10, 11);
    layout->setSpacing(3);
    auto* label = new QLabel(caption);
    label->setObjectName(QStringLiteral("resultCaption"));
    label->setAlignment(Qt::AlignCenter);
    value = new QLabel(QStringLiteral("—"));
    value->setAlignment(Qt::AlignCenter);
    value->setMinimumHeight(secondary ? 42 : 58);
    value->setMinimumWidth(0);
    value->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    value->setProperty("metric", true);
    value->setStyleSheet(QStringLiteral("color:%1;").arg(color));
    layout->addWidget(label);
    layout->addWidget(value);
    if (secondary) {
        *secondary = new QLabel(QStringLiteral("—"));
        (*secondary)->setObjectName(QStringLiteral("mortarMil"));
        (*secondary)->setAlignment(Qt::AlignCenter);
        (*secondary)->setStyleSheet(QStringLiteral(
            "color:#c4b5fd;font-family:'Bahnschrift';font-size:21px;"
            "font-weight:700;"));
        layout->addWidget(*secondary);
    }
    return card;
}

QIcon pin_icon() {
    QPixmap image(24, 24);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    const QColor color(QStringLiteral("#e2e8f0"));
    painter.setPen(QPen(color, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(color);
    const QPolygonF body{{8.0, 3.0}, {16.0, 3.0}, {14.3, 6.0},
                         {14.3, 11.0}, {17.5, 14.0}, {6.5, 14.0},
                         {9.7, 11.0}, {9.7, 6.0}};
    painter.drawPolygon(body);
    painter.drawLine(QPointF(12.0, 14.0), QPointF(12.0, 21.0));
    return QIcon(image);
}

QIcon weapon_mode_icon(bool vehicle_mode) {
    QPixmap image(24, 24);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    const QColor color(QStringLiteral("#e2e8f0"));
    painter.setPen(QPen(color, 1.8, Qt::SolidLine, Qt::RoundCap,
                        Qt::RoundJoin));
    if (vehicle_mode) {
        painter.drawRoundedRect(QRectF(3, 14, 18, 6), 2, 2);
        painter.drawRoundedRect(QRectF(7, 10, 8, 5), 1, 1);
        painter.drawLine(QPointF(12, 10), QPointF(20.5, 4));
        painter.drawEllipse(QPointF(7, 20), 1.5, 1.5);
        painter.drawEllipse(QPointF(17, 20), 1.5, 1.5);
    } else {
        painter.drawLine(QPointF(7, 18), QPointF(15.5, 5));
        painter.drawLine(QPointF(9, 19), QPointF(17.5, 6));
        painter.drawLine(QPointF(5, 20), QPointF(15, 20));
        painter.drawLine(QPointF(10, 19.5), QPointF(14.5, 14));
    }
    return QIcon(image);
}

enum class UiGlyph { location, target, scan, refresh, clear, settings, reticle };

QIcon ui_icon(UiGlyph glyph) {
    QPixmap image(22, 22);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    const QColor color(QStringLiteral("#d7e1ee"));
    painter.setPen(QPen(color, 1.7, Qt::SolidLine, Qt::RoundCap,
                        Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);

    switch (glyph) {
    case UiGlyph::location:
        painter.drawEllipse(QRectF(6.0, 3.0, 10.0, 10.0));
        painter.drawEllipse(QPointF(11.0, 8.0), 1.7, 1.7);
        painter.drawLine(QPointF(7.4, 11.5), QPointF(11.0, 19.0));
        painter.drawLine(QPointF(14.6, 11.5), QPointF(11.0, 19.0));
        break;
    case UiGlyph::target:
        painter.drawEllipse(QPointF(11.0, 11.0), 6.0, 6.0);
        painter.drawEllipse(QPointF(11.0, 11.0), 2.2, 2.2);
        painter.drawLine(QPointF(11.0, 2.0), QPointF(11.0, 6.0));
        painter.drawLine(QPointF(11.0, 16.0), QPointF(11.0, 20.0));
        painter.drawLine(QPointF(2.0, 11.0), QPointF(6.0, 11.0));
        painter.drawLine(QPointF(16.0, 11.0), QPointF(20.0, 11.0));
        break;
    case UiGlyph::scan:
        painter.drawLine(QPointF(4.0, 8.0), QPointF(4.0, 4.0));
        painter.drawLine(QPointF(4.0, 4.0), QPointF(8.0, 4.0));
        painter.drawLine(QPointF(14.0, 4.0), QPointF(18.0, 4.0));
        painter.drawLine(QPointF(18.0, 4.0), QPointF(18.0, 8.0));
        painter.drawLine(QPointF(4.0, 14.0), QPointF(4.0, 18.0));
        painter.drawLine(QPointF(4.0, 18.0), QPointF(8.0, 18.0));
        painter.drawLine(QPointF(14.0, 18.0), QPointF(18.0, 18.0));
        painter.drawLine(QPointF(18.0, 18.0), QPointF(18.0, 14.0));
        painter.drawLine(QPointF(6.0, 11.0), QPointF(16.0, 11.0));
        break;
    case UiGlyph::refresh:
        painter.drawArc(QRectF(4.0, 4.0, 14.0, 14.0), 35 * 16, 270 * 16);
        painter.drawLine(QPointF(16.8, 4.8), QPointF(17.9, 9.1));
        painter.drawLine(QPointF(16.8, 4.8), QPointF(12.6, 5.8));
        break;
    case UiGlyph::clear:
        painter.drawRoundedRect(QRectF(6.0, 7.0, 10.0, 11.0), 1.5, 1.5);
        painter.drawLine(QPointF(5.0, 6.0), QPointF(17.0, 6.0));
        painter.drawLine(QPointF(8.5, 3.8), QPointF(13.5, 3.8));
        painter.drawLine(QPointF(9.0, 10.0), QPointF(9.0, 15.0));
        painter.drawLine(QPointF(13.0, 10.0), QPointF(13.0, 15.0));
        break;
    case UiGlyph::settings:
        painter.drawEllipse(QPointF(11.0, 11.0), 3.0, 3.0);
        painter.drawEllipse(QPointF(11.0, 11.0), 7.0, 7.0);
        for (int index = 0; index < 8; ++index) {
            constexpr double pi = 3.14159265358979323846;
            const double angle = index * pi / 4.0;
            painter.drawLine(QPointF(11.0 + std::cos(angle) * 7.0,
                                     11.0 + std::sin(angle) * 7.0),
                             QPointF(11.0 + std::cos(angle) * 9.0,
                                     11.0 + std::sin(angle) * 9.0));
        }
        break;
    case UiGlyph::reticle:
        painter.drawEllipse(QPointF(11.0, 11.0), 5.5, 5.5);
        painter.drawEllipse(QPointF(11.0, 11.0), 1.4, 1.4);
        painter.drawLine(QPointF(11.0, 2.0), QPointF(11.0, 6.0));
        painter.drawLine(QPointF(11.0, 16.0), QPointF(11.0, 20.0));
        painter.drawLine(QPointF(2.0, 11.0), QPointF(6.0, 11.0));
        painter.drawLine(QPointF(16.0, 11.0), QPointF(20.0, 11.0));
        break;
    }
    return QIcon(image);
}

enum class OcrAction { base, target, calibration_impact };

const char* action_name(OcrAction action) {
    switch (action) {
    case OcrAction::base: return "base";
    case OcrAction::target: return "target";
    case OcrAction::calibration_impact: return "calibration_impact";
    }
    return "unknown";
}

std::string foreground_summary() {
    const HWND foreground = GetForegroundWindow();
    DWORD process_id = 0;
    if (foreground) GetWindowThreadProcessId(foreground, &process_id);
    // Never open another process or retain other applications' captions/paths.
    return std::string("foreground=") + (!foreground ? "none" :
        process_id == GetCurrentProcessId() ? "own" : "external");
}

bool process_is_elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool elevated = GetTokenInformation(token, TokenElevation, &elevation,
                                               sizeof(elevation), &size) &&
                          elevation.TokenIsElevated != 0;
    CloseHandle(token);
    return elevated;
}

bool is_wardogs_window_title(std::wstring name) {
    // The real game caption can be padded (observed: "Wardogs  ").
    // Use Unicode whitespace rules without depending on the process locale.
    const auto not_space = [](wchar_t ch) { return !QChar::isSpace(static_cast<char32_t>(ch)); };
    name.erase(std::find_if(name.rbegin(), name.rend(), not_space).base(), name.end());
    name.erase(name.begin(), std::find_if(name.begin(), name.end(), not_space));
    std::transform(name.begin(), name.end(), name.begin(), [](wchar_t ch) { return std::towlower(ch); });
    // Window metadata is only a conservative convenience gate, never a
    // trustworthy process identity or anti-cheat compatibility certificate.
    return name == L"wardogs" || name == L"wardogsclient" ||
           name == L"wardogsclient-win64-shipping" ||
           name.starts_with(L"wardogs (64-bit, pc") ||
           name.starts_with(L"wardogsclient (64-bit, pc");
}

bool is_wardogs_window(HWND window) {
    DWORD pid = 0;
    if (!window || !GetWindowThreadProcessId(window, &pid) || pid == GetCurrentProcessId()) return false;
    std::array<wchar_t, 512> title{};
    if (GetWindowTextW(window, title.data(), static_cast<int>(title.size())) <= 0) return false;
    std::array<wchar_t, 128> window_class{};
    if (GetClassNameW(window, window_class.data(), static_cast<int>(window_class.size())) <= 0) return false;
    return std::wstring_view(window_class.data()) == L"UnrealWindow" &&
           is_wardogs_window_title(title.data());
}

bool wardogs_is_foreground() { return is_wardogs_window(GetForegroundWindow()); }

struct AutomaticChatCapture { wardogs::CaptureRegion region; HWND window{}; };

AutomaticChatCapture automatic_chat_capture_region() {
    HWND window = GetForegroundWindow();
    // Only visible game windows are eligible. Never activate or open a process.
    if (!is_wardogs_window(window)) {
        struct Search { HWND match{}; unsigned count{}; bool failed{}; } search;
        EnumWindows([](HWND candidate, LPARAM data) noexcept -> BOOL {
            auto& found = *reinterpret_cast<Search*>(data);
            try {
                if (IsWindowVisible(candidate) && !IsIconic(candidate) && is_wardogs_window(candidate)) {
                    found.match = candidate;
                    ++found.count;
                }
            } catch (...) { found.failed = true; return FALSE; }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&search));
        if (search.failed) throw std::runtime_error("Не удалось проверить окно игры.");
        if (search.count != 1)
            throw std::runtime_error("Откройте одно окно WARDOGS и используйте клавишу захвата. Или выберите область вручную.");
        window = search.match;
    }
    if (!IsWindowVisible(window) || IsIconic(window))
        throw std::runtime_error("Окно WARDOGS свёрнуто. Откройте игру перед захватом.");
    RECT client{};
    if (!GetClientRect(window, &client)) throw std::runtime_error("Не удалось определить размер окна WARDOGS.");
    POINT origin{client.left, client.top};
    if (!ClientToScreen(window, &origin)) throw std::runtime_error("Не удалось определить положение окна WARDOGS.");
    const RECT physical{origin.x, origin.y, origin.x + client.right, origin.y + client.bottom};
    const auto area = wardogs::make_chat_search_rect(physical);
    const HMONITOR monitor = MonitorFromPoint({area.left, area.top}, MONITOR_DEFAULTTONULL);
    MONITORINFO info{sizeof(info)};
    if (!monitor || !GetMonitorInfoW(monitor, &info) || area.left < info.rcMonitor.left ||
        area.top < info.rcMonitor.top || area.right > info.rcMonitor.right || area.bottom > info.rcMonitor.bottom)
        throw std::runtime_error("Чат выходит за границы одного монитора. Переместите окно игры или выберите одну строку вручную.");
    return {wardogs::make_capture_region(monitor, area), window};
}

struct OcrMessage {
    bool success{};
    OcrAction action{OcrAction::target};
    wardogs::Point point{};
    std::wstring text;
    float confidence{};
    wardogs::OcrCoordinateAssessment assessment;
    bool confirmed{};
    bool automatic_chat{};
    bool force_review{};
    QString evidence_review_reason;
    bool map_coordinates{};
    std::optional<wardogs::MiddleMouseEvent> map_event;

    [[nodiscard]] bool requires_review() const noexcept {
        return force_review || assessment.requires_confirmation();
    }
    QString error;
    std::optional<wardogs::Point> impact_target;
    std::optional<wardogs::FiringSnapshot> impact_firing;
    std::uint64_t calibration_epoch{};
    std::uint64_t input_epoch{};
    double elapsed_ms{};
};

struct CapturedOcrJob {
    wardogs::Image image;
    std::optional<wardogs::MapOcrSearchLayout> map_layout;
    OcrMessage context;
};

struct MapCapture {
    wardogs::CaptureRegion search;
    wardogs::MapOcrSearchLayout layout;
    HWND window{};
    RECT client{};
};

MapCapture map_capture_regions(wardogs::MiddleMouseEvent event, double scale = 1.0) {
    const auto window = reinterpret_cast<HWND>(event.foreground_window);
    if (!window || GetForegroundWindow() != window || !is_wardogs_window(window))
        throw std::runtime_error("Средняя кнопка считывает карту только в активном окне WARDOGS.");
    RECT client{};
    POINT origin{};
    if (!GetClientRect(window, &client) || !ClientToScreen(window, &origin))
        throw std::runtime_error("Не удалось определить границы карты WARDOGS.");
    const RECT physical{origin.x, origin.y, origin.x + client.right, origin.y + client.bottom};
    const POINT cursor{event.x, event.y};
    const auto fields = wardogs::make_map_coordinate_search_rects(physical, cursor, scale);
    const HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONULL);
    MONITORINFO info{sizeof(info)};
    if (!monitor || !GetMonitorInfoW(monitor, &info))
        throw std::runtime_error("Монитор карты недоступен.");
    for (const auto& field : {fields.search})
        if (field.left < info.rcMonitor.left || field.top < info.rcMonitor.top ||
            field.right > info.rcMonitor.right || field.bottom > info.rcMonitor.bottom)
            throw std::runtime_error("Подпись карты выходит за границы одного монитора.");
    const auto local = [&](RECT field) -> wardogs::ImageRect {
        return {field.left - fields.search.left, field.top - fields.search.top,
                field.right - fields.search.left, field.bottom - fields.search.top};
    };
    wardogs::MapOcrSearchLayout layout{local(fields.preferred.x_field), local(fields.preferred.y_field),
        cursor.x - fields.search.left, cursor.y - fields.search.top, scale};
    return {wardogs::make_capture_region(monitor, fields.search), layout, window, physical};
}

bool map_source_matches(wardogs::MiddleMouseEvent event, const std::optional<RECT>& expected_client) {
    const auto window = reinterpret_cast<HWND>(event.foreground_window);
    POINT cursor{};
    if (!window || GetForegroundWindow() != window || !is_wardogs_window(window) ||
        !GetCursorPos(&cursor) || cursor.x != event.x || cursor.y != event.y) return false;
    if (!expected_client) return true;
    RECT client{};
    POINT origin{};
    if (!GetClientRect(window, &client) || !ClientToScreen(window, &origin)) return false;
    const RECT physical{origin.x, origin.y, origin.x + client.right, origin.y + client.bottom};
    return EqualRect(&physical, &*expected_client) != FALSE;
}

class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(bool diagnostic = false) : diagnostic_(diagnostic) {
        try {
            settings_ = diagnostic_ ? wardogs::AppSettings{} : wardogs::load_settings();
            wardogs::log_info("settings.loaded");
        } catch (const std::exception& error) {
            settings_ = {};
            startup_error_ = error_text(error);
            wardogs::log_warning("settings.load_failed error=" +
                                 std::string(error.what()));
        }
        if (diagnostic_) settings_.language = wardogs::i18n::language();
        else wardogs::i18n::set_language(settings_.language);
        configure_frameless_window(this);
        bool saved_region_invalid = false;
        if (settings_.capture_region) {
            region_ = settings_.capture_region;
            try {
                (void)wardogs::resolve_capture_region(*settings_.capture_region);
            } catch (...) {
                // A disconnected or resized display does not delete the
                // user's selection. Capture validates it again before OCR.
                saved_region_invalid = true;
            }
        }
        setWindowTitle(QStringLiteral("WARDOGS Fire Control"));
        resize(1040, 790);
        setMinimumSize(580, 320);
        terrain_discovery_ = wardogs::discover_available_terrain_maps();
        build_ui();
        ghost_window_ = std::make_unique<GhostReticleWindow>(
            settings_.ghost_reticle, [this](int width) {
                settings_.ghost_reticle.width = width;
                try { if (!diagnostic_) wardogs::save_settings(settings_); }
                catch (const std::exception& e) { wardogs::log_warning("settings.save_failed error=" + std::string(e.what())); }
                set_status(wardogs::i18n::text(QStringLiteral("Размер прицела сохранён")));
            });
        sync_ghost_monitor();
        update_continuous_controls();
        update_coordinates();
        update_engine_summary();
        update_action_labels();
        update_region_summary();
        fit_window_to_content();
        QTimer::singleShot(0, this, [this] { fit_window_to_content(); });
        if (saved_region_invalid && !settings_.automatic_chat_region)
            set_status(wardogs::i18n::text(QStringLiteral("Дополнительная область недоступна. %1 продолжает находить орудие автоматически."))
                .arg(qtext(settings_.base_hotkey)));
        else if (region_)
            set_status(wardogs::i18n::text(QStringLiteral("Дополнительная область восстановлена. Орудие: M → ПКМ → Отметить координаты → ")) + qtext(settings_.base_hotkey));
        enable_rounded_window_corners(this);
        setup_tray();
        if (!diagnostic_) unlock_event_ = wardogs_ui::create_pinned_unlock_event();
        if (unlock_event_) {
            unlock_notifier_ = new QWinEventNotifier(unlock_event_, this);
            connect(unlock_notifier_, &QWinEventNotifier::activated, this,
                    [this](HANDLE) { unlock_pinned_window("taskbar"); });
        } else if (!diagnostic_) {
            wardogs::log_warning(
                "pinned.unlock_event_create_failed windows_error=" +
                std::to_string(GetLastError()));
        }
        bool startup_hotkeys_ready = diagnostic_;
        try {
            if (!diagnostic_) {
                restore_current_hotkeys();
                startup_hotkeys_ready = true;
            }
        }
        catch (const std::exception& error) {
            wardogs::log_error("hotkey.startup_failed error=" +
                               std::string(error.what()));
            set_status(wardogs::i18n::text(QStringLiteral("Горячие клавиши недоступны: ")) + error_text(error), true);
        }
        if (!diagnostic_ && settings_.quick_workflow_migrated) {
            try {
                wardogs::save_settings(settings_);
                settings_.quick_workflow_migrated = false;
                if (startup_hotkeys_ready)
                    set_status(wardogs::i18n::text(QStringLiteral("Быстрая игра готова: ")) + qtext(settings_.base_hotkey) +
                        wardogs::i18n::text(QStringLiteral(" — орудие; средняя кнопка — цель.")));
            } catch (const std::exception& error) {
                wardogs::log_error("settings.quick_workflow_save_failed error=" + std::string(error.what()));
                set_status(wardogs::i18n::text(QStringLiteral("Быстрая игра доступна в этом сеансе, но настройки не сохранены: ")) + error_text(error), true);
            }
        }
        if (!startup_error_.isEmpty()) set_status(wardogs::i18n::text(QStringLiteral("Настройки не загружены: ")) + startup_error_, true);
        if (!diagnostic_) {
            show_event_ = CreateEventW(nullptr, FALSE, FALSE, L"Local\\SoNiX.WardogsFireControl.Show");
            if (show_event_) {
                show_notifier_ = new QWinEventNotifier(show_event_, this);
                connect(show_notifier_, &QWinEventNotifier::activated, this, [this](HANDLE) { exit_game_mode(); });
            }
        }
        wardogs::log_info("window.ready");
        wardogs::i18n::watch(this);
        if (!diagnostic_ && settings_.check_updates_on_start) {
            QTimer::singleShot(0, this, [this] {
                if (!closing_ && settings_.check_updates_on_start && updates_) updates_->check(false);
            });
        }
    }

    ~MainWindow() override {
        closing_ = true;
        unregister_hotkeys();
        mouse_listener_.stop();
        if (worker_.joinable()) { worker_.request_stop(); worker_.join(); }
        delete unlock_notifier_;
        delete show_notifier_;
        if (unlock_event_) CloseHandle(unlock_event_);
        if (show_event_) CloseHandle(show_event_);
    }

    bool export_snapshot(const QString& mode, const QString& path) {
        if (mode == QStringLiteral("selection")) {
            const QFileInfo file(path);
            if (!QDir().mkpath(file.absolutePath()) || !selector_.begin([](auto, auto) {})) return false;
            QApplication::processEvents();
            const bool rendered = selector_.export_preview(file.absoluteFilePath());
            selector_.cancel();
            QSaveFile receipt(file.absoluteFilePath() + QStringLiteral(".json"));
            const auto bytes = QJsonDocument(QJsonObject{
                {QStringLiteral("language"), settings_.language == wardogs::UiLanguage::english ? QStringLiteral("en") : QStringLiteral("ru")},
                {QStringLiteral("mode"), mode}, {QStringLiteral("rendering"), QStringLiteral("Shared Win32 paint routine at actual window DPI; no desktop pixels")},
                {QStringLiteral("widgets"), QJsonArray{QJsonObject{
                    {QStringLiteral("name"), QStringLiteral("nativeSelectionInstructions")},
                    {QStringLiteral("text"), wardogs::i18n::text(QStringLiteral("Выделите текст координат X / Y\nEsc или правая кнопка — отмена"))}}}}}).toJson();
            const bool written = receipt.open(QIODevice::WriteOnly) && receipt.write(bytes) == bytes.size() && receipt.commit();
            return rendered && written;
        }
        if (mode != QStringLiteral("ui") && mode != QStringLiteral("settings") && mode != QStringLiteral("recognition") &&
            mode != QStringLiteral("recognition-bottom") && mode != QStringLiteral("tutorial-bottom") &&
            mode != QStringLiteral("tutorial") && mode != QStringLiteral("notice")) {
            terrain_selector_->setCurrentIndex(terrain_selector_->findData(static_cast<int>(wardogs::GameMap::training)));
            base_input_->setText(QStringLiteral("80 80"));
            manual_base();
            target_input_->setText(QStringLiteral("84 83"));
            manual_target();
        }
        std::unique_ptr<QTemporaryDir> planning_snapshot_data;
        std::unique_ptr<QDialog> dialog;
        QWidget* view = this;
        if (mode == QStringLiteral("settings") || mode.startsWith(QStringLiteral("recognition"))) {
            dialog = std::make_unique<SettingsDialog>(settings_);
            if (mode.startsWith(QStringLiteral("recognition")))
                if (auto* tabs = dialog->findChild<QTabWidget*>(QStringLiteral("settingsTabs"))) tabs->setCurrentIndex(1);
        }
        else if (mode == QStringLiteral("tutorial") || mode == QStringLiteral("tutorial-bottom")) dialog.reset(make_help_dialog(false));
        else if (mode == QStringLiteral("notice")) dialog.reset(make_help_dialog(true));
        else if (mode.startsWith(QStringLiteral("planning"))) {
            planning_snapshot_data = std::make_unique<QTemporaryDir>();
            if (!planning_snapshot_data->isValid()) return false;
            if (!vehicle_mode_) toggle_mode();
            target_input_->setText(QStringLiteral("100 80"));
            manual_target();
            dialog = std::make_unique<PlanningDialog>([this] { return planning_context(); },
                [this](auto kind, auto point, auto map, auto weapon) {
                    apply_planning_point(kind, point, map, weapon);
                }, this, std::filesystem::path(planning_snapshot_data->path().toStdWString()));
            if (auto* tabs = dialog->findChild<QTabWidget*>(QStringLiteral("planningTabs"))) {
                if (mode == QStringLiteral("planning-positions")) tabs->setCurrentIndex(1);
                else if (mode == QStringLiteral("planning-times")) tabs->setCurrentIndex(2);
                else if (mode == QStringLiteral("planning-profiles")) tabs->setCurrentIndex(3);
            }
        }
        else if (mode == QStringLiteral("folder")) {
            dialog = make_terrain_folder_dialog();
        } else if (mode == QStringLiteral("error")) {
            QString problem;
            try { (void)wardogs::parse_manual_coordinate(L"invalid"); }
            catch (const std::exception& error) { problem = error_text(error); }
            dialog = std::make_unique<QMessageBox>(QMessageBox::Warning,
                wardogs::i18n::text(QStringLiteral("Диагностика")), problem, QMessageBox::Ok, this);
        }
        else if (mode == QStringLiteral("pinned") || mode == QStringLiteral("pinned-menu")) {
            enter_pinned_mode(); view = pinned_window_.get();
            if (mode == QStringLiteral("pinned-menu")) {
                QContextMenuEvent event(QContextMenuEvent::Mouse, QPoint(20, 20), pinned_window_->mapToGlobal(QPoint(20, 20)));
                QApplication::sendEvent(pinned_window_.get(), &event);
                if (auto* menu = pinned_window_->findChild<QWidget*>(QStringLiteral("pinnedContextMenu"))) view = menu;
            }
        }
        else if (mode == QStringLiteral("reticle")) {
            ghost_window_->begin_adjustment();
            view = ghost_window_.get();
        }
        else if (mode == QStringLiteral("vehicle") || mode == QStringLiteral("vehicle-pinned") ||
                 mode == QStringLiteral("calibration")) {
            terrain_selector_->setCurrentIndex(terrain_selector_->findData(static_cast<int>(wardogs::GameMap::training)));
            if (!vehicle_mode_) toggle_mode();
            target_input_->setText(QStringLiteral("92 90"));
            manual_target();
            toggle_ghost_arc();
            if (mode == QStringLiteral("vehicle-pinned")) {
                enter_pinned_mode();
                view = pinned_window_.get();
            } else if (mode == QStringLiteral("calibration")) {
                calibration_toggle_->setChecked(true);
                QApplication::processEvents();
                findChild<QScrollArea*>(QStringLiteral("mainContentScroll"))->ensureWidgetVisible(calibration_group_);
            }
        }
        else if (mode == QStringLiteral("fire-control")) {
            if (!vehicle_mode_) toggle_mode();
            accept_manual_target({80,102});
            if (effective_vehicle_arc() != wardogs::Arc::high) toggle_ghost_arc();
            OcrMessage context;
            capture_impact_context(context);
            if (!record_continuous_impact({80.2,101.7}, QStringLiteral("fixture"), *context.impact_firing)) return false;
        }
        else if (mode == QStringLiteral("review") || mode == QStringLiteral("review-bottom")) {
            OcrMessage message;
            message.action = OcrAction::target; message.success = true;
            message.input_epoch = input_epoch_; message.point = {84, 83};
            message.text = L"x81.25, y83.50\nx84.00, y83.00";
            message.assessment = wardogs::assess_ocr_result({message.text, 0.92F, 0.62F});
            finish_ocr(std::move(message));
        }
        else if (mode == QStringLiteral("compact")) resize(640, 500);
        else if (mode == QStringLiteral("manual") || mode == QStringLiteral("manual-bottom")) manual_toggle_->setChecked(true);
        if (dialog) { view = dialog.get(); view->show(); }
        view->show();
        QApplication::processEvents();
        // Content changes can post a second layout request to the outer footer.
        // Settle that request before rendering the diagnostic window.
        QApplication::processEvents();
        if (mode == QStringLiteral("recognition-bottom")) {
            if (auto* tabs = dialog->findChild<QTabWidget*>(QStringLiteral("settingsTabs")))
                if (auto* scroll = qobject_cast<QScrollArea*>(tabs->currentWidget()))
                    scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
        } else if (mode == QStringLiteral("recognition-hotkeys") || mode == QStringLiteral("recognition-reticle")) {
            if (auto* tabs = dialog->findChild<QTabWidget*>(QStringLiteral("settingsTabs")))
                if (auto* scroll = qobject_cast<QScrollArea*>(tabs->currentWidget())) {
                    auto* control = dialog->findChild<QWidget*>(mode == QStringLiteral("recognition-reticle")
                        ? QStringLiteral("ghostReticlePreset") : QStringLiteral("impactHotkey"));
                    if (control) scroll->ensureWidgetVisible(control, 0, 80);
                }
        } else if (mode == QStringLiteral("review-bottom")) {
            findChild<QScrollArea*>(QStringLiteral("mainContentScroll"))->ensureWidgetVisible(ocr_review_, 0, 10);
        } else if (mode == QStringLiteral("manual-bottom")) {
            findChild<QScrollArea*>(QStringLiteral("mainContentScroll"))->ensureWidgetVisible(ocr_text_, 0, 10);
        } else if (mode == QStringLiteral("tutorial-bottom")) {
            if (auto* browser = dialog->findChild<QTextBrowser*>(QStringLiteral("helpText")))
                browser->verticalScrollBar()->setValue(browser->verticalScrollBar()->maximum());
        } else if (mode == QStringLiteral("planning-times")) {
            // This page can scroll after its guidance text wraps. Capture the
            // actual measurement table, which is the surface this mode verifies;
            // the accepted coordinates above the tabs stay in view.
            auto* tabs = dialog->findChild<QTabWidget*>(QStringLiteral("planningTabs"));
            auto* scroll = tabs ? qobject_cast<QScrollArea*>(tabs->currentWidget()) : nullptr;
            auto* table = dialog->findChild<QWidget*>(QStringLiteral("flightMeasurements"));
            if (!scroll || !table) return false;
            scroll->ensureWidgetVisible(table, 0, 10);
        } else if (mode == QStringLiteral("fire-control")) {
            // Fix only the diagnostic canvas after startup layout settles.
            // Native maximum tracking size on a small desktop can constrain a
            // plain resize, hiding the ranging report below the scroll viewport.
            setFixedSize(1120,1040);
        }
        QApplication::processEvents();
        if (mode == QStringLiteral("fire-control")) QApplication::processEvents();
        const QFileInfo file(path);
        if (!QDir().mkpath(file.absolutePath())) return false;
        QJsonArray widgets;
        auto objects = view->findChildren<QWidget*>();
        objects.prepend(view);
        for (auto* widget : objects) {
            QJsonObject item{{QStringLiteral("type"), QString::fromLatin1(widget->metaObject()->className())},
                {QStringLiteral("name"), widget->objectName()}, {QStringLiteral("visible"), widget->isVisible()},
                {QStringLiteral("width"), widget->width()}, {QStringLiteral("height"), widget->height()}};
            QRect snapshot_rect(view->mapFromGlobal(widget->mapToGlobal(QPoint{})), widget->size());
            if (widget->isVisible()) {
                for (auto* ancestor = widget->parentWidget(); ancestor && ancestor != view;
                     ancestor = ancestor->parentWidget()) {
                    if (ancestor->isWindow()) break;
                    snapshot_rect = snapshot_rect.intersected(
                        QRect(view->mapFromGlobal(ancestor->mapToGlobal(QPoint{})), ancestor->size()));
                }
                snapshot_rect = snapshot_rect.intersected(view->rect());
            } else {
                snapshot_rect = {};
            }
            item.insert(QStringLiteral("snapshot_rect"), QJsonObject{
                {QStringLiteral("x"), snapshot_rect.x()}, {QStringLiteral("y"), snapshot_rect.y()},
                {QStringLiteral("width"), snapshot_rect.width()}, {QStringLiteral("height"), snapshot_rect.height()}});
            for (const auto* property : {"text", "title", "windowTitle", "toolTip", "accessibleName", "placeholderText"}) {
                const auto value = widget->property(property).toString();
                if (!value.isEmpty()) item.insert(QString::fromLatin1(property), value);
            }
            if (auto* label = qobject_cast<QLabel*>(widget)) {
                item.insert(QStringLiteral("wordWrap"), label->wordWrap());
                item.insert(QStringLiteral("text_width"), QFontMetrics(label->font()).horizontalAdvance(label->text()));
                item.insert(QStringLiteral("content_width"), label->contentsRect().width());
                item.insert(QStringLiteral("required_height"), label->heightForWidth(label->width()));
            }
            if (auto* browser = qobject_cast<QTextBrowser*>(widget))
                item.insert(QStringLiteral("content"), browser->toPlainText());
            if (auto* combo = qobject_cast<QComboBox*>(widget)) {
                QJsonArray values;
                for (int index = 0; index < combo->count(); ++index) values.append(combo->itemText(index));
                item.insert(QStringLiteral("items"), values);
            }
            if (auto* tabs = qobject_cast<QTabWidget*>(widget)) {
                QJsonArray values;
                for (int index = 0; index < tabs->count(); ++index) values.append(tabs->tabText(index));
                item.insert(QStringLiteral("tabs"), values);
            }
            widgets.append(item);
        }
        QSaveFile receipt(file.absoluteFilePath() + QStringLiteral(".json"));
        const auto bytes = QJsonDocument(QJsonObject{
            {QStringLiteral("language"), settings_.language == wardogs::UiLanguage::english ? QStringLiteral("en") : QStringLiteral("ru")},
            {QStringLiteral("mode"), mode}, {QStringLiteral("snapshot_dpr"), view->devicePixelRatioF()},
            {QStringLiteral("snapshot_width"), view->width()}, {QStringLiteral("snapshot_height"), view->height()},
            {QStringLiteral("widgets"), widgets}}).toJson();
        const bool written = receipt.open(QIODevice::WriteOnly) && receipt.write(bytes) == bytes.size() && receipt.commit();
        return written && view->grab().save(file.absoluteFilePath(), "PNG");
    }

    QJsonObject run_self_test(const QString& image_path, bool test_capture_window = false,
                             DWORD capture_fixture_pid = 0) {
        auto previous_clipboard = std::make_unique<QMimeData>();
        if (const auto* mime = QApplication::clipboard()->mimeData())
            for (const auto& format : mime->formats()) previous_clipboard->setData(format, mime->data(format));
        QJsonArray checks;
        bool passed = true;
        auto check = [&](const char* name, bool success) {
            passed = passed && success;
            QJsonObject item{{QStringLiteral("name"), QString::fromUtf8(name)}, {QStringLiteral("passed"), success}};
            if (!success) {
                item.insert(QStringLiteral("status"), status_->text());
                item.insert(QStringLiteral("ocr"), ocr_text_->text());
                if (pending_ocr_) item.insert(QStringLiteral("review_reason"), ocr_review_reason_->text());
                if (std::string_view{name}.starts_with("clipboard_")) {
                    const auto clipboard_text = QApplication::clipboard()->text();
                    item.insert(QStringLiteral("clipboard_characters"), clipboard_text.size());
                    item.insert(QStringLiteral("clipboard_has_weapon"), clipboard_text.contains(QStringLiteral("SPH-2")));
                    item.insert(QStringLiteral("clipboard_has_mil"), clipboard_text.contains(QStringLiteral("MIL")));
                    item.insert(QStringLiteral("clipboard_owned"), QApplication::clipboard()->ownsClipboard());
                    item.insert(QStringLiteral("clipboard_unicode_format"), static_cast<bool>(IsClipboardFormatAvailable(CF_UNICODETEXT)));
                    item.insert(QStringLiteral("clipboard_sequence_now"), static_cast<qint64>(GetClipboardSequenceNumber()));
                    item.insert(QStringLiteral("clipboard_copy"), clipboard_copy_diagnostics_);
                }
            }
            checks.append(item);
        };
        auto click = [&](const char* name) {
            auto* button = findChild<QPushButton*>(QString::fromUtf8(name));
            if (button) button->click();
            QApplication::processEvents();
        };
        check("first_launch_has_no_fake_base", !base_set_ && !target_ && distance_->text() == QStringLiteral("—"));
        check("diagnostic_previews_do_not_construct_network_updates", !updates_ &&
              !findChild<QPushButton*>(QStringLiteral("checkUpdatesButton"))->isEnabled());
        check("game_map_must_be_explicitly_confirmed_for_new_session", !map_confirmed_ &&
              selected_game_map() == wardogs::GameMap::unselected && confirm_map_->isVisible());
        show_result({84, 83});
        enter_game_mode();
        check("unselected_map_blocks_guidance_copy_and_game_entry", !map_confirmed_ &&
              !low_result_ && !high_result_ && !mortar_mil_result_ && !copy_button_->isEnabled() && !game_mode_);
        terrain_selector_->setCurrentIndex(terrain_selector_->findData(static_cast<int>(wardogs::GameMap::training)));
        check("explicit_training_map_is_ready_and_reports_absent_heights", map_confirmed_ &&
              current_game_map_ == wardogs::GameMap::training && !terrain_ &&
              terrain_summary_->text().contains(wardogs::i18n::text(QStringLiteral("Рельеф не учтён"))));
        check("default_profile_is_ready_for_quick_game", settings_.game_integration_enabled &&
              settings_.middle_mouse_enabled && settings_.automatic_chat_region &&
              settings_.base_hotkey == L"Alt+X" && settings_.exit_game_mode_hotkey == L"Alt+C" &&
              !hotkey_listener_.active() && !mouse_listener_.active());
        map_confirmed_ = false;
        update_readiness();
        check("game_button_explains_explicit_confirmation", game_button_->text() ==
              wardogs::i18n::text(QStringLiteral("Подтвердить карту и в игру")));
        enter_game_mode();
        check("automatic_game_entry_cannot_confirm_a_saved_map", !map_confirmed_ && !game_mode_);
        click("gameButton");
        check("explicit_confirm_and_game_click_confirms_selected_map_and_hides_main",
              map_confirmed_ && game_mode_ && pinned_mode_ && !isVisible() && pinned_window_->isVisible());
        exit_game_mode();
        check("return_restores_main_and_simple_game_label", isVisible() && !game_mode_ &&
              game_button_->text() == wardogs::i18n::text(QStringLiteral("В игру")));
        check("manual_controls_are_optional", manual_toggle_ && !manual_toggle_->isChecked() &&
              !findChild<QGroupBox*>(QStringLiteral("coordinatesGroup"))->isVisible());
        const auto requested_settings = settings_;
        std::array effective_keys{
            wardogs::parse_hotkey(L"Ctrl+Alt+R"),
            wardogs::parse_hotkey(settings_.base_hotkey),
            wardogs::parse_hotkey(settings_.target_hotkey),
            wardogs::parse_hotkey(settings_.quick_target_hotkey),
            wardogs::parse_hotkey(settings_.impact_hotkey),
            wardogs::parse_hotkey(settings_.ghost_arc_hotkey),
            wardogs::parse_hotkey(L"Ctrl+Alt+F10"),
            wardogs::parse_hotkey(L"Ctrl+Alt+F11")};
        const auto replacement_notice = apply_registered_hotkeys(settings_, effective_keys);
        update_action_labels();
        check("effective_hotkeys_update_actions_and_correct_settings_fields",
              settings_.region_hotkey == L"Ctrl+Alt+R" &&
              settings_.base_hotkey == requested_settings.base_hotkey &&
              settings_.pinned_card.unlock_hotkey == L"Ctrl+Alt+F10" &&
              settings_.exit_game_mode_hotkey == L"Ctrl+Alt+F11" &&
              region_button_->text().contains(QStringLiteral("Ctrl+Alt+R")) &&
              replacement_notice.contains(QStringLiteral("Alt+R → Ctrl+Alt+R")) &&
              settings_.game_integration_enabled == requested_settings.game_integration_enabled &&
              settings_.middle_mouse_enabled == requested_settings.middle_mouse_enabled);
        settings_ = requested_settings;
        bool partial_hotkeys_rejected = false;
        try { (void)apply_registered_hotkeys(settings_, std::span(effective_keys).first(7)); }
        catch (const std::invalid_argument&) { partial_hotkeys_rejected = true; }
        check("partial_effective_hotkeys_do_not_change_settings",
              partial_hotkeys_rejected && settings_.region_hotkey == requested_settings.region_hotkey &&
              settings_.pinned_card.unlock_hotkey == requested_settings.pinned_card.unlock_hotkey);
        update_action_labels();
        settings_.game_integration_enabled = false;
        settings_.middle_mouse_enabled = false;
        diagnostic_ = false;
        begin_region_setup(); begin_quick_target(); start_ocr(OcrAction::base);
        enter_game_mode(); enter_pinned_mode(); set_ghost_enabled(true);
        check("standalone_mode_blocks_capture_game_and_overlays", !selecting_ && !busy_ &&
              !game_mode_ && !pinned_mode_ && !ghost_enabled_ && !hotkey_listener_.active() && !mouse_listener_.active());
        diagnostic_ = true;
        settings_ = requested_settings;
        update_action_labels();
        manual_toggle_->setChecked(true);
        check("optional_impact_before_base_rejected",
              !record_continuous_impact({12, 10}, QStringLiteral("test"),
                  {{12, 10}, wardogs::Arc::low, 30, 20, 0}) && !continuous_calibration_);
        target_input_->setText(QStringLiteral("3 4"));
        click("manualTargetButton");
        check("target_before_base_rejected", !target_ && !copy_button_->isEnabled());
        base_input_->setText(QStringLiteral("0 0"));
        click("manualBaseButton");
        check("real_zero_base_is_allowed", base_set_ && base_ == wardogs::Point{0, 0});
        target_input_->setText(QStringLiteral("3 4"));
        click("manualTargetButton");
        const auto shot = wardogs::calculate_shot(base_, *target_);
        check("manual_ui_distance_bearing_mil", std::abs(shot.distance - 5) < 1e-9 &&
              std::abs(shot.angle - 36.86989764584402) < 1e-9 && mortar_mil_result_.has_value() &&
              distance_->text() == wardogs::i18n::text(qtext(wardogs::format_distance_meters(5))) && copy_button_->isEnabled());
        QApplication::clipboard()->setText(QStringLiteral("x4.00, y3.00"));
        click("pasteTargetButton");
        check("explicit_clipboard_action_calculates_target", target_ == wardogs::Point{4, 3} && mortar_mil_result_);
        QApplication::clipboard()->setText(QStringLiteral("not a coordinate"));
        click("pasteTargetButton");
        check("invalid_clipboard_preserves_target", target_ == wardogs::Point{4, 3});
        target_input_->setText(QStringLiteral("3.123456789 4.234567891")); manual_target();
        const auto precise_target = *target_;
        const auto precise_mil = mortar_mil_result_;
        target_input_->setText(QStringLiteral("4 3")); manual_target();
        recall_target(1);
        check("history_restores_original_coordinate_precision_and_guidance",
              target_ == precise_target && history_.front() == precise_target && mortar_mil_result_ == precise_mil);
        base_capture_pending_ = true;
        recall_target(1);
        check("history_cannot_bypass_an_unconfirmed_new_gun", target_ == precise_target);
        base_capture_pending_ = false;
        target_input_->setText(QStringLiteral("3 4")); manual_target();
        const auto valid_target = target_;
        target_input_->setText(QStringLiteral("not coordinates"));
        click("manualTargetButton");
        check("invalid_manual_input_preserves_last_target", target_ == valid_target);
        const QPoint status_origin = status_->mapTo(this, QPoint{});
        check("feedback_is_outside_scroll_and_visible", status_->text().contains(wardogs::i18n::text(QStringLiteral("Введите"))) &&
              status_origin.y() >= 0 && status_origin.y() + status_->height() <= height());
        OcrMessage stale;
        stale.success = true; stale.point = {100, 100}; stale.input_epoch = input_epoch_;
        target_input_->setText(QStringLiteral("4 3"));
        click("manualTargetButton");
        finish_ocr(stale);
        check("late_ocr_cannot_overwrite_manual_target", target_ == wardogs::Point{4, 3});
        stale.action = OcrAction::base; stale.input_epoch = input_epoch_;
        base_input_->setText(QStringLiteral("1 1"));
        click("manualBaseButton");
        finish_ocr(stale);
        check("late_ocr_cannot_overwrite_manual_base", base_ == wardogs::Point{1, 1} && !target_);
        target_input_->setText(QStringLiteral("4 3")); manual_target();
        const auto before_review = target_;
        const auto before_review_history = history_.size();
        OcrMessage uncertain;
        uncertain.success = true; uncertain.point = {6, 5}; uncertain.text = L"x4.00 y3.00 x6.00 y5.00";
        uncertain.action = OcrAction::target; uncertain.input_epoch = input_epoch_;
        uncertain.assessment = wardogs::assess_ocr_result({uncertain.text, 0.99F, 0.98F});
        finish_ocr(uncertain);
        check("ambiguous_ocr_preserves_target_and_history", target_ == before_review && history_.size() == before_review_history && pending_ocr_.has_value());
        check("ambiguous_ocr_hides_previous_solution", ocr_hold_ && !mortar_mil_result_ && !copy_button_->isEnabled() && ocr_review_->isVisible());
        ocr_candidates_->setCurrentText(QStringLiteral("6 5"));
        click("confirmOcrButton");
        check("confirmed_ocr_applies_selected_coordinate", target_ == wardogs::Point{6, 5} && !pending_ocr_ && !ocr_hold_ && mortar_mil_result_);
        uncertain.input_epoch = input_epoch_;
        finish_ocr(uncertain);
        click("cancelOcrButton");
        check("cancel_ocr_review_restores_previous_solution", target_ == wardogs::Point{6, 5} && !pending_ocr_ && !ocr_hold_ && mortar_mil_result_);
        uncertain.input_epoch = input_epoch_;
        finish_ocr(uncertain);
        target_input_->setText(QStringLiteral("5 4")); manual_target();
        click("confirmOcrButton");
        check("manual_change_invalidates_pending_ocr", target_ == wardogs::Point{5, 4} && !pending_ocr_ && !ocr_hold_);
        OcrMessage failed;
        failed.input_epoch = input_epoch_; failed.error = QStringLiteral("test capture has no coordinate");
        finish_ocr(failed);
        check("failed_ocr_hides_old_mil_without_changing_target", target_ == wardogs::Point{5, 4} && ocr_hold_ && !mortar_mil_result_ && !copy_button_->isEnabled());
        target_input_->setText(QStringLiteral("5 4"));
        manual_target();
        check("manual_input_recovers_after_ocr_failure", !ocr_hold_ && mortar_mil_result_);
        const auto old_base = base_;
        const auto old_target = target_;
        auto uncertain_base = uncertain;
        uncertain_base.action = OcrAction::base;
        uncertain_base.input_epoch = input_epoch_;
        base_capture_pending_ = true;
        finish_ocr(uncertain_base);
        const auto held_base_epoch = input_epoch_;
        start_map_ocr({});
        check("new_uncertain_gun_blocks_map_without_discarding_review", pending_ocr_ &&
              pending_ocr_->action == OcrAction::base && base_capture_pending_ &&
              input_epoch_ == held_base_epoch && base_ == old_base && target_ == old_target);
        cancel_ocr_review();
        check("rejecting_new_gun_restores_previous_gun_and_solution", !base_capture_pending_ &&
              !ocr_hold_ && base_ == old_base && target_ == old_target && mortar_mil_result_);
        base_capture_pending_ = true;
        failed.action = OcrAction::base; failed.input_epoch = input_epoch_;
        finish_ocr(failed);
        target_input_->setText(QStringLiteral("6 5")); manual_target();
        start_map_ocr({});
        check("failed_new_gun_blocks_auto_and_manual_targets", base_capture_pending_ &&
              base_ == old_base && target_ == old_target && ocr_hold_ && !mortar_mil_result_);
        base_input_->setText(qtext(wardogs::format_point(old_base))); manual_base();
        target_input_->setText(QStringLiteral("5 4")); manual_target();
        check("explicit_manual_gun_recovers_after_failed_capture", !base_capture_pending_ &&
              !ocr_hold_ && base_ == old_base && target_ == old_target && mortar_mil_result_);
        uncertain.input_epoch = input_epoch_;
        finish_ocr(uncertain);
        toggle_mode();
        check("mode_change_explains_cancelled_review", !pending_ocr_ && ocr_hold_ &&
              status_->text().contains(wardogs::i18n::text(QStringLiteral("заново"))) && !copy_button_->isEnabled());
        toggle_mode();
        target_input_->setText(QStringLiteral("5 4")); manual_target();
        settings_.automatic_chat_region = false;
        click("gameButton");
        check("quick_game_mode_does_not_require_saved_region", game_mode_ && pinned_mode_);
        exit_game_mode();
        settings_.automatic_chat_region = true;
        uncertain_base.input_epoch = input_epoch_;
        base_capture_pending_ = true;
        finish_ocr(uncertain_base);
        confirm_ocr();
        check("confirmed_custom_capture_gun_enters_game_workflow", !pending_ocr_ &&
              !base_capture_pending_ && base_ == uncertain_base.point && game_mode_ && pinned_mode_);
        exit_game_mode();
        base_input_->setText(qtext(wardogs::format_point(old_base))); manual_base();
        target_input_->setText(QStringLiteral("5 4")); manual_target();
        uncertain.text = L"x6.00 y5.00";
        uncertain.point = {6, 5}; uncertain.force_review = true;
        uncertain.assessment = wardogs::assess_ocr_result({uncertain.text, 0.99F, 0.98F});
        uncertain.input_epoch = input_epoch_;
        const auto before_automatic = target_;
        finish_ocr(uncertain);
        check("unproven_source_requires_review_even_for_one_clear_pair",
              pending_ocr_.has_value() && target_ == before_automatic && ocr_hold_);
        cancel_ocr_review();
        uncertain.force_review = true;
        uncertain.input_epoch = input_epoch_;
        finish_ocr(uncertain);
        game_mode_ = true;
        const auto before_review_return = target_;
        exit_game_mode();
        check("return_to_main_preserves_optional_ocr_review", !game_mode_ && pending_ocr_ &&
              pending_ocr_->input_epoch == input_epoch_ && ocr_review_->isVisible());
        check("return_to_review_preserves_safe_hold_and_target", ocr_hold_ &&
              target_ == before_review_return && !copy_button_->isEnabled());
        confirm_ocr();
        check("optional_review_applies_after_return", !pending_ocr_ && !ocr_hold_ &&
              target_ == wardogs::Point{6, 5} && mortar_mil_result_);
        uncertain.force_review = false;
        target_input_->setText(QStringLiteral("100 100"));
        click("manualTargetButton");
        check("out_of_range_has_no_fictional_mil", !mortar_mil_result_ && target_.has_value());
        click("weaponButton");
        check("SPH2_mode_changes_ui", vehicle_mode_ && !mortar_result_group_->isVisible() && vehicle_result_group_->isVisible());
        base_input_->setText(QStringLiteral("80 80")); click("manualBaseButton");
        target_input_->setText(QStringLiteral("92 90")); click("manualTargetButton");
        check("SPH2_has_both_trajectories", low_result_.has_value() && high_result_.has_value());
        target_input_->setText(QStringLiteral("79.999998 92")); manual_target();
        copy_solution();
        check("copied_SPH2_north_bearing_matches_card_wrap",
              low_result_ && high_result_ && QApplication::clipboard()->text().contains(wardogs::i18n::text(QStringLiteral("Настильная: 0.0° N"))) &&
              QApplication::clipboard()->text().contains(wardogs::i18n::text(QStringLiteral("Навесная: 0.0° N"))) &&
              !QApplication::clipboard()->text().contains(QStringLiteral("360.0°")));
        target_input_->setText(QStringLiteral("92 90")); manual_target();
        check("SPH2_target_distance_uses_meters_on_both_trajectory_cards",
              low_solution_->distance_text() == wardogs::i18n::text(QStringLiteral("1562 м")) &&
              high_solution_->distance_text() == wardogs::i18n::text(QStringLiteral("1562 м")));
        check("SPH2_quick_state_matches_available_guidance", !quick_state_->text().contains(wardogs::i18n::text(QStringLiteral("Наводка недоступна"))) &&
              quick_state_->text().contains(wardogs::i18n::text(QStringLiteral("необязательная поправка"))));
        const auto default_base_hotkey = settings_.base_hotkey;
        settings_.base_hotkey = L"Ctrl+Alt+X";
        base_capture_pending_ = true;
        update_action_labels();
        update_readiness();
        check("custom_base_hotkey_is_used_in_readiness_and_pending_capture_hints",
              quick_state_->text().contains(wardogs::i18n::text(QStringLiteral("повторите Ctrl+Alt+X"))) &&
              readiness_->text().contains(wardogs::i18n::text(QStringLiteral("Ctrl+Alt+X: чат"))) &&
              base_button_->text().contains(QStringLiteral("Ctrl+Alt+X")));
        base_capture_pending_ = false;
        settings_.base_hotkey = default_base_hotkey;
        update_action_labels();
        update_readiness();
        const auto observation_count = [this] {
            return continuous_calibration_ ? continuous_calibration_->sample_count() : std::size_t{};
        };
        const auto impact_for = [this](wardogs::FiringSnapshot firing, double bearing_offset, double mil_offset) {
            const double range = wardogs::sph2_distance_for_mil(firing.mil - mil_offset, firing.arc);
            const double radians = (firing.bearing_deg - bearing_offset) * std::numbers::pi / 180.0;
            return wardogs::Point{base_.x + std::sin(radians) * range / 100.0,
                                  base_.y + std::cos(radians) * range / 100.0};
        };
        check("SPH2_flat_solution_is_ready_without_calibration_or_impact_history",
              !continuous_calibration_ && low_result_ && high_result_ && copy_button_->isEnabled() &&
              vehicle_note_->text().contains(wardogs::i18n::text(QStringLiteral("Табличный расчёт"))) &&
              vehicle_note_->text().contains(wardogs::i18n::text(QStringLiteral("рельеф не учтён"))));
        check("SPH2_optional_editor_has_no_first_second_shot_sequence",
              calibration_toggle_->text() == wardogs::i18n::text(QStringLiteral("Поправки по попаданию")) &&
              continuous_aim_->isReadOnly() && continuous_impact_->isEnabled() &&
              !calibration_summary_->text().contains(QStringLiteral("30°")) &&
              !quick_state_->text().contains(QStringLiteral("1/2")));
        check("unified_ranging_is_visible_without_opening_advanced_editor",
              findChild<QGroupBox*>(QStringLiteral("fireControlGroup"))->isVisible() &&
              !calibration_group_->isVisible() && !fire_control_reset_->isEnabled());
        check("ranging_hint_preserves_angular_and_arc_direction_units", fire_control_summary_->text().contains(
              wardogs::i18n::text(QStringLiteral("\nПравее / левее — градусы азимута. Дальше / ближе — MIL: настильная + / −, навесная − / +."))));
        settings_.ghost_reticle.preferred_arc = wardogs::Arc::low;
        toggle_ghost_arc();
        OcrMessage initial_high_context;
        capture_impact_context(initial_high_context);
        check("SPH2_F4_and_first_optional_impact_use_displayed_high_guidance",
              effective_vehicle_arc() == wardogs::Arc::high && initial_high_context.impact_firing &&
              initial_high_context.impact_firing->arc == wardogs::Arc::high &&
              initial_high_context.impact_firing->bearing_deg == high_result_->bearing_deg &&
              initial_high_context.impact_firing->mil == high_result_->mil &&
              high_solution_->selected() && !low_solution_->selected() && !continuous_calibration_);
        const auto first_raw_high = *high_result_;
        const auto first_raw_low = *low_result_;
        const auto first_history = history_;
        initial_high_context.success = true;
        initial_high_context.action = OcrAction::calibration_impact;
        initial_high_context.input_epoch = input_epoch_;
        initial_high_context.calibration_epoch = calibration_epoch_;
        initial_high_context.point = impact_for(*initial_high_context.impact_firing, 1.0, -10.0);
        initial_high_context.text = wardogs::format_point(initial_high_context.point);
        initial_high_context.assessment = wardogs::assess_ocr_result({initial_high_context.text, 0.99F, 0.99F});
        finish_ocr(initial_high_context);
        check("SPH2_first_optional_impact_creates_local_correction_immediately",
              observation_count() == 1 && high_result_ &&
              std::abs(std::remainder(high_result_->bearing_deg - first_raw_high.bearing_deg, 360.0) - 1.0) < 1e-6 &&
              std::abs(high_result_->mil - first_raw_high.mil + 10.0) < 1e-6);
        check("SPH2_first_optional_impact_preserves_target_history_and_other_arc",
              target_ == initial_high_context.impact_firing->target && history_ == first_history &&
              low_result_ && low_result_->bearing_deg == first_raw_low.bearing_deg &&
              low_result_->mil == first_raw_low.mil && !ocr_hold_);
        const auto unified_context = planning_context();
        check("planning_and_ranging_share_the_exact_corrected_active_command",
              unified_context.active_arc == wardogs::Arc::high && unified_context.active_solution &&
              unified_context.active_solution->bearing_deg == high_result_->bearing_deg &&
              unified_context.active_solution->mil == high_result_->mil && last_impact_feedback_ &&
              last_impact_feedback_->target == *target_ && fire_control_reset_->isEnabled());
        check("automatic_analysis_keeps_altI_out_of_flight_timing_and_does_not_switch_arc",
              automatic_analysis_ && automatic_analysis_->target == *target_ &&
              automatic_analysis_->arcs[1] && !automatic_analysis_->arcs[1]->flight_time &&
              effective_vehicle_arc() == wardogs::Arc::high);
        check("SPH2_optional_impact_never_claims_a_global_platform_calibration",
              continuous_calibration_->global_calibration().rotation == wardogs::identity_rotation() &&
              continuous_calibration_->global_rotation_adjustment_deg() == 0.0 &&
              status_->text().contains(wardogs::i18n::text(QStringLiteral("Поправка учтена"))) &&
              !status_->text().contains(wardogs::i18n::text(QStringLiteral("Пристрелка готова"))));
        OcrMessage repeated_same_target;
        capture_impact_context(repeated_same_target);
        const bool same_target_recorded = record_continuous_impact(
            impact_for(*repeated_same_target.impact_firing, 1.0, -10.0), QStringLiteral("test"),
            *repeated_same_target.impact_firing);
        check("SPH2_second_impact_on_the_same_target_needs_no_orthogonal_shot",
              same_target_recorded && observation_count() == 2 && target_ == repeated_same_target.impact_firing->target &&
              std::abs(std::remainder(high_result_->bearing_deg - first_raw_high.bearing_deg, 360.0) - 1.0) < 1e-6 &&
              std::abs(high_result_->mil - first_raw_high.mil + 10.0) < 1e-6);
        bool every_direction_capture_allowed = true;
        bool unrelated_targets_unchanged = true;
        for (const auto point : std::array{wardogs::Point{92, 90.6}, wardogs::Point{70, 92}, wardogs::Point{68, 70}}) {
            target_input_->setText(qtext(wardogs::format_point(point))); manual_target();
            OcrMessage any_direction;
            try { capture_impact_context(any_direction); }
            catch (const std::exception&) { every_direction_capture_allowed = false; }
            every_direction_capture_allowed = every_direction_capture_allowed && any_direction.impact_firing &&
                any_direction.impact_firing->target == point;
            const auto raw = wardogs::corrected_solution(base_, point, {wardogs::identity_rotation(), 0.0}, wardogs::Arc::high);
            unrelated_targets_unchanged = unrelated_targets_unchanged && high_result_ &&
                high_result_->mil == raw.mil && high_result_->bearing_deg == raw.bearing_deg;
        }
        check("SPH2_optional_capture_allows_near_direction_perpendicular_and_opposite_targets", every_direction_capture_allowed);
        check("SPH2_local_history_never_changes_distant_targets_or_global_rotation", unrelated_targets_unchanged &&
              observation_count() == 2 && continuous_calibration_->global_calibration().rotation == wardogs::identity_rotation());
        check("SPH2_unrelated_current_target_does_not_claim_an_active_local_correction",
              vehicle_note_->text().contains(wardogs::i18n::text(QStringLiteral("Табличный расчёт"))) &&
              !vehicle_note_->text().contains(wardogs::i18n::text(QStringLiteral("Локальная поправка"))));
        target_input_->setText(QStringLiteral("92 90")); manual_target();
        OcrMessage before_F4;
        capture_impact_context(before_F4);
        before_F4.success = true; before_F4.action = OcrAction::calibration_impact;
        before_F4.point = *target_; before_F4.text = wardogs::format_point(*target_);
        before_F4.assessment = wardogs::assess_ocr_result({before_F4.text, 0.99F, 0.99F});
        before_F4.input_epoch = input_epoch_; before_F4.calibration_epoch = calibration_epoch_;
        continuous_impact_->setText(QStringLiteral("92 90"));
        toggle_ghost_arc();
        finish_ocr(before_F4);
        check("SPH2_F4_discards_stale_optional_impact_without_changing_history",
              observation_count() == 2 && effective_vehicle_arc() == wardogs::Arc::low && !pending_ocr_ &&
              continuous_impact_->text().isEmpty());
        check("SPH2_changing_to_other_arc_keeps_its_raw_guidance",
              low_result_->mil == first_raw_low.mil && low_result_->bearing_deg == first_raw_low.bearing_deg);
        clear_continuous_calibration();
        check("SPH2_clearing_optional_corrections_restores_raw_guidance_without_new_setup",
              !continuous_calibration_ && target_ && base_set_ && low_result_ && high_result_ &&
              high_result_->mil == first_raw_high.mil && !ocr_hold_ && copy_button_->isEnabled());
        target_input_->setText(QStringLiteral("80 89")); manual_target();
        OcrMessage high_only_context;
        capture_impact_context(high_only_context);
        check("SPH2_high_only_fallback_matches_optional_impact", !low_result_ && high_result_ &&
              effective_vehicle_arc() == wardogs::Arc::high && high_only_context.impact_firing &&
              high_only_context.impact_firing->arc == wardogs::Arc::high && !continuous_calibration_);
        target_input_->setText(QStringLiteral("80 81")); manual_target();
        start_ocr(OcrAction::calibration_impact);
        check("SPH2_no_solution_rejects_optional_impact_without_capture", !busy_ && !selecting_ && !continuous_calibration_);
        target_input_->setText(QStringLiteral("92 90")); manual_target();
        ocr_hold_ = true;
        bool held_impact_rejected = false;
        try { OcrMessage held; capture_impact_context(held); }
        catch (const std::invalid_argument&) { held_impact_rejected = true; }
        check("SPH2_hidden_guidance_rejects_optional_impact", held_impact_rejected && !continuous_calibration_);
        ocr_hold_ = false; show_result(*target_);
        const auto target_before_failed_impact = target_;
        const auto history_before_failed_impact = history_;
        const auto calibration_before_failed_impact = calibration_epoch_;
        OcrMessage failed_impact;
        failed_impact.action = OcrAction::calibration_impact;
        capture_impact_context(failed_impact);
        failed_impact.input_epoch = input_epoch_;
        failed_impact.calibration_epoch = calibration_epoch_;
        failed_impact.error = QStringLiteral("test impact coordinate was not read");
        ocr_hold_ = true;
        clear_result(QStringLiteral("test impact capture pending"));
        finish_ocr(failed_impact);
        OcrMessage repeated_impact;
        bool failed_impact_can_repeat = true;
        try { capture_impact_context(repeated_impact); }
        catch (const std::exception&) { failed_impact_can_repeat = false; }
        check("SPH2_failed_impact_restores_guidance_and_allows_repeat_without_changing_state",
              failed_impact_can_repeat && !ocr_hold_ && low_result_ && high_result_ &&
              copy_button_->isEnabled() && target_ == target_before_failed_impact &&
              history_ == history_before_failed_impact && !continuous_calibration_ &&
              calibration_epoch_ == calibration_before_failed_impact);
        const wardogs::CaptureRegion missing_monitor{L"WARDOGS_TEST_MISSING_MONITOR", {0, 0, 100, 50}, {1000, 700}};
        start_ocr(missing_monitor, OcrAction::calibration_impact);
        check("SPH2_early_custom_impact_capture_failure_restores_existing_guidance",
              !busy_ && !pending_ocr_ && !ocr_hold_ && low_result_ && high_result_ &&
              target_ == target_before_failed_impact && history_ == history_before_failed_impact &&
              !continuous_calibration_ && calibration_epoch_ == calibration_before_failed_impact);
        auto failed_target = failed_impact;
        failed_target.action = OcrAction::target; failed_target.input_epoch = input_epoch_;
        finish_ocr(failed_target);
        check("SPH2_failed_target_keeps_guidance_hidden", ocr_hold_ && !low_result_ && !high_result_ &&
              !copy_button_->isEnabled() && target_ == target_before_failed_impact);
        auto stale_failed_impact = failed_impact;
        advance_input_epoch();
        check("SPH2_stale_failed_impact_cannot_restore_hidden_guidance",
              !restore_failed_impact_guidance(stale_failed_impact) && ocr_hold_ && !low_result_ && !high_result_);
        ocr_hold_ = false; show_result(*target_);
        ghost_window_->begin_adjustment(); ghost_window_->hide(); sync_ghost_solution();
        check("reticle_adjustment_restores_after_capture", ghost_window_->adjusting() && ghost_window_->isVisible());
        QKeyEvent cancel_adjustment(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(ghost_window_.get(), &cancel_adjustment);
        OcrMessage first_direct;
        capture_impact_context(first_direct);
        check("SPH2_first_impact_after_reset_always_has_frozen_firing_context", first_direct.impact_firing && !continuous_calibration_);
        const auto first_large_point = impact_for(*first_direct.impact_firing, 10.0, -100.0);
        const bool first_large_rejected = !record_continuous_impact(first_large_point, QStringLiteral("test"), *first_direct.impact_firing);
        check("SPH2_large_first_miss_does_not_publish_an_identity_or_partial_engine", first_large_rejected &&
              !continuous_calibration_ && low_result_ && high_result_ && !ocr_hold_ &&
              status_->text().contains(wardogs::i18n::text(QStringLiteral("Поправка не применена"))));
        continuous_impact_->clear(); record_manual_continuous_impact();
        check("SPH2_empty_manual_impact_does_not_create_correction_history", !continuous_calibration_ && low_result_ && high_result_);
        continuous_impact_->setText(qtext(wardogs::format_point(impact_for(*first_direct.impact_firing, 1.0, 10.0))));
        record_manual_continuous_impact();
        check("SPH2_first_manual_impact_is_optional_and_needs_no_prior_calibration", observation_count() == 1 && continuous_impact_->text().isEmpty());
        const bool conflicting_recorded = record_continuous_impact(
            impact_for(*first_direct.impact_firing, -2.9, -50.0), QStringLiteral("test"),
            *first_direct.impact_firing);
        check("SPH2_conflicting_optional_hits_explain_weakened_correction_without_extra_actions", conflicting_recorded &&
              observation_count() == 2 && low_result_ && high_result_ && !ocr_hold_ &&
              status_->text().contains(wardogs::i18n::text(QStringLiteral("Попадания расходятся; точность поправки ограничена"))));
        const auto prior_low = *low_result_; const auto prior_high = *high_result_;
        const auto prior_samples = observation_count(); const auto prior_history = history_;
        OcrMessage current_direct; capture_impact_context(current_direct);
        const bool large_rejected = !record_continuous_impact(impact_for(*current_direct.impact_firing, 10.0, -100.0),
                                                              QStringLiteral("test"), *current_direct.impact_firing);
        check("SPH2_rejected_large_miss_preserves_accepted_history_and_current_guidance", large_rejected &&
              observation_count() == prior_samples && history_ == prior_history && low_result_ && high_result_ &&
              low_result_->mil == prior_low.mil && high_result_->mil == prior_high.mil && !ocr_hold_);
        const auto epoch_before_busy_clear = calibration_epoch_;
        busy_ = true; ocr_hold_ = true; clear_result(QStringLiteral("test active coordinate read"));
        clear_continuous_calibration();
        check("SPH2_clear_during_active_read_preserves_history_and_does_not_restore_unaccepted_guidance",
              busy_ && ocr_hold_ && !low_result_ && !high_result_ && observation_count() == prior_samples &&
              calibration_epoch_ == epoch_before_busy_clear &&
              status_->text() == wardogs::i18n::text(QStringLiteral("Дождитесь окончания чтения координат перед сбросом поправок.")));
        busy_ = false; ocr_hold_ = false; show_result(*target_);
        const bool nonfinite_rejected = !record_continuous_impact({std::numeric_limits<double>::quiet_NaN(), 90},
                                                                 QStringLiteral("test"), *current_direct.impact_firing);
        check("SPH2_nonfinite_impact_preserves_existing_correction", nonfinite_rejected && observation_count() == prior_samples &&
              low_result_->mil == prior_low.mil && high_result_->mil == prior_high.mil);
        OcrMessage impact_review = current_direct;
        impact_review.success = true; impact_review.action = OcrAction::calibration_impact;
        impact_review.input_epoch = input_epoch_; impact_review.calibration_epoch = calibration_epoch_;
        impact_review.point = *target_; impact_review.text = L"x92.00 y90.00\nx70.00 y92.00";
        impact_review.assessment = wardogs::assess_ocr_result({impact_review.text, 0.99F, 0.98F});
        finish_ocr(impact_review);
        check("ambiguous_optional_impact_cannot_change_history", pending_ocr_ && observation_count() == prior_samples);
        auto pending_failure = impact_review; pending_failure.success = false;
        check("SPH2_impact_failure_recovery_does_not_bypass_pending_review",
              !restore_failed_impact_guidance(pending_failure) && pending_ocr_ && ocr_hold_ &&
              !low_result_ && !high_result_ && observation_count() == prior_samples);
        ocr_candidates_->setCurrentText(QStringLiteral("92 90")); confirm_ocr();
        check("confirmed_optional_impact_uses_the_original_firing_snapshot", !pending_ocr_ &&
              !ocr_hold_ && observation_count() == prior_samples + 1 && target_ == current_direct.impact_firing->target);
        impact_review.input_epoch = input_epoch_; impact_review.calibration_epoch = calibration_epoch_;
        finish_ocr(impact_review); clear_continuous_calibration(); confirm_ocr();
        check("clearing_optional_correction_discards_pending_impact_without_applying_it", !pending_ocr_ &&
              !continuous_calibration_ && !ocr_hold_ && low_result_ && high_result_ &&
              low_result_->mil == first_raw_low.mil);
        OcrMessage reset_stale; capture_impact_context(reset_stale);
        reset_stale.success = true; reset_stale.action = OcrAction::calibration_impact;
        reset_stale.point = *target_; reset_stale.text = wardogs::format_point(*target_);
        reset_stale.assessment = wardogs::assess_ocr_result({reset_stale.text, 0.99F, 0.99F});
        reset_stale.input_epoch = input_epoch_; reset_stale.calibration_epoch = calibration_epoch_;
        clear_continuous_calibration(); finish_ocr(reset_stale);
        check("SPH2_successful_impact_from_before_reset_cannot_recreate_correction", !continuous_calibration_ &&
              !pending_ocr_ && !ocr_hold_ && low_result_ && high_result_ && low_result_->mil == first_raw_low.mil);
        reset_stale.text = L"x92.00 y90.00\nx70.00 y92.00";
        reset_stale.assessment = wardogs::assess_ocr_result({reset_stale.text, 0.99F, 0.98F});
        finish_ocr(reset_stale);
        check("SPH2_ambiguous_impact_from_before_reset_cannot_open_a_new_review", !continuous_calibration_ &&
              !pending_ocr_ && !ocr_hold_ && low_result_ && high_result_ && low_result_->mil == first_raw_low.mil);
        reset_stale.success = false; reset_stale.error = QStringLiteral("test stale impact read failed");
        finish_ocr(reset_stale);
        check("SPH2_failed_impact_from_before_reset_cannot_hide_fresh_raw_guidance", !continuous_calibration_ &&
              !pending_ocr_ && !ocr_hold_ && low_result_ && high_result_ && low_result_->mil == first_raw_low.mil);
        OcrMessage terrain_stale; capture_impact_context(terrain_stale);
        terrain_stale.success = true; terrain_stale.action = OcrAction::calibration_impact;
        terrain_stale.point = *target_; terrain_stale.text = wardogs::format_point(*target_);
        terrain_stale.assessment = wardogs::assess_ocr_result({terrain_stale.text, 0.99F, 0.99F});
        terrain_stale.input_epoch = input_epoch_; terrain_stale.calibration_epoch = calibration_epoch_;
        const auto epoch_before_terrain = input_epoch_;
        on_terrain_changed(); finish_ocr(terrain_stale);
        check("SPH2_terrain_change_without_history_discards_first_optional_capture",
              input_epoch_ > epoch_before_terrain && !continuous_calibration_ && low_result_ && high_result_);
        OcrMessage before_map_change; capture_impact_context(before_map_change);
        before_map_change.success = true; before_map_change.action = OcrAction::calibration_impact;
        before_map_change.point = *target_; before_map_change.text = wardogs::format_point(*target_);
        before_map_change.assessment = wardogs::assess_ocr_result({before_map_change.text, 0.99F, 0.99F});
        before_map_change.input_epoch = input_epoch_; before_map_change.calibration_epoch = calibration_epoch_;
        record_continuous_impact(*target_, QStringLiteral("test"), *before_map_change.impact_firing);
        auto pending_base_for_map_change = before_map_change;
        pending_base_for_map_change.action = OcrAction::base;
        pending_base_for_map_change.force_review = true;
        base_capture_pending_ = true;
        finish_ocr(pending_base_for_map_change);
        check("map_change_fixture_has_an_unconfirmed_new_gun", pending_ocr_ && ocr_hold_ && base_capture_pending_);
        terrain_selector_->setCurrentIndex(terrain_selector_->findData(static_cast<int>(wardogs::GameMap::other)));
        finish_ocr(before_map_change);
        check("changing_game_map_clears_coordinates_history_guidance_and_stale_impact", map_confirmed_ &&
              !base_set_ && !target_ && history_.empty() && !continuous_calibration_ && !low_result_ && !high_result_ &&
              !copy_button_->isEnabled() && !pending_ocr_ && !ocr_hold_ && !base_capture_pending_);
        terrain_selector_->setCurrentIndex(terrain_selector_->findData(static_cast<int>(wardogs::GameMap::training)));
        base_input_->setText(QStringLiteral("80 80")); manual_base();
        target_input_->setText(QStringLiteral("92 90")); manual_target();
        auto same_map_pending_base = before_map_change;
        same_map_pending_base.action = OcrAction::base;
        same_map_pending_base.force_review = true;
        same_map_pending_base.input_epoch = input_epoch_;
        base_capture_pending_ = true;
        finish_ocr(same_map_pending_base);
        on_terrain_changed();
        check("same_map_confirmation_cannot_restore_an_unconfirmed_old_gun", map_confirmed_ &&
              !base_set_ && !target_ && !pending_ocr_ && !ocr_hold_ && !base_capture_pending_);
        base_input_->setText(QStringLiteral("80 80")); manual_base();
        target_input_->setText(QStringLiteral("92 90")); manual_target();
        OcrMessage before_base_change; capture_impact_context(before_base_change);
        record_continuous_impact(*target_, QStringLiteral("test"), *before_base_change.impact_firing);
        check("SPH2_clean_target_hit_is_valid_optional_evidence", observation_count() == 1);
        base_input_->setText(QStringLiteral("81 80")); manual_base();
        check("SPH2_new_gun_position_clears_direct_history_without_legacy_rows",
              base_ == wardogs::Point{81, 80} && !target_ && !continuous_calibration_ && !has_calibration_data());
        target_input_->setText(QStringLiteral("92 90")); manual_target();
        const auto new_base_raw = wardogs::corrected_solution(base_, *target_, {wardogs::identity_rotation(), 0.0}, wardogs::Arc::low);
        check("SPH2_new_gun_position_is_ready_for_immediate_raw_calculation",
              low_result_ && low_result_->mil == new_base_raw.mil && low_result_->bearing_deg == new_base_raw.bearing_deg);
        base_input_->setText(QStringLiteral("94.18 110.34")); manual_base();
        target_input_->setText(QStringLiteral("84.56 90.44")); manual_target();
        settings_.ghost_reticle.preferred_arc = wardogs::Arc::high;
        show_result(*target_);
        const auto field_low_before = *low_result_;
        const auto field_high_before = *high_result_;
        const auto field_history_before = history_;
        OcrMessage field_context;
        capture_impact_context(field_context);
        const bool field_impact_recorded = record_continuous_impact(
            {83.57, 92.59}, QStringLiteral("test field replay"), *field_context.impact_firing);
        check("SPH2_confirmed_field_miss_accepts_237_m_without_arbitrary_angular_rejection",
              field_impact_recorded && observation_count() == 1 && high_result_ &&
              effective_vehicle_arc() == wardogs::Arc::high &&
              std::abs(high_result_->bearing_deg - 200.7311095796922) < 1e-6 &&
              std::abs(high_result_->mil - 914.2386) < 1e-4 &&
              !ocr_hold_ && !failure_state_ && copy_button_->isEnabled());
        check("SPH2_field_impact_preserves_target_history_and_unobserved_low_arc",
              target_ == field_context.impact_firing->target && history_ == field_history_before &&
              low_result_->bearing_deg == field_low_before.bearing_deg &&
              low_result_->mil == field_low_before.mil &&
              status_->text().contains(wardogs::i18n::text(QStringLiteral("промах 237 м"))) &&
              status_->text().contains(wardogs::i18n::text(QStringLiteral("Азимут -5.1°"))) &&
              status_->text().contains(QStringLiteral("MIL -58.7")));
        const auto field_corrected = *high_result_;
        OcrMessage field_outlier_context;
        capture_impact_context(field_outlier_context);
        const bool field_outlier_rejected = !record_continuous_impact(
            {97.50, 90.44}, QStringLiteral("test unrelated impact"), *field_outlier_context.impact_firing);
        check("SPH2_field_history_and_displayed_command_survive_geometrically_unrelated_impact",
              field_outlier_rejected && observation_count() == 1 &&
              high_result_->bearing_deg == field_corrected.bearing_deg &&
              high_result_->mil == field_corrected.mil && history_ == field_history_before &&
              target_ == field_context.impact_firing->target && !ocr_hold_);
        clear_continuous_calibration();
        check("SPH2_clearing_field_correction_restores_original_973_MIL_command",
              !continuous_calibration_ && high_result_->bearing_deg == field_high_before.bearing_deg &&
              high_result_->mil == field_high_before.mil && !failure_state_);
        settings_.ghost_reticle.preferred_arc = wardogs::Arc::low;
        base_input_->setText(QStringLiteral("80 80")); manual_base();
        // Map changes deliberately clear earlier history. Seed this assertion
        // with current-map targets instead of relying on previous scenarios.
        for (int index = 0; index < 16; ++index)
            accept_manual_target({92.0 + index * 0.01, 90.0});
        const auto history_before_duplicate = history_;
        accept_manual_target(history_.front());
        check("recent_history_is_bounded_and_repeated_target_does_not_duplicate",
              history_.size() == 12 && history_ == history_before_duplicate);
        target_input_->setText(QStringLiteral("92 90")); manual_target();
        check("recent_targets_populated", history_.size() >= 3 && history_selector_->isEnabled() &&
              history_selector_->currentText() == qtext(wardogs::format_point(*target_)));
        click("copySolutionButton");
        check("clipboard_contains_real_solution", QApplication::clipboard()->text().contains(QStringLiteral("SPH-2")) &&
              QApplication::clipboard()->text().contains(QStringLiteral("MIL")));
        if (QGuiApplication::platformName() == QStringLiteral("windows")) {
            std::atomic<int> clipboard_lock_state{0};
            std::jthread clipboard_locker([&](std::stop_token stop) {
                const bool opened = OpenClipboard(nullptr) != FALSE;
                clipboard_lock_state.store(opened ? 1 : -1);
                if (opened) {
                    while (!stop.stop_requested()) Sleep(1);
                    CloseClipboard();
                }
            });
            QElapsedTimer lock_wait; lock_wait.start();
            while (clipboard_lock_state.load() == 0 && lock_wait.elapsed() < 1000) Sleep(1);
            const auto target_before_clipboard_error = target_;
            copy_solution();
            const bool copy_error_reported = failure_state_ &&
                !status_->text().contains(wardogs::i18n::text(QStringLiteral("Расчёт скопирован")));
            clipboard_locker.request_stop(); clipboard_locker.join();
            check("locked_clipboard_reports_failure_without_changing_guidance",
                  clipboard_lock_state.load() == 1 && copy_error_reported && target_ == target_before_clipboard_error &&
                  low_result_ && high_result_);
            copy_solution(); QApplication::processEvents();
            check("clipboard_recovers_after_external_lock_is_released", !failure_state_ &&
                  QApplication::clipboard()->text().contains(QStringLiteral("SPH-2")) &&
                  QApplication::clipboard()->text().contains(QStringLiteral("MIL")));
        }
        enter_pinned_mode(); QApplication::processEvents();
        check("pinned_card_visible", pinned_mode_ && pinned_window_->isVisible());
        exit_pinned_mode(); QApplication::processEvents();
        check("return_from_card_restores_main", !pinned_mode_ && isVisible());
        check("mouse_foreground_filters_helpers", is_wardogs_window_title(L"WARDOGS") &&
              !is_wardogs_window_title(L"WarDogsDistanceCalculator.exe") && !is_wardogs_window_title(L"WARDOGSLauncher.exe") &&
              !is_wardogs_window_title(L"notepad.exe") && !is_wardogs_window_title(L"WARDOGS · notes") &&
              !is_wardogs_window_title(L"WARDOGS Fire Control"));
        check("game_caption_matches_observed_trailing_spaces", is_wardogs_window_title(L"Wardogs  "));
        check("game_caption_trims_unicode_whitespace", is_wardogs_window_title(L"\t\u00a0WaRdOgS\u2003\r\n") &&
              is_wardogs_window_title(L"\u3000WardogsClient\u202f"));
        check("game_caption_rejects_empty_or_only_whitespace", !is_wardogs_window_title(L"") &&
              !is_wardogs_window_title(L"\t\u00a0\u2003\u3000\r\n"));
        check("game_caption_padding_does_not_accept_helpers", !is_wardogs_window_title(L"  WARDOGS Fire Control  ") &&
              !is_wardogs_window_title(L"\u3000WARDOGSLauncher.exe\u2003") &&
              !is_wardogs_window_title(L"\tWARDOGS · notes\r\n"));
        const auto saved_region = region_;
        const auto saved_capture_monitor = last_capture_monitor_;
        region_ = wardogs::CaptureRegion{L"custom-monitor"};
        last_capture_monitor_ = L"captured-monitor";
        sync_ghost_monitor();
        check("capture_monitor_overrides_custom_region_for_reticle",
              ghost_window_->target_monitor_name() == QStringLiteral("captured-monitor") &&
              region_->monitor_device == L"custom-monitor");
        region_ = saved_region;
        last_capture_monitor_ = saved_capture_monitor;
        sync_ghost_monitor();
        if (!image_path.isEmpty()) {
            if (vehicle_mode_) toggle_mode();
            base_input_->setText(QStringLiteral("113 191")); manual_base();
            busy_ = true;
            OcrMessage context; context.action = OcrAction::target; context.input_epoch = input_epoch_;
            launch_ocr_worker(wardogs::load_image_file(std::filesystem::path{image_path.toStdWString()}), std::move(context));
            QElapsedTimer timer; timer.start();
            while (busy_ && timer.elapsed() < 10000) {
                QApplication::processEvents(QEventLoop::AllEvents, 20);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            check("real_ocr_worker_to_ui", !busy_ && target_.has_value() &&
                  std::abs(target_->x - 114.51) < 1e-8 && std::abs(target_->y - 191.81) < 1e-8 && mortar_mil_result_.has_value());
        }
        if (test_capture_window) {
            if (vehicle_mode_) toggle_mode();
            base_input_->setText(QStringLiteral("113 191")); manual_base();
            target_input_->setText(QStringLiteral("115 192")); manual_target();
            const auto before_capture = target_;
            const auto before_history = history_.size();
            settings_.game_integration_enabled = true;
            settings_.automatic_chat_region = true;
            // Cooperatively hand focus back to the test's own fixture, as a
            // user would return to the game before pressing its capture key.
            // Never send this message to a real game or an unverified process.
            // Exact production caption, including its two trailing spaces.
            const HWND fixture = FindWindowW(L"UnrealWindow", L"Wardogs  ");
            DWORD fixture_pid{};
            DWORD_PTR prepared{};
            const bool verified_fixture = fixture && capture_fixture_pid != 0 &&
                GetWindowThreadProcessId(fixture, &fixture_pid) && fixture_pid == capture_fixture_pid;
            bool fixture_ready = verified_fixture && GetForegroundWindow() == fixture;
            if (verified_fixture && !fixture_ready) {
                // Permission transfer is useful when this child owns focus,
                // but its failure says nothing about the parent's existing
                // foreground rights. Always ask the verified parent and
                // judge the actual foreground result, not this advisory API.
                const bool permission_transferred = AllowSetForegroundWindow(fixture_pid) != 0;
                wardogs::log_info(std::string("test.fixture_foreground_permission=") +
                                  (permission_transferred ? "1" : "0"));
                fixture_ready = SendMessageTimeoutW(fixture, WM_APP + 0x331, 0, 0,
                                                    SMTO_ABORTIFHUNG, 1000, &prepared) &&
                                prepared != 0 && GetForegroundWindow() == fixture;
            }
            check("native_test_fixture_returns_to_foreground", fixture_ready);
            start_ocr(OcrAction::target);
            QElapsedTimer timer; timer.start();
            while (busy_ && timer.elapsed() < 10000) {
                QApplication::processEvents(QEventLoop::AllEvents, 20);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            check("native_chat_capture_to_review_preserves_previous_target",
                  !busy_ && pending_ocr_ && target_ == before_capture &&
                  history_.size() == before_history && ocr_hold_ && !copy_button_->isEnabled());
            check("native_chat_capture_reads_golden_pair", pending_ocr_ &&
                  pending_ocr_->point == wardogs::Point{114.51, 191.81} &&
                  pending_ocr_->assessment.multiple_lines && pending_ocr_->force_review);
            MONITORINFOEXW captured_monitor{};
            captured_monitor.cbSize = sizeof(captured_monitor);
            check("native_capture_reticle_uses_game_monitor",
                  GetMonitorInfoW(MonitorFromWindow(fixture, MONITOR_DEFAULTTONULL), &captured_monitor) &&
                  ghost_window_->target_monitor_name() == QString::fromWCharArray(captured_monitor.szDevice));
            if (pending_ocr_) confirm_ocr();
            check("native_chat_confirmation_updates_real_solution", !pending_ocr_ &&
                  target_ == wardogs::Point{114.51, 191.81} && !ocr_hold_ && mortar_mil_result_);
            prepared = 0;
            if (verified_fixture) (void)AllowSetForegroundWindow(fixture_pid);
            const bool draft_ready = verified_fixture &&
                SendMessageTimeoutW(fixture, WM_APP + 0x331, 1, 0, SMTO_ABORTIFHUNG, 1000, &prepared) &&
                prepared != 0 && GetForegroundWindow() == fixture;
            check("native_active_draft_fixture_is_ready", draft_ready);
            const auto manual_backend = settings_.backend;
            settings_.backend = wardogs::OcrBackend::windows;
            settings_.automatic_chat_region = false;
            const auto wait_for_ocr = [&] {
                QElapsedTimer deadline; deadline.start();
                while ((busy_ || (mouse_timer_ && mouse_timer_->isActive())) && deadline.elapsed() < 10000) {
                    QApplication::processEvents(QEventLoop::AllEvents, 20);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            };
            POINT map_cursor{700, 500};
            const bool converted = ClientToScreen(fixture, &map_cursor) != 0;
            const wardogs::MiddleMouseEvent map_event{map_cursor.x, map_cursor.y,
                reinterpret_cast<std::uintptr_t>(fixture)};
            OcrMessage delayed_map;
            delayed_map.map_coordinates = true;
            delayed_map.map_event = map_event;
            delayed_map.input_epoch = input_epoch_;
            finish_ocr(delayed_map);
            check("native_failed_map_schedules_bounded_retry", mouse_timer_->isActive() && map_retry_event_);
            start_ocr(OcrAction::base);
            check("native_alt_x_cancels_old_map_retry", !mouse_timer_->isActive() && !map_retry_event_);
            wait_for_ocr();
            check("native_alt_x_draft_sets_complete_base_without_review", !busy_ && !pending_ocr_ &&
                  base_set_ && base_ == wardogs::Point{98.74, 111.85} && !target_ && !ocr_hold_);
            check("native_automatic_workflow_is_independent_of_manual_windows_backend",
                  settings_.backend == wardogs::OcrBackend::windows && !pending_ocr_ &&
                  base_ == wardogs::Point{98.74, 111.85} && game_mode_);
            check("native_alt_x_ignores_stale_manual_region_setting",
                  !settings_.automatic_chat_region && !pending_ocr_ && !base_capture_pending_ &&
                  base_ == wardogs::Point{98.74, 111.85});
            check("native_base_automatically_arms_game_and_card_without_focus_theft", game_mode_ &&
                  pinned_mode_ && pinned_window_->isVisible() && GetForegroundWindow() == fixture);
            const auto target_before_viewport_change = target_;
            map_request_client_ = RECT{0, 0, 1, 1};
            OcrMessage changed_viewport;
            changed_viewport.success = true;
            changed_viewport.point = {99.51, 113.81};
            changed_viewport.map_coordinates = true;
            changed_viewport.map_event = map_event;
            changed_viewport.input_epoch = input_epoch_;
            finish_ocr(changed_viewport);
            check("native_viewport_change_before_apply_refuses_coordinates", target_ == target_before_viewport_change &&
                  !pending_ocr_ && ocr_hold_ && !mouse_timer_->isActive() && !mortar_mil_result_);
            start_map_ocr(map_event, false, OcrAction::target, true);
            check("native_middle_initial_capture_waits_for_game_render", !busy_ &&
                  mouse_timer_->isActive() && map_retry_event_ && ocr_hold_ && !mortar_mil_result_);
            wait_for_ocr();
            check("native_delayed_middle_capture_reaches_temporal_consensus", !busy_ &&
                  !pending_ocr_ && target_ == wardogs::Point{99.51, 113.81} && !ocr_hold_ && mortar_mil_result_);
            start_map_ocr(map_event);
            wait_for_ocr();
            check("native_middle_map_fields_calculate_without_chat_or_review", converted &&
                  !busy_ && !pending_ocr_ && target_ == wardogs::Point{99.51, 113.81} &&
                  !ocr_hold_ && mortar_mil_result_ && !history_.empty() && history_.front() == *target_);
            const auto map_target = target_;
            auto moved_event = map_event; ++moved_event.x;
            start_map_ocr(moved_event);
            check("native_moved_pointer_refuses_stale_tooltip", !busy_ && ocr_hold_ &&
                  target_ == map_target && !mortar_mil_result_ && !copy_button_->isEnabled());
            start_map_ocr(map_event);
            start_map_ocr(map_event);
            check("native_latest_map_request_replaces_busy_work", busy_ && queued_map_job_ && ocr_hold_);
            wait_for_ocr();
            check("native_repeated_map_target_recovers_and_latest_job_applies", !busy_ &&
                  !queued_map_job_ && !pending_ocr_ && !ocr_hold_ && target_ == map_target && mortar_mil_result_);
            exit_game_mode();
            check("native_return_restores_main_and_cancels_inflight_capture", !game_mode_ && !pinned_mode_ &&
                  !mouse_listener_.active() && !queued_map_job_ && !map_retry_event_ && isVisible());
            prepared = 0;
            if (verified_fixture) (void)AllowSetForegroundWindow(fixture_pid);
            const bool returned_to_fixture = verified_fixture &&
                SendMessageTimeoutW(fixture, WM_APP + 0x331, 1, 0, SMTO_ABORTIFHUNG, 1000, &prepared) &&
                prepared != 0 && GetForegroundWindow() == fixture;
            if (returned_to_fixture) start_map_ocr(map_event);
            wait_for_ocr();
            check("native_map_capture_remains_ready_after_return_to_main", returned_to_fixture &&
                  !game_mode_ && pinned_mode_ && !pending_ocr_ && !ocr_hold_ &&
                  target_ == map_target && mortar_mil_result_);
            if (!vehicle_mode_) toggle_mode();
            base_input_->setText(QStringLiteral("85 105")); manual_base();
            target_input_->setText(QStringLiteral("99.51 113.81")); manual_target();
            settings_.automatic_chat_region = true;
            settings_.ghost_reticle.preferred_arc = wardogs::Arc::low;
            sync_ghost_solution();
            handle_hotkey(5);
            const auto sph_target_before_impact = target_;
            const auto sph_history_before_impact = history_;
            const auto sph_samples_before_impact = continuous_calibration_
                ? continuous_calibration_->sample_count() : 0U;
            auto failed_impact_event = map_event;
            ++failed_impact_event.x;
            start_map_ocr(failed_impact_event, false, OcrAction::calibration_impact);
            check("native_SPH2_early_map_impact_failure_restores_guidance_for_repeat", !busy_ && !pending_ocr_ &&
                  !ocr_hold_ && low_result_ && high_result_ && target_ == sph_target_before_impact &&
                  history_ == sph_history_before_impact &&
                  (continuous_calibration_ ? continuous_calibration_->sample_count() : 0U) ==
                      sph_samples_before_impact);
            start_map_ocr(map_event, false, OcrAction::calibration_impact, true);
            const bool impact_cancel_cursor_moved = SetCursorPos(map_cursor.x + 1, map_cursor.y) != 0;
            wait_for_ocr();
            check("native_SPH2_cancelled_map_retry_restores_same_shot_guidance", impact_cancel_cursor_moved &&
                  !busy_ && !pending_ocr_ && !ocr_hold_ && low_result_ && high_result_ &&
                  target_ == sph_target_before_impact && history_ == sph_history_before_impact &&
                  (continuous_calibration_ ? continuous_calibration_->sample_count() : 0U) == sph_samples_before_impact);
            (void)SetCursorPos(map_cursor.x, map_cursor.y);
            handle_hotkey(4);
            check("native_SPH2_AltI_uses_map_capture_with_immutable_high_arc", busy_ && ocr_hold_ &&
                  !pending_ocr_ && target_ == sph_target_before_impact);
            wait_for_ocr();
            check("native_SPH2_map_impact_records_without_chat_or_target_change", !busy_ && !pending_ocr_ &&
                  !ocr_hold_ && continuous_calibration_ &&
                  continuous_calibration_->sample_count() == sph_samples_before_impact + 1U &&
                  target_ == sph_target_before_impact && history_ == sph_history_before_impact &&
                  effective_vehicle_arc() == wardogs::Arc::high && continuous_impact_->text().isEmpty() &&
                  low_result_ && high_result_);
            pinned_window_->set_locked(true);
            handle_hotkey(6);
            check("native_unlock_hotkey_opens_pinned_controls", !pinned_window_->is_locked());
            handle_hotkey(7);
            check("native_return_hotkey_restores_main_without_changing_SPH2_base", isVisible() &&
                  !pinned_mode_ && base_ == wardogs::Point{85, 105} && target_ == sph_target_before_impact);
            exit_game_mode();
            settings_.automatic_chat_region = true;
            settings_.backend = manual_backend;
        }
        const QSize previous_size = size();
        const bool previous_vehicle_mode = vehicle_mode_;
        for (const bool test_vehicle : {false, true}) {
            if (vehicle_mode_ != test_vehicle) toggle_mode();
            QApplication::processEvents();
            resize(640, 500);
            QApplication::processEvents();
            QApplication::processEvents();
            const auto* coordinates = findChild<QGroupBox*>(QStringLiteral("coordinatesGroup"));
            const auto* scroll = findChild<QScrollArea*>(QStringLiteral("mainContentScroll"));
            const auto fits_group = [](const QWidget* child, const QWidget* group) {
                return child && group && group->rect().contains(
                    QRect(child->mapTo(group, QPoint{}), child->size()));
            };
            const bool inputs_fit = coordinates &&
                coordinates->height() >= coordinates->minimumSizeHint().height() &&
                fits_group(base_summary_, coordinates) && fits_group(target_summary_, coordinates) &&
                fits_group(base_input_, coordinates) && fits_group(target_input_, coordinates) &&
                base_input_->height() >= base_input_->minimumSizeHint().height() &&
                target_input_->height() >= target_input_->minimumSizeHint().height();
            const auto* results = test_vehicle ? vehicle_result_group_ : mortar_result_group_;
            const bool results_fit = results->height() >= results->minimumSizeHint().height() &&
                (test_vehicle
                    ? fits_group(low_solution_, results) && fits_group(high_solution_, results)
                    : fits_group(distance_, results) && fits_group(bearing_, results) && fits_group(mortar_mil_, results));
            check(test_vehicle ? "compact_SPH2_sections_not_clipped" : "compact_L81_sections_not_clipped",
                  inputs_fit && results_fit);
            check(test_vehicle ? "compact_SPH2_scrolls" : "compact_L81_scrolls",
                  scroll && scroll->widget()->height() > scroll->viewport()->height() &&
                  scroll->verticalScrollBar()->maximum() > 0);
            check(test_vehicle ? "compact_SPH2_status_stays_visible" : "compact_L81_status_stays_visible",
                  rect().contains(QRect(status_->mapTo(this, QPoint{}), status_->size())));
            set_status(wardogs::i18n::text(QStringLiteral("Навесная · Стрельбище: расчёт готов.\n"
                                      "Alt+I — распознать разрыв и учесть поправку для следующего выстрела.")));
            QApplication::processEvents();
            QApplication::processEvents();
            check(test_vehicle ? "compact_SPH2_multiline_status_not_clipped" : "compact_L81_multiline_status_not_clipped",
                  status_->hasHeightForWidth() &&
                  status_->height() >= status_->heightForWidth(status_->width()) &&
                  rect().contains(QRect(status_->mapTo(this, QPoint{}), status_->size())));
        }
        if (vehicle_mode_ != previous_vehicle_mode) toggle_mode();
        QApplication::processEvents();
        const auto original_language = settings_.language;
        const auto language_index = [](wardogs::UiLanguage value) { return value == wardogs::UiLanguage::english ? 1 : 0; };
        const auto recorded_base = base_;
        const auto recorded_target = target_;
        const auto recorded_history = history_;
        const auto recorded_epoch = input_epoch_;
        const auto recorded_map = current_game_map_;
        const auto recorded_arc = effective_vehicle_arc();
        for (int iteration = 0; iteration < 4; ++iteration) {
            language_selector_->setCurrentIndex(iteration % 2 == 0 ? 1 : 0);
            QApplication::processEvents();
            check("language_switch_preserves_calculation_state",
                base_ == recorded_base && target_ == recorded_target && history_ == recorded_history &&
                input_epoch_ == recorded_epoch && current_game_map_ == recorded_map &&
                effective_vehicle_arc() == recorded_arc);
        }
        language_selector_->setCurrentIndex(1);
        target_input_->setText(QStringLiteral("84 83"));
        manual_target();
        check("english_new_result_uses_meters",
            distance_->text().endsWith(QStringLiteral(" m")) &&
            !raw_result_->text().contains(QRegularExpression(QStringLiteral("\\p{Cyrillic}"))));
        copy_solution();
        check("english_clipboard_fully_localized",
            QApplication::clipboard()->text().contains(QStringLiteral("Gun")) &&
            !QApplication::clipboard()->text().contains(QRegularExpression(QStringLiteral("\\p{Cyrillic}"))));
        for (int worker_language : {0, 1}) {
            language_selector_->setCurrentIndex(worker_language);
            OcrMessage queued_review;
            queued_review.action = OcrAction::target;
            queued_review.success = true;
            queued_review.force_review = true;
            queued_review.input_epoch = input_epoch_;
            queued_review.point = {84, 83};
            queued_review.text = L"x84, y83";
            queued_review.evidence_review_reason = QStringLiteral("текст координат обрезан у края");
            language_selector_->setCurrentIndex(1 - worker_language);
            finish_ocr(std::move(queued_review));
            check("queued_ocr_review_uses_language_at_completion",
                pending_ocr_ && ocr_review_reason_->text().contains(
                    wardogs::i18n::text(QStringLiteral("текст координат обрезан у края"))) &&
                (settings_.language == wardogs::UiLanguage::russian ||
                    !ocr_review_reason_->text().contains(QRegularExpression(QStringLiteral("\\p{Cyrillic}")))));
            discard_ocr_review();
        }
        language_selector_->setCurrentIndex(1);
        OcrMessage language_review;
        language_review.action = OcrAction::target; language_review.success = true;
        language_review.input_epoch = input_epoch_; language_review.point = {84, 83};
        language_review.text = L"x81.25, y83.50\nx84.00, y83.00";
        language_review.assessment = wardogs::assess_ocr_result({language_review.text, 0.92F, 0.62F});
        finish_ocr(std::move(language_review));
        const auto entered_review = QStringLiteral("84.123456 83.987654");
        ocr_candidates_->setCurrentText(entered_review);
        const auto review_epoch = input_epoch_;
        language_selector_->setCurrentIndex(0);
        language_selector_->setCurrentIndex(1);
        check("language_switch_preserves_pending_ocr_and_entered_coordinates",
            pending_ocr_.has_value() && ocr_hold_ && input_epoch_ == review_epoch &&
            ocr_candidates_->currentText() == entered_review && !copy_button_->isEnabled());
        discard_ocr_review();
        language_selector_->setCurrentIndex(language_index(original_language));
        // Exercise the planning callbacks against the actual main-window state,
        // including stale OCR epochs, out-of-range cards and gun restoration.
        if (vehicle_mode_) toggle_mode();
        terrain_selector_->setCurrentIndex(terrain_selector_->findData(static_cast<int>(wardogs::GameMap::training)));
        map_confirmed_ = true;
        ocr_hold_ = false;
        accept_manual_base({0, 0});
        accept_manual_target({5, 0});
        QTemporaryDir planning_storage;
        if (!planning_storage.isValid()) throw std::runtime_error("Unable to create isolated planning fixture directory");
        {
            PlanningDialog planning([this] { return planning_context(); },
                [this](auto kind, auto point, auto map, auto weapon) {
                    apply_planning_point(kind, point, map, weapon);
                }, this, std::filesystem::path(planning_storage.path().toStdWString()));
            check("planning_entry_is_available", planning_storage.isValid() &&
                  findChild<QPushButton*>(QStringLiteral("planningButton")));
            const auto press = [&](const char* name) {
                if (auto* button = planning.findChild<QPushButton*>(QString::fromUtf8(name))) button->click();
                QApplication::processEvents();
            };
            const auto epoch = input_epoch_;
            OcrMessage planning_stale_ocr;
            planning_stale_ocr.action = OcrAction::target;
            planning_stale_ocr.input_epoch = epoch;
            planning_stale_ocr.point = {4, 4};
            pending_ocr_ = planning_stale_ocr;
            press("correctRight");
            check("planning_correction_uses_gun_frame_and_invalidates_old_ocr",
                target_ && std::abs(target_->x - 5) < 1e-12 &&
                std::abs(target_->y + .1) < 1e-12 && input_epoch_ > epoch &&
                !pending_ocr_ && mortar_mil_result_ && history_.front() == *target_);
            const wardogs::Point precise{std::nextafter(0.1234567890123456, 1.0), -0.0};
            accept_manual_base(precise);
            planning.refresh();
            auto* name = planning.findChild<QLineEdit*>(QStringLiteral("missionName"));
            name->setText(QStringLiteral("CI exact gun"));
            press("saveFiringPosition");
            accept_manual_base({1, 1});
            accept_manual_target({5, 1});
            planning.refresh();
            auto* saved = planning.findChild<QListWidget*>(QStringLiteral("savedFireMissions"));
            saved->setCurrentRow(0);
            continuous_calibration_.emplace(base_, wardogs::PlatformCalibration{wardogs::identity_rotation(), 0.0});
            press("restoreFireMission");
            check("planning_restore_preserves_exact_gun_and_clears_target_and_calibration",
                base_ == precise && std::signbit(base_.y) && !target_ && !continuous_calibration_ &&
                !mortar_mil_result_ && !low_result_ && !high_result_);
            accept_manual_base({0, 0});
            accept_manual_target({6.7, 0});
            planning.refresh();
            planning.findChild<QComboBox*>(QStringLiteral("correctionStep"))->setCurrentIndex(3);
            press("correctAdd");
            check("planning_out_of_range_correction_hides_previous_mil",
                target_ && std::abs(target_->x - 7.7) < 1e-12 &&
                !mortar_mil_result_ && mortar_mil_->text() == wardogs::i18n::text(QStringLiteral("Вне диапазона")));
            bool wrong_map_rejected = false;
            try { apply_planning_point(wardogs::FireMissionKind::target, {5, 0},
                                      wardogs::GameMap::other, wardogs::AnalysisWeapon::l81); }
            catch (const std::invalid_argument&) { wrong_map_rejected = true; }
            check("planning_owner_rechecks_confirmed_map", wrong_map_rejected && target_->x > 7.6);
        }
        OcrMessage automatic_base;
        automatic_base.success = true;
        automatic_base.action = OcrAction::base;
        automatic_base.point = {80, 80};
        automatic_base.text = L"x80.00 y80.00";
        automatic_base.assessment = wardogs::assess_ocr_result({automatic_base.text, 0.99F, 0.99F});
        automatic_base.input_epoch = input_epoch_;
        automatic_base.calibration_epoch = calibration_epoch_;
        finish_ocr(automatic_base);
        check("automatic_base_acceptance_is_exercised_before_log_audit",
              base_set_ && base_ == automatic_base.point && !target_ && !pending_ocr_ && !ocr_hold_);
        resize(previous_size);
        QApplication::processEvents();
        QApplication::clipboard()->setMimeData(previous_clipboard.release());
        check("battle_diagnostics_flush_while_application_is_open", wardogs::flush_session_log());
        QFile diagnostic_log(QString::fromStdWString(wardogs::active_log_path().wstring()));
        const bool log_readable = diagnostic_log.open(QIODevice::ReadOnly);
        const auto log_bytes = log_readable ? diagnostic_log.readAll() : QByteArray{};
        check("battle_diagnostics_use_separate_diagnostic_file", log_readable &&
              wardogs::active_log_path().filename() == L"diagnostic.log");
        for (const auto* event : {"coordinates.accepted action=base source=manual",
                                 "coordinates.accepted action=target source=history",
                                 "coordinates.accepted action=base source=planning",
                                 "coordinates.accepted action=base source=ocr_auto",
                                 "coordinates.accepted action=target source=ocr_confirmed",
                                 "ocr.review_required", "ocr.review_confirmed", "ocr.review_cancelled",
                                 "weapon.selected", "guidance.arc_selected", "continuous.impact_recorded",
                                 "continuous.cleared", "solution.l81", "terrain.solution",
                                 "low_available=0 high_available=1", "selected_arc=unavailable"}) {
            const auto name = std::string("battle_log_contains_") + event;
            check(name.c_str(), log_bytes.contains(event));
        }
        return {{QStringLiteral("passed"), passed}, {QStringLiteral("checks"), checks},
                {QStringLiteral("version"), QStringLiteral(WARDOGS_VERSION)},
                {QStringLiteral("boundary"), QStringLiteral("Diagnostic user paths; live game focus/fullscreen/hits not simulated")}};
    }

protected:
    void closeEvent(QCloseEvent* event) override {
        closing_ = true;
        advance_input_epoch();
        if (mouse_timer_) mouse_timer_->stop();
        unregister_hotkeys();
        mouse_listener_.stop();
        if (ghost_window_) ghost_window_->hide();
        if (pinned_window_) pinned_window_->hide();
        if (tray_) tray_->hide();
        if (worker_.joinable()) { worker_.request_stop(); worker_.join(); }
        event->accept();
        QApplication::quit();
    }

    void resizeEvent(QResizeEvent* event) override {
        QMainWindow::resizeEvent(event);
        if (workspace_layout_) {
            const bool compact = width() < 900;
            workspace_layout_->setDirection(compact ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
            if (side_panel_) side_panel_->setMaximumWidth(compact ? QWIDGETSIZE_MAX : 290);
        }
    }

    bool nativeEvent(const QByteArray& event_type, void* message,
                     qintptr* result) override {
        if (handle_frameless_native_event(this, message, result)) return true;
        return QMainWindow::nativeEvent(event_type, message, result);
    }

private:
    wardogs::AppSettings settings_{};
    QJsonObject clipboard_copy_diagnostics_;
    wardogs::Point base_{};
    bool base_set_{};
    bool base_capture_pending_{};
    bool diagnostic_{};
    wardogs::updates::Controller* updates_{};
    bool closing_{};
    bool selecting_{};
    bool game_mode_{};
    std::uint64_t input_epoch_{1};
    QString startup_error_;
    std::deque<wardogs::Point> history_;
    std::optional<wardogs::Point> target_;
    std::optional<OcrMessage> pending_ocr_;
    std::optional<CapturedOcrJob> queued_map_job_;
    std::optional<wardogs::MiddleMouseEvent> map_retry_event_;
    unsigned map_retry_count_{};
    wardogs::MapCaptureConsensus map_consensus_;
    std::optional<OcrMessage> map_best_result_;
    std::optional<OcrMessage> map_request_context_;
    OcrAction map_retry_action_{OcrAction::target};
    std::optional<RECT> map_request_client_;
    std::optional<std::chrono::steady_clock::time_point> map_requested_at_;
    bool ocr_hold_{};
    wardogs::TerrainDiscovery terrain_discovery_;
    std::unique_ptr<wardogs::TerrainPackage> terrain_;
    std::optional<wardogs::InstalledTerrainMap> terrain_map_;
    wardogs::GameMap current_game_map_{wardogs::GameMap::unselected};
    bool map_confirmed_{};
    std::optional<wardogs::ContinuousCalibration> continuous_calibration_;
    std::optional<wardogs::ImpactFeedback> last_impact_feedback_;
    double last_impact_consistency_{};
    std::array<std::pair<double,double>, 2> active_aim_offsets_{};
    QLabel *fire_control_summary_{}, *terrain_assistance_{};
    QPushButton *fire_control_reset_{};
    struct AnalysisCache {
        wardogs::Point base, target;
        wardogs::GameMap map;
        const wardogs::TerrainPackage* terrain;
        std::array<std::optional<wardogs::FiringAnalysis>, 2> arcs;
    };
    std::optional<AnalysisCache> automatic_analysis_;
    QString recent_history_error_;
    std::uint64_t calibration_epoch_{};
    std::optional<wardogs::CaptureRegion> region_;
    std::wstring last_capture_monitor_;
    std::unique_ptr<wardogs::RapidOcr> rapid_;
    std::unique_ptr<wardogs::WindowsOcr> windows_;
    wardogs::GlobalHotkeyListener hotkey_listener_;
    wardogs::GlobalMouseListener mouse_listener_;
    QTimer* mouse_timer_{};
    QSystemTrayIcon* tray_{};
    std::uint64_t pending_mouse_epoch_{};
    HANDLE show_event_{};
    QWinEventNotifier *unlock_notifier_{}, *show_notifier_{};
    HANDLE unlock_event_{};
    std::jthread worker_;
    std::atomic_bool busy_{false};
    SelectionOverlay selector_;
    std::unique_ptr<PinnedResultWindow> pinned_window_;
    std::unique_ptr<GhostReticleWindow> ghost_window_;
    std::optional<wardogs::CorrectedSolution> low_result_;
    std::optional<wardogs::CorrectedSolution> high_result_;
    std::optional<double> mortar_mil_result_;
    bool vehicle_mode_{};
    bool pinned_mode_{};
    bool ghost_enabled_{};
    bool failure_state_{};
    QFrame* app_frame_{};
    QBoxLayout* workspace_layout_{};
    QWidget* side_panel_{};
    DirectionPlot* direction_plot_{};
    QComboBox* history_selector_{};
    QComboBox* language_selector_{};
    QPushButton *game_button_{}, *copy_button_{};
    QToolButton* calibration_toggle_{};
    QLabel* readiness_{};
    QLabel *quick_guide_{}, *quick_state_{};
    QToolButton* manual_toggle_{};
    QLabel *base_summary_{}, *target_summary_{}, *distance_{}, *bearing_{},
        *mortar_mil_{};
    QLabel *raw_result_{}, *engine_summary_{}, *region_summary_{}, *ocr_text_{}, *status_{};
    QGroupBox* ocr_review_{};
    QLabel* ocr_review_reason_{};
    QComboBox* ocr_candidates_{};
    QLabel *terrain_summary_{}, *calibration_summary_{}, *vehicle_note_{};
    QLineEdit *base_input_{}, *target_input_{};
    QLineEdit *continuous_aim_{}, *continuous_impact_{};
    QPushButton *region_button_{}, *base_button_{}, *target_button_{},
        *quick_target_button_{};
    QPushButton *ghost_button_{}, *mode_button_{}, *pin_button_{},
        *calibration_ocr_{}, *calibration_manual_{}, *continuous_arc_{},
        *clear_continuous_{};
    QComboBox *terrain_selector_{};
    QPushButton *confirm_map_{}, *import_terrain_{};
    QGroupBox *terrain_group_{}, *calibration_group_{}, *mortar_result_group_{},
        *vehicle_result_group_{};
    VehicleSolutionWidget *low_solution_{}, *high_solution_{};

    bool require_game_integration() {
        if (diagnostic_ || settings_.game_integration_enabled) return true;
        set_status(wardogs::i18n::text(QStringLiteral("Сейчас работает отдельный калькулятор. Захват и окна поверх игры включаются в настройках → «В игре». Разрешение BULKHEAD не подтверждено.")), true);
        return false;
    }

    void discard_ocr_review() {
        if (pending_ocr_)
            wardogs::log_info(std::string("ocr.review_discarded action=") +
                action_name(pending_ocr_->action) + " epoch=" + std::to_string(input_epoch_));
        pending_ocr_.reset();
        if (ocr_review_) ocr_review_->hide();
    }

    void advance_input_epoch(bool preserve_review = false) {
        ++input_epoch_;
        queued_map_job_.reset();
        map_retry_event_.reset();
        map_consensus_ = {};
        map_best_result_.reset();
        map_request_context_.reset();
        map_request_client_.reset();
        map_requested_at_.reset();
        if (mouse_timer_) mouse_timer_->stop();
        if (preserve_review && pending_ocr_) pending_ocr_->input_epoch = input_epoch_;
        else discard_ocr_review();
    }

    void confirm_ocr() {
        if (!pending_ocr_) return;
        if (pending_ocr_->input_epoch != input_epoch_ ||
            (pending_ocr_->action == OcrAction::calibration_impact &&
             pending_ocr_->calibration_epoch != calibration_epoch_)) {
            discard_ocr_review();
            set_status(wardogs::i18n::text(QStringLiteral("Исходные данные изменились. Захватите координаты заново.")), true);
            return;
        }
        try {
            auto message = *pending_ocr_;
            message.point = wardogs::parse_manual_coordinate(ocr_candidates_->currentText().toStdWString());
            if (message.action == OcrAction::target)
                (void)wardogs::calculate_shot(base_, message.point);
            message.confirmed = true;
            log_coordinate_event("ocr.review_confirmed", action_name(message.action),
                                 "user", message.point);
            discard_ocr_review();
            ocr_hold_ = false;
            finish_ocr(std::move(message));
        } catch (const std::exception& error) {
            set_status(wardogs::i18n::text(QStringLiteral("Проверьте координаты: ")) + error_text(error), true);
        }
    }

    void cancel_ocr_review() {
        if (!pending_ocr_) return;
        log_coordinate_event("ocr.review_cancelled", action_name(pending_ocr_->action),
                             "user", pending_ocr_->point);
        if (pending_ocr_->action == OcrAction::base) base_capture_pending_ = false;
        advance_input_epoch();
        ocr_hold_ = false;
        if (target_ && base_set_) show_result(*target_);
        update_readiness();
        set_status(wardogs::i18n::text(QStringLiteral("Захват отклонён. Восстановлен предыдущий расчёт.")));
    }

    void update_game_button() {
        if (!game_button_) return;
        if (game_mode_) game_button_->setText(wardogs::i18n::text(QStringLiteral("Вернуться · ")) + qtext(settings_.exit_game_mode_hotkey));
        else game_button_->setText(!settings_.game_integration_enabled
            ? wardogs::i18n::text(QStringLiteral("Игровые функции"))
            : map_confirmed_ ? wardogs::i18n::text(QStringLiteral("В игру"))
                             : wardogs::i18n::text(QStringLiteral("Подтвердить карту и в игру")));
    }

    void remember_accepted_point(wardogs::FireMissionKind kind, wardogs::Point point) {
        if (diagnostic_ || !map_confirmed_) return;
        try {
            wardogs::FireMissionRepository repository(wardogs::recent_fire_missions_path());
            (void)repository.remember(current_game_map_, vehicle_mode_ ? wardogs::FireMissionWeapon::sph2
                                                                      : wardogs::FireMissionWeapon::l81,
                                      kind, point);
            recent_history_error_.clear();
        } catch (const std::exception& error) {
            recent_history_error_ = QString::fromUtf8(error.what());
            wardogs::log_warning("history.accepted_point_save_failed error=" + std::string(error.what()));
        }
    }

    void cache_automatic_analysis(const wardogs::Shot& shot) {
        if (automatic_analysis_ && automatic_analysis_->base == shot.base &&
            automatic_analysis_->target == shot.target && automatic_analysis_->map == current_game_map_ &&
            automatic_analysis_->terrain == terrain_.get()) return;
        AnalysisCache cache{shot.base, shot.target, current_game_map_, terrain_.get(), {}};
        for (std::size_t index = 0; index < 2; ++index) {
            try {
                wardogs::FiringAnalysisRequest request;
                request.weapon = wardogs::AnalysisWeapon::sph2;
                request.arc = index == 0 ? wardogs::Arc::low : wardogs::Arc::high;
                request.base = shot.base; request.target = shot.target;
                if (terrain_) request.terrain = [this](wardogs::Point point) { return terrain_->height_at(point); };
                // No assumed gravity, speed, flight time, or Alt+I timestamp.
                cache.arcs[index] = wardogs::analyze_firing(request);
            } catch (const std::exception& error) {
                wardogs::log_warning("analysis.automatic_failed error=" + std::string(error.what()));
            }
        }
        automatic_analysis_ = std::move(cache);
    }

    void update_fire_control() {
        if (!fire_control_summary_) return;
        const auto arc = effective_vehicle_arc();
        const bool ready = map_confirmed_ && base_set_ && target_ && arc &&
                           !base_capture_pending_ && !ocr_hold_;
        fire_control_reset_->setEnabled(has_calibration_data());
        QString text;
        if (!ready) text = wardogs::i18n::text(QStringLiteral("Подтвердите карту, задайте орудие и цель. Пристрелка появится после первого наблюдения попадания."));
        else {
            text = wardogs::i18n::text(QStringLiteral("Наблюдений: %1 · %2 у попадания — автоматическая поправка, цель остаётся на месте."))
                .arg(continuous_calibration_ ? continuous_calibration_->sample_count() : 0U)
                .arg(qtext(settings_.impact_hotkey));
            if (last_impact_feedback_ && last_impact_feedback_->target == *target_ && last_impact_feedback_->arc == *arc) {
                const auto& feedback = *last_impact_feedback_;
                const auto signed_value = [](double value, int precision = 1) {
                    if (std::abs(value) < 0.05) value = 0.0;
                    return (value > 0.0 ? QStringLiteral("+") : QString{}) + QString::number(value, 'f', precision);
                };
                text += wardogs::i18n::text(QStringLiteral("\nПоследний промах: %1 м · вправо + / влево −: %2 м · перелёт + / недолёт −: %3 м."))
                    .arg(QString::number(feedback.miss_m, 'f', 0), signed_value(feedback.right_m), signed_value(feedback.far_m));
                text += wardogs::i18n::text(QStringLiteral("\nУчтённое изменение: азимут %1° · MIL %2 · по таблице %3 м."))
                    .arg(signed_value(feedback.bearing_change_deg), signed_value(feedback.mil_change), signed_value(feedback.table_range_change_m));
                if (last_impact_consistency_ < 0.35)
                    text += wardogs::i18n::text(QStringLiteral("\nНаблюдения расходятся: точность поправки ограничена."));
            }
            text += wardogs::i18n::text(QStringLiteral("\nПравее / левее — градусы азимута. Дальше / ближе — MIL: настильная + / −, навесная − / +."));
            if (has_calibration_data()) {
                const auto [bearing_offset,mil_offset] = active_aim_offsets_[*arc == wardogs::Arc::low ? 0U : 1U];
                text += wardogs::i18n::text(QStringLiteral("\nВ текущей команде учтено: азимут %1° · MIL %2. При смене цели или дуги влияние поправки может уменьшиться."))
                    .arg(bearing_offset,0,'f',1).arg(mil_offset,0,'f',1);
            }
        }
        fire_control_summary_->setText(text);
        terrain_assistance_->clear();
        if (!ready) return;
        if (!terrain_) {
            terrain_assistance_->setText(wardogs::i18n::text(QStringLiteral("Профиль земли не проверен: нет высот. Здания и препятствия не определяются.")));
            return;
        }
        const auto index = *arc == wardogs::Arc::low ? 0U : 1U;
        if (!automatic_analysis_ || !automatic_analysis_->arcs[index]) {
            terrain_assistance_->setText(wardogs::i18n::text(QStringLiteral("Оценка профиля земли недоступна.")));
            return;
        }
        const auto& analysis = *automatic_analysis_->arcs[index];
        QString ground;
        switch (analysis.clearance.status) {
        case wardogs::TerrainClearanceStatus::clear_at_samples:
            ground = wardogs::i18n::text(QStringLiteral("По приближённой модели пересечений земли в проверенных точках нет.")); break;
        case wardogs::TerrainClearanceStatus::blocked:
            ground = wardogs::i18n::text(QStringLiteral("Приближённая дуга пересекает землю на ≈%1 м от орудия."))
                .arg(analysis.clearance.first_blocked_distance_m.value_or(0.0), 0, 'f', 0);
            if (const auto& other = automatic_analysis_->arcs[1U - index];
                other && other->clearance.status == wardogs::TerrainClearanceStatus::clear_at_samples &&
                (*arc == wardogs::Arc::low ? high_result_.has_value() : low_result_.has_value()))
                ground += wardogs::i18n::text(QStringLiteral(" Проверьте другую траекторию через %1; она не переключается автоматически."))
                    .arg(qtext(settings_.ghost_arc_hotkey));
            break;
        case wardogs::TerrainClearanceStatus::incomplete:
            ground = wardogs::i18n::text(QStringLiteral("Профиль земли неполный: часть высот недоступна.")); break;
        default:
            ground = wardogs::i18n::text(QStringLiteral("Оценка профиля земли недоступна.")); break;
        }
        terrain_assistance_->setText(ground + wardogs::i18n::text(QStringLiteral("\nЭто номинальная модель без поправки попадания; здания и высота ствола не учтены.")));
    }

    void update_readiness() {
        update_game_button();
        update_fire_control();
        if (quick_state_)
            quick_state_->setText(base_set_
                ? wardogs::i18n::text(QStringLiteral("Орудие задано: ")) + qtext(wardogs::format_point(base_)) +
                    (base_capture_pending_ ? wardogs::i18n::text(QStringLiteral("\nНовое орудие ещё не прочитано · повторите %1 или проверьте захват.")).arg(qtext(settings_.base_hotkey))
                     : ocr_hold_ ? wardogs::i18n::text(QStringLiteral("\nНовый захват ещё не подтверждён · прежняя наводка скрыта."))
                     : target_ ? wardogs::i18n::text(QStringLiteral("\nЦель: ")) + qtext(wardogs::format_point(*target_))
                               : settings_.game_integration_enabled && settings_.middle_mouse_enabled
                                   ? wardogs::i18n::text(QStringLiteral("\nГотово. Ставьте отметки средней кнопкой на карте."))
                                   : settings_.game_integration_enabled
                                       ? wardogs::i18n::text(QStringLiteral("\nУкажите цель или нажмите «В игру»."))
                                       : wardogs::i18n::text(QStringLiteral("\nВведите координаты цели.")))
                : settings_.game_integration_enabled
                    ? wardogs::i18n::text(QStringLiteral("Орудие ещё не задано · M → ПКМ → Отметить координаты → ")) + qtext(settings_.base_hotkey)
                    : wardogs::i18n::text(QStringLiteral("Орудие ещё не задано · введите его координаты вручную.")));
        if (quick_state_ && !map_confirmed_)
            quick_state_->setText(wardogs::i18n::text(QStringLiteral("Сначала выберите и подтвердите текущую карту выше.")));
        else if (quick_state_ && vehicle_mode_ && target_ && !ocr_hold_ && !base_capture_pending_)
            quick_state_->setText(quick_state_->text() + QStringLiteral("\n") + sph2_workflow_hint());
        if (quick_state_ && !recent_history_error_.isEmpty()) quick_state_->setText(quick_state_->text() + QStringLiteral("\n") +
            wardogs::i18n::text(QStringLiteral("История не сохранена: ")) + wardogs::i18n::text(recent_history_error_));
        if (!readiness_) return;
        const auto capture = !settings_.game_integration_enabled ? wardogs::i18n::text(QStringLiteral("○ Захват выключен"))
            : settings_.middle_mouse_enabled ? wardogs::i18n::text(QStringLiteral("✓ %1: чат · средняя кнопка: карта")).arg(qtext(settings_.base_hotkey))
                                             : wardogs::i18n::text(QStringLiteral("✓ Орудие: автопоиск в чате"));
        const bool guidance_ready = map_confirmed_ && base_set_ && !base_capture_pending_ && !ocr_hold_ &&
            (vehicle_mode_ ? low_result_.has_value() || high_result_.has_value() : mortar_mil_result_.has_value());
        if (quick_guide_) quick_guide_->setVisible(!guidance_ready);
        readiness_->setText(wardogs::i18n::text(QStringLiteral("%1 Карта подтверждена\n%2 Орудие задано\n%3\n%4 Цель рассчитана"))
            .arg(map_confirmed_ ? QStringLiteral("✓") : QStringLiteral("○"))
            .arg(base_set_ && !base_capture_pending_ ? QStringLiteral("✓") : QStringLiteral("○"))
            .arg(capture)
            .arg(guidance_ready ? QStringLiteral("✓") : QStringLiteral("○")));
    }

    void log_coordinate_event(const char* event, const char* action,
                              const char* source, wardogs::Point point) {
        std::ostringstream diagnostic;
        diagnostic.imbue(std::locale::classic());
        diagnostic.precision(17);
        diagnostic << event << " action=" << action << " source=" << source
                   << " point=" << point.x << ',' << point.y
                   << " map=" << utf8(qtext(std::wstring{wardogs::game_map_key(current_game_map_)}))
                   << " map_confirmed=" << map_confirmed_
                   << " weapon=" << (vehicle_mode_ ? "sph2" : "l81")
                   << " input_epoch=" << input_epoch_ << " calibration_epoch=" << calibration_epoch_;
        wardogs::log_info(diagnostic.str());
    }

    void remember_target(wardogs::Point target, const char* source = "manual") {
        log_coordinate_event("coordinates.accepted", "target", source, target);
        remember_accepted_point(wardogs::FireMissionKind::target, target);
        const auto existing = std::find(history_.begin(), history_.end(), target);
        if (existing != history_.end()) history_.erase(existing);
        history_.push_front(target);
        if (history_.size() > 12) history_.pop_back();
        QSignalBlocker blocker(history_selector_);
        history_selector_->clear();
        for (const auto& point : history_) history_selector_->addItem(qtext(wardogs::format_point(point)));
        history_selector_->setCurrentIndex(0);
        history_selector_->setEnabled(true);
    }

    void copy_solution() {
        if (!base_set_ || !target_ || ocr_hold_) return;
        try {
        const auto shot = wardogs::calculate_shot(base_, *target_);
        QString text = wardogs::i18n::text(QStringLiteral("%1 | Орудие %2 | Цель %3 | %4 | %5"))
            .arg(vehicle_mode_ ? QStringLiteral("SPH-2") : QStringLiteral("L81"))
            .arg(qtext(wardogs::format_point(base_)), qtext(wardogs::format_point(*target_)),
                 wardogs::i18n::text(qtext(wardogs::format_distance_meters(shot.distance))), qtext(wardogs::format_bearing(shot.angle)));
        if (vehicle_mode_) {
            if (const auto arc = effective_vehicle_arc()) text += *arc == wardogs::Arc::low
                ? wardogs::i18n::text(QStringLiteral(" | Выбрана: настильная")) : wardogs::i18n::text(QStringLiteral(" | Выбрана: навесная"));
            if (low_result_) text += wardogs::i18n::text(QStringLiteral(" | Настильная: %1 / %2 MIL / по таблице ≈%3 м")).arg(qtext(wardogs::format_bearing(low_result_->bearing_deg))).arg(low_result_->mil, 0, 'f', 1).arg(low_result_->reticle_distance_m, 0, 'f', 1);
            if (high_result_) text += wardogs::i18n::text(QStringLiteral(" | Навесная: %1 / %2 MIL / по таблице ≈%3 м")).arg(qtext(wardogs::format_bearing(high_result_->bearing_deg))).arg(high_result_->mil, 0, 'f', 1).arg(high_result_->reticle_distance_m, 0, 'f', 1);
        } else if (mortar_mil_result_) text += QStringLiteral(" | %1 MIL").arg(qRound(*mortar_mil_result_));
        auto* clipboard = QApplication::clipboard();
        const DWORD sequence_before = diagnostic_ ? GetClipboardSequenceNumber() : 0;
        clipboard->setText(text);
        if (diagnostic_) clipboard_copy_diagnostics_ = {
            {QStringLiteral("sequence_before"), static_cast<qint64>(sequence_before)},
            {QStringLiteral("sequence_after_set"), static_cast<qint64>(GetClipboardSequenceNumber())},
            {QStringLiteral("owned_after_set"), clipboard->ownsClipboard()},
            {QStringLiteral("matches_after_set"), clipboard->text() == text}};
        if (QGuiApplication::platformName() == QStringLiteral("windows")) {
            if (!clipboard->ownsClipboard())
                throw std::runtime_error("Буфер обмена недоступен. Повторите копирование.");
            // Materialize this app's text instead of leaving a delayed OLE
            // object whose subsequent message processing can lose ownership.
            const HRESULT flushed = OleFlushClipboard();
            if (diagnostic_) clipboard_copy_diagnostics_.insert(
                QStringLiteral("flush_hresult"), static_cast<qint64>(flushed));
            if (FAILED(flushed))
                throw std::runtime_error("Не удалось сохранить расчёт в буфере обмена. Повторите копирование.");
        }
        if (clipboard->text() != text)
            throw std::runtime_error("Расчёт не записан в буфер обмена. Повторите копирование.");
        set_status(wardogs::i18n::text(QStringLiteral("Расчёт скопирован")));
        } catch (const std::exception& error) {
            set_status(wardogs::i18n::text(QStringLiteral("Не удалось скопировать расчёт: ")) + error_text(error), true);
        }
    }

    void cancel_map_capture(const QString& reason) {
        const auto context = map_request_context_;
        if (context) restore_failed_impact_guidance(*context);
        advance_input_epoch();
        update_readiness();
        set_status(reason, true);
        wardogs::log_info("capture.map_cancelled epoch=" + std::to_string(input_epoch_));
    }

    void setup_tray() {
        mouse_timer_ = new QTimer(this);
        mouse_timer_->setSingleShot(true);
        connect(mouse_timer_, &QTimer::timeout, this, [this] {
            if (!closing_ && settings_.game_integration_enabled &&
                (settings_.middle_mouse_enabled || map_retry_action_ == OcrAction::calibration_impact) &&
                base_set_ && !base_capture_pending_ && !selecting_ && map_retry_event_ &&
                !QApplication::activeModalWidget() &&
                pending_mouse_epoch_ == input_epoch_ && wardogs_is_foreground()) {
                const auto event = *map_retry_event_;
                if (map_source_matches(event, map_request_client_))
                    start_map_ocr(event, true, map_retry_action_);
                else {
                    cancel_map_capture(wardogs::i18n::text(QStringLiteral("Курсор или окно изменились. Повторите захват этой точки.")));
                }
            } else if (map_retry_event_ && pending_mouse_epoch_ == input_epoch_) {
                cancel_map_capture(wardogs::i18n::text(QStringLiteral("Повторное чтение отменено. Вернитесь в игру и нажмите среднюю кнопку на цели.")));
            }
        });
        if (diagnostic_ || !QSystemTrayIcon::isSystemTrayAvailable()) return;
        tray_ = new QSystemTrayIcon(wardogs_application_icon(), this);
        tray_->setToolTip(QStringLiteral("WARDOGS Fire Control"));
        auto* menu = new QMenu(this);
        connect(menu->addAction(wardogs::i18n::text(QStringLiteral("Открыть расчёт"))), &QAction::triggered, this, [this] { exit_game_mode(); });
        connect(menu->addAction(wardogs::i18n::text(QStringLiteral("Мини-карточка"))), &QAction::triggered, this, &MainWindow::enter_pinned_mode);
        connect(menu->addAction(wardogs::i18n::text(QStringLiteral("Снять блокировку карточки"))), &QAction::triggered, this, [this] { unlock_pinned_window("tray"); });
        menu->addSeparator();
        connect(menu->addAction(wardogs::i18n::text(QStringLiteral("Завершить"))), &QAction::triggered, this, &QWidget::close);
        tray_->setContextMenu(menu);
        connect(tray_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
            if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) exit_game_mode();
        });
        tray_->show();
    }

    void register_mouse_trigger() { register_mouse_trigger(settings_); }

    void register_mouse_trigger(const wardogs::AppSettings& settings) {
        if (diagnostic_) return;
        mouse_listener_.stop();
        if (!settings.game_integration_enabled || !settings.middle_mouse_enabled) return;
        const QPointer<MainWindow> self(this);
        mouse_listener_.start([self](wardogs::MiddleMouseEvent event) {
            if (self) QMetaObject::invokeMethod(self, [self, event] {
                if (!self || self->closing_ || !wardogs_is_foreground() ||
                    reinterpret_cast<HWND>(event.foreground_window) != GetForegroundWindow()) return;
                if (!self->settings_.game_integration_enabled || !self->settings_.middle_mouse_enabled) return;
                if (self->selecting_ || QApplication::activeModalWidget()) {
                    wardogs::log_info("map.request_ignored reason=modal_or_selection");
                    return;
                }
                if (!self->base_set_) {
                    wardogs::log_info("map.request_ignored reason=base_not_set");
                    self->set_status(wardogs::i18n::text(QStringLiteral("Сначала задайте орудие: M → ПКМ → Отметить координаты → ")) +
                                     qtext(self->settings_.base_hotkey), true);
                    return;
                }
                wardogs::log_info("map.middle_request_accepted");
                self->start_map_ocr(event, false, OcrAction::target, true);
            }, Qt::QueuedConnection);
        });
    }

    void enter_game_mode(bool explicit_confirmation = false) {
        if (!require_game_integration()) return;
        if (busy_ || selecting_) { set_status(wardogs::i18n::text(QStringLiteral("Дождитесь окончания текущего действия"))); return; }
        // Clicking the clearly labelled button explicitly confirms the selected
        // map. A stored last-map preference alone still never confirms a match.
        if (!map_confirmed_) {
            if (!explicit_confirmation) {
                set_status(wardogs::i18n::text(QStringLiteral("Перед игрой выберите и подтвердите текущую карту.")), true);
                return;
            }
            on_terrain_changed();
            if (!map_confirmed_) { terrain_selector_->setFocus(); return; }
        }
        game_mode_ = true;
        try { if (!mouse_listener_.active()) register_mouse_trigger(); }
        catch (const std::exception& error) {
            game_mode_ = false;
            set_status(wardogs::i18n::text(QStringLiteral("Режим игры не включён: ")) + error_text(error), true);
            return;
        }
        game_button_->setText(wardogs::i18n::text(QStringLiteral("Вернуться · ")) + qtext(settings_.exit_game_mode_hotkey));
        if (tray_) tray_->setToolTip(wardogs::i18n::text(QStringLiteral("WARDOGS · режим игры · ")) + qtext(settings_.exit_game_mode_hotkey));
        enter_pinned_mode();
        set_status((settings_.middle_mouse_enabled
            ? (base_set_ ? wardogs::i18n::text(QStringLiteral("Орудие: ")) + qtext(wardogs::format_point(base_)) +
                          wardogs::i18n::text(QStringLiteral("\nСредняя кнопка — цель · возврат "))
                         : wardogs::i18n::text(QStringLiteral("M → ПКМ → Отметить координаты → ")) + qtext(settings_.base_hotkey) + wardogs::i18n::text(QStringLiteral(". Возврат — ")))
            : wardogs::i18n::text(QStringLiteral("Режим игры: захват цели — ")) + qtext(settings_.target_hotkey) + wardogs::i18n::text(QStringLiteral(". Возврат — ")))
            + qtext(settings_.exit_game_mode_hotkey));
        update_readiness();
    }

    void exit_game_mode() {
        game_mode_ = false;
        advance_input_epoch(true);
        if (mouse_timer_) mouse_timer_->stop();
        if (selecting_) selector_.cancel();
        update_game_button();
        if (tray_) tray_->setToolTip(QStringLiteral("WARDOGS Fire Control"));
        if (pinned_mode_) exit_pinned_mode();
        else { showNormal(); raise(); activateWindow(); }
        update_readiness();
        set_status(pending_ocr_
            ? wardogs::i18n::text(QStringLiteral("Сложный захват: проверьте координаты или повторите чтение в игре."))
            : ocr_hold_ ? wardogs::i18n::text(QStringLiteral("Новый захват не завершён. Повторите чтение в игре; прежняя наводка скрыта."))
                        : wardogs::i18n::text(QStringLiteral("Расчёт открыт. Для игры вернитесь в WARDOGS.")), pending_ocr_.has_value() || ocr_hold_);
    }

    QDialog* make_help_dialog(bool notices) {
        auto* dialog = new QDialog(this);
        dialog->setObjectName(notices ? QStringLiteral("noticeDialog") : QStringLiteral("tutorialDialog"));
        dialog->setWindowTitle(notices ? wardogs::i18n::text(QStringLiteral("Лицензии и источники")) : wardogs::i18n::text(QStringLiteral("Как пользоваться WARDOGS Fire Control")));
        dialog->resize(660, 580);
        auto* layout = new QVBoxLayout(dialog);
        layout->setContentsMargins(24, 24, 24, 20);
        auto* browser = new QTextBrowser;
        browser->setOpenExternalLinks(true);
        browser->setObjectName(QStringLiteral("helpText"));
        browser->setStyleSheet(QStringLiteral("QTextBrowser { background:#111b28; border:0; padding:12px; }"));
        const QString content = notices
            ? wardogs::i18n::text(QStringLiteral("<h2>Лицензии и источники</h2><p>Основной код: Rico217 / Ricoz217, MIT.<br>Доработка и интерфейс: SoNiX.</p>"
                "<p>Qt 6: LGPL v3 / GPL v3. ONNX Runtime: MIT. PaddleOCR: Apache 2.0. Zstandard: BSD / GPL v2.</p>"
                "<p>Полные тексты сохранены рядом с приложением в LICENSE, THIRD_PARTY_NOTICES.md, models и licenses.</p>"
                "<p>Рельеф: данные сообщества Apollyon, уведомление TERRAIN_DATA_NOTICE.md. Они не перелицензируются MIT.</p>"
                "<p>Неофициальный инструмент для игры WARDOGS. Не связан с BULKHEAD.</p>"
                "<p><a href='https://github.com/Ricoz217/WarDogs_Distance_Calculator'>Исходный проект</a></p>"))
            : wardogs::i18n::text(QStringLiteral("<h2>Три шага до расчёта</h2>"
                "<p><b>1. Запустите программу и подтвердите карту игры вверху окна.</b> Для стрельбища выберите «Стрельбище · высот нет». L81 выбран по умолчанию; SPH-2 можно выбрать кнопкой орудия. Затем вернитесь в WARDOGS.</p>"
                "<p><b>2. На карте M нажмите ПКМ у своего орудия → Mark Coordinates → %1.</b> Пара в поле чата задаст орудие автоматически. Отправлять текст и выделять область не нужно. Мини-карточка подтвердит координаты.</p>"
                "<p><b>3. Ставьте цели средней кнопкой на карте.</b> Удерживайте курсор неподвижно: после появления подписей программа находит X/Y возле курсора и подтверждает их по двум отдельным кадрам. В чат цель отправлять не нужно. Возврат к основному окну — <b>%2</b>.</p>"
                "<p>При перемещении орудия повторите шаг 2. Если подпись обрезана или распознавание расходится, прежняя наводка скрывается: наведите курсор на цель и повторите среднюю кнопку. Если подписи закрыты, используйте M → ПКМ на цели → Отметить координаты → клавишу захвата цели (по умолчанию Alt+T). Проверка вручную доступна для сложного захвата.</p>"
                "<h3>Ручной ввод и настройки</h3><p>Раскройте «Ручной ввод и диагностика» для координат, вставки текста и выбора собственной области. Форматы: <b>x12.34, y56.78</b> или <b>12.34 56.78</b>; точка (0, 0) разрешена. В настройках «Дополнительно» находятся клавиши, OCR, прицел и отдельный режим калькулятора. Блокировка карточки снимается через <b>%3</b>.</p>"
                "<h3>Точность</h3><p>Одна единица карты равна 100 м. Север — 0°, восток — 90°. MIL берётся из игровых таблиц; вне табличной дальности он не выдаётся. Если пакет высот карты не установлен, рельеф не учтён. Поправка высоты по установленному пакету приближённая.</p>"
                "<h3>SPH-2</h3><p><b>%4</b> выбирает траекторию; выбранная отмечена галочкой. Можно стрелять сразу по указанным азимуту и MIL. Если нужен учёт промаха, наведите курсор на фактическое попадание на карте и нажмите <b>%5</b>. Это необязательно; цель сохраняется. Первый принятый промах уточняет ту же траекторию рядом с целью. По одной точке программа не определяет общий наклон машины.</p>"
                "<p>Запишите попадание до смены цели или траектории. Программа сохраняет предъявленную наводку при чтении и не отслеживает сам выстрел. После новой позиции орудия или смены рельефа поправки сбрасываются. Основное расстояние — до цели; табличный эквивалент наводки отмечен приблизительным значением в подсказке.</p>"
                "<p>Захват работает только для окна WARDOGS на переднем плане. При недоступной мини-карточке в полноэкранном режиме используйте оконный режим без рамки. <a href='https://www.wardogs.com/enforcement'>Правила WARDOGS</a> доступны на сайте игры.</p>"))
                .arg(qtext(settings_.base_hotkey), qtext(settings_.exit_game_mode_hotkey),
                     qtext(settings_.pinned_card.unlock_hotkey), qtext(settings_.ghost_arc_hotkey),
                     qtext(settings_.impact_hotkey));
        wardogs::i18n::bind_html(browser, content);
        layout->addWidget(browser, 1);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
        buttons->button(QDialogButtonBox::Close)->setText(wardogs::i18n::text(QStringLiteral("Понятно")));
        connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
        if (!notices) {
            auto* licenses = buttons->addButton(wardogs::i18n::text(QStringLiteral("Лицензии")), QDialogButtonBox::ActionRole);
            connect(licenses, &QPushButton::clicked, dialog, [this] {
                std::unique_ptr<QDialog> notice(make_help_dialog(true));
                notice->exec();
            });
        }
        layout->addWidget(buttons);
        wardogs::i18n::watch(dialog);
        return dialog;
    }

    void show_tutorial() {
        std::unique_ptr<QDialog> dialog(make_help_dialog(false));
        dialog->exec();
    }

    void build_ui() {
        app_frame_ = new QFrame;
        app_frame_->setObjectName(QStringLiteral("appFrame"));
        app_frame_->setProperty("error", false);
        auto* outer = new QVBoxLayout(app_frame_);
        outer->setContentsMargins(0, 0, 0, 0);
        outer->setSpacing(0);
        outer->addWidget(new WindowTitleBar(this));
        auto* content = new QWidget;
        content->setObjectName(QStringLiteral("mainContent"));
        content->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
        auto* root = new QVBoxLayout(content);
        root->setSizeConstraint(QLayout::SetMinimumSize);
        root->setContentsMargins(24, 20, 24, 20);
        root->setSpacing(17);

        auto* heading = new QHBoxLayout;
        auto* brand = new QVBoxLayout;
        auto* title = new QLabel(QStringLiteral("WARDOGS"));
        title->setObjectName(QStringLiteral("brandTitle"));
        auto* subtitle = new QLabel(wardogs::i18n::text(QStringLiteral("FIRE CONTROL  /  ОГНЕВОЙ РАСЧЁТ")));
        subtitle->setObjectName(QStringLiteral("brandSubtitle"));
        brand->addWidget(title);
        brand->addWidget(subtitle);
        heading->addLayout(brand, 1);
        auto* badge = new QLabel(QStringLiteral("v") + QStringLiteral(WARDOGS_VERSION));
        badge->setObjectName(QStringLiteral("versionBadge"));
        heading->addWidget(badge);
        language_selector_ = new QComboBox;
        language_selector_->setObjectName(QStringLiteral("languageSelector"));
        language_selector_->addItem(QStringLiteral("RU"), static_cast<int>(wardogs::UiLanguage::russian));
        language_selector_->addItem(QStringLiteral("EN"), static_cast<int>(wardogs::UiLanguage::english));
        language_selector_->setCurrentIndex(settings_.language == wardogs::UiLanguage::english ? 1 : 0);
        language_selector_->setFixedWidth(76);
        language_selector_->setToolTip(wardogs::i18n::text(QStringLiteral("Язык интерфейса · сохраняется автоматически")));
        language_selector_->setAccessibleName(wardogs::i18n::text(QStringLiteral("Язык интерфейса")));
        heading->addWidget(language_selector_);
        connect(language_selector_, &QComboBox::currentIndexChanged, this, [this](int index) {
            change_language(index == 1 ? wardogs::UiLanguage::english : wardogs::UiLanguage::russian);
        });
        auto* help_button = new QPushButton(QStringLiteral("?"));
        help_button->setObjectName(QStringLiteral("helpButton"));
        help_button->setFixedSize(36, 36);
        help_button->setToolTip(wardogs::i18n::text(QStringLiteral("Как пользоваться")));
        help_button->setAccessibleName(wardogs::i18n::text(QStringLiteral("Как пользоваться")));
        auto* settings_button = new QPushButton;
        settings_button->setObjectName(QStringLiteral("settingsButton"));
        settings_button->setIcon(ui_icon(UiGlyph::settings));
        settings_button->setFixedSize(36, 36);
        settings_button->setToolTip(wardogs::i18n::text(QStringLiteral("Настройки")));
        settings_button->setAccessibleName(wardogs::i18n::text(QStringLiteral("Настройки")));
        heading->addWidget(help_button);
        heading->addWidget(settings_button);
        auto* update_button = new QPushButton(QStringLiteral("↻"));
        update_button->setObjectName(QStringLiteral("checkUpdatesButton"));
        update_button->setFixedSize(36, 36);
        update_button->setToolTip(wardogs::i18n::text(QStringLiteral("Проверить обновления GitHub")));
        update_button->setAccessibleName(wardogs::i18n::text(QStringLiteral("Проверить обновления GitHub")));
        update_button->setEnabled(!diagnostic_);
        heading->addWidget(update_button);
        root->addLayout(heading);
        if (!diagnostic_) {
            updates_ = new wardogs::updates::Controller(this, QStringLiteral(WARDOGS_VERSION),
                QApplication::applicationDirPath());
            updates_->set_before_install([this] {
                if (busy_.load() || selecting_) {
                    QMessageBox::information(this, wardogs::i18n::text(QStringLiteral("Обновления")),
                        wardogs::i18n::text(QStringLiteral("Завершите захват координат перед установкой обновления.")));
                    return false;
                }
                exit_game_mode();
                return true;
            });
            root->addWidget(updates_->banner_widget());
            connect(update_button, &QPushButton::clicked, updates_, [this] { updates_->check(true); });
        }

        auto* tools = new QHBoxLayout;
        mode_button_ = new QPushButton;
        mode_button_->setObjectName(QStringLiteral("weaponButton"));
        mode_button_->setIconSize(QSize(22, 22));
        mode_button_->setMinimumHeight(30);
        game_button_ = new QPushButton(wardogs::i18n::text(QStringLiteral("Начать игру")));
        game_button_->setObjectName(QStringLiteral("gameButton"));
        game_button_->setProperty("primary", true);
        game_button_->setMinimumHeight(30);
        game_button_->setToolTip(wardogs::i18n::text(QStringLiteral("Скрыть окно и включить захват цели средней кнопкой в WARDOGS")));
        pin_button_ = new QPushButton(wardogs::i18n::text(QStringLiteral("Мини-карточка")));
        pin_button_->setObjectName(QStringLiteral("pinButton"));
        pin_button_->setIcon(pin_icon());
        pin_button_->setMinimumHeight(30);
        ghost_button_ = new QPushButton(wardogs::i18n::text(QStringLiteral("Прицел")));
        ghost_button_->setObjectName(QStringLiteral("ghostButton"));
        ghost_button_->setCheckable(true);
        ghost_button_->setIcon(ui_icon(UiGlyph::reticle));
        ghost_button_->setToolTip(wardogs::i18n::text(QStringLiteral("Шкала наводки поверх игры")));
        tools->addWidget(mode_button_, 1);
        tools->addWidget(game_button_, 1);
        tools->addWidget(pin_button_);
        tools->addWidget(ghost_button_);
        root->addLayout(tools);
        update_mode_button();

        workspace_layout_ = new QBoxLayout(QBoxLayout::LeftToRight);
        workspace_layout_->setSpacing(18);
        auto* work = new QWidget;
        work->setObjectName(QStringLiteral("workPanel"));
        work->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
        auto* work_layout = new QVBoxLayout(work);
        work_layout->setSizeConstraint(QLayout::SetMinimumSize);
        work_layout->setContentsMargins(0, 0, 0, 0);
        work_layout->setSpacing(15);

        auto* quick = new QGroupBox(wardogs::i18n::text(QStringLiteral("БЫСТРАЯ ИГРА")));
        quick->setObjectName(QStringLiteral("quickWorkflow"));
        auto* quick_layout = new QVBoxLayout(quick);
        quick_layout->setContentsMargins(16, 23, 16, 15);
        quick_guide_ = new QLabel;
        quick_guide_->setObjectName(QStringLiteral("quickGuide"));
        quick_guide_->setWordWrap(true);
        quick_state_ = new QLabel;
        quick_state_->setObjectName(QStringLiteral("quickState"));
        quick_state_->setWordWrap(true);
        quick_layout->addWidget(quick_guide_);
        quick_layout->addWidget(quick_state_);
        work_layout->addWidget(quick);

        auto* planning_button = new QPushButton(wardogs::i18n::text(QStringLiteral("Дополнительные инструменты · позиции и полёт")));
        planning_button->setObjectName(QStringLiteral("planningButton"));
        planning_button->setToolTip(wardogs::i18n::text(QStringLiteral("История и именованные точки, перенос цели, измерения времени и профиль рельефа")));
        work_layout->addWidget(planning_button);
        connect(planning_button, &QPushButton::clicked, this, [this] {
            PlanningDialog dialog([this] { return planning_context(); },
                [this](auto kind, auto point, auto map, auto weapon) {
                    apply_planning_point(kind, point, map, weapon);
                }, this);
            dialog.exec();
        });

        manual_toggle_ = new QToolButton;
        manual_toggle_->setObjectName(QStringLiteral("manualControlsToggle"));
        manual_toggle_->setText(wardogs::i18n::text(QStringLiteral("Ручной ввод и диагностика")));
        manual_toggle_->setCheckable(true);
        manual_toggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        manual_toggle_->setArrowType(Qt::RightArrow);
        work_layout->addWidget(manual_toggle_);
        auto* coordinates = new QGroupBox(wardogs::i18n::text(QStringLiteral("01  КООРДИНАТЫ")));
        coordinates->setObjectName(QStringLiteral("coordinatesGroup"));
        coordinates->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
        auto* coordinate_layout = new QVBoxLayout(coordinates);
        coordinate_layout->setContentsMargins(16, 23, 16, 15);
        coordinate_layout->setSpacing(9);
        base_summary_ = new QLabel;
        base_summary_->setObjectName(QStringLiteral("baseSummary"));
        target_summary_ = new QLabel;
        target_summary_->setObjectName(QStringLiteral("targetSummary"));
        base_input_ = new QLineEdit;
        base_input_->setObjectName(QStringLiteral("baseInput"));
        base_input_->setPlaceholderText(wardogs::i18n::text(QStringLiteral("x12.34, y56.78  или  12.34 56.78")));
        base_input_->setAccessibleName(wardogs::i18n::text(QStringLiteral("Координаты орудия")));
        auto* paste_base = new QPushButton;
        paste_base->setProperty("quiet", true);
        paste_base->setText(wardogs::i18n::text(QStringLiteral("Вставить")));
        paste_base->setObjectName(QStringLiteral("pasteBaseButton"));
        paste_base->setToolTip(wardogs::i18n::text(QStringLiteral("Задать орудие из скопированной пары X/Y одним нажатием")));
        connect(paste_base, &QPushButton::clicked, this, [this] { paste_coordinates(true); });
        auto* manual_base = new QPushButton(wardogs::i18n::text(QStringLiteral("Задать")));
        manual_base->setObjectName(QStringLiteral("manualBaseButton"));
        manual_base->setIcon(ui_icon(UiGlyph::location));
        auto* base_row = new QHBoxLayout;
        base_row->addWidget(base_input_, 1);
        base_row->addWidget(paste_base);
        base_row->addWidget(manual_base);
        coordinate_layout->addWidget(base_summary_);
        coordinate_layout->addLayout(base_row);
        target_input_ = new QLineEdit;
        target_input_->setObjectName(QStringLiteral("targetInput"));
        target_input_->setPlaceholderText(wardogs::i18n::text(QStringLiteral("Координаты цели из карты или чата")));
        target_input_->setAccessibleName(wardogs::i18n::text(QStringLiteral("Координаты цели")));
        auto* paste_target = new QPushButton;
        paste_target->setProperty("quiet", true);
        paste_target->setText(wardogs::i18n::text(QStringLiteral("Вставить")));
        paste_target->setObjectName(QStringLiteral("pasteTargetButton"));
        paste_target->setToolTip(wardogs::i18n::text(QStringLiteral("Рассчитать цель из скопированной пары X/Y одним нажатием")));
        connect(paste_target, &QPushButton::clicked, this, [this] { paste_coordinates(false); });
        auto* manual_target = new QPushButton(wardogs::i18n::text(QStringLiteral("Рассчитать")));
        manual_target->setObjectName(QStringLiteral("manualTargetButton"));
        manual_target->setProperty("primary", true);
        manual_target->setIcon(ui_icon(UiGlyph::target));
        auto* target_row = new QHBoxLayout;
        target_row->addWidget(target_input_, 1);
        target_row->addWidget(paste_target);
        target_row->addWidget(manual_target);
        coordinate_layout->addWidget(target_summary_);
        coordinate_layout->addLayout(target_row);
        work_layout->addWidget(coordinates);
        coordinates->hide();

        ocr_review_ = new QGroupBox(wardogs::i18n::text(QStringLiteral("ПРОВЕРЬТЕ КООРДИНАТЫ OCR")));
        ocr_review_->setObjectName(QStringLiteral("ocrReview"));
        auto* review_layout = new QVBoxLayout(ocr_review_);
        ocr_review_reason_ = new QLabel;
        ocr_review_reason_->setObjectName(QStringLiteral("ocrReviewReason"));
        ocr_review_reason_->setWordWrap(true);
        review_layout->addWidget(ocr_review_reason_);
        ocr_candidates_ = new QComboBox;
        ocr_candidates_->setObjectName(QStringLiteral("ocrCandidates"));
        ocr_candidates_->setEditable(true);
        ocr_candidates_->setAccessibleName(wardogs::i18n::text(QStringLiteral("Подтверждаемые координаты OCR")));
        review_layout->addWidget(ocr_candidates_);
        auto* review_buttons = new QHBoxLayout;
        auto* confirm = new QPushButton(wardogs::i18n::text(QStringLiteral("Применить проверенные координаты")));
        confirm->setObjectName(QStringLiteral("confirmOcrButton"));
        confirm->setProperty("primary", true);
        auto* cancel = new QPushButton(wardogs::i18n::text(QStringLiteral("Отклонить")));
        cancel->setObjectName(QStringLiteral("cancelOcrButton"));
        review_buttons->addWidget(confirm);
        review_buttons->addWidget(cancel);
        review_layout->addLayout(review_buttons);
        review_layout->addWidget(new QLabel(wardogs::i18n::text(QStringLiteral("Можно исправить X и Y перед применением."))));
        connect(confirm, &QPushButton::clicked, this, &MainWindow::confirm_ocr);
        connect(cancel, &QPushButton::clicked, this, &MainWindow::cancel_ocr_review);
        work_layout->addWidget(ocr_review_);
        ocr_review_->hide();

        mortar_result_group_ = new QGroupBox(wardogs::i18n::text(QStringLiteral("02  РЕШЕНИЕ ДЛЯ L81")));
        mortar_result_group_->setObjectName(QStringLiteral("mortarResultGroup"));
        mortar_result_group_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
        auto* result_layout = new QVBoxLayout(mortar_result_group_);
        result_layout->setContentsMargins(12, 23, 12, 11);
        auto* cards = new QHBoxLayout;
        cards->setSpacing(8);
        cards->addWidget(result_card(wardogs::i18n::text(QStringLiteral("ДАЛЬНОСТЬ")), QStringLiteral("#f0b45d"), distance_), 1);
        cards->addWidget(result_card(wardogs::i18n::text(QStringLiteral("АЗИМУТ")), QStringLiteral("#63d8c5"), bearing_), 1);
        cards->addWidget(result_card(wardogs::i18n::text(QStringLiteral("НАВОДКА · MIL")), QStringLiteral("#c4b5fd"), mortar_mil_), 1);
        distance_->setObjectName(QStringLiteral("mainDistance"));
        bearing_->setObjectName(QStringLiteral("mainBearing"));
        mortar_mil_->setObjectName(QStringLiteral("mainMil"));
        mortar_mil_->setToolTip(wardogs::i18n::text(QStringLiteral("Игровая шкала наведения L81 из таблицы дальности. MIL — не расстояние в милях.")));
        raw_result_ = new QLabel(wardogs::i18n::text(QStringLiteral("Укажите положение орудия и цель")));
        raw_result_->setObjectName(QStringLiteral("rawResult"));
        raw_result_->setWordWrap(true);
        raw_result_->setAlignment(Qt::AlignCenter);
        result_layout->addLayout(cards);
        result_layout->addWidget(raw_result_);
        work_layout->addWidget(mortar_result_group_);

        vehicle_result_group_ = new QGroupBox(wardogs::i18n::text(QStringLiteral("02  РЕШЕНИЕ ДЛЯ SPH-2")));
        vehicle_result_group_->setObjectName(QStringLiteral("vehicleResultGroup"));
        vehicle_result_group_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
        auto* vehicle_results = new QVBoxLayout(vehicle_result_group_);
        vehicle_results->setContentsMargins(12, 23, 12, 12);
        low_solution_ = new VehicleSolutionWidget(wardogs::Arc::low);
        high_solution_ = new VehicleSolutionWidget(wardogs::Arc::high);
        vehicle_results->addWidget(low_solution_);
        vehicle_results->addWidget(high_solution_);
        vehicle_note_ = new QLabel(wardogs::i18n::text(QStringLiteral("Табличный расчёт · рельеф не учтён. Поправка по попаданию необязательна.")));
        vehicle_note_->setObjectName(QStringLiteral("rawResult"));
        vehicle_note_->setWordWrap(true);
        vehicle_results->addWidget(vehicle_note_);
        vehicle_result_group_->hide();
        work_layout->addWidget(vehicle_result_group_);
        copy_button_ = new QPushButton(wardogs::i18n::text(QStringLiteral("Скопировать расчёт")));
        copy_button_->setObjectName(QStringLiteral("copySolutionButton"));
        copy_button_->setProperty("quiet", true);
        copy_button_->setEnabled(false);
        work_layout->addWidget(copy_button_);

        auto* ocr = new QGroupBox(wardogs::i18n::text(QStringLiteral("03  КООРДИНАТЫ С ЭКРАНА")));
        ocr->setObjectName(QStringLiteral("ocrGroup"));
        ocr->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
        auto* ocr_layout = new QVBoxLayout(ocr);
        ocr_layout->setContentsMargins(16, 23, 16, 13);
        ocr_layout->setSpacing(9);
        engine_summary_ = new QLabel;
        engine_summary_->setObjectName(QStringLiteral("muted"));
        region_summary_ = new QLabel;
        region_summary_->setObjectName(QStringLiteral("regionSummary"));
        region_summary_->setWordWrap(true);
        ocr_layout->addWidget(engine_summary_);
        ocr_layout->addWidget(region_summary_);
        auto* coordinate_hint = new QLabel(wardogs::i18n::text(QStringLiteral(
            "M → правая кнопка → Mark Coordinates.\n"
            "Пара X/Y появится в поле чата. Можно читать её до отправки.")));
        coordinate_hint->setObjectName(QStringLiteral("coordinateHint"));
        coordinate_hint->setWordWrap(true);
        ocr_layout->addWidget(coordinate_hint);
        auto* actions = new QGridLayout;
        region_button_ = new QPushButton;
        base_button_ = new QPushButton;
        target_button_ = new QPushButton;
        quick_target_button_ = new QPushButton;
        region_button_->setObjectName(QStringLiteral("regionButton"));
        base_button_->setObjectName(QStringLiteral("captureBaseButton"));
        target_button_->setObjectName(QStringLiteral("captureTargetButton"));
        quick_target_button_->setObjectName(QStringLiteral("quickTargetButton"));
        region_button_->setIcon(ui_icon(UiGlyph::scan));
        base_button_->setIcon(ui_icon(UiGlyph::location));
        target_button_->setIcon(ui_icon(UiGlyph::target));
        quick_target_button_->setIcon(ui_icon(UiGlyph::scan));
        actions->addWidget(region_button_, 0, 0);
        actions->addWidget(base_button_, 0, 1);
        actions->addWidget(target_button_, 1, 0);
        actions->addWidget(quick_target_button_, 1, 1);
        ocr_layout->addLayout(actions);
        ocr_text_ = new QLabel(wardogs::i18n::text(QStringLiteral("Текст: —")));
        ocr_text_->setObjectName(QStringLiteral("ocrText"));
        ocr_text_->setWordWrap(true);
        ocr_text_->setTextFormat(Qt::PlainText);
        ocr_layout->addWidget(ocr_text_);
        work_layout->addWidget(ocr);
        ocr->hide();
        connect(manual_toggle_, &QToolButton::toggled, this, [coordinates, ocr, this](bool expanded) {
            coordinates->setVisible(expanded);
            ocr->setVisible(expanded);
            manual_toggle_->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
        });

        terrain_group_ = new QGroupBox(wardogs::i18n::text(QStringLiteral("КАРТА ИГРЫ · ОБЯЗАТЕЛЬНО")));
        terrain_group_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
        auto* terrain_layout = new QVBoxLayout(terrain_group_);
        terrain_layout->setContentsMargins(16, 23, 16, 13);
        terrain_selector_ = new QComboBox;
        terrain_selector_->setObjectName(QStringLiteral("gameMapSelector"));
        for (const auto map : {wardogs::GameMap::unselected, wardogs::GameMap::bakurani,
                               wardogs::GameMap::ozeti, wardogs::GameMap::zestafona,
                               wardogs::GameMap::training, wardogs::GameMap::other})
            terrain_selector_->addItem(game_map_name(map), static_cast<int>(map));
        terrain_selector_->setCurrentIndex(terrain_selector_->findData(static_cast<int>(settings_.last_game_map)));
        terrain_selector_->setToolTip(wardogs::i18n::text(QStringLiteral("Выберите текущую карту. После запуска подтвердите прошлый выбор; программа не определяет карту игры автоматически.")));
        auto* map_row = new QHBoxLayout;
        map_row->addWidget(terrain_selector_, 1);
        confirm_map_ = new QPushButton(wardogs::i18n::text(QStringLiteral("Подтвердить карту")));
        confirm_map_->setObjectName(QStringLiteral("confirmGameMap"));
        map_row->addWidget(confirm_map_);
        import_terrain_ = new QPushButton(wardogs::i18n::text(QStringLiteral("Подключить локальные данные высот…")));
        import_terrain_->setObjectName(QStringLiteral("importTerrain"));
        import_terrain_->setProperty("quiet", true);
        import_terrain_->setToolTip(wardogs::i18n::text(QStringLiteral("Проверить и установить ранее полученные пакеты bakurani.wdt, ozeti.wdt и zestafona.wdt. Обновление программы сохраняет эти данные.")));
        terrain_summary_ = new QLabel;
        terrain_summary_->setObjectName(QStringLiteral("muted"));
        terrain_summary_->setWordWrap(true);
        terrain_layout->addLayout(map_row);
        terrain_layout->addWidget(terrain_summary_);
        terrain_layout->addWidget(import_terrain_);
        work_layout->insertWidget(0, terrain_group_);

        calibration_toggle_ = new QToolButton;
        calibration_toggle_->setObjectName(QStringLiteral("calibrationToggle"));
        calibration_toggle_->setText(wardogs::i18n::text(QStringLiteral("Поправки по попаданию")));
        calibration_toggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        calibration_toggle_->setArrowType(Qt::RightArrow);
        calibration_toggle_->setCheckable(true);
        calibration_toggle_->hide();
        calibration_group_ = build_calibration_group();
        calibration_group_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
        calibration_group_->hide();
        work_layout->addWidget(calibration_toggle_);
        work_layout->addWidget(calibration_group_);
        auto* fire_control = new QGroupBox(wardogs::i18n::text(QStringLiteral("ПРИСТРЕЛКА")));
        fire_control->setObjectName(QStringLiteral("fireControlGroup"));
        auto* fire_layout = new QVBoxLayout(fire_control);
        fire_control_summary_ = new QLabel;
        fire_control_summary_->setObjectName(QStringLiteral("fireControlSummary"));
        fire_control_summary_->setWordWrap(true);
        fire_layout->addWidget(fire_control_summary_);
        terrain_assistance_ = new QLabel;
        terrain_assistance_->setObjectName(QStringLiteral("terrainAssistance"));
        terrain_assistance_->setWordWrap(true);
        fire_layout->addWidget(terrain_assistance_);
        fire_control_reset_ = new QPushButton(wardogs::i18n::text(QStringLiteral("Сбросить поправки")));
        fire_control_reset_->setObjectName(QStringLiteral("resetFireControl"));
        fire_control_reset_->setProperty("quiet", true);
        connect(fire_control_reset_, &QPushButton::clicked, this, &MainWindow::clear_continuous_calibration);
        fire_layout->addWidget(fire_control_reset_);
        work_layout->insertWidget(work_layout->indexOf(planning_button), fire_control);
        work_layout->insertWidget(work_layout->indexOf(fire_control), vehicle_result_group_);
        fire_control->hide();
        work_layout->addStretch();

        side_panel_ = new QWidget;
        side_panel_->setObjectName(QStringLiteral("sidePanel"));
        side_panel_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
        side_panel_->setMaximumWidth(290);
        auto* side = new QVBoxLayout(side_panel_);
        side->setSizeConstraint(QLayout::SetMinimumSize);
        side->setContentsMargins(0, 0, 0, 0);
        side->setSpacing(15);
        auto* readiness_group = new QGroupBox(wardogs::i18n::text(QStringLiteral("ПЕРЕД ВЫСТРЕЛОМ")));
        readiness_group->setObjectName(QStringLiteral("readinessGroup"));
        readiness_group->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
        auto* readiness_layout = new QVBoxLayout(readiness_group);
        readiness_layout->setContentsMargins(16, 23, 16, 15);
        readiness_ = new QLabel;
        readiness_->setObjectName(QStringLiteral("readiness"));
        readiness_->setWordWrap(true);
        readiness_layout->addWidget(readiness_);
        side->addWidget(readiness_group);
        direction_plot_ = new DirectionPlot;
        side->addWidget(direction_plot_);
        auto* history_group = new QGroupBox(wardogs::i18n::text(QStringLiteral("НЕДАВНИЕ ЦЕЛИ")));
        history_group->setObjectName(QStringLiteral("historyGroup"));
        history_group->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
        auto* history_layout = new QVBoxLayout(history_group);
        history_layout->setContentsMargins(16, 23, 16, 15);
        history_selector_ = new QComboBox;
        history_selector_->setObjectName(QStringLiteral("targetHistory"));
        history_selector_->setPlaceholderText(wardogs::i18n::text(QStringLiteral("Пока нет целей")));
        history_selector_->setEnabled(false);
        history_layout->addWidget(history_selector_);
        auto* history_note = new QLabel(wardogs::i18n::text(QStringLiteral("До 12 целей в этом сеансе.\nВыберите цель для нового расчёта.")));
        history_note->setObjectName(QStringLiteral("muted"));
        history_note->setWordWrap(true);
        history_layout->addWidget(history_note);
        side->addWidget(history_group);
        auto* tip = new QLabel;
        tip->setObjectName(QStringLiteral("gameTip"));
        tip->setWordWrap(true);
        side->addWidget(tip);
        side->addStretch();
        workspace_layout_->addWidget(work, 1);
        workspace_layout_->addWidget(side_panel_);
        root->addLayout(workspace_layout_);
        status_ = new QLabel(wardogs::i18n::text(QStringLiteral("Готово. Задайте орудие по подсказке выше — дополнительных настроек не требуется.")));
        status_->setObjectName(QStringLiteral("status"));
        status_->setWordWrap(true);
        status_->setTextFormat(Qt::PlainText);
        QSizePolicy status_policy(QSizePolicy::Expanding, QSizePolicy::Minimum);
        status_policy.setHeightForWidth(true);
        status_->setSizePolicy(status_policy);
        auto* scroll = new QScrollArea;
        scroll->setObjectName(QStringLiteral("mainContentScroll"));
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->viewport()->setObjectName(QStringLiteral("mainContentViewport"));
        scroll->setWidget(content);
        outer->addWidget(scroll, 1);
        auto* footer = new QWidget;
        auto* footer_layout = new QVBoxLayout(footer);
        footer_layout->setContentsMargins(24, 8, 24, 12);
        footer_layout->addWidget(status_);
        outer->addWidget(footer);
        setCentralWidget(app_frame_);
        update_readiness();
        update_terrain_summary();

        connect(manual_base, &QPushButton::clicked, this, [this] { this->manual_base(); });
        connect(base_input_, &QLineEdit::returnPressed, this, [this] { this->manual_base(); });
        connect(manual_target, &QPushButton::clicked, this, [this] { this->manual_target(); });
        connect(target_input_, &QLineEdit::returnPressed, this, [this] { this->manual_target(); });
        connect(mode_button_, &QPushButton::clicked, this, &MainWindow::toggle_mode);
        connect(ghost_button_, &QPushButton::clicked, this, [this](bool enabled) { set_ghost_enabled(enabled); });
        connect(pin_button_, &QPushButton::clicked, this, &MainWindow::enter_pinned_mode);
        connect(game_button_, &QPushButton::clicked, this, [this] {
            if (game_mode_) exit_game_mode();
            else if (!diagnostic_ && !settings_.game_integration_enabled) edit_settings();
            else enter_game_mode(true);
        });
        connect(terrain_selector_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) { on_terrain_changed(); });
        connect(confirm_map_, &QPushButton::clicked, this, [this] { on_terrain_changed(); });
        connect(import_terrain_, &QPushButton::clicked, this, [this] { import_local_terrain(); });
        connect(region_button_, &QPushButton::clicked, this, [this] { begin_region_setup(); });
        connect(base_button_, &QPushButton::clicked, this, [this] { start_ocr(OcrAction::base); });
        connect(target_button_, &QPushButton::clicked, this, [this] { start_ocr(OcrAction::target); });
        connect(quick_target_button_, &QPushButton::clicked, this, &MainWindow::begin_quick_target);
        connect(settings_button, &QPushButton::clicked, this, &MainWindow::edit_settings);
        connect(help_button, &QPushButton::clicked, this, [this] { show_tutorial(); });
        connect(copy_button_, &QPushButton::clicked, this, [this] { copy_solution(); });
        connect(history_selector_, qOverload<int>(&QComboBox::activated), this, [this](int index) {
            recall_target(index);
        });
        connect(calibration_toggle_, &QToolButton::toggled, this, [this](bool expanded) {
            calibration_group_->setVisible(expanded && vehicle_mode_);
            calibration_toggle_->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
        });
    }

    QGroupBox* build_calibration_group() {
        auto* group = new QGroupBox;
        auto* layout = new QGridLayout(group);
        layout->setContentsMargins(14, 14, 14, 14);
        layout->setHorizontalSpacing(8);
        layout->setVerticalSpacing(7);
        layout->addWidget(new QLabel, 0, 0);
        layout->addWidget(new QLabel(wardogs::i18n::text(QStringLiteral("Текущая цель"))), 0, 1);
        layout->addWidget(new QLabel(wardogs::i18n::text(QStringLiteral("Фактическое попадание"))), 0, 2);
        layout->addWidget(new QLabel(wardogs::i18n::text(QStringLiteral("Траектория"))), 0, 3);
        continuous_aim_ = new QLineEdit;
        continuous_aim_->setObjectName(QStringLiteral("continuousAim"));
        continuous_aim_->setReadOnly(true);
        continuous_aim_->setPlaceholderText(wardogs::i18n::text(QStringLiteral("Текущая цель")));
        continuous_impact_ = new QLineEdit;
        continuous_impact_->setObjectName(QStringLiteral("continuousImpact"));
        continuous_impact_->setPlaceholderText(wardogs::i18n::text(QStringLiteral("OCR или вручную")));
        continuous_arc_ = make_arc_button();
        continuous_arc_->setToolTip(wardogs::i18n::text(QStringLiteral("Траектория для поправок")));
        layout->addWidget(new QLabel(wardogs::i18n::text(QStringLiteral("Поправка"))), 1, 0);
        layout->addWidget(continuous_aim_, 1, 1);
        layout->addWidget(continuous_impact_, 1, 2);
        layout->addWidget(continuous_arc_, 1, 3);

        calibration_ocr_ = new QPushButton(wardogs::i18n::text(QStringLiteral("OCR попадания")));
        calibration_ocr_->setIcon(ui_icon(UiGlyph::scan));
        calibration_ocr_->setProperty("quiet", true);
        calibration_manual_ = new QPushButton(wardogs::i18n::text(QStringLiteral("Записать попадание")));
        calibration_manual_->setIcon(ui_icon(UiGlyph::location));
        calibration_manual_->setProperty("quiet", true);
        clear_continuous_ = new QPushButton(wardogs::i18n::text(QStringLiteral("Сбросить поправки")));
        clear_continuous_->setObjectName(QStringLiteral("clearContinuousCalibration"));
        clear_continuous_->setIcon(ui_icon(UiGlyph::clear));
        clear_continuous_->setProperty("quiet", true);
        layout->addWidget(calibration_ocr_, 2, 0, 1, 2);
        layout->addWidget(calibration_manual_, 2, 2, 1, 2);
        layout->addWidget(clear_continuous_, 3, 0, 1, 4);
        calibration_summary_ = new QLabel(
            wardogs::i18n::text(QStringLiteral("Можно стрелять сразу. Если нужно, запишите промах по текущей цели: он уточнит только эту траекторию рядом с целью.")));
        calibration_summary_->setObjectName(QStringLiteral("muted"));
        calibration_summary_->setWordWrap(true);
        layout->addWidget(calibration_summary_, 4, 0, 1, 4);
        connect(calibration_ocr_, &QPushButton::clicked, this,
                [this] { start_ocr(OcrAction::calibration_impact); });
        connect(calibration_manual_, &QPushButton::clicked, this,
                &MainWindow::record_manual_continuous_impact);
        connect(continuous_impact_, &QLineEdit::returnPressed, this,
                &MainWindow::record_manual_continuous_impact);
        connect(clear_continuous_, &QPushButton::clicked, this,
                &MainWindow::clear_continuous_calibration);
        update_continuous_controls();
        return group;
    }

    QPushButton* make_arc_button() {
        auto* button = new QPushButton;
        button->setObjectName(QStringLiteral("arcToggle"));
        button->setToolTip(wardogs::i18n::text(QStringLiteral("Переключить настильную / навесную траекторию")));
        set_arc_button(button, wardogs::Arc::low);
        connect(button, &QPushButton::clicked, this, [this, button] {
            set_arc_button(button, arc_from_button(button) == wardogs::Arc::low
                                       ? wardogs::Arc::high
                                       : wardogs::Arc::low);
            advance_input_epoch();
            ++calibration_epoch_;
            continuous_impact_->clear();
            if (button == continuous_arc_) {
                settings_.ghost_reticle.preferred_arc = arc_from_button(button);
                sync_ghost_solution();
                sync_pinned_result();
            }
        });
        return button;
    }

    static wardogs::Arc arc_from_button(const QPushButton* button) {
        return button->property("trajectory").toString() == QStringLiteral("high")
                   ? wardogs::Arc::high
                   : wardogs::Arc::low;
    }

    static void set_arc_button(QPushButton* button, wardogs::Arc arc) {
        const bool high = arc == wardogs::Arc::high;
        button->setText(high ? wardogs::i18n::text(QStringLiteral("Навесная")) : wardogs::i18n::text(QStringLiteral("Настильная")));
        button->setProperty("trajectory", high ? QStringLiteral("high")
                                                : QStringLiteral("low"));
        button->setProperty("highlighted", high);
        button->style()->unpolish(button);
        button->style()->polish(button);
        button->update();
    }

    void update_mode_button() {
        mode_button_->setIcon(weapon_mode_icon(vehicle_mode_));
        mode_button_->setText(vehicle_mode_ ? wardogs::i18n::text(QStringLiteral("SPH-2 · Артиллерия")) : wardogs::i18n::text(QStringLiteral("L81 · Миномёт")));
        mode_button_->setToolTip(vehicle_mode_
            ? wardogs::i18n::text(QStringLiteral("Переключить на миномёт L81"))
            : wardogs::i18n::text(QStringLiteral("Переключить на SPH-2")));
        mode_button_->setAccessibleName(vehicle_mode_
            ? wardogs::i18n::text(QStringLiteral("SPH-2: переключить на L81"))
            : wardogs::i18n::text(QStringLiteral("L81: переключить на SPH-2")));
    }

    void toggle_mode() {
        advance_input_epoch();
        vehicle_mode_ = !vehicle_mode_;
        wardogs::log_info(std::string("weapon.selected weapon=") + (vehicle_mode_ ? "sph2" : "l81"));
        update_mode_button();
        update_action_labels();
        terrain_group_->show();
        calibration_toggle_->setVisible(vehicle_mode_);
        calibration_group_->setVisible(vehicle_mode_ && calibration_toggle_->isChecked());
        findChild<QGroupBox*>(QStringLiteral("fireControlGroup"))->setVisible(vehicle_mode_);
        vehicle_result_group_->setVisible(vehicle_mode_);
        mortar_result_group_->setVisible(!vehicle_mode_);
        if (target_) {
            const bool warning = show_result(*target_);
            if (!warning)
                set_status(vehicle_mode_
                    ? sph2_workflow_hint()
                    : wardogs::i18n::text(QStringLiteral("L81: дальность, азимут и табличная наводка для 132–684 м.")));
        } else {
            set_status(vehicle_mode_
                ? wardogs::i18n::text(QStringLiteral("SPH-2: задайте орудие и цель. Поправка по попаданию необязательна."))
                : wardogs::i18n::text(QStringLiteral("L81: дальность, азимут и табличная наводка для 132–684 м.")));
        }
        if (ocr_hold_) set_status(wardogs::i18n::text(QStringLiteral("Проверка OCR отменена при смене орудия. Наводка скрыта · введите или захватите цель заново.")), true);
        sync_pinned_result();
        sync_ghost_solution();
        fit_window_to_content();
        QTimer::singleShot(0, this, [this] { fit_window_to_content(); });
    }

    void fit_window_to_content() {
        const QRect available = screen() ? screen()->availableGeometry() : QRect(0, 0, 1280, 900);
        const int max_width = std::max(320, available.width() - 32);
        const int max_height = std::max(240, available.height() - 48);
        setMinimumSize(std::min(640, max_width), std::min(420, max_height));
        resize(std::min(1040, max_width), std::min(790, max_height));
    }

    void change_language(wardogs::UiLanguage value) {
        if (value == settings_.language) return;
        wardogs::i18n::watch(this);
        settings_.language = value;
        wardogs::i18n::set_language(value);
        update_coordinates();
        update_action_labels();
        update_engine_summary();
        update_region_summary();
        if (tray_) tray_->setToolTip(game_mode_
            ? wardogs::i18n::text(QStringLiteral("WARDOGS · режим игры · ")) + qtext(settings_.exit_game_mode_hotkey)
            : QStringLiteral("WARDOGS Fire Control"));
        if (!diagnostic_) {
            try { wardogs::save_settings(settings_); }
            catch (const std::exception& error) {
                set_status(wardogs::i18n::text(QStringLiteral("Язык изменён для этого сеанса. Сохранить не удалось: ")) + error_text(error), true);
            }
            wardogs_ui::install_unlock_jump_list_task();
        }
        updateGeometry();
    }

    static QString game_map_name(wardogs::GameMap map) {
        switch (map) {
        case wardogs::GameMap::unselected: return wardogs::i18n::text(QStringLiteral("Выберите текущую карту…"));
        case wardogs::GameMap::bakurani: return wardogs::i18n::text(QStringLiteral("Бакурани / Bakurani"));
        case wardogs::GameMap::ozeti: return wardogs::i18n::text(QStringLiteral("Озети / Ozeti"));
        case wardogs::GameMap::zestafona: return wardogs::i18n::text(QStringLiteral("Зестафона / Zestafona"));
        case wardogs::GameMap::training: return wardogs::i18n::text(QStringLiteral("Стрельбище · высот нет"));
        case wardogs::GameMap::other: return wardogs::i18n::text(QStringLiteral("Другая карта · высот нет"));
        }
        return wardogs::i18n::text(QStringLiteral("Выберите текущую карту…"));
    }

    wardogs::GameMap selected_game_map() const {
        return static_cast<wardogs::GameMap>(terrain_selector_->currentData().toInt());
    }

    void update_terrain_summary(std::optional<double> height_delta = std::nullopt) {
        confirm_map_->setEnabled(selected_game_map() != wardogs::GameMap::unselected);
        confirm_map_->setVisible(!map_confirmed_);
        import_terrain_->setVisible(terrain_discovery_.installed.size() < wardogs::official_terrain_maps().size());
        if (!map_confirmed_) {
            terrain_summary_->setText(selected_game_map() == wardogs::GameMap::unselected
                ? wardogs::i18n::text(QStringLiteral("Перед расчётом выберите карту, на которой играете."))
                : wardogs::i18n::text(QStringLiteral("Подтвердите текущую карту перед расчётом. Прошлый выбор не определяет новый матч.")));
            return;
        }
        if (!terrain_map_) {
            terrain_summary_->setText(game_map_name(current_game_map_) +
                wardogs::i18n::text(QStringLiteral(". Рельеф не учтён: высоты орудия и цели считаются равными.")));
            return;
        }
        auto text = game_map_name(current_game_map_) +
                    (vehicle_mode_ ? wardogs::i18n::text(QStringLiteral(" · высоты подключены для SPH-2"))
                                   : wardogs::i18n::text(QStringLiteral(" · L81 использует прежнюю таблицу без поправки высоты")));
        if (vehicle_mode_ && base_set_ && height_delta) {
            const auto base_height = terrain_->height_at(base_);
            if (base_height) text += wardogs::i18n::text(QStringLiteral("\nВысота по карте · орудие: %1 м")).arg(*base_height, 0, 'f', 1);
            if (target_) {
                const auto target_height = terrain_->height_at(*target_);
                if (target_height) text += wardogs::i18n::text(QStringLiteral(" · цель: %1 м")).arg(*target_height, 0, 'f', 1);
            }
        }
        if (height_delta) {
            const auto value = QString::number(*height_delta, 'f', 1);
            text += wardogs::i18n::text(QStringLiteral(" · перепад высоты цели ")) +
                    (*height_delta >= 0.0 ? QStringLiteral("+") : QString{}) +
                    value + QStringLiteral(" m");
        }
        terrain_summary_->setText(text);
    }

    std::optional<double> terrain_height(wardogs::Point point) {
        if (!map_confirmed_) throw std::invalid_argument("Сначала выберите и подтвердите текущую карту");
        if (!terrain_) {
            if (wardogs::game_map_has_terrain(current_game_map_))
                throw std::invalid_argument("Для выбранной карты не загружены проверенные данные высот");
            return 0.0;
        }
        return terrain_->height_at(point);
    }

    double target_height_delta(wardogs::Point target) {
        const auto base_height = terrain_height(base_);
        const auto target_height = terrain_height(target);
        if (!base_height)
            throw std::invalid_argument("Орудие находится за пределами высотной карты");
        if (!target_height)
            throw std::invalid_argument("Цель находится за пределами высотной карты");
        return *target_height - *base_height;
    }

    void on_terrain_changed() {
        automatic_analysis_.reset();
        const bool incomplete_base = base_capture_pending_;
        const bool incomplete_target = ocr_hold_ || pending_ocr_.has_value();
        advance_input_epoch();
        ++calibration_epoch_;
        const auto selected = selected_game_map();
        const bool different_map = current_game_map_ != wardogs::GameMap::unselected && selected != current_game_map_;
        const bool had_data = has_calibration_data();
        map_confirmed_ = false;
        terrain_.reset();
        terrain_map_.reset();
        current_game_map_ = selected;
        wardogs::log_info("terrain.selection_requested map=" +
            utf8(qtext(std::wstring{wardogs::game_map_key(selected)})) + " confirmed=0");
        invalidate_corrections();
        ocr_hold_ = false;
        base_capture_pending_ = false;
        if (different_map || incomplete_base) {
            base_set_ = false;
            target_.reset();
            base_input_->clear();
            target_input_->clear();
            history_.clear();
            history_selector_->clear();
            history_selector_->setEnabled(false);
            update_coordinates();
        } else if (incomplete_target) {
            target_.reset();
            target_input_->clear();
            update_coordinates();
        }
        clear_result(wardogs::i18n::text(QStringLiteral("Выберите и подтвердите текущую карту")));
        if (selected == wardogs::GameMap::unselected) {
            update_readiness();
            set_status(wardogs::i18n::text(QStringLiteral("Перед расчётом выберите текущую карту.")));
            return;
        }
        if (wardogs::game_map_has_terrain(selected)) {
            try {
                const auto key = wardogs::game_map_key(selected);
                const auto& specs = wardogs::official_terrain_maps();
                const auto spec = std::find_if(specs.begin(), specs.end(), [&](const auto& entry) {
                    return QString::fromStdString(entry.map_id).toStdWString() == key;
                });
                if (spec == specs.end()) throw std::invalid_argument("Для выбранной карты нет описания высот");
                const auto discovery = wardogs::discover_available_terrain_maps(
                    wardogs::default_terrain_directory(), wardogs::user_terrain_directory(), {*spec});
                if (discovery.installed.empty()) {
                    QStringList errors;
                    for (const auto& problem : discovery.problems) errors.push_back(wardogs::i18n::text(qtext(problem)));
                    throw std::invalid_argument(utf8(errors.join(QStringLiteral("; "))));
                }
                const auto& installed = discovery.installed.front();
                terrain_ = std::make_unique<wardogs::TerrainPackage>(installed.path);
                terrain_map_ = installed;
            } catch (const std::exception& error) {
                update_terrain_summary();
                terrain_summary_->setText(wardogs::i18n::text(QStringLiteral("Высоты выбранной карты недоступны. Подключите проверенные локальные данные; расчёт заблокирован.")));
                import_terrain_->show();
                update_readiness();
                set_status(wardogs::i18n::text(QStringLiteral("Карта не подтверждена: ")) + error_text(error), true);
                return;
            }
        }
        map_confirmed_ = true;
        settings_.last_game_map = selected;
        try { if (!diagnostic_) wardogs::save_settings(settings_); }
        catch (const std::exception& error) {
            wardogs::log_warning("settings.game_map_save_failed error=" + std::string(error.what()));
        }
        std::ostringstream diagnostic;
        diagnostic << "terrain.selected map=" << utf8(qtext(std::wstring{wardogs::game_map_key(selected)}))
                   << " heights=" << static_cast<bool>(terrain_) << " coordinates_cleared=" << different_map;
        wardogs::log_info(diagnostic.str());
        if (had_data)
            calibration_summary_->setText(
                wardogs::i18n::text(QStringLiteral("Рельеф изменён. Поправки по прежним попаданиям сброшены.")));
        update_terrain_summary();
        update_readiness();
        if (target_ && show_result(*target_)) return;
        if (terrain_map_)
            set_status(wardogs::i18n::text(QStringLiteral("Загружен рельеф ")) +
                       game_map_name(selected) +
                       (different_map ? wardogs::i18n::text(QStringLiteral(" · заново задайте орудие и цель"))
                                      : wardogs::i18n::text(QStringLiteral(" · SPH-2 учитывает перепад высоты"))));
        else
            set_status(game_map_name(selected) + wardogs::i18n::text(QStringLiteral(" · рельеф не учтён")) +
                       (had_data ? wardogs::i18n::text(QStringLiteral(" · прежние поправки сброшены"))
                                 : QString{}));
    }

    std::unique_ptr<QFileDialog> make_terrain_folder_dialog() {
        auto dialog = std::make_unique<QFileDialog>(this,
            wardogs::i18n::text(QStringLiteral("Папка ранее полученных карт высот")));
        dialog->setOption(QFileDialog::DontUseNativeDialog);
        dialog->setOption(QFileDialog::ShowDirsOnly);
        dialog->setFileMode(QFileDialog::Directory);
        dialog->resize(780, 520);
        // Translated metadata headings must fit at every DPI; file names stay
        // resizable and retain their ordinary file-dialog elision behavior.
        for (auto* tree : dialog->findChildren<QTreeView*>()) {
            tree->header()->setStretchLastSection(false);
            for (int column = 1; column < tree->header()->count(); ++column)
                tree->header()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
        }
        return dialog;
    }

    void import_local_terrain() {
        if (busy_ || selecting_) { set_status(wardogs::i18n::text(QStringLiteral("Дождитесь окончания чтения координат."))); return; }
        const auto dialog = make_terrain_folder_dialog();
        if (dialog->exec() != QDialog::Accepted || dialog->selectedFiles().isEmpty()) return;
        const auto folder = dialog->selectedFiles().front();
        try {
            const auto result = wardogs::install_terrain_maps(std::filesystem::path{folder.toStdWString()});
            terrain_discovery_ = wardogs::discover_available_terrain_maps();
            // Unrelated imports and failed folders must not erase a useful
            // local correction. Reload only an unavailable selected map.
            if (!map_confirmed_ && wardogs::game_map_has_terrain(selected_game_map()) &&
                std::any_of(terrain_discovery_.installed.begin(), terrain_discovery_.installed.end(), [&](const auto& entry) {
                    return QString::fromStdString(entry.spec.map_id).toStdWString() == wardogs::game_map_key(selected_game_map());
                })) on_terrain_changed();
            else update_terrain_summary();
            if (!result.problems.empty()) {
                QStringList errors;
                for (const auto& problem : result.problems) errors.push_back(wardogs::i18n::text(qtext(problem)));
                set_status(wardogs::i18n::text(QStringLiteral("Подключены карты: %1. %2")).arg(result.installed.size()).arg(errors.join(QStringLiteral("; "))), true);
            } else set_status(wardogs::i18n::text(QStringLiteral("Локальные высоты подключены · карт: %1.")).arg(result.installed.size()));
        } catch (const std::exception& error) {
            set_status(wardogs::i18n::text(QStringLiteral("Не удалось подключить высоты: ")) + error_text(error), true);
        }
    }

    bool has_calibration_data() const {
        return continuous_calibration_ && continuous_calibration_->sample_count() > 0;
    }

    void update_continuous_controls() {
        continuous_aim_->setEnabled(true);
        continuous_impact_->setEnabled(true);
        continuous_arc_->setEnabled(true);
        clear_continuous_->setEnabled(true);
    }

    void update_calibration_summary() {
        if (!calibration_summary_) return;
        calibration_summary_->setText(has_calibration_data()
            ? wardogs::i18n::text(QStringLiteral("Учтено попаданий: %1/%2. Поправка действует только рядом с записанной целью и для той же траектории."))
                .arg(continuous_calibration_->sample_count()).arg(wardogs::maximum_continuous_observations)
            : wardogs::i18n::text(QStringLiteral("Можно стрелять сразу. Поправка необязательна: после выстрела по показанным азимуту и MIL наведите курсор на фактическое попадание и нажмите %1 до смены цели или траектории."))
                .arg(qtext(settings_.impact_hotkey)));
    }

    std::optional<wardogs::Arc> effective_vehicle_arc() const {
        if (!vehicle_mode_) return std::nullopt;
        return wardogs::effective_ghost_arc(low_result_.has_value(), high_result_.has_value(),
                                            settings_.ghost_reticle.preferred_arc);
    }

    QString sph2_workflow_hint() const {
        if (!map_confirmed_) return wardogs::i18n::text(QStringLiteral("Сначала выберите и подтвердите текущую карту"));
        const auto arc = effective_vehicle_arc();
        if (!arc) return wardogs::i18n::text(QStringLiteral("Наводка недоступна · выберите цель в диапазоне SPH-2"));
        auto text = *arc == wardogs::Arc::low ? wardogs::i18n::text(QStringLiteral("Настильная")) : wardogs::i18n::text(QStringLiteral("Навесная"));
        text += QStringLiteral(" · ") + game_map_name(current_game_map_);
        text += terrain_map_ ? wardogs::i18n::text(QStringLiteral(" · рельеф учтён")) : wardogs::i18n::text(QStringLiteral(" · рельеф не учтён"));
        text += wardogs::i18n::text(QStringLiteral(" · смена %1")).arg(qtext(settings_.ghost_arc_hotkey));
        text += wardogs::i18n::text(QStringLiteral("\n%1 у попадания — необязательная поправка."))
            .arg(qtext(settings_.impact_hotkey));
        return text;
    }

    void capture_impact_context(OcrMessage& context) {
        const auto arc = effective_vehicle_arc();
        if (!map_confirmed_ || !vehicle_mode_ || !base_set_ || !target_ || base_capture_pending_ || ocr_hold_ || !arc)
            throw std::invalid_argument("Сначала получите действующую наводку SPH-2 для цели");
        context.impact_target = target_;
        const auto& solution = *(*arc == wardogs::Arc::low ? low_result_ : high_result_);
        context.impact_firing = wardogs::FiringSnapshot{
            *target_, *arc, solution.bearing_deg, solution.mil, target_height_delta(*target_)};
    }

    bool restore_failed_impact_guidance(const OcrMessage& context) {
        // A failed landing read does not invalidate the unchanged target's
        // firing solution. Never restore a different target/model or dismiss
        // a review that still requires an explicit decision.
        if (context.action != OcrAction::calibration_impact || pending_ocr_ ||
            context.input_epoch != input_epoch_ || context.calibration_epoch != calibration_epoch_ ||
            !vehicle_mode_ || !base_set_ || !target_ || base_capture_pending_ ||
            !context.impact_target || *context.impact_target != *target_ ||
            !context.impact_firing) return false;
        ocr_hold_ = false;
        try {
            show_result(*target_);
            if (effective_vehicle_arc()) {
                map_retry_event_.reset();
                update_readiness();
                return true;
            }
        } catch (const std::exception& error) {
            wardogs::log_warning("impact.guidance_restore_failed error=" + std::string(error.what()));
        }
        ocr_hold_ = true;
        clear_result(wardogs::i18n::text(QStringLiteral("Наводка недоступна · заново задайте цель SPH-2")));
        update_readiness();
        return false;
    }

    void reset_continuous_calibration() {
        ++calibration_epoch_;
        wardogs::log_info("continuous.reset previous_count=" +
            std::to_string(continuous_calibration_ ? continuous_calibration_->sample_count() : 0U) +
            " calibration_epoch=" + std::to_string(calibration_epoch_));
        continuous_calibration_.reset();
        last_impact_feedback_.reset();
        continuous_impact_->clear();
        update_continuous_controls();
        update_calibration_summary();
    }

    void record_manual_continuous_impact() {
        if (!vehicle_mode_ || !base_set_ || !target_) {
            set_status(wardogs::i18n::text(QStringLiteral("Сначала задайте орудие и цель SPH-2")), true);
            return;
        }
        try {
            OcrMessage context;
            capture_impact_context(context);
            const auto firing = *context.impact_firing;
            record_continuous_impact(
                wardogs::parse_manual_coordinate(
                    continuous_impact_->text().toStdWString()),
                wardogs::i18n::text(QStringLiteral("вручную")), firing);
        } catch (const std::exception& error) {
            set_status(wardogs::i18n::text(QStringLiteral("Некорректное попадание: ")) + error_text(error), true);
        }
    }

    bool record_continuous_impact(wardogs::Point impact, const QString& source,
                                  wardogs::FiringSnapshot firing) {
        try {
            std::ostringstream request;
            request.imbue(std::locale::classic());
            request.precision(17);
            request << "continuous.impact_requested source=" << utf8(source)
                    << " map=" << utf8(qtext(std::wstring{wardogs::game_map_key(current_game_map_)}))
                    << " base=" << base_.x << ',' << base_.y
                    << " target=" << firing.target.x << ',' << firing.target.y
                    << " impact=" << impact.x << ',' << impact.y
                    << " arc=" << (firing.arc == wardogs::Arc::low ? "low" : "high")
                    << " bearing=" << firing.bearing_deg << " mil=" << firing.mil
                    << " target_height_delta_m=" << firing.target_height_delta_m
                    << " calibration_epoch=" << calibration_epoch_;
            wardogs::log_info(request.str());
            if (!vehicle_mode_ || !base_set_ || !target_ || base_capture_pending_ || ocr_hold_)
                throw std::invalid_argument("Сначала получите действующую наводку SPH-2 для цели");
            if (firing.target != *target_)
                throw std::invalid_argument("Цель изменилась. Попадание не относится к текущему расчёту.");
            if (effective_vehicle_arc() != firing.arc)
                throw std::invalid_argument("Траектория изменилась. Попадание не относится к текущей наводке.");
            auto candidate = continuous_calibration_;
            if (!candidate)
                candidate.emplace(base_, wardogs::PlatformCalibration{wardogs::identity_rotation(), 0.0},
                                  wardogs::ContinuousCorrectionMode::local_only);
            const auto impact_height_delta = target_height_delta(impact);
            const auto assessment = candidate->add_landing(firing, impact, impact_height_delta);
            const auto corrected = candidate->solution(firing.target, firing.arc,
                                                       firing.target_height_delta_m);
            const auto feedback = wardogs::impact_feedback(base_, firing, impact, corrected);
            const double observed_miss_m = wardogs::calculate_shot(firing.target, impact).distance * 100.0;
            const double bearing_change = std::remainder(corrected.bearing_deg - firing.bearing_deg, 360.0);
            const double mil_change = corrected.mil - firing.mil;
            continuous_calibration_ = std::move(candidate);
            last_impact_feedback_ = feedback;
            last_impact_consistency_ = assessment.confidence;
            ++calibration_epoch_;
            continuous_aim_->setText(qtext(wardogs::format_point(firing.target)));
            continuous_impact_->clear();
            update_continuous_controls();
            update_calibration_summary();
            std::ostringstream diagnostic;
            diagnostic.imbue(std::locale::classic());
            diagnostic.precision(17);
            diagnostic << "continuous.impact_recorded source=" << utf8(source)
                       << " mode=local_only count=" << assessment.observation_count
                       << " arc=" << (firing.arc == wardogs::Arc::low ? "low" : "high")
                       << " target=" << firing.target.x << ',' << firing.target.y
                       << " impact=" << impact.x << ',' << impact.y
                       << " bearing=" << firing.bearing_deg
                       << " mil=" << firing.mil
                       << " observed_miss_m=" << observed_miss_m
                       << " consistency=" << assessment.confidence
                       << " correction_bearing_deg=" << bearing_change
                       << " correction_mil=" << mil_change
                       << " next_bearing=" << corrected.bearing_deg << " next_mil=" << corrected.mil
                       << " map=" << utf8(qtext(std::wstring{wardogs::game_map_key(current_game_map_)}))
                       << " base=" << base_.x << ',' << base_.y
                       << " target_height_delta_m=" << firing.target_height_delta_m
                       << " impact_height_delta_m=" << impact_height_delta;
            wardogs::log_info(diagnostic.str());
            show_result(*target_);
            const auto signed_value = [](double value) {
                if (std::abs(value) < 0.05) value = 0.0;
                const auto text = QString::number(value, 'f', 1);
                return value > 0.0 ? QStringLiteral("+") + text : text;
            };
            const auto terrain_status = terrain_map_ ? wardogs::i18n::text(QStringLiteral("учтён рельеф карты"))
                                                      : wardogs::i18n::text(QStringLiteral("рельеф не учтён"));
            set_status(assessment.confidence < 0.35
                ? wardogs::i18n::text(QStringLiteral("Попадания расходятся; точность поправки ограничена.\nПромах %1 м · азимут %2° · MIL %3."))
                      .arg(QString::number(observed_miss_m, 'f', 0), signed_value(bearing_change), signed_value(mil_change))
                : wardogs::i18n::text(QStringLiteral("Поправка учтена · промах %1 м.\nАзимут %2° · MIL %3; %4."))
                      .arg(QString::number(observed_miss_m, 'f', 0), signed_value(bearing_change), signed_value(mil_change), terrain_status));
            return true;
        } catch (const std::exception& error) {
            log_coordinate_event("continuous.impact_rejected", "impact", "observation", impact);
            set_status(wardogs::i18n::text(QStringLiteral("Поправка не применена: ")) + error_text(error), true);
            return false;
        }
    }

    void clear_continuous_calibration() {
        if (busy_) {
            set_status(wardogs::i18n::text(QStringLiteral("Дождитесь окончания чтения координат перед сбросом поправок.")));
            return;
        }
        if (pending_ocr_ && pending_ocr_->action == OcrAction::calibration_impact) {
            advance_input_epoch();
            ocr_hold_ = false;
        }
        reset_continuous_calibration();
        wardogs::log_info("continuous.cleared");
        if (target_) show_result(*target_);
        set_status(pending_ocr_ ? wardogs::i18n::text(QStringLiteral("Поправки сброшены. Новый захват ещё требует проверки."))
                               : wardogs::i18n::text(QStringLiteral("Поправки сброшены. Можно стрелять по табличному расчёту.")));
    }

    void invalidate_corrections() {
        reset_continuous_calibration();
    }

    void set_status(const QString& text, bool error = false) {
        if (error) wardogs::log_error("ui.error message=" + utf8(text));
        set_failure_state(error);
        status_->setProperty("error", error);
        status_->style()->unpolish(status_);
        status_->style()->polish(status_);
        const auto displayed = wardogs::i18n::text(text);
        status_->setText(displayed);
        if (pinned_window_) pinned_window_->set_workflow_status(displayed);
    }

    void set_failure_state(bool failed) {
        failure_state_ = failed;
        if (app_frame_) {
            app_frame_->setProperty("error", failed);
            app_frame_->style()->unpolish(app_frame_);
            app_frame_->style()->polish(app_frame_);
            app_frame_->update();
        }
        if (pinned_window_) pinned_window_->set_error(failed);
    }

    void enter_pinned_mode() {
        if (!require_game_integration()) return;
        wardogs::log_info("window.enter_pinned_mode");
        if (!pinned_window_) {
            pinned_window_ = std::make_unique<PinnedResultWindow>(
                [this] { if (game_mode_) exit_game_mode(); else exit_pinned_mode(); }, settings_.pinned_card,
                [this](PinnedResultWindow::Preferences& preferences) {
                    const auto previous = settings_;
                    auto candidate = settings_;
                    candidate.pinned_card = preferences;
                    const bool hotkey_changed =
                        candidate.pinned_card.unlock_hotkey !=
                        previous.pinned_card.unlock_hotkey;
                    QString hotkey_changes;
                    if (hotkey_changed) {
                        try {
                            hotkey_changes = register_hotkeys(candidate);
                        } catch (const std::exception& error) {
                            try { auto restored = previous; register_hotkeys(restored, false); }
                            catch (const std::exception& rollback_error) {
                                wardogs::log_error("pinned.hotkey_rollback_failed error=" + std::string(rollback_error.what()));
                            }
                            wardogs::log_error(
                                "pinned.unlock_hotkey_rejected error=" +
                                std::string(error.what()));
                            return false;
                        }
                        wardogs::log_info(
                            "pinned.unlock_hotkey_changed value=" +
                            one_line_utf8(qtext(
                                candidate.pinned_card.unlock_hotkey)));
                    }
                    if (candidate.pinned_card.locked !=
                        previous.pinned_card.locked)
                        wardogs::log_info(
                            std::string("pinned.lock_changed locked=") +
                            (candidate.pinned_card.locked ? "1" : "0"));
                    settings_ = candidate;
                    preferences = settings_.pinned_card;
                    update_action_labels();
                    try {
                        if (!diagnostic_) wardogs::save_settings(settings_);
                        if (!hotkey_changes.isEmpty())
                            set_status(wardogs::i18n::text(QStringLiteral("Занятые сочетания заменены: ")) + hotkey_changes);
                    } catch (const std::exception& error) {
                        wardogs::log_warning("pinned.preferences_save_failed error=" + std::string(error.what()));
                        set_status(wardogs::i18n::text(QStringLiteral("Параметры карточки действуют в этом сеансе. Сохранить не удалось: ")) + error_text(error), true);
                    }
                    return true;
                });
            pinned_window_->configure_ghost_controls(
                ghost_enabled_, settings_.ghost_reticle.opacity_percent,
                [this](bool enabled) { set_ghost_enabled(enabled); },
                [this](int opacity) { set_ghost_opacity(opacity); });
        }
        sync_pinned_result();
        pinned_window_->set_workflow_status(status_->text());
        pinned_window_->set_error(failure_state_);
        pinned_window_->move(frameGeometry().topLeft());
        pinned_mode_ = true;
        hide();
        pinned_window_->show();
        pinned_window_->raise();
    }

    void exit_pinned_mode() {
        if (!pinned_mode_) return;
        wardogs::log_info("window.exit_pinned_mode");
        pinned_mode_ = false;
        if (pinned_window_) pinned_window_->hide();
        showNormal();
        raise();
        activateWindow();
    }

    void hide_for_selection() {
        if (ghost_window_) ghost_window_->hide();
        selecting_ = true;
        if (mouse_timer_) mouse_timer_->stop();
        if (pinned_mode_ && pinned_window_) pinned_window_->hide();
        else hide();
    }

    void restore_after_selection() {
        selecting_ = false;
        sync_ghost_solution();
        if (pinned_mode_ && pinned_window_) {
            pinned_window_->show();
            pinned_window_->raise();
        } else if (!game_mode_) {
            showNormal();
            raise();
            activateWindow();
        }
    }

    void update_coordinates() {
        base_summary_->setText(wardogs::i18n::text(QStringLiteral("Орудие: ")) + (base_set_ ? qtext(wardogs::format_point(base_)) : wardogs::i18n::text(QStringLiteral("не задано"))));
        update_readiness();
        target_summary_->setText(wardogs::i18n::text(QStringLiteral("Цель: ")) +
            (target_ ? qtext(wardogs::format_point(*target_)) : QStringLiteral("—")));
        if (continuous_aim_) {
            const QString current = target_
                ? qtext(wardogs::format_point(*target_)) : QString{};
            if (continuous_aim_->text() != current) {
                continuous_aim_->setText(current);
                continuous_impact_->clear();
            }
        }
    }

    void update_action_labels() {
        if (quick_guide_)
            quick_guide_->setText(settings_.game_integration_enabled
                ? wardogs::i18n::text(QStringLiteral("1. Выберите или подтвердите текущую карту выше.\n"
                                 "2. В игре: M → ПКМ по позиции орудия → Отметить координаты.\n"
                                 "3. Нажмите %1 — орудие считается из чата, область выбирать не нужно.\n"
                                 "4. Средняя кнопка по цели на карте — автоматический расчёт. Возврат: %2."))
                    .arg(qtext(settings_.base_hotkey), qtext(settings_.exit_game_mode_hotkey))
                : wardogs::i18n::text(QStringLiteral("Выберите текущую карту. Откройте «Ручной ввод» или включите быструю игру в дополнительных настройках.")));
        if (quick_guide_ && vehicle_mode_ && settings_.game_integration_enabled)
            quick_guide_->setText(quick_guide_->text() + wardogs::i18n::text(QStringLiteral("\n5. %1 меняет траекторию. Поправка необязательна: наведите курсор на фактическое попадание и нажмите %2 до смены цели или траектории."))
                .arg(qtext(settings_.ghost_arc_hotkey), qtext(settings_.impact_hotkey)));
        if (manual_toggle_ && !settings_.game_integration_enabled) manual_toggle_->setChecked(true);
        if (!settings_.game_integration_enabled) {
            if (auto* tip = findChild<QLabel*>(QStringLiteral("gameTip")))
                tip->setText(wardogs::i18n::text(QStringLiteral("ОТДЕЛЬНЫЙ КАЛЬКУЛЯТОР\nКоординаты вводятся вручную.\nВзаимодействие с игрой включается в настройках.")));
        }
        else if (auto* tip = findChild<QLabel*>(QStringLiteral("gameTip")))
            tip->setText((settings_.middle_mouse_enabled
                ? wardogs::i18n::text(QStringLiteral("БЫСТРАЯ ИГРА\nОрудие — ")) + qtext(settings_.base_hotkey) +
                  wardogs::i18n::text(QStringLiteral(".\nЦель — средняя кнопка на карте.\nВозврат — "))
                : wardogs::i18n::text(QStringLiteral("ЗАХВАТ КЛАВИШЕЙ\nЦель — ")) + qtext(settings_.target_hotkey) + wardogs::i18n::text(QStringLiteral(".\nВозврат — ")))
                + qtext(settings_.exit_game_mode_hotkey));
        if (game_mode_) game_button_->setText(wardogs::i18n::text(QStringLiteral("Вернуться · ")) + qtext(settings_.exit_game_mode_hotkey));
        else update_game_button();
        region_button_->setText(qtext(settings_.region_hotkey) +
                                wardogs::i18n::text(QStringLiteral(" · Область")));
        region_button_->setToolTip(wardogs::i18n::text(QStringLiteral("Выделить одну строку и перейти с автопоиска на свою область")));
        base_input_->setToolTip(wardogs::i18n::text(QStringLiteral("Введите X/Y или вставьте скопированную пару из игрового чата: Ctrl+V, Enter")));
        target_input_->setToolTip(base_input_->toolTip());
        base_button_->setText(qtext(settings_.base_hotkey) + wardogs::i18n::text(QStringLiteral(" · Орудие")));
        target_button_->setText(qtext(settings_.target_hotkey) + wardogs::i18n::text(QStringLiteral(" · Цель")));
        quick_target_button_->setText(qtext(settings_.quick_target_hotkey) +
                                      wardogs::i18n::text(QStringLiteral(" · Разовая область")));
        calibration_ocr_->setToolTip(
            wardogs::i18n::text(QStringLiteral("%1 в игре · Прочитать попадание возле курсора карты. Из главного окна кнопка читает чат или свою область. Запишите попадание до смены цели/траектории."))
                .arg(qtext(settings_.impact_hotkey)));
    }

    void update_engine_summary() {
        engine_summary_->setText(settings_.backend == wardogs::OcrBackend::rapid
            ? wardogs::i18n::text(QStringLiteral("Быстрый захват: RapidOCR · работает офлайн"))
            : wardogs::i18n::text(QStringLiteral("Быстрый захват: RapidOCR · своя область: Windows OCR")));
        if (!settings_.game_integration_enabled)
            engine_summary_->setText(wardogs::i18n::text(QStringLiteral("Захват экрана отключён · включается в настройках")));
    }

    void update_region_summary() {
        const auto quick = wardogs::i18n::text(QStringLiteral("Орудие: черновик чата. Цель: X/Y возле курсора. Область выбирается автоматически."));
        if (settings_.automatic_chat_region) {
            region_summary_->setText(quick);
            return;
        }
        if (!region_) {
            region_summary_->setText(quick + wardogs::i18n::text(QStringLiteral("\nДополнительная область не выбрана.")));
            return;
        }
        const RECT& rect = region_->relative;
        region_summary_->setText(
            quick + wardogs::i18n::text(QStringLiteral("\nДополнительная область: %1 · %2×%3 px · (%4, %5)"))
                .arg(qtext(region_->monitor_device))
                .arg(rect.right - rect.left).arg(rect.bottom - rect.top)
                .arg(rect.left).arg(rect.top));
    }

    void clear_result(const QString& text) {
        if (direction_plot_) direction_plot_->set_shot(std::nullopt);
        if (copy_button_) copy_button_->setEnabled(false);
        distance_->setText(QStringLiteral("—"));
        bearing_->setText(QStringLiteral("—"));
        mortar_mil_->setText(QStringLiteral("—"));
        mortar_mil_result_.reset();
        mortar_mil_->setProperty("rangeError", false);
        mortar_mil_->style()->unpolish(mortar_mil_);
        mortar_mil_->style()->polish(mortar_mil_);
        raw_result_->setText(text);
        low_solution_->set_waiting();
        high_solution_->set_waiting();
        low_result_.reset();
        high_result_.reset();
        set_vehicle_result_error(false);
        vehicle_note_->setText(text);
        update_terrain_summary();
        sync_pinned_result();
        sync_ghost_solution();
    }

    bool show_result(wardogs::Point target) {
        if (!map_confirmed_) {
            clear_result(wardogs::i18n::text(QStringLiteral("Сначала выберите и подтвердите текущую карту")));
            update_readiness();
            set_status(wardogs::i18n::text(QStringLiteral("Расчёт заблокирован до выбора текущей карты.")));
            return true;
        }
        if (ocr_hold_) { clear_result(wardogs::i18n::text(QStringLiteral("Новый захват не подтверждён · проверьте OCR или введите координаты вручную"))); return true; }
        if (!base_set_) { clear_result(wardogs::i18n::text(QStringLiteral("Сначала задайте орудие"))); return true; }
        const auto result = wardogs::calculate_shot(base_, target);
        direction_plot_->set_shot(result);
        copy_button_->setEnabled(true);
        if (vehicle_mode_) return show_vehicle_result(result);
        distance_->setText(wardogs::i18n::text(qtext(wardogs::format_distance_meters(result.distance))));
        bearing_->setText(qtext(wardogs::format_bearing(result.angle)));
        raw_result_->setText(wardogs::i18n::text(qtext(wardogs::format_raw_distance(result.distance))));
        bool outside_mortar_table = false;
        try {
            mortar_mil_result_ =
                wardogs::mortar_mil_for_distance(result.distance * 100.0);
            mortar_mil_->setText(
                QStringLiteral("%1 MIL").arg(qRound(*mortar_mil_result_)));
        } catch (const std::invalid_argument&) {
            mortar_mil_result_.reset();
            mortar_mil_->setText(wardogs::i18n::text(QStringLiteral("Вне диапазона")));
            outside_mortar_table = true;
        }
        mortar_mil_->setProperty("rangeError", outside_mortar_table);
        mortar_mil_->style()->unpolish(mortar_mil_);
        mortar_mil_->style()->polish(mortar_mil_);
        sync_pinned_result();
        sync_ghost_solution();
        if (outside_mortar_table)
            set_status(wardogs::i18n::text(QStringLiteral("L81: наводка доступна для 132–684 м. Дальность и азимут рассчитаны.")));
        std::ostringstream diagnostic;
        diagnostic.imbue(std::locale::classic());
        diagnostic.precision(17);
        diagnostic << "solution.l81 map=" << utf8(qtext(std::wstring{wardogs::game_map_key(current_game_map_)}))
                   << " base=" << base_.x << ',' << base_.y << " target=" << target.x << ',' << target.y
                   << " range_m=" << result.distance * 100.0 << " bearing=" << result.angle
                   << " available=" << static_cast<bool>(mortar_mil_result_) << " heights=0";
        if (mortar_mil_result_) diagnostic << " mil=" << *mortar_mil_result_;
        wardogs::log_info(diagnostic.str());
        return outside_mortar_table;
    }

    bool show_vehicle_result(const wardogs::Shot& result) {
        cache_automatic_analysis(result);
        active_aim_offsets_ = {};
        double height_delta{};
        try {
            height_delta = target_height_delta(result.target);
        } catch (const std::exception& error) {
            low_result_.reset();
            high_result_.reset();
            log_coordinate_event("solution.sph2_height_unavailable", "target", "calculation", result.target);
            low_solution_->set_height_unavailable();
            high_solution_->set_height_unavailable();
            vehicle_note_->setText(wardogs::i18n::text(QStringLiteral("Высота орудия или цели недоступна")));
            set_vehicle_result_error(true);
            update_terrain_summary();
            sync_pinned_result();
            sync_ghost_solution();
            set_status(wardogs::i18n::text(QStringLiteral("Ошибка рельефа: ")) + error_text(error), true);
            return true;
        }
        update_terrain_summary(height_delta);
        const wardogs::PlatformCalibration calibration{wardogs::identity_rotation(), 0.0};
        int available = 0;
        low_result_.reset();
        high_result_.reset();
        QStringList warnings;
        QStringList corrected_arcs;
        const auto raw_distance = wardogs::i18n::text(qtext(wardogs::format_distance_meters(result.distance)));
        const auto raw_bearing = qtext(wardogs::format_bearing(result.angle));
        for (const auto [arc, name, card] : std::array{
                 std::tuple{wardogs::Arc::low, wardogs::i18n::text(QStringLiteral("Настильная")), low_solution_},
                 std::tuple{wardogs::Arc::high, wardogs::i18n::text(QStringLiteral("Навесная")), high_solution_}}) {
            try {
                const auto solution = continuous_calibration_
                    ? continuous_calibration_->solution(
                          result.target, arc, height_delta)
                    : wardogs::corrected_solution(
                          result.base, result.target, calibration, arc,
                          height_delta);
                card->set_solution(solution, result.distance * 100.0);
                if (continuous_calibration_) {
                    const auto baseline = wardogs::corrected_solution(
                        result.base, result.target, calibration, arc, height_delta);
                    active_aim_offsets_[arc == wardogs::Arc::low ? 0U : 1U] = {
                        std::remainder(solution.bearing_deg - baseline.bearing_deg,360.0), solution.mil-baseline.mil};
                    if (std::abs(std::remainder(solution.bearing_deg - baseline.bearing_deg, 360.0)) > 1e-9 ||
                        std::abs(solution.mil - baseline.mil) > 1e-9)
                        corrected_arcs.push_back(name.toLower());
                }
                if (arc == wardogs::Arc::low)
                    low_result_ = solution;
                else
                    high_result_ = solution;
                ++available;
            } catch (const std::exception& error) {
                card->set_unavailable(raw_distance, raw_bearing);
                wardogs::log_warning(std::string("solution.sph2_arc_unavailable arc=") +
                    (arc == wardogs::Arc::low ? "low" : "high") + " error=" + error.what());
                warnings.push_back(name + QStringLiteral("：") + error_text(error));
            }
        }
        const bool none_available = available == 0;
        set_vehicle_result_error(none_available);
        sync_ghost_solution();
        sync_pinned_result();
        std::ostringstream diagnostic;
        diagnostic.imbue(std::locale::classic());
        diagnostic.precision(17);
        diagnostic << "terrain.solution map=" << utf8(qtext(std::wstring{wardogs::game_map_key(current_game_map_)}))
                   << " heights=" << static_cast<bool>(terrain_) << " base=" << result.base.x << ',' << result.base.y
                   << " target=" << result.target.x << ',' << result.target.y
                   << " target_range_m=" << result.distance * 100.0 << " height_delta_m=" << height_delta;
        const auto selected_arc = effective_vehicle_arc();
        diagnostic << " selected_arc=" << (!selected_arc ? "unavailable" :
            *selected_arc == wardogs::Arc::high ? "high" : "low")
                   << " low_available=" << static_cast<bool>(low_result_)
                   << " high_available=" << static_cast<bool>(high_result_)
                   << " correction_count=" << (continuous_calibration_ ? continuous_calibration_->sample_count() : 0U)
                   << " calibration_epoch=" << calibration_epoch_;
        if (low_result_) diagnostic << " low_bearing=" << low_result_->bearing_deg << " low_mil=" << low_result_->mil;
        if (high_result_) diagnostic << " high_bearing=" << high_result_->bearing_deg << " high_mil=" << high_result_->mil;
        wardogs::log_info(diagnostic.str());
        if (!warnings.isEmpty()) {
            if (available > 0) {
                vehicle_note_->setText(wardogs::i18n::text(QStringLiteral("Одна траектория недоступна · используйте доступную")));
                set_status(warnings.join(QStringLiteral("；")) +
                           wardogs::i18n::text(QStringLiteral(" · другая траектория доступна")));
            } else {
                vehicle_note_->setText(
                    wardogs::i18n::text(QStringLiteral("Вне дальности SPH-2 · доступны только расстояние и азимут")));
                set_status(wardogs::i18n::text(QStringLiteral("Вне дальности: ")) +
                           warnings.join(QStringLiteral("；")), true);
            }
            return true;
        }
        vehicle_note_->setText((corrected_arcs.isEmpty() ? wardogs::i18n::text(QStringLiteral("Табличный расчёт"))
            : wardogs::i18n::text(QStringLiteral("Локальная поправка: ")) + corrected_arcs.join(QStringLiteral(", "))) +
            (terrain_map_ ? wardogs::i18n::text(QStringLiteral(" · %1 · перепад %2 м"))
                .arg(wardogs::i18n::text(qtext(terrain_map_->spec.display_name))).arg(height_delta, 0, 'f', 1)
                          : wardogs::i18n::text(QStringLiteral(" · рельеф не учтён"))));
        return false;
    }

    void set_vehicle_result_error(bool error) {
        vehicle_result_group_->setProperty("error", error);
        vehicle_result_group_->style()->unpolish(vehicle_result_group_);
        vehicle_result_group_->style()->polish(vehicle_result_group_);
        vehicle_result_group_->update();
    }

    void sync_pinned_result() {
        update_readiness();
        if (!pinned_window_) return;
        pinned_window_->set_mode(vehicle_mode_);
        if (vehicle_mode_)
            pinned_window_->set_vehicle_values(*low_solution_, *high_solution_);
        else
            pinned_window_->set_values(distance_->text(), bearing_->text(),
                                       mortar_mil_result_
                                           ? mortar_mil_->text()
                                           : QStringLiteral("—"));
        pinned_window_->set_selected_arc(effective_vehicle_arc());
        pinned_window_->set_ghost_enabled(ghost_enabled_);
        pinned_window_->set_ghost_opacity_percent(
            settings_.ghost_reticle.opacity_percent);
    }

    void set_ghost_enabled(bool enabled) {
        if (enabled && !require_game_integration()) {
            const QSignalBlocker blocker(ghost_button_);
            ghost_button_->setChecked(false);
            return;
        }
        ghost_enabled_ = enabled;
        if (ghost_button_) {
            const QSignalBlocker blocker(ghost_button_);
            ghost_button_->setChecked(enabled);
            ghost_button_->setProperty("highlighted", enabled);
            ghost_button_->setToolTip(enabled
                ? wardogs::i18n::text(QStringLiteral("Скрыть прицел"))
                : wardogs::i18n::text(QStringLiteral("Прицел поверх игры")));
            ghost_button_->setAccessibleName(ghost_button_->toolTip());
            ghost_button_->style()->unpolish(ghost_button_);
            ghost_button_->style()->polish(ghost_button_);
        }
        if (ghost_window_) ghost_window_->set_overlay_enabled(enabled);
        if (pinned_window_) pinned_window_->set_ghost_enabled(enabled);
        sync_ghost_solution();
    }

    void set_ghost_opacity(int opacity_percent) {
        settings_.ghost_reticle.opacity_percent = std::clamp(
            opacity_percent,
            wardogs::GhostReticlePreferences::minimum_opacity_percent,
            wardogs::GhostReticlePreferences::maximum_opacity_percent);
        if (ghost_window_)
            ghost_window_->set_opacity_percent(
                settings_.ghost_reticle.opacity_percent);
        if (pinned_window_)
            pinned_window_->set_ghost_opacity_percent(
                settings_.ghost_reticle.opacity_percent);
        try { if (!diagnostic_) wardogs::save_settings(settings_); }
                catch (const std::exception& e) { wardogs::log_warning("settings.save_failed error=" + std::string(e.what())); }
    }

    void toggle_ghost_arc() {
        if (!vehicle_mode_) {
            set_status(wardogs::i18n::text(QStringLiteral("Для L81 используется одна табличная траектория")));
            return;
        }
        settings_.ghost_reticle.preferred_arc =
            settings_.ghost_reticle.preferred_arc == wardogs::Arc::low
                ? wardogs::Arc::high : wardogs::Arc::low;
        advance_input_epoch();
        ++calibration_epoch_;
        continuous_impact_->clear();
        try { if (!diagnostic_) wardogs::save_settings(settings_); }
                catch (const std::exception& e) { wardogs::log_warning("settings.save_failed error=" + std::string(e.what())); }
        sync_ghost_solution();
        sync_pinned_result();
        const bool high =
            settings_.ghost_reticle.preferred_arc == wardogs::Arc::high;
        wardogs::log_info(std::string("guidance.arc_selected preferred=") + (high ? "high" : "low") +
            " effective=" + (!effective_vehicle_arc() ? "unavailable" :
                            *effective_vehicle_arc() == wardogs::Arc::high ? "high" : "low") +
            " calibration_epoch=" + std::to_string(calibration_epoch_));
        set_status(high ? wardogs::i18n::text(QStringLiteral("Предпочтительная траектория: навесная; при недоступности используется настильная"))
                        : wardogs::i18n::text(QStringLiteral("Предпочтительная траектория: настильная; при недоступности используется навесная")));
        if (effective_vehicle_arc()) set_status(sph2_workflow_hint());
    }

    void sync_ghost_monitor() {
        if (!ghost_window_) return;
        ghost_window_->set_target_monitor(!last_capture_monitor_.empty()
            ? last_capture_monitor_ : region_ ? region_->monitor_device : std::wstring{});
    }

    void sync_ghost_solution() {
        const auto arc = effective_vehicle_arc();
        update_readiness();
        low_solution_->set_selected(vehicle_mode_ && arc == wardogs::Arc::low);
        high_solution_->set_selected(vehicle_mode_ && arc == wardogs::Arc::high);
        if (vehicle_mode_) {
            const auto selected = arc.value_or(settings_.ghost_reticle.preferred_arc);
            set_arc_button(continuous_arc_, selected);
        }
        if (!ghost_window_) return;
        if (!vehicle_mode_) {
            std::optional<double> bearing;
            if (mortar_mil_result_ && target_)
                bearing = wardogs::calculate_shot(base_, *target_).angle;
            ghost_window_->set_mortar_solution(bearing, mortar_mil_result_);
            if (ghost_window_->adjusting()) ghost_window_->show();
            return;
        }
        std::optional<wardogs::CorrectedSolution> selected;
        if (vehicle_mode_) {
            if (arc == wardogs::Arc::low) selected = low_result_;
            if (arc == wardogs::Arc::high) selected = high_result_;
        }
        ghost_window_->set_solution(std::move(selected));
        if (ghost_window_->adjusting()) ghost_window_->show();
    }

    void paste_coordinates(bool base) {
        try {
            const auto text = QApplication::clipboard()->text();
            const auto point = wardogs::parse_manual_coordinate(text.toStdWString());
            if (!base) {
                if (!base_set_) throw std::invalid_argument("Сначала задайте координаты орудия.");
                (void)wardogs::calculate_shot(base_, point);
            }
            (base ? base_input_ : target_input_)->setText(text.trimmed());
            if (base) manual_base("clipboard"); else manual_target("clipboard");
        } catch (const std::exception& error) {
            set_status(wardogs::i18n::text(QStringLiteral("Вставка не выполнена: ")) + error_text(error), true);
        }
    }

    void manual_base(const char* source = "manual") {
        try {
            const auto point = wardogs::parse_manual_coordinate(base_input_->text().toStdWString());
            accept_manual_base(point, source);
        } catch (const std::exception& error) { set_status(error_text(error), true); }
    }

    void accept_manual_base(wardogs::Point point, const char* source = "manual") {
        if (!std::isfinite(point.x) || !std::isfinite(point.y))
            throw std::invalid_argument("Координаты должны быть конечными числами.");
        advance_input_epoch();
        ocr_hold_ = false;
        base_capture_pending_ = false;
        base_ = point;
        base_set_ = true;
        remember_accepted_point(wardogs::FireMissionKind::firing_position, point);
        target_.reset();
        invalidate_corrections();
        log_coordinate_event("coordinates.accepted", "base", source, point);
        base_input_->clear();
        update_coordinates();
        clear_result(wardogs::i18n::text(QStringLiteral("Орудие задано · укажите цель")));
        set_status(wardogs::i18n::text(QStringLiteral("Орудие: ")) + qtext(wardogs::format_point(base_)));
    }

    PlanningContext planning_context() {
        PlanningContext context;
        context.map = current_game_map_;
        context.weapon = vehicle_mode_ ? wardogs::AnalysisWeapon::sph2 : wardogs::AnalysisWeapon::l81;
        context.preferred_arc = settings_.ghost_reticle.preferred_arc;
        context.active_arc = effective_vehicle_arc();
        if (context.active_arc && !ocr_hold_ && !base_capture_pending_ && map_confirmed_) {
            context.preferred_arc = *context.active_arc;
            context.active_solution = *context.active_arc == wardogs::Arc::low ? low_result_ : high_result_;
        }
        context.map_confirmed = map_confirmed_;
        context.capture_pending = base_capture_pending_;
        context.solution_held = ocr_hold_;
        if (base_set_) context.base = base_;
        context.target = target_;
        if (terrain_) {
            const auto map = current_game_map_;
            context.terrain = [this, map](wardogs::Point point) -> std::optional<double> {
                if (!map_confirmed_ || current_game_map_ != map || !terrain_) return std::nullopt;
                return terrain_->height_at(point);
            };
        }
        return context;
    }

    void apply_planning_point(wardogs::FireMissionKind kind, wardogs::Point point,
                              wardogs::GameMap map, wardogs::AnalysisWeapon weapon) {
        const auto context = planning_context();
        if (!context.map_confirmed || context.map != map || context.weapon != weapon ||
            context.capture_pending)
            throw std::invalid_argument("Подтвердите карту и завершите чтение координат.");
        if (kind == wardogs::FireMissionKind::firing_position) accept_manual_base(point, "planning");
        else {
            if (!base_set_) throw std::invalid_argument("Сначала задайте координаты орудия.");
            if (ocr_hold_) throw std::invalid_argument("Подтвердите карту и завершите чтение координат.");
            accept_manual_target(point, "planning");
        }
    }

    void manual_target(const char* source = "manual") {
        if (!base_set_) { set_status(wardogs::i18n::text(QStringLiteral("Сначала задайте координаты орудия")), true); return; }
        if (base_capture_pending_) { set_status(wardogs::i18n::text(QStringLiteral("Сначала завершите чтение нового орудия или отклоните его захват")), true); return; }
        try {
            const auto point = wardogs::parse_manual_coordinate(target_input_->text().toStdWString());
            accept_manual_target(point, source);
        } catch (const std::exception& error) { set_status(error_text(error), true); }
    }

    void accept_manual_target(wardogs::Point point, const char* source = "manual") {
        (void)wardogs::calculate_shot(base_, point);
        advance_input_epoch();
        ocr_hold_ = false;
        target_ = point;
        remember_target(point, source);
        target_input_->clear();
        update_coordinates();
        if (!show_result(point)) set_status(vehicle_mode_ ? sph2_workflow_hint() : wardogs::i18n::text(QStringLiteral("Расчёт готов")));
    }

    void recall_target(int index) {
        if (index < 0 || index >= static_cast<int>(history_.size())) return;
        if (!base_set_) { set_status(wardogs::i18n::text(QStringLiteral("Сначала задайте координаты орудия")), true); return; }
        if (base_capture_pending_) { set_status(wardogs::i18n::text(QStringLiteral("Сначала завершите чтение нового орудия или отклоните его захват")), true); return; }
        // Display text is rounded; restore the retained point before MRU reordering.
        const auto point = history_[static_cast<std::size_t>(index)];
        try { accept_manual_target(point, "history"); }
        catch (const std::exception& error) { set_status(error_text(error), true); }
    }

    void begin_region_setup(std::optional<OcrAction> resume_action = std::nullopt) {
        if (!require_game_integration()) return;
        if (closing_ || selecting_) return;
        wardogs::log_info("selection.region_requested " + foreground_summary());
        if (busy_) { set_status(wardogs::i18n::text(QStringLiteral("Распознавание выполняется…"))); return; }
        hide_for_selection();
        const bool started = selector_.begin(
            [this, resume_action](std::optional<wardogs::CaptureRegion> region, QString error) {
                restore_after_selection();
                if (!error.isEmpty()) { set_status(error, true); return; }
                if (!region) { set_status(wardogs::i18n::text(QStringLiteral("Выбор области отменён"))); return; }
                advance_input_epoch();
                region_ = std::move(region);
                update_readiness();
                settings_.capture_region = region_;
                // The expert area selects only supplemental target/impact
                // capture. Alt+X and middle-click keep their dedicated sources.
                settings_.automatic_chat_region = false;
                last_capture_monitor_ = region_->monitor_device;
                sync_ghost_monitor();
                update_region_summary();
                try {
                    if (!diagnostic_) wardogs::save_settings(settings_);
                    set_status(wardogs::i18n::text(QStringLiteral("Область координат сохранена")));
                } catch (const std::exception& error) {
                    set_status(wardogs::i18n::text(QStringLiteral("Область действует в этом сеансе. Сохранить не удалось: ")) +
                                   error_text(error), true);
                }
                update_readiness();
                if (resume_action) start_ocr(*region_, *resume_action);
            });
        if (!started) {
            restore_after_selection();
            set_status(wardogs::i18n::text(QStringLiteral("Не удалось начать выбор области")), true);
        }
    }

    void begin_quick_target() {
        if (!require_game_integration()) return;
        if (closing_ || selecting_) return;
        if (!base_set_) { set_status(wardogs::i18n::text(QStringLiteral("Сначала задайте координаты орудия")), true); return; }
        wardogs::log_info("selection.quick_target_requested " + foreground_summary());
        if (busy_) { set_status(wardogs::i18n::text(QStringLiteral("Распознавание выполняется…"))); return; }
        hide_for_selection();
        const bool started = selector_.begin(
            [this](std::optional<wardogs::CaptureRegion> region, QString error) {
                if (!error.isEmpty()) {
                    restore_after_selection();
                    set_status(error, true);
                    return;
                }
                if (!region) {
                    restore_after_selection();
                    set_status(wardogs::i18n::text(QStringLiteral("Разовый захват отменён")));
                    return;
                }
                start_ocr(*region, OcrAction::target);
                restore_after_selection();
            });
        if (!started) {
            restore_after_selection();
            set_status(wardogs::i18n::text(QStringLiteral("Не удалось начать разовый захват")), true);
        }
    }

    void start_ocr(OcrAction action, bool middle_trigger = false) {
        if (!require_game_integration()) return;
        if (closing_ || selecting_) return;
        if (action != OcrAction::base && !base_set_) { set_status(wardogs::i18n::text(QStringLiteral("Сначала задайте координаты орудия")), true); return; }
        wardogs::log_info(std::string("ocr.request action=") + action_name(action) +
                          " saved_region=" + (region_ ? "1" : "0") + " " +
                          foreground_summary());
        if (action == OcrAction::calibration_impact) {
            if (!vehicle_mode_) {
                set_status(wardogs::i18n::text(QStringLiteral("Сначала переключите орудие на SPH-2")), true);
                return;
            }
            if (!target_) {
                set_status(wardogs::i18n::text(QStringLiteral("Задайте цель перед записью попадания")), true);
                return;
            }
            if (base_capture_pending_ || ocr_hold_ || !effective_vehicle_arc()) {
                set_status(wardogs::i18n::text(QStringLiteral("Сначала получите действующую наводку SPH-2 для цели. Попадание не записано.")), true);
                return;
            }
            try { OcrMessage context; capture_impact_context(context); }
            catch (const std::exception& error) { set_status(error_text(error), true); return; }
            if (settings_.automatic_chat_region && wardogs_is_foreground()) {
                POINT pointer{};
                if (!GetCursorPos(&pointer)) {
                    set_status(wardogs::i18n::text(QStringLiteral("Не удалось определить точку попадания на карте")), true);
                    return;
                }
                start_map_ocr({pointer.x, pointer.y,
                    reinterpret_cast<std::uintptr_t>(GetForegroundWindow())}, false, action);
                return;
            }
        }
        if ((pending_ocr_ && action != OcrAction::base) || busy_) {
            set_status(wardogs::i18n::text(QStringLiteral("Дождитесь окончания предыдущего захвата"))); return;
        }
        if (action != OcrAction::base && base_capture_pending_) {
            set_status(wardogs::i18n::text(QStringLiteral("Сначала завершите чтение нового орудия или отклоните его захват")), true);
            return;
        }
        // Gun capture is a dedicated quick workflow. A saved expert area or
        // manual backend must never reroute Alt+X through whole-strip OCR.
        if (action == OcrAction::base || settings_.automatic_chat_region) {
            try {
                const auto capture = automatic_chat_capture_region();
                if (action == OcrAction::base) {
                    advance_input_epoch();
                    base_capture_pending_ = true;
                    enter_pinned_mode();
                }
                start_ocr(capture.region, action, true, middle_trigger, capture.window);
            }
            catch (const std::exception& error) {
                ocr_hold_ = true;
                clear_result(wardogs::i18n::text(QStringLiteral("Захват не выполнен · прежняя наводка скрыта")));
                update_readiness();
                set_status(error_text(error), true);
            }
            return;
        }
        if (!region_) { begin_region_setup(action); return; }
        start_ocr(*region_, action, false, middle_trigger);
    }

    void start_ocr(const wardogs::CaptureRegion& capture_region, OcrAction action,
                   bool automatic_chat = false, bool middle_trigger = false, HWND expected_window = nullptr) {
        if (!require_game_integration()) return;
        if (action != OcrAction::base && base_capture_pending_) {
            set_status(wardogs::i18n::text(QStringLiteral("Сначала завершите чтение нового орудия или отклоните его захват")), true);
            return;
        }
        if (pending_ocr_) { set_status(wardogs::i18n::text(QStringLiteral("Сначала подтвердите или отклоните предыдущий захват")), true); return; }
        if (busy_.exchange(true)) {
            wardogs::log_warning(std::string("ocr.busy action=") + action_name(action));
            set_status(wardogs::i18n::text(QStringLiteral("Распознавание выполняется…")));
            return;
        }
        if (action == OcrAction::base) base_capture_pending_ = true;
        OcrMessage context;
        context.action = action;
        context.calibration_epoch = calibration_epoch_;
        context.input_epoch = input_epoch_;
        context.automatic_chat = automatic_chat;
        // A marker click is not proof that a fresh coordinate appeared in chat.
        context.force_review = automatic_chat || middle_trigger ||
            capture_region.monitor_size.cx == 0 || capture_region.monitor_size.cy == 0;
        if (action == OcrAction::calibration_impact) {
            try {
                capture_impact_context(context);
            } catch (const std::exception& error) {
                busy_ = false;
                set_status(wardogs::i18n::text(QStringLiteral("Расчёт недоступен: ")) + error_text(error), true);
                return;
            }
        }
        {
            const auto& rect = capture_region.relative;
            std::ostringstream diagnostic;
            diagnostic << "capture.begin action=" << action_name(action)
                       << " monitor=" << one_line_utf8(qtext(capture_region.monitor_device))
                       << " rect=" << rect.left << ',' << rect.top << ','
                       << rect.right << ',' << rect.bottom;
            wardogs::log_info(diagnostic.str());
        }
        wardogs::Image image;
        const bool main_was_visible = isVisible() && !isMinimized();
        const bool pinned_was_visible = pinned_window_ && pinned_window_->isVisible();
        const bool ghost_was_visible = ghost_window_ && ghost_window_->isVisible();
        if (main_was_visible) hide();
        if (pinned_was_visible) pinned_window_->hide();
        if (ghost_was_visible) ghost_window_->hide();
        const auto restore_overlays = [this, main_was_visible, pinned_was_visible, ghost_was_visible] {
            if (main_was_visible) show();
            if (pinned_was_visible && pinned_window_) pinned_window_->show();
            if (ghost_was_visible) sync_ghost_solution();
        };
        try {
            DwmFlush();
            const auto check_chat_source = [&] {
                if (!expected_window || GetForegroundWindow() != expected_window || !is_wardogs_window(expected_window))
                    throw std::runtime_error("Перед автозахватом перейдите в WARDOGS и нажмите клавишу захвата. Другое окно закрывает игру.");
                const auto current = automatic_chat_capture_region();
                if (current.window != expected_window || current.region.monitor_device != capture_region.monitor_device ||
                    !EqualRect(&current.region.relative, &capture_region.relative))
                    throw std::runtime_error("Размер или положение окна игры изменились. Повторите захват орудия.");
            };
            if (automatic_chat) check_chat_source();
            image = wardogs::capture_screen(capture_region);
            if (automatic_chat) check_chat_source();
            last_capture_monitor_ = capture_region.monitor_device;
            sync_ghost_monitor();
            restore_overlays();
            wardogs::log_info("capture.success width=" +
                              std::to_string(image.width) + " height=" +
                              std::to_string(image.height));
        }
        catch (const std::exception& error) {
            busy_ = false;
            restore_overlays();
            wardogs::log_error("capture.failed error=" + std::string(error.what()));
            ocr_hold_ = true;
            clear_result(wardogs::i18n::text(QStringLiteral("Новый захват не выполнен · прежняя наводка скрыта")));
            restore_failed_impact_guidance(context);
            update_readiness();
            set_status(wardogs::i18n::text(QStringLiteral("Захват экрана не выполнен: ")) + error_text(error), true);
            return;
        }
        launch_ocr_worker(std::move(image), std::move(context));
    }

    void start_map_ocr(wardogs::MiddleMouseEvent event, bool retry = false,
                       OcrAction action = OcrAction::target, bool defer_initial = false) {
        if (!require_game_integration() || closing_ || selecting_ || !base_set_) return;
        if (base_capture_pending_) {
            wardogs::log_info("map.request_ignored reason=base_capture_pending");
            set_status(wardogs::i18n::text(QStringLiteral("Новое орудие ещё не задано · повторите %1 или подтвердите его координаты"))
                .arg(qtext(settings_.base_hotkey)), true);
            return;
        }
        OcrMessage impact_context;
        if (retry && map_request_context_) impact_context = *map_request_context_;
        if (retry && action == OcrAction::calibration_impact &&
            (!map_request_context_ || impact_context.input_epoch != input_epoch_ ||
             impact_context.calibration_epoch != calibration_epoch_)) {
            wardogs::log_info("impact.stale_map_retry_discarded");
            cancel_map_capture(wardogs::i18n::text(QStringLiteral("Исходные данные изменились. Захватите координаты заново.")));
            return;
        }
        if (action == OcrAction::calibration_impact && !retry) {
            if (busy_ || pending_ocr_) {
                set_status(wardogs::i18n::text(QStringLiteral("Дождитесь завершения предыдущего захвата")));
                return;
            }
            try { capture_impact_context(impact_context); }
            catch (const std::exception& error) { set_status(error_text(error), true); return; }
        }
        if (!retry) {
            advance_input_epoch();
            map_retry_count_ = 0;
            map_requested_at_ = std::chrono::steady_clock::now();
        }
        impact_context.action = action;
        impact_context.input_epoch = input_epoch_;
        if (!retry) impact_context.calibration_epoch = calibration_epoch_;
        map_request_context_ = impact_context;
        map_retry_action_ = action;
        map_retry_event_ = event;
        ocr_hold_ = true;
        clear_result(wardogs::i18n::text(QStringLiteral("Считываю координаты новой отметки на карте…")));
        update_readiness();
        try {
            RECT client{};
            if (!GetClientRect(reinterpret_cast<HWND>(event.foreground_window), &client))
                throw std::runtime_error("Не удалось определить границы карты WARDOGS.");
            const double scale = std::clamp(static_cast<double>(client.bottom - client.top) / 1080.0, 0.5, 4.0);
            const auto capture = map_capture_regions(event, scale);
            if (map_request_client_ && !EqualRect(&*map_request_client_, &capture.client))
                throw std::runtime_error("Размер или положение окна игры изменились. Повторите захват этой точки.");
            map_request_client_ = capture.client;
            if (defer_initial) {
                if (!map_source_matches(event, map_request_client_))
                    throw std::runtime_error("Курсор или окно изменились. Повторите захват этой точки.");
                pending_mouse_epoch_ = input_epoch_;
                // Pin the viewport at the click, then wait for the game to
                // render its marker without blocking the UI or taking focus.
                mouse_timer_->start(std::clamp(settings_.mouse_capture_delay_ms, 80, 2000));
                wardogs::log_info("capture.map_settle_scheduled epoch=" + std::to_string(input_epoch_));
                return;
            }
            // Returning to the main window does not disarm capture. Hide our
            // own UI before reading the game again, without taking its focus.
            if (!pinned_mode_) enter_pinned_mode();
            const bool pinned_visible = pinned_window_ && pinned_window_->isVisible();
            const bool ghost_visible = ghost_window_ && ghost_window_->isVisible();
            if (pinned_visible) pinned_window_->hide();
            if (ghost_visible) ghost_window_->hide();
            CapturedOcrJob job;
            try {
                DwmFlush();
                POINT cursor{};
                const auto pointer_matches = [&] {
                    return GetForegroundWindow() == capture.window && GetCursorPos(&cursor) &&
                        cursor.x == event.x && cursor.y == event.y;
                };
                if (!pointer_matches())
                    throw std::runtime_error("Курсор или окно изменились. Повторите захват этой точки.");
                auto frame = wardogs::capture_screen(capture.search);
                const auto after = map_capture_regions(event, scale);
                if (!pointer_matches() || !EqualRect(&after.client, &capture.client))
                    throw std::runtime_error("Курсор или границы игры изменились. Повторите захват этой точки.");
                job.image = std::move(frame);
                job.map_layout = capture.layout;
            } catch (...) {
                if (pinned_visible) pinned_window_->show();
                if (ghost_visible) sync_ghost_solution();
                throw;
            }
            if (pinned_visible) pinned_window_->show();
            if (ghost_visible) sync_ghost_solution();
            last_capture_monitor_ = capture.search.monitor_device;
            sync_ghost_monitor();
            job.context = std::move(impact_context);
            job.context.action = action;
            job.context.map_coordinates = true;
            job.context.map_event = event;
            wardogs::log_info(std::string("capture.map source=cursor_neighborhood chat=0 action=") +
                action_name(action) + " retry=" + std::to_string(map_retry_count_) +
                " width=" + std::to_string(job.image.width) + " height=" + std::to_string(job.image.height) +
                " scale=" + std::to_string(scale));
            if (busy_) {
                queued_map_job_ = std::move(job);
                set_status(wardogs::i18n::text(QStringLiteral("Новая отметка принята · считываю последние координаты")));
            } else {
                busy_ = true;
                launch_ocr_worker(std::move(job.image), std::move(job.context), std::move(job.map_layout));
            }
        } catch (const std::exception& error) {
            restore_failed_impact_guidance(impact_context);
            set_status((action == OcrAction::calibration_impact ? wardogs::i18n::text(QStringLiteral("Попадание не прочитано: ")) : wardogs::i18n::text(QStringLiteral("Отметка не прочитана: "))) + error_text(error) +
                (action == OcrAction::calibration_impact
                    ? wardogs::i18n::text(QStringLiteral(" · повторите %1 на точке попадания")).arg(qtext(settings_.impact_hotkey)) : QString{}), true);
        }
    }

    void launch_ocr_worker(wardogs::Image image, OcrMessage context,
                           std::optional<wardogs::MapOcrSearchLayout> map_layout = std::nullopt) {
        const auto action = context.action;
        if (worker_.joinable()) worker_.join();
        ocr_hold_ = true;
        clear_result(wardogs::i18n::text(QStringLiteral("Распознавание нового захвата · предыдущая наводка скрыта")));
        update_readiness();
        const auto backend = context.map_coordinates || context.automatic_chat
            ? wardogs::OcrBackend::rapid : settings_.backend;
        const auto pattern = settings_.coordinate_pattern;
        set_status(backend == wardogs::OcrBackend::rapid
                       ? wardogs::i18n::text(QStringLiteral("Распознавание координат…"))
                       : wardogs::i18n::text(QStringLiteral("Распознавание средствами Windows…")));
        QPointer<MainWindow> self(this);
        worker_ = std::jthread([this, self, image = std::move(image), map_layout = std::move(map_layout), backend,
                                pattern, action,
                                context = std::move(context)](std::stop_token stop) mutable {
            const auto started_at = std::chrono::steady_clock::now();
            wardogs::log_info(std::string("ocr.worker_started action=") +
                              action_name(action) + " backend=" +
                              (backend == wardogs::OcrBackend::rapid ? "rapid"
                                                                      : "windows"));
            OcrMessage message = std::move(context);
            try {
                wardogs::OcrResult result;
                if (backend == wardogs::OcrBackend::rapid) {
                    if (!rapid_) rapid_ = std::make_unique<wardogs::RapidOcr>(
                        executable_directory() / L"models" / L"PP-OCRv6_rec_small.onnx");
                    result = map_layout ? rapid_->recognize_map_neighborhood(image, *map_layout, stop)
                                   : message.automatic_chat ? rapid_->recognize_chat(image, stop)
                                                            : rapid_->recognize(image, stop);
                } else {
                    if (!windows_) windows_ = std::make_unique<wardogs::WindowsOcr>();
                    result = windows_->recognize(image, stop);
                    try { (void)wardogs::parse_ocr_coordinate(result.text, pattern); }
                    catch (const std::invalid_argument&) {
                        const auto original_text = result.text;
                        const auto original_line_count = result.line_count;
                        result = windows_->recognize_high_contrast(image, stop);
                        result.alternate_text = original_text;
                        result.line_count = std::max(result.line_count, original_line_count);
                    }
                }
                message.text = result.text;
                message.confidence = result.confidence;
                message.assessment = wardogs::assess_ocr_result(result,
                    message.map_coordinates ? wardogs::default_ocr_coordinate_pattern : std::wstring_view{pattern});
                if (message.automatic_chat || message.map_coordinates) {
                    message.force_review = !(result.isolated_coordinate_pair &&
                        !result.coordinate_boundary_clipped && result.coordinate_passes_agree &&
                        result.coordinate_glyph_count_matches && message.assessment.selected &&
                        message.assessment.candidates.size() == 1 &&
                        (!message.automatic_chat || result.coordinate_is_chat_draft) &&
                        (!message.map_coordinates || result.map_axes_labeled));
                    if (!message.force_review && message.map_coordinates)
                        message.assessment.multiple_lines = false; // Two semantic axis fields, one pair.
                    QStringList evidence_reasons;
                    // Worker messages stay canonical. The app language can
                    // change before this queued result reaches the GUI thread.
                    if (!result.isolated_coordinate_pair)
                        evidence_reasons << QStringLiteral("не найдена единственная полная пара X/Y");
                    if (result.coordinate_boundary_clipped)
                        evidence_reasons << QStringLiteral("текст координат обрезан у края");
                    if (!result.coordinate_passes_agree)
                        evidence_reasons << QStringLiteral("повторное чтение не подтвердило координаты");
                    if (!result.coordinate_glyph_count_matches)
                        evidence_reasons << QStringLiteral("не все видимые знаки удалось прочитать");
                    if (message.automatic_chat && !result.coordinate_is_chat_draft)
                        evidence_reasons << QStringLiteral("не подтверждено активное поле чата");
                    if (message.map_coordinates && !result.map_axes_labeled)
                        evidence_reasons << QStringLiteral("не прочитаны подписи X и Y");
                    message.evidence_review_reason = evidence_reasons.join(QStringLiteral("; "));
                }
                std::ostringstream evidence;
                evidence << "ocr.evidence source=" << (message.map_coordinates ? "map"
                    : message.automatic_chat ? "chat_draft" : "manual")
                    << " epoch=" << message.input_epoch
                    << " minimum_confidence=" << result.minimum_confidence
                    << " isolated=" << result.isolated_coordinate_pair
                    << " clipped=" << result.coordinate_boundary_clipped
                    << " passes_agree=" << result.coordinate_passes_agree
                    << " glyph_count_matches=" << result.coordinate_glyph_count_matches
                    << " axes_labeled=" << result.map_axes_labeled
                    << " chat_draft=" << result.coordinate_is_chat_draft
                    << " candidates=" << message.assessment.candidates.size()
                    << " low_confidence=" << message.assessment.low_confidence
                    << " ambiguous=" << message.assessment.ambiguous
                    << " pass_disagreement=" << message.assessment.pass_disagreement
                    << " multiple_lines=" << message.assessment.multiple_lines
                    << " force_review=" << message.force_review;
                wardogs::log_info(evidence.str());
                if (!message.assessment.selected)
                    throw std::invalid_argument(message.map_coordinates
                        ? message.action == OcrAction::calibration_impact
                            ? "Подведите курсор к точке попадания на карте и повторите клавишу чтения попадания."
                            : "Удерживайте курсор на цели и повторите среднюю кнопку. Если подписи закрыты: M → ПКМ → Отметить координаты → клавиша захвата цели."
                        : "Откройте M → ПКМ → Отметить координаты и повторите захват орудия.");
                message.point = *message.assessment.selected;
                message.success = true;
            } catch (const std::exception& error) {
                message.error = QString::fromUtf8(error.what());
                wardogs::log_error(std::string("ocr.worker_failed action=") +
                                   action_name(action) + " error=" + error.what());
            }
            message.elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started_at).count();
            if (stop.stop_requested()) return;
            if (self) QMetaObject::invokeMethod(self,
                [self, message = std::move(message)]() mutable {
                    if (self) self->finish_ocr(std::move(message));
                }, Qt::QueuedConnection);
        });
    }

    void finish_ocr(OcrMessage message) {
        busy_ = false;
        if (closing_) return;
        if (queued_map_job_ && queued_map_job_->context.input_epoch == input_epoch_) {
            auto job = std::move(*queued_map_job_);
            queued_map_job_.reset();
            busy_ = true;
            launch_ocr_worker(std::move(job.image), std::move(job.context), std::move(job.map_layout));
            return;
        }
        if (message.input_epoch != input_epoch_) {
            wardogs::log_info("ocr.stale_result_discarded");
            return;
        }
        if (message.action == OcrAction::calibration_impact &&
            message.calibration_epoch != calibration_epoch_) {
            wardogs::log_info("impact.stale_correction_result_discarded");
            return;
        }
        if (message.map_coordinates && message.map_event && !message.confirmed) {
            const auto decision = map_consensus_.observe(
                message.success ? std::optional{message.point} : std::nullopt,
                message.success && !message.requires_review());
            if (message.success && (!map_best_result_ ||
                (map_best_result_->requires_review() && !message.requires_review())))
                map_best_result_ = message;
            wardogs::log_info("ocr.temporal epoch=" + std::to_string(input_epoch_) +
                " frames=" + std::to_string(map_consensus_.frames()) +
                " agreeing=" + std::to_string(map_consensus_.agreeing_frames()) +
                " conflict=" + std::to_string(map_consensus_.conflict()));
            const auto event = *message.map_event;
            const bool source_unchanged = map_source_matches(event, map_request_client_);
            if (decision == wardogs::MapCaptureDecision::retry && source_unchanged &&
                settings_.game_integration_enabled &&
                (settings_.middle_mouse_enabled || message.action == OcrAction::calibration_impact) && base_set_) {
                ++map_retry_count_;
                map_retry_event_ = event;
                map_retry_action_ = message.action;
                pending_mouse_epoch_ = input_epoch_;
                mouse_timer_->start(std::clamp(settings_.mouse_capture_delay_ms, 120, 350));
                set_status(wardogs::i18n::text(QStringLiteral("Уточняю координаты отметки…")));
                return;
            }
            if (!source_unchanged) {
                message.success = false;
                message.error = wardogs::i18n::text(QStringLiteral("Курсор или окно изменились. Повторите захват этой точки."));
            } else if (decision != wardogs::MapCaptureDecision::accept && map_best_result_) {
                message = *map_best_result_;
                message.force_review = true;
                message.evidence_review_reason = map_consensus_.conflict()
                    ? QStringLiteral("координаты в отдельных кадрах расходятся")
                    : QStringLiteral("координаты не подтверждены двумя отдельными кадрами");
                for (const auto& candidate : map_consensus_.candidates())
                    if (std::find(message.assessment.candidates.begin(), message.assessment.candidates.end(), candidate) ==
                        message.assessment.candidates.end()) message.assessment.candidates.push_back(candidate);
                message.assessment.ambiguous = message.assessment.candidates.size() > 1;
            }
            map_retry_event_.reset();
            map_request_context_.reset();
            map_request_client_.reset();
            if (map_requested_at_)
                message.elapsed_ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - *map_requested_at_).count();
            map_requested_at_.reset();
        }
        if (message.success && message.action == OcrAction::target &&
            (message.confirmed || !message.requires_review())) {
            try { (void)wardogs::calculate_shot(base_, message.point); }
            catch (const std::exception& error) { message.success = false; message.error = QString::fromUtf8(error.what()); }
        }
        if (!message.success) {
            wardogs::log_error(std::string("ocr.finished success=0 action=") +
                               action_name(message.action) +
                               " input_epoch=" + std::to_string(message.input_epoch) +
                               " elapsed_ms=" + std::to_string(message.elapsed_ms) + " error=" +
                               utf8(message.error));
            ocr_text_->setText(wardogs::i18n::text(QStringLiteral("Текст: ")) +
                (message.text.empty() ? wardogs::i18n::text(QStringLiteral("пусто")) : qtext(message.text)));
            ocr_hold_ = true;
            clear_result(wardogs::i18n::text(QStringLiteral("OCR не распознало новый захват · введите координаты вручную или повторите захват")));
            restore_failed_impact_guidance(message);
            update_readiness();
            set_status(wardogs::i18n::text(QStringLiteral("Координаты не распознаны: ")) + message.error, true);
            return;
        }
        {
            std::ostringstream diagnostic;
            diagnostic.imbue(std::locale::classic());
            diagnostic.precision(17);
            diagnostic << "ocr.finished success=1 action="
                       << action_name(message.action) << " point="
                       << message.point.x << ',' << message.point.y
                       << " confidence=" << message.confidence
                       << " elapsed_ms=" << message.elapsed_ms
                       << " review=" << message.requires_review()
                       << " confirmed=" << message.confirmed
                       << " input_epoch=" << message.input_epoch;
            wardogs::log_info(diagnostic.str());
        }
        QString confidence;
        if (message.confidence > 0.0F)
            confidence = wardogs::i18n::text(QStringLiteral(" · уверенность символов %1%")).arg(qRound(message.confidence * 100.0F));
        ocr_text_->setText(wardogs::i18n::text(QStringLiteral("Текст: ")) + qtext(message.text) + confidence + wardogs::i18n::text(QStringLiteral(" · %1 мс")).arg(qRound(message.elapsed_ms)));
        if (!message.confirmed && message.requires_review()) {
            log_coordinate_event("ocr.review_required", action_name(message.action),
                                 "ocr", message.point);
            QStringList reasons;
            if (message.force_review) reasons << (!message.evidence_review_reason.isEmpty()
                ? wardogs::i18n::text(message.evidence_review_reason) : message.map_coordinates
                ? wardogs::i18n::text(QStringLiteral("не удалось подтвердить полное чтение двух координат возле курсора"))
                : wardogs::i18n::text(QStringLiteral("не удалось подтвердить полную пару в активном поле чата")));
            if (message.assessment.ambiguous) reasons << wardogs::i18n::text(QStringLiteral("несколько разных пар X/Y"));
            if (message.assessment.pass_disagreement) reasons << wardogs::i18n::text(QStringLiteral("проходы распознавания расходятся"));
            if (message.assessment.low_confidence) reasons << wardogs::i18n::text(QStringLiteral("слабая уверенность символов"));
            if (message.assessment.multiple_lines) reasons << wardogs::i18n::text(QStringLiteral("выделено несколько строк"));
            wardogs::log_info("ocr.review_reason action=" + std::string(action_name(message.action)) +
                " ambiguous=" + std::to_string(message.assessment.ambiguous) +
                " pass_disagreement=" + std::to_string(message.assessment.pass_disagreement) +
                " low_confidence=" + std::to_string(message.assessment.low_confidence) +
                " evidence=" + utf8(message.evidence_review_reason));
            ocr_review_reason_->setText(wardogs::i18n::text(QStringLiteral("%1. Орудие, цель и поправки пока не изменены.")).arg(reasons.join(QStringLiteral("; "))));
            ocr_candidates_->clear();
            for (const auto& candidate : message.assessment.candidates)
                ocr_candidates_->addItem(qtext(wardogs::format_point(candidate)));
            ocr_candidates_->setCurrentText(qtext(wardogs::format_point(message.point)));
            pending_ocr_ = std::move(message);
            ocr_hold_ = true;
            clear_result(wardogs::i18n::text(QStringLiteral("Проверьте новый захват · наводка скрыта до подтверждения")));
            ocr_review_->show();
            if (mouse_timer_) mouse_timer_->stop();
            update_readiness();
            set_status(game_mode_ ? wardogs::i18n::text(QStringLiteral("OCR требует проверки · вернитесь через ")) + qtext(settings_.exit_game_mode_hotkey)
                                  : wardogs::i18n::text(QStringLiteral("Проверьте координаты в карточке OCR и подтвердите их")), true);
            return;
        }
        ocr_hold_ = false;
        if (message.action == OcrAction::calibration_impact) {
            if (target_ && base_set_) show_result(*target_);
            update_readiness();
            if (message.impact_firing)
                record_continuous_impact(message.point, QStringLiteral("OCR"),
                                         *message.impact_firing);
            else set_status(wardogs::i18n::text(QStringLiteral("Наводка выстрела не сохранена. Попадание не применено.")), true);
            return;
        }
        if (message.action == OcrAction::base) {
            advance_input_epoch();
            base_capture_pending_ = false;
            base_ = message.point;
            base_set_ = true;
            remember_accepted_point(wardogs::FireMissionKind::firing_position, message.point);
            target_.reset();
            invalidate_corrections();
            log_coordinate_event("coordinates.accepted", "base",
                message.confirmed ? "ocr_confirmed" : "ocr_auto", message.point);
            clear_result(wardogs::i18n::text(QStringLiteral("Орудие задано · укажите цель")));
            set_status(wardogs::i18n::text(QStringLiteral("Орудие распознано: ")) + qtext(wardogs::format_point(message.point)));
            base_input_->setText(qtext(wardogs::format_point(message.point)));
        } else {
            advance_input_epoch();
            target_ = message.point;
            remember_target(message.point, message.confirmed ? "ocr_confirmed" : "ocr_auto");
            if (!show_result(message.point))
                set_status(vehicle_mode_ ? sph2_workflow_hint()
                    : wardogs::i18n::text(QStringLiteral("Цель распознана: ")) +
                      qtext(wardogs::format_point(message.point)) + wardogs::i18n::text(QStringLiteral(" · расчёт готов")));
        }
        update_coordinates();
        if (message.action == OcrAction::base &&
            settings_.game_integration_enabled) enter_game_mode();
    }

    void edit_settings() {
        wardogs::log_info("settings.dialog_opened");
        if (busy_) {
            set_status(wardogs::i18n::text(QStringLiteral("Дождитесь окончания распознавания перед изменением настроек")));
            return;
        }
        SettingsDialog dialog(settings_, this);
        if (mouse_timer_) mouse_timer_->stop();
        unregister_hotkeys();
        if (dialog.exec() != QDialog::Accepted) {
            try {
                restore_current_hotkeys();
            } catch (const std::exception& error) {
                set_status(wardogs::i18n::text(QStringLiteral("Не удалось восстановить горячие клавиши: ")) + error_text(error), true);
            }
            return;
        }
        auto candidate = dialog.settings();
        const bool adjust_ghost = dialog.adjust_ghost_requested();
        const auto previous = settings_;
        try {
            const auto hotkey_changes = register_hotkeys(candidate);
            register_mouse_trigger(candidate);
            if (!diagnostic_) wardogs::save_settings(candidate);
            settings_ = candidate;
            if (settings_.automatic_chat_region != previous.automatic_chat_region)
                last_capture_monitor_.clear();
            advance_input_epoch();
            discard_ocr_review();
            if (!settings_.game_integration_enabled) {
                exit_game_mode();
                set_ghost_enabled(false);
                if (ghost_window_) { ghost_window_->cancel_adjustment(); ghost_window_->hide(); }
            }
            if (ghost_window_) {
                ghost_window_->set_opacity_percent(
                    settings_.ghost_reticle.opacity_percent);
                ghost_window_->set_bearing_compensation(
                    settings_.ghost_reticle.bearing_compensation_deg);
                ghost_window_->set_width(settings_.ghost_reticle.width);
                sync_ghost_monitor();
                if (adjust_ghost && settings_.game_integration_enabled) ghost_window_->begin_adjustment();
            }
            if (pinned_window_) {
                pinned_window_->configure_unlock_hotkey(settings_.pinned_card.unlock_hotkey);
                pinned_window_->configure_ghost_controls(
                    ghost_enabled_, settings_.ghost_reticle.opacity_percent,
                    [this](bool enabled) { set_ghost_enabled(enabled); },
                    [this](int opacity) { set_ghost_opacity(opacity); });
            }
            if (worker_.joinable()) worker_.join();
            rapid_.reset(); windows_.reset();
            update_engine_summary(); update_action_labels(); update_region_summary(); update_readiness();
            wardogs::log_info("settings.saved");
            const auto saved_notice = ocr_hold_
                ? wardogs::i18n::text(QStringLiteral("Настройки сохранены. Последний OCR не применён · введите или захватите координаты заново."))
                : wardogs::i18n::text(QStringLiteral("Настройки сохранены и применены"));
            set_status(saved_notice + (hotkey_changes.isEmpty() ? QString{} :
                wardogs::i18n::text(QStringLiteral(". Занятые сочетания заменены: ")) + hotkey_changes));
        } catch (const std::exception& error) {
            try { auto restored = previous; register_hotkeys(restored, false); register_mouse_trigger(previous); }
            catch (const std::exception& rollback_error) {
                wardogs::log_error("settings.rollback_listener_failed error=" + std::string(rollback_error.what()));
            }
            set_status(wardogs::i18n::text(QStringLiteral("Настройки не сохранены: ")) + error_text(error), true);
        }
    }

    void unregister_hotkeys() {
        hotkey_listener_.stop();
    }

    void unlock_pinned_window(const char* source) {
        if (!pinned_mode_ || !pinned_window_ || !pinned_window_->is_locked()) {
            wardogs::log_info(std::string("pinned.unlock_ignored source=") +
                              source);
            return;
        }
        wardogs::log_info(std::string("pinned.unlocked source=") + source);
        pinned_window_->set_locked(false);
    }

    void restore_current_hotkeys() {
        const auto changes = register_hotkeys(settings_);
        update_action_labels();
        if (pinned_window_)
            pinned_window_->configure_unlock_hotkey(settings_.pinned_card.unlock_hotkey);
        QString notice;
        bool notice_error = false;
        if (!changes.isEmpty()) {
            try {
                if (!diagnostic_) wardogs::save_settings(settings_);
                notice = wardogs::i18n::text(QStringLiteral("Занятые сочетания заменены и сохранены: ")) + changes;
            } catch (const std::exception& error) {
                notice_error = true;
                notice = wardogs::i18n::text(QStringLiteral("Свободные сочетания действуют в этом сеансе: ")) + changes +
                         wardogs::i18n::text(QStringLiteral(". Сохранить не удалось: ")) + error_text(error);
            }
        }
        try { register_mouse_trigger(); }
        catch (const std::exception& error) {
            exit_game_mode();
            set_status(wardogs::i18n::text(QStringLiteral("Клавиши восстановлены")) +
                (changes.isEmpty() ? QString{} : QStringLiteral(": ") + changes) +
                wardogs::i18n::text(QStringLiteral(". Режим игры завершён: обработчик средней кнопки недоступен. ")) +
                error_text(error), true);
            return;
        }
        if (!notice.isEmpty()) set_status(notice, notice_error);
    }

    QString register_hotkeys(wardogs::AppSettings& settings, bool recover_conflicts = true) {
        if (diagnostic_) return {};
        if (!settings.game_integration_enabled) {
            unregister_hotkeys();
            mouse_listener_.stop();
            wardogs::log_info("game_integration.disabled hotkeys=0 mouse=0");
            return {};
        }
        const std::array values{
            wardogs::parse_hotkey(settings.region_hotkey),
            wardogs::parse_hotkey(settings.base_hotkey),
            wardogs::parse_hotkey(settings.target_hotkey),
            wardogs::parse_hotkey(settings.quick_target_hotkey),
            wardogs::parse_hotkey(settings.impact_hotkey),
            wardogs::parse_hotkey(settings.ghost_arc_hotkey),
            wardogs::parse_hotkey(settings.pinned_card.unlock_hotkey),
            wardogs::parse_hotkey(settings.exit_game_mode_hotkey)};
        wardogs::validate_unique_hotkeys(values);
        {
            std::ostringstream diagnostic;
            diagnostic << "hotkey.configure";
            for (std::size_t index = 0; index < values.size(); ++index) {
                diagnostic << " key" << index << '='
                           << one_line_utf8(qtext(values[index].display))
                           << "(vk=0x" << std::hex << std::uppercase
                           << values[index].virtual_key << ",mod=0x"
                           << values[index].modifiers << std::dec << ')';
            }
            wardogs::log_info(diagnostic.str());
        }
        const QPointer<MainWindow> self(this);
        const auto callback = [self](std::size_t index) {
            if (!self) return;
            QMetaObject::invokeMethod(self, [self, index] {
                if (!self) return;
                auto* const window = self.data();
                window->handle_hotkey(index);
            }, Qt::QueuedConnection);
        };
        if (!recover_conflicts) {
            hotkey_listener_.start(values, callback);
            return {};
        }
        const auto actual = hotkey_listener_.start_with_conflict_fallback(values, callback);
        try {
            const auto changes = apply_registered_hotkeys(settings, actual);
            if (!changes.isEmpty())
                wardogs::log_info("hotkey.conflicts_resolved changes=" + utf8(changes));
            return changes;
        }
        catch (...) { hotkey_listener_.stop(); throw; }
    }

    void handle_hotkey(std::size_t index) {
                if (closing_ || !settings_.game_integration_enabled || QApplication::activeModalWidget()) return;
                if (index < 6 && !isActiveWindow() && !wardogs_is_foreground()) {
                    wardogs::log_info("hotkey.ignored foreground_not_game_or_own");
                    return;
                }
                wardogs::log_info("hotkey.handle index=" +
                                  std::to_string(index) + " active_window=" +
                                  (isActiveWindow() ? "1" : "0") +
                                  " visible=" + (isVisible() ? "1" : "0") +
                                  " pinned=" + (pinned_mode_ ? "1" : "0") +
                                  " " + foreground_summary());
                if (index == 0) begin_region_setup();
                else if (index == 1) start_ocr(OcrAction::base);
                else if (index == 2) start_ocr(OcrAction::target);
                else if (index == 3) begin_quick_target();
                else if (index == 4) start_ocr(OcrAction::calibration_impact);
                else if (index == 5) toggle_ghost_arc();
                else if (index == 6) unlock_pinned_window("hotkey");
                else if (index == 7) exit_game_mode();
    }
};

constexpr auto style_sheet = R"(
QWidget { color:#dbe4ef; font-size:13px; }
QMainWindow,QDialog { background:#0b1018; }
QScrollArea#mainContentScroll { background:#0b1018; border:0; }
QWidget#mainContent,QWidget#mainContentViewport { background:#0b1018; }
QFrame#appFrame { background:#0b1018; border:3px solid transparent;
                  border-radius:9px; }
QFrame#appFrame[error="true"] { border-color:#ef4444; }
QWidget#windowTitleBar { background:#101821; border:0; }
QLabel#windowTitleText { color:#cbd5e1; font-size:12px; font-weight:500; }
QToolButton[windowControl="true"] { background:transparent; border:0;
    border-radius:7px; padding:0; }
QToolButton[windowControl="true"]:hover { background:#1d2a3b; }
QToolButton[windowControl="true"]:pressed { background:#263750; }
QToolButton[closeControl="true"]:hover { background:#c42b1c; }
QToolButton[closeControl="true"]:pressed { background:#a92317; }
QFrame#pinnedFrame { background:#0f172a; border:3px solid transparent;
                     border-radius:10px; }
QFrame#pinnedFrame[error="true"] { border-color:#ef4444; }
QFrame#pinnedFrame QFrame#resultCard,
QFrame#pinnedFrame QFrame#vehicleSolutionCard {
    background:#0b1220; border:1px solid #334155; border-radius:8px;
}
QWidget#pinnedContextMenu { background:transparent; }
QLabel#pinnedUnlockLabel { color:#8190a3; font-size:11px; }
QKeySequenceEdit#pinnedUnlockHotkey { background:#0b1220; border:1px solid transparent;
    border-radius:7px; padding:5px 7px; font-size:12px; }
QKeySequenceEdit#pinnedUnlockHotkey:focus { border-color:#3569ae; }
QToolButton[pinnedMenuButton="true"] { background:transparent; border:0;
    border-radius:9px; padding:5px; }
QToolButton[pinnedMenuButton="true"]:hover { background:#1e293b; }
QToolButton[pinnedMenuButton="true"]:checked { background:#1e3e75; }
QToolButton[pinnedMenuButton="true"]:checked:hover { background:#254b8c; }
QSlider[pinnedMenuSlider="true"]::groove:horizontal { height:5px; background:#334155;
    border-radius:2px; }
QSlider[pinnedMenuSlider="true"]::sub-page:horizontal { background:#38bdf8;
    border-radius:2px; }
QSlider[pinnedMenuSlider="true"]::handle:horizontal { background:#e2e8f0;
    border:1px solid #64748b; width:15px; margin:-6px 0; border-radius:7px; }
QSlider[pinnedMenuSlider="true"]::handle:horizontal:hover { background:#f8fafc;
    border-color:#38bdf8; }
QLabel#title,QLabel#dialogTitle { color:#f4f7fb; font-size:25px; font-weight:700; }
QLabel#dialogTitle { font-size:22px; }
QLabel#muted { color:#8190a3; }
QLabel#status { color:#75c9e8; padding:6px 4px 2px 4px; }
QLabel#status[error="true"] { color:#fca5a5; }
QLabel#resultCaption { color:#8190a3; font-size:12px; font-weight:600; }
QLabel#rawResult { color:#66768a; font-size:12px; padding:3px; }
QFrame#resultCard { background:#0d1521; border:0; border-radius:10px; }
QFrame#vehicleSolutionCard { background:#0d1521; border:0; border-radius:10px; }
QFrame#vehicleSolutionCard[unavailable="true"] { background:#171b25; }
QLabel#solutionArc { color:#8291a5; font-size:12px; font-weight:600; }
QLabel#solutionMetricCaption { color:#b5c5dc; font-size:11px; font-weight:600; }
QLabel#solutionDistance,QLabel#solutionBearing,QLabel#solutionMil {
    font-family:"Bahnschrift"; font-size:25px; font-weight:700; }
QLabel#solutionDistance { color:#fbbf24; }
QLabel#solutionBearing { color:#67e8f9; }
QLabel#solutionMil { color:#c4b5fd; }
QLabel#solutionMil[unavailable="true"] { color:#fca5a5; font-size:19px; }
QGroupBox#vehicleResultGroup[error="true"] { border:2px solid #ef4444; }
QGroupBox { background:#111925; border:0; border-radius:12px;
            margin-top:0; padding-top:0; font-weight:500; }
QLineEdit,QPlainTextEdit,QKeySequenceEdit,QComboBox,QDoubleSpinBox { background:#0c1420;
    border:1px solid transparent; border-radius:8px; padding:8px 9px; color:#f3f6fa;
    selection-background-color:#2563eb; }
QLineEdit:focus,QPlainTextEdit:focus,QKeySequenceEdit:focus,QComboBox:focus,QDoubleSpinBox:focus {
    border-color:#3569ae;
}
QComboBox::drop-down { border:0; width:28px; }
QComboBox QAbstractItemView { background:#131d2b; border:0;
    color:#f3f6fa; selection-background-color:#1e3e75; padding:5px; }
QPushButton { background:#192638; border:0; border-radius:8px;
              padding:8px 11px; min-height:18px; outline:0; }
QPushButton:hover { background:#23344a; }
QPushButton:pressed { background:#182f55; }
QPushButton[quiet="true"] { background:#151f2e; color:#c8d3df; }
QPushButton[quiet="true"]:hover { background:#202f43; color:#f2f6fb; }
QPushButton[primary="true"] { background:#1e3e75; color:#f7f9fc; }
QPushButton[primary="true"]:hover { background:#285297; }
QPushButton#arcToggle { min-width:58px; padding-left:8px; padding-right:8px;
    background:#0c1420; color:#9aa8b8; }
QPushButton#arcToggle:hover { background:#192638; color:#e6edf5; }
QPushButton#arcToggle[highlighted="true"] { background:#1e3e75; color:#f8fafc;
    font-weight:700; }
QPushButton#arcToggle[highlighted="true"]:hover { background:#285297; }
QPushButton#arcToggle[highlighted="true"]:pressed { background:#183563; }
QPushButton#iconButton { background:transparent; border:0;
                         border-radius:9px; padding:6px; min-height:0; }
QPushButton#iconButton:hover { background:#192638; }
QPushButton#iconButton:pressed { background:#1e3e75; }
QPushButton#iconButton[highlighted="true"] { background:#1e3e75; color:#f8fafc; }
QPushButton:disabled { color:#667487; background:#141c28; }
QToolTip { color:#eef3f8; background:#1a2636; border:0; padding:5px; }
QScrollBar:vertical { background:transparent; width:9px; margin:0; }
QScrollBar::handle:vertical { background:#334459; border-radius:4px; min-height:24px; }
QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical { height:0; }

QLabel#brandTitle { color:#edf4ff; font-family:"Bahnschrift"; font-size:31px; font-weight:700; letter-spacing:3px; }
QLabel#brandSubtitle { color:#8fa1ba; font-size:10px; letter-spacing:1px; }
QLabel#versionBadge { color:#63d8c5; background:#14292b; border:1px solid #224247; border-radius:8px; padding:6px 10px; font-size:10px; }
QWidget { font-family:"Segoe UI"; }
QGroupBox { background:#111b28; border:1px solid #213047; border-radius:12px; margin-top:8px; padding-top:7px; }
QGroupBox::title { subcontrol-origin:margin; left:16px; padding:0 5px; color:#8fa1ba; font-size:10px; font-weight:700; }
QLabel[metric="true"] { font-family:"Bahnschrift"; font-size:29px; font-weight:700; }
QLabel#mainMil[rangeError="true"] { font-size:15px; }
QLabel#baseSummary,QLabel#targetSummary { font-size:12px; color:#b5c5dc; padding-top:3px; }
QLabel#regionSummary,QLabel#ocrText { color:#8fa1ba; font-size:11px; }
QLabel#readiness { color:#b5c5dc; padding:5px 0; }
QLabel#gameTip { color:#8fa1ba; background:#101b28; border:1px solid #25364b; border-radius:12px; padding:16px; font-size:12px; }
QLabel#status { color:#63d8c5; background:#111b28; border:1px solid #213047; border-radius:10px; padding:12px 14px; font-size:12px; }
QLabel#status[error="true"] { color:#ffa09e; border-color:#6b3841; background:#251b25; }
QPushButton[primary="true"] { background:#63d8c5; color:#071b1a; font-weight:700; }
QPushButton[primary="true"]:hover { background:#81e7d7; }
QPushButton[primary="true"]:pressed { background:#42b8a7; }
QPushButton[primary="true"]:disabled { background:#213b3e; color:#708b91; }
QPushButton { border:1px solid #2b3b50; min-height:22px; }
QPushButton:hover { background:#253b50; border-color:#3e5c70; }
QPushButton:checked { background:#214d4b; border-color:#63d8c5; }
QPushButton#weaponButton { background:#1c2a3d; border-color:#3a4e65; color:#f0b45d; }
QPushButton#helpButton,QPushButton#settingsButton { background:#152234; padding:0; }
QToolButton#calibrationToggle { color:#b5c5dc; background:#152234; border:1px solid #2b3b50; border-radius:8px; padding:10px; }
QToolButton#calibrationToggle:checked { color:#63d8c5; border-color:#326c65; }
QToolButton#manualControlsToggle { color:#b5c5dc; background:#152234; border:1px solid #2b3b50; border-radius:8px; padding:9px 12px; }
QToolButton#manualControlsToggle:hover, QToolButton#manualControlsToggle:checked { color:#63d8c5; border-color:#326c65; }
QLineEdit:focus,QPlainTextEdit:focus,QKeySequenceEdit:focus,QComboBox:focus,QDoubleSpinBox:focus { border-color:#63d8c5; }
QLineEdit { min-height:22px; border-color:#29374b; }
QTabWidget::pane { border:1px solid #2b3b50; border-radius:8px; background:#111b28; }
QTabBar::tab { background:#152234; color:#8fa1ba; padding:10px 17px; border:0; margin:2px; border-radius:6px; }
QTabBar::tab:selected { background:#244b4b; color:#63d8c5; }
QSpinBox { background:#0c1420; color:#e8eef7; border:1px solid #2b3b50; border-radius:8px; padding:7px; }
QMenu { background:#111b28; border:1px solid #34445a; padding:6px; }
QMenu::item { padding:8px 22px; border-radius:5px; }
QMenu::item:selected { background:#244b4b; }
)";

}  // namespace

int run_application(int argc, char* argv[]) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    struct ApartmentScope {
        ApartmentScope() { winrt::init_apartment(winrt::apartment_type::single_threaded); }
        ~ApartmentScope() { winrt::uninit_apartment(); }
    } apartment;
    QApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("WardogsFireControl"));
    app.setApplicationDisplayName(QStringLiteral("WARDOGS Fire Control"));
    app.setApplicationVersion(QStringLiteral(WARDOGS_VERSION));
    app.setWindowIcon(wardogs_application_icon());
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QFont font(QStringLiteral("Segoe UI"));
    font.setPixelSize(13);
    app.setFont(font);
    app.setStyleSheet(QString::fromUtf8(style_sheet));
    const auto arguments = app.arguments();
    auto argument_value = [&](const QString& flag) {
        for (const auto& argument : arguments)
            if (argument.startsWith(flag + QLatin1Char('='))) return argument.mid(flag.size() + 1);
        return QString{};
    };
    QString snapshot_path, snapshot_mode;
    for (const auto& mode : {QStringLiteral("ui"), QStringLiteral("manual-ui"), QStringLiteral("compact-ui"), QStringLiteral("pinned-ui"), QStringLiteral("review-ui"),
                            QStringLiteral("settings-ui"), QStringLiteral("tutorial-ui"), QStringLiteral("notice-ui"),
                            QStringLiteral("vehicle-ui"), QStringLiteral("vehicle-pinned-ui"),
                            QStringLiteral("calibration-ui"), QStringLiteral("recognition"),
                            QStringLiteral("recognition-bottom"), QStringLiteral("tutorial-bottom"),
                            QStringLiteral("pinned-menu-ui"), QStringLiteral("reticle-ui"), QStringLiteral("selection-ui"),
                            QStringLiteral("recognition-hotkeys"), QStringLiteral("recognition-reticle"),
                            QStringLiteral("review-bottom-ui"), QStringLiteral("manual-bottom-ui"),
                            QStringLiteral("folder-ui"), QStringLiteral("error-ui"),
                            QStringLiteral("planning-ui"), QStringLiteral("planning-positions-ui"),
                            QStringLiteral("planning-times-ui"), QStringLiteral("planning-profiles-ui"),
                            QStringLiteral("fire-control-ui")}) {
        const auto flag = QStringLiteral("--") + mode + QStringLiteral("-snapshot");
        QString path = argument_value(flag);
        if (path.isEmpty()) {
            QString variable = mode.toUpper();
            variable.replace(QLatin1Char('-'), QLatin1Char('_'));
            path = qEnvironmentVariable((QStringLiteral("WARDOGS_") + variable + QStringLiteral("_SNAPSHOT")).toLatin1().constData());
        }
        if (!path.isEmpty()) {
            snapshot_path = path; snapshot_mode = mode;
            snapshot_mode.remove(QStringLiteral("-ui"));
            if (snapshot_mode == QStringLiteral("manual")) snapshot_mode = QStringLiteral("manual");
            break;
        }
    }
    const QString test_path = argument_value(QStringLiteral("--self-test"));
    bool valid_snapshot_delay = false;
    const int requested_snapshot_delay = argument_value(QStringLiteral("--snapshot-delay-ms")).toInt(&valid_snapshot_delay);
    const int snapshot_delay = valid_snapshot_delay ? std::clamp(requested_snapshot_delay, 100, 10000) : 100;
    const bool diagnostic = !snapshot_path.isEmpty() || !test_path.isEmpty();
    wardogs::i18n::set_language(argument_value(QStringLiteral("--language")) == QStringLiteral("en")
        ? wardogs::UiLanguage::english : wardogs::UiLanguage::russian);
    if (arguments.contains(QStringLiteral("--help"))) {
        QMessageBox::information(nullptr, QStringLiteral("WARDOGS Fire Control"),
            wardogs::i18n::text(QStringLiteral("Двойной щелчок запускает приложение.\n--unlock-pinned: снять блокировку карточки.\n"
                           "--ui-snapshot=путь.png: снимок окна без изменения профиля.\n"
                           "--self-test=путь.json: диагностические проверки.\n"
                           "--test-image=путь.png: дополнительный OCR-тест.")));
        return 0;
    }
    if (!diagnostic && arguments.contains(QStringLiteral("--unlock-pinned"))) {
        const bool signaled = wardogs_ui::signal_pinned_unlock_event();
        return signaled ? 0 : 1;
    }
    struct HandleOwner {
        HANDLE value{};
        ~HandleOwner() { if (value) CloseHandle(value); }
    } instance;
    if (!diagnostic) {
        instance.value = CreateMutexW(nullptr, FALSE, L"Local\\SoNiX.WardogsFireControl.Instance");
        if (!instance.value) {
            QMessageBox::critical(nullptr, wardogs::i18n::text(QStringLiteral("Не удалось запустить")), wardogs::i18n::text(QStringLiteral("Windows не разрешила создать экземпляр приложения.")));
            return 1;
        }
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            HandleOwner show{OpenEventW(EVENT_MODIFY_STATE, FALSE, L"Local\\SoNiX.WardogsFireControl.Show")};
            if (show.value) SetEvent(show.value);
            return 0;
        }
        wardogs_ui::configure_taskbar_identity();
    }
    try {
        const auto log_path = diagnostic
            ? std::filesystem::path{QFileInfo(test_path.isEmpty() ? snapshot_path : test_path).absolutePath().toStdWString()} / L"diagnostic.log"
            : wardogs::settings_path().parent_path() / L"logs" / L"latest.log";
        const bool logging_started = wardogs::initialize_session_log(log_path, WARDOGS_VERSION);
        if (!logging_started) {
            if (diagnostic) return 1;
            QMessageBox::warning(nullptr, wardogs::i18n::text(QStringLiteral("Диагностика")),
                wardogs::i18n::text(QStringLiteral("Журнал недоступен. Проверьте доступ к папке приложения в профиле Windows.")) +
                QStringLiteral("\n") + QString::fromStdWString(log_path.wstring()));
        }
        wardogs::log_info("application.initialized version=" WARDOGS_VERSION);
        wardogs::log_info("application.context mode=" + std::string(diagnostic ? "diagnostic" : "user") +
            " executable=" + QCoreApplication::applicationFilePath().toUtf8().toStdString() +
            " started_utc=" + QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toStdString());
        MainWindow window(diagnostic);
        // Publish even an unfinished capture or a quiet session once per second.
        // Report a later disk/rotation failure once, without blocking input.
        QTimer log_timer(&window);
        bool log_failure_reported = !logging_started;
        QObject::connect(&log_timer, &QTimer::timeout, &window, [&] {
            if (log_failure_reported || wardogs::flush_session_log()) return;
            log_failure_reported = true;
            if (diagnostic) { app.exit(1); return; }
            auto* warning = new QMessageBox(QMessageBox::Warning,
                wardogs::i18n::text(QStringLiteral("Диагностика")),
                wardogs::i18n::text(QStringLiteral("Запись журнала остановлена. Новые действия не сохраняются. Проверьте свободное место и доступ к папке журналов, затем перезапустите программу.")) +
                QStringLiteral("\n") + QString::fromStdWString(log_path.wstring()),
                QMessageBox::Ok, &window);
            warning->setAttribute(Qt::WA_DeleteOnClose);
            warning->setWindowModality(Qt::NonModal);
            warning->show();
        });
        log_timer.start(1000);
        if (!diagnostic) wardogs::log_info(std::string("taskbar.action success=") +
            (wardogs_ui::install_unlock_jump_list_task() ? "1" : "0"));
        window.show();
        if (!snapshot_path.isEmpty()) {
            QTimer::singleShot(snapshot_delay, &window, [&] {
                const bool success = window.export_snapshot(snapshot_mode, snapshot_path);
                app.exit(success ? 0 : 1);
            });
        } else if (!test_path.isEmpty()) {
            QTimer::singleShot(50, &window, [&] {
                try {
                    const auto receipt = window.run_self_test(argument_value(QStringLiteral("--test-image")),
                        arguments.contains(QStringLiteral("--test-capture-window")),
                        argument_value(QStringLiteral("--capture-fixture-pid")).toUInt());
                    const QFileInfo file(test_path);
                    QDir().mkpath(file.absolutePath());
                    QSaveFile output(file.absoluteFilePath());
                    const auto bytes = QJsonDocument(receipt).toJson(QJsonDocument::Indented);
                    const bool written = output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size() && output.commit();
                    app.exit(written && receipt.value(QStringLiteral("passed")).toBool() ? 0 : 1);
                } catch (const std::exception& error) {
                    wardogs::log_error("diagnostic.failed error=" + std::string(error.what()));
                    app.exit(1);
                }
            });
        }
        const int result = app.exec();
        wardogs::log_info("application.exit code=" + std::to_string(result));
        const bool logging_complete = wardogs::flush_session_log();
        wardogs::shutdown_session_log();
        return diagnostic && !logging_complete ? 1 : result;
    } catch (const std::exception& error) {
        QMessageBox::critical(nullptr, wardogs::i18n::text(QStringLiteral("Не удалось запустить")), error_text(error));
        wardogs::shutdown_session_log();
        return 1;
    }
}
