#pragma once

#include "wardogs/settings.hpp"

#include <QDialog>

#include <functional>
#include <optional>

class QComboBox;
class QCheckBox;
class QDoubleSpinBox;
class QHBoxLayout;
class QKeySequenceEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;

class SettingsDialog final : public QDialog {
public:
    explicit SettingsDialog(const wardogs::AppSettings& settings,
                            QWidget* parent = nullptr);

    [[nodiscard]] wardogs::AppSettings settings() const;
    [[nodiscard]] bool adjust_ghost_requested() const {
        return adjust_ghost_requested_;
    }

protected:
    void changeEvent(QEvent* event) override;
    bool nativeEvent(const QByteArray& event_type, void* message,
                     qintptr* result) override;

private:
    static std::wstring hotkey_text(const QKeySequenceEdit* editor);
    void accept_if_valid();
    void request_ghost_adjustment();

    QComboBox* backend_{};
    QCheckBox* automatic_chat_region_{};
    QKeySequenceEdit* region_key_{};
    QKeySequenceEdit* base_key_{};
    QKeySequenceEdit* target_key_{};
    QKeySequenceEdit* quick_target_key_{};
    QKeySequenceEdit* impact_key_{};
    QKeySequenceEdit* ghost_arc_key_{};
    QKeySequenceEdit* exit_game_mode_key_{};
    QCheckBox* standalone_mode_{};
    QCheckBox* middle_mouse_enabled_{};
    QSpinBox* mouse_capture_delay_{};
    QComboBox* ghost_preset_{};
    QDoubleSpinBox* ghost_bearing_compensation_{};
    QPlainTextEdit* pattern_{};
    std::optional<wardogs::CaptureRegion> capture_region_;
    wardogs::PinnedCardPreferences pinned_card_;
    wardogs::GhostReticlePreferences ghost_reticle_;
    wardogs::AppSettings original_settings_;
    std::function<void()> update_workflow_;
    bool ghost_bearing_compensation_changed_{};
    bool adjust_ghost_requested_{};
};
