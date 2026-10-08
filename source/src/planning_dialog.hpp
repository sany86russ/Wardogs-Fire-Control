#pragma once

#include "wardogs/fire_missions.hpp"
#include "wardogs/firing_analysis.hpp"
#include "wardogs/game_map.hpp"

#include <QDialog>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

struct PlanningContext {
    wardogs::GameMap map{wardogs::GameMap::unselected};
    wardogs::AnalysisWeapon weapon{wardogs::AnalysisWeapon::l81};
    wardogs::Arc preferred_arc{wardogs::Arc::low};
    bool map_confirmed{};
    bool capture_pending{};
    bool solution_held{};
    std::optional<wardogs::Point> base;
    std::optional<wardogs::Point> target;
    wardogs::HeightLookup terrain;
};

// The provider is read again before every user action. The owner applies an
// explicitly chosen point through the same epoch/calibration rules as manual
// entry, and verifies the map and weapon again before accepting it.
class PlanningDialog final : public QDialog {
public:
    using Provider = std::function<PlanningContext()>;
    using Apply = std::function<void(wardogs::FireMissionKind, wardogs::Point,
                                    wardogs::GameMap, wardogs::AnalysisWeapon)>;
    PlanningDialog(Provider provider, Apply apply, QWidget* parent = nullptr,
                   std::filesystem::path storage_directory = {});
    ~PlanningDialog() override;
    void refresh();

private:
    struct State;
    std::unique_ptr<State> state_;
};
