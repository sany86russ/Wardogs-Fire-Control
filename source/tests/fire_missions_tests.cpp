#include "wardogs/fire_missions.hpp"

#include <Windows.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QTemporaryDir>
#include <QUuid>

#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

namespace {

int checks{};
int failures{};

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
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("test fixture cannot be read");
    return file.readAll();
}

void write_bytes(const std::filesystem::path& path, const QByteArray& data) {
    QDir().mkpath(QString::fromStdWString(path.parent_path().native()));
    QFile file(QString::fromStdWString(path.native()));
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size())
        throw std::runtime_error("test fixture cannot be written");
}

bool exact(double first, double second) {
    return std::bit_cast<std::uint64_t>(first) == std::bit_cast<std::uint64_t>(second);
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Wardogs fire mission tests"));
    using namespace wardogs;
    namespace fs = std::filesystem;
    QTemporaryDir temporary(QDir::tempPath() + QStringLiteral("/wardogs-fire-missions-tests-XXXXXX"));
    if (!temporary.isValid()) { std::cerr << "Cannot create an isolated test directory\n"; return 1; }
    temporary.setAutoRemove(false);
    const fs::path root{temporary.path().toStdWString()};
    try {
        const auto stable = fire_missions_path();
        check(stable.filename() == L"fire-missions.json" && stable.parent_path().filename() == L"WardogsFireControl",
              "default storage belongs to the user profile outside the portable installation");
        const auto path = root / L"profile" / L"fire-missions.json";
        const FireMissionRepository repository(path);
        check(repository.load().empty() && !fs::exists(path.parent_path()),
              "loading a missing store does not create directories or an empty database");

        const Point base{std::nextafter(102.47, 200.0), std::nextafter(119.47, 0.0)};
        const auto position = repository.create(GameMap::bakurani, FireMissionWeapon::l81,
            FireMissionKind::firing_position, L"  Северная позиция  ", base);
        check(position.name == L"Северная позиция" && !position.id.empty(),
              "a new named position receives a persistent identity and a trimmed Unicode name");
        const auto target = repository.create(GameMap::bakurani, FireMissionWeapon::l81,
            FireMissionKind::target, L"Мост / Bridge", {-0.0, std::nextafter(0.005, 1.0)});
        const auto second_map = repository.create(GameMap::ozeti, FireMissionWeapon::l81,
            FireMissionKind::target, L"Мост / Bridge", {97.45123456789012, 116.73000000000002});
        const auto second_weapon = repository.create(GameMap::bakurani, FireMissionWeapon::sph2,
            FireMissionKind::target, L"Мост / Bridge", {98.5, 118.9});
        auto loaded = repository.load();
        check(loaded.size() == 4 && loaded[0] == position && loaded[1] == target &&
              loaded[2] == second_map && loaded[3] == second_weapon,
              "records round trip with map, weapon, point kind, identity, Unicode name and exact point");
        check(exact(loaded[0].point.x, base.x) && exact(loaded[0].point.y, base.y) &&
              exact(loaded[1].point.x, -0.0) && exact(loaded[1].point.y, target.point.y),
              "persistence does not round coordinates and preserves negative zero");
        const auto root_json = QJsonDocument::fromJson(bytes(path)).object();
        check(root_json.value(QStringLiteral("version")).toInt() == 1 &&
              root_json.value(QStringLiteral("coordinate_encoding")).toString() == QStringLiteral("decimal-max-digits10") &&
              root_json.value(QStringLiteral("missions")).toArray()[0].toObject().value(QStringLiteral("point")).toObject()
                  .value(QStringLiteral("x")).isString(),
              "the persistent document is versioned and declares its exact coordinate encoding");

        const auto extremes = repository.create(GameMap::training, FireMissionWeapon::sph2,
            FireMissionKind::firing_position, L"Finite limits", {std::numeric_limits<double>::max(),
                                                                std::numeric_limits<double>::denorm_min()});
        const auto extremes_loaded = repository.load().back();
        check(exact(extremes_loaded.point.x, extremes.point.x) && exact(extremes_loaded.point.y, extremes.point.y),
              "all finite double values including subnormals survive the exact decimal round trip");

        const auto updated = repository.update(position.id, L"Западная позиция", {102.98765432109876, 118.0});
        check(updated.id == position.id && updated.map == position.map && updated.weapon == position.weapon &&
              updated.kind == position.kind && updated.point.x == 102.98765432109876 &&
              repository.load()[0] == updated,
              "editing changes only the chosen record's name and precise coordinates");
        const auto before_duplicate = bytes(path);
        rejects([&] { (void)repository.create(GameMap::bakurani, FireMissionWeapon::l81,
            FireMissionKind::target, L"мост / bridge", {1.0, 2.0}); },
            "case-insensitive duplicate names in one map, weapon and point kind are rejected");
        check(bytes(path) == before_duplicate, "duplicate creation cannot alter the previous collection");
        const auto other_position = repository.create(GameMap::bakurani, FireMissionWeapon::l81,
            FireMissionKind::firing_position, L"Резерв", {100.0, 110.0});
        const auto before_edit = bytes(path);
        rejects([&] { (void)repository.update(other_position.id, L"западная позиция", {2.0, 3.0}); },
            "rename collisions do not silently overwrite another position");
        check(bytes(path) == before_edit, "rejected editing preserves all stored bytes");

        const auto missing_id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
        rejects([&] { (void)repository.update(missing_id, L"Missing", {1.0, 2.0}); },
            "editing a concurrently deleted identity fails honestly");
        check(!repository.erase(missing_id) && bytes(path) == before_edit,
              "deleting an absent identity reports false and leaves the database unchanged");
        check(repository.erase(other_position.id) && !repository.erase(other_position.id),
              "deletion is persistent and cannot recreate an already-deleted record");
        check(repository.load().size() == 5, "deletion preserves all other maps and weapons");

        const auto restored = restore_fire_mission(updated, GameMap::bakurani, FireMissionWeapon::l81, true);
        check(exact(restored.x, updated.point.x) && exact(restored.y, updated.point.y),
              "explicit restoration returns the full precise point for matching confirmed context");
        rejects([&] { (void)restore_fire_mission(updated, GameMap::bakurani, FireMissionWeapon::l81, false); },
            "restoration cannot bypass session map confirmation");
        rejects([&] { (void)restore_fire_mission(updated, GameMap::unselected, FireMissionWeapon::l81, true); },
            "a fabricated confirmed flag cannot restore an unselected map");
        rejects([&] { (void)restore_fire_mission(updated, GameMap::ozeti, FireMissionWeapon::l81, true); },
            "restoration does not substitute another map's terrain context");
        rejects([&] { (void)restore_fire_mission(updated, GameMap::bakurani, FireMissionWeapon::sph2, true); },
            "restoration never changes the active weapon implicitly");

        const auto before_invalid = bytes(path);
        for (const auto& name : {std::wstring{}, std::wstring{L"   "}, std::wstring{L"line\nbreak"},
                                std::wstring(121, L'x'), std::wstring(1, static_cast<wchar_t>(0xd800))}) {
            rejects([&] { (void)repository.create(GameMap::bakurani, FireMissionWeapon::l81,
                FireMissionKind::target, name, {1.0, 2.0}); }, "empty, oversized, control and malformed Unicode names are rejected");
        }
        rejects([&] { (void)repository.create(GameMap::unselected, FireMissionWeapon::l81,
            FireMissionKind::target, L"No map", {1.0, 2.0}); }, "saving requires an actual map");
        rejects([&] { (void)repository.create(static_cast<GameMap>(99), FireMissionWeapon::l81,
            FireMissionKind::target, L"Unknown map", {1.0, 2.0}); }, "invalid map enumerators are rejected");
        rejects([&] { (void)repository.create(GameMap::bakurani, static_cast<FireMissionWeapon>(99),
            FireMissionKind::target, L"Unknown weapon", {1.0, 2.0}); }, "invalid weapon enumerators are rejected");
        rejects([&] { (void)repository.create(GameMap::bakurani, FireMissionWeapon::l81,
            static_cast<FireMissionKind>(99), L"Unknown kind", {1.0, 2.0}); }, "invalid point kinds are rejected");
        rejects([&] { (void)repository.create(GameMap::bakurani, FireMissionWeapon::l81,
            FireMissionKind::target, L"NaN", {std::numeric_limits<double>::quiet_NaN(), 2.0}); }, "NaN is rejected before writing");
        rejects([&] { (void)repository.update(updated.id, L"Infinity", {1.0, std::numeric_limits<double>::infinity()}); },
            "infinity is rejected on editing");
        rejects([&] { (void)repository.erase("../../another-file"); }, "malformed identities cannot address filesystem paths");
        check(bytes(path) == before_invalid, "all rejected input preserves the accepted file bytes");

        auto verify_invalid_document = [&](const QJsonObject& document, const char* message) {
            const auto invalid_path = root / (L"invalid-" + std::to_wstring(checks) + L".json");
            write_bytes(invalid_path, QJsonDocument(document).toJson());
            const auto original = bytes(invalid_path);
            const FireMissionRepository invalid(invalid_path);
            rejects([&] { (void)invalid.load(); }, message);
            rejects([&] { (void)invalid.create(GameMap::training, FireMissionWeapon::l81,
                FireMissionKind::target, L"New", {1.0, 2.0}); }, "mutations cannot replace unsupported or malformed user data");
            check(bytes(invalid_path) == original, "malformed data remains available for manual recovery");
        };
        auto document = QJsonDocument::fromJson(bytes(path)).object();
        document.insert(QStringLiteral("version"), 2);
        verify_invalid_document(document, "unknown future schema fails without silently resetting the collection");
        document = QJsonDocument::fromJson(bytes(path)).object();
        document.insert(QStringLiteral("future_extension"), QStringLiteral("preserve me"));
        verify_invalid_document(document, "unknown root fields are not silently discarded by a write");
        document = QJsonDocument::fromJson(bytes(path)).object();
        auto array = document.value(QStringLiteral("missions")).toArray();
        auto record = array[0].toObject();
        record.insert(QStringLiteral("map"), QStringLiteral("unknown-world"));
        array[0] = record;
        document.insert(QStringLiteral("missions"), array);
        verify_invalid_document(document, "unknown map identifiers are rejected on load");
        document = QJsonDocument::fromJson(bytes(path)).object();
        array = document.value(QStringLiteral("missions")).toArray();
        record = array[0].toObject();
        record.insert(QStringLiteral("point"), QJsonObject{{QStringLiteral("x"), QStringLiteral("NaN")}, {QStringLiteral("y"), QStringLiteral("2")}});
        array[0] = record;
        document.insert(QStringLiteral("missions"), array);
        verify_invalid_document(document, "non-finite persisted coordinates are rejected");
        document = QJsonDocument::fromJson(bytes(path)).object();
        array = document.value(QStringLiteral("missions")).toArray();
        record = array[0].toObject();
        record.insert(QStringLiteral("name"), QStringLiteral("Different name"));
        array.append(record);
        document.insert(QStringLiteral("missions"), array);
        verify_invalid_document(document, "duplicated persisted identities are rejected rather than merged");
        document = QJsonDocument::fromJson(bytes(path)).object();
        QJsonArray oversized_records;
        record = document.value(QStringLiteral("missions")).toArray()[0].toObject();
        for (std::size_t index = 0; index <= maximum_saved_fire_missions; ++index) {
            record.insert(QStringLiteral("id"), QUuid::createUuid().toString(QUuid::WithoutBraces));
            record.insert(QStringLiteral("name"), QStringLiteral("Point %1").arg(static_cast<qulonglong>(index)));
            oversized_records.append(record);
        }
        document.insert(QStringLiteral("missions"), oversized_records);
        verify_invalid_document(document, "record-count limit is enforced before accepting a collection");

        const auto corrupt_path = root / L"corrupt.json";
        write_bytes(corrupt_path, QByteArray{"{not valid JSON"});
        const FireMissionRepository corrupt(corrupt_path);
        rejects([&] { (void)corrupt.load(); }, "malformed JSON is a visible read failure");
        rejects([&] { (void)corrupt.erase(updated.id); }, "deletion cannot hide a corrupt store");
        check(bytes(corrupt_path) == QByteArray{"{not valid JSON"}, "a read failure leaves malformed JSON intact");
        const auto oversized_path = root / L"oversized.json";
        write_bytes(oversized_path, QByteArray(static_cast<qsizetype>(maximum_fire_missions_file_bytes) + 1, ' '));
        rejects([&] { (void)FireMissionRepository(oversized_path).load(); }, "oversized files are bounded before JSON parsing");
        rejects([&] { (void)FireMissionRepository(root).load(); }, "a directory is not accepted as an empty store");

        {
            QLockFile occupied(QString::fromStdWString(path.native()) + QStringLiteral(".lock"));
            check(occupied.tryLock(0), "test obtains the exact repository transaction lock");
            const auto original = bytes(path);
            rejects([&] { (void)repository.update(updated.id, L"Locked edit", {1.0, 2.0}); },
                "a busy transaction reports failure instead of overwriting another writer");
            check(bytes(path) == original, "transaction lock contention preserves existing data");
        }
        {
            const HANDLE read_lock = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            check(read_lock != INVALID_HANDLE_VALUE, "test locks the destination against atomic replacement");
            if (read_lock != INVALID_HANDLE_VALUE) {
                const auto original = bytes(path);
                rejects([&] { (void)repository.update(updated.id, L"Blocked replacement", {1.0, 2.0}); },
                    "an atomic replacement failure is reported honestly");
                check(bytes(path) == original, "failed commit preserves the complete original file");
                CloseHandle(read_lock);
            }
        }

        const auto concurrent_path = root / L"concurrent.json";
        const FireMissionRepository recent(root / L"recent-fire-missions.json");
        const auto named_before_history = bytes(path);
        const Point recent_precise{100.00000000000003, -0.0};
        const auto remembered = recent.remember(GameMap::other, FireMissionWeapon::sph2,
            FireMissionKind::firing_position, recent_precise);
        (void)recent.remember(GameMap::training, FireMissionWeapon::sph2, FireMissionKind::target, {1,2});
        const auto recalled = recent.remember(GameMap::other, FireMissionWeapon::sph2,
            FireMissionKind::firing_position, recent_precise);
        check(recalled.id == remembered.id && recent.load().size() == 2 && recent.load().front().id == remembered.id,
              "automatic history deduplicates exact map/weapon/kind/point and moves it to the front");
        check(exact(recent.load().front().point.x,recent_precise.x) && exact(recent.load().front().point.y,recent_precise.y),
              "automatic history preserves exact double coordinates including signed zero");
        for (int index=0; index<70; ++index)
            (void)recent.remember(GameMap::other, FireMissionWeapon::l81, FireMissionKind::target, {double(index),42});
        check(recent.load().size() == maximum_recent_fire_missions && recent.load().front().point.x == 69 &&
              recent.load().back().point.x == 6, "automatic history evicts only oldest entries at its 64 point bound");
        rejects([&] { (void)repository.remember(GameMap::other,FireMissionWeapon::l81,FireMissionKind::target,{1,2}); },
                "automatic eviction cannot target the named mission store");
        check(bytes(path)==named_before_history,"named positions remain byte-identical during automatic history writes");
        const auto history_original=bytes(recent.path());
        write_bytes(recent.path(),QByteArray{"{broken"});
        rejects([&] { (void)recent.remember(GameMap::other,FireMissionWeapon::l81,FireMissionKind::target,{1,2}); },
                "corrupt automatic history is reported and cannot be replaced by a fresh collection");
        check(bytes(recent.path())==QByteArray{"{broken"},"failed automatic history save preserves corrupted evidence");
        write_bytes(recent.path(),history_original);
        const FireMissionRepository first_writer(concurrent_path), second_writer(concurrent_path);
        std::atomic<int> concurrency_failures{};
        auto write_points = [&](const FireMissionRepository& writer, const wchar_t* prefix) {
            try {
                for (int index = 0; index < 12; ++index)
                    (void)writer.create(GameMap::other, FireMissionWeapon::l81, FireMissionKind::target,
                        std::wstring{prefix} + std::to_wstring(index), {100.0 + index, 110.0});
            } catch (const std::exception&) { ++concurrency_failures; }
        };
        std::thread first(write_points, std::cref(first_writer), L"First ");
        std::thread second(write_points, std::cref(second_writer), L"Second ");
        first.join(); second.join();
        check(concurrency_failures == 0 && first_writer.load().size() == 24,
              "two independent repositories reload under the shared lock and cannot lose each other's additions");
    } catch (const std::exception& error) {
        ++failures;
        std::cerr << "Unhandled test failure: " << error.what() << '\n';
    }
    std::cout << checks << " fire mission assertions; " << failures << " failure(s)\n"
              << "Retained isolated fixtures: " << temporary.path().toStdString() << '\n';
    return failures == 0 ? 0 : 1;
}
