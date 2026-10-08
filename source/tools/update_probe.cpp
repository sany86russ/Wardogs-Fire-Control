#include "update_service.hpp"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTextStream>

#include <optional>
#include <utility>

namespace {

bool is_latest_api(const QUrl& url) {
    return url.toString(QUrl::FullyEncoded) == QStringLiteral(
        "https://api.github.com/repos/sany86russ/Wardogs-Fire-Control/releases/latest");
}

// This console probe runs on shared GitHub runner addresses. Its optional CI
// credential belongs only to the exact metadata endpoint, never to downloads.
// The shipped application continues to use the ordinary anonymous Service.
class ProbeNetwork final : public QNetworkAccessManager {
public:
    explicit ProbeNetwork(QByteArray token) : token_(std::move(token)) {
        connect(this, &QNetworkAccessManager::finished, this, [](QNetworkReply* reply) {
            if (!is_latest_api(reply->request().url())) return;
            const int status = reply->attribute(
                QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (status != 403 && status != 429) return;
            QTextStream output(stderr);
            output << "GitHub API rate-limit diagnostic: HTTP " << status;
            for (const auto& field : {QByteArray("X-RateLimit-Remaining"),
                                      QByteArray("X-RateLimit-Reset")}) {
                bool numeric = false;
                const auto value = reply->rawHeader(field).toLongLong(&numeric);
                if (numeric && value >= 0) output << ", " << field << '=' << value;
            }
            output << '\n';
        });
    }

protected:
    QNetworkReply* createRequest(Operation operation, const QNetworkRequest& request,
                                 QIODevice* outgoing_data) override {
        auto scoped_request = request;
        scoped_request.setRawHeader("Authorization", {});
        if (!token_.isEmpty() && is_latest_api(request.url()))
            scoped_request.setRawHeader("Authorization", "Bearer " + token_);
        return QNetworkAccessManager::createRequest(operation, scoped_request,
                                                     outgoing_data);
    }

private:
    QByteArray token_;
};

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    const auto arguments = application.arguments().mid(1);
    QString destination;
    bool live_check = false;
    for (const auto& argument : arguments) {
        if (argument == QStringLiteral("--live-check") && !live_check)
            live_check = true;
        else if (argument.startsWith(QStringLiteral("--download=")) &&
                 argument.size() > 11 && destination.isEmpty())
            destination = argument.mid(11);
        else {
            QTextStream(stderr) << "Usage: wardogs_update_probe --live-check "
                                   "[--download=<absolute fresh archive path>]\n";
            return 2;
        }
    }
    if (!live_check) {
        QTextStream(stderr) << "--live-check is required.\n";
        return 2;
    }
    QByteArray ci_token;
    if (qgetenv("GITHUB_ACTIONS") == QByteArray("true")) {
        ci_token = qgetenv("WARDOGS_CI_GITHUB_TOKEN");
        if (ci_token.isEmpty()) {
            QTextStream(stderr) << "CI GitHub API probe requires WARDOGS_CI_GITHUB_TOKEN.\n";
            return 2;
        }
        for (const char character : ci_token) {
            const auto byte = static_cast<unsigned char>(character);
            if (byte <= 32 || byte >= 127) {
                QTextStream(stderr) << "CI GitHub API probe received an invalid bearer token.\n";
                return 2;
            }
        }
    }
    ProbeNetwork network(std::move(ci_token));
    wardogs::updates::Service service(nullptr, &network);
    service.check(QStringLiteral("0.0.0"),
        [&](std::optional<wardogs::updates::ReleaseInfo> release, QString error) {
            if (!error.isEmpty() || !release) {
                QTextStream(stderr) << (error.isEmpty()
                    ? QStringLiteral("No stable release is available.") : error) << '\n';
                application.exit(1);
                return;
            }
            QJsonObject result;
            result.insert(QStringLiteral("version"), release->version);
            result.insert(QStringLiteral("tag"), release->tag);
            result.insert(QStringLiteral("size"), release->size);
            result.insert(QStringLiteral("sha256"),
                          QString::fromLatin1(release->sha256.toHex()));
            if (destination.isEmpty()) {
                QTextStream(stdout) << QJsonDocument(result).toJson(QJsonDocument::Compact)
                                    << '\n';
                application.exit(0);
                return;
            }
            service.download(*release, destination, {},
                [&, result](QString path, QString download_error) mutable {
                    if (!download_error.isEmpty()) {
                        QTextStream(stderr) << download_error << '\n';
                        application.exit(1);
                        return;
                    }
                    result.insert(QStringLiteral("verified_archive"), path);
                    QTextStream(stdout) << QJsonDocument(result).toJson(QJsonDocument::Compact)
                                        << '\n';
                    application.exit(0);
                });
        });
    return application.exec();
}
