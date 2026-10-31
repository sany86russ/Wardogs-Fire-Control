#include "game_helpers_dialog.hpp"
#include "localization.hpp"
#include "window_title_bar.hpp"
#include "wardogs/hotkeys.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFormLayout>
#include <QGroupBox>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <exception>
#include <vector>

namespace {
QString helper_text(const char* value) { return wardogs::i18n::text(QString::fromUtf8(value)); }

QLabel* paragraph(const char* text, QWidget* parent = nullptr) {
    auto* label = new QLabel(helper_text(text), parent);
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    label->setObjectName(QStringLiteral("helperDescription"));
    return label;
}

QVBoxLayout* page_for(QTabWidget* tabs, const QString& title) {
    auto* scroll = new QScrollArea;
    scroll->setObjectName(QStringLiteral("helperPageScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* page = new QWidget;
    page->setObjectName(QStringLiteral("helperPage"));
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 12, 4, 4);
    layout->setSpacing(12);
    scroll->setWidget(page);
    tabs->addTab(scroll, title);
    return layout;
}

QFormLayout* card(QVBoxLayout* page, const char* title) {
    auto* box = new QGroupBox(helper_text(title));
    box->setObjectName(QStringLiteral("helperCard"));
    auto* form = new QFormLayout(box);
    form->setContentsMargins(18, 24, 18, 16);
    form->setHorizontalSpacing(18);
    form->setVerticalSpacing(10);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    page->addWidget(box);
    return form;
}

QSpinBox* milliseconds(QFormLayout* form, const char* caption, const char* name,
                       int minimum, int maximum, int value) {
    auto* spin = new QSpinBox;
    spin->setObjectName(QString::fromLatin1(name));
    spin->setRange(minimum, maximum);
    spin->setValue(value);
    spin->setSingleStep(10);
    spin->setSuffix(helper_text(" мс"));
    spin->setAccessibleName(helper_text(caption));
    spin->setKeyboardTracking(false);
    spin->setMinimumWidth(130);
    form->addRow(helper_text(caption), spin);
    return spin;
}

QKeySequenceEdit* hotkey(QFormLayout* form, const std::wstring& value, const char* name) {
    auto* edit = new QKeySequenceEdit(QKeySequence(QString::fromStdWString(value)));
    edit->setObjectName(QString::fromLatin1(name));
    edit->setMaximumSequenceLength(1);
    edit->setFinishingKeyCombinations({QKeyCombination(Qt::Key_Tab),
                                       QKeyCombination(Qt::Key_Backtab)});
    edit->setAccessibleName(helper_text("Клавиша СТАРТ / СТОП"));
    edit->setToolTip(helper_text("Нажмите сочетание в этом поле. Повторное нажатие в игре останавливает цикл."));
    form->addRow(helper_text("Клавиша СТАРТ / СТОП"), edit);
    return edit;
}

std::wstring chord(const QKeySequenceEdit* edit) {
    return edit->keySequence().toString(QKeySequence::PortableText).toStdWString();
}
}  // namespace

GameHelpersDialog::GameHelpersDialog(const wardogs::AppSettings& settings, QWidget* parent)
    : QDialog(parent), original_settings_(settings) {
    using namespace wardogs::game_helpers_limits;
    configure_frameless_window(this);
    setObjectName(QStringLiteral("gameHelpersDialog"));
    setWindowTitle(helper_text("Помощники · WARDOGS"));
    setModal(true);
    setMinimumSize(460, 420);
    const QScreen* screen = parent ? parent->screen() : QGuiApplication::primaryScreen();
    const auto available = screen ? screen->availableGeometry().size() : QSize(1280, 800);
    resize(std::min(700, available.width() - 32), std::min(780, available.height() - 48));
    setStyleSheet(QStringLiteral(
        "QDialog#gameHelpersDialog,QWidget#helperContent,QScrollArea#helperPageScroll,"
        "QWidget#helperPageScroll>QWidget>QWidget,QWidget#helperPage { background:#0b1018; border:0; }"
        "QLabel#helperTitle { color:#f3f6fa; font-size:24px; font-weight:700; }"
        "QLabel#helperDescription { color:#a8b7cc; }"
        "QLabel#helperStopped { color:#5eead4; background:#102b2c; border-radius:7px; padding:9px 12px; }"
        "QGroupBox#helperCard { background:#111c2b; border:1px solid #203049; border-radius:10px;"
        " margin-top:8px; padding-top:4px; font-weight:600; }"
        "QGroupBox#helperCard::title { subcontrol-origin:margin; left:14px; padding:0 5px; color:#cbd9ed; }"
        "QLabel[helperSummary=\"true\"] { background:#112737; color:#7dd3fc; border-radius:8px; padding:12px; }"
        "QLabel#helperValidationError { color:#fda4af; background:#321c29; border-radius:7px; padding:9px; }"
        "QSpinBox { background:#0c1420; color:#f3f6fa; border:1px solid #2a3b53; border-radius:6px; padding:7px; }"
        "QSpinBox:disabled,QKeySequenceEdit:disabled { color:#64748b; }"
        "QPushButton#helperSaveButton { background:#1e4c78; color:#f8fafc; font-weight:600; padding:9px 20px; }"
        "QPushButton#helperSaveButton:hover { background:#286497; }"));

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    outer->addWidget(new WindowTitleBar(this));
    auto* content = new QWidget;
    content->setObjectName(QStringLiteral("helperContent"));
    content->setAttribute(Qt::WA_StyledBackground, true);
    auto* root = new QVBoxLayout(content);
    root->setContentsMargins(24, 18, 24, 18);
    root->setSpacing(10);
    auto* title = new QLabel(helper_text("Игровые помощники"));
    title->setObjectName(QStringLiteral("helperTitle"));
    root->addWidget(title);
    root->addWidget(paragraph("Два отдельных сценария ЛКМ. Настройки помощников не влияют на артиллерийские расчёты."));
    auto* stopped = paragraph("Остановлены · запуск только по вашей клавише в игре");
    stopped->setObjectName(QStringLiteral("helperStopped"));
    root->addWidget(stopped);
    auto* tabs = new QTabWidget;
    tabs->setObjectName(QStringLiteral("gameHelpersTabs"));
    root->addWidget(tabs, 1);

    const auto& build = settings.game_helpers.build;
    auto* build_page = page_for(tabs, QStringLiteral("Build helper"));
    build_page->addWidget(paragraph("Строительный молоток: удержание ЛКМ → отпускание → пауза → следующий удар. Наведите большой молоток на точку строительства."));
    auto* build_control = card(build_page, "Управление");
    build_enabled_ = new QCheckBox(helper_text("Включить горячую клавишу Build helper"));
    build_enabled_->setObjectName(QStringLiteral("buildHelperEnabled"));
    build_enabled_->setChecked(build.enabled);
    build_control->addRow(build_enabled_);
    build_hotkey_ = hotkey(build_control, build.hotkey, "buildHelperHotkey");
    auto* build_timing = card(build_page, "Ритм молотка");
    build_timing_form_ = build_timing;
    build_hold_ = milliseconds(build_timing, "Удержание ЛКМ", "buildHelperHold", minimum_hold_ms, maximum_hold_ms, build.hold_ms);
    build_pause_ = milliseconds(build_timing, "Пауза после отпускания", "buildHelperPause", minimum_pause_ms, maximum_pause_ms, build.pause_ms);
    build_random_ = new QCheckBox(helper_text("Случайная добавка к паузе"));
    build_random_->setObjectName(QStringLiteral("buildHelperRandom"));
    build_random_->setChecked(build.random_enabled);
    build_timing->addRow(build_random_);
    build_random_min_ = milliseconds(build_timing, "Добавка от", "buildHelperRandomMin", 0, maximum_random_ms, build.random_min_ms);
    build_random_max_ = milliseconds(build_timing, "Добавка до", "buildHelperRandomMax", 0, maximum_random_ms, build.random_max_ms);
    build_summary_ = paragraph("");
    build_summary_->setObjectName(QStringLiteral("buildCycleSummary"));
    build_summary_->setProperty("helperSummary", true);
    build_page->addWidget(build_summary_);
    build_page->addWidget(paragraph("Пресет 514 / 33 мс — стартовая настройка. Игроки описывают отпускание кнопки сразу после удара; точное время зависит от игры и требует проверки. Случайная добавка меняет только паузу, удержание остаётся точным."));
    build_page->addStretch();

    const auto& team = settings.game_helpers.team;
    auto* team_page = page_for(tabs, QStringLiteral("Team Piker"));
    team_page->addWidget(paragraph("Повторяет короткий клик ЛКМ в текущем положении курсора. Наведите курсор на нужную игровую кнопку; помощник не распознаёт интерфейс и не перемещает мышь."));
    auto* team_control = card(team_page, "Управление");
    team_enabled_ = new QCheckBox(helper_text("Включить горячую клавишу Team Piker"));
    team_enabled_->setObjectName(QStringLiteral("teamPikerEnabled"));
    team_enabled_->setChecked(team.enabled);
    team_control->addRow(team_enabled_);
    team_hotkey_ = hotkey(team_control, team.hotkey, "teamPikerHotkey");
    auto* team_timing = card(team_page, "Интервал кликов");
    team_timing_form_ = team_timing;
    team_interval_ = milliseconds(team_timing, "Базовый интервал", "teamPikerInterval", minimum_interval_ms, maximum_interval_ms, team.interval_ms);
    team_random_ = new QCheckBox(helper_text("Добавлять случайную задержку"));
    team_random_->setObjectName(QStringLiteral("teamPikerRandom"));
    team_random_->setChecked(team.random_enabled);
    team_timing->addRow(team_random_);
    team_random_min_ = milliseconds(team_timing, "Добавка от", "teamPikerRandomMin", 0, maximum_random_ms, team.random_min_ms);
    team_random_max_ = milliseconds(team_timing, "Добавка до", "teamPikerRandomMax", 0, maximum_random_ms, team.random_max_ms);
    team_summary_ = paragraph("");
    team_summary_->setObjectName(QStringLiteral("teamCycleSummary"));
    team_summary_->setProperty("helperSummary", true);
    team_page->addWidget(team_summary_);
    team_page->addWidget(paragraph("Интервал измеряется между началами кликов и уже включает короткое нажатие 30 мс. По умолчанию: 800 + случайные 100–200 = 900–1000 мс."));
    team_page->addStretch();

    root->addWidget(paragraph("1. Включите нужную клавишу и сохраните настройки.\n2. Вернитесь в WARDOGS и нажмите её для старта.\n3. Нажмите повторно для остановки. Смена окна останавливает цикл; возврат в игру его не возобновляет."));
    auto* boundaries = paragraph("Одновременно работает один помощник. После запуска программы оба цикла остановлены. Правила WARDOGS ограничивают макросы и эксплуатацию ошибок; возможны санкции. Разрешение издателя не получено. Рандомизация меняет только ритм.");
    boundaries->setObjectName(QStringLiteral("helperBoundaries"));
    boundaries->setWordWrap(true);
    root->addWidget(boundaries);
    validation_error_ = paragraph("");
    validation_error_->setObjectName(QStringLiteral("helperValidationError"));
    validation_error_->hide();
    root->addWidget(validation_error_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    buttons->setObjectName(QStringLiteral("helperDialogButtons"));
    buttons->button(QDialogButtonBox::Save)->setObjectName(QStringLiteral("helperSaveButton"));
    buttons->button(QDialogButtonBox::Save)->setText(helper_text("Сохранить"));
    buttons->button(QDialogButtonBox::Cancel)->setText(helper_text("Отмена"));
    connect(buttons, &QDialogButtonBox::accepted, this, &GameHelpersDialog::accept_if_valid);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);
    outer->addWidget(content, 1);

    for (auto* spin : {build_hold_, build_pause_, build_random_min_, build_random_max_,
                       team_interval_, team_random_min_, team_random_max_})
        connect(spin, &QSpinBox::valueChanged, this, [this] { update_summaries(); });
    for (auto* check : {build_enabled_, build_random_, team_enabled_, team_random_})
        connect(check, &QCheckBox::toggled, this, [this] { update_summaries(); });
    update_summaries();
    wardogs::i18n::watch(this);
}

wardogs::GameHelpersPreferences GameHelpersDialog::preferences() const {
    auto result = original_settings_.game_helpers;
    result.build.enabled = build_enabled_->isChecked();
    result.build.hotkey = chord(build_hotkey_);
    result.build.hold_ms = build_hold_->value();
    result.build.pause_ms = build_pause_->value();
    result.build.random_enabled = build_random_->isChecked();
    result.build.random_min_ms = build_random_min_->value();
    result.build.random_max_ms = build_random_max_->value();
    result.team.enabled = team_enabled_->isChecked();
    result.team.hotkey = chord(team_hotkey_);
    result.team.interval_ms = team_interval_->value();
    result.team.random_enabled = team_random_->isChecked();
    result.team.random_min_ms = team_random_min_->value();
    result.team.random_max_ms = team_random_max_->value();
    return result;
}

void GameHelpersDialog::update_summaries() {
    const auto value = preferences();
    build_hotkey_->setEnabled(value.build.enabled);
    team_hotkey_->setEnabled(value.team.enabled);
    build_random_min_->setEnabled(value.build.random_enabled);
    build_random_max_->setEnabled(value.build.random_enabled);
    team_random_min_->setEnabled(value.team.random_enabled);
    team_random_max_->setEnabled(value.team.random_enabled);
    build_timing_form_->setRowVisible(build_random_min_, value.build.random_enabled);
    build_timing_form_->setRowVisible(build_random_max_, value.build.random_enabled);
    team_timing_form_->setRowVisible(team_random_min_, value.team.random_enabled);
    team_timing_form_->setRowVisible(team_random_max_, value.team.random_enabled);
    const int build_min = value.build.pause_ms + (value.build.random_enabled ? value.build.random_min_ms : 0);
    const int build_max = value.build.pause_ms + (value.build.random_enabled ? value.build.random_max_ms : 0);
    build_summary_->setText(helper_text("ЛКМ %1 мс → пауза %2–%3 мс\nПолный цикл: %4–%5 мс")
        .arg(value.build.hold_ms).arg(build_min).arg(build_max)
        .arg(value.build.hold_ms + build_min).arg(value.build.hold_ms + build_max));
    const int team_min = value.team.interval_ms + (value.team.random_enabled ? value.team.random_min_ms : 0);
    const int team_max = value.team.interval_ms + (value.team.random_enabled ? value.team.random_max_ms : 0);
    team_summary_->setText(helper_text("Клик ЛКМ каждые %1–%2 мс\nПервый клик — после команды запуска.").arg(team_min).arg(team_max));
    validation_error_->hide();
}

void GameHelpersDialog::accept_if_valid() {
    try {
        const auto value = preferences();
        wardogs::validate_game_helpers_preferences(value);
        const std::array artillery{
            original_settings_.region_hotkey, original_settings_.base_hotkey,
            original_settings_.target_hotkey, original_settings_.quick_target_hotkey,
            original_settings_.impact_hotkey, original_settings_.ghost_arc_hotkey,
            original_settings_.pinned_card.unlock_hotkey, original_settings_.exit_game_mode_hotkey};
        std::vector<wardogs::Hotkey> keys;
        for (const auto& key : artillery) keys.push_back(wardogs::parse_hotkey(key));
        // Validate both stored assignments even if disabled, but reserve only
        // enabled helpers when checking against the artillery shortcuts.
        for (const auto& entry : {std::pair{value.build.enabled, value.build.hotkey},
                                  std::pair{value.team.enabled, value.team.hotkey}}) {
            const auto parsed = wardogs::parse_hotkey(entry.second);
            const std::array one{parsed};
            wardogs::validate_global_hotkeys(one);
            if (parsed.virtual_key == VK_ESCAPE)
                throw std::invalid_argument("Esc используется для закрытия окон; выберите другую клавишу помощника");
            if (entry.first) keys.push_back(parsed);
        }
        wardogs::validate_unique_hotkeys(keys);
        QDialog::accept();
    } catch (const std::exception& error) {
        show_error(wardogs::i18n::text(QString::fromUtf8(error.what())));
    }
}

void GameHelpersDialog::show_error(const QString& message) {
    validation_error_->setText(message);
    validation_error_->show();
}

void GameHelpersDialog::changeEvent(QEvent* event) {
    QDialog::changeEvent(event);
    if (event->type() == QEvent::LanguageChange && build_summary_ && team_summary_)
        update_summaries();
}

bool GameHelpersDialog::nativeEvent(const QByteArray& event_type, void* message,
                                    qintptr* result) {
    if (handle_frameless_native_event(this, message, result)) return true;
    return QDialog::nativeEvent(event_type, message, result);
}
