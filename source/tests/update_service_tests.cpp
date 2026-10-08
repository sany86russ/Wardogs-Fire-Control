#include "update_service.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTemporaryDir>
#include <QTimer>

#include <algorithm>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace {

int failures{};
int assertions{};

void check(bool value, const char* message) {
    ++assertions;
    if (value) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

QByteArray sha256(const QByteArray& bytes) {
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}

QJsonObject release_object(const QByteArray& content = QByteArray("portable ZIP"),
                           const QString& tag = QStringLiteral("v2.8.0")) {
    const auto prefix = QStringLiteral(
        "https://github.com/sany86russ/Wardogs-Fire-Control/releases/");
    const auto name = QStringLiteral("WardogsFireControl-") + tag +
                      QStringLiteral("-win-x64.zip");
    QJsonObject asset;
    asset.insert(QStringLiteral("name"), name);
    asset.insert(QStringLiteral("browser_download_url"),
                 prefix + QStringLiteral("download/") + tag + '/' + name);
    asset.insert(QStringLiteral("size"), content.size());
    asset.insert(QStringLiteral("digest"),
                 QStringLiteral("sha256:") + QString::fromLatin1(sha256(content).toHex()));
    QJsonObject release;
    release.insert(QStringLiteral("draft"), false);
    release.insert(QStringLiteral("prerelease"), false);
    release.insert(QStringLiteral("tag_name"), tag);
    release.insert(QStringLiteral("html_url"), prefix + QStringLiteral("tag/") + tag);
    release.insert(QStringLiteral("assets"), QJsonArray{asset});
    return release;
}

QByteArray encode(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

void change_asset(QJsonObject& release,
                  const std::function<void(QJsonObject&)>& change) {
    auto asset = release.value(QStringLiteral("assets")).toArray().at(0).toObject();
    change(asset);
    release.insert(QStringLiteral("assets"), QJsonArray{asset});
}

void rejects(const QJsonObject& object, const char* message) {
    try {
        wardogs::updates::parse_release(encode(object), QStringLiteral("2.7.0"));
        check(false, message);
    } catch (const std::runtime_error&) {
        check(true, message);
    }
}

struct Response {
    int status{200};
    QByteArray body;
    QUrl redirect;
    QNetworkReply::NetworkError error{QNetworkReply::NoError};
    qsizetype chunk_size{4096};
    bool finish{true};
};

class MockReply final : public QNetworkReply {
public:
    MockReply(const QNetworkRequest& request, QNetworkAccessManager::Operation operation,
              Response response, QObject* parent)
        : QNetworkReply(parent), response_(std::move(response)) {
        setRequest(request);
        setUrl(request.url());
        setOperation(operation);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, response_.status);
        if (!response_.redirect.isEmpty())
            setAttribute(QNetworkRequest::RedirectionTargetAttribute, response_.redirect);
        setHeader(QNetworkRequest::ContentLengthHeader, response_.body.size());
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        QTimer::singleShot(0, this, [this] { deliver(); });
    }

    void abort() override {
        if (isFinished()) return;
        aborted_ = true;
        setError(OperationCanceledError, QStringLiteral("Mock cancellation"));
        setFinished(true);
        QTimer::singleShot(0, this, [this] {
            emit errorOccurred(OperationCanceledError);
            emit finished();
        });
    }

    qint64 bytesAvailable() const override {
        return readable_ - position_ + QNetworkReply::bytesAvailable();
    }

protected:
    qint64 readData(char* output, qint64 maximum) override {
        const qint64 count = std::min(maximum, readable_ - position_);
        if (count <= 0) return isFinished() ? -1 : 0;
        std::memcpy(output, response_.body.constData() + position_,
                    static_cast<std::size_t>(count));
        position_ += count;
        return count;
    }

private:
    Response response_;
    qint64 readable_{};
    qint64 position_{};
    bool aborted_{};
    bool started_{};

    void deliver() {
        if (aborted_) return;
        if (!started_) {
            started_ = true;
            emit metaDataChanged();
        }
        readable_ = std::min<qint64>(response_.body.size(),
                                     readable_ + response_.chunk_size);
        if (readable_ > position_) emit readyRead();
        if (aborted_) return;
        if (readable_ < response_.body.size()) {
            QTimer::singleShot(0, this, [this] { deliver(); });
            return;
        }
        if (!response_.finish) return;
        if (response_.error != NoError) {
            setError(response_.error, QStringLiteral("Mock network failure"));
            emit errorOccurred(response_.error);
        }
        setFinished(true);
        emit finished();
    }
};

class MockNetwork final : public QNetworkAccessManager {
public:
    std::deque<Response> responses;
    QList<QNetworkRequest> requests;

protected:
    QNetworkReply* createRequest(Operation operation,
                                 const QNetworkRequest& request,
                                 QIODevice*) override {
        requests.append(request);
        Response response;
        if (responses.empty()) {
            response.status = 500;
            response.error = QNetworkReply::InternalServerError;
        } else {
            response = std::move(responses.front());
            responses.pop_front();
        }
        return new MockReply(request, operation, std::move(response), this);
    }
};

void wait_for(bool& done, wardogs::updates::Service& service) {
    if (done) return;
    QEventLoop loop;
    QTimer tick;
    tick.setInterval(1);
    QObject::connect(&tick, &QTimer::timeout, &loop, [&] {
        if (done) loop.quit();
    });
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&] {
        check(false, "offline asynchronous operation finishes within three seconds");
        service.cancel();
        loop.quit();
    });
    tick.start();
    timeout.start(3000);
    loop.exec();
    check(done, "asynchronous operation invokes its completion callback");
}

void parser_tests() {
    const auto valid = release_object();
    const auto result = wardogs::updates::parse_release(encode(valid), "2.7.0");
    check(result && result->version == "2.8.0" && result->tag == "v2.8.0",
          "new stable release parses with semantic version and exact asset");
    check(result && result->sha256 == sha256("portable ZIP"),
          "digest is returned as the exact 32 SHA-256 bytes");
    check(!wardogs::updates::parse_release(encode(valid), "2.8.0"),
          "equal version does not offer an update");
    check(!wardogs::updates::parse_release(encode(valid), "3.0.0"),
          "older release does not downgrade the installed version");
    check(wardogs::updates::parse_release(
              encode(release_object("archive", "v2.10.0")), "2.9.0").has_value(),
          "multi-digit semantic components compare numerically");
    auto object = valid;
    object.insert("draft", true);
    check(!wardogs::updates::parse_release(encode(object), "2.7.0"),
          "draft release is ignored");
    object = valid;
    object.insert("prerelease", true);
    object.insert("tag_name", "v2.8.0-rc1");
    check(!wardogs::updates::parse_release(encode(object), "2.7.0"),
          "marked prerelease is ignored before stable-version parsing");
    for (const auto& tag : {"v2.8.0-rc1", "2.8.0", "v2.8.0.1", "v02.8.0",
                           "v2147483648.8.0", "v2.8.0\n"}) {
        object = valid;
        object.insert("tag_name", tag);
        rejects(object, "unsupported or overflowed stable tag is rejected");
    }
    try {
        wardogs::updates::parse_release("{}", "2.7.0\n");
        check(false, "current version rejects trailing whitespace");
    } catch (const std::runtime_error&) {
        check(true, "current version rejects trailing whitespace");
    }
    object = valid;
    object.remove("draft");
    rejects(object, "missing stability flags are rejected");
    object = valid;
    object.insert("assets", QJsonArray{});
    rejects(object, "missing compatible portable archive is rejected");
    object = valid;
    const auto asset = object.value("assets").toArray().at(0);
    object.insert("assets", QJsonArray{asset, asset});
    rejects(object, "duplicate expected archive names are rejected");
    for (const auto& bad : {"https://example.com/archive.zip",
                            "http://github.com/sany86russ/Wardogs-Fire-Control/releases/download/v2.8.0/WardogsFireControl-v2.8.0-win-x64.zip",
                            "https://user@github.com/sany86russ/Wardogs-Fire-Control/releases/download/v2.8.0/WardogsFireControl-v2.8.0-win-x64.zip",
                            "https://github.com:443/sany86russ/Wardogs-Fire-Control/releases/download/v2.8.0/WardogsFireControl-v2.8.0-win-x64.zip",
                            "https://github.com/sany86russ/Other/releases/download/v2.8.0/WardogsFireControl-v2.8.0-win-x64.zip",
                            "https://github.com/sany86russ/Wardogs-Fire-Control/releases/download/v2.8.0/WardogsFireControl-v2.8.0-win-x64.zip?q=1",
                            "https://github.com/sany86russ/Wardogs-Fire-Control/releases/download/v2.8.0/WardogsFireControl-v2.8.0-win-x64.zip#fragment"}) {
        object = valid;
        change_asset(object, [&](QJsonObject& item) { item.insert("browser_download_url", bad); });
        rejects(object, "unsafe or foreign archive URL is rejected");
    }
    object = valid;
    object.insert("html_url", "https://github.com/sany86russ/Other/releases/tag/v2.8.0");
    rejects(object, "release notes link cannot point at another repository");
    for (const auto& bad : {"", "sha256:abcd", "md5:0123456789abcdef",
                           "sha256:ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff\n",
                           "sha256:gggggggggggggggggggggggggggggggggggggggggggggggggggggggggggggggg"}) {
        object = valid;
        change_asset(object, [&](QJsonObject& item) { item.insert("digest", bad); });
        rejects(object, "missing or malformed digest is rejected");
    }
    for (const double size : {-1.0, 0.0, 3.5, 536870913.0}) {
        object = valid;
        change_asset(object, [&](QJsonObject& item) { item.insert("size", size); });
        rejects(object, "non-positive fractional or oversized archive is rejected");
    }
    try {
        wardogs::updates::parse_release("not json", "2.7.0");
        check(false, "malformed JSON is rejected");
    } catch (const std::runtime_error&) {
        check(true, "malformed JSON is rejected");
    }
}

void check_tests() {
    MockNetwork network;
    wardogs::updates::Service service(nullptr, &network);
    network.responses.push_back({200, encode(release_object())});
    bool done = false;
    service.check("2.7.0", [&](auto release, QString error) {
        check(release.has_value() && error.isEmpty(), "asynchronous metadata check succeeds offline");
        check(!service.busy(), "service is idle before completion callback");
        done = true;
    });
    check(service.busy(), "check returns immediately while network request is pending");
    wait_for(done, service);
    check(network.requests.size() == 1 && network.requests.front().url().toString() ==
          "https://api.github.com/repos/sany86russ/Wardogs-Fire-Control/releases/latest",
          "release API endpoint is fixed to the author's public repository");
    check(network.requests.front().transferTimeout() == 15000 &&
          network.requests.front().attribute(QNetworkRequest::RedirectPolicyAttribute) ==
              QNetworkRequest::ManualRedirectPolicy,
          "metadata has a bounded timeout and manual redirect policy");
    check(!network.requests.front().rawHeader("User-Agent").isEmpty() &&
          network.requests.front().rawHeader("Accept") == "application/vnd.github+json",
          "GitHub API request supplies its required headers");
    network.responses.push_back({404, "not found", {}, QNetworkReply::ContentNotFoundError});
    done = false;
    service.check("2.7.0", [&](auto release, QString error) {
        check(!release && error.isEmpty(), "HTTP 404 means no published stable release");
        done = true;
    });
    wait_for(done, service);
    network.responses.push_back({200, QByteArray(1024 * 1024 + 1, 'x')});
    done = false;
    service.check("2.7.0", [&](auto release, QString error) {
        check(!release && !error.isEmpty(), "metadata streaming stops at its one MiB limit");
        done = true;
    });
    wait_for(done, service);
    network.responses.push_back({302, {}, QUrl("https://example.com/metadata")});
    done = false;
    const auto before = network.requests.size();
    service.check("2.7.0", [&](auto release, QString error) {
        check(!release && !error.isEmpty(), "metadata redirects are rejected");
        done = true;
    });
    wait_for(done, service);
    check(network.requests.size() == before + 1, "unsafe metadata redirect is never requested");
}

void download_tests() {
    QTemporaryDir directory;
    check(directory.isValid(), "private temporary download test directory exists");
    const QByteArray payload = QByteArray("PK", 2) + QByteArray::fromHex("030400ff") +
                               QByteArray(" verified portable bytes");
    const auto release = *wardogs::updates::parse_release(encode(release_object(payload)), "2.7.0");
    MockNetwork network;
    wardogs::updates::Service service(nullptr, &network);
    network.responses.push_back({200, payload, {}, QNetworkReply::NoError, 3});
    const auto target = directory.filePath("verified.zip");
    bool done = false;
    qint64 latest_progress{};
    service.download(release, target, [&](qint64 received, qint64 total) {
        check(received >= latest_progress && received <= total && total == payload.size(),
              "streamed progress is monotonic and uses the expected asset size");
        latest_progress = received;
    }, [&](QString path, QString error) {
        check(path == target && error.isEmpty(), "verified archive is promoted to its final path");
        done = true;
    });
    wait_for(done, service);
    QFile downloaded(target);
    check(downloaded.open(QIODevice::ReadOnly) && downloaded.readAll() == payload,
          "streamed binary download preserves every byte");
    downloaded.close();
    check(!QFile::exists(target + ".partial"), "successful download leaves no partial archive");
    const auto requests_before = network.requests.size();
    done = false;
    service.download(release, target, {}, [&](QString path, QString error) {
        check(path.isEmpty() && !error.isEmpty(), "existing destination is never overwritten");
        done = true;
    });
    check(done && network.requests.size() == requests_before,
          "occupied destination fails before any network request");

    const auto fails_download = [&](const QByteArray& body, const char* message) {
        const auto destination = directory.filePath(QStringLiteral("failure-%1.zip").arg(assertions));
        network.responses.push_back({200, body, {}, QNetworkReply::NoError, 4});
        bool finished = false;
        service.download(release, destination, {}, [&](QString path, QString error) {
            check(path.isEmpty() && !error.isEmpty(), message);
            finished = true;
        });
        wait_for(finished, service);
        check(!QFile::exists(destination) && !QFile::exists(destination + ".partial"),
              "failed verification removes the owned partial archive");
    };
    fails_download(payload.left(payload.size() - 1), "truncated archive is rejected");
    fails_download(payload + 'x', "oversized stream is rejected before promotion");
    auto changed = payload;
    changed[0] = 'X';
    fails_download(changed, "equal-size archive with wrong SHA-256 is rejected");

    const auto blocked_target = directory.filePath("blocked.zip");
    network.responses.push_back({302, "ignored redirect body", QUrl("https://example.com/evil.zip")});
    done = false;
    const auto blocked_before = network.requests.size();
    service.download(release, blocked_target, {}, [&](QString path, QString error) {
        check(path.isEmpty() && !error.isEmpty(), "foreign download redirect is rejected");
        done = true;
    });
    wait_for(done, service);
    check(network.requests.size() == blocked_before + 1,
          "foreign host is rejected before sending a request to it");

    const auto redirect_target = directory.filePath("redirect.zip");
    network.responses.push_back({302, "redirect body must not enter archive",
        QUrl("https://release-assets.githubusercontent.com/github-production-release-asset/asset?signature=value")});
    network.responses.push_back({200, payload});
    done = false;
    service.download(release, redirect_target, {}, [&](QString path, QString error) {
        check(path == redirect_target && error.isEmpty(), "approved HTTPS asset redirect is followed and verified");
        done = true;
    });
    wait_for(done, service);

    const auto loop_target = directory.filePath("redirect-loop.zip");
    for (int index = 0; index < 6; ++index)
        network.responses.push_back({302, {},
            QUrl("https://release-assets.githubusercontent.com/loop")});
    done = false;
    const auto loop_before = network.requests.size();
    service.download(release, loop_target, {}, [&](QString path, QString error) {
        check(path.isEmpty() && !error.isEmpty(), "redirect loop stops after five followed redirects");
        done = true;
    });
    wait_for(done, service);
    check(network.requests.size() == loop_before + 6,
          "bounded redirect loop never sends a seventh request");

    const auto cancel_target = directory.filePath("cancel.zip");
    network.responses.push_back({200, payload, {}, QNetworkReply::NoError, 3});
    done = false;
    int callbacks = 0;
    service.download(release, cancel_target, [&](qint64, qint64) { service.cancel(); },
        [&](QString path, QString error) {
            ++callbacks;
            check(path.isEmpty() && !error.isEmpty(), "cancellation reports failure without a final archive");
            done = true;
        });
    wait_for(done, service);
    QCoreApplication::processEvents();
    check(callbacks == 1 && !service.busy() && !QFile::exists(cancel_target + ".partial"),
          "cancellation invokes callback once, cleans partial file, and permits reuse");

    const auto stall_target = directory.filePath("stall.zip");
    network.responses.push_back({200, {}, {}, QNetworkReply::NoError, 4096, false});
    done = false;
    service.download(release, stall_target, {}, [&](QString path, QString error) {
        check(path.isEmpty() && !error.isEmpty(), "stalled transfer can be cancelled immediately");
        done = true;
    });
    bool busy_rejected = false;
    service.check("2.7.0", [&](auto value, QString error) {
        check(!value && !error.isEmpty(), "concurrent operation is rejected without disrupting active download");
        busy_rejected = true;
    });
    check(busy_rejected && service.busy(), "service preserves active operation when a second call arrives");
    service.cancel();
    check(done && !service.busy(), "cancel does not wait for a stalled network response");

    const auto occupied_partial = directory.filePath("occupied.zip");
    QFile original(occupied_partial + ".partial");
    check(original.open(QIODevice::WriteOnly), "existing partial fixture can be created");
    original.write("preserve me");
    original.close();
    done = false;
    service.download(release, occupied_partial, {}, [&](QString path, QString error) {
        check(path.isEmpty() && !error.isEmpty(), "existing partial file prevents download ownership collision");
        done = true;
    });
    check(done && original.open(QIODevice::ReadOnly) && original.readAll() == "preserve me",
          "service never overwrites or removes a pre-existing partial file");
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    parser_tests();
    check_tests();
    download_tests();
    std::cout << assertions << " updater assertions, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
