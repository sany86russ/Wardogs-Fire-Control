#include "wardogs/core.hpp"
#include "wardogs/firing_analysis.hpp"
#include "wardogs/terrain_package.hpp"
#include "wardogs/vehicle_ballistics.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
constexpr std::size_t trials = 9;

struct Checksum {
    std::uint64_t value{14695981039346656037ULL};
    void add(std::uint64_t part) {
        for (unsigned byte = 0; byte < 8; ++byte) {
            value ^= (part >> (byte * 8)) & 0xff;
            value *= 1099511628211ULL;
        }
    }
    void add(double number) { add(std::bit_cast<std::uint64_t>(number)); }
    void add(const std::optional<double>& number) {
        add(static_cast<std::uint64_t>(number.has_value()));
        if (number) add(*number);
    }
};

void checksum_analysis(Checksum& checksum, const wardogs::FiringAnalysis& result) {
    checksum.add(result.distance_m);
    checksum.add(result.bearing_deg);
    checksum.add(result.height_delta_m);
    checksum.add(static_cast<std::uint64_t>(result.status));
    checksum.add(result.nominal_mil);
    checksum.add(static_cast<std::uint64_t>(result.trajectory_status));
    checksum.add(static_cast<std::uint64_t>(result.clearance.status));
    checksum.add(static_cast<std::uint64_t>(result.clearance.samples_checked));
    checksum.add(static_cast<std::uint64_t>(result.clearance.samples_missing));
    checksum.add(result.clearance.minimum_clearance_m);
    checksum.add(result.clearance.first_blocked_distance_m);
    if (result.trajectory) {
        checksum.add(result.trajectory->elevation_rad);
        checksum.add(result.trajectory->apex_distance_m);
        checksum.add(result.trajectory->apex_height_above_muzzle_m);
        for (const auto& sample : result.trajectory->samples) {
            checksum.add(sample.position.x); checksum.add(sample.position.y);
            checksum.add(sample.distance_m); checksum.add(sample.height_above_muzzle_m);
            checksum.add(sample.terrain_height_above_muzzle_m); checksum.add(sample.clearance_m);
        }
    }
}

template <typename Function>
QJsonObject measure(const char* name, int operations_per_trial, Function function) {
    std::array<double, trials> elapsed_us{};
    std::optional<std::uint64_t> expected;
    QJsonArray runs;
    for (std::size_t trial = 0; trial < trials; ++trial) {
        Checksum checksum;
        const auto started = Clock::now();
        function(checksum);
        elapsed_us[trial] = std::chrono::duration<double, std::micro>(Clock::now() - started).count()
                            / operations_per_trial;
        if (expected && checksum.value != *expected)
            throw std::runtime_error("Repeated benchmark results changed");
        expected = checksum.value;
        runs.append(elapsed_us[trial]);
    }
    std::sort(elapsed_us.begin(), elapsed_us.end());
    return {{"scenario", QString::fromLatin1(name)}, {"operations_per_trial", operations_per_trial},
            {"trials", static_cast<int>(trials)}, {"median_us", elapsed_us[trials / 2]},
            {"max_us", elapsed_us.back()}, {"runs_us", runs},
            {"result_checksum", QString::number(*expected, 16)}};
}

QJsonObject read_metadata(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    input.seekg(8);
    std::array<unsigned char, 4> bytes{};
    input.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
    std::uint32_t length{};
    for (unsigned index = 0; index < bytes.size(); ++index)
        length |= static_cast<std::uint32_t>(bytes[index]) << (8 * index);
    if (!input || length == 0 || length > 1024 * 1024)
        throw std::runtime_error("Invalid bounded benchmark metadata");
    input.seekg(16);
    std::string encoded(length, '\0');
    input.read(encoded.data(), encoded.size());
    if (!input) throw std::runtime_error("Truncated benchmark metadata");
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(encoded), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        throw std::runtime_error("Invalid benchmark metadata JSON");
    return document.object();
}

QJsonObject terrain_benchmarks(const wardogs::InstalledTerrainMap& map) {
    const auto coverage = read_metadata(map.path)["coverage"].toObject();
    const double x_min = coverage["gameXMin"].toDouble();
    const double x_max = coverage["gameXMax"].toDouble();
    const double y_min = coverage["gameYMin"].toDouble();
    const double y_max = coverage["gameYMax"].toDouble();
    const wardogs::Point centre{std::midpoint(x_min, x_max), std::midpoint(y_min, y_max)};
    QJsonArray scenarios;
    scenarios.append(measure("cold_package_and_25_distributed_heights", 1, [&](Checksum& checksum) {
        wardogs::TerrainPackage terrain(map.path);
        for (const double y_fraction : {0.0, 0.12345, 0.5, 0.87654, 1.0}) {
            for (const double x_fraction : {0.0, 0.12345, 0.5, 0.87654, 1.0}) {
                const auto height = terrain.height_at({std::lerp(x_min, x_max, x_fraction),
                                                       std::lerp(y_min, y_max, y_fraction)});
                if (!height) throw std::runtime_error("Covered height is missing");
                checksum.add(*height);
            }
        }
        if (terrain.cached_chunk_count() > 8) throw std::runtime_error("Terrain cache exceeded its limit");
    }));
    wardogs::TerrainPackage warm(map.path);
    (void)warm.height_at(centre);
    scenarios.append(measure("warm_height_lookup", 20000, [&](Checksum& checksum) {
        for (int index = 0; index < 20000; ++index)
            checksum.add(warm.height_at({centre.x + (index % 101) * 0.001,
                                        centre.y + (index % 73) * 0.001}));
    }));
    const double half_distance_units = std::min(9.0, (x_max - x_min) * 0.2);
    const wardogs::Point base{centre.x - half_distance_units, centre.y};
    const wardogs::Point target{centre.x + half_distance_units, centre.y};
    const auto analysis = [&](wardogs::TerrainPackage& terrain, Checksum& checksum) {
        for (const auto arc : {wardogs::Arc::low, wardogs::Arc::high}) {
            wardogs::FiringAnalysisRequest request;
            request.base = base; request.target = target; request.arc = arc;
            request.terrain = [&](wardogs::Point point) { return terrain.height_at(point); };
            checksum_analysis(checksum, wardogs::analyze_firing(request));
        }
    };
    scenarios.append(measure("cold_two_arc_automatic_analysis", 1, [&](Checksum& checksum) {
        wardogs::TerrainPackage terrain(map.path);
        analysis(terrain, checksum);
    }));
    wardogs::TerrainPackage warmed_analysis(map.path);
    Checksum ignored;
    analysis(warmed_analysis, ignored);
    scenarios.append(measure("warm_two_arc_automatic_analysis", 1, [&](Checksum& checksum) {
        analysis(warmed_analysis, checksum);
    }));
    return {{"map_id", QString::fromStdString(map.spec.map_id)},
            {"package_sha256", QString::fromStdString(map.spec.sha256)}, {"scenarios", scenarios}};
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc > 2) {
        std::cerr << "usage: wardogs_calculation_benchmark [verified-terrain-directory]\n";
        return 2;
    }
    try {
        QJsonArray scenarios;
        scenarios.append(measure("calculate_shot", 20000, [](Checksum& checksum) {
            for (int index = 0; index < 20000; ++index) {
                const auto shot = wardogs::calculate_shot({50.25, 70.75},
                    {50.25 + (index % 701 - 350) * .01, 70.75 + (index % 907 - 453) * .01});
                checksum.add(shot.distance); checksum.add(shot.angle);
            }
        }));
        scenarios.append(measure("three_retained_table_lookups", 20000, [](Checksum& checksum) {
            for (int index = 0; index < 20000; ++index) {
                checksum.add(wardogs::mortar_mil_for_distance(132 + (index % 55200) * .01));
                checksum.add(wardogs::sph2_mil_for_distance(1181 + (index % 144800) * .01, wardogs::Arc::low));
                checksum.add(wardogs::sph2_mil_for_distance(735 + (index % 189400) * .01, wardogs::Arc::high));
            }
        }));
        scenarios.append(measure("two_arc_height_corrected_solutions", 10000, [](Checksum& checksum) {
            const wardogs::PlatformCalibration calibration{wardogs::identity_rotation(), 0};
            for (int index = 0; index < 10000; ++index) {
                for (const auto arc : {wardogs::Arc::low, wardogs::Arc::high}) {
                    const auto result = wardogs::corrected_solution({50, 50},
                        {50 + (index % 51) * .01, 68}, calibration, arc, index % 41 - 20.0);
                    checksum.add(result.bearing_deg); checksum.add(result.mil); checksum.add(result.reticle_distance_m);
                }
            }
        }));
        QJsonArray terrain;
        std::optional<double> verified_discovery_us;
        if (argc == 2) {
            const auto started = Clock::now();
            const auto discovery = wardogs::discover_terrain_maps(std::filesystem::path(argv[1]));
            verified_discovery_us = std::chrono::duration<double, std::micro>(Clock::now() - started).count();
            if (discovery.installed.size() != wardogs::official_terrain_maps().size() || !discovery.problems.empty())
                throw std::runtime_error("Benchmark directory must contain all three verified official maps");
            for (const auto& map : discovery.installed) terrain.append(terrain_benchmarks(map));
        }
        QJsonObject result{{"schema", "wardogs-calculation-benchmark-v1"},
                           {"scenarios", scenarios}, {"terrain", terrain},
                           {"application_started", false}, {"game_started", false},
                           {"persistent_data_written", false},
                           {"scope", "isolated CPU and read-only native terrain; not game responsiveness or hit accuracy"}};
        if (verified_discovery_us) result.insert("verified_discovery_us", *verified_discovery_us);
        std::cout << QJsonDocument(result).toJson(QJsonDocument::Indented).toStdString();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
