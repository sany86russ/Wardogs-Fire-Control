#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QUrl>

#include <functional>
#include <memory>
#include <optional>

class QNetworkAccessManager;

namespace wardogs::updates {

struct ReleaseInfo {
    QString version;
    QString tag;
    QUrl release_url;
    QUrl download_url;
    qint64 size{};
    QByteArray sha256;
};

// The GitHub response must describe a newer stable release and its exact
// Windows asset. Unsafe or incomplete metadata raises std::runtime_error.
std::optional<ReleaseInfo> parse_release(const QByteArray& json,
                                        const QString& current_version);

class Service final : public QObject {
public:
    using CheckCallback =
        std::function<void(std::optional<ReleaseInfo>, QString error)>;
    using DownloadCallback =
        std::function<void(QString archive_path, QString error)>;
    using ProgressCallback = std::function<void(qint64 received, qint64 total)>;

    explicit Service(QObject* parent = nullptr,
                     QNetworkAccessManager* network = nullptr);
    ~Service() override;

    void check(const QString& current_version, CheckCallback callback);
    void download(const ReleaseInfo& release, const QString& destination,
                  ProgressCallback progress, DownloadCallback callback);
    void cancel();
    bool busy() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace wardogs::updates
