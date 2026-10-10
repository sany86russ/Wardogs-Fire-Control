#pragma once

#include "wardogs/terrain_package.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace wardogs {

enum class TerrainTaskKind { discovery, load_map, import_maps };

struct TerrainTaskRequest {
    std::uint64_t generation{};
    TerrainTaskKind kind{TerrainTaskKind::discovery};
    std::string map_id;
    std::optional<std::filesystem::path> import_source;
};

struct TerrainTaskResult {
    TerrainTaskRequest request;
    TerrainDiscovery discovery;
    TerrainDiscovery imported;
    std::unique_ptr<TerrainPackage> package;
    std::optional<InstalledTerrainMap> map;
    std::string error;
};

// One persistent worker owns all terrain I/O. At most one read request waits;
// newer pending reads replace older ones. A separately accepted import is
// retained, runs before the pending read, and is never coalesced or cancelled.
// Imports are limited to one outstanding request, including the running task.
// Results are delivered on the worker thread; UI owners must marshal them and
// check generation/map identity before applying them. Destruction joins the
// worker and finishes an accepted import, but discards pending read work.
class TerrainTaskRunner final {
public:
    using Result = std::shared_ptr<TerrainTaskResult>;
    using Callback = std::function<void(Result)>;
    // Injectable execution supports deterministic, filesystem-free tests.
    using Executor = std::function<Result(const TerrainTaskRequest&)>;

    explicit TerrainTaskRunner(Callback callback, Executor executor = {});
    ~TerrainTaskRunner();
    TerrainTaskRunner(const TerrainTaskRunner&) = delete;
    TerrainTaskRunner& operator=(const TerrainTaskRunner&) = delete;

    // False means closing or an import already outstanding. A successful read
    // submission may supersede an earlier read that has not started yet.
    [[nodiscard]] bool submit(TerrainTaskRequest request);
    // Idempotent. No callbacks run after stop returns. Never call stop or
    // destroy the runner from its own callback/executor thread.
    void stop() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace wardogs
