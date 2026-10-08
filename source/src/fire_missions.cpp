#include "wardogs/fire_missions.hpp"

#include <Windows.h>
#include <ShlObj.h>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLockFile>
#include <QSaveFile>
#include <QSet>
#include <QString>
#include <QUuid>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <system_error>

namespace wardogs {
namespace {

constexpr auto format_identifier = "wardogs-fire-missions";
constexpr auto coordinate_encoding = "decimal-max-digits10";
constexpr int lock_timeout_ms = 1000;

QString qpath(const std::filesystem::path& path) {
    return QString::fromStdWString(path.native());
}

std::runtime_error file_error(const char* message, const QString& detail) {
    return std::runtime_error(std::string{message} + ": " + detail.toUtf8().toStdString());
}

std::filesystem::path checked_path(const std::filesystem::path& requested) {
    if (requested.empty() || requested.filename().empty() ||
        requested.filename() == L"." || requested.filename() == L"..")
        throw std::invalid_argument("Не задан файл сохранённых позиций и целей");
    const auto path = std::filesystem::absolute(requested).lexically_normal();
    for (auto ancestor = path; !ancestor.empty();) {
        const DWORD attributes = GetFileAttributesW(ancestor.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const auto error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
                throw std::system_error(static_cast<int>(error), std::system_category(),
                                        "Не удалось проверить путь хранилища позиций");
        } else if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            throw std::invalid_argument("Хранилище позиций не поддерживает ссылки и точки перенаправления");
        }
        const auto parent = ancestor.parent_path();
        if (parent == ancestor) break;
        ancestor = parent;
    }
    return path;
}

bool file_exists(const std::filesystem::path& path) {
    (void)checked_path(path);
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return false;
        throw std::system_error(static_cast<int>(error), std::system_category(),
                                "Не удалось прочитать хранилище позиций");
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        throw std::invalid_argument("Вместо файла сохранённых позиций обнаружена папка");
    return true;
}

QString weapon_key(FireMissionWeapon weapon) {
    switch (weapon) {
    case FireMissionWeapon::l81: return QStringLiteral("l81");
    case FireMissionWeapon::sph2: return QStringLiteral("sph2");
    }
    throw std::invalid_argument("Неизвестное орудие сохранённой позиции");
}

QString kind_key(FireMissionKind kind) {
    switch (kind) {
    case FireMissionKind::firing_position: return QStringLiteral("firing_position");
    case FireMissionKind::target: return QStringLiteral("target");
    }
    throw std::invalid_argument("Неизвестный тип сохранённой точки");
}

void validate_id(std::string_view id) {
    if (id.size() != 36) throw std::invalid_argument("Некорректный идентификатор сохранённой точки");
    const auto text = QString::fromLatin1(id.data(), static_cast<qsizetype>(id.size()));
    const QUuid uuid(text);
    if (uuid.isNull() || uuid.toString(QUuid::WithoutBraces) != text)
        throw std::invalid_argument("Некорректный идентификатор сохранённой точки");
}

QString normalized_name(std::wstring_view name) {
    const auto text = QString::fromStdWString(std::wstring{name});
    if (text.size() > static_cast<qsizetype>(maximum_fire_mission_name_length))
        throw std::invalid_argument("Имя сохранённой точки слишком длинное");
    for (qsizetype index = 0; index < text.size(); ++index) {
        const QChar character = text[index];
        if (character.category() == QChar::Other_Control ||
            character.category() == QChar::Other_Format)
            throw std::invalid_argument("Имя сохранённой точки содержит скрытые или управляющие символы");
        if (character.isHighSurrogate()) {
            if (index + 1 >= text.size() || !text[index + 1].isLowSurrogate())
                throw std::invalid_argument("Имя сохранённой точки содержит некорректный Unicode");
            ++index;
        } else if (character.isLowSurrogate()) {
            throw std::invalid_argument("Имя сохранённой точки содержит некорректный Unicode");
        }
    }
    const auto normalized = text.trimmed().normalized(QString::NormalizationForm_C);
    if (normalized.isEmpty()) throw std::invalid_argument("Задайте имя сохранённой точки");
    if (normalized.size() > static_cast<qsizetype>(maximum_fire_mission_name_length))
        throw std::invalid_argument("Имя сохранённой точки слишком длинное");
    return normalized;
}

QString point_component(double value) {
    if (!std::isfinite(value)) throw std::invalid_argument("Координаты сохранённой точки должны быть конечными числами");
    std::array<char, 64> result{};
    const auto encoded = std::to_chars(result.data(), result.data() + result.size(), value,
        std::chars_format::general, std::numeric_limits<double>::max_digits10);
    if (encoded.ec != std::errc{}) throw std::runtime_error("Не удалось сохранить точные координаты");
    return QString::fromLatin1(result.data(), static_cast<qsizetype>(encoded.ptr - result.data()));
}

double read_component(const QJsonValue& value) {
    if (!value.isString()) throw std::invalid_argument("Некорректный формат точных координат");
    const auto text = value.toString();
    if (text.isEmpty() || text.size() > 64) throw std::invalid_argument("Некорректный формат точных координат");
    const auto bytes = text.toLatin1();
    // Reject non-ASCII characters instead of allowing Latin-1 replacement.
    if (QString::fromLatin1(bytes) != text) throw std::invalid_argument("Некорректный формат точных координат");
    double result{};
    const auto parsed = std::from_chars(bytes.constData(), bytes.constData() + bytes.size(),
                                        result, std::chars_format::general);
    if (parsed.ec != std::errc{} || parsed.ptr != bytes.constData() + bytes.size() || !std::isfinite(result))
        throw std::invalid_argument("Некорректные координаты сохранённой точки");
    return result;
}

void require_keys(const QJsonObject& object, std::initializer_list<const char*> keys) {
    if (object.size() != static_cast<qsizetype>(keys.size()))
        throw std::invalid_argument("Хранилище точек содержит неподдерживаемые или отсутствующие поля");
    for (const auto key : keys)
        if (!object.contains(QLatin1String(key)))
            throw std::invalid_argument("В хранилище точек отсутствует обязательное поле");
}

QString name_identity(const SavedFireMission& mission) {
    return QString::fromStdWString(std::wstring{game_map_key(mission.map)}) + QLatin1Char('|') +
        weapon_key(mission.weapon) + QLatin1Char('|') + kind_key(mission.kind) + QLatin1Char('|') +
        normalized_name(mission.name).toCaseFolded();
}

void validate_collection(const std::vector<SavedFireMission>& missions) {
    if (missions.size() > maximum_saved_fire_missions)
        throw std::invalid_argument("Достигнут предел сохранённых позиций и целей");
    QSet<QString> identifiers;
    QSet<QString> names;
    for (const auto& mission : missions) {
        validate_fire_mission(mission);
        const auto id = QString::fromStdString(mission.id);
        if (identifiers.contains(id)) throw std::invalid_argument("Хранилище содержит повторяющиеся идентификаторы точек");
        identifiers.insert(id);
        const auto identity = name_identity(mission);
        if (names.contains(identity)) throw std::invalid_argument("Такое имя уже сохранено для этой карты, орудия и типа точки");
        names.insert(identity);
    }
}

std::vector<SavedFireMission> read_collection(const std::filesystem::path& path) {
    if (!file_exists(path)) return {};
    QFile file(qpath(path));
    if (!file.open(QIODevice::ReadOnly)) throw file_error("Не удалось открыть сохранённые позиции", file.errorString());
    if (file.size() < 0 || file.size() > static_cast<qint64>(maximum_fire_missions_file_bytes))
        throw std::invalid_argument("Файл сохранённых точек превышает предел 1 МиБ");
    const auto bytes = file.read(static_cast<qint64>(maximum_fire_missions_file_bytes) + 1);
    if (file.error() != QFileDevice::NoError) throw file_error("Не удалось прочитать сохранённые позиции", file.errorString());
    if (bytes.size() > static_cast<qsizetype>(maximum_fire_missions_file_bytes))
        throw std::invalid_argument("Файл сохранённых точек превышает предел 1 МиБ");
    QJsonParseError error{};
    const auto document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        throw std::invalid_argument("Файл сохранённых позиций повреждён: требуется корректный объект JSON");
    const auto root = document.object();
    require_keys(root, {"format", "version", "coordinate_encoding", "missions"});
    if (root.value(QStringLiteral("format")).toString() != QLatin1String(format_identifier) ||
        !root.value(QStringLiteral("version")).isDouble() ||
        root.value(QStringLiteral("version")).toDouble() != fire_missions_schema_version ||
        root.value(QStringLiteral("coordinate_encoding")).toString() != QLatin1String(coordinate_encoding) ||
        !root.value(QStringLiteral("missions")).isArray())
        throw std::invalid_argument("Версия или формат хранилища позиций не поддерживается");
    const auto array = root.value(QStringLiteral("missions")).toArray();
    if (array.size() > static_cast<qsizetype>(maximum_saved_fire_missions))
        throw std::invalid_argument("В хранилище слишком много сохранённых точек");
    std::vector<SavedFireMission> missions;
    missions.reserve(static_cast<std::size_t>(array.size()));
    for (const auto& entry : array) {
        if (!entry.isObject()) throw std::invalid_argument("Некорректная запись сохранённой точки");
        const auto object = entry.toObject();
        require_keys(object, {"id", "name", "map", "weapon", "kind", "point"});
        for (const auto key : {"id", "name", "map", "weapon", "kind"})
            if (!object.value(QLatin1String(key)).isString()) throw std::invalid_argument("Некорректная запись сохранённой точки");
        if (!object.value(QStringLiteral("point")).isObject()) throw std::invalid_argument("Некорректные координаты сохранённой точки");
        const auto point = object.value(QStringLiteral("point")).toObject();
        require_keys(point, {"x", "y"});
        const auto weapon = object.value(QStringLiteral("weapon")).toString();
        const auto kind = object.value(QStringLiteral("kind")).toString();
        if (weapon != QStringLiteral("l81") && weapon != QStringLiteral("sph2"))
            throw std::invalid_argument("Неизвестное орудие сохранённой точки");
        if (kind != QStringLiteral("firing_position") && kind != QStringLiteral("target"))
            throw std::invalid_argument("Неизвестный тип сохранённой точки");
        missions.push_back({object.value(QStringLiteral("id")).toString().toStdString(),
            object.value(QStringLiteral("name")).toString().toStdWString(),
            game_map_from_key(object.value(QStringLiteral("map")).toString().toStdWString()),
            weapon == QStringLiteral("l81") ? FireMissionWeapon::l81 : FireMissionWeapon::sph2,
            kind == QStringLiteral("target") ? FireMissionKind::target : FireMissionKind::firing_position,
            {read_component(point.value(QStringLiteral("x"))), read_component(point.value(QStringLiteral("y")))}});
    }
    validate_collection(missions);
    return missions;
}

void write_collection(const std::filesystem::path& path, const std::vector<SavedFireMission>& missions) {
    validate_collection(missions);
    QJsonArray array;
    for (const auto& mission : missions) {
        array.append(QJsonObject{{QStringLiteral("id"), QString::fromStdString(mission.id)},
            {QStringLiteral("name"), QString::fromStdWString(mission.name)},
            {QStringLiteral("map"), QString::fromStdWString(std::wstring{game_map_key(mission.map)})},
            {QStringLiteral("weapon"), weapon_key(mission.weapon)}, {QStringLiteral("kind"), kind_key(mission.kind)},
            {QStringLiteral("point"), QJsonObject{{QStringLiteral("x"), point_component(mission.point.x)},
                                                  {QStringLiteral("y"), point_component(mission.point.y)}}}});
    }
    const QJsonDocument document(QJsonObject{{QStringLiteral("format"), QLatin1String(format_identifier)},
        {QStringLiteral("version"), fire_missions_schema_version},
        {QStringLiteral("coordinate_encoding"), QLatin1String(coordinate_encoding)},
        {QStringLiteral("missions"), array}});
    const auto bytes = document.toJson(QJsonDocument::Indented);
    if (bytes.size() > static_cast<qsizetype>(maximum_fire_missions_file_bytes))
        throw std::invalid_argument("Файл сохранённых точек превышает предел 1 МиБ");
    (void)checked_path(path);
    QSaveFile output(qpath(path));
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly)) throw file_error("Не удалось подготовить сохранение позиций", output.errorString());
    if (output.write(bytes) != bytes.size()) {
        const auto detail = output.errorString();
        output.cancelWriting();
        throw file_error("Не удалось полностью записать сохранённые позиции", detail);
    }
    (void)checked_path(path);
    if (!output.commit()) throw file_error("Не удалось заменить файл сохранённых позиций", output.errorString());
}

class StoreLock {
public:
    explicit StoreLock(const std::filesystem::path& path)
        : lock_(qpath(path) + QStringLiteral(".lock")) {
        (void)checked_path(path);
        (void)checked_path(std::filesystem::path{path.native() + L".lock"});
        if (!QDir().mkpath(qpath(path.parent_path())))
            throw std::runtime_error("Не удалось создать папку сохранённых позиций");
        (void)checked_path(path);
        // Never steal a live transaction merely because storage is slow.
        // QLockFile can still identify a lock belonging to a process that exited.
        lock_.setStaleLockTime(0);
        if (!lock_.tryLock(lock_timeout_ms)) throw std::runtime_error("Хранилище позиций занято или недоступно; попробуйте ещё раз");
    }
private:
    QLockFile lock_;
};

}  // namespace

std::filesystem::path fire_missions_path() {
    PWSTR raw = nullptr;
    const auto result = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &raw);
    if (FAILED(result)) throw std::runtime_error("Не удалось найти папку профиля для сохранённых позиций");
    const std::filesystem::path directory{raw};
    CoTaskMemFree(raw);
    return directory / L"WardogsFireControl" / L"fire-missions.json";
}

void validate_fire_mission(const SavedFireMission& mission) {
    validate_id(mission.id);
    if (mission.map == GameMap::unselected || game_map_key(mission.map).empty())
        throw std::invalid_argument("Для сохранённой точки нужна выбранная карта");
    (void)weapon_key(mission.weapon);
    (void)kind_key(mission.kind);
    const auto name = normalized_name(mission.name);
    if (name != QString::fromStdWString(mission.name))
        throw std::invalid_argument("Имя сохранённой точки не соответствует формату хранилища");
    if (!std::isfinite(mission.point.x) || !std::isfinite(mission.point.y))
        throw std::invalid_argument("Координаты сохранённой точки должны быть конечными числами");
}

Point restore_fire_mission(const SavedFireMission& mission, GameMap confirmed_map,
                           FireMissionWeapon confirmed_weapon, bool map_is_confirmed) {
    validate_fire_mission(mission);
    (void)weapon_key(confirmed_weapon);
    if (!map_is_confirmed || confirmed_map == GameMap::unselected)
        throw std::invalid_argument("Сначала явно подтвердите текущую карту");
    if (confirmed_map != mission.map) throw std::invalid_argument("Сохранённая точка относится к другой карте");
    if (confirmed_weapon != mission.weapon) throw std::invalid_argument("Сохранённая точка относится к другому орудию");
    return mission.point;
}

FireMissionRepository::FireMissionRepository(std::filesystem::path path) : path_(checked_path(path)) {}

std::vector<SavedFireMission> FireMissionRepository::load() const { return read_collection(path_); }

SavedFireMission FireMissionRepository::create(GameMap map, FireMissionWeapon weapon,
                                              FireMissionKind kind, std::wstring name, Point point) const {
    SavedFireMission mission{QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(),
        normalized_name(name).toStdWString(), map, weapon, kind, point};
    validate_fire_mission(mission);
    StoreLock lock(path_);
    auto missions = read_collection(path_);
    missions.push_back(mission);
    write_collection(path_, missions);
    return mission;
}

SavedFireMission FireMissionRepository::update(std::string_view id, std::wstring name, Point point) const {
    validate_id(id);
    const auto normalized = normalized_name(name).toStdWString();
    if (!std::isfinite(point.x) || !std::isfinite(point.y))
        throw std::invalid_argument("Координаты сохранённой точки должны быть конечными числами");
    StoreLock lock(path_);
    auto missions = read_collection(path_);
    const auto found = std::find_if(missions.begin(), missions.end(), [&](const auto& mission) { return mission.id == id; });
    if (found == missions.end()) throw std::out_of_range("Сохранённая точка больше не существует");
    found->name = normalized;
    found->point = point;
    const auto updated = *found;
    write_collection(path_, missions);
    return updated;
}

bool FireMissionRepository::erase(std::string_view id) const {
    validate_id(id);
    if (!file_exists(path_)) return false;
    StoreLock lock(path_);
    auto missions = read_collection(path_);
    const auto found = std::find_if(missions.begin(), missions.end(), [&](const auto& mission) { return mission.id == id; });
    if (found == missions.end()) return false;
    missions.erase(found);
    write_collection(path_, missions);
    return true;
}

}  // namespace wardogs
