#pragma once

#include "wardogs/vehicle_ballistics.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace wardogs {

enum class AnalysisWeapon { l81, sph2 };
enum class EstimateBasis {
    retained_geometric_model,
    assumed_vacuum_model,
    user_measurement,
    interpolated_user_measurement
};

struct EstimateSource {
    EstimateBasis basis{EstimateBasis::retained_geometric_model};
    std::string source;
    std::string game_version;
};

struct FlightTimeSample {
    double distance_m{};
    double seconds{};
    std::optional<double> uncertainty_s;
};

// User-supplied observations, not certified game constants. Coverage is closed;
// there is no extrapolation or transfer between weapons, arcs, ammo or patches.
struct FlightTimeProfile {
    AnalysisWeapon weapon{AnalysisWeapon::sph2};
    Arc arc{Arc::low};
    double height_delta_m{};
    std::string ammunition_id;
    std::string game_version;
    std::string source;
    std::vector<FlightTimeSample> samples;
};

// Optional L81 trajectory assumption. The speed and gravity have no defaults;
// this independent vacuum model does not reinterpret the table's MIL as radians
// and never changes the retained L81 sight command.
struct VacuumFlightProfile {
    double speed_mps{};
    double gravity_mps2{};
    std::string source;
};

inline constexpr std::size_t maximum_flight_time_samples = 256;
inline constexpr std::size_t maximum_trajectory_samples = 8192;
inline constexpr double flight_profile_height_tolerance_m = 1e-6;

struct FiringAnalysisRequest {
    AnalysisWeapon weapon{AnalysisWeapon::sph2};
    Arc arc{Arc::low};
    Point base;
    Point target;
    // If supplied, includes any firing-point/target offsets. Otherwise derive
    // from both terrain endpoints, or explicitly report assumed level terrain.
    std::optional<double> height_delta_m;
    HeightLookup terrain;
    double muzzle_height_above_ground_m{};
    double target_height_above_ground_m{};
    double sample_step_m{2.0};
    double clearance_margin_m{};
    // SPH-2 only: explicitly assume g and v=sqrt(2629*g) to estimate time.
    // An omitted g leaves the retained geometric trajectory without seconds.
    std::optional<double> gravity_mps2;
    std::optional<VacuumFlightProfile> vacuum_profile;
    std::optional<FlightTimeProfile> flight_profile;
    std::string ammunition_id;
    std::string game_version;
};

enum class HeightSource { assumed_level, explicit_delta, terrain, unavailable };
enum class FiringAnalysisStatus { available, unsupported_range, height_unavailable, unreachable };
enum class TrajectoryStatus { unavailable, estimated, unreachable, height_unavailable };
enum class FlightProfileStatus { not_provided, matched, identity_mismatch, height_mismatch, out_of_coverage };
enum class TerrainClearanceStatus { not_checked, model_unavailable, incomplete, clear_at_samples, blocked };

struct FlightTimeEstimate {
    double seconds{};
    std::optional<double> uncertainty_s;
    EstimateSource source;
};

struct TrajectorySample {
    Point position;
    double distance_m{};
    double height_above_muzzle_m{};
    // Times belong only to the explicitly assumed physical model. A measured
    // total flight time does not establish timing at intermediate positions.
    std::optional<double> model_time_s;
    std::optional<double> terrain_height_above_muzzle_m;
    std::optional<double> clearance_m;
};

struct TrajectoryEstimate {
    EstimateSource source;
    double elevation_rad{};
    // Maximum along the gun-to-target segment, including either endpoint.
    double apex_distance_m{};
    double apex_height_above_muzzle_m{};
    std::optional<double> assumed_speed_mps;
    std::optional<double> assumed_gravity_mps2;
    std::optional<double> model_flight_time_s;
    std::vector<TrajectorySample> samples;
};

struct TerrainClearance {
    TerrainClearanceStatus status{TerrainClearanceStatus::not_checked};
    std::size_t samples_checked{};
    std::size_t samples_missing{};
    // Interior samples, plus any endpoint actually below ground. Touching
    // ground at the firing/impact endpoint is expected. Finite sampling and
    // approximate flight never guarantee safety.
    std::optional<double> minimum_clearance_m;
    std::optional<double> first_blocked_distance_m;
    std::optional<Point> first_blocked_position;
    double actual_sample_step_m{};
};

struct FiringAnalysis {
    double distance_m{};
    double bearing_deg{};
    std::optional<double> height_delta_m;
    HeightSource height_source{HeightSource::assumed_level};
    FiringAnalysisStatus status{FiringAnalysisStatus::available};
    std::optional<double> nominal_mil;
    TrajectoryStatus trajectory_status{TrajectoryStatus::unavailable};
    std::optional<TrajectoryEstimate> trajectory;
    FlightProfileStatus flight_profile_status{FlightProfileStatus::not_provided};
    std::optional<FlightTimeEstimate> flight_time;
    TerrainClearance clearance;
};

// Throws invalid_argument for malformed data (including non-finite values,
// duplicate/unsorted distances and missing provenance). No I/O or persistence.
void validate_flight_time_profile(const FlightTimeProfile& profile);

// Nominal analysis of the accepted target, not telemetry of an actual shot or
// a replacement for the existing local correction. All trajectory/time models
// are estimates; only user observations can supply measured flight time.
[[nodiscard]] FiringAnalysis analyze_firing(const FiringAnalysisRequest& request);

// Relative to gun -> target: positive lateral is right, positive longitudinal
// is add/far. Inputs/outputs use map units; offsets use metres. Rejects an
// undefined zero-length frame, non-finite inputs and arithmetic overflow.
[[nodiscard]] Point offset_target(Point base, Point target,
                                  double lateral_m, double longitudinal_m);

}  // namespace wardogs
