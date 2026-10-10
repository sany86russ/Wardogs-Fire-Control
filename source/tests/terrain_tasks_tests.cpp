#include "wardogs/terrain_tasks.hpp"
#include "wardogs/logger.hpp"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;
int failures{};

void check(bool condition, const char* label) {
    if (!condition) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
}

wardogs::TerrainTaskRequest read(std::uint64_t generation) {
    return {generation, wardogs::TerrainTaskKind::load_map, "bakurani", {}};
}
wardogs::TerrainTaskRequest import_request(std::uint64_t generation) {
    return {generation, wardogs::TerrainTaskKind::import_maps, {}, std::filesystem::path(L"unused-test-source")};
}

struct Harness {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::uint64_t> executions;
    std::vector<wardogs::TerrainTaskRunner::Result> results;
    std::uint64_t blocked_generation{1};
    bool released{};
    std::atomic<int> active{};
    std::atomic<int> maximum_active{};
    std::thread::id callback_thread;

    wardogs::TerrainTaskRunner::Result execute(const wardogs::TerrainTaskRequest& request) {
        const int current_active = ++active;
        maximum_active.store(std::max(current_active, maximum_active.load()));
        {
            std::unique_lock lock(mutex);
            executions.push_back(request.generation);
            changed.notify_all();
            if (request.generation == blocked_generation &&
                !changed.wait_for(lock, 5s, [&] { return released; })) {
                --active;
                throw std::runtime_error("Test executor gate timed out");
            }
        }
        auto result = std::make_shared<wardogs::TerrainTaskResult>();
        result->request = request;
        --active;
        return result;
    }

    void completed(wardogs::TerrainTaskRunner::Result result) {
        std::scoped_lock lock(mutex);
        callback_thread = std::this_thread::get_id();
        results.push_back(std::move(result));
        changed.notify_all();
    }

    bool wait_executions(std::size_t count) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, 5s, [&] { return executions.size() >= count; });
    }
    bool wait_results(std::size_t count) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, 5s, [&] { return results.size() >= count; });
    }
    void release() {
        std::scoped_lock lock(mutex);
        released = true;
        changed.notify_all();
    }
};

void bounded_serial_work() {
    Harness harness;
    wardogs::TerrainTaskRunner runner(
        [&](auto result) { harness.completed(std::move(result)); },
        [&](const auto& request) { return harness.execute(request); });
    check(runner.submit(read(1)), "first read is accepted");
    check(harness.wait_executions(1), "first read starts on persistent worker");
    check(runner.submit(read(2)), "one pending read is accepted");
    check(runner.submit(read(3)), "new pending read supersedes earlier pending read");
    check(runner.submit(import_request(4)), "import has a separate pending slot");
    check(!runner.submit(import_request(5)), "second outstanding import is rejected explicitly");
    harness.release();
    check(harness.wait_results(3), "active read, retained import and latest read complete");
    runner.stop();
    check(harness.executions == std::vector<std::uint64_t>{1, 4, 3},
          "read coalescing preserves accepted import and gives import priority");
    check(harness.maximum_active == 1, "terrain operations never overlap");
    check(harness.callback_thread != std::this_thread::get_id(), "callback is delivered from worker thread");
    check(!runner.submit(read(6)), "stopped runner rejects read submissions");
    check(!runner.submit(import_request(7)), "stopped runner rejects imports");
    runner.stop();
    check(harness.results.size() == 3, "idempotent stop cannot duplicate callbacks");
}

void close_finishes_import() {
    Harness harness;
    wardogs::TerrainTaskRunner runner(
        [&](auto result) { harness.completed(std::move(result)); },
        [&](const auto& request) { return harness.execute(request); });
    check(runner.submit(read(1)), "close test starts read");
    check(harness.wait_executions(1), "close test active read is gated");
    check(runner.submit(import_request(2)), "close test accepts import");
    check(runner.submit(read(3)), "close test has pending read");
    std::atomic<bool> stop_returned{};
    std::thread closer([&] { runner.stop(); stop_returned = true; });
    // Wait for the runner's own closing flag, without relying on sleep timing.
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    bool closing_observed{};
    while (std::chrono::steady_clock::now() < deadline) {
        if (!runner.submit(read(4))) { closing_observed = true; break; }
        std::this_thread::yield();
    }
    check(closing_observed, "stop makes new submissions fail before joining active I/O");
    check(!stop_returned, "stop waits for in-flight operation");
    harness.release();
    closer.join();
    check(stop_returned, "stop returns after accepted import is complete");
    check(harness.executions == std::vector<std::uint64_t>{1, 2},
          "closing drains accepted import while discarding coalesced pending reads");
    check(harness.results.empty(), "closing owner receives no new completion callbacks");
}

void running_import_limit() {
    Harness harness;
    wardogs::TerrainTaskRunner runner(
        [&](auto result) { harness.completed(std::move(result)); },
        [&](const auto& request) { return harness.execute(request); });
    check(runner.submit(import_request(1)), "running import test accepts import");
    check(harness.wait_executions(1), "import starts before checking capacity");
    check(!runner.submit(import_request(2)), "import limit also includes running import");
    check(runner.submit(read(3)), "running import does not block bounded pending read");
    harness.release();
    check(harness.wait_results(2), "running import and pending read complete serially");
    check(runner.submit(import_request(4)), "completed import releases import capacity");
    check(harness.wait_results(3), "new import completes");
    runner.stop();
    check(harness.executions == std::vector<std::uint64_t>{1, 3, 4},
          "only explicitly accepted imports execute");
}

void failures_are_results() {
    Harness harness;
    harness.blocked_generation = 0;
    std::atomic<int> callback_calls{};
    wardogs::TerrainTaskRunner runner([&](auto result) {
        ++callback_calls;
        harness.completed(result);
        if (result->request.generation == 4) throw std::runtime_error("Test consumer failure");
    }, [&](const auto& request) -> wardogs::TerrainTaskRunner::Result {
        if (request.generation == 1) throw std::runtime_error("Test task failure");
        if (request.generation == 2) throw 42;
        if (request.generation == 3) return {};
        auto result = std::make_shared<wardogs::TerrainTaskResult>();
        result->request.generation = 999;
        return result;
    });
    for (std::uint64_t generation = 1; generation <= 5; ++generation) {
        check(runner.submit(read(generation)), "error sequence request is accepted");
        check(harness.wait_results(static_cast<std::size_t>(generation)), "error sequence delivers each result");
    }
    runner.stop();
    if (harness.results.size() == 5) {
        check(harness.results[0]->error == "Test task failure", "standard task exception is returned explicitly");
        check(!harness.results[1]->error.empty(), "unknown task exception is returned explicitly");
        check(!harness.results[2]->error.empty(), "null injected result is rejected explicitly");
        check(harness.results[3]->request.generation == 4 && harness.results[4]->request.generation == 5,
              "executor cannot alter generation and callback exception cannot kill subsequent work");
    }
    check(callback_calls == 5, "each completed task is delivered exactly once");
    bool rejected{};
    try { wardogs::TerrainTaskRunner invalid({}); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "missing completion consumer is rejected before starting a thread");
}

void default_executor_rejects_without_writes() {
    Harness harness;
    wardogs::TerrainTaskRunner runner([&](auto result) { harness.completed(std::move(result)); });
    auto request = read(1);
    request.map_id = "unsupported-test-map";
    check(runner.submit(request), "unsupported map operation is scheduled");
    check(harness.wait_results(1), "unsupported map has a completion result");
    auto missing_source = import_request(2);
    missing_source.import_source.reset();
    check(runner.submit(missing_source), "missing import source is scheduled for validation");
    check(harness.wait_results(2), "missing import source has a completion result");
    runner.stop();
    if (harness.results.size() == 2) {
        check(!harness.results[0]->error.empty() && !harness.results[0]->package,
              "unrecognized map cannot reach terrain discovery or become usable");
        check(!harness.results[1]->error.empty() && harness.results[1]->imported.installed.empty(),
              "missing import source cannot reach installation or write profile data");
    }
}

void closing_import_is_audited() {
    const auto filename = "wardogs-terrain-worker-audit-" + std::to_string(GetCurrentProcessId()) + "-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".log";
    const auto path = std::filesystem::temp_directory_path() / filename;
    check(!std::filesystem::exists(path), "audit test owns a new temporary log leaf");
    if (std::filesystem::exists(path)) return;
    const bool initialized = wardogs::initialize_session_log(path, "terrain-worker-test");
    check(initialized, "audit test opens isolated temporary session log");
    if (!initialized) return;
    for (int scenario = 0; scenario < 3; ++scenario) {
        Harness harness;
        wardogs::TerrainTaskRunner runner(
            [&](auto result) { harness.completed(std::move(result)); },
            [&](const auto& request) -> wardogs::TerrainTaskRunner::Result {
                auto result = harness.execute(request);
                if (request.kind == wardogs::TerrainTaskKind::import_maps) {
                    if (scenario == 1)
                        throw std::runtime_error("C:\\private\\audit-sensitive-marker\\source.wdt failed");
                    result->imported.installed.resize(scenario == 2 ? 1 : 2);
                    if (scenario == 2)
                        result->imported.problems.push_back(L"C:\\private\\audit-sensitive-marker\\bad.wdt rejected");
                }
                return result;
            });
        check(runner.submit(read(1)), "audit test starts gated read");
        check(harness.wait_executions(1), "audit test read starts");
        check(runner.submit(import_request(2)), "audit test accepts import before closing");
        std::thread closer([&] { runner.stop(); });
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        bool closing_observed{};
        while (std::chrono::steady_clock::now() < deadline) {
            if (!runner.submit(read(3))) { closing_observed = true; break; }
            std::this_thread::yield();
        }
        check(closing_observed, "audit test observes closing before import finishes");
        harness.release();
        closer.join();
        check(harness.results.empty(), "closed owner does not receive audit test callbacks");
    }
    check(wardogs::flush_session_log(), "closed-owner import outcomes can be published before logger shutdown");
    wardogs::shutdown_session_log();
    std::ifstream input(path, std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    input.close();
    check(bytes.find("outcome=success installed_count=2 problem_count=0") != std::string::npos,
          "successful accepted import remains auditable after owner closure");
    check(bytes.find("outcome=failed installed_count=0 problem_count=0") != std::string::npos &&
          bytes.find("closing=1 error=task_failed") != std::string::npos,
          "failed accepted import remains auditable without a completion callback");
    check(bytes.find("outcome=partial installed_count=1 problem_count=1") != std::string::npos &&
          bytes.find("closing=1 error=package_rejected") != std::string::npos,
          "partial accepted import records retained package and rejected-package counts");
    check(bytes.find("audit-sensitive-marker") == std::string::npos &&
          bytes.find("C:\\private") == std::string::npos,
          "import audit does not disclose raw exceptions, rejected-package text or paths");
    check(std::filesystem::remove(path), "audit test removes only its owned temporary log leaf");
}
}  // namespace

int main() {
    bounded_serial_work();
    close_finishes_import();
    running_import_limit();
    failures_are_results();
    default_executor_rejects_without_writes();
    closing_import_is_audited();
    if (failures) return 1;
    std::cout << "PASS: bounded terrain worker, coalescing, serial imports, closure and failures\n";
    return 0;
}
