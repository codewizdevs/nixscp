// Port of WinSCP source/core/Security.{h,cpp}, the password manager part of
// core/Cryptography.cpp (AES-256 with HMAC-SHA1, password scrambling,
// IsValidPassword) and the master password logic of windows/WinConfiguration.cpp
// (StronglyRecryptPassword, DecryptPassword, SetMasterPassword, ...).
//
// Passwords stored in WinSCP.ini / nixscp.ini are byte-compatible with WinSCP:
// without a master password they are obfuscated (PWALG_SIMPLE); with a master
// password they are AES-256 encrypted and wrapped as "external" passwords, so a
// nixscp.ini protected by a master password can be used by WinSCP and vice versa.
#pragma once

#include <QByteArray>
#include <QString>

#include <functional>

namespace Security
{
  constexpr unsigned char PWALG_SIMPLE_MAGIC = 0xA3;
  constexpr int PWALG_SIMPLE_MAXLEN = 50;
  constexpr unsigned char PWALG_SIMPLE_FLAG = 0xFF;

  // ---- Stored passwords ----
  // TConfiguration::EncryptPassword + TWinConfiguration::StronglyRecryptPassword:
  // AES-encrypted (external) form when a master password is in use and known,
  // simple obfuscation otherwise
  QByteArray EncryptPassword(const QString & Password, const QString & Key);
  // TWinConfiguration::DecryptPassword: decrypts both forms; for the AES form
  // the master password prompt (SetMasterPasswordPrompt) is used when the master
  // password is not known yet. Returns an empty string when the password cannot
  // be decrypted (wrong key, prompt cancelled, no prompt handler).
  QString DecryptPassword(const QByteArray & Password, const QString & Key);
  // Encrypted with the master password (TWinConfiguration::GetExternalEncryptedPassword)
  bool IsStronglyEncrypted(const QByteArray & Password);
  // Two stored forms of the same password (both forms use random padding/salt,
  // so the raw values differ on each save)
  bool SamePassword(const QByteArray & Password1, const QByteArray & Password2);

  // ---- core/Security.cpp ----
  QByteArray SimpleEncryptPassword(const QString & Password, const QString & Key);
  QString SimpleDecryptPassword(const QByteArray & Password, const QString & Key);
  QByteArray SetExternalEncryptedPassword(const QByteArray & Password);
  bool GetExternalEncryptedPassword(const QByteArray & Encrypted, QByteArray & Password);

  // ---- core/Cryptography.cpp ----
  // Brian Gladman's password based file encryption (AES-256 in CTR mode,
  // HMAC-SHA1, PBKDF2 with 1000 iterations); Output = salt + data + MAC
  void AES256EncryptWithMAC(const QByteArray & Input, const QString & Password, QByteArray & Output);
  bool AES256DecryptWithMAC(const QByteArray & Input, const QString & Password, QByteArray & Output);
  void AES256CreateVerifier(const QString & Input, QByteArray & Verifier);
  bool AES256Verify(const QString & Input, const QByteArray & Verifier);
  QByteArray ScramblePassword(const QString & Password);
  bool UnscramblePassword(const QByteArray & Scrambled, QString & Password);
  int PasswordMaxLength();
  // -1 = invalid (empty or too long), 0 = too simple, 1 = good
  int IsValidPassword(const QString & Password);
  QString BytesToHex(const QByteArray & Bytes);   // upper case, as WinSCP
  QByteArray HexToBytes(const QString & Hex);

  // ---- Master password (TWinConfiguration) ----
  // State as stored in Configuration\Security (UseMasterPassword, MasterPasswordVerifier);
  // set by Configuration::Load. Forgets a known master password, if the verifier changed.
  void SetMasterPasswordState(bool UseMasterPassword, const QString & VerifierHex);
  bool UseMasterPassword();
  QString MasterPasswordVerifier();
  bool ValidateMasterPassword(const QString & Password);
  // TWinConfiguration::SetMasterPassword: remembers the plain master password
  // (for decrypting only, when master password is not in use)
  void SetMasterPassword(const QString & Password);
  // TWinConfiguration::ChangeMasterPassword / ClearMasterPassword (state only;
  // Configuration::ChangeMasterPassword re-encrypts and saves the stored passwords)
  void ChangeMasterPassword(const QString & Password);
  void ClearMasterPassword();
  // The master password is known (passwords can be encrypted with it)
  bool IsMasterPasswordKnown();
  // Prompt handler (GUI: master password dialog, CLI: console prompt). It has to
  // call SetMasterPassword with a validated password and return true, or false when cancelled.
  using MasterPasswordPromptFunc = std::function<bool()>;
  void SetMasterPasswordPrompt(const MasterPasswordPromptFunc & Prompt);
  // Asks for the master password, unless it is known already; false when cancelled
  bool AskForMasterPasswordIfNotSet();

  // TWinConfiguration::BeginMasterPasswordSession: within a session the user
  // is asked for the master password once at most (e.g. while loading all sites)
  class MasterPasswordSession
  {
  public:
    MasterPasswordSession();
    ~MasterPasswordSession();
  };
  // Within the scope, the user is not asked for the master password (e.g. when
  // re-reading the configuration saved by another instance in the background)
  class NoMasterPasswordPrompt
  {
  public:
    NoMasterPasswordPrompt();
    ~NoMasterPasswordPrompt();
  };
  // FDontDecryptPasswords: within the scope, strongly encrypted passwords are not decrypted
  class DontDecryptPasswords
  {
  public:
    DontDecryptPasswords();
    ~DontDecryptPasswords();
  };
}
