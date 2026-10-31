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
    check(!dialog.preferences().build.enabled && !dialog.preferences().team.enabled,
          "both helper shortcuts are opt-in on a new profile");
    auto* tabs = child<QTabWidget>(dialog, "gameHelpersTabs");
    check(tabs->count() == 2 && tabs->tabText(0) == QStringLiteral("Build helper") &&
          tabs->tabText(1) == QStringLiteral("Team Piker"), "two helpers share one tabbed dialog");
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
    for (const auto& forbidden : {QStringLiteral("F12"), QStringLiteral("Meta+F6"), QStringLiteral("Esc")}) {
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
        for (int index = 0; index < 2; ++index) {
            tabs->setCurrentIndex(index);
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            check(dialog.grab().save(evidence + QStringLiteral("/helpers-%1-%2.png").arg(language).arg(index)),
                  "dialog snapshot saved");
        }
    };
    capture(QStringLiteral("ru"));
    wardogs::i18n::set_language(wardogs::UiLanguage::english);
    QApplication::processEvents(QEventLoop::AllEvents, 40);
    check(dialog.windowTitle() == QStringLiteral("Helpers · WARDOGS"), "open dialog translates its title live");
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
        validation();
        translation_and_layout(evidence);
    } catch (const std::exception& error) {
        ++failures;
        std::cerr << "EXCEPTION: " << error.what() << '\n';
    }
    std::cout << checks << " checks; " << failures << " failures\n";
    return failures ? 1 : 0;
}
