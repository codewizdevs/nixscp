// Encryption of the local configuration for NixSCP Pro users. The whole
// nixscp.ini (and the open tabs cache and the sync backups) is stored as an
// NXS1 snapshot (core/CloudCrypto.h) with slot "local", encrypted with the
// same data key (DEK) as the cloud sync. The file carries the DEK wrapped with
// the user's NixSCP password (Argon2id) and recovery key, so it is
// self-contained: the password, or the recovery key, unlocks it. Inside it is
// a plain WinSCP-format configuration (no WinSCP master password), which is
// what exports write, so WinSCP can read them.
//
// The key lives in memory only. CloudSync enables the encryption whenever it
// has the key (mandatory for Pro users); the application unlocks an
// encrypted file at startup, before the configuration is loaded.
#pragma once

#include "core/CloudCrypto.h"

#include <QByteArray>
#include <QString>

namespace ConfigVault
{
  constexpr const char * Slot = "local";

  // The data is an encrypted local configuration (NXS1, slot "local")
  bool IsSealed(const QByteArray & Data);
  bool IsSealedFile(const QString & FileName);

  // Encryption is on: protected files are written encrypted
  bool Enabled();
  // The key, its wraps and the account it belongs to (CloudSync)
  void SetKey(const QByteArray & Dek, const CloudCrypto::SnapshotHeader & Wrap);
  QByteArray Key();
  QString User();
  CloudCrypto::SnapshotHeader Wrap();
  // Writes protected files unencrypted again (the key is still used to read
  // what is encrypted until it is rewritten)
  void Disable();
  // Forgets the key (as a new process: encrypted files are locked)
  void Forget();

  // Unlocks with the password or recovery key of an encrypted file (reads the
  // wraps from it); false with Error for a wrong secret or a damaged file
  bool Unlock(const QString & FileName, const QString & Secret, bool IsRecoveryKey, QString * Error = nullptr);

  // Files written encrypted while enabled: the configuration, the open tabs
  // cache and the sync backups (exports to other places stay readable)
  bool IsProtectedFile(const QString & FileName);

  // Encrypted with the key (Enabled() must be true)
  QByteArray Seal(const QByteArray & Plaintext);
  // Plaintext of Data: unchanged when not encrypted, decrypted with the key
  // when it is; false when it is encrypted and cannot be decrypted
  bool Open(const QByteArray & Data, QByteArray & Plaintext);

  // Decrypts an encrypted file made with another key (an encrypted backup of
  // another account or an old password) with its password or recovery key,
  // without changing the current key
  bool OpenWithSecret(const QByteArray & Data, const QString & Secret, bool IsRecoveryKey, QByteArray & Plaintext,
    QString * Error = nullptr);
  // An encrypted backup for Export: the same as the local file, opened with the
  // NixSCP password or recovery key on any computer (Enabled() must be true)
  QByteArray SealExport(const QByteArray & Plaintext);

  // For writing FileName: encrypted when enabled and the file is protected
  QByteArray ForFile(const QString & FileName, const QByteArray & Plaintext);
  // Reads a file and decrypts it when needed; Locked is set when it is
  // encrypted and the key is not known (Data is then empty)
  QByteArray ReadFile(const QString & FileName, bool * Locked = nullptr);
}
