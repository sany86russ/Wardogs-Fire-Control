#pragma once

#include "update_service.hpp"

#include <QObject>
#include <QElapsedTimer>
#include <QPointer>
#include <QStringList>

#include <memory>
#include <optional>
#include <utility>

class QFrame;
class QNetworkAccessManager;
class QLabel;
class QProcess;
class QProgressDialog;
class QPushButton;
class QTemporaryDir;
class QTimer;
class QWidget;

namespace wardogs::updates {

// Created only for an ordinary application window. Diagnostic previews never
// construct this controller or its network service.
class Controller final : public QObject {
public:
    Controller(QWidget* window, QString current_version, QString install_directory,
               QNetworkAccessManager* network = nullptr);
    ~Controller() override;

    QFrame* banner_widget() const { return banner_; }
    void check(bool manual);
    void set_before_install(std::function<bool()> callback) { before_install_ = std::move(callback); }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void refresh_text();
    void begin_update();
    void prepare_update();
    void launch_installer();
    void poll_installer();
    void cancel_operation();
    void finish_operation();
    void fail_operation(const QString& detail);
    void open_release();
    QString powershell_path() const;
    QStringList helper_arguments(const QString& mode) const;

    QPointer<QWidget> window_;
    QString current_version_;
    QString install_directory_;
    Service* service_{};
    QFrame* banner_{};
    QLabel* banner_text_{};
    QPushButton *update_button_{}, *notes_button_{}, *later_button_{};
    std::optional<ReleaseInfo> pending_;
    std::function<bool()> before_install_;
    std::unique_ptr<QTemporaryDir> work_;
    QPointer<QProgressDialog> progress_;
    QPointer<QProcess> prepare_;
    QTimer* ready_timer_{};
    QString phase_source_;
    QElapsedTimer ready_wait_;
    void* installer_handle_{};
    qint64 received_{}, total_{};
    bool operation_active_{};
    bool cancelled_{};
    bool installer_started_{};
    bool shutting_down_{};
};

}  // namespace wardogs::updates
