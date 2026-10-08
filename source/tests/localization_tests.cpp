#include "localization.hpp"
#include "ghost_reticle_window.hpp"
#include "pinned_result_window.hpp"
#include "settings_dialog.hpp"
#include "vehicle_solution_widget.hpp"

#include <Windows.h>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QToolButton>
#include <QVBoxLayout>

#include <iostream>
#include <memory>

namespace {

using wardogs::UiLanguage;
int failures{};
int assertions{};

void check(bool value, const char* message) {
    ++assertions;
    if (value) return;
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
}

bool has_cyrillic(const QString& value) {
    static const QRegularExpression letters(QStringLiteral("[\\x{0400}-\\x{04ff}]"));
    return letters.match(value).hasMatch();
}

QString tx(const char* russian) {
    return wardogs::i18n::text(QString::fromUtf8(russian));
}

void switch_to(UiLanguage language) {
    wardogs::i18n::set_language(language);
    QApplication::processEvents();
}

void snapshot(QWidget& widget, const QString& name) {
    const auto directory = qEnvironmentVariable("WARDOGS_L10N_SCREENSHOT_DIR");
    if (directory.isEmpty()) return;
    QDir().mkpath(directory);
    widget.ensurePolished();
    widget.show();
    QApplication::processEvents();
    check(widget.grab().save(QDir(directory).filePath(name + QStringLiteral(".png"))),
          "requested localization screenshot is saved");
    widget.hide();
}

class DisplayFixture final : public QWidget {
public:
    DisplayFixture() {
        setWindowTitle(tx("Настройки · WARDOGS"));
        auto* layout = new QVBoxLayout(this);
        heading = new QLabel(tx("Азимут"), this);
        heading->setToolTip(tx("Схема направления. Не отображает рельеф и препятствия."));
        heading->setAccessibleName(tx("Дальность до цели SPH-2"));
        heading->setAccessibleDescription(tx("Дальность до цели SPH-2: ожидается цель"));
        layout->addWidget(heading);
        button = new QPushButton(tx("Сохранить"), this);
        layout->addWidget(button);
        action = new QAction(tx("Отмена"), this);
        action->setStatusTip(tx("Показать прицел"));
        addAction(action);
        group = new QGroupBox(tx("Размер и выравнивание"), this);
        layout->addWidget(group);
        combo = new QComboBox(this);
        combo->addItem(tx("RapidOCR — рекомендуется"), 101);
        combo->addItem(tx("Windows OCR — совместимость"), 202);
        combo->setItemData(1, tx("Прозрачность прицела"), Qt::ToolTipRole);
        combo->setCurrentIndex(1);
        layout->addWidget(combo);
        QObject::connect(combo, qOverload<int>(&QComboBox::currentIndexChanged),
                         this, [this](int) { ++combo_changes; });
        QObject::connect(combo, &QComboBox::currentTextChanged,
                         this, [this](const QString&) { ++combo_changes; });
        tabs = new QTabWidget(this);
        tabs->addTab(new QWidget, tx("Основные"));
        tabs->addTab(new QWidget, tx("Дополнительно"));
        tabs->setTabToolTip(0, tx("Быстрый запуск"));
        tabs->setCurrentIndex(1);
        layout->addWidget(tabs);
        QObject::connect(tabs, &QTabWidget::currentChanged,
                         this, [this](int) { ++tab_changes; });
        line = new QLineEdit(QStringLiteral("x12.34, y56.78 · цель пользователя"), this);
        line->setPlaceholderText(tx("Выбрать область координат"));
        line->setSelection(1, 6);
        layout->addWidget(line);
        pattern = new QPlainTextEdit(QStringLiteral("X\\s*([\\d.]+)\\s*Y\\s*([\\d.]+) · орудие"), this);
        layout->addWidget(pattern);
        delay = new QSpinBox(this);
        delay->setRange(0, 2000);
        delay->setValue(125);
        delay->setSuffix(tx(" мс"));
        layout->addWidget(delay);
        editable = new QComboBox(this);
        editable->setEditable(true);
        editable->addItem(tx("Основные"), 303);
        editable->setEditText(QStringLiteral("target/CloseCall · моя цель"));
        editable->lineEdit()->setSelection(3, 5);
        layout->addWidget(editable);
        dynamic = new QLabel(tx("Свой размер · %1 × %2").arg(1920).arg(1080), this);
        dynamic->setToolTip(tx("Прозрачность карточки: %1%").arg(87));
        layout->addWidget(dynamic);
        wardogs::i18n::watch(this);
    }

    void assert_language(UiLanguage language) const {
        const bool english = language == UiLanguage::english;
        check(windowTitle() == (english ? QStringLiteral("Settings · WARDOGS")
                                         : QStringLiteral("Настройки · WARDOGS")), "window title switches");
        check(heading->text() == (english ? QStringLiteral("Bearing") : QStringLiteral("Азимут")),
              "static caption switches");
        check(button->text() == (english ? QStringLiteral("Save") : QStringLiteral("Сохранить")),
              "action button switches");
        check(action->text() == (english ? QStringLiteral("Cancel") : QStringLiteral("Отмена")),
              "QAction caption switches");
        check(action->statusTip() == (english ? QStringLiteral("Show reticle") : QStringLiteral("Показать прицел")),
              "QAction status tip switches");
        check(group->title() == (english ? QStringLiteral("Size and alignment") : QStringLiteral("Размер и выравнивание")),
              "group title switches");
        check(tabs->tabText(1) == (english ? QStringLiteral("Advanced") : QStringLiteral("Дополнительно")),
              "tab caption switches");
        check(tabs->tabToolTip(0) == (english ? QStringLiteral("Quick start") : QStringLiteral("Быстрый запуск")),
              "tab tooltip switches");
        check(combo->itemText(1) == (english ? QStringLiteral("Windows OCR — compatibility")
                                            : QStringLiteral("Windows OCR — совместимость")), "combo caption switches");
        check(combo->itemData(1, Qt::ToolTipRole).toString() == (english ? QStringLiteral("Reticle opacity")
                                                                      : QStringLiteral("Прозрачность прицела")),
              "combo tooltip switches");
        check(combo->currentIndex() == 1 && combo->currentData().toInt() == 202 &&
              combo->itemData(0).toInt() == 101, "combo selection and semantic item data survive");
        check(tabs->currentIndex() == 1 && combo_changes == 0 && tab_changes == 0,
              "translation does not fire input-selection actions");
        check(line->text() == QStringLiteral("x12.34, y56.78 · цель пользователя") &&
              line->selectionStart() == 1 && line->selectedText() == QStringLiteral("12.34,"),
              "coordinate input and selection stay unchanged");
        check(pattern->toPlainText() == QStringLiteral("X\\s*([\\d.]+)\\s*Y\\s*([\\d.]+) · орудие"),
              "editable OCR pattern is never translated");
        check(editable->currentText() == QStringLiteral("target/CloseCall · моя цель") &&
              editable->lineEdit()->selectionStart() == 3 &&
              editable->lineEdit()->selectedText() == QStringLiteral("get/C") &&
              editable->currentData().toInt() == 303, "editable combo input and selection stay unchanged");
        check(delay->value() == 125 && delay->suffix() == (english ? QStringLiteral(" ms") : QStringLiteral(" мс")),
              "unit suffix switches without changing the numeric value");
        check(dynamic->text() == (english ? QStringLiteral("Custom size · 1920 × 1080")
                                          : QStringLiteral("Свой размер · 1920 × 1080")),
              "formatted labels retain both arguments");
        check(dynamic->toolTip() == (english ? QStringLiteral("Card opacity: 87%")
                                             : QStringLiteral("Прозрачность карточки: 87%")),
              "formatted tooltip retains percentage");
        check(!english || (!has_cyrillic(heading->toolTip()) &&
              heading->accessibleName() == QStringLiteral("SPH-2 target range") &&
              heading->accessibleDescription() == QStringLiteral("SPH-2 target range: waiting for a target")),
              "tooltip and accessibility descriptions fully switch to English");
    }

    QLabel *heading{}, *dynamic{};
    QPushButton* button{};
    QAction* action{};
    QGroupBox* group{};
    QComboBox *combo{}, *editable{};
    QTabWidget* tabs{};
    QLineEdit* line{};
    QPlainTextEdit* pattern{};
    QSpinBox* delay{};
    int combo_changes{}, tab_changes{};
};

void test_bindings_and_roundtrip(UiLanguage startup) {
    switch_to(startup);
    DisplayFixture fixture;
    fixture.assert_language(startup);
    for (int iteration = 0; iteration < 5; ++iteration) {
        switch_to(UiLanguage::english);
        fixture.assert_language(UiLanguage::english);
        switch_to(UiLanguage::russian);
        fixture.assert_language(UiLanguage::russian);
    }
    // Statuses change while the window remains alive; the old captured caption
    // must not replace the new status at the next language switch.
    fixture.dynamic->setText(QStringLiteral("Нет решения"));
    switch_to(UiLanguage::english);
    check(fixture.dynamic->text() == QStringLiteral("No solution"), "new RU state is translated on switch");
    fixture.dynamic->setText(QStringLiteral("≈ 942 m"));
    switch_to(UiLanguage::russian);
    check(fixture.dynamic->text() == QStringLiteral("≈ 942 м"), "new formatted EN state reverses without stale values");
    snapshot(fixture, startup == UiLanguage::english ? QStringLiteral("fixture-en-startup")
                                                    : QStringLiteral("fixture-ru-startup"));
}

void test_paths_and_nested_errors() {
    switch_to(UiLanguage::english);
    const QString file(QStringLiteral("C:/target/CloseCall.wdt"));
    check(wardogs::i18n::text(QStringLiteral("Не удалось открыть файл рельефа: ") + file) ==
          QStringLiteral("Could not open the terrain file: ") + file,
          "a platform error translates while preserving its filepath");
    check(wardogs::i18n::source_text(QStringLiteral("Could not open the terrain file: ") + file) ==
          QStringLiteral("Не удалось открыть файл рельефа: ") + file,
          "reverse source conversion preserves filepath words");
    check(wardogs::i18n::source_text(QStringLiteral("https://example.test/target/CloseCall")) ==
          QStringLiteral("https://example.test/target/CloseCall"), "URLs are never localized as prose");
    const auto registration = tx("Не удалось зарегистрировать %1: ошибка Windows %2")
        .arg(QStringLiteral("Ctrl+Alt+X")).arg(1409);
    check(registration == QStringLiteral("Could not register Ctrl+Alt+X: Windows error 1409"),
          "formatted errors preserve shortcut and platform code");
    check(wardogs::i18n::source_text(registration) ==
          QStringLiteral("Не удалось зарегистрировать Ctrl+Alt+X: ошибка Windows 1409"),
          "formatted errors restore the canonical RU source");
    check(wardogs::i18n::source_text(QStringLiteral("X: 12.34 / Y: 56.78")) ==
          QStringLiteral("X: 12.34 / Y: 56.78"), "coordinate data cannot match a generic UI template");
}

void test_html_startup_and_updates() {
    switch_to(UiLanguage::english);
    QTextBrowser browser;
    browser.resize(550, 350);
    const auto link = QStringLiteral("https://example.test/target/CloseCall");
    const auto html = QStringLiteral("<h2>Быстрый запуск</h2><p><a href='%1'>WARDOGS</a></p>").arg(link);
    wardogs::i18n::bind_html(&browser, html);
    check(browser.toPlainText().contains(QStringLiteral("Quick start")),
          "raw RU HTML is immediately localized during EN startup");
    check(browser.document()->find(QStringLiteral("WARDOGS")).charFormat().anchorHref() == link,
          "HTML hyperlink target is preserved during initial translation");
    switch_to(UiLanguage::russian);
    check(browser.toPlainText().contains(QStringLiteral("Быстрый запуск")), "HTML restores RU after EN startup");
    check(browser.document()->find(QStringLiteral("WARDOGS")).charFormat().anchorHref() == link,
          "HTML hyperlink survives the language roundtrip");
    switch_to(UiLanguage::english);
    wardogs::i18n::bind_html(&browser, QStringLiteral("<b>Нет решения</b>"));
    check(browser.toPlainText() == QStringLiteral("No solution"), "rebinding HTML updates the actual visible content");
    switch_to(UiLanguage::russian);
    check(browser.toPlainText() == QStringLiteral("Нет решения"), "rebinding HTML replaces its saved canonical source");
}

void test_stock_controls_and_deleted_roots() {
    for (const auto language : {UiLanguage::russian, UiLanguage::english}) {
        switch_to(language);
        QDialogButtonBox buttons(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
        check(buttons.button(QDialogButtonBox::Save)->text().remove('&') ==
              (language == UiLanguage::english ? QStringLiteral("Save") : QStringLiteral("Сохранить")),
              "new stock dialog buttons use the app language");
        QLineEdit input(QStringLiteral("coordinates"));
        input.selectAll();
        std::unique_ptr<QMenu> menu(input.createStandardContextMenu());
        bool copy_found{};
        for (auto* action : menu->actions()) {
            const auto label = action->text().section('\t', 0, 0).remove('&');
            if (label == (language == UiLanguage::english ? QStringLiteral("Copy") : QStringLiteral("Копировать")))
                copy_found = true;
        }
        check(copy_found, "stock edit context menu uses the app language");
    }
    auto* deleted = new QWidget;
    auto* caption = new QLabel(tx("Азимут"), deleted);
    (void)caption;
    wardogs::i18n::watch(deleted);
    delete deleted;
    switch_to(UiLanguage::russian);
    switch_to(UiLanguage::english);
    check(wardogs::i18n::language() == UiLanguage::english,
          "language switches safely after a watched window has been deleted");
}

void test_real_dialogs_and_solution_state() {
    switch_to(UiLanguage::english);
    wardogs::AppSettings settings;
    settings.game_integration_enabled = false;
    SettingsDialog dialog(settings);
    auto* tabs = dialog.findChild<QTabWidget*>(QStringLiteral("settingsTabs"));
    auto* pattern = dialog.findChild<QPlainTextEdit*>(QStringLiteral("coordinatePattern"));
    auto* mode = dialog.findChild<QCheckBox*>(QStringLiteral("standaloneMode"));
    auto* preset = dialog.findChild<QComboBox*>(QStringLiteral("ghostReticlePreset"));
    check(tabs && pattern && mode && preset, "real settings dialog exposes expected controls");
    if (!tabs || !pattern || !mode || !preset) return;
    const auto original_pattern = pattern->toPlainText();
    const auto original_preset = preset->currentData();
    tabs->setCurrentIndex(1);
    for (auto* label : dialog.findChildren<QLabel*>())
        check(!has_cyrillic(label->text()), "every real settings QLabel has an English display caption");
    for (auto* button : dialog.findChildren<QAbstractButton*>())
        check(!has_cyrillic(button->text()) && !has_cyrillic(button->toolTip()),
              "every real settings button and tooltip is English");
    snapshot(dialog, QStringLiteral("settings-en-advanced"));
    switch_to(UiLanguage::russian);
    check(dialog.windowTitle() == QStringLiteral("Настройки · WARDOGS") &&
          tabs->currentIndex() == 1 && pattern->toPlainText() == original_pattern &&
          preset->currentData() == original_preset && mode->isChecked(),
          "real settings values and selected tab survive the switch");
    snapshot(dialog, QStringLiteral("settings-ru-advanced"));
    mode->setChecked(false);
    switch_to(UiLanguage::english);
    auto* workflow = dialog.findChild<QLabel*>(QStringLiteral("quickWorkflowSteps"));
    check(workflow && workflow->text().contains(QStringLiteral("Alt+X")) &&
          !has_cyrillic(workflow->text()), "state-dependent settings guide refreshes in English");
    tabs->setCurrentIndex(0);
    snapshot(dialog, QStringLiteral("settings-en-general"));

    VehicleSolutionWidget vehicle(wardogs::Arc::low);
    VehicleSolutionWidget compact(wardogs::Arc::low, true);
    vehicle.set_solution({wardogs::Arc::low, 73.125, 970, 410}, 950);
    vehicle.set_selected(true);
    const auto original_bearing = vehicle.bearing_text();
    const auto original_mil = vehicle.mil_text();
    switch_to(UiLanguage::russian);
    check(vehicle.selected() && vehicle.distance_text() == QStringLiteral("950 м") &&
          vehicle.bearing_text() == original_bearing && vehicle.mil_text() == original_mil,
          "SPH-2 selected arc and numeric aiming values survive translation");
    switch_to(UiLanguage::english);
    vehicle.set_unavailable(QStringLiteral("700 м"), QStringLiteral("45°"));
    compact.copy_from(vehicle);
    check(vehicle.distance_text() == QStringLiteral("700 m") &&
          vehicle.mil_text() == QStringLiteral("No solution") && compact.mil_text() == QStringLiteral("—"),
          "English unavailable solution copies the intended compact placeholder");
    vehicle.set_height_unavailable();
    compact.copy_from(vehicle);
    check(vehicle.bearing_text() == QStringLiteral("No elevation data") &&
          compact.bearing_text() == QStringLiteral("—"),
          "English missing-height state copies the intended compact placeholder");

    PinnedResultWindow pinned([] {});
    pinned.set_values(QStringLiteral("350 м"), QStringLiteral("270°"), QStringLiteral("600 MIL"));
    pinned.resize(pinned.minimumWidth(), pinned.height());
    pinned.set_workflow_status(
        tx("Новый захват не подтверждён · проверьте OCR или введите координаты вручную") +
        QStringLiteral("\n") +
        tx("OCR не распознало новый захват · введите координаты вручную или повторите захват"));
    pinned.show();
    QApplication::processEvents();
    auto* status = pinned.findChild<QLabel*>(QStringLiteral("pinnedWorkflowStatus"));
    check(status && status->height() >= status->heightForWidth(status->width()) && status->height() > 32,
          "long English capture status is fully visible at the minimum card width");
    const int narrow_status_height = status ? status->height() : 0;
    const int narrow_status_width = status ? status->width() : 0;
    const int narrow_card_width = pinned.width();
    pinned.resize(650, pinned.height());
    QApplication::processEvents();
    if (status && !(status->height() >= status->heightForWidth(status->width()) &&
                    status->height() < narrow_status_height))
        std::cerr << "STATUS HEIGHT: narrow=" << narrow_status_height << " wide=" << status->height()
                  << " narrow label/card widths=" << narrow_status_width << "/" << narrow_card_width
                  << " actual label width=" << status->width() << " needed=" << status->heightForWidth(status->width())
                  << " card width=" << pinned.width() << '\n';
    check(status && status->height() >= status->heightForWidth(status->width()) &&
          status->height() < narrow_status_height, "capture status height adapts when the card is resized wider");
    pinned.hide();
    pinned.set_workflow_status(tx("Новый захват не подтверждён · проверьте OCR или введите координаты вручную"));
    snapshot(pinned, QStringLiteral("pinned-en-mortar"));
    QContextMenuEvent open_menu(QContextMenuEvent::Mouse, QPoint(20, 20), pinned.mapToGlobal(QPoint(20, 20)));
    QApplication::sendEvent(&pinned, &open_menu);
    auto* popup = pinned.findChild<QWidget*>(QStringLiteral("pinnedContextMenu"));
    if (popup) snapshot(*popup, QStringLiteral("pinned-en-menu"));
    check(popup != nullptr, "real pinned context menu is available for visual QA");
    for (auto* label : pinned.findChildren<QLabel*>())
        check(!has_cyrillic(label->text()), "pinned result and menu captions are fully English");
    pinned.set_mode(true);
    pinned.set_vehicle_values(vehicle, compact);
    snapshot(pinned, QStringLiteral("pinned-en-vehicle"));
    switch_to(UiLanguage::russian);
    snapshot(pinned, QStringLiteral("pinned-ru-vehicle"));
    if (popup) snapshot(*popup, QStringLiteral("pinned-ru-menu"));
    GhostReticleWindow reticle({}, {});
    reticle.set_mortar_solution(270, 600);
    reticle.begin_adjustment();
    snapshot(reticle, QStringLiteral("reticle-ru-adjustment"));
    switch_to(UiLanguage::english);
    snapshot(reticle, QStringLiteral("reticle-en-adjustment"));
    reticle.cancel_adjustment();
}

}  // namespace

int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    wchar_t windows_directory[MAX_PATH]{};
    const auto length = GetWindowsDirectoryW(windows_directory, MAX_PATH);
    if (length > 0 && length < MAX_PATH)
        qputenv("QT_QPA_FONTDIR", (QString::fromWCharArray(windows_directory) + QStringLiteral("\\Fonts")).toUtf8());
    QApplication app(argc, argv);
    app.setApplicationVersion(QStringLiteral("test"));
    try {
        test_bindings_and_roundtrip(UiLanguage::russian);
        test_bindings_and_roundtrip(UiLanguage::english);
        test_paths_and_nested_errors();
        test_html_startup_and_updates();
        test_stock_controls_and_deleted_roots();
        test_real_dialogs_and_solution_state();
    } catch (const std::exception& error) {
        std::cerr << "Unhandled localization test error: " << error.what() << '\n';
        return 2;
    }
    std::cout << assertions << " localization assertions; " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
