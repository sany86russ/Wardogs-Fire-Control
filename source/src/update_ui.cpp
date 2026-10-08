#include "update_ui.hpp"
#include "localization.hpp"
#include "wardogs/logger.hpp"

#include <Windows.h>

#include <QApplication>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QTimer>

#include <algorithm>
#include <utility>

namespace wardogs::updates {
namespace {

QString translated(const char* source) {
    return i18n::text(QString::fromUtf8(source));
}

void hide_process_window(QProcess* process) {
    process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* arguments) {
        arguments->flags |= CREATE_NO_WINDOW;
        arguments->startupInfo->dwFlags |= STARTF_USESHOWWINDOW;
        arguments->startupInfo->wShowWindow = SW_HIDE;
    });
}

}  // namespace

Controller::Controller(QWidget* window, QString current_version, QString install_directory,
                       QNetworkAccessManager* network)
    : QObject(window), window_(window), current_version_(std::move(current_version)),
      install_directory_(QDir::cleanPath(std::move(install_directory))),
      service_(new Service(this, network)), banner_(new QFrame(window)),
      ready_timer_(new QTimer(this)) {
    banner_->setObjectName(QStringLiteral("updateBanner"));
    banner_->setFrameShape(QFrame::StyledPanel);
    auto* layout = new QHBoxLayout(banner_);
    layout->setContentsMargins(12, 10, 12, 10);
    banner_text_ = new QLabel;
    banner_text_->setObjectName(QStringLiteral("updateBannerText"));
    banner_text_->setWordWrap(true);
    layout->addWidget(banner_text_, 1);
    update_button_ = new QPushButton;
    update_button_->setObjectName(QStringLiteral("installUpdateButton"));
    update_button_->setProperty("primary", true);
    notes_button_ = new QPushButton;
    notes_button_->setObjectName(QStringLiteral("updateNotesButton"));
    later_button_ = new QPushButton;
    later_button_->setObjectName(QStringLiteral("dismissUpdateButton"));
    layout->addWidget(update_button_);
    layout->addWidget(notes_button_);
    layout->addWidget(later_button_);
    connect(update_button_, &QPushButton::clicked, this, [this] { begin_update(); });
    connect(notes_button_, &QPushButton::clicked, this, [this] { open_release(); });
    connect(later_button_, &QPushButton::clicked, banner_, &QWidget::hide);
    banner_->hide();
    banner_->installEventFilter(this);
    ready_timer_->setInterval(100);
    connect(ready_timer_, &QTimer::timeout, this, [this] { poll_installer(); });
    refresh_text();
    i18n::watch(banner_);
}

Controller::~Controller() {
    shutting_down_ = true;
    if (operation_active_) cancel_operation();
}

bool Controller::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::LanguageChange &&
        (watched == banner_ || watched == progress_)) refresh_text();
    return QObject::eventFilter(watched, event);
}

void Controller::refresh_text() {
    update_button_->setText(translated("Обновить"));
    notes_button_->setText(translated("Что нового"));
    later_button_->setText(translated("Позже"));
    update_button_->setToolTip(translated("Скачать проверенный релиз GitHub, установить и перезапустить приложение"));
    later_button_->setToolTip(translated("Скрыть уведомление до следующего запуска"));
    if (pending_) {
        banner_text_->setText(translated("Доступна версия %1 · установлена %2")
            .arg(pending_->version, current_version_));
    }
    if (progress_) {
        progress_->setWindowTitle(translated("Обновление WARDOGS Fire Control"));
        progress_->setCancelButtonText(translated("Отмена"));
        QString text = i18n::text(phase_source_);
        if (received_ > 0 && total_ > 0) {
            text += translated("\nСкачано %1 из %2 МиБ")
                .arg(QString::number(received_ / (1024.0 * 1024.0), 'f', 1),
                     QString::number(total_ / (1024.0 * 1024.0), 'f', 1));
        }
        progress_->setLabelText(text);
    }
}

void Controller::check(bool manual) {
    if (operation_active_ || service_->busy()) {
        if (manual) QMessageBox::information(window_, translated("Обновления"),
            translated("Проверка или установка обновления уже выполняется."));
        return;
    }
    const QPointer<Controller> guard(this);
    service_->check(current_version_, [guard, manual](std::optional<ReleaseInfo> release, QString error) {
        if (!guard || guard->shutting_down_) return;
        if (!error.isEmpty()) {
            log_warning("updates.check_failed detail=" + error.toStdString());
            if (manual) QMessageBox::warning(guard->window_, translated("Обновления"),
                translated("Не удалось проверить обновления.\n%1").arg(error));
            return;
        }
        if (!release) {
            guard->pending_.reset();
            guard->banner_->hide();
            if (manual) QMessageBox::information(guard->window_, translated("Обновления"),
                translated("Установлена последняя доступная версия: %1.").arg(guard->current_version_));
            return;
        }
        guard->pending_ = std::move(release);
        guard->refresh_text();
        guard->banner_->show();
    });
}

void Controller::open_release() {
    const QUrl url = pending_ ? pending_->release_url
        : QUrl(QStringLiteral("https://github.com/sany86russ/Wardogs-Fire-Control/releases/latest"));
    if (!QDesktopServices::openUrl(url)) {
        QMessageBox::information(window_, translated("Обновления"),
            translated("Страница релиза: %1").arg(url.toString()));
    }
}

QString Controller::powershell_path() const {
    const QString system_root = qEnvironmentVariable("SystemRoot");
    if (!QDir::isAbsolutePath(system_root)) return {};
    const QString path = QDir(system_root).filePath(
        QStringLiteral("System32/WindowsPowerShell/v1.0/powershell.exe"));
    return QFileInfo(path).isFile() ? path : QString{};
}

QStringList Controller::helper_arguments(const QString& mode) const {
    const QString helper = mode == QStringLiteral("Prepare")
        ? QDir(install_directory_).filePath(QStringLiteral("Update.ps1"))
        : QDir(work_->path()).filePath(QStringLiteral("Update.ps1"));
    QStringList result{QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
        QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
        QStringLiteral("-File"), helper, QStringLiteral("-Mode"), mode,
        QStringLiteral("-InstallDirectory"), install_directory_,
        QStringLiteral("-WorkDirectory"), work_->path(),
        QStringLiteral("-ArchiveSha256"), QString::fromLatin1(pending_->sha256.toHex()),
        QStringLiteral("-Version"), pending_->version};
    if (mode == QStringLiteral("Install")) {
        result << QStringLiteral("-ParentId") << QString::number(GetCurrentProcessId());
    }
    return result;
}

void Controller::begin_update() {
    if (!pending_ || operation_active_ || service_->busy()) return;
    const QDir install(install_directory_);
    if (!QFileInfo(install.filePath(QStringLiteral("package-manifest.json"))).isFile() ||
        !QFileInfo(install.filePath(QStringLiteral("Update.ps1"))).isFile()) {
        QMessageBox message(QMessageBox::Information, translated("Обновления"),
            translated("Эта копия не содержит установщика обновлений. Скачайте Windows ZIP со страницы релиза и распакуйте его в отдельную папку."),
            QMessageBox::NoButton, window_);
        auto* release = message.addButton(translated("Открыть релиз"), QMessageBox::ActionRole);
        message.addButton(translated("Закрыть"), QMessageBox::RejectRole);
        i18n::watch(&message);
        message.exec();
        if (message.clickedButton() == release) open_release();
        return;
    }
    if (powershell_path().isEmpty()) {
        QMessageBox::warning(window_, translated("Обновления"),
            translated("Windows PowerShell 5.1 не найден. Скачайте обновление вручную со страницы релиза."));
        return;
    }
    if (before_install_ && !before_install_()) return;
    work_ = std::make_unique<QTemporaryDir>(install.filePath(QStringLiteral(".wardogs-update-XXXXXX")));
    if (!work_->isValid()) {
        work_.reset();
        QMessageBox::warning(window_, translated("Обновления"),
            translated("Не удалось создать папку обновления рядом с приложением. Проверьте права записи или скачайте релиз вручную."));
        return;
    }
    cancelled_ = false;
    installer_started_ = false;
    operation_active_ = true;
    received_ = total_ = 0;
    phase_source_ = QString::fromUtf8("Скачивание и проверка релиза GitHub…");
    progress_ = new QProgressDialog(window_);
    progress_->setObjectName(QStringLiteral("updateProgressDialog"));
    progress_->setMinimumDuration(0);
    progress_->setWindowModality(Qt::ApplicationModal);
    progress_->setAutoClose(false);
    progress_->setAutoReset(false);
    progress_->setRange(0, 1000);
    const QPointer<Controller> lifetime(this);
    progress_->setValue(0);
    if (!lifetime || !operation_active_ || !progress_) return;
    progress_->setMinimumWidth(430);
    progress_->installEventFilter(this);
    connect(progress_, &QProgressDialog::canceled, this, [this] { cancel_operation(); });
    refresh_text();
    i18n::watch(progress_);
    progress_->show();
    const QPointer<Controller> guard(this);
    service_->download(*pending_, QDir(work_->path()).filePath(QStringLiteral("download.zip")),
        [guard](qint64 received, qint64 total) {
            if (!guard || guard->cancelled_ || !guard->progress_) return;
            guard->received_ = received;
            guard->total_ = total;
            if (total > 0) guard->progress_->setValue(
                static_cast<int>(std::clamp(1000.0 * received / total, 0.0, 999.0)));
            if (!guard || guard->cancelled_ || !guard->operation_active_ || !guard->progress_) return;
            guard->refresh_text();
        },
        [guard](QString archive, QString error) {
            if (!guard || guard->cancelled_ || !guard->operation_active_) return;
            if (!error.isEmpty() || archive.isEmpty()) {
                guard->fail_operation(error.isEmpty() ? QStringLiteral("The verified archive is missing.") : error);
                return;
            }
            guard->prepare_update();
        });
}

void Controller::prepare_update() {
    phase_source_ = QString::fromUtf8("Подготовка обновления и проверка файлов…");
    received_ = total_ = 0;
    progress_->setRange(0, 0);
    refresh_text();
    prepare_ = new QProcess(this);
    prepare_->setProgram(powershell_path());
    prepare_->setArguments(helper_arguments(QStringLiteral("Prepare")));
    prepare_->setWorkingDirectory(work_->path());
    hide_process_window(prepare_);
    connect(prepare_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (!cancelled_ && operation_active_ && error == QProcess::FailedToStart)
            fail_operation(prepare_ ? prepare_->errorString() : QStringLiteral("Could not start the update helper."));
    });
    connect(prepare_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
        [this](int code, QProcess::ExitStatus status) {
            if (cancelled_ || !operation_active_) return;
            if (status != QProcess::NormalExit || code != 0) {
                QFile log(QDir(work_->path()).filePath(QStringLiteral("error.log")));
                const QString detail = log.open(QIODevice::ReadOnly)
                    ? QString::fromUtf8(log.read(8192))
                    : QStringLiteral("Update preparation failed (exit %1).").arg(code);
                fail_operation(detail);
                return;
            }
            launch_installer();
        });
    prepare_->start();
}

void Controller::launch_installer() {
    const QString helper = QDir(work_->path()).filePath(QStringLiteral("Update.ps1"));
    if (!QFile::copy(QDir(install_directory_).filePath(QStringLiteral("Update.ps1")), helper)) {
        fail_operation(QStringLiteral("Could not stage the update helper."));
        return;
    }
    QProcess process;
    process.setProgram(powershell_path());
    process.setArguments(helper_arguments(QStringLiteral("Install")));
    process.setWorkingDirectory(work_->path());
    hide_process_window(&process);
    qint64 installer_pid{};
    if (!process.startDetached(&installer_pid)) {
        fail_operation(QStringLiteral("Could not start the installation helper."));
        return;
    }
    installer_started_ = true;
    installer_handle_ = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE,
        static_cast<DWORD>(installer_pid));
    // A detached helper may still be reading this directory after a timeout or
    // cancellation. Keep it until that helper has safely observed the marker.
    work_->setAutoRemove(false);
    ready_wait_.start();
    phase_source_ = QString::fromUtf8("Проверка готовности установщика…");
    refresh_text();
    ready_timer_->start();
}

void Controller::poll_installer() {
    if (!operation_active_ || cancelled_ || !work_) return;
    QFile error(QDir(work_->path()).filePath(QStringLiteral("error.log")));
    if (error.exists() && error.open(QIODevice::ReadOnly)) {
        fail_operation(QString::fromUtf8(error.read(8192)));
        return;
    }
    QFile ready(QDir(work_->path()).filePath(QStringLiteral("install-ready.json")));
    if (ready.open(QIODevice::ReadOnly)) {
        const auto object = QJsonDocument::fromJson(ready.read(4096)).object();
        if (object.value(QStringLiteral("state")).toString() == QStringLiteral("ready") &&
            object.value(QStringLiteral("parent_id")).toDouble() == static_cast<double>(GetCurrentProcessId())) {
            QSaveFile approval(QDir(work_->path()).filePath(QStringLiteral("install-approved.json")));
            const QByteArray confirmation = QJsonDocument(QJsonObject{
                {QStringLiteral("state"), QStringLiteral("approved")},
                {QStringLiteral("parent_id"), static_cast<qint64>(GetCurrentProcessId())}})
                .toJson(QJsonDocument::Compact);
            if (!approval.open(QIODevice::WriteOnly) ||
                approval.write(confirmation) != confirmation.size() || !approval.commit()) {
                fail_operation(QStringLiteral("Could not confirm installation readiness: ") + approval.errorString());
                return;
            }
            ready_timer_->stop();
            work_->setAutoRemove(false);
            shutting_down_ = true;
            finish_operation();
            QApplication::quit();
            return;
        }
    }
    if (ready_wait_.elapsed() >= 60000) {
        fail_operation(QStringLiteral("The installation helper did not confirm readiness within 60 seconds."));
    }
}

void Controller::cancel_operation() {
    if (!operation_active_) return;
    cancelled_ = true;
    service_->cancel();
    ready_timer_->stop();
    if (prepare_) {
        prepare_->disconnect(this);
        if (prepare_->state() != QProcess::NotRunning) {
            prepare_->kill();
            prepare_->waitForFinished(3000);
        }
    }
    if (installer_started_ && work_) {
        QFile marker(QDir(work_->path()).filePath(QStringLiteral("cancel-install")));
        if (marker.open(QIODevice::WriteOnly)) {
            marker.write("cancelled\n");
            marker.close();
        } else {
            qWarning().noquote() << "updates.cancellation_marker_failed:" << marker.errorString();
        }
        // The helper is still waiting for this live process. Termination is
        // safe here and prevents a later unrelated exit from applying a
        // cancelled update even if writing the cancellation marker failed.
        if (installer_handle_) {
            if (WaitForSingleObject(installer_handle_, 0) == WAIT_TIMEOUT) {
                if (!TerminateProcess(installer_handle_, 1)) {
                    qWarning() << "updates.installer_stop_failed:" << GetLastError();
                } else {
                    WaitForSingleObject(installer_handle_, 3000);
                }
            }
        }
    }
    finish_operation();
}

void Controller::finish_operation() {
    operation_active_ = false;
    ready_timer_->stop();
    if (installer_handle_) {
        CloseHandle(installer_handle_);
        installer_handle_ = nullptr;
    }
    if (prepare_) {
        prepare_->deleteLater();
        prepare_.clear();
    }
    if (progress_) {
        progress_->disconnect(this);
        progress_->accept();
        progress_->deleteLater();
        progress_.clear();
    }
    work_.reset();
}

void Controller::fail_operation(const QString& detail) {
    cancel_operation();
    if (!shutting_down_) QMessageBox::warning(window_, translated("Обновления"),
        translated("Не удалось установить обновление. Текущая версия продолжает работать.\n%1").arg(detail));
}

}  // namespace wardogs::updates
