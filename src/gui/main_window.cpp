#include "main_window.h"
#include "dpapi_secret.h"
#include "profile.h"
#include "profile_dialog.h"
#include "swanctl_runner.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStyle>
#include <QSvgRenderer>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

namespace {
constexpr int kPollIntervalMs = 2000;
}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , m_runner(new SwanctlRunner(this))
{
    setWindowTitle(tr("dhc-vpn (alpha)"));
    resize(720, 520);

    auto *central = new QWidget(this);
    auto *layout  = new QVBoxLayout(central);

    // --- Header / status row -------------------------------------------
    auto *headerRow = new QHBoxLayout();
    auto *logoLabel = new QLabel(central);
    {
        // Pre-render the SVG to a crisp small pixmap once. Using
        // QSvgRenderer + QPainter avoids relying on the QSvgIconEngine
        // plugin and gives us pixel-exact control over the size.
        constexpr int logoH = 32;
        QSvgRenderer svg(QStringLiteral(":/dhc-logo.svg"));
        if (svg.isValid()) {
            const QSize defSize = svg.defaultSize();
            const int logoW = defSize.height() > 0
                ? logoH * defSize.width() / defSize.height()
                : logoH * 3;
            const qreal dpr = devicePixelRatioF();
            QPixmap px(QSize(logoW, logoH) * dpr);
            px.setDevicePixelRatio(dpr);
            px.fill(Qt::transparent);
            QPainter p(&px);
            p.setRenderHint(QPainter::Antialiasing);
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            svg.render(&p);
            logoLabel->setPixmap(px);
        }
        logoLabel->setFixedHeight(logoH);
    }
    headerRow->addWidget(logoLabel);

    m_statusLabel = new QLabel(central);
    QFont f = m_statusLabel->font();
    f.setPointSize(f.pointSize() + 2);
    f.setBold(true);
    m_statusLabel->setFont(f);
    headerRow->addWidget(m_statusLabel);
    headerRow->addStretch();
    layout->addLayout(headerRow);

    m_pathLabel = new QLabel(tr("swanctl: %1").arg(
        m_runner->swanctlPath().isEmpty()
            ? QStringLiteral("<not found>")
            : m_runner->swanctlPath()), central);
    m_pathLabel->setStyleSheet("color: gray; font-size: 9pt;");
    layout->addWidget(m_pathLabel);

    // --- Connection picker ---------------------------------------------
    auto *connRow = new QHBoxLayout();
    connRow->addWidget(new QLabel(tr("Connection:"), central));
    m_connCombo = new QComboBox(central);
    m_connCombo->setMinimumWidth(220);
    m_connCombo->addItem(tr("(loading...)"));
    m_connCombo->setEnabled(false);
    connRow->addWidget(m_connCombo);
    connRow->addStretch();
    layout->addLayout(connRow);

    // --- Action buttons -------------------------------------------------
    auto *btnRow = new QHBoxLayout();
    m_connectBtn    = new QPushButton(tr("Connect"),    central);
    m_disconnectBtn = new QPushButton(tr("Disconnect"), central);
    m_refreshBtn    = new QPushButton(tr("Refresh"),    central);
    m_disconnectBtn->setEnabled(false);
    m_connectBtn->setEnabled(false); // until we have a conn loaded
    btnRow->addWidget(m_connectBtn);
    btnRow->addWidget(m_disconnectBtn);
    btnRow->addStretch();
    btnRow->addWidget(m_refreshBtn);
    layout->addLayout(btnRow);

    // --- Profile management row ----------------------------------------
    auto *profRow = new QHBoxLayout();
    m_newBtn    = new QPushButton(tr("New Profile..."),  central);
    m_editBtn   = new QPushButton(tr("Edit..."),         central);
    m_deleteBtn = new QPushButton(tr("Delete"),          central);
    profRow->addWidget(m_newBtn);
    profRow->addWidget(m_editBtn);
    profRow->addWidget(m_deleteBtn);
    profRow->addStretch();
    layout->addLayout(profRow);

    // --- Output panel ---------------------------------------------------
    m_output = new QPlainTextEdit(central);
    m_output->setReadOnly(true);
    m_output->setStyleSheet("font-family: 'Consolas', 'Courier New', monospace;");
    m_output->setPlaceholderText(tr("swanctl output will appear here"));
    layout->addWidget(m_output, 1);

    setCentralWidget(central);

    // --- Polling timer (only ticks while connected) ---------------------
    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(kPollIntervalMs);
    connect(m_pollTimer, &QTimer::timeout, this, &MainWindow::onPollTick);

    // --- Wiring ---------------------------------------------------------
    connect(m_connectBtn,    &QPushButton::clicked, this, &MainWindow::onConnectClicked);
    connect(m_disconnectBtn, &QPushButton::clicked, this, &MainWindow::onDisconnectClicked);
    connect(m_refreshBtn,    &QPushButton::clicked, this, &MainWindow::onRefreshClicked);
    connect(m_newBtn,        &QPushButton::clicked, this, &MainWindow::onNewProfileClicked);
    connect(m_editBtn,       &QPushButton::clicked, this, &MainWindow::onEditProfileClicked);
    connect(m_deleteBtn,     &QPushButton::clicked, this, &MainWindow::onDeleteProfileClicked);
    connect(m_runner, &SwanctlRunner::finished, this, &MainWindow::onSwanctlFinished);

    setStatus(Status::Disconnected);
    buildTray();

    // Pull initial state. load-all populates charon's view, then
    // list-conns lets us fill the dropdown, then list-sas tells us
    // whether anything is already up.
    m_output->appendPlainText(tr("=== load-all (initial) ==="));
    m_runner->loadAll();
}

void MainWindow::buildTray()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        return;
    }
    m_tray = new QSystemTrayIcon(this);
    m_tray->setToolTip(tr("dhc-vpn (disconnected)"));

    auto *menu = new QMenu(this);
    m_actShow       = menu->addAction(tr("Show"));
    menu->addSeparator();
    m_actConnect    = menu->addAction(tr("Connect"));
    m_actDisconnect = menu->addAction(tr("Disconnect"));
    menu->addSeparator();
    m_actQuit       = menu->addAction(tr("Quit"));

    connect(m_actShow,       &QAction::triggered, this, [this]() {
        showNormal(); raise(); activateWindow();
    });
    connect(m_actConnect,    &QAction::triggered, this, &MainWindow::onConnectClicked);
    connect(m_actDisconnect, &QAction::triggered, this, &MainWindow::onDisconnectClicked);
    connect(m_actQuit,       &QAction::triggered, this, &MainWindow::quitFromTray);

    connect(m_tray, &QSystemTrayIcon::activated,
            this, &MainWindow::onTrayActivated);

    m_tray->setContextMenu(menu);
    updateTrayIcon();
    m_tray->show();
}

void MainWindow::updateTrayIcon()
{
    if (!m_tray) {
        return;
    }
    QStyle::StandardPixmap sp = QStyle::SP_ComputerIcon;
    QString tip = tr("dhc-vpn");
    switch (m_status) {
    case Status::Disconnected:
        sp  = QStyle::SP_BrowserStop;            // stop sign / red-ish
        tip = tr("dhc-vpn (disconnected)");
        break;
    case Status::Connecting:
        sp  = QStyle::SP_BrowserReload;          // arrows / orange-ish
        tip = tr("dhc-vpn (connecting...)");
        break;
    case Status::Connected:
        sp  = QStyle::SP_DialogApplyButton;      // green check
        tip = tr("dhc-vpn (connected: %1)").arg(currentConnName());
        break;
    case Status::Error:
        sp  = QStyle::SP_MessageBoxCritical;     // red X
        tip = tr("dhc-vpn (error)");
        break;
    }
    m_tray->setIcon(style()->standardIcon(sp));
    m_tray->setToolTip(tip);

    // The window itself wears the same icon so the taskbar matches.
    setWindowIcon(style()->standardIcon(sp));

    if (m_actConnect && m_actDisconnect) {
        m_actConnect->setEnabled(m_status == Status::Disconnected
                                 && !currentConnName().isEmpty());
        m_actDisconnect->setEnabled(m_status == Status::Connected);
    }
}

void MainWindow::setStatus(Status s, const QString &detail)
{
    m_status = s;
    QString text;
    QString color;
    switch (s) {
    case Status::Disconnected: text = tr("disconnected");                  break;
    case Status::Connecting:   text = tr("connecting...");  color = "orange"; break;
    case Status::Connected:    text = tr("connected");      color = "green";  break;
    case Status::Error:        text = tr("error");          color = "red";    break;
    }
    if (!detail.isEmpty()) {
        text += " (" + detail + ")";
    }
    m_statusLabel->setText(tr("Status: %1").arg(text));
    m_statusLabel->setStyleSheet(color.isEmpty()
                                 ? QString()
                                 : QString("color: %1;").arg(color));
    updateTrayIcon();
}

void MainWindow::setBusy(bool busy)
{
    m_refreshBtn->setEnabled(!busy);
    m_connCombo->setEnabled(!busy && m_connCombo->count() > 0
                            && m_connCombo->itemText(0) != tr("(loading...)"));
    if (busy) {
        m_connectBtn->setEnabled(false);
        m_disconnectBtn->setEnabled(false);
    }
    // When un-busy, the conn/disc state is reset by callers based on
    // the actual SA state. We don't enable both blindly here.
}

QString MainWindow::currentConnName() const
{
    if (!m_connCombo || m_connCombo->currentIndex() < 0) {
        return QString();
    }
    const QString s = m_connCombo->currentText();
    if (s == tr("(loading...)") || s == tr("(none)")) {
        return QString();
    }
    return s;
}

void MainWindow::populateConnsFromOutput(const QString &out)
{
    // Match top-level connection names: lines like "drhc-cert: IKEv2, ..."
    // (no leading whitespace, name followed by colon + IKE version).
    static const QRegularExpression rx(
        QStringLiteral(R"(^([A-Za-z0-9_.\-]+):\s+IKEv\d)"),
        QRegularExpression::MultilineOption);

    QStringList names;
    auto it = rx.globalMatch(out);
    while (it.hasNext()) {
        names << it.next().captured(1);
    }

    const QString prev = m_connCombo->currentText();
    m_connCombo->clear();
    if (names.isEmpty()) {
        m_connCombo->addItem(tr("(none)"));
        m_connCombo->setEnabled(false);
        m_connectBtn->setEnabled(false);
    } else {
        m_connCombo->addItems(names);
        m_connCombo->setEnabled(true);
        // Restore previous selection if still available.
        const int idx = names.indexOf(prev);
        if (idx >= 0) {
            m_connCombo->setCurrentIndex(idx);
        }
        if (m_status == Status::Disconnected) {
            m_connectBtn->setEnabled(true);
        }
    }
    updateTrayIcon();
}

void MainWindow::onConnectClicked()
{
    const QString conn = currentConnName();
    if (conn.isEmpty()) {
        return;
    }

    // EAP profiles need a plaintext password materialized into a temp
    // secrets file BEFORE load-all, so charon ingests it during the
    // load. We figure that out by reading the saved profile (if any).
    bool ok = false;
    Profile p = Profile::load(swanctlDir(), conn, &ok);
    if (ok && p.authMode == Profile::AuthMode::EapMschapv2) {
        QString pw;
        const QString blob = secretBlobPath(conn);
        if (DpapiSecret::fileExists(blob)) {
            QString err;
            if (!DpapiSecret::loadFromFile(blob, &pw, &err)) {
                QMessageBox::warning(this, tr("Saved password unreadable"),
                    tr("Could not decrypt saved password: %1\n\n"
                       "You will be prompted instead.").arg(err));
                pw.clear();
            }
        }
        if (pw.isEmpty()) {
            bool gotPw = false;
            pw = QInputDialog::getText(this, tr("VPN password"),
                tr("Password for %1:").arg(p.eapUsername),
                QLineEdit::Password, QString(), &gotPw);
            if (!gotPw || pw.isEmpty()) {
                m_output->appendPlainText(tr("(connect cancelled)"));
                return;
            }
        }
        if (!writeEapSecretsTemp(p, pw)) {
            QMessageBox::warning(this, tr("Could not stage EAP secret"),
                tr("Failed to write the temporary secrets file. "
                   "Check permissions on the swanctl directory."));
            return;
        }
        m_pendingEapConn = conn;
    }

    setStatus(Status::Connecting);
    setBusy(true);
    m_output->appendPlainText(tr("\n=== load-all ==="));
    m_runner->loadAll();
    // initiate is fired in onSwanctlFinished after load-all returns.
}

void MainWindow::onDisconnectClicked()
{
    if (currentConnName().isEmpty()) {
        return;
    }
    setStatus(Status::Connecting, tr("disconnecting"));
    setBusy(true);
    m_pollTimer->stop();
    m_output->appendPlainText(tr("\n=== terminate ==="));
    m_runner->terminate(currentConnName());
}

void MainWindow::onRefreshClicked()
{
    if (m_runner->isRunning()) {
        return;
    }
    m_output->appendPlainText(tr("\n=== list-conns ==="));
    m_runner->listConns();
}

void MainWindow::onPollTick()
{
    if (m_runner->isRunning()) {
        return;
    }
    m_runner->listSas();
}

void MainWindow::onSwanctlFinished(const QString &op,
                                   const QString &out,
                                   const QString &err,
                                   int exitCode,
                                   bool crashed)
{
    if (!out.isEmpty()) {
        m_output->appendPlainText(out.trimmed());
    }
    if (!err.isEmpty()) {
        m_output->appendPlainText(QStringLiteral("[stderr] ") + err.trimmed());
    }

    if (crashed || exitCode != 0) {
        // load-all failure leaves the temp secrets file dangling on disk.
        // Wipe it now -- security beats keeping the password around for
        // a manual retry. The user will be re-prompted on next Connect.
        if (op == "load-all" && !m_pendingEapConn.isEmpty()) {
            clearEapSecretsTemp();
            m_pendingEapConn.clear();
        }
        // swanctl prints "connecting to 'tcp://127.0.0.1:4502' failed:
        // Connection refused" when charon-svc isn't listening. Catch it
        // and tell the user explicitly -- the default exit-code message
        // is opaque.
        const bool daemonDown =
            err.contains("Connection refused") ||
            err.contains("connecting to 'default' URI failed");
        if (daemonDown) {
            // First daemon-down hit in this session: try to start the service
            // before nagging the user. install.ps1 grants AU SERVICE_START so
            // this works without UAC. Falls through to the manual hint if the
            // start-up itself fails (no service registered, dev-mode build,
            // ACL not loosened on a pre-M9b install).
            if (!m_serviceStartAttempted) {
                m_serviceStartAttempted = true;
                m_output->appendPlainText(tr("\n=== starting charon-svc ==="));
                QProcess sc;
                sc.start("sc.exe", QStringList{"start", "charon-svc"});
                sc.waitForFinished(3000);
                const QString scOut = QString::fromLocal8Bit(sc.readAllStandardOutput()).trimmed();
                const QString scErr = QString::fromLocal8Bit(sc.readAllStandardError()).trimmed();
                if (!scOut.isEmpty()) m_output->appendPlainText(scOut);
                if (!scErr.isEmpty()) m_output->appendPlainText(QStringLiteral("[stderr] ") + scErr);
                const int rc = sc.exitCode();
                // sc.exe: 0 = started, 1056 = already running. Both fine.
                if (rc == 0 || rc == 1056) {
                    QTimer::singleShot(2000, this, [this, op]() {
                        if (m_runner->isRunning()) return;
                        if (op == "load-all" || op == "initiate") m_runner->loadAll();
                        else if (op == "list-conns")              m_runner->listConns();
                        else if (op == "list-sas")                m_runner->listSas();
                        // terminate retry: skipped; user clicked disconnect.
                    });
                    // Stay in Connecting state; do not bail to error.
                    return;
                }
                m_output->appendPlainText(
                    tr("(sc.exe start failed: rc=%1 -- check that install.ps1 ran)").arg(rc));
            }
            setStatus(Status::Error, tr("charon-svc not running"));
            if (!m_daemonHintShown) {
                m_daemonHintShown = true;
                // Derive repo root from the GUI binary: dev layout is
                // <repo>/build/src/gui/dhc-vpn.exe -> ../../.. == <repo>.
                // Fall back to the literal "<repo>" placeholder when the
                // binary lives somewhere else (installed build).
                const QString appDir = QCoreApplication::applicationDirPath();
                QDir up(appDir);
                bool haveScript = false;
                for (int i = 0; i < 3 && up.cdUp(); ++i) {}
                if (QFile::exists(up.filePath("scripts/run-charon.ps1"))) {
                    haveScript = true;
                }
                const QString repoHint = haveScript
                    ? QDir::toNativeSeparators(up.absolutePath())
                    : QStringLiteral("<repo>");
                QMessageBox::warning(this, tr("VPN service not running"),
                    tr("Could not reach the strongSwan daemon (charon-svc) "
                       "on tcp://127.0.0.1:4502.\n\n"
                       "Start it from an elevated PowerShell:\n"
                       "    cd %1\n"
                       "    .\\scripts\\run-charon.ps1\n\n"
                       "Then click Refresh in this window.\n\n"
                       "(A proper Windows Service install is on the "
                       "roadmap -- M9.)")
                        .arg(repoHint));
            }
        } else {
            setStatus(Status::Error, tr("op=%1 exit=%2").arg(op).arg(exitCode));
        }
        setBusy(false);
        m_pollTimer->stop();
        // After an error, still try to populate the conn list -- maybe
        // it was just initiate that failed and we still want to know
        // what's available. Skip the retry when the daemon is down --
        // it would just fail the same way and re-trigger the dialog.
        if (op != "list-conns" && !daemonDown) {
            QTimer::singleShot(300, this, [this]() {
                if (!m_runner->isRunning()) m_runner->listConns();
            });
        }
        return;
    }

    // Any successful call means the daemon is alive again -- arm the
    // hint and the auto-start retry for the next outage.
    m_daemonHintShown = false;
    m_serviceStartAttempted = false;

    if (op == "load-all") {
        // Secrets are now in charon's memory; clear the on-disk copy.
        if (!m_pendingEapConn.isEmpty()) {
            clearEapSecretsTemp();
            m_pendingEapConn.clear();
        }
        // After load-all, query the conn list to populate the dropdown.
        // If a Connect click triggered this, chain into initiate AFTER
        // we know the conn name is valid.
        if (m_status == Status::Connecting) {
            m_output->appendPlainText(tr("\n=== initiate %1 ===")
                                          .arg(currentConnName()));
            m_runner->initiate(currentConnName());
        } else {
            m_runner->listConns();
        }
        return;
    }

    if (op == "list-conns") {
        populateConnsFromOutput(out);
        // After a daemon restart charon has zero conns loaded, but our
        // conf.d/ profiles are still on disk. Auto-fire one --load-all
        // to repopulate. The flag prevents an infinite loop if --load-all
        // itself yields zero (e.g. genuinely empty conf.d/).
        const bool empty = (m_connCombo->count() == 0)
                        || (m_connCombo->count() == 1
                            && m_connCombo->itemText(0) == tr("(none)"));
        if (empty && !m_autoLoadAttempted) {
            m_autoLoadAttempted = true;
            m_output->appendPlainText(tr("\n=== load-all (auto, dropdown empty) ==="));
            m_runner->loadAll();
            return;
        }
        if (!empty) {
            m_autoLoadAttempted = false;
        }
        // Follow up with a list-sas so we know if anything's already up.
        if (!m_runner->isRunning()) {
            m_runner->listSas();
        }
        return;
    }

    if (op == "initiate") {
        setStatus(Status::Connected);
        setBusy(false);
        m_disconnectBtn->setEnabled(true);
        m_connectBtn->setEnabled(false);
        m_pollTimer->start();
        QTimer::singleShot(200, this, [this]() {
            if (!m_runner->isRunning()) m_runner->listSas();
        });
        return;
    }

    if (op == "terminate") {
        setStatus(Status::Disconnected);
        setBusy(false);
        m_disconnectBtn->setEnabled(false);
        m_connectBtn->setEnabled(!currentConnName().isEmpty());
        return;
    }

    if (op == "list-sas") {
        const bool hasSa = out.contains("ESTABLISHED");
        if (hasSa) {
            if (m_status != Status::Connected) {
                setStatus(Status::Connected);
                m_pollTimer->start();
            }
            m_disconnectBtn->setEnabled(true);
            m_connectBtn->setEnabled(false);
        } else {
            if (m_status == Status::Connected) {
                // SA disappeared underneath us (e.g. terminated from
                // another swanctl elsewhere).
                setStatus(Status::Disconnected);
                m_pollTimer->stop();
            }
            m_disconnectBtn->setEnabled(false);
            m_connectBtn->setEnabled(!currentConnName().isEmpty());
        }
        return;
    }
}

void MainWindow::onTrayActivated(QSystemTrayIcon::ActivationReason reason)
{
    if (reason == QSystemTrayIcon::DoubleClick ||
        reason == QSystemTrayIcon::Trigger) {
        if (isHidden() || !isActiveWindow()) {
            showNormal();
            raise();
            activateWindow();
        } else {
            hide();
        }
    }
}

void MainWindow::quitFromTray()
{
    m_reallyQuit = true;
    if (m_tray) {
        m_tray->hide();
    }
    qApp->quit();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_reallyQuit || !m_tray || !m_tray->isVisible()) {
        event->accept();
        return;
    }
    // First close: hide to tray. Show a short balloon to teach the
    // user where the app went.
    static bool firstHide = true;
    hide();
    if (firstHide && m_tray) {
        firstHide = false;
        m_tray->showMessage(tr("dhc-vpn"),
                            tr("Still running in the system tray. "
                               "Right-click the icon to quit."),
                            QSystemTrayIcon::Information,
                            4000);
    }
    event->ignore();
}

// ---------------------------------------------------------------------------
// Profile management
// ---------------------------------------------------------------------------

QString MainWindow::swanctlDir() const
{
    return m_runner ? m_runner->swanctlDir() : QString();
}

QString MainWindow::secretBlobPath(const QString &profileName) const
{
    return QDir::cleanPath(swanctlDir() + "/secrets.d/" + profileName + ".dat");
}

QString MainWindow::stageCertFile(const QString &srcPath, const QString &subdir)
{
    if (srcPath.isEmpty()) {
        return QString();
    }
    QFileInfo fi(srcPath);
    // If the line edit already holds a bare basename (the user is editing
    // an existing profile and didn't re-pick), there's nothing to copy.
    // We treat any non-existing path as "already in place" rather than
    // erroring out.
    if (!fi.isAbsolute() || !fi.exists()) {
        return fi.fileName();
    }
    const QString destDir = QDir::cleanPath(swanctlDir() + "/" + subdir);
    QDir().mkpath(destDir);
    const QString dest = destDir + "/" + fi.fileName();
    if (QFileInfo(dest) == fi) {
        return fi.fileName();      // already where it needs to be
    }
    if (QFile::exists(dest)) {
        QFile::remove(dest);
    }
    if (!QFile::copy(srcPath, dest)) {
        m_output->appendPlainText(tr("[warn] failed to copy %1 -> %2")
                                      .arg(srcPath, dest));
        return QString();
    }
    return fi.fileName();
}

bool MainWindow::saveProfile(const Profile &p,
                             bool savePw,
                             const QString &plaintextPw,
                             QString *err)
{
    if (!p.save(swanctlDir(), err)) {
        return false;
    }
    if (p.authMode == Profile::AuthMode::EapMschapv2 && savePw) {
        const QString blob = secretBlobPath(p.name);
        QString dpapiErr;
        if (!DpapiSecret::storeToFile(blob, plaintextPw, &dpapiErr)) {
            if (err) *err = QString("profile saved, but storing password failed: %1")
                                .arg(dpapiErr);
            return false;
        }
    } else if (p.authMode == Profile::AuthMode::Certificate) {
        // No DPAPI for cert mode -- but if a stale blob exists from a
        // previous EAP incarnation of the same profile name, remove it.
        DpapiSecret::removeFile(secretBlobPath(p.name));
    } else if (!savePw) {
        // EAP without "remember": ensure no old blob lingers.
        DpapiSecret::removeFile(secretBlobPath(p.name));
    }
    return true;
}

bool MainWindow::writeEapSecretsTemp(const Profile &p,
                                     const QString &plaintextPw)
{
    // Filename starts with an underscore + "eap" so it sorts late and is
    // visually obvious as machine-managed. Lives inside conf.d/ which is
    // already in the include path.
    const QString path = QDir::cleanPath(
        swanctlDir() + "/conf.d/_eap-" + p.name + ".secret.conf");
    QDir().mkpath(QFileInfo(path).absolutePath());

    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }
    const QByteArray bytes = p.toSecretBlock(plaintextPw).toUtf8();
    bool ok = (f.write(bytes) == bytes.size()) && f.commit();
    return ok;
}

void MainWindow::clearEapSecretsTemp()
{
    if (m_pendingEapConn.isEmpty()) return;
    const QString path = QDir::cleanPath(
        swanctlDir() + "/conf.d/_eap-" + m_pendingEapConn + ".secret.conf");
    if (QFile::exists(path)) {
        // Overwrite then delete -- the password is short, but better
        // not to leave even a freshly-deleted plaintext on disk.
        QFile f(path);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            const QByteArray pad(256, 0);
            f.write(pad);
            f.close();
        }
        QFile::remove(path);
    }
}

void MainWindow::onNewProfileClicked()
{
    ProfileDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }
    Profile p = dlg.profile();
    if (p.name.isEmpty() || p.remoteAddr.isEmpty()) {
        QMessageBox::warning(this, tr("Missing fields"),
            tr("Profile name and remote address are required."));
        return;
    }
    if (p.authMode == Profile::AuthMode::Certificate) {
        // Copy any user-picked absolute-path cert/key files into the
        // matching swanctl subdir, replacing the basename in the profile.
        p.clientCertFile = stageCertFile(dlg.rawClientCertPath(), "x509");
        p.clientKeyFile  = stageCertFile(dlg.rawClientKeyPath(),  "private");
        p.caCertFile     = stageCertFile(dlg.rawCaCertPath(),     "x509ca");
        p.remoteCertFile = stageCertFile(dlg.rawRemoteCertPath(), "x509");
    }

    QString err;
    if (!saveProfile(p, dlg.savePassword(), dlg.plaintextPassword(), &err)) {
        QMessageBox::warning(this, tr("Save failed"), err);
        return;
    }
    m_output->appendPlainText(tr("\n=== profile saved: %1 ===").arg(p.name));
    m_output->appendPlainText(tr("\n=== load-all (after save) ==="));
    m_runner->loadAll();
}

void MainWindow::onEditProfileClicked()
{
    const QString name = currentConnName();
    if (name.isEmpty()) return;

    bool ok = false;
    Profile p = Profile::load(swanctlDir(), name, &ok);
    if (!ok) {
        QMessageBox::information(this, tr("Cannot edit"),
            tr("This connection has no profile file in conf.d/. "
               "It probably came from the legacy swanctl.conf."));
        return;
    }

    ProfileDialog dlg(this);
    dlg.setProfile(p, /*editing=*/true);
    if (dlg.exec() != QDialog::Accepted) return;

    Profile np = dlg.profile();
    np.name = p.name;        // edit-mode name is locked

    if (np.authMode == Profile::AuthMode::Certificate) {
        np.clientCertFile = stageCertFile(dlg.rawClientCertPath(), "x509");
        np.clientKeyFile  = stageCertFile(dlg.rawClientKeyPath(),  "private");
        np.caCertFile     = stageCertFile(dlg.rawCaCertPath(),     "x509ca");
        np.remoteCertFile = stageCertFile(dlg.rawRemoteCertPath(), "x509");
    }

    QString err;
    if (!saveProfile(np, dlg.savePassword(), dlg.plaintextPassword(), &err)) {
        QMessageBox::warning(this, tr("Save failed"), err);
        return;
    }
    m_output->appendPlainText(tr("\n=== profile updated: %1 ===").arg(np.name));
    m_output->appendPlainText(tr("\n=== load-all (after edit) ==="));
    m_runner->loadAll();
}

void MainWindow::onDeleteProfileClicked()
{
    const QString name = currentConnName();
    if (name.isEmpty()) return;

    auto resp = QMessageBox::question(this, tr("Delete profile"),
        tr("Delete profile \"%1\"?\n\n"
           "This removes conf.d/%1.conf and the saved password (if any). "
           "Certificate files in x509/, x509ca/ and private/ are NOT "
           "deleted -- remove them manually if no longer needed.")
            .arg(name),
        QMessageBox::Yes | QMessageBox::No);
    if (resp != QMessageBox::Yes) return;

    if (!Profile::remove(swanctlDir(), name)) {
        QMessageBox::information(this, tr("Nothing to delete"),
            tr("No profile file found for \"%1\".").arg(name));
        return;
    }
    m_output->appendPlainText(tr("\n=== profile deleted: %1 ===").arg(name));
    m_output->appendPlainText(tr("\n=== load-all (after delete) ==="));
    m_runner->loadAll();
}
