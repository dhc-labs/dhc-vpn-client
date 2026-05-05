// dpapi_secret: thin wrapper around Win32 DPAPI (CryptProtectData /
// CryptUnprotectData) for storing/loading EAP passwords.
//
// We use CRYPTPROTECT_LOCAL_MACHINE = 0 (current-user scope), so blobs can
// only be decrypted by the same Windows user that wrote them. This is the
// right choice for a per-user VPN profile and matches what most consumer
// VPN clients do.
//
// Blob layout on disk: just the raw output of CryptProtectData -- no
// header, no version. Reading back the raw bytes and feeding them to
// CryptUnprotectData round-trips correctly. If we ever need to migrate
// to per-machine scope we'd add a small header.

#pragma once

#include <QByteArray>
#include <QString>

namespace DpapiSecret {

// Encrypt `plaintext` using DPAPI (current-user) and write the resulting
// opaque blob to `path`. Creates parent directories as needed. Returns
// false on any error; if `err` is non-null it receives a description.
bool storeToFile(const QString &path,
                 const QString &plaintext,
                 QString *err = nullptr);

// Read DPAPI blob from `path` and decrypt it. On success `plaintext`
// holds the recovered string. On failure returns false and (optionally)
// fills `err`. The plaintext QString tries to zero its memory on
// destruction, but Qt's implicit-shared storage means this is best-effort
// only -- treat the lifetime as "as short as possible".
bool loadFromFile(const QString &path,
                  QString *plaintext,
                  QString *err = nullptr);

bool fileExists(const QString &path);
bool removeFile(const QString &path);

} // namespace DpapiSecret
