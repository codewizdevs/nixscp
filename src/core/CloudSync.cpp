#include "core/CloudSync.h"
#include "core/Platform.h"

#ifdef Q_OS_ANDROID
#include <QJniObject>
#endif
#include "core/ConfigVault.h"
#include "core/Security.h"

#include "core/CloudSettings.h"
#include "core/Configuration.h"
#include "core/Workspaces.h"

#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSysInfo>
#include <QTimer>
#include <QUrlQuery>

#include <algorithm>
#include <memory>

static constexpr int DebounceMs = 10 * 1000;
static constexpr int PeriodicMs = 5 * 60 * 1000;
static constexpr int MaxBackups = 10;
static constexpr int MaxSyncAttempts = 3;
static const QString SettingsSlot = QStringLiteral("settings");
static const QString SessionSlot = QStringLiteral("session");

static QString RolledBackText(qint64 Cloud, qint64 Seen)
{
  return QStringLiteral("The NixSCP cloud has older data (v%1) than this computer already saw (v%2). "
    "Syncing is stopped to protect your settings; please contact support.").arg(Cloud).arg(Seen);
}

static QString ForkedText()
{
  return QStringLiteral("The NixSCP cloud shows a history that does not continue what this computer saw. "
    "Syncing is stopped to protect your settings; please contact support.");
}

static QString NotSwitchedText()
{
  return QStringLiteral("The cloud has the new data key, but this computer's files could not be written. "
    "NixSCP tries again at the next sync. Is the disk full?");
}

static QString CalledOffText()
{
  return QStringLiteral("Replacing the data key did not get through to the cloud, so it was not replaced: "
    "your password and recovery key stay as they were. Replace it again if you still want to.");
}

// What a request started before a sign-out gets: it no longer counts
static CloudClient::Reply SignedOutReply()
{
  CloudClient::Reply Reply;
  Reply.Error = QStringLiteral("You are signed out of the NixSCP cloud.");
  return Reply;
}

CloudSync & CloudSync::Instance()
{
  static CloudSync Instance;
  return Instance;
}

CloudSync::CloudSync() :
  FClient(new CloudClient(this)),
  FDebounce(new QTimer(this)),
  FPeriodic(new QTimer(this))
{
  FDebounce->setSingleShot(true);
  FDebounce->setInterval(DebounceMs);
  connect(FDebounce, &QTimer::timeout, this, &CloudSync::TimedSync);
  FPeriodic->setInterval(PeriodicMs);
  connect(FPeriodic, &QTimer::timeout, this, &CloudSync::TimedSync);
  connect(FClient, &CloudClient::TokensChanged, this, &CloudSync::SaveState);
  FClient->SetTokenSource([this]() { return NewerStateToken(); });
  connect(FClient, &CloudClient::SignedOut, this, [this](const QString & Reason)
  {
    FPeriodic->stop();
    FBusy = false;
    FSyncPending = false;
    FGeneration++;
    QString Message;
    if (Reason == QStringLiteral("password_changed"))
      Message = QStringLiteral("This computer was signed out of the NixSCP cloud because your account password was changed.");
    else if (Reason == QStringLiteral("password_reset"))
      Message = QStringLiteral("This computer was signed out of the NixSCP cloud because your account password was reset.");
    else if (Reason == QStringLiteral("removed"))
      Message = QStringLiteral("This computer was removed from your NixSCP account on the website.");
    else if (Reason == QStringLiteral("token_reused"))
      Message = QStringLiteral("This computer was signed out of the NixSCP cloud for security: its sign-in was used from somewhere else.");
    else
      Message = QStringLiteral("This computer was signed out of the NixSCP cloud.");
    Message += QStringLiteral(" Sign in again to continue syncing; your sync password is not needed again.");
    // The data key stays: signing in to the same account resumes at once
    SetState(csSignedOut, Message);
    SaveState();
    emit SignedOutRemotely(Message);
  });
}

QString CloudSync::DeviceName()
{
#ifdef Q_OS_ANDROID
  // phones have no host name ("localhost"): the maker and model
  QString Maker = QJniObject::getStaticObjectField<jstring>("android/os/Build", "MANUFACTURER").toString();
  QString Model = QJniObject::getStaticObjectField<jstring>("android/os/Build", "MODEL").toString();
  QString Phone = Model.startsWith(Maker, Qt::CaseInsensitive) ? Model : (Maker + QLatin1Char(' ') + Model);
  return QStringLiteral("%1 (Android %2)").arg(Phone.trimmed(), QSysInfo::productVersion()).left(100);
#endif
  QString Name = QSysInfo::machineHostName();
  QString Os = QSysInfo::prettyProductName();
  if (!Os.isEmpty())
  {
    Name += QStringLiteral(" (%1)").arg(Os);
  }
  return Name.left(100);
}

void CloudSync::Initialize(Configuration & Config)
{
  FConfig = &Config;
  if (Config.IsNulStorage() || Config.IniFileName().isEmpty())
  {
    SetState(csDisabled);
    return;
  }
  FStateFileName = QFileInfo(Config.IniFileName()).absolutePath() + QStringLiteral("/nixscp-cloud.ini");
  // Available now; LoadState may restore a postponed conflict (csPaused)
  FState = csSignedOut;
  LoadState();
  LoadJournal();
  // A plain file while the key is known was not written by NixSCP, or a crash
  // came between the configuration and the state: it does not stay
  if (ConfigVault::Enabled() && !ConfigVault::IsSealedFile(FStateFileName))
  {
    SaveState();
  }
  // The configuration opened with the new key: a crash came after the cloud took
  // it, and the rest of this computer follows before anything else happens
  if (FRotation && FDek.Equals(FRotation->NewKey.View()))
  {
    SwitchToNewKey();
  }
  // Newer wraps in the configuration than in the state: a crash came between the
  // two after a new password, and the "not uploaded yet" did not reach the disk
  if (HasKey() && (ConfigVault::User() == FUser) && (ConfigVault::Wrap().WrapEpoch > FWrap.WrapEpoch))
  {
    FWrapDirty = true;
  }
  connect(&Config, &Configuration::Saved, this, &CloudSync::ScheduleSync);
  connect(&Config, &Configuration::StoredSessionsChanged, this, &CloudSync::ScheduleSync);
  UpdateReadyState();
  if (Ready())
  {
    FPeriodic->start();
    QTimer::singleShot(0, this, &CloudSync::TimedSync);
  }
}

//---------------------------------------------------------------------------
// State file (nixscp-cloud.ini)

void CloudSync::LoadState()
{
  QFile File(FStateFileName);
  QByteArray Data = File.open(QIODevice::ReadOnly) ? File.readAll() : QByteArray();
  File.close();
  bool Sealed = ConfigVault::LooksSealed(Data);
  FStateFile.Clear();
  QByteArray Plaintext;
  if (!Sealed)
  {
    FStateFile.LoadFromData(Data);
  }
  else if (ConfigVault::OpenFile(FStateFileName, Data, Plaintext))
  {
    FStateFile.LoadFromData(Plaintext);
  }
  else
  {
    // Damaged: this computer signs in again. Without a key it stays as it is.
    qWarning("NixSCP cloud: the sync state cannot be opened");
    FStateUnreadable = ConfigVault::Enabled();
    Sealed = false;
  }
  auto Read = [this](const char * Section, const char * Key, const QString & Default = QString())
  {
    return FStateFile.ReadRaw(QString::fromLatin1(Section), QString::fromLatin1(Key), Default);
  };
  FRememberKey = (Read("Options", "RememberKey", QStringLiteral("1")) == QStringLiteral("1"));
  FSyncSessions = (Read("Options", "SyncSessions", QStringLiteral("1")) == QStringLiteral("1"));
  FRestoreSessions = (Read("Options", "RestoreSessions", QStringLiteral("1")) == QStringLiteral("1"));
  // $NIXSCP_CLOUD_URL wins (testing); else the server this computer signed in to
  QString Url = qEnvironmentVariable("NIXSCP_CLOUD_URL");
  FClient->SetBaseUrl(!Url.isEmpty() ? Url : (Sealed ? Read("Account", "BaseUrl", CloudClient::DefaultBaseUrl()) : CloudClient::DefaultBaseUrl()));
  // Whoever can write to this folder can write a plain file: the options above
  // are all it is trusted for. Everything below is authenticated by the data key.
  if (!Sealed)
  {
    return;
  }
  FSavedToken = Read("Account", "RefreshToken");
  FClient->SetRefreshToken(FSavedToken);
  FEmail = Read("Account", "Email");
  FUser = Read("Account", "User");
  // Unlocked at startup with the NixSCP password (the same key)
  if (!FUser.isEmpty() && (ConfigVault::User() == FUser) && (ConfigVault::SecureKey().Size() == CloudCrypto::KeyLength))
  {
    FDek = ConfigVault::SecureKey();
  }
  FWrap = CloudCrypto::SnapshotHeader();
  FWrap.User = FUser;
  FWrap.Kdf.MemoryKib = Read("Key", "MemoryKib", QStringLiteral("65536")).toUInt();
  FWrap.Kdf.Iterations = Read("Key", "Iterations", QStringLiteral("3")).toUInt();
  FWrap.Kdf.Lanes = Read("Key", "Lanes", QStringLiteral("1")).toUInt();
  FWrap.Kdf.Salt = QByteArray::fromBase64(Read("Key", "Salt").toLatin1());
  FWrap.DekPassword = QByteArray::fromBase64(Read("Key", "DekPassword").toLatin1());
  FWrap.DekRecovery = QByteArray::fromBase64(Read("Key", "DekRecovery").toLatin1());
  FWrap.KeyEpoch = Read("Key", "KeyEpoch", QStringLiteral("0")).toLongLong();
  FWrap.WrapEpoch = Read("Key", "WrapEpoch", QStringLiteral("0")).toLongLong();
  FWrap.Rotated = QByteArray::fromBase64(Read("Key", "Rotated").toLatin1());
  FWrapDirty = (Read("Key", "Dirty") == QStringLiteral("1"));
  FSettingsBase = Read("Settings", "BaseVersion", QStringLiteral("0")).toLongLong();
  FSettingsHash = Read("Settings", "BaseHash");
  FSettingsMark.Version = Read("Settings", "SeenVersion", QStringLiteral("0")).toLongLong();
  FSettingsMark.Hash = Read("Settings", "SeenHash");
  FSessionBase = Read("Session", "BaseVersion", QStringLiteral("0")).toLongLong();
  FSessionMark.Version = Read("Session", "SeenVersion", QStringLiteral("0")).toLongLong();
  FSessionMark.Hash = Read("Session", "SeenHash");
  FLastSync = QDateTime::fromString(Read("Settings", "LastSync"), Qt::ISODate);
  FLicenseNotified = (Read("Settings", "LicenseNotified") == QStringLiteral("1"));
  if (Read("Settings", "Paused") == QStringLiteral("1"))
  {
    FState = csPaused;
  }
}

bool CloudSync::KeyMatchesVault() const
{
  return !ConfigVault::Enabled() || ConfigVault::SecureKey().Equals(FDek.View());
}

bool CloudSync::RewriteConfiguration()
{
  FRewriteFailed = false;
  if (FConfig != nullptr)
  {
    if (FConfig->UseMasterPassword() && Security::IsMasterPasswordKnown())
    {
      FConfig->ClearMasterPassword();
    }
    FRewriteFailed = !FConfig->Rewrite();
  }
  return !FRewriteFailed;
}

// The local configuration is encrypted with the data key (NixSCP Pro): on
// whenever the key is known; rewritten when its password changed. A WinSCP
// master password is retired: the NixSCP password replaces it. A different key
// never gets here (AdoptKey): what does not match stops syncing instead.
void CloudSync::ProtectConfiguration()
{
  if ((FDek.Size() != CloudCrypto::KeyLength) || FWrap.DekPassword.isEmpty() || FUser.isEmpty())
  {
    return;
  }
  const SecureBytes & VaultKey = ConfigVault::SecureKey();
  bool SameKey = VaultKey.Equals(FDek.View());
  if (!SameKey && !VaultKey.IsEmpty())
  {
    return;
  }
  CloudCrypto::SnapshotHeader Current = ConfigVault::Wrap();
  // Another NixSCP on this computer changed the password: its wraps reached the
  // vault through a file it wrote, and the older ones held here must not undo that
  if (SameKey && (Current.User == FUser) && (Current.WrapEpoch > FWrap.WrapEpoch))
  {
    FWrap.Kdf = Current.Kdf;
    FWrap.DekPassword = Current.DekPassword;
    FWrap.DekRecovery = Current.DekRecovery;
    FWrap.WrapEpoch = Current.WrapEpoch;
  }
  CloudCrypto::SnapshotHeader Wrap = FWrap;
  Wrap.User = FUser;
  bool Same = ConfigVault::Enabled() && SameKey && (Current.DekPassword == Wrap.DekPassword) &&
    (Current.DekRecovery == Wrap.DekRecovery) && (Current.User == Wrap.User) && (Current.WrapEpoch == Wrap.WrapEpoch);
  if (!Same)
  {
    ConfigVault::SetKey(FDek.View(), Wrap);
    RewriteConfiguration();
    ConfigVault::StepReached("wraps-in-config");
  }
}

bool CloudSync::AdoptKey(const QByteArray & Dek, const CloudCrypto::SnapshotHeader & Wrap)
{
  SecureBytes OldKey = ConfigVault::SecureKey();
  QString OldUser = ConfigVault::User();
  CloudCrypto::SnapshotHeader NewWrap = Wrap;
  NewWrap.User = FUser;
  if (FUser.isEmpty() || NewWrap.DekPassword.isEmpty())
  {
    FDek.Assign(Dek);
    FWrap = NewWrap;
    return true;
  }
  // Before the configuration is written: the file on disk is merged with it, so
  // it has to open with the new key
  if (!ConfigVault::ReplaceKey(Dek, NewWrap))
  {
    return false;
  }
  FDek.Assign(Dek);
  FWrap = NewWrap;
  // The private keys of Teams may still be under the old key, when whoever
  // replaced it did not get to move them: only that key opens them
  if (!OldKey.IsEmpty() && !OldKey.Equals(Dek) && (OldUser == FUser))
  {
    RetireKey(OldKey);
    // What waits to go up follows the key
    if (!FPendingKeys.isEmpty())
    {
      QJsonObject Wraps = RewrapUserKeys(FUser, QJsonDocument::fromJson(FPendingKeys.toUtf8()).object(), OldKey.View(), Dek);
      FPendingKeys = Wraps.isEmpty() ? QString() : QString::fromUtf8(QJsonDocument(Wraps).toJson(QJsonDocument::Compact));
    }
  }
  // The account's key moved: a replacement waiting here has nothing left to decide
  if (FRotation && !FRotation->OldKey.Equals(Dek))
  {
    if (FRotation->NewKey.Equals(Dek))
    {
      FPendingKeys = FRotation->Keys;
      FRecoveryKey = FRotation->RecoveryKey;
    }
    FRotation.reset();
  }
  SaveJournal();
  RewriteConfiguration();
  if (SaveState())
  {
    ConfigVault::EndReplaceKey();
  }
  return true;
}

bool CloudSync::MergeStateFile()
{
  if (!ConfigVault::IsSealedFile(FStateFileName))
  {
    return true;
  }
  IniFile Disk;
  if (!ConfigVault::Enabled())
  {
    return false;
  }
  if (!Disk.Load(FStateFileName))
  {
    // Not left unreadable from the start: the key was replaced by another
    // NixSCP on this computer, whose state is newer than anything here
    return FStateUnreadable;
  }
  FStateUnreadable = false;
  auto Read = [&Disk](const char * Section, const char * Key) { return Disk.ReadRaw(QString::fromLatin1(Section), QString::fromLatin1(Key)); };
  // A refresh token works once: the one the other NixSCP received replaces the
  // one held here, unless this one was received later
  QString Token = Read("Account", "RefreshToken");
  if (!Token.isEmpty() && (Token != FSavedToken) && (FClient->RefreshToken() == FSavedToken))
  {
    FClient->SetRefreshToken(Token);
    FSavedToken = Token;
  }
  NoteSeen(FSettingsMark, Read("Settings", "SeenVersion").toLongLong(), Read("Settings", "SeenHash"));
  NoteSeen(FSessionMark, Read("Session", "SeenVersion").toLongLong(), Read("Session", "SeenHash"));
  return true;
}

QString CloudSync::NewerStateToken()
{
  IniFile Disk;
  if (!ConfigVault::Enabled() || !ConfigVault::IsSealedFile(FStateFileName) || !Disk.Load(FStateFileName))
  {
    return QString();
  }
  QString Token = Disk.ReadRaw(QStringLiteral("Account"), QStringLiteral("RefreshToken"));
  if (Token.isEmpty() || (Token == FSavedToken))
  {
    return QString();
  }
  FSavedToken = Token;
  return Token;
}

bool CloudSync::SaveState()
{
  if (FStateFileName.isEmpty())
  {
    return false;
  }
  // The configuration first: a sealed state refuses a plain configuration, so
  // one that did not reach the disk leaves the state as it is (a full disk would
  // read as a replaced file at the next start). The next save tries again.
  ProtectConfiguration();
  if (FRewriteFailed)
  {
    if (ConfigVault::Enabled() && !RewriteConfiguration())
    {
      return false;
    }
    FRewriteFailed = false;
  }
  if (!MergeStateFile())
  {
    return false;
  }
  IniFile & F = FStateFile;
  F.Clear();
  auto Write = [&F](const char * Section, const char * Key, const QString & Value)
  {
    F.WriteRaw(QString::fromLatin1(Section), QString::fromLatin1(Key), Value);
  };
  auto Bool = [](bool Value) { return Value ? QStringLiteral("1") : QStringLiteral("0"); };
  Write("Options", "RememberKey", Bool(FRememberKey));
  Write("Options", "SyncSessions", Bool(FSyncSessions));
  Write("Options", "RestoreSessions", Bool(FRestoreSessions));
  // The rest only sealed (IniFile::Save, role "state"). Without a key the
  // sign-in stays in memory: there is nothing yet to protect it with.
  if (ConfigVault::Enabled())
  {
    FSavedToken = FClient->RefreshToken();
    Write("Account", "BaseUrl", FClient->BaseUrl());
    Write("Account", "Email", FEmail);
    Write("Account", "User", FUser);
    Write("Account", "RefreshToken", FSavedToken);
    // The data key is never stored: the NixSCP password unlocks it at startup
    // (ConfigVault), and it encrypts the local configuration too
    Write("Key", "MemoryKib", QString::number(FWrap.Kdf.MemoryKib));
    Write("Key", "Iterations", QString::number(FWrap.Kdf.Iterations));
    Write("Key", "Lanes", QString::number(FWrap.Kdf.Lanes));
    Write("Key", "Salt", QString::fromLatin1(FWrap.Kdf.Salt.toBase64()));
    Write("Key", "DekPassword", QString::fromLatin1(FWrap.DekPassword.toBase64()));
    Write("Key", "DekRecovery", QString::fromLatin1(FWrap.DekRecovery.toBase64()));
    Write("Key", "KeyEpoch", QString::number(FWrap.KeyEpoch));
    Write("Key", "WrapEpoch", QString::number(FWrap.WrapEpoch));
    Write("Key", "Rotated", QString::fromLatin1(FWrap.Rotated.toBase64()));
    Write("Key", "Dirty", Bool(FWrapDirty));
    Write("Settings", "BaseVersion", QString::number(FSettingsBase));
    Write("Settings", "BaseHash", FSettingsHash);
    Write("Settings", "SeenVersion", QString::number(FSettingsMark.Version));
    Write("Settings", "SeenHash", FSettingsMark.Hash);
    Write("Settings", "LastSync", FLastSync.toUTC().toString(Qt::ISODate));
    Write("Settings", "Paused", Bool(FState == csPaused));
    Write("Settings", "LicenseNotified", Bool(FLicenseNotified));
    Write("Session", "BaseVersion", QString::number(FSessionBase));
    Write("Session", "SeenVersion", QString::number(FSessionMark.Version));
    Write("Session", "SeenHash", FSessionMark.Hash);
  }
  if (!F.Save(FStateFileName))
  {
    return false;
  }
  QFile::setPermissions(FStateFileName, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  return true;
}

QString CloudSync::JournalFileName() const
{
  return QFileInfo(FStateFileName).absolutePath() + QStringLiteral("/nixscp-cloud-keys.ini");
}

void CloudSync::LoadJournal()
{
  if (FStateFileName.isEmpty())
  {
    return;
  }
  QString Name = JournalFileName();
  IniFile File;
  bool Exists = QFile::exists(Name);
  // Sealed with a key this computer does not have now: what is held here stays
  if (Exists && (!ConfigVault::Enabled() || !File.Load(Name)))
  {
    return;
  }
  auto Read = [&File](const char * Section, const char * Key) { return File.ReadRaw(QString::fromLatin1(Section), QString::fromLatin1(Key)); };
  auto Bytes = [&Read](const char * Key) { return QByteArray::fromBase64(Read("Rotation", Key).toLatin1()); };
  FRotation.reset();
  FPendingKeys.clear();
  FRetiredKeys.Clear();
  FRecoveryKey.clear();
  if (!Exists || FUser.isEmpty() || (Read("Keys", "User") != FUser))
  {
    return;
  }
  FPendingKeys = Read("Keys", "PendingKeys");
  QByteArray Retired = QByteArray::fromBase64(Read("Keys", "Retired").toLatin1());
  FRetiredKeys.Assign(Retired);
  Security::WipeBytes(Retired);
  FRecoveryKey = Read("Keys", "RecoveryKey");
  QByteArray NewKey = Bytes("NewKey");
  QByteArray OldKey = Bytes("OldKey");
  if ((NewKey.size() == CloudCrypto::KeyLength) && (OldKey.size() == CloudCrypto::KeyLength))
  {
    FRotation = std::make_unique<Rotation>();
    FRotation->NewKey.Assign(NewKey);
    FRotation->OldKey.Assign(OldKey);
    CloudCrypto::SnapshotHeader & Wrap = FRotation->Wrap;
    Wrap.User = FUser;
    Wrap.Kdf.MemoryKib = Read("Rotation", "MemoryKib").toUInt();
    Wrap.Kdf.Iterations = Read("Rotation", "Iterations").toUInt();
    Wrap.Kdf.Lanes = Read("Rotation", "Lanes").toUInt();
    Wrap.Kdf.Salt = Bytes("Salt");
    Wrap.DekPassword = Bytes("DekPassword");
    Wrap.DekRecovery = Bytes("DekRecovery");
    Wrap.KeyEpoch = Read("Rotation", "KeyEpoch").toLongLong();
    Wrap.WrapEpoch = Read("Rotation", "WrapEpoch").toLongLong();
    Wrap.Rotated = Bytes("Rotated");
    FRotation->Base = Read("Rotation", "Base").toLongLong();
    FRotation->RecoveryKey = Read("Rotation", "RecoveryKey");
    FRotation->Keys = Read("Rotation", "Keys");
  }
  Security::WipeBytes(NewKey);
  Security::WipeBytes(OldKey);
}

bool CloudSync::SaveJournal()
{
  if (FStateFileName.isEmpty())
  {
    return false;
  }
  QString Name = JournalFileName();
  if (!FRotation && FPendingKeys.isEmpty() && FRetiredKeys.IsEmpty() && FRecoveryKey.isEmpty())
  {
    return !QFile::exists(Name) || QFile::remove(Name);
  }
  // Never plain: this is key material
  if (!ConfigVault::Enabled())
  {
    return false;
  }
  IniFile F;
  auto Write = [&F](const char * Section, const char * Key, const QString & Value)
  {
    F.WriteRaw(QString::fromLatin1(Section), QString::fromLatin1(Key), Value);
  };
  auto Bytes = [&Write](const char * Key, const QByteArray & Value) { Write("Rotation", Key, QString::fromLatin1(Value.toBase64())); };
  Write("Keys", "User", FUser);
  Write("Keys", "PendingKeys", FPendingKeys);
  Write("Keys", "Retired", QString::fromLatin1(FRetiredKeys.View().toBase64()));
  Write("Keys", "RecoveryKey", FRecoveryKey);
  if (FRotation)
  {
    const CloudCrypto::SnapshotHeader & Wrap = FRotation->Wrap;
    Bytes("NewKey", FRotation->NewKey.View());
    Bytes("OldKey", FRotation->OldKey.View());
    Write("Rotation", "MemoryKib", QString::number(Wrap.Kdf.MemoryKib));
    Write("Rotation", "Iterations", QString::number(Wrap.Kdf.Iterations));
    Write("Rotation", "Lanes", QString::number(Wrap.Kdf.Lanes));
    Bytes("Salt", Wrap.Kdf.Salt);
    Bytes("DekPassword", Wrap.DekPassword);
    Bytes("DekRecovery", Wrap.DekRecovery);
    Write("Rotation", "KeyEpoch", QString::number(Wrap.KeyEpoch));
    Write("Rotation", "WrapEpoch", QString::number(Wrap.WrapEpoch));
    Bytes("Rotated", Wrap.Rotated);
    Write("Rotation", "Base", QString::number(FRotation->Base));
    Write("Rotation", "RecoveryKey", FRotation->RecoveryKey);
    Write("Rotation", "Keys", FRotation->Keys);
  }
  return F.Save(Name);
}

void CloudSync::ResetSyncState()
{
  FDek.Clear();
  FWrap = CloudCrypto::SnapshotHeader();
  FWrapDirty = false;
  FSettingsBase = 0;
  FSettingsHash.clear();
  FSessionBase = 0;
  // Also the way out after a server was really restored from a backup: signing
  // out and in trusts it again, as a new computer does
  FSettingsMark = SlotMark();
  FSessionMark = SlotMark();
  // The file stays: the journal waits for this account to come back
  FRotation.reset();
  FPendingKeys.clear();
  FRetiredKeys.Clear();
  FRecoveryKey.clear();
  FLastSync = QDateTime();
}

void CloudSync::SetState(TState State, const QString & Error)
{
  FState = State;
  FLastError = Error;
  emit StateChanged();
}

bool CloudSync::Ready() const
{
  return (FConfig != nullptr) && (FState != csDisabled) && SignedIn() && HasKey() && !FSigningOut && KeyMatchesVault();
}

void CloudSync::UpdateReadyState()
{
  if (FState == csDisabled)
  {
    return;
  }
  if (!SignedIn())
  {
    // Keeps the reason of a sign-out by the server (SignedOutRemotely)
    SetState(csSignedOut, (FState == csSignedOut) ? FLastError : QString());
  }
  else if (!HasKey())
  {
    SetState(csNeedKey);
  }
  else if ((FState == csSignedOut) || (FState == csNeedKey))
  {
    SetState(csIdle);
  }
  else
  {
    emit StateChanged();
  }
}

QString CloudSync::StatusText() const
{
  switch (FState)
  {
    case csDisabled: return QStringLiteral("Cloud sync is not available without a configuration file.");
    case csSignedOut: return FLastError.isEmpty() ? QStringLiteral("Not signed in.") : FLastError;
    case csNeedKey: return FLastError.isEmpty() ? QStringLiteral("Enter your sync password to start syncing on this computer.") : FLastError;
    case csIdle:
      return FLastSync.isValid()
        ? QStringLiteral("Synced, last checked %1.").arg(QLocale().toString(FLastSync.toLocalTime(), QLocale::ShortFormat))
        : QStringLiteral("Ready to sync.");
    case csSyncing: return QStringLiteral("Syncing...");
    case csConflict: return QStringLiteral("Settings were changed both here and on another computer.");
    case csPaused: return QStringLiteral("Syncing is paused: settings were changed both here and on another computer. Use Sync now to decide.");
    case csNoLicense: return QStringLiteral("Uploading needs an active NixSCP Pro license. Downloading still works.");
    case csError: return FLastError.isEmpty() ? QStringLiteral("Syncing failed; it will be retried.") : FLastError;
  }
  return QString();
}

void CloudSync::SetSyncSessions(bool Value)
{
  FSyncSessions = Value;
  SaveState();
}

void CloudSync::SetRestoreSessions(bool Value)
{
  FRestoreSessions = Value;
  SaveState();
}

void CloudSync::SetRememberKey(bool Value)
{
  FRememberKey = Value;
  SaveState();
}

//---------------------------------------------------------------------------
// Helpers

QString CloudSync::AutoWorkspacePrefix() const
{
  return CloudSettings::AutoWorkspacePrefix(FConfig->AutoSaveWorkspace, Workspaces::WorkspaceName(*FConfig));
}

QByteArray CloudSync::LocalSettings() const
{
  return CloudSettings::Canonical(CloudSettings::Syncable(FConfig->ExportData(), AutoWorkspacePrefix()));
}

CloudCrypto::SnapshotHeader CloudSync::NewHeader(const QString & Slot, qint64 Version) const
{
  return NewHeader(FWrap, Slot, Version);
}

CloudCrypto::SnapshotHeader CloudSync::NewHeader(const CloudCrypto::SnapshotHeader & Wrap, const QString & Slot, qint64 Version) const
{
  CloudCrypto::SnapshotHeader Header = Wrap;
  Header.Slot = Slot;
  Header.User = FUser;
  Header.Version = Version;
  Header.SavedAt = QDateTime::currentDateTimeUtc();
  Header.Device = DeviceName();
  const SlotMark & Mark = (Slot == SessionSlot) ? FSessionMark : FSettingsMark;
  Header.PrevVersion = Mark.Version;
  Header.PrevHash = Mark.Hash;
  return Header;
}

void CloudSync::AdoptWrap(const CloudCrypto::SnapshotHeader & Header)
{
  // The settings head carries the current sync password (it may have been
  // changed on another computer). Only a head that was authenticated gets here.
  if (Header.Slot != SettingsSlot)
  {
    return;
  }
  // A local change not uploaded yet wins, and goes up above what the cloud has
  if (FWrapDirty)
  {
    FWrap.WrapEpoch = qMax(FWrap.WrapEpoch, Header.WrapEpoch + 1);
    return;
  }
  // Old wraps, from a computer that missed the change: never taken over, and
  // the current ones go back up with the next upload
  if (Header.WrapEpoch < FWrap.WrapEpoch)
  {
    FWrapDirty = true;
    return;
  }
  FWrap.Kdf = Header.Kdf;
  FWrap.DekPassword = Header.DekPassword;
  FWrap.DekRecovery = Header.DekRecovery;
  FWrap.WrapEpoch = Header.WrapEpoch;
  FWrap.KeyEpoch = Header.KeyEpoch;
  FWrap.Rotated = Header.Rotated;
}

bool CloudSync::Continues(const SlotMark & Mark, const CloudCrypto::SnapshotHeader & Header, const QString & Hash)
{
  if (Mark.Version == 0)
  {
    return true;   // nothing to compare with
  }
  if (Header.Version <= Mark.Version)
  {
    return (Header.Version == Mark.Version) && (Hash == Mark.Hash);
  }
  if (Header.PrevVersion == Mark.Version)
  {
    return Header.PrevHash == Mark.Hash;
  }
  return Header.PrevVersion > Mark.Version;
}

void CloudSync::NoteSeen(SlotMark & Mark, qint64 Version, const QString & Hash)
{
  if (Version > Mark.Version)
  {
    Mark.Version = Version;
    Mark.Hash = Hash;
  }
}

void CloudSync::VerifyChain(const CloudCrypto::SnapshotHeader & Header, const QString & Hash, const DoneCallback & Done)
{
  if (!Continues(FSettingsMark, Header, Hash))
  {
    Done(false, (Header.Version < FSettingsMark.Version) ? RolledBackText(Header.Version, FSettingsMark.Version) : ForkedText());
  }
  else if ((FSettingsMark.Version == 0) || (Header.Version == FSettingsMark.Version) || (Header.PrevVersion == FSettingsMark.Version))
  {
    Done(true, QString());
  }
  else
  {
    FollowChain(Header.PrevVersion, Header.PrevHash, Done);
  }
}

void CloudSync::FollowChain(qint64 Version, const QString & Hash, const DoneCallback & Done)
{
  Download(SettingsSlot, Version, [this, Version, Hash, Done](int Status, const QByteArray & Data, const QString & Error)
  {
    if (Status == 404)
    {
      // Beyond the versions the server keeps: nothing left to compare with
      qWarning("NixSCP cloud: settings v%lld is no longer kept, the history is checked up to there", static_cast<long long>(Version));
      Done(true, QString());
      return;
    }
    if (Status != 200)
    {
      Done(false, Error);
      return;
    }
    // Authenticated by the hash in the newer snapshot: no key is needed, so
    // this also works across a change of the data key
    CloudCrypto::SnapshotHeader Header;
    if ((CloudSettings::Hash(Data) != Hash) || !CloudCrypto::ReadHeader(Data, Header) || (Header.Version != Version) ||
        (Header.PrevVersion >= Version) || !Continues(FSettingsMark, Header, Hash))
    {
      Done(false, ForkedText());
    }
    else if (Header.PrevVersion == FSettingsMark.Version)
    {
      Done(true, QString());
    }
    else
    {
      FollowChain(Header.PrevVersion, Header.PrevHash, Done);
    }
  });
}

bool CloudSync::Decode(const QByteArray & Data, const QString & Slot, qint64 Version, CloudCrypto::SnapshotHeader & Header,
  QByteArray & Plaintext, QString & Error) const
{
  if (!CloudCrypto::DecodeSnapshot(Data, FDek.View(), Header, Plaintext, &Error))
  {
    return false;
  }
  // Authenticated fields: the server cannot swap users, slots or versions
  if ((Header.Slot != Slot) || (Header.User != FUser) || (Header.Version != Version))
  {
    Error = QStringLiteral("The NixSCP cloud returned a snapshot that does not belong here (%1 v%2 instead of %3 v%4).")
      .arg(Header.Slot).arg(Header.Version).arg(Slot).arg(Version);
    return false;
  }
  return true;
}

bool CloudSync::HasStoredSites(const IniFile & File)
{
  for (const QString & Section : File.Sections())
  {
    if (Section.startsWith(QStringLiteral("Sessions\\"), Qt::CaseInsensitive) &&
        (Section.compare(QStringLiteral("Sessions\\Default%20Settings"), Qt::CaseInsensitive) != 0))
    {
      return true;
    }
  }
  return false;
}

void CloudSync::Backup(const IniFile & Local) const
{
  QString Dir = QFileInfo(FStateFileName).absolutePath() + QStringLiteral("/backups");
  if (!QDir().mkpath(Dir))
  {
    return;
  }
  QString Name = Dir + QStringLiteral("/nixscp-%1.ini").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
  if (Local.Save(Name))
  {
    Platform::MakePrivate(Name);
  }
  QStringList Old = QDir(Dir).entryList({QStringLiteral("nixscp-*.ini")}, QDir::Files, QDir::Name);
  while (Old.size() > MaxBackups)
  {
    QFile::remove(Dir + QLatin1Char('/') + Old.takeFirst());
  }
}

CloudSync::TAction CloudSync::Decide(qint64 BaseVersion, const QString & BaseHash, qint64 HeadVersion, const QString & LocalHash)
{
  if (HeadVersion < BaseVersion)
  {
    return saRolledBack;
  }
  bool LocalChanged = BaseHash.isEmpty() || (LocalHash != BaseHash);
  bool RemoteChanged = (HeadVersion != BaseVersion);
  if (!LocalChanged && !RemoteChanged)
  {
    return saNone;
  }
  if (!LocalChanged)
  {
    return saPull;
  }
  if (!RemoteChanged)
  {
    return saPush;
  }
  return saConflict;
}

//---------------------------------------------------------------------------
// HTTP building blocks

void CloudSync::GetHead(const QString & Slot, const HeadCallback & Done)
{
  int Generation = FGeneration;
  FClient->Api("GET", QStringLiteral("/v1/sync/head?slot=%1").arg(Slot), [this, Generation, Done](const CloudClient::Reply & Reply)
  {
    if (Generation != FGeneration)
    {
      Done(false, Head(), SignedOutReply());
      return;
    }
    Head Result;
    if (Reply.Ok())
    {
      Result.Version = Reply.Json.value(QStringLiteral("version")).toInteger();
      Result.Device = Reply.Json.value(QStringLiteral("device")).toString();
      Result.UpdatedAt = QDateTime::fromString(Reply.Json.value(QStringLiteral("updated_at")).toString(), Qt::ISODateWithMs);
    }
    Done(Reply.Ok(), Result, Reply);
  });
}

void CloudSync::Download(const QString & Slot, qint64 Version, const StatusDataCallback & Done)
{
  int Generation = FGeneration;
  FClient->Api("GET", QStringLiteral("/v1/sync/download-url?slot=%1&version=%2").arg(Slot).arg(Version),
    [this, Generation, Done](const CloudClient::Reply & Reply)
  {
    if (Generation != FGeneration)
    {
      Done(0, QByteArray(), SignedOutReply().Error);
      return;
    }
    if (!Reply.Ok())
    {
      Done(Reply.Status, QByteArray(), Reply.Error);
      return;
    }
    QString Sha = Reply.Json.value(QStringLiteral("sha256")).toString();
    qint64 Size = Reply.Json.value(QStringLiteral("size")).toInteger();
    if ((Size <= 0) || (Size > CloudCrypto::MaxSnapshotLength))
    {
      Done(0, QByteArray(), QStringLiteral("The NixSCP cloud announced a snapshot of an impossible size."));
      return;
    }
    FClient->Transfer("GET", QUrl(Reply.Json.value(QStringLiteral("url")).toString()), QByteArray(), {},
      [this, Generation, Done, Sha](const CloudClient::Reply & Get)
    {
      if (Generation != FGeneration)
      {
        Done(0, QByteArray(), SignedOutReply().Error);
        return;
      }
      if (!Get.Ok())
      {
        Done(0, QByteArray(), Get.Error.isEmpty() ? QStringLiteral("Downloading from the NixSCP cloud failed (%1).").arg(Get.Status) : Get.Error);
        return;
      }
      if (QString::fromLatin1(QCryptographicHash::hash(Get.Data, QCryptographicHash::Sha256).toHex()) != Sha)
      {
        Done(0, QByteArray(), QStringLiteral("The downloaded snapshot is incomplete."));
        return;
      }
      Done(200, Get.Data, QString());
    }, Size);
  });
}

void CloudSync::DownloadRaw(const QString & Slot, qint64 Version, const DataCallback & Done)
{
  Download(Slot, Version, [Done](int Status, const QByteArray & Data, const QString & Error) { Done(Status == 200, Data, Error); });
}

void CloudSync::Upload(const QString & Slot, qint64 Base, const QByteArray & Snapshot, const UploadCallback & Done,
  const std::function<void()> & BeforeCommit)
{
  auto ParseHead = [](const QJsonObject & Json)
  {
    QJsonObject H = Json.value(QStringLiteral("head")).toObject();
    Head Result;
    Result.Version = H.value(QStringLiteral("version")).toInteger();
    Result.Device = H.value(QStringLiteral("device")).toString();
    Result.UpdatedAt = QDateTime::fromString(H.value(QStringLiteral("updated_at")).toString(), Qt::ISODateWithMs);
    return Result;
  };
  QJsonObject Request
  {
    {QStringLiteral("slot"), Slot},
    {QStringLiteral("base_version"), Base},
    {QStringLiteral("size"), Snapshot.size()},
  };
  int Generation = FGeneration;
  FClient->Api("POST", QStringLiteral("/v1/sync/upload-url"), Request,
    [this, Generation, Slot, Base, Snapshot, Done, BeforeCommit, ParseHead](const CloudClient::Reply & Reply)
  {
    if (Generation != FGeneration)
    {
      Done(0, 0, Head(), SignedOutReply().Error);
      return;
    }
    if (!Reply.Ok())
    {
      Done(Reply.Status, 0, (Reply.Status == 409) ? ParseHead(Reply.Json) : Head(), Reply.Error);
      return;
    }
    QString ObjectKey = Reply.Json.value(QStringLiteral("object_key")).toString();
    CloudClient::Headers PutHeaders{{"Content-Type", "application/octet-stream"}};
    // Part of the signature when the store supports it: the URL then cannot
    // overwrite the object once it exists
    if (Reply.Json.value(QStringLiteral("headers")).toObject().value(QStringLiteral("If-None-Match")).toString() == QStringLiteral("*"))
    {
      PutHeaders.append(qMakePair(QByteArray("If-None-Match"), QByteArray("*")));
    }
    FClient->Transfer("PUT", QUrl(Reply.Json.value(QStringLiteral("url")).toString()), Snapshot,
      PutHeaders, [this, Generation, Slot, Base, Snapshot, ObjectKey, Done, BeforeCommit, ParseHead](const CloudClient::Reply & Put)
    {
      if (Generation != FGeneration)
      {
        Done(0, 0, Head(), SignedOutReply().Error);
        return;
      }
      if (!Put.Ok())
      {
        Done(Put.Status ? Put.Status : 0, 0, Head(), QStringLiteral("Uploading to the NixSCP cloud failed (%1).").arg(Put.Status));
        return;
      }
      if (BeforeCommit)
      {
        BeforeCommit();
      }
      QJsonObject Commit
      {
        {QStringLiteral("slot"), Slot},
        {QStringLiteral("base_version"), Base},
        {QStringLiteral("object_key"), ObjectKey},
        {QStringLiteral("size"), Snapshot.size()},
        {QStringLiteral("sha256"), QString::fromLatin1(QCryptographicHash::hash(Snapshot, QCryptographicHash::Sha256).toHex())},
      };
      FClient->Api("POST", QStringLiteral("/v1/sync/commit"), Commit, [this, Generation, Done, ParseHead](const CloudClient::Reply & Result)
      {
        if (Generation != FGeneration)
        {
          Done(0, 0, Head(), SignedOutReply().Error);
          return;
        }
        Done(Result.Ok() ? 200 : Result.Status, Result.Json.value(QStringLiteral("version")).toInteger(),
          (Result.Status == 409) ? ParseHead(Result.Json) : Head(), Result.Error);
      });
    });
  });
}

//---------------------------------------------------------------------------
// Account

CloudSync::SignInRequest CloudSync::NewSignInRequest(quint16 LoopbackPort) const
{
  SignInRequest Request;
  Request.Verifier = QString::fromLatin1(CloudCrypto::RandomBytes(32).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
  Request.State = QString::fromLatin1(CloudCrypto::RandomBytes(16).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
  Request.RedirectUri = QStringLiteral("http://127.0.0.1:%1/callback").arg(LoopbackPort);
  QByteArray Challenge = QCryptographicHash::hash(Request.Verifier.toLatin1(), QCryptographicHash::Sha256)
    .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
  QUrlQuery Query;
  Query.addQueryItem(QStringLiteral("client_id"), QStringLiteral("nixscp-desktop"));
  Query.addQueryItem(QStringLiteral("redirect_uri"), Request.RedirectUri);
  Query.addQueryItem(QStringLiteral("code_challenge"), QString::fromLatin1(Challenge));
  Query.addQueryItem(QStringLiteral("code_challenge_method"), QStringLiteral("S256"));
  Query.addQueryItem(QStringLiteral("state"), Request.State);
  Query.addQueryItem(QStringLiteral("device_name"), DeviceName());
  QUrl Url(FClient->BaseUrl() + QStringLiteral("/authorize"));
  Url.setQuery(Query);
  Request.Url = Url.toString(QUrl::FullyEncoded);
  return Request;
}

void CloudSync::CompleteSignIn(const SignInRequest & Request, const QString & Code, const std::function<void(bool, const QString &)> & Done)
{
  FClient->ExchangeCode(Code, Request.Verifier, Request.RedirectUri, [this, Done](const CloudClient::Reply & Reply)
  {
    if (!Reply.Ok())
    {
      Done(false, Reply.Error);
      return;
    }
    FetchAccount([this, Done](bool Ok, const QJsonObject & Me)
    {
      QJsonObject User = Me.value(QStringLiteral("user")).toObject();
      QString Uuid = User.value(QStringLiteral("uuid")).toString();
      if (!Ok || Uuid.isEmpty())
      {
        // A sign-in without its account would go on to a key that belongs to
        // nobody: it does not stay half done
        FClient->SignOut([this, Done]()
        {
          UpdateReadyState();
          Done(false, QStringLiteral("Signed in, but the account could not be loaded. Try again."));
        });
        return;
      }
      if (Uuid != FUser)
      {
        // Another account than before: nothing of the old sync state applies
        ResetSyncState();
        FUser = Uuid;
        LoadJournal();
      }
      FEmail = User.value(QStringLiteral("email")).toString();
      if (FState == csPaused)
      {
        FState = csIdle;
      }
      SaveState();
      UpdateReadyState();
      if (Ready())
      {
        FPeriodic->start();
        SyncNow();
      }
      Done(true, QString());
    });
  });
}

void CloudSync::FetchAccount(const std::function<void(bool, const QJsonObject &)> & Done)
{
  FClient->Api("GET", QStringLiteral("/v1/me"), [Done](const CloudClient::Reply & Reply) { Done(Reply.Ok(), Reply.Json); });
}

void CloudSync::CheckLicense(const std::function<void(bool Active)> & Done)
{
  if (!SignedIn())
  {
    FLicenseActive = false;
    if (Done)
    {
      Done(false);
    }
    return;
  }
  FetchAccount([this, Done](bool Ok, const QJsonObject & Me)
  {
    if (!Ok || !SignedIn())
    {
      if (Done)
      {
        Done(Ok ? false : FLicenseActive);
      }
      return;
    }
    FLicenseActive = Me.value(QStringLiteral("license")).toObject().value(QStringLiteral("active")).toBool();
    FLicenseCheckedAt = QDateTime::currentDateTimeUtc();
    if (FLicenseActive)
    {
      if (FLicenseNotified)
      {
        FLicenseNotified = false;
        SaveState();
      }
    }
    else
    {
      NoteNoLicense();
    }
    if (Done)
    {
      Done(FLicenseActive);
    }
  });
}

void CloudSync::SignOut()
{
  FPeriodic->stop();
  FDebounce->stop();
  // A sync in flight must not finish into the state of an account that is gone
  FSigningOut = true;
  FGeneration++;
  FBusy = false;
  FSyncPending = false;
  FClient->SignOut([this]()
  {
    ResetSyncState();
    FEmail.clear();
    FUser.clear();
    FSigningOut = false;
    SetState(csSignedOut);
    SaveState();
    QFile::remove(SessionCacheFileName());
  });
}

bool CloudSync::RemoveEncryption(QString * Error)
{
  // Everything of the account goes, and the state before the configuration: a
  // sealed state refuses a plain configuration
  ResetSyncState();
  FEmail.clear();
  FUser.clear();
  ConfigVault::Disable();
  QFile::remove(FStateFileName);
  QFile::remove(JournalFileName());
  ConfigVault::EndReplaceKey();
  SaveState();
  if ((FConfig != nullptr) && !FConfig->Rewrite())
  {
    if (Error != nullptr)
    {
      *Error = FConfig->LastSaveError();
    }
    return false;
  }
  return true;
}

//---------------------------------------------------------------------------
// Key

void CloudSync::ProbeCloud(const std::function<void(bool, bool, bool, const QString &)> & Done)
{
  // UnlockKey starts from this head as well: when it is of an old format, no
  // password will ever open it
  auto Look = [this, Done](const QString & Slot, qint64 Version)
  {
    DownloadRaw(Slot, Version, [Done](bool Ok, const QByteArray & Data, const QString & Error)
    {
      Done(Ok, Ok, Ok && CloudCrypto::IsOldFormat(Data), Error);
    });
  };
  GetHead(SettingsSlot, [this, Done, Look](bool Ok, const Head & Settings, const CloudClient::Reply & Reply)
  {
    if (!Ok)
    {
      Done(false, false, false, Reply.Error);
      return;
    }
    if (Settings.Version > 0)
    {
      Look(SettingsSlot, Settings.Version);
      return;
    }
    GetHead(SessionSlot, [Done, Look](bool Ok2, const Head & Session, const CloudClient::Reply & Reply2)
    {
      if (Ok2 && (Session.Version > 0))
      {
        Look(SessionSlot, Session.Version);
        return;
      }
      Done(Ok2, false, false, Reply2.Error);
    });
  });
}

void CloudSync::CreateKey(const QString & Password, bool WithRecoveryKey, const std::function<void(bool, const QString &)> & Done)
{
  CreateKeyOver(Password, WithRecoveryKey, 0, 0, Done);
}

void CloudSync::ReplaceOldCloud(const QString & Password, bool WithRecoveryKey, const std::function<void(bool, const QString &)> & Done)
{
  // Looked at again: what the user agreed to is replacing data that cannot be read
  ProbeCloud([this, Password, WithRecoveryKey, Done](bool Ok, bool, bool OldFormat, const QString & Error)
  {
    if (!Ok || !OldFormat)
    {
      Done(false, Ok ? QStringLiteral("The data in the cloud is no longer of an old format, so nothing was replaced.") : Error);
      return;
    }
    GetHead(SettingsSlot, [this, Password, WithRecoveryKey, Done](bool Ok2, const Head & Settings, const CloudClient::Reply & Reply)
    {
      if (!Ok2)
      {
        Done(false, Reply.Error);
        return;
      }
      GetHead(SessionSlot, [this, Password, WithRecoveryKey, Done, Settings](bool Ok3, const Head & Session, const CloudClient::Reply & Reply2)
      {
        if (!Ok3)
        {
          Done(false, Reply2.Error);
          return;
        }
        CreateKeyOver(Password, WithRecoveryKey, Settings.Version, Session.Version, Done);
      });
    });
  });
}

void CloudSync::CreateKeyOver(const QString & Password, bool WithRecoveryKey, qint64 SettingsBase, qint64 SessionBase,
  const std::function<void(bool, const QString &)> & Done)
{
  QString Problem = CloudCrypto::PasswordProblem(Password, FEmail);
  if (!Problem.isEmpty())
  {
    Done(false, Problem);
    return;
  }
  QByteArray Dek = CloudCrypto::RandomBytes(CloudCrypto::KeyLength);
  QByteArray RecoveryKey = WithRecoveryKey ? CloudCrypto::NewRecoveryKey() : QByteArray();
  CloudCrypto::SnapshotHeader Wrap;
  Wrap.User = FUser;
  Wrap.KeyEpoch = 1;
  Wrap.WrapEpoch = 1;
  QString Error;
  if (!CloudCrypto::WrapDek(Wrap, Dek, Password, RecoveryKey, &Error))
  {
    Security::WipeBytes(Dek);
    Security::WipeBytes(RecoveryKey);
    Done(false, Error);
    return;
  }
  QString Result = WithRecoveryKey ? CloudCrypto::FormatRecoveryKey(RecoveryKey) : QString();
  Security::WipeBytes(RecoveryKey);
  // With the key: from the first save on it waits to be confirmed
  FRecoveryKey = Result;
  bool Adopted = AdoptKey(Dek, Wrap);
  Security::WipeBytes(Dek);
  if (!Adopted)
  {
    FRecoveryKey.clear();
    Done(false, QStringLiteral("The configuration could not be written, so the key was not created. Is the disk full?"));
    return;
  }
  FWrapDirty = false;
  // Above the old data, when that is what the user asked to replace: the first
  // upload then goes on top of it without reading it
  FSettingsBase = SettingsBase;
  FSettingsHash.clear();
  FSessionBase = SessionBase;
  FSettingsMark = SlotMark();
  FSessionMark = SlotMark();
  SaveState();
  UpdateReadyState();
  FPeriodic->start();
  SyncNow();
  Done(true, Result);
}

bool CloudSync::TryUnlock(const QByteArray & Data, const QString & Slot, qint64 Version, const QString & Secret, bool IsRecoveryKey,
  QByteArray & Dek, CloudCrypto::SnapshotHeader & Header, QString & Problem) const
{
  if (!CloudCrypto::ReadHeader(Data, Header, &Problem))
  {
    return false;
  }
  if (IsRecoveryKey)
  {
    QByteArray RecoveryKey = CloudCrypto::ParseRecoveryKey(Secret);
    Dek = CloudCrypto::UnwrapDekWithRecoveryKey(Header, RecoveryKey);
    bool Valid = !RecoveryKey.isEmpty();
    Security::WipeBytes(RecoveryKey);
    if (Dek.isEmpty())
    {
      Problem = Valid ? QStringLiteral("The recovery key is not correct.") : QStringLiteral("This is not a valid recovery key.");
      return false;
    }
  }
  else
  {
    Dek = CloudCrypto::UnwrapDekWithPassword(Header, Secret, &Problem);
    if (Dek.isEmpty())
    {
      return false;
    }
  }
  // Prove the key on the snapshot itself before using it
  CloudCrypto::SnapshotHeader Checked;
  QByteArray Plaintext;
  bool Proved = CloudCrypto::DecodeSnapshot(Data, Dek, Checked, Plaintext, &Problem) &&
    (Checked.Slot == Slot) && (Checked.User == FUser) && (Checked.Version == Version);
  Security::WipeBytes(Plaintext);
  if (!Proved)
  {
    if (Problem.isEmpty())
    {
      Problem = QStringLiteral("The synced data does not belong to this account.");
    }
    Security::WipeBytes(Dek);
    return false;
  }
  return true;
}

void CloudSync::UnlockKey(const QString & Secret, bool IsRecoveryKey, const std::function<void(bool, const QString &)> & Done)
{
  auto Adopt = [this, Done](const QByteArray & Dek, const CloudCrypto::SnapshotHeader & Header)
  {
    CloudCrypto::SnapshotHeader Wrap;
    Wrap.Kdf = Header.Kdf;
    Wrap.DekPassword = Header.DekPassword;
    Wrap.DekRecovery = Header.DekRecovery;
    Wrap.KeyEpoch = Header.KeyEpoch;
    Wrap.WrapEpoch = Header.WrapEpoch;
    Wrap.Rotated = Header.Rotated;
    if (!AdoptKey(Dek, Wrap))
    {
      Done(false, QStringLiteral("The configuration could not be written with this key. Is the disk full?"));
      return;
    }
    FWrapDirty = false;
    // A computer that knows this account only lost the key, because it was
    // replaced elsewhere: it goes on from where it was, so unchanged settings
    // are no conflict. What it saw stays either way (high-water marks).
    if (FSettingsMark.Version == 0)
    {
      FSettingsBase = 0;
      FSettingsHash.clear();
    }
    if (FState == csPaused)
    {
      FState = csIdle;
    }
    SaveState();
    UpdateReadyState();
    FPeriodic->start();
    SyncNow();
    Done(true, QString());
  };
  auto Unlock = [this, Secret, IsRecoveryKey, Done, Adopt](const QString & Slot, qint64 Version)
  {
    DownloadRaw(Slot, Version, [this, Slot, Version, Secret, IsRecoveryKey, Done, Adopt](bool Ok, const QByteArray & Data, const QString & Error)
    {
      QByteArray Dek;
      CloudCrypto::SnapshotHeader Header;
      QString Problem = Error;
      if (Ok && TryUnlock(Data, Slot, Version, Secret, IsRecoveryKey, Dek, Header, Problem))
      {
        Adopt(Dek, Header);
        Security::WipeBytes(Dek);
        return;
      }
      // The newest snapshot may have been uploaded without the sync password
      // (a computer signed in to the account that does not know it): an older
      // version unlocks with the right password too
      auto Found = std::make_shared<QPair<QByteArray, CloudCrypto::SnapshotHeader>>();
      FindSnapshot(Slot, Version, 5, [this, Slot, Secret, IsRecoveryKey, Found](qint64 Older, const QByteArray & OlderData)
      {
        QString Ignored;
        return TryUnlock(OlderData, Slot, Older, Secret, IsRecoveryKey, Found->first, Found->second, Ignored);
      }, [Done, Adopt, Found, Problem](TFound Result, qint64, const QByteArray &)
      {
        if (Result == fdFound)
        {
          Adopt(Found->first, Found->second);
        }
        else
        {
          Done(false, Problem);
        }
        Security::WipeBytes(Found->first);
      });
    });
  };
  GetHead(SettingsSlot, [this, Done, Unlock](bool Ok, const Head & Settings, const CloudClient::Reply & Reply)
  {
    if (!Ok)
    {
      Done(false, Reply.Error);
      return;
    }
    if (Settings.Version > 0)
    {
      Unlock(SettingsSlot, Settings.Version);
      return;
    }
    GetHead(SessionSlot, [Unlock, Done](bool Ok2, const Head & Session, const CloudClient::Reply & Reply2)
    {
      if (!Ok2 || (Session.Version == 0))
      {
        Done(false, Ok2 ? QStringLiteral("There is no synced data in the cloud yet.") : Reply2.Error);
        return;
      }
      Unlock(SessionSlot, Session.Version);
    });
  });
}

bool CloudSync::CheckSecret(const QString & Secret, bool IsRecoveryKey, QString * Error) const
{
  QByteArray Dek;
  if (IsRecoveryKey)
  {
    QByteArray Recovery = CloudCrypto::ParseRecoveryKey(Secret);
    Dek = CloudCrypto::UnwrapDekWithRecoveryKey(FWrap, Recovery);
    Security::WipeBytes(Recovery);
  }
  else
  {
    Dek = CloudCrypto::UnwrapDekWithPassword(FWrap, Secret);
  }
  bool Ok = !Dek.isEmpty() && FDek.Equals(Dek);
  Security::WipeBytes(Dek);
  if (!Ok && (Error != nullptr))
  {
    *Error = IsRecoveryKey ? QStringLiteral("The recovery key is not correct.") : QStringLiteral("The current password is not correct.");
  }
  return Ok;
}

QJsonObject CloudSync::RewrapUserKeys(const QString & User, const QJsonObject & Keys, const QByteArray & OldDek, const QByteArray & NewDek)
{
  auto Text = [&Keys](const char * Name) { return Keys.value(QString::fromLatin1(Name)).toString(); };
  QString PublicKey = Text("public_key");
  QString IdentityKey = Text("identity_key");
  QByteArray Private;
  if (!CloudCrypto::Open(OldDek, QByteArray::fromBase64(Text("wrapped_private_key").toLatin1()), QByteArray(), Private))
  {
    return QJsonObject();
  }
  // The private halves are stored together: the identity half is the one that
  // gives the identity key
  QByteArray Identity;
  QByteArray IdentityPublic = QByteArray::fromBase64(IdentityKey.toLatin1());
  for (qsizetype Offset = 0; Identity.isEmpty() && (Offset + CloudCrypto::KeyLength <= Private.size()); Offset += CloudCrypto::KeyLength)
  {
    QByteArray Half = Private.mid(Offset, CloudCrypto::KeyLength);
    if (CloudCrypto::Ed25519Public(Half) == IdentityPublic)
    {
      Identity = std::exchange(Half, QByteArray());
    }
    Security::WipeBytes(Half);
  }
  QString Wrapped = QString::fromLatin1(CloudCrypto::Seal(NewDek, Private, QByteArray()).toBase64());
  Security::WipeBytes(Private);
  if (Identity.isEmpty())
  {
    return QJsonObject();
  }
  QByteArray Signed = (PublicKey + QLatin1Char('|') + IdentityKey + QLatin1Char('|') + Wrapped).toUtf8();
  QByteArray Message = "NXT2-KEYS-REPLACE|" + User.toUtf8() + '|' + CloudSettings::Hash(Signed).toLatin1();
  QByteArray Signature = CloudCrypto::Ed25519Sign(Identity, Message);
  Security::WipeBytes(Identity);
  return QJsonObject
  {
    {QStringLiteral("public_key"), PublicKey},
    {QStringLiteral("identity_key"), IdentityKey},
    {QStringLiteral("key_signature"), Text("key_signature")},
    {QStringLiteral("wrapped_private_key"), Wrapped},
    {QStringLiteral("replace_signature"), QString::fromLatin1(Signature.toBase64())},
  };
}

void CloudSync::ChangeKey(const KeyChange & Change, const std::function<void(bool, const QString &)> & Done)
{
  if (!HasKey())
  {
    Done(false, QStringLiteral("Enter the current sync password first."));
    return;
  }
  if (FRotation)
  {
    Done(false, QStringLiteral("Replacing the data key has not finished yet: NixSCP does that as soon as the cloud can be reached. "
      "Try again after that."));
    return;
  }
  QString Error;
  if (!CheckSecret(Change.Current, Change.CurrentIsRecoveryKey, &Error))
  {
    Done(false, Error);
    return;
  }
  if (!Change.NewPassword.isEmpty())
  {
    Error = CloudCrypto::PasswordProblem(Change.NewPassword, FEmail);
    if (!Error.isEmpty())
    {
      Done(false, Error);
      return;
    }
  }
  bool Rotate = Change.RotateDataKey;
  QString Password = !Change.NewPassword.isEmpty() ? Change.NewPassword : (Change.CurrentIsRecoveryKey ? QString() : Change.Current);
  if (Rotate && Password.isEmpty())
  {
    Done(false, QStringLiteral("A new data key needs a password: choose a new one."));
    return;
  }
  // The old recovery key cannot open a new data key
  bool NewRecovery = Change.NewRecoveryKey || (Rotate && !FWrap.DekRecovery.isEmpty());
  QByteArray RecoveryKey = NewRecovery ? CloudCrypto::NewRecoveryKey() : QByteArray();
  QByteArray Dek = Rotate ? CloudCrypto::RandomBytes(CloudCrypto::KeyLength) : FDek.Copy();
  CloudCrypto::SnapshotHeader Wrap;
  Wrap.User = FUser;
  Wrap.KeyEpoch = FWrap.KeyEpoch + (Rotate ? 1 : 0);
  Wrap.WrapEpoch = FWrap.WrapEpoch + 1;
  Wrap.Rotated = Rotate ? CloudCrypto::RotationProof(FDek.View(), FUser, Wrap.KeyEpoch) : FWrap.Rotated;
  bool Ok = true;
  if (!Rotate && Change.NewPassword.isEmpty())
  {
    Wrap.Kdf = FWrap.Kdf;
    Wrap.DekPassword = FWrap.DekPassword;
    Wrap.DekRecovery = FWrap.DekRecovery;
    if (NewRecovery)
    {
      CloudCrypto::WrapDekForRecovery(Wrap, Dek, RecoveryKey);
    }
  }
  else
  {
    Ok = CloudCrypto::WrapDek(Wrap, Dek, Password, RecoveryKey, &Error);
    if (!NewRecovery)
    {
      // The recovery key wraps the same data key: it keeps working
      Wrap.DekRecovery = FWrap.DekRecovery;
    }
  }
  QString Result = NewRecovery ? CloudCrypto::FormatRecoveryKey(RecoveryKey) : QString();
  Security::WipeBytes(RecoveryKey);
  if (!Ok)
  {
    Security::WipeBytes(Dek);
    Done(false, Error);
  }
  else if (Rotate)
  {
    RotateDataKey(Dek, Wrap, Result, Done);
    Security::WipeBytes(Dek);
  }
  else
  {
    Security::WipeBytes(Dek);
    // Before the wraps: a recovery key they do not know is never shown
    // (PendingRecoveryKey), one they know must not get lost
    QString OldRecovery = std::exchange(FRecoveryKey, Result);
    SaveJournal();
    CloudCrypto::SnapshotHeader OldWrap = std::exchange(FWrap, Wrap);
    bool OldDirty = std::exchange(FWrapDirty, true);
    if (!SaveState())
    {
      // Not on this computer's disk: then not in the cloud either, and it did not happen
      FRecoveryKey = OldRecovery;
      SaveJournal();
      FWrap = OldWrap;
      FWrapDirty = OldDirty;
      ConfigVault::SetKey(FDek.View(), OldWrap);
      FRewriteFailed = false;
      Done(false, QStringLiteral("The configuration could not be written, so nothing was changed. Is the disk full?"));
      return;
    }
    SyncNow();
    Done(true, Result);
  }
}

QString CloudSync::PendingRecoveryKey() const
{
  QByteArray Key = CloudCrypto::ParseRecoveryKey(FRecoveryKey);
  QByteArray Dek = CloudCrypto::UnwrapDekWithRecoveryKey(FWrap, Key);
  bool Works = !Dek.isEmpty() && FDek.Equals(Dek);
  Security::WipeBytes(Key);
  Security::WipeBytes(Dek);
  return Works ? FRecoveryKey : QString();
}

void CloudSync::RecoveryKeySaved()
{
  FRecoveryKey.clear();
  SaveJournal();
}

// The journal first, then the cloud: from the upload on the outcome may stay
// unknown, and what only this computer knows must survive that. When the new head
// is refused nothing has changed; once it is committed the new key is the
// account's, whatever happens next. Only then the local files. The private keys
// on the server follow last; they were already wrapped for the new key, and wait
// in the journal until they arrive.
void CloudSync::RotateDataKey(const QByteArray & Dek, const CloudCrypto::SnapshotHeader & Wrap, const QString & Result,
  const std::function<void(bool, const QString &)> & Done)
{
  if (!Ready())
  {
    Done(false, QStringLiteral("Replacing the data key needs this computer to be signed in to the NixSCP cloud."));
    return;
  }
  if (FBusy || (FState == csConflict) || (FState == csPaused))
  {
    Done(false, QStringLiteral("Wait until syncing has finished, and resolve any conflict first."));
    return;
  }
  FBusy = true;
  FSyncGeneration = FGeneration;
  SetState(csSyncing);
  auto NewKey = std::make_shared<SecureBytes>(Dek);
  auto Fail = [this, Done](const QString & Error)
  {
    FinishSync(csError, Error);
    Done(false, Error);
  };
  GetHead(SettingsSlot, [this, NewKey, Wrap, Result, Done, Fail](bool Ok, const Head & Remote, const CloudClient::Reply & Reply)
  {
    if (!Ok)
    {
      Fail(Reply.Error);
      return;
    }
    QByteArray Local = LocalSettings();
    QString LocalHash = CloudSettings::Hash(Local);
    TAction Action = Decide(FSettingsBase, FSettingsHash, Remote.Version, LocalHash);
    if ((Action != saNone) && (Action != saPush))
    {
      Fail(QStringLiteral("The cloud has settings this computer does not have yet. Sync, then try again."));
      return;
    }
    int Generation = FGeneration;
    FClient->Api("GET", QStringLiteral("/v1/keys/me"),
      [this, Generation, NewKey, Wrap, Result, Done, Fail, Remote, Local, LocalHash](const CloudClient::Reply & Keys)
    {
      if ((Generation != FGeneration) || (!Keys.Ok() && (Keys.Status != 404)))
      {
        Fail(Keys.Error.isEmpty() ? SignedOutReply().Error : Keys.Error);
        return;
      }
      // Still on their way after a previous change: those are the current ones
      QJsonObject Source = FPendingKeys.isEmpty() ? Keys.Json : QJsonDocument::fromJson(FPendingKeys.toUtf8()).object();
      QJsonObject Wraps;
      if (Keys.Ok() || !FPendingKeys.isEmpty())
      {
        Wraps = RewrapWithAnyKey(Source, NewKey->View());
        if (Wraps.isEmpty())
        {
          Fail(QStringLiteral("Your team key cannot be read with this computer's key, so the data key was not replaced."));
          return;
        }
      }
      FRotation = std::make_unique<Rotation>();
      FRotation->NewKey = *NewKey;
      FRotation->OldKey = FDek;
      FRotation->Wrap = Wrap;
      FRotation->Base = Remote.Version;
      FRotation->RecoveryKey = Result;
      FRotation->Keys = Wraps.isEmpty() ? QString() : QString::fromUtf8(QJsonDocument(Wraps).toJson(QJsonDocument::Compact));
      if (!SaveJournal())
      {
        FRotation.reset();
        Fail(QStringLiteral("The change could not be written on this computer, so the data key was not replaced. Is the disk full?"));
        return;
      }
      ConfigVault::StepReached("journal-written");
      QByteArray Snapshot = CloudCrypto::EncodeSnapshot(NewHeader(Wrap, SettingsSlot, Remote.Version + 1), NewKey->View(), Local);
      QString SnapshotHash = CloudSettings::Hash(Snapshot);
      auto CommitSent = std::make_shared<bool>(false);
      Upload(SettingsSlot, Remote.Version, Snapshot,
        [this, Generation, Result, Done, Fail, LocalHash, SnapshotHash, CommitSent](int Status, qint64 Version, const Head &, const QString & Error)
      {
        // Signed out meanwhile: the journal waits for this account
        if (Generation != FGeneration)
        {
          Done(false, SignedOutReply().Error);
          return;
        }
        if (Status == 200)
        {
          ConfigVault::StepReached("committed");
          FSettingsBase = Version;
          FSettingsHash = LocalHash;
          FSettingsMark.Version = Version;
          FSettingsMark.Hash = SnapshotHash;
          if (!SwitchToNewKey())
          {
            Fail(NotSwitchedText());
            return;
          }
          RotationLanded();
          FinishSync(csIdle);
          Done(true, Result);
        }
        else if (*CommitSent)
        {
          // A lost reply says nothing: the head tells
          FRotationDone = Done;
          RunSettingsSync(0);
        }
        else
        {
          // It never got as far as the commit, so nothing has changed
          FRotation.reset();
          SaveJournal();
          Fail((Status == 402) ? QStringLiteral("Replacing the data key needs an active NixSCP Pro license.") :
            ((Status == 409) ? QStringLiteral("Another computer uploaded settings at the same moment. Try again.") : Error));
        }
      }, [CommitSent]()
      {
        *CommitSent = true;
        ConfigVault::StepReached("uploaded");
      });
    });
  });
}

CloudSync::TRotationVerdict CloudSync::JudgeRotation(const QByteArray & Data, qint64 Version) const
{
  auto Opens = [this, &Data, Version](const SecureBytes & Key)
  {
    CloudCrypto::SnapshotHeader Header;
    QByteArray Plaintext;
    bool Ok = CloudCrypto::DecodeSnapshot(Data, Key.View(), Header, Plaintext) && (Header.Slot == SettingsSlot) &&
      (Header.User == FUser) && (Header.Version == Version);
    Security::WipeBytes(Plaintext);
    return Ok;
  };
  CloudCrypto::SnapshotHeader Claimed;
  bool HasHeader = CloudCrypto::ReadHeader(Data, Claimed);
  // Only whoever has the new key makes these, and it left this computer in that upload
  if (Opens(FRotation->NewKey) || (HasHeader && CloudCrypto::ProvesRotation(Claimed, FRotation->NewKey.View())))
  {
    return rvLanded;
  }
  // Above the base under the old key, or another replacement of that key: the
  // commit lost the compare-and-swap for good
  if (Opens(FRotation->OldKey) || (HasHeader && CloudCrypto::ProvesRotation(Claimed, FRotation->OldKey.View())))
  {
    return rvRefused;
  }
  return rvUnknown;
}

void CloudSync::SettleRotation(const Head & Remote, int Attempt)
{
  static const QString Later = QStringLiteral("Replacing the data key has not finished: the NixSCP cloud could not be read completely. "
    "NixSCP tries again later.");
  // Only the files of this computer were missing
  if (FDek.Equals(FRotation->NewKey.View()))
  {
    RotationJudged(rvLanded, Attempt);
    return;
  }
  qint64 Base = FRotation->Base;
  if (Remote.Version < qMax(Base, FSettingsMark.Version))
  {
    FinishSync(csError, RolledBackText(Remote.Version, qMax(Base, FSettingsMark.Version)));
    return;
  }
  if (Remote.Version == Base)
  {
    // Not committed as far as the head shows. A commit still on its way could yet
    // come through, so the replacement is only called off by closing that base:
    // anything committed on it makes the other fail (PushSettings)
    QByteArray Local = LocalSettings();
    PushSettings(Base, Local, CloudSettings::Hash(Local), Attempt);
    return;
  }
  DownloadRaw(SettingsSlot, Remote.Version, [this, Remote, Base, Attempt](bool Ok, const QByteArray & Data, const QString & Error)
  {
    if (!Ok || !FRotation)
    {
      FinishSync(csError, Error);
      return;
    }
    TRotationVerdict Verdict = JudgeRotation(Data, Remote.Version);
    if (Verdict != rvUnknown)
    {
      RotationJudged(Verdict, Attempt);
      return;
    }
    // An upload nobody here can read is on top: the versions under it tell
    auto Found = std::make_shared<TRotationVerdict>(rvUnknown);
    FindSnapshot(SettingsSlot, Remote.Version, 5, [this, Base, Found](qint64 Version, const QByteArray & Older)
    {
      *Found = (FRotation && (Version > Base)) ? JudgeRotation(Older, Version) : rvUnknown;
      return *Found != rvUnknown;
    }, [this, Attempt, Found](TFound Result, qint64, const QByteArray &)
    {
      if ((Result == fdFailed) || !FRotation)
      {
        FinishSync(csError, Later);
      }
      else
      {
        // Nothing of this key above the base: the commit cannot have been the
        // one that took it, and the usual repair of such a head can go on
        RotationJudged((Result == fdFound) ? *Found : rvRefused, Attempt);
      }
    });
  });
}

void CloudSync::RotationJudged(TRotationVerdict Verdict, int Attempt)
{
  if (Verdict == rvRefused)
  {
    RotationRefused(QStringLiteral("Another computer changed the cloud first, so the data key was not replaced: "
      "your password and recovery key stay as they were. Replace it again if you still want to."));
  }
  else if (SwitchToNewKey())
  {
    RotationLanded();
    KeyChangeTold(true, QStringLiteral("The data key was replaced after all, so your NixSCP password is now the new one you chose."));
  }
  else
  {
    FinishSync(csError, NotSwitchedText());
    return;
  }
  RunSettingsSync(Attempt);
}

bool CloudSync::SwitchToNewKey()
{
  CloudCrypto::SnapshotHeader Wrap = FRotation->Wrap;
  Wrap.User = FUser;
  if (!ConfigVault::SecureKey().Equals(FRotation->NewKey.View()) && !ConfigVault::ReplaceKey(FRotation->NewKey.View(), Wrap))
  {
    return false;
  }
  ConfigVault::StepReached("key-switched");
  FDek = FRotation->NewKey;
  FWrap = Wrap;
  FWrapDirty = false;
  RewriteConfiguration();
  ConfigVault::StepReached("config-rewritten");
  // The state first: with the rotation still in the journal, a crash here does this again
  if (!SaveState())
  {
    return false;
  }
  ConfigVault::StepReached("state-saved");
  // Told there are no team keys: whether that was the truth shows at the next
  // sync, and the old key is the only one that would open them
  if (FRotation->Keys.isEmpty())
  {
    RetireKey(FRotation->OldKey);
  }
  else
  {
    FPendingKeys = FRotation->Keys;
  }
  FRecoveryKey = FRotation->RecoveryKey;
  auto Finished = std::move(FRotation);
  if (!SaveJournal())
  {
    FRotation = std::move(Finished);
    return false;
  }
  ConfigVault::StepReached("journal-cleared");
  ConfigVault::EndReplaceKey();
  return true;
}

void CloudSync::RotationLanded()
{
  // The open tabs under the new key as well: the old head cannot be read anymore
  QByteArray Tabs = FSyncSessions ? ReadSessionCache() : QByteArray();
  if (!Tabs.isEmpty())
  {
    PushSession(Tabs);
  }
}

void CloudSync::RotationRefused(const QString & Why)
{
  FRotation.reset();
  SaveJournal();
  KeyChangeTold(false, Why);
}

void CloudSync::KeyChangeTold(bool Replaced, const QString & Message)
{
  if (FRotationDone)
  {
    auto Done = std::exchange(FRotationDone, {});
    Done(Replaced, Replaced ? FRecoveryKey : Message);
  }
  else
  {
    emit KeyChangeSettled(Replaced, Message);
  }
}

void CloudSync::RetireKey(const SecureBytes & Key)
{
  QByteArray Retired;
  Retired.reserve(FRetiredKeys.Size() + Key.Size());
  Retired.append(FRetiredKeys.View());
  Retired.append(Key.View());
  FRetiredKeys.Assign(Retired);
  Security::WipeBytes(Retired);
}

QJsonObject CloudSync::RewrapWithAnyKey(const QJsonObject & Keys, const QByteArray & NewDek) const
{
  QJsonObject Wraps = RewrapUserKeys(FUser, Keys, FDek.View(), NewDek);
  QByteArray Retired = FRetiredKeys.Copy();
  for (qsizetype Offset = 0; Wraps.isEmpty() && (Offset + CloudCrypto::KeyLength <= Retired.size()); Offset += CloudCrypto::KeyLength)
  {
    QByteArray Key = Retired.mid(Offset, CloudCrypto::KeyLength);
    Wraps = RewrapUserKeys(FUser, Keys, Key, NewDek);
    Security::WipeBytes(Key);
  }
  Security::WipeBytes(Retired);
  return Wraps;
}

void CloudSync::UploadPendingKeys()
{
  if (FKeysBusy)
  {
    return;
  }
  if (FPendingKeys.isEmpty())
  {
    MoveRetiredKeys();
    return;
  }
  QJsonObject Body = QJsonDocument::fromJson(FPendingKeys.toUtf8()).object();
  QString Sent = FPendingKeys;
  FKeysBusy = true;
  FClient->Api("PUT", QStringLiteral("/v1/keys/me"), Body, [this, Sent](const CloudClient::Reply & Reply)
  {
    FKeysBusy = false;
    if (Reply.Ok() && (FPendingKeys == Sent))
    {
      FPendingKeys.clear();
      SaveJournal();
      MoveRetiredKeys();
    }
  });
}

void CloudSync::MoveRetiredKeys()
{
  if (FRetiredKeys.IsEmpty())
  {
    return;
  }
  int Generation = FGeneration;
  FKeysBusy = true;
  FClient->Api("GET", QStringLiteral("/v1/keys/me"), [this, Generation](const CloudClient::Reply & Keys)
  {
    FKeysBusy = false;
    bool None = (Keys.ErrorCode() == QStringLiteral("no_keys"));
    if ((Generation != FGeneration) || FRetiredKeys.IsEmpty() || !FPendingKeys.isEmpty() || !HasKey() || (!Keys.Ok() && !None))
    {
      return;
    }
    QByteArray Wrapped = QByteArray::fromBase64(Keys.Json.value(QStringLiteral("wrapped_private_key")).toString().toLatin1());
    QByteArray Private;
    bool Moved = None || CloudCrypto::Open(FDek.View(), Wrapped, QByteArray(), Private);
    Security::WipeBytes(Private);
    if (!Moved)
    {
      QJsonObject Wraps = RewrapWithAnyKey(Keys.Json, FDek.View());
      if (Wraps.isEmpty())
      {
        // Under a key this computer never had: they stay for a later look
        return;
      }
      FPendingKeys = QString::fromUtf8(QJsonDocument(Wraps).toJson(QJsonDocument::Compact));
    }
    FRetiredKeys.Clear();
    if (SaveJournal() && !Moved)
    {
      UploadPendingKeys();
    }
  });
}

//---------------------------------------------------------------------------
// Data key in configuration backups

bool CloudSync::ExportKey(IniFile & File) const
{
  if (!HasKey() || FUser.isEmpty())
  {
    return false;
  }
  const QString & S = CloudSettings::KeySection;
  auto Hex = [](const QByteArray & Data) { return QString::fromLatin1(Data.toHex()); };
  File.EraseSectionTree(S);
  File.AddSection(S);
  File.WriteRaw(S, QStringLiteral("Format"), QStringLiteral("1"));
  File.WriteRaw(S, QStringLiteral("User"), FUser);
  File.WriteRaw(S, QStringLiteral("Email"), MungeStr(FEmail, true));
  File.WriteRaw(S, QStringLiteral("Dek"), Hex(FDek.View()));
  File.WriteRaw(S, QStringLiteral("MemoryKib"), QString::number(FWrap.Kdf.MemoryKib));
  File.WriteRaw(S, QStringLiteral("Iterations"), QString::number(FWrap.Kdf.Iterations));
  File.WriteRaw(S, QStringLiteral("Lanes"), QString::number(FWrap.Kdf.Lanes));
  File.WriteRaw(S, QStringLiteral("Salt"), Hex(FWrap.Kdf.Salt));
  File.WriteRaw(S, QStringLiteral("DekPassword"), Hex(FWrap.DekPassword));
  File.WriteRaw(S, QStringLiteral("DekRecovery"), Hex(FWrap.DekRecovery));
  File.WriteRaw(S, QStringLiteral("KeyEpoch"), QString::number(FWrap.KeyEpoch));
  File.WriteRaw(S, QStringLiteral("WrapEpoch"), QString::number(FWrap.WrapEpoch));
  File.WriteRaw(S, QStringLiteral("Rotated"), Hex(FWrap.Rotated));
  return true;
}

CloudSync::TKeyImport CloudSync::ImportKey(IniFile & File, QString * Email)
{
  const QString & S = CloudSettings::KeySection;
  if (!File.SectionExists(S))
  {
    File.EraseSectionTree(S);
    return kiNone;
  }
  auto Read = [&File](const char * Key) { return File.ReadRaw(S, QString::fromLatin1(Key)); };
  auto Hex = [&Read](const char * Key) { return QByteArray::fromHex(Read(Key).toLatin1()); };
  QString User = Read("User");
  QByteArray Dek = Hex("Dek");
  auto WipeDek = qScopeGuard([&Dek]() { Security::WipeBytes(Dek); });
  CloudCrypto::SnapshotHeader Wrap;
  Wrap.User = User;
  Wrap.Kdf.MemoryKib = Read("MemoryKib").toUInt();
  Wrap.Kdf.Iterations = Read("Iterations").toUInt();
  Wrap.Kdf.Lanes = Read("Lanes").toUInt();
  Wrap.Kdf.Salt = Hex("Salt");
  Wrap.DekPassword = Hex("DekPassword");
  Wrap.DekRecovery = Hex("DekRecovery");
  Wrap.KeyEpoch = Read("KeyEpoch").toLongLong();
  Wrap.WrapEpoch = Read("WrapEpoch").toLongLong();
  Wrap.Rotated = Hex("Rotated");
  QString KeyEmail = UnMungeStr(Read("Email"));
  bool KnownFormat = (Read("Format") == QStringLiteral("1"));
  File.EraseSectionTree(S);
  if (Email != nullptr)
  {
    *Email = KeyEmail;
  }
  if (!KnownFormat || User.isEmpty() || (Dek.size() != CloudCrypto::KeyLength) ||
      Wrap.Kdf.Salt.isEmpty() || Wrap.DekPassword.isEmpty())
  {
    return kiInvalid;
  }
  if (FState == csDisabled)
  {
    return kiInvalid;
  }
  if (SignedIn() && (User != FUser))
  {
    return kiOtherAccount;
  }
  if (FDek.Equals(Dek) && (User == FUser))
  {
    return kiImported;
  }
  // When signed out, the key waits for this account (CompleteSignIn keeps it)
  if (!SignedIn())
  {
    FUser = User;
    FEmail = KeyEmail;
    LoadJournal();
  }
  if (!AdoptKey(Dek, Wrap))
  {
    return kiInvalid;
  }
  FWrapDirty = false;
  // The imported settings meet the cloud as on a new computer (FirstSync)
  FSettingsBase = 0;
  FSettingsHash.clear();
  FSessionBase = 0;
  if (FState == csPaused)
  {
    FState = csIdle;
  }
  SaveState();
  UpdateReadyState();
  if (Ready())
  {
    FPeriodic->start();
    QTimer::singleShot(0, this, &CloudSync::SyncNow);
  }
  return kiImported;
}

//---------------------------------------------------------------------------
// Settings slot

void CloudSync::ScheduleSync()
{
  if (!FApplying && Ready())
  {
    FDebounce->start();
  }
}

void CloudSync::TimedSync()
{
  // A postponed conflict waits for the user (Sync now)
  if ((FState != csPaused) && (FState != csConflict))
  {
    SyncNow();
  }
}

void CloudSync::SyncNow()
{
  // The key of the configuration and the key of the sync are one key: settings
  // must never leave under another one
  if (SignedIn() && HasKey() && !KeyMatchesVault())
  {
    SetState(csError, QStringLiteral("The sync key does not match the key of the local configuration, so syncing is stopped. "
      "Restart NixSCP."));
    return;
  }
  if (!Ready() || (FState == csConflict))
  {
    return;
  }
  if (FBusy)
  {
    FSyncPending = true;
    return;
  }
  FBusy = true;
  FSyncGeneration = FGeneration;
  FDebounce->stop();
  SetState(csSyncing);
  RunSettingsSync(0);
}

void CloudSync::SyncIfStale(int MaxAgeSeconds)
{
  if (!FLastSync.isValid() || (FLastSync.secsTo(QDateTime::currentDateTimeUtc()) >= MaxAgeSeconds))
  {
    TimedSync();
  }
}

void CloudSync::FlushSettings(int TimeoutMs)
{
  if (!Ready() || (!FDebounce->isActive() && !FBusy))
  {
    return;
  }
  QEventLoop Loop;
  QTimer::singleShot(TimeoutMs, &Loop, &QEventLoop::quit);
  QMetaObject::Connection Watch = connect(this, &CloudSync::StateChanged, &Loop, [this, &Loop]()
  {
    if (!FBusy && (FState != csSyncing))
    {
      Loop.quit();
    }
  });
  if (FDebounce->isActive())
  {
    FDebounce->stop();
    SyncNow();
  }
  if (FBusy)
  {
    Loop.exec();
  }
  disconnect(Watch);
}

void CloudSync::NoteNoLicense()
{
  if (!FLicenseNotified)
  {
    FLicenseNotified = true;
    SaveState();
    emit LicenseInactive();
  }
}

void CloudSync::TellRotationWaiting(const QString & Error)
{
  if (FRotationDone)
  {
    // Undecided after a lost reply: the next sync finds out
    auto Waiting = std::exchange(FRotationDone, {});
    Waiting(false, (Error == NotSwitchedText()) ? Error : QStringLiteral("The connection was lost while the data key was being replaced. "
      "NixSCP finds out what happened as soon as the cloud can be reached, and tells you then."));
  }
}

void CloudSync::FinishSync(TState State, const QString & Error)
{
  // Signed out meanwhile: this sync no longer counts
  if (FSyncGeneration != FGeneration)
  {
    TellRotationWaiting(Error);
    return;
  }
  FBusy = false;
  if (State == csNoLicense)
  {
    NoteNoLicense();
  }
  if (State == csIdle)
  {
    FLastSync = QDateTime::currentDateTimeUtc();
  }
  SetState(State, Error);
  SaveState();
  TellRotationWaiting(Error);
  // Only now: a head that opens with this key shows that the key is still the
  // account's, and the team keys must not go back over a newer replacement
  if (State == csIdle)
  {
    UploadPendingKeys();
  }
  if (FSyncPending && (State == csIdle))
  {
    FSyncPending = false;
    QTimer::singleShot(0, this, &CloudSync::SyncNow);
  }
}

void CloudSync::Unauthorized(const QString & Error)
{
  // Still signed in: the server refused this one call, and the next sync tries
  // again; a state left on "syncing" would block that for good
  if (SignedIn())
  {
    FinishSync(csError, Error);
  }
  else if (FSyncGeneration == FGeneration)
  {
    FBusy = false;
    TellRotationWaiting(Error);
    UpdateReadyState();
  }
}

void CloudSync::RunSettingsSync(int Attempt)
{
  // Another NixSCP on this computer may have got further
  LoadJournal();
  GetHead(SettingsSlot, [this, Attempt](bool Ok, const Head & Remote, const CloudClient::Reply & Reply)
  {
    if (!Ok)
    {
      if (Reply.Status == 401)
      {
        Unauthorized(Reply.Error);
        return;
      }
      FinishSync(csError, Reply.Error);
      return;
    }
    if (FRotation)
    {
      SettleRotation(Remote, Attempt);
      return;
    }
    QByteArray Local = LocalSettings();
    QString LocalHash = CloudSettings::Hash(Local);
    TAction Action = Decide(FSettingsBase, FSettingsHash, Remote.Version, LocalHash);
    if (Remote.Version < FSettingsMark.Version)
    {
      // Also after the key was entered again, when the base starts over
      Action = saRolledBack;
    }
    if ((Action == saNone) && FWrapDirty)
    {
      // Nothing changed, but the new sync password must reach the cloud
      Action = saPush;
    }
    switch (Action)
    {
      case saNone:
        FinishSync(csIdle);
        break;
      case saPull:
        PullSettings(Remote);
        break;
      case saPush:
        PushSettings(FSettingsBase, Local, LocalHash, Attempt);
        break;
      case saRolledBack:
        FinishSync(csError, RolledBackText(Remote.Version, qMax(FSettingsBase, FSettingsMark.Version)));
        break;
      case saConflict:
        if ((FSettingsBase == 0) && FSettingsHash.isEmpty())
        {
          FirstSync(Remote, Local, LocalHash);
        }
        else
        {
          CheckConflict(Remote, Local, LocalHash);
        }
        break;
    }
  });
}

void CloudSync::OpenHead(const Head & Remote, const SnapshotCallback & Done)
{
  DownloadRaw(SettingsSlot, Remote.Version, [this, Remote, Done](bool Ok, const QByteArray & Data, const QString & Error)
  {
    if (!Ok)
    {
      FinishSync(csError, Error);
      return;
    }
    CloudCrypto::SnapshotHeader Header;
    QByteArray Plaintext;
    QString Problem;
    if (!Decode(Data, SettingsSlot, Remote.Version, Header, Plaintext, Problem))
    {
      HandleUndecryptable(Remote, Data);
      return;
    }
    QString Hash = CloudSettings::Hash(Data);
    VerifyChain(Header, Hash, [this, Remote, Done, Header, Plaintext, Hash](bool Good, const QString & Why)
    {
      if (!Good)
      {
        FinishSync(csError, Why);
        return;
      }
      NoteSeen(FSettingsMark, Remote.Version, Hash);
      AdoptWrap(Header);
      Done(Header, Plaintext);
    });
  });
}

void CloudSync::AskConflict(const Head & Remote)
{
  FBusy = false;
  FConflictHead = Remote;
  SetState(csConflict);
  SaveState();
  emit ConflictDetected(Remote.Device, Remote.UpdatedAt);
}

void CloudSync::CheckConflict(const Head & Remote, const QByteArray & Local, const QString & LocalHash)
{
  OpenHead(Remote, [this, Remote, Local, LocalHash](const CloudCrypto::SnapshotHeader &, const QByteArray & Plaintext)
  {
    if (Plaintext == Local)
    {
      FSettingsBase = Remote.Version;
      FSettingsHash = LocalHash;
      FinishSync(csIdle);
    }
    else
    {
      AskConflict(Remote);
    }
  });
}

void CloudSync::FirstSync(const Head & Remote, const QByteArray & Local, const QString & LocalHash)
{
  // This computer syncs for the first time and the cloud already has settings
  OpenHead(Remote, [this, Remote, Local, LocalHash](const CloudCrypto::SnapshotHeader & Header, const QByteArray & Plaintext)
  {
    if (Plaintext == Local)
    {
      FSettingsBase = Remote.Version;
      FSettingsHash = LocalHash;
      FinishSync(csIdle);
    }
    else if (!HasStoredSites(FConfig->ExportData()))
    {
      // A fresh installation: simply take the cloud settings
      ApplySettings(Plaintext, Remote.Version, Header.Device);
    }
    else
    {
      AskConflict(Remote);
    }
  });
}

void CloudSync::PullSettings(const Head & Remote)
{
  OpenHead(Remote, [this, Remote](const CloudCrypto::SnapshotHeader & Header, const QByteArray & Plaintext)
  {
    ApplySettings(Plaintext, Remote.Version, Header.Device);
  });
}

void CloudSync::FindSnapshot(const QString & Slot, qint64 Below, int MaxTries,
  const std::function<bool(qint64, const QByteArray &)> & Accept, const FoundCallback & Done)
{
  int Generation = FGeneration;
  FClient->Api("GET", QStringLiteral("/v1/sync/history?slot=%1").arg(Slot), [this, Generation, Slot, Below, MaxTries, Accept, Done](const CloudClient::Reply & Reply)
  {
    QJsonDocument Doc = QJsonDocument::fromJson(Reply.Data);
    if ((Generation != FGeneration) || !Reply.Ok() || !Doc.isArray())
    {
      Done(fdFailed, 0, QByteArray());
      return;
    }
    QList<qint64> Versions;
    for (const QJsonValue & Item : Doc.array())
    {
      qint64 Version = Item.toObject().value(QStringLiteral("version")).toInteger();
      if ((Below == 0) || (Version < Below))
      {
        Versions.append(Version);
      }
    }
    std::sort(Versions.begin(), Versions.end(), std::greater<qint64>());
    if (Versions.size() > MaxTries)
    {
      Versions.resize(MaxTries);
    }
    // Tried one after the other, newest first. Only the request in flight keeps
    // this alive: Accept may hold the password
    using Step = std::function<void(int)>;
    auto Next = std::make_shared<Step>();
    auto Failed = std::make_shared<bool>(false);
    *Next = [this, Slot, Versions, Accept, Done, Failed, Self = std::weak_ptr<Step>(Next)](int Index)
    {
      if (Index >= Versions.size())
      {
        Done(*Failed ? fdFailed : fdNotFound, 0, QByteArray());
        return;
      }
      qint64 Version = Versions[Index];
      DownloadRaw(Slot, Version, [Version, Index, Accept, Done, Failed, Keep = Self.lock()](bool Ok, const QByteArray & Data, const QString &)
      {
        if (Ok && Accept(Version, Data))
        {
          Done(fdFound, Version, Data);
          return;
        }
        *Failed = *Failed || !Ok;
        (*Keep)(Index + 1);
      });
    };
    (*Next)(0);
  });
}

void CloudSync::KeyReplaced(const QString & Message)
{
  FDek.Clear();
  FBusy = false;
  SaveState();
  SetState(csNeedKey, Message);
}

void CloudSync::HandleUndecryptable(const Head & Remote, const QByteArray & Data)
{
  static const QString Replaced = QStringLiteral("The data key was replaced on another computer. Enter your current sync password.");
  CloudCrypto::SnapshotHeader Claimed;
  bool HasHeader = CloudCrypto::ReadHeader(Data, Claimed);
  if (HasHeader && CloudCrypto::ProvesRotation(Claimed, FDek.View()))
  {
    KeyReplaced(Replaced);
    return;
  }
  QString Device = HasHeader ? Claimed.Device : QString();
  // Does the key of this computer still decrypt an older version? On the way
  // back there may be a version that proves a replacement of this very key.
  auto Rotated = std::make_shared<bool>(false);
  FindSnapshot(SettingsSlot, Remote.Version, 5, [this, Rotated](qint64 Version, const QByteArray & Older)
  {
    CloudCrypto::SnapshotHeader Header;
    QByteArray Plaintext;
    QString Error;
    if (Decode(Older, SettingsSlot, Version, Header, Plaintext, Error))
    {
      return true;
    }
    *Rotated = CloudCrypto::ReadHeader(Older, Header) && CloudCrypto::ProvesRotation(Header, FDek.View());
    return *Rotated;
  }, [this, Remote, Device, Rotated](TFound Result, qint64 Version, const QByteArray & Older)
  {
    if (Result == fdFailed)
    {
      // Says nothing about the key: only "nothing decrypts" may take it away
      FinishSync(csError, QStringLiteral("The NixSCP cloud could not be read completely; retrying later."));
    }
    else if (*Rotated)
    {
      KeyReplaced(Replaced);
    }
    else if (Result == fdNotFound)
    {
      // Nothing decrypts: the account's key is not the one of this computer
      KeyReplaced(QStringLiteral("The synced settings cannot be decrypted with the key on this computer. Enter your sync password."));
    }
    else
    {
      RepairForeignHead(Remote, Device, Version, Older);
    }
  });
}

void CloudSync::RepairForeignHead(const Head & Remote, const QString & Device, qint64 Version, const QByteArray & Data)
{
  // The newest version was uploaded without the sync password: ignore it and
  // put valid settings on top (it stays in the cloud history)
  qWarning("NixSCP cloud: settings v%lld cannot be decrypted, v%lld can; replacing it", static_cast<long long>(Remote.Version), static_cast<long long>(Version));
  emit ForeignSnapshotIgnored(Device);
  QByteArray Local = LocalSettings();
  QString LocalHash = CloudSettings::Hash(Local);
  bool FirstTime = (FSettingsBase == 0) && FSettingsHash.isEmpty();
  if (!FirstTime && (Version <= FSettingsBase))
  {
    // This computer already has the newest valid settings
    PushSettings(Remote.Version, Local, LocalHash, 0);
    return;
  }
  CloudCrypto::SnapshotHeader Header;
  QByteArray Plaintext;
  QString Error;
  Decode(Data, SettingsSlot, Version, Header, Plaintext, Error);
  QString Hash = CloudSettings::Hash(Data);
  VerifyChain(Header, Hash, [this, Remote, Version, Header, Plaintext, Hash, Local, LocalHash, FirstTime](bool Good, const QString & Why)
  {
    if (!Good)
    {
      FinishSync(csError, Why);
      return;
    }
    NoteSeen(FSettingsMark, Version, Hash);
    AdoptWrap(Header);
    if ((LocalHash == FSettingsHash) || (FirstTime && !HasStoredSites(FConfig->ExportData())))
    {
      // Nothing changed here (or a fresh installation): take the newest valid
      // settings, then put them on top of the foreign snapshot
      ApplySettings(Plaintext, Version, Header.Device, Remote.Version);
    }
    else if (FirstTime && (Plaintext == Local))
    {
      // First sync of a computer with its own sites: identical settings need no decision
      FSettingsBase = Version;
      FSettingsHash = LocalHash;
      PushSettings(Remote.Version, Local, LocalHash, 0);
    }
    else if (FirstTime)
    {
      FinishSync(csError, QStringLiteral("This computer's settings differ from the cloud, and the newest cloud upload is unreadable. "
        "Sync from another computer first; retrying later."));
    }
    else
    {
      FinishSync(csError, QStringLiteral("Settings changed here and on another computer while the cloud holds an unreadable upload; "
        "retrying later."));
    }
  });
}

void CloudSync::ApplySettings(const QByteArray & Plaintext, qint64 Version, const QString & Device, qint64 PushOnTop)
{
  IniFile Remote;
  Remote.LoadFromData(Plaintext);
  IniFile Local = FConfig->ExportData();
  Backup(Local);
  IniFile Merged = CloudSettings::MergePulled(Remote, Local, AutoWorkspacePrefix());
  QString Error;
  FApplying = true;
  bool Ok = FConfig->ImportData(Merged, &Error);
  FApplying = false;
  FDebounce->stop();
  if (!Ok)
  {
    FinishSync(csError, Error);
    return;
  }
  // Settings of a computer still using a WinSCP master password: the NixSCP
  // password replaces it here as well
  if (ConfigVault::Enabled() && FConfig->UseMasterPassword() && Security::IsMasterPasswordKnown())
  {
    FConfig->ClearMasterPassword();
  }
  FSettingsBase = Version;
  // As this computer sees the settings now (the configuration may write some
  // values in its own form): no needless upload follows
  QByteArray Applied = LocalSettings();
  FSettingsHash = CloudSettings::Hash(Applied);
  emit SettingsPulled(Device);
  if (PushOnTop > 0)
  {
    // Replaces an unreadable head (HandleUndecryptable)
    PushSettings(PushOnTop, Applied, FSettingsHash, 0);
    return;
  }
  FinishSync(csIdle);
}

void CloudSync::PushSettings(qint64 Base, const QByteArray & Plaintext, const QString & LocalHash, int Attempt)
{
  QByteArray Snapshot = CloudCrypto::EncodeSnapshot(NewHeader(SettingsSlot, Base + 1), FDek.View(), Plaintext);
  QString SnapshotHash = CloudSettings::Hash(Snapshot);
  Upload(SettingsSlot, Base, Snapshot, [this, LocalHash, Attempt, SnapshotHash](int Status, qint64 Version, const Head &, const QString & Error)
  {
    if (Status == 200)
    {
      FSettingsBase = Version;
      FSettingsHash = LocalHash;
      NoteSeen(FSettingsMark, Version, SnapshotHash);
      FWrapDirty = false;
      FLicenseNotified = false;
      if (FRotation)
      {
        // This version took the place of the new key's (SettleRotation)
        RotationRefused(CalledOffText());
      }
      FinishSync(csIdle);
    }
    else if ((Status == 409) && (Attempt + 1 < MaxSyncAttempts))
    {
      // Another computer committed first: decide again (pull or conflict)
      RunSettingsSync(Attempt + 1);
    }
    else if (Status == 402)
    {
      // The server takes no commit at all now, of the new key either
      if (FRotation)
      {
        RotationRefused(CalledOffText());
      }
      FinishSync(csNoLicense);
    }
    else if (Status == 401)
    {
      Unauthorized(Error);
    }
    else
    {
      FinishSync(csError, Error);
    }
  });
}

void CloudSync::ResolveConflict(TResolution Resolution)
{
  if ((FState != csConflict) && (FState != csPaused))
  {
    return;
  }
  if (Resolution == crLater)
  {
    SetState(csPaused);
    SaveState();
    return;
  }
  FBusy = true;
  FSyncGeneration = FGeneration;
  SetState(csSyncing);
  // The head as it is now: after a restart nothing else is known about a
  // postponed conflict, and the cloud may have moved on since the question
  GetHead(SettingsSlot, [this, Resolution](bool Ok, const Head & Remote, const CloudClient::Reply & Reply)
  {
    if (!Ok)
    {
      if (Reply.Status == 401)
      {
        Unauthorized(Reply.Error);
        return;
      }
      FinishSync(csError, Reply.Error);
      return;
    }
    if (Remote.Version < qMax(FSettingsBase, FSettingsMark.Version))
    {
      FinishSync(csError, RolledBackText(Remote.Version, qMax(FSettingsBase, FSettingsMark.Version)));
      return;
    }
    auto PushLocal = [this, Remote]()
    {
      QByteArray Local = LocalSettings();
      PushSettings(Remote.Version, Local, CloudSettings::Hash(Local), 0);
    };
    if (Remote.Version == 0)
    {
      PushLocal();
    }
    else if (Resolution == crUseCloud)
    {
      PullSettings(Remote);
    }
    else
    {
      // Explicit overwrite on top of the cloud head; the old head stays in
      // history. The head is read first: it may carry a newer password, and the
      // old wraps of this computer must not replace it for everyone.
      OpenHead(Remote, [PushLocal](const CloudCrypto::SnapshotHeader &, const QByteArray &) { PushLocal(); });
    }
  });
}

//---------------------------------------------------------------------------
// Session slot

QString CloudSync::SessionCacheFileName() const
{
  return FStateFileName.isEmpty() ? QString() : QFileInfo(FStateFileName).absolutePath() + QStringLiteral("/nixscp-cloud-session.ini");
}

void CloudSync::WriteSessionCache(const QByteArray & Payload) const
{
  QString Name = SessionCacheFileName();
  if (Name.isEmpty())
  {
    return;
  }
  QSaveFile File(Name);
  if (File.open(QIODevice::WriteOnly))
  {
    Platform::MakePrivate(File);
    File.write(ConfigVault::ForFile(Name, Payload));
    File.commit();
  }
}

QByteArray CloudSync::ReadSessionCache() const
{
  return ConfigVault::ReadFile(SessionCacheFileName());
}

void CloudSync::PushSession(const QByteArray & Payload)
{
  if (!Ready() || !FSyncSessions)
  {
    return;
  }
  WriteSessionCache(Payload);
  if (FSessionBusy)
  {
    FPendingSession = Payload;
    return;
  }
  FSessionBusy = true;
  PushSessionAttempt(Payload, QDateTime::currentDateTimeUtc(), 0, [this](bool, const QString &)
  {
    FSessionBusy = false;
    if (!FPendingSession.isEmpty())
    {
      PushSession(std::exchange(FPendingSession, QByteArray()));
    }
  });
}

void CloudSync::PushSessionAttempt(const QByteArray & Payload, const QDateTime & SavedAt, int Attempt, const DoneCallback & Done)
{
  CloudCrypto::SnapshotHeader Header = NewHeader(SessionSlot, FSessionBase + 1);
  Header.SavedAt = SavedAt;
  QByteArray Snapshot = CloudCrypto::EncodeSnapshot(Header, FDek.View(), Payload);
  QString SnapshotHash = CloudSettings::Hash(Snapshot);
  Upload(SessionSlot, FSessionBase, Snapshot,
    [this, Payload, SavedAt, Attempt, Done, SnapshotHash](int Status, qint64 Version, const Head & Conflict, const QString & Error)
  {
    if (Status == 200)
    {
      FSessionBase = Version;
      NoteSeen(FSessionMark, Version, SnapshotHash);
      SaveState();
      Done(true, QString());
      return;
    }
    if (Status == 402)
    {
      NoteNoLicense();
    }
    if ((Status != 409) || (Attempt + 1 >= MaxSyncAttempts))
    {
      Done(false, Error);
      return;
    }
    if (Conflict.Version < FSessionMark.Version)
    {
      Done(false, RolledBackText(Conflict.Version, FSessionMark.Version));
      return;
    }
    // Another computer saved its tabs too: the later capture wins
    DownloadRaw(SessionSlot, Conflict.Version, [this, Payload, SavedAt, Attempt, Done, Conflict](bool Ok, const QByteArray & Data, const QString &)
    {
      FSessionBase = Conflict.Version;
      // Only an authenticated time counts: a snapshot uploaded without the
      // sync password could otherwise claim to be newer forever
      CloudCrypto::SnapshotHeader Remote;
      QByteArray Plaintext;
      QString Error;
      if (Ok && Decode(Data, SessionSlot, Conflict.Version, Remote, Plaintext, Error))
      {
        NoteSeen(FSessionMark, Conflict.Version, CloudSettings::Hash(Data));
        if (Remote.SavedAt.isValid() && (Remote.SavedAt > SavedAt))
        {
          SaveState();
          Done(true, QString());
          return;
        }
      }
      PushSessionAttempt(Payload, SavedAt, Attempt + 1, Done);
    });
  });
}

void CloudSync::FlushSession(const QByteArray & Payload, int TimeoutMs)
{
  if (!Ready() || !FSyncSessions)
  {
    return;
  }
  WriteSessionCache(Payload);
  QEventLoop Loop;
  auto Finished = std::make_shared<bool>(false);
  QTimer::singleShot(TimeoutMs, &Loop, &QEventLoop::quit);
  // Pushed directly, even when an older push is in flight: the newer capture wins
  PushSessionAttempt(Payload, QDateTime::currentDateTimeUtc(), 0, [Finished, &Loop](bool, const QString &)
  {
    *Finished = true;
    Loop.quit();
  });
  if (!*Finished)
  {
    Loop.exec();
  }
}

void CloudSync::FetchSession(int TimeoutMs, const std::function<void(bool, const QByteArray &)> & Done)
{
  if (!Ready() || !FRestoreSessions)
  {
    Done(false, QByteArray());
    return;
  }
  auto Finished = std::make_shared<bool>(false);
  auto Finish = [this, Finished, Done](bool UseCache, const QByteArray & Payload)
  {
    if (*Finished)
    {
      return;
    }
    *Finished = true;
    QByteArray Result = UseCache ? ReadSessionCache() : Payload;
    Done(!Result.trimmed().isEmpty(), Result);
  };
  QTimer::singleShot(TimeoutMs, this, [Finish]() { Finish(true, QByteArray()); });
  // What the cloud shows must continue what this computer saw, as for the
  // settings; the copy on disk is the newest thing known otherwise
  auto Refuse = [this, Finish](const QString & Problem)
  {
    qWarning("NixSCP cloud: the open tabs in the cloud are ignored: %s", qPrintable(Problem));
    if (!FBusy)
    {
      SetState(csError, Problem);
    }
    Finish(true, QByteArray());
  };
  GetHead(SessionSlot, [this, Finish, Refuse](bool Ok, const Head & Remote, const CloudClient::Reply &)
  {
    if (!Ok)
    {
      Finish(true, QByteArray());
      return;
    }
    qint64 Seen = qMax(FSessionBase, FSessionMark.Version);
    if (Remote.Version < Seen)
    {
      Refuse(RolledBackText(Remote.Version, Seen));
      return;
    }
    if (Remote.Version == 0)
    {
      Finish(false, QByteArray());
      return;
    }
    DownloadRaw(SessionSlot, Remote.Version, [this, Finish, Refuse, Remote](bool Ok2, const QByteArray & Data, const QString &)
    {
      CloudCrypto::SnapshotHeader Header;
      QByteArray Plaintext;
      QString Error;
      if (!Ok2 || !Decode(Data, SessionSlot, Remote.Version, Header, Plaintext, Error))
      {
        Finish(true, QByteArray());
        return;
      }
      QString Hash = CloudSettings::Hash(Data);
      if (!Continues(FSessionMark, Header, Hash))
      {
        Refuse(ForkedText());
        return;
      }
      FSessionBase = Remote.Version;
      NoteSeen(FSessionMark, Remote.Version, Hash);
      SaveState();
      WriteSessionCache(Plaintext);
      Finish(false, Plaintext);
    });
  });
}
