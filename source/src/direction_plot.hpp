#pragma once

#include "wardogs/core.hpp"
#include <QWidget>
#include <optional>

// A north-up schematic, never a claim to be a terrain map.
class DirectionPlot final : public QWidget {
public:
    explicit DirectionPlot(QWidget* parent = nullptr);
    void set_shot(std::optional<wardogs::Shot> shot);
protected:
    void changeEvent(QEvent* event) override;
    void paintEvent(QPaintEvent*) override;
private:
    std::optional<wardogs::Shot> shot_;
};
