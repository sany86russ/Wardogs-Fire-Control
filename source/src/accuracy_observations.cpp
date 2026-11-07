#include "wardogs/accuracy_observations.hpp"

#include "wardogs/fire_missions.hpp"
#include "wardogs/game_map.hpp"

#include <Windows.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLockFile>
#include <QSaveFile>
#include <QSet>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>

namespace wardogs {
namespace {

constexpr auto format_identifier = "wardogs-accuracy-observations";
constexpr auto decimal_encoding = "decimal-max-digits10";
constexpr std::int64_t maximum_timestamp_ms = 253402300799999; // end of year 9999
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
        throw std::invalid_argument("Accuracy observations require a file path");
    const auto path = std::filesystem::absolute(requested).lexically_normal();
    for (auto ancestor = path; !ancestor.empty();) {
        const DWORD attributes = GetFileAttributesW(ancestor.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const auto error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
                throw std::system_error(static_cast<int>(error), std::system_category(),
                                        "Cannot inspect accuracy observation path");
        } else if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            throw std::invalid_argument("Accuracy observation storage does not support redirected paths");
        }
        const auto parent = ancestor.parent_path();
        if (parent == ancestor) break;
        ancestor = parent;
    }
    return path;
}

bool file_exists(const std::filesystem::path& path) {
    (void)checked_path(path);
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return false;
        throw std::system_error(static_cast<int>(error), std::system_category(),
                                "Cannot inspect accuracy observations");
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        throw std::invalid_argument("Accuracy observation file is a directory");
    return true;
}

void require_keys(const QJsonObject& object, std::initializer_list<const char*> keys) {
    if (object.size() != static_cast<qsizetype>(keys.size()))
        throw std::invalid_argument("Accuracy observations contain unsupported or missing fields");
    for (const auto key : keys)
        if (!object.contains(QLatin1String(key)))
            throw std::invalid_argument("Accuracy observations have a missing field");
}

void validate_token(const std::string& value) {
    if (value.empty() || value.size() > 128)
        throw std::invalid_argument("Accuracy observation metadata is empty or too long");
    for (const unsigned char ch : value) {
        const bool valid = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == ':';
        if (!valid) throw std::invalid_argument("Accuracy observation metadata is not a bounded identifier");
    }
}

QString decimal(double value) {
    if (!std::isfinite(value)) throw std::invalid_argument("Accuracy observation contains a non-finite value");
    std::array<char, 64> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
        std::chars_format::general, std::numeric_limits<double>::max_digits10);
    if (result.ec != std::errc{}) throw std::runtime_error("Cannot encode accuracy observation value");
    return QString::fromLatin1(buffer.data(), static_cast<qsizetype>(result.ptr - buffer.data()));
}

double number(const QJsonValue& value) {
    if (!value.isString()) throw std::invalid_argument("Accuracy observation value is not exact decimal text");
    const auto text = value.toString();
    if (text.isEmpty() || text.size() > 64) throw std::invalid_argument("Invalid accuracy observation value");
    const auto bytes = text.toLatin1();
    if (QString::fromLatin1(bytes) != text) throw std::invalid_argument("Invalid accuracy observation value");
    double result{};
    const auto parsed = std::from_chars(bytes.constData(), bytes.constData() + bytes.size(), result,
                                        std::chars_format::general);
    if (parsed.ec != std::errc{} || parsed.ptr != bytes.constData() + bytes.size() || !std::isfinite(result))
        throw std::invalid_argument("Invalid accuracy observation value");
    return result;
}

QString token(const QJsonObject& object, const char* key) {
    const auto value = object.value(QLatin1String(key));
    if (!value.isString()) throw std::invalid_argument("Invalid accuracy observation metadata");
    const auto text = value.toString();
    const auto bytes = text.toLatin1();
    if (QString::fromLatin1(bytes) != text) throw std::invalid_argument("Invalid accuracy observation metadata");
    const auto result = bytes.toStdString();
    validate_token(result);
    return text;
}

bool boolean(const QJsonObject& object, const char* key) {
    const auto value = object.value(QLatin1String(key));
    if (!value.isBool()) throw std::invalid_argument("Invalid accuracy observation verification flag");
    return value.toBool();
}

QString identity(const AccuracyObservation& value) {
    // Identifiers cannot contain '|', so distinct pairs cannot collide.
    return QString::fromStdString(value.session_id) + QLatin1Char('|') +
        QString::fromStdString(value.observation_id);
}

QJsonObject point_json(Point point) {
    return {{QStringLiteral("x"), decimal(point.x)}, {QStringLiteral("y"), decimal(point.y)}};
}

Point read_point(const QJsonValue& value) {
    if (!value.isObject()) throw std::invalid_argument("Invalid accuracy observation point");
    const auto object = value.toObject();
    require_keys(object, {"x", "y"});
    return {number(object.value(QStringLiteral("x"))), number(object.value(QStringLiteral("y")))};
}

QJsonObject record_json(const AccuracyObservation& value) {
    const auto text = [](const std::string& input) { return QString::fromStdString(input); };
    return {{QStringLiteral("session_id"), text(value.session_id)},
        {QStringLiteral("observation_id"), text(value.observation_id)},
        {QStringLiteral("command_id"), text(value.command_id)},
        {QStringLiteral("station_id"), text(value.station_id)},
        {QStringLiteral("recorded_at_utc_ms"), QString::number(value.recorded_at_utc_ms)},
        {QStringLiteral("map"), text(value.map_id)}, {QStringLiteral("weapon"), QStringLiteral("sph2")},
        {QStringLiteral("base"), point_json(value.base)}, {QStringLiteral("target"), point_json(value.firing.target)},
        {QStringLiteral("impact"), point_json(value.impact)},
        {QStringLiteral("arc"), value.firing.arc == Arc::low ? QStringLiteral("low") : QStringLiteral("high")},
        {QStringLiteral("bearing_deg"), decimal(value.firing.bearing_deg)},
        {QStringLiteral("mil"), decimal(value.firing.mil)},
        {QStringLiteral("target_height_delta_m"), decimal(value.firing.target_height_delta_m)},
        {QStringLiteral("impact_height_delta_m"), decimal(value.impact_height_delta_m)},
        {QStringLiteral("command_source"), value.command_source == AccuracyCommandSource::displayed_unverified
            ? QStringLiteral("displayed_unverified") : QStringLiteral("hud_observed")},
        {QStringLiteral("sight_verified"), value.sight_verified},
        {QStringLiteral("discharge_observed"), value.discharge_observed},
        {QStringLiteral("physical_shot_verified"), false},
        {QStringLiteral("app_version"), text(value.app_version)},
        {QStringLiteral("table_profile"), text(value.table_profile)},
        {QStringLiteral("game_version"), text(value.game_version)},
        {QStringLiteral("height_source"), text(value.height_source)},
        {QStringLiteral("terrain_profile"), text(value.terrain_profile)}};
}

AccuracyObservation read_record(const QJsonValue& value) {
    if (!value.isObject()) throw std::invalid_argument("Invalid accuracy observation record");
    const auto object = value.toObject();
    auto required_fields = object;
    required_fields.remove(QStringLiteral("height_source"));
    required_fields.remove(QStringLiteral("terrain_profile"));
    require_keys(required_fields, {"session_id", "observation_id", "command_id", "station_id", "recorded_at_utc_ms",
        "map", "weapon", "base", "target", "impact", "arc", "bearing_deg", "mil", "target_height_delta_m",
        "impact_height_delta_m", "command_source", "sight_verified", "discharge_observed",
        "physical_shot_verified", "app_version", "table_profile", "game_version"});
    AccuracyObservation result;
    result.session_id = token(object, "session_id").toStdString();
    result.observation_id = token(object, "observation_id").toStdString();
    result.command_id = token(object, "command_id").toStdString();
    result.station_id = token(object, "station_id").toStdString();
    result.map_id = token(object, "map").toStdString();
    if (token(object, "weapon") != QStringLiteral("sph2"))
        throw std::invalid_argument("Unsupported accuracy observation weapon");
    const auto timestamp = token(object, "recorded_at_utc_ms").toLatin1();
    const auto parsed = std::from_chars(timestamp.constData(), timestamp.constData() + timestamp.size(),
                                        result.recorded_at_utc_ms);
    if (parsed.ec != std::errc{} || parsed.ptr != timestamp.constData() + timestamp.size() ||
        result.recorded_at_utc_ms <= 0)
        throw std::invalid_argument("Invalid accuracy observation timestamp");
    result.base = read_point(object.value(QStringLiteral("base")));
    result.firing.target = read_point(object.value(QStringLiteral("target")));
    result.impact = read_point(object.value(QStringLiteral("impact")));
    const auto arc = token(object, "arc");
    if (arc != QStringLiteral("low") && arc != QStringLiteral("high"))
        throw std::invalid_argument("Invalid accuracy observation arc");
    result.firing.arc = arc == QStringLiteral("low") ? Arc::low : Arc::high;
    result.firing.bearing_deg = number(object.value(QStringLiteral("bearing_deg")));
    result.firing.mil = number(object.value(QStringLiteral("mil")));
    result.firing.target_height_delta_m = number(object.value(QStringLiteral("target_height_delta_m")));
    result.impact_height_delta_m = number(object.value(QStringLiteral("impact_height_delta_m")));
    const auto source = token(object, "command_source");
    if (source != QStringLiteral("displayed_unverified") && source != QStringLiteral("hud_observed"))
        throw std::invalid_argument("Invalid accuracy observation command source");
    result.command_source = source == QStringLiteral("displayed_unverified")
        ? AccuracyCommandSource::displayed_unverified : AccuracyCommandSource::hud_observed;
    result.sight_verified = boolean(object, "sight_verified");
    result.discharge_observed = boolean(object, "discharge_observed");
    if (boolean(object, "physical_shot_verified"))
        throw std::invalid_argument("Physical-shot verification is unavailable for automatic observations");
    result.app_version = token(object, "app_version").toStdString();
    result.table_profile = token(object, "table_profile").toStdString();
    result.game_version = token(object, "game_version").toStdString();
    if (object.contains(QStringLiteral("height_source")))
        result.height_source = token(object, "height_source").toStdString();
    if (object.contains(QStringLiteral("terrain_profile")))
        result.terrain_profile = token(object, "terrain_profile").toStdString();
    validate_accuracy_observation(result);
    return result;
}

std::vector<AccuracyObservation> read_collection(const std::filesystem::path& path) {
    if (!file_exists(path)) return {};
    QFile file(qpath(path));
    if (!file.open(QIODevice::ReadOnly)) throw file_error("Cannot open accuracy observations", file.errorString());
    if (file.size() < 0 || file.size() > static_cast<qint64>(maximum_accuracy_observations_file_bytes))
        throw std::invalid_argument("Accuracy observation store exceeds 8 MiB");
    const auto bytes = file.read(static_cast<qint64>(maximum_accuracy_observations_file_bytes) + 1);
    if (file.error() != QFileDevice::NoError) throw file_error("Cannot read accuracy observations", file.errorString());
    if (bytes.size() > static_cast<qsizetype>(maximum_accuracy_observations_file_bytes))
        throw std::invalid_argument("Accuracy observation store exceeds 8 MiB");
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        throw std::invalid_argument("Accuracy observation store is corrupt");
    const auto root = document.object();
    require_keys(root, {"format", "version", "numeric_encoding", "observations"});
    if (root.value(QStringLiteral("format")).toString() != QLatin1String(format_identifier) ||
        !root.value(QStringLiteral("version")).isDouble() ||
        root.value(QStringLiteral("version")).toDouble() != accuracy_observations_schema_version ||
        root.value(QStringLiteral("numeric_encoding")).toString() != QLatin1String(decimal_encoding) ||
        !root.value(QStringLiteral("observations")).isArray())
        throw std::invalid_argument("Unsupported accuracy observation schema");
    const auto entries = root.value(QStringLiteral("observations")).toArray();
    if (entries.size() > static_cast<qsizetype>(maximum_accuracy_observations))
        throw std::invalid_argument("Accuracy observation store exceeds 4096 records");
    std::vector<AccuracyObservation> result;
    result.reserve(static_cast<std::size_t>(entries.size()));
    QSet<QString> identifiers;
    for (const auto& entry : entries) {
        auto observation = read_record(entry);
        const auto id = identity(observation);
        if (identifiers.contains(id)) throw std::invalid_argument("Duplicate accuracy observation identity");
        identifiers.insert(id);
        result.push_back(std::move(observation));
    }
    return result;
}

void write_collection(const std::filesystem::path& path, const std::vector<AccuracyObservation>& records) {
    QJsonArray entries;
    for (const auto& record : records) entries.append(record_json(record));
    const auto bytes = QJsonDocument(QJsonObject{{QStringLiteral("format"), QLatin1String(format_identifier)},
        {QStringLiteral("version"), accuracy_observations_schema_version},
        {QStringLiteral("numeric_encoding"), QLatin1String(decimal_encoding)},
        {QStringLiteral("observations"), entries}}).toJson(QJsonDocument::Compact);
    if (bytes.size() > static_cast<qsizetype>(maximum_accuracy_observations_file_bytes))
        throw std::invalid_argument("Accuracy observation store exceeds 8 MiB");
    (void)checked_path(path);
    QSaveFile output(qpath(path));
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly)) throw file_error("Cannot prepare accuracy observations", output.errorString());
    if (output.write(bytes) != bytes.size()) {
        const auto detail = output.errorString();
        output.cancelWriting();
        throw file_error("Cannot write complete accuracy observations", detail);
    }
    (void)checked_path(path);
    if (!output.commit()) throw file_error("Cannot commit accuracy observations", output.errorString());
}

class StoreLock {
public:
    explicit StoreLock(const std::filesystem::path& path)
        : lock_(qpath(path) + QStringLiteral(".lock")) {
        (void)checked_path(path);
        (void)checked_path(std::filesystem::path{path.native() + L".lock"});
        if (!QDir().mkpath(qpath(path.parent_path())))
            throw std::runtime_error("Cannot create accuracy observation directory");
        (void)checked_path(path);
        lock_.setStaleLockTime(0);
        if (!lock_.tryLock(lock_timeout_ms)) throw std::runtime_error("Accuracy observation store is busy or inaccessible");
    }
private:
    QLockFile lock_;
};

} // namespace

bool AccuracyObservation::operator==(const AccuracyObservation& other) const {
    return session_id == other.session_id && observation_id == other.observation_id &&
        command_id == other.command_id && station_id == other.station_id && map_id == other.map_id &&
        base == other.base && firing.target == other.firing.target && firing.arc == other.firing.arc &&
        firing.bearing_deg == other.firing.bearing_deg && firing.mil == other.firing.mil &&
        firing.target_height_delta_m == other.firing.target_height_delta_m && impact == other.impact &&
        impact_height_delta_m == other.impact_height_delta_m && command_source == other.command_source &&
        sight_verified == other.sight_verified && discharge_observed == other.discharge_observed &&
        app_version == other.app_version && table_profile == other.table_profile &&
        game_version == other.game_version && height_source == other.height_source &&
        terrain_profile == other.terrain_profile && recorded_at_utc_ms == other.recorded_at_utc_ms;
}

std::filesystem::path accuracy_observations_path() {
    return fire_missions_path().parent_path() / L"accuracy-observations.json";
}

void validate_accuracy_observation(const AccuracyObservation& value) {
    for (const auto* field : {&value.session_id, &value.observation_id, &value.command_id, &value.station_id,
                             &value.map_id, &value.app_version, &value.table_profile, &value.game_version,
                             &value.height_source, &value.terrain_profile})
        validate_token(*field);
    if (value.height_source != "unknown" && value.height_source != "terrain" &&
        value.height_source != "assumed_level")
        throw std::invalid_argument("Unknown accuracy observation height source");
    const auto map = game_map_from_key(QString::fromStdString(value.map_id).toStdWString());
    if (map == GameMap::unselected) throw std::invalid_argument("Unknown accuracy observation map");
    for (const double number : {value.base.x, value.base.y, value.firing.target.x, value.firing.target.y,
                               value.impact.x, value.impact.y, value.firing.bearing_deg, value.firing.mil,
                               value.firing.target_height_delta_m, value.impact_height_delta_m})
        if (!std::isfinite(number)) throw std::invalid_argument("Accuracy observation contains a non-finite value");
    if (value.firing.bearing_deg < 0.0 || value.firing.bearing_deg >= 360.0)
        throw std::invalid_argument("Accuracy observation bearing is outside [0, 360)");
    if (value.firing.arc != Arc::low && value.firing.arc != Arc::high)
        throw std::invalid_argument("Invalid accuracy observation arc");
    (void)sph2_distance_for_mil(value.firing.mil, value.firing.arc);
    if (value.command_source != AccuracyCommandSource::displayed_unverified &&
        value.command_source != AccuracyCommandSource::hud_observed)
        throw std::invalid_argument("Invalid accuracy observation command source");
    if (value.sight_verified != (value.command_source == AccuracyCommandSource::hud_observed) ||
        (value.discharge_observed && !value.sight_verified))
        throw std::invalid_argument("Accuracy observation verification flags contradict their source");
    if (value.recorded_at_utc_ms < 0 || value.recorded_at_utc_ms > maximum_timestamp_ms)
        throw std::invalid_argument("Invalid accuracy observation timestamp");
    // A durable landing must contain usable finite geometry, without changing
    // the main correction's acceptance policy or fabricating physical proof.
    const auto target = calculate_shot(value.base, value.firing.target);
    const auto impact = calculate_shot(value.base, value.impact);
    if (target.distance <= 0.0 || impact.distance <= 0.0)
        throw std::invalid_argument("Accuracy observation landing or target coincides with the gun");
}

AccuracyObservationRepository::AccuracyObservationRepository(std::filesystem::path path)
    : path_(checked_path(path)) {}

std::vector<AccuracyObservation> AccuracyObservationRepository::load() const {
    return read_collection(path_);
}

bool AccuracyObservationRepository::append(AccuracyObservation observation) const {
    return append_many(std::span<const AccuracyObservation>{&observation, 1}) != 0;
}

std::size_t AccuracyObservationRepository::append_many(std::span<const AccuracyObservation> observations) const {
    if (observations.empty()) return 0;
    if (observations.size() > maximum_accuracy_observations)
        throw std::invalid_argument("Accuracy observation batch exceeds 4096 records");
    for (const auto& observation : observations) validate_accuracy_observation(observation);
    StoreLock lock(path_);
    auto records = read_collection(path_);
    std::size_t added{};
    for (auto observation : observations) {
        const auto found = std::find_if(records.begin(), records.end(), [&](const auto& existing) {
            return existing.session_id == observation.session_id &&
                existing.observation_id == observation.observation_id;
        });
        if (found != records.end()) {
            if (observation.recorded_at_utc_ms == 0) observation.recorded_at_utc_ms = found->recorded_at_utc_ms;
            if (observation != *found) throw std::invalid_argument("Conflicting accuracy observation identity");
            continue;
        }
        if (observation.recorded_at_utc_ms == 0)
            observation.recorded_at_utc_ms = QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();
        validate_accuracy_observation(observation);
        records.push_back(std::move(observation));
        ++added;
    }
    if (added == 0) return 0;
    if (records.size() > maximum_accuracy_observations)
        records.erase(records.begin(), records.end() - static_cast<std::ptrdiff_t>(maximum_accuracy_observations));
    write_collection(path_, records);
    return added;
}

struct AccuracyObservationWriter::Impl {
    AccuracyObservationRepository repository;
    Completion completion;
    mutable std::mutex mutex;
    std::condition_variable ready;
    std::deque<AccuracyObservation> pending;
    bool stopping{};
    std::string failure;
    std::jthread worker;

    Impl(std::filesystem::path path, Completion callback)
        : repository(std::move(path)), completion(std::move(callback)) {
        worker = std::jthread([this] { run(); });
    }

    void notify(std::string message) noexcept {
        if (!completion) return;
        try { completion(std::move(message)); }
        catch (...) {
            // A notification bug must neither terminate a disk worker nor be
            // silently ignored: callers can still inspect the failure state.
            std::lock_guard lock(mutex);
            if (failure.empty()) failure = "Accuracy observation completion callback failed";
            stopping = true;
        }
    }

    void run() {
        for (;;) {
            AccuracyObservation next;
            {
                std::unique_lock lock(mutex);
                ready.wait(lock, [this] { return stopping || !pending.empty(); });
                if (pending.empty()) return;
                next = std::move(pending.front());
                pending.pop_front();
            }
            try { (void)repository.append(std::move(next)); }
            catch (const std::exception& error) {
                std::string message;
                {
                    std::lock_guard lock(mutex);
                    failure = std::string{error.what()} + "; unsaved observations=" +
                        std::to_string(pending.size() + 1);
                    message = failure;
                    stopping = true;
                    pending.clear();
                }
                notify(std::move(message));
                return;
            }
            notify({});
        }
    }
};

AccuracyObservationWriter::AccuracyObservationWriter(std::filesystem::path path, Completion completion)
    : impl_(std::make_unique<Impl>(std::move(path), std::move(completion))) {}

AccuracyObservationWriter::~AccuracyObservationWriter() { stop(); }

bool AccuracyObservationWriter::submit(AccuracyObservation observation) {
    validate_accuracy_observation(observation);
    std::lock_guard lock(impl_->mutex);
    if (impl_->stopping || !impl_->failure.empty() || impl_->pending.size() >= maximum_pending_accuracy_observations)
        return false;
    impl_->pending.push_back(std::move(observation));
    impl_->ready.notify_one();
    return true;
}

void AccuracyObservationWriter::stop() {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->stopping = true;
    }
    impl_->ready.notify_one();
    if (impl_->worker.joinable()) impl_->worker.join();
}

std::string AccuracyObservationWriter::error() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->failure;
}

} // namespace wardogs
