// Profile: in-memory representation of a single VPN connection profile,
// plus serialization to swanctl.conf-style fragments.
//
// Each profile is persisted as one file under SWANCTL_DIR/conf.d/<name>.conf
// containing a `connections {}` block (and, for cert mode, the file-based
// secret references). EAP passwords are NEVER stored in this file -- they
// live DPAPI-encrypted under SWANCTL_DIR/secrets.d/<name>.dat and are
// materialized into a temp secrets file only at connect time.

#pragma once

#include <QString>
#include <QStringList>

class QDir;

struct Profile
{
    enum class AuthMode { Certificate, EapMschapv2 };

    // Identity
    QString name;            // also the conf.d filename stem
    QString remoteAddr;      // gateway host or IP
    QString remoteId;        // peer IKE ID (often == remoteAddr)
    QString localId;         // local IKE ID. Cert mode: %fromcert by default

    // Topology
    QString remoteTs = "0.0.0.0/0";  // remote traffic selector / route

    // Crypto (kept conservative -- matches our M5 Cisco baseline)
    QString proposals     = "aes256-sha256-modp2048";
    QString espProposals  = "aes256-sha256";

    // Auth
    AuthMode authMode = AuthMode::Certificate;

    // Certificate mode
    QString clientCertFile;  // basename inside swanctl/x509/
    QString clientKeyFile;   // basename inside swanctl/private/
    QString caCertFile;      // basename inside swanctl/x509ca/ (optional)
    QString remoteCertFile;  // basename inside swanctl/x509/ (optional, for self-signed peer)

    // EAP mode
    QString eapUsername;
    // Password is NOT held in this struct after save -- it goes through DPAPI
    // into secrets.d/. transient_password is set only during dialog interaction
    // and right before a connect attempt.
    QString transientPassword;

    // ----- conversion helpers -----

    // Render the connections{} block (no secrets inside).
    QString toConnectionBlock() const;

    // Render the secrets{} block for EAP mode using the given plaintext
    // password. Empty string for Certificate mode (cert auth uses file
    // references in connections{}, not separate secrets here).
    QString toSecretBlock(const QString &plaintextPassword) const;

    // Persist / load the profile (without password) to/from
    // <swanctlDir>/conf.d/<name>.conf.
    bool save(const QString &swanctlDir, QString *err = nullptr) const;
    static Profile load(const QString &swanctlDir,
                        const QString &name,
                        bool *ok = nullptr);

    // List all <name>.conf stems in <swanctlDir>/conf.d/.
    static QStringList listProfiles(const QString &swanctlDir);

    // Remove conf.d/<name>.conf and secrets.d/<name>.dat (if present).
    static bool remove(const QString &swanctlDir, const QString &name);
};
