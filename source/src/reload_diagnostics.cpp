#include "reload_diagnostics.hpp"

#include <Windows.h>

#include <QDateTime>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QString>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>

namespace wardogs_ui {
namespace {

QString qpath(const std::filesystem::path& path) {
    return QString::fromStdWString(path.native());
}

void require_plain_ancestors(const std::filesystem::path& path) {
    for (auto current = path; !current.empty();) {
        const DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES) {
            if (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
                throw std::runtime_error("Reload diagnostic path contains a reparse point");
            if (!(attributes & FILE_ATTRIBUTE_DIRECTORY))
                throw std::runtime_error("Reload diagnostic path contains a non-directory ancestor");
        } else {
            const auto error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
                throw std::runtime_error("Cannot inspect reload diagnostic directory: Windows error " +
                                         std::to_string(error));
        }
        const auto parent = current.parent_path();
        if (parent == current) break;
        current = parent;
    }
}

QJsonArray rect_json(wardogs::ImageRect rect) {
    return {rect.left, rect.top, rect.right, rect.bottom};
}

QString direction_name(wardogs::ReloadDirection direction) {
    switch (direction) {
    case wardogs::ReloadDirection::up: return QStringLiteral("up");
    case wardogs::ReloadDirection::right: return QStringLiteral("right");
    case wardogs::ReloadDirection::down: return QStringLiteral("down");
    case wardogs::ReloadDirection::left: return QStringLiteral("left");
    }
    return QStringLiteral("invalid");
}

void save_png(const std::filesystem::path& path, const QImage& image) {
    if (image.isNull()) throw std::runtime_error("Cannot allocate reload diagnostic image");
    if (std::filesystem::exists(path))
        throw std::runtime_error("Refusing to overwrite reload diagnostic image");
    QSaveFile output(qpath(path));
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly) || !image.save(&output, "PNG") || !output.commit())
        throw std::runtime_error("Cannot save reload diagnostic PNG: " + output.errorString().toStdString());
}

}  // namespace

ReloadDiagnosticSession::ReloadDiagnosticSession(std::filesystem::path directory,
                                                 std::size_t memory_limit_bytes)
    : memory_limit_bytes_(memory_limit_bytes) {
    if (directory.empty() || !directory.has_filename() || memory_limit_bytes > maximum_bytes)
        throw std::invalid_argument("Invalid reload diagnostic directory or memory budget");
    for (const auto& part : directory)
        if (part == L".." || part == L".")
            throw std::invalid_argument("Reload diagnostic directory must not traverse paths");
    directory_ = std::filesystem::absolute(directory).lexically_normal();
    if (directory_ == directory_.root_path())
        throw std::invalid_argument("Reload diagnostic directory must name a new session");
    frames_.reserve(maximum_frames);
}

bool ReloadDiagnosticSession::add_frame(wardogs::Image image,
    wardogs::ImageRect client_area, const wardogs::ReloadVisionDiagnostics& diagnostics,
    std::optional<wardogs::ReloadObservation> observation,
    double capture_ms, double recognition_ms, double elapsed_ms) {
    if (!std::isfinite(capture_ms) || !std::isfinite(recognition_ms) || !std::isfinite(elapsed_ms) ||
        capture_ms < 0 || recognition_ms < 0 || elapsed_ms < 0)
        throw std::invalid_argument("Invalid reload diagnostic timing");
    if (frames_.size() == maximum_frames) {
        ++ignored_frames_;
        return false;
    }
    Frame frame;
    frame.width = image.width;
    frame.height = image.height;
    frame.area = client_area;
    frame.diagnostics = diagnostics;
    if (frame.diagnostics.glyph_bounds.size() > 512) {
        frame.diagnostics.glyph_bounds.resize(512);
        frame.glyph_bounds_truncated = true;
    }
    frame.observation = std::move(observation);
    frame.capture_ms = capture_ms;
    frame.recognition_ms = recognition_ms;
    frame.elapsed_ms = elapsed_ms;
    const bool valid = image.width > 0 && image.height > 0 && image.width <= 16384 &&
        image.height <= 8192 && static_cast<std::uint64_t>(image.width) * image.height <= 40'000'000 &&
        image.bgr.size() == static_cast<std::uint64_t>(image.width) * image.height * 3;
    const bool stored = valid && image.bgr.size() <= memory_limit_bytes_ - stored_bytes_;
    if (stored) {
        stored_bytes_ += image.bgr.size();
        frame.image = std::move(image);
        frame.image_status = "stored";
    } else frame.image_status = valid ? "memory_budget_exceeded" : "invalid_image";
    frames_.push_back(std::move(frame));
    return stored;
}

void ReloadDiagnosticSession::save(const std::string& outcome) const {
    require_plain_ancestors(directory_);
    if (std::filesystem::exists(directory_))
        throw std::runtime_error("Refusing to overwrite an existing reload diagnostic session");
    std::filesystem::create_directories(directory_.parent_path());
    require_plain_ancestors(directory_.parent_path());
    if (!std::filesystem::create_directory(directory_))
        throw std::runtime_error("Cannot create a new reload diagnostic session");
    require_plain_ancestors(directory_);

    QJsonArray records;
    for (std::size_t index = 0; index < frames_.size(); ++index) {
        const auto& frame = frames_[index];
        const auto& analysis = frame.diagnostics;
        QJsonArray candidates;
        for (const auto& box : analysis.glyph_bounds) candidates.append(rect_json(box));
        QJsonObject record{
            {QStringLiteral("frame"), static_cast<int>(index + 1)},
            {QStringLiteral("size_px"), QJsonArray{frame.width, frame.height}},
            {QStringLiteral("client_area_px"), rect_json(frame.area)},
            {QStringLiteral("image_status"), QString::fromStdString(frame.image_status)},
            {QStringLiteral("capture_ms"), frame.capture_ms},
            {QStringLiteral("recognition_ms"), frame.recognition_ms},
            {QStringLiteral("elapsed_ms"), frame.elapsed_ms},
            {QStringLiteral("found"), frame.observation.has_value()},
            {QStringLiteral("failure"), QString::fromLatin1(wardogs::reload_detection_failure_name(analysis.failure))},
            {QStringLiteral("components"), static_cast<qint64>(analysis.components)},
            {QStringLiteral("glyphs"), static_cast<qint64>(analysis.glyphs)},
            {QStringLiteral("rows"), static_cast<qint64>(analysis.rows)},
            {QStringLiteral("joined_recovery_rois"), static_cast<qint64>(analysis.joined_recovery_rois)},
            {QStringLiteral("contour_glyphs"), static_cast<qint64>(analysis.contour_glyphs)},
            {QStringLiteral("contour_passes"), static_cast<qint64>(analysis.contour_passes)},
            {QStringLiteral("recovery_budget_exhausted"), analysis.recovery_budget_exhausted},
            {QStringLiteral("glyph_bounds_truncated"), frame.glyph_bounds_truncated},
            {QStringLiteral("glyph_bounds_px"), candidates}
        };
        if (frame.observation) {
            const auto& observation = *frame.observation;
            QJsonArray arrows;
            for (const auto& arrow : observation.arrows)
                arrows.append(QJsonObject{{QStringLiteral("direction"), direction_name(arrow.direction)},
                                           {QStringLiteral("accepted"), arrow.accepted}});
            record.insert(QStringLiteral("arrows"), arrows);
            record.insert(QStringLiteral("confidence"), observation.confidence);
            record.insert(QStringLiteral("bounds_px"), rect_json(observation.bounds));
        }
        if (frame.image) {
            const auto stem = "frame-" + std::to_string(index + 1);
            const auto raw_name = stem + ".png";
            const auto white_name = stem + "-white-mask.png";
            const auto green_name = stem + "-green-mask.png";
            const auto& image = *frame.image;
            const QImage raw(image.bgr.data(), image.width, image.height, image.width * 3, QImage::Format_BGR888);
            QImage white_mask(image.width, image.height, QImage::Format_Grayscale8);
            QImage green_mask(image.width, image.height, QImage::Format_Grayscale8);
            if (white_mask.isNull() || green_mask.isNull())
                throw std::runtime_error("Cannot allocate reload diagnostic masks");
            white_mask.fill(0); green_mask.fill(0);
            for (int y = 0; y < image.height; ++y) {
                const auto* input = image.bgr.data() + static_cast<std::size_t>(y) * image.width * 3;
                auto* whites = white_mask.scanLine(y);
                auto* greens = green_mask.scanLine(y);
                for (int x = 0; x < image.width; ++x) {
                    const auto kind = wardogs::reload_pixel_class(input + x * 3);
                    whites[x] = kind == 1 ? 255 : 0;
                    greens[x] = kind == 2 ? 255 : 0;
                }
            }
            save_png(directory_ / raw_name, raw);
            save_png(directory_ / white_name, white_mask);
            save_png(directory_ / green_name, green_mask);
            record.insert(QStringLiteral("image"), QString::fromStdString(raw_name));
            record.insert(QStringLiteral("white_mask"), QString::fromStdString(white_name));
            record.insert(QStringLiteral("green_mask"), QString::fromStdString(green_name));
        }
        records.append(record);
    }
    const QJsonObject document{
        {QStringLiteral("format_version"), 1},
        {QStringLiteral("algorithm_revision"), QString::fromLatin1(wardogs::reload_vision_revision)},
        {QStringLiteral("saved_utc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("coordinate_space"), QStringLiteral("physical_game_client_pixels; glyph bounds relative to each frame")},
        {QStringLiteral("capture_contract"), QStringLiteral("runtime frames only; no extra captures or input")},
        {QStringLiteral("outcome"), QString::fromUtf8(outcome.data(), static_cast<qsizetype>(std::min<std::size_t>(outcome.size(), 4096)))},
        {QStringLiteral("frame_limit"), static_cast<qint64>(maximum_frames)},
        {QStringLiteral("memory_limit_bytes"), static_cast<qint64>(memory_limit_bytes_)},
        {QStringLiteral("stored_bytes"), static_cast<qint64>(stored_bytes_)},
        {QStringLiteral("ignored_frames"), static_cast<qint64>(ignored_frames_)},
        {QStringLiteral("frames"), records}
    };
    const auto bytes = QJsonDocument(document).toJson(QJsonDocument::Indented);
    QSaveFile output(qpath(directory_ / L"session.json"));
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit())
        throw std::runtime_error("Cannot save reload diagnostic metadata: " + output.errorString().toStdString());
}

}  // namespace wardogs_ui
