#include "wardogs/terrain_package.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

int wmain(int argc, wchar_t** argv) {
    const bool install = argc >= 2 && std::wstring_view(argv[1]) == L"--install";
    if ((!install && argc != 2) || (install && argc != 3 && argc != 4)) {
        std::cerr << "usage: terrain_probe <terrain-packs-directory>\n"
                     "       terrain_probe --install <local-source-directory> [destination-directory]\n";
        return 2;
    }
    try {
        const auto destination = install && argc == 3 ? wardogs::user_terrain_directory() :
            std::filesystem::path(argv[install ? 3 : 1]);
        const auto discovery = install ? wardogs::install_terrain_maps(argv[2], destination) :
            wardogs::discover_terrain_maps(destination);
        if (discovery.installed.size() != wardogs::official_terrain_maps().size())
            throw std::runtime_error("Не все поддерживаемые карты установлены и прошли проверку SHA-256");
        QJsonArray maps;
        for (const auto& installed : discovery.installed) {
            wardogs::TerrainPackage terrain(installed.path);
            std::ifstream input(installed.path, std::ios::binary);
            input.seekg(8);
            std::array<unsigned char, 4> bytes{};
            input.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
            std::uint32_t metadata_size{};
            for (unsigned index = 0; index < bytes.size(); ++index)
                metadata_size |= static_cast<std::uint32_t>(bytes[index]) << (index * 8);
            if (metadata_size == 0 || metadata_size > 1024 * 1024)
                throw std::runtime_error("Неверный размер метаданных карты");
            input.seekg(16);
            std::string encoded(metadata_size, '\0');
            input.read(encoded.data(), encoded.size());
            if (!input) throw std::runtime_error("Не удалось прочитать метаданные карты");
            const auto metadata = QJsonDocument::fromJson(QByteArray::fromStdString(encoded)).object();
            const auto coverage = metadata["coverage"].toObject();
            const double x_min = coverage["gameXMin"].toDouble();
            const double x_max = coverage["gameXMax"].toDouble();
            const double y_min = coverage["gameYMin"].toDouble();
            const double y_max = coverage["gameYMax"].toDouble();
            QJsonArray samples;
            constexpr std::array fractions{0.0, 0.12345, 0.5, 0.87654, 1.0};
            for (int y = 0; y < 5; ++y) {
                for (int x = 0; x < 5; ++x) {
                    const wardogs::Point point{
                        x_min + (x_max - x_min) * fractions[x],
                        y_min + (y_max - y_min) * fractions[y]};
                    const auto height = terrain.height_at(point);
                    if (!height || !std::isfinite(*height))
                        throw std::runtime_error("Не удалось прочитать конечную высоту на границе или внутри карты");
                    samples.append(QJsonObject{{"x", point.x}, {"y", point.y},
                                                {"height_m", *height}});
                }
            }
            if (terrain.height_at({x_min - 1, y_min}) ||
                terrain.height_at({x_max + 1, y_max}) ||
                terrain.height_at({x_min, y_min - 1}) ||
                terrain.height_at({x_max, y_max + 1}) ||
                terrain.cached_chunk_count() > 8)
                throw std::runtime_error("Нарушены границы карты или лимит кэша");
            maps.append(QJsonObject{
                {"map_id", QString::fromStdString(installed.spec.map_id)},
                {"sha256", QString::fromStdString(installed.spec.sha256)},
                {"metadata", metadata}, {"samples", samples},
                {"cached_chunks", static_cast<qint64>(terrain.cached_chunk_count())},
                {"outside_coverage_rejected", true}});
        }
        const QJsonObject report{{"all_official_digests_match", true},
                                  {"operation", install ? "verified-local-install" : "verify"},
                                  {"directory", QString::fromStdWString(destination.native())},
                                  {"maps", maps},
                                  {"scope", "package compatibility and bounded terrain sampling; not game hit accuracy"}};
        std::cout << QJsonDocument(report).toJson(QJsonDocument::Indented).toStdString();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
