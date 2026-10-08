#include "direction_plot.hpp"
#include "localization.hpp"

#include <QPainter>
#include <QEvent>
#include <QPainterPath>
#include <algorithm>
#include <cmath>

DirectionPlot::DirectionPlot(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("directionPlot"));
    setMinimumSize(210, 230);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    setAccessibleName(wardogs::i18n::text(QStringLiteral("Схема направления на цель, север сверху")));
    setToolTip(wardogs::i18n::text(QStringLiteral("Схема направления. Не отображает рельеф и препятствия.")));
    wardogs::i18n::watch(this);
}

void DirectionPlot::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) update();
}

void DirectionPlot::set_shot(std::optional<wardogs::Shot> shot) {
    shot_ = std::move(shot);
    update();
}

void DirectionPlot::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    // Keep the compass labels outside the rings and away from the header/footer.
    constexpr double plot_top = 56.0;
    const double plot_bottom = height() - 50.0;
    const QPointF center(width() / 2.0, (plot_top + plot_bottom) / 2.0);
    const double radius = std::max(32.0,
        std::min(width() / 2.0 - 30.0, (plot_bottom - plot_top) / 2.0));
    p.setBrush(QColor(QStringLiteral("#0b131e")));
    p.setPen(QPen(QColor(QStringLiteral("#253248")), 1));
    p.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 13, 13);
    for (int i = 1; i <= 3; ++i) {
        const double r = radius * i / 3.0;
        p.setPen(QPen(QColor(QStringLiteral("#253248")), 1, i == 3 ? Qt::SolidLine : Qt::DotLine));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(center, r, r);
    }
    p.setPen(QPen(QColor(QStringLiteral("#253248")), 1, Qt::DotLine));
    p.drawLine(center - QPointF(radius, 0), center + QPointF(radius, 0));
    p.drawLine(center - QPointF(0, radius), center + QPointF(0, radius));
    auto font = p.font();
    font.setPixelSize(11);
    font.setBold(true);
    p.setFont(font);
    p.setPen(QColor(QStringLiteral("#8293ab")));
    p.drawText(QRectF(center.x() - 15, center.y() - radius - 21, 30, 18), Qt::AlignCenter, wardogs::i18n::text(QStringLiteral("Север")).left(1));
    p.drawText(QRectF(center.x() - 15, center.y() + radius + 3, 30, 18), Qt::AlignCenter, wardogs::i18n::text(QStringLiteral("Юг")).left(1));
    p.drawText(QRectF(center.x() - radius - 22, center.y() - 9, 18, 18), Qt::AlignCenter, wardogs::i18n::text(QStringLiteral("Запад")).left(1));
    p.drawText(QRectF(center.x() + radius + 4, center.y() - 9, 18, 18), Qt::AlignCenter, wardogs::i18n::text(QStringLiteral("Восток")).left(1));
    if (shot_ && shot_->distance > 0.0) {
        const QPointF direction(shot_->dx / shot_->distance, -shot_->dy / shot_->distance);
        const QPointF end = center + direction * (radius * .81);
        const QPointF perpendicular(-direction.y(), direction.x());
        p.setPen(QPen(QColor(QStringLiteral("#63d8c5")), 2.5, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(center, end - direction * 9);
        p.setBrush(QColor(QStringLiteral("#63d8c5")));
        QPainterPath head;
        head.moveTo(end);
        head.lineTo(end - direction * 12 + perpendicular * 5);
        head.lineTo(end - direction * 12 - perpendicular * 5);
        head.closeSubpath();
        p.drawPath(head);
        p.setPen(QColor(QStringLiteral("#f0b45d")));
        p.drawText(QRectF(0, 12, width(), 20), Qt::AlignCenter,
                   QStringLiteral("%1 · %2")
                       .arg(QString::fromStdWString(wardogs::format_bearing(shot_->angle)),
                            wardogs::i18n::text(QString::fromStdWString(wardogs::format_distance_meters(shot_->distance)))));
    } else {
        p.setPen(QColor(QStringLiteral("#8293ab")));
        p.drawText(QRectF(10, 12, width() - 20, 20), Qt::AlignCenter, wardogs::i18n::text(QStringLiteral("СХЕМА НАПРАВЛЕНИЯ")));
    }
    p.setPen(QPen(QColor(QStringLiteral("#f0b45d")), 2));
    p.setBrush(QColor(QStringLiteral("#111b28")));
    p.drawEllipse(center, 5, 5);
    p.setPen(QColor(QStringLiteral("#8293ab")));
    font.setPixelSize(10);
    font.setBold(false);
    p.setFont(font);
    p.drawText(QRectF(8, height() - 20, width() - 16, 15), Qt::AlignCenter, wardogs::i18n::text(QStringLiteral("ОРУДИЕ В ЦЕНТРЕ · СЕВЕР СВЕРХУ")));
}
