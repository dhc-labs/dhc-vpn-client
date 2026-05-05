#pragma once

#include <QMainWindow>
#include <QString>
#include <QSystemTrayIcon>

class QAction;
class QCloseEvent;
class QComboBox;
class QLabel;
class QMenu;
class QPlainTextEdit;
class QPushButton;
class QTimer;
class SwanctlRunner;
struct Profile;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onConnectClicked();
    void onDisconnectClicked();
    void onRefreshClicked();
    void onNewProfileClicked();
    void onEditProfileClicked();
    void onDeleteProfileClicked();
    void onPollTick();
    void onSwanctlFinished(const QString &op,
                           const QString &out,
                           const QString &err,
                           int exitCode,
                           bool crashed);
    void onTrayActivated(QSystemTrayIcon::ActivationReason reason);
    void quitFromTray();

private:
    enum class Status { Disconnected, Connecting, Connected, Error };

    void setStatus(Status s, const QString &detail = QString());
    void setBusy(bool busy);
    void populateConnsFromOutput(const QString &out);
    void buildTray();
    void updateTrayIcon();

    // Path helpers -- these resolve the swanctl directory the daemon
    // actually reads (parent of x509/, private/, conf.d/, secrets.d/).
    QString swanctlDir() const;
    QString secretBlobPath(const QString &profileName) const;

    // Copy a user-picked file into the appropriate swanctl subdir
    // (x509/ for client+peer certs, x509ca/ for CA, private/ for keys).
    // Returns the basename to write into the .conf, or empty on failure.
    QString stageCertFile(const QString &srcPath, const QString &subdir);

    // Save profile + (optionally) DPAPI password. Wraps the dialog's
    // accept callback path so onNew/onEdit can share it.
    bool saveProfile(const Profile &p,
                     bool savePassword,
                     const QString &plaintextPassword,
                     QString *err);

    // For EAP: write a temp secrets file into conf.d/, run --load-all,
    // delete the temp file. Plaintext password lives on disk for the
    // duration of one swanctl invocation (~50-100 ms typically).
    bool writeEapSecretsTemp(const Profile &p, const QString &plaintextPassword);
    void clearEapSecretsTemp();

    // Active connection name. MVP assumes child name == connection name,
    // which matches every config we have so far. We expose both as the
    // same string so the user only sees one dropdown.
    QString currentConnName() const;

    QLabel         *m_statusLabel  = nullptr;
    QLabel         *m_pathLabel    = nullptr;
    QComboBox      *m_connCombo    = nullptr;
    QPushButton    *m_connectBtn   = nullptr;
    QPushButton    *m_disconnectBtn= nullptr;
    QPushButton    *m_refreshBtn   = nullptr;
    QPushButton    *m_newBtn       = nullptr;
    QPushButton    *m_editBtn      = nullptr;
    QPushButton    *m_deleteBtn    = nullptr;
    QPlainTextEdit *m_output       = nullptr;
    QTimer         *m_pollTimer    = nullptr;

    // Set when an EAP-mode connect is in flight, so the next load-all
    // result can chain to initiate. Cleared on completion / error.
    QString         m_pendingEapConn;

    SwanctlRunner  *m_runner       = nullptr;

    QSystemTrayIcon *m_tray        = nullptr;
    QAction         *m_actShow     = nullptr;
    QAction         *m_actConnect  = nullptr;
    QAction         *m_actDisconnect = nullptr;
    QAction         *m_actQuit     = nullptr;

    Status m_status = Status::Disconnected;

    // Set true only when the user picks Quit from the tray menu, so
    // closeEvent knows to actually quit instead of hiding to tray.
    bool m_reallyQuit = false;

    // Suppress repeat dialogs when every swanctl call fails because
    // charon-svc is unreachable. Reset on the first successful call so
    // the user sees the hint again if the daemon dies later.
    bool m_daemonHintShown = false;

    // On the first daemon-down detection in a session, try to start
    // charon-svc via sc.exe. install.ps1 grants AU SERVICE_START so this
    // works without UAC. Set to true after one attempt to avoid loops;
    // reset on successful op so a later restart cycle works again.
    bool m_serviceStartAttempted = false;

    // After a daemon restart charon has zero conns loaded, so list-conns
    // returns empty even though our conf.d/ has profiles. Detect that
    // case and auto-fire one --load-all to repopulate. The flag prevents
    // an infinite loop if --load-all itself yields zero conns.
    bool m_autoLoadAttempted = false;
};
