// NixSCP Pro cloud sync engine (nixscp-cloud docs/SYNC.md).
//
// Two slots, both end-to-end encrypted with the same data key (DEK):
//  - "settings": the normalized nixscp.ini (CloudSettings). Version tags with
//    compare-and-swap on the server; local and cloud changes made at the same
//    time are a conflict that only the user resolves (ConflictDetected).
//  - "session": the open tabs, as a workspace. Last writer wins; it is applied
//    at startup only (FetchSession).
//
// The sync state (tokens, versions, the wrapped key and, when remembered, the
// DEK) is kept in nixscp-cloud.ini next to the configuration file, readable by
// the owner only. It is never part of the synced settings.
#pragma once

#include "core/CloudClient.h"
#include "core/CloudCrypto.h"
#include "core/IniStorage.h"

#include <QDateTime>
#include <QObject>
#include <QString>

#include <functional>

class Configuration;
class QTimer;

class CloudSync : public QObject
{
  Q_OBJECT

public:
  enum TState
  {
    csDisabled,     // no configuration file (/ini=nul), or not initialized
    csSignedOut,
    csNeedKey,      // signed in, the sync password was not entered on this computer
    csIdle,
    csSyncing,
    csConflict,     // waiting for the user (ConflictDetected)
    csPaused,       // conflict postponed ("Decide later")
    csNoLicense,    // uploading needs an active NixSCP Pro license
    csError,        // last attempt failed (network...); retried later
  };
  enum TResolution { crUseCloud, crUseLocal, crLater };

  static CloudSync & Instance();

  // Loads the sync state and starts syncing (when signed in and unlocked)
  void Initialize(Configuration & Config);

  TState State() const { return FState; }
  QString StatusText() const;
  QString LastError() const { return FLastError; }
  QDateTime LastSync() const { return FLastSync; }
  QString Email() const { return FEmail; }
  bool SignedIn() const { return FClient->HasRefreshToken(); }
  bool HasKey() const { return !FDek.isEmpty(); }
  CloudClient & Client() { return *FClient; }
  static QString DeviceName();

  // ---- Options (this computer only) ----
  bool SyncSessions() const { return FSyncSessions; }
  void SetSyncSessions(bool Value);
  bool RestoreSessions() const { return FRestoreSessions; }
  void SetRestoreSessions(bool Value);
  bool RememberKey() const { return FRememberKey; }
  void SetRememberKey(bool Value);

  // ---- Account ----
  // Desktop sign-in (OAuth PKCE, loopback redirect): the URL to open in the
  // browser; CompleteSignIn is called with the code the browser brings back
  struct SignInRequest
  {
    QString Url;
    QString Verifier;
    QString State;
    QString RedirectUri;
  };
  SignInRequest NewSignInRequest(quint16 LoopbackPort) const;
  void CompleteSignIn(const SignInRequest & Request, const QString & Code, const std::function<void(bool, const QString &)> & Done);
  void SignOut();
  // /v1/me: license summary (active, plan, status...) for the UI
  void FetchAccount(const std::function<void(bool, const QJsonObject &)> & Done);
  // Looks the license up (also when nothing uploads, e.g. when the beta ends):
  // LicenseInactive once per lapse
  void CheckLicense();

  // ---- Key (sync password) ----
  // Whether the cloud already has encrypted data (then UnlockKey, else CreateKey)
  void ProbeCloud(const std::function<void(bool Ok, bool HasData, const QString & Error)> & Done);
  // First computer: a new data key. Done gets the formatted recovery key (when
  // requested) or the error.
  void CreateKey(const QString & Password, bool WithRecoveryKey, const std::function<void(bool, const QString &)> & Done);
  // Another computer: unlocks the data key from the cloud with the sync
  // password or the recovery key
  void UnlockKey(const QString & Secret, bool IsRecoveryKey, const std::function<void(bool, const QString &)> & Done);
  // New sync password (the data key stays; other computers keep working)
  void ChangePassword(const QString & NewPassword, const std::function<void(bool, const QString &)> & Done);

  // ---- Data key in configuration backups (Export/Import Configuration) ----
  enum TKeyImport { kiNone, kiImported, kiOtherAccount, kiInvalid };
  // Adds the CloudSettings::KeySection to an exported configuration; false
  // when this computer has no key
  bool ExportKey(IniFile & File) const;
  // Takes the key section out of an imported configuration (always) and uses
  // the key when it belongs to the signed-in account, or to the account that
  // signs in next. Email gets the account of the key.
  TKeyImport ImportKey(IniFile & File, QString * Email = nullptr);

  // ---- Settings slot ----
  void SyncNow();
  // Checks the cloud when the last check is older than MaxAgeSeconds (window focus)
  void SyncIfStale(int MaxAgeSeconds);
  // On exit: uploads settings changed in the last seconds (waits at most TimeoutMs)
  void FlushSettings(int TimeoutMs);
  void ResolveConflict(TResolution Resolution);

  // ---- Session slot ----
  // Uploads the open tabs (a workspace in WinSCP.ini format); last writer wins
  void PushSession(const QByteArray & Payload);
  // The same, waiting at most TimeoutMs (on exit)
  void FlushSession(const QByteArray & Payload, int TimeoutMs);
  // The tabs saved last by any computer; Found = false when there are none
  // (or the cloud was not reachable in time and nothing is cached)
  void FetchSession(int TimeoutMs, const std::function<void(bool Found, const QByteArray & Payload)> & Done);

  // Pure decision of the settings algorithm (docs/SYNC.md), for tests
  enum TAction { saNone, saPull, saPush, saConflict, saRolledBack };
  static TAction Decide(qint64 BaseVersion, const QString & BaseHash, qint64 HeadVersion, const QString & LocalHash);

signals:
  void StateChanged();
  // Settings changed here and in the cloud: call ResolveConflict
  void ConflictDetected(const QString & CloudDevice, const QDateTime & CloudTime);
  // Settings from another computer were applied
  void SettingsPulled(const QString & Device);
  // The server signed this computer out (password changed, removed on the website...)
  void SignedOutRemotely(const QString & Message);
  // Uploading stopped: the NixSCP Pro license is not active (once per lapse)
  void LicenseInactive();
  // A snapshot that this computer's key cannot decrypt was ignored and replaced
  // (uploaded by a computer that does not know the sync password)
  void ForeignSnapshotIgnored(const QString & Device);

private:
  CloudSync();

  struct Head
  {
    qint64 Version = 0;
    QString Device;
    QDateTime UpdatedAt;
  };
  using HeadCallback = std::function<void(bool Ok, const Head &, const CloudClient::Reply &)>;
  using DataCallback = std::function<void(bool Ok, const QByteArray & Data, const QString & Error)>;
  using DoneCallback = std::function<void(bool Ok, const QString & Error)>;
  using UploadCallback = std::function<void(int Status, qint64 Version, const Head & Conflict, const QString & Error)>;

  Configuration * FConfig = nullptr;
  CloudClient * FClient;
  QString FStateFileName;
  IniFile FStateFile;
  TState FState = csDisabled;
  QString FLastError;
  QDateTime FLastSync;
  QString FEmail;
  QString FUser;
  QByteArray FDek;
  CloudCrypto::SnapshotHeader FWrap;  // KDF parameters and wrapped DEK, copied into every header
  bool FWrapDirty = false;            // new sync password not uploaded yet
  qint64 FSettingsBase = 0;
  QString FSettingsHash;
  qint64 FSessionBase = 0;
  bool FSyncSessions = true;
  bool FRestoreSessions = true;
  bool FRememberKey = true;
  bool FBusy = false;
  bool FSyncPending = false;
  bool FApplying = false;
  Head FConflictHead;
  QByteArray FPendingSession;
  bool FSessionBusy = false;
  bool FLicenseNotified = false;
  QTimer * FDebounce;
  QTimer * FPeriodic;

  void LoadState();
  void SaveState();
  void SetState(TState State, const QString & Error = QString());
  bool Ready() const;
  void UpdateReadyState();
  void ResetSyncState();
  QString AutoWorkspacePrefix() const;
  QByteArray LocalSettings() const;
  CloudCrypto::SnapshotHeader NewHeader(const QString & Slot, qint64 Version) const;
  void AdoptWrap(const CloudCrypto::SnapshotHeader & Header);
  // Authenticates and decrypts a downloaded snapshot of Slot at Version
  bool Decode(const QByteArray & Data, const QString & Slot, qint64 Version, CloudCrypto::SnapshotHeader & Header,
    QByteArray & Plaintext, QString & Error) const;

  void ScheduleSync();
  void TimedSync();
  void FinishSync(TState State, const QString & Error = QString());
  void RunSettingsSync(int Attempt);
  void FirstSync(const Head & Remote, const QByteArray & Local, const QString & LocalHash);
  void PullSettings(const Head & Remote);
  void PushSettings(qint64 Base, const QByteArray & Plaintext, const QString & LocalHash, int Attempt);
  void ApplySettings(const QByteArray & Plaintext, qint64 Version, const QString & Device, qint64 PushOnTop = 0);
  void NoteNoLicense();
  // Unwraps the data key of a snapshot with the sync password (or recovery key)
  // and proves it by decrypting the snapshot
  bool TryUnlock(const QByteArray & Data, const QString & Slot, qint64 Version, const QString & Secret, bool IsRecoveryKey,
    QByteArray & Dek, CloudCrypto::SnapshotHeader & Header, QString & Problem) const;
  // The settings head does not decrypt: a foreign snapshot when this key still
  // decrypts an older version (it is then replaced), otherwise a stale key
  void HandleUndecryptable(const Head & Remote, const QString & Device);
  using FoundCallback = std::function<void(bool Found, qint64 Version, const QByteArray & Data)>;
  // Newest snapshot of Slot below Below (0 = any) that passes Accept
  void FindSnapshot(const QString & Slot, qint64 Below, int MaxTries,
    const std::function<bool(qint64 Version, const QByteArray & Data)> & Accept, const FoundCallback & Done);
  void Backup(const IniFile & Local) const;
  static bool HasStoredSites(const IniFile & File);

  void GetHead(const QString & Slot, const HeadCallback & Done);
  void DownloadRaw(const QString & Slot, qint64 Version, const DataCallback & Done);
  void Upload(const QString & Slot, qint64 Base, const QByteArray & Snapshot, const UploadCallback & Done);
  void PushSessionAttempt(const QByteArray & Payload, const QDateTime & SavedAt, int Attempt, const DoneCallback & Done);
  QString SessionCacheFileName() const;
  void WriteSessionCache(const QByteArray & Payload) const;
  QByteArray ReadSessionCache() const;
};
