#pragma once

#include "wardogs/continuous_calibration.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace wardogs {

enum class AccuracyCommandSource { displayed_unverified, hud_observed };

// A marked landing, not proof of a physical shot. A station identity separates
// unobserved changes of pose even when map coordinates happen to be identical.
// Unknown game versions stay explicit; application versions are not patches.
struct AccuracyObservation {
    std::string session_id;
    std::string observation_id;
    std::string command_id;
    std::string station_id{"unknown"};
    std::string map_id;
    Point base;
    FiringSnapshot firing;
    Point impact;
    double impact_height_delta_m{};
    AccuracyCommandSource command_source{AccuracyCommandSource::displayed_unverified};
    bool sight_verified{};
    bool discharge_observed{};
    std::string app_version;
    std::string table_profile{"sph2_retained_v1"};
    std::string game_version{"unknown"};
    std::string height_source{"unknown"};
    // A terrain-package identifier/hash, never a local path. Older schema-1
    // receipts missing these two provenance fields load as explicitly unknown.
    std::string terrain_profile{"unknown"};
    // Zero requests an automatic UTC timestamp on first append. A duplicate
    // with zero reuses the stored receipt time and remains idempotent.
    std::int64_t recorded_at_utc_ms{};

    bool operator==(const AccuracyObservation& other) const;
};

inline constexpr int accuracy_observations_schema_version = 1;
inline constexpr std::size_t maximum_accuracy_observations = 4096;
inline constexpr std::size_t maximum_accuracy_observations_file_bytes = 8 * 1024 * 1024;

// Outside the portable application and updater manifest, beside saved points.
[[nodiscard]] std::filesystem::path accuracy_observations_path();
void validate_accuracy_observation(const AccuracyObservation& observation);

// Synchronous file API for a background collector. No screen/chat pixels,
// process inspection, networking or calibration restoration. Missing files
// load as empty; corrupt, unsupported and inaccessible files fail explicitly.
class AccuracyObservationRepository {
public:
    explicit AccuracyObservationRepository(
        std::filesystem::path path = accuracy_observations_path());

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] std::vector<AccuracyObservation> load() const;
    // Transactions lock, reload, validate, deduplicate and atomically replace.
    // Same session+observation identity with conflicting contents is rejected.
    // Keep the newest 4096 append receipts; named positions are never touched.
    [[nodiscard]] bool append(AccuracyObservation observation) const;
    [[nodiscard]] std::size_t append_many(std::span<const AccuracyObservation> observations) const;

private:
    std::filesystem::path path_;
};

inline constexpr std::size_t maximum_pending_accuracy_observations = 64;

// One bounded worker owns all disk I/O. The completion runs on that worker;
// GUI callers must queue their notification onto the UI thread. Errors are
// explicit and permanent for this writer instance: queued losses are counted,
// future submissions fail, and corrupt stores are never silently recreated.
class AccuracyObservationWriter {
public:
    using Completion = std::function<void(std::string error)>;
    explicit AccuracyObservationWriter(std::filesystem::path path,
                                        Completion completion = {});
    ~AccuracyObservationWriter();
    AccuracyObservationWriter(const AccuracyObservationWriter&) = delete;
    AccuracyObservationWriter& operator=(const AccuracyObservationWriter&) = delete;

    // False means backpressure, a stopped writer, or permanent storage failure.
    // Invalid input still throws before admission and never reaches the queue.
    [[nodiscard]] bool submit(AccuracyObservation observation);
    // Reject new work, flush accepted work and join the worker. Idempotent.
    void stop();
    [[nodiscard]] std::string error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace wardogs
