#include "core/TeamSync.h"
#include "core/CloudClient.h"
#include "core/CloudCrypto.h"
#include "core/CloudSettings.h"
#include "core/CloudSync.h"
#include "core/ConfigVault.h"
#include "core/Configuration.h"
#include "core/IniStorage.h"
#include "core/SessionData.h"

#include <QCryptographicHash>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QUrl>

namespace
{

QString B64(const QByteArray & Data)
{
  return QString::fromLatin1(Data.toBase64());
}

QByteArray UnB64(const QJsonValue & Value)
{
  return QByteArray::fromBase64(Value.toString().toLatin1());
}

QString ApiError(const CloudClient::Reply & Reply, const QString & What)
{
  QString Code = Reply.ErrorCode();
  if (Code == QStringLiteral("license_required")) return QStringLiteral("Teams need NixSCP Pro.");
  if (Code == QStringLiteral("role_required")) return QStringLiteral("Your role in the team does not allow this.");
  if (Code == QStringLiteral("invalid_invite")) return QStringLiteral("The invite code is not valid (wrong, used or expired).");
  if (Code == QStringLiteral("already_member")) return QStringLiteral("You are already a member of this team.");
  if (Code == QStringLiteral("team_full")) return QStringLiteral("The team is full.");
  if (Code == QStringLiteral("too_many_teams")) return QStringLiteral("You own too many teams.");
  if (Code == QStringLiteral("owner_cannot_leave")) return QStringLiteral("The owner cannot leave the team; delete it instead.");
  if (Code == QStringLiteral("cannot_remove_owner")) return QStringLiteral("The owner cannot be removed.");
  return QStringLiteral("%1: %2").arg(What, Reply.Error.isEmpty() ? QStringLiteral("error %1").arg(Reply.Status) : Reply.Error);
}

// Agent access is each person's own decision: never shared through a team
void ClearAgentSettings(SessionData & Data)
{
  SessionData Defaults;
  Data.AgentAccess = Defaults.AgentAccess;
  Data.AgentPermissions = Defaults.AgentPermissions;
  Data.AgentName = Defaults.AgentName;
  Data.AgentAllow = Defaults.AgentAllow;
  Data.AgentDeny = Defaults.AgentDeny;
  Data.AgentRecord = Defaults.AgentRecord;
}

}

TeamSync & TeamSync::Instance()
{
  static TeamSync * Sync = new TeamSync();
  return *Sync;
}

void TeamSync::Initialize(Configuration & Config, CloudSync & Sync)
{
  FConfig = &Config;
  FSync = &Sync;
  LoadStates();
}

QString TeamSync::FolderName(const QString & TeamName)
{
  QString Name = TeamName;
  Name.replace(QLatin1Char('/'), QLatin1Char('-'));
  return CloudSettings::TeamFolderPrefix + Name.trimmed();
}

const TeamSync::Team * TeamSync::TeamOfSite(const QString & SiteName) const
{
  for (auto It = FTeams.cbegin(); It != FTeams.cend(); ++It)
  {
    if (SiteName.startsWith(FolderName(It->Name) + QLatin1Char('/')))
    {
      return &It.value();
    }
  }
  return nullptr;
}

const TeamSync::Team * TeamSync::Find(const QString & Uuid) const
{
  auto It = FTeams.constFind(Uuid);
  return (It == FTeams.cend()) ? nullptr : &It.value();
}

bool TeamSync::Available() const
{
  return (FSync != nullptr) && FSync->SignedIn() && FSync->HasKey() && !FSync->UserUuid().isEmpty();
}

//---------------------------------------------------------------------------
// Local state

QString TeamSync::StateFileName() const
{
  return QFileInfo(FConfig->IniFileName()).absolutePath() + QStringLiteral("/nixscp-teams.ini");
}

void TeamSync::LoadStates()
{
  FStates.clear();
  if ((FConfig == nullptr) || FConfig->IsNulStorage())
  {
    return;
  }
  IniFile File;
  File.Load(StateFileName());
  for (const QString & Section : File.Sections())
  {
    if (!Section.startsWith(QStringLiteral("Team\\")))
    {
      continue;
    }
    State S;
    S.Base = File.ReadRaw(Section, QStringLiteral("Base")).toLongLong();
    S.Hash = File.ReadRaw(Section, QStringLiteral("Hash"));
    S.Folder = UnMungeStr(File.ReadRaw(Section, QStringLiteral("Folder")));
    FStates.insert(Section.mid(5), S);
  }
}

void TeamSync::SaveStates()
{
  if ((FConfig == nullptr) || FConfig->IsNulStorage())
  {
    return;
  }
  IniFile File;
  for (auto It = FStates.cbegin(); It != FStates.cend(); ++It)
  {
    QString Section = QStringLiteral("Team\\") + It.key();
    File.WriteRaw(Section, QStringLiteral("Base"), QString::number(It->Base));
    File.WriteRaw(Section, QStringLiteral("Hash"), It->Hash);
    File.WriteRaw(Section, QStringLiteral("Folder"), MungeStr(It->Folder, true));
  }
  File.Save(StateFileName());
}

//---------------------------------------------------------------------------
// Content

QByteArray TeamSync::LocalContent(const Team & T) const
{
  StoredSessionList Content;
  QString Prefix = FolderName(T.Name) + QLatin1Char('/');
  for (const auto & Data : FConfig->StoredSessions().Sessions())
  {
    if (Data->Name.startsWith(Prefix))
    {
      auto Copy = std::make_shared<SessionData>(*Data);
      Copy->Name = Data->Name.mid(Prefix.size());
      ClearAgentSettings(*Copy);
      Content.Add(Copy);
    }
  }
  IniFile File;
  Content.Save(File);
  IniFile Sites;
  for (const QString & Section : File.Sections())
  {
    if (Section.startsWith(QStringLiteral("Sessions\\")) && (Section != QStringLiteral("Sessions\\") + MungeStr(StoredSessionList::DefaultSettingsName(), false)))
    {
      for (const QString & Key : File.Keys(Section))
      {
        Sites.WriteRaw(Section, Key, File.ReadRaw(Section, Key));
      }
    }
  }
  return CloudSettings::Canonical(Sites);
}

void TeamSync::RemoveLocal(const QString & Folder)
{
  if (Folder.isEmpty())
  {
    return;
  }
  StoredSessionList & Sites = FConfig->StoredSessions();
  QStringList Names;
  for (const auto & Data : Sites.Sessions())
  {
    if (Data->Name.startsWith(Folder + QLatin1Char('/')))
    {
      Names.append(Data->Name);
    }
  }
  for (const QString & Name : Names)
  {
    Sites.Remove(Name);
  }
}

void TeamSync::SaveSites()
{
  FConfig->Save();
  // Save() signals only changes of another instance
  emit FConfig->StoredSessionsChanged();
}

// What tells a changed site apart for the people using it
static QString Summary(const SessionData & Data)
{
  return QStringList{Data.HostName, Data.UserName, QString::number(Data.PortNumber), Data.Password,
    Data.RemoteDirectory, Data.PublicKeyFile}.join(QLatin1Char('\n'));
}

void TeamSync::Apply(Team & T, const QByteArray & Content)
{
  QString Folder = FolderName(T.Name);
  StoredSessionList & Sites = FConfig->StoredSessions();
  // this person's own agent settings of the team's sites survive
  QHash<QString, SessionData> Own;
  for (const auto & Data : Sites.Sessions())
  {
    if (Data->Name.startsWith(Folder + QLatin1Char('/')))
    {
      Own.insert(Data->Name, *Data);
    }
  }
  // what the people using the sites see change
  QHash<QString, QString> Before;
  for (auto It = Own.cbegin(); It != Own.cend(); ++It)
  {
    Before.insert(It.key(), Summary(It.value()));
  }
  RemoveLocal(Folder);
  State & S = FStates[T.Uuid];
  if (!S.Folder.isEmpty() && (S.Folder != Folder))
  {
    RemoveLocal(S.Folder);   // the team was renamed
  }
  IniFile File;
  File.LoadFromData(Content);
  StoredSessionList Received;
  Received.Load(File);
  for (const auto & Data : Received.Sessions())
  {
    auto Copy = std::make_shared<SessionData>(*Data);
    Copy->Name = Folder + QLatin1Char('/') + Data->Name;
    ClearAgentSettings(*Copy);
    auto It = Own.constFind(Copy->Name);
    if (It != Own.cend())
    {
      Copy->AgentAccess = It->AgentAccess;
      Copy->AgentPermissions = It->AgentPermissions;
      Copy->AgentName = It->AgentName;
      Copy->AgentAllow = It->AgentAllow;
      Copy->AgentDeny = It->AgentDeny;
      Copy->AgentRecord = It->AgentRecord;
    }
    Sites.Add(Copy);
  }
  int Added = 0;
  int Changed = 0;
  QSet<QString> After;
  for (const auto & Data : Sites.Sessions())
  {
    if (Data->Name.startsWith(Folder + QLatin1Char('/')))
    {
      After.insert(Data->Name);
      auto It = Before.constFind(Data->Name);
      if (It == Before.cend())
      {
        Added++;
      }
      else if (*It != Summary(*Data))
      {
        Changed++;
      }
    }
  }
  int Removed = 0;
  for (auto It = Before.cbegin(); It != Before.cend(); ++It)
  {
    if (!After.contains(It.key()))
    {
      Removed++;
    }
  }
  SaveSites();
  // not for the first sync of a team (its sites arrive), nor after a rename
  if ((Added + Removed + Changed > 0) && (S.Base > 0) && (S.Folder.isEmpty() || (S.Folder == Folder)))
  {
    emit SitesReceived(T.Name, Added, Removed, Changed);
  }
  S.Folder = Folder;
  S.Hash = CloudSettings::Hash(LocalContent(T));
}

//---------------------------------------------------------------------------
// Sync

void TeamSync::Finish(bool Ok, const QString & Error)
{
  FSyncing = false;
  FLastError = Ok ? QString() : Error;
  SaveStates();
  QList<Done> Waiting;
  Waiting.swap(FWaiting);
  emit Changed();
  for (const Done & Callback : Waiting)
  {
    if (Callback)
    {
      Callback(Ok, Error);
    }
  }
}

void TeamSync::SyncAll(const Done & Callback)
{
  if (!Available())
  {
    if (Callback)
    {
      Callback(false, QStringLiteral("Sign in to NixSCP Pro and unlock cloud sync to use teams."));
    }
    return;
  }
  FWaiting.append(Callback);
  if (FSyncing)
  {
    return;
  }
  FSyncing = true;
  EnsureKeys([this](bool Ok, const QString & Error)
  {
    if (!Ok)
    {
      Finish(false, Error);
      return;
    }
    FetchTeams([this](bool Ok, const QString & Error)
    {
      if (!Ok)
      {
        Finish(false, Error);
        return;
      }
      SyncTeams(FTeams.keys(), [this](bool Ok, const QString & Error) { Finish(Ok, Error); });
    });
  });
}

void TeamSync::EnsureKeys(const Done & Callback)
{
  if (!FKeys.Private.isEmpty() && (FKeysUser == FSync->UserUuid()))
  {
    Callback(true, QString());
    return;
  }
  FSync->Client().Api("GET", QStringLiteral("/v1/keys/me"), [this, Callback](const CloudClient::Reply & Reply)
  {
    if (Reply.Ok())
    {
      QByteArray Private = TeamCrypto::UnwrapPrivateKey(FSync->DataKey(), UnB64(Reply.Json.value(QStringLiteral("wrapped_private_key"))));
      QByteArray Public = UnB64(Reply.Json.value(QStringLiteral("public_key")));
      if (Private.isEmpty() || (TeamCrypto::PublicKey(Private) != Public))
      {
        Callback(false, QStringLiteral("Your team key cannot be read with this computer's sync key."));
        return;
      }
      FKeys.Private = Private;
      FKeys.Public = Public;
      FKeysUser = FSync->UserUuid();
      Callback(true, QString());
      return;
    }
    if (Reply.Status != 404)
    {
      Callback(false, ApiError(Reply, QStringLiteral("Cannot read your team key")));
      return;
    }
    // the first time: a key pair for this account
    TeamCrypto::KeyPair Keys = TeamCrypto::NewKeyPair();
    QJsonObject Body{{QStringLiteral("public_key"), B64(Keys.Public)},
      {QStringLiteral("wrapped_private_key"), B64(TeamCrypto::WrapPrivateKey(FSync->DataKey(), Keys.Private))}};
    FSync->Client().Api("PUT", QStringLiteral("/v1/keys/me"), Body, [this, Keys, Callback](const CloudClient::Reply & Put)
    {
      if (!Put.Ok())
      {
        Callback(false, ApiError(Put, QStringLiteral("Cannot store your team key")));
        return;
      }
      FKeys = Keys;
      FKeysUser = FSync->UserUuid();
      Callback(true, QString());
    });
  });
}

void TeamSync::FetchTeams(const Done & Callback)
{
  FSync->Client().Api("GET", QStringLiteral("/v1/teams"), [this, Callback](const CloudClient::Reply & Reply)
  {
    if (!Reply.Ok())
    {
      Callback(false, ApiError(Reply, QStringLiteral("Cannot list your teams")));
      return;
    }
    QHash<QString, Team> Teams;
    for (const QJsonValue & V : QJsonDocument::fromJson(Reply.Data).array())
    {
      QJsonObject O = V.toObject();
      Team T;
      T.Uuid = O.value(QStringLiteral("uuid")).toString();
      T.Name = O.value(QStringLiteral("name")).toString();
      T.Role = O.value(QStringLiteral("role")).toString();
      T.KeyVersion = O.value(QStringLiteral("key_version")).toInteger();
      T.NeedsRotation = O.value(QStringLiteral("needs_rotation")).toBool();
      T.MemberCount = O.value(QStringLiteral("member_count")).toInt();
      T.HeadVersion = O.value(QStringLiteral("head")).toObject().value(QStringLiteral("version")).toInteger();
      qint64 MyVersion = O.value(QStringLiteral("my_key_version")).toInteger();
      QByteArray Wrap = UnB64(O.value(QStringLiteral("wrapped_team_key")));
      if (!Wrap.isEmpty() && (MyVersion == T.KeyVersion))
      {
        T.Key = TeamCrypto::UnwrapForMember(FKeys, Wrap, T.Uuid, T.KeyVersion);
      }
      Teams.insert(T.Uuid, T);
    }
    // teams this account no longer belongs to: their sites leave this computer
    for (auto It = FStates.begin(); It != FStates.end(); )
    {
      if (!Teams.contains(It.key()))
      {
        QString Folder = It->Folder;
        RemoveLocal(Folder);
        SaveSites();
        emit Notice(QStringLiteral("You are no longer a member of the team \"%1\"; its sites were removed from this computer.")
          .arg(Folder.mid(CloudSettings::TeamFolderPrefix.size())));
        It = FStates.erase(It);
      }
      else
      {
        ++It;
      }
    }
    FTeams = Teams;
    Callback(true, QString());
  });
}

void TeamSync::SyncTeams(QStringList Pending, const Done & Callback)
{
  if (Pending.isEmpty())
  {
    Callback(true, QString());
    return;
  }
  QString Uuid = Pending.takeFirst();
  SyncTeam(Uuid, [this, Pending, Callback](bool Ok, const QString & Error)
  {
    if (!Ok)
    {
      FLastError = Error;   // the other teams still sync
    }
    SyncTeams(Pending, Callback);
  });
}

void TeamSync::SyncTeam(const QString & Uuid, const Done & Callback)
{
  Team & T = FTeams[Uuid];
  if (T.NeedsRotation && T.CanEdit())
  {
    Rotate(Uuid, [this, Uuid, Callback](bool Ok, const QString & Error)
    {
      if (!Ok)
      {
        Callback(false, Error);
        return;
      }
      SyncTeam(Uuid, Callback);
    });
    return;
  }
  if (T.Key.isEmpty())
  {
    // invited but not given the key yet, or waiting for an admin's rotation
    Callback(true, QString());
    return;
  }
  State & S = FStates[Uuid];
  QString LocalHash = CloudSettings::Hash(LocalContent(T));
  if (T.HeadVersion > S.Base)
  {
    Pull(Uuid, T.HeadVersion, Callback);
  }
  else if (LocalHash != S.Hash)
  {
    if (T.CanEdit())
    {
      Push(Uuid, Callback);
    }
    else if (T.HeadVersion > 0)
    {
      // members use the team's sites as they are: local changes are reverted
      emit Notice(QStringLiteral("Changes to the sites of the team \"%1\" were reverted: only its owner and admins can edit them.").arg(T.Name));
      Pull(Uuid, T.HeadVersion, Callback);
    }
    else
    {
      Callback(true, QString());
    }
  }
  else
  {
    Callback(true, QString());
  }
}

void TeamSync::Pull(const QString & Uuid, qint64 Version, const Done & Callback)
{
  FSync->Client().Api("GET", QStringLiteral("/v1/teams/%1/sync/download-url?version=%2").arg(Uuid).arg(Version),
    [this, Uuid, Version, Callback](const CloudClient::Reply & Reply)
  {
    if (!Reply.Ok())
    {
      Callback(false, ApiError(Reply, QStringLiteral("Cannot download the team's sites")));
      return;
    }
    QString Sha = Reply.Json.value(QStringLiteral("sha256")).toString();
    FSync->Client().Transfer("GET", QUrl(Reply.Json.value(QStringLiteral("url")).toString()), QByteArray(), {},
      [this, Uuid, Version, Sha, Callback](const CloudClient::Reply & Get)
    {
      if (!Get.Ok() || (QString::fromLatin1(QCryptographicHash::hash(Get.Data, QCryptographicHash::Sha256).toHex()) != Sha))
      {
        Callback(false, QStringLiteral("Downloading the team's sites failed."));
        return;
      }
      if (!FTeams.contains(Uuid))
      {
        Callback(true, QString());
        return;
      }
      Team & T = FTeams[Uuid];
      CloudCrypto::SnapshotHeader Header;
      QByteArray Content;
      QString Error;
      if (!CloudCrypto::DecodeSnapshot(Get.Data, T.Key, Header, Content, &Error) || !Header.IsTeam() ||
          (Header.User != T.Uuid) || (Header.Version != Version) || (Header.KeyVersion != T.KeyVersion))
      {
        // still under a previous key (an admin rotates and pushes again)
        if (T.CanEdit())
        {
          Push(Uuid, Callback);
          return;
        }
        Callback(true, QString());
        return;
      }
      Apply(T, Content);
      FStates[Uuid].Base = Version;
      Callback(true, QString());
    });
  });
}

void TeamSync::Push(const QString & Uuid, const Done & Callback)
{
  Team & T = FTeams[Uuid];
  State & S = FStates[Uuid];
  QByteArray Content = LocalContent(T);
  CloudCrypto::SnapshotHeader Header;
  Header.Slot = QStringLiteral("team");
  Header.User = T.Uuid;
  Header.Version = T.HeadVersion + 1;
  Header.KeyVersion = T.KeyVersion;
  Header.SavedAt = QDateTime::currentDateTimeUtc();
  Header.Device = CloudSync::DeviceName();
  QByteArray Snapshot = CloudCrypto::EncodeSnapshot(Header, T.Key, Content);
  qint64 Base = T.HeadVersion;
  FSync->Client().Api("POST", QStringLiteral("/v1/teams/%1/sync/upload-url").arg(Uuid),
    QJsonObject{{QStringLiteral("base_version"), Base}, {QStringLiteral("size"), Snapshot.size()}},
    [this, Uuid, Base, Snapshot, Content, Callback](const CloudClient::Reply & Reply)
  {
    if (!Reply.Ok())
    {
      Callback(false, ApiError(Reply, QStringLiteral("Cannot upload the team's sites")));
      return;
    }
    QString ObjectKey = Reply.Json.value(QStringLiteral("object_key")).toString();
    FSync->Client().Transfer("PUT", QUrl(Reply.Json.value(QStringLiteral("url")).toString()), Snapshot,
      {{"Content-Type", "application/octet-stream"}}, [this, Uuid, Base, Snapshot, Content, ObjectKey, Callback](const CloudClient::Reply & Put)
    {
      if (!Put.Ok())
      {
        Callback(false, QStringLiteral("Uploading the team's sites failed (%1).").arg(Put.Status));
        return;
      }
      QJsonObject Commit{{QStringLiteral("base_version"), Base}, {QStringLiteral("object_key"), ObjectKey},
        {QStringLiteral("size"), Snapshot.size()}, {QStringLiteral("key_version"), FTeams.value(Uuid).KeyVersion},
        {QStringLiteral("sha256"), QString::fromLatin1(QCryptographicHash::hash(Snapshot, QCryptographicHash::Sha256).toHex())}};
      FSync->Client().Api("POST", QStringLiteral("/v1/teams/%1/sync/commit").arg(Uuid), Commit,
        [this, Uuid, Content, Callback](const CloudClient::Reply & Result)
      {
        if (Result.Status == 409)
        {
          // someone else changed the team first: their version wins here
          qint64 Head = Result.Json.value(QStringLiteral("head")).toObject().value(QStringLiteral("version")).toInteger();
          emit Notice(QStringLiteral("Another admin changed the sites of the team \"%1\" at the same time; their version was kept.")
            .arg(FTeams.value(Uuid).Name));
          if (Head > 0)
          {
            FTeams[Uuid].HeadVersion = Head;
            Pull(Uuid, Head, Callback);
            return;
          }
          Callback(false, ApiError(Result, QStringLiteral("Cannot save the team's sites")));
          return;
        }
        if (!Result.Ok())
        {
          Callback(false, ApiError(Result, QStringLiteral("Cannot save the team's sites")));
          return;
        }
        Team & T = FTeams[Uuid];
        T.HeadVersion = Result.Json.value(QStringLiteral("version")).toInteger();
        State & S = FStates[Uuid];
        S.Base = T.HeadVersion;
        S.Hash = CloudSettings::Hash(Content);
        S.Folder = FolderName(T.Name);
        Callback(true, QString());
      });
    });
  });
  Q_UNUSED(S);
}

void TeamSync::Rotate(const QString & Uuid, const Done & Callback)
{
  Members(Uuid, [this, Uuid, Callback](bool Ok, const QString & Error, const QJsonArray & List)
  {
    if (!Ok)
    {
      Callback(false, Error);
      return;
    }
    Team & T = FTeams[Uuid];
    qint64 NewVersion = T.KeyVersion + 1;
    QByteArray NewKey = TeamCrypto::NewTeamKey();
    QJsonArray Wraps;
    for (const QJsonValue & V : List)
    {
      QJsonObject M = V.toObject();
      QByteArray Public = UnB64(M.value(QStringLiteral("public_key")));
      Wraps.append(QJsonObject{{QStringLiteral("user_uuid"), M.value(QStringLiteral("user_uuid")).toString()},
        {QStringLiteral("wrapped_team_key"), B64(TeamCrypto::WrapForMember(Public, NewKey, Uuid, NewVersion))}});
    }
    FSync->Client().Api("POST", QStringLiteral("/v1/teams/%1/rotate").arg(Uuid),
      QJsonObject{{QStringLiteral("key_version"), NewVersion}, {QStringLiteral("wraps"), Wraps}},
      [this, Uuid, NewVersion, NewKey, Callback](const CloudClient::Reply & Reply)
    {
      if (!Reply.Ok())
      {
        Callback(false, ApiError(Reply, QStringLiteral("Cannot renew the team key")));
        return;
      }
      Team & T = FTeams[Uuid];
      T.KeyVersion = NewVersion;
      T.Key = NewKey;
      T.NeedsRotation = false;
      // the content again, under the new key (removed members cannot read it)
      Push(Uuid, Callback);
    });
  });
}

//---------------------------------------------------------------------------
// Team management

void TeamSync::CreateTeam(const QString & Name, const Done & Callback)
{
  EnsureKeys([this, Name, Callback](bool Ok, const QString & Error)
  {
    if (!Ok)
    {
      Callback(false, Error);
      return;
    }
    QByteArray Key = TeamCrypto::NewTeamKey();
    // the uuid comes from the server, but the wrap is bound to it: create, then store the wrap
    FSync->Client().Api("POST", QStringLiteral("/v1/teams"),
      QJsonObject{{QStringLiteral("name"), Name.trimmed()}, {QStringLiteral("wrapped_team_key"), B64(QByteArray(TeamCrypto::KeyLength + 60, '\0'))}},
      [this, Key, Callback](const CloudClient::Reply & Reply)
    {
      if (!Reply.Ok())
      {
        Callback(false, ApiError(Reply, QStringLiteral("Cannot create the team")));
        return;
      }
      QString Uuid = Reply.Json.value(QStringLiteral("uuid")).toString();
      qint64 Version = Reply.Json.value(QStringLiteral("key_version")).toInteger(1);
      FSync->Client().Api("PUT", QStringLiteral("/v1/teams/%1/members/me/key").arg(Uuid),
        QJsonObject{{QStringLiteral("wrapped_team_key"), B64(TeamCrypto::WrapForMember(FKeys.Public, Key, Uuid, Version))},
          {QStringLiteral("key_version"), Version}},
        [this, Callback](const CloudClient::Reply & Put)
      {
        if (!Put.Ok())
        {
          Callback(false, ApiError(Put, QStringLiteral("Cannot store the team key")));
          return;
        }
        SyncAll(Callback);
      });
    });
  });
}

void TeamSync::Rename(const QString & Uuid, const QString & Name, const Done & Callback)
{
  FSync->Client().Api("PATCH", QStringLiteral("/v1/teams/%1").arg(Uuid), QJsonObject{{QStringLiteral("name"), Name.trimmed()}},
    [this, Uuid, Name, Callback](const CloudClient::Reply & Reply)
  {
    if (!Reply.Ok())
    {
      Callback(false, ApiError(Reply, QStringLiteral("Cannot rename the team")));
      return;
    }
    // the local folder follows the name
    if (FTeams.contains(Uuid))
    {
      QString From = FolderName(FTeams[Uuid].Name);
      QString To = FolderName(Name);
      FConfig->StoredSessions().RenameFolder(From, To);
      SaveSites();
      FTeams[Uuid].Name = Name.trimmed();
      FStates[Uuid].Folder = To;
    }
    SyncAll(Callback);
  });
}

void TeamSync::DeleteTeam(const QString & Uuid, const Done & Callback)
{
  FSync->Client().Api("DELETE", QStringLiteral("/v1/teams/%1").arg(Uuid), [this, Uuid, Callback](const CloudClient::Reply & Reply)
  {
    if (!Reply.Ok())
    {
      Callback(false, ApiError(Reply, QStringLiteral("Cannot delete the team")));
      return;
    }
    RemoveLocal(FStates.value(Uuid).Folder);
    SaveSites();
    FStates.remove(Uuid);
    FTeams.remove(Uuid);
    SyncAll(Callback);
  });
}

void TeamSync::Leave(const QString & Uuid, const Done & Callback)
{
  FSync->Client().Api("DELETE", QStringLiteral("/v1/teams/%1/members/me").arg(Uuid), [this, Uuid, Callback](const CloudClient::Reply & Reply)
  {
    if (!Reply.Ok())
    {
      Callback(false, ApiError(Reply, QStringLiteral("Cannot leave the team")));
      return;
    }
    RemoveLocal(FStates.value(Uuid).Folder);
    SaveSites();
    FStates.remove(Uuid);
    FTeams.remove(Uuid);
    SyncAll(Callback);
  });
}

void TeamSync::ShareFolder(const QString & Uuid, const QString & Folder, const Done & Callback)
{
  const Team * T = Find(Uuid);
  if ((T == nullptr) || !T->CanEdit() || T->Key.isEmpty())
  {
    Callback(false, QStringLiteral("Only owners and admins can add sites to a team."));
    return;
  }
  QString Leaf = Folder.section(QLatin1Char('/'), -1);
  QString Target = FolderName(T->Name) + QLatin1Char('/') + Leaf;
  FConfig->StoredSessions().RenameFolder(Folder, Target);
  SaveSites();
  SyncAll(Callback);
}

QStringList TeamSync::SharedSites(const QString & Uuid) const
{
  QStringList Result;
  const Team * T = Find(Uuid);
  if (T == nullptr)
  {
    return Result;
  }
  QString Prefix = FolderName(T->Name) + QLatin1Char('/');
  for (const auto & Data : FConfig->StoredSessions().Sessions())
  {
    if (Data->Name.startsWith(Prefix) && !Data->IsWorkspace)
    {
      Result.append(Data->Name.mid(Prefix.size()));
    }
  }
  std::sort(Result.begin(), Result.end(), [](const QString & A, const QString & B)
  {
    return StoredSessionList::CompareLogicalText(A, B) < 0;
  });
  return Result;
}

void TeamSync::ShareSites(const QString & Uuid, const QStringList & Names, const QString & Subfolder, bool Move, const Done & Callback)
{
  const Team * T = Find(Uuid);
  if ((T == nullptr) || !T->CanEdit() || T->Key.isEmpty())
  {
    Callback(false, QStringLiteral("Only owners and admins can add sites to a team."));
    return;
  }
  QString Target = FolderName(T->Name);
  QString Sub = Subfolder.trimmed();
  while (Sub.startsWith(QLatin1Char('/'))) Sub.remove(0, 1);
  while (Sub.endsWith(QLatin1Char('/'))) Sub.chop(1);
  if (!Sub.isEmpty())
  {
    Target += QLatin1Char('/') + Sub;
  }
  StoredSessionList & Sites = FConfig->StoredSessions();
  int Count = 0;
  for (const QString & Name : Names)
  {
    std::shared_ptr<SessionData> Data = Sites.FindByName(Name);
    if ((Data == nullptr) || Data->IsWorkspace || Name.startsWith(CloudSettings::TeamFolderPrefix))
    {
      continue;
    }
    auto Copy = std::make_shared<SessionData>(*Data);
    Copy->Name = Target + QLatin1Char('/') + Name.section(QLatin1Char('/'), -1);
    Copy->MakeUniqueIn(Sites);
    if (Move)
    {
      Sites.Remove(Name);
    }
    Sites.Add(Copy);
    Count++;
  }
  if (Count == 0)
  {
    Callback(false, QStringLiteral("There is no site to share (sites of a team or workspaces cannot be shared)."));
    return;
  }
  SaveSites();
  SyncAll(Callback);
}

void TeamSync::Unshare(const QString & Uuid, const QStringList & Names, bool KeepCopy, const Done & Callback)
{
  const Team * T = Find(Uuid);
  if ((T == nullptr) || !T->CanEdit() || T->Key.isEmpty())
  {
    Callback(false, QStringLiteral("Only owners and admins can stop sharing a team's sites."));
    return;
  }
  QString Prefix = FolderName(T->Name) + QLatin1Char('/');
  StoredSessionList & Sites = FConfig->StoredSessions();
  QStringList Selected;
  for (const auto & Data : Sites.Sessions())
  {
    if (!Data->Name.startsWith(Prefix))
    {
      continue;
    }
    QString Relative = Data->Name.mid(Prefix.size());
    for (const QString & Name : Names)
    {
      if ((Relative == Name) || (Name.endsWith(QLatin1Char('/')) && Relative.startsWith(Name)))
      {
        Selected.append(Data->Name);
        break;
      }
    }
  }
  if (Selected.isEmpty())
  {
    Callback(false, QStringLiteral("The selected sites are not shared in this team."));
    return;
  }
  for (const QString & Name : Selected)
  {
    std::shared_ptr<SessionData> Data = Sites.FindByName(Name);
    if (KeepCopy)
    {
      auto Copy = std::make_shared<SessionData>(*Data);
      Copy->Name = Name.mid(Prefix.size());
      Sites.Remove(Name);
      Copy->MakeUniqueIn(Sites);
      Sites.Add(Copy);
    }
    else
    {
      Sites.Remove(Name);
    }
  }
  SaveSites();
  SyncAll(Callback);
}

void TeamSync::CreateInvite(const QString & Uuid, const QString & Role, int Days,
  const std::function<void(bool, const QString &, const QString &)> & Callback)
{
  const Team * T = Find(Uuid);
  if ((T == nullptr) || T->Key.isEmpty())
  {
    Callback(false, QStringLiteral("The team key is not available on this computer yet."), QString());
    return;
  }
  TeamCrypto::Invite Code = TeamCrypto::NewInvite();
  QJsonObject Body{
    {QStringLiteral("id"), Code.Id},
    {QStringLiteral("role"), Role},
    {QStringLiteral("wrapped_team_key"), B64(TeamCrypto::WrapForInvite(Code, T->Key, Uuid, T->KeyVersion))},
    {QStringLiteral("key_version"), T->KeyVersion},
    {QStringLiteral("verifier_hash"), TeamCrypto::InviteVerifierHash(Code)},
    {QStringLiteral("expires_in_days"), Days},
  };
  FSync->Client().Api("POST", QStringLiteral("/v1/teams/%1/invites").arg(Uuid), Body, [Code, Callback](const CloudClient::Reply & Reply)
  {
    if (!Reply.Ok())
    {
      Callback(false, ApiError(Reply, QStringLiteral("Cannot create the invite")), QString());
      return;
    }
    Callback(true, QString(), Code.Code());
  });
}

void TeamSync::Invites(const QString & Uuid, const std::function<void(bool, const QString &, const QJsonArray &)> & Callback)
{
  FSync->Client().Api("GET", QStringLiteral("/v1/teams/%1/invites").arg(Uuid), [Callback](const CloudClient::Reply & Reply)
  {
    Callback(Reply.Ok(), Reply.Ok() ? QString() : ApiError(Reply, QStringLiteral("Cannot list the invites")),
      QJsonDocument::fromJson(Reply.Data).array());
  });
}

void TeamSync::RevokeInvite(const QString & Uuid, const QString & Id, const Done & Callback)
{
  FSync->Client().Api("DELETE", QStringLiteral("/v1/teams/%1/invites/%2").arg(Uuid, Id), [Callback](const CloudClient::Reply & Reply)
  {
    Callback(Reply.Ok(), Reply.Ok() ? QString() : ApiError(Reply, QStringLiteral("Cannot revoke the invite")));
  });
}

void TeamSync::JoinWithCode(const QString & Text, const Done & Callback)
{
  TeamCrypto::Invite Code;
  if (!TeamCrypto::ParseInviteCode(Text, Code))
  {
    Callback(false, QStringLiteral("This is not a NixSCP team invite code."));
    return;
  }
  EnsureKeys([this, Code, Callback](bool Ok, const QString & Error)
  {
    if (!Ok)
    {
      Callback(false, Error);
      return;
    }
    FSync->Client().Api("POST", QStringLiteral("/v1/invites/%1/accept").arg(Code.Id),
      QJsonObject{{QStringLiteral("verifier"), B64(TeamCrypto::InviteVerifier(Code))}},
      [this, Code, Callback](const CloudClient::Reply & Reply)
    {
      if (!Reply.Ok())
      {
        Callback(false, ApiError(Reply, QStringLiteral("Cannot join the team")));
        return;
      }
      QString Uuid = Reply.Json.value(QStringLiteral("team")).toObject().value(QStringLiteral("uuid")).toString();
      qint64 Version = Reply.Json.value(QStringLiteral("key_version")).toInteger();
      QByteArray Key = TeamCrypto::UnwrapForInvite(Code, UnB64(Reply.Json.value(QStringLiteral("wrapped_team_key"))), Uuid, Version);
      if (Key.isEmpty())
      {
        Callback(false, QStringLiteral("The invite code does not fit the team's key."));
        return;
      }
      // from now on the key comes wrapped for this account
      FSync->Client().Api("PUT", QStringLiteral("/v1/teams/%1/members/me/key").arg(Uuid),
        QJsonObject{{QStringLiteral("wrapped_team_key"), B64(TeamCrypto::WrapForMember(FKeys.Public, Key, Uuid, Version))},
          {QStringLiteral("key_version"), Version}},
        [this, Callback](const CloudClient::Reply & Put)
      {
        if (!Put.Ok())
        {
          Callback(false, ApiError(Put, QStringLiteral("Cannot store the team key")));
          return;
        }
        SyncAll(Callback);
      });
    });
  });
}

void TeamSync::Members(const QString & Uuid, const std::function<void(bool, const QString &, const QJsonArray &)> & Callback)
{
  FSync->Client().Api("GET", QStringLiteral("/v1/teams/%1/members").arg(Uuid), [Callback](const CloudClient::Reply & Reply)
  {
    Callback(Reply.Ok(), Reply.Ok() ? QString() : ApiError(Reply, QStringLiteral("Cannot list the members")),
      QJsonDocument::fromJson(Reply.Data).array());
  });
}

void TeamSync::SetRole(const QString & Uuid, const QString & UserUuid, const QString & Role, const Done & Callback)
{
  FSync->Client().Api("PATCH", QStringLiteral("/v1/teams/%1/members/%2").arg(Uuid, UserUuid), QJsonObject{{QStringLiteral("role"), Role}},
    [Callback](const CloudClient::Reply & Reply)
  {
    Callback(Reply.Ok(), Reply.Ok() ? QString() : ApiError(Reply, QStringLiteral("Cannot change the role")));
  });
}

void TeamSync::RemoveMember(const QString & Uuid, const QString & UserUuid, const Done & Callback)
{
  FSync->Client().Api("DELETE", QStringLiteral("/v1/teams/%1/members/%2").arg(Uuid, UserUuid),
    [this, Callback](const CloudClient::Reply & Reply)
  {
    if (!Reply.Ok())
    {
      Callback(false, ApiError(Reply, QStringLiteral("Cannot remove the member")));
      return;
    }
    // the next sync renews the key (needs_rotation) and pushes the sites again
    SyncAll(Callback);
  });
}
