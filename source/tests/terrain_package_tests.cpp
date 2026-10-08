#include "wardogs/terrain_package.hpp"

#include <Windows.h>

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void close(double actual, double expected, const char* message) {
    check(std::abs(actual - expected) < 1e-8, message);
}

std::uint32_t little32(const std::vector<char>& bytes, std::size_t offset) {
    std::uint32_t result{};
    for (unsigned index = 0; index < 4; ++index)
        result |= static_cast<std::uint32_t>(
            static_cast<unsigned char>(bytes.at(offset + index))) << (8 * index);
    return result;
}

void write32(std::vector<char>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned index = 0; index < 4; ++index)
        bytes.at(offset + index) = static_cast<char>((value >> (8 * index)) & 0xff);
}

std::vector<char> metadata_change(const std::vector<char>& original,
                                  const std::function<void(QJsonObject&)>& change) {
    const auto metadata_size = little32(original, 8);
    auto metadata = QJsonDocument::fromJson(
        QByteArray(original.data() + 16, metadata_size)).object();
    change(metadata);
    const auto encoded = QJsonDocument(metadata).toJson(QJsonDocument::Compact);
    std::vector<char> result(original.begin(), original.begin() + 16);
    write32(result, 8, static_cast<std::uint32_t>(encoded.size()));
    result.insert(result.end(), encoded.begin(), encoded.end());
    result.insert(result.end(), original.begin() + 16 + metadata_size, original.end());
    return result;
}

void rejects(const std::function<void()>& operation, const char* message) {
    try {
        operation();
        check(false, message);
    } catch (const std::invalid_argument&) {
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    const std::filesystem::path fixture{WARDOGS_TERRAIN_TEST_PACKAGE};
    const auto digest = wardogs::sha256_file(fixture);
    check(digest.size() == 64, "SHA-256 is returned as 64 hexadecimal digits");

    wardogs::TerrainPackage terrain(fixture, 1);
    check(terrain.map_id() == "test-map", "package map id is read");
    check(terrain.cached_chunk_count() == 0, "package starts without decoded chunks");
    const auto first = terrain.height_at({0.5, 0.5});
    check(first.has_value(), "first covered point has terrain height");
    close(*first, -8.0, "first chunk is decoded and bilinearly sampled");
    check(terrain.cached_chunk_count() == 1, "only requested chunk is cached");
    const auto second = terrain.height_at({2.5, 0.5});
    check(second.has_value(), "second covered point has terrain height");
    close(*second, 2.0, "second chunk is decoded and bilinearly sampled");
    check(terrain.cached_chunk_count() == 1, "LRU cache honors its chunk limit");
    for (int repeat = 0; repeat < 50; ++repeat) {
        close(*terrain.height_at({2.5, 0.5}), 2.0,
              "repeated terrain cache hits preserve the decoded vertex grid");
        check(terrain.cached_chunk_count() == 1,
              "cache hits cannot create duplicate recency entries or grow the cache");
    }
    close(*terrain.height_at({0.5, 0.5}), -8.0,
          "evicted chunks reload correctly after repeated cache hits");
    close(*terrain.height_at({2.5, 0.5}), 2.0,
          "in-place residual decoding preserves second-chunk heights after eviction");
    check(!terrain.height_at({5, 1}), "point outside coverage has no height");
    rejects([&] { (void)terrain.height_at({std::numeric_limits<double>::quiet_NaN(), 0}); },
            "NaN coordinates cannot reach integer indexing");
    rejects([&] { (void)terrain.height_at({0, std::numeric_limits<double>::infinity()}); },
            "infinite coordinates cannot reach integer indexing");
    rejects([&] { wardogs::TerrainPackage invalid(fixture, 0); },
            "zero cache capacity is rejected");
    rejects([&] { wardogs::TerrainPackage invalid(fixture, 65); },
            "unbounded terrain cache capacity is rejected");

    const auto temporary = std::filesystem::temp_directory_path() /
        (L"wardogs-terrain-test-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(temporary);
    const auto valid_path = temporary / L"valid.wdt";
    const auto damaged_path = temporary / L"damaged.wdt";
    std::filesystem::copy_file(fixture, valid_path,
                               std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file(fixture, damaged_path,
                               std::filesystem::copy_options::overwrite_existing);
    std::ofstream(damaged_path, std::ios::binary | std::ios::app).put('x');

    const std::vector specs{
        wardogs::TerrainMapSpec{"test-map", L"有效", L"valid.wdt", digest},
        wardogs::TerrainMapSpec{"test-map", L"损坏", L"damaged.wdt", digest},
        wardogs::TerrainMapSpec{"missing", L"缺失", L"missing.wdt", digest},
    };
    const auto discovery = wardogs::discover_terrain_maps(temporary, specs);
    check(discovery.installed.size() == 1,
          "discovery exposes only verified terrain packages");
    check(discovery.problems.size() == 2,
          "discovery reports damaged and missing packages");

    const std::vector install_specs{specs.front()};
    const auto cache_directory = temporary / L"user-cache";
    const auto installed = wardogs::install_terrain_maps(temporary, cache_directory, install_specs);
    check(installed.installed.size() == 1 && installed.problems.empty(),
          "local import installs only a verified package into the independent user cache");
    check(wardogs::sha256_file(cache_directory / L"valid.wdt") == digest,
          "installed bytes retain the recognized SHA-256 digest");
    const auto repeated = wardogs::install_terrain_maps(temporary, cache_directory, install_specs);
    check(repeated.installed.size() == 1 && repeated.problems.empty(),
          "repeated local import safely preserves an already valid package");
    const auto absent_application = temporary / L"absent-app";
    const auto available = wardogs::discover_available_terrain_maps(
        absent_application, cache_directory, install_specs);
    check(available.installed.size() == 1 && available.problems.empty() &&
          available.installed.front().path == cache_directory / L"valid.wdt",
          "user terrain survives an application upgrade without bundled data");
    const auto preferred = wardogs::discover_available_terrain_maps(
        temporary, cache_directory, install_specs);
    check(preferred.installed.size() == 1 &&
          preferred.installed.front().path == valid_path,
          "a verified portable package takes precedence over the user cache");
    const auto corrupt_source = temporary / L"bad-source";
    std::filesystem::create_directories(corrupt_source);
    std::filesystem::copy_file(damaged_path, corrupt_source / L"valid.wdt",
                               std::filesystem::copy_options::overwrite_existing);
    const auto refused = wardogs::install_terrain_maps(corrupt_source, cache_directory, install_specs);
    check(refused.installed.empty() && refused.problems.size() == 1 &&
          wardogs::sha256_file(cache_directory / L"valid.wdt") == digest,
          "a corrupt source cannot replace the valid user package");
    const auto fallback = wardogs::discover_available_terrain_maps(
        corrupt_source, cache_directory, install_specs);
    check(fallback.installed.size() == 1 && fallback.problems.size() == 1 &&
          fallback.installed.front().path == cache_directory / L"valid.wdt",
          "corrupt portable data is reported while independent verified data remains available");
    const std::vector wrong_map_specs{
        wardogs::TerrainMapSpec{"another-map", L"wrong", L"valid.wdt", digest}};
    const auto wrong_map = wardogs::install_terrain_maps(temporary, cache_directory, wrong_map_specs);
    check(wrong_map.installed.empty() && wrong_map.problems.size() == 1,
          "a matching digest does not bypass the selected map identifier");
    const std::vector traversal_specs{
        wardogs::TerrainMapSpec{"test-map", L"escape", L"../valid.wdt", digest}};
    const auto traversal = wardogs::install_terrain_maps(cache_directory, cache_directory, traversal_specs);
    check(traversal.installed.empty() && traversal.problems.size() == 1,
          "package descriptions cannot escape the chosen import directory");
    const auto repaired = wardogs::install_terrain_maps(temporary, corrupt_source, install_specs);
    check(repaired.installed.size() == 1 && repaired.problems.empty() &&
          wardogs::sha256_file(corrupt_source / L"valid.wdt") == digest,
          "a valid local import can atomically repair a corrupt recognized package");

    std::ifstream input(fixture, std::ios::binary);
    const std::vector<char> original((std::istreambuf_iterator<char>(input)),
                                    std::istreambuf_iterator<char>());
    const auto record_start = 16 + little32(original, 8);
    const auto invalid_path = temporary / L"malformed.wdt";
    const auto malformed = [&](const std::vector<char>& bytes, const char* message) {
        {
            std::ofstream output(invalid_path, std::ios::binary | std::ios::trunc);
            output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }
        rejects([&] {
            wardogs::TerrainPackage package(invalid_path);
            (void)package.height_at({0.5, 0.5});
        }, message);
    };
    auto changed = original;
    write32(changed, 8, std::numeric_limits<std::uint32_t>::max());
    malformed(changed, "untrusted metadata size is bounded before allocation");
    changed = original;
    write32(changed, 12, std::numeric_limits<std::uint32_t>::max());
    malformed(changed, "untrusted record count is bounded before indexing");
    changed = original;
    write32(changed, record_start + 8, 19);
    malformed(changed, "odd raw size is rejected before decompression can overwrite memory");
    changed = original;
    write32(changed, record_start + 8, 2);
    malformed(changed, "raw size smaller than the vertex grid is rejected");
    changed = original;
    write32(changed, record_start + 4, 0);
    malformed(changed, "empty compressed block is rejected");
    changed = original;
    write32(changed, record_start + 4, std::numeric_limits<std::uint32_t>::max());
    malformed(changed, "compressed block cannot point beyond end of file");
    changed = original;
    changed.at(record_start) = 2;
    malformed(changed, "chunk coordinates must belong to the declared grid");
    changed = original;
    const auto second_record = record_start + 16 + little32(original, record_start + 4);
    changed.at(second_record) = 0;
    malformed(changed, "duplicate chunk coordinates are rejected");
    changed = original;
    changed.at(record_start + 12) ^= 1;
    malformed(changed, "decoded vertex data must match its CRC");
    changed = original;
    changed.at(record_start + 16) = 0;
    malformed(changed, "invalid compressed frame is reported");
    changed = original;
    changed.pop_back();
    malformed(changed, "truncated final block is detected at package load");
    changed = original;
    changed.push_back('x');
    malformed(changed, "trailing payload is rejected");
    malformed(metadata_change(original, [](auto& metadata) {
        metadata["verticesPerSide"] = 2;
    }), "vertex dimensions must match the declared quads");
    malformed(metadata_change(original, [](auto& metadata) {
        metadata["chunkQuads"] = 0;
    }), "zero quads cannot cause division by zero");
    malformed(metadata_change(original, [](auto& metadata) {
        metadata["chunkQuads"] = 2.5;
    }), "fractional grid dimensions are rejected");
    malformed(metadata_change(original, [](auto& metadata) {
        metadata["heightBaseDecimeters"] = -65535;
        metadata["heightStepMeters"] = 1e308;
    }), "zero upper height endpoint cannot conceal overflowing lower terrain heights");
    malformed(metadata_change(original, [](auto& metadata) {
        metadata["chunkXMin"] = -1e100;
    }), "out-of-range floating metadata cannot overflow integer conversion");
    malformed(metadata_change(original, [](auto& metadata) {
        metadata["gameUnitsToLandscapeQuadsX"] = 0;
    }), "zero coordinate scale is rejected");
    malformed(metadata_change(original, [](auto& metadata) {
        metadata.remove("gameUnitsToLandscapeQuadsX");
    }), "missing coordinate scale cannot silently become zero");
    malformed(metadata_change(original, [](auto& metadata) {
        metadata["heightStepMeters"] = -0.1;
    }), "negative height quantization step is rejected");
    malformed(metadata_change(original, [](auto& metadata) {
        auto coverage = metadata["coverage"].toObject();
        coverage["gameXMax"] = 5;
        metadata["coverage"] = coverage;
    }), "coverage cannot silently clamp to a distant grid edge");
    const auto mirrored = metadata_change(original, [](auto& metadata) {
        metadata["gameUnitsToLandscapeQuadsY"] = -1;
        metadata["globalQuadOffsetY"] = 2;
    });
    {
        std::ofstream output(invalid_path, std::ios::binary | std::ios::trunc);
        output.write(mirrored.data(), static_cast<std::streamsize>(mirrored.size()));
    }
    {
        wardogs::TerrainPackage mirrored_terrain(invalid_path);
        close(*mirrored_terrain.height_at({0.5, 0.5}),
              *terrain.height_at({0.5, 1.5}),
              "negative coordinate scale preserves the mirrored Y axis used by official maps");
        close(*mirrored_terrain.height_at({0, 2}), *terrain.height_at({0, 0}),
              "mirrored coverage endpoints sample the correct boundary vertex");
    }
    std::error_code ignored;
    std::filesystem::remove_all(temporary, ignored);

    if (failures) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All terrain package tests passed\n";
    return 0;
}
