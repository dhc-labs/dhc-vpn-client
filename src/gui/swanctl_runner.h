// swanctl_runner: thin Qt wrapper that runs `swanctl.exe` as a child
// process with the right env vars, captures stdout/stderr, and reports
// the result via a `finished` signal.
//
// MVP-grade: we shell out to swanctl rather than linking libvici. That
// avoids dragging the strongSwan transitive-dep mountain into our Qt
// link, at the cost of ~50-100 ms per invocation (process spawn). Fine
// for control-plane calls (load-all, initiate, terminate, list-sas
// every 2 s). If we later need real-time event subscription
// (kernel SA push notifications), we revisit and switch to libvici.

#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

class SwanctlRunner : public QObject
{
    Q_OBJECT

public:
    explicit SwanctlRunner(QObject *parent = nullptr);

    // High-level convenience methods. Each one starts a child process
    // and returns immediately; result arrives via the `finished` signal.
    // Only one operation can be in flight at a time -- if you call a
    // second method while one is running, the new one is silently
    // dropped (returns false).
    bool listConns();
    bool listSas();
    bool listCerts();
    bool loadAll();
    bool initiate(const QString &child);
    bool terminate(const QString &ike);

    // For diagnostics: where does this expect to find swanctl.
    QString swanctlPath() const { return m_swanctl; }
    // Same directory we set in SWANCTL_DIR for the child process: parent
    // of x509/, x509ca/, private/, conf.d/, secrets.d/.
    QString swanctlDir()  const { return m_swanctlDir; }
    bool isRunning() const;

signals:
    // Emitted when the child process exits. `op` is the verb that was
    // requested (e.g. "list-conns", "initiate"). `stdoutText` and
    // `stderrText` are utf-8 captures, `exitCode` is the swanctl exit
    // status (0 = ok). `crashed` is true if the process didn't exit
    // normally (e.g. couldn't be spawned at all).
    void finished(const QString &op,
                  const QString &stdoutText,
                  const QString &stderrText,
                  int exitCode,
                  bool crashed);

private slots:
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);
    void onProcessErrorOccurred(QProcess::ProcessError error);

private:
    bool run(const QString &op, const QStringList &args);

    QProcess m_proc;
    QString  m_swanctl;     // absolute path to swanctl.exe
    QString  m_swanctlDir;  // SWANCTL_DIR (parent of x509/, conf.d/, ...)
    QString  m_currentOp;   // verb being executed, empty if idle
};
