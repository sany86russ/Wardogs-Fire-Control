#include "update_service.hpp"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

#include <optional>

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
    wardogs::updates::Service service;
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
