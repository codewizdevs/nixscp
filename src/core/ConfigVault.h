// Encryption of the local configuration for NixSCP Pro users. The whole
// nixscp.ini (and the sync state, the open tabs cache and the sync backups) is
// stored as an NXS1 snapshot (core/CloudCrypto.h) with slot "local", encrypted
// with the same data key (DEK) as the cloud sync. The file carries the DEK
// wrapped with the user's NixSCP password (Argon2id) and recovery key, so it is
// self-contained: the password, or the recovery key, unlocks it. Inside it is
// a plain WinSCP-format configuration (no WinSCP master password), which is
// what exports write, so WinSCP can read them.
//
// The key lives in memory only (SecureBytes). CloudSync enables the encryption
// whenever it has the key (mandatory for Pro users); the application unlocks an
// encrypted file at startup, before the configuration is loaded.
//
// Every sealed file names its role in the authenticated header, so a backup
// cannot be put in place of the configuration, nor one file in place of another.
// A stolen disk gives an attacker the headers: the salt and the wraps allow
// guessing the password offline at Argon2id's cost, and nothing else opens
// without it. Older sealed copies of the same file cannot be told apart here:
// with write access to the folder, a rollback of a file to an earlier copy of
// itself, or deleting everything, stays possible.
//
// A new key reaches the files one by one (ReplaceKey). A crash in between leaves
// a companion file, sealed with the new key, that holds the old one: whoever
// unlocks with the new key finishes the work before anything is read.
#pragma once

#include "core/CloudCrypto.h"
#include "core/Security.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace ConfigVault
{
  constexpr const char * Slot = "local";
  // The role of a sealed file
  constexpr const char * RoleConfig = "config";
  constexpr const char * RoleState = "state";       // nixscp-cloud.ini
  constexpr const char * RoleSession = "session";
  constexpr const char * RoleTeams = "teams";
  constexpr const char * RoleKeys = "keys";         // nixscp-cloud-keys.ini: key changes under way (CloudSync)
  constexpr const char * RoleBridge = "bridge";     // nixscp-cloud-bridge.ini: the old key during ReplaceKey
  constexpr const char * RoleBackup = "backup";
  constexpr const char * RoleAgent = "agent";
  constexpr const char * RoleExport = "export";     // an encrypted backup the user exported
  constexpr const char * RoleFile = "file";         // sealed on request (Seal): SSH keys on the phone

  // The data is an encrypted local configuration (NXS1, slot "local")
  bool IsSealed(const QByteArray & Data);
  // Starts like an encrypted file: sealed or damaged, but never plaintext
  bool LooksSealed(const QByteArray & Data);
  bool IsSealedFile(const QString & FileName);
  // The sync state next to the configuration is sealed, so the configuration
  // must be too: when it is plain or missing it was replaced or removed, and it
  // is refused. Removing encryption rewrites the state first (CloudSync). This is
  // about the configuration the state goes with: the state names it, and another
  // file in the same folder (/ini=) is free to be plain.
  bool RefusesConfig(const QString & ConfigFile);
  // What to tell the user then
  QString RefusedText(const QString & ConfigFile);
  // Why the configuration cannot be used whatever the password is: it is refused
  // (above), damaged, or encrypted by NixSCP 1.2.3 or older. Empty when it can.
  QString ConfigProblem(const QString & ConfigFile);
  // The same with the way out, for where the user can reach the files
  QString ConfigProblemText(const QString & ConfigFile);
  // What starting over removes: the configuration and its companion files that
  // exist. Encrypted backups stay.
  QStringList StartOverFiles(const QString & ConfigFile);

  // Encryption is on: protected files are written encrypted
  bool Enabled();
  // The key, its wraps and the account it belongs to (CloudSync)
  void SetKey(const QByteArray & Dek, const CloudCrypto::SnapshotHeader & Wrap);
  QByteArray Key();
  const SecureBytes & SecureKey();
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
  // The password or recovery key opens the current key (for sensitive changes
  // in an unlocked session); the key and the files stay as they are
  bool CheckSecret(const QString & Secret, bool IsRecoveryKey, QString * Error = nullptr);

  // Files written encrypted while enabled: the configuration, the sync state,
  // the open tabs cache and the sync backups (exports to other places stay readable)
  bool IsProtectedFile(const QString & FileName);
  // The role of a protected file, empty for any other file
  QString RoleOf(const QString & FileName);

  // Encrypted with the key (Enabled() must be true), role "file"
  QByteArray Seal(const QByteArray & Plaintext);
  // Plaintext of Data: unchanged when not encrypted, decrypted with the key
  // when it is; false when it is encrypted and cannot be decrypted, or damaged.
  // For data of any role: an import, or a file outside the configuration.
  bool Open(const QByteArray & Data, QByteArray & Plaintext);
  // The same for the contents of FileName: a protected file must have its role,
  // and the configuration is refused as RefusesConfig says
  bool OpenFile(const QString & FileName, const QByteArray & Data, QByteArray & Plaintext);

  // Decrypts an encrypted file made with another key (an encrypted backup of
  // another account or an old password) with its password or recovery key,
  // without changing the current key
  bool OpenWithSecret(const QByteArray & Data, const QString & Secret, bool IsRecoveryKey, QByteArray & Plaintext,
    QString * Error = nullptr);
  // An encrypted backup for Export: opened with the NixSCP password or recovery
  // key on any computer, through Import (Enabled() must be true)
  QByteArray SealExport(const QByteArray & Plaintext);

  // For writing FileName: encrypted when enabled and the file is protected
  QByteArray ForFile(const QString & FileName, const QByteArray & Plaintext);
  // Reads a file and decrypts it when needed; Locked is set when it is
  // encrypted and the key is not known, damaged, or refused (Data is then empty)
  QByteArray ReadFile(const QString & FileName, bool * Locked = nullptr);

  // The key was replaced: encrypts again what OldKey opens, in the configuration
  // folder and the folders added here. A file left behind could not be read anymore.
  // The configuration goes first and the sync state last, so whatever a crash
  // leaves, the state still opens from the key the configuration asks for. False
  // when the configuration could not be written: nothing else was touched then.
  bool ResealFiles(const QByteArray & OldKey);
  void AddSealedFolder(const QString & Folder);
  // SetKey for another key, with the files following it (ResealFiles); a crash is
  // finished by Unlock. False when the configuration cannot be written: the
  // old key stays then. The caller ends it with EndReplaceKey once its state is saved.
  bool ReplaceKey(const QByteArray & Dek, const CloudCrypto::SnapshotHeader & Wrap);
  void EndReplaceKey();

  // Tests: called at each step of a key change, to stop the process right there
  extern void (*KeyChangeStep)(const char * Step);
  inline void StepReached(const char * Step)
  {
    if (KeyChangeStep != nullptr)
    {
      KeyChangeStep(Step);
    }
  }
}
