#include "vehicle_solution_widget.hpp"
#include "localization.hpp"

#include "wardogs/core.hpp"
#include "wardogs/presentation.hpp"

#include <QHBoxLayout>
#include <QEvent>
#include <QFontMetrics>
#include <QLabel>
#include <QStyle>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {

void update_style(QWidget* widget, const QString& style) {
    if (widget->styleSheet() != style) widget->setStyleSheet(style);
}

// Reserve the complete rendered text for both the short caption and its value.
// Fixed column proportions and QLabel's word-wrap minimum hint can leave less
// room than the current font needs after polishing or changing its scale.
class MetricTextLabel final : public QLabel {
public:
    using QLabel::QLabel;

    void refresh_minimum() {
        const QSize previous = minimumSize();
        const QFontMetrics metrics(font());
        const auto padding = contentsMargins();
        const int text_width = std::max(metrics.horizontalAdvance(text()), metrics.boundingRect(text()).width());
        const int width = text_width + padding.left() + padding.right() + 2 * margin() + 4;
        // Clear the previous height before asking Qt for its wrapped text
        // height, so switching to a smaller font can shrink the card again.
        setMinimumSize(width, 0);
        setMinimumHeight(std::max(metrics.height() + padding.top() + padding.bottom() + 2 * margin() + 2,
                                  wordWrap() ? heightForWidth(width) : 0));
        if (minimumSize() == previous) return;
        // A metric lives in a QWidget with its own layout. Propagate that
        // layout's new minimum before the card's outer layout reads its cached
        // QWidgetItem, including while the card is hidden between displays.
        if (auto* column = parentWidget(); column && column->layout()) {
            column->layout()->invalidate();
            column->layout()->activate();
            column->updateGeometry();
            if (auto* card = column->parentWidget(); card && card->layout()) {
                card->layout()->invalidate();
                card->layout()->activate();
            }
        }
    }

protected:
    bool event(QEvent* event) override {
        const bool handled = QLabel::event(event);
        if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange ||
            event->type() == QEvent::Polish || event->type() == QEvent::LanguageChange)
            refresh_minimum();
        return handled;
    }
};

} // namespace

VehicleSolutionWidget::VehicleSolutionWidget(wardogs::Arc arc, bool compact,
                                             QWidget* parent)
    : QFrame(parent), arc_(arc), compact_(compact) {
    setObjectName(QStringLiteral("vehicleSolutionCard"));
    setProperty("unavailable", false);
    setProperty("compact", compact_);
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(compact_ ? 8 : 12, compact_ ? 5 : 9,
                               compact_ ? 8 : 12, compact_ ? 6 : 10);
    layout->setSpacing(compact_ ? 6 : 14);
    trajectory_ = new QLabel;
    trajectory_->setObjectName(QStringLiteral("solutionArc"));
    trajectory_->setTextFormat(Qt::PlainText);
    trajectory_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    trajectory_->setWordWrap(!compact_);
    trajectory_->setMinimumWidth(compact_ ? 62 : 88);
    layout->addWidget(trajectory_);
    if (!compact_) layout->addStretch();
    distance_ = add_metric(layout, wardogs::i18n::text(QStringLiteral("До цели")),
                           QStringLiteral("solutionDistance"), compact_ ? 82 : 105,
                           &distance_caption_);
    bearing_ = add_metric(layout, wardogs::i18n::text(QStringLiteral("Азимут")),
                          QStringLiteral("solutionBearing"), compact_ ? 106 : 140);
    mil_ = add_metric(layout, wardogs::i18n::text(QStringLiteral("Установить")),
                      QStringLiteral("solutionMil"), compact_ ? 100 : 130);
    table_distance_ = add_metric(layout, wardogs::i18n::text(QStringLiteral("По таблице ≈")),
                                QStringLiteral("solutionTableDistance"), compact_ ? 76 : 95);
    table_distance_->setProperty("secondaryMetric", true);
    table_distance_->setAccessibleName(wardogs::i18n::text(QStringLiteral("Оценка табличной дальности SPH-2")));
    if (!compact_) table_distance_->setStyleSheet(QStringLiteral("color:#94a3b8;font-size:17px;"));
    distance_->setAccessibleName(wardogs::i18n::text(QStringLiteral("Дальность до цели SPH-2")));
    distance_->setToolTip(wardogs::i18n::text(QStringLiteral("Горизонтальное расстояние до цели по карте.")));
    mil_->setAccessibleName(wardogs::i18n::text(QStringLiteral("Наводка SPH-2 по игровой шкале MIL")));
    update_trajectory_label();
    if (compact_) set_compact_scale(1.0);
    wardogs::i18n::watch(this);
}

void VehicleSolutionWidget::changeEvent(QEvent* event) {
    QFrame::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        update_trajectory_label();
        refresh_metric_minimums();
    }
}

QLabel* VehicleSolutionWidget::add_metric(QHBoxLayout* layout,
                                          const QString& caption,
                                          const QString& object_name, int width,
                                          QLabel** caption_label) {
    auto* block = new QWidget;
    // An explicit QWidget minimum would replace, rather than bound, the
    // minimum from its child layout. Let the current caption/value determine
    // that minimum; the outer stretch factor still controls column proportions.
    block->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto* column = new QVBoxLayout(block);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);
    {
        auto* label = new MetricTextLabel(caption);
        label->setObjectName(QStringLiteral("solutionMetricCaption"));
        label->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
        label->setWordWrap(true);
        if (compact_) {
            label->setAlignment(Qt::AlignCenter);
            label->setStyleSheet(QStringLiteral("color:#b5c5dc;font-size:10px;font-weight:600;"));
        }
        label->refresh_minimum();
        column->addWidget(label);
        if (caption_label) *caption_label = label;
    }
    auto* value = new MetricTextLabel(QStringLiteral("—"));
    value->setObjectName(object_name);
    value->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
    value->refresh_minimum();
    if (compact_) value->setAlignment(Qt::AlignCenter);
    column->addWidget(value);
    layout->addWidget(block, width);
    return value;
}

void VehicleSolutionWidget::set_waiting() {
    solution_ready_ = false;
    selected_ = false;
    distance_is_target_ = true;
    if (distance_caption_) distance_caption_->setText(wardogs::i18n::text(QStringLiteral("До цели")));
    distance_->setAccessibleName(wardogs::i18n::text(QStringLiteral("Дальность до цели SPH-2: ожидается цель")));
    distance_->setToolTip(wardogs::i18n::text(QStringLiteral("Выберите цель, чтобы получить расстояние по карте и наводку.")));
    distance_->setText(QStringLiteral("—"));
    bearing_->setText(compact_ ? QStringLiteral("—") : wardogs::i18n::text(QStringLiteral("Ждём цель")));
    mil_->setText(QStringLiteral("—"));
    table_distance_->setText(QStringLiteral("—"));
    table_distance_->setToolTip({});
    set_unavailable_state(false);
}

void VehicleSolutionWidget::set_solution(
    const wardogs::CorrectedSolution& solution, std::optional<double> target_distance_m) {
    if (target_distance_m && (!std::isfinite(*target_distance_m) || *target_distance_m < 0.0))
        throw std::invalid_argument("Дальность до цели должна быть конечной и неотрицательной");
    if (solution.arc != arc_ || !std::isfinite(solution.bearing_deg))
        throw std::invalid_argument("Некорректная траектория или азимут наводки");
    const auto command = wardogs::displayed_firing_command(solution);
    solution_ready_ = true;
    distance_is_target_ = target_distance_m.has_value();
    const auto table_distance = QString::number(std::round(command.table_distance_m));
    table_distance_->setText(wardogs::i18n::text(QStringLiteral("≈ %1 м")).arg(table_distance));
    table_distance_->setToolTip(wardogs::i18n::text(QStringLiteral("Табличная дальность для наводки ≈ %1 м — оценка модели. "
                                           "Она может отличаться от расстояния до цели и игровой шкалы. "
                                           "Выставляйте MIL выбранной траектории."))
            .arg(table_distance));
    if (distance_caption_) distance_caption_->setText(wardogs::i18n::text(QStringLiteral("До цели")));
    if (target_distance_m) {
        distance_->setAccessibleName(wardogs::i18n::text(QStringLiteral("Горизонтальная дальность до цели SPH-2")));
        const auto target_distance = QString::number(std::round(*target_distance_m));
        distance_->setText(target_distance + wardogs::i18n::text(QStringLiteral(" м")));
        distance_->setToolTip(wardogs::i18n::text(QStringLiteral("До цели %1 м; табличная дальность для наводки ≈ %2 м. "
                                           "Табличное значение — оценка модели и может отличаться от игровой шкалы. "
                                           "Выставляйте MIL выбранной траектории."))
            .arg(target_distance, table_distance));
    } else {
        distance_->setAccessibleName(wardogs::i18n::text(QStringLiteral("Дальность до цели SPH-2: ожидается цель")));
        distance_->setText(QStringLiteral("—"));
        distance_->setToolTip(wardogs::i18n::text(QStringLiteral("Горизонтальная дальность до цели не передана. Табличная оценка показана отдельно.")));
    }
    bearing_->setText(QString::fromStdWString(wardogs::format_bearing(command.bearing_deg)));
    mil_->setText(QStringLiteral("%1 MIL").arg(command.mil));
    mil_->setToolTip(wardogs::i18n::text(QStringLiteral("Игровая наводка SPH-2 в MIL из таблицы орудия с поправками. MIL здесь не означает расстояние в милях.")));
    set_unavailable_state(false);
}

void VehicleSolutionWidget::set_unavailable(const QString& distance,
                                             const QString& bearing) {
    solution_ready_ = false;
    distance_is_target_ = true;
    if (distance_caption_) distance_caption_->setText(wardogs::i18n::text(QStringLiteral("До цели")));
    distance_->setAccessibleName(wardogs::i18n::text(QStringLiteral("Горизонтальная дальность до цели SPH-2")));
    distance_->setToolTip(wardogs::i18n::text(QStringLiteral("Расстояние до цели по карте. Наводка для этой траектории недоступна.")));
    distance_->setText(wardogs::i18n::text(distance));
    bearing_->setText(wardogs::i18n::text(bearing));
    mil_->setText(compact_ ? QStringLiteral("—") : wardogs::i18n::text(QStringLiteral("Нет решения")));
    mil_->setToolTip(wardogs::i18n::text(QStringLiteral("Дальность или угол выходят за пределы таблицы орудия")));
    table_distance_->setText(QStringLiteral("—"));
    table_distance_->setToolTip({});
    set_unavailable_state(true);
}

void VehicleSolutionWidget::set_height_unavailable() {
    solution_ready_ = false;
    distance_is_target_ = true;
    if (distance_caption_) distance_caption_->setText(wardogs::i18n::text(QStringLiteral("До цели")));
    distance_->setAccessibleName(wardogs::i18n::text(QStringLiteral("Дальность до цели SPH-2: нет данных высоты")));
    distance_->setToolTip(wardogs::i18n::text(QStringLiteral("Не удалось получить данные высоты. Наводка недоступна.")));
    distance_->setText(QStringLiteral("—"));
    bearing_->setText(compact_ ? QStringLiteral("—") : wardogs::i18n::text(QStringLiteral("Нет высот")));
    mil_->setText(QStringLiteral("—"));
    table_distance_->setText(QStringLiteral("—"));
    table_distance_->setToolTip({});
    set_unavailable_state(true);
}

void VehicleSolutionWidget::copy_from(const VehicleSolutionWidget& source) {
    solution_ready_ = source.solution_ready_;
    distance_is_target_ = source.distance_is_target_;
    selected_ = source.selected_;
    distance_->setText(source.distance_text());
    distance_->setToolTip(source.distance_->toolTip());
    distance_->setAccessibleName(source.distance_->accessibleName());
    if (distance_caption_)
        distance_caption_->setText(wardogs::i18n::text(QStringLiteral("До цели")));
    table_distance_->setText(source.table_distance_text());
    table_distance_->setToolTip(source.table_distance_->toolTip());
    table_distance_->setAccessibleName(source.table_distance_->accessibleName());
    const auto source_bearing = source.bearing_text();
    bearing_->setText(source_bearing == wardogs::i18n::text(QStringLiteral("Нет высот")) ||
                              source_bearing == wardogs::i18n::text(QStringLiteral("Ждём цель"))
                          ? QStringLiteral("—")
                          : source_bearing);
    mil_->setText(source.mil_text() == wardogs::i18n::text(QStringLiteral("Нет решения"))
                      ? QStringLiteral("—")
                      : source.mil_text());
    mil_->setToolTip(source.mil_->toolTip());
    set_unavailable_state(source.unavailable());
}

void VehicleSolutionWidget::set_compact_scale(double scale) {
    if (!compact_) return;
    compact_scale_ = scale;
    const int label_size = std::clamp(qRound(10 * scale), 8, 12);
    update_style(trajectory_, QStringLiteral("color:%1;font-family:'Segoe UI';font-size:%2px;font-weight:%3;")
        .arg(selected_ ? QStringLiteral("#63d8c5") : QStringLiteral("#94a3b8"))
        .arg(label_size).arg(selected_ ? 700 : 400));
    const auto name = arc_ == wardogs::Arc::low
        ? wardogs::i18n::text(QStringLiteral("Настильная")) : wardogs::i18n::text(QStringLiteral("Навесная"));
    const int name_width = QFontMetrics(trajectory_->font()).horizontalAdvance(name);
    trajectory_->setFixedWidth(std::max(std::clamp(qRound(82 * scale), 62, 99), name_width));
    const int size = std::max(16, qRound(20 * scale));
    const int distance_size = distance_is_target_ ? size : std::max(16, size - 1);
    update_style(distance_, QStringLiteral("color:#dce6f4;font-family:'Segoe UI';font-size:%1px;").arg(distance_size));
    update_style(table_distance_, QStringLiteral("color:#94a3b8;font-family:'Segoe UI';font-size:%1px;").arg(std::max(13, size - 4)));
    for (auto* caption : findChildren<QLabel*>(QStringLiteral("solutionMetricCaption")))
        update_style(caption, QStringLiteral("color:#b5c5dc;font-size:%1px;font-weight:600;").arg(std::clamp(qRound(10 * scale), 10, 12)));
    update_style(bearing_, QStringLiteral("color:#e8eef7;font-family:'Segoe UI';font-size:%1px;font-weight:600;").arg(size));
    const auto color = unavailable() ? QStringLiteral("#ff9d9d")
                                     : QStringLiteral("#e8eef7");
    update_style(mil_,
        QStringLiteral("color:%1;font-family:'Segoe UI';font-size:%2px;font-weight:700;").arg(color).arg(size));
    refresh_metric_minimums();
}

void VehicleSolutionWidget::refresh_metric_minimums() {
    for (auto* value : {distance_, bearing_, mil_, table_distance_}) {
        if (!value) continue;
        value->ensurePolished();
        static_cast<MetricTextLabel*>(value)->refresh_minimum();
    }
    for (auto* caption : findChildren<QLabel*>(QStringLiteral("solutionMetricCaption"))) {
        caption->ensurePolished();
        static_cast<MetricTextLabel*>(caption)->refresh_minimum();
    }
    for (auto* column : findChildren<QWidget*>(QString{}, Qt::FindDirectChildrenOnly)) {
        if (!column->layout()) continue;
        column->layout()->invalidate();
        column->layout()->activate();
        column->updateGeometry();
    }
    layout()->invalidate();
    layout()->activate();
    updateGeometry();
}

QString VehicleSolutionWidget::distance_text() const { return distance_->text(); }
QString VehicleSolutionWidget::table_distance_text() const { return table_distance_->text(); }
QString VehicleSolutionWidget::bearing_text() const { return bearing_->text(); }
QString VehicleSolutionWidget::mil_text() const { return mil_->text(); }
bool VehicleSolutionWidget::unavailable() const {
    return property("unavailable").toBool();
}

bool VehicleSolutionWidget::selected() const { return selected_; }

void VehicleSolutionWidget::set_selected(bool selected) {
    selected_ = selected && solution_ready_ && !unavailable();
    update_trajectory_label();
}

void VehicleSolutionWidget::update_trajectory_label() {
    const auto name = arc_ == wardogs::Arc::low
        ? wardogs::i18n::text(QStringLiteral("Настильная")) : wardogs::i18n::text(QStringLiteral("Навесная"));
    const auto state = !solution_ready_ ? unavailable() ? wardogs::i18n::text(QStringLiteral("недоступна"))
                                                       : wardogs::i18n::text(QStringLiteral("ожидает цель"))
        : selected_ ? wardogs::i18n::text(QStringLiteral("выбрана")) : wardogs::i18n::text(QStringLiteral("доступна"));
    trajectory_->setText((selected_ ? compact_ ? QStringLiteral("✓\n") : QStringLiteral("✓ ")
                                  : QString{}) + name);
    setProperty("selected", selected_);
    const auto description = wardogs::i18n::text(QStringLiteral("%1 траектория: %2"))
        .arg(name, state);
    setAccessibleName(description);
    setToolTip(distance_is_target_
        ? wardogs::i18n::text(QStringLiteral("%1. Дальность до цели, азимут и игровая наводка в MIL."))
              .arg(description)
        : wardogs::i18n::text(QStringLiteral("%1. Оценка табличной дальности, азимут и игровая наводка в MIL."))
              .arg(description));
    trajectory_->setToolTip(toolTip());
    trajectory_->setAccessibleName(accessibleName());
    if (compact_) set_compact_scale(compact_scale_);
    else trajectory_->setStyleSheet(selected_
        ? QStringLiteral("color:#63d8c5;font-weight:700;")
        : QStringLiteral("color:#94a3b8;"));
}

void VehicleSolutionWidget::set_unavailable_state(bool unavailable) {
    if (unavailable || !solution_ready_) selected_ = false;
    setProperty("unavailable", unavailable);
    mil_->setProperty("unavailable", unavailable);
    if (compact_) {
        set_compact_scale(compact_scale_);
    } else {
        mil_->setStyleSheet(unavailable
            ? QStringLiteral("color:#ff9d9d;font-size:17px;")
            : QStringLiteral("color:#e8eef7;font-size:25px;font-weight:700;"));
    }
    update_trajectory_label();
    refresh_metric_minimums();
    update();
}
