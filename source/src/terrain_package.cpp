#include "wardogs/terrain_package.hpp"

#include <Windows.h>
#include <bcrypt.h>
#include <zstd.h>

#include <QJsonDocument>
#include <QJsonObject>
#include <QCryptographicHash>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <list>
#include <limits>
#include <map>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace wardogs {
namespace {

constexpr std::array<char, 8> package_magic{'W', 'D', 'T', 'R', 'N', '2', 'M', '1'};
constexpr auto package_format = "wardogs-terrain-pack-v1";
constexpr std::uint32_t maximum_metadata_bytes = 1024 * 1024;
constexpr std::uint32_t maximum_chunk_count = 1'000'000;
constexpr int maximum_chunk_quads = 2048;
// Recognized local packages are below 30 MiB. Bound untrusted file hashing too,
// before parsing or checksum work can consume excessive startup resources.
constexpr std::uintmax_t maximum_recognized_package_bytes = 128ULL * 1024 * 1024;

struct HashHandles {
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    // CNG may retain this memory until BCryptDestroyHash has completed.
    std::vector<unsigned char> object;

    HashHandles() = default;
    HashHandles(const HashHandles&) = delete;
    HashHandles& operator=(const HashHandles&) = delete;

    ~HashHandles() {
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    }
};

template <typename Value>
Value read_little(std::istream& stream) {
    static_assert(std::is_integral_v<Value>);
    std::array<unsigned char, sizeof(Value)> bytes{};
    stream.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
    if (!stream) throw std::invalid_argument("Файл рельефа неожиданно закончился");
    std::make_unsigned_t<Value> result{};
    for (std::size_t index = 0; index < bytes.size(); ++index)
        result |= static_cast<std::make_unsigned_t<Value>>(bytes[index]) <<
                  (index * 8);
    return std::bit_cast<Value>(result);
}

std::vector<char> read_exact(std::istream& stream, std::size_t size) {
    std::vector<char> result(size);
    stream.read(result.data(), static_cast<std::streamsize>(size));
    if (!stream) throw std::invalid_argument("Файл рельефа неожиданно закончился");
    return result;
}

std::uint32_t crc32(std::span<const std::uint16_t> values) {
    static constexpr auto table = [] {
        std::array<std::uint32_t, 256> result{};
        for (std::uint32_t byte = 0; byte < result.size(); ++byte) {
            std::uint32_t remainder = byte;
            for (int bit = 0; bit < 8; ++bit)
                remainder = (remainder >> 1) ^
                    (0xedb88320U & (0U - (remainder & 1U)));
            result[byte] = remainder;
        }
        return result;
    }();
    std::uint32_t crc = 0xffffffffU;
    const auto* bytes = reinterpret_cast<const unsigned char*>(values.data());
    for (std::size_t index = 0; index < values.size_bytes(); ++index)
        crc = (crc >> 8) ^ table[(crc ^ bytes[index]) & 0xffU];
    return crc ^ 0xffffffffU;
}

std::wstring widen(std::string_view text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) return L"?";
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        result.data(), size);
    return result;
}

std::string json_string(const QJsonObject& object, const char* key) {
    const auto value = object.value(QString::fromLatin1(key));
    if (!value.isString()) throw std::invalid_argument("В метаданных рельефа отсутствует текстовое поле");
    const auto utf8 = value.toString().toUtf8();
    return {utf8.constData(), static_cast<std::size_t>(utf8.size())};
}

void require_plain_path(const std::filesystem::path& path) {
    auto ancestor = std::filesystem::absolute(path);
    while (!ancestor.empty()) {
        const auto attributes = GetFileAttributesW(ancestor.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::invalid_argument("Путь рельефа содержит ссылку или перенаправление");
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const auto error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
                throw std::system_error(static_cast<int>(error), std::system_category(),
                                        "Не удалось проверить путь рельефа");
        }
        const auto parent = ancestor.parent_path();
        if (parent == ancestor) break;
        ancestor = parent;
    }
}

double json_number(const QJsonObject& object, const char* key) {
    const auto value = object.value(QString::fromLatin1(key));
    if (!value.isDouble()) throw std::invalid_argument("В метаданных рельефа отсутствует числовое поле");
    const double result = value.toDouble();
    if (!std::isfinite(result))
        throw std::invalid_argument("Метаданные рельефа содержат нечисловое значение");
    return result;
}

int json_integer(const QJsonObject& object, const char* key,
                 int minimum = std::numeric_limits<int>::min(),
                 int maximum = std::numeric_limits<int>::max()) {
    const double number = json_number(object, key);
    if (number < minimum || number > maximum || std::trunc(number) != number)
        throw std::invalid_argument("Метаданные рельефа содержат неверное целое число");
    return static_cast<int>(number);
}

}  // namespace

struct TerrainPackage::Impl {
    using Key = std::pair<int, int>;
    struct Record {
        std::uint64_t offset{};
        std::uint32_t compressed_size{};
        std::uint32_t raw_size{};
        std::uint32_t checksum{};
    };
    struct CachedChunk {
        std::vector<std::uint16_t> values;
        std::list<Key>::iterator recency;
    };

    std::filesystem::path path;
    std::ifstream stream;
    std::string map_id;
    std::map<Key, Record> index;
    std::map<Key, CachedChunk> cache;
    std::list<Key> lru;
    std::size_t cache_limit{};
    int chunk_x_min{}, chunk_x_max{}, chunk_y_min{}, chunk_y_max{};
    int chunk_quads{}, vertices_per_side{};
    double global_quad_offset_x{}, global_quad_offset_y{};
    double factor_x{}, factor_y{};
    int height_base_decimeters{};
    double height_step_meters{};
    double coverage_x_min{}, coverage_x_max{}, coverage_y_min{}, coverage_y_max{};

    explicit Impl(const std::filesystem::path& source, std::size_t cache_chunks)
        : path(source), stream(source, std::ios::binary), cache_limit(cache_chunks) {
        if (!stream) throw std::invalid_argument("Не удалось открыть файл рельефа");
        if (cache_limit < 1 || cache_limit > 64)
            throw std::invalid_argument("Кэш рельефа должен содержать от 1 до 64 блоков");
        const auto file_size = std::filesystem::file_size(path);
        if (file_size < 16)
            throw std::invalid_argument("Файл рельефа обрезан");
        const auto magic = read_exact(stream, package_magic.size());
        if (!std::equal(magic.begin(), magic.end(), package_magic.begin()))
            throw std::invalid_argument("Неизвестный заголовок файла рельефа");
        const auto metadata_size = read_little<std::uint32_t>(stream);
        const auto chunk_count = read_little<std::uint32_t>(stream);
        if (metadata_size == 0 || metadata_size > maximum_metadata_bytes ||
            metadata_size > file_size - 16)
            throw std::invalid_argument("Неверный размер метаданных рельефа");
        if (chunk_count == 0 || chunk_count > maximum_chunk_count ||
            chunk_count > (file_size - 16 - metadata_size) / 16)
            throw std::invalid_argument("Неверное количество блоков рельефа");
        const auto metadata_bytes = read_exact(stream, metadata_size);
        QJsonParseError parse_error;
        const auto document = QJsonDocument::fromJson(
            QByteArray(metadata_bytes.data(), static_cast<qsizetype>(metadata_bytes.size())),
            &parse_error);
        if (parse_error.error != QJsonParseError::NoError || !document.isObject())
            throw std::invalid_argument("Некорректные метаданные рельефа");
        const auto metadata = document.object();
        if (json_string(metadata, "format") != package_format)
            throw std::invalid_argument("Формат рельефа не поддерживается");
        if (static_cast<std::uint32_t>(json_integer(
                metadata, "chunkCount", 1, maximum_chunk_count)) != chunk_count)
            throw std::invalid_argument("Количество блоков рельефа не совпадает");
        map_id = json_string(metadata, "mapId");
        if (map_id.empty() || map_id.size() > 128)
            throw std::invalid_argument("Неверное название карты в пакете рельефа");
        chunk_x_min = json_integer(metadata, "chunkXMin", -32768, 32767);
        chunk_x_max = json_integer(metadata, "chunkXMax", -32768, 32767);
        chunk_y_min = json_integer(metadata, "chunkYMin", -32768, 32767);
        chunk_y_max = json_integer(metadata, "chunkYMax", -32768, 32767);
        chunk_quads = json_integer(metadata, "chunkQuads", 1, maximum_chunk_quads);
        vertices_per_side = json_integer(metadata, "verticesPerSide", 2,
                                         maximum_chunk_quads + 1);
        if (chunk_x_min > chunk_x_max || chunk_y_min > chunk_y_max ||
            vertices_per_side != chunk_quads + 1 ||
            chunk_count > static_cast<std::uint64_t>(chunk_x_max - chunk_x_min + 1) *
                              (chunk_y_max - chunk_y_min + 1))
            throw std::invalid_argument("Неверная геометрия блоков рельефа");
        global_quad_offset_x = json_number(metadata, "globalQuadOffsetX");
        global_quad_offset_y = json_number(metadata, "globalQuadOffsetY");
        const auto shared_factor = metadata.value(QStringLiteral("gameUnitsToLandscapeQuads"));
        const auto factor = [&](const char* key) {
            if (metadata.contains(QString::fromLatin1(key))) return json_number(metadata, key);
            if (!shared_factor.isDouble())
                throw std::invalid_argument("В рельефе не задан масштаб координат");
            return json_number(metadata, "gameUnitsToLandscapeQuads");
        };
        factor_x = factor("gameUnitsToLandscapeQuadsX");
        factor_y = factor("gameUnitsToLandscapeQuadsY");
        height_base_decimeters = json_integer(metadata, "heightBaseDecimeters");
        height_step_meters = json_number(metadata, "heightStepMeters");
        const auto coverage = metadata.value(QStringLiteral("coverage")).toObject();
        coverage_x_min = json_number(coverage, "gameXMin");
        coverage_x_max = json_number(coverage, "gameXMax");
        coverage_y_min = json_number(coverage, "gameYMin");
        coverage_y_max = json_number(coverage, "gameYMax");
        if (factor_x == 0 || factor_y == 0 || height_step_meters <= 0 ||
            !std::isfinite(static_cast<double>(height_base_decimeters) *
                          height_step_meters) ||
            !std::isfinite((static_cast<double>(height_base_decimeters) + 65535) *
                          height_step_meters) ||
            coverage_x_min >= coverage_x_max || coverage_y_min >= coverage_y_max)
            throw std::invalid_argument("Неверный масштаб или границы рельефа");
        const auto covered_axis = [](double offset, double factor, double first,
                                     double last, int chunk_min, int chunk_max,
                                     int quads) {
            const double first_quad = offset + first * factor;
            const double last_quad = offset + last * factor;
            const double low = std::min(first_quad, last_quad);
            const double high = std::max(first_quad, last_quad);
            return std::isfinite(low) && std::isfinite(high) &&
                low >= static_cast<double>(chunk_min) * quads - 1e-6 &&
                high <= static_cast<double>(chunk_max + 1) * quads + 1e-6;
        };
        if (!covered_axis(global_quad_offset_x, factor_x, coverage_x_min,
                          coverage_x_max, chunk_x_min, chunk_x_max, chunk_quads) ||
            !covered_axis(global_quad_offset_y, factor_y, coverage_y_min,
                          coverage_y_max, chunk_y_min, chunk_y_max, chunk_quads))
            throw std::invalid_argument("Границы координат выходят за сетку рельефа");
        const auto expected_raw_size = static_cast<std::uint32_t>(
            static_cast<std::size_t>(vertices_per_side) * vertices_per_side *
            sizeof(std::uint16_t));

        for (std::uint32_t number = 0; number < chunk_count; ++number) {
            const int x = read_little<std::int16_t>(stream);
            const int y = read_little<std::int16_t>(stream);
            Record record;
            record.compressed_size = read_little<std::uint32_t>(stream);
            record.raw_size = read_little<std::uint32_t>(stream);
            record.checksum = read_little<std::uint32_t>(stream);
            record.offset = static_cast<std::uint64_t>(stream.tellg());
            if (x < chunk_x_min || x > chunk_x_max || y < chunk_y_min ||
                y > chunk_y_max || record.raw_size != expected_raw_size ||
                record.compressed_size == 0 ||
                record.compressed_size > expected_raw_size * 2ULL + 1024 ||
                record.offset > file_size ||
                record.compressed_size > file_size - record.offset)
                throw std::invalid_argument("Неверный размер, адрес или геометрия блока рельефа");
            if (!index.emplace(Key{x, y}, record).second)
                throw std::invalid_argument("В рельефе есть повторяющиеся блоки");
            stream.seekg(record.compressed_size, std::ios::cur);
            if (!stream) throw std::invalid_argument("Данные блока рельефа обрезаны");
        }
        const auto indexed_end = static_cast<std::uint64_t>(stream.tellg());
        if (indexed_end != file_size)
            throw std::invalid_argument("Файл рельефа содержит лишние или обрезанные данные");
    }

    const std::vector<std::uint16_t>& chunk(Key key) {
        if (const auto found = cache.find(key); found != cache.end()) {
            lru.splice(lru.end(), lru, found->second.recency);
            return found->second.values;
        }
        const auto found = index.find(key);
        if (found == index.end()) throw std::invalid_argument("Нужный блок рельефа отсутствует");
        const auto& record = found->second;
        stream.clear();
        stream.seekg(static_cast<std::streamoff>(record.offset));
        const auto compressed = read_exact(stream, record.compressed_size);
        std::vector<std::uint16_t> values(record.raw_size / sizeof(std::uint16_t));
        const std::size_t decoded = ZSTD_decompress(
            values.data(), values.size() * sizeof(std::uint16_t),
            compressed.data(), compressed.size());
        if (ZSTD_isError(decoded) || decoded != record.raw_size)
            throw std::invalid_argument("Не удалось распаковать блок рельефа");
        // Previous vertices have already been restored in row order, so the
        // prediction can be reversed in the decompression buffer itself.
        for (int y = 0; y < vertices_per_side; ++y) {
            for (int x = 0; x < vertices_per_side; ++x) {
                const auto offset = static_cast<std::size_t>(y * vertices_per_side + x);
                std::uint32_t restored = values[offset];
                if (x > 0) restored += values[offset - 1];
                if (y > 0) restored += values[offset - vertices_per_side];
                if (x > 0 && y > 0)
                    restored -= values[offset - vertices_per_side - 1];
                values[offset] = static_cast<std::uint16_t>(restored & 0xffffU);
            }
        }
        if (crc32(values) != record.checksum)
            throw std::invalid_argument("Контрольная сумма блока рельефа не совпадает");
        lru.push_back(key);
        std::map<Key, CachedChunk>::iterator inserted;
        try {
            inserted = cache.emplace(key, CachedChunk{
                std::move(values), std::prev(lru.end())}).first;
        } catch (...) {
            lru.pop_back();
            throw;
        }
        while (cache.size() > cache_limit) {
            cache.erase(lru.front());
            lru.pop_front();
        }
        return inserted->second.values;
    }
};

TerrainPackage::TerrainPackage(const std::filesystem::path& path,
                               std::size_t cache_chunks)
    : impl_(std::make_unique<Impl>(path, cache_chunks)) {}
TerrainPackage::~TerrainPackage() = default;
TerrainPackage::TerrainPackage(TerrainPackage&&) noexcept = default;
TerrainPackage& TerrainPackage::operator=(TerrainPackage&&) noexcept = default;

const std::string& TerrainPackage::map_id() const { return impl_->map_id; }
std::size_t TerrainPackage::cached_chunk_count() const { return impl_->cache.size(); }

std::optional<double> TerrainPackage::height_at(Point point) {
    auto& value = *impl_;
    if (!std::isfinite(point.x) || !std::isfinite(point.y))
        throw std::invalid_argument("Координаты рельефа должны быть конечными числами");
    if (point.x < value.coverage_x_min || point.x > value.coverage_x_max ||
        point.y < value.coverage_y_min || point.y > value.coverage_y_max)
        return std::nullopt;
    const double quad_x = value.global_quad_offset_x + point.x * value.factor_x;
    const double quad_y = value.global_quad_offset_y + point.y * value.factor_y;
    const int chunk_x = std::clamp(
        static_cast<int>(std::floor(quad_x / value.chunk_quads)), value.chunk_x_min,
        value.chunk_x_max);
    const int chunk_y = std::clamp(
        static_cast<int>(std::floor(quad_y / value.chunk_quads)), value.chunk_y_min,
        value.chunk_y_max);
    const double local_x = std::clamp(quad_x - chunk_x * value.chunk_quads, 0.0,
                                      static_cast<double>(value.chunk_quads));
    const double local_y = std::clamp(quad_y - chunk_y * value.chunk_quads, 0.0,
                                      static_cast<double>(value.chunk_quads));
    const auto& heights = value.chunk({chunk_x, chunk_y});
    const int maximum_vertex = value.vertices_per_side - 1;
    const int x0 = std::min(maximum_vertex, static_cast<int>(std::floor(local_x)));
    const int y0 = std::min(maximum_vertex, static_cast<int>(std::floor(local_y)));
    const int x1 = std::min(maximum_vertex, x0 + 1);
    const int y1 = std::min(maximum_vertex, y0 + 1);
    const double fx = local_x - x0;
    const double fy = local_y - y0;
    const auto at = [&](int x, int y) {
        return static_cast<double>(heights[static_cast<std::size_t>(
            y * value.vertices_per_side + x)]);
    };
    const double top = at(x0, y0) * (1 - fx) + at(x1, y0) * fx;
    const double bottom = at(x0, y1) * (1 - fx) + at(x1, y1) * fx;
    const double quantized = std::clamp(top * (1 - fy) + bottom * fy, 0.0, 65535.0);
    const double height = (static_cast<double>(value.height_base_decimeters) + quantized) *
                          value.height_step_meters;
    if (!std::isfinite(height))
        throw std::invalid_argument("Высота рельефа выходит за пределы конечных чисел");
    return height;
}

std::string sha256_file(const std::filesystem::path& path) {
    HashHandles handles;
    DWORD object_size{};
    DWORD result_size{};
    if (BCryptOpenAlgorithmProvider(&handles.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptGetProperty(handles.algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size),
                          &result_size, 0) < 0) {
        throw std::runtime_error("Не удалось инициализировать SHA-256");
    }
    handles.object.resize(object_size);
    if (BCryptCreateHash(handles.algorithm, &handles.hash, handles.object.data(), object_size, nullptr, 0, 0) < 0) {
        throw std::runtime_error("Не удалось создать SHA-256");
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Не удалось открыть рельеф для проверки");
    }
    std::vector<char> buffer(1024 * 1024);
    while (stream) {
        stream.read(buffer.data(), buffer.size());
        const auto count = stream.gcount();
        if (count > 0 && BCryptHashData(
                             handles.hash, reinterpret_cast<PUCHAR>(buffer.data()),
                             static_cast<ULONG>(count), 0) < 0) {
            throw std::runtime_error("Не удалось вычислить SHA-256");
        }
    }
    if (!stream.eof()) {
        throw std::runtime_error("Не удалось полностью прочитать файл рельефа");
    }
    std::array<unsigned char, 32> digest{};
    const auto status = BCryptFinishHash(
        handles.hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
    if (status < 0) throw std::runtime_error("Не удалось вычислить SHA-256");
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto byte : digest) output << std::setw(2) << static_cast<int>(byte);
    return output.str();
}

std::filesystem::path default_terrain_directory() {
    std::wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(),
                                            static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size())
        throw std::runtime_error("Не удалось определить папку приложения");
    executable.resize(length);
    return std::filesystem::path(executable).parent_path() / L"terrain-packs";
}

std::filesystem::path user_terrain_directory() {
    const auto directory = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (directory.isEmpty())
        throw std::runtime_error("Не удалось определить папку данных пользователя");
    return std::filesystem::path(directory.toStdWString()) /
        L"WardogsFireControl" / L"terrain-packs";
}

const std::vector<TerrainMapSpec>& official_terrain_maps() {
    static const std::vector<TerrainMapSpec> maps{
        {"bakurani", L"Bakurani", L"bakurani.wdt",
         "9c79af2f69df5023f6e2e944329ae80981432e7115832ae801da067bd79bfdde"},
        {"ozeti", L"Ozeti", L"ozeti.wdt",
         "f636225e89ffe19111da58466d4db60e4ccff285c3cfa11d8c30b959f544c400"},
        {"zestafona", L"Zestafona", L"zestafona.wdt",
         "e60f95a6e23791164342fe51b465ac04238f338f1f4293aef664b61513ecc048"},
    };
    return maps;
}

TerrainDiscovery discover_terrain_maps(const std::filesystem::path& directory,
                                        const std::vector<TerrainMapSpec>& specs) {
    TerrainDiscovery result;
    for (const auto& spec : specs) {
        const auto path = directory / spec.filename;
        try {
            if (spec.filename.empty() || spec.filename.has_parent_path() ||
                spec.filename.is_absolute() || spec.map_id.empty() ||
                spec.sha256.size() != 64 ||
                !std::all_of(spec.sha256.begin(), spec.sha256.end(), [](char value) {
                    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
                }))
                throw std::invalid_argument("Некорректное описание пакета рельефа");
            require_plain_path(path);
            if (!std::filesystem::is_regular_file(path)) {
                result.problems.push_back(spec.display_name + L": не установлен");
                continue;
            }
            if (std::filesystem::file_size(path) > maximum_recognized_package_bytes)
                throw std::invalid_argument("Размер рельефа превышает допустимый предел");
            if (sha256_file(path) != spec.sha256)
                throw std::invalid_argument("Контрольная сумма SHA-256 не совпадает");
            TerrainPackage package(path);
            if (package.map_id() != spec.map_id)
                throw std::invalid_argument("Название карты не совпадает");
            result.installed.push_back({spec, path});
        } catch (const std::exception& error) {
            result.problems.push_back(spec.display_name + L": проверка не пройдена (" +
                                      widen(error.what()) + L")");
        }
    }
    return result;
}

TerrainDiscovery discover_available_terrain_maps(
    const std::filesystem::path& application_directory,
    const std::filesystem::path& local_directory,
    const std::vector<TerrainMapSpec>& specs) {
    TerrainDiscovery result;
    for (const auto& spec : specs) {
        std::error_code application_path_error;
        const bool application_present = std::filesystem::exists(
            application_directory / spec.filename, application_path_error);
        const std::vector single{spec};
        const auto application = discover_terrain_maps(application_directory, single);
        if (!application.installed.empty()) {
            result.installed.push_back(application.installed.front());
            continue;
        }
        const auto local = discover_terrain_maps(local_directory, single);
        if (!local.installed.empty()) {
            result.installed.push_back(local.installed.front());
            // A corrupt portable package is still worth reporting when a valid
            // independent user copy restores this map's availability.
            if (application_present || application_path_error)
                result.problems.insert(result.problems.end(), application.problems.begin(),
                                       application.problems.end());
            continue;
        }
        result.problems.insert(result.problems.end(), local.problems.begin(), local.problems.end());
        if (application_present || application_path_error)
            result.problems.insert(result.problems.end(), application.problems.begin(),
                                   application.problems.end());
    }
    return result;
}

TerrainDiscovery install_terrain_maps(const std::filesystem::path& source_directory,
                                      const std::filesystem::path& destination_directory,
                                      const std::vector<TerrainMapSpec>& specs) {
    TerrainDiscovery result;
    const auto source = discover_terrain_maps(source_directory, specs);
    result.problems = source.problems;
    for (const auto& installed : source.installed) {
        try {
            const auto destination = destination_directory / installed.spec.filename;
            const std::vector single{installed.spec};
            const auto existing = discover_terrain_maps(destination_directory, single);
            if (!existing.installed.empty()) {
                result.installed.push_back(existing.installed.front());
                continue;
            }
            require_plain_path(destination);
            std::filesystem::create_directories(destination_directory);
            QSaveFile output(QString::fromStdWString(destination.native()));
            output.setDirectWriteFallback(false);
            if (!output.open(QIODevice::WriteOnly))
                throw std::runtime_error("Не удалось создать файл для установки рельефа");
            std::ifstream input(installed.path, std::ios::binary);
            if (!input) throw std::runtime_error("Исходный файл рельефа больше недоступен");
            std::array<char, 64 * 1024> buffer{};
            QCryptographicHash copied_hash(QCryptographicHash::Sha256);
            while (input) {
                input.read(buffer.data(), buffer.size());
                const auto count = input.gcount();
                if (count > 0 && output.write(buffer.data(), count) != count)
                    throw std::runtime_error("Не удалось полностью записать рельеф");
                if (count > 0) copied_hash.addData(QByteArrayView(buffer.data(), count));
            }
            if (!input.eof())
                throw std::runtime_error("Не удалось полностью прочитать исходный рельеф");
            // Hash the exact bytes being copied before commit. A concurrently
            // changed source cannot replace the previous destination package.
            if (copied_hash.result().toHex().toStdString() != installed.spec.sha256)
                throw std::runtime_error("Исходный рельеф изменился во время установки");
            if (!output.commit())
                throw std::runtime_error("Не удалось завершить установку рельефа");
            const auto verified = discover_terrain_maps(destination_directory, single);
            if (verified.installed.empty())
                throw std::runtime_error("Установленный рельеф не прошёл контрольную проверку");
            result.installed.push_back(verified.installed.front());
        } catch (const std::exception& error) {
            result.problems.push_back(installed.spec.display_name + L": установка не выполнена (" +
                                      widen(error.what()) + L")");
        }
    }
    return result;
}

}  // namespace wardogs
