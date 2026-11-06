// NixSCP Pro cloud sync engine (nixscp-cloud docs/SYNC.md).
//
// Two slots, both end-to-end encrypted with the same data key (DEK):
//  - "settings": the normalized nixscp.ini (CloudSettings). Version tags with
//    compare-and-swap on the server; local and cloud changes made at the same
//    time are a conflict that only the user resolves (ConflictDetected).
//  - "session": the open tabs, as a workspace. Last writer wins; it is applied
//    at startup only (FetchSession).
//
// The sync state (the server, tokens, versions, the wraps of the data key and
// the newest version seen of each slot) is kept in nixscp-cloud.ini next to the
// configuration file. It is sealed with the data key (core/ConfigVault), so a
// change to it is noticed and a stolen disk gives no usable sign-in. It is never
// part of the synced settings. Before a key exists there is nothing to seal
// with: a sign-in waiting for the sync password stays in memory, and the file
// holds only this computer's options, which are trusted for nothing else.
//
// What a change of the keys must not lose is kept in nixscp-cloud-keys.ini, sealed
// as well: the journal. A replacement of the data key goes there before anything
// is uploaded, so a lost reply or a crash leaves the new key, its recovery key and
// the team keys wrapped for it on this computer; the next sync reads the head and
// finishes the replacement or calls it off (SettleRotation). Team keys waiting
// to reach the server, old keys that may still open them, and a recovery key the
// user has not confirmed stay there too. It belongs to one account: a sign-out
// leaves it for that account's return.
//
// What a malicious server can still do: an established computer refuses older
// versions and histories that do not continue what it saw (rollback, fork). A
// new computer has seen nothing, so it believes the first valid version it is
// served, however old. And no computer notices being frozen on the newest
// version it saw, or a history pruned before it could follow the chain.
#pragma once

#include "core/CloudClient.h"
#include "core/CloudCrypto.h"
#include "core/IniStorage.h"
#include "core/Security.h"

#include <QDateTime>
#include <QObject>
#include <QString>

#include <functional>
#include <memory>

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
  bool HasKey() const { return !FDek.IsEmpty(); }
  // Teams (core/TeamSync): the account and its data key
  QString UserUuid() const { return FUser; }
  QByteArray DataKey() const { return FDek.Copy(); }
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
  // LicenseInactive once per lapse. Done (optional) gets whether it is active
  // (false also when not signed in or the server cannot be reached).
  void CheckLicense(const std::function<void(bool Active)> & Done = {});
  // Result of the last license check
  bool LicenseActive() const { return FLicenseActive; }
  QDateTime LicenseCheckedAt() const { return FLicenseCheckedAt; }

  // ---- Key (sync password) ----
  // Whether the cloud already has encrypted data (then UnlockKey, else CreateKey).
  // OldFormat: it was written by NixSCP 1.2.3 or older, so no password opens it
  // here. The way out is ReplaceOldCloud, which only the user may ask for: this
  // is what the server says, and it may lie.
  void ProbeCloud(const std::function<void(bool Ok, bool HasData, bool OldFormat, const QString & Error)> & Done);
  // First computer: a new data key. Done gets the formatted recovery key (when
  // requested) or the error.
  void CreateKey(const QString & Password, bool WithRecoveryKey, const std::function<void(bool, const QString &)> & Done);
  // The user confirmed starting over: a new data key, and this computer's settings
  // go on top of the old cloud data, which stays in the history unread. Refused
  // when the head is not of an old format (ProbeCloud).
  void ReplaceOldCloud(const QString & Password, bool WithRecoveryKey, const std::function<void(bool, const QString &)> & Done);
  // Another computer: unlocks the data key from the cloud with the sync
  // password or the recovery key
  void UnlockKey(const QString & Secret, bool IsRecoveryKey, const std::function<void(bool, const QString &)> & Done);
  // The current password or recovery key opens the data key of this computer
  bool CheckSecret(const QString & Secret, bool IsRecoveryKey, QString * Error = nullptr) const;
  struct KeyChange
  {
    // Needed for every change: an unlocked session alone does not allow one
    QString Current;
    bool CurrentIsRecoveryKey = false;
    QString NewPassword;          // empty: the password stays
    bool NewRecoveryKey = false;  // a new recovery key; the old one stops working on new data
    // A new data key. Changing only a password or recovery key leaves the data
    // key, so the old secret and any old header still open it: this revokes
    // them. Everything is encrypted again, this computer's files and the newest
    // cloud version; other computers ask for the current password once. Older
    // versions kept in the cloud stay readable with the old key. Needs the cloud
    // to be reachable and a password to wrap the key (a recovery key cannot
    // stand in for one), and always replaces an existing recovery key.
    bool RotateDataKey = false;
  };
  // Done gets the new recovery key, formatted, if there is one, or the error.
  // Replacing the data key may stay undecided (the connection was lost): Done gets
  // an error saying so, and KeyChangeSettled tells the outcome later.
  void ChangeKey(const KeyChange & Change, const std::function<void(bool, const QString &)> & Done);
  // A recovery key that works and that the user did not confirm yet (a crash may
  // have come between the change and showing it): show it, then RecoveryKeySaved
  QString PendingRecoveryKey() const;
  void RecoveryKeySaved();
  // The request of PUT /v1/keys/me (nixscp-cloud docs/TEAMS.md) that moves the
  // private keys of Teams to a new data key: Keys is what GET /v1/keys/me
  // returned, the public keys stay, and the stored identity key signs the
  // change, because a session alone cannot replace them. Empty when they cannot
  // be opened. The one place that knows how user_keys is wrapped: everything
  // added to it under the data key has to be moved here as well.
  static QJsonObject RewrapUserKeys(const QString & User, const QJsonObject & Keys, const QByteArray & OldDek, const QByteArray & NewDek);
  // Signed out: stores the configuration and the state unencrypted again
  bool RemoveEncryption(QString * Error);

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
  // A replacement of the data key that was left undecided came to its end:
  // Message tells the user what it means, PendingRecoveryKey has the new recovery key
  void KeyChangeSettled(bool Replaced, const QString & Message);

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
  using StatusDataCallback = std::function<void(int Status, const QByteArray & Data, const QString & Error)>;
  using DoneCallback = std::function<void(bool Ok, const QString & Error)>;
  using SnapshotCallback = std::function<void(const CloudCrypto::SnapshotHeader & Header, const QByteArray & Plaintext)>;
  using UploadCallback = std::function<void(int Status, qint64 Version, const Head & Conflict, const QString & Error)>;
  // The newest snapshot of a slot this computer authenticated
  struct SlotMark
  {
    qint64 Version = 0;
    QString Hash;
  };
  // A replacement of the data key from before its upload to its end
  struct Rotation
  {
    SecureBytes NewKey;
    SecureBytes OldKey;
    CloudCrypto::SnapshotHeader Wrap;  // the wraps of the new key
    qint64 Base = 0;                   // the settings version it goes on top of
    QString RecoveryKey;               // formatted, empty when there is none
    QString Keys;                      // PUT /v1/keys/me for the new key (JSON), empty without team keys
  };

  Configuration * FConfig = nullptr;
  CloudClient * FClient;
  QString FStateFileName;
  IniFile FStateFile;
  TState FState = csDisabled;
  QString FLastError;
  QDateTime FLastSync;
  QString FEmail;
  QString FUser;
  SecureBytes FDek;
  CloudCrypto::SnapshotHeader FWrap;  // KDF parameters and wrapped DEK, copied into every header
  bool FWrapDirty = false;            // new sync password not uploaded yet
  qint64 FSettingsBase = 0;
  QString FSettingsHash;
  qint64 FSessionBase = 0;
  SlotMark FSettingsMark;
  SlotMark FSessionMark;
  // The journal (nixscp-cloud-keys.ini)
  std::unique_ptr<Rotation> FRotation;
  QString FPendingKeys;               // PUT /v1/keys/me for a new data key, until the server took it (JSON)
  SecureBytes FRetiredKeys;           // earlier data keys, one after the other, until the team keys left them
  QString FRecoveryKey;               // until the user confirmed it (RecoveryKeySaved)
  bool FKeysBusy = false;             // a request about them is on its way
  // Who waits for a replacement of the data key that went silent
  std::function<void(bool, const QString &)> FRotationDone;
  QString FSavedToken;                // the refresh token in the state file
  bool FStateUnreadable = false;      // at the start the state did not open with the key: it may be replaced
  bool FRewriteFailed = false;        // the configuration did not reach the disk encrypted
  bool FSigningOut = false;
  // Raised by a sign-out: requests of before it no longer count
  int FGeneration = 0;
  int FSyncGeneration = 0;
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
  bool FLicenseActive = false;
  QDateTime FLicenseCheckedAt;
  QTimer * FDebounce;
  QTimer * FPeriodic;

  void LoadState();
  // False when the state did not reach the disk
  bool SaveState();
  // Only what belongs to the signed-in account is read; what another NixSCP on
  // this computer left there replaces what is held here
  void LoadJournal();
  // False when it did not reach the disk. An empty journal is no file.
  bool SaveJournal();
  QString JournalFileName() const;
  // Takes over what another NixSCP on this computer saved; false when the state
  // file must not be overwritten (locked, or sealed with a newer key)
  bool MergeStateFile();
  QString NewerStateToken();
  void ProtectConfiguration();
  // The only way to another data key: the local files follow it, and an old key
  // stays until the team keys are known to have left it. False when the
  // configuration cannot be written: nothing changed then.
  bool AdoptKey(const QByteArray & Dek, const CloudCrypto::SnapshotHeader & Wrap);
  bool RewriteConfiguration();
  bool KeyMatchesVault() const;
  void SetState(TState State, const QString & Error = QString());
  bool Ready() const;
  void UpdateReadyState();
  void ResetSyncState();
  QString AutoWorkspacePrefix() const;
  QByteArray LocalSettings() const;
  CloudCrypto::SnapshotHeader NewHeader(const QString & Slot, qint64 Version) const;
  CloudCrypto::SnapshotHeader NewHeader(const CloudCrypto::SnapshotHeader & Wrap, const QString & Slot, qint64 Version) const;
  void AdoptWrap(const CloudCrypto::SnapshotHeader & Header);
  // The snapshot (already authenticated) continues what this computer saw, as
  // far as its header tells; PrevVersion above the mark needs FollowChain
  static bool Continues(const SlotMark & Mark, const CloudCrypto::SnapshotHeader & Header, const QString & Hash);
  static void NoteSeen(SlotMark & Mark, qint64 Version, const QString & Hash);
  // Checks a settings snapshot against FSettingsMark, walking through the
  // versions this computer did not see. Only hashes are compared, so a version
  // under an older data key counts too; a version no longer kept ends the walk.
  void VerifyChain(const CloudCrypto::SnapshotHeader & Header, const QString & Hash, const DoneCallback & Done);
  void FollowChain(qint64 Version, const QString & Hash, const DoneCallback & Done);
  // Authenticates and decrypts a downloaded snapshot of Slot at Version
  bool Decode(const QByteArray & Data, const QString & Slot, qint64 Version, CloudCrypto::SnapshotHeader & Header,
    QByteArray & Plaintext, QString & Error) const;

  void ScheduleSync();
  void TimedSync();
  void FinishSync(TState State, const QString & Error = QString());
  void RunSettingsSync(int Attempt);
  void FirstSync(const Head & Remote, const QByteArray & Local, const QString & LocalHash);
  // Settings changed here and in the cloud: identical changes are no conflict
  void CheckConflict(const Head & Remote, const QByteArray & Local, const QString & LocalHash);
  void AskConflict(const Head & Remote);
  void PullSettings(const Head & Remote);
  // Downloads, authenticates and checks the settings head, then adopts its
  // wraps; anything else ends the sync (HandleUndecryptable, FinishSync)
  void OpenHead(const Head & Remote, const SnapshotCallback & Done);
  void Unauthorized(const QString & Error);
  void PushSettings(qint64 Base, const QByteArray & Plaintext, const QString & LocalHash, int Attempt);
  void ApplySettings(const QByteArray & Plaintext, qint64 Version, const QString & Device, qint64 PushOnTop = 0);
  void NoteNoLicense();
  // Unwraps the data key of a snapshot with the sync password (or recovery key)
  // and proves it by decrypting the snapshot
  bool TryUnlock(const QByteArray & Data, const QString & Slot, qint64 Version, const QString & Secret, bool IsRecoveryKey,
    QByteArray & Dek, CloudCrypto::SnapshotHeader & Header, QString & Problem) const;
  // The settings head does not decrypt: replaced by another computer when it
  // proves that (the current password is needed then); a foreign snapshot when
  // this key still decrypts an older version (it is then replaced); otherwise a
  // stale key
  void HandleUndecryptable(const Head & Remote, const QByteArray & Data);
  void RepairForeignHead(const Head & Remote, const QString & Device, qint64 Version, const QByteArray & Data);
  void KeyReplaced(const QString & Message);
  // Failed: the search could not finish (the server cannot be reached), which
  // says nothing about the key
  enum TFound { fdNotFound, fdFound, fdFailed };
  using FoundCallback = std::function<void(TFound Result, qint64 Version, const QByteArray & Data)>;
  // Newest snapshot of Slot below Below (0 = any) that passes Accept
  void FindSnapshot(const QString & Slot, qint64 Below, int MaxTries,
    const std::function<bool(qint64 Version, const QByteArray & Data)> & Accept, const FoundCallback & Done);
  void Backup(const IniFile & Local) const;
  static bool HasStoredSites(const IniFile & File);
  void RotateDataKey(const QByteArray & Dek, const CloudCrypto::SnapshotHeader & Wrap, const QString & Result,
    const std::function<void(bool, const QString &)> & Done);
  // The outcome of FRotation, from the head: the sync goes on when it is known
  void SettleRotation(const Head & Remote, int Attempt);
  enum TRotationVerdict { rvUnknown, rvLanded, rvRefused };
  TRotationVerdict JudgeRotation(const QByteArray & Data, qint64 Version) const;
  void RotationJudged(TRotationVerdict Verdict, int Attempt);
  // The new key is the account's: this computer follows. Starts over safely at
  // any point. False when the files could not be written; retried by the next sync.
  bool SwitchToNewKey();
  // Replaced for sure: what comes after, and whoever waits is told
  void RotationLanded();
  // Not replaced, and it never will be
  void RotationRefused(const QString & Why);
  void KeyChangeTold(bool Replaced, const QString & Message);
  // The sync ended with the outcome still unknown
  void TellRotationWaiting(const QString & Error);
  void RetireKey(const SecureBytes & Key);
  // The private keys of Teams opened with this key or an earlier one
  QJsonObject RewrapWithAnyKey(const QJsonObject & Keys, const QByteArray & NewDek) const;
  void CreateKeyOver(const QString & Password, bool WithRecoveryKey, qint64 SettingsBase, qint64 SessionBase,
    const std::function<void(bool, const QString &)> & Done);
  void UploadPendingKeys();
  // The team keys of the server are looked at with the earlier keys (FRetiredKeys)
  void MoveRetiredKeys();

  void GetHead(const QString & Slot, const HeadCallback & Done);
  void Download(const QString & Slot, qint64 Version, const StatusDataCallback & Done);
  void DownloadRaw(const QString & Slot, qint64 Version, const DataCallback & Done);
  // BeforeCommit: the snapshot is stored, and from here on the outcome can be unknown
  void Upload(const QString & Slot, qint64 Base, const QByteArray & Snapshot, const UploadCallback & Done,
    const std::function<void()> & BeforeCommit = {});
  void PushSessionAttempt(const QByteArray & Payload, const QDateTime & SavedAt, int Attempt, const DoneCallback & Done);
  QString SessionCacheFileName() const;
  void WriteSessionCache(const QByteArray & Payload) const;
  QByteArray ReadSessionCache() const;
};
