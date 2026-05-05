#include "profile_dialog.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace {

// Adds a row "<label>: [QLineEdit] [Browse...]" to a form, returning the
// line edit so the caller can wire it up. The browse callback is invoked
// when the button is clicked.
QLineEdit* addBrowseRow(QFormLayout *form, const QString &label,
                        const std::function<void(QLineEdit*)> &onBrowse)
{
    auto *row = new QHBoxLayout();
    auto *edit = new QLineEdit();
    auto *btn  = new QPushButton(QObject::tr("Browse..."));
    row->addWidget(edit, 1);
    row->addWidget(btn);
    form->addRow(label, row);
    QObject::connect(btn, &QPushButton::clicked, edit,
                     [edit, onBrowse]() { onBrowse(edit); });
    return edit;
}

} // namespace

ProfileDialog::ProfileDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("VPN Profile"));
    resize(540, 480);

    auto *root = new QVBoxLayout(this);

    // --- Common fields --------------------------------------------------
    auto *commonBox = new QGroupBox(tr("Connection"), this);
    auto *commonForm = new QFormLayout(commonBox);
    m_name        = new QLineEdit(commonBox);
    m_remoteAddr  = new QLineEdit(commonBox);
    m_remoteId    = new QLineEdit(commonBox);
    m_localId     = new QLineEdit(commonBox);
    m_remoteTs    = new QLineEdit("0.0.0.0/0", commonBox);
    m_proposals   = new QLineEdit("aes256-sha256-modp2048", commonBox);
    m_espProposals= new QLineEdit("aes256-sha256", commonBox);

    m_name      ->setPlaceholderText(tr("e.g. drhc-cert"));
    m_remoteAddr->setPlaceholderText(tr("vpn.example.com or 1.2.3.4"));
    m_remoteId  ->setPlaceholderText(tr("(optional, defaults to remote address)"));
    m_localId   ->setPlaceholderText(tr("(optional)"));

    commonForm->addRow(tr("Profile name:"),  m_name);
    commonForm->addRow(tr("Remote address:"),m_remoteAddr);
    commonForm->addRow(tr("Remote ID:"),     m_remoteId);
    commonForm->addRow(tr("Local ID:"),      m_localId);
    commonForm->addRow(tr("Remote subnet:"), m_remoteTs);
    commonForm->addRow(tr("IKE proposals:"), m_proposals);
    commonForm->addRow(tr("ESP proposals:"), m_espProposals);
    root->addWidget(commonBox);

    // --- Auth mode toggle -----------------------------------------------
    auto *authBox = new QGroupBox(tr("Authentication"), this);
    auto *authLayout = new QVBoxLayout(authBox);
    auto *modeRow = new QHBoxLayout();
    m_modeCert = new QRadioButton(tr("Certificate"), authBox);
    m_modeEap  = new QRadioButton(tr("Username + Password (EAP-MSCHAPv2)"), authBox);
    m_modeCert->setChecked(true);
    auto *grp = new QButtonGroup(authBox);
    grp->addButton(m_modeCert);
    grp->addButton(m_modeEap);
    modeRow->addWidget(m_modeCert);
    modeRow->addWidget(m_modeEap);
    modeRow->addStretch();
    authLayout->addLayout(modeRow);

    m_modeStack = new QStackedWidget(authBox);

    // Cert page
    auto *certPage = new QWidget(m_modeStack);
    auto *certForm = new QFormLayout(certPage);
    m_clientCert = addBrowseRow(certForm, tr("Client certificate:"),
        [this](QLineEdit*){ onPickClientCert(); });
    m_clientKey = addBrowseRow(certForm, tr("Client private key:"),
        [this](QLineEdit*){ onPickClientKey(); });
    m_caCert = addBrowseRow(certForm, tr("CA certificate:"),
        [this](QLineEdit*){ onPickCaCert(); });
    m_remoteCert = addBrowseRow(certForm, tr("Peer certificate:"),
        [this](QLineEdit*){ onPickRemoteCert(); });
    certForm->addRow(new QLabel(
        tr("<small>Files are copied into the swanctl directory on save. "
           "Leave CA and peer cert empty if not needed.</small>"), certPage));
    m_modeStack->addWidget(certPage);

    // EAP page
    auto *eapPage = new QWidget(m_modeStack);
    auto *eapForm = new QFormLayout(eapPage);
    m_eapUser     = new QLineEdit(eapPage);
    m_eapPassword = new QLineEdit(eapPage);
    m_eapPassword->setEchoMode(QLineEdit::Password);
    m_savePassword = new QCheckBox(
        tr("Remember password (encrypted via Windows DPAPI, current user)"),
        eapPage);
    m_savePassword->setChecked(true);

    eapForm->addRow(tr("Username:"), m_eapUser);
    eapForm->addRow(tr("Password:"), m_eapPassword);
    eapForm->addRow(QString(),       m_savePassword);
    m_modeStack->addWidget(eapPage);

    authLayout->addWidget(m_modeStack);
    root->addWidget(authBox);

    // --- Buttons --------------------------------------------------------
    auto *btns = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    connect(btns, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(btns, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(btns);

    connect(m_modeCert, &QRadioButton::toggled,
            this, &ProfileDialog::onAuthModeChanged);
    onAuthModeChanged();
}

void ProfileDialog::onAuthModeChanged()
{
    m_modeStack->setCurrentIndex(m_modeCert->isChecked() ? 0 : 1);
}

void ProfileDialog::setProfile(const Profile &p, bool editing)
{
    m_editing = editing;
    m_name        ->setText(p.name);
    m_name        ->setReadOnly(editing);
    m_remoteAddr  ->setText(p.remoteAddr);
    m_remoteId    ->setText(p.remoteId);
    m_localId     ->setText(p.localId);
    m_remoteTs    ->setText(p.remoteTs);
    m_proposals   ->setText(p.proposals);
    m_espProposals->setText(p.espProposals);
    m_clientCert  ->setText(p.clientCertFile);
    m_clientKey   ->setText(p.clientKeyFile);
    m_caCert      ->setText(p.caCertFile);
    m_remoteCert  ->setText(p.remoteCertFile);
    m_eapUser     ->setText(p.eapUsername);
    m_eapPassword ->clear();

    if (p.authMode == Profile::AuthMode::EapMschapv2) {
        m_modeEap->setChecked(true);
    } else {
        m_modeCert->setChecked(true);
    }
    onAuthModeChanged();
}

Profile ProfileDialog::profile() const
{
    Profile p;
    p.name         = m_name->text().trimmed();
    p.remoteAddr   = m_remoteAddr->text().trimmed();
    p.remoteId     = m_remoteId->text().trimmed();
    p.localId      = m_localId->text().trimmed();
    p.remoteTs     = m_remoteTs->text().trimmed();
    p.proposals    = m_proposals->text().trimmed();
    p.espProposals = m_espProposals->text().trimmed();

    if (m_modeCert->isChecked()) {
        p.authMode       = Profile::AuthMode::Certificate;
        p.clientCertFile = QFileInfo(m_clientCert->text()).fileName();
        p.clientKeyFile  = QFileInfo(m_clientKey->text()).fileName();
        p.caCertFile     = QFileInfo(m_caCert->text()).fileName();
        p.remoteCertFile = QFileInfo(m_remoteCert->text()).fileName();
    } else {
        p.authMode    = Profile::AuthMode::EapMschapv2;
        p.eapUsername = m_eapUser->text().trimmed();
    }
    return p;
}

bool ProfileDialog::savePassword() const
{
    return m_modeEap->isChecked() && m_savePassword->isChecked()
        && !m_eapPassword->text().isEmpty();
}

QString ProfileDialog::plaintextPassword() const
{
    return m_eapPassword->text();
}

QString ProfileDialog::rawClientCertPath() const { return m_clientCert->text(); }
QString ProfileDialog::rawClientKeyPath()  const { return m_clientKey ->text(); }
QString ProfileDialog::rawCaCertPath()     const { return m_caCert    ->text(); }
QString ProfileDialog::rawRemoteCertPath() const { return m_remoteCert->text(); }

// ---- file pickers --------------------------------------------------------
//
// We store only the basename of the picked file in the line edit. The
// MainWindow handles the actual copy into swanctl/x509/, x509ca/, private/
// after the dialog is accepted, because that touches the filesystem and
// belongs to the same place that handles --load-all.

void ProfileDialog::onPickClientCert()
{
    const QString f = QFileDialog::getOpenFileName(
        this, tr("Select client certificate"),
        QString(), tr("Certificates (*.pem *.crt *.cer);;All (*)"));
    if (!f.isEmpty()) m_clientCert->setText(f);
}

void ProfileDialog::onPickClientKey()
{
    const QString f = QFileDialog::getOpenFileName(
        this, tr("Select private key"),
        QString(), tr("Keys (*.pem *.key);;All (*)"));
    if (!f.isEmpty()) m_clientKey->setText(f);
}

void ProfileDialog::onPickCaCert()
{
    const QString f = QFileDialog::getOpenFileName(
        this, tr("Select CA certificate"),
        QString(), tr("Certificates (*.pem *.crt *.cer);;All (*)"));
    if (!f.isEmpty()) m_caCert->setText(f);
}

void ProfileDialog::onPickRemoteCert()
{
    const QString f = QFileDialog::getOpenFileName(
        this, tr("Select peer certificate (for self-signed peers)"),
        QString(), tr("Certificates (*.pem *.crt *.cer);;All (*)"));
    if (!f.isEmpty()) m_remoteCert->setText(f);
}
