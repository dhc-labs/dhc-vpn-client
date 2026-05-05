#include "swanctl_runner.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>

namespace {

// Find swanctl.exe relative to the running .exe. Installed layout has it
// next to the GUI (or in ../sbin); dev workflow has it under the build
// tree.
QString locateSwanctl()
{
    const QString appDir = QCoreApplication::applicationDirPath();

    const QStringList candidates = {
        appDir + "/swanctl.exe",
        appDir + "/../sbin/swanctl.exe",
        appDir + "/../../charon-install/sbin/swanctl.exe",
        appDir + "/../../../build/charon-install/sbin/swanctl.exe",
    };
    for (const QString &c : candidates) {
        QFileInfo fi(QDir::cleanPath(c));
        if (fi.exists() && fi.isExecutable()) {
            return fi.absoluteFilePath();
        }
    }
    return QString();
}

QString locateCharonEtc(const QString &swanctlPath)
{
    // ../etc relative to swanctl.exe (which lives in sbin/).
    const QString sbinDir = QFileInfo(swanctlPath).absolutePath();
    return QDir::cleanPath(sbinDir + "/../etc");
}

} // namespace

SwanctlRunner::SwanctlRunner(QObject *parent)
    : QObject(parent)
    , m_swanctl(locateSwanctl())
{
    m_proc.setProcessChannelMode(QProcess::SeparateChannels);

    if (!m_swanctl.isEmpty()) {
        const QString etcDir = locateCharonEtc(m_swanctl);
        m_swanctlDir = etcDir + "/swanctl";
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("STRONGSWAN_CONF", etcDir + "/strongswan.conf");
        env.insert("SWANCTL_DIR",     m_swanctlDir);
        m_proc.setProcessEnvironment(env);
    }

    connect(&m_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &SwanctlRunner::onProcessFinished);
    connect(&m_proc, &QProcess::errorOccurred,
            this, &SwanctlRunner::onProcessErrorOccurred);
}

bool SwanctlRunner::isRunning() const
{
    return m_proc.state() != QProcess::NotRunning;
}

bool SwanctlRunner::listConns()  { return run("list-conns", {"--list-conns"}); }
bool SwanctlRunner::listSas()    { return run("list-sas",   {"--list-sas"}); }
bool SwanctlRunner::listCerts()  { return run("list-certs", {"--list-certs"}); }
bool SwanctlRunner::loadAll()    { return run("load-all",   {"--load-all"}); }

bool SwanctlRunner::initiate(const QString &child)
{
    return run("initiate", {"--initiate", "--child", child});
}

bool SwanctlRunner::terminate(const QString &ike)
{
    return run("terminate", {"--terminate", "--ike", ike});
}

bool SwanctlRunner::run(const QString &op, const QStringList &args)
{
    if (m_swanctl.isEmpty()) {
        // Synthesize a "crashed" finished signal so the UI can show the
        // error -- otherwise the user gets a button that does nothing.
        emit finished(op, QString(),
                      "swanctl.exe not found (looked next to the GUI and in standard build-tree paths)",
                      -1, true);
        return false;
    }
    if (isRunning()) {
        return false;
    }
    m_currentOp = op;
    m_proc.start(m_swanctl, args);
    return true;
}

void SwanctlRunner::onProcessFinished(int exitCode, QProcess::ExitStatus status)
{
    const QString op = m_currentOp;
    m_currentOp.clear();

    const QString out = QString::fromUtf8(m_proc.readAllStandardOutput());
    const QString err = QString::fromUtf8(m_proc.readAllStandardError());
    emit finished(op, out, err, exitCode, status != QProcess::NormalExit);
}

void SwanctlRunner::onProcessErrorOccurred(QProcess::ProcessError error)
{
    // FailedToStart fires before finished() and means the child process
    // could not be launched at all -- file missing, permission denied,
    // etc. Other errors (Crashed, Timedout) will also produce a
    // finished() event, so we only synthesize one here.
    if (error != QProcess::FailedToStart) {
        return;
    }
    const QString op = m_currentOp;
    m_currentOp.clear();
    emit finished(op, QString(), m_proc.errorString(), -1, true);
}
