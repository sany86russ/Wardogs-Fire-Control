#pragma once

#include "wardogs/pinned_preferences.hpp"
#include "wardogs/vehicle_ballistics.hpp"

#include <QPoint>
#include <QRect>
#include <QSize>
#include <QWidget>

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>

class QFrame;
class QContextMenuEvent;
class QKeySequenceEdit;
class QLabel;
class QMouseEvent;
class QResizeEvent;
class QShowEvent;
class QScreen;
class QSlider;
class QTimer;
class QToolButton;
class VehicleSolutionWidget;

class PinnedResultWindow final : public QWidget {
public:
    using Preferences = wardogs::PinnedCardPreferences;
    // The owner may return an effective replacement before it is displayed.
    using PreferencesChanged = std::function<bool(Preferences&)>;
    using GhostEnabledChanged = std::function<void(bool)>;
    using GhostOpacityChanged = std::function<void(int)>;

    explicit PinnedResultWindow(std::function<void()> exit_callback,
                                QWidget* parent = nullptr);
    PinnedResultWindow(std::function<void()> exit_callback,
                       Preferences preferences,
                       PreferencesChanged preferences_changed,
                       QWidget* parent = nullptr);

    void set_mode(bool vehicle_mode);
    void set_values(const QString& distance, const QString& bearing,
                    const QString& mortar_mil = {});
    void set_vehicle_values(const VehicleSolutionWidget& low,
                            const VehicleSolutionWidget& high);
    void set_selected_arc(std::optional<wardogs::Arc> arc);
    void set_error(bool error);
    void set_context_caption(const QString& caption, const QString& detail = {});
    void set_workflow_status(const QString& caption, const QString& detail = {});
    // Restore once on entering pinned mode. Later entries retain the live placement.
    void prepare_for_show(const QRect& anchor);
    // Flush the pending user gesture before the owner snapshots settings or exits.
    bool flush_preferences();
    [[nodiscard]] bool hasHeightForWidth() const override;
    void set_locked(bool locked);
    [[nodiscard]] bool is_locked() const { return preferences_.locked; }
    void set_always_on_top(bool enabled);
    [[nodiscard]] bool always_on_top() const { return preferences_.always_on_top; }
    void set_opacity_percent(int opacity_percent);
    void configure_unlock_hotkey(const std::wstring& hotkey);
    [[nodiscard]] int opacity_percent() const {
        return preferences_.opacity_percent;
    }
    void configure_ghost_controls(bool enabled, int opacity_percent,
                                  GhostEnabledChanged enabled_changed,
                                  GhostOpacityChanged opacity_changed);
    void set_ghost_enabled(bool enabled);
    void set_ghost_opacity_percent(int opacity_percent);

protected:
    bool event(QEvent* event) override;
    bool nativeEvent(const QByteArray& event_type, void* message,
                     qintptr* result) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    using Edges = std::set<std::string>;

    static QWidget* result_card(const QString& color, const QString& caption,
                                QLabel*& value,
                                QLabel** secondary = nullptr);
    [[nodiscard]] Edges resize_edges_at(QPoint position) const;
    [[nodiscard]] static Qt::CursorShape cursor_for_edges(const Edges& edges);
    void resize_from_pointer(QPoint pointer);
    void apply_font_scale();
    [[nodiscard]] int workflow_status_extra() const;
    void update_workflow_status_layout(int previous_extra,
                                       bool preserve_result_height = true);
    void build_context_menu();
    void update_lock_control();
    void update_header_controls();
    void update_unlock_hotkey_control();
    void fit_unlock_editor_height() const;
    [[nodiscard]] bool commit_preferences(Preferences preferences);
    void apply_mouse_transparency();
    void show_context_menu();
    void observe_screen(QScreen* screen);
    void fit_current_placement();
    [[nodiscard]] std::optional<wardogs::PinnedCardPlacement> current_placement() const;
    void queue_geometry_preferences();
    [[nodiscard]] QPoint context_menu_position() const;

    std::function<void()> exit_callback_;
    PreferencesChanged preferences_changed_;
    Preferences preferences_;
    QFrame* frame_{};
    QWidget* context_menu_{};
    QLabel* weapon_caption_{};
    QLabel* lock_hint_{};
    QToolButton *header_lock_button_{}, *controls_button_{}, *header_return_button_{};
    QToolButton* lock_button_{};
    QToolButton* topmost_button_{};
    QSlider* opacity_slider_{};
    QToolButton* ghost_button_{};
    QSlider* ghost_opacity_slider_{};
    QKeySequenceEdit* unlock_hotkey_{};
    GhostEnabledChanged ghost_enabled_changed_;
    GhostOpacityChanged ghost_opacity_changed_;
    bool ghost_enabled_{};
    int ghost_opacity_percent_{80};
    QWidget *mortar_panel_{}, *vehicle_panel_{};
    QLabel *distance_{}, *bearing_{}, *mortar_mil_{};
    QLabel* workflow_status_{};
    QLabel* context_caption_{};
    VehicleSolutionWidget *low_{}, *high_{};
    bool vehicle_mode_{};
    bool dragging_{};
    QPoint drag_offset_{};
    Edges resize_edges_;
    QPoint resize_start_global_{};
    QRect resize_start_geometry_{};
    QRect gesture_start_geometry_{};
    std::map<bool, QSize> mode_sizes_;
    QTimer* geometry_save_timer_{};
    std::optional<wardogs::PinnedCardPlacement> pending_placement_;
    std::array<std::optional<wardogs::PinnedCardSize>, 2> pending_mode_sizes_;
    bool placement_initialized_{};
    QRect placement_anchor_{};
    std::optional<wardogs::PinnedCardPlacement> live_placement_;
    double font_scale_{1.0};
    bool applying_font_scale_{};
    bool updating_workflow_status_layout_{};
};
