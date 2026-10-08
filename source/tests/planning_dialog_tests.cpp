#include "planning_dialog.hpp"
#include "localization.hpp"

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFont>
#include <QFontMetrics>
#include <QGroupBox>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLockFile>
#include <QPalette>
#include <QPixmap>
#include <QPushButton>
#include <QRawFont>
#include <QRegularExpression>
#include <QScrollArea>
#include <QStyleFactory>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QUuid>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int assertions{};
int failures{};
QString evidence_directory;
QJsonArray screenshots;
QJsonArray fixtures;
QJsonArray surface_contrast;

void check(bool condition, const char* message) {
    ++assertions;
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

void require(bool condition, const char* message) {
    check(condition, message);
    if (!condition) throw std::runtime_error(message);
}

QString translated(const char* russian) {
    return wardogs::i18n::text(QString::fromUtf8(russian));
}

bool cyrillic(const QString& text) {
    static const QRegularExpression letters(QStringLiteral("[\\x{0400}-\\x{052f}]"));
    return letters.match(text).hasMatch();
}

void settle() {
    QApplication::sendPostedEvents();
    QApplication::processEvents(QEventLoop::AllEvents, 20);
}

template<class Widget>
Widget* child(PlanningDialog& dialog, const char* name) {
    auto* widget = dialog.findChild<Widget*>(QString::fromLatin1(name));
    require(widget != nullptr, name);
    return widget;
}

std::filesystem::path native_path(const QString& path) {
    return std::filesystem::path(path.toStdWString());
}

QByteArray read_bytes(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "the isolated fixture file can be read");
    return file.readAll();
}

void write_bytes(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
            file.write(bytes) == bytes.size(), "the isolated fixture file can be written");
}

bool same_point(wardogs::Point a, wardogs::Point b) {
    return std::bit_cast<std::uint64_t>(a.x) == std::bit_cast<std::uint64_t>(b.x) &&
           std::bit_cast<std::uint64_t>(a.y) == std::bit_cast<std::uint64_t>(b.y);
}

struct AppliedPoint {
    wardogs::FireMissionKind kind;
    wardogs::Point point;
    wardogs::GameMap map;
    wardogs::AnalysisWeapon weapon;
};

struct Harness {
    PlanningContext context;
    int provider_calls{};
    std::vector<AppliedPoint> applied;

    Harness() {
        context.map = wardogs::GameMap::training;
        context.map_confirmed = true;
        context.weapon = wardogs::AnalysisWeapon::l81;
        context.preferred_arc = wardogs::Arc::high;
        context.base = wardogs::Point{0, 0};
        context.target = wardogs::Point{3, 0};
    }

    PlanningDialog::Provider provider() {
        return [this] { ++provider_calls; return context; };
    }

    PlanningDialog::Apply apply() {
        return [this](wardogs::FireMissionKind kind, wardogs::Point point,
                      wardogs::GameMap map, wardogs::AnalysisWeapon weapon) {
            applied.push_back({kind, point, map, weapon});
            if (kind == wardogs::FireMissionKind::firing_position) context.base = point;
            else context.target = point;
        };
    }
};

void click(PlanningDialog& dialog, const char* name) {
    child<QPushButton>(dialog, name)->click();
    settle();
}

void choose_tab(PlanningDialog& dialog, int index) {
    auto* tabs = child<QTabWidget>(dialog, "planningTabs");
    require(index >= 0 && index < tabs->count(), "the requested planning tab exists");
    tabs->setCurrentIndex(index);
    settle();
}

void make_visible(PlanningDialog& dialog, QWidget* widget) {
    auto* tabs = child<QTabWidget>(dialog, "planningTabs");
    auto* scroll = qobject_cast<QScrollArea*>(tabs->currentWidget());
    require(scroll != nullptr, "each planning page has a real scroll viewport");
    scroll->ensureWidgetVisible(widget, 12, 12);
    settle();
    check(widget->isVisible() && !widget->visibleRegion().isEmpty(),
          "the chosen planning control is visible inside the native dialog");
}

void screenshot(PlanningDialog& dialog, const QString& name, QWidget* focus = nullptr) {
    if (focus) make_visible(dialog, focus);
    settle();
    const auto image = dialog.grab();
    require(!image.isNull() && image.width() >= 440 && image.height() >= 380,
            "the planning screenshot contains a rendered native dialog");
    const QString path = QDir(evidence_directory).filePath(name + QStringLiteral(".png"));
    require(image.save(path), "the planning screenshot is saved as evidence");
    screenshots.append(QJsonObject{{"file", path}, {"width", image.width()}, {"height", image.height()}});
}

bool has_colour(const QImage& image, QRgb colour) {
    int count{};
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if ((image.pixel(x, y) & 0x00ffffffU) == (colour & 0x00ffffffU) && ++count > 4)
                return true;
    return false;
}

double luminance(const QColor& colour) {
    return .2126 * colour.red() + .7152 * colour.green() + .0722 * colour.blue();
}

void assert_dark_surface(QWidget* surface, QPalette::ColorRole text_role,
                         const QString& name, bool page_gutter) {
    require(surface && surface->isVisible() && !surface->visibleRegion().isEmpty(),
            "contrast acceptance inspects a visible native surface");
    const auto image = surface->grab().toImage();
    require(!image.isNull() && image.width() > 12 && image.height() > 12,
            "contrast acceptance samples actual rendered pixels");
    // Page content has a six-pixel layout gutter. Item views have one retained
    // row and blank space beneath it. Median samples avoid text, selection and
    // borders without assuming any implementation-specific background colour.
    const int offset = std::max(2, static_cast<int>(2 * image.devicePixelRatio()));
    const int x = page_gutter ? offset : image.width() - offset - 1;
    std::vector<QColor> samples;
    for (int i = 0; i < 7; ++i) {
        const int y = static_cast<int>(image.height() * (.2 + .1 * i));
        samples.push_back(image.pixelColor(x, y));
    }
    std::sort(samples.begin(), samples.end(), [](const auto& a, const auto& b) {
        return luminance(a) < luminance(b);
    });
    const auto background = samples[samples.size() / 2];
    const auto foreground = surface->palette().color(QPalette::Active, text_role);
    const double background_luminance = luminance(background);
    const double contrast = luminance(foreground) - background_luminance;
    check(background_luminance < 80,
          "rendered pages, lists and tables keep a dark background instead of Fusion white");
    check(contrast >= 100,
          "the native text colour has clear luminance contrast against the rendered background");
    int contrasting_pixels{};
    for (int y = 0; y < image.height() && contrasting_pixels < 20; ++y)
        for (int column = 0; column < image.width() && contrasting_pixels < 20; ++column)
            if (luminance(image.pixelColor(column, y)) - background_luminance >= 100)
                ++contrasting_pixels;
    check(contrasting_pixels >= 20, "real native surfaces visibly render contrasting foreground content");
    surface_contrast.append(QJsonObject{{"surface", name}, {"background", background.name()},
        {"foreground", foreground.name()}, {"background_luminance", background_luminance},
        {"luminance_difference", contrast}, {"contrasting_pixels_at_least", contrasting_pixels}});
}

void inspect_page_contrast(PlanningDialog& dialog, const QString& name) {
    auto* tabs = child<QTabWidget>(dialog, "planningTabs");
    auto* page = qobject_cast<QScrollArea*>(tabs->currentWidget());
    require(page != nullptr, "contrast acceptance inspects the current real tab viewport");
    assert_dark_surface(page->viewport(), QPalette::WindowText, name + QStringLiteral("-page"), true);
    if (tabs->currentIndex() == 1) {
        auto* list = child<QListWidget>(dialog, "savedFireMissions");
        make_visible(dialog, list);
        assert_dark_surface(list->viewport(), QPalette::Text, name + QStringLiteral("-list"), false);
    }
    if (tabs->currentIndex() == 2) {
        auto* table = child<QTableWidget>(dialog, "flightMeasurements");
        make_visible(dialog, table);
        assert_dark_surface(table->viewport(), QPalette::Text, name + QStringLiteral("-table"), false);
    }
}

void inspect_english(PlanningDialog& dialog) {
    check(!cyrillic(dialog.windowTitle()), "the English planning title contains no untranslated Cyrillic");
    for (auto* label : dialog.findChildren<QLabel*>())
        check(!cyrillic(label->text()), "English planning labels contain no untranslated Cyrillic");
    for (auto* button : dialog.findChildren<QAbstractButton*>()) {
        check(!cyrillic(button->text()), "English planning buttons contain no untranslated Cyrillic");
        check(!cyrillic(button->toolTip()) && !cyrillic(button->accessibleName()),
              "English button hints and accessible names are translated");
    }
    for (auto* group : dialog.findChildren<QGroupBox*>())
        check(!cyrillic(group->title()), "English planning groups are translated");
    for (auto* edit : dialog.findChildren<QLineEdit*>())
        check(!cyrillic(edit->placeholderText()), "English editable field placeholders are translated");
    for (auto* combo : dialog.findChildren<QComboBox*>())
        for (int i = 0; i < combo->count(); ++i)
            check(!cyrillic(combo->itemText(i)), "English planning choices are translated without changing data");
    auto* tabs = child<QTabWidget>(dialog, "planningTabs");
    for (int i = 0; i < tabs->count(); ++i)
        check(!cyrillic(tabs->tabText(i)), "all four English planning tab names are translated");
    auto* table = child<QTableWidget>(dialog, "flightMeasurements");
    for (int i = 0; i < table->columnCount(); ++i)
        check(table->horizontalHeaderItem(i) && !cyrillic(table->horizontalHeaderItem(i)->text()),
              "English measurement column headers are translated");
    for (auto* browser : dialog.findChildren<QTextBrowser*>())
        check(!cyrillic(browser->toPlainText()), "English source descriptions contain no untranslated Cyrillic");
}

bool time_unknown(PlanningDialog& dialog) {
    return child<QLabel>(dialog, "flightTimeResult")->text().contains(
        translated("\nВремя полёта неизвестно: добавьте замеры или явно включите модель."));
}

void assert_error(PlanningDialog& dialog) {
    auto* status = child<QLabel>(dialog, "planningStatus");
    check(status->property("error").toBool() && !status->text().isEmpty(),
          "a rejected planning action presents an honest error");
    if (wardogs::i18n::language() == wardogs::UiLanguage::english)
        check(!cyrillic(status->text()), "English planning errors are translated");
}

void correction_and_guard_tests(PlanningDialog& dialog, Harness& harness) {
    auto* step = child<QComboBox>(dialog, "correctionStep");
    check(step->count() == 4 && step->itemData(0).toInt() == 10 &&
          step->itemData(1).toInt() == 25 && step->itemData(2).toInt() == 50 &&
          step->itemData(3).toInt() == 100, "real correction choices preserve metre-valued item data");
    step->setCurrentIndex(2);
    const wardogs::Point base{0, 0}, target{0, 3};
    struct Shift { const char* button; wardogs::Point expected; };
    for (const auto& shift : {Shift{"correctLeft", {-0.5, 3}}, Shift{"correctRight", {0.5, 3}},
                             Shift{"correctDrop", {0, 2.5}}, Shift{"correctAdd", {0, 3.5}}}) {
        harness.context.base = base;
        harness.context.target = target;
        dialog.refresh();
        const auto before = harness.applied.size();
        click(dialog, shift.button);
        check(harness.applied.size() == before + 1 &&
              same_point(harness.applied.back().point, shift.expected),
              "the actual correction button uses gun-to-target orientation and metres/100 map units");
        require(harness.applied.size() == before + 1,
                "a ready correction delivers exactly one accepted point");
        check(harness.applied.back().kind == wardogs::FireMissionKind::target &&
              harness.applied.back().map == harness.context.map &&
              harness.applied.back().weapon == harness.context.weapon,
              "corrections apply the accepted target with explicit current map and weapon");
    }
    const auto baseline = harness.context;
    for (int guard = 0; guard < 4; ++guard) {
        harness.context = baseline;
        dialog.refresh();
        require(child<QPushButton>(dialog, "correctRight")->isEnabled(),
                "the stale-context test starts from an enabled real button");
        if (guard == 0) harness.context.map_confirmed = false;
        if (guard == 1) harness.context.capture_pending = true;
        if (guard == 2) harness.context.solution_held = true;
        if (guard == 3) harness.context.target.reset();
        const auto before = harness.applied.size();
        const int providers = harness.provider_calls;
        click(dialog, "correctRight");
        check(harness.applied.size() == before && harness.provider_calls > providers,
              "a correction re-reads Provider and rejects a newly invalid context before Apply");
        assert_error(dialog);
        dialog.refresh();
        check(!child<QPushButton>(dialog, "correctRight")->isEnabled() &&
              !child<QPushButton>(dialog, "saveFireTarget")->isEnabled() &&
              !child<QPushButton>(dialog, "recordFlightTime")->isEnabled(),
              "unconfirmed, pending, held or missing coordinates disable target-changing analysis actions");
    }
    harness.context = baseline;
    dialog.refresh();
}

void mission_tests(PlanningDialog& dialog, Harness& harness, const QString& directory) {
    choose_tab(dialog, 1);
    auto* name = child<QLineEdit>(dialog, "missionName");
    auto* saved = child<QListWidget>(dialog, "savedFireMissions");
    wardogs::FireMissionRepository repository(native_path(directory) / L"fire-missions.json");
    const wardogs::Point exact_base{std::nextafter(0.125, 1.0), -0.0};
    const wardogs::Point exact_target{std::nextafter(3.125, 4.0), std::nextafter(0.0, 1.0)};
    harness.context.base = exact_base;
    harness.context.target = exact_target;
    dialog.refresh();
    name->setText(QStringLiteral("Alpha position"));
    click(dialog, "saveFiringPosition");
    name->setText(QStringLiteral("Bravo target"));
    click(dialog, "saveFireTarget");
    require(saved->count() == 2 && repository.load().size() == 2,
            "real save buttons persist separately named firing positions and targets");
    check(same_point(repository.load()[0].point, exact_base) &&
          same_point(repository.load()[1].point, exact_target),
          "UI save preserves full double precision including negative zero and subnormal coordinates");
    saved->setCurrentRow(0);
    name->setText(QStringLiteral("Renamed position"));
    click(dialog, "renameFireMission");
    check(repository.load()[0].name == L"Renamed position" &&
          same_point(repository.load()[0].point, exact_base), "the real rename button preserves stored coordinates");
    saved->setCurrentRow(1);
    const auto baseline = harness.context;
    for (int guard = 0; guard < 5; ++guard) {
        harness.context = baseline;
        dialog.refresh();
        saved->setCurrentRow(1);
        require(child<QPushButton>(dialog, "restoreFireMission")->isEnabled(),
                "the stale restore test starts with a selected enabled saved target");
        if (guard == 0) harness.context.map_confirmed = false;
        if (guard == 1) harness.context.map = wardogs::GameMap::ozeti;
        if (guard == 2) harness.context.weapon = wardogs::AnalysisWeapon::sph2;
        if (guard == 3) harness.context.capture_pending = true;
        if (guard == 4) harness.context.solution_held = true;
        const auto before = harness.applied.size();
        click(dialog, "restoreFireMission");
        check(harness.applied.size() == before,
              "restore rejects newly changed map, weapon, confirmation, pending or held context");
        assert_error(dialog);
    }
    harness.context = baseline;
    dialog.refresh();
    saved->setCurrentRow(1);
    harness.context.target = wardogs::Point{3, 0};
    const auto before = harness.applied.size();
    click(dialog, "restoreFireMission");
    check(harness.applied.size() == before + 1 && same_point(harness.applied.back().point, exact_target),
          "explicit target restore passes the exact stored Point to Apply without rounding");
    saved->setCurrentRow(0);
    click(dialog, "restoreFireMission");
    check(harness.applied.back().kind == wardogs::FireMissionKind::firing_position &&
          same_point(harness.applied.back().point, exact_base),
          "explicit position restore keeps its kind and full-precision coordinates");
    (void)repository.create(wardogs::GameMap::ozeti, wardogs::FireMissionWeapon::l81,
                           wardogs::FireMissionKind::target, L"Other map", {3, 0});
    (void)repository.create(wardogs::GameMap::training, wardogs::FireMissionWeapon::sph2,
                           wardogs::FireMissionKind::target, L"Other weapon", {20, 0});
    dialog.refresh();
    check(saved->count() == 2, "saved entries from another map or weapon are filtered from the real list");
    harness.context.map = wardogs::GameMap::ozeti;
    dialog.refresh();
    check(saved->count() == 1 && saved->item(0)->text().contains(QStringLiteral("Other map")),
          "switching maps displays only that map's saved points");
    harness.context = baseline;
    harness.context.weapon = wardogs::AnalysisWeapon::sph2;
    dialog.refresh();
    check(saved->count() == 1 && saved->item(0)->text().contains(QStringLiteral("Other weapon")),
          "switching weapons displays only that weapon's saved points");
    harness.context = baseline;
    dialog.refresh();
    saved->setCurrentRow(0);
    click(dialog, "deleteFireMission");
    check(saved->count() == 1 && repository.load().size() == 3,
          "the real delete button removes only the selected saved point");
    const auto bytes = read_bytes(directory + QStringLiteral("/fire-missions.json"));
    name->setText(QStringLiteral("Should not save"));
    harness.context.capture_pending = true; // No refresh: exercise the action-time guard.
    click(dialog, "saveFireTarget");
    check(read_bytes(directory + QStringLiteral("/fire-missions.json")) == bytes,
          "a pending capture blocks save without changing existing persistent user data");
    assert_error(dialog);
    harness.context = baseline;
    dialog.refresh();
}

void timing_tests(PlanningDialog& dialog, Harness& harness, const QString& directory) {
    choose_tab(dialog, 0);
    auto* assume = child<QCheckBox>(dialog, "assumeFlightModel");
    auto* speed = child<QDoubleSpinBox>(dialog, "assumedSpeed");
    auto* gravity = child<QDoubleSpinBox>(dialog, "assumedGravity");
    harness.context.base = wardogs::Point{0, 0};
    harness.context.target = wardogs::Point{3, 0};
    dialog.refresh();
    check(!assume->isChecked() && !speed->isEnabled() && !gravity->isEnabled() && time_unknown(dialog),
          "L81 starts with unknown flight time and disabled, explicitly opt-in physical assumptions");
    assume->click();
    settle();
    check(assume->isChecked() && speed->isEnabled() && gravity->isEnabled() && time_unknown(dialog),
          "enabling L81 assumptions does not invent a muzzle speed or known flight time");
    speed->setValue(100);
    settle();
    check(!time_unknown(dialog) && child<QLabel>(dialog, "flightTimeResult")->text().contains(
              translated("оценка предположенной модели")),
          "a user-provided L81 speed enables a visibly labelled assumed-model time");
    assume->click();
    settle();
    check(time_unknown(dialog), "turning assumptions off removes the model flight time");
    harness.context.weapon = wardogs::AnalysisWeapon::sph2;
    harness.context.target = wardogs::Point{20, 0};
    dialog.refresh();
    check(time_unknown(dialog), "SPH-2's retained spatial curve does not invent flight seconds by default");
    assume->click();
    settle();
    check(!time_unknown(dialog) && !speed->isEnabled() && gravity->isEnabled(),
          "SPH-2 seconds require explicit gravity assumption while L81 speed remains inapplicable");
    assume->click();
    harness.context.weapon = wardogs::AnalysisWeapon::l81;
    harness.context.target = wardogs::Point{2, 0};
    dialog.refresh();
    choose_tab(dialog, 2);
    auto* version = child<QLineEdit>(dialog, "flightGameVersion");
    auto* source = child<QLineEdit>(dialog, "flightMeasurementSource");
    auto* seconds = child<QDoubleSpinBox>(dialog, "measuredFlightSeconds");
    auto* uncertainty = child<QDoubleSpinBox>(dialog, "flightUncertainty");
    auto* table = child<QTableWidget>(dialog, "flightMeasurements");
    const QString path = directory + QStringLiteral("/flight-profiles.json");
    click(dialog, "recordFlightTime");
    check(!QFile::exists(path), "missing source/version prevents a timing record from being written");
    assert_error(dialog);
    version->setText(QStringLiteral("fixture-v1"));
    source->setText(QStringLiteral("Video fixture, own observation"));
    seconds->setValue(10);
    uncertainty->setValue(11);
    click(dialog, "recordFlightTime");
    check(!QFile::exists(path), "invalid uncertainty is rejected without creating a record");
    assert_error(dialog);
    uncertainty->setValue(.5);
    click(dialog, "recordFlightTime");
    require(table->rowCount() == 1, "the real timing button creates a sourced measurement row");
    auto document = QJsonDocument::fromJson(read_bytes(path));
    auto values = document.object().value(QStringLiteral("observations")).toArray();
    check(values.size() == 1 && values[0].toObject().value(QStringLiteral("game_version")).toString() ==
              QStringLiteral("fixture-v1") && values[0].toObject().value(QStringLiteral("source")).toString() ==
              QStringLiteral("Video fixture, own observation") &&
              values[0].toObject().value(QStringLiteral("height_delta_m")).toDouble() == 0,
          "the persistent timing record retains source, game/profile version and the measured height context");
    harness.context.target = wardogs::Point{4, 0};
    dialog.refresh();
    seconds->setValue(20);
    uncertainty->setValue(1.5);
    click(dialog, "recordFlightTime");
    require(table->rowCount() == 2, "a second distance adds an independent measured timing sample");
    harness.context.target = wardogs::Point{3, 0};
    dialog.refresh();
    const auto result = child<QLabel>(dialog, "flightTimeResult")->text();
    check(result.contains(QStringLiteral("15.000")) && result.contains(QStringLiteral("fixture-v1")) &&
          result.contains(QStringLiteral("Video fixture, own observation")) &&
          result.contains(translated("интерполяция собственных замеров")),
          "the actual analysis label interpolates measured times and identifies their provenance");
    for (double x : {1.4, 6.8}) {
        harness.context.target = wardogs::Point{x, 0};
        dialog.refresh();
        check(time_unknown(dialog), "timing observations are never extrapolated outside their measured coverage");
    }
    harness.context.target = wardogs::Point{3, 0};
    version->setText(QStringLiteral("fixture-v2"));
    dialog.refresh();
    check(time_unknown(dialog), "a different game/profile version cannot reuse old measured flight time");
    version->setText(QStringLiteral("fixture-v1"));
    harness.context.map = wardogs::GameMap::bakurani;
    harness.context.terrain = [](wardogs::Point p) -> std::optional<double> { return p.x * 10; };
    dialog.refresh();
    check(time_unknown(dialog), "a mismatched endpoint height delta cannot reuse level-terrain observations");
    harness.context.terrain = {};
    dialog.refresh();
    check(time_unknown(dialog) && !child<QPushButton>(dialog, "recordFlightTime")->isEnabled(),
          "a named map without terrain has unknown heights and cannot record a misleading timing context");
    harness.context.map = wardogs::GameMap::training;
    dialog.refresh();
    table->selectRow(0);
    click(dialog, "deleteFlightTime");
    check(table->rowCount() == 1 && QJsonDocument::fromJson(read_bytes(path)).object().value(
              QStringLiteral("observations")).toArray().size() == 1,
          "the real delete timing button persists deletion of only the selected measurement");
    QLockFile held(path + QStringLiteral(".lock"));
    require(held.tryLock(), "an independent timing writer holds the real persistent-store lock");
    const auto before = read_bytes(path);
    click(dialog, "recordFlightTime");
    check(read_bytes(path) == before, "lock contention is an honest UI error and preserves existing timing bytes");
    assert_error(dialog);
    held.unlock();
    assume->setChecked(true);
    dialog.refresh();
}

void terrain_render_tests(PlanningDialog& dialog, Harness& harness, const QString& language) {
    choose_tab(dialog, 0);
    auto* assume = child<QCheckBox>(dialog, "assumeFlightModel");
    assume->setChecked(false);
    harness.context.map = wardogs::GameMap::bakurani;
    harness.context.weapon = wardogs::AnalysisWeapon::l81;
    harness.context.base = wardogs::Point{0, 0};
    harness.context.target = wardogs::Point{3, 0};
    harness.context.terrain = [](wardogs::Point) -> std::optional<double> { return std::nullopt; };
    dialog.refresh();
    auto* graph = child<QWidget>(dialog, "terrainProfile");
    make_visible(dialog, graph);
    const auto unknown = graph->grab().toImage();
    check(!has_colour(unknown, qRgb(113, 190, 164)) && !has_colour(unknown, qRgb(239, 183, 110)),
          "the real graph draws neither a fabricated flat surface nor a flight curve when both are unknown");
    screenshot(dialog, language + QStringLiteral("-terrain-unknown"), graph);
    if (wardogs::i18n::language() == wardogs::UiLanguage::english) inspect_english(dialog);
    harness.context.weapon = wardogs::AnalysisWeapon::sph2;
    harness.context.target = wardogs::Point{20, 0};
    harness.context.terrain = [](wardogs::Point p) -> std::optional<double> {
        return p.x > 7 && p.x < 13 ? 10000.0 : 0.0;
    };
    dialog.refresh();
    check(child<QLabel>(dialog, "terrainClearance")->text().contains(
              translated("Модель пересекает землю примерно на %1 м.").section(QStringLiteral("%1"), 0, 0)),
          "a sampled terrain obstruction is reported by the actual graph's clearance label");
    make_visible(dialog, graph);
    const auto blocked = graph->grab().toImage();
    check(has_colour(blocked, qRgb(113, 190, 164)) && has_colour(blocked, qRgb(239, 183, 110)),
          "the blocked graph actually paints the terrain and separate estimated flight curve");
    screenshot(dialog, language + QStringLiteral("-terrain-blocked"), graph);
    if (wardogs::i18n::language() == wardogs::UiLanguage::english) inspect_english(dialog);
    harness.context.terrain = [](wardogs::Point p) -> std::optional<double> {
        if (p.x > 7 && p.x < 13) return std::nullopt;
        return 0.0;
    };
    dialog.refresh();
    check(child<QLabel>(dialog, "terrainClearance")->text().contains(
              translated("Проверка неполная: отсутствуют высоты в %1 точках. Свободный полёт не подтверждён.")
                  .section(QStringLiteral("%1"), 0, 0)),
          "terrain gaps produce an explicit incomplete verdict instead of a clear-flight promise");
    screenshot(dialog, language + QStringLiteral("-terrain-incomplete"), graph);
    if (wardogs::i18n::language() == wardogs::UiLanguage::english) inspect_english(dialog);
    harness.context.terrain = {};
    harness.context.map = wardogs::GameMap::training;
    harness.context.weapon = wardogs::AnalysisWeapon::l81;
    harness.context.target = wardogs::Point{3, 0};
    dialog.refresh();
}

void corruption_tests(Harness& harness, const QString& language) {
    QTemporaryDir storage(QDir::tempPath() + QStringLiteral("/wardogs-planning-corrupt-XXXXXX"));
    require(storage.isValid(), "corruption tests have an isolated persistent directory");
    storage.setAutoRemove(false);
    fixtures.append(storage.path());
    const QString missions = storage.path() + QStringLiteral("/fire-missions.json");
    const QString observations = storage.path() + QStringLiteral("/flight-profiles.json");
    const QByteArray damaged("{ malformed retained user data\n");
    write_bytes(missions, damaged);
    write_bytes(observations, damaged);
    PlanningDialog dialog(harness.provider(), harness.apply(), nullptr, native_path(storage.path()));
    dialog.show();
    settle();
    child<QLineEdit>(dialog, "missionName")->setText(QStringLiteral("Must not replace"));
    click(dialog, "saveFireTarget");
    assert_error(dialog);
    check(read_bytes(missions) == damaged, "a real Save button cannot clobber corrupted saved mission JSON");
    child<QLineEdit>(dialog, "flightGameVersion")->setText(QStringLiteral("fixture-v1"));
    child<QLineEdit>(dialog, "flightMeasurementSource")->setText(QStringLiteral("Video fixture"));
    click(dialog, "recordFlightTime");
    assert_error(dialog);
    check(read_bytes(observations) == damaged, "a real Record button cannot clobber corrupted timing JSON");
    write_bytes(observations, QByteArray("{\"schema\":99,\"observations\":[]}\n"));
    const auto future = read_bytes(observations);
    dialog.refresh();
    click(dialog, "recordFlightTime");
    assert_error(dialog);
    check(read_bytes(observations) == future, "an unsupported future timing schema is retained without reset");
    choose_tab(dialog, 2);
    screenshot(dialog, language + QStringLiteral("-corrupt-store"));
    dialog.close();
}

void timing_byte_limit_tests(const QString& language) {
    QTemporaryDir storage(QDir::tempPath() + QStringLiteral("/wardogs-planning-byte-limit-XXXXXX"));
    require(storage.isValid(), "timing byte-limit regression has an isolated persistent directory");
    storage.setAutoRemove(false);
    fixtures.append(storage.path());
    constexpr qsizetype byte_limit = 256 * 1024;
    const QString version(80, QChar(0x6d4b));
    const QString source(200, QChar(0x8bd5));
    QJsonArray observations;
    QByteArray retained;
    QByteArray over_limit;
    for (int i = 0; i < 256; ++i) {
        auto candidate = observations;
        candidate.append(QJsonObject{
            {"id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {"weapon", "l81"}, {"arc", "high"}, {"distance_m", 200 + i},
            {"height_delta_m", 0}, {"seconds", 10}, {"uncertainty_s", .5},
            {"game_version", version}, {"source", source}});
        const auto bytes = QJsonDocument(QJsonObject{{"schema", 1}, {"observations", candidate}}).toJson();
        if (bytes.size() > byte_limit) {
            over_limit = bytes;
            break;
        }
        observations = std::move(candidate);
        retained = bytes;
    }
    require(!retained.isEmpty() && retained.size() <= byte_limit && !over_limit.isEmpty() &&
            observations.size() < 256, "valid multibyte observations reach the byte limit before the entry limit");
    require(version.toUtf8().size() == 240 && source.toUtf8().size() == 600,
            "the fixture tests real UTF-8 bytes with valid 80/200-character field lengths");
    const QString path = storage.path() + QStringLiteral("/flight-profiles.json");
    write_bytes(path, retained);
    Harness harness;
    harness.context.target = wardogs::Point{6.5, 0}; // Unique 650 m, inside the retained L81 table.
    {
        PlanningDialog dialog(harness.provider(), harness.apply(), nullptr, native_path(storage.path()));
        dialog.show();
        settle();
        choose_tab(dialog, 2);
        auto* table = child<QTableWidget>(dialog, "flightMeasurements");
        require(table->rowCount() == observations.size(), "the real dialog reads the valid near-limit timing file");
        child<QLineEdit>(dialog, "flightGameVersion")->setText(version);
        child<QLineEdit>(dialog, "flightMeasurementSource")->setText(source);
        child<QDoubleSpinBox>(dialog, "measuredFlightSeconds")->setValue(10);
        child<QDoubleSpinBox>(dialog, "flightUncertainty")->setValue(.5);
        dialog.refresh();
        require(child<QPushButton>(dialog, "recordFlightTime")->isEnabled(),
                "the byte-limit regression uses an enabled real Record button with a valid firing solution");
        click(dialog, "recordFlightTime");
        assert_error(dialog);
        check(child<QLabel>(dialog, "planningStatus")->text() ==
                  translated("Файл измерений времени полёта слишком большой."),
              "the actual Record button reports the byte limit rather than a count or input-validation error");
        check(read_bytes(path) == retained && table->rowCount() == observations.size(),
              "a would-be oversized timing write preserves all previous user bytes and rows");
        screenshot(dialog, language + QStringLiteral("-timing-byte-limit"));
        dialog.close();
    }
    {
        PlanningDialog reopened(harness.provider(), harness.apply(), nullptr, native_path(storage.path()));
        reopened.show();
        settle();
        check(child<QTableWidget>(reopened, "flightMeasurements")->rowCount() == observations.size() &&
              read_bytes(path) == retained,
              "after rejected oversized save the real reader can still reopen all original observations");
        reopened.close();
    }
    write_bytes(QDir(evidence_directory).filePath(language + QStringLiteral("-timing-byte-limit-preserved.json")),
                read_bytes(path));
}

void language_suite(wardogs::UiLanguage language, const QString& code) {
    wardogs::i18n::set_language(language);
    QTemporaryDir storage(QDir::tempPath() + QStringLiteral("/wardogs-planning-ui-XXXXXX"));
    require(storage.isValid(), "each language suite gets independent temporary persistent files");
    storage.setAutoRemove(false);
    fixtures.append(storage.path());
    Harness harness;
    {
        PlanningDialog dialog(harness.provider(), harness.apply(), nullptr, native_path(storage.path()));
        dialog.show();
        settle();
        require(dialog.isVisible(), "the CI runner displays the real planning dialog");
        check(QApplication::platformName() == QStringLiteral("windows"),
              "planning acceptance uses native Qt Windows, never the offscreen backend");
        require(child<QTabWidget>(dialog, "planningTabs")->count() == 4,
                "the real planning dialog exposes all four promised tabs");
        check(child<QLabel>(dialog, "planningCoordinates")->isVisible() &&
              !child<QLabel>(dialog, "planningCoordinates")->text().isEmpty(),
              "accepted coordinates and weapon context are visible to the user");
        correction_and_guard_tests(dialog, harness);
        if (language == wardogs::UiLanguage::english) inspect_english(dialog);
        mission_tests(dialog, harness, storage.path());
        if (language == wardogs::UiLanguage::english) inspect_english(dialog);
        timing_tests(dialog, harness, storage.path());
        if (language == wardogs::UiLanguage::english) inspect_english(dialog);
        for (int tab = 0; tab < 4; ++tab) {
            choose_tab(dialog, tab);
            if (language == wardogs::UiLanguage::english) inspect_english(dialog);
            QWidget* focus{};
            if (tab == 0) focus = child<QWidget>(dialog, "terrainProfile");
            if (tab == 1) focus = child<QListWidget>(dialog, "savedFireMissions");
            if (tab == 2) focus = child<QTableWidget>(dialog, "flightMeasurements");
            if (tab == 3) focus = child<QLabel>(dialog, "weaponProfileInfo");
            screenshot(dialog, code + QStringLiteral("-tab-%1").arg(tab + 1), focus);
            inspect_page_contrast(dialog, code + QStringLiteral("-tab-%1").arg(tab + 1));
            for (auto* button : dialog.findChildren<QPushButton*>()) {
                if (!button->isVisible() || button->visibleRegion().isEmpty()) continue;
                check(button->fontMetrics().horizontalAdvance(button->text()) + 12 <= button->width(),
                      "visible planning action text fits its real button width");
            }
        }
        dialog.close();
    }
    const auto before_reopen = harness.applied.size();
    {
        PlanningDialog reopened(harness.provider(), harness.apply(), nullptr, native_path(storage.path()));
        reopened.show();
        settle();
        check(harness.applied.size() == before_reopen,
              "reopening persistent planning data never restores coordinates automatically");
        check(child<QListWidget>(reopened, "savedFireMissions")->count() == 1 &&
              child<QTableWidget>(reopened, "flightMeasurements")->rowCount() == 1,
              "reopened real UI loads saved missions and retained timing rows");
        check(child<QCheckBox>(reopened, "assumeFlightModel")->isChecked() &&
              child<QDoubleSpinBox>(reopened, "assumedSpeed")->value() == 100 &&
              child<QLineEdit>(reopened, "flightGameVersion")->text() == QStringLiteral("fixture-v1") &&
              child<QLineEdit>(reopened, "flightMeasurementSource")->text() ==
                  QStringLiteral("Video fixture, own observation"),
              "explicit model choices, source and game/profile version survive closing and reopening");
        if (language == wardogs::UiLanguage::english) inspect_english(reopened);
        terrain_render_tests(reopened, harness, code);
        reopened.close();
    }
    corruption_tests(harness, code);
    timing_byte_limit_tests(code);
}

} // namespace

int main(int argc, char** argv) {
    // This guard is deliberately before QApplication and any fixture/file work.
    // The owner's Windows PC must never start GUI acceptance by running CTest.
    if (qgetenv("GITHUB_ACTIONS") != QByteArray("true")) {
        std::cout << "SKIP: planning dialog acceptance runs only on the GitHub Windows runner.\n";
        return 77;
    }
    qputenv("QT_QPA_PLATFORM", "windows");
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    application.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QFont font(QStringLiteral("Segoe UI"));
    font.setPixelSize(13);
    application.setFont(font);
    QTemporaryDir fallback;
    evidence_directory = fallback.path();
    const auto arguments = application.arguments();
    const int evidence_argument = arguments.indexOf(QStringLiteral("--evidence-dir"));
    if (evidence_argument >= 0 && evidence_argument + 1 < arguments.size())
        evidence_directory = arguments[evidence_argument + 1];
    try {
        require(QDir::isAbsolutePath(evidence_directory) && QDir().mkpath(evidence_directory),
                "planning UI evidence uses an accessible absolute directory");
        fallback.setAutoRemove(false);
        const auto raw_font = QRawFont::fromFont(application.font());
        check(raw_font.isValid() && raw_font.supportsCharacter(0x0041U) &&
              raw_font.supportsCharacter(0x0414U), "native screenshot fonts provide real Latin and Cyrillic glyphs");
        language_suite(wardogs::UiLanguage::russian, QStringLiteral("ru"));
        language_suite(wardogs::UiLanguage::english, QStringLiteral("en"));
    } catch (const std::exception& error) {
        ++failures;
        std::cerr << "UNEXPECTED: " << error.what() << '\n';
    }
    const QJsonObject receipt{{"assertions", assertions}, {"failures", failures},
        {"platform", QApplication::platformName()}, {"screenshots", screenshots},
        {"surface_contrast", surface_contrast},
        {"isolated_storage_directories", fixtures},
        {"boundary", "Native planning dialog and temporary local data only; no game capture, global hotkeys or long-term gameplay proof."}};
    QFile output(QDir(evidence_directory).filePath(QStringLiteral("planning-ui-receipt.json")));
    const auto bytes = QJsonDocument(receipt).toJson();
    check(output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size(),
          "the planning UI acceptance receipt is retained");
    std::cout << assertions << " planning UI assertions; " << failures << " failure(s)\n";
    std::cout << "Evidence: " << evidence_directory.toStdString() << '\n';
    return failures ? 1 : 0;
}
