#include "pinned_result_window.hpp"
#include "localization.hpp"

#include "app_icon.hpp"
#include "vehicle_solution_widget.hpp"
#include "wardogs/hotkeys.hpp"

#include <Windows.h>

#include <QEvent>
#include <QContextMenuEvent>
#include <QCursor>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHoverEvent>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QKeySequenceEdit>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSlider>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QToolButton>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <exception>
#include <utility>
#include <vector>

namespace {

constexpr int resize_margin = 8;
constexpr double minimum_font_scale = 0.78;
constexpr double maximum_font_scale = 2.5;
constexpr int header_height = 28;
constexpr int header_extra = header_height + 6;

QSize minimum_size(bool vehicle) { return vehicle ? QSize{480, 128 + header_extra} : QSize{320, 96 + header_extra}; }
QSize default_size(bool vehicle) { return vehicle ? QSize{540, 148 + header_extra} : QSize{430, 112 + header_extra}; }

wardogs::PinnedCardRect card_rect(const QRect& rect) {
    return {rect.x(), rect.y(), rect.width(), rect.height()};
}

QRect qt_rect(const wardogs::PinnedCardRect& rect) {
    return {rect.x, rect.y, rect.width, rect.height};
}

std::vector<wardogs::PinnedCardScreen> current_screens() {
    std::vector<wardogs::PinnedCardScreen> screens;
    for (auto* screen : QGuiApplication::screens())
        screens.push_back({screen->name().toStdWString(),
                           card_rect(screen->availableGeometry()),
                           screen == QGuiApplication::primaryScreen()});
    return screens;
}

class JumpSlider final : public QSlider {
public:
    using QSlider::QSlider;

protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) {
            QSlider::mousePressEvent(event);
            return;
        }
        QStyleOptionSlider option;
        initStyleOption(&option);
        const QRect handle = style()->subControlRect(
            QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);
        if (handle.contains(event->position().toPoint())) {
            QSlider::mousePressEvent(event);
            return;
        }
        jump_dragging_ = true;
        set_value_at(event->position().toPoint());
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (jump_dragging_ && (event->buttons() & Qt::LeftButton)) {
            set_value_at(event->position().toPoint());
            event->accept();
            return;
        }
        QSlider::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (jump_dragging_ && event->button() == Qt::LeftButton) {
            set_value_at(event->position().toPoint());
            jump_dragging_ = false;
            event->accept();
            return;
        }
        QSlider::mouseReleaseEvent(event);
    }

private:
    void set_value_at(QPoint position) {
        QStyleOptionSlider option;
        initStyleOption(&option);
        const QRect groove = style()->subControlRect(
            QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, this);
        const QRect handle = style()->subControlRect(
            QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);
        const int slider_minimum = groove.left();
        const int slider_maximum = groove.right() - handle.width() + 1;
        const int pointer = position.x() - handle.width() / 2;
        setSliderPosition(QStyle::sliderValueFromPosition(
            minimum(), maximum(), pointer - slider_minimum,
            std::max(1, slider_maximum - slider_minimum), option.upsideDown));
    }

    bool jump_dragging_{};
};

class RoundedPopup final : public QWidget {
public:
    explicit RoundedPopup(QWidget* parent)
        : QWidget(parent, Qt::Popup | Qt::FramelessWindowHint |
                              Qt::NoDropShadowWindowHint) {
        setAttribute(Qt::WA_TranslucentBackground);
    }

    void set_effective_opacity(qreal opacity) {
        effective_opacity_ = opacity;
        setWindowOpacity(effective_opacity_);
    }

protected:
    void paintEvent(QPaintEvent* event) override {
        (void)event;
        QPainter painter(this);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(rect(), Qt::transparent);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(QStringLiteral("#111926")));
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5),
                                10.0, 10.0);
    }

    void showEvent(QShowEvent* event) override {
        QWidget::showEvent(event);
        setWindowOpacity(effective_opacity_);
        QTimer::singleShot(0, this, [this] {
            if (isVisible()) setWindowOpacity(effective_opacity_);
        });
    }

private:
    qreal effective_opacity_{1.0};
};

void set_popup_opacity(QWidget* popup, qreal opacity) {
    static_cast<RoundedPopup*>(popup)->set_effective_opacity(opacity);
}

QIcon lock_icon(bool locked) {
    QImage image(24, 24, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    const QColor color(locked ? QStringLiteral("#63d8c5")
                              : QStringLiteral("#94a3b8"));
    painter.setPen(QPen(color, 1.8, Qt::SolidLine, Qt::RoundCap,
                        Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(5.5, 10.0, 13.0, 10.0), 2.0, 2.0);
    QPainterPath shackle;
    if (locked) {
        shackle.moveTo(8.0, 10.0);
        shackle.lineTo(8.0, 7.5);
        shackle.cubicTo(8.0, 2.8, 16.0, 2.8, 16.0, 7.5);
        shackle.lineTo(16.0, 10.0);
    } else {
        shackle.moveTo(10.0, 10.0);
        shackle.lineTo(10.0, 7.5);
        shackle.cubicTo(10.0, 3.0, 17.0, 3.0, 17.0, 7.5);
    }
    painter.drawPath(shackle);
    painter.setBrush(color);
    painter.drawEllipse(QPointF(12.0, 14.4), 1.2, 1.2);
    painter.drawLine(QPointF(12.0, 15.4), QPointF(12.0, 17.3));
    return QIcon(QPixmap::fromImage(image));
}

QIcon reticle_icon(bool enabled) {
    QImage image(24, 24, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    const QColor color(enabled ? QStringLiteral("#63d8c5")
                               : QStringLiteral("#94a3b8"));
    painter.setPen(QPen(color, 1.7, Qt::SolidLine, Qt::RoundCap));
    painter.drawEllipse(QPointF(12, 12), 5.5, 5.5);
    painter.drawEllipse(QPointF(12, 12), 1.4, 1.4);
    painter.drawLine(QPointF(12, 2.5), QPointF(12, 7));
    painter.drawLine(QPointF(12, 17), QPointF(12, 21.5));
    painter.drawLine(QPointF(2.5, 12), QPointF(7, 12));
    painter.drawLine(QPointF(17, 12), QPointF(21.5, 12));
    return QIcon(QPixmap::fromImage(image));
}

QIcon settings_icon() {
    QImage image(24, 24, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(QStringLiteral("#c2cfdf")), 1.8,
                        Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    QPainterPath gear;
    constexpr double radians = 3.141592653589793 / 180.0;
    constexpr double offsets[]{-14.0, -8.0, 8.0, 14.0};
    constexpr double radii[]{7.5, 9.8, 9.8, 7.5};
    for (int tooth = 0; tooth < 8; ++tooth)
        for (int corner = 0; corner < 4; ++corner) {
            const double angle = (tooth * 45.0 + offsets[corner]) * radians;
            const QPointF point(12.0 + radii[corner] * std::cos(angle),
                                12.0 + radii[corner] * std::sin(angle));
            if (tooth == 0 && corner == 0) gear.moveTo(point);
            else gear.lineTo(point);
        }
    gear.closeSubpath();
    painter.drawPath(gear);
    painter.drawEllipse(QPointF(12.0, 12.0), 3.0, 3.0);
    return QIcon(QPixmap::fromImage(image));
}

}  // namespace

PinnedResultWindow::PinnedResultWindow(std::function<void()> exit_callback,
                                       QWidget* parent)
    : PinnedResultWindow(std::move(exit_callback), Preferences{}, {}, parent) {}

PinnedResultWindow::PinnedResultWindow(
    std::function<void()> exit_callback, Preferences preferences,
    PreferencesChanged preferences_changed, QWidget* parent)
    : QWidget(parent, Qt::Window | Qt::FramelessWindowHint |
                          (preferences.always_on_top ? Qt::WindowFlags(Qt::WindowStaysOnTopHint) : Qt::WindowFlags{}) |
                          Qt::WindowDoesNotAcceptFocus),
      exit_callback_(std::move(exit_callback)),
      preferences_changed_(std::move(preferences_changed)),
      preferences_(preferences) {
    preferences_.opacity_percent = std::clamp(
        preferences_.opacity_percent, Preferences::minimum_opacity_percent,
        Preferences::maximum_opacity_percent);
    for (bool vehicle : {false, true}) {
        const auto& saved = preferences_.mode_sizes[vehicle ? 1 : 0];
        mode_sizes_[vehicle] = saved && wardogs::valid_pinned_card_size(*saved)
            ? QSize(saved->width, saved->height).expandedTo(minimum_size(vehicle))
            : default_size(vehicle);
    }
    geometry_save_timer_ = new QTimer(this);
    geometry_save_timer_->setSingleShot(true);
    geometry_save_timer_->setInterval(400);
    connect(geometry_save_timer_, &QTimer::timeout, this, [this] { flush_preferences(); });
    setObjectName(QStringLiteral("pinnedWindow"));
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_Hover);
    setMouseTracking(true);
    setCursor(Qt::ArrowCursor);
    setWindowOpacity(preferences_.opacity_percent / 100.0);
    setWindowTitle(wardogs::i18n::text(QStringLiteral("Решение · WARDOGS")));
    setWindowIcon(wardogs_application_icon());
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    frame_ = new QFrame;
    frame_->setObjectName(QStringLiteral("pinnedFrame"));
    frame_->setProperty("error", false);
    auto* layout = new QVBoxLayout(frame_);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    auto* header = new QWidget(frame_);
    header->setObjectName(QStringLiteral("pinnedHeader"));
    header->setFixedHeight(header_height);
    auto* header_layout = new QHBoxLayout(header);
    header_layout->setContentsMargins(3, 0, 0, 0);
    header_layout->setSpacing(4);
    weapon_caption_ = new QLabel(QStringLiteral("L81"), header);
    weapon_caption_->setObjectName(QStringLiteral("pinnedWeaponCaption"));
    weapon_caption_->setAttribute(Qt::WA_TransparentForMouseEvents);
    weapon_caption_->setStyleSheet(QStringLiteral("color:#c2cfdf;font-size:12px;font-weight:600;"));
    header_layout->addWidget(weapon_caption_);
    lock_hint_ = new QLabel(header);
    lock_hint_->setObjectName(QStringLiteral("pinnedLockHint"));
    lock_hint_->setStyleSheet(QStringLiteral("color:#94a8c1;font-size:11px;"));
    lock_hint_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    lock_hint_->hide();
    header_layout->addWidget(lock_hint_, 1);
    const auto header_button = [header, header_layout](const QString& name) {
        auto* button = new QToolButton(header);
        button->setObjectName(name);
        button->setFocusPolicy(Qt::NoFocus);
        button->setAutoRaise(true);
        button->setFixedSize(30, header_height);
        button->setIconSize(QSize(18, 18));
        button->setStyleSheet(QStringLiteral(
            "QToolButton{color:#c2cfdf;background:transparent;border:none;border-radius:4px;"
            "font-size:18px;padding:0;}"
            "QToolButton:hover{color:#e8eef7;background:#26364b;}"
            "QToolButton:pressed{background:#345066;}"));
        header_layout->addWidget(button);
        return button;
    };
    header_lock_button_ = header_button(QStringLiteral("pinnedHeaderLockButton"));
    header_lock_button_->setCheckable(true);
    controls_button_ = header_button(QStringLiteral("pinnedControlsButton"));
    controls_button_->setIcon(settings_icon());
    header_return_button_ = header_button(QStringLiteral("pinnedHeaderReturnButton"));
    header_return_button_->setText(QStringLiteral("↩"));
    connect(header_lock_button_, &QToolButton::toggled, this,
            [this](bool locked) { set_locked(locked); });
    connect(controls_button_, &QToolButton::clicked, this,
            [this] { show_context_menu(); });
    connect(header_return_button_, &QToolButton::clicked, this, [this] {
        if (context_menu_) context_menu_->hide();
        if (exit_callback_) exit_callback_();
    });
    layout->addWidget(header);

    mortar_panel_ = new QWidget;
    mortar_panel_->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto* mortar_layout = new QHBoxLayout(mortar_panel_);
    mortar_layout->setContentsMargins(0, 0, 0, 0);
    mortar_layout->setSpacing(8);
    mortar_layout->addWidget(
        result_card(QStringLiteral("#f0b45d"), wardogs::i18n::text(QStringLiteral("ДАЛЬНОСТЬ")), distance_, &mortar_mil_), 1);
    mortar_layout->addWidget(result_card(QStringLiteral("#63d8c5"), wardogs::i18n::text(QStringLiteral("АЗИМУТ")), bearing_), 1);
    distance_->setObjectName(QStringLiteral("pinnedDistance"));
    bearing_->setObjectName(QStringLiteral("pinnedBearing"));
    mortar_mil_->setAccessibleName(wardogs::i18n::text(QStringLiteral("Наводка L81 по игровой шкале MIL")));
    mortar_mil_->setToolTip(wardogs::i18n::text(QStringLiteral("MIL — игровая шкала наведения L81, а не расстояние в милях. Значение берётся из таблицы дальности.")));
    layout->addWidget(mortar_panel_);

    vehicle_panel_ = new QWidget;
    vehicle_panel_->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto* vehicle_layout = new QVBoxLayout(vehicle_panel_);
    vehicle_layout->setContentsMargins(0, 0, 0, 0);
    vehicle_layout->setSpacing(6);
    low_ = new VehicleSolutionWidget(wardogs::Arc::low, true);
    high_ = new VehicleSolutionWidget(wardogs::Arc::high, true);
    vehicle_layout->addWidget(low_);
    vehicle_layout->addWidget(high_);
    layout->addWidget(vehicle_panel_);
    vehicle_panel_->hide();
    context_caption_ = new QLabel;
    context_caption_->setObjectName(QStringLiteral("pinnedContextCaption"));
    context_caption_->setTextFormat(Qt::PlainText);
    context_caption_->setWordWrap(true);
    context_caption_->setFixedHeight(18);
    context_caption_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    context_caption_->setStyleSheet(QStringLiteral("color:#94a8c1;font-size:11px;"));
    layout->addWidget(context_caption_);
    context_caption_->hide();
    workflow_status_ = new QLabel;
    workflow_status_->setObjectName(QStringLiteral("pinnedWorkflowStatus"));
    workflow_status_->setTextFormat(Qt::PlainText);
    workflow_status_->setWordWrap(true);
    workflow_status_->setFixedHeight(18);
    workflow_status_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    workflow_status_->setStyleSheet(QStringLiteral("color:#c2cfdf;font-size:12px;"));
    workflow_status_->setAccessibleName(wardogs::i18n::text(QStringLiteral("Состояние считывания координат")));
    layout->addWidget(workflow_status_);
    workflow_status_->hide();
    outer->addWidget(frame_);
    setMinimumSize(minimum_size(false));
    resize(mode_sizes_[false]);
    build_context_menu();
    apply_font_scale();
    apply_mouse_transparency();
    for (auto* screen : QGuiApplication::screens()) observe_screen(screen);
    connect(qGuiApp, &QGuiApplication::screenAdded, this, [this](QScreen* screen) {
        observe_screen(screen);
        fit_current_placement();
    });
    connect(qGuiApp, &QGuiApplication::screenRemoved, this,
            [this](QScreen*) { fit_current_placement(); });
    wardogs::i18n::watch(this);
}

void PinnedResultWindow::build_context_menu() {
    context_menu_ = new RoundedPopup(this);
    context_menu_->setObjectName(QStringLiteral("pinnedContextMenu"));
    set_popup_opacity(context_menu_, preferences_.opacity_percent / 100.0);

    auto* layout = new QVBoxLayout(context_menu_);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);
    auto* card_caption = new QLabel(wardogs::i18n::text(QStringLiteral("Карточка · прозрачность")));
    card_caption->setObjectName(QStringLiteral("muted"));
    layout->addWidget(card_caption);

    auto* controls = new QWidget(context_menu_);
    auto* controls_layout = new QHBoxLayout(controls);
    controls_layout->setContentsMargins(0, 0, 0, 0);
    controls_layout->setSpacing(8);

    lock_button_ = new QToolButton(context_menu_);
    lock_button_->setObjectName(QStringLiteral("pinnedLockButton"));
    lock_button_->setProperty("pinnedMenuButton", true);
    lock_button_->setCheckable(true);
    lock_button_->setAutoRaise(false);
    lock_button_->setFixedSize(42, 40);
    lock_button_->setIconSize(QSize(22, 22));
    controls_layout->addWidget(lock_button_);

    opacity_slider_ = new JumpSlider(Qt::Horizontal, context_menu_);
    opacity_slider_->setObjectName(QStringLiteral("pinnedOpacitySlider"));
    opacity_slider_->setProperty("pinnedMenuSlider", true);
    opacity_slider_->setRange(Preferences::minimum_opacity_percent,
                              Preferences::maximum_opacity_percent);
    opacity_slider_->setValue(preferences_.opacity_percent);
    opacity_slider_->setMinimumWidth(150);
    opacity_slider_->setToolTip(
        wardogs::i18n::text(QStringLiteral("Прозрачность карточки: %1%")).arg(preferences_.opacity_percent));
    opacity_slider_->setAccessibleName(wardogs::i18n::text(QStringLiteral("Прозрачность карточки результата")));
    controls_layout->addWidget(opacity_slider_);
    layout->addWidget(controls);

    topmost_button_ = new QToolButton(context_menu_);
    topmost_button_->setObjectName(QStringLiteral("pinnedTopmostButton"));
    topmost_button_->setCheckable(true);
    topmost_button_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    topmost_button_->setText(wardogs::i18n::text(QStringLiteral("Поверх остальных окон")));
    topmost_button_->setToolTip(topmost_button_->text());
    topmost_button_->setAccessibleName(topmost_button_->text());
    topmost_button_->setChecked(preferences_.always_on_top);
    layout->addWidget(topmost_button_);

    auto* reticle_caption = new QLabel(wardogs::i18n::text(QStringLiteral("Прицел · прозрачность")));
    reticle_caption->setObjectName(QStringLiteral("muted"));
    layout->addWidget(reticle_caption);

    auto* ghost_row = new QWidget(context_menu_);
    ghost_row->setObjectName(QStringLiteral("pinnedGhostRow"));
    auto* ghost_layout = new QHBoxLayout(ghost_row);
    ghost_layout->setContentsMargins(0, 0, 0, 0);
    ghost_layout->setSpacing(8);
    ghost_button_ = new QToolButton(context_menu_);
    ghost_button_->setObjectName(QStringLiteral("ghostReticleToggle"));
    ghost_button_->setProperty("pinnedMenuButton", true);
    ghost_button_->setCheckable(true);
    ghost_button_->setAutoRaise(false);
    ghost_button_->setFixedSize(42, 40);
    ghost_button_->setIconSize(QSize(22, 22));
    ghost_layout->addWidget(ghost_button_);
    ghost_opacity_slider_ = new JumpSlider(Qt::Horizontal, context_menu_);
    ghost_opacity_slider_->setObjectName(
        QStringLiteral("ghostReticleOpacitySlider"));
    ghost_opacity_slider_->setProperty("pinnedMenuSlider", true);
    ghost_opacity_slider_->setRange(20, 100);
    ghost_opacity_slider_->setValue(ghost_opacity_percent_);
    ghost_opacity_slider_->setMinimumWidth(150);
    ghost_opacity_slider_->setAccessibleName(wardogs::i18n::text(QStringLiteral("Прозрачность прицела")));
    ghost_layout->addWidget(ghost_opacity_slider_);
    layout->addWidget(ghost_row);

    auto* hotkey_row = new QWidget(context_menu_);
    hotkey_row->setObjectName(QStringLiteral("pinnedUnlockRow"));
    auto* hotkey_layout = new QVBoxLayout(hotkey_row);
    hotkey_layout->setContentsMargins(0, 1, 0, 0);
    hotkey_layout->setSpacing(4);
    auto* hotkey_label = new QLabel(wardogs::i18n::text(QStringLiteral("Разблокировать")), hotkey_row);
    hotkey_label->setObjectName(QStringLiteral("pinnedUnlockLabel"));
    hotkey_layout->addWidget(hotkey_label);
    unlock_hotkey_ = new QKeySequenceEdit(
        QKeySequence(QString::fromStdWString(preferences_.unlock_hotkey)),
        hotkey_row);
    unlock_hotkey_->setObjectName(QStringLiteral("pinnedUnlockHotkey"));
    unlock_hotkey_->setMaximumSequenceLength(1);
    unlock_hotkey_->setMinimumWidth(140);
    unlock_hotkey_->setMaximumWidth(280);
    unlock_hotkey_->setToolTip(wardogs::i18n::text(QStringLiteral("Сочетание работает даже при заблокированной карточке")));
    unlock_hotkey_->setAccessibleName(wardogs::i18n::text(QStringLiteral("Горячая клавиша разблокировки")));
    hotkey_layout->addWidget(unlock_hotkey_, 1);
    layout->addWidget(hotkey_row);
    auto* interaction_hint = new QLabel(wardogs::i18n::text(QStringLiteral(
        "Перетащите карточку или её края.")));
    interaction_hint->setObjectName(QStringLiteral("muted"));
    interaction_hint->setWordWrap(true);
    interaction_hint->setMaximumWidth(280);
    interaction_hint->setToolTip(wardogs::i18n::text(QStringLiteral(
        "Перетаскивайте карточку и её края.\nБлокировка пропускает нажатия в игру.")));
    layout->addWidget(interaction_hint);
    auto* return_button = new QPushButton(wardogs::i18n::text(QStringLiteral("Вернуться в калькулятор")));
    return_button->setObjectName(QStringLiteral("pinnedReturnButton"));
    connect(return_button, &QPushButton::clicked, this, [this] {
        context_menu_->hide();
        if (exit_callback_) exit_callback_();
    });
    layout->addWidget(return_button);

    connect(lock_button_, &QToolButton::toggled, this,
            [this](bool locked) { set_locked(locked); });
    connect(topmost_button_, &QToolButton::toggled, this,
            [this](bool enabled) { set_always_on_top(enabled); });
    connect(opacity_slider_, &QSlider::valueChanged, this,
            [this](int value) { set_opacity_percent(value); });
    connect(ghost_button_, &QToolButton::toggled,
            this, [this](bool enabled) {
                ghost_enabled_ = enabled;
                ghost_button_->setIcon(reticle_icon(enabled));
                ghost_button_->setToolTip(enabled
                    ? wardogs::i18n::text(QStringLiteral("Выключить прицел"))
                    : wardogs::i18n::text(QStringLiteral("Показать прицел")));
                if (ghost_enabled_changed_) ghost_enabled_changed_(enabled);
            });
    connect(ghost_opacity_slider_, &QSlider::valueChanged,
            this, [this](int value) {
                ghost_opacity_percent_ = std::clamp(value, 20, 100);
                ghost_opacity_slider_->setToolTip(
                    wardogs::i18n::text(QStringLiteral("Прозрачность прицела: %1%"))
                        .arg(ghost_opacity_percent_));
                if (ghost_opacity_changed_)
                    ghost_opacity_changed_(ghost_opacity_percent_);
            });
    connect(unlock_hotkey_, &QKeySequenceEdit::editingFinished, this, [this] {
        try {
            const auto parsed = wardogs::parse_hotkey(
                unlock_hotkey_->keySequence()
                    .toString(QKeySequence::PortableText)
                    .toStdWString());
            auto candidate = preferences_;
            candidate.unlock_hotkey = parsed.display;
            if (!commit_preferences(std::move(candidate))) {
                update_unlock_hotkey_control();
                unlock_hotkey_->setToolTip(
                    wardogs::i18n::text(QStringLiteral("Сочетание занято или совпадает с другим действием")));
                return;
            }
            update_unlock_hotkey_control();
            unlock_hotkey_->setToolTip(
                wardogs::i18n::text(QStringLiteral("Сочетание работает даже при заблокированной карточке")));
        } catch (const std::exception&) {
            update_unlock_hotkey_control();
            unlock_hotkey_->setToolTip(wardogs::i18n::text(QStringLiteral("Введите одно действительное сочетание клавиш")));
        }
    });
    update_lock_control();
    update_unlock_hotkey_control();
    fit_unlock_editor_height();
    set_ghost_enabled(false);
    set_ghost_opacity_percent(ghost_opacity_percent_);
    wardogs::i18n::watch(context_menu_);
}

void PinnedResultWindow::configure_ghost_controls(
    bool enabled, int opacity_percent,
    GhostEnabledChanged enabled_changed,
    GhostOpacityChanged opacity_changed) {
    ghost_enabled_changed_ = std::move(enabled_changed);
    ghost_opacity_changed_ = std::move(opacity_changed);
    set_ghost_enabled(enabled);
    set_ghost_opacity_percent(opacity_percent);
}

void PinnedResultWindow::set_ghost_enabled(bool enabled) {
    ghost_enabled_ = enabled;
    if (!ghost_button_) return;
    const QSignalBlocker blocker(ghost_button_);
    ghost_button_->setChecked(enabled);
    ghost_button_->setIcon(reticle_icon(enabled));
    ghost_button_->setToolTip(enabled
        ? wardogs::i18n::text(QStringLiteral("Выключить прицел"))
        : wardogs::i18n::text(QStringLiteral("Показать прицел")));
    ghost_button_->setAccessibleName(ghost_button_->toolTip());
}

void PinnedResultWindow::set_ghost_opacity_percent(int opacity_percent) {
    ghost_opacity_percent_ = std::clamp(opacity_percent, 20, 100);
    if (!ghost_opacity_slider_) return;
    const QSignalBlocker blocker(ghost_opacity_slider_);
    ghost_opacity_slider_->setValue(ghost_opacity_percent_);
    ghost_opacity_slider_->setToolTip(
        wardogs::i18n::text(QStringLiteral("Прозрачность прицела: %1%"))
            .arg(ghost_opacity_percent_));
}

void PinnedResultWindow::update_lock_control() {
    if (!lock_button_) return;
    const QSignalBlocker blocker(lock_button_);
    lock_button_->setChecked(preferences_.locked);
    lock_button_->setIcon(lock_icon(preferences_.locked));
    lock_button_->setToolTip(preferences_.locked
                                 ? wardogs::i18n::text(QStringLiteral("Разблокировать карточку"))
                                 : wardogs::i18n::text(QStringLiteral("Заблокировать карточку")));
    lock_button_->setAccessibleName(lock_button_->toolTip());
    update_header_controls();
    setToolTip(preferences_.locked
        ? wardogs::i18n::text(QStringLiteral("Карточка заблокирована. Разблокировка: %1"))
              .arg(QString::fromStdWString(preferences_.unlock_hotkey))
        : wardogs::i18n::text(QStringLiteral("Тяните карточку для перемещения, края — для размера. "
                         "Правая кнопка — настройки, двойной щелчок — возврат.")));
}

void PinnedResultWindow::update_header_controls() {
    if (weapon_caption_)
        weapon_caption_->setText(vehicle_mode_ ? QStringLiteral("SPH-2") : QStringLiteral("L81"));
    if (lock_hint_) {
        lock_hint_->setText(wardogs::i18n::text(QStringLiteral("Разблокировать: %1"))
            .arg(QString::fromStdWString(preferences_.unlock_hotkey)));
        lock_hint_->setToolTip(wardogs::i18n::text(QStringLiteral("Карточка заблокирована. Разблокировка: %1"))
            .arg(QString::fromStdWString(preferences_.unlock_hotkey)));
        lock_hint_->setAccessibleDescription(lock_hint_->toolTip());
        lock_hint_->setVisible(preferences_.locked);
    }
    if (header_lock_button_) {
        const QSignalBlocker blocker(header_lock_button_);
        header_lock_button_->setChecked(preferences_.locked);
        header_lock_button_->setIcon(lock_icon(preferences_.locked));
        header_lock_button_->setToolTip(preferences_.locked
            ? wardogs::i18n::text(QStringLiteral("Карточка заблокирована. Разблокировка: %1"))
                  .arg(QString::fromStdWString(preferences_.unlock_hotkey))
            : wardogs::i18n::text(QStringLiteral("Заблокировать карточку")));
        header_lock_button_->setAccessibleName(header_lock_button_->toolTip());
    }
    if (controls_button_) {
        controls_button_->setToolTip(wardogs::i18n::text(QStringLiteral("Настройки")));
        controls_button_->setAccessibleName(controls_button_->toolTip());
    }
    if (header_return_button_) {
        header_return_button_->setToolTip(wardogs::i18n::text(QStringLiteral("Вернуться в калькулятор")));
        header_return_button_->setAccessibleName(header_return_button_->toolTip());
    }
    if (topmost_button_) {
        const QSignalBlocker blocker(topmost_button_);
        topmost_button_->setChecked(preferences_.always_on_top);
    }
}

void PinnedResultWindow::update_unlock_hotkey_control() {
    if (!unlock_hotkey_) return;
    const QSignalBlocker blocker(unlock_hotkey_);
    unlock_hotkey_->setKeySequence(
        QKeySequence(QString::fromStdWString(preferences_.unlock_hotkey)));
}

void PinnedResultWindow::fit_unlock_editor_height() const {
    if (!unlock_hotkey_) return;
    unlock_hotkey_->ensurePolished();
    auto* editor = unlock_hotkey_->findChild<QLineEdit*>();
    if (!editor) return;
    editor->ensurePolished();
    // The native QKeySequenceEdit hint can stay at 22 DIP after its internal
    // line edit adopts the application's padded 40-DIP style at fractional DPI.
    // Give the actual child its required room, including any larger font/style.
    unlock_hotkey_->setMinimumHeight(std::max({40, editor->minimumSizeHint().height(),
                                               editor->sizeHint().height()}));
    if (auto* inner_layout = unlock_hotkey_->layout()) {
        inner_layout->invalidate();
        inner_layout->activate();
    }
    unlock_hotkey_->updateGeometry();
    // Hidden construction can leave the wrapper's cached hint at the old
    // editor height. Refresh it synchronously before sizing the outer popup.
    if (auto* row = unlock_hotkey_->parentWidget()) {
        if (auto* row_layout = row->layout()) {
            row_layout->invalidate();
            row_layout->activate();
            row->setMinimumHeight(row_layout->totalMinimumSize().height());
        }
        row->updateGeometry();
    }
    if (auto* popup_layout = context_menu_->layout()) {
        popup_layout->invalidate();
        popup_layout->activate();
    }
    context_menu_->updateGeometry();
}

void PinnedResultWindow::configure_unlock_hotkey(const std::wstring& hotkey) {
    preferences_.unlock_hotkey = wardogs::parse_hotkey(hotkey).display;
    update_unlock_hotkey_control();
    update_lock_control();
}

bool PinnedResultWindow::commit_preferences(Preferences preferences) {
    if (preferences_changed_ && !preferences_changed_(preferences)) return false;
    preferences_ = std::move(preferences);
    return true;
}

void PinnedResultWindow::set_locked(bool locked) {
    if (preferences_.locked == locked) return;
    auto candidate = preferences_;
    candidate.locked = locked;
    if (!commit_preferences(std::move(candidate))) {
        update_lock_control();
        return;
    }
    if ((dragging_ || !resize_edges_.empty()) && geometry() != gesture_start_geometry_)
        queue_geometry_preferences();
    dragging_ = false;
    resize_edges_.clear();
    setCursor(Qt::ArrowCursor);
    update_lock_control();
    if (locked && context_menu_) context_menu_->hide();
    apply_mouse_transparency();
}

void PinnedResultWindow::set_always_on_top(bool enabled) {
    if (preferences_.always_on_top == enabled) return;
    auto candidate = preferences_;
    candidate.always_on_top = enabled;
    if (!commit_preferences(std::move(candidate))) {
        update_header_controls();
        return;
    }
    const QRect previous_geometry = geometry();
    const bool was_visible = isVisible();
    const bool menu_visible = context_menu_ && context_menu_->isVisible();
    setWindowFlag(Qt::WindowStaysOnTopHint, enabled);
    setGeometry(previous_geometry);
    if (was_visible) show();
    SetWindowPos(reinterpret_cast<HWND>(winId()),
                 enabled ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    apply_mouse_transparency();
    update_header_controls();
    if (menu_visible) show_context_menu();
}

void PinnedResultWindow::set_opacity_percent(int opacity_percent) {
    const int clamped = std::clamp(
        opacity_percent, Preferences::minimum_opacity_percent,
        Preferences::maximum_opacity_percent);
    if (preferences_.opacity_percent == clamped) {
        if (opacity_slider_)
            opacity_slider_->setToolTip(
                wardogs::i18n::text(QStringLiteral("Прозрачность карточки: %1%")).arg(clamped));
        return;
    }
    auto candidate = preferences_;
    candidate.opacity_percent = clamped;
    if (!commit_preferences(std::move(candidate))) return;
    setWindowOpacity(clamped / 100.0);
    if (context_menu_) set_popup_opacity(context_menu_, clamped / 100.0);
    if (opacity_slider_) {
        const QSignalBlocker blocker(opacity_slider_);
        opacity_slider_->setValue(clamped);
        opacity_slider_->setToolTip(
            wardogs::i18n::text(QStringLiteral("Прозрачность карточки: %1%")).arg(clamped));
    }
}

void PinnedResultWindow::apply_mouse_transparency() {
    const bool was_visible = isVisible();
    setAttribute(Qt::WA_TransparentForMouseEvents, preferences_.locked);
    const bool has_input_transparency =
        windowFlags().testFlag(Qt::WindowTransparentForInput);
    if (has_input_transparency != preferences_.locked) {
        const QRect previous_geometry = geometry();
        auto flags = windowFlags();
        if (preferences_.locked)
            flags |= Qt::WindowTransparentForInput;
        else
            flags &= ~Qt::WindowTransparentForInput;
        setWindowFlags(flags);
        setGeometry(previous_geometry);
    }
    const auto handle = reinterpret_cast<HWND>(winId());
    auto extended_style = GetWindowLongPtrW(handle, GWL_EXSTYLE);
    extended_style |= WS_EX_LAYERED | WS_EX_NOACTIVATE;
    extended_style |= WS_EX_APPWINDOW;
    extended_style &= ~static_cast<LONG_PTR>(WS_EX_TOOLWINDOW);
    if (preferences_.locked)
        extended_style |= WS_EX_TRANSPARENT;
    else
        extended_style &= ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT);
    SetWindowLongPtrW(handle, GWL_EXSTYLE, extended_style);
    SetWindowPos(handle, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                     SWP_FRAMECHANGED);
    if (was_visible && !isVisible()) {
        show();
        raise();
    }
}

QWidget* PinnedResultWindow::result_card(const QString& color, const QString& caption,
                                         QLabel*& value,
                                         QLabel** secondary) {
    auto* card = new QFrame;
    card->setObjectName(QStringLiteral("resultCard"));
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(8, 5, 8, 6);
    layout->setSpacing(0);
    auto* heading = new QLabel(caption);
    heading->setObjectName(QStringLiteral("pinnedMetricCaption"));
    heading->setAlignment(Qt::AlignCenter);
    heading->setStyleSheet(QStringLiteral("color:#8da1b9;font-size:10px;font-weight:600;"));
    layout->addWidget(heading);
    value = new QLabel(QStringLiteral("—"));
    value->setAlignment(Qt::AlignCenter);
    value->setStyleSheet(QStringLiteral(
        "color:%1;font-family:'Segoe UI';font-size:30px;font-weight:700;")
                             .arg(color));
    layout->addWidget(value);
    if (secondary) {
        *secondary = new QLabel(QStringLiteral("—"));
        (*secondary)->setObjectName(QStringLiteral("pinnedMortarMil"));
        (*secondary)->setAlignment(Qt::AlignCenter);
        (*secondary)->setStyleSheet(QStringLiteral(
            "color:#e8eef7;font-family:'Segoe UI';font-size:18px;"
            "font-weight:700;"));
        layout->addWidget(*secondary);
    }
    return card;
}

bool PinnedResultWindow::event(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        update_lock_control();
        set_ghost_enabled(ghost_enabled_);
        set_ghost_opacity_percent(ghost_opacity_percent_);
        if (opacity_slider_)
            opacity_slider_->setToolTip(
                wardogs::i18n::text(QStringLiteral("Прозрачность карточки: %1%"))
                    .arg(preferences_.opacity_percent));
        update_workflow_status_layout(workflow_status_extra());
        apply_font_scale();
    }
    if (event->type() == QEvent::HoverMove && !dragging_ && resize_edges_.empty()) {
        const auto* hover = static_cast<QHoverEvent*>(event);
        setCursor(preferences_.locked
                      ? Qt::ArrowCursor
                      : cursor_for_edges(
                            resize_edges_at(hover->position().toPoint())));
    }
    return QWidget::event(event);
}

bool PinnedResultWindow::nativeEvent(const QByteArray& event_type, void* message,
                                     qintptr* result) {
    auto* native_message = static_cast<MSG*>(message);
    if (native_message && native_message->message == WM_NCHITTEST &&
        preferences_.locked) {
        *result = HTTRANSPARENT;
        return true;
    }
    if (native_message && native_message->message == WM_SETCURSOR) {
        LPCWSTR cursor_id = IDC_ARROW;
        if (!preferences_.locked) {
            const auto edges = resize_edges_at(mapFromGlobal(QCursor::pos()));
            if (edges == Edges{"left", "top"} ||
                edges == Edges{"bottom", "right"})
                cursor_id = IDC_SIZENWSE;
            else if (edges == Edges{"right", "top"} ||
                     edges == Edges{"bottom", "left"})
                cursor_id = IDC_SIZENESW;
            else if (edges.contains("left") || edges.contains("right"))
                cursor_id = IDC_SIZEWE;
            else if (edges.contains("top") || edges.contains("bottom"))
                cursor_id = IDC_SIZENS;
        }
        ::SetCursor(::LoadCursorW(nullptr, cursor_id));
        *result = TRUE;
        return true;
    }
    return QWidget::nativeEvent(event_type, message, result);
}

bool PinnedResultWindow::hasHeightForWidth() const {
    // Windows checks height-for-width before delivering resizeEvent, while
    // the rows still use the previous canvas's font. The adaptive vehicle
    // card owns its stable minimum and fits fonts after accepting that canvas;
    // its children's wrapping must not impose the old font's native minimum.
    return !vehicle_mode_ && QWidget::hasHeightForWidth();
}

std::optional<wardogs::PinnedCardPlacement> PinnedResultWindow::current_placement() const {
    auto* current_screen = QGuiApplication::screenAt(frameGeometry().center());
    if (!current_screen) current_screen = screen();
    if (!current_screen) return std::nullopt;
    wardogs::PinnedCardPlacement placement{
        current_screen->name().toStdWString(),
        card_rect(current_screen->availableGeometry()), card_rect(geometry())};
    return wardogs::valid_pinned_card_placement(placement)
        ? std::optional(placement) : std::nullopt;
}

void PinnedResultWindow::prepare_for_show(const QRect& anchor) {
    placement_anchor_ = anchor;
    const auto screens = current_screens();
    std::optional<wardogs::PinnedCardPlacement> saved = preferences_.placement;
    QSize requested = mode_sizes_[vehicle_mode_] + QSize(0, workflow_status_extra());
    if (placement_initialized_) {
        saved = live_placement_ ? live_placement_ : current_placement();
        if (saved) saved->rect = card_rect(geometry());
        requested = size();
    }
    requested = requested.expandedTo(minimumSize());
    const auto restored = wardogs::restore_pinned_card(
        saved, screens, {requested.width(), requested.height()}, card_rect(anchor));
    if (restored) {
        setGeometry(qt_rect(restored->rect));
        live_placement_ = *restored;
    }
    placement_initialized_ = true;
}

void PinnedResultWindow::observe_screen(QScreen* observed) {
    connect(observed, &QScreen::availableGeometryChanged, this,
            [this](const QRect&) { fit_current_placement(); });
}

void PinnedResultWindow::fit_current_placement() {
    if (!placement_initialized_) return;
    auto saved = live_placement_ ? live_placement_ : current_placement();
    if (saved) saved->rect = card_rect(geometry());
    const auto screens = current_screens();
    const auto restored = wardogs::restore_pinned_card(
        saved, screens, {width(), height()}, card_rect(placement_anchor_));
    if (!restored) return;
    if (geometry() != qt_rect(restored->rect)) setGeometry(qt_rect(restored->rect));
    live_placement_ = *restored;
}

void PinnedResultWindow::queue_geometry_preferences() {
    pending_placement_ = current_placement();
    if (!pending_placement_) return;
    live_placement_ = pending_placement_;
    for (bool vehicle : {false, true}) {
        const QSize canvas = mode_sizes_[vehicle].expandedTo(minimum_size(vehicle));
        pending_mode_sizes_[vehicle ? 1 : 0] = wardogs::PinnedCardSize{canvas.width(), canvas.height()};
    }
    geometry_save_timer_->start();
}

bool PinnedResultWindow::flush_preferences() {
    geometry_save_timer_->stop();
    if (!pending_placement_) return true;
    auto candidate = preferences_;
    candidate.placement = pending_placement_;
    candidate.mode_sizes = pending_mode_sizes_;
    if (!commit_preferences(std::move(candidate))) return false;
    pending_placement_.reset();
    return true;
}

void PinnedResultWindow::set_mode(bool vehicle_mode) {
    mortar_panel_->setVisible(!vehicle_mode);
    vehicle_panel_->setVisible(vehicle_mode);
    if (vehicle_mode != vehicle_mode_) {
        mode_sizes_[vehicle_mode_] = QSize(width(), height() - workflow_status_extra());
        vehicle_mode_ = vehicle_mode;
        const QSize requested = mode_sizes_[vehicle_mode_] + QSize(0, workflow_status_extra());
        setMinimumSize(minimum_size(vehicle_mode_) + QSize(0, workflow_status_extra()));
        resize(requested.expandedTo(minimumSize()));
    }
    update_header_controls();
    apply_font_scale();
}

void PinnedResultWindow::set_values(const QString& distance,
                                    const QString& bearing,
                                    const QString& mortar_mil) {
    distance_->setText(wardogs::i18n::text(distance));
    bearing_->setText(wardogs::i18n::text(bearing));
    mortar_mil_->setText(mortar_mil.isEmpty() ? QStringLiteral("—")
                                               : wardogs::i18n::text(QStringLiteral("Наводка: %1"))
                                                     .arg(mortar_mil));
}

void PinnedResultWindow::set_vehicle_values(const VehicleSolutionWidget& low,
                                            const VehicleSolutionWidget& high) {
    low_->copy_from(low);
    high_->copy_from(high);
    if (vehicle_mode_) apply_font_scale();
}

void PinnedResultWindow::set_selected_arc(std::optional<wardogs::Arc> arc) {
    low_->set_selected(arc == wardogs::Arc::low);
    high_->set_selected(arc == wardogs::Arc::high);
    if (vehicle_mode_) apply_font_scale();
}

void PinnedResultWindow::set_error(bool error) {
    if (frame_->property("error").toBool() == error) return;
    frame_->setProperty("error", error);
    frame_->style()->unpolish(frame_);
    frame_->style()->polish(frame_);
    frame_->update();
    if (vehicle_mode_) apply_font_scale();
}

int PinnedResultWindow::workflow_status_extra() const {
    const int context = context_caption_ && !context_caption_->isHidden()
        ? context_caption_->height() + 6 : 0;
    const int workflow = workflow_status_ && !workflow_status_->isHidden()
        ? workflow_status_->height() + 6 : 0;
    return context + workflow;
}

void PinnedResultWindow::set_context_caption(const QString& caption, const QString& detail) {
    const int previous_extra = workflow_status_extra();
    const QString localized = wardogs::i18n::text(caption);
    const QString description = detail.isEmpty() ? localized : wardogs::i18n::text(detail);
    context_caption_->setText(localized);
    context_caption_->setToolTip(description);
    context_caption_->setAccessibleDescription(description);
    context_caption_->setVisible(!caption.isEmpty());
    update_workflow_status_layout(previous_extra);
}

void PinnedResultWindow::set_workflow_status(const QString& text, const QString& detail) {
    const int previous_extra = workflow_status_extra();
    const bool visible = !text.isEmpty();
    const auto localized = wardogs::i18n::text(text);
    workflow_status_->setText(localized);
    const QString description = detail.isEmpty() ? localized : wardogs::i18n::text(detail);
    workflow_status_->setToolTip(description);
    workflow_status_->setAccessibleDescription(description);
    workflow_status_->setVisible(visible);
    update_workflow_status_layout(previous_extra);
}

void PinnedResultWindow::update_workflow_status_layout(int previous_extra,
                                                       bool preserve_result_height) {
    if (!workflow_status_ || updating_workflow_status_layout_) return;
    updating_workflow_status_layout_ = true;
    const QSize previous_size = size();
    // Release the old fixed height before measuring: QLabel's heightForWidth
    // otherwise includes minimumHeight and can grow but never shrink.
    workflow_status_->setMinimumHeight(0);
    workflow_status_->setMaximumHeight(QWIDGETSIZE_MAX);
    const QMargins frame_padding = frame_->contentsMargins();
    const QMargins layout_padding = frame_->layout()->contentsMargins();
    const int horizontal_padding = frame_padding.left() + frame_padding.right() +
        layout_padding.left() + layout_padding.right();
    const int text_height = workflow_status_->heightForWidth(
        std::max(1, width() - horizontal_padding));
    workflow_status_->setFixedHeight(std::max(18, text_height));
    context_caption_->setMinimumHeight(0);
    context_caption_->setMaximumHeight(QWIDGETSIZE_MAX);
    const int context_height = context_caption_->heightForWidth(
        std::max(1, width() - horizontal_padding));
    context_caption_->setFixedHeight(std::max(18, context_height));
    const int extra = workflow_status_extra();
    const int difference = extra - previous_extra;
    setMinimumSize(minimum_size(vehicle_mode_) + QSize(0, extra));
    // A changed message adds/removes its own space. During user resizing the
    // requested canvas already includes the footer, so wrapping only changes
    // its share of that canvas; the content minimum enforces the remaining room.
    if (preserve_result_height && difference != 0)
        resize(QSize(previous_size.width(), previous_size.height() + difference)
                   .expandedTo(minimumSize()));
    updating_workflow_status_layout_ = false;
    apply_font_scale();
}

PinnedResultWindow::Edges PinnedResultWindow::resize_edges_at(QPoint position) const {
    Edges result;
    if (position.x() <= resize_margin)
        result.insert("left");
    else if (position.x() >= width() - resize_margin - 1)
        result.insert("right");
    if (position.y() <= resize_margin)
        result.insert("top");
    else if (position.y() >= height() - resize_margin - 1)
        result.insert("bottom");
    return result;
}

Qt::CursorShape PinnedResultWindow::cursor_for_edges(const Edges& edges) {
    if (edges == Edges{"left", "top"} || edges == Edges{"bottom", "right"})
        return Qt::SizeFDiagCursor;
    if (edges == Edges{"right", "top"} || edges == Edges{"bottom", "left"})
        return Qt::SizeBDiagCursor;
    if (edges.contains("left") || edges.contains("right"))
        return Qt::SizeHorCursor;
    if (edges.contains("top") || edges.contains("bottom"))
        return Qt::SizeVerCursor;
    return Qt::ArrowCursor;
}

void PinnedResultWindow::resize_from_pointer(QPoint pointer) {
    const auto delta = pointer - resize_start_global_;
    auto resized = resize_start_geometry_;
    if (resize_edges_.contains("left"))
        resized.setLeft(std::min(resize_start_geometry_.left() + delta.x(),
                                 resize_start_geometry_.right() - minimumWidth() + 1));
    if (resize_edges_.contains("right"))
        resized.setRight(std::max(resize_start_geometry_.right() + delta.x(),
                                  resize_start_geometry_.left() + minimumWidth() - 1));
    if (resize_edges_.contains("top"))
        resized.setTop(std::min(resize_start_geometry_.top() + delta.y(),
                                resize_start_geometry_.bottom() - minimumHeight() + 1));
    if (resize_edges_.contains("bottom"))
        resized.setBottom(std::max(
            resize_start_geometry_.bottom() + delta.y(),
            resize_start_geometry_.top() + minimumHeight() - 1));
    setGeometry(resized);
}

void PinnedResultWindow::apply_font_scale() {
    if (applying_font_scale_) return;
    applying_font_scale_ = true;
    const auto base = default_size(vehicle_mode_);
    const auto requested_scale = [&] {
        return std::clamp(
            std::min(width() / static_cast<double>(base.width()),
                     (height() - workflow_status_extra() - header_extra) /
                         static_cast<double>(base.height() - header_extra)),
            minimum_font_scale, maximum_font_scale);
    };
    if (vehicle_mode_) {
        frame_->ensurePolished();
        const auto measure = [&](double scale) {
            low_->set_compact_scale(scale);
            high_->set_compact_scale(scale);
            // Propagate the actual row minima, including stylesheet borders,
            // through both containers before measuring the top-level layout.
            for (auto* panel : {vehicle_panel_, static_cast<QWidget*>(frame_)}) {
                panel->layout()->invalidate();
                panel->layout()->activate();
                panel->updateGeometry();
            }
            layout()->invalidate();
            return layout()->totalMinimumSize();
        };
        // The resize floor is measured with the smallest permitted font. A
        // minimum based on an enlarged font would prevent shrinking the card.
        const QSize content_floor = measure(minimum_font_scale);
        setMinimumSize((minimum_size(true) + QSize(0, workflow_status_extra()))
                           .expandedTo(content_floor));
        const double requested = requested_scale();
        font_scale_ = requested;
        const QSize requested_minimum = measure(requested);
        if (requested_minimum.width() > width() || requested_minimum.height() > height()) {
            // Find the largest font that fits the current canvas. Integer font
            // sizes make this monotonic; eight bounded steps avoid resize loops.
            double fits = minimum_font_scale;
            double clips = requested;
            for (int attempt = 0; attempt < 8; ++attempt) {
                const double candidate = (fits + clips) / 2;
                const QSize needed = measure(candidate);
                if (needed.width() <= width() && needed.height() <= height()) fits = candidate;
                else clips = candidate;
            }
            font_scale_ = fits;
            measure(font_scale_);
        }
        layout()->activate();
        applying_font_scale_ = false;
        return;
    }
    font_scale_ = requested_scale();
    const int size = std::max(22, qRound(30 * font_scale_));
    distance_->setStyleSheet(QStringLiteral(
        "color:#f0b45d;font-family:'Segoe UI';font-size:%1px;font-weight:700;")
                                 .arg(size));
    bearing_->setStyleSheet(QStringLiteral(
        "color:#63d8c5;font-family:'Segoe UI';font-size:%1px;font-weight:700;")
                                .arg(size));
    const int mil_size = std::max(15, qRound(18 * font_scale_));
    mortar_mil_->setStyleSheet(QStringLiteral(
        "color:#e8eef7;font-family:'Segoe UI';font-size:%1px;"
        "font-weight:700;")
                                   .arg(mil_size));
    applying_font_scale_ = false;
}

void PinnedResultWindow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (!applying_font_scale_ && !updating_workflow_status_layout_) {
        update_workflow_status_layout(workflow_status_extra(), false);
        if (low_) apply_font_scale();
    }
    mode_sizes_[vehicle_mode_] = QSize(width(), std::max(1, height() - workflow_status_extra()));
}

void PinnedResultWindow::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    apply_mouse_transparency();
    apply_font_scale();
}

void PinnedResultWindow::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        if (preferences_.locked) {
            event->accept();
            return;
        }
        gesture_start_geometry_ = geometry();
        const auto edges = resize_edges_at(event->position().toPoint());
        if (!edges.empty()) {
            resize_edges_ = edges;
            resize_start_global_ = event->globalPosition().toPoint();
            resize_start_geometry_ = geometry();
            setCursor(cursor_for_edges(edges));
            event->accept();
            return;
        }
        dragging_ = true;
        drag_offset_ = event->globalPosition().toPoint() - frameGeometry().topLeft();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void PinnedResultWindow::mouseMoveEvent(QMouseEvent* event) {
    if (preferences_.locked) {
        setCursor(Qt::ArrowCursor);
        event->accept();
        return;
    }
    if (!resize_edges_.empty() && (event->buttons() & Qt::LeftButton)) {
        resize_from_pointer(event->globalPosition().toPoint());
        event->accept();
        return;
    }
    if (dragging_ && (event->buttons() & Qt::LeftButton)) {
        move(event->globalPosition().toPoint() - drag_offset_);
        event->accept();
        return;
    }
    setCursor(cursor_for_edges(resize_edges_at(event->position().toPoint())));
    QWidget::mouseMoveEvent(event);
}

void PinnedResultWindow::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        if (preferences_.locked) {
            event->accept();
            return;
        }
        if ((dragging_ || !resize_edges_.empty()) && geometry() != gesture_start_geometry_)
            queue_geometry_preferences();
        resize_edges_.clear();
        dragging_ = false;
        setCursor(cursor_for_edges(resize_edges_at(event->position().toPoint())));
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void PinnedResultWindow::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        if (preferences_.locked) {
            event->accept();
            return;
        }
        dragging_ = false;
        resize_edges_.clear();
        if (exit_callback_) exit_callback_();
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void PinnedResultWindow::leaveEvent(QEvent* event) {
    if (!dragging_ && resize_edges_.empty())
        setCursor(Qt::ArrowCursor);
    QWidget::leaveEvent(event);
}

void PinnedResultWindow::contextMenuEvent(QContextMenuEvent* event) {
    show_context_menu();
    event->accept();
}

void PinnedResultWindow::show_context_menu() {
    if (!context_menu_) build_context_menu();
    update_lock_control();
    const qreal opacity = preferences_.opacity_percent / 100.0;
    set_popup_opacity(context_menu_, opacity);
    context_menu_->move(context_menu_position());
    context_menu_->show();
    context_menu_->raise();
    set_popup_opacity(context_menu_, opacity);
}

QPoint PinnedResultWindow::context_menu_position() const {
    context_menu_->ensurePolished();
    fit_unlock_editor_height();
    context_menu_->adjustSize();
    constexpr int gap = 2;
    const QSize menu_size = context_menu_->sizeHint().expandedTo(context_menu_->size());
    QPoint position = mapToGlobal(QPoint(width() + gap, 0));
    QScreen* screen = QGuiApplication::screenAt(mapToGlobal(rect().center()));
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (!screen) return position;
    const QRect available = screen->availableGeometry();
    if (position.x() + menu_size.width() > available.right() + 1)
        position.setX(mapToGlobal(QPoint(-menu_size.width() - gap, 0)).x());
    const int maximum_x = std::max(
        available.left(), available.right() - menu_size.width() + 1);
    const int maximum_y = std::max(
        available.top(), available.bottom() - menu_size.height() + 1);
    position.setX(std::clamp(position.x(), available.left(), maximum_x));
    position.setY(std::clamp(position.y(), available.top(), maximum_y));
    return position;
}
