#include "game_helpers_dialog.hpp"
#include "localization.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QEventLoop>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>

#include <iostream>
#include <stdexcept>

namespace {
int checks{};
int failures{};

void check(bool value, const char* message) {
    ++checks;
    if (value) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

template<class T>
T* child(GameHelpersDialog& dialog, const char* name) {
    auto* result = dialog.findChild<T*>(QString::fromLatin1(name));
    if (!result) throw std::runtime_error(name);
    return result;
}

void save(GameHelpersDialog& dialog) {
    child<QPushButton>(dialog, "helperSaveButton")->click();
    QApplication::sendPostedEvents();
    QApplication::processEvents(QEventLoop::AllEvents, 20);
}

bool cyrillic(const QString& text) {
    static const QRegularExpression expression(QStringLiteral("[\\x{0400}-\\x{052f}]"));
    return expression.match(text).hasMatch();
}

void defaults_and_editing() {
    wardogs::AppSettings settings;
    GameHelpersDialog dialog(settings);
    check(dialog.preferences() == settings.game_helpers, "opening dialog preserves all stored helper preferences");
    check(!dialog.preferences().build.enabled && !dialog.preferences().team.enabled && !dialog.preferences().reload.enabled,
          "all helper shortcuts are opt-in on a new profile");
    auto* tabs = child<QTabWidget>(dialog, "gameHelpersTabs");
    check(tabs->count() == 3 && tabs->tabText(0) == QStringLiteral("Build helper") &&
          tabs->tabText(1) == QStringLiteral("Team Piker") &&
          tabs->tabText(2) == QStringLiteral("SPH-2 · перезарядка"), "three helpers share one tabbed dialog");
    check(child<QCheckBox>(dialog, "reloadAutomaticRegion")->isChecked() &&
          !child<QKeySequenceEdit>(dialog, "reloadHelperHotkey")->isEnabled() &&
          !child<QPushButton>(dialog, "reloadSelectRegionButton")->isEnabled(),
          "reload defaults to automatic search without reserving a shortcut or offering an unavailable selector");
    check(child<QLabel>(dialog, "buildCycleSummary")->text().contains(QStringLiteral("547")),
          "build default cycle is 514 held plus 33 released");
    check(child<QLabel>(dialog, "teamCycleSummary")->text().contains(QStringLiteral("900–1000")),
          "team default interval includes 100 to 200 ms random addition");
    check(!child<QKeySequenceEdit>(dialog, "buildHelperHotkey")->isEnabled(),
          "disabled helper does not offer an active shortcut");
    check(!child<QSpinBox>(dialog, "buildHelperRandomMin")->isEnabled() &&
           child<QSpinBox>(dialog, "teamPikerRandomMin")->isEnabled(),
          "random range controls reflect each independent toggle");
    check(child<QSpinBox>(dialog, "buildHelperRandomMin")->isHidden() &&
          child<QSpinBox>(dialog, "buildHelperRandomMax")->isHidden() &&
          !child<QSpinBox>(dialog, "teamPikerRandomMin")->isHidden() &&
          !child<QSpinBox>(dialog, "teamPikerRandomMax")->isHidden(),
          "only enabled randomization shows its range rows");
    child<QCheckBox>(dialog, "buildHelperEnabled")->setChecked(true);
    child<QKeySequenceEdit>(dialog, "buildHelperHotkey")->setKeySequence(QKeySequence(QStringLiteral("Ctrl+F8")));
    child<QSpinBox>(dialog, "buildHelperHold")->setValue(710);
    child<QSpinBox>(dialog, "buildHelperPause")->setValue(120);
    child<QCheckBox>(dialog, "buildHelperRandom")->setChecked(true);
    check(!child<QSpinBox>(dialog, "buildHelperRandomMin")->isHidden() &&
          !child<QSpinBox>(dialog, "buildHelperRandomMax")->isHidden(),
          "enabling build randomization reveals both range rows");
    child<QSpinBox>(dialog, "buildHelperRandomMin")->setValue(10);
    child<QSpinBox>(dialog, "buildHelperRandomMax")->setValue(40);
    const auto edited = dialog.preferences();
    check(edited.build.enabled && edited.build.hotkey == L"Ctrl+F8" && edited.build.hold_ms == 710 &&
          edited.build.pause_ms == 120 && edited.build.random_enabled &&
          edited.build.random_min_ms == 10 && edited.build.random_max_ms == 40,
          "all build controls produce exact typed preferences");
    check(edited.team == settings.game_helpers.team, "build edits preserve the team configuration");
    check(child<QLabel>(dialog, "buildCycleSummary")->text().contains(QStringLiteral("840–870")),
          "cycle summary includes random pause but does not alter hold");
    child<QCheckBox>(dialog, "teamPikerRandom")->setChecked(false);
    check(child<QLabel>(dialog, "teamCycleSummary")->text().contains(QStringLiteral("800–800")),
          "disabling team randomization returns exact base interval");
    check(child<QSpinBox>(dialog, "teamPikerRandomMin")->isHidden() &&
          child<QSpinBox>(dialog, "teamPikerRandomMax")->isHidden() &&
          dialog.preferences().team.random_min_ms == 100 &&
          dialog.preferences().team.random_max_ms == 200,
          "hiding team random ranges retains their saved values");
    check(child<QKeySequenceEdit>(dialog, "buildHelperHotkey")->maximumSequenceLength() == 1,
          "shortcut editor records one chord only");
    check(child<QKeySequenceEdit>(dialog, "buildHelperHotkey")->finishingKeyCombinations()
          .contains(QKeyCombination(Qt::Key_Tab)), "Tab finishes key capture and supports navigation");
    save(dialog);
    check(dialog.result() == QDialog::Accepted, "valid helper configuration is accepted");
}

void validation() {
    wardogs::AppSettings settings;
    settings.game_helpers.build.enabled = true;
    settings.game_helpers.team.enabled = true;
    for (const auto* editor : {"reloadHelperHotkey", "buildHelperHotkey", "teamPikerHotkey"}) {
        for (const auto& direction : {QStringLiteral("Up"), QStringLiteral("Down"),
                                     QStringLiteral("Left"), QStringLiteral("Right")}) {
            GameHelpersDialog dialog(settings);
            child<QCheckBox>(dialog, "reloadHelperEnabled")->setChecked(true);
            child<QKeySequenceEdit>(dialog, editor)->setKeySequence(QKeySequence(direction));
            save(dialog);
            check(dialog.result() != QDialog::Accepted &&
                  !child<QLabel>(dialog, "helperValidationError")->isHidden(),
                  "reload cannot synthesize another enabled helper's plain-arrow shortcut");
        }
    }
    {
        auto artillery_conflict = settings;
        artillery_conflict.base_hotkey = L"Up";
        GameHelpersDialog dialog(artillery_conflict);
        child<QCheckBox>(dialog, "reloadHelperEnabled")->setChecked(true);
        save(dialog);
        check(dialog.result() != QDialog::Accepted, "reload generated input cannot trigger artillery shortcuts");
    }
    {
        GameHelpersDialog dialog(settings);
        child<QCheckBox>(dialog, "reloadHelperEnabled")->setChecked(true);
        child<QKeySequenceEdit>(dialog, "buildHelperHotkey")->setKeySequence(QKeySequence(QStringLiteral("Ctrl+Up")));
        save(dialog);
        check(dialog.result() == QDialog::Accepted, "modified arrow hotkeys remain independent of generated reload input");
    }
    {
        GameHelpersDialog dialog(settings);
        child<QCheckBox>(dialog, "reloadHelperEnabled")->setChecked(true);
        child<QCheckBox>(dialog, "buildHelperEnabled")->setChecked(false);
        child<QKeySequenceEdit>(dialog, "buildHelperHotkey")->setKeySequence(QKeySequence(QStringLiteral("Up")));
        save(dialog);
        check(dialog.result() == QDialog::Accepted, "disabled stored arrow assignment is not registered by the editor");
    }
    {
        GameHelpersDialog dialog(settings);
        child<QKeySequenceEdit>(dialog, "buildHelperHotkey")->setKeySequence(QKeySequence(QStringLiteral("Up")));
        save(dialog);
        check(dialog.result() == QDialog::Accepted, "without reload existing arrow hotkeys remain compatible");
    }
    {
        GameHelpersDialog dialog(settings);
        child<QCheckBox>(dialog, "reloadHelperEnabled")->setChecked(true);
        child<QKeySequenceEdit>(dialog, "reloadHelperHotkey")->setKeySequence(QKeySequence(QStringLiteral("F6")));
        save(dialog);
        check(dialog.result() != QDialog::Accepted, "reload cannot steal another enabled helper shortcut");
    }
    {
        GameHelpersDialog dialog(settings);
        child<QCheckBox>(dialog, "reloadAutomaticRegion")->setChecked(false);
        save(dialog);
        check(dialog.result() != QDialog::Accepted, "manual reload search requires a selected nonempty region");
    }
    {
        GameHelpersDialog dialog(settings);
        child<QKeySequenceEdit>(dialog, "teamPikerHotkey")->setKeySequence(QKeySequence(QStringLiteral("F6")));
        save(dialog);
        check(dialog.result() != QDialog::Accepted && !child<QLabel>(dialog, "helperValidationError")->isHidden(),
              "duplicate enabled helper shortcuts show inline error without accepting");
    }
    {
        GameHelpersDialog dialog(settings);
        child<QKeySequenceEdit>(dialog, "buildHelperHotkey")->setKeySequence(QKeySequence(QStringLiteral("Alt+X")));
        save(dialog);
        check(dialog.result() != QDialog::Accepted, "helper cannot steal artillery shortcut");
    }
    for (const auto& allowed : {QStringLiteral("F12"), QStringLiteral("Esc"), QStringLiteral("Ctrl+Alt+F4")}) {
        GameHelpersDialog dialog(settings);
        child<QKeySequenceEdit>(dialog, "buildHelperHotkey")->setKeySequence(QKeySequence(allowed));
        save(dialog);
        check(dialog.result() == QDialog::Accepted, "helper editor accepts F12, Escape and multi-modifier keyboard chords");
    }
    for (const auto& forbidden : {QStringLiteral("Meta+F6")}) {
        GameHelpersDialog dialog(settings);
        child<QKeySequenceEdit>(dialog, "buildHelperHotkey")->setKeySequence(QKeySequence(forbidden));
        save(dialog);
        check(dialog.result() != QDialog::Accepted, "reserved shortcut is rejected before OS registration");
    }
    {
        GameHelpersDialog dialog(settings);
        child<QSpinBox>(dialog, "teamPikerRandomMin")->setValue(500);
        child<QSpinBox>(dialog, "teamPikerRandomMax")->setValue(100);
        save(dialog);
        check(dialog.result() != QDialog::Accepted, "inverted random range is not silently corrected");
    }
    {
        GameHelpersDialog dialog(settings);
        child<QKeySequenceEdit>(dialog, "buildHelperHotkey")->clear();
        save(dialog);
        check(dialog.result() != QDialog::Accepted, "empty shortcut is rejected inline");
        dialog.show_error(QStringLiteral("OS registration failed"));
        check(child<QLabel>(dialog, "helperValidationError")->text() == QStringLiteral("OS registration failed"),
              "parent can display save or OS registration failure without losing editor state");
    }
    {
        GameHelpersDialog dialog(settings);
        child<QSpinBox>(dialog, "buildHelperHold")->setValue(900);
        child<QDialogButtonBox>(dialog, "helperDialogButtons")->button(QDialogButtonBox::Cancel)->click();
        check(dialog.result() == QDialog::Rejected && settings.game_helpers.build.hold_ms == 514,
              "cancel never mutates the caller settings");
    }
}

void reload_timing_and_humanizer() {
    wardogs::AppSettings settings;
    GameHelpersDialog dialog(settings);
    check(child<QSpinBox>(dialog, "reloadHelperHold")->value() == 50 &&
          child<QSpinBox>(dialog, "reloadHelperKeyGap")->value() == 90 &&
          child<QSpinBox>(dialog, "reloadHelperStageGap")->value() == 750,
          "reload timing uses the owner-selected 50/90/750 defaults");
    check(!child<QCheckBox>(dialog, "reloadKeyRandom")->isChecked() &&
          !child<QCheckBox>(dialog, "reloadStageRandom")->isChecked() &&
          child<QSpinBox>(dialog, "reloadKeyRandomMin")->isHidden() &&
          child<QSpinBox>(dialog, "reloadStageRandomMin")->isHidden(),
          "both independent humanizer groups start disabled with their ranges hidden");
    check(child<QSpinBox>(dialog, "reloadHelperHold")->minimum() == 20 &&
          child<QSpinBox>(dialog, "reloadHelperHold")->maximum() == 1000 &&
          child<QSpinBox>(dialog, "reloadHelperKeyGap")->maximum() == 2000 &&
          child<QSpinBox>(dialog, "reloadHelperStageGap")->maximum() == 10000,
          "reload timing controls expose the validated bounded ranges");
    child<QSpinBox>(dialog, "reloadHelperHold")->setValue(70);
    child<QSpinBox>(dialog, "reloadHelperKeyGap")->setValue(140);
    child<QSpinBox>(dialog, "reloadHelperStageGap")->setValue(1500);
    child<QCheckBox>(dialog, "reloadKeyRandom")->setChecked(true);
    child<QSpinBox>(dialog, "reloadKeyRandomMin")->setValue(5);
    child<QSpinBox>(dialog, "reloadKeyRandomMax")->setValue(25);
    child<QCheckBox>(dialog, "reloadStageRandom")->setChecked(true);
    child<QSpinBox>(dialog, "reloadStageRandomMin")->setValue(80);
    child<QSpinBox>(dialog, "reloadStageRandomMax")->setValue(220);
    const auto edited = dialog.preferences();
    check(edited.reload.hold_ms == 70 && edited.reload.key_gap_ms == 140 && edited.reload.stage_gap_ms == 1500 &&
          edited.reload.key_random_enabled && edited.reload.key_random_min_ms == 5 && edited.reload.key_random_max_ms == 25 &&
          edited.reload.stage_random_enabled && edited.reload.stage_random_min_ms == 80 && edited.reload.stage_random_max_ms == 220 &&
          edited.build == settings.game_helpers.build && edited.team == settings.game_helpers.team,
          "reload timing and both random ranges produce exact preferences without changing other helpers");
    const auto summary = child<QLabel>(dialog, "reloadTimingSummary")->text();
    check(summary.contains(QStringLiteral("75–95")) && summary.contains(QStringLiteral("145–165")) &&
          summary.contains(QStringLiteral("1580–1720")),
          "reload timing summary adds independent key and sequence ranges to base intervals");
    child<QCheckBox>(dialog, "reloadKeyRandom")->setChecked(false);
    check(child<QSpinBox>(dialog, "reloadKeyRandomMin")->isHidden() &&
          !child<QSpinBox>(dialog, "reloadStageRandomMin")->isHidden() &&
          dialog.preferences().reload.key_random_min_ms == 5 && dialog.preferences().reload.key_random_max_ms == 25 &&
          child<QLabel>(dialog, "reloadTimingSummary")->text().contains(QStringLiteral("70–70")),
          "disabling key humanizer retains its range and leaves sequence randomization independent");
    save(dialog);
    check(dialog.result() == QDialog::Accepted, "custom reload timing saves through ordinary validation");
    for (const auto* minimum : {"reloadKeyRandomMin", "reloadStageRandomMin"}) {
        GameHelpersDialog invalid(settings);
        child<QSpinBox>(invalid, minimum)->setValue(500);
        save(invalid);
        check(invalid.result() != QDialog::Accepted && !child<QLabel>(invalid, "helperValidationError")->isHidden(),
              "inverted reload humanizer ranges report an inline error without accepting");
    }
    GameHelpersDialog cancelled(settings);
    child<QSpinBox>(cancelled, "reloadHelperStageGap")->setValue(2500);
    child<QCheckBox>(cancelled, "reloadStageRandom")->setChecked(true);
    child<QDialogButtonBox>(cancelled, "helperDialogButtons")->button(QDialogButtonBox::Cancel)->click();
    check(cancelled.result() == QDialog::Rejected && settings.game_helpers.reload.stage_gap_ms == 750 &&
          !settings.game_helpers.reload.stage_random_enabled,
          "cancelling reload timing changes leaves the caller's settings untouched");
}

void one_cycle_reload_diagnostics() {
    wardogs::AppSettings settings;
    const auto before = settings.game_helpers;
    GameHelpersDialog dialog(settings);
    check(!dialog.reload_diagnostics_requested() &&
          !child<QCheckBox>(dialog, "reloadDiagnosticsOnce")->isChecked(),
          "one-cycle diagnostics is opt-in on every dialog opening");
    check(!child<QPushButton>(dialog, "reloadDiagnosticsFolder")->text().isEmpty(),
          "diagnostic files have a discoverable folder action");
    child<QCheckBox>(dialog, "reloadDiagnosticsOnce")->setChecked(true);
    check(dialog.reload_diagnostics_requested() && dialog.preferences() == before,
          "diagnostic selection is independent of saved helper preferences");
    save(dialog);
    check(dialog.result() == QDialog::Accepted && dialog.reload_diagnostics_requested() &&
          settings.game_helpers == before,
          "saving returns a one-cycle request without changing caller preferences or starting a helper");
    GameHelpersDialog cancelled(settings);
    child<QCheckBox>(cancelled, "reloadDiagnosticsOnce")->setChecked(true);
    child<QDialogButtonBox>(cancelled, "helperDialogButtons")->button(QDialogButtonBox::Cancel)->click();
    check(cancelled.result() == QDialog::Rejected && cancelled.preferences() == before &&
          settings.game_helpers == before,
          "cancelling diagnostics leaves saved helper configuration untouched");
    GameHelpersDialog reopened(settings);
    check(!reopened.reload_diagnostics_requested(),
          "diagnostics is not restored from either a saved or cancelled dialog");
}

void reload_region_selection() {
    wardogs::AppSettings settings;
    GameHelpersDialog* opened{};
    int requests{};
    GameHelpersDialog dialog(settings, nullptr, [&](const auto& current)
        -> std::optional<wardogs::ReloadHelperPreferences> {
        ++requests;
        check(opened && opened->windowOpacity() == 0.0 && !opened->isEnabled(),
              "region selector runs while the configuration surface is invisible and disabled");
        auto result = current;
        result.left = 4200; result.top = 3200; result.right = 5800; result.bottom = 4100;
        return result;
    });
    opened = &dialog;
    dialog.show();
    child<QCheckBox>(dialog, "reloadHelperEnabled")->setChecked(true);
    child<QKeySequenceEdit>(dialog, "reloadHelperHotkey")->setKeySequence(QKeySequence(QStringLiteral("Ctrl+F8")));
    child<QSpinBox>(dialog, "reloadHelperHold")->setValue(75);
    child<QSpinBox>(dialog, "reloadHelperStageGap")->setValue(1600);
    child<QCheckBox>(dialog, "reloadKeyRandom")->setChecked(true);
    child<QPushButton>(dialog, "reloadSelectRegionButton")->click();
    const auto selected = dialog.preferences().reload;
    check(requests == 1 && dialog.isVisible() && selected.enabled && selected.hotkey == L"Ctrl+F8" &&
          !selected.automatic_region && selected.left == 4200 && selected.top == 3200 &&
          selected.right == 5800 && selected.bottom == 4100 && selected.hold_ms == 75 &&
          selected.stage_gap_ms == 1600 && selected.key_random_enabled,
          "successful selection preserves shortcut edits and switches to the selected normalized manual region");
    child<QCheckBox>(dialog, "reloadAutomaticRegion")->setChecked(true);
    check(dialog.preferences().reload.left == 4200 && dialog.preferences().reload.right == 5800,
          "automatic search preserves the optional manual fallback rectangle");
    save(dialog);
    check(dialog.result() == QDialog::Accepted, "reload settings save through normal dialog validation");

    settings.game_helpers.reload = selected;
    GameHelpersDialog cancelled(settings, nullptr, [](const auto&)
        -> std::optional<wardogs::ReloadHelperPreferences> { return std::nullopt; });
    const auto before = cancelled.preferences();
    child<QPushButton>(cancelled, "reloadSelectRegionButton")->click();
    check(cancelled.preferences() == before, "cancelled region selection preserves the existing rectangle and mode");

    GameHelpersDialog invalid(settings, nullptr, [](const auto&)
        -> std::optional<wardogs::ReloadHelperPreferences> { return wardogs::ReloadHelperPreferences{}; });
    child<QPushButton>(invalid, "reloadSelectRegionButton")->click();
    check(invalid.preferences() == settings.game_helpers &&
          !child<QLabel>(invalid, "helperValidationError")->isHidden(),
          "invalid selection is reported without replacing the existing region");

    QWidget owner;
    owner.show();
    GameHelpersDialog modal(settings, &owner, [&](const auto& current)
        -> std::optional<wardogs::ReloadHelperPreferences> {
        owner.hide();
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        owner.show();
        return current;
    });
    QTimer::singleShot(0, &modal, [&] {
        child<QPushButton>(modal, "reloadSelectRegionButton")->click();
        QTimer::singleShot(0, &modal, [&] { save(modal); });
    });
    QTimer::singleShot(1000, &modal, &QDialog::reject);
    check(modal.exec() == QDialog::Accepted && modal.preferences() == settings.game_helpers,
          "manual selection and temporary parent hiding keep modal exec alive until Save");
}

void translation_and_layout(const QString& evidence) {
    wardogs::i18n::set_language(wardogs::UiLanguage::russian);
    wardogs::AppSettings settings;
    GameHelpersDialog dialog(settings);
    dialog.resize(700, 720);
    dialog.show();
    QApplication::processEvents(QEventLoop::AllEvents, 40);
    check(dialog.width() <= 720 && dialog.height() <= 780, "dialog fits a conventional desktop height");
    for (auto* scroll : dialog.findChildren<QScrollArea*>())
        check(scroll->widgetResizable() && scroll->horizontalScrollBarPolicy() == Qt::ScrollBarAlwaysOff,
              "tab page can scroll vertically without horizontal navigation");
    auto* tabs = child<QTabWidget>(dialog, "gameHelpersTabs");
    const auto capture = [&](const QString& language) {
        if (evidence.isEmpty()) return;
        QDir().mkpath(evidence);
        for (int index = 0; index < tabs->count(); ++index) {
            tabs->setCurrentIndex(index);
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            check(dialog.grab().save(evidence + QStringLiteral("/helpers-%1-%2.png").arg(language).arg(index)),
                  "dialog snapshot saved");
        }
    };
    capture(QStringLiteral("ru"));
    child<QCheckBox>(dialog, "reloadDiagnosticsOnce")->setChecked(true);
    wardogs::i18n::set_language(wardogs::UiLanguage::english);
    QApplication::processEvents(QEventLoop::AllEvents, 40);
    check(dialog.windowTitle() == QStringLiteral("Helpers · WARDOGS"), "open dialog translates its title live");
    check(tabs->tabText(2) == QStringLiteral("SPH-2 · reload"), "reload tab title translates in an open dialog");
    check(child<QCheckBox>(dialog, "reloadDiagnosticsOnce")->text() ==
          QStringLiteral("Record one cycle for diagnostics") && dialog.reload_diagnostics_requested(),
          "diagnostic opt-in translates live while preserving the current selection");
    check(child<QPushButton>(dialog, "reloadDiagnosticsFolder")->text() ==
          QStringLiteral("Open diagnostics folder"), "diagnostic folder action translates live");
    for (auto* label : dialog.findChildren<QLabel*>()) {
        if (cyrillic(label->text()))
            std::cerr << "UNTRANSLATED LABEL: " << label->objectName().toStdString()
                      << ": " << label->text().toUtf8().constData() << '\n';
        check(!cyrillic(label->text()), "English helper dialog contains no untranslated Russian label");
    }
    for (auto* box : dialog.findChildren<QCheckBox*>())
        check(!cyrillic(box->text()), "English helper checkboxes are translated");
    check(child<QSpinBox>(dialog, "buildHelperHold")->suffix() == QStringLiteral(" ms"),
          "milliseconds suffix follows the chosen language");
    check(child<QLabel>(dialog, "buildCycleSummary")->text() ==
          QStringLiteral("LMB 514 ms → pause 33–33 ms\nFull cycle: 547–547 ms"),
          "live language change renders the build summary through the application catalog");
    check(child<QLabel>(dialog, "teamCycleSummary")->text() ==
          QStringLiteral("Left-click every 900–1000 ms\nThe first click follows the start command."),
          "live language change renders the team summary through the application catalog");
    check(child<QLabel>(dialog, "reloadTimingSummary")->text().startsWith(QStringLiteral("Key: 50–50 ms")),
          "reload humanizer summary follows the selected language");
    check(dialog.preferences() == settings.game_helpers, "live translation preserves user preferences");
    capture(QStringLiteral("en"));
    child<QSpinBox>(dialog, "buildHelperHold")->setValue(700);
    check(child<QLabel>(dialog, "buildCycleSummary")->text().startsWith(QStringLiteral("LMB 700 ms")),
          "editing a delay keeps dynamic summaries in English");
    {
        GameHelpersDialog english(settings);
        check(english.windowTitle() == QStringLiteral("Helpers · WARDOGS") &&
              child<QLabel>(english, "teamCycleSummary")->text().startsWith(QStringLiteral("Left-click")),
              "opening a new dialog directly in English translates title and summaries");
    }
    wardogs::i18n::set_language(wardogs::UiLanguage::russian);
    QApplication::processEvents(QEventLoop::AllEvents, 20);
    check(child<QLabel>(dialog, "buildCycleSummary")->text().startsWith(QStringLiteral("ЛКМ 700 мс")) &&
          child<QSpinBox>(dialog, "buildHelperHold")->suffix() == QStringLiteral(" мс"),
          "switching back to Russian preserves edited values and translates dynamic summaries");
    dialog.close();
}
}  // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    const auto arguments = app.arguments();
    const int evidence_index = arguments.indexOf(QStringLiteral("--evidence-dir"));
    const QString evidence = evidence_index >= 0 && evidence_index + 1 < arguments.size()
        ? arguments.at(evidence_index + 1) : QString{};
    try {
        wardogs::i18n::set_language(wardogs::UiLanguage::russian);
        defaults_and_editing();
        reload_timing_and_humanizer();
        one_cycle_reload_diagnostics();
        validation();
        reload_region_selection();
        translation_and_layout(evidence);
    } catch (const std::exception& error) {
        ++failures;
        std::cerr << "EXCEPTION: " << error.what() << '\n';
    }
    std::cout << checks << " checks; " << failures << " failures\n";
    return failures ? 1 : 0;
}
