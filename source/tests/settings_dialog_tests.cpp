#include "settings_dialog.hpp"
#include "localization.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QEventLoop>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QTabWidget>

#include <iostream>
#include <stdexcept>

namespace {
int checks{}, failures{};

void check(bool value, const char* message) {
    ++checks;
    if (!value) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}

template<class T>
T* child(SettingsDialog& dialog, const char* name) {
    auto* result = dialog.findChild<T*>(QString::fromLatin1(name));
    if (!result) throw std::runtime_error(name);
    return result;
}

void events() {
    QApplication::sendPostedEvents();
    QApplication::processEvents(QEventLoop::AllEvents, 20);
}

void first_shot_policy() {
    wardogs::AppSettings original;
    SettingsDialog dialog(original);
    auto* checkbox = child<QCheckBox>(dialog, "sph2LowHeightModel");
    auto* help = child<QLabel>(dialog, "sph2LowHeightModelHelp");
    auto* group = child<QGroupBox>(dialog, "sph2FirstShotSettings");
    auto* tabs = child<QTabWidget>(dialog, "settingsTabs");
    check(tabs->count() == 4 && tabs->widget(0)->isAncestorOf(checkbox),
          "SPH-2 first-shot policy belongs to the existing General page without adding a tab");
    check(!checkbox->isChecked() && !dialog.settings().sph2_low_height_model,
          "fresh settings show the empirical low table as the default");
    check(checkbox->text() == QStringLiteral("Приближённая высотная поправка\nдля настильной SPH-2") &&
              help->text().contains(QStringLiteral("не подтверждена игровыми измерениями")) &&
              help->text().contains(QStringLiteral("Высоты по-прежнему показываются")) &&
              help->text().contains(QStringLiteral("При смене режима пристрелка сбрасывается")),
          "the option explains the unverified model and continued height display");
    checkbox->setChecked(true);
    check(dialog.settings().sph2_low_height_model && !original.sph2_low_height_model &&
              dialog.settings().game_helpers == original.game_helpers &&
              dialog.settings().impact_hotkey == original.impact_hotkey,
          "editing the policy changes only the staged setting, not caller preferences or helpers");

    dialog.show();
    events();
    wardogs::i18n::set_language(wardogs::UiLanguage::english);
    events();
    check(group->title() == QStringLiteral("SPH-2 first shot") &&
              checkbox->text() == QStringLiteral("Approximate height correction\nfor SPH-2 low trajectory") &&
              help->text().contains(QStringLiteral("has not been validated by in-game measurements")) &&
              help->text().contains(QStringLiteral("Changing this mode resets ranging corrections")) &&
              checkbox->isChecked() && dialog.settings().sph2_low_height_model,
          "live English translation preserves the staged policy and explains its limits");
    wardogs::i18n::set_language(wardogs::UiLanguage::russian);
    events();
    check(checkbox->isChecked() && checkbox->text().startsWith(QStringLiteral("Приближённая")),
          "returning to Russian preserves the selected model");

    auto* buttons = dialog.findChild<QDialogButtonBox*>();
    if (!buttons) throw std::runtime_error("settings buttons");
    buttons->button(QDialogButtonBox::Save)->click();
    events();
    check(dialog.result() == QDialog::Accepted && dialog.settings().sph2_low_height_model,
          "Save accepts the explicitly enabled policy through ordinary validation");

    auto enabled = dialog.settings();
    SettingsDialog cancel(enabled);
    child<QCheckBox>(cancel, "sph2LowHeightModel")->setChecked(false);
    auto* cancel_buttons = cancel.findChild<QDialogButtonBox*>();
    if (!cancel_buttons) throw std::runtime_error("cancel buttons");
    cancel_buttons->button(QDialogButtonBox::Cancel)->click();
    events();
    check(cancel.result() == QDialog::Rejected && enabled.sph2_low_height_model,
          "Cancel preserves the previously accepted caller policy");
    SettingsDialog reopen(enabled);
    check(child<QCheckBox>(reopen, "sph2LowHeightModel")->isChecked(),
          "reopening settings restores the selected model");
    child<QCheckBox>(reopen, "sph2LowHeightModel")->setChecked(false);
    auto* reopen_buttons = reopen.findChild<QDialogButtonBox*>();
    if (!reopen_buttons) throw std::runtime_error("reopen buttons");
    reopen_buttons->button(QDialogButtonBox::Save)->click();
    events();
    check(reopen.result() == QDialog::Accepted && !reopen.settings().sph2_low_height_model,
          "Save can restore empirical low-table mode after enabling the approximation");
}
}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        wardogs::i18n::set_language(wardogs::UiLanguage::russian);
        first_shot_policy();
    } catch (const std::exception& error) {
        ++failures;
        std::cerr << "EXCEPTION: " << error.what() << '\n';
    }
    std::cout << checks << " settings dialog checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
