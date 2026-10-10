#include "pinned_result_window.hpp"
#include "app_icon.hpp"
#include "ghost_reticle_window.hpp"
#include "settings_dialog.hpp"
#include "selection_overlay.hpp"
#include "vehicle_solution_widget.hpp"
#include "localization.hpp"
#include "window_title_bar.hpp"
#include "wardogs/presentation.hpp"

#include <Windows.h>

#include <QApplication>
#include <QContextMenuEvent>
#include <QComboBox>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFontMetrics>
#include <QHoverEvent>
#include <QImage>
#include <QKeySequenceEdit>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QMouseEvent>
#include <QSlider>
#include <QSpinBox>
#include <QTabWidget>
#include <QTemporaryFile>
#include <QToolButton>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QScreen>
#include <QTimer>
#include <QWindow>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

struct SettingsSaveAttempt {
    bool accepted{};
    int warning_count{};
    QString warning_title;
};

SettingsSaveAttempt save_settings_dialog(SettingsDialog& dialog) {
    auto* buttons = dialog.findChild<QDialogButtonBox*>();
    auto* save = buttons ? buttons->button(QDialogButtonBox::Save) : nullptr;
    check(save != nullptr, "settings validation uses the real Save button");
    SettingsSaveAttempt attempt;
    QElapsedTimer deadline;
    deadline.start();
    QTimer close_warning;
    close_warning.setInterval(10);
    QObject::connect(&close_warning, &QTimer::timeout, &dialog, [&] {
        check(deadline.elapsed() < 2000,
              "a settings validation warning must never block the UI test indefinitely");
        for (auto* warning : dialog.findChildren<QMessageBox*>()) {
            if (!warning->isVisible()) continue;
            ++attempt.warning_count;
            attempt.warning_title = warning->windowTitle();
            warning->accept();
        }
    });
    dialog.show();
    QApplication::processEvents();
    close_warning.start();
    save->click();
    close_warning.stop();
    attempt.accepted = dialog.result() == QDialog::Accepted;
    dialog.hide();
    return attempt;
}

void check_coordinate_pattern_settings() {
    wardogs::AppSettings settings;
    settings.game_integration_enabled = false;
    settings.middle_mouse_enabled = false;
    settings.ghost_reticle.bearing_compensation_deg = 0.123456789012345;
    const std::wstring canonical{wardogs::default_ocr_coordinate_pattern};
    const QString qt_canonical = QString::fromStdWString(canonical);
    check(QString::fromUtf8(qt_canonical.toUtf8()).toStdWString() == canonical,
          "QString UTF-16 and UTF-8 round trips preserve the fullwidth default separators");
    SettingsDialog standard(settings);
    auto* standard_editor = standard.findChild<QPlainTextEdit*>(QStringLiteral("coordinatePattern"));
    check(standard_editor && standard_editor->toPlainText().toStdWString() == canonical,
          "QPlainTextEdit preserves the canonical built-in pattern without inserting visual line wraps");
    const auto standard_save = save_settings_dialog(standard);
    check(standard_save.accepted && standard_save.warning_count == 0 &&
              standard.settings().coordinate_pattern == canonical &&
              standard.settings().ghost_reticle.bearing_compensation_deg == settings.ghost_reticle.bearing_compensation_deg,
          "a default settings dialog saves the real canonical pattern without a warning");

    std::wstring damaged = canonical;
    std::replace(damaged.begin(), damaged.end(), L'，', L',');
    std::replace(damaged.begin(), damaged.end(), L'；', L';');
    settings.coordinate_pattern = damaged;
    SettingsDialog legacy(settings);
    const auto legacy_save = save_settings_dialog(legacy);
    check(legacy_save.accepted && legacy_save.warning_count == 0 &&
              legacy.settings().coordinate_pattern == canonical,
          "an ANSI best-fit copy of the built-in pattern saves and returns the canonical Unicode form");

    settings.coordinate_pattern = LR"(x(.*)y(.*))";
    SettingsDialog reset(settings);
    auto* reset_button = reset.findChild<QPushButton*>(QStringLiteral("resetCoordinatePattern"));
    check(reset_button != nullptr, "the coordinate pattern recovery button is available");
    reset_button->click();
    const auto reset_save = save_settings_dialog(reset);
    check(reset_save.accepted && reset_save.warning_count == 0 &&
              reset.settings().coordinate_pattern == canonical,
          "Reset followed by Save recovers an arbitrary invalid pattern through the real UI path");

    const std::wstring safe_custom{LR"(A\s+([\d.]+)\s+B\s+([\d.]+))"};
    settings.coordinate_pattern = safe_custom;
    SettingsDialog custom(settings);
    const auto custom_save = save_settings_dialog(custom);
    check(custom_save.accepted && custom_save.warning_count == 0 &&
              custom.settings().coordinate_pattern == safe_custom,
          "a safe custom coordinate pattern survives settings Save without silent replacement");

    settings.coordinate_pattern = LR"(x(.*)y(.*))";
    SettingsDialog invalid(settings);
    const auto invalid_save = save_settings_dialog(invalid);
    check(!invalid_save.accepted && invalid_save.warning_count == 1 &&
              invalid_save.warning_title == QStringLiteral("Проверьте шаблон"),
          "arbitrary unsafe custom patterns remain rejected with a bounded template warning");
}

void check_metric_geometry(QWidget& card, int row_count) {
    QApplication::processEvents();
    const auto fits_ancestors = [&](const QWidget* child) {
        for (auto* ancestor = child->parentWidget(); ancestor; ancestor = ancestor->parentWidget()) {
            if (!ancestor->rect().contains(QRect(child->mapTo(ancestor, QPoint{}), child->size())))
                return false;
            if (ancestor == &card) return true;
        }
        return false;
    };
    for (const auto* name : {"solutionDistance", "solutionBearing", "solutionMil", "solutionTableDistance"}) {
        const auto values = card.findChildren<QLabel*>(QString::fromLatin1(name));
        check(values.size() == row_count, "each displayed trajectory has all four metric values");
        for (const auto* value : values) {
            const auto text_width = QFontMetrics(value->font()).horizontalAdvance(value->text());
            if (text_width > value->width())
                std::cerr << "metric=" << name << " text=" << value->text().toStdString()
                          << " width=" << value->width() << " needs=" << text_width << '\n';
            check(value->isVisible() && text_width <= value->width() &&
                      QFontMetrics(value->font()).height() <= value->height(),
                  "native target metres, bearing degrees, set MIL and table estimate fit without truncation");
            check(card.rect().contains(QRect(value->mapTo(&card, QPoint{}), value->size())),
                  "each native metric is fully inside the shown solution card");
            check(value->parentWidget()->rect().contains(value->geometry()),
                  "each native metric fits its own column without overlapping adjacent columns");
            check(fits_ancestors(value),
                  "each native metric remains fully visible through every enclosing panel");
        }
    }
    const auto captions = card.findChildren<QLabel*>(QStringLiteral("solutionMetricCaption"));
    check(captions.size() == 4 * row_count, "all four captions are visible for every native row");
    for (const auto* caption : captions) {
        const auto text_width = QFontMetrics(caption->font()).horizontalAdvance(caption->text());
        const int required_height = caption->heightForWidth(caption->width());
        const QPoint mapped = caption->mapTo(&card, QPoint{});
        const bool inside = card.rect().contains(QRect(mapped, caption->size()));
        if (!caption->isVisible() || caption->height() < required_height || !inside)
            std::cerr << "caption=" << caption->text().toStdString()
                      << " size=" << caption->width() << 'x' << caption->height()
                      << " text_width=" << text_width << " required_height=" << required_height
                      << " minimum=" << caption->minimumWidth() << 'x' << caption->minimumHeight()
                      << " card=" << card.width() << 'x' << card.height()
                      << " mapped=" << mapped.x() << ',' << mapped.y()
                      << " column=" << caption->parentWidget()->width() << 'x' << caption->parentWidget()->height()
                      << " inside=" << inside << '\n';
        check(caption->isVisible() && caption->height() >= caption->heightForWidth(caption->width()) &&
                  card.rect().contains(QRect(caption->mapTo(&card, QPoint{}), caption->size())),
              "native metric captions fit at the tested card/font scale");
        check(caption->parentWidget()->rect().contains(caption->geometry()),
              "each native caption fits its own column without overlapping adjacent columns");
        check(fits_ancestors(caption),
              "each native caption remains fully visible through every enclosing panel");
        check(caption->minimumWidth() >= text_width &&
                  caption->minimumHeight() >= caption->heightForWidth(caption->minimumWidth()),
              "production short captions reserve their complete translated text and rendered height");
    }
}

void native_solution_geometry_tests() {
    check(QApplication::platformName() == QStringLiteral("windows"),
          "solution geometry acceptance uses real native Qt Windows rendering");
    for (const auto language : {wardogs::UiLanguage::russian, wardogs::UiLanguage::english}) {
        wardogs::i18n::set_language(language);
        VehicleSolutionWidget low(wardogs::Arc::low), high(wardogs::Arc::high);
        low.set_solution({wardogs::Arc::low, 188.4, 1391, 90}, 2221);
        high.set_solution({wardogs::Arc::high, 188.4, 2616, 673}, 2221);
        PinnedResultWindow mini([] {});
        mini.set_mode(true);
        mini.set_vehicle_values(low, high);
        mini.set_selected_arc(wardogs::Arc::high);
        mini.show();
        const QSize minimum = mini.minimumSize();
        for (double scale : {1.0, 1.5, 2.0}) {
            for (const double high_mil : {673.0, 1400.0}) {
                high.set_solution({wardogs::Arc::high, 203.0, 0,
                    high_mil}, high_mil == 1400.0 ? 735.0 : 2221.0);
                mini.set_vehicle_values(low, high);
                mini.set_selected_arc(wardogs::Arc::high);
                mini.resize(qRound(minimum.width() * scale), qRound(minimum.height() * scale));
                QApplication::processEvents();
                check_metric_geometry(mini, 2);
                for (auto* full : {&low, &high}) {
                    full->setStyleSheet(QStringLiteral(
                        "QLabel#solutionDistance,QLabel#solutionBearing,QLabel#solutionMil { font-family:'Segoe UI';font-size:%1px; }"
                        "QLabel#solutionMetricCaption { font-family:'Segoe UI';font-size:%2px; }")
                        .arg(qRound(25 * scale)).arg(qRound(11 * scale)));
                    full->ensurePolished();
                    full->resize(full->minimumSizeHint());
                    full->show();
                    QApplication::processEvents();
                    check_metric_geometry(*full, 1);
                    const auto* mil = full->findChild<QLabel*>(QStringLiteral("solutionMil"));
                    check(mil && mil->minimumWidth() >= QFontMetrics(mil->font()).horizontalAdvance(mil->text()),
                          "the production metric reserves its complete number and unit at every rendered font");
                    full->hide();
                }
            }
            std::cout << "Native solution geometry "
                      << (language == wardogs::UiLanguage::russian ? "RU" : "EN")
                      << " card/font scale=" << scale << " passed\n";
        }
        for (auto* full : {&low, &high}) {
            const auto* caption = full->findChild<QLabel*>(QStringLiteral("solutionMetricCaption"));
            check(caption != nullptr, "font scaling retains the production metric caption");
            const QSize enlarged = caption->minimumSize();
            full->setStyleSheet(QStringLiteral(
                "QLabel#solutionDistance,QLabel#solutionBearing,QLabel#solutionMil { font-family:'Segoe UI';font-size:25px; }"
                "QLabel#solutionMetricCaption { font-family:'Segoe UI';font-size:11px; }"));
            full->ensurePolished();
            full->resize(full->minimumSizeHint());
            full->show();
            QApplication::processEvents();
            check_metric_geometry(*full, 1);
            check(caption->minimumWidth() < enlarged.width() && caption->minimumHeight() < enlarged.height(),
                  "reducing the rendered font shrinks both caption dimensions without stale minimums");
            full->hide();
        }
        mini.hide();

        // Exercise the real mini-card border and workflow footer together.
        // Their padding is absent from the bare widget/font geometry cases.
        PinnedResultWindow framed([] {});
        framed.setStyleSheet(QStringLiteral(
            "QWidget {font-size:13px;}"
            "QFrame#pinnedFrame {border:3px solid transparent;}"
            "QFrame#pinnedFrame QFrame#vehicleSolutionCard {border:1px solid #334155;}"));
        framed.set_mode(true);
        framed.set_vehicle_values(low, high);
        framed.set_selected_arc(wardogs::Arc::high);
        framed.set_workflow_status(language == wardogs::UiLanguage::russian
            ? QStringLiteral("Навесная · Стрельбище · высот нет · рельеф не учтён · смена F4\nAlt+I у попадания — необязательная поправка.")
            : QStringLiteral("High arc · Firing range · no heights · terrain unavailable · F4\nAlt+I at the impact applies an optional correction."));
        framed.show();
        framed.resize(framed.minimumSize());
        QApplication::processEvents();
        check(!framed.hasHeightForWidth() && framed.layout()->hasHeightForWidth(),
              "the adaptive vehicle canvas retains child wrapping without exposing old-font native constraints");
        check_metric_geometry(framed, 2);
        const QSize minimum_canvas = framed.size();
        const QSize needed = framed.layout()->totalMinimumSize();
        check(needed.width() <= minimum_canvas.width() && needed.height() <= minimum_canvas.height(),
              "the production minimum includes both styled rows, frame padding and the workflow footer");
        framed.resize(minimum_canvas * 2);
        QApplication::processEvents();
        check_metric_geometry(framed, 2);
        check(framed.minimumWidth() <= minimum_canvas.width() && framed.minimumHeight() <= minimum_canvas.height(),
              "enlarging the font does not lock the card at the enlarged content minimum");
        const QSize old_font_constraint = QLayout::closestAcceptableSize(&framed, minimum_canvas);
        const int old_font_height_for_width = framed.layout()->minimumHeightForWidth(minimum_canvas.width());
        std::cout << "Adaptive styled mini-card "
                  << (language == wardogs::UiLanguage::russian ? "RU" : "EN")
                  << " requested=" << minimum_canvas.width() << 'x' << minimum_canvas.height()
                  << " old-font-layout=" << old_font_constraint.width() << 'x' << old_font_constraint.height()
                  << " native-height-for-width=" << framed.hasHeightForWidth() << '\n';
        framed.resize(minimum_canvas);
        QApplication::processEvents();
        if (framed.size() != minimum_canvas) {
            const auto* footer = framed.findChild<QLabel*>(QStringLiteral("pinnedWorkflowStatus"));
            const auto* handle = framed.windowHandle();
            const QSize native_minimum = handle ? handle->minimumSize() : QSize{};
            std::cerr << "styled mini-card requested=" << minimum_canvas.width() << 'x' << minimum_canvas.height()
                      << " actual=" << framed.width() << 'x' << framed.height()
                      << " minimum=" << framed.minimumWidth() << 'x' << framed.minimumHeight()
                      << " old-font-height-for-width=" << old_font_height_for_width
                      << " current-height-for-width=" << framed.layout()->minimumHeightForWidth(framed.width())
                      << " native-minimum=" << native_minimum.width() << 'x' << native_minimum.height()
                      << " footer=" << (footer ? footer->height() : -1) << '\n';
        }
        check(framed.size() == minimum_canvas,
              "an enlarged styled mini-card can return to its original compact size");
        check_metric_geometry(framed, 2);
        framed.set_workflow_status({});
        framed.resize(framed.minimumSize());
        QApplication::processEvents();
        check_metric_geometry(framed, 2);
        check(framed.height() < minimum_canvas.height(),
              "hiding the workflow footer releases only its reserved vertical space");
        framed.set_mode(false);
        check(framed.hasHeightForWidth() == framed.layout()->hasHeightForWidth(),
              "the mortar canvas retains Qt's existing height-for-width policy");
        framed.hide();
    }
    std::cout << "Native four-metric solution geometry passed\n" << std::flush;
}

void check_pinned_passive_geometry() {
    VehicleSolutionWidget low(wardogs::Arc::low), high(wardogs::Arc::high);
    low.set_solution({wardogs::Arc::low, 188.4, 1391, 90}, 2221);
    high.set_solution({wardogs::Arc::high, 188.4, 2616, 673}, 2221);
    PinnedResultWindow card([] {});
    card.set_mode(true);
    card.set_vehicle_values(low, high);
    card.set_selected_arc(wardogs::Arc::high);
    card.show();
    QApplication::processEvents();
    const auto available = QGuiApplication::primaryScreen()->availableGeometry();
    card.resize(std::min(720, available.width() - 40), std::min(240, available.height() - 40));
    card.move(available.topLeft() + QPoint(20, 20));
    QApplication::processEvents();
    const QRect before_data = card.geometry();
    low.set_solution({wardogs::Arc::low, 190.2, 1410, 92}, 2250);
    high.set_solution({wardogs::Arc::high, 190.2, 2620, 670}, 2250);
    card.set_vehicle_values(low, high);
    card.set_selected_arc(wardogs::Arc::low);
    card.set_error(true);
    card.set_error(false);
    QApplication::processEvents();
    check(card.geometry() == before_data,
          "new commands, selected arc and error styling preserve a user's roomy mini-card geometry");

    const QPoint origin = card.pos();
    card.set_context_caption(QStringLiteral("SPH-2 · Training · no heights"),
                             QStringLiteral("Nominal model; terrain, buildings and barrel height are unavailable."));
    QApplication::processEvents();
    check(card.pos() == origin && card.width() == before_data.width(),
          "persistent map and model context adds its own space without moving the mini-card");
    card.set_workflow_status(QStringLiteral("Reading coordinates; old guidance is hidden until confirmation."));
    QApplication::processEvents();
    check(card.pos() == origin && card.width() == before_data.width(),
          "a transient workflow message reserves its own height without moving the card");
    card.set_workflow_status({});
    card.set_context_caption({});
    card.hide();
    card.show();
    QApplication::processEvents();
    check(card.pos() == origin,
          "capture-style hide and show preserve the independently positioned mini-card");

    const QRect before_lock = card.geometry();
    card.set_locked(true);
    QApplication::processEvents();
    check(card.geometry() == before_lock && card.isVisible(),
          "locking a visible mini-card preserves its native position and canvas");
    auto* lock_hint = card.findChild<QLabel*>(QStringLiteral("pinnedLockHint"));
    check(lock_hint && lock_hint->isVisible() && lock_hint->text().contains(QStringLiteral("Ctrl+Alt+Q")),
          "a locked click-through card visibly explains the actual recovery shortcut");
    card.configure_unlock_hotkey(L"Ctrl+Alt+U");
    check(lock_hint->text().contains(QStringLiteral("Ctrl+Alt+U")) &&
              !lock_hint->text().contains(QStringLiteral("Ctrl+Alt+Q")) &&
              lock_hint->toolTip().contains(QStringLiteral("Ctrl+Alt+U")) &&
              lock_hint->accessibleDescription() == lock_hint->toolTip(),
          "replacing a recovery shortcut immediately updates the visible and accessible locked instruction");
    check(card.geometry() == before_lock,
          "updating the locked recovery instruction preserves the mini-card canvas");
    card.set_locked(false);
    QApplication::processEvents();
    check(card.geometry() == before_lock && card.isVisible(),
          "unlocking a visible mini-card preserves its native position and canvas");
    check(!lock_hint->isVisible(), "an unlocked card collapses the dedicated recovery instruction");
    card.set_opacity_percent(57);
    check(card.geometry() == before_lock,
          "changing mini-card opacity does not reposition or resize its results");
    card.set_always_on_top(false);
    QApplication::processEvents();
    check(card.geometry() == before_lock && card.isVisible(),
          "changing always-on-top native flags preserves a visible mini-card's geometry");
    card.set_always_on_top(true);

    low.set_waiting();
    high.set_waiting();
    card.set_vehicle_values(low, high);
    card.set_selected_arc(std::nullopt);
    QApplication::processEvents();
    for (auto* value : card.findChildren<QLabel*>(QStringLiteral("solutionMil")))
        check(value->text() == QStringLiteral("—"),
              "waiting mini-card rows cannot retain a previously displayed firing command");
    check(card.pos() == origin,
          "invalidating old guidance does not move the mini-card to the main window");
    card.set_mode(false);
    card.set_values(QStringLiteral("470 m"), QStringLiteral("180.0° S"), QStringLiteral("500 MIL"));
    card.set_mode(true);
    QApplication::processEvents();
    check(card.pos() == origin,
          "weapon changes preserve mini-card placement while each mode owns its canvas");
    card.hide();
}

void check_pinned_preferences_reconstruction() {
    const auto* screen = QGuiApplication::primaryScreen();
    const QRect available = screen->availableGeometry();
    PinnedResultWindow::Preferences saved;
    int commits = 0;
    PinnedResultWindow original([] {}, {}, [&](PinnedResultWindow::Preferences& preferences) {
        saved = preferences;
        ++commits;
        return true;
    });
    original.set_values(QStringLiteral("470 m"), QStringLiteral("180.0° S"), QStringLiteral("500 MIL"));
    original.set_context_caption(QStringLiteral("Training · no heights"),
                                 QStringLiteral("Nominal model; buildings are not checked."));
    original.prepare_for_show(QRect(available.topLeft() + QPoint(10, 10), QSize(200, 100)));
    original.show();
    original.resize(std::min(520, available.width() - 100), std::min(190, available.height() - 100));
    original.move(available.topLeft() + QPoint(20, 20));
    QApplication::processEvents();
    check(commits == 0 && original.flush_preferences() && commits == 0,
          "programmatic placement and layout do not overwrite a persisted user placement");

    const auto gesture = [](PinnedResultWindow& card, QPoint local, QPoint delta, bool resize) {
        const QRect before = card.geometry();
        const QPoint start = card.mapToGlobal(local);
        const QPoint finish = start + delta;
        // Qt 6's constructors update the pointing device's shared event point.
        // Construct and deliver each event before creating the next one, as a
        // real pointer does; preconstructing the batch corrupts its press point.
        bool press_accepted{}, motion_accepted{}, release_accepted{};
        {
            QMouseEvent press(QEvent::MouseButtonPress, QPointF(local), QPointF(start),
                              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(&card, &press);
            press_accepted = press.isAccepted();
            check(press.globalPosition().toPoint() == start && card.geometry() == before,
                  "the actual gesture press retains its starting pointer and canvas before motion");
        }
        {
            QMouseEvent motion(QEvent::MouseMove, QPointF(card.mapFromGlobal(finish)), QPointF(finish),
                               Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(&card, &motion);
            motion_accepted = motion.isAccepted();
            check(motion.globalPosition().toPoint() == finish,
                  "the actual gesture motion retains its own final global pointer coordinate");
        }
        const QRect after_motion = card.geometry();
        {
            QMouseEvent release(QEvent::MouseButtonRelease, QPointF(card.mapFromGlobal(finish)), QPointF(finish),
                                Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(&card, &release);
            release_accepted = release.isAccepted();
        }
        QApplication::processEvents();
        const QRect expected = resize
            ? QRect(before.topLeft(), before.size() + QSize(delta.x(), delta.y()))
            : before.translated(delta);
        if (!press_accepted || !motion_accepted || !release_accepted ||
            after_motion != expected || card.geometry() != expected) {
            const auto describe = [](const QRect& rect) {
                std::cerr << rect.x() << ',' << rect.y() << ',' << rect.width() << ',' << rect.height();
            };
            std::cerr << "Mini-card " << (resize ? "resize" : "drag") << " gesture: before=";
            describe(before);
            std::cerr << " motion=";
            describe(after_motion);
            std::cerr << " released=";
            describe(card.geometry());
            std::cerr << " expected=";
            describe(expected);
            std::cerr << " accepted=" << press_accepted << ',' << motion_accepted << ',' << release_accepted << '\n';
        }
        check(press_accepted && motion_accepted && release_accepted,
              "the unlocked card accepts each press, held-button motion and release of a real gesture");
        check(after_motion == expected && card.geometry() == expected,
              "each actual user gesture changes exactly its intended position or bottom-right canvas size");
    };
    gesture(original, QPoint(original.width() / 2, original.height() / 2), QPoint(24, 18), false);
    gesture(original, QPoint(original.width() - 2, original.height() - 2), QPoint(28, 14), true);
    const bool flushed = original.flush_preferences();
    if (!flushed || commits == 0 || !saved.placement || !saved.mode_sizes[0])
        std::cerr << "Mini-card placement flush: screen='" << screen->name().toStdString()
                  << "' available=" << available.x() << ',' << available.y() << ','
                  << available.width() << ',' << available.height() << " geometry="
                  << original.x() << ',' << original.y() << ',' << original.width() << ',' << original.height()
                  << " flush=" << flushed << " commits=" << commits
                  << " placement=" << bool(saved.placement) << " mortar_size=" << bool(saved.mode_sizes[0]) << '\n';
    check(flushed, "a completed valid mini-card gesture can flush its pending settings without a save error");
    check(commits > 0 && saved.placement && saved.mode_sizes[0],
          "dragging and resizing flush an actual user placement and mortar canvas before exit");
    check(wardogs::valid_pinned_card_placement(*saved.placement) &&
              wardogs::valid_pinned_card_size(*saved.mode_sizes[0]),
          "a completed gesture persists a valid named screen placement and weapon canvas");
    const QRect moved_geometry = original.geometry();
    check(saved.placement->rect == wardogs::PinnedCardRect{moved_geometry.x(), moved_geometry.y(),
                                                         moved_geometry.width(), moved_geometry.height()},
          "saved placement matches the actual user gesture geometry in logical pixels");
    check(saved.placement->screen_id == screen->name().toStdWString(),
          "user placement retains the screen identity instead of assuming the primary origin");
    check(saved.mode_sizes[0]->height < moved_geometry.height(),
          "a saved weapon canvas excludes the separately measured context footer height");
    VehicleSolutionWidget low(wardogs::Arc::low), high(wardogs::Arc::high);
    low.set_solution({wardogs::Arc::low, 188.4, 1391, 90}, 2221);
    high.set_solution({wardogs::Arc::high, 188.4, 2616, 673}, 2221);
    original.set_mode(true);
    original.set_vehicle_values(low, high);
    original.set_selected_arc(wardogs::Arc::high);
    gesture(original, QPoint(original.width() - 2, original.height() - 2), QPoint(24, 18), true);
    check(original.flush_preferences() && saved.mode_sizes[0] && saved.mode_sizes[1],
          "user geometry stores both weapon canvases without conflating their content heights");
    const QSize vehicle_canvas = original.size();
    original.set_mode(false);
    check(original.geometry() == moved_geometry,
          "returning to the mortar restores its independently resized canvas and keeps its placement");
    original.configure_unlock_hotkey(L"Ctrl+Alt+U");
    original.set_always_on_top(false);
    original.set_opacity_percent(63);
    original.set_locked(true);
    check(original.flush_preferences(), "pending geometry can be flushed alongside ordinary card preferences");
    original.hide();

    PinnedResultWindow restored([] {}, saved, {});
    restored.set_values(QStringLiteral("470 m"), QStringLiteral("180.0° S"), QStringLiteral("500 MIL"));
    restored.set_context_caption(QStringLiteral("Training · no heights"),
                                 QStringLiteral("Nominal model; buildings are not checked."));
    restored.prepare_for_show(QRect(available.topLeft() + QPoint(150, 120), QSize(120, 80)));
    restored.show();
    QApplication::processEvents();
    check(restored.geometry() == moved_geometry && restored.is_locked() && !restored.always_on_top() &&
              restored.opacity_percent() == 63,
          "a new card reconstructs the saved user's geometry and preferences independently of the main anchor");
    auto* restored_hint = restored.findChild<QLabel*>(QStringLiteral("pinnedLockHint"));
    check(restored_hint && restored_hint->isVisible() &&
              restored_hint->text().contains(QStringLiteral("Ctrl+Alt+U")),
          "a reconstructed locked card exposes the saved custom recovery shortcut");
    restored.hide();
    restored.prepare_for_show(QRect(available.topLeft() + QPoint(200, 160), QSize(100, 90)));
    restored.show();
    QApplication::processEvents();
    check(restored.geometry() == moved_geometry,
          "re-entering the same restored card retains its live placement after the main window moves");
    restored.set_mode(true);
    restored.set_vehicle_values(low, high);
    restored.set_selected_arc(wardogs::Arc::high);
    QApplication::processEvents();
    check(restored.pos() == moved_geometry.topLeft() && restored.size() == vehicle_canvas,
          "a reconstructed card restores the separately saved SPH-2 canvas at the same user placement");
    restored.hide();
}

}  // namespace

int main(int argc, char* argv[]) {
    const bool native_geometry = argc == 2 && std::string_view(argv[1]) == "--solution-geometry-native";
    // Qt 6.8's default offscreen screen has no name. Give this isolated
    // fixture a real backend identity while retaining all default geometry,
    // DPI and frame behavior, so persistence uses the same validation as Windows.
    // A relative basename also avoids ':' splitting a Windows drive in QPA args.
    QTemporaryFile offscreen_config(QStringLiteral("wardogs-offscreen-XXXXXX.json"));
    if (native_geometry) {
        qputenv("QT_QPA_PLATFORM", "windows");
    } else {
        check(offscreen_config.open(), "the isolated offscreen fixture can create its temporary backend configuration");
        const QByteArray config = R"({"synchronousWindowSystemEvents":false,"windowFrameMargins":true,
            "screens":[{"name":"pinned-fixture-screen","x":0,"y":0,"width":800,"height":800,
            "logicalDpi":96,"logicalBaseDpi":96,"dpr":1.0}]})";
        check(offscreen_config.write(config) == config.size() && offscreen_config.flush(),
              "the isolated offscreen fixture writes its complete named-screen configuration");
        const QByteArray backend = QByteArray("offscreen:configfile=") +
            QFileInfo(offscreen_config.fileName()).fileName().toUtf8();
        offscreen_config.close();
        check(qputenv("QT_QPA_PLATFORM", backend), "the isolated fixture selects the configured offscreen backend");
    }
    // The offscreen backend otherwise uses its fixed-width fallback glyphs,
    // unlike the Windows backend used by the application. Measure the same
    // installed fonts so minimum-width assertions reflect the actual UI.
    wchar_t windows_directory[MAX_PATH]{};
    const auto windows_length = GetWindowsDirectoryW(windows_directory, MAX_PATH);
    check(windows_length > 0 && windows_length < MAX_PATH,
          "the UI fixture locates the Windows font directory");
    qputenv("QT_QPA_FONTDIR", (QString::fromWCharArray(windows_directory) +
                              QStringLiteral("\\Fonts")).toUtf8());
    QApplication app(argc, argv);
    if (!native_geometry)
        check(QGuiApplication::primaryScreen() &&
                  QGuiApplication::primaryScreen()->name() == QStringLiteral("pinned-fixture-screen") &&
                  QGuiApplication::primaryScreen()->geometry() == QRect(0, 0, 800, 800),
              "the real offscreen backend applies only the fixture's screen identity to its default canvas");
    if (native_geometry) {
        native_solution_geometry_tests();
        check_pinned_passive_geometry();
        check_pinned_preferences_reconstruction();
        return 0;
    }
    check_pinned_passive_geometry();
    check_pinned_preferences_reconstruction();
    check(!wardogs_application_icon().isNull(),
          "the application icon is available to windows and title bars");
    check_coordinate_pattern_settings();

    int cancelled_selections = 0;
    bool rejected_callback_called = false;
    SelectionOverlay selection;
    auto selection_cancelled = [&](std::optional<wardogs::CaptureRegion> region,
                                    QString error) {
        check(!region && error.isEmpty(), "cancellation does not invent a capture or error");
        ++cancelled_selections;
    };
    check(selection.begin(selection_cancelled), "native selection overlay starts");
    const HWND first_selection = GetCapture();
    check(first_selection != nullptr, "native selector owns its mouse capture");
    check(!selection.begin([&](auto, auto) { rejected_callback_called = true; }),
          "a repeated begin cannot replace the active selection callback");
    SendMessageW(first_selection, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(30, 30));
    SendMessageW(first_selection, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(60, 60));
    selection.cancel();
    check(cancelled_selections == 1 && !rejected_callback_called &&
              !IsWindow(first_selection) && GetCapture() == nullptr,
          "cancel releases capture, destroys the window, and calls only the original callback");
    selection.cancel();
    check(cancelled_selections == 1, "repeated cancellation is idempotent");
    check(selection.begin(selection_cancelled), "selection can restart after cancellation");
    const HWND second_selection = GetCapture();
    SendMessageW(second_selection, WM_MOUSEMOVE, 0, MAKELPARAM(100, 100));
    SendMessageW(second_selection, WM_LBUTTONUP, 0, MAKELPARAM(100, 100));
    check(cancelled_selections == 1 && IsWindow(second_selection),
          "a restarted selector has no stale drag from the previous selection");
    SendMessageW(second_selection, WM_KEYDOWN, VK_ESCAPE, 0);
    check(cancelled_selections == 2 && GetCapture() == nullptr,
          "Escape follows the same cancellation path");
    check(selection.begin(selection_cancelled), "selection restarts after Escape");
    SendMessageW(GetCapture(), WM_RBUTTONDOWN, 0, MAKELPARAM(30, 30));
    check(cancelled_selections == 3 && GetCapture() == nullptr,
          "right-click also releases capture and calls back once");
    check(selection.begin(selection_cancelled), "selection restarts after right-click");
    SendMessageW(GetCapture(), WM_CLOSE, 0, 0);
    check(cancelled_selections == 4 && GetCapture() == nullptr,
          "native close follows cancellation instead of leaving the caller in selection mode");
    bool destructor_callback_called = false;
    {
        SelectionOverlay temporary_selection;
        check(temporary_selection.begin([&](auto, auto) { destructor_callback_called = true; }),
              "a temporary selection can start");
    }
    check(!destructor_callback_called && GetCapture() == nullptr,
          "teardown releases capture without calling a closing owner's UI callback");

    wardogs::AppSettings crowded_settings;
    crowded_settings.ghost_reticle.width = 1110;
    SettingsDialog settings_dialog(crowded_settings);
    settings_dialog.show();
    QApplication::processEvents();
    const auto hotkey_editors = settings_dialog.findChildren<QKeySequenceEdit*>();
    check(hotkey_editors.size() == 7,
          "settings exposes seven configurable global hotkeys including game-mode exit");
    auto* settings_tabs = settings_dialog.findChild<QTabWidget*>(
        QStringLiteral("settingsTabs"));
    check(settings_tabs && settings_tabs->count() == 4 &&
              settings_tabs->tabText(0) == QStringLiteral("Основные") &&
              settings_tabs->tabText(1) == QStringLiteral("Клавиши") &&
              settings_tabs->tabText(2) == QStringLiteral("Прицел") &&
              settings_tabs->tabText(3) == QStringLiteral("Распознавание"),
          "settings groups the main workflow, shortcuts, reticle and recognition into focused pages");
    auto* middle_mouse = settings_dialog.findChild<QCheckBox*>(
        QStringLiteral("middleMouseEnabled"));
    auto* capture_delay = settings_dialog.findChild<QSpinBox*>(
        QStringLiteral("mouseCaptureDelay"));
    auto* exit_hotkey = settings_dialog.findChild<QKeySequenceEdit*>(
        QStringLiteral("exitGameModeHotkey"));
    auto* standalone = settings_dialog.findChild<QCheckBox*>(QStringLiteral("standaloneMode"));
    auto* automatic_chat = settings_dialog.findChild<QCheckBox*>(QStringLiteral("automaticChatRegion"));
    auto* workflow_steps = settings_dialog.findChild<QLabel*>(QStringLiteral("quickWorkflowSteps"));
    auto* base_hotkey = settings_dialog.findChild<QKeySequenceEdit*>(QStringLiteral("baseHotkey"));
    auto* backend_choice = settings_dialog.findChild<QComboBox*>(QStringLiteral("ocrBackend"));
    auto* coordinate_pattern = settings_dialog.findChild<QPlainTextEdit*>(QStringLiteral("coordinatePattern"));
    check(capture_delay && !capture_delay->isVisible() && workflow_steps && workflow_steps->isVisible() &&
              workflow_steps->text().contains(QStringLiteral("Alt+X")) &&
              base_hotkey && !base_hotkey->isVisible() && backend_choice && !backend_choice->isVisible() &&
              coordinate_pattern && !coordinate_pattern->isVisible() && standalone && standalone->isVisible() &&
              middle_mouse && middle_mouse->isVisible(),
          "the first settings view exposes the workflow and capture mode without advanced recognition controls");
    auto* pattern_details = settings_dialog.findChild<QToolButton*>(QStringLiteral("coordinatePatternDetails"));
    check(pattern_details && !pattern_details->isChecked(),
          "custom coordinate patterns start collapsed instead of competing with ordinary recognition settings");
    if (settings_tabs && pattern_details && coordinate_pattern && capture_delay) {
        settings_tabs->setCurrentIndex(3);
        QApplication::processEvents();
        check(capture_delay->isVisible() && backend_choice->isVisible() && !coordinate_pattern->isVisible(),
              "recognition settings expose the engine and retry delay while keeping the custom pattern secondary");
        const auto previous_pattern = coordinate_pattern->toPlainText();
        pattern_details->click();
        QApplication::processEvents();
        check(coordinate_pattern->isVisible() && coordinate_pattern->toPlainText() == previous_pattern,
              "opening custom settings reveals the existing pattern without changing its value");
        pattern_details->click();
        settings_tabs->setCurrentIndex(0);
    }
    check(automatic_chat && automatic_chat->isChecked(),
          "fresh profiles enable automatic coordinate search");
    automatic_chat->setChecked(false);
    check(!settings_dialog.settings().automatic_chat_region,
          "a custom capture area can be selected independently of the OCR backend");
    check(standalone && !standalone->isChecked() &&
              middle_mouse && middle_mouse->isChecked() && middle_mouse->isEnabled() && capture_delay &&
              capture_delay->minimum() == 0 && capture_delay->maximum() == 2000 &&
              capture_delay->value() == 250 && exit_hotkey &&
              exit_hotkey->keySequence().toString(QKeySequence::PortableText) ==
                  QStringLiteral("Alt+C"),
          "the fresh dialog is ready for automatic targets and retains bounded marker delay");
    if (standalone) standalone->setChecked(true);
    check(!settings_dialog.settings().game_integration_enabled && middle_mouse && !middle_mouse->isEnabled() &&
              capture_delay && !capture_delay->isEnabled() && workflow_steps &&
              workflow_steps->text().contains(QStringLiteral("вручную")),
          "an explicit standalone choice disables game controls and updates the main guide");
    if (standalone) standalone->setChecked(false);
    if (middle_mouse) middle_mouse->setChecked(true);
    if (capture_delay) capture_delay->setValue(700);
    if (exit_hotkey) exit_hotkey->setKeySequence(QKeySequence(QStringLiteral("Ctrl+Alt+C")));
    check(settings_dialog.settings().game_integration_enabled && settings_dialog.settings().middle_mouse_enabled &&
              settings_dialog.settings().mouse_capture_delay_ms == 700 &&
              settings_dialog.settings().exit_game_mode_hotkey == L"Ctrl+Alt+C" &&
              settings_dialog.settings().pinned_card.unlock_hotkey ==
                  crowded_settings.pinned_card.unlock_hotkey,
          "editing game controls preserves unrelated card preferences");
    if (base_hotkey) base_hotkey->setKeySequence(QKeySequence(QStringLiteral("Ctrl+Alt+X")));
    check(workflow_steps && workflow_steps->text().contains(QStringLiteral("Ctrl+Alt+X")),
          "the main guide displays the actual edited base shortcut");
    if (settings_tabs) settings_tabs->setCurrentIndex(1);
    QApplication::processEvents();
    check(base_hotkey && base_hotkey->isVisible() &&
              base_hotkey->keySequence().toString(QKeySequence::PortableText) == QStringLiteral("Ctrl+Alt+X") &&
              backend_choice && !backend_choice->isVisible() &&
              coordinate_pattern && !coordinate_pattern->isVisible(),
          "the shortcuts page retains the edited key and keeps recognition controls on their own page");
    if (settings_tabs) settings_tabs->setCurrentIndex(3);
    QApplication::processEvents();
    check(base_hotkey && !base_hotkey->isVisible() && backend_choice && backend_choice->isVisible() &&
              coordinate_pattern && !coordinate_pattern->isVisible() && capture_delay &&
              capture_delay->isVisible() && capture_delay->value() == 700 && automatic_chat && !automatic_chat->isChecked(),
          "the recognition page retains the edited capture settings with the custom pattern collapsed");
    if (pattern_details && coordinate_pattern) {
        const auto current_pattern = coordinate_pattern->toPlainText();
        pattern_details->click();
        QApplication::processEvents();
        check(pattern_details->isChecked() && coordinate_pattern->isVisible() &&
                  coordinate_pattern->toPlainText() == current_pattern,
              "the recognition disclosure exposes the existing editable coordinate pattern without changing it");
    }
    if (settings_tabs) settings_tabs->setCurrentIndex(2);
    QApplication::processEvents();
    const auto* manual_adjust = settings_dialog.findChild<QPushButton*>(
        QStringLiteral("adjustGhostReticle"));
    auto* preset = settings_dialog.findChild<QComboBox*>(
        QStringLiteral("ghostReticlePreset"));
    check(manual_adjust && manual_adjust->text() == QStringLiteral("Настроить размер на экране"),
          "the size button clearly distinguishes manual adjustment");
    check(manual_adjust && manual_adjust->isVisible() && preset && preset->isVisible() &&
              backend_choice && !backend_choice->isVisible() &&
              coordinate_pattern && !coordinate_pattern->isVisible(),
          "the sight page exposes manual adjustment and presets without recognition fields");
    check(preset && preset->currentText().startsWith(QStringLiteral("Свой размер")),
          "a manually changed size is represented as a custom preset");
    const std::array<std::pair<QSize, int>, 4> expected_presets{{
        {{1280, 720}, 480}, {{1600, 900}, 600},
        {{1920, 1080}, 720}, {{2560, 1440}, 960},
    }};
    for (const auto& [resolution, expected_width] : expected_presets) {
        const int index = preset ? preset->findData(resolution) : -1;
        check(index >= 0, "all verified 16:9 resolutions are listed");
        if (preset) preset->setCurrentIndex(index);
        check(settings_dialog.settings().ghost_reticle.width == expected_width,
              "a resolution preset restores its proportional reticle size");
    }
    auto* compensation = settings_dialog.findChild<QDoubleSpinBox*>(
        QStringLiteral("ghostBearingCompensation"));
    auto* decrease_compensation = settings_dialog.findChild<QPushButton*>(
        QStringLiteral("decreaseGhostBearingCompensation"));
    auto* increase_compensation = settings_dialog.findChild<QPushButton*>(
        QStringLiteral("increaseGhostBearingCompensation"));
    check(compensation && decrease_compensation && increase_compensation,
          "settings exposes editable ghost bearing compensation controls");
    if (increase_compensation) increase_compensation->click();
    check(compensation && qAbs(compensation->value() - 0.05) < 1e-9,
          "the right compensation button advances by 0.05 degrees");
    if (decrease_compensation) decrease_compensation->click();
    check(compensation && qAbs(compensation->value()) < 1e-9,
          "the left compensation button decreases by 0.05 degrees");
    if (compensation) compensation->setValue(-0.35);
    check(qAbs(settings_dialog.settings()
                       .ghost_reticle.bearing_compensation_deg +
                   0.35) < 1e-9,
          "manually entered bearing compensation is returned by settings");
    settings_dialog.hide();

    int saved_ghost_width = 0;
    GhostReticleWindow ghost({}, [&saved_ghost_width](int width) {
        saved_ghost_width = width;
    });
    check(ghost.width() * 3 == ghost.height() * 4,
          "ghost reticle always starts at the measured 4:3 aspect ratio");
    check(ghost.testAttribute(Qt::WA_TransparentForMouseEvents) &&
              ghost.windowFlags().testFlag(Qt::WindowTransparentForInput),
          "ordinary ghost mode is completely click-through");
    ghost.set_solution(wardogs::CorrectedSolution{
        wardogs::Arc::low, 188.4, 1248.0, 403.25});
    ghost.set_overlay_enabled(true);
    QApplication::processEvents();
    check(ghost.isVisible(), "enabled ghost reticle appears when a solution exists");
    QImage ghost_render(ghost.size(), QImage::Format_ARGB32_Premultiplied);
    ghost_render.fill(Qt::transparent);
    ghost.render(&ghost_render);
    bool has_ghost_pixels = false;
    for (int y = 0; y < ghost_render.height() && !has_ghost_pixels; ++y) {
        for (int x = 0; x < ghost_render.width(); ++x) {
            if (qAlpha(ghost_render.pixel(x, y)) != 0) {
                has_ghost_pixels = true;
                break;
            }
        }
    }
    check(has_ghost_pixels, "bearing and MIL scales are drawn procedurally");
    const auto rendered_ghost = [&ghost] {
        QImage image(ghost.size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        ghost.render(&image);
        return image;
    };
    for (const auto arc : {wardogs::Arc::low, wardogs::Arc::high}) {
        const wardogs::CorrectedSolution precise{arc, 359.96, 1.0,
            arc == wardogs::Arc::low ? 399.49 : 1149.51};
        ghost.set_solution(precise);
        const auto precise_render = rendered_ghost();
        const auto command = wardogs::displayed_firing_command(precise);
        ghost.set_solution(wardogs::CorrectedSolution{
            arc, command.bearing_deg, command.table_distance_m, command.mil});
        check(rendered_ghost() == precise_render,
              "both ghost trajectories render exactly the integer MIL and north azimuth shown by the card");
        check(precise.bearing_deg == 359.96 && precise.reticle_distance_m == 1.0 &&
                  precise.mil == (arc == wardogs::Arc::low ? 399.49 : 1149.51),
              "ghost command rendering does not mutate the full-precision caller solution");
        ghost.set_solution(wardogs::CorrectedSolution{
            arc, command.bearing_deg, 1.0, command.mil + 1.0});
        check(rendered_ghost() != precise_render,
              "changing the commanded MIL by one moves the ghost ruler rather than retaining stale pixels");
    }
    ghost.set_mortar_solution(215.0, 500.0);
    QImage mortar_render(ghost.size(), QImage::Format_ARGB32_Premultiplied);
    mortar_render.fill(Qt::transparent);
    ghost.render(&mortar_render);
    bool has_mortar_bearing_pixels = false;
    for (int y = 45; y < 125 && !has_mortar_bearing_pixels; ++y) {
        for (int x = 100; x < mortar_render.width() - 100; ++x) {
            if (qAlpha(mortar_render.pixel(x, y)) != 0) {
                has_mortar_bearing_pixels = true;
                break;
            }
        }
    }
    check(has_mortar_bearing_pixels,
          "mortar ghost reticle keeps the shared top bearing scale");
    ghost.begin_adjustment();
    check(ghost.adjusting() &&
              !ghost.testAttribute(Qt::WA_TransparentForMouseEvents) &&
              !ghost.windowFlags().testFlag(Qt::WindowTransparentForInput),
          "size adjustment temporarily accepts input and exposes the border");
    auto* adjustment_controls = ghost.findChild<QWidget*>(
        QStringLiteral("ghostReticleAdjustmentControls"));
    auto* confirm_ghost = ghost.findChild<QToolButton*>(
        QStringLiteral("ghostReticleConfirm"));
    auto* cancel_ghost = ghost.findChild<QToolButton*>(
        QStringLiteral("ghostReticleCancel"));
    check(adjustment_controls && confirm_ghost && cancel_ghost &&
              confirm_ghost->text().isEmpty() &&
              cancel_ghost->text().isEmpty() &&
              !confirm_ghost->icon().isNull() &&
              !cancel_ghost->icon().isNull(),
          "adjustment mode uses drawn confirm and cancel icons");
    check(adjustment_controls &&
              qAbs(adjustment_controls->geometry().center().x() -
                   ghost.rect().center().x()) <= 1 &&
              ghost.height() - adjustment_controls->geometry().bottom() <= 13,
          "adjustment controls are centered along the bottom edge");
    const int original_ghost_width = ghost.width();
    QMouseEvent hover_ghost_edge(
        QEvent::MouseMove, QPointF(1, ghost.height() / 2.0),
        QPointF(ghost.mapToGlobal(QPoint(1, ghost.height() / 2))),
        Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&ghost, &hover_ghost_edge);
    check(ghost.cursor().shape() == Qt::SizeHorCursor,
          "hovering an adjustment edge exposes the resize cursor");
    ghost.resize(original_ghost_width - 80,
                 qRound((original_ghost_width - 80) * 3.0 / 4.0));
    if (cancel_ghost) cancel_ghost->click();
    check(!ghost.adjusting() && ghost.width() == original_ghost_width &&
              saved_ghost_width == 0,
          "cancel restores the size from before adjustment without saving");
    ghost.begin_adjustment();
    ghost.resize(original_ghost_width - 40,
                 qRound((original_ghost_width - 40) * 3.0 / 4.0));
    QKeyEvent save_ghost_size(QEvent::KeyPress, Qt::Key_Escape,
                              Qt::NoModifier);
    QApplication::sendEvent(&ghost, &save_ghost_size);
    check(!ghost.adjusting() && saved_ghost_width ==
              qRound(ghost.width() * QGuiApplication::primaryScreen()->devicePixelRatio()),
          "Escape saves the adjusted ghost-reticle size in physical game pixels");
    ghost.hide();

    QWidget host;
    host.setWindowTitle(QStringLiteral("测试窗口"));
    configure_frameless_window(&host);
    WindowTitleBar title_bar(&host);
    check(host.windowFlags().testFlag(Qt::FramelessWindowHint),
          "custom title bar removes the native Windows caption");
    const auto* title_text =
        title_bar.findChild<QLabel*>(QStringLiteral("windowTitleText"));
    const auto caption_buttons = title_bar.findChildren<QToolButton*>();
    check(title_text && title_text->text() == QStringLiteral("测试窗口"),
          "custom title bar follows the host window title");
    check(caption_buttons.size() == 3,
          "custom title bar provides minimize, maximize, and close controls");
    for (const auto* button : caption_buttons) {
        check(button->text().isEmpty() && !button->icon().isNull() &&
                  !button->accessibleName().isEmpty(),
              "caption controls are icon-only and remain accessible");
    }
    NCCALCSIZE_PARAMS frame_parameters{};
    MSG frame_message{};
    frame_message.message = WM_NCCALCSIZE;
    frame_message.wParam = TRUE;
    frame_message.lParam = reinterpret_cast<LPARAM>(&frame_parameters);
    qintptr frame_result = -1;
    check(handle_frameless_native_event(&host, &frame_message, &frame_result) &&
              frame_result == 0,
          "custom chrome owns non-client sizing after a window is shown again");

    {
        VehicleSolutionWidget quantized(wardogs::Arc::low);
        const wardogs::CorrectedSolution precise{wardogs::Arc::low, 359.96, 9999.0, 399.49};
        quantized.set_solution(precise, 2221.0);
        check(quantized.mil_text() == QStringLiteral("399 MIL") &&
                  quantized.bearing_text() == QStringLiteral("0.0° N") &&
                  quantized.table_distance_text() == QStringLiteral("≈ 2470 м") &&
                  quantized.distance_text() == QStringLiteral("2221 м"),
              "the card reports the integer MIL command and its 2469.5 metre table range, separately from target distance");
        check(precise.mil == 399.49 && precise.bearing_deg == 359.96 &&
                  precise.reticle_distance_m == 9999.0,
              "rendering a firing command preserves the precise calculation supplied by its caller");
        bool unsupported_rounding_rejected = false;
        try { quantized.set_solution({wardogs::Arc::low, 0.0, 1181.0, 19.9}, 2000.0); }
        catch (const std::invalid_argument&) { unsupported_rounding_rejected = true; }
        check(unsupported_rounding_rejected && quantized.mil_text() == QStringLiteral("399 MIL") &&
                  quantized.table_distance_text() == QStringLiteral("≈ 2470 м"),
              "card validation rejects an unsupported raw MIL before rounding without replacing valid guidance");
    }
    VehicleSolutionWidget full(wardogs::Arc::low);
    full.set_unavailable(QStringLiteral("3100 m"), QStringLiteral("203.0° SW"));
    check(full.unavailable(), "unavailable result stores warning state");
    check(full.mil_text() == QStringLiteral("Нет решения"),
          "full card explains unavailable reticle");

    full.set_solution({wardogs::Arc::low, 188.4, 1248.0, 33.0});
    check(!full.unavailable(), "valid result clears warning state");
    check(full.distance_text() == QStringLiteral("—") &&
              full.table_distance_text() == QStringLiteral("≈ 1247 м"),
          "missing target distance stays unknown while inverse final MIL is visibly estimated separately");
    check(full.bearing_text().contains(QStringLiteral("188.4°")),
          "bearing keeps degrees and compass direction");
    check(full.mil_text() == QStringLiteral("33 MIL"),
          "reticle is prominent and includes its unit");
    const auto* full_arc = full.findChild<QLabel*>(QStringLiteral("solutionArc"));
    const auto* sight_distance = full.findChild<QLabel*>(QStringLiteral("solutionDistance"));
    const auto* table_distance = full.findChild<QLabel*>(QStringLiteral("solutionTableDistance"));
    const auto* sight_mil = full.findChild<QLabel*>(QStringLiteral("solutionMil"));
    check(table_distance && table_distance->accessibleName().contains(QStringLiteral("Оценка табличной дальности")) &&
              table_distance->toolTip().contains(QStringLiteral("оценка модели")) &&
              table_distance->toolTip().contains(QStringLiteral("игровой шкалы")) && sight_mil &&
              sight_mil->toolTip().contains(QStringLiteral("Игровая наводка")),
          "SPH-2 distinguishes the estimated table distance from the game HUD and MIL setting");
    const auto full_captions = full.findChildren<QLabel*>(QStringLiteral("solutionMetricCaption"));
    check(full_captions.size() == 4 && full_captions[0]->text() == QStringLiteral("До цели") &&
              full_captions[2]->text() == QStringLiteral("Установить") &&
              full_captions[3]->text() == QStringLiteral("По таблице ≈"),
          "target metres, set MIL and estimated table metres have separate visible captions");
    full.set_selected(true);
    check(full.selected() && full.property("selected").toBool() && full_arc &&
              full_arc->text() == QStringLiteral("✓ Настильная") &&
              full.accessibleName().contains(QStringLiteral("выбрана")),
          "the effective firing trajectory is visible and accessible on the full solution card");
    full.set_solution({wardogs::Arc::low, 188.4, 1391.0, 90.0}, 2221.0);
    check(full.distance_text() == QStringLiteral("2221 м") && sight_distance &&
              sight_distance->accessibleName().contains(QStringLiteral("Горизонтальная дальность до цели")) &&
              sight_distance->toolTip().contains(QStringLiteral("До цели 2221 м")) &&
              sight_distance->toolTip().contains(QStringLiteral("≈ 1529 м")) &&
              full.table_distance_text() == QStringLiteral("≈ 1529 м") &&
              full_captions[0]->text() == QStringLiteral("До цели") &&
              full.toolTip().contains(QStringLiteral("Дальность до цели")),
          "a stale supplied reticle distance cannot replace the inverse corrected low-arc MIL");
    bool invalid_target_distance_rejected = false;
    try { full.set_solution({wardogs::Arc::low, 188.4, 1391.0, 90.0}, std::numeric_limits<double>::quiet_NaN()); }
    catch (const std::invalid_argument&) { invalid_target_distance_rejected = true; }
    check(invalid_target_distance_rejected && full.distance_text() == QStringLiteral("2221 м"),
          "invalid supplied target distance cannot replace a valid displayed result");
    bool invalid_mil_rejected = false;
    try { full.set_solution({wardogs::Arc::low, 188.4, 1391.0, 9999.0}, 2000.0); }
    catch (const std::invalid_argument&) { invalid_mil_rejected = true; }
    check(invalid_mil_rejected && full.distance_text() == QStringLiteral("2221 м") &&
              full.mil_text() == QStringLiteral("90 MIL") && full.table_distance_text() == QStringLiteral("≈ 1529 м"),
          "unsupported final MIL cannot publish a plausible table estimate or replace a valid command");

    VehicleSolutionWidget compact(wardogs::Arc::high, true);
    full.set_unavailable(QStringLiteral("3100 m"), QStringLiteral("203.0° SW"));
    full.set_selected(true);
    check(!full.selected() && full_arc && full_arc->text() == QStringLiteral("Настильная") &&
              full.accessibleName().contains(QStringLiteral("недоступна")) && sight_distance &&
              sight_distance->accessibleName().contains(QStringLiteral("дальность до цели")) &&
              !sight_distance->toolTip().contains(QStringLiteral("1391")),
          "unavailable guidance cannot be marked as the selected firing trajectory");
    compact.copy_from(full);
    check(compact.mil_text() == QStringLiteral("—"),
          "pinned card uses a dash instead of explanatory text");
    const auto* compact_arc = compact.findChild<QLabel*>(QStringLiteral("solutionArc"));
    check(compact_arc && compact_arc->text() == QStringLiteral("Навесная") &&
              compact.accessibleName().contains(QStringLiteral("недоступна")),
          "the floating SPH-2 rows explicitly name their trajectories and expose unavailable status");
    compact.set_solution({wardogs::Arc::high, 203.0, 1248.0, 1280.0});
    check(compact.distance_text() == QStringLiteral("—") &&
              compact.table_distance_text() == QStringLiteral("≈ 1245 м") &&
              compact.toolTip().contains(QStringLiteral("Оценка табличной дальности")),
          "compact table-only guidance exposes the same estimated-distance meaning");
    compact.set_solution({wardogs::Arc::high, 203.0, 2616.0, 673.0}, 2221.0);
    check(compact.distance_text() == QStringLiteral("2221 м") &&
              compact.table_distance_text() == QStringLiteral("≈ 2616 м") &&
              compact.findChild<QLabel*>(QStringLiteral("solutionDistance"))->toolTip().contains(QStringLiteral("≈ 2616 м")),
          "compact guidance visibly separates target distance and final high-arc table equivalent");
    compact.set_selected(true);
    check(compact.selected() && compact_arc->text() == QStringLiteral("✓\nНавесная"),
          "the floating row marks the effective selected trajectory");
    compact.set_waiting();
    check(!compact.selected() && compact_arc->text() == QStringLiteral("Навесная") &&
              compact.accessibleName().contains(QStringLiteral("ожидает цель")) &&
              compact.distance_text() == QStringLiteral("—") &&
              compact.table_distance_text() == QStringLiteral("—") &&
              !compact.findChild<QLabel*>(QStringLiteral("solutionDistance"))->toolTip().contains(QStringLiteral("2616")),
          "clearing guidance removes the selected trajectory and stale estimated distance metadata");
    compact.set_solution({wardogs::Arc::high, 203.0, 2616.0, 673.0});
    compact.set_height_unavailable();
    check(compact.distance_text() == QStringLiteral("—") && compact.table_distance_text() == QStringLiteral("—") && compact.unavailable() &&
              compact.findChild<QLabel*>(QStringLiteral("solutionDistance"))->accessibleName().contains(QStringLiteral("нет данных высоты")) &&
              !compact.findChild<QLabel*>(QStringLiteral("solutionDistance"))->toolTip().contains(QStringLiteral("2616")),
          "height failure clears the stale table distance and its metadata");

    VehicleSolutionWidget high_full(wardogs::Arc::high);
    full.set_solution({wardogs::Arc::low, 188.4, 1391.0, 90.0}, 2221.0);
    high_full.set_solution({wardogs::Arc::high, 188.4, 2616.0, 673.0}, 2221.0);
    PinnedResultWindow trajectory_card([] {});
    trajectory_card.set_mode(true);
    const QSize default_vehicle_size = trajectory_card.size();
    trajectory_card.set_vehicle_values(full, high_full);
    trajectory_card.set_selected_arc(wardogs::Arc::high);
    trajectory_card.show();
    QApplication::processEvents();
    const auto trajectory_distances = trajectory_card.findChildren<QLabel*>(QStringLiteral("solutionDistance"));
    check(trajectory_distances.size() == 2 &&
              trajectory_distances[0]->text() == QStringLiteral("2221 м") &&
              trajectory_distances[1]->text() == QStringLiteral("2221 м") &&
              trajectory_distances[0]->toolTip().contains(QStringLiteral("≈ 1529 м")) &&
              trajectory_distances[1]->toolTip().contains(QStringLiteral("≈ 2616 м")) &&
              trajectory_distances[0]->accessibleName().contains(QStringLiteral("до цели")) &&
              trajectory_distances[1]->accessibleName().contains(QStringLiteral("до цели")),
          "both floating trajectories retain the same target distance and their separate model estimates");
    const auto trajectory_tables = trajectory_card.findChildren<QLabel*>(QStringLiteral("solutionTableDistance"));
    const auto compact_captions = trajectory_card.findChildren<QLabel*>(QStringLiteral("solutionMetricCaption"));
    check(trajectory_tables.size() == 2 && trajectory_tables[0]->text() == QStringLiteral("≈ 1529 м") &&
              trajectory_tables[1]->text() == QStringLiteral("≈ 2616 м") && compact_captions.size() == 8,
          "both mini-card rows expose all four captions and inverse corrected-MIL estimates");
    const auto trajectory_labels = trajectory_card.findChildren<QLabel*>(QStringLiteral("solutionArc"));
    check(trajectory_labels.size() == 2 &&
              trajectory_labels[0]->text() == QStringLiteral("Настильная") &&
              trajectory_labels[1]->text() == QStringLiteral("✓\nНавесная") &&
              trajectory_card.size() == default_vehicle_size,
          "floating selection identifies exactly one trajectory without resizing the result card");
    trajectory_card.set_selected_arc(wardogs::Arc::low);
    check(trajectory_labels[0]->text() == QStringLiteral("✓\nНастильная") &&
              trajectory_labels[1]->text() == QStringLiteral("Навесная"),
          "switching the effective firing arc clears the old floating selection");
    trajectory_card.resize(trajectory_card.minimumSize());
    QApplication::processEvents();
    for (const auto* label : trajectory_labels) {
        const QFontMetrics font_metrics(label->font());
        int text_width = 0;
        const auto lines = label->text().split('\n');
        for (const auto& line : lines)
            text_width = std::max(text_width, font_metrics.horizontalAdvance(line));
        if (text_width > label->width())
            std::cerr << "trajectory label text=" << label->text().toStdString()
                      << " text_width=" << text_width << " widget_width=" << label->width()
                      << " font_pixels=" << label->font().pixelSize()
                      << " font_family=" << label->font().family().toStdString()
                      << " label_hint=" << label->sizeHint().width() << '\n';
        check(text_width <= label->width(),
              "trajectory names and the selection marker fit at the minimum floating width");
        check(font_metrics.height() * lines.size() <= label->height(),
              "the full trajectory name and selection marker fit at the minimum floating height");
    }
    for (const auto* label : trajectory_distances + trajectory_tables) {
        if (QFontMetrics(label->font()).horizontalAdvance(label->text()) > label->width())
            std::cerr << "distance label text=" << label->text().toStdString()
                      << " text_width=" << QFontMetrics(label->font()).horizontalAdvance(label->text())
                      << " widget_width=" << label->width()
                      << " font_pixels=" << label->font().pixelSize()
                      << " font_family=" << label->font().family().toStdString() << '\n';
        check(QFontMetrics(label->font()).horizontalAdvance(label->text()) <= label->width(),
              "the target distance fits at the minimum floating width");
    }
    full.set_solution({wardogs::Arc::low, 188.4, 1391.0, 90.0});
    high_full.set_solution({wardogs::Arc::high, 188.4, 2616.0, 673.0});
    trajectory_card.set_vehicle_values(full, high_full);
    QApplication::processEvents();
    check(trajectory_distances[0]->text() == QStringLiteral("—") &&
              trajectory_distances[1]->text() == QStringLiteral("—") &&
              trajectory_tables[0]->text() == QStringLiteral("≈ 1529 м") &&
              trajectory_tables[1]->text() == QStringLiteral("≈ 2616 м") &&
              trajectory_tables[0]->accessibleName().contains(QStringLiteral("Оценка табличной дальности")) &&
              trajectory_tables[1]->toolTip().contains(QStringLiteral("оценка модели")),
          "copying table-only rows to the floating card retains their estimate labels and metadata");
    for (const auto* label : trajectory_tables) {
        if (QFontMetrics(label->font()).horizontalAdvance(label->text()) > label->width())
            std::cerr << "estimated distance text=" << label->text().toStdString()
                      << " text_width=" << QFontMetrics(label->font()).horizontalAdvance(label->text())
                      << " widget_width=" << label->width()
                      << " font_pixels=" << label->font().pixelSize()
                      << " font_family=" << label->font().family().toStdString() << '\n';
        check(QFontMetrics(label->font()).horizontalAdvance(label->text()) <= label->width(),
              "an approximate table distance fits at the minimum floating width");
    }
    full.set_solution({wardogs::Arc::low, 188.4, 1391.0, 90.0}, 2221.0);
    high_full.set_unavailable(QStringLiteral("3100 м"), QStringLiteral("188.4° S"));
    trajectory_card.set_vehicle_values(full, high_full);
    trajectory_card.set_selected_arc(wardogs::Arc::high);
    check(trajectory_labels[0]->text() == QStringLiteral("Настильная") &&
              trajectory_labels[1]->text() == QStringLiteral("Навесная"),
          "an unavailable requested arc does not show a misleading selected floating row");
    trajectory_card.set_selected_arc(std::nullopt);
    trajectory_card.hide();

    PinnedResultWindow workflow_card([] {});
    auto* workflow_status = workflow_card.findChild<QLabel*>(QStringLiteral("pinnedWorkflowStatus"));
    const QSize plain_card_size = workflow_card.size();
    workflow_card.set_values(QStringLiteral("470 m"), QStringLiteral("180.0° S"), QStringLiteral("500 mil"));
    const auto* workflow_mil = workflow_card.findChild<QLabel*>(QStringLiteral("pinnedMortarMil"));
    const QString plain_mil_style = workflow_mil ? workflow_mil->styleSheet() : QString{};
    check(workflow_status && workflow_status->isHidden(),
          "the workflow footer is hidden by default and preserves ordinary pinned screenshots");
    const QString ready_message = QStringLiteral("Орудие 98.12 54.67 · средняя кнопка — цель");
    workflow_card.set_workflow_status(ready_message);
    check(workflow_status && !workflow_status->isHidden() && workflow_status->text() == ready_message &&
              workflow_status->toolTip() == ready_message && workflow_card.width() == plain_card_size.width() &&
              workflow_card.height() > plain_card_size.height() && workflow_mil &&
              workflow_mil->text() == QStringLiteral("Наводка: 500 mil") && workflow_mil->styleSheet() == plain_mil_style,
          "a visible workflow hint adds its own space without changing result values or font scaling");
    const QString failure_message = QStringLiteral("Координаты не прочитаны: <повторите отметку>");
    workflow_card.set_workflow_status(failure_message);
    workflow_card.set_mode(true);
    check(workflow_status && workflow_status->textFormat() == Qt::PlainText &&
              workflow_status->text() == failure_message && workflow_status->accessibleDescription() == failure_message &&
              !workflow_status->isHidden(),
          "failure details remain literal and accessible when switching weapon modes");
    workflow_card.set_mode(false);
    workflow_card.set_workflow_status({});
    check(workflow_status && workflow_status->isHidden() && workflow_card.size() == plain_card_size,
          "clearing the workflow hint restores the original floating card dimensions");

    int exits = 0;
    int saved_opacity = 0;
    bool saved_lock = false;
    std::wstring saved_unlock_hotkey;
    PinnedResultWindow pinned(
        [&exits] { ++exits; }, {true, 67},
        [&saved_lock, &saved_opacity, &saved_unlock_hotkey](
            const PinnedResultWindow::Preferences& preferences) {
            saved_lock = preferences.locked;
            saved_opacity = preferences.opacity_percent;
            saved_unlock_hotkey = preferences.unlock_hotkey;
            return true;
        });
    bool ghost_enabled = false;
    int ghost_opacity = 0;
    pinned.configure_ghost_controls(
        false, 74,
        [&ghost_enabled](bool enabled) { ghost_enabled = enabled; },
        [&ghost_opacity](int opacity) { ghost_opacity = opacity; });
    check(pinned.is_locked(), "pinned card restores its locked state");
    check(pinned.opacity_percent() == 67,
          "pinned card restores its opacity setting");
    check(pinned.windowFlags().testFlag(Qt::WindowDoesNotAcceptFocus),
          "pinned card cannot steal focus from the game");
    check(pinned.windowType() == Qt::Window,
          "pinned mode keeps a normal taskbar window instead of a tool window");
    check(pinned.testAttribute(Qt::WA_TransparentForMouseEvents),
          "a locked card is completely transparent to mouse input");
    check(pinned.windowFlags().testFlag(Qt::WindowTransparentForInput),
          "the native window system passes locked-card input through");
    pinned.set_values(QStringLiteral("470 m"), QStringLiteral("180.0° S"),
                      QStringLiteral("500 mil"));
    const auto* pinned_mortar_mil = pinned.findChild<QLabel*>(
        QStringLiteral("pinnedMortarMil"));
    check(pinned_mortar_mil &&
              pinned_mortar_mil->text() == QStringLiteral("Наводка: 500 mil") &&
              pinned_mortar_mil->accessibleName().contains(QStringLiteral("Наводка L81")) &&
              pinned_mortar_mil->toolTip().contains(QStringLiteral("не расстояние в милях")),
          "pinned mortar card clearly labels the angular sight scale instead of distance miles");

    pinned.move(100, 100);
    pinned.resize(430, 78);
    const QRect locked_geometry = pinned.geometry();
    QMouseEvent locked_press(
        QEvent::MouseButtonPress, QPointF(20, 20), QPointF(120, 120),
        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&pinned, &locked_press);
    QMouseEvent locked_move(
        QEvent::MouseMove, QPointF(100, 60), QPointF(200, 160),
        Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&pinned, &locked_move);
    QMouseEvent locked_release(
        QEvent::MouseButtonRelease, QPointF(100, 60), QPointF(200, 160),
        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&pinned, &locked_release);
    check(pinned.geometry() == locked_geometry,
          "left-button drag and resize do nothing while the card is locked");

    QMouseEvent locked_double_click(
        QEvent::MouseButtonDblClick, QPointF(20, 20), QPointF(20, 20),
        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&pinned, &locked_double_click);
    check(exits == 0, "double click cannot exit while the pinned card is locked");

    pinned.set_locked(false);
    check(!pinned.testAttribute(Qt::WA_TransparentForMouseEvents),
          "the global unlock action restores mouse interaction");
    check(!pinned.windowFlags().testFlag(Qt::WindowTransparentForInput),
          "unlocking removes native input transparency");
    const QPointF edge_position(1, pinned.height() / 2.0);
    QHoverEvent edge_hover(QEvent::HoverMove, edge_position,
                           edge_position + QPointF(100, 100), QPointF(20, 20),
                           Qt::NoModifier);
    QApplication::sendEvent(&pinned, &edge_hover);
    check(pinned.cursor().shape() == Qt::SizeHorCursor,
          "an unlocked card shows the horizontal resize cursor at its edge");
    const QPointF center_position(pinned.width() / 2.0,
                                  pinned.height() / 2.0);
    QHoverEvent center_hover(QEvent::HoverMove, center_position,
                             center_position + QPointF(100, 100), edge_position,
                             Qt::NoModifier);
    QApplication::sendEvent(&pinned, &center_hover);
    check(pinned.cursor().shape() == Qt::ArrowCursor,
          "an unlocked card keeps the ordinary cursor in its interior");
    QMouseEvent unlocked_double_click(
        QEvent::MouseButtonDblClick, QPointF(20, 20), QPointF(20, 20),
        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&pinned, &unlocked_double_click);
    check(exits == 1, "double click exits after the pinned card is unlocked");
    check(!saved_lock && saved_opacity == 67,
          "changing the lock persists both pinned-card preferences");

    pinned.set_opacity_percent(1);
    check(pinned.opacity_percent() == 35,
          "pinned card remains visible at the minimum opacity");
    check(saved_opacity == 35,
          "changing opacity immediately persists the clamped value");

    int effective_preferences_commits = 0;
    PinnedResultWindow recovered_card([] {}, {},
        [&effective_preferences_commits](PinnedResultWindow::Preferences& preferences) {
            ++effective_preferences_commits;
            preferences.unlock_hotkey = L"Ctrl+Alt+F10";
            return true;
        });
    auto* recovered_unlock = recovered_card.findChild<QKeySequenceEdit*>(
        QStringLiteral("pinnedUnlockHotkey"));
    check(recovered_unlock, "recovered card exposes its unlock editor");
    recovered_unlock->setKeySequence(QKeySequence(QStringLiteral("Alt+F10")));
    QMetaObject::invokeMethod(recovered_unlock, "editingFinished", Qt::DirectConnection);
    check(effective_preferences_commits == 1 &&
              recovered_unlock->keySequence().toString(QKeySequence::PortableText) == QStringLiteral("Ctrl+Alt+F10"),
          "pinned editor displays the effective replacement accepted by its owner");
    recovered_card.configure_unlock_hotkey(L"Ctrl+Alt+F11");
    check(effective_preferences_commits == 1 &&
              recovered_unlock->keySequence().toString(QKeySequence::PortableText) == QStringLiteral("Ctrl+Alt+F11"),
          "parent hotkey updates synchronize the pinned editor without another preferences commit");

    // Leave room for Russian captions on the 800 px offscreen test display.
    pinned.move(20, 100);
    QContextMenuEvent menu_event(QContextMenuEvent::Mouse, QPoint(10, 10),
                                 QPoint(10, 10));
    QApplication::sendEvent(&pinned, &menu_event);
    auto* menu = pinned.findChild<QWidget*>(QStringLiteral("pinnedContextMenu"));
    const auto* lock_button =
        pinned.findChild<QToolButton*>(QStringLiteral("pinnedLockButton"));
    auto* opacity_slider =
        pinned.findChild<QSlider*>(QStringLiteral("pinnedOpacitySlider"));
    auto* unlock_hotkey = pinned.findChild<QKeySequenceEdit*>(
        QStringLiteral("pinnedUnlockHotkey"));
    auto* ghost_row = pinned.findChild<QWidget*>(
        QStringLiteral("pinnedGhostRow"));
    auto* unlock_row = pinned.findChild<QWidget*>(
        QStringLiteral("pinnedUnlockRow"));
    auto* ghost_button = pinned.findChild<QToolButton*>(
        QStringLiteral("ghostReticleToggle"));
    auto* ghost_slider = pinned.findChild<QSlider*>(
        QStringLiteral("ghostReticleOpacitySlider"));
    check(menu && lock_button && opacity_slider && ghost_row && unlock_row &&
              ghost_button && ghost_slider && unlock_hotkey,
          "right click exposes card, ghost-reticle, and unlock controls");
    check(ghost_row && unlock_row && ghost_row->geometry().top() <
                                      unlock_row->geometry().top(),
          "ghost reticle controls are inserted above the bottom unlock row");
    check(menu && qobject_cast<QMenu*>(menu) == nullptr &&
              menu->windowType() == Qt::Popup,
          "side controls use the same translucent QWidget approach as the card");
    check(lock_button && lock_button->text().isEmpty(),
          "lock control is icon-only");
    check(opacity_slider && opacity_slider->minimum() == 35 &&
              opacity_slider->maximum() == 100,
          "opacity slider keeps the card between 35 and 100 percent visible");
    check(ghost_slider && ghost_slider->minimum() == 20 &&
              ghost_slider->maximum() == 100 && ghost_slider->value() == 74,
          "the middle row has an independent ghost-reticle opacity slider");
    check(lock_button && ghost_button &&
              lock_button->property("pinnedMenuButton").toBool() &&
              ghost_button->property("pinnedMenuButton").toBool() &&
              opacity_slider->property("pinnedMenuSlider").toBool() &&
              ghost_slider->property("pinnedMenuSlider").toBool(),
          "both side-menu rows share the same button and slider styling hooks");
    if (ghost_button) {
        ghost_button->setChecked(true);
        check(ghost_enabled, "the middle-row icon toggles the ghost reticle");
    }
    if (ghost_slider) {
        ghost_slider->setValue(58);
        check(ghost_opacity == 58,
              "the middle-row slider changes only ghost-reticle opacity");
    }
    check(unlock_hotkey &&
              unlock_hotkey->keySequence().toString(QKeySequence::PortableText) ==
                  QStringLiteral("Ctrl+Alt+Q"),
          "the side controls show the default unlock hotkey");
    if (unlock_hotkey) {
        unlock_hotkey->setKeySequence(QKeySequence(QStringLiteral("Ctrl+Shift+U")));
        QMetaObject::invokeMethod(unlock_hotkey, "editingFinished",
                                  Qt::DirectConnection);
        check(saved_unlock_hotkey == L"Ctrl+Shift+U",
              "editing the side control immediately persists the unlock hotkey");
    }
    const QRect screen_available = QGuiApplication::primaryScreen()->availableGeometry();
    const bool room_on_right = menu && pinned.frameGeometry().right() + 3 +
        menu->width() <= screen_available.right() + 1;
    const bool room_on_left = menu && pinned.frameGeometry().left() - 2 -
        menu->width() >= screen_available.left();
    if (room_on_right) {
        check(menu->frameGeometry().left() > pinned.frameGeometry().right() &&
                  menu->frameGeometry().left() - pinned.frameGeometry().right() <= 3,
              "the side menu joins the right edge when there is screen space");
    } else if (room_on_left) {
        check(menu->frameGeometry().right() < pinned.frameGeometry().left() &&
                  pinned.frameGeometry().left() - menu->frameGeometry().right() <= 3,
              "the side menu joins the left edge when the right edge is blocked");
    } else {
        check(menu && menu->frameGeometry().left() >= screen_available.left() &&
                  menu->frameGeometry().right() <= screen_available.right(),
              "on a narrow screen the complete menu remains inside the viewport");
    }
    const bool room_below = menu && pinned.frameGeometry().top() + menu->height() <=
        screen_available.bottom() + 1;
    check(menu && (room_below
        ? menu->frameGeometry().top() == pinned.frameGeometry().top()
        : menu->frameGeometry().bottom() <= screen_available.bottom()),
          "the side menu aligns to the card or fits above the screen bottom");
    check(menu && menu->testAttribute(Qt::WA_TranslucentBackground) &&
              menu->mask().isEmpty(),
          "the menu uses an antialiased translucent edge instead of a binary mask");
    if (menu) {
        QImage rendered(menu->size(), QImage::Format_ARGB32_Premultiplied);
        rendered.fill(Qt::transparent);
        menu->render(&rendered);
        check(qAlpha(rendered.pixel(0, 0)) == 0 &&
                  qAlpha(rendered.pixel(rendered.width() - 1, 0)) == 0,
              "both top menu corners remain transparent");
        bool has_antialiased_edge = false;
        for (int y = 0; y < std::min(12, rendered.height()); ++y) {
            for (int x = 0; x < std::min(12, rendered.width()); ++x) {
                const int alpha = qAlpha(rendered.pixel(x, y));
                has_antialiased_edge = has_antialiased_edge ||
                                       (alpha > 0 && alpha < 255);
            }
        }
        check(has_antialiased_edge,
              "the rounded menu edge contains antialiased transition pixels");
    }
    check(menu && qAbs(menu->windowOpacity() - pinned.windowOpacity()) < 0.001,
          "the context menu follows the card opacity");
    if (opacity_slider) {
        opacity_slider->resize(200, 34);
        opacity_slider->setValue(100);
        QMouseEvent track_click(
            QEvent::MouseButtonPress, QPointF(100, 17), QPointF(100, 17),
            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(opacity_slider, &track_click);
        check(opacity_slider->value() >= 65 && opacity_slider->value() <= 70,
              "clicking the slider track jumps directly to that position");
    }
    if (menu) {
        menu->hide();
        pinned.set_opacity_percent(42);
        menu->setWindowOpacity(1.0);
        QContextMenuEvent reopened_menu_event(
            QContextMenuEvent::Mouse, QPoint(20, 20), QPoint(20, 20));
        QApplication::sendEvent(&pinned, &reopened_menu_event);
        check(qAbs(menu->windowOpacity() - 0.42) < 0.001,
              "a reopened menu reapplies the current card opacity");
        menu->hide();
    }

    std::cout << "All vehicle UI tests passed\n" << std::flush;
    // Qt's Windows offscreen plugin can wait indefinitely during process teardown.
    // All assertions are complete and no persistent resources are owned by this test.
    std::_Exit(0);
}
