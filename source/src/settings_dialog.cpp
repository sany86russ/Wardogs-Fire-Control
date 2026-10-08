#include "settings_dialog.hpp"
#include "localization.hpp"
#include "window_title_bar.hpp"
#include "wardogs/hotkeys.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <exception>
#include <regex>

namespace {
QString qtext(const std::wstring& value) { return QString::fromStdWString(value); }

QLabel* explanation(const QString& text) {
    auto* label = new QLabel(text);
    label->setObjectName(QStringLiteral("muted"));
    label->setWordWrap(true);
    return label;
}

QFormLayout* form_for(QGroupBox* group) {
    auto* form = new QFormLayout(group);
    form->setContentsMargins(16, 18, 16, 16);
    form->setHorizontalSpacing(18);
    form->setVerticalSpacing(12);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    return form;
}

QVBoxLayout* tab_page(QTabWidget* tabs, const QString& name) {
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(4, 14, 4, 4);
    layout->setSpacing(16);
    scroll->setWidget(page);
    tabs->addTab(scroll, name);
    return layout;
}

QKeySequenceEdit* add_hotkey(QFormLayout* form, const QString& caption,
                           const std::wstring& shortcut, const QString& object_name) {
    auto* editor = new QKeySequenceEdit(QKeySequence(qtext(shortcut)));
    editor->setMaximumSequenceLength(1);
    editor->setObjectName(object_name);
    editor->setAccessibleName(caption);
    editor->setMinimumWidth(140);
    form->addRow(caption, editor);
    return editor;
}
}  // namespace

SettingsDialog::SettingsDialog(const wardogs::AppSettings& settings, QWidget* parent)
    : QDialog(parent), capture_region_(settings.capture_region),
      pinned_card_(settings.pinned_card), ghost_reticle_(settings.ghost_reticle),
      original_settings_(settings) {
    configure_frameless_window(this);
    setWindowTitle(wardogs::i18n::text(QStringLiteral("Настройки · WARDOGS")));
    setModal(true);
    setMinimumSize(440, 380);
    const QScreen* screen = parent ? parent->screen() : QGuiApplication::primaryScreen();
    const QSize available = screen ? screen->availableGeometry().size() : QSize(1280, 720);
    resize(std::min(660, available.width() - 32), std::min(580, available.height() - 48));
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    outer->addWidget(new WindowTitleBar(this));
    auto* content = new QWidget;
    auto* root = new QVBoxLayout(content);
    root->setContentsMargins(24, 18, 24, 18);
    root->setSpacing(12);
    auto* title = new QLabel(wardogs::i18n::text(QStringLiteral("Быстрый запуск")));
    title->setObjectName(QStringLiteral("dialogTitle"));
    title->setWordWrap(true);
    root->addWidget(title);
    root->addWidget(explanation(wardogs::i18n::text(QStringLiteral(
        "Приложение готово к работе после запуска. "
        "Изменения ниже применятся после сохранения."))));
    auto* tabs = new QTabWidget;
    tabs->setObjectName(QStringLiteral("settingsTabs"));
    root->addWidget(tabs, 1);

    auto* quick_page = tab_page(tabs, wardogs::i18n::text(QStringLiteral("Основные")));
    auto* workflow = new QGroupBox(wardogs::i18n::text(QStringLiteral("Орудие → цель → расчёт")));
    workflow->setObjectName(QStringLiteral("quickWorkflowGuide"));
    auto* workflow_layout = new QVBoxLayout(workflow);
    workflow_layout->setContentsMargins(16, 18, 16, 16);
    auto* workflow_text = explanation(wardogs::i18n::text(QStringLiteral(
        "1. В игре: M → правая кнопка → Mark Coordinates для позиции орудия.\n"
        "2. %1 — считать позицию орудия.\n"
        "3. Средняя кнопка на карте — цель из подсказки отметки и расчёт."))
        .arg(qtext(settings.base_hotkey)));
    workflow_text->setObjectName(QStringLiteral("quickWorkflowSteps"));
    workflow_layout->addWidget(workflow_text);
    quick_page->addWidget(workflow);
    auto* advanced_page = tab_page(tabs, wardogs::i18n::text(QStringLiteral("Дополнительно")));
    advanced_page->addWidget(explanation(wardogs::i18n::text(QStringLiteral(
        "Эти параметры нужны для своего способа работы. "
        "Обычно достаточно стандартных настроек."))));
    auto* integration = new QGroupBox(wardogs::i18n::text(QStringLiteral("Режим работы")));
    auto* integration_form = form_for(integration);
    standalone_mode_ = new QCheckBox(wardogs::i18n::text(QStringLiteral("Отдельный калькулятор · только ручной ввод")));
    standalone_mode_->setObjectName(QStringLiteral("standaloneMode"));
    standalone_mode_->setChecked(!settings.game_integration_enabled);
    integration_form->addRow(standalone_mode_);
    integration_form->addRow(explanation(wardogs::i18n::text(QStringLiteral(
        "В этом режиме глобальные клавиши, захват экрана и игровые окна выключены. "
        "Выбор сохраняется между запусками."))));
    middle_mouse_enabled_ = new QCheckBox(wardogs::i18n::text(QStringLiteral("Считать цель средней кнопкой мыши")));
    middle_mouse_enabled_->setObjectName(QStringLiteral("middleMouseEnabled"));
    middle_mouse_enabled_->setChecked(settings.middle_mouse_enabled);
    integration_form->addRow(middle_mouse_enabled_);
    advanced_page->addWidget(integration);
    auto* actions = new QGroupBox(wardogs::i18n::text(QStringLiteral("Горячие клавиши")));
    auto* shortcuts = form_for(actions);
    region_key_ = add_hotkey(shortcuts, wardogs::i18n::text(QStringLiteral("Выбрать область координат")),
                            settings.region_hotkey, QStringLiteral("regionHotkey"));
    base_key_ = add_hotkey(shortcuts, wardogs::i18n::text(QStringLiteral("Считать позицию орудия")),
                          settings.base_hotkey, QStringLiteral("baseHotkey"));
    target_key_ = add_hotkey(shortcuts, wardogs::i18n::text(QStringLiteral("Считать цель")),
                            settings.target_hotkey, QStringLiteral("targetHotkey"));
    quick_target_key_ = add_hotkey(shortcuts, wardogs::i18n::text(QStringLiteral("Быстрая цель")),
                                  settings.quick_target_hotkey, QStringLiteral("quickTargetHotkey"));
    impact_key_ = add_hotkey(shortcuts, wardogs::i18n::text(QStringLiteral("Считать точку попадания")),
                            settings.impact_hotkey, QStringLiteral("impactHotkey"));
    impact_key_->setToolTip(wardogs::i18n::text(QStringLiteral(
        "Необязательно: наведите курсор на фактическое попадание на карте. Уточняет текущую цель и выбранную траекторию; цель не меняется. Запишите попадание до смены цели/траектории. Пробные выстрелы в сторону не нужны.")));
    ghost_arc_key_ = add_hotkey(shortcuts, wardogs::i18n::text(QStringLiteral("Сменить траекторию прицела")),
                               settings.ghost_arc_hotkey, QStringLiteral("ghostArcHotkey"));
    ghost_arc_key_->setToolTip(wardogs::i18n::text(QStringLiteral("Выбирает траекторию SPH-2 для прицела, карточки и записи попаданий")));
    exit_game_mode_key_ = add_hotkey(shortcuts, wardogs::i18n::text(QStringLiteral("Вернуться из игрового режима")),
                                    settings.exit_game_mode_hotkey, QStringLiteral("exitGameModeHotkey"));
    shortcuts->addRow(explanation(wardogs::i18n::text(QStringLiteral(
        "Если сочетание занято другой программой, при сохранении подбирается "
        "свободное с той же клавишей и дополнительным Ctrl или Shift. "
        "Новое сочетание появится на кнопке действия и сохранится в настройках."))));
    advanced_page->addWidget(actions);
    auto* mouse_group = new QGroupBox(wardogs::i18n::text(QStringLiteral("Отметка на карте")));
    auto* mouse_form = form_for(mouse_group);
    mouse_capture_delay_ = new QSpinBox;
    mouse_capture_delay_->setObjectName(QStringLiteral("mouseCaptureDelay"));
    mouse_capture_delay_->setRange(0, 2000);
    mouse_capture_delay_->setSingleStep(50);
    mouse_capture_delay_->setSuffix(wardogs::i18n::text(QStringLiteral(" мс")));
    mouse_capture_delay_->setValue(settings.mouse_capture_delay_ms);
    mouse_capture_delay_->setToolTip(wardogs::i18n::text(QStringLiteral(
        "Первое чтение выполняется сразу при нажатии средней кнопки. "
        "Если оно не удалось, выполняются до двух повторов с паузой 80–350 мс. "
        "Значения вне этого диапазона ограничиваются только на время повторного чтения.")));
    mouse_form->addRow(wardogs::i18n::text(QStringLiteral("Пауза перед повторным чтением")), mouse_capture_delay_);
    mouse_form->addRow(explanation(wardogs::i18n::text(QStringLiteral(
        "Первое чтение — сразу при нажатии средней кнопки. "
        "При неудаче калькулятор повторит чтение до двух раз; пауза ограничена 80–350 мс."))));
    quick_page->addWidget(mouse_group);
    const auto update_integration = [this, actions, mouse_group] {
        const bool enabled = !standalone_mode_->isChecked();
        actions->setEnabled(enabled);
        mouse_group->setEnabled(enabled);
        middle_mouse_enabled_->setEnabled(enabled);
    };
    connect(standalone_mode_, &QCheckBox::toggled, this, update_integration);
    update_integration();
    update_workflow_ = [this, workflow_text] {
        if (standalone_mode_->isChecked()) {
            workflow_text->setText(wardogs::i18n::text(QStringLiteral("Отдельный калькулятор: введите координаты орудия и цели вручную.")));
            return;
        }
        const auto target_step = middle_mouse_enabled_->isChecked()
            ? wardogs::i18n::text(QStringLiteral("Средняя кнопка на карте — цель из подсказки отметки и расчёт."))
            : wardogs::i18n::text(QStringLiteral("%1 — считать цель и выполнить расчёт."))
                  .arg(qtext(hotkey_text(target_key_)));
        workflow_text->setText(wardogs::i18n::text(QStringLiteral(
            "1. В игре: M → правая кнопка → Mark Coordinates для позиции орудия.\n"
            "2. %1 — считать позицию орудия.\n3. %2"))
            .arg(qtext(hotkey_text(base_key_)), target_step));
    };
    connect(standalone_mode_, &QCheckBox::toggled, this, update_workflow_);
    connect(middle_mouse_enabled_, &QCheckBox::toggled, this, update_workflow_);
    connect(base_key_, &QKeySequenceEdit::keySequenceChanged, this, update_workflow_);
    connect(target_key_, &QKeySequenceEdit::keySequenceChanged, this, update_workflow_);
    update_workflow_();
    quick_page->addStretch();

    auto* reticle_page = advanced_page;
    reticle_page->addWidget(explanation(wardogs::i18n::text(QStringLiteral(
        "Прозрачная шкала поверх игры помогает выставить азимут и угол. "
        "В обычном режиме прицел пропускает нажатия мыши."))));
    auto* reticle_group = new QGroupBox(wardogs::i18n::text(QStringLiteral("Размер и выравнивание")));
    auto* reticle_form = form_for(reticle_group);
    ghost_preset_ = new QComboBox;
    ghost_preset_->setObjectName(QStringLiteral("ghostReticlePreset"));
    const std::array<QSize, 4> ghost_presets{
        QSize{1280, 720}, QSize{1600, 900}, QSize{1920, 1080}, QSize{2560, 1440},
    };
    const auto saved_preset_width = wardogs::ghost_preset_width(
        settings.ghost_reticle.preset_screen_width, settings.ghost_reticle.preset_screen_height);
    const bool saved_preset_matches = saved_preset_width && *saved_preset_width == settings.ghost_reticle.width;
    if (!saved_preset_matches)
        ghost_preset_->addItem(wardogs::i18n::text(QStringLiteral("Свой размер · %1 × %2"))
                                  .arg(settings.ghost_reticle.width)
                                  .arg(qRound(settings.ghost_reticle.width * 3.0 / 4.0)));
    int selected_preset = saved_preset_matches ? -1 : 0;
    for (const QSize preset : ghost_presets) {
        ghost_preset_->addItem(QStringLiteral("%1 × %2").arg(preset.width()).arg(preset.height()), preset);
        if (saved_preset_matches && preset.width() == settings.ghost_reticle.preset_screen_width &&
            preset.height() == settings.ghost_reticle.preset_screen_height)
            selected_preset = ghost_preset_->count() - 1;
    }
    if (selected_preset >= 0) ghost_preset_->setCurrentIndex(selected_preset);
    ghost_preset_->setToolTip(wardogs::i18n::text(QStringLiteral(
        "Разрешение игры 16:9. Ширина прицела — 37,5% ширины экрана.")));
    reticle_form->addRow(wardogs::i18n::text(QStringLiteral("Разрешение игры")), ghost_preset_);
    auto* compensation_row = new QWidget;
    auto* compensation_layout = new QHBoxLayout(compensation_row);
    compensation_layout->setContentsMargins(0, 0, 0, 0);
    compensation_layout->setSpacing(8);
    auto* decrease_compensation = new QPushButton(QStringLiteral("−"));
    decrease_compensation->setObjectName(QStringLiteral("decreaseGhostBearingCompensation"));
    decrease_compensation->setMaximumWidth(40);
    decrease_compensation->setAutoRepeat(true);
    decrease_compensation->setToolTip(wardogs::i18n::text(QStringLiteral("Уменьшить на 0,05°")));
    compensation_layout->addWidget(decrease_compensation);
    ghost_bearing_compensation_ = new QDoubleSpinBox;
    ghost_bearing_compensation_->setObjectName(QStringLiteral("ghostBearingCompensation"));
    ghost_bearing_compensation_->setRange(
        wardogs::GhostReticlePreferences::minimum_bearing_compensation_deg,
        wardogs::GhostReticlePreferences::maximum_bearing_compensation_deg);
    ghost_bearing_compensation_->setDecimals(2);
    ghost_bearing_compensation_->setSingleStep(
        wardogs::GhostReticlePreferences::bearing_compensation_step_deg);
    ghost_bearing_compensation_->setValue(settings.ghost_reticle.bearing_compensation_deg);
    ghost_bearing_compensation_->setSuffix(QStringLiteral("°"));
    ghost_bearing_compensation_->setButtonSymbols(QAbstractSpinBox::NoButtons);
    ghost_bearing_compensation_->setAlignment(Qt::AlignCenter);
    ghost_bearing_compensation_->setMinimumWidth(100);
    ghost_bearing_compensation_->setToolTip(wardogs::i18n::text(QStringLiteral(
        "Эта поправка прибавляется к азимуту на прозрачной шкале")));
    connect(ghost_bearing_compensation_, qOverload<double>(&QDoubleSpinBox::valueChanged),
        this, [this](double) { ghost_bearing_compensation_changed_ = true; });
    compensation_layout->addWidget(ghost_bearing_compensation_, 1);
    auto* increase_compensation = new QPushButton(QStringLiteral("+"));
    increase_compensation->setObjectName(QStringLiteral("increaseGhostBearingCompensation"));
    increase_compensation->setMaximumWidth(40);
    increase_compensation->setAutoRepeat(true);
    increase_compensation->setToolTip(wardogs::i18n::text(QStringLiteral("Увеличить на 0,05°")));
    compensation_layout->addWidget(increase_compensation);
    reticle_form->addRow(wardogs::i18n::text(QStringLiteral("Поправка азимута")), compensation_row);
    auto* adjust_ghost = new QPushButton(wardogs::i18n::text(QStringLiteral("Настроить размер на экране")));
    adjust_ghost->setObjectName(QStringLiteral("adjustGhostReticle"));
    adjust_ghost->setToolTip(wardogs::i18n::text(QStringLiteral("Перетаскивайте края рамки; пропорции прицела — 4:3")));
    reticle_form->addRow(adjust_ghost);
    reticle_page->addWidget(reticle_group);
    adjust_ghost->setEnabled(settings.game_integration_enabled);
    connect(standalone_mode_, &QCheckBox::toggled, adjust_ghost,
        [adjust_ghost](bool standalone) { adjust_ghost->setEnabled(!standalone); });
    connect(adjust_ghost, &QPushButton::clicked, this, &SettingsDialog::request_ghost_adjustment);
    connect(decrease_compensation, &QPushButton::clicked, this, [this] {
        ghost_bearing_compensation_->setValue(ghost_bearing_compensation_->value() -
            wardogs::GhostReticlePreferences::bearing_compensation_step_deg);
    });
    connect(increase_compensation, &QPushButton::clicked, this, [this] {
        ghost_bearing_compensation_->setValue(ghost_bearing_compensation_->value() +
            wardogs::GhostReticlePreferences::bearing_compensation_step_deg);
    });
    connect(ghost_preset_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        const QSize preset = ghost_preset_->currentData().toSize();
        if (const auto width = wardogs::ghost_preset_width(preset.width(), preset.height())) {
            ghost_reticle_.preset_screen_width = preset.width();
            ghost_reticle_.preset_screen_height = preset.height();
            ghost_reticle_.width = *width;
        }
    });

    auto* ocr_page = advanced_page;
    auto* engine_group = new QGroupBox(wardogs::i18n::text(QStringLiteral("Распознавание координат")));
    auto* engine_form = form_for(engine_group);
    backend_ = new QComboBox;
    backend_->setObjectName(QStringLiteral("ocrBackend"));
    backend_->addItem(wardogs::i18n::text(QStringLiteral("RapidOCR — рекомендуется")), 0);
    backend_->addItem(wardogs::i18n::text(QStringLiteral("Windows OCR — совместимость")), 1);
    backend_->setCurrentIndex(settings.backend == wardogs::OcrBackend::windows ? 1 : 0);
    backend_->setToolTip(wardogs::i18n::text(QStringLiteral(
        "Выбранный движок применяется при чтении своей области экрана. "
        "Автоматическое чтение чата и координат возле курсора на карте всегда использует RapidOCR.")));
    engine_form->addRow(wardogs::i18n::text(QStringLiteral("OCR для своей области")), backend_);
    automatic_chat_region_ = new QCheckBox(wardogs::i18n::text(QStringLiteral("Автопоиск для дополнительных захватов")));
    automatic_chat_region_->setObjectName(QStringLiteral("automaticChatRegion"));
    automatic_chat_region_->setChecked(settings.automatic_chat_region);
    engine_form->addRow(automatic_chat_region_);
    engine_form->addRow(explanation(wardogs::i18n::text(QStringLiteral(
        "Alt+X всегда находит орудие в поле чата автоматически. "
        "Средняя кнопка всегда читает X/Y возле курсора на карте. "
        "Эта настройка выбирает источник только для дополнительных захватов цели и попадания; "
        "своя область не нужна для быстрой игры."))));
    engine_form->addRow(explanation(wardogs::i18n::text(QStringLiteral(
        "Автоматическое чтение чата и карты использует локальный RapidOCR. "
        "Движок выше применяется только к своей области экрана. "
        "Windows OCR доступен при установленном английском языковом пакете."))));
    ocr_page->addWidget(engine_group);
    auto* advanced = new QGroupBox(wardogs::i18n::text(QStringLiteral("Шаблон координат · для опытных пользователей")));
    auto* advanced_layout = new QVBoxLayout(advanced);
    advanced_layout->setContentsMargins(16, 18, 16, 16);
    advanced_layout->setSpacing(10);
    advanced_layout->addWidget(explanation(wardogs::i18n::text(QStringLiteral(
        "Две группы захвата должны содержать X и Y. "
        "Меняйте шаблон только для другого формата текста координат. "
        "Сложные повторы групп, альтернативы и обратные ссылки отклоняются, чтобы поиск не зависал."))));
    pattern_ = new QPlainTextEdit(qtext(
        wardogs::normalize_ocr_coordinate_pattern(settings.coordinate_pattern)));
    pattern_->setObjectName(QStringLiteral("coordinatePattern"));
    pattern_->setMinimumHeight(112);
    pattern_->setAccessibleName(wardogs::i18n::text(QStringLiteral("Шаблон координат X и Y")));
    advanced_layout->addWidget(pattern_);
    auto* reset_pattern = new QPushButton(wardogs::i18n::text(QStringLiteral("Восстановить стандартный шаблон WARDOGS")));
    reset_pattern->setObjectName(QStringLiteral("resetCoordinatePattern"));
    connect(reset_pattern, &QPushButton::clicked, this, [this] {
        pattern_->setPlainText(qtext(std::wstring{wardogs::default_ocr_coordinate_pattern}));
    });
    advanced_layout->addWidget(reset_pattern);
    ocr_page->addWidget(advanced);
    ocr_page->addStretch();

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Save)->setText(wardogs::i18n::text(QStringLiteral("Сохранить")));
    buttons->button(QDialogButtonBox::Save)->setProperty("primary", true);
    buttons->button(QDialogButtonBox::Cancel)->setText(wardogs::i18n::text(QStringLiteral("Отмена")));
    connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::accept_if_valid);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto* footer = new QHBoxLayout;
    footer->addWidget(explanation(wardogs::i18n::text(QStringLiteral("Версия %1")).arg(QApplication::applicationVersion())));
    footer->addStretch();
    footer->addWidget(buttons);
    root->addLayout(footer);
    outer->addWidget(content, 1);
    enable_rounded_window_corners(this);
    wardogs::i18n::watch(this);
}

void SettingsDialog::changeEvent(QEvent* event) {
    QDialog::changeEvent(event);
    if (event->type() == QEvent::LanguageChange && update_workflow_)
        update_workflow_();
}

bool SettingsDialog::nativeEvent(const QByteArray& event_type, void* message, qintptr* result) {
    if (handle_frameless_native_event(this, message, result)) return true;
    return QDialog::nativeEvent(event_type, message, result);
}

wardogs::AppSettings SettingsDialog::settings() const {
    auto value = original_settings_;
    value.backend = backend_->currentData().toInt() == 1
                        ? wardogs::OcrBackend::windows : wardogs::OcrBackend::rapid;
    value.region_hotkey = hotkey_text(region_key_);
    value.automatic_chat_region = automatic_chat_region_->isChecked();
    value.base_hotkey = hotkey_text(base_key_);
    value.target_hotkey = hotkey_text(target_key_);
    value.quick_target_hotkey = hotkey_text(quick_target_key_);
    value.impact_hotkey = hotkey_text(impact_key_);
    value.ghost_arc_hotkey = hotkey_text(ghost_arc_key_);
    value.exit_game_mode_hotkey = hotkey_text(exit_game_mode_key_);
    value.game_integration_enabled = !standalone_mode_->isChecked();
    value.middle_mouse_enabled = middle_mouse_enabled_->isChecked();
    value.mouse_capture_delay_ms = mouse_capture_delay_->value();
    value.coordinate_pattern = wardogs::normalize_ocr_coordinate_pattern(
        pattern_->toPlainText().trimmed().toStdWString());
    value.capture_region = capture_region_;
    value.pinned_card = pinned_card_;
    value.ghost_reticle = ghost_reticle_;
    if (ghost_bearing_compensation_changed_)
        value.ghost_reticle.bearing_compensation_deg = ghost_bearing_compensation_->value();
    if (ghost_preset_->currentData().canConvert<QSize>()) {
        const QSize preset = ghost_preset_->currentData().toSize();
        value.ghost_reticle.preset_screen_width = preset.width();
        value.ghost_reticle.preset_screen_height = preset.height();
    }
    return value;
}

std::wstring SettingsDialog::hotkey_text(const QKeySequenceEdit* editor) {
    return editor->keySequence().toString(QKeySequence::PortableText).toStdWString();
}

void SettingsDialog::accept_if_valid() {
    try {
        const auto value = settings();
        const std::array hotkeys{
            wardogs::parse_hotkey(value.region_hotkey), wardogs::parse_hotkey(value.base_hotkey),
            wardogs::parse_hotkey(value.target_hotkey), wardogs::parse_hotkey(value.quick_target_hotkey),
            wardogs::parse_hotkey(value.impact_hotkey), wardogs::parse_hotkey(value.ghost_arc_hotkey),
            wardogs::parse_hotkey(value.exit_game_mode_hotkey),
            wardogs::parse_hotkey(value.pinned_card.unlock_hotkey),
        };
        if (value.game_integration_enabled) wardogs::validate_global_hotkeys(hotkeys);
        else wardogs::validate_unique_hotkeys(hotkeys);
        try { wardogs::validate_ocr_coordinate_pattern(value.coordinate_pattern); }
        catch (const std::exception& error) {
            QMessageBox::warning(this, wardogs::i18n::text(QStringLiteral("Проверьте шаблон")),
                wardogs::i18n::text(QString::fromUtf8(error.what())));
            return;
        }
        QDialog::accept();
    } catch (const std::regex_error&) {
        QMessageBox::warning(this, wardogs::i18n::text(QStringLiteral("Проверьте шаблон")),
            wardogs::i18n::text(QStringLiteral("В шаблоне координат есть синтаксическая ошибка.")));
    } catch (const std::exception& error) {
        QMessageBox::warning(this, wardogs::i18n::text(QStringLiteral("Проверьте горячие клавиши")),
            wardogs::i18n::text(QString::fromUtf8(error.what())));
    }
}

void SettingsDialog::request_ghost_adjustment() {
    adjust_ghost_requested_ = true;
    accept_if_valid();
    if (result() != QDialog::Accepted) adjust_ghost_requested_ = false;
}
