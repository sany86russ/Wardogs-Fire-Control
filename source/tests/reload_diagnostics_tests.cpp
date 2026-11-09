#include "reload_diagnostics.hpp"

#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <array>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
using wardogs_ui::ReloadDiagnosticSession;
int checks{}, failures{};

void check(bool value, const char* message) {
    ++checks;
    if (!value) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}

template<class Action>
bool rejects(Action action) {
    try { action(); } catch (const std::exception&) { return true; }
    return false;
}

wardogs::Image image() {
    // BGR: white, green, red and a second neutral white.
    return {2, 2, {255, 255, 255, 35, 200, 105, 0, 0, 255, 200, 200, 200}};
}

QByteArray contents(const std::filesystem::path& path) {
    QFile file(QString::fromStdWString(path.native()));
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read diagnostic fixture");
    return file.readAll();
}

QJsonObject manifest(const std::filesystem::path& path) {
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(contents(path / L"session.json"), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        throw std::runtime_error("Invalid diagnostic JSON");
    return document.object();
}

void exact_capture_and_metadata(const std::filesystem::path& root) {
    const auto directory = root / L"complete";
    ReloadDiagnosticSession session(directory);
    check(!std::filesystem::exists(directory), "constructing diagnostics creates no directory");
    wardogs::ReloadVisionDiagnostics missing;
    missing.failure = wardogs::ReloadDetectionFailure::no_row;
    missing.components = 17;
    missing.glyphs = 3;
    missing.joined_recovery_rois = 2;
    missing.contour_glyphs = 4;
    missing.contour_passes = 1;
    missing.glyph_bounds.push_back({0, 0, 1, 1});
    check(session.add_frame(image(), {100, 200, 102, 202}, missing, std::nullopt, 3.5, 1.25, 601.0),
          "a failed recognition retains the actual captured image");
    wardogs::ReloadVisionDiagnostics found;
    found.glyphs = 4; found.rows = 1;
    wardogs::ReloadObservation observation;
    observation.bounds = {0, 0, 2, 2};
    observation.confidence = .91;
    for (std::size_t index = 0; index < observation.arrows.size(); ++index)
        observation.arrows[index] = {static_cast<wardogs::ReloadDirection>(index), index == 0};
    check(session.add_frame(image(), {20, 30, 22, 32}, found, observation, 2.0, .5, 1400.0) &&
          session.frame_count() == 2 && session.stored_bytes() == 24,
          "one cycle retains exact raw byte accounting and each frame's independent area");
    check(!std::filesystem::exists(directory), "retaining runtime frames performs no PNG or metadata I/O");
    session.save("row missing");
    const auto data = manifest(directory);
    const auto frames = data.value(QStringLiteral("frames")).toArray();
    check(data.value(QStringLiteral("format_version")).toInt() == 1 &&
          data.value(QStringLiteral("algorithm_revision")).toString() == QString::fromLatin1(wardogs::reload_vision_revision) &&
          data.value(QStringLiteral("outcome")).toString() == QStringLiteral("row missing") &&
          data.value(QStringLiteral("capture_contract")).toString().contains(QStringLiteral("no extra captures or input")) &&
          frames.size() == 2, "session metadata identifies its version, outcome and capture-only contract");
    const auto first = frames[0].toObject();
    const auto second = frames[1].toObject();
    check(!first.value(QStringLiteral("found")).toBool() &&
          first.value(QStringLiteral("failure")).toString() == QStringLiteral("no_row") &&
          first.value(QStringLiteral("components")).toInt() == 17 &&
          first.value(QStringLiteral("glyphs")).toInt() == 3 &&
          first.value(QStringLiteral("joined_recovery_rois")).toInt() == 2 &&
          first.value(QStringLiteral("contour_glyphs")).toInt() == 4 &&
          first.value(QStringLiteral("contour_passes")).toInt() == 1 &&
          first.value(QStringLiteral("capture_ms")).toDouble() == 3.5 &&
          first.value(QStringLiteral("recognition_ms")).toDouble() == 1.25,
          "failure metadata retains the detector reason, candidate counts and measured durations");
    check(first.value(QStringLiteral("client_area_px")).toArray() == QJsonArray{100, 200, 102, 202} &&
          first.value(QStringLiteral("glyph_bounds_px")).toArray()[0].toArray() == QJsonArray{0, 0, 1, 1} &&
          second.value(QStringLiteral("bounds_px")).toArray() == QJsonArray{0, 0, 2, 2},
          "physical client area and frame-relative glyph coordinates remain distinct");
    const auto arrows = second.value(QStringLiteral("arrows")).toArray();
    const std::array names{"up", "right", "down", "left"};
    bool directions = arrows.size() == 4;
    for (int index = 0; directions && index < arrows.size(); ++index) {
        const auto arrow = arrows[index].toObject();
        directions = arrow.value(QStringLiteral("direction")).toString() == QString::fromLatin1(names[index]) &&
            arrow.value(QStringLiteral("accepted")).toBool() == (index == 0);
    }
    check(second.value(QStringLiteral("found")).toBool() && directions &&
          second.value(QStringLiteral("confidence")).toDouble() == .91,
          "all four actual directions, acceptance bits and shape confidence are retained without inference");
    const QImage raw(QString::fromStdWString((directory / L"frame-1.png").native()));
    const QImage white(QString::fromStdWString((directory / L"frame-1-white-mask.png").native()));
    const QImage green(QString::fromStdWString((directory / L"frame-1-green-mask.png").native()));
    check(!raw.isNull() && raw.size() == QSize(2, 2) && raw.pixelColor(1, 0) == QColor(105, 200, 35) &&
          raw.pixelColor(0, 1) == QColor(255, 0, 0), "PNG serialization preserves exact BGR pixels and orientation");
    check(!white.isNull() && white.pixelColor(0, 0).red() == 255 &&
          white.pixelColor(1, 1).red() == 255 && white.pixelColor(1, 0).red() == 0 &&
          white.pixelColor(0, 1).red() == 0 && !green.isNull() &&
          green.pixelColor(1, 0).red() == 255 && green.pixelColor(0, 0).red() == 0 &&
          green.pixelColor(0, 1).red() == 0 && green.pixelColor(1, 1).red() == 0,
          "separate binary masks use the detector's exact white and green classification");
    check(QDir(QString::fromStdWString(directory.native())).entryList(QDir::Files).size() == 7,
          "two captures produce only six PNGs and a final metadata file");
    const auto before = contents(directory / L"session.json");
    check(rejects([&] { session.save("overwrite"); }) && contents(directory / L"session.json") == before,
          "saving again cannot overwrite or delete a previously recorded cycle");
}

void bounds_and_failures(const std::filesystem::path& root) {
    wardogs::ReloadVisionDiagnostics diagnostics;
    diagnostics.failure = wardogs::ReloadDetectionFailure::not_enough_glyphs;
    const auto directory = root / L"bounded";
    ReloadDiagnosticSession bounded(directory, 12);
    check(bounded.add_frame(image(), {0, 0, 2, 2}, diagnostics, std::nullopt, 0, 0, 0),
          "the exact configured byte budget accepts one raw image");
    for (int index = 0; index < 4; ++index)
        check(!bounded.add_frame(image(), {0, 0, 2, 2}, diagnostics, std::nullopt, 0, 0, 0),
              "images exceeding memory or frame budget are explicitly omitted");
    check(bounded.frame_count() == 4 && bounded.stored_bytes() == 12 && !std::filesystem::exists(directory),
          "metadata and raw bytes remain bounded without writing during the cycle");
    bounded.save("budget");
    const auto data = manifest(directory);
    const auto frames = data.value(QStringLiteral("frames")).toArray();
    check(frames.size() == 4 && data.value(QStringLiteral("ignored_frames")).toInt() == 1 &&
          frames[1].toObject().value(QStringLiteral("image_status")).toString() == QStringLiteral("memory_budget_exceeded") &&
          !frames[1].toObject().contains(QStringLiteral("image")) &&
          !std::filesystem::exists(directory / L"frame-2.png"),
          "budget omissions are visible in metadata rather than fabricated as successful captures");
    ReloadDiagnosticSession invalid(root / L"invalid");
    check(!invalid.add_frame({2, 2, {1, 2, 3}}, {0, 0, 2, 2}, diagnostics, std::nullopt, 0, 0, 0),
          "a malformed raw image is not passed to the PNG encoder");
    invalid.save("invalid");
    check(manifest(invalid.directory()).value(QStringLiteral("frames")).toArray()[0].toObject()
              .value(QStringLiteral("image_status")).toString() == QStringLiteral("invalid_image"),
          "malformed image omission has an explicit diagnostic status");
    check(rejects([&] { (void)invalid.add_frame(image(), {}, diagnostics, std::nullopt, -1, 0, 0); }) &&
          rejects([&] { (void)invalid.add_frame(image(), {}, diagnostics, std::nullopt,
              std::numeric_limits<double>::quiet_NaN(), 0, 0); }),
          "invalid measured durations fail explicitly before altering session metadata");
    ReloadDiagnosticSession bounds(root / L"candidate-bounds", 0);
    diagnostics.glyph_bounds.resize(513, {1, 2, 3, 4});
    check(!bounds.add_frame(image(), {}, diagnostics, std::nullopt, 0, 0, 0),
          "zero byte budget preserves only diagnostic metadata");
    bounds.save("bounds");
    const auto frame = manifest(bounds.directory()).value(QStringLiteral("frames")).toArray()[0].toObject();
    check(frame.value(QStringLiteral("glyph_bounds_px")).toArray().size() == 512 &&
          frame.value(QStringLiteral("glyph_bounds_truncated")).toBool(),
          "diagnostic candidate lists have their own explicit bound");
    const auto blocked = root / L"parent-file";
    QFile file(QString::fromStdWString(blocked.native()));
    if (!file.open(QIODevice::WriteOnly) || file.write("keep") != 4) throw std::runtime_error("Cannot create I/O fixture");
    file.close();
    ReloadDiagnosticSession failed(blocked / L"new-session");
    check(rejects([&] { failed.save("failure"); }) && contents(blocked) == QByteArray("keep"),
          "directory/write failures are reported and an existing file remains untouched");
    check(rejects([&] { ReloadDiagnosticSession empty({}); }) &&
          rejects([&] { ReloadDiagnosticSession traversal(root / L".." / L"escape"); }) &&
          rejects([&] { ReloadDiagnosticSession oversized(root / L"oversize", ReloadDiagnosticSession::maximum_bytes + 1); }),
          "invalid directory traversals and unsupported memory quotas are rejected");
}
}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir temporary(QStringLiteral("wardogs-reload-diagnostics-XXXXXX"));
    if (!temporary.isValid()) return 1;
    try {
        const std::filesystem::path root{temporary.path().toStdWString()};
        exact_capture_and_metadata(root);
        bounds_and_failures(root);
    } catch (const std::exception& error) {
        ++failures;
        std::cerr << "EXCEPTION: " << error.what() << '\n';
    }
    std::cout << checks << " reload diagnostic checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
