#include "core/CloudSync.h"
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
  connect(FClient, &CloudClient::SignedOut, this, [this](const QString & Reason)
  {
    FPeriodic->stop();
    FBusy = false;
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
  FStateFile.Clear();
  if (QFileInfo::exists(FStateFileName))
  {
    FStateFile.Load(FStateFileName);
  }
  auto Read = [this](const char * Section, const char * Key, const QString & Default = QString())
  {
    return FStateFile.ReadRaw(QString::fromLatin1(Section), QString::fromLatin1(Key), Default);
  };
  // $NIXSCP_CLOUD_URL wins (testing); else the server this computer signed in to
  QString Url = qEnvironmentVariable("NIXSCP_CLOUD_URL");
  FClient->SetBaseUrl(!Url.isEmpty() ? Url : Read("Account", "BaseUrl", CloudClient::DefaultBaseUrl()));
  FClient->SetRefreshToken(Read("Account", "RefreshToken"));
  FEmail = Read("Account", "Email");
  FUser = Read("Account", "User");
  FRememberKey = (Read("Options", "RememberKey", QStringLiteral("1")) == QStringLiteral("1"));
  FSyncSessions = (Read("Options", "SyncSessions", QStringLiteral("1")) == QStringLiteral("1"));
  FRestoreSessions = (Read("Options", "RestoreSessions", QStringLiteral("1")) == QStringLiteral("1"));
  // Versions before 1.2 could remember the raw data key here; it is taken
  // once (moved into memory and the encrypted configuration) and erased
  FDek = QByteArray::fromBase64(Read("Key", "Dek").toLatin1());
  if (FDek.size() != CloudCrypto::KeyLength)
  {
    FDek.clear();
  }
  // Unlocked at startup with the NixSCP password (the same key)
  if (FDek.isEmpty() && !FUser.isEmpty() && (ConfigVault::User() == FUser) && (ConfigVault::Key().size() == CloudCrypto::KeyLength))
  {
    FDek = ConfigVault::Key();
  }
  FWrap = CloudCrypto::SnapshotHeader();
  FWrap.Kdf.MemoryKib = Read("Key", "MemoryKib", QStringLiteral("65536")).toUInt();
  FWrap.Kdf.Iterations = Read("Key", "Iterations", QStringLiteral("3")).toUInt();
  FWrap.Kdf.Lanes = Read("Key", "Lanes", QStringLiteral("1")).toUInt();
  FWrap.Kdf.Salt = QByteArray::fromBase64(Read("Key", "Salt").toLatin1());
  FWrap.DekPassword = QByteArray::fromBase64(Read("Key", "DekPassword").toLatin1());
  FWrap.DekRecovery = QByteArray::fromBase64(Read("Key", "DekRecovery").toLatin1());
  FWrapDirty = (Read("Key", "Dirty") == QStringLiteral("1"));
  FSettingsBase = Read("Settings", "BaseVersion", QStringLiteral("0")).toLongLong();
  FSettingsHash = Read("Settings", "BaseHash");
  FSessionBase = Read("Session", "BaseVersion", QStringLiteral("0")).toLongLong();
  FLastSync = QDateTime::fromString(Read("Settings", "LastSync"), Qt::ISODate);
  FLicenseNotified = (Read("Settings", "LicenseNotified") == QStringLiteral("1"));
  if (Read("Settings", "Paused") == QStringLiteral("1"))
  {
    FState = csPaused;
  }
}

// The local configuration is encrypted with the data key (NixSCP Pro): on
// whenever the key is known; rewritten when the key or its password changed.
// A WinSCP master password is retired: the NixSCP password replaces it.
void CloudSync::ProtectConfiguration()
{
  if ((FDek.size() != CloudCrypto::KeyLength) || FWrap.DekPassword.isEmpty() || FUser.isEmpty())
  {
    return;
  }
  CloudCrypto::SnapshotHeader Wrap = FWrap;
  Wrap.User = FUser;
  CloudCrypto::SnapshotHeader Current = ConfigVault::Wrap();
  bool Same = ConfigVault::Enabled() && (ConfigVault::Key() == FDek) && (Current.DekPassword == Wrap.DekPassword) &&
    (Current.DekRecovery == Wrap.DekRecovery) && (Current.User == Wrap.User);
  if (Same)
  {
    return;
  }
  ConfigVault::SetKey(FDek, Wrap);
  if (FConfig != nullptr)
  {
    if (FConfig->UseMasterPassword() && Security::IsMasterPasswordKnown())
    {
      FConfig->ClearMasterPassword();
    }
    FConfig->Rewrite();
  }
}

void CloudSync::SaveState()
{
  if (FStateFileName.isEmpty())
  {
    return;
  }
  IniFile & F = FStateFile;
  F.Clear();
  auto Write = [&F](const char * Section, const char * Key, const QString & Value)
  {
    F.WriteRaw(QString::fromLatin1(Section), QString::fromLatin1(Key), Value);
  };
  auto Bool = [](bool Value) { return Value ? QStringLiteral("1") : QStringLiteral("0"); };
  Write("Account", "BaseUrl", FClient->BaseUrl());
  Write("Account", "Email", FEmail);
  Write("Account", "User", FUser);
  Write("Account", "RefreshToken", FClient->RefreshToken());
  Write("Options", "RememberKey", Bool(FRememberKey));
  Write("Options", "SyncSessions", Bool(FSyncSessions));
  Write("Options", "RestoreSessions", Bool(FRestoreSessions));
  // The data key is never stored: the NixSCP password unlocks it at startup
  // (ConfigVault), and it encrypts the local configuration too
  Write("Key", "Dek", QString());
  Write("Key", "MemoryKib", QString::number(FWrap.Kdf.MemoryKib));
  Write("Key", "Iterations", QString::number(FWrap.Kdf.Iterations));
  Write("Key", "Lanes", QString::number(FWrap.Kdf.Lanes));
  Write("Key", "Salt", QString::fromLatin1(FWrap.Kdf.Salt.toBase64()));
  Write("Key", "DekPassword", QString::fromLatin1(FWrap.DekPassword.toBase64()));
  Write("Key", "DekRecovery", QString::fromLatin1(FWrap.DekRecovery.toBase64()));
  Write("Key", "Dirty", Bool(FWrapDirty));
  Write("Settings", "BaseVersion", QString::number(FSettingsBase));
  Write("Settings", "BaseHash", FSettingsHash);
  Write("Settings", "LastSync", FLastSync.toUTC().toString(Qt::ISODate));
  Write("Settings", "Paused", Bool(FState == csPaused));
  Write("Settings", "LicenseNotified", Bool(FLicenseNotified));
  Write("Session", "BaseVersion", QString::number(FSessionBase));
  if (F.Save(FStateFileName))
  {
    QFile::setPermissions(FStateFileName, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  }
  ProtectConfiguration();
}

void CloudSync::ResetSyncState()
{
  FDek.clear();
  FWrap = CloudCrypto::SnapshotHeader();
  FWrapDirty = false;
  FSettingsBase = 0;
  FSettingsHash.clear();
  FSessionBase = 0;
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
  return (FConfig != nullptr) && (FState != csDisabled) && SignedIn() && HasKey();
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
    case csNeedKey: return QStringLiteral("Enter your sync password to start syncing on this computer.");
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
  CloudCrypto::SnapshotHeader Header = FWrap;
  Header.Format = 1;
  Header.Slot = Slot;
  Header.User = FUser;
  Header.Version = Version;
  Header.SavedAt = QDateTime::currentDateTimeUtc();
  Header.Device = DeviceName();
  return Header;
}

void CloudSync::AdoptWrap(const CloudCrypto::SnapshotHeader & Header)
{
  // The settings head carries the current sync password (it may have been
  // changed on another computer); a local change not uploaded yet wins
  if (FWrapDirty || (Header.Slot != SettingsSlot))
  {
    return;
  }
  FWrap.Kdf = Header.Kdf;
  FWrap.DekPassword = Header.DekPassword;
  FWrap.DekRecovery = Header.DekRecovery;
}

bool CloudSync::Decode(const QByteArray & Data, const QString & Slot, qint64 Version, CloudCrypto::SnapshotHeader & Header,
  QByteArray & Plaintext, QString & Error) const
{
  if (!CloudCrypto::DecodeSnapshot(Data, FDek, Header, Plaintext, &Error))
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
    QFile::setPermissions(Name, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
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
  FClient->Api("GET", QStringLiteral("/v1/sync/head?slot=%1").arg(Slot), [Done](const CloudClient::Reply & Reply)
  {
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

void CloudSync::DownloadRaw(const QString & Slot, qint64 Version, const DataCallback & Done)
{
  FClient->Api("GET", QStringLiteral("/v1/sync/download-url?slot=%1&version=%2").arg(Slot).arg(Version),
    [this, Done](const CloudClient::Reply & Reply)
  {
    if (!Reply.Ok())
    {
      Done(false, QByteArray(), Reply.Error);
      return;
    }
    QString Sha = Reply.Json.value(QStringLiteral("sha256")).toString();
    FClient->Transfer("GET", QUrl(Reply.Json.value(QStringLiteral("url")).toString()), QByteArray(), {},
      [Done, Sha](const CloudClient::Reply & Get)
    {
      if (!Get.Ok())
      {
        Done(false, QByteArray(), QStringLiteral("Downloading from the NixSCP cloud failed (%1).").arg(Get.Status));
        return;
      }
      if (QString::fromLatin1(QCryptographicHash::hash(Get.Data, QCryptographicHash::Sha256).toHex()) != Sha)
      {
        Done(false, QByteArray(), QStringLiteral("The downloaded snapshot is incomplete."));
        return;
      }
      Done(true, Get.Data, QString());
    });
  });
}

void CloudSync::Upload(const QString & Slot, qint64 Base, const QByteArray & Snapshot, const UploadCallback & Done)
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
  FClient->Api("POST", QStringLiteral("/v1/sync/upload-url"), Request, [this, Slot, Base, Snapshot, Done, ParseHead](const CloudClient::Reply & Reply)
  {
    if (!Reply.Ok())
    {
      Done(Reply.Status, 0, (Reply.Status == 409) ? ParseHead(Reply.Json) : Head(), Reply.Error);
      return;
    }
    QString ObjectKey = Reply.Json.value(QStringLiteral("object_key")).toString();
    FClient->Transfer("PUT", QUrl(Reply.Json.value(QStringLiteral("url")).toString()), Snapshot,
      {{"Content-Type", "application/octet-stream"}}, [this, Slot, Base, Snapshot, ObjectKey, Done, ParseHead](const CloudClient::Reply & Put)
    {
      if (!Put.Ok())
      {
        Done(Put.Status ? Put.Status : 0, 0, Head(), QStringLiteral("Uploading to the NixSCP cloud failed (%1).").arg(Put.Status));
        return;
      }
      QJsonObject Commit
      {
        {QStringLiteral("slot"), Slot},
        {QStringLiteral("base_version"), Base},
        {QStringLiteral("object_key"), ObjectKey},
        {QStringLiteral("size"), Snapshot.size()},
        {QStringLiteral("sha256"), QString::fromLatin1(QCryptographicHash::hash(Snapshot, QCryptographicHash::Sha256).toHex())},
      };
      FClient->Api("POST", QStringLiteral("/v1/sync/commit"), Commit, [Done, ParseHead](const CloudClient::Reply & Result)
      {
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
      if (!Ok)
      {
        Done(false, QStringLiteral("Signed in, but the account could not be loaded."));
        return;
      }
      QJsonObject User = Me.value(QStringLiteral("user")).toObject();
      QString Uuid = User.value(QStringLiteral("uuid")).toString();
      if (Uuid != FUser)
      {
        // Another account than before: nothing of the old sync state applies
        ResetSyncState();
        FUser = Uuid;
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
  FClient->SignOut([this]()
  {
    ResetSyncState();
    FEmail.clear();
    FUser.clear();
    FBusy = false;
    SetState(csSignedOut);
    SaveState();
    QFile::remove(SessionCacheFileName());
  });
}

//---------------------------------------------------------------------------
// Key

void CloudSync::ProbeCloud(const std::function<void(bool, bool, const QString &)> & Done)
{
  GetHead(SettingsSlot, [this, Done](bool Ok, const Head & Settings, const CloudClient::Reply & Reply)
  {
    if (!Ok)
    {
      Done(false, false, Reply.Error);
      return;
    }
    if (Settings.Version > 0)
    {
      Done(true, true, QString());
      return;
    }
    GetHead(SessionSlot, [Done](bool Ok2, const Head & Session, const CloudClient::Reply & Reply2)
    {
      Done(Ok2, Ok2 && (Session.Version > 0), Reply2.Error);
    });
  });
}

void CloudSync::CreateKey(const QString & Password, bool WithRecoveryKey, const std::function<void(bool, const QString &)> & Done)
{
  QByteArray Dek = CloudCrypto::RandomBytes(CloudCrypto::KeyLength);
  QByteArray RecoveryKey = WithRecoveryKey ? CloudCrypto::NewRecoveryKey() : QByteArray();
  CloudCrypto::SnapshotHeader Wrap;
  Wrap.User = FUser;
  QString Error;
  if (!CloudCrypto::WrapDek(Wrap, Dek, Password, RecoveryKey, &Error))
  {
    Done(false, Error);
    return;
  }
  FDek = Dek;
  FWrap = Wrap;
  FWrapDirty = false;
  FSettingsBase = 0;
  FSettingsHash.clear();
  FSessionBase = 0;
  SaveState();
  UpdateReadyState();
  FPeriodic->start();
  SyncNow();
  Done(true, WithRecoveryKey ? CloudCrypto::FormatRecoveryKey(RecoveryKey) : QString());
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
    if (Dek.isEmpty())
    {
      Problem = RecoveryKey.isEmpty() ? QStringLiteral("This is not a valid recovery key.") : QStringLiteral("The recovery key is not correct.");
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
  if (!CloudCrypto::DecodeSnapshot(Data, Dek, Checked, Plaintext, &Problem) ||
      (Checked.Slot != Slot) || (Checked.User != FUser) || (Checked.Version != Version))
  {
    if (Problem.isEmpty())
    {
      Problem = QStringLiteral("The synced data does not belong to this account.");
    }
    Dek.clear();
    return false;
  }
  return true;
}

void CloudSync::UnlockKey(const QString & Secret, bool IsRecoveryKey, const std::function<void(bool, const QString &)> & Done)
{
  auto Adopt = [this, Done](const QByteArray & Dek, const CloudCrypto::SnapshotHeader & Header)
  {
    FDek = Dek;
    FWrapDirty = false;
    FWrap = CloudCrypto::SnapshotHeader();
    FWrap.User = FUser;
    FWrap.Kdf = Header.Kdf;
    FWrap.DekPassword = Header.DekPassword;
    FWrap.DekRecovery = Header.DekRecovery;
    FSettingsBase = 0;
    FSettingsHash.clear();
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
      }, [Done, Adopt, Found, Problem](bool Any, qint64, const QByteArray &)
      {
        if (Any)
        {
          Adopt(Found->first, Found->second);
        }
        else
        {
          Done(false, Problem);
        }
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

void CloudSync::ChangePassword(const QString & NewPassword, const std::function<void(bool, const QString &)> & Done)
{
  if (!HasKey())
  {
    Done(false, QStringLiteral("Enter the current sync password first."));
    return;
  }
  CloudCrypto::SnapshotHeader Wrap;
  Wrap.User = FUser;
  QString Error;
  if (!CloudCrypto::WrapDek(Wrap, FDek, NewPassword, QByteArray(), &Error))
  {
    Done(false, Error);
    return;
  }
  // The recovery key wraps the same data key: it keeps working
  Wrap.DekRecovery = FWrap.DekRecovery;
  FWrap = Wrap;
  FWrapDirty = true;
  SaveState();
  SyncNow();
  Done(true, QString());
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
  File.WriteRaw(S, QStringLiteral("Dek"), Hex(FDek));
  File.WriteRaw(S, QStringLiteral("MemoryKib"), QString::number(FWrap.Kdf.MemoryKib));
  File.WriteRaw(S, QStringLiteral("Iterations"), QString::number(FWrap.Kdf.Iterations));
  File.WriteRaw(S, QStringLiteral("Lanes"), QString::number(FWrap.Kdf.Lanes));
  File.WriteRaw(S, QStringLiteral("Salt"), Hex(FWrap.Kdf.Salt));
  File.WriteRaw(S, QStringLiteral("DekPassword"), Hex(FWrap.DekPassword));
  File.WriteRaw(S, QStringLiteral("DekRecovery"), Hex(FWrap.DekRecovery));
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
  CloudCrypto::SnapshotHeader Wrap;
  Wrap.User = User;
  Wrap.Kdf.MemoryKib = Read("MemoryKib").toUInt();
  Wrap.Kdf.Iterations = Read("Iterations").toUInt();
  Wrap.Kdf.Lanes = Read("Lanes").toUInt();
  Wrap.Kdf.Salt = Hex("Salt");
  Wrap.DekPassword = Hex("DekPassword");
  Wrap.DekRecovery = Hex("DekRecovery");
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
  if ((Dek == FDek) && (User == FUser))
  {
    return kiImported;
  }
  // When signed out, the key waits for this account (CompleteSignIn keeps it)
  if (!SignedIn())
  {
    FUser = User;
    FEmail = KeyEmail;
  }
  FDek = Dek;
  FWrap = Wrap;
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

void CloudSync::FinishSync(TState State, const QString & Error)
{
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
  if (FSyncPending && (State == csIdle))
  {
    FSyncPending = false;
    QTimer::singleShot(0, this, &CloudSync::SyncNow);
  }
}

void CloudSync::RunSettingsSync(int Attempt)
{
  GetHead(SettingsSlot, [this, Attempt](bool Ok, const Head & Remote, const CloudClient::Reply & Reply)
  {
    if (!Ok)
    {
      if (Reply.Status == 401)
      {
        FBusy = false;
        UpdateReadyState();
        return;
      }
      FinishSync(csError, Reply.Error);
      return;
    }
    QByteArray Local = LocalSettings();
    QString LocalHash = CloudSettings::Hash(Local);
    TAction Action = Decide(FSettingsBase, FSettingsHash, Remote.Version, LocalHash);
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
        FinishSync(csError, QStringLiteral("The NixSCP cloud has older settings (v%1) than this computer already synced (v%2). "
          "Syncing is stopped to protect your settings; please contact support.").arg(Remote.Version).arg(FSettingsBase));
        break;
      case saConflict:
        if ((FSettingsBase == 0) && FSettingsHash.isEmpty())
        {
          FirstSync(Remote, Local, LocalHash);
        }
        else
        {
          FBusy = false;
          FConflictHead = Remote;
          SetState(csConflict);
          emit ConflictDetected(Remote.Device, Remote.UpdatedAt);
        }
        break;
    }
  });
}

void CloudSync::FirstSync(const Head & Remote, const QByteArray & Local, const QString & LocalHash)
{
  // This computer syncs for the first time and the cloud already has settings
  DownloadRaw(SettingsSlot, Remote.Version, [this, Remote, Local, LocalHash](bool Ok, const QByteArray & Data, const QString & Error)
  {
    CloudCrypto::SnapshotHeader Header;
    QByteArray Plaintext;
    QString Problem = Error;
    if (!Ok)
    {
      FinishSync(csError, Problem);
      return;
    }
    if (!Decode(Data, SettingsSlot, Remote.Version, Header, Plaintext, Problem))
    {
      HandleUndecryptable(Remote, CloudCrypto::ReadHeader(Data, Header) ? Header.Device : QString());
      return;
    }
    AdoptWrap(Header);
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
      FBusy = false;
      FConflictHead = Remote;
      SetState(csConflict);
      emit ConflictDetected(Remote.Device, Remote.UpdatedAt);
    }
  });
}

void CloudSync::PullSettings(const Head & Remote)
{
  DownloadRaw(SettingsSlot, Remote.Version, [this, Remote](bool Ok, const QByteArray & Data, const QString & Error)
  {
    CloudCrypto::SnapshotHeader Header;
    QByteArray Plaintext;
    QString Problem = Error;
    if (!Ok)
    {
      FinishSync(csError, Problem);
      return;
    }
    if (!Decode(Data, SettingsSlot, Remote.Version, Header, Plaintext, Problem))
    {
      HandleUndecryptable(Remote, CloudCrypto::ReadHeader(Data, Header) ? Header.Device : QString());
      return;
    }
    AdoptWrap(Header);
    ApplySettings(Plaintext, Remote.Version, Header.Device);
  });
}

void CloudSync::FindSnapshot(const QString & Slot, qint64 Below, int MaxTries,
  const std::function<bool(qint64, const QByteArray &)> & Accept, const FoundCallback & Done)
{
  FClient->Api("GET", QStringLiteral("/v1/sync/history?slot=%1").arg(Slot), [this, Slot, Below, MaxTries, Accept, Done](const CloudClient::Reply & Reply)
  {
    QList<qint64> Versions;
    QJsonDocument Doc = QJsonDocument::fromJson(Reply.Data);
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
    // Tried one after the other, newest first
    auto Next = std::make_shared<std::function<void(int)>>();
    *Next = [this, Slot, Versions, Accept, Done, Next](int Index)
    {
      if (Index >= Versions.size())
      {
        Done(false, 0, QByteArray());
        return;
      }
      qint64 Version = Versions[Index];
      DownloadRaw(Slot, Version, [Version, Index, Accept, Done, Next](bool Ok, const QByteArray & Data, const QString &)
      {
        if (Ok && Accept(Version, Data))
        {
          Done(true, Version, Data);
        }
        else
        {
          (*Next)(Index + 1);
        }
      });
    };
    (*Next)(0);
  });
}

void CloudSync::HandleUndecryptable(const Head & Remote, const QString & Device)
{
  // Does the key of this computer still decrypt an older version?
  FindSnapshot(SettingsSlot, Remote.Version, 5, [this](qint64 Version, const QByteArray & Data)
  {
    CloudCrypto::SnapshotHeader Header;
    QByteArray Plaintext;
    QString Error;
    return Decode(Data, SettingsSlot, Version, Header, Plaintext, Error);
  }, [this, Remote, Device](bool Found, qint64 Version, const QByteArray & Data)
  {
    if (!Found)
    {
      // Nothing decrypts: the account's key is not the one of this computer
      FDek.clear();
      FBusy = false;
      SaveState();
      SetState(csNeedKey, QStringLiteral("The synced settings cannot be decrypted with the key on this computer. Enter your sync password."));
      return;
    }
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
    }
    else if ((LocalHash == FSettingsHash) || (FirstTime && !HasStoredSites(FConfig->ExportData())))
    {
      // Nothing changed here (or a fresh installation): take the newest valid
      // settings, then put them on top of the foreign snapshot
      // A newer valid version from another computer: take it, then repair
      CloudCrypto::SnapshotHeader Header;
      QByteArray Plaintext;
      QString Error;
      Decode(Data, SettingsSlot, Version, Header, Plaintext, Error);
      AdoptWrap(Header);
      ApplySettings(Plaintext, Version, Header.Device, Remote.Version);
    }
    else if (FirstTime)
    {
      // First sync of a computer with its own sites: identical settings need no decision
      CloudCrypto::SnapshotHeader Header;
      QByteArray Plaintext;
      QString Error;
      Decode(Data, SettingsSlot, Version, Header, Plaintext, Error);
      AdoptWrap(Header);
      if (Plaintext == Local)
      {
        FSettingsBase = Version;
        FSettingsHash = LocalHash;
        PushSettings(Remote.Version, Local, LocalHash, 0);
      }
      else
      {
        FinishSync(csError, QStringLiteral("This computer's settings differ from the cloud, and the newest cloud upload is unreadable. "
          "Sync from another computer first; retrying later."));
      }
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
  QByteArray Snapshot = CloudCrypto::EncodeSnapshot(NewHeader(SettingsSlot, Base + 1), FDek, Plaintext);
  Upload(SettingsSlot, Base, Snapshot, [this, LocalHash, Attempt](int Status, qint64 Version, const Head &, const QString & Error)
  {
    if (Status == 200)
    {
      FSettingsBase = Version;
      FSettingsHash = LocalHash;
      FWrapDirty = false;
      FLicenseNotified = false;
      FinishSync(csIdle);
    }
    else if ((Status == 409) && (Attempt + 1 < MaxSyncAttempts))
    {
      // Another computer committed first: decide again (pull or conflict)
      RunSettingsSync(Attempt + 1);
    }
    else if (Status == 402)
    {
      FinishSync(csNoLicense);
    }
    else if (Status == 401)
    {
      FBusy = false;
      UpdateReadyState();
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
  SetState(csSyncing);
  if (Resolution == crUseCloud)
  {
    PullSettings(FConflictHead);
  }
  else
  {
    // Explicit overwrite on top of the cloud head; the old head stays in history
    QByteArray Local = LocalSettings();
    PushSettings(FConflictHead.Version, Local, CloudSettings::Hash(Local), 0);
  }
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
    File.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
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
  QByteArray Snapshot = CloudCrypto::EncodeSnapshot(Header, FDek, Payload);
  Upload(SessionSlot, FSessionBase, Snapshot, [this, Payload, SavedAt, Attempt, Done](int Status, qint64 Version, const Head & Conflict, const QString & Error)
  {
    if (Status == 200)
    {
      FSessionBase = Version;
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
    // Another computer saved its tabs too: the later capture wins
    DownloadRaw(SessionSlot, Conflict.Version, [this, Payload, SavedAt, Attempt, Done, Conflict](bool Ok, const QByteArray & Data, const QString &)
    {
      FSessionBase = Conflict.Version;
      // Only an authenticated time counts: a snapshot uploaded without the
      // sync password could otherwise claim to be newer forever
      CloudCrypto::SnapshotHeader Remote;
      QByteArray Plaintext;
      QString Error;
      if (Ok && Decode(Data, SessionSlot, Conflict.Version, Remote, Plaintext, Error) && Remote.SavedAt.isValid() && (Remote.SavedAt > SavedAt))
      {
        SaveState();
        Done(true, QString());
        return;
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
  GetHead(SessionSlot, [this, Finish](bool Ok, const Head & Remote, const CloudClient::Reply &)
  {
    if (!Ok)
    {
      Finish(true, QByteArray());
      return;
    }
    if (Remote.Version == 0)
    {
      Finish(false, QByteArray());
      return;
    }
    DownloadRaw(SessionSlot, Remote.Version, [this, Finish, Remote](bool Ok2, const QByteArray & Data, const QString &)
    {
      CloudCrypto::SnapshotHeader Header;
      QByteArray Plaintext;
      QString Error;
      if (!Ok2 || !Decode(Data, SessionSlot, Remote.Version, Header, Plaintext, Error))
      {
        Finish(true, QByteArray());
        return;
      }
      FSessionBase = Remote.Version;
      SaveState();
      WriteSessionCache(Plaintext);
      Finish(false, Plaintext);
    });
  });
}
