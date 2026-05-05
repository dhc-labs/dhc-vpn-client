// ProfileDialog: modal QDialog for creating / editing a VPN profile.
//
// Shown by MainWindow when the user clicks New or Edit. Holds a Profile
// in `profile()` and, on accept, the caller is responsible for:
//   1. p.save(swanctlDir)                       -- writes conf.d/<name>.conf
//   2. if EAP & savePassword(): write DPAPI blob to secrets.d/<name>.dat
//   3. trigger swanctl --load-all so the daemon picks up the new conn
//
// We deliberately do NOT touch the filesystem from inside the dialog --
// keeps the dialog self-contained and easy to test, and gives MainWindow
// a single place to handle errors.

#pragma once

#include "profile.h"

#include <QDialog>

class QButtonGroup;
class QCheckBox;
class QFormLayout;
class QLineEdit;
class QRadioButton;
class QStackedWidget;

class ProfileDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ProfileDialog(QWidget *parent = nullptr);

    // Pre-fill fields from an existing profile (for Edit). The name field
    // is locked when editing so the user can't rename a profile (would
    // require renaming the conf.d file -- out of scope for MVP).
    void setProfile(const Profile &p, bool editing);

    Profile profile() const;
    bool savePassword() const;
    QString plaintextPassword() const;

    // Raw file paths as the user typed/picked them (may be absolute
    // outside-of-swanctl-dir paths that need staging, or just bare
    // basenames already inside swanctl/). Empty if not set.
    QString rawClientCertPath() const;
    QString rawClientKeyPath()  const;
    QString rawCaCertPath()     const;
    QString rawRemoteCertPath() const;

private slots:
    void onAuthModeChanged();
    void onPickClientCert();
    void onPickClientKey();
    void onPickCaCert();
    void onPickRemoteCert();

private:
    QLineEdit *m_name        = nullptr;
    QLineEdit *m_remoteAddr  = nullptr;
    QLineEdit *m_remoteId    = nullptr;
    QLineEdit *m_localId     = nullptr;
    QLineEdit *m_remoteTs    = nullptr;
    QLineEdit *m_proposals   = nullptr;
    QLineEdit *m_espProposals= nullptr;

    QRadioButton *m_modeCert = nullptr;
    QRadioButton *m_modeEap  = nullptr;
    QStackedWidget *m_modeStack = nullptr;

    // Cert mode
    QLineEdit *m_clientCert  = nullptr;
    QLineEdit *m_clientKey   = nullptr;
    QLineEdit *m_caCert      = nullptr;
    QLineEdit *m_remoteCert  = nullptr;

    // EAP mode
    QLineEdit *m_eapUser     = nullptr;
    QLineEdit *m_eapPassword = nullptr;
    QCheckBox *m_savePassword= nullptr;

    bool m_editing = false;
};
