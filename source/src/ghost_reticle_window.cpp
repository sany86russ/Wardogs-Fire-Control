#include "ghost_reticle_window.hpp"
#include "localization.hpp"

#include <Windows.h>

#include <QGuiApplication>
#include <QCursor>
#include <QEvent>
#include <QFont>
#include <QIcon>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QScreen>
#include <QToolButton>
#include <QHBoxLayout>
#include <QWindow>

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

QString normalized_monitor_name(QString name) {
    name = name.trimmed();
    if (name.startsWith(QStringLiteral("\\\\.\\"))) name.remove(0, 4);
    return name;
}

QColor ghost_color(int opacity_percent) {
    QColor color(QStringLiteral("#ff3b30"));
    color.setAlphaF(std::clamp(opacity_percent, 20, 100) / 100.0);
    return color;
}

QIcon adjustment_icon(bool confirm) {
    QPixmap pixmap(24, 24);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(QStringLiteral("#e8eef7")), 2.4,
                        Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    if (confirm) {
        QPainterPath path;
        path.moveTo(4.5, 12.5);
        path.lineTo(9.5, 17.0);
        path.lineTo(19.5, 6.5);
        painter.drawPath(path);
    } else {
        painter.drawLine(QPointF(6.0, 6.0), QPointF(18.0, 18.0));
        painter.drawLine(QPointF(18.0, 6.0), QPointF(6.0, 18.0));
    }
    return QIcon(pixmap);
}

}  // namespace

GhostReticleWindow::GhostReticleWindow(Preferences preferences,
                                       SizeSaved size_saved,
                                       QWidget* parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint |
                          Qt::WindowStaysOnTopHint |
                          Qt::WindowDoesNotAcceptFocus),
      preferences_(preferences), size_saved_(std::move(size_saved)) {
    preferences_.opacity_percent = std::clamp(
        preferences_.opacity_percent, Preferences::minimum_opacity_percent,
        Preferences::maximum_opacity_percent);
    preferences_.width = std::clamp(
        preferences_.width, Preferences::minimum_width,
        Preferences::maximum_width);
    preferences_.bearing_compensation_deg = std::clamp(
        preferences_.bearing_compensation_deg,
        Preferences::minimum_bearing_compensation_deg,
        Preferences::maximum_bearing_compensation_deg);
    setObjectName(QStringLiteral("ghostReticleWindow"));
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setMouseTracking(true);

    adjustment_controls_ = new QWidget(this);
    adjustment_controls_->setObjectName(
        QStringLiteral("ghostReticleAdjustmentControls"));
    adjustment_controls_->setAttribute(Qt::WA_StyledBackground);
    adjustment_controls_->setFixedSize(112, 42);
    adjustment_controls_->setStyleSheet(QStringLiteral(
        "QWidget#ghostReticleAdjustmentControls{background:rgba(11,16,24,210);"
        "border:1px solid #63d8c5;border-radius:9px;}"
        "QToolButton{background:transparent;color:#e8eef7;border:0;"
        "font-size:22px;font-weight:700;}"
        "QToolButton:hover{background:rgba(99,216,197,40);border-radius:6px;}"));
    auto* adjustment_layout = new QHBoxLayout(adjustment_controls_);
    adjustment_layout->setContentsMargins(5, 4, 5, 4);
    adjustment_layout->setSpacing(4);
    confirm_button_ = new QToolButton(adjustment_controls_);
    confirm_button_->setObjectName(QStringLiteral("ghostReticleConfirm"));
    confirm_button_->setIcon(adjustment_icon(true));
    confirm_button_->setIconSize(QSize(24, 24));
    confirm_button_->setToolTip(wardogs::i18n::text(QStringLiteral("Сохранить размер")));
    confirm_button_->setAccessibleName(wardogs::i18n::text(QStringLiteral("Сохранить размер прицела")));
    cancel_button_ = new QToolButton(adjustment_controls_);
    cancel_button_->setObjectName(QStringLiteral("ghostReticleCancel"));
    cancel_button_->setIcon(adjustment_icon(false));
    cancel_button_->setIconSize(QSize(24, 24));
    cancel_button_->setToolTip(wardogs::i18n::text(QStringLiteral("Отменить изменение")));
    cancel_button_->setAccessibleName(wardogs::i18n::text(QStringLiteral("Отменить изменение размера прицела")));
    adjustment_layout->addWidget(confirm_button_);
    adjustment_layout->addWidget(cancel_button_);
    adjustment_controls_->hide();
    connect(confirm_button_, &QToolButton::clicked,
            this, [this] { finish_adjustment(true); });
    connect(cancel_button_, &QToolButton::clicked,
            this, [this] { finish_adjustment(false); });

    center_on_screen(preferences_.width);
    apply_input_mode();
    wardogs::i18n::watch(this);
}

void GhostReticleWindow::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) update();
}

void GhostReticleWindow::set_solution(
    std::optional<wardogs::CorrectedSolution> solution) {
    mortar_bearing_.reset();
    mortar_mil_.reset();
    solution_ = std::move(solution);
    update();
    if (overlay_enabled_ && has_reticle_result() && !isVisible()) {
        show();
        raise();
    } else if (!adjusting_ && (!overlay_enabled_ || !has_reticle_result())) {
        hide();
    }
}

void GhostReticleWindow::set_mortar_solution(
    std::optional<double> bearing_degrees, std::optional<double> mil) {
    solution_.reset();
    mortar_bearing_ = std::move(bearing_degrees);
    mortar_mil_ = std::move(mil);
    update();
    if (overlay_enabled_ && has_reticle_result() && !isVisible()) {
        show();
        raise();
    } else if (!adjusting_ && (!overlay_enabled_ || !has_reticle_result())) {
        hide();
    }
}

bool GhostReticleWindow::has_reticle_result() const {
    return solution_.has_value() ||
           (mortar_bearing_.has_value() && mortar_mil_.has_value());
}

void GhostReticleWindow::set_overlay_enabled(bool enabled) {
    overlay_enabled_ = enabled;
    if (adjusting_ || (overlay_enabled_ && has_reticle_result())) {
        show();
        raise();
    } else {
        hide();
    }
    update();
}

void GhostReticleWindow::set_opacity_percent(int opacity_percent) {
    preferences_.opacity_percent = std::clamp(
        opacity_percent, Preferences::minimum_opacity_percent,
        Preferences::maximum_opacity_percent);
    update();
}

void GhostReticleWindow::set_bearing_compensation(double degrees) {
    preferences_.bearing_compensation_deg = std::clamp(
        degrees, Preferences::minimum_bearing_compensation_deg,
        Preferences::maximum_bearing_compensation_deg);
    update();
}

void GhostReticleWindow::set_width(int width) {
    preferences_.width = std::clamp(
        width, Preferences::minimum_width, Preferences::maximum_width);
    center_on_screen(preferences_.width);
    update();
}

void GhostReticleWindow::set_target_monitor(
    const std::wstring& monitor_device) {
    target_monitor_name_ = QString::fromStdWString(monitor_device);
    center_on_screen(preferences_.width);
}

void GhostReticleWindow::cancel_adjustment() {
    if (adjusting_) finish_adjustment(false);
}

void GhostReticleWindow::begin_adjustment() {
    adjusting_ = true;
    resizing_ = false;
    const auto* target = target_screen();
    const qreal pixel_ratio = target ? target->devicePixelRatio() : devicePixelRatioF();
    adjustment_original_width_ = qRound(width() * pixel_ratio);
    center_on_screen(preferences_.width);
    apply_input_mode();
    adjustment_controls_->show();
    adjustment_controls_->move((width() - adjustment_controls_->width()) / 2,
                               height() - adjustment_controls_->height() - 12);
    show();
    raise();
    activateWindow();
    setFocus(Qt::OtherFocusReason);
    update();
}

void GhostReticleWindow::finish_adjustment(bool save) {
    if (!adjusting_) return;
    adjusting_ = false;
    resizing_ = false;
    if (save) {
        const auto* target = target_screen();
        const qreal pixel_ratio = target ? target->devicePixelRatio() : devicePixelRatioF();
        preferences_.width = std::clamp(qRound(width() * pixel_ratio),
                                       Preferences::minimum_width,
                                       Preferences::maximum_width);
        if (size_saved_) size_saved_(preferences_.width);
    } else {
        center_on_screen(adjustment_original_width_);
    }
    adjustment_controls_->hide();
    setCursor(Qt::ArrowCursor);
    apply_input_mode();
    if (!overlay_enabled_ || !has_reticle_result()) hide();
    update();
}

Qt::CursorShape GhostReticleWindow::resize_cursor_at(QPoint position) const {
    if (!adjusting_) return Qt::ArrowCursor;
    constexpr int margin = 28;
    const bool left = position.x() <= margin;
    const bool right = position.x() >= width() - margin - 1;
    const bool top = position.y() <= margin;
    const bool bottom = position.y() >= height() - margin - 1;
    if ((left && top) || (right && bottom)) return Qt::SizeFDiagCursor;
    if ((right && top) || (left && bottom)) return Qt::SizeBDiagCursor;
    if (left || right) return Qt::SizeHorCursor;
    if (top || bottom) return Qt::SizeVerCursor;
    return Qt::ArrowCursor;
}

bool GhostReticleWindow::nativeEvent(const QByteArray& event_type,
                                     void* message, qintptr* result) {
    auto* native_message = static_cast<MSG*>(message);
    if (native_message && native_message->message == WM_SETCURSOR) {
        const Qt::CursorShape shape = resize_cursor_at(
            mapFromGlobal(QCursor::pos()));
        LPCWSTR cursor_id = IDC_ARROW;
        if (shape == Qt::SizeFDiagCursor)
            cursor_id = IDC_SIZENWSE;
        else if (shape == Qt::SizeBDiagCursor)
            cursor_id = IDC_SIZENESW;
        else if (shape == Qt::SizeHorCursor)
            cursor_id = IDC_SIZEWE;
        else if (shape == Qt::SizeVerCursor)
            cursor_id = IDC_SIZENS;
        ::SetCursor(::LoadCursorW(nullptr, cursor_id));
        *result = TRUE;
        return true;
    }
    return QWidget::nativeEvent(event_type, message, result);
}

void GhostReticleWindow::center_on_screen(int requested_width) {
    QScreen* target = target_screen();
    if (!target) return;
    if (windowHandle() && windowHandle()->screen() != target)
        windowHandle()->setScreen(target);
    const QRect screen = target->geometry();
    const qreal pixel_ratio = target->devicePixelRatio();
    // Presets and saved widths describe game pixels. Qt uses logical pixels.
    // Quantizing logical width to four pixels keeps an exact 4:3 rectangle.
    const int maximum_width = std::max(4,
        std::min(screen.width(), screen.height() * 4 / 3) / 4 * 4);
    const int minimum_width = std::min(maximum_width,
        std::max(4, qCeil(Preferences::minimum_width / pixel_ratio / 4.0) * 4));
    const int width_value = std::clamp(
        qRound(requested_width / pixel_ratio / 4.0) * 4,
        minimum_width, maximum_width);
    const QSize size(width_value, width_value * 3 / 4);
    setGeometry(QRect(QPoint(screen.center().x() - size.width() / 2,
                            screen.center().y() - size.height() / 2),
                      size));
    if (adjustment_controls_)
        adjustment_controls_->move(
            (width() - adjustment_controls_->width()) / 2,
            height() - adjustment_controls_->height() - 12);
}

QScreen* GhostReticleWindow::target_screen() const {
    if (!target_monitor_name_.isEmpty()) {
        for (QScreen* screen : QGuiApplication::screens()) {
            if (normalized_monitor_name(screen->name()).compare(
                    normalized_monitor_name(target_monitor_name_),
                    Qt::CaseInsensitive) == 0)
                return screen;
        }
    }
    return QGuiApplication::primaryScreen();
}

void GhostReticleWindow::apply_input_mode() {
    const bool transparent = !adjusting_;
    setAttribute(Qt::WA_TransparentForMouseEvents, transparent);
    auto flags = windowFlags();
    if (transparent) {
        flags |= Qt::WindowTransparentForInput;
        flags |= Qt::WindowDoesNotAcceptFocus;
    } else {
        flags &= ~Qt::WindowTransparentForInput;
        flags &= ~Qt::WindowDoesNotAcceptFocus;
    }
    const bool visible = isVisible();
    const QRect previous = geometry();
    setWindowFlags(flags);
    setGeometry(previous);

    const auto handle = reinterpret_cast<HWND>(winId());
    auto style = GetWindowLongPtrW(handle, GWL_EXSTYLE);
    style |= WS_EX_LAYERED | WS_EX_TOOLWINDOW;
    style &= ~static_cast<LONG_PTR>(WS_EX_APPWINDOW);
    if (transparent)
        style |= WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
    else
        style &= ~(static_cast<LONG_PTR>(WS_EX_TRANSPARENT) |
                   static_cast<LONG_PTR>(WS_EX_NOACTIVATE));
    SetWindowLongPtrW(handle, GWL_EXSTYLE, style);
    SetWindowPos(handle, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    if (visible) show();
}

void GhostReticleWindow::paintEvent(QPaintEvent* event) {
    (void)event;
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const double scale = width() / wardogs::ghost_base_width;

    if ((overlay_enabled_ || adjusting_) && solution_) {
        draw_bearing(painter, scale, solution_->bearing_deg);
        draw_mil(painter, scale);
    } else if ((overlay_enabled_ || adjusting_) && mortar_bearing_ &&
               mortar_mil_) {
        draw_bearing(painter, scale, *mortar_bearing_);
        draw_mortar_mil(painter, scale);
    }

    if (adjusting_) {
        painter.setPen(QPen(QColor(QStringLiteral("#63d8c5")),
                            std::max(2.0, 2.5 * scale)));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(QRectF(rect()).adjusted(1.0, 1.0, -1.0, -1.0));
        painter.setPen(QColor(QStringLiteral("#e8eef7")));
        QFont font(QStringLiteral("Segoe UI"));
        font.setPixelSize(std::max(12, qRound(14 * scale)));
        painter.setFont(font);
        painter.drawText(QRectF(12 * scale, 10 * scale, 690 * scale,
                                28 * scale),
                         wardogs::i18n::text(QStringLiteral("Тяните края · Формат 4:3 · Esc — сохранить")));
    }
}

void GhostReticleWindow::draw_bearing(QPainter& painter, double scale,
                                      double bearing_degrees) const {
    const QColor color = ghost_color(preferences_.opacity_percent);
    painter.setPen(QPen(color, std::max(2.0, 2.6 * scale),
                        Qt::SolidLine, Qt::SquareCap));
    const double baseline = 88.0 * scale;
    painter.drawLine(QPointF(wardogs::ghost_bearing_left * scale, baseline),
                     QPointF(wardogs::ghost_bearing_right * scale, baseline));
    QFont font(QStringLiteral("Bahnschrift"));
    font.setPixelSize(std::max(12, qRound(17 * scale)));
    font.setWeight(QFont::Bold);
    painter.setFont(font);
    for (const auto& tick :
         wardogs::ghost_bearing_ticks(
             wardogs::ghost_game_bearing(
                 bearing_degrees,
                 preferences_.bearing_compensation_deg))) {
        const double x = tick.position * scale;
        const double length = (tick.major ? 25.0 : 14.0) * scale;
        painter.drawLine(QPointF(x, baseline), QPointF(x, baseline - length));
        if (tick.major) {
            painter.drawText(QRectF(x - 30 * scale, baseline + 4 * scale,
                                    60 * scale, 26 * scale),
                             Qt::AlignHCenter | Qt::AlignTop,
                             QString::number(tick.value));
        }
    }
}

void GhostReticleWindow::draw_mil(QPainter& painter, double scale) const {
    const QColor color = ghost_color(preferences_.opacity_percent);
    painter.save();
    painter.setClipRect(QRectF(0, wardogs::ghost_mil_top * scale,
                               width(),
                               (wardogs::ghost_mil_bottom -
                                wardogs::ghost_mil_top) * scale));
    painter.setPen(QPen(color, std::max(2.0, 2.7 * scale),
                        Qt::SolidLine, Qt::SquareCap));
    QFont font(QStringLiteral("Bahnschrift"));
    font.setPixelSize(std::max(12, qRound(17 * scale)));
    font.setWeight(QFont::Bold);
    painter.setFont(font);
    const double rail_x = 790.0 * scale;
    for (const auto& tick : wardogs::ghost_mil_ticks(solution_->mil)) {
        const double y = tick.position * scale;
        painter.drawLine(QPointF(rail_x, y),
                         QPointF(rail_x + 26 * scale, y));
        painter.drawLine(QPointF(rail_x, y - 7 * scale),
                         QPointF(rail_x, y + 7 * scale));
        painter.drawText(QRectF(rail_x - 126 * scale, y - 14 * scale,
                                116 * scale, 28 * scale),
                         Qt::AlignRight | Qt::AlignVCenter,
                         QStringLiteral("%1 MIL").arg(tick.value));
    }
    painter.restore();
}

void GhostReticleWindow::draw_mortar_mil(QPainter& painter,
                                          double scale) const {
    const QColor color = ghost_color(preferences_.opacity_percent);
    painter.save();
    painter.setClipRect(QRectF(0, wardogs::ghost_mil_top * scale,
                               width(),
                               (wardogs::ghost_mil_bottom -
                                wardogs::ghost_mil_top) * scale));
    painter.setPen(QPen(color, std::max(2.0, 2.7 * scale),
                        Qt::SolidLine, Qt::SquareCap));
    QFont font(QStringLiteral("Bahnschrift"));
    font.setPixelSize(std::max(12, qRound(17 * scale)));
    font.setWeight(QFont::Bold);
    painter.setFont(font);
    const double rail_x = 790.0 * scale;
    for (const auto& tick :
         wardogs::ghost_mortar_mil_ticks(*mortar_mil_)) {
        const double y = tick.position * scale;
        painter.drawLine(QPointF(rail_x, y),
                         QPointF(rail_x + 26 * scale, y));
        painter.drawLine(QPointF(rail_x, y - 7 * scale),
                         QPointF(rail_x, y + 7 * scale));
        painter.drawText(QRectF(rail_x - 86 * scale, y - 14 * scale,
                                76 * scale, 28 * scale),
                         Qt::AlignRight | Qt::AlignVCenter,
                         QString::number(tick.value));
    }
    painter.restore();
}

void GhostReticleWindow::keyPressEvent(QKeyEvent* event) {
    if (adjusting_ && event->key() == Qt::Key_Escape) {
        finish_adjustment(true);
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void GhostReticleWindow::mousePressEvent(QMouseEvent* event) {
    if (adjusting_ && event->button() == Qt::LeftButton) {
        const QPoint point = event->position().toPoint();
        if (resize_cursor_at(point) != Qt::ArrowCursor) {
            resizing_ = true;
            resize_center_ = frameGeometry().center();
            event->accept();
            return;
        }
    }
    QWidget::mousePressEvent(event);
}

void GhostReticleWindow::mouseMoveEvent(QMouseEvent* event) {
    if (adjusting_ && resizing_ && (event->buttons() & Qt::LeftButton)) {
        const QPoint offset = event->globalPosition().toPoint() - resize_center_;
        const int half_width = std::max(
            std::abs(offset.x()), qRound(std::abs(offset.y()) * 4.0 / 3.0));
        const auto* target = target_screen();
        const qreal pixel_ratio = target ? target->devicePixelRatio() : devicePixelRatioF();
        center_on_screen(qRound(half_width * 2 * pixel_ratio));
        event->accept();
        return;
    }
    if (adjusting_)
        setCursor(resize_cursor_at(event->position().toPoint()));
    QWidget::mouseMoveEvent(event);
}

void GhostReticleWindow::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && resizing_) {
        resizing_ = false;
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void GhostReticleWindow::leaveEvent(QEvent* event) {
    if (!resizing_) setCursor(Qt::ArrowCursor);
    QWidget::leaveEvent(event);
}
