#include "profile.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTextStream>

namespace {

QString confPath(const QString &swanctlDir, const QString &name)
{
    return QDir::cleanPath(swanctlDir + "/conf.d/" + name + ".conf");
}

QString secretBlobPath(const QString &swanctlDir, const QString &name)
{
    return QDir::cleanPath(swanctlDir + "/secrets.d/" + name + ".dat");
}

} // namespace

QString Profile::toConnectionBlock() const
{
    QString body;
    QTextStream s(&body);
    s << "connections {\n";
    s << "    " << name << " {\n";
    s << "        version = 2\n";
    s << "        local_addrs = %any\n";
    s << "        remote_addrs = " << remoteAddr << "\n";
    s << "        proposals = " << proposals << "\n";
    s << "        vips = 0.0.0.0\n";
    s << "\n";

    s << "        local {\n";
    if (authMode == AuthMode::Certificate) {
        s << "            auth = pubkey\n";
        s << "            id = " << (localId.isEmpty() ? "%fromcert" : localId) << "\n";
        if (!clientCertFile.isEmpty()) {
            s << "            certs = " << clientCertFile << "\n";
        }
    } else {
        s << "            auth = eap-mschapv2\n";
        // EAP requires an explicit identity -- charon sends this in the
        // first IDi payload. We pin it to the username so it matches what
        // strongSwan looks up in the secrets table.
        s << "            id = " << (localId.isEmpty() ? eapUsername : localId) << "\n";
        s << "            eap_id = " << eapUsername << "\n";
    }
    s << "        }\n";
    s << "\n";

    s << "        remote {\n";
    s << "            auth = pubkey\n";
    if (!remoteId.isEmpty()) {
        s << "            id = " << remoteId << "\n";
    }
    if (!remoteCertFile.isEmpty()) {
        s << "            certs = " << remoteCertFile << "\n";
    }
    if (!caCertFile.isEmpty()) {
        s << "            cacerts = " << caCertFile << "\n";
    }
    s << "        }\n";
    s << "\n";

    s << "        children {\n";
    s << "            " << name << " {\n";
    s << "                remote_ts = " << remoteTs << "\n";
    s << "                start_action = none\n";
    s << "                close_action = none\n";
    s << "                dpd_action = clear\n";
    s << "                esp_proposals = " << espProposals << "\n";
    s << "            }\n";
    s << "        }\n";
    s << "\n";

    s << "        send_cert = always\n";
    s << "        dpd_delay = 30s\n";
    s << "    }\n";
    s << "}\n";
    return body;
}

QString Profile::toSecretBlock(const QString &plaintextPassword) const
{
    if (authMode != AuthMode::EapMschapv2) {
        return QString();
    }
    QString body;
    QTextStream s(&body);
    s << "secrets {\n";
    s << "    eap-" << name << " {\n";
    s << "        id = " << eapUsername << "\n";
    s << "        secret = \"" << plaintextPassword << "\"\n";
    s << "    }\n";
    s << "}\n";
    return body;
}

bool Profile::save(const QString &swanctlDir, QString *err) const
{
    if (name.isEmpty()) {
        if (err) *err = "profile name is empty";
        return false;
    }
    static const QRegularExpression valid(R"(^[A-Za-z0-9_.\-]+$)");
    if (!valid.match(name).hasMatch()) {
        if (err) *err = "profile name must match [A-Za-z0-9_.-]+";
        return false;
    }
    const QString path = confPath(swanctlDir, name);
    QDir().mkpath(QFileInfo(path).absolutePath());

    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (err) *err = f.errorString();
        return false;
    }
    const QByteArray bytes = toConnectionBlock().toUtf8();
    if (f.write(bytes) != bytes.size()) {
        if (err) *err = "short write";
        return false;
    }
    if (!f.commit()) {
        if (err) *err = "commit failed";
        return false;
    }
    return true;
}

Profile Profile::load(const QString &swanctlDir, const QString &name, bool *ok)
{
    Profile p;
    p.name = name;

    QFile f(confPath(swanctlDir, name));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (ok) *ok = false;
        return p;
    }
    const QString src = QString::fromUtf8(f.readAll());

    auto findValue = [&](const QString &key) -> QString {
        QRegularExpression rx(QString(R"(\b%1\s*=\s*([^\n#]+))").arg(key));
        auto m = rx.match(src);
        if (!m.hasMatch()) return QString();
        return m.captured(1).trimmed();
    };

    p.remoteAddr    = findValue("remote_addrs");
    p.proposals     = findValue("proposals");
    p.espProposals  = findValue("esp_proposals");
    p.remoteTs      = findValue("remote_ts");

    // local{} parsing -- crude but enough for our own files. Pull the
    // local block, then look for auth + certs/eap_id inside it.
    QRegularExpression localRx(R"(local\s*\{([^}]*)\})",
                               QRegularExpression::DotMatchesEverythingOption);
    auto localMatch = localRx.match(src);
    if (localMatch.hasMatch()) {
        const QString local = localMatch.captured(1);
        if (local.contains("auth = eap-mschapv2")) {
            p.authMode = AuthMode::EapMschapv2;
            QRegularExpression eapRx(R"(eap_id\s*=\s*([^\n#]+))");
            auto m = eapRx.match(local);
            if (m.hasMatch()) p.eapUsername = m.captured(1).trimmed();
        } else {
            p.authMode = AuthMode::Certificate;
            QRegularExpression certsRx(R"(certs\s*=\s*([^\n#]+))");
            auto m = certsRx.match(local);
            if (m.hasMatch()) p.clientCertFile = m.captured(1).trimmed();
        }
        QRegularExpression idRx(R"(\bid\s*=\s*([^\n#]+))");
        auto m = idRx.match(local);
        if (m.hasMatch()) p.localId = m.captured(1).trimmed();
    }

    // remote{} block -- id and (optional) certs.
    QRegularExpression remoteRx(R"(remote\s*\{([^}]*)\})",
                                QRegularExpression::DotMatchesEverythingOption);
    auto remoteMatch = remoteRx.match(src);
    if (remoteMatch.hasMatch()) {
        const QString remote = remoteMatch.captured(1);
        QRegularExpression idRx(R"(\bid\s*=\s*([^\n#]+))");
        auto m = idRx.match(remote);
        if (m.hasMatch()) p.remoteId = m.captured(1).trimmed();
        // \b keeps "certs" from matching inside "cacerts".
        QRegularExpression certsRx(R"(\bcerts\s*=\s*([^\n#]+))");
        auto m2 = certsRx.match(remote);
        if (m2.hasMatch()) p.remoteCertFile = m2.captured(1).trimmed();
        QRegularExpression caRx(R"(\bcacerts\s*=\s*([^\n#]+))");
        auto m3 = caRx.match(remote);
        if (m3.hasMatch()) p.caCertFile = m3.captured(1).trimmed();
    }

    if (ok) *ok = !p.remoteAddr.isEmpty();
    return p;
}

QStringList Profile::listProfiles(const QString &swanctlDir)
{
    QDir d(swanctlDir + "/conf.d");
    QStringList out;
    if (!d.exists()) return out;
    for (const QString &f : d.entryList(QStringList() << "*.conf", QDir::Files)) {
        out << QFileInfo(f).completeBaseName();
    }
    return out;
}

bool Profile::remove(const QString &swanctlDir, const QString &name)
{
    bool any = false;
    QFile cf(confPath(swanctlDir, name));
    if (cf.exists()) any |= cf.remove();
    QFile sf(secretBlobPath(swanctlDir, name));
    if (sf.exists()) any |= sf.remove();
    return any;
}
