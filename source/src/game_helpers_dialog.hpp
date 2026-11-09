#pragma once

#include "wardogs/settings.hpp"

#include <QDialog>
#include <functional>
#include <optional>

class QCheckBox;
class QFormLayout;
class QKeySequenceEdit;
class QLabel;
class QSpinBox;
class QPushButton;

// Configuration only: opening this dialog never starts a helper.
class GameHelpersDialog final : public QDialog {
public:
    using ReloadRegionSelector = std::function<std::optional<wardogs::ReloadHelperPreferences>(
        const wardogs::ReloadHelperPreferences&)>;
    explicit GameHelpersDialog(const wardogs::AppSettings& settings,
                               QWidget* parent = nullptr,
                               ReloadRegionSelector select_reload_region = {});
    [[nodiscard]] wardogs::GameHelpersPreferences preferences() const;
    // Read after Accepted, like preferences(); never persisted in settings.
    [[nodiscard]] bool reload_diagnostics_requested() const;
    void show_error(const QString& message);

protected:
    void changeEvent(QEvent* event) override;
    bool nativeEvent(const QByteArray& event_type, void* message,
                     qintptr* result) override;

private:
    void update_summaries();
    void accept_if_valid();
    void select_reload_region();

    wardogs::AppSettings original_settings_;
    ReloadRegionSelector reload_region_selector_;
    wardogs::ReloadHelperPreferences reload_region_;
    QCheckBox* build_enabled_{};
    QKeySequenceEdit* build_hotkey_{};
    QSpinBox* build_hold_{};
    QSpinBox* build_pause_{};
    QCheckBox* build_random_{};
    QSpinBox* build_random_min_{};
    QSpinBox* build_random_max_{};
    QFormLayout* build_timing_form_{};
    QLabel* build_summary_{};
    QCheckBox* team_enabled_{};
    QKeySequenceEdit* team_hotkey_{};
    QSpinBox* team_interval_{};
    QCheckBox* team_random_{};
    QSpinBox* team_random_min_{};
    QSpinBox* team_random_max_{};
    QFormLayout* team_timing_form_{};
    QLabel* team_summary_{};
    QCheckBox* reload_enabled_{};
    QKeySequenceEdit* reload_hotkey_{};
    QCheckBox* reload_automatic_{};
    QPushButton* reload_select_{};
    QLabel* reload_summary_{};
    QSpinBox* reload_hold_{};
    QSpinBox* reload_key_gap_{};
    QSpinBox* reload_stage_gap_{};
    QCheckBox* reload_key_random_{};
    QSpinBox* reload_key_random_min_{};
    QSpinBox* reload_key_random_max_{};
    QCheckBox* reload_stage_random_{};
    QSpinBox* reload_stage_random_min_{};
    QSpinBox* reload_stage_random_max_{};
    QFormLayout* reload_random_form_{};
    QLabel* reload_timing_summary_{};
    QCheckBox* reload_diagnostics_once_{};
    QLabel* validation_error_{};
};
