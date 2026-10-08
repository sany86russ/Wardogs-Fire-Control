#pragma once

#include "wardogs/vehicle_ballistics.hpp"

#include <QFrame>

#include <optional>

class QHBoxLayout;
class QLabel;

class VehicleSolutionWidget final : public QFrame {
public:
    explicit VehicleSolutionWidget(wardogs::Arc arc, bool compact = false,
                                   QWidget* parent = nullptr);

    void set_waiting();
    void set_solution(const wardogs::CorrectedSolution& solution,
                      std::optional<double> target_distance_m = std::nullopt);
    void set_unavailable(const QString& distance, const QString& bearing);
    void set_height_unavailable();
    void copy_from(const VehicleSolutionWidget& source);
    void set_compact_scale(double scale);
    void set_selected(bool selected);

    [[nodiscard]] QString distance_text() const;
    [[nodiscard]] QString bearing_text() const;
    [[nodiscard]] QString mil_text() const;
    [[nodiscard]] bool unavailable() const;
    [[nodiscard]] bool selected() const;

protected:
    void changeEvent(QEvent* event) override;

private:
    QLabel* add_metric(QHBoxLayout* layout, const QString& caption,
                       const QString& object_name, int width,
                       QLabel** caption_label = nullptr);
    void set_unavailable_state(bool unavailable);
    void update_trajectory_label();

    wardogs::Arc arc_;
    bool compact_{};
    bool solution_ready_{};
    bool distance_is_target_{true};
    bool selected_{};
    double compact_scale_{1.0};
    QLabel *trajectory_{}, *distance_caption_{}, *distance_{}, *bearing_{}, *mil_{};
};
