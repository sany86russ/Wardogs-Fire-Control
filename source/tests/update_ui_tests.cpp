#include "update_ui.hpp"
#include "localization.hpp"
#include "wardogs/logger.hpp"

#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProgressDialog>
#include <QPointer>
#include <QPushButton>
#include <QRawFont>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QStyleFactory>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>

namespace {

int failures{};
int assertions{};

void check(bool value, const char* message) {
    ++assertions;
    if (value) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

bool cyrillic(const QString& value) {
    static const QRegularExpression letters(QStringLiteral("[\\x{0400}-\\x{04ff}]"));
    return letters.match(value).hasMatch();
}

bool wait_for(const std::function<bool()>& condition) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!condition() && elapsed.elapsed() < 3000)
        QApplication::processEvents(QEventLoop::AllEvents, 10);
    const bool result = condition();
    check(result, "offline UI operation completes within three seconds");
    return result;
}

QByteArray release_json() {
    const QString tag = QStringLiteral("v2.8.0");
    const QString name = QStringLiteral("WardogsFireControl-v2.8.0-win-x64.zip");
    const QString prefix = QStringLiteral("https://github.com/sany86russ/Wardogs-Fire-Control/releases/");
    const QByteArray archive("held download");
    const auto digest = QCryptographicHash::hash(archive, QCryptographicHash::Sha256).toHex();
    return QJsonDocument(QJsonObject{
        {QStringLiteral("draft"), false}, {QStringLiteral("prerelease"), false},
        {QStringLiteral("tag_name"), tag},
        {QStringLiteral("html_url"), prefix + QStringLiteral("tag/") + tag},
        {QStringLiteral("assets"), QJsonArray{QJsonObject{
            {QStringLiteral("name"), name},
            {QStringLiteral("browser_download_url"), prefix + QStringLiteral("download/") + tag + '/' + name},
            {QStringLiteral("size"), archive.size()},
            {QStringLiteral("digest"), QStringLiteral("sha256:") + QString::fromLatin1(digest)}}}}})
        .toJson(QJsonDocument::Compact);
}

void write_portable_helper(const QString& directory, const QByteArray& script = QByteArray("installed trusted helper")) {
    const auto digest = QCryptographicHash::hash(script, QCryptographicHash::Sha256).toHex();
    const auto manifest = QJsonDocument(QJsonObject{
        {QStringLiteral("product"), QStringLiteral("WARDOGS Fire Control")},
        {QStringLiteral("version"), QStringLiteral("2.7.0")},
        {QStringLiteral("files"), QJsonArray{QJsonObject{
            {QStringLiteral("path"), QStringLiteral("Update.ps1")},
            {QStringLiteral("length"), script.size()},
            {QStringLiteral("sha256"), QString::fromLatin1(digest)}}}}})
        .toJson(QJsonDocument::Compact);
    for (const auto& entry : {std::pair{QStringLiteral("Update.ps1"), script},
                              std::pair{QStringLiteral("package-manifest.json"), manifest}}) {
        QFile file(QDir(directory).filePath(entry.first));
        if (!file.open(QIODevice::WriteOnly) || file.write(entry.second) != entry.second.size())
            throw std::runtime_error("Cannot create a portable updater fixture");
    }
}

struct Response {
    QByteArray body;
    bool finish{true};
    int status{200};
    QNetworkReply::NetworkError error{QNetworkReply::NoError};
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
        setHeader(QNetworkRequest::ContentLengthHeader, response_.body.size());
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        QTimer::singleShot(0, this, [this] {
            if (isFinished()) return;
            available_ = response_.body.size();
            emit metaDataChanged();
            emit readyRead();
            emit downloadProgress(available_, available_);
            if (response_.finish && !isFinished()) {
                if (response_.error != NoError)
                    setError(response_.error, QStringLiteral("Error transferring CDN?signature=must-not-escape"));
                setFinished(true); emit finished();
            }
        });
    }
    void abort() override {
        if (isFinished()) return;
        setError(OperationCanceledError, QStringLiteral("Offline test cancellation"));
        setFinished(true);
        QTimer::singleShot(0, this, [this] { emit finished(); });
    }
    qint64 bytesAvailable() const override { return available_ - position_ + QNetworkReply::bytesAvailable(); }

protected:
    qint64 readData(char* output, qint64 maximum) override {
        const qint64 count = std::min(maximum, available_ - position_);
        if (count <= 0) return isFinished() ? -1 : 0;
        std::memcpy(output, response_.body.constData() + position_, static_cast<std::size_t>(count));
        position_ += count;
        return count;
    }

private:
    Response response_;
    qint64 position_{}, available_{};
};

class MockNetwork final : public QNetworkAccessManager {
public:
    std::deque<Response> responses;
    QList<QUrl> requests;

protected:
    QNetworkReply* createRequest(Operation operation, const QNetworkRequest& request, QIODevice*) override {
        requests.append(request.url());
        check(!responses.empty(), "every UI request is served by an explicit offline fixture");
        Response response;
        if (!responses.empty()) { response = std::move(responses.front()); responses.pop_front(); }
        return new MockReply(request, operation, std::move(response), this);
    }
};

void screenshot(QWidget& window, const QString& name) {
    QDir().mkpath(QStringLiteral("Testing"));
    window.resize(900, 130);
    window.show();
    QApplication::processEvents();
    check(window.grab().save(QDir::current().filePath(QStringLiteral("Testing/") + name + QStringLiteral(".png"))),
          "the offline update banner screenshot is saved");
    window.hide();
}

void banner_and_language_tests() {
    wardogs::i18n::set_language(wardogs::UiLanguage::russian);
    QWidget window;
    auto* layout = new QVBoxLayout(&window);
    layout->addWidget(new QLabel(QStringLiteral("WARDOGS Fire Control · v2.7.0")));
    MockNetwork network;
    network.responses.push_back({release_json()});
    auto controller = std::make_unique<wardogs::updates::Controller>(
        &window, QStringLiteral("2.7.0"), QDir::tempPath(), &network);
    layout->addWidget(controller->banner_widget());
    controller->check(true);
    wait_for([&] { return !controller->banner_widget()->isHidden(); });
    const auto text = window.findChild<QLabel*>(QStringLiteral("updateBannerText"));
    const auto update = window.findChild<QPushButton*>(QStringLiteral("installUpdateButton"));
    const auto notes = window.findChild<QPushButton*>(QStringLiteral("updateNotesButton"));
    const auto later = window.findChild<QPushButton*>(QStringLiteral("dismissUpdateButton"));
    check(text && text->text().contains(QStringLiteral("2.8.0")) && text->text().contains(QStringLiteral("2.7.0")),
          "manual checking presents the new and installed versions");
    check(update && update->text() == QString::fromUtf8("Обновить") &&
          notes && notes->text() == QString::fromUtf8("Что нового") &&
          later && later->text() == QString::fromUtf8("Позже"),
          "all three Russian update banner actions are present");
    screenshot(window, QStringLiteral("update-banner-ru"));
    wardogs::i18n::set_language(wardogs::UiLanguage::english);
    check(update->text() == QStringLiteral("Update") && notes->text() == QStringLiteral("What's new") &&
          later->text() == QStringLiteral("Later") && !cyrillic(text->text()) &&
          !cyrillic(update->toolTip()) && !cyrillic(later->toolTip()),
          "the existing update banner and tooltips switch to English immediately");
    screenshot(window, QStringLiteral("update-banner-en"));
    later->click();
    check(controller->banner_widget()->isHidden() && network.requests.size() == 1,
          "Later dismisses the notification without downloading an update");
    network.responses.push_back({release_json()});
    controller->check(true);
    wait_for([&] { return !controller->banner_widget()->isHidden(); });
    check(network.requests.size() == 2, "manual checking can redisplay a dismissed notification");
}

void current_version_message_test() {
    QWidget window;
    MockNetwork network;
    network.responses.push_back({release_json()});
    wardogs::updates::Controller controller(&window, QStringLiteral("2.8.0"), QDir::tempPath(), &network);
    QString notice;
    QTimer dismiss;
    dismiss.setInterval(1);
    QObject::connect(&dismiss, &QTimer::timeout, &window, [&] {
        for (auto* widget : QApplication::topLevelWidgets()) {
            if (auto* message = qobject_cast<QMessageBox*>(widget); message && message->isVisible()) {
                notice = message->text();
                message->accept();
            }
        }
    });
    dismiss.start();
    controller.check(true);
    wait_for([&] { return !notice.isEmpty(); });
    check(notice.contains(QStringLiteral("2.8.0")) && notice.startsWith(QStringLiteral("The latest available version")),
          "manual checking reports an up-to-date copy in the selected language");
    check(controller.banner_widget()->isHidden(), "an up-to-date copy does not show the update banner");
}

void download_cancellation_and_lifetime_tests() {
    QWidget window;
    QTemporaryDir install;
    check(install.isValid(), "the fake portable installation is created");
    write_portable_helper(install.path());
    MockNetwork network;
    network.responses.push_back({release_json()});
    network.responses.push_back({QByteArray("held download"), false});
    auto controller = std::make_unique<wardogs::updates::Controller>(
        &window, QStringLiteral("2.7.0"), install.path(), &network);
    controller->check(false);
    wait_for([&] { return !controller->banner_widget()->isHidden(); });
    window.findChild<QPushButton*>(QStringLiteral("installUpdateButton"))->click();
    wait_for([&] { return network.requests.size() == 2 && window.findChild<QProgressDialog*>(); });
    const QPointer<QProgressDialog> progress = window.findChild<QProgressDialog*>();
    check(progress && progress->isVisible(), "a user-requested download presents cancellable modal progress");
    wardogs::i18n::set_language(wardogs::UiLanguage::russian);
    check(progress->windowTitle() == QString::fromUtf8("Обновление WARDOGS Fire Control"),
          "an existing download dialog follows the Russian language selection");
    wardogs::i18n::set_language(wardogs::UiLanguage::english);
    check(progress->windowTitle() == QStringLiteral("WARDOGS Fire Control update"),
          "the same download dialog follows the English language selection");
    for (auto* label : progress->findChildren<QLabel*>())
        check(!cyrillic(label->text()), "English progress messages contain no untranslated Russian text");
    QMetaObject::invokeMethod(progress, "canceled", Qt::DirectConnection);
    QApplication::processEvents();
    check(!progress || !progress->isVisible(), "cancellation dismisses progress while the application remains open");
    check(QDir(install.path()).entryList({QStringLiteral(".wardogs-update-*")}, QDir::Dirs | QDir::Hidden).isEmpty(),
          "cancellation removes the staged download directory and partial archive");
    check(!QFile::exists(QDir(install.path()).filePath(QStringLiteral("install-approved.json"))),
          "a cancelled download never approves installation");
    controller.reset();
    network.responses.push_back({release_json(), false});
    auto pending = std::make_unique<wardogs::updates::Controller>(
        &window, QStringLiteral("2.7.0"), install.path(), &network);
    pending->check(false);
    pending.reset();
    QApplication::processEvents();
    check(network.requests.size() == 3, "destroying a controller cancels pending callbacks without another request");
}

void installed_helper_tests() {
    QTemporaryDir install;
    write_portable_helper(install.path());
    check(wardogs::updates::installed_installer_script(install.path(), "2.7.0") == "installed trusted helper",
          "only hash-checked installed helper bytes are selected for execution");
    const auto staged = QDir(install.path()).filePath(".wardogs-update-ABC123/staged");
    check(QDir().mkpath(staged), "a deliberately different downloaded helper fixture can be staged");
    QFile untrusted(QDir(staged).filePath("Update.ps1"));
    check(untrusted.open(QIODevice::WriteOnly) && untrusted.write("downloaded script is not the trust anchor") > 0,
          "the new package may contain different installer code");
    untrusted.close();
    check(wardogs::updates::installed_installer_script(install.path(), "2.7.0") == "installed trusted helper",
          "downloaded installer code cannot replace the installed bootstrap trust anchor");
    const auto rejects = [&](const QString& version, const char* message) {
        bool rejected = false;
        try { (void)wardogs::updates::installed_installer_script(install.path(), version); }
        catch (const std::runtime_error&) { rejected = true; }
        check(rejected, message);
    };
    rejects("2.8.0", "a mixed installation version cannot launch an unrelated helper");
    QFile helper(QDir(install.path()).filePath("Update.ps1"));
    check(helper.open(QIODevice::WriteOnly) && helper.write("tampered trusted helper") > 0,
          "a damaged installed helper fixture can be written");
    helper.close();
    rejects("2.7.0", "a changed installed helper is rejected before PowerShell can execute it");
    write_portable_helper(install.path());
    check(helper.remove(), "installed helper fixture can be removed");
    rejects("2.7.0", "missing installed helper produces a recoverable preflight failure");
    write_portable_helper(install.path());
    QFile manifest(QDir(install.path()).filePath("package-manifest.json"));
    check(manifest.open(QIODevice::WriteOnly) && manifest.write("not JSON") > 0,
          "a damaged installed manifest fixture can be written");
    manifest.close();
    rejects("2.7.0", "malformed installed metadata cannot authorize helper execution");
}

void zero_byte_failure_tests() {
    for (const bool english : {false, true}) {
        wardogs::i18n::set_language(english ? wardogs::UiLanguage::english : wardogs::UiLanguage::russian);
        QWidget window;
        QTemporaryDir install;
        write_portable_helper(install.path());
        const auto log_path = QDir(install.path()).filePath("update-test.log");
        check(wardogs::initialize_session_log(std::filesystem::path(log_path.toStdWString()), "2.7.0"),
              "zero-byte updater failures have an isolated diagnostic log fixture");
        MockNetwork network;
        network.responses.push_back({release_json()});
        network.responses.push_back({QByteArray("access denied"), true, 403, QNetworkReply::ContentAccessDenied});
        wardogs::updates::Controller controller(&window, QStringLiteral("2.7.0"), install.path(), &network);
        controller.check(false);
        wait_for([&] { return !controller.banner_widget()->isHidden(); });
        bool observed = false;
        QString text, details;
        QTimer dismiss;
        dismiss.setInterval(1);
        QObject::connect(&dismiss, &QTimer::timeout, &window, [&] {
            for (auto* widget : QApplication::topLevelWidgets()) {
                auto* message = qobject_cast<QMessageBox*>(widget);
                if (!message || !message->isVisible() || message->objectName() != "updateFailureDialog") continue;
                text = message->text(); details = message->detailedText();
                const auto retry = message->findChild<QPushButton*>("retryUpdateButton");
                const auto release = message->findChild<QPushButton*>("openFailedUpdateReleaseButton");
                check(retry && release && retry->isVisible() && release->isVisible(),
                      "zero-byte failure exposes Retry and the manual release action");
                check(retry && retry->text() == (english ? QStringLiteral("Retry") : QString::fromUtf8("Повторить")),
                      "the recovery action follows the selected language");
                QDir().mkpath("Testing");
                check(message->grab().save(QDir::current().filePath(english ?
                    "Testing/update-error-en.png" : "Testing/update-error-ru.png")),
                    "the production recovery dialog is captured on the isolated CI runner");
                observed = true;
                message->reject();
            }
        });
        dismiss.start();
        window.findChild<QPushButton*>("installUpdateButton")->click();
        wait_for([&] { return observed; });
        dismiss.stop();
        check(text.contains(english ? QStringLiteral("Stage:") : QString::fromUtf8("Этап:")) &&
              text.contains(english ? QStringLiteral("Downloading") : QString::fromUtf8("Скачивание")),
              "the failure dialog identifies downloading as the failing phase before progress");
        check(details.contains("host github.com") && details.contains("HTTP 403") &&
              details.contains("network error 201") && !details.contains("must-not-escape") &&
              !details.contains("signature="),
              "technical details keep safe error codes while omitting signed CDN queries");
        check(network.requests.size() == 2 && QDir(install.path()).entryList(
                  {QStringLiteral(".wardogs-update-*")}, QDir::Dirs | QDir::Hidden).isEmpty(),
              "a zero-byte access failure does not start the installer or retain a partial archive");
        wardogs::shutdown_session_log();
        QFile log(log_path);
        check(log.open(QIODevice::ReadOnly), "the failed update diagnostic can be read after cleanup");
        const auto diagnostic = log.readAll();
        check(diagnostic.contains("updates.request stage=archive host=github.com") &&
              diagnostic.contains("updates.failed") && diagnostic.contains("HTTP 403") &&
              !diagnostic.contains("must-not-escape") && !diagnostic.contains("signature="),
              "the durable session log retains safe initial-connection evidence after deleting staging");
    }
}

void failed_prepare_start_test() {
    // Only the isolated Windows CI runner executes this fixture. The fake
    // executable is deliberately invalid: no PowerShell or installer can run.
    wardogs::i18n::set_language(wardogs::UiLanguage::english);
    QWidget window;
    QTemporaryDir install;
    QTemporaryDir system;
    check(install.isValid() && system.isValid(), "failed-start fixtures have private temporary directories");
    write_portable_helper(install.path());
    const auto fake_powershell = QDir(system.path()).filePath("System32/WindowsPowerShell/v1.0/powershell.exe");
    check(QDir().mkpath(QFileInfo(fake_powershell).absolutePath()),
          "the fake Windows PowerShell directory can be created");
    QFile executable(fake_powershell);
    check(executable.open(QIODevice::WriteOnly) && executable.write("not a Windows executable") > 0,
          "the helper-start fixture contains invalid executable bytes instead of runnable code");
    executable.close();
    struct SystemRootScope {
        QByteArray previous{qgetenv("SystemRoot")};
        bool existed{qEnvironmentVariableIsSet("SystemRoot")};
        explicit SystemRootScope(const QByteArray& replacement) {
            if (!qputenv("SystemRoot", replacement)) throw std::runtime_error("Cannot set fake SystemRoot");
        }
        ~SystemRootScope() {
            if (existed) qputenv("SystemRoot", previous);
            else qunsetenv("SystemRoot");
        }
    } system_root(system.path().toUtf8());
    MockNetwork network;
    network.responses.push_back({release_json()});
    network.responses.push_back({QByteArray("held download")});
    wardogs::updates::Controller controller(&window, QStringLiteral("2.7.0"), install.path(), &network);
    controller.check(false);
    wait_for([&] { return !controller.banner_widget()->isHidden(); });
    bool observed = false;
    QString text, details;
    QTimer dismiss;
    dismiss.setInterval(1);
    QObject::connect(&dismiss, &QTimer::timeout, &window, [&] {
        for (auto* widget : QApplication::topLevelWidgets()) {
            auto* message = qobject_cast<QMessageBox*>(widget);
            if (!message || !message->isVisible() || message->objectName() != "updateFailureDialog") continue;
            text = message->text();
            details = message->detailedText();
            // FailedToStart can be synchronous on Windows. Exercise deferred
            // deletion inside its nested error dialog before start() returns,
            // including the QProcess-owned deadline that previously dangled.
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            observed = true;
            message->reject();
        }
    });
    dismiss.start();
    window.findChild<QPushButton*>("installUpdateButton")->click();
    wait_for([&] { return observed; });
    dismiss.stop();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    check(text.contains("Stage:") && text.contains("Preparing the update") && !details.isEmpty(),
          "a failed PowerShell start reports its preparation phase and diagnostic instead of crashing");
    check(network.requests.size() == 2 && !controller.banner_widget()->isHidden(),
          "the failed preparation follows a verified download and keeps the current application usable");
    check(QDir(install.path()).entryList({QStringLiteral(".wardogs-update-*")},
              QDir::Dirs | QDir::Hidden).isEmpty(),
          "failed helper startup removes its private work directory without approving installation");
    check(wardogs::updates::installed_installer_script(install.path(), "2.7.0") == "installed trusted helper",
          "failed helper startup preserves the original installed payload");
}

}  // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    auto* style = QStyleFactory::create(QStringLiteral("Fusion"));
    check(style != nullptr, "the screenshot fixture uses the available Fusion widget style");
    if (style) application.setStyle(style);
    QFont font(QStringLiteral("Segoe UI"));
    font.setPixelSize(13);
    application.setFont(font);
    const QFontMetrics metrics(application.font());
    check(metrics.height() > 0 &&
          metrics.horizontalAdvance(QStringLiteral("Update")) > metrics.horizontalAdvance(QStringLiteral(" ")) &&
          metrics.horizontalAdvance(QString::fromUtf8("Обновить")) > metrics.horizontalAdvance(QStringLiteral(" ")),
          "the screenshot font has measurable Latin and Cyrillic text");
    const QRawFont raw = QRawFont::fromFont(application.font());
    check(raw.isValid() && raw.supportsCharacter(0x0041U) && raw.supportsCharacter(0x0414U) &&
          raw.supportsCharacter(0x044FU),
          "the screenshot font provides real Latin and Cyrillic glyphs instead of missing-character boxes");
    if (raw.isValid()) {
        const auto glyphs = raw.glyphIndexesForString(QString::fromUtf8("WARDOGS Доступна"));
        check(!glyphs.isEmpty() && std::all_of(glyphs.begin(), glyphs.end(), [](quint32 glyph) { return glyph != 0; }),
              "the rendered update banner text maps to existing font glyphs");
    }
    banner_and_language_tests();
    current_version_message_test();
    download_cancellation_and_lifetime_tests();
    installed_helper_tests();
    zero_byte_failure_tests();
    failed_prepare_start_test();
    std::cout << assertions << " offline update UI assertions, " << failures << " failure(s)\n";
    return failures ? 1 : 0;
}
