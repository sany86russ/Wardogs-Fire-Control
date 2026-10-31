#pragma once

#include "wardogs/settings.hpp"

#include <QDialog>

class QCheckBox;
class QFormLayout;
class QKeySequenceEdit;
class QLabel;
class QSpinBox;

// Configuration only: opening this dialog never starts a helper.
class GameHelpersDialog final : public QDialog {
public:
    explicit GameHelpersDialog(const wardogs::AppSettings& settings,
                               QWidget* parent = nullptr);
    [[nodiscard]] wardogs::GameHelpersPreferences preferences() const;
    void show_error(const QString& message);

protected:
    void changeEvent(QEvent* event) override;
    bool nativeEvent(const QByteArray& event_type, void* message,
                     qintptr* result) override;

private:
    void update_summaries();
    void accept_if_valid();

    wardogs::AppSettings original_settings_;
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
    QLabel* validation_error_{};
};
