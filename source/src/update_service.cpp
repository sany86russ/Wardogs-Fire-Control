#include "update_service.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QTimer>

#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace wardogs::updates {
namespace {

constexpr qint64 maximum_archive_size = 512LL * 1024 * 1024;
constexpr qsizetype maximum_metadata_size = 1024 * 1024;
const QString repository =
    QStringLiteral("https://github.com/sany86russ/Wardogs-Fire-Control");
const QUrl latest_endpoint(QStringLiteral(
    "https://api.github.com/repos/sany86russ/Wardogs-Fire-Control/releases/latest"));

std::array<int, 3> version_parts(const QString& text, bool tag) {
    static const QRegularExpression plain(QStringLiteral(
        "^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\z"));
    const QString version = tag && text.startsWith(QLatin1Char('v'))
        ? text.mid(1) : text;
    if (tag && !text.startsWith(QLatin1Char('v')))
        throw std::runtime_error("Release tag is not a stable vX.Y.Z version.");
    const auto match = plain.match(version);
    if (!match.hasMatch())
        throw std::runtime_error("Version is not a stable X.Y.Z version.");
    std::array<int, 3> result{};
    for (int index = 0; index < 3; ++index) {
        bool valid = false;
        result[index] = match.captured(index + 1).toInt(&valid);
        if (!valid)
            throw std::runtime_error("Version component is too large.");
    }
    return result;
}

bool exact_repository_url(const QUrl& url, const QString& suffix) {
    return url.isValid() && url.scheme() == QStringLiteral("https") &&
           url.host() == QStringLiteral("github.com") && url.port() == -1 &&
           url.userInfo().isEmpty() && !url.hasQuery() && !url.hasFragment() &&
           url.toString(QUrl::FullyEncoded) == repository + suffix;
}

bool approved_redirect(const QUrl& url, const QUrl& original) {
    if (!url.isValid() || url.scheme() != QStringLiteral("https") ||
        url.port() != -1 || !url.userInfo().isEmpty() || url.hasFragment())
        return false;
    if (url.host() == QStringLiteral("github.com")) return url == original;
    return url.host() == QStringLiteral("release-assets.githubusercontent.com") ||
           url.host() == QStringLiteral("objects.githubusercontent.com") ||
           url.host() == QStringLiteral("github-releases.githubusercontent.com");
}

void validate_release(const ReleaseInfo& release) {
    version_parts(release.tag, true);
    if (release.version != release.tag.mid(1) || release.size <= 0 ||
        release.size > maximum_archive_size || release.sha256.size() != 32 ||
        !exact_repository_url(release.release_url,
            QStringLiteral("/releases/tag/") + release.tag) ||
        !exact_repository_url(release.download_url,
            QStringLiteral("/releases/download/") + release.tag +
            QStringLiteral("/WardogsFireControl-") + release.tag +
            QStringLiteral("-win-x64.zip")))
        throw std::runtime_error("Release download metadata is unsafe or incomplete.");
}

QNetworkRequest request_for(const QUrl& url, bool metadata) {
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "WardogsFireControl-Updater");
    request.setRawHeader("Accept", metadata ? "application/vnd.github+json"
                                           : "application/octet-stream");
    request.setRawHeader("Accept-Encoding", "identity");
    request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(metadata ? 15000 : 30000);
    return request;
}

}  // namespace

std::optional<ReleaseInfo> parse_release(const QByteArray& json,
                                        const QString& current_version) {
    const auto current = version_parts(current_version, false);
    if (json.size() > maximum_metadata_size)
        throw std::runtime_error("Release metadata exceeds the size limit.");
    QJsonParseError parse_error{};
    const auto document = QJsonDocument::fromJson(json, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject())
        throw std::runtime_error("GitHub returned invalid release metadata.");
    const auto object = document.object();
    if (!object.value(QStringLiteral("draft")).isBool() ||
        !object.value(QStringLiteral("prerelease")).isBool())
        throw std::runtime_error("Release stability metadata is missing.");
    if (object.value(QStringLiteral("draft")).toBool() ||
        object.value(QStringLiteral("prerelease")).toBool())
        return std::nullopt;
    ReleaseInfo result;
    result.tag = object.value(QStringLiteral("tag_name")).toString();
    const auto latest = version_parts(result.tag, true);
    if (latest <= current) return std::nullopt;
    result.version = result.tag.mid(1);
    result.release_url = QUrl(object.value(QStringLiteral("html_url")).toString(),
                              QUrl::StrictMode);
    const auto assets_value = object.value(QStringLiteral("assets"));
    if (!assets_value.isArray())
        throw std::runtime_error("Release assets are missing.");
    const auto asset_name = QStringLiteral("WardogsFireControl-") + result.tag +
                            QStringLiteral("-win-x64.zip");
    bool found = false;
    for (const auto& value : assets_value.toArray()) {
        if (!value.isObject())
            throw std::runtime_error("Release asset metadata is invalid.");
        const auto asset = value.toObject();
        if (asset.value(QStringLiteral("name")).toString() != asset_name) continue;
        if (found) throw std::runtime_error("Release contains duplicate Windows assets.");
        found = true;
        result.download_url = QUrl(
            asset.value(QStringLiteral("browser_download_url")).toString(),
            QUrl::StrictMode);
        const auto size_value = asset.value(QStringLiteral("size"));
        const double size = size_value.toDouble(-1);
        if (!size_value.isDouble() || !std::isfinite(size) || size <= 0 ||
            size > static_cast<double>(maximum_archive_size) || std::floor(size) != size)
            throw std::runtime_error("Release asset size is invalid.");
        result.size = static_cast<qint64>(size);
        const auto digest = asset.value(QStringLiteral("digest")).toString();
        static const QRegularExpression digest_pattern(
            QStringLiteral("^sha256:([0-9a-fA-F]{64})\\z"));
        const auto digest_match = digest_pattern.match(digest);
        if (!digest_match.hasMatch())
            throw std::runtime_error("Release asset has no valid SHA-256 digest.");
        result.sha256 = QByteArray::fromHex(digest_match.captured(1).toLatin1());
    }
    if (!found)
        throw std::runtime_error("Release has no supported Windows archive.");
    validate_release(result);
    return result;
}

struct Service::Impl {
    enum class Operation { none, check, download };
    Service* owner;
    QPointer<QNetworkAccessManager> network;
    QPointer<QNetworkReply> reply;
    QTimer deadline;
    Operation operation{Operation::none};
    CheckCallback check_callback;
    DownloadCallback download_callback;
    ProgressCallback progress_callback;
    QString current_version;
    QByteArray metadata;
    ReleaseInfo release;
    QString destination;
    QString partial_path;
    std::unique_ptr<QFile> file;
    std::unique_ptr<QCryptographicHash> hash;
    qint64 received{};
    int redirects{};

    explicit Impl(Service* service, QNetworkAccessManager* manager)
        : owner(service), network(manager ? manager :
                                 new QNetworkAccessManager(service)) {
        deadline.setSingleShot(true);
        QObject::connect(&deadline, &QTimer::timeout, owner, [this] {
            fail(QStringLiteral("The update request timed out."));
        });
    }

    ~Impl() { reset(true); }

    void reset(bool remove_partial) {
        deadline.stop();
        operation = Operation::none;
        if (reply) {
            QObject::disconnect(reply, nullptr, owner, nullptr);
            reply->abort();
            reply->deleteLater();
            reply = nullptr;
        }
        file.reset();
        hash.reset();
        if (remove_partial && !partial_path.isEmpty()) QFile::remove(partial_path);
        partial_path.clear();
        metadata.clear();
        check_callback = {};
        download_callback = {};
        progress_callback = {};
    }

    void finish_check(std::optional<ReleaseInfo> value, const QString& error) {
        auto callback = std::move(check_callback);
        reset(true);
        if (callback) callback(std::move(value), error);
    }

    void finish_download(const QString& path, const QString& error) {
        auto callback = std::move(download_callback);
        reset(true);
        if (callback) callback(path, error);
    }

    void fail(const QString& error) {
        if (operation == Operation::check) finish_check(std::nullopt, error);
        else if (operation == Operation::download) finish_download({}, error);
    }

    void get(const QUrl& url) {
        if (!network) {
            fail(QStringLiteral("The update network service is unavailable."));
            return;
        }
        reply = network->get(request_for(url, operation == Operation::check));
        reply->setReadBufferSize(128 * 1024);
        auto* current_reply = reply.data();
        QObject::connect(current_reply, &QNetworkReply::readyRead, owner,
                         [this, current_reply] {
            if (reply == current_reply) consume();
        });
        QObject::connect(current_reply, &QNetworkReply::finished, owner,
                         [this, current_reply] {
            if (reply == current_reply) completed();
        });
    }

    void consume() {
        if (!reply || operation == Operation::none) return;
        const auto status = reply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        // Redirect and error bodies must never enter the staged archive.
        if (status != 200) {
            reply->readAll();
            return;
        }
        const auto bytes = reply->readAll();
        if (operation == Operation::check) {
            if (bytes.size() > maximum_metadata_size - metadata.size()) {
                fail(QStringLiteral("Release metadata exceeds the size limit."));
                return;
            }
            metadata.append(bytes);
        } else {
            if (bytes.size() > release.size - received) {
                fail(QStringLiteral("The downloaded archive exceeds its expected size."));
                return;
            }
            if (file->write(bytes) != bytes.size()) {
                fail(QStringLiteral("The update archive could not be written to disk."));
                return;
            }
            hash->addData(bytes);
            received += bytes.size();
            auto progress = progress_callback;
            if (progress) progress(received, release.size);
        }
    }

    void completed() {
        if (!reply || operation == Operation::none) return;
        auto* completed_reply = reply.data();
        const int status = completed_reply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (operation == Operation::check && status == 404) {
            finish_check(std::nullopt, {});
            return;
        }
        if (status >= 300 && status < 400) {
            if (operation != Operation::download) {
                fail(QStringLiteral("GitHub returned an unexpected metadata redirect."));
                return;
            }
            const auto target = completed_reply->url().resolved(
                completed_reply->attribute(
                    QNetworkRequest::RedirectionTargetAttribute).toUrl());
            if (++redirects > 5 || !approved_redirect(target, release.download_url)) {
                fail(QStringLiteral("GitHub redirected the update to an unapproved address."));
                return;
            }
            QObject::disconnect(completed_reply, nullptr, owner, nullptr);
            reply = nullptr;
            completed_reply->deleteLater();
            get(target);
            return;
        }
        if (completed_reply->error() != QNetworkReply::NoError || status != 200) {
            // Qt error strings can contain a signed CDN URL. Keep temporary
            // query credentials out of dialogs, logs, and diagnostic output.
            fail(QStringLiteral("The GitHub update request failed (HTTP %1, network error %2).")
                     .arg(status).arg(static_cast<int>(completed_reply->error())));
            return;
        }
        const QPointer<Service> owner_guard(owner);
        consume();
        if (!owner_guard || reply != completed_reply ||
            operation == Operation::none) return;
        if (operation == Operation::check) {
            try {
                const auto result = parse_release(metadata, current_version);
                finish_check(result, {});
            } catch (const std::exception& error) {
                finish_check(std::nullopt, QString::fromUtf8(error.what()));
            }
            return;
        }
        if (received != release.size || hash->result() != release.sha256) {
            fail(QStringLiteral("The update archive failed its size or SHA-256 verification."));
            return;
        }
        if (!file->flush()) {
            fail(QStringLiteral("The update archive could not be flushed to disk."));
            return;
        }
        file->close();
        if (!file->rename(destination)) {
            fail(QStringLiteral("The verified update archive could not be saved."));
            return;
        }
        partial_path.clear();
        finish_download(destination, {});
    }
};

Service::Service(QObject* parent, QNetworkAccessManager* network)
    : QObject(parent), impl_(std::make_unique<Impl>(this, network)) {}

Service::~Service() = default;

bool Service::busy() const { return impl_->operation != Impl::Operation::none; }

void Service::check(const QString& current_version, CheckCallback callback) {
    if (busy()) {
        if (callback) callback(std::nullopt, QStringLiteral("An update operation is already running."));
        return;
    }
    try {
        version_parts(current_version, false);
    } catch (const std::exception& error) {
        if (callback) callback(std::nullopt, QString::fromUtf8(error.what()));
        return;
    }
    impl_->operation = Impl::Operation::check;
    impl_->current_version = current_version;
    impl_->check_callback = std::move(callback);
    impl_->metadata.clear();
    impl_->deadline.start(15000);
    impl_->get(latest_endpoint);
}

void Service::download(const ReleaseInfo& release, const QString& destination,
                       ProgressCallback progress, DownloadCallback callback) {
    if (busy()) {
        if (callback) callback({}, QStringLiteral("An update operation is already running."));
        return;
    }
    try {
        validate_release(release);
    } catch (const std::exception& error) {
        if (callback) callback({}, QString::fromUtf8(error.what()));
        return;
    }
    const QFileInfo target(destination);
    if (!target.isAbsolute() || target.exists() ||
        !QDir().mkpath(target.absolutePath())) {
        if (callback) callback({}, QStringLiteral("The update destination is unavailable or already exists."));
        return;
    }
    auto file = std::make_unique<QFile>(destination + QStringLiteral(".partial"));
    if (!file->open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        if (callback) callback({}, QStringLiteral("A temporary update archive could not be created."));
        return;
    }
    impl_->operation = Impl::Operation::download;
    impl_->release = release;
    impl_->destination = destination;
    impl_->partial_path = file->fileName();
    impl_->file = std::move(file);
    impl_->hash = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
    impl_->received = 0;
    impl_->redirects = 0;
    impl_->progress_callback = std::move(progress);
    impl_->download_callback = std::move(callback);
    impl_->deadline.start(10 * 60 * 1000);
    impl_->get(release.download_url);
}

void Service::cancel() {
    if (busy()) impl_->fail(QStringLiteral("The update was cancelled."));
}

}  // namespace wardogs::updates
