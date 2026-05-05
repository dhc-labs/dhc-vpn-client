#include "dpapi_secret.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <windows.h>
#include <dpapi.h>

namespace DpapiSecret {

bool storeToFile(const QString &path, const QString &plaintext, QString *err)
{
    QDir().mkpath(QFileInfo(path).absolutePath());

    const QByteArray utf8 = plaintext.toUtf8();
    DATA_BLOB in;
    in.pbData = const_cast<BYTE*>(reinterpret_cast<const BYTE*>(utf8.constData()));
    in.cbData = static_cast<DWORD>(utf8.size());

    DATA_BLOB out{};
    // dwFlags = 0 -> current-user scope; entropy = NULL -> no extra key.
    if (!CryptProtectData(&in, L"dhc-vpn-eap", nullptr, nullptr, nullptr,
                          0, &out)) {
        if (err) *err = QString("CryptProtectData failed (gle=%1)").arg(GetLastError());
        return false;
    }

    QSaveFile f(path);
    bool ok = false;
    if (f.open(QIODevice::WriteOnly)) {
        const qint64 n = f.write(reinterpret_cast<const char*>(out.pbData),
                                 out.cbData);
        ok = (n == out.cbData) && f.commit();
        if (!ok && err) *err = f.errorString();
    } else {
        if (err) *err = f.errorString();
    }

    if (out.pbData) LocalFree(out.pbData);
    return ok;
}

bool loadFromFile(const QString &path, QString *plaintext, QString *err)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (err) *err = f.errorString();
        return false;
    }
    QByteArray blob = f.readAll();
    f.close();
    if (blob.isEmpty()) {
        if (err) *err = "secret blob empty";
        return false;
    }

    DATA_BLOB in;
    in.pbData = reinterpret_cast<BYTE*>(blob.data());
    in.cbData = static_cast<DWORD>(blob.size());

    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr,
                            0, &out)) {
        if (err) *err = QString("CryptUnprotectData failed (gle=%1)").arg(GetLastError());
        return false;
    }

    if (plaintext) {
        *plaintext = QString::fromUtf8(reinterpret_cast<const char*>(out.pbData),
                                       static_cast<int>(out.cbData));
    }
    // Best-effort wipe of the decrypted buffer before freeing.
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return true;
}

bool fileExists(const QString &path) { return QFileInfo::exists(path); }
bool removeFile(const QString &path) { return QFile::remove(path); }

} // namespace DpapiSecret
