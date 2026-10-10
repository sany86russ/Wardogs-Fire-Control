#include "wardogs/terrain_tasks.hpp"
#include "wardogs/logger.hpp"

#include <QString>

#include <algorithm>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace wardogs {
namespace {

TerrainTaskRunner::Result execute_terrain_task(const TerrainTaskRequest& request) {
    auto result = std::make_shared<TerrainTaskResult>();
    result->request = request;
    switch (request.kind) {
    case TerrainTaskKind::discovery:
        result->discovery = discover_available_terrain_maps();
        break;
    case TerrainTaskKind::load_map: {
        const auto& specs = official_terrain_maps();
        const auto spec = std::find_if(specs.begin(), specs.end(), [&](const auto& value) {
            return value.map_id == request.map_id;
        });
        if (spec == specs.end()) throw std::invalid_argument("Для выбранной карты нет описания высот");
        result->discovery = discover_available_terrain_maps(
            default_terrain_directory(), user_terrain_directory(), {*spec});
        if (result->discovery.installed.empty()) {
            std::string problems;
            for (const auto& problem : result->discovery.problems) {
                if (!problems.empty()) problems += "; ";
                problems += QString::fromStdWString(problem).toUtf8().toStdString();
            }
            result->error = problems.empty() ? "Проверенные высоты выбранной карты недоступны" : problems;
            break;
        }
        result->map = result->discovery.installed.front();
        result->package = std::make_unique<TerrainPackage>(result->map->path);
        break;
    }
    case TerrainTaskKind::import_maps:
        if (!request.import_source || request.import_source->empty())
            throw std::invalid_argument("Для подключения высот нужна выбранная локальная папка");
        result->imported = install_terrain_maps(*request.import_source);
        result->discovery = discover_available_terrain_maps();
        break;
    default:
        throw std::invalid_argument("Неизвестная операция рельефа");
    }
    return result;
}

void log_import_outcome(const TerrainTaskResult& result, bool closing) {
    const bool task_failed = !result.error.empty();
    const bool import_failed = result.imported.installed.empty() && !result.imported.problems.empty();
    const bool incomplete = !result.imported.problems.empty() || !result.discovery.problems.empty();
    const char* outcome = task_failed || import_failed ? "failed" : incomplete ? "partial" : "success";
    // Exception strings and discovery problem text can contain local paths or
    // user-controlled text. Audit only fixed error categories and counts.
    const char* error = task_failed ? "task_failed" : !result.imported.problems.empty()
        ? "package_rejected" : !result.discovery.problems.empty() ? "discovery_incomplete" : "none";
    const std::string message = "terrain.import_completed generation=" + std::to_string(result.request.generation) +
        " outcome=" + outcome + " installed_count=" + std::to_string(result.imported.installed.size()) +
        " problem_count=" + std::to_string(result.imported.problems.size()) +
        " discovery_problem_count=" + std::to_string(result.discovery.problems.size()) +
        " closing=" + (closing ? "1" : "0") + " error=" + error;
    if (task_failed || incomplete) log_warning(message);
    else log_info(message);
}

}  // namespace

struct TerrainTaskRunner::Impl {
    Callback callback;
    Executor executor;
    std::mutex mutex;
    std::condition_variable changed;
    std::optional<TerrainTaskRequest> pending_read;
    std::optional<TerrainTaskRequest> pending_import;
    bool import_outstanding{};
    bool closing{};
    std::mutex stop_mutex;
    std::jthread worker;

    Impl(Callback on_result, Executor execute)
        : callback(std::move(on_result)),
          executor(execute ? std::move(execute) : Executor(execute_terrain_task)) {
        if (!callback) throw std::invalid_argument("TerrainTaskRunner requires a completion callback");
        worker = std::jthread([this] { run(); });
    }

    bool submit(TerrainTaskRequest request) {
        std::scoped_lock lock(mutex);
        if (closing) return false;
        switch (request.kind) {
        case TerrainTaskKind::discovery:
        case TerrainTaskKind::load_map:
            pending_read = std::move(request);
            break;
        case TerrainTaskKind::import_maps:
            if (import_outstanding) return false;
            pending_import = std::move(request);
            import_outstanding = true;
            break;
        default:
            throw std::invalid_argument("Неизвестная операция рельефа");
        }
        changed.notify_one();
        return true;
    }

    Result execute(const TerrainTaskRequest& request) {
        try {
            auto result = executor(request);
            if (!result) throw std::runtime_error("Terrain executor returned no result");
            // Executor injection cannot alter the task's identity or defeat
            // the UI owner's generation check.
            result->request = request;
            return result;
        } catch (const std::exception& error) {
            auto result = std::make_shared<TerrainTaskResult>();
            result->request = request;
            result->error = error.what();
            return result;
        } catch (...) {
            auto result = std::make_shared<TerrainTaskResult>();
            result->request = request;
            result->error = "Неизвестная ошибка фоновой проверки рельефа";
            return result;
        }
    }

    void run() {
        for (;;) {
            TerrainTaskRequest request;
            {
                std::unique_lock lock(mutex);
                changed.wait(lock, [&] { return closing || pending_import || pending_read; });
                if (closing && !pending_import) return;
                if (pending_import) {
                    request = std::move(*pending_import);
                    pending_import.reset();
                } else {
                    request = std::move(*pending_read);
                    pending_read.reset();
                }
            }
            auto result = execute(request);
            bool deliver{};
            {
                std::scoped_lock lock(mutex);
                if (request.kind == TerrainTaskKind::import_maps) import_outstanding = false;
                deliver = !closing;
            }
            if (request.kind == TerrainTaskKind::import_maps) log_import_outcome(*result, !deliver);
            if (deliver) {
                try {
                    callback(std::move(result));
                } catch (...) {
                    // A broken consumer must not terminate the worker or lose
                    // an independently accepted import. Make the failure
                    // explicit through the application's existing logger.
                    log_error("terrain.callback_failed; terrain completion consumer threw an exception");
                }
            }
        }
    }

    void stop() noexcept {
        std::scoped_lock stop_lock(stop_mutex);
        {
            std::scoped_lock lock(mutex);
            closing = true;
            pending_read.reset();
            // An accepted import remains queued and is drained before exit.
        }
        changed.notify_one();
        if (worker.joinable()) worker.join();
    }
};

TerrainTaskRunner::TerrainTaskRunner(Callback callback, Executor executor)
    : impl_(std::make_unique<Impl>(std::move(callback), std::move(executor))) {}
TerrainTaskRunner::~TerrainTaskRunner() { stop(); }
bool TerrainTaskRunner::submit(TerrainTaskRequest request) { return impl_->submit(std::move(request)); }
void TerrainTaskRunner::stop() noexcept { impl_->stop(); }

}  // namespace wardogs
