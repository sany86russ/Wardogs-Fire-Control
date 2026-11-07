#include "wardogs/accuracy_observations.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QTemporaryDir>

#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

namespace {

int checks{}, failures{};

void check(bool condition, const char* message) {
    ++checks;
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}

template <typename Action>
void rejects(Action&& action, const char* message) {
    bool failed = false;
    try { action(); } catch (const std::exception&) { failed = true; }
    check(failed, message);
}

QByteArray bytes(const std::filesystem::path& path) {
    QFile file(QString::fromStdWString(path.native()));
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read isolated fixture");
    return file.readAll();
}

void write_bytes(const std::filesystem::path& path, const QByteArray& data) {
    if (!QDir().mkpath(QString::fromStdWString(path.parent_path().native())))
        throw std::runtime_error("Cannot create isolated fixture directory");
    QFile file(QString::fromStdWString(path.native()));
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size())
        throw std::runtime_error("Cannot write isolated fixture");
}

wardogs::AccuracyObservation observation(std::string id = "1") {
    wardogs::AccuracyObservation value;
    value.session_id = "session-1";
    value.observation_id = std::move(id);
    value.command_id = "command-1";
    value.station_id = "station-1";
    value.map_id = "bakurani";
    value.base = {std::nextafter(95.25, 100.0), -0.0};
    value.firing = {{95.25, 18.0}, wardogs::Arc::high, 0.1, 1123.0, -12.25};
    value.impact = {95.12, 17.97};
    value.impact_height_delta_m = -11.75;
    value.app_version = "2.14.1";
    return value;
}

void mutate_root(const std::filesystem::path& path,
                 const std::function<void(QJsonObject&)>& mutation) {
    auto root = QJsonDocument::fromJson(bytes(path)).object();
    mutation(root);
    write_bytes(path, QJsonDocument(root).toJson(QJsonDocument::Compact));
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    using namespace wardogs;
    namespace fs = std::filesystem;
    QTemporaryDir temporary(QDir::tempPath() + QStringLiteral("/wardogs-accuracy-observations-XXXXXX"));
    if (!temporary.isValid()) { std::cerr << "Cannot create isolated test directory\n"; return 1; }
    const fs::path root{temporary.path().toStdWString()};
    try {
        const auto default_path = accuracy_observations_path();
        check(default_path.filename() == L"accuracy-observations.json" &&
              default_path.parent_path().filename() == L"WardogsFireControl",
              "automatic observations belong to the existing profile outside the portable application");

        const auto path = root / L"profile" / L"accuracy-observations.json";
        AccuracyObservationRepository repository(path);
        check(repository.load().empty() && !fs::exists(path.parent_path()),
              "a missing store loads empty without creating directories or a replacement file");
        auto first = observation();
        const auto before_time = QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();
        check(repository.append(first), "a marked landing is persisted automatically");
        const auto loaded = repository.load();
        first.recorded_at_utc_ms = loaded.front().recorded_at_utc_ms;
        check(loaded.size() == 1 && loaded.front() == first &&
              first.recorded_at_utc_ms >= before_time &&
              first.recorded_at_utc_ms <= QDateTime::currentDateTimeUtc().toMSecsSinceEpoch(),
              "roundtrip retains context and assigns an honest UTC receipt time");
        check(std::bit_cast<std::uint64_t>(loaded.front().base.x) == std::bit_cast<std::uint64_t>(first.base.x) &&
              std::bit_cast<std::uint64_t>(loaded.front().base.y) == std::bit_cast<std::uint64_t>(-0.0),
              "exact decimal serialization preserves precise coordinates and negative zero");
        const auto before_duplicate = bytes(path);
        check(!repository.append(observation()) && bytes(path) == before_duplicate,
              "same identity and values with automatic timestamp are idempotent without rewriting");
        auto conflict = observation();
        conflict.impact.x += 0.01;
        rejects([&] { (void)repository.append(conflict); }, "conflicting duplicate identity is rejected");
        check(bytes(path) == before_duplicate, "a conflicting duplicate cannot alter the existing store");

        auto second = observation("2");
        second.command_source = AccuracyCommandSource::hud_observed;
        second.sight_verified = true;
        second.discharge_observed = true;
        second.game_version = "known-test-patch";
        second.height_source = "terrain";
        second.terrain_profile = std::string(64, 'a');
        check(repository.append(second), "explicit HUD provenance stays separate from assumed displayed commands");
        const auto stored_second = repository.load().back();
        second.recorded_at_utc_ms = stored_second.recorded_at_utc_ms;
        check(stored_second == second,
              "explicit provenance and commanded firing snapshot survive roundtrip");
        const auto json = QJsonDocument::fromJson(bytes(path)).object();
        const auto records = json.value(QStringLiteral("observations")).toArray();
        const auto first_json = records.at(0).toObject();
        check(json.value(QStringLiteral("version")).toInt() == accuracy_observations_schema_version &&
              json.value(QStringLiteral("numeric_encoding")).toString() == QStringLiteral("decimal-max-digits10") &&
              first_json.value(QStringLiteral("game_version")).toString() == QStringLiteral("unknown") &&
              !first_json.value(QStringLiteral("sight_verified")).toBool() &&
              !first_json.value(QStringLiteral("discharge_observed")).toBool() &&
              !first_json.value(QStringLiteral("physical_shot_verified")).toBool(),
              "schema separates unknown patch and assumed commands from actual evidence");
        check(first_json.size() == 24 && !first_json.contains(QStringLiteral("raw_ocr")) &&
              !first_json.contains(QStringLiteral("screenshot")) && !first_json.contains(QStringLiteral("chat")),
              "automatic archive has only the declared structured fields and no raw screen or chat data");
        check(first_json.value(QStringLiteral("height_source")).toString() == QStringLiteral("unknown") &&
              first_json.value(QStringLiteral("terrain_profile")).toString() == QStringLiteral("unknown") &&
              stored_second.height_source == "terrain" && stored_second.terrain_profile == std::string(64, 'a'),
              "height provenance and package identity are preserved without guessing missing terrain data");

        const auto legacy_path = root / L"legacy-schema-1.json";
        auto legacy_root = QJsonDocument::fromJson(before_duplicate).object();
        auto legacy_entries = legacy_root.value(QStringLiteral("observations")).toArray();
        auto legacy_entry = legacy_entries.at(0).toObject();
        legacy_entry.remove(QStringLiteral("height_source"));
        legacy_entry.remove(QStringLiteral("terrain_profile"));
        legacy_entries[0] = legacy_entry;
        legacy_root.insert(QStringLiteral("observations"), legacy_entries);
        write_bytes(legacy_path, QJsonDocument(legacy_root).toJson(QJsonDocument::Compact));
        AccuracyObservationRepository legacy(legacy_path);
        check(legacy.load().front() == first && legacy.append(observation("new")) &&
              legacy.load().front().height_source == "unknown" &&
              legacy.load().front().terrain_profile == "unknown",
              "older schema-1 records retain exact data and explicitly unknown new height provenance");

        for (int index = 0; index < 12; ++index) {
            auto invalid = observation("invalid");
            switch (index) {
            case 0: invalid.base.x = std::numeric_limits<double>::quiet_NaN(); break;
            case 1: invalid.firing.mil = 1.0; break;
            case 2: invalid.firing.bearing_deg = 360.0; break;
            case 3: invalid.map_id = "unknown-map"; break;
            case 4: invalid.session_id = "raw chat is not metadata"; break;
            case 5: invalid.game_version.clear(); break;
            case 6: invalid.sight_verified = true; break;
            case 7: invalid.discharge_observed = true; break;
            case 8: invalid.recorded_at_utc_ms = -1; break;
            case 9: invalid.impact = invalid.base; break;
            case 10: invalid.height_source = "guessed"; break;
            case 11: invalid.terrain_profile = "C:/private/package.wdt"; break;
            }
            const auto before = bytes(path);
            rejects([&] { (void)repository.append(invalid); }, "invalid observation is rejected before persistence");
            check(bytes(path) == before, "invalid data never mutates the previous archive");
        }

        const auto corrupt_path = root / L"corrupt.json";
        for (const auto& data : {QByteArray{"not JSON"}, QByteArray{"{}"}, QByteArray{"[]"}}) {
            write_bytes(corrupt_path, data);
            AccuracyObservationRepository corrupt(corrupt_path);
            rejects([&] { (void)corrupt.load(); }, "corrupt and unsupported files are rejected on load");
            rejects([&] { (void)corrupt.append(observation()); }, "corrupt files are not replaced by append");
            check(bytes(corrupt_path) == data, "unknown source bytes remain untouched after failure");
        }
        for (int index = 0; index < 5; ++index) {
            write_bytes(corrupt_path, before_duplicate);
            mutate_root(corrupt_path, [=](QJsonObject& document) {
                auto entries = document.value(QStringLiteral("observations")).toArray();
                auto entry = entries.at(0).toObject();
                switch (index) {
                case 0: document.insert(QStringLiteral("version"), 99); break;
                case 1: entry.insert(QStringLiteral("raw_ocr"), QStringLiteral("unrecognised future field")); break;
                case 2: entry.insert(QStringLiteral("physical_shot_verified"), true); break;
                case 3: entries.append(entry); break;
                case 4: entry.insert(QStringLiteral("sight_verified"), QStringLiteral("false")); break;
                }
                entries[0] = entry;
                document.insert(QStringLiteral("observations"), entries);
            });
            const auto corrupt_bytes = bytes(corrupt_path);
            AccuracyObservationRepository corrupt(corrupt_path);
            rejects([&] { (void)corrupt.append(observation("3")); },
                    "future fields, duplicate IDs and false verification claims fail closed");
            check(bytes(corrupt_path) == corrupt_bytes, "failed validation cannot discard unknown fields or records");
        }
        write_bytes(corrupt_path, QByteArray(static_cast<qsizetype>(maximum_accuracy_observations_file_bytes + 1), ' '));
        rejects([&] { (void)AccuracyObservationRepository(corrupt_path).load(); },
                "oversized archive is rejected before unbounded parsing");

        const auto bounded_path = root / L"bounded.json";
        AccuracyObservationRepository bounded(bounded_path);
        std::vector<AccuracyObservation> batch;
        batch.reserve(maximum_accuracy_observations);
        for (std::size_t index = 0; index < maximum_accuracy_observations; ++index)
            batch.push_back(observation(std::to_string(index)));
        check(bounded.append_many(batch) == maximum_accuracy_observations &&
              bounded.load().size() == maximum_accuracy_observations,
              "a bounded batch can fill the automatic history without per-record rewrites");
        check(bounded.append(observation("next")), "new observations remain admissible after the retention bound");
        const auto retained = bounded.load();
        check(retained.size() == maximum_accuracy_observations && retained.front().observation_id == "1" &&
              retained.back().observation_id == "next" &&
              bytes(bounded_path).size() <= static_cast<qsizetype>(maximum_accuracy_observations_file_bytes),
              "automatic retention evicts only the oldest observation and keeps bounded structured history");

        const auto locked_path = root / L"locked.json";
        QLockFile external(QString::fromStdWString(locked_path.native()) + QStringLiteral(".lock"));
        external.setStaleLockTime(0);
        if (!external.tryLock()) throw std::runtime_error("Cannot hold isolated test lock");
        rejects([&] { (void)AccuracyObservationRepository(locked_path).append(observation()); },
                "a live writer lock is not stolen");
        check(!fs::exists(locked_path), "a busy store does not create a partial archive");
        external.unlock();

        const auto simultaneous_path = root / L"simultaneous.json";
        std::atomic_int transaction_errors{};
        auto append_range = [&](int begin) {
            try {
                AccuracyObservationRepository simultaneous(simultaneous_path);
                for (int index = begin; index < begin + 6; ++index)
                    (void)simultaneous.append(observation(std::to_string(index)));
            } catch (...) { ++transaction_errors; }
        };
        std::thread first_writer(append_range, 10), second_writer(append_range, 20);
        first_writer.join(); second_writer.join();
        check(transaction_errors == 0 && AccuracyObservationRepository(simultaneous_path).load().size() == 12,
              "two serialized repository writers do not lose each other's observations");

        const auto async_path = root / L"async.json";
        std::atomic_int async_success{}, async_failures{};
        AccuracyObservationWriter writer(async_path, [&](std::string error) {
            if (error.empty()) ++async_success; else ++async_failures;
        });
        for (int index = 0; index < 16; ++index)
            check(writer.submit(observation(std::to_string(index))), "nonblocking writer accepts a normal bounded burst");
        writer.stop();
        writer.stop();
        check(AccuracyObservationRepository(async_path).load().size() == 16 && async_success == 16 &&
              async_failures == 0 && writer.error().empty() && !writer.submit(observation("late")),
              "clean stop flushes accepted observations, joins once and rejects later submissions");

        const auto broken_async_path = root / L"broken-async.json";
        const QByteArray broken_bytes{"unrecognised archive"};
        write_bytes(broken_async_path, broken_bytes);
        std::string reported_failure;
        AccuracyObservationWriter failed_writer(broken_async_path, [&](std::string error) { reported_failure = std::move(error); });
        check(failed_writer.submit(observation()), "a queued write can expose an existing storage failure asynchronously");
        failed_writer.stop();
        check(!reported_failure.empty() && !failed_writer.error().empty() &&
              reported_failure.find("unsaved observations=") != std::string::npos &&
              !failed_writer.submit(observation("later")) && bytes(broken_async_path) == broken_bytes,
              "permanent storage failure is reported, blocks later submissions and preserves corrupt bytes");

        const auto backpressure_path = root / L"backpressure.json";
        QLockFile block(QString::fromStdWString(backpressure_path.native()) + QStringLiteral(".lock"));
        block.setStaleLockTime(0);
        if (!block.tryLock()) throw std::runtime_error("Cannot hold isolated backpressure lock");
        AccuracyObservationWriter limited_writer(backpressure_path);
        std::size_t admitted{};
        const auto started = std::chrono::steady_clock::now();
        for (std::size_t index = 0; index < maximum_pending_accuracy_observations + 4; ++index)
            if (limited_writer.submit(observation(std::to_string(index)))) ++admitted;
        const auto submission_time = std::chrono::steady_clock::now() - started;
        check(admitted >= maximum_pending_accuracy_observations &&
              admitted <= maximum_pending_accuracy_observations + 1 &&
              submission_time < std::chrono::milliseconds(500),
              "bounded queue rejects excess work promptly while disk storage is busy");
        block.unlock();
        limited_writer.stop();
        check(limited_writer.error().empty() && AccuracyObservationRepository(backpressure_path).load().size() == admitted,
              "releasing storage contention lets clean shutdown flush every admitted record");
    } catch (const std::exception& error) {
        ++failures;
        std::cerr << "Unexpected fixture failure: " << error.what() << '\n';
    }
    std::cout << checks << " accuracy observation checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
