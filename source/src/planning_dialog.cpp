#include "planning_dialog.hpp"
#include "localization.hpp"
#include "window_title_bar.hpp"
#include "wardogs/presentation.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLockFile>
#include <QPainter>
#include <QPalette>
#include <QPushButton>
#include <QSaveFile>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
QString ui_text(const char* value) { return wardogs::i18n::text(QString::fromUtf8(value)); }
QString error_text(const std::exception& e) {
    return wardogs::i18n::text(QString::fromUtf8(e.what()));
}
QString path_text(const std::filesystem::path& p) { return QString::fromStdWString(p.wstring()); }
QString point_text(wardogs::Point p) {
    return QStringLiteral("X %1 · Y %2").arg(p.x, 0, 'g', 17).arg(p.y, 0, 'g', 17);
}
wardogs::FireMissionWeapon mission_weapon(wardogs::AnalysisWeapon weapon) {
    return weapon == wardogs::AnalysisWeapon::l81 ? wardogs::FireMissionWeapon::l81
                                                 : wardogs::FireMissionWeapon::sph2;
}
QLabel* note(const QString& text, QWidget* parent = nullptr) {
    auto* result = new QLabel(text, parent);
    result->setWordWrap(true);
    result->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return result;
}
QVBoxLayout* page(QTabWidget* tabs, const QString& title) {
    auto* scroll = new QScrollArea;
    scroll->setObjectName(QStringLiteral("planningPageScroll"));
    scroll->viewport()->setObjectName(QStringLiteral("planningPageViewport"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget;
    content->setObjectName(QStringLiteral("planningPageContent"));
    content->setAutoFillBackground(true);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(6, 12, 6, 6);
    layout->setSpacing(12);
    scroll->setWidget(content);
    tabs->addTab(scroll, title);
    return layout;
}
QDoubleSpinBox* number(const QString& name, double minimum, double maximum, int decimals) {
    auto* result = new QDoubleSpinBox;
    result->setObjectName(name);
    result->setRange(minimum, maximum);
    result->setDecimals(decimals);
    result->setKeyboardTracking(false);
    return result;
}

struct ProfilePoint {
    double distance{};
    std::optional<double> ground;
    std::optional<double> shell;
};

class TerrainProfileWidget final : public QWidget {
public:
    using QWidget::QWidget;
    void set_samples(std::vector<ProfilePoint> samples) {
        samples_ = std::move(samples);
        update();
    }
    QSize minimumSizeHint() const override { return QSize(300, 210); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillRect(rect(), QColor("#101820"));
        const QRectF plot = QRectF(rect()).adjusted(62, 18, -18, -42);
        painter.setPen(QColor("#8c9ba5"));
        painter.drawRect(plot);
        if (samples_.empty() || samples_.back().distance <= 0) {
            painter.drawText(plot, Qt::AlignCenter, ui_text("Задайте орудие и цель"));
            return;
        }
        double low = 0, high = 1;
        bool any = false;
        for (const auto& value : samples_) {
            for (const auto h : {value.ground, value.shell}) {
                if (h) { low = std::min(low, *h); high = std::max(high, *h); any = true; }
            }
        }
        const double padding = std::max(5.0, (high - low) * .08);
        low -= padding; high += padding;
        const double range = samples_.back().distance;
        const auto project = [&](double x, double y) {
            return QPointF(plot.left() + x / range * plot.width(),
                           plot.bottom() - (y - low) / (high - low) * plot.height());
        };
        painter.setPen(QColor("#a9b5bd"));
        painter.drawText(QRectF(0, plot.top() - 8, 58, 22), Qt::AlignRight,
                         QString::number(high, 'f', 0));
        painter.drawText(QRectF(0, plot.bottom() - 14, 58, 22), Qt::AlignRight,
                         QString::number(low, 'f', 0));
        painter.drawText(QRectF(plot.left(), plot.bottom() + 9, plot.width(), 24),
                         Qt::AlignCenter, ui_text("Дальность, м · высота относительно орудия, м"));
        painter.drawText(QRectF(plot.left(), plot.bottom() + 6, 65, 20), Qt::AlignLeft, "0");
        painter.drawText(QRectF(plot.right() - 65, plot.bottom() + 6, 65, 20),
                         Qt::AlignRight, QString::number(range, 'f', 0));
        // Unknown terrain is a gap, never a flat segment or an interpolated
        // obstacle-free line. This widget also displays terrain without a model.
        for (bool shell : {false, true}) {
            std::optional<QPointF> previous;
            painter.setPen(QPen(shell ? QColor("#efb76e") : QColor("#71bea4"), 2));
            for (const auto& value : samples_) {
                const auto height = shell ? value.shell : value.ground;
                if (!height) { previous.reset(); continue; }
                const auto current = project(value.distance, *height);
                if (previous) painter.drawLine(*previous, current);
                previous = current;
            }
        }
        if (!any) {
            painter.setPen(QColor("#c1cbd1"));
            painter.drawText(plot, Qt::AlignCenter, ui_text("Данные рельефа недоступны"));
        }
    }
private:
    std::vector<ProfilePoint> samples_;
};

struct Observation {
    QString id;
    wardogs::AnalysisWeapon weapon{};
    wardogs::Arc arc{};
    double distance{}, height{}, seconds{}, uncertainty{};
    QString version, source;
};

constexpr qsizetype maximum_observations_bytes = 256 * 1024;
constexpr qsizetype maximum_observations = 256;

std::vector<Observation> read_observations(const QString& path) {
    if (!QFile::exists(path)) return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Не удалось прочитать измерения времени полёта.");
    if (file.size() > maximum_observations_bytes)
        throw std::runtime_error("Файл измерений времени полёта слишком большой.");
    QJsonParseError error{};
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        throw std::runtime_error("Повреждён файл измерений времени полёта.");
    const auto root = document.object();
    if (root.value("schema").toInt(-1) != 1 || root.size() != 2 ||
        !root.value("observations").isArray())
        throw std::runtime_error("Неизвестный формат измерений времени полёта.");
    const auto entries = root.value("observations").toArray();
    if (entries.size() > maximum_observations)
        throw std::runtime_error("Слишком много измерений времени полёта.");
    std::vector<Observation> result;
    for (const auto& entry : entries) {
        if (!entry.isObject())
            throw std::runtime_error("Повреждена запись времени полёта.");
        const auto o = entry.toObject();
        const auto weapon = o.value("weapon").toString();
        const auto arc = o.value("arc").toString();
        Observation value;
        value.id = o.value("id").toString();
        value.weapon = weapon == "l81" ? wardogs::AnalysisWeapon::l81 : wardogs::AnalysisWeapon::sph2;
        value.arc = arc == "low" ? wardogs::Arc::low : wardogs::Arc::high;
        value.distance = o.value("distance_m").toDouble(std::numeric_limits<double>::quiet_NaN());
        value.height = o.value("height_delta_m").toDouble(std::numeric_limits<double>::quiet_NaN());
        value.seconds = o.value("seconds").toDouble(std::numeric_limits<double>::quiet_NaN());
        value.uncertainty = o.value("uncertainty_s").toDouble(std::numeric_limits<double>::quiet_NaN());
        value.version = o.value("game_version").toString();
        value.source = o.value("source").toString();
        if (o.size() != 9 || value.id.isEmpty() || value.id.size() > 64 ||
            (weapon != "l81" && weapon != "sph2") || (arc != "low" && arc != "high") ||
            !std::isfinite(value.distance) || value.distance <= 0 || value.distance > 100000 ||
            !std::isfinite(value.height) || std::abs(value.height) > 10000 ||
            !std::isfinite(value.seconds) || value.seconds <= 0 || value.seconds > 600 ||
            !std::isfinite(value.uncertainty) || value.uncertainty < 0 || value.uncertainty > value.seconds ||
            value.source.isEmpty() || value.source.size() > 200 ||
            value.version.isEmpty() || value.version.size() > 80 ||
            std::any_of(result.begin(), result.end(), [&](const auto& v) { return v.id == value.id; }))
            throw std::runtime_error("Повреждена запись времени полёта.");
        result.push_back(std::move(value));
    }
    return result;
}

void write_observations(const QString& path, const std::vector<Observation>& values) {
    if (values.size() > static_cast<std::size_t>(maximum_observations))
        throw std::runtime_error("Слишком много измерений времени полёта.");
    QJsonArray entries;
    for (const auto& v : values) {
        entries.append(QJsonObject{
            {"id", v.id}, {"weapon", v.weapon == wardogs::AnalysisWeapon::l81 ? "l81" : "sph2"},
            {"arc", v.arc == wardogs::Arc::low ? "low" : "high"},
            {"distance_m", v.distance}, {"height_delta_m", v.height}, {"seconds", v.seconds},
            {"uncertainty_s", v.uncertainty}, {"game_version", v.version}, {"source", v.source}});
    }
    const auto bytes = QJsonDocument(QJsonObject{{"schema", 1}, {"observations", entries}}).toJson();
    if (bytes.size() > maximum_observations_bytes)
        throw std::runtime_error("Файл измерений времени полёта слишком большой.");
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        throw std::runtime_error("Не удалось сохранить измерения времени полёта.");
}
} // namespace

struct PlanningDialog::State {
    PlanningDialog* dialog;
    Provider provider;
    Apply apply;
    PlanningContext context;
    wardogs::FireMissionRepository missions;
    wardogs::FireMissionRepository recent_missions;
    QString directory, observations_path;
    struct ListedMission { wardogs::SavedFireMission mission; bool recent{}; };
    std::vector<ListedMission> shown_missions;
    std::vector<Observation> observations;
    std::optional<wardogs::FiringAnalysis> analysis;
    QLabel *coordinates{}, *status{}, *result{}, *clearance{}, *profile_info{};
    TerrainProfileWidget* plot{};
    QComboBox *arc{}, *step{};
    QCheckBox* assume{};
    QDoubleSpinBox *gravity{}, *speed{}, *seconds{}, *uncertainty{};
    QLineEdit *name{}, *version{}, *source{};
    QListWidget* saved{};
    QTableWidget* times{};
    QPushButton *restore{}, *save_gun{}, *save_target{}, *record{}, *rename{}, *erase{};
    std::vector<QPushButton*> shifts;
    bool loading{true};

    State(PlanningDialog* owner, Provider get, Apply set, std::filesystem::path storage)
        : dialog(owner), provider(std::move(get)), apply(std::move(set)),
          missions(storage.empty() ? wardogs::fire_missions_path() : storage / L"fire-missions.json"),
          recent_missions(storage.empty() ? wardogs::recent_fire_missions_path() : storage / L"recent-fire-missions.json") {
        directory = path_text(missions.path().parent_path());
        observations_path = directory + QStringLiteral("/flight-profiles.json");
    }
    wardogs::Arc selected_arc() const {
        return context.weapon == wardogs::AnalysisWeapon::l81 || arc->currentIndex() == 1
            ? wardogs::Arc::high : wardogs::Arc::low;
    }
    bool ready() const {
        return context.map_confirmed && context.base && context.target &&
               !context.capture_pending && !context.solution_held;
    }
    void message(const QString& text, bool error = false) {
        status->setText(text);
        status->setProperty("error", error);
    }
    template <class Action> void attempt(Action action) {
        try { action(); }
        catch (const std::exception& e) { message(error_text(e), true); }
    }
    void load_missions() {
        const auto selected = saved->currentItem() ? saved->currentItem()->data(Qt::UserRole).toString() : QString{};
        const bool selected_history = selection_recent();
        const auto values = missions.load();
        const auto recent_values = recent_missions.load();
        saved->clear();
        shown_missions.clear();
        const auto append = [&](const wardogs::SavedFireMission& value, bool recent) {
            if (value.map != context.map || value.weapon != mission_weapon(context.weapon)) return;
            shown_missions.push_back({value, recent});
            auto* item = new QListWidgetItem(
                (recent ? ui_text("История") + QStringLiteral(" · ") : QString{}) +
                (value.kind == wardogs::FireMissionKind::firing_position ? ui_text("Орудие") : ui_text("Цель")) +
                QStringLiteral(" · ") + QString::fromStdWString(value.name) +
                QStringLiteral("\n") + point_text(value.point), saved);
            const auto id = QString::fromStdString(value.id);
            item->setData(Qt::UserRole, id);
            item->setData(Qt::UserRole + 1, recent);
            if (id == selected && recent == selected_history) saved->setCurrentItem(item);
        };
        for (const auto& value : recent_values) append(value, true);
        for (const auto& value : values) append(value, false);
        update_mission_actions();
    }
    bool selection_recent() const {
        return saved->currentItem() && saved->currentItem()->data(Qt::UserRole + 1).toBool();
    }
    void update_mission_actions() {
        const bool named = saved->currentItem() && !selection_recent();
        if (rename) rename->setEnabled(named);
        if (erase) erase->setEnabled(named);
    }
    std::optional<wardogs::SavedFireMission> selection() const {
        if (!saved->currentItem()) return std::nullopt;
        const auto id = saved->currentItem()->data(Qt::UserRole).toString().toStdString();
        const auto found = std::find_if(shown_missions.begin(), shown_missions.end(),
            [&](const auto& value) { return value.mission.id == id && value.recent == selection_recent(); });
        if (found == shown_missions.end()) return std::nullopt;
        return found->mission;
    }
    void mission_action(wardogs::FireMissionKind kind) {
        context = provider();
        if (!context.map_confirmed || context.capture_pending ||
            (kind == wardogs::FireMissionKind::target && context.solution_held))
            throw std::runtime_error("Подтвердите карту и завершите чтение координат.");
        const auto point = kind == wardogs::FireMissionKind::target ? context.target : context.base;
        if (!point) throw std::runtime_error("Сначала задайте координаты для сохранения.");
        (void)missions.create(context.map, mission_weapon(context.weapon), kind,
                             name->text().toStdWString(), *point);
        load_missions();
        message(ui_text("Позиция сохранена"));
    }
    void refresh_times() {
        observations.clear();
        observations = read_observations(observations_path);
        times->setRowCount(0);
        for (const auto& value : observations) {
            if (value.weapon != context.weapon || value.arc != selected_arc()) continue;
            const int row = times->rowCount();
            times->insertRow(row);
            const QStringList labels{QString::number(value.distance, 'f', 2),
                QString::number(value.height, 'f', 2), QString::number(value.seconds, 'f', 3),
                value.version, value.source};
            for (int column = 0; column < labels.size(); ++column) {
                auto* item = new QTableWidgetItem(labels[column]);
                item->setData(Qt::UserRole, value.id);
                times->setItem(row, column, item);
            }
        }
    }
    void change_observation(bool remove) {
        if (!QDir().mkpath(directory))
            throw std::runtime_error("Не удалось создать папку планирования.");
        QLockFile lock(observations_path + QStringLiteral(".lock"));
        if (!lock.tryLock(0)) throw std::runtime_error("Измерения времени полёта сейчас заняты.");
        auto latest = read_observations(observations_path);
        if (remove) {
            if (times->currentRow() < 0) return;
            const auto id = times->item(times->currentRow(), 0)->data(Qt::UserRole).toString();
            std::erase_if(latest, [&](const auto& v) { return v.id == id; });
        } else {
            context = provider();
            dialog->refresh();
            if (!ready() || !analysis || !analysis->nominal_mil || !analysis->height_delta_m)
                throw std::runtime_error("Для измерения нужен доступный расчёт и известный перепад высот.");
            if (version->text().trimmed().isEmpty() || source->text().trimmed().isEmpty())
                throw std::runtime_error("Укажите версию игры и источник измерения.");
            if (uncertainty->value() > seconds->value())
                throw std::runtime_error("Погрешность не должна превышать измеренное время.");
            Observation value{QUuid::createUuid().toString(QUuid::WithoutBraces),
                context.weapon, selected_arc(), analysis->distance_m, *analysis->height_delta_m,
                seconds->value(), uncertainty->value(), version->text().trimmed(), source->text().trimmed()};
            const auto found = std::find_if(latest.begin(), latest.end(), [&](const auto& v) {
                return v.weapon == value.weapon && v.arc == value.arc && v.version == value.version &&
                       std::abs(v.height - value.height) <= wardogs::flight_profile_height_tolerance_m &&
                       v.distance == value.distance;
            });
            if (found != latest.end()) { value.id = found->id; *found = value; }
            else latest.push_back(value);
        }
        write_observations(observations_path, latest);
        dialog->refresh();
        message(remove ? ui_text("Измерение удалено") : ui_text("Измерение сохранено"));
    }
    std::optional<wardogs::FlightTimeProfile> time_profile(double height, double distance) const {
        wardogs::FlightTimeProfile profile;
        profile.weapon = context.weapon;
        profile.arc = selected_arc();
        profile.height_delta_m = height;
        profile.ammunition_id = context.weapon == wardogs::AnalysisWeapon::l81 ? "l81-standard" : "sph2-standard";
        profile.game_version = version->text().trimmed().toStdString();
        std::vector<const Observation*> matching;
        for (const auto& value : observations) {
            if (value.weapon == profile.weapon && value.arc == profile.arc &&
                value.version.toStdString() == profile.game_version &&
                std::abs(value.height - height) <= wardogs::flight_profile_height_tolerance_m)
                matching.push_back(&value);
        }
        if (matching.empty()) return std::nullopt;
        std::sort(matching.begin(), matching.end(), [](const auto* a, const auto* b) {
            return a->distance < b->distance;
        });
        for (const auto* value : matching)
            profile.samples.push_back({value->distance, value->seconds, value->uncertainty});
        QStringList sources;
        const auto add_source = [&](const Observation* value) {
            if (!sources.contains(value->source)) sources.push_back(value->source);
        };
        const auto exact = std::find_if(matching.begin(), matching.end(), [&](const auto* value) {
            return std::abs(value->distance - distance) <= 64 * std::numeric_limits<double>::epsilon() *
                   std::max(value->distance, distance);
        });
        if (exact != matching.end()) add_source(*exact);
        else {
            const auto right = std::lower_bound(matching.begin(), matching.end(), distance,
                [](const auto* value, double range) { return value->distance < range; });
            if (right != matching.end()) add_source(*right);
            if (right != matching.begin()) add_source(*std::prev(right));
        }
        profile.source = sources.join(QStringLiteral("; ")).toStdString();
        if (profile.source.empty()) profile.source = matching.front()->source.toStdString();
        return profile;
    }
};

PlanningDialog::PlanningDialog(Provider provider, Apply apply, QWidget* parent,
                               std::filesystem::path storage_directory)
    : QDialog(parent), state_(std::make_unique<State>(this, std::move(provider),
                                                    std::move(apply), std::move(storage_directory))) {
    auto& s = *state_;
    s.context = s.provider();
    setObjectName(QStringLiteral("planningDialog"));
    // The application stylesheet supplies light text, but an unstyled scroll
    // viewport/list otherwise inherits Fusion's white Base. Keep all planning
    // surfaces and item views readable, including standalone diagnostics.
    auto colors = palette();
    colors.setColor(QPalette::Window, QColor("#0b1019"));
    colors.setColor(QPalette::Base, QColor("#0d1521"));
    colors.setColor(QPalette::AlternateBase, QColor("#111b28"));
    colors.setColor(QPalette::WindowText, QColor("#dce6f4"));
    colors.setColor(QPalette::Text, QColor("#dce6f4"));
    colors.setColor(QPalette::Button, QColor("#192638"));
    colors.setColor(QPalette::ButtonText, QColor("#dce6f4"));
    colors.setColor(QPalette::Highlight, QColor("#244b4b"));
    colors.setColor(QPalette::HighlightedText, QColor("#eafbf8"));
    setPalette(colors);
    // A stylesheet can reset child palettes during polish. Style the planning
    // surfaces explicitly while keeping the application's control styles.
    setStyleSheet(QStringLiteral(R"(
QScrollArea#planningPageScroll { background:#0b1019; border:0; }
QWidget#planningPageContent,QWidget#planningPageViewport { background:#0b1019; color:#dce6f4; }
QLabel,QCheckBox { color:#dce6f4; }
QListWidget#savedFireMissions,QTableWidget#flightMeasurements,QTextBrowser {
    background:#0d1521; alternate-background-color:#111b28; color:#dce6f4;
    selection-background-color:#244b4b; selection-color:#eafbf8;
    border:1px solid #2b3b50;
}
QWidget#planningMissionsViewport,QWidget#planningMeasurementsViewport,QWidget#planningSourcesViewport {
    background:#0d1521; color:#dce6f4;
}
QHeaderView::section,QTableCornerButton::section {
    background:#192638; color:#dce6f4; border:1px solid #2b3b50; padding:6px;
}
)"));
    configure_frameless_window(this);
    setWindowTitle(ui_text("Дополнительные инструменты · WARDOGS"));
    setModal(true);
    setMinimumSize(440, 380);
    const auto available = screen()->availableGeometry().size();
    resize(std::min(860, available.width() - 32), std::min(760, available.height() - 48));
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(new WindowTitleBar(this));
    auto* content = new QWidget;
    auto* root = new QVBoxLayout(content);
    root->setContentsMargins(18, 12, 18, 12);
    s.coordinates = note({});
    s.coordinates->setObjectName(QStringLiteral("planningCoordinates"));
    root->addWidget(s.coordinates);
    root->addWidget(note(ui_text("Для обычной стрельбы достаточно выбрать цель и отметить попадание Alt+I. Здесь — перенос цели, история позиций и необязательные измерения.")));
    auto* tabs = new QTabWidget;
    tabs->setObjectName(QStringLiteral("planningTabs"));
    root->addWidget(tabs, 1);

    auto* analysis_page = page(tabs, ui_text("Полёт и рельеф"));
    auto* shifts = new QGroupBox(ui_text("Перенести цель"));
    auto* shift_layout = new QHBoxLayout(shifts);
    s.step = new QComboBox;
    s.step->setObjectName(QStringLiteral("correctionStep"));
    for (int value : {10, 25, 50, 100}) s.step->addItem(ui_text("%1 м").arg(value), value);
    s.step->setAccessibleName(ui_text("Шаг переноса цели"));
    shift_layout->addWidget(s.step);
    struct Shift { const char* text; const char* name; double lateral; double longitudinal; };
    for (const auto shift : {Shift{"Левее", "correctLeft", -1, 0}, Shift{"Правее", "correctRight", 1, 0},
                             Shift{"Ближе", "correctDrop", 0, -1}, Shift{"Дальше", "correctAdd", 0, 1}}) {
        auto* button = new QPushButton(ui_text(shift.text));
        button->setObjectName(QString::fromUtf8(shift.name));
        shift_layout->addWidget(button);
        s.shifts.push_back(button);
        connect(button, &QPushButton::clicked, this, [this, shift] {
            auto& value = *state_;
            value.attempt([&] {
                value.context = value.provider();
                if (!value.ready()) throw std::runtime_error("Подтвердите карту и завершите чтение координат.");
                const double distance = value.step->currentData().toDouble();
                const auto point = wardogs::offset_target(*value.context.base, *value.context.target,
                    shift.lateral * distance, shift.longitudinal * distance);
                value.apply(wardogs::FireMissionKind::target, point, value.context.map, value.context.weapon);
                refresh();
                value.message(ui_text("Цель перенесена · наводка пересчитана"));
            });
        });
    }
    analysis_page->addWidget(shifts);
    analysis_page->addWidget(note(ui_text("Лево/право задаются от орудия к цели. Ближе/дальше смещают саму цель; это команда корректировщика, а не калибровка по попаданию Alt+I.")));
    auto* choices = new QHBoxLayout;
    s.arc = new QComboBox;
    s.arc->setObjectName(QStringLiteral("planningArc"));
    s.arc->addItems({ui_text("Настильная"), ui_text("Навесная")});
    s.arc->setCurrentIndex(s.context.active_arc.value_or(s.context.preferred_arc) == wardogs::Arc::high ? 1 : 0);
    choices->addWidget(new QLabel(ui_text("Траектория")));
    choices->addWidget(s.arc);
    choices->addStretch();
    analysis_page->addLayout(choices);
    s.result = note({});
    s.result->setObjectName(QStringLiteral("flightTimeResult"));
    analysis_page->addWidget(s.result);
    s.plot = new TerrainProfileWidget;
    s.plot->setObjectName(QStringLiteral("terrainProfile"));
    s.plot->setMinimumHeight(230);
    analysis_page->addWidget(s.plot);
    analysis_page->addWidget(note(ui_text("График — геометрическая оценка по цели; фактический путь после поправки Alt+I не измерен.")));
    analysis_page->addWidget(note(ui_text("Зелёный — поверхность земли, оранжевый — оценочная дуга. Пробелы означают отсутствие данных. Дома, крыши, мосты, деревья и высота ствола в файле рельефа не представлены.")));
    s.clearance = note({});
    s.clearance->setObjectName(QStringLiteral("terrainClearance"));
    analysis_page->addWidget(s.clearance);
    s.assume = new QCheckBox(ui_text("Включить предположение о скорости и гравитации"));
    s.assume->setObjectName(QStringLiteral("assumeFlightModel"));
    analysis_page->addWidget(s.assume);
    auto* model = new QFormLayout;
    model->setRowWrapPolicy(QFormLayout::WrapLongRows);
    s.gravity = number(QStringLiteral("assumedGravity"), .001, 1000, 5);
    s.gravity->setValue(9.80665);
    s.speed = number(QStringLiteral("assumedSpeed"), 0, 10000, 3);
    s.speed->setSpecialValueText(ui_text("Не задана"));
    model->addRow(ui_text("Предполагаемая g, м/с²"), s.gravity);
    model->addRow(ui_text("Скорость L81, м/с"), s.speed);
    analysis_page->addLayout(model);
    analysis_page->addWidget(note(ui_text("9,80665 м/с² — выбранное земное предположение, не установленная гравитация WARDOGS. SPH-2: v = √(2629·g); L81: скорость задайте сами. Модель без сопротивления воздуха не меняет табличный MIL и не подтверждает попадание.")));
    analysis_page->addStretch();

    auto* missions_page = page(tabs, ui_text("Позиции и цели"));
    missions_page->addWidget(note(ui_text("Последние позиции и цели сохраняются автоматически в истории. Именованные записи создаются вручную. Для восстановления подтвердите карту и орудие; прежние поправки не восстанавливаются.")));
    s.name = new QLineEdit;
    s.name->setObjectName(QStringLiteral("missionName"));
    s.name->setMaxLength(static_cast<int>(wardogs::maximum_fire_mission_name_length));
    s.name->setPlaceholderText(ui_text("Название позиции или цели"));
    missions_page->addWidget(s.name);
    auto* save_row = new QHBoxLayout;
    s.save_gun = new QPushButton(ui_text("Сохранить орудие"));
    s.save_gun->setObjectName(QStringLiteral("saveFiringPosition"));
    s.save_target = new QPushButton(ui_text("Сохранить цель"));
    s.save_target->setObjectName(QStringLiteral("saveFireTarget"));
    save_row->addWidget(s.save_gun);
    save_row->addWidget(s.save_target);
    missions_page->addLayout(save_row);
    s.saved = new QListWidget;
    s.saved->setObjectName(QStringLiteral("savedFireMissions"));
    s.saved->viewport()->setObjectName(QStringLiteral("planningMissionsViewport"));
    s.saved->setMinimumHeight(180);
    missions_page->addWidget(s.saved, 1);
    auto* actions = new QHBoxLayout;
    s.restore = new QPushButton(ui_text("Восстановить выбранное"));
    s.restore->setObjectName(QStringLiteral("restoreFireMission"));
    s.rename = new QPushButton(ui_text("Переименовать"));
    s.rename->setObjectName(QStringLiteral("renameFireMission"));
    s.erase = new QPushButton(ui_text("Удалить"));
    s.erase->setObjectName(QStringLiteral("deleteFireMission"));
    actions->addWidget(s.restore);
    actions->addWidget(s.rename);
    actions->addWidget(s.erase);
    missions_page->addLayout(actions);
    connect(s.save_gun, &QPushButton::clicked, this, [&s] { s.attempt([&] { s.mission_action(wardogs::FireMissionKind::firing_position); }); });
    connect(s.save_target, &QPushButton::clicked, this, [&s] { s.attempt([&] { s.mission_action(wardogs::FireMissionKind::target); }); });
    connect(s.saved, &QListWidget::currentRowChanged, this, [&s] {
        if (const auto value = s.selection()) s.name->setText(QString::fromStdWString(value->name));
        s.update_mission_actions();
    });
    connect(s.restore, &QPushButton::clicked, this, [this] {
        auto& value = *state_;
        value.attempt([&] {
            const auto selected = value.selection();
            if (!selected) return;
            value.context = value.provider();
            if (value.context.capture_pending ||
                (selected->kind == wardogs::FireMissionKind::target && value.context.solution_held))
                throw std::runtime_error("Подтвердите карту и завершите чтение координат.");
            const auto point = wardogs::restore_fire_mission(*selected, value.context.map,
                mission_weapon(value.context.weapon), value.context.map_confirmed);
            value.apply(selected->kind, point, value.context.map, value.context.weapon);
            refresh();
            value.message(ui_text("Позиция восстановлена"));
        });
    });
    connect(s.rename, &QPushButton::clicked, this, [&s] { s.attempt([&] {
        if (s.selection_recent()) return;
        if (const auto selected = s.selection()) {
            (void)s.missions.update(selected->id, s.name->text().toStdWString(), selected->point);
            s.load_missions(); s.message(ui_text("Название сохранено"));
        }
    }); });
    connect(s.erase, &QPushButton::clicked, this, [&s] { s.attempt([&] {
        if (s.selection_recent()) return;
        if (const auto selected = s.selection()) {
            (void)s.missions.erase(selected->id); s.load_missions(); s.message(ui_text("Запись удалена"));
        }
    }); });

    auto* time_page = page(tabs, ui_text("Измерения времени"));
    time_page->addWidget(note(ui_text("Alt+I отмечает координаты попадания и не измеряет время полёта: момент выстрела неизвестен. Время ниже — только ваш отдельный замер.")));
    time_page->addWidget(note(ui_text("Измерьте время от выстрела до попадания. Запись относится к текущей цели, орудию, траектории, перепаду высот и версии игры. Между измеренными дальностями используется интерполяция, за пределами — время неизвестно. При смене боеприпаса используйте отдельную метку версии/профиля.")));
    auto* timing_form = new QFormLayout;
    timing_form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    s.version = new QLineEdit;
    s.version->setObjectName(QStringLiteral("flightGameVersion"));
    s.version->setMaxLength(80);
    s.version->setPlaceholderText(ui_text("Версия игры / боеприпас / заряд"));
    s.source = new QLineEdit;
    s.source->setObjectName(QStringLiteral("flightMeasurementSource"));
    s.source->setMaxLength(200);
    s.source->setPlaceholderText(ui_text("Например, собственный замер по видеозаписи"));
    s.seconds = number(QStringLiteral("measuredFlightSeconds"), .001, 600, 3);
    s.seconds->setValue(20);
    s.uncertainty = number(QStringLiteral("flightUncertainty"), 0, 600, 3);
    s.uncertainty->setValue(.5);
    timing_form->addRow(ui_text("Версия / профиль"), s.version);
    timing_form->addRow(ui_text("Источник"), s.source);
    timing_form->addRow(ui_text("Измеренное время, с"), s.seconds);
    timing_form->addRow(ui_text("Погрешность замера ±с"), s.uncertainty);
    time_page->addLayout(timing_form);
    s.record = new QPushButton(ui_text("Записать время для текущей цели"));
    s.record->setObjectName(QStringLiteral("recordFlightTime"));
    time_page->addWidget(s.record);
    s.times = new QTableWidget(0, 5);
    s.times->setObjectName(QStringLiteral("flightMeasurements"));
    s.times->viewport()->setObjectName(QStringLiteral("planningMeasurementsViewport"));
    s.times->setHorizontalHeaderLabels({ui_text("Дальность, м"), ui_text("Перепад, м"), ui_text("Время, с"), ui_text("Версия / профиль"), ui_text("Источник")});
    s.times->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    s.times->horizontalHeader()->setStretchLastSection(true);
    s.times->setEditTriggers(QAbstractItemView::NoEditTriggers);
    s.times->setSelectionBehavior(QAbstractItemView::SelectRows);
    s.times->setSelectionMode(QAbstractItemView::SingleSelection);
    s.times->setMinimumHeight(160);
    time_page->addWidget(s.times, 1);
    auto* remove_time = new QPushButton(ui_text("Удалить выбранный замер"));
    remove_time->setObjectName(QStringLiteral("deleteFlightTime"));
    time_page->addWidget(remove_time);
    connect(s.record, &QPushButton::clicked, this, [&s] { s.attempt([&] { s.change_observation(false); }); });
    connect(remove_time, &QPushButton::clicked, this, [&s] { s.attempt([&] { s.change_observation(true); }); });

    auto* profiles_page = page(tabs, ui_text("Профили и источники"));
    s.profile_info = note({});
    s.profile_info->setObjectName(QStringLiteral("weaponProfileInfo"));
    profiles_page->addWidget(s.profile_info);
    auto* links = new QTextBrowser;
    links->viewport()->setObjectName(QStringLiteral("planningSourcesViewport"));
    links->setOpenExternalLinks(true);
    links->setMinimumHeight(150);
    links->setHtml(QStringLiteral("<style>a { color:#63d8c5; }</style><p><a href='https://github.com/apollyon-sys/wardogs-calculator/blob/main/data/weapons.json'>Apollyon · weapons.json</a></p>"
        "<p><a href='https://github.com/apollyon-sys/wardogs-calculator/blob/main/docs/features.md'>Apollyon · declared limits / table coverage</a></p>"
        "<p><a href='https://wardogs.t0ki.cn/'>t0ki · community calculator</a></p>"
        "<p><a href='https://github.com/Firepanda415/MZ-Wardogs/blob/main/docs/sph2-scale.md'>MZ-Wardogs · SPH-2 scale observations</a></p>"
        "<p><a href='https://metaforge.app/wardogs/map'>MetaForge · external comparison</a></p>"));
    profiles_page->addWidget(links);
    profiles_page->addWidget(note(ui_text("Источники сообщества не являются официальной спецификацией игры. L52 в MetaForge не подтверждён как тот же профиль, что SPH-2. Разные калькуляторы и замеры шкалы расходятся; обновление игры может изменить баллистику.")));
    profiles_page->addStretch();

    s.status = note({});
    s.status->setObjectName(QStringLiteral("planningStatus"));
    root->addWidget(s.status);
    auto* close = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(close, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(close);
    outer->addWidget(content);
    QSettings settings(s.directory + QStringLiteral("/planning.ini"), QSettings::IniFormat);
    s.assume->setChecked(settings.value("assume_model", false).toBool());
    const double saved_g = settings.value("gravity", 9.80665).toDouble();
    const double saved_v = settings.value("l81_speed", 0).toDouble();
    if (std::isfinite(saved_g) && saved_g >= .001 && saved_g <= 1000) s.gravity->setValue(saved_g);
    if (std::isfinite(saved_v) && saved_v >= 0 && saved_v <= 10000) s.speed->setValue(saved_v);
    s.version->setText(settings.value("game_version").toString().left(80));
    s.source->setText(settings.value("measurement_source").toString().left(200));
    s.loading = false;
    connect(s.arc, &QComboBox::currentIndexChanged, this, [this] { refresh(); });
    connect(s.assume, &QCheckBox::toggled, this, [this] { refresh(); });
    connect(s.gravity, &QDoubleSpinBox::valueChanged, this, [this] { refresh(); });
    connect(s.speed, &QDoubleSpinBox::valueChanged, this, [this] { refresh(); });
    connect(s.version, &QLineEdit::editingFinished, this, [this] { refresh(); });
    refresh();
    wardogs::i18n::watch(this);
    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [this] {
        const auto next = state_->provider();
        const auto& old = state_->context;
        const bool command_changed = next.active_solution.has_value() != old.active_solution.has_value() ||
            (next.active_solution && old.active_solution &&
             (next.active_solution->arc != old.active_solution->arc ||
              next.active_solution->bearing_deg != old.active_solution->bearing_deg ||
              next.active_solution->mil != old.active_solution->mil));
        if (next.map != old.map || next.weapon != old.weapon || next.base != old.base ||
            next.target != old.target || next.map_confirmed != old.map_confirmed ||
            next.capture_pending != old.capture_pending || next.solution_held != old.solution_held ||
            next.active_arc != old.active_arc || command_changed)
            refresh();
    });
    timer->start(500);
}

PlanningDialog::~PlanningDialog() {
    auto& s = *state_;
    QSettings settings(s.directory + QStringLiteral("/planning.ini"), QSettings::IniFormat);
    settings.setValue("assume_model", s.assume->isChecked());
    settings.setValue("gravity", s.gravity->value());
    settings.setValue("l81_speed", s.speed->value());
    settings.setValue("game_version", s.version->text().trimmed());
    settings.setValue("measurement_source", s.source->text().trimmed());
    settings.sync();
}

void PlanningDialog::refresh() {
    auto& s = *state_;
    if (s.loading) return;
    const auto previous_active_arc = s.context.active_arc;
    s.context = s.provider();
    if (s.context.active_arc && s.context.active_arc != previous_active_arc) {
        const QSignalBlocker block(s.arc);
        s.arc->setCurrentIndex(*s.context.active_arc == wardogs::Arc::high ? 1 : 0);
    }
    s.message({});
    const bool l81 = s.context.weapon == wardogs::AnalysisWeapon::l81;
    s.arc->setEnabled(!l81);
    if (l81) { QSignalBlocker block(s.arc); s.arc->setCurrentIndex(1); }
    s.gravity->setEnabled(s.assume->isChecked());
    s.speed->setEnabled(s.assume->isChecked() && l81);
    s.coordinates->setText(QStringLiteral("%1 · %2\n%3\n%4")
        .arg(QString::fromStdWString(std::wstring(wardogs::game_map_key(s.context.map))),
             l81 ? QStringLiteral("L81") : QStringLiteral("SPH-2"),
             s.context.base ? ui_text("Орудие: %1").arg(point_text(*s.context.base)) : ui_text("Орудие не задано"),
             s.context.target ? ui_text("Цель: %1").arg(point_text(*s.context.target)) : ui_text("Цель не задана")));
    s.profile_info->setText(l81
        ? ui_text("L81 · 71 точка · 132–684 м · 850–150 MIL\nИсточник: Apollyon, таблица получена 03.10.2026. Строки 80–131 м и выше 684 м не входят в заявленный рабочий диапазон. MIL — команда прицела; её связь с физическим углом не подтверждена. Штатная высотная модель и время полёта неизвестны.")
        : ui_text("SPH-2 · настильная: 1181–2629 м, 20–600 MIL; навесная: 735–2629 м, 1400–610 MIL\nИсточник: t0ki, таблица получена 11.09.2026. Перепад высот и дуга используют геометрическую модель R=2629 м. Это оценка, а не официальная игровая физика; локальные поправки Alt+I анализируются отдельно."));
    s.save_gun->setEnabled(s.context.map_confirmed && s.context.base && !s.context.capture_pending);
    s.save_target->setEnabled(s.ready());
    s.restore->setEnabled(s.context.map_confirmed && !s.context.capture_pending);
    for (auto* button : s.shifts) button->setEnabled(s.ready());
    s.record->setEnabled(s.ready());
    s.analysis.reset();
    s.plot->set_samples({});
    s.clearance->setText(ui_text("Профиль рельефа не рассчитан"));
    s.result->setText(ui_text("Время полёта неизвестно"));
    s.attempt([&] { s.load_missions(); });
    s.attempt([&] { s.refresh_times(); });
    if (!s.ready()) {
        s.message(ui_text("Подтвердите карту и задайте орудие и цель. Неподтверждённый захват блокирует анализ."));
        return;
    }
    s.attempt([&] {
        wardogs::FiringAnalysisRequest request;
        request.weapon = s.context.weapon;
        request.arc = s.selected_arc();
        request.base = *s.context.base;
        request.target = *s.context.target;
        request.terrain = s.context.terrain;
        request.ammunition_id = l81 ? "l81-standard" : "sph2-standard";
        request.game_version = s.version->text().trimmed().toStdString();
        if (s.context.map == wardogs::GameMap::training || s.context.map == wardogs::GameMap::other)
            request.height_delta_m = 0.0;
        // Named terrain maps without installed data must not silently become
        // level-terrain solutions. For L81 the sight table remains horizontal.
        else if (!request.terrain) {
            request.terrain = [](wardogs::Point) -> std::optional<double> { return std::nullopt; };
            s.message(ui_text("Установите рельеф текущей карты; перепад высот неизвестен."), true);
        }
        if (s.assume->isChecked()) {
            if (!l81) request.gravity_mps2 = s.gravity->value();
            else if (s.speed->value() > 0)
                request.vacuum_profile = wardogs::VacuumFlightProfile{s.speed->value(), s.gravity->value(), "User-assumed vacuum model"};
        }
        auto first = wardogs::analyze_firing(request);
        if (first.height_delta_m) request.flight_profile = s.time_profile(*first.height_delta_m, first.distance_m);
        s.analysis = wardogs::analyze_firing(request);
        const auto& result = *s.analysis;
        QString summary = ui_text("До цели %1 м").arg(result.distance_m, 0, 'f', 2);
        if (!l81 && s.context.active_solution && s.context.active_arc &&
            s.context.active_solution->arc == *s.context.active_arc &&
            s.selected_arc() == *s.context.active_arc) {
            const auto command = wardogs::displayed_firing_command(*s.context.active_solution);
            summary += ui_text("\nАктивная наводка с учётом Alt+I: установить %1 MIL · азимут %2°")
                .arg(command.mil, 0, 'f', 0).arg(command.bearing_deg, 0, 'f', 1);
            summary += ui_text("\nПо таблице ≈ %1 м").arg(std::round(command.table_distance_m), 0, 'f', 0);
        } else if (result.nominal_mil) {
            summary += l81
                ? ui_text("\nУстановить %1 MIL · азимут %2°")
                    .arg(*result.nominal_mil, 0, 'f', 2).arg(result.bearing_deg, 0, 'f', 2)
                : ui_text("\nПредварительный расчёт: %1 MIL · азимут %2°; без поправки Alt+I. Это не активная наводка.")
                    .arg(*result.nominal_mil, 0, 'f', 2).arg(result.bearing_deg, 0, 'f', 2);
        } else summary += ui_text("\nТабличная наводка недоступна");
        if (result.flight_time) {
            const auto& time = *result.flight_time;
            const bool measured = time.source.basis == wardogs::EstimateBasis::user_measurement ||
                                  time.source.basis == wardogs::EstimateBasis::interpolated_user_measurement;
            summary += ui_text("\nВремя полёта: %1 с · %2").arg(time.seconds, 0, 'f', 3).arg(
                measured ? (time.source.basis == wardogs::EstimateBasis::user_measurement
                    ? ui_text("собственный замер") : ui_text("интерполяция собственных замеров")) : ui_text("оценка предположенной модели"));
            if (time.uncertainty_s) summary += ui_text(" · ±%1 с").arg(*time.uncertainty_s, 0, 'f', 3);
            if (measured) summary += ui_text("\nВерсия / профиль: %1").arg(QString::fromStdString(time.source.game_version)) +
                ui_text("\nИсточник замера: %1").arg(QString::fromStdString(time.source.source));
        } else summary += ui_text("\nВремя полёта неизвестно: добавьте замеры или явно включите модель.");
        if (l81 && s.assume->isChecked() && s.speed->value() <= 0)
            summary += ui_text("\nДля модели L81 задайте скорость снаряда.");
        if (result.trajectory)
            summary += ui_text("\nВершина оценочной дуги: +%1 м на дальности %2 м")
                .arg(result.trajectory->apex_height_above_muzzle_m, 0, 'f', 1)
                .arg(result.trajectory->apex_distance_m, 0, 'f', 1);
        s.result->setText(summary);
        std::vector<ProfilePoint> samples;
        if (result.trajectory) {
            for (const auto& sample : result.trajectory->samples)
                samples.push_back({sample.distance_m, sample.terrain_height_above_muzzle_m, sample.height_above_muzzle_m});
        } else if (request.terrain) {
            const auto origin = request.terrain(request.base);
            const int count = std::clamp(static_cast<int>(std::min(result.distance_m / 2.0 + 1, 1024.0)), 2, 1024);
            for (int i = 0; i <= count; ++i) {
                const double ratio = static_cast<double>(i) / count;
                const wardogs::Point p{request.base.x + (request.target.x - request.base.x) * ratio,
                                      request.base.y + (request.target.y - request.base.y) * ratio};
                const auto height = request.terrain(p);
                samples.push_back({result.distance_m * ratio,
                    height && origin ? std::optional<double>(*height - *origin) : std::nullopt, std::nullopt});
            }
        }
        s.plot->set_samples(std::move(samples));
        QString clearance;
        switch (result.clearance.status) {
        case wardogs::TerrainClearanceStatus::blocked:
            clearance = ui_text("Модель пересекает землю примерно на %1 м.")
                .arg(result.clearance.first_blocked_distance_m.value_or(0), 0, 'f', 1); break;
        case wardogs::TerrainClearanceStatus::clear_at_samples:
            clearance = ui_text("В проверенных точках пересечений с землёй нет. Это не гарантия свободного полёта."); break;
        case wardogs::TerrainClearanceStatus::incomplete:
            clearance = ui_text("Проверка неполная: отсутствуют высоты в %1 точках. Свободный полёт не подтверждён.")
                .arg(static_cast<qulonglong>(result.clearance.samples_missing)); break;
        case wardogs::TerrainClearanceStatus::model_unavailable:
            clearance = ui_text("Траектория неизвестна; пересечения с рельефом не проверены."); break;
        default:
            clearance = ui_text("Рельеф не проверен: данные поверхности отсутствуют."); break;
        }
        if (result.clearance.minimum_clearance_m)
            clearance += ui_text("\nМинимальный зазор в выборке: %1 м · шаг %2 м")
                .arg(*result.clearance.minimum_clearance_m, 0, 'f', 1)
                .arg(result.clearance.actual_sample_step_m, 0, 'f', 2);
        s.clearance->setText(clearance);
        s.record->setEnabled(result.nominal_mil.has_value() && result.height_delta_m.has_value());
    });
}
