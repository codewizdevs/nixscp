#include "core/TeamSync.h"
#include "core/CloudClient.h"
#include "core/CloudCrypto.h"
#include "core/CloudSettings.h"
#include "core/CloudSync.h"
#include "core/ConfigVault.h"
#include "core/Configuration.h"
#include "core/IniStorage.h"
#include "core/SessionData.h"
#include "core/TeamContent.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QSet>
#include <QUrl>
#include <QUuid>

namespace
{

constexpr int MaxAttempts = 5;

QString B64(const QByteArray & Data)
{
  return QString::fromLatin1(Data.toBase64());
}

QByteArray UnB64(const QJsonValue & Value)
{
  return QByteArray::fromBase64(Value.toString().toLatin1());
}

QString Sha256(const QByteArray & Data)
{
  return QString::fromLatin1(QCryptographicHash::hash(Data, QCryptographicHash::Sha256).toHex());
}

QString ApiError(const CloudClient::Reply & Reply, const QString & What)
{
  QString Code = Reply.ErrorCode();
  if (Code == QStringLiteral("license_required")) return QStringLiteral("Teams need NixSCP Pro.");
  if (Code == QStringLiteral("email_not_verified")) return QStringLiteral("Verify your e-mail address on the website first.");
  if (Code == QStringLiteral("role_required")) return QStringLiteral("Your role in the team does not allow this.");
  if (Code == QStringLiteral("invalid_invite")) return QStringLiteral("The invite code is not valid (wrong, used or expired).");
  if (Code == QStringLiteral("already_member")) return QStringLiteral("You are already a member of this team.");
  if (Code == QStringLiteral("team_full")) return QStringLiteral("The team is full.");
  if (Code == QStringLiteral("too_many_teams")) return QStringLiteral("You own too many teams.");
  if (Code == QStringLiteral("owner_cannot_leave")) return QStringLiteral("The owner cannot leave the team; delete it instead.");
  if (Code == QStringLiteral("cannot_remove_owner")) return QStringLiteral("The owner cannot be removed.");
  if (Code == QStringLiteral("head_conflict")) return QStringLiteral("The team's sites must be in sync first; if they cannot be verified, restore a version.");
  if (Code == QStringLiteral("rotation_required")) return QStringLiteral("The team key must be renewed first; this happens at the next sync.");
  if (Code == QStringLiteral("roster_full")) return QStringLiteral("The team's member list is full; create a new team.");
  if (Code == QStringLiteral("invalid_public_key")) return QStringLiteral("This account's team key is not usable.");
  return QStringLiteral("%1: %2").arg(What, Reply.Error.isEmpty() ? QStringLiteral("error %1").arg(Reply.Status) : Reply.Error);
}

bool Belongs(const QString & SiteName, const QString & Folder)
{
  // Exact: what belongs to a team is never a matter of folding names. Folders that
  // fold to the same name are kept apart instead (UniqueFolder, KeepFolderApart).
  return !Folder.isEmpty() && (SiteName.size() > Folder.size() + 1) && (SiteName.at(Folder.size()) == QLatin1Char('/')) &&
    SiteName.startsWith(Folder);
}

QString Relative(const QString & SiteName, const QString & Folder)
{
  return SiteName.mid(Folder.size() + 1);
}

QString Placeholder(const QString & Uuid)
{
  return QStringLiteral("Team %1").arg(Uuid.left(8));
}

QString JsonOfMap(const QHash<QString, QString> & Map)
{
  QJsonObject Json;
  for (auto It = Map.cbegin(); It != Map.cend(); ++It)
  {
    Json.insert(It.key(), It.value());
  }
  return B64(QJsonDocument(Json).toJson(QJsonDocument::Compact));
}

QHash<QString, QString> MapOfJson(const QString & Text)
{
  QHash<QString, QString> Result;
  QJsonObject Json = QJsonDocument::fromJson(QByteArray::fromBase64(Text.toLatin1())).object();
  for (auto It = Json.constBegin(); It != Json.constEnd(); ++It)
  {
    Result.insert(It.key(), It.value().toString());
  }
  return Result;
}

QByteArray OldKeyAad(const QString & Uuid, qint64 Version)
{
  return QByteArrayLiteral("NXT2-OLD-KEY|") + Uuid.toUtf8() + '|' + QByteArray::number(Version);
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
  FTeams.clear();
  FRosters.clear();
  FGone.clear();
  FRestoring.clear();
  LoadStates();
}

QString TeamSync::FolderName(const QString & TeamName)
{
  QString Name = TeamName;
  Name.replace(QLatin1Char('/'), QLatin1Char('-'));
  Name.replace(QLatin1Char('\\'), QLatin1Char('-'));
  return CloudSettings::TeamFolderPrefix + Name.trimmed();
}

const TeamSync::Team * TeamSync::TeamOfSite(const QString & SiteName) const
{
  for (auto It = FTeams.cbegin(); It != FTeams.cend(); ++It)
  {
    if (Belongs(SiteName, It->Folder))
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

QString TeamSync::Fingerprint() const
{
  return FIdentity.IsValid() ? TeamCrypto::Fingerprint(FIdentity.Public) : QString();
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
  FPinnedIdentities.clear();
  if ((FConfig == nullptr) || FConfig->IsNulStorage())
  {
    return;
  }
  IniFile File;
  // Locked (encrypted, no key yet): nothing may overwrite it before it was read
  FStatesLoaded = File.Load(StateFileName()) || !QFile::exists(StateFileName());
  for (const QString & Section : File.Sections())
  {
    if (Section.startsWith(QStringLiteral("Identity\\")))
    {
      FPinnedIdentities.insert(Section.mid(9), File.ReadRaw(Section, QStringLiteral("Public")));
      continue;
    }
    if (!Section.startsWith(QStringLiteral("Team2\\")))
    {
      continue;
    }
    State S;
    S.Base = File.ReadRaw(Section, QStringLiteral("Base")).toLongLong();
    S.Hash = File.ReadRaw(Section, QStringLiteral("Hash"));
    S.BaseSites = MapOfJson(File.ReadRaw(Section, QStringLiteral("Sites")));
    S.BaseCritical = MapOfJson(File.ReadRaw(Section, QStringLiteral("Critical")));
    S.Role = File.ReadRaw(Section, QStringLiteral("Role"));
    S.Detached = File.ReadRaw(Section, QStringLiteral("Detached")) == QStringLiteral("1");
    S.Ignored = File.ReadRaw(Section, QStringLiteral("Ignored")) == QStringLiteral("1");
    S.Folder = UnMungeStr(File.ReadRaw(Section, QStringLiteral("Folder")));
    S.Name = UnMungeStr(File.ReadRaw(Section, QStringLiteral("Name")));
    S.PendingName = UnMungeStr(File.ReadRaw(Section, QStringLiteral("PendingName")));
    S.NeedsPush = File.ReadRaw(Section, QStringLiteral("NeedsPush")) == QStringLiteral("1");
    S.AllowEmpty = File.ReadRaw(Section, QStringLiteral("AllowEmpty")) == QStringLiteral("1");
    S.Moves = MapOfJson(File.ReadRaw(Section, QStringLiteral("Moves")));
    S.ConflictVersion = File.ReadRaw(Section, QStringLiteral("ConflictVersion")).toLongLong();
    S.EmptyConflict = File.ReadRaw(Section, QStringLiteral("EmptyConflict")) == QStringLiteral("1");
    S.PinVersion = File.ReadRaw(Section, QStringLiteral("PinVersion")).toLongLong();
    S.PinSha = File.ReadRaw(Section, QStringLiteral("PinSha"));
    S.PinKeyVersion = File.ReadRaw(Section, QStringLiteral("PinKeyVersion")).toLongLong();
    S.RosterSeq = File.ReadRaw(Section, QStringLiteral("RosterSeq")).toLongLong();
    S.RosterHash = File.ReadRaw(Section, QStringLiteral("RosterHash"));
    for (const QString & Key : File.Keys(Section))
    {
      if (Key.startsWith(QStringLiteral("OldKey")))
      {
        S.OldKeys.insert(Key.mid(6).toLongLong(), QByteArray::fromBase64(File.ReadRaw(Section, Key).toLatin1()));
      }
    }
    FStates.insert(Section.mid(6), S);
  }
  FSavedStates = File.SaveToData();
}

void TeamSync::SaveStates()
{
  if ((FConfig == nullptr) || FConfig->IsNulStorage() || !FStatesLoaded)
  {
    return;
  }
  IniFile File;
  for (auto It = FPinnedIdentities.cbegin(); It != FPinnedIdentities.cend(); ++It)
  {
    File.WriteRaw(QStringLiteral("Identity\\") + It.key(), QStringLiteral("Public"), It.value());
  }
  for (auto It = FStates.cbegin(); It != FStates.cend(); ++It)
  {
    QString Section = QStringLiteral("Team2\\") + It.key();
    File.WriteRaw(Section, QStringLiteral("Base"), QString::number(It->Base));
    File.WriteRaw(Section, QStringLiteral("Hash"), It->Hash);
    File.WriteRaw(Section, QStringLiteral("Sites"), JsonOfMap(It->BaseSites));
    File.WriteRaw(Section, QStringLiteral("Critical"), JsonOfMap(It->BaseCritical));
    File.WriteRaw(Section, QStringLiteral("Role"), It->Role);
    File.WriteRaw(Section, QStringLiteral("Detached"), It->Detached ? QStringLiteral("1") : QStringLiteral("0"));
    File.WriteRaw(Section, QStringLiteral("Ignored"), It->Ignored ? QStringLiteral("1") : QStringLiteral("0"));
    File.WriteRaw(Section, QStringLiteral("Folder"), MungeStr(It->Folder, true));
    File.WriteRaw(Section, QStringLiteral("Name"), MungeStr(It->Name, true));
    File.WriteRaw(Section, QStringLiteral("PendingName"), MungeStr(It->PendingName, true));
    File.WriteRaw(Section, QStringLiteral("NeedsPush"), It->NeedsPush ? QStringLiteral("1") : QStringLiteral("0"));
    File.WriteRaw(Section, QStringLiteral("AllowEmpty"), It->AllowEmpty ? QStringLiteral("1") : QStringLiteral("0"));
    File.WriteRaw(Section, QStringLiteral("Moves"), JsonOfMap(It->Moves));
    File.WriteRaw(Section, QStringLiteral("ConflictVersion"), QString::number(It->ConflictVersion));
    File.WriteRaw(Section, QStringLiteral("EmptyConflict"), It->EmptyConflict ? QStringLiteral("1") : QStringLiteral("0"));
    File.WriteRaw(Section, QStringLiteral("PinVersion"), QString::number(It->PinVersion));
    File.WriteRaw(Section, QStringLiteral("PinSha"), It->PinSha);
    File.WriteRaw(Section, QStringLiteral("PinKeyVersion"), QString::number(It->PinKeyVersion));
    File.WriteRaw(Section, QStringLiteral("RosterSeq"), QString::number(It->RosterSeq));
    File.WriteRaw(Section, QStringLiteral("RosterHash"), It->RosterHash);
    for (auto Key = It->OldKeys.cbegin(); Key != It->OldKeys.cend(); ++Key)
    {
      File.WriteRaw(Section, QStringLiteral("OldKey%1").arg(Key.key()), B64(Key.value()));
    }
  }
  QByteArray Data = File.SaveToData();
  // Written every minute otherwise, and it may be large
  if ((Data != FSavedStates) && File.Save(StateFileName()))
  {
    FSavedStates = Data;
  }
}

TeamSync::State & TeamSync::StateOf(const QString & Uuid)
{
  return FStates[FAccount + QLatin1Char('/') + Uuid];
}

const TeamSync::State * TeamSync::FindState(const QString & Uuid) const
{
  auto It = FStates.constFind(FAccount + QLatin1Char('/') + Uuid);
  return (It == FStates.cend()) ? nullptr : &It.value();
}

//---------------------------------------------------------------------------
// Local sites

IniFile TeamSync::LocalSites(const QString & Folder) const
{
  IniFile Sites;
  for (const auto & Data : FConfig->StoredSessions().Sessions())
  {
    if (!Data->IsWorkspace && Belongs(Data->Name, Folder))
    {
      TeamContent::AddSite(Sites, Relative(Data->Name, Folder), *Data);
    }
  }
  return Sites;
}

QByteArray TeamSync::LocalContent(const Team & T) const
{
  return CloudSettings::Canonical(LocalSites(T.Folder));
}

QString TeamSync::UniqueFolder(const QString & Name, const QString & Uuid) const
{
  return UniqueFolderLike(FolderName(Name), Uuid);
}

QString TeamSync::UniqueFolderLike(const QString & Base, const QString & Uuid) const
{
  const State * Own = FindState(Uuid);
  QString OwnFolder = (Own != nullptr) ? Own->Folder : QString();
  // Everything but the team's own folder, spelled exactly: a folder that differs
  // from it only by fold or lookalike is someone else's
  QSet<QString> Taken;
  auto Take = [&](const QString & Folder)
  {
    if (!Folder.isEmpty() && (Folder != OwnFolder))
    {
      Taken.insert(TeamContent::Skeleton(Folder));
    }
  };
  for (const auto & Data : FConfig->StoredSessions().Sessions())
  {
    Take(Data->Name.section(QLatin1Char('/'), 0, 0));
  }
  for (const QString & Folder : FConfig->StoredSessions().Folders())
  {
    Take(Folder.section(QLatin1Char('/'), 0, 0));
  }
  for (const State & Other : FStates)
  {
    Take(Other.Folder);
  }
  QString Candidate = Base;
  for (int Number = 2; Taken.contains(TeamContent::Skeleton(Candidate)); Number++)
  {
    Candidate = QStringLiteral("%1 (%2)").arg(Base).arg(Number);
  }
  return Candidate;
}

void TeamSync::KeepFolderApart(const QString & Uuid)
{
  State & S = StateOf(Uuid);
  if (S.Folder.isEmpty())
  {
    return;
  }
  QString Key = TeamContent::Fold(S.Folder);
  for (const auto & Data : FConfig->StoredSessions().Sessions())
  {
    QString Folder = Data->Name.section(QLatin1Char('/'), 0, 0);
    if ((Folder != S.Folder) && (TeamContent::Fold(Folder) == Key))
    {
      QString New = UniqueFolder(S.Name.isEmpty() ? Placeholder(Uuid) : S.Name, Uuid);
      MoveFolder(S.Folder, New);
      SaveSites();
      S.Folder = New;
      auto It = FTeams.find(Uuid);
      if (It != FTeams.end())
      {
        It->Folder = New;
      }
      return;
    }
  }
}

QString TeamSync::EnsureFolder(const QString & Uuid)
{
  State & S = StateOf(Uuid);
  if (S.Folder.isEmpty())
  {
    S.Folder = UniqueFolder(S.Name.isEmpty() ? Placeholder(Uuid) : S.Name, Uuid);
    S.Hash = TeamContent::Hash(IniFile());
    auto It = FTeams.find(Uuid);
    if (It != FTeams.end())
    {
      It->Folder = S.Folder;
    }
  }
  return S.Folder;
}

void TeamSync::MoveFolder(const QString & From, const QString & To)
{
  StoredSessionList & Sites = FConfig->StoredSessions();
  QList<QPair<QString, QString>> Moved;
  for (const auto & Data : Sites.Sessions())
  {
    if (Belongs(Data->Name, From))
    {
      Moved.append({Data->Name, To + QLatin1Char('/') + Relative(Data->Name, From)});
    }
  }
  Sites.RenameFolder(From, To);
  for (const auto & Site : Moved)
  {
    emit SiteMoved(Site.first, Site.second);
  }
}

void TeamSync::RemoveFolderSites(const QString & Folder)
{
  StoredSessionList & Sites = FConfig->StoredSessions();
  QStringList Names;
  for (const auto & Data : Sites.Sessions())
  {
    if (!Data->IsWorkspace && Belongs(Data->Name, Folder))
    {
      Names.append(Data->Name);
    }
  }
  Sites.Replace(Names, {});
}

bool TeamSync::SaveSites()
{
  bool Saved = FConfig->Save();
  // Save() signals only changes of another instance
  emit FConfig->StoredSessionsChanged();
  return Saved;
}

int TeamSync::RemoveTeamLocally(const QString & Uuid)
{
  const State * S = FindState(Uuid);
  if ((S == nullptr) || S->Folder.isEmpty())
  {
    return 0;
  }
  QString Folder = S->Folder;
  QHash<QString, QString> Base = S->BaseSites;
  QHash<QString, QString> Now = TeamContent::SiteHashes(LocalSites(Folder));
  StoredSessionList & Sites = FConfig->StoredSessions();
  // What was never shared is this person's, and goes with them
  QList<std::shared_ptr<SessionData>> Kept;
  for (const auto & Data : Sites.Sessions())
  {
    if (!Data->IsWorkspace && Belongs(Data->Name, Folder))
    {
      QString Name = Relative(Data->Name, Folder);
      if (Now.value(Name) != Base.value(Name))
      {
        auto Copy = std::make_shared<SessionData>(*Data);
        Kept.append(Copy);
      }
    }
  }
  RemoveFolderSites(Folder);
  for (const auto & Copy : Kept)
  {
    QString From = Copy->Name;
    Copy->Name = Relative(From, Folder);
    Copy->MakeUniqueIn(Sites);
    Sites.Add(Copy);
    emit SiteMoved(From, Copy->Name);
  }
  SaveSites();
  return static_cast<int>(Kept.size());
}

void TeamSync::ForgetTeam(const QString & Uuid, bool Ignore)
{
  // Everything but the floor goes. A 404 of the server may be what brought this
  // on, and it must not reset what a later answer is measured against.
  State & S = StateOf(Uuid);
  State Floor;
  Floor.Detached = true;
  Floor.Ignored = Ignore;
  Floor.RosterSeq = S.RosterSeq;
  Floor.RosterHash = S.RosterHash;
  Floor.PinVersion = S.PinVersion;
  Floor.PinSha = S.PinSha;
  Floor.PinKeyVersion = S.PinKeyVersion;
  Floor.OldKeys = S.OldKeys;
  S = Floor;
  FRestoring.remove(Uuid);
  FRosters.remove(Uuid);
  FTeams.remove(Uuid);
  FWarned.remove(Uuid);
  FGone.insert(Uuid);
}

void TeamSync::FinishMoves(State & S, const QHash<QString, QString> & Committed)
{
  QString TeamFolder = S.Folder;
  if (S.Moves.isEmpty())
  {
    return;
  }
  StoredSessionList & Sites = FConfig->StoredSessions();
  QSet<QString> Folders;
  for (auto It = S.Moves.cbegin(); It != S.Moves.cend(); ++It)
  {
    // only when the team really has it now
    if (Committed.contains(It.value()))
    {
      Sites.Remove(It.key());
      Folders.insert(It.key().section(QLatin1Char('/'), 0, -2));
      emit SiteMoved(It.key(), TeamFolder + QLatin1Char('/') + It.value());
    }
  }
  S.Moves.clear();
  for (const QString & Folder : Folders)
  {
    bool Empty = !Folder.isEmpty();
    for (const auto & Data : Sites.Sessions())
    {
      Empty = Empty && !Data->Name.startsWith(Folder + QLatin1Char('/'));
    }
    if (Empty)
    {
      Sites.RemoveFolder(Folder);
    }
  }
}

void TeamSync::ApplyName(const QString & Uuid, const QString & Name)
{
  State & S = StateOf(Uuid);
  QString Clean = TeamContent::NormalizeName(Name);
  if (Clean.isEmpty())
  {
    Clean = S.Name.isEmpty() ? Placeholder(Uuid) : S.Name;
  }
  if (!S.Folder.isEmpty() && (S.Name == Clean))
  {
    return;
  }
  QString Previous = S.Name;
  QString Old = S.Folder;
  QString New = UniqueFolder(Clean, Uuid);
  if (Old.isEmpty())
  {
    S.Hash = TeamContent::Hash(IniFile());
  }
  else if (Old != New)
  {
    MoveFolder(Old, New);
    SaveSites();
  }
  S.Name = Clean;
  S.Folder = New;
  auto It = FTeams.find(Uuid);
  if (It != FTeams.end())
  {
    It->Name = Clean;
    It->Folder = New;
  }
  if (!Previous.isEmpty() && (Previous != Clean) && S.PendingName.isEmpty())
  {
    emit Notice(QStringLiteral("The team \"%1\" was renamed to \"%2\".").arg(Previous, Clean));
  }
}

// What tells a site apart for the people connecting to it
// Where a site connects, for the notice of a change: user@host:port, protocol
// and whether it is encrypted (a downgrade to plain FTP must show)
static QString Address(const IniFile & Sites, const QString & Relative)
{
  QString Section = TeamContent::SiteSection(Relative);
  QString Result = QStringLiteral("%1@%2").arg(UnMungeStr(Sites.ReadRaw(Section, QStringLiteral("UserName"))),
    UnMungeStr(Sites.ReadRaw(Section, QStringLiteral("HostName"))));
  QString Port = Sites.ReadRaw(Section, QStringLiteral("PortNumber"));
  if (!Port.isEmpty())
  {
    Result += QLatin1Char(':') + Port;
  }
  QString Protocol = Sites.ReadRaw(Section, QStringLiteral("FSProtocol"));
  bool Encrypted = !Sites.ReadRaw(Section, QStringLiteral("Ftps")).isEmpty() && (Sites.ReadRaw(Section, QStringLiteral("Ftps")) != QStringLiteral("0"));
  if (Protocol == QStringLiteral("5"))
  {
    Result += Encrypted ? QStringLiteral(" (FTPS)") : QStringLiteral(" (FTP, not encrypted)");
  }
  else if (Protocol == QStringLiteral("6"))
  {
    Result += Encrypted ? QStringLiteral(" (WebDAVS)") : QStringLiteral(" (WebDAV, not encrypted)");
  }
  else if (Protocol == QStringLiteral("7"))
  {
    Result += Encrypted ? QStringLiteral(" (S3)") : QStringLiteral(" (S3, not encrypted)");
  }
  else if (Protocol == QStringLiteral("0"))
  {
    Result += QStringLiteral(" (SCP)");
  }
  return Result;
}

// What nobody may change without the members being told: where it connects, how,
// and which host key it expects. Address first: the notice shows that line.
static QString Critical(const IniFile & Sites, const QString & Relative)
{
  QString Section = TeamContent::SiteSection(Relative);
  return QStringList{Address(Sites, Relative), Sites.ReadRaw(Section, QStringLiteral("FSProtocol")),
    Sites.ReadRaw(Section, QStringLiteral("Ftps")), Sites.ReadRaw(Section, QStringLiteral("SshHostKey"))}.join(QLatin1Char('\n'));
}

void TeamSync::SetBase(State & S, const IniFile & Sites)
{
  S.Hash = TeamContent::Hash(Sites);
  S.BaseSites = TeamContent::SiteHashes(Sites);
  S.BaseCritical.clear();
  for (auto It = S.BaseSites.cbegin(); It != S.BaseSites.cend(); ++It)
  {
    S.BaseCritical.insert(TeamContent::Fold(It.key()), Critical(Sites, It.key()));
  }
}

bool TeamSync::Apply(const QString & Uuid, const Verified & Snapshot, QString & Error)
{
  State & S = StateOf(Uuid);
  bool Notify = (S.Base > 0);
  ApplyName(Uuid, Snapshot.Name);
  KeepFolderApart(Uuid);
  QString Folder = S.Folder;
  QString TeamName = S.Name;
  QString Ignored;
  IniFile Received;
  TeamContent::Decode(Snapshot.Plain, Ignored, Received);
  StoredSessionList & Sites = FConfig->StoredSessions();
  IniFile Before = LocalSites(Folder);
  QHash<QString, QString> Old = TeamContent::SiteHashes(Before);
  QHash<QString, QString> New = TeamContent::SiteHashes(Received);
  // this person's own settings of the team's sites survive
  QHash<QString, std::shared_ptr<SessionData>> Own;
  for (const auto & Data : Sites.Sessions())
  {
    if (!Data->IsWorkspace && Belongs(Data->Name, Folder))
    {
      Own.insert(TeamContent::Fold(Relative(Data->Name, Folder)), Data);
    }
  }
  // By folded name, as the site list: "db" and "DB" are one site that changed
  QHash<QString, QString> OldByFold;
  for (auto It = Old.cbegin(); It != Old.cend(); ++It)
  {
    OldByFold.insert(TeamContent::Fold(It.key()), It.value());
  }
  QSet<QString> NewFolds;
  QSet<QString> BaseFolds;
  for (auto It = S.BaseSites.cbegin(); It != S.BaseSites.cend(); ++It)
  {
    BaseFolds.insert(TeamContent::Fold(It.key()));
  }
  int Added = 0;
  int Changed = 0;
  int Removed = 0;
  QStringList Redirected;
  for (auto It = New.cbegin(); It != New.cend(); ++It)
  {
    QString Key = TeamContent::Fold(It.key());
    NewFolds.insert(Key);
    auto Previous = OldByFold.constFind(Key);
    if (Previous == OldByFold.cend())
    {
      Added++;
    }
    else if (*Previous != It.value())
    {
      Changed++;
    }
    // Against what the team had, not what this computer has: a local edit neither
    // hides a change of the team's nor reads as one when it is reverted
    auto Was = S.BaseCritical.constFind(Key);
    QString Now = Critical(Received, It.key());
    if ((Was != S.BaseCritical.cend()) && (*Was != Now))
    {
      QString From = Was->section(QLatin1Char('\n'), 0, 0);
      QString To = Now.section(QLatin1Char('\n'), 0, 0);
      Redirected.append((From != To) ? QStringLiteral("\"%1\" now connects to %2 (was %3)").arg(It.key(), To, From)
        : QStringLiteral("\"%1\" has another encryption setting or host key").arg(It.key()));
    }
  }
  // Sites added here and never shared do not vanish with the revert
  QList<std::shared_ptr<SessionData>> Kept;
  for (auto It = Old.cbegin(); It != Old.cend(); ++It)
  {
    QString Key = TeamContent::Fold(It.key());
    if (!NewFolds.contains(Key))
    {
      Removed++;
      if (!BaseFolds.contains(Key) && Own.contains(Key))
      {
        Kept.append(std::make_shared<SessionData>(*Own.value(Key)));
      }
    }
  }
  QList<std::shared_ptr<SessionData>> Merged;
  for (const QString & Section : Received.Sections())
  {
    QString Name = TeamContent::SiteName(Section);
    auto Local = Own.value(TeamContent::Fold(Name));
    auto Site = TeamContent::Merge(Received, Section, Local.get(), Folder + QLatin1Char('/') + Name);
    // Forwarding the agent was this person's decision for the old host only
    if ((Local != nullptr) && (Local->HostName != Site->HostName))
    {
      Site->AgentFwd = false;
    }
    Merged.append(Site);
  }
  RemoveFolderSites(Folder);
  Sites.Replace(QStringList(), Merged);
  for (const auto & Copy : Kept)
  {
    QString From = Copy->Name;
    Copy->Name = Relative(From, Folder);
    Copy->MakeUniqueIn(Sites);
    Sites.Add(Copy);
    emit SiteMoved(From, Copy->Name);
  }
  // A move whose site is not in the team after all stays a personal site
  for (auto It = S.Moves.begin(); It != S.Moves.end(); )
  {
    It = New.contains(It.value()) ? std::next(It) : S.Moves.erase(It);
  }
  if (!SaveSites())
  {
    Error = QStringLiteral("The sites of the team \"%1\" could not be saved: %2").arg(TeamName, FConfig->LastSaveError());
    return false;
  }
  IniFile After = LocalSites(Folder);
  S.Base = Snapshot.Version;
  SetBase(S, After);
  S.ConflictVersion = 0;
  S.EmptyConflict = false;
  if (!Kept.isEmpty())
  {
    emit Notice(QStringLiteral("Sites you added to the team \"%1\" were not shared; they were moved to your own sites.").arg(TeamName));
  }
  // not for the first sync of a team (its sites arrive)
  if (Notify && (Added + Removed + Changed > 0))
  {
    emit SitesReceived(TeamName, Added, Removed, Changed);
  }
  if (Notify && !Redirected.isEmpty())
  {
    QString Message = Redirected.mid(0, 5).join(QStringLiteral("; "));
    if (Redirected.size() > 5)
    {
      Message += QStringLiteral("; and %1 more sites").arg(Redirected.size() - 5);
    }
    emit Notice(QStringLiteral("Team \"%1\": %2.").arg(TeamName, Message));
  }
  return true;
}

//---------------------------------------------------------------------------
// Sync

void TeamSync::SwitchAccount()
{
  QString Account = FSync->UserUuid();
  if (FAccount == Account)
  {
    return;
  }
  // The other account's sites and state stay as they are, for its return
  FAccount = Account;
  FTeams.clear();
  FRosters.clear();
  FWarned.clear();
  FGone.clear();
  Security::WipeBytes(FKeys.Private);
  Security::WipeBytes(FIdentity.Seed);
  FKeys = TeamCrypto::KeyPair();
  FIdentity = TeamCrypto::SignKey();
  FKeysUser.clear();
}

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
  // Callers that came during this sync: their change may have missed it
  if (!FNext.isEmpty() && !FSyncing)
  {
    FWaiting.swap(FNext);
    StartSync();
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
  if (FSyncing)
  {
    if (Callback)
    {
      FNext.append(Callback);
    }
    return;
  }
  FWaiting.append(Callback);
  StartSync();
}

void TeamSync::StartSync()
{
  if (!Available())
  {
    Finish(false, QStringLiteral("Sign in to NixSCP Pro and unlock cloud sync to use teams."));
    return;
  }
  FSyncing = true;
  FResults.clear();
  if (!FStatesLoaded)
  {
    LoadStates();
  }
  SwitchAccount();
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
      SyncTeams(FTeams.keys(), QStringList(), [this](bool Ok, const QString & Error) { Finish(Ok, Error); });
    });
  });
}

void TeamSync::SyncFor(const QString & Uuid, const Done & Callback)
{
  SyncAll([this, Uuid, Callback](bool Ok, const QString & Error)
  {
    auto It = FResults.constFind(Uuid);
    if (It == FResults.cend())
    {
      Callback(Ok, Error);
    }
    else
    {
      Callback(It->isEmpty(), *It);
    }
  });
}

bool TeamSync::AdoptKeys(const QByteArray & Wrapped, QString & Error)
{
  TeamCrypto::KeyPair Box;
  TeamCrypto::SignKey Identity;
  if (!TeamCrypto::SplitKeyBlob(TeamCrypto::UnwrapPrivateKey(FSync->DataKey(), Wrapped), Box, Identity))
  {
    Error = QStringLiteral("Your team keys cannot be read with this computer's sync key.");
    return false;
  }
  QString Pinned = FPinnedIdentities.value(FAccount);
  if (!Pinned.isEmpty() && (Pinned != B64(Identity.Public)))
  {
    Error = QStringLiteral("The identity key of this account changed on the server. NixSCP keeps using the one it knows and does not sync teams.");
    return false;
  }
  FKeys = Box;
  FIdentity = Identity;
  FKeysUser = FAccount;
  FPinnedIdentities.insert(FAccount, B64(Identity.Public));
  return true;
}

void TeamSync::FinishKeys(bool Ok, const QString & Error)
{
  FKeysBusy = false;
  QList<Done> Waiting;
  Waiting.swap(FKeysWaiting);
  for (const Done & Callback : Waiting)
  {
    Callback(Ok, Error);
  }
}

void TeamSync::EnsureKeys(const Done & Callback)
{
  SwitchAccount();
  if (FIdentity.IsValid() && !FKeys.Private.isEmpty() && (FKeysUser == FAccount))
  {
    Callback(true, QString());
    return;
  }
  // One at a time: two first uploads would make two key pairs
  FKeysWaiting.append(Callback);
  if (FKeysBusy)
  {
    return;
  }
  FKeysBusy = true;
  FSync->Client().Api("GET", QStringLiteral("/v1/keys/me"), [this](const CloudClient::Reply & Reply)
  {
    if (Reply.Ok())
    {
      QString Error;
      bool Ok = AdoptKeys(UnB64(Reply.Json.value(QStringLiteral("wrapped_private_key"))), Error);
      FinishKeys(Ok, Error);
      return;
    }
    if (Reply.Status != 404)
    {
      FinishKeys(false, ApiError(Reply, QStringLiteral("Cannot read your team keys")));
      return;
    }
    if (FPinnedIdentities.contains(FAccount))
    {
      // This account has keys: never make new ones because of an answer
      FinishKeys(false, QStringLiteral("The server no longer has the team keys of this account."));
      return;
    }
    UploadKeys();
  });
}

void TeamSync::UploadKeys()
{
  // the first time: key pairs for this account
  TeamCrypto::KeyPair Box = TeamCrypto::NewKeyPair();
  TeamCrypto::SignKey Identity = TeamCrypto::NewSignKey();
  QJsonObject Body{{QStringLiteral("public_key"), B64(Box.Public)}, {QStringLiteral("identity_key"), B64(Identity.Public)},
    {QStringLiteral("key_signature"), B64(TeamCrypto::Sign(Identity, TeamCrypto::BoxKeyMessage(FAccount, Box.Public)))},
    {QStringLiteral("wrapped_private_key"), B64(TeamCrypto::WrapPrivateKey(FSync->DataKey(), TeamCrypto::KeyBlob(Box, Identity)))}};
  FSync->Client().Api("PUT", QStringLiteral("/v1/keys/me"), Body, [this, Box, Identity](const CloudClient::Reply & Put)
  {
    if (Put.ErrorCode() == QStringLiteral("keys_exist"))
    {
      // Another computer of this account was faster: its keys count
      FSync->Client().Api("GET", QStringLiteral("/v1/keys/me"), [this](const CloudClient::Reply & Reply)
      {
        QString Error = ApiError(Reply, QStringLiteral("Cannot read your team keys"));
        bool Ok = Reply.Ok() && AdoptKeys(UnB64(Reply.Json.value(QStringLiteral("wrapped_private_key"))), Error);
        FinishKeys(Ok, Error);
      });
      return;
    }
    if (!Put.Ok())
    {
      FinishKeys(false, ApiError(Put, QStringLiteral("Cannot store your team keys")));
      return;
    }
    FKeys = Box;
    FIdentity = Identity;
    FKeysUser = FAccount;
    FPinnedIdentities.insert(FAccount, B64(Identity.Public));
    SaveStates();
    FinishKeys(true, QString());
  });
}

void TeamSync::Warn(const QString & Uuid, const QString & Message)
{
  if (FWarned.value(Uuid) != Message)
  {
    FWarned.insert(Uuid, Message);
    emit Notice(Message);
  }
}

void TeamSync::Refused(Team & T, const QString & Why)
{
  T.Error = QStringLiteral("The team \"%1\" failed verification and is not synced: %2. Your sites were not touched.")
    .arg(T.Name.isEmpty() ? Placeholder(T.Uuid) : T.Name, Why);
  Warn(T.Uuid, T.Error);
}

void TeamSync::RosterNotices(const QString & Uuid, const QList<TeamRoster::Entry> & Items, qint64 After)
{
  const TeamRoster::State & Roster = FRosters[Uuid];
  QString Name = StateOf(Uuid).Name;
  Name = Name.isEmpty() ? Placeholder(Uuid) : Name;
  for (const TeamRoster::Entry & Item : Items)
  {
    if ((Item.Seq <= After) || (Item.Signer == FAccount))
    {
      continue;
    }
    if (Item.Type == QStringLiteral("join"))
    {
      QByteArray Identity = UnB64(Item.Data.value(QStringLiteral("identity")));
      QString Email = Item.Data.value(QStringLiteral("email")).toString();
      bool KeyChanged = false;
      for (const TeamRoster::RoleChange & Change : Roster.Roles.value(Item.Signer))
      {
        KeyChanged = KeyChanged || ((Change.Seq < Item.Seq) && (Change.Identity != Identity));
      }
      emit Notice(KeyChanged
        ? QStringLiteral("The key of %1 in the team \"%2\" changed. Compare the new fingerprint with them: %3").arg(Email, Name, TeamCrypto::Fingerprint(Identity))
        : QStringLiteral("%1 joined the team \"%2\". Their key fingerprint: %3").arg(Email, Name, TeamCrypto::Fingerprint(Identity)));
    }
    else if ((Item.Type == QStringLiteral("role")) && (Item.Data.value(QStringLiteral("user")).toString() == FAccount))
    {
      emit Notice(QStringLiteral("Your role in the team \"%1\" is now %2.").arg(Name, Item.Data.value(QStringLiteral("role")).toString()));
    }
  }
}

bool TeamSync::AboveFloor(const State & S, const TeamRoster::State & New, QString & Why)
{
  if (S.RosterSeq == 0)
  {
    return true;
  }
  if (New.Seq < S.RosterSeq)
  {
    Why = QStringLiteral("the member list went back from entry %1 to %2").arg(S.RosterSeq).arg(New.Seq);
    return false;
  }
  if (New.Hashes.value(S.RosterSeq) != S.RosterHash)
  {
    Why = QStringLiteral("the member list was rewritten");
    return false;
  }
  return true;
}

bool TeamSync::AcceptRoster(const QString & Uuid, const QJsonArray & Entries, QString & Why)
{
  TeamRoster::State New;
  QList<TeamRoster::Entry> Items;
  if (!TeamRoster::Replay(Entries, Uuid, New, &Items, &Why))
  {
    return false;
  }
  State & S = StateOf(Uuid);
  qint64 Pinned = S.RosterSeq;
  if (!AboveFloor(S, New, Why))
  {
    return false;
  }
  if (Pinned == 0)
  {
    // New on this computer: the account's own signature is the anchor. Its entry
    // chains to everything before it, and it verified that when it joined.
    bool Anchored = false;
    for (const TeamRoster::Entry & Item : Items)
    {
      Anchored = Anchored || (((Item.Type == QStringLiteral("create")) || (Item.Type == QStringLiteral("join"))) &&
        (Item.Signer == FAccount) && (UnB64(Item.Data.value(QStringLiteral("identity"))) == FIdentity.Public));
    }
    if (!Anchored)
    {
      Why = QStringLiteral("this account never signed itself into it");
      return false;
    }
  }
  const TeamRoster::Member * Me = New.Find(FAccount);
  if ((Me != nullptr) && (Me->Identity != FIdentity.Public))
  {
    Why = QStringLiteral("it lists another key for this account");
    return false;
  }
  FRosters.insert(Uuid, New);
  S.RosterSeq = New.Seq;
  S.RosterHash = New.Hash;
  if (Pinned > 0)
  {
    RosterNotices(Uuid, Items, Pinned);
  }
  return true;
}

void TeamSync::LoadRoster(const QString & Uuid, const std::function<void(bool Gone, const QString & Error)> & Callback)
{
  FSync->Client().Api("GET", QStringLiteral("/v1/teams/%1/roster").arg(Uuid), [this, Uuid, Callback](const CloudClient::Reply & Reply)
  {
    if ((Reply.Status == 404) && (Reply.ErrorCode() == QStringLiteral("not_found")))
    {
      Callback(true, QString());
      return;
    }
    if (!Reply.Ok())
    {
      Callback(false, ApiError(Reply, QStringLiteral("Cannot read the team's member list")));
      return;
    }
    QString Why;
    if (!AcceptRoster(Uuid, Reply.Json.value(QStringLiteral("entries")).toArray(), Why))
    {
      Callback(false, QStringLiteral("!") + Why);
      return;
    }
    Callback(false, QString());
  });
}

void TeamSync::BuildTeam(const Listed & Item, Team & T)
{
  const TeamRoster::State & Roster = FRosters[Item.Uuid];
  const TeamRoster::Member * Me = Roster.Find(FAccount);
  State & S = StateOf(Item.Uuid);
  T.Role = (Me != nullptr) ? Me->Role : QString();
  T.KeyVersion = Roster.KeyVersion;
  T.NeedsRotation = Roster.NeedsRotation;
  T.MemberCount = static_cast<int>(Roster.Members.size());
  if (Me == nullptr)
  {
    return;
  }
  S.Role = Me->Role;
  // A sealed box has no sender: only a wrap the roster names is the team's
  if ((Item.MyKeyVersion != Roster.KeyVersion) || (Me->WrapVersion != Roster.KeyVersion) || (TeamRoster::WrapHash(Item.Wrap) != Me->WrapHash))
  {
    Refused(T, QStringLiteral("the team key offered is not the one in the signed member list"));
    return;
  }
  {
    QByteArray Opened = TeamCrypto::UnwrapForMember(FKeys, Item.Wrap, Item.Uuid, Roster.KeyVersion);
    T.Key.Assign(Opened);
    Security::WipeBytes(Opened);
  }
  if (T.Key.IsEmpty())
  {
    Refused(T, QStringLiteral("the team key cannot be opened"));
    return;
  }
  if (!S.OldKeys.contains(T.KeyVersion))
  {
    S.OldKeys.insert(T.KeyVersion, CloudCrypto::Seal(FSync->DataKey(), T.Key.View(), OldKeyAad(Item.Uuid, T.KeyVersion)));
  }
  // Back after a 404 and verified above the floor
  S.Detached = false;
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
    // Anything but a proper list says nothing about memberships
    const QString Invalid = QStringLiteral("The server's reply is not a list of teams.");
    QJsonDocument Document = QJsonDocument::fromJson(Reply.Data);
    if (!Document.isArray())
    {
      Callback(false, Invalid);
      return;
    }
    QList<Listed> List;
    for (const QJsonValue & V : Document.array())
    {
      QJsonObject O = V.toObject();
      QJsonValue Roster = O.value(QStringLiteral("roster"));
      QJsonValue Head = O.value(QStringLiteral("head"));
      Listed Item;
      Item.Uuid = O.value(QStringLiteral("uuid")).toString();
      if (!TeamRoster::IsUuid(Item.Uuid) || !Roster.isObject() || !Head.isObject())
      {
        Callback(false, Invalid);
        return;
      }
      Item.RosterSeq = Roster.toObject().value(QStringLiteral("seq")).toInteger();
      Item.RosterHash = Roster.toObject().value(QStringLiteral("hash")).toString();
      Item.HeadVersion = Head.toObject().value(QStringLiteral("version")).toInteger();
      Item.HeadSha = Head.toObject().value(QStringLiteral("sha256")).toString();
      Item.MyKeyVersion = O.value(QStringLiteral("my_key_version")).toInteger();
      Item.Wrap = UnB64(O.value(QStringLiteral("wrapped_team_key")));
      List.append(Item);
    }
    VerifyListed(List, QSet<QString>(), QHash<QString, Team>(), Callback);
  });
}

void TeamSync::VerifyListed(QList<Listed> Pending, QSet<QString> Seen, QHash<QString, Team> Teams, const Done & Callback)
{
  if (Pending.isEmpty())
  {
    // Teams of this computer that the list left out
    QStringList Missing;
    QString Prefix = FAccount + QLatin1Char('/');
    for (auto It = FStates.begin(); It != FStates.end(); )
    {
      QString Uuid = It.key().mid(Prefix.size());
      if (!It.key().startsWith(Prefix) || Seen.contains(Uuid) || It->Detached)
      {
        ++It;
      }
      else if (It->RosterSeq == 0)
      {
        // listed once, never verified: nothing here belongs to it
        It = FStates.erase(It);
      }
      else
      {
        Missing.append(Uuid);
        ++It;
      }
    }
    FGone.intersect(Seen);
    CheckMissing(Missing, Teams, QStringList(), Callback);
    return;
  }
  Listed Item = Pending.takeFirst();
  Seen.insert(Item.Uuid);
  const State * Known = FindState(Item.Uuid);
  if (FGone.contains(Item.Uuid) || ((Known != nullptr) && Known->Ignored))
  {
    VerifyListed(Pending, Seen, Teams, Callback);
    return;
  }
  auto Continue = [this, Item, Pending, Seen, Teams, Callback](bool Gone, const QString & Error) mutable
  {
    if (Gone)
    {
      // left in a race with this list; the next one settles it
      Seen.remove(Item.Uuid);
      VerifyListed(Pending, Seen, Teams, Callback);
      return;
    }
    Team T;
    T.Uuid = Item.Uuid;
    const State & S = StateOf(Item.Uuid);
    T.Name = S.Name.isEmpty() ? Placeholder(Item.Uuid) : S.Name;
    T.Folder = S.Folder;
    T.HeadVersion = Item.HeadVersion;
    T.HeadSha = Item.HeadSha;
    if (Error.startsWith(QLatin1Char('!')))
    {
      Refused(T, Error.mid(1));
    }
    else if (!Error.isEmpty())
    {
      T.Error = Error;
    }
    if (FRosters.contains(Item.Uuid))
    {
      if (Error.isEmpty())
      {
        BuildTeam(Item, T);
        if (T.Role.isEmpty())
        {
          // The verified list says this account left or was removed
          Seen.remove(Item.Uuid);
          VerifyListed(Pending, Seen, Teams, Callback);
          return;
        }
      }
      else
      {
        const TeamRoster::State & Roster = FRosters[Item.Uuid];
        const TeamRoster::Member * Me = Roster.Find(FAccount);
        T.Role = (Me != nullptr) ? Me->Role : QString();
        T.KeyVersion = Roster.KeyVersion;
        T.MemberCount = static_cast<int>(Roster.Members.size());
      }
    }
    // A list that cannot be verified: the owner must still find Delete
    T.Role = T.Role.isEmpty() ? S.Role : T.Role;
    Teams.insert(T.Uuid, T);
    VerifyListed(Pending, Seen, Teams, Callback);
  };
  auto Roster = FRosters.constFind(Item.Uuid);
  if ((Roster != FRosters.cend()) && (Roster->Seq == Item.RosterSeq) && (Roster->Hash == Item.RosterHash))
  {
    Continue(false, QString());
  }
  else
  {
    LoadRoster(Item.Uuid, Continue);
  }
}

void TeamSync::CheckMissing(QStringList Pending, QHash<QString, Team> Teams, QStringList Errors, const Done & Callback)
{
  if (Pending.isEmpty())
  {
    for (const QString & Uuid : FGone)
    {
      Teams.remove(Uuid);
    }
    FTeams = Teams;
    Callback(Errors.isEmpty(), Errors.join(QLatin1Char('\n')));
    return;
  }
  QString Uuid = Pending.takeFirst();
  auto Leave = [this, Uuid]()
  {
    QString Name = StateOf(Uuid).Name;
    int Kept = RemoveTeamLocally(Uuid);
    ForgetTeam(Uuid);
    FGone.remove(Uuid);
    QString Message = QStringLiteral("You are no longer a member of the team \"%1\"; its sites were removed from this computer.").arg(Name);
    emit Notice((Kept > 0) ? Message + QStringLiteral(" %1 sites with changes that were never shared were moved to your own sites.").arg(Kept) : Message);
  };
  // Only the server's plain "no such team" or a verified removal ends a membership
  LoadRoster(Uuid, [this, Uuid, Leave, Pending, Teams, Errors, Callback](bool Gone, const QString & Error) mutable
  {
    if (!Gone && Error.isEmpty() && (FRosters[Uuid].Find(FAccount) == nullptr))
    {
      Gone = true;
    }
    if (Gone)
    {
      Leave();
    }
    else
    {
      // Still this account's by all it can verify: it stays, and nothing happens to it
      Team T;
      T.Uuid = Uuid;
      const State & S = StateOf(Uuid);
      T.Name = S.Name.isEmpty() ? Placeholder(Uuid) : S.Name;
      T.Folder = S.Folder;
      if (Error.startsWith(QLatin1Char('!')))
      {
        Refused(T, Error.mid(1));
      }
      else
      {
        T.Error = Error.isEmpty() ? QStringLiteral("The server left the team \"%1\" out of your teams.").arg(T.Name) : Error;
      }
      Errors.append(T.Error);
      Teams.insert(Uuid, T);
    }
    CheckMissing(Pending, Teams, Errors, Callback);
  });
}

void TeamSync::SyncTeams(QStringList Pending, QStringList Errors, const Done & Callback)
{
  if (Pending.isEmpty())
  {
    Callback(Errors.isEmpty(), Errors.join(QLatin1Char('\n')));
    return;
  }
  QString Uuid = Pending.takeFirst();
  SyncTeam(Uuid, [this, Uuid, Pending, Errors, Callback](bool Ok, const QString & Error) mutable
  {
    FResults.insert(Uuid, Ok ? QString() : Error);
    if (Ok)
    {
      FWarned.remove(Uuid);
    }
    else
    {
      Errors.append(Error);   // the other teams still sync
    }
    SyncTeams(Pending, Errors, Callback);
  });
}

void TeamSync::SyncTeam(const QString & Uuid, const Done & Callback, int Attempt)
{
  auto It = FTeams.find(Uuid);
  if (It == FTeams.end())
  {
    // left or deleted meanwhile
    Callback(true, QString());
    return;
  }
  Team & T = *It;
  if (!T.Error.isEmpty())
  {
    Callback(false, T.Error);
    return;
  }
  if (Attempt >= MaxAttempts)
  {
    Callback(false, QStringLiteral("The team \"%1\" keeps changing; it is synced again later.").arg(T.Name));
    return;
  }
  State & S = StateOf(Uuid);
  if (T.HeadVersion < S.PinVersion)
  {
    Refused(T, QStringLiteral("its content went back from version %1 to %2").arg(S.PinVersion).arg(T.HeadVersion));
    Callback(false, T.Error);
    return;
  }
  if ((S.PinVersion > 0) && (T.HeadVersion == S.PinVersion) && (T.HeadSha != S.PinSha))
  {
    // The hash in a list is what the committer said it is; the bytes decide
    Fetch(Uuid, T.HeadVersion, true, [this, Uuid, Callback, Attempt](bool Ok, const QString & Error, const Verified & Snapshot)
    {
      auto It = FTeams.find(Uuid);
      if (It == FTeams.end())
      {
        Callback(true, QString());
      }
      else if (!Ok)
      {
        Callback(false, FetchFailed(*It, Error));
      }
      else
      {
        It->HeadSha = Snapshot.Sha;
        SyncTeam(Uuid, Callback, Attempt + 1);
      }
    });
    return;
  }
  if (T.NeedsRotation && T.CanEdit())
  {
    Rotate(Uuid, [this, Uuid, Callback, Attempt](bool Ok, const QString & Error)
    {
      if (!Ok)
      {
        Callback(false, Error);
        return;
      }
      SyncTeam(Uuid, Callback, Attempt + 1);
    });
    return;
  }
  bool Local = !S.Folder.isEmpty();
  IniFile Sites = LocalSites(S.Folder);
  QString LocalHash = TeamContent::Hash(Sites);
  bool Changed = Local && (LocalHash != S.Hash);
  if (FForcePull.remove(Uuid) && (T.HeadVersion > 0))
  {
    PullHead(Uuid, false, Callback, Attempt);
    return;
  }
  if (T.HeadVersion > S.Base)
  {
    if (Changed && T.CanEdit())
    {
      if ((S.ConflictVersion == T.HeadVersion) && !S.EmptyConflict)
      {
        HoldConflict(Uuid, false, Callback);
        return;
      }
      // Both changed, unless the head is what this computer has
      Fetch(Uuid, T.HeadVersion, true, [this, Uuid, LocalHash, Callback, Attempt](bool Ok, const QString & Error, const Verified & Snapshot)
      {
        auto It = FTeams.find(Uuid);
        if (It == FTeams.end())
        {
          Callback(true, QString());
          return;
        }
        if (!Ok)
        {
          Callback(false, FetchFailed(*It, Error));
          return;
        }
        PinHead(Uuid, Snapshot);
        ApplyName(Uuid, Snapshot.Name);
        State & S = StateOf(Uuid);
        if (Snapshot.SitesHash == LocalHash)
        {
          S.Base = Snapshot.Version;
          SetBase(S, LocalSites(S.Folder));
          S.ConflictVersion = 0;
          SyncTeam(Uuid, Callback, Attempt + 1);
          return;
        }
        HoldConflict(Uuid, false, Callback);
      });
      return;
    }
    PullHead(Uuid, Changed, Callback, Attempt);
    return;
  }
  if (!Changed && S.PendingName.isEmpty() && !S.NeedsPush)
  {
    S.ConflictVersion = 0;
    S.EmptyConflict = false;
    T.Conflict.clear();
    Callback(true, QString());
    return;
  }
  if (!T.CanEdit())
  {
    S.NeedsPush = false;
    S.PendingName.clear();
    if (Changed && (T.HeadVersion > 0))
    {
      PullHead(Uuid, true, Callback, Attempt);
    }
    else
    {
      Callback(true, QString());
    }
    return;
  }
  // Pushing nothing over something takes a decision, never an accident
  if (Changed && Sites.Sections().isEmpty() && !S.BaseSites.isEmpty() && !S.AllowEmpty)
  {
    HoldConflict(Uuid, true, Callback);
    return;
  }
  Push(Uuid, Callback, Attempt);
}

void TeamSync::HoldConflict(const QString & Uuid, bool Empty, const Done & Callback)
{
  Team & T = FTeams[Uuid];
  State & S = StateOf(Uuid);
  T.Conflict = Empty
    ? QStringLiteral("All sites of the team \"%1\" are gone from this computer. Nothing was uploaded: keep this as the team's content, or get the sites back.").arg(T.Name)
    : QStringLiteral("The sites of the team \"%1\" changed both here and on the server. Your changes are kept here and were not uploaded: "
        "keep yours for everyone, or take the team's version.").arg(T.Name);
  bool Told = Empty ? S.EmptyConflict : (S.ConflictVersion == T.HeadVersion);
  S.ConflictVersion = T.HeadVersion;
  S.EmptyConflict = Empty;
  if (!Told)
  {
    emit Notice(T.Conflict);
  }
  Callback(false, T.Conflict);
}

void TeamSync::PullHead(const QString & Uuid, bool Revert, const Done & Callback, int Attempt)
{
  qint64 Version = FTeams.value(Uuid).HeadVersion;
  Fetch(Uuid, Version, true, [this, Uuid, Revert, Callback, Attempt](bool Ok, const QString & Error, const Verified & Snapshot)
  {
    auto It = FTeams.find(Uuid);
    if (It == FTeams.end())
    {
      Callback(true, QString());
      return;
    }
    if (!Ok)
    {
      Callback(false, FetchFailed(*It, Error));
      return;
    }
    PinHead(Uuid, Snapshot);
    if (Revert)
    {
      // members use the team's sites as they are: local changes are reverted
      emit Notice(QStringLiteral("Changes to the sites of the team \"%1\" were reverted: only its owner and admins can edit them.").arg(It->Name));
    }
    QString Failed;
    if (!Apply(Uuid, Snapshot, Failed))
    {
      Callback(false, Failed);
      return;
    }
    It = FTeams.find(Uuid);
    It->Conflict.clear();
    SyncTeam(Uuid, Callback, Attempt + 1);
  });
}

QString TeamSync::FetchFailed(Team & T, const QString & Error)
{
  // A snapshot that does not verify is an event; a download that failed is not
  if (Error.startsWith(QLatin1Char('!')))
  {
    Refused(T, Error.mid(1));
    // Nothing is stuck: an older version can be made the head
    T.Error += T.CanEdit() ? QStringLiteral(" You can restore an earlier version of the team's sites.")
                           : QStringLiteral(" An owner or admin can restore an earlier version.");
    return T.Error;
  }
  return Error;
}


void TeamSync::PinHead(const QString & Uuid, const Verified & Snapshot)
{
  State & S = StateOf(Uuid);
  if (Snapshot.Version >= S.PinVersion)
  {
    S.PinVersion = Snapshot.Version;
    S.PinSha = Snapshot.Sha;
    S.PinKeyVersion = Snapshot.KeyVersion;
  }
}

bool TeamSync::CheckSnapshot(const QString & Uuid, qint64 Version, const QByteArray & Data, bool AsHead, Verified & Result, QString & Why) const
{
  const Team * T = Find(Uuid);
  const State * S = FindState(Uuid);
  auto Roster = FRosters.constFind(Uuid);
  if ((T == nullptr) || (S == nullptr) || (Roster == FRosters.cend()))
  {
    Why = QStringLiteral("the team is not known");
    return false;
  }
  TeamCrypto::Snapshot Header;
  if (!TeamCrypto::ReadSnapshot(Data, Header, &Why))
  {
    return false;
  }
  if ((Header.Team != Uuid) || (Header.Version != Version))
  {
    Why = QStringLiteral("the snapshot belongs to another team or version");
    return false;
  }
  QByteArray Identity;
  if (!Roster->AuthorAllowed(Header.Author, Header.RosterSeq, Version, &Identity))
  {
    Why = QStringLiteral("the snapshot is not by an owner or admin of the team");
    return false;
  }
  if (!TeamCrypto::VerifySnapshot(Data, Identity))
  {
    Why = QStringLiteral("the signature of the snapshot is not valid");
    return false;
  }
  if (Roster->KeyVersionAt(Header.RosterSeq) != Header.KeyVersion)
  {
    Why = QStringLiteral("the snapshot's key version does not fit the member list");
    return false;
  }
  QString Sha = Sha256(Data);
  if (AsHead)
  {
    if ((Header.KeyVersion != Roster->KeyVersion) || (Header.KeyVersion < S->PinKeyVersion) || (Version < S->PinVersion))
    {
      Why = QStringLiteral("the content is older than what this computer has seen");
      return false;
    }
    if ((S->PinVersion > 0) && (((Version == S->PinVersion) && (Sha != S->PinSha)) ||
        ((Version == S->PinVersion + 1) && (Header.Previous != S->PinSha))))
    {
      Why = QStringLiteral("the content's history was rewritten");
      return false;
    }
  }
  QByteArray Key = T->Key.Copy();
  auto WipeKey = qScopeGuard([&Key]() { Security::WipeBytes(Key); });
  if (Header.KeyVersion != T->KeyVersion)
  {
    auto Old = S->OldKeys.constFind(Header.KeyVersion);
    if ((Old == S->OldKeys.cend()) || !CloudCrypto::Open(FSync->DataKey(), *Old, OldKeyAad(Uuid, Header.KeyVersion), Key))
    {
      Why = QStringLiteral("this version was encrypted with an older team key that this computer does not have");
      return false;
    }
  }
  if (!TeamCrypto::DecodeSnapshot(Data, Key, Result.Plain, &Why))
  {
    return false;
  }
  IniFile Sites;
  if (!TeamContent::Decode(Result.Plain, Result.Name, Sites))
  {
    Why = QStringLiteral("the snapshot holds no team content");
    return false;
  }
  Result.Version = Version;
  Result.Sha = Sha;
  Result.KeyVersion = Header.KeyVersion;
  Result.SitesHash = TeamContent::Hash(Sites);
  return true;
}

void TeamSync::Fetch(const QString & Uuid, qint64 Version, bool AsHead, const FetchDone & Callback)
{
  FSync->Client().Api("GET", QStringLiteral("/v1/teams/%1/sync/download-url?version=%2").arg(Uuid).arg(Version),
    [this, Uuid, Version, AsHead, Callback](const CloudClient::Reply & Reply)
  {
    if (!Reply.Ok())
    {
      Callback(false, ApiError(Reply, QStringLiteral("Cannot download the team's sites")), Verified());
      return;
    }
    FSync->Client().Transfer("GET", QUrl(Reply.Json.value(QStringLiteral("url")).toString()), QByteArray(), {},
      [this, Uuid, Version, AsHead, Callback](const CloudClient::Reply & Get)
    {
      if (!Get.Ok())
      {
        Callback(false, QStringLiteral("Downloading the team's sites failed (%1).").arg(Get.Status), Verified());
        return;
      }
      Verified Snapshot;
      QString Why;
      bool Ok = CheckSnapshot(Uuid, Version, Get.Data, AsHead, Snapshot, Why);
      Callback(Ok, Ok ? QString() : QStringLiteral("!") + Why, Snapshot);
    });
  });
}

void TeamSync::Upload(const QString & Uuid, qint64 Base, const QByteArray & Data,
  const std::function<void(const QString & ObjectKey, const CloudClient::Reply & Failed)> & Callback)
{
  FSync->Client().Api("POST", QStringLiteral("/v1/teams/%1/sync/upload-url").arg(Uuid),
    QJsonObject{{QStringLiteral("base_version"), Base}, {QStringLiteral("size"), Data.size()}},
    [this, Data, Callback](const CloudClient::Reply & Reply)
  {
    if (!Reply.Ok())
    {
      Callback(QString(), Reply);
      return;
    }
    QString ObjectKey = Reply.Json.value(QStringLiteral("object_key")).toString();
    // What the URL was signed with; If-None-Match: it can create the object, never rewrite it
    CloudClient::Headers Headers{{"Content-Type", "application/octet-stream"}};
    QJsonObject Signed = Reply.Json.value(QStringLiteral("headers")).toObject();
    if (Signed.contains(QStringLiteral("If-None-Match")))
    {
      Headers.append({"If-None-Match", Signed.value(QStringLiteral("If-None-Match")).toString().toLatin1()});
    }
    FSync->Client().Transfer("PUT", QUrl(Reply.Json.value(QStringLiteral("url")).toString()), Data, Headers,
      [ObjectKey, Callback](const CloudClient::Reply & Put)
    {
      Callback(Put.Ok() ? ObjectKey : QString(), Put);
    });
  });
}

void TeamSync::Push(const QString & Uuid, const Done & Callback, int Attempt)
{
  Team & T = FTeams[Uuid];
  State & S = StateOf(Uuid);
  KeepFolderApart(Uuid);
  QString Folder = EnsureFolder(Uuid);
  IniFile Sites = LocalSites(Folder);
  QString Name = S.PendingName.isEmpty() ? S.Name : S.PendingName;
  Name = Name.isEmpty() ? T.Name : Name;
  TeamCrypto::Snapshot Header;
  Header.Team = Uuid;
  Header.Version = T.HeadVersion + 1;
  Header.KeyVersion = T.KeyVersion;
  Header.Author = FAccount;
  Header.RosterSeq = FRosters[Uuid].Seq;
  Header.Previous = T.HeadSha;
  Header.SavedAt = QDateTime::currentDateTimeUtc();
  QByteArray Snapshot = TeamCrypto::EncodeSnapshot(Header, T.Key.View(), FIdentity, TeamContent::Encode(Name, Sites));
  qint64 Base = T.HeadVersion;
  qint64 KeyVersion = T.KeyVersion;
  QString Sha = Sha256(Snapshot);
  auto Conflict = [this, Uuid, Callback, Attempt](const CloudClient::Reply & Reply)
  {
    // Someone else was first: never over their version; the head is looked at again
    auto It = FTeams.find(Uuid);
    QJsonObject Head = Reply.Json.value(QStringLiteral("head")).toObject();
    if ((It == FTeams.end()) || Head.isEmpty())
    {
      Callback(false, ApiError(Reply, QStringLiteral("Cannot save the team's sites")));
      return;
    }
    It->HeadVersion = Head.value(QStringLiteral("version")).toInteger();
    It->HeadSha = Head.value(QStringLiteral("sha256")).toString();
    SyncTeam(Uuid, Callback, Attempt + 1);
  };
  Upload(Uuid, Base, Snapshot, [this, Uuid, Base, Sha, KeyVersion, Name, Sites, Snapshot, Conflict, Callback](const QString & ObjectKey, const CloudClient::Reply & Failed)
  {
    if (ObjectKey.isEmpty())
    {
      if (Failed.ErrorCode() == QStringLiteral("version_conflict"))
      {
        Conflict(Failed);
        return;
      }
      Callback(false, ApiError(Failed, QStringLiteral("Uploading the team's sites failed")));
      return;
    }
    QJsonObject Commit{{QStringLiteral("base_version"), Base}, {QStringLiteral("object_key"), ObjectKey},
      {QStringLiteral("size"), Snapshot.size()}, {QStringLiteral("key_version"), KeyVersion}, {QStringLiteral("sha256"), Sha}};
    FSync->Client().Api("POST", QStringLiteral("/v1/teams/%1/sync/commit").arg(Uuid), Commit,
      [this, Uuid, Base, Sha, Name, Sites, Conflict, Callback](const CloudClient::Reply & Result)
    {
      if (Result.ErrorCode() == QStringLiteral("version_conflict"))
      {
        Conflict(Result);
        return;
      }
      if (Result.ErrorCode() == QStringLiteral("key_version_mismatch"))
      {
        // Not another admin's edit: the key was renewed meanwhile
        Callback(false, QStringLiteral("The team key changed during the upload; the sites are uploaded again at the next sync."));
        return;
      }
      if (!Result.Ok())
      {
        Callback(false, ApiError(Result, QStringLiteral("Cannot save the team's sites")));
        return;
      }
      if (Result.Json.value(QStringLiteral("version")).toInteger() != Base + 1)
      {
        Callback(false, QStringLiteral("The server did not store the team's sites as the next version."));
        return;
      }
      QString Error;
      bool Ok = AfterPush(Uuid, Base + 1, Sha, Name, Sites, Error);
      Callback(Ok, Error);
    });
  });
}

bool TeamSync::AfterPush(const QString & Uuid, qint64 Version, const QString & Sha, const QString & Name, const IniFile & Sites, QString & Error)
{
  auto It = FTeams.find(Uuid);
  if (It == FTeams.end())
  {
    return true;
  }
  State & S = StateOf(Uuid);
  It->HeadVersion = Version;
  It->HeadSha = Sha;
  It->Conflict.clear();
  QString Pending = S.PendingName;
  S.PendingName.clear();
  if (!Pending.isEmpty())
  {
    // said by this person; the others get a notice with the snapshot
    S.Name.clear();
    ApplyName(Uuid, Name);
  }
  // What went up, not what the sites are by now: a later edit is still a change
  S.Base = Version;
  SetBase(S, Sites);
  S.PinVersion = Version;
  S.PinSha = Sha;
  S.PinKeyVersion = It->KeyVersion;
  S.NeedsPush = false;
  S.AllowEmpty = false;
  S.ConflictVersion = 0;
  S.EmptyConflict = false;
  bool Moved = !S.Moves.isEmpty();
  FinishMoves(S, S.BaseSites);
  if (Moved && !SaveSites())
  {
    Error = QStringLiteral("The moved sites could not be removed from your own sites: %1").arg(FConfig->LastSaveError());
    return false;
  }
  return true;
}

void TeamSync::Rotate(const QString & Uuid, const Done & Callback)
{
  const Team * T = Find(Uuid);
  if ((T == nullptr) || T->Key.IsEmpty())
  {
    // An admin without the key has nothing it could re-encrypt
    Callback(false, QStringLiteral("The team key must be renewed, but this computer does not have the current key."));
    return;
  }
  auto Go = [this, Uuid, Callback](const Verified & Head, bool Restoring)
  {
    Team & T = FTeams[Uuid];
    TeamRoster::State Next = FRosters[Uuid];
    qint64 NewVersion = Next.KeyVersion + 1;
    QByteArray NewKey = TeamCrypto::NewTeamKey();
    // Only the keys of the signed roster, never a list from the server
    QJsonArray Wraps;
    QJsonObject Hashes;
    for (const TeamRoster::Member & M : Next.Members)
    {
      if (!M.Usable)
      {
        continue;   // nothing can be wrapped for them: they do not get the new key
      }
      QByteArray Wrap = TeamCrypto::WrapForMember(M.Box, NewKey, Uuid, NewVersion);
      Wraps.append(QJsonObject{{QStringLiteral("user_uuid"), M.Uuid}, {QStringLiteral("wrapped_team_key"), B64(Wrap)}});
      Hashes.insert(M.Uuid, TeamRoster::WrapHash(Wrap));
    }
    TeamRoster::Entry Item = TeamRoster::Make(Next, Uuid, QStringLiteral("rotate"), FAccount,
      QJsonObject{{QStringLiteral("key_version"), NewVersion}, {QStringLiteral("wraps"), Hashes}}, FIdentity);
    QString Why;
    if (!TeamRoster::Apply(Next, Item, &Why))
    {
      Callback(false, QStringLiteral("Cannot renew the team key: %1").arg(Why));
      return;
    }
    QJsonObject Body{{QStringLiteral("entry"), TeamRoster::ToWire(Item)}, {QStringLiteral("wraps"), Wraps}};
    qint64 Base = T.HeadVersion;
    QByteArray Snapshot;
    if (Base > 0)
    {
      // The head's content again, under the new key, in one step with the key
      // change: removed members cannot read it, and no half-done rotation exists
      TeamCrypto::Snapshot Header;
      Header.Team = Uuid;
      Header.Version = Base + 1;
      Header.KeyVersion = NewVersion;
      Header.Author = FAccount;
      Header.RosterSeq = Item.Seq;
      Header.Previous = T.HeadSha;
      Header.SavedAt = QDateTime::currentDateTimeUtc();
      Snapshot = TeamCrypto::EncodeSnapshot(Header, NewKey, FIdentity, Head.Plain);
    }
    auto Send = [this, Uuid, Next, NewVersion, NewKey, Base, Snapshot, Restoring, Callback](const QJsonObject & Request)
    {
      FSync->Client().Api("POST", QStringLiteral("/v1/teams/%1/rotate").arg(Uuid), Request,
        [this, Uuid, Next, NewVersion, NewKey, Base, Snapshot, Restoring, Callback](const CloudClient::Reply & Reply)
      {
        if (!Reply.Ok())
        {
          QString Code = Reply.ErrorCode();
          Callback(false, ((Code == QStringLiteral("roster_conflict")) || (Code == QStringLiteral("version_conflict")))
            ? QStringLiteral("The team changed while its key was renewed; this is repeated at the next sync.")
            : ApiError(Reply, QStringLiteral("Cannot renew the team key")));
          return;
        }
        auto It = FTeams.find(Uuid);
        if (It == FTeams.end())
        {
          Callback(true, QString());
          return;
        }
        State & S = StateOf(Uuid);
        FRosters.insert(Uuid, Next);
        S.RosterSeq = Next.Seq;
        S.RosterHash = Next.Hash;
        It->KeyVersion = NewVersion;
        It->Key.Assign(NewKey);
        It->NeedsRotation = false;
        S.OldKeys.insert(NewVersion, CloudCrypto::Seal(FSync->DataKey(), NewKey, OldKeyAad(Uuid, NewVersion)));
        if (Base > 0)
        {
          QString Sha = Sha256(Snapshot);
          It->HeadVersion = Base + 1;
          It->HeadSha = Sha;
          S.PinVersion = Base + 1;
          S.PinSha = Sha;
          S.PinKeyVersion = NewVersion;
          // The same content: local changes stay changes on top of it
          if (S.Base == Base)
          {
            S.Base = Base + 1;
          }
          // A restored version went up with the key: there is nothing left to push
          if (Restoring)
          {
            SetBase(S, LocalSites(S.Folder));
            S.AllowEmpty = false;
            FRestoring.remove(Uuid);
          }
        }
        SaveStates();
        Callback(true, QString());
      });
    };
    if (Base == 0)
    {
      Send(Body);
      return;
    }
    Upload(Uuid, Base, Snapshot, [Body, Base, Snapshot, Send, Callback](const QString & ObjectKey, const CloudClient::Reply & Failed) mutable
    {
      if (ObjectKey.isEmpty())
      {
        Callback(false, ApiError(Failed, QStringLiteral("Uploading the team's sites failed")));
        return;
      }
      Body.insert(QStringLiteral("head"), QJsonObject{{QStringLiteral("base_version"), Base}, {QStringLiteral("object_key"), ObjectKey},
        {QStringLiteral("size"), Snapshot.size()}, {QStringLiteral("sha256"), Sha256(Snapshot)}});
      Send(Body);
    });
  };
  // The admin chose an older version because the head cannot be used: that goes
  // up with the new key, and the head need not verify
  auto Restored = FRestoring.constFind(Uuid);
  if (Restored != FRestoring.cend())
  {
    Go(*Restored, true);
    return;
  }
  if (T->HeadVersion == 0)
  {
    Go(Verified(), false);
    return;
  }
  // The head, not whatever this computer has: it may be behind or changed
  Fetch(Uuid, T->HeadVersion, true, [this, Uuid, Go, Callback](bool Ok, const QString & Error, const Verified & Snapshot)
  {
    auto It = FTeams.find(Uuid);
    if (It == FTeams.end())
    {
      Callback(true, QString());
      return;
    }
    if (!Ok)
    {
      Callback(false, FetchFailed(*It, Error));
      return;
    }
    PinHead(Uuid, Snapshot);
    Go(Snapshot, false);
  });
}

//---------------------------------------------------------------------------
// Team management

void TeamSync::RefreshRoster(const QString & Uuid, const std::function<void(const QString & Error)> & Then)
{
  EnsureKeys([this, Uuid, Then](bool Ok, const QString & Error)
  {
    if (!Ok)
    {
      Then(Error);
      return;
    }
    LoadRoster(Uuid, [this, Uuid, Then](bool Gone, const QString & Error)
    {
      if (Gone)
      {
        Then(QStringLiteral("The team no longer exists."));
      }
      else if (Error.startsWith(QLatin1Char('!')))
      {
        auto It = FTeams.find(Uuid);
        if (It != FTeams.end())
        {
          Refused(*It, Error.mid(1));
        }
        Then(QStringLiteral("The team failed verification: %1.").arg(Error.mid(1)));
      }
      else
      {
        Then(Error);
      }
    });
  });
}

void TeamSync::AppendAtHead(const QString & Uuid, const QString & Type, QJsonObject Data, const Done & Callback)
{
  const State * S = FindState(Uuid);
  Data.insert(QStringLiteral("head"), (S != nullptr) ? S->PinVersion : 0);
  AppendEntry(Uuid, Type, Data, Callback);
}

void TeamSync::AppendEntry(const QString & Uuid, const QString & Type, const QJsonObject & Data, const Done & Callback, int Attempt)
{
  RefreshRoster(Uuid, [this, Uuid, Type, Data, Callback, Attempt](const QString & Error)
  {
    if (!Error.isEmpty())
    {
      Callback(false, Error);
      return;
    }
    TeamRoster::State Next = FRosters[Uuid];
    TeamRoster::Entry Item = TeamRoster::Make(Next, Uuid, Type, FAccount, Data, FIdentity);
    QString Why;
    if (!TeamRoster::Apply(Next, Item, &Why))
    {
      Callback(false, QStringLiteral("Your role in the team does not allow this (%1).").arg(Why));
      return;
    }
    FSync->Client().Api("POST", QStringLiteral("/v1/teams/%1/roster").arg(Uuid), QJsonObject{{QStringLiteral("entry"), TeamRoster::ToWire(Item)}},
      [this, Uuid, Type, Data, Next, Callback, Attempt](const CloudClient::Reply & Reply)
    {
      if ((Reply.ErrorCode() == QStringLiteral("roster_conflict")) && (Attempt + 1 < MaxAttempts))
      {
        AppendEntry(Uuid, Type, Data, Callback, Attempt + 1);
        return;
      }
      if ((Reply.ErrorCode() == QStringLiteral("head_conflict")) && (Attempt + 1 < MaxAttempts))
      {
        // The sites moved on: sync, so that what gets signed is a verified version
        SyncFor(Uuid, [this, Uuid, Type, Data, Callback, Attempt](bool, const QString &)
        {
          const State * S = FindState(Uuid);
          QJsonObject Again = Data;
          Again.insert(QStringLiteral("head"), (S != nullptr) ? S->PinVersion : 0);
          AppendEntry(Uuid, Type, Again, Callback, Attempt + 1);
        });
        return;
      }
      if (!Reply.Ok())
      {
        Callback(false, ApiError(Reply, QStringLiteral("The team could not be changed")));
        return;
      }
      FRosters.insert(Uuid, Next);
      State & S = StateOf(Uuid);
      S.RosterSeq = Next.Seq;
      S.RosterHash = Next.Hash;
      SaveStates();
      Callback(true, QString());
    });
  });
}

void TeamSync::CreateTeam(const QString & Name, const Done & Callback)
{
  QString Clean = TeamContent::NormalizeName(Name);
  if (Clean.isEmpty())
  {
    Callback(false, QStringLiteral("Enter a name for the team."));
    return;
  }
  EnsureKeys([this, Clean, Callback](bool Ok, const QString & Error)
  {
    if (!Ok)
    {
      Callback(false, Error);
      return;
    }
    QString Uuid = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
    QByteArray Key = TeamCrypto::NewTeamKey();
    QByteArray Wrap = TeamCrypto::WrapForMember(FKeys.Public, Key, Uuid, 1);
    TeamRoster::State Roster;
    TeamRoster::Entry Item = TeamRoster::Make(Roster, Uuid, QStringLiteral("create"), FAccount,
      TeamRoster::CreateData(FSync->Email(), FIdentity, FAccount, FKeys.Public, Wrap), FIdentity);
    QString Why;
    if (!TeamRoster::Apply(Roster, Item, &Why))
    {
      Callback(false, QStringLiteral("Cannot create the team: %1").arg(Why));
      return;
    }
    // One step: there is no moment when the only key exists on this computer alone
    QJsonObject Body{{QStringLiteral("uuid"), Uuid}, {QStringLiteral("entry"), TeamRoster::ToWire(Item)},
      {QStringLiteral("wrapped_team_key"), B64(Wrap)}};
    FSync->Client().Api("POST", QStringLiteral("/v1/teams"), Body, [this, Uuid, Clean, Roster, Callback](const CloudClient::Reply & Reply)
    {
      if (!Reply.Ok())
      {
        Callback(false, ApiError(Reply, QStringLiteral("Cannot create the team")));
        return;
      }
      FRosters.insert(Uuid, Roster);
      State & S = StateOf(Uuid);
      S.Name = Clean;
      S.Folder = UniqueFolder(Clean, Uuid);
      S.Hash = TeamContent::Hash(IniFile());
      S.RosterSeq = Roster.Seq;
      S.RosterHash = Roster.Hash;
      // The team's name travels in its first snapshot
      S.NeedsPush = true;
      SaveStates();
      SyncFor(Uuid, Callback);
    });
  });
}

void TeamSync::Rename(const QString & Uuid, const QString & Name, const Done & Callback)
{
  QString Clean = TeamContent::NormalizeName(Name);
  const Team * T = Find(Uuid);
  if ((T == nullptr) || !T->CanEdit() || T->Key.IsEmpty() || Clean.isEmpty())
  {
    Callback(false, QStringLiteral("Only owners and admins can rename a team."));
    return;
  }
  if (Clean == T->Name)
  {
    Callback(true, QString());
    return;
  }
  StateOf(Uuid).PendingName = Clean;
  SaveStates();
  SyncFor(Uuid, Callback);
}

void TeamSync::DeleteTeam(const QString & Uuid, const Done & Callback)
{
  EnsureKeys([this, Uuid, Callback](bool Ok, const QString & Error)
  {
    if (!Ok)
    {
      Callback(false, Error);
      return;
    }
    SignDelete(Uuid, Callback, 0);
  });
}

void TeamSync::SignDelete(const QString & Uuid, const Done & Callback, int Attempt)
{
  auto Gone = [this, Uuid, Callback]()
  {
    RemoveTeamLocally(Uuid);
    ForgetTeam(Uuid);
    SaveStates();
    Callback(true, QString());
    emit Changed();
  };
  // On top of the head the server names, verified or not: the entry only says
  // that this owner wants the team gone, and a team that cannot be verified must
  // be deletable. Another computer's rules decide nothing here; the server's do.
  FSync->Client().Api("GET", QStringLiteral("/v1/teams/%1/roster").arg(Uuid), [this, Uuid, Gone, Callback, Attempt](const CloudClient::Reply & Reply)
  {
    if ((Reply.Status == 404) && (Reply.ErrorCode() == QStringLiteral("not_found")))
    {
      Gone();
      return;
    }
    if (!Reply.Ok())
    {
      Callback(false, ApiError(Reply, QStringLiteral("Cannot delete the team")));
      return;
    }
    TeamRoster::State Head;
    Head.Seq = Reply.Json.value(QStringLiteral("seq")).toInteger();
    Head.Hash = Reply.Json.value(QStringLiteral("hash")).toString();
    TeamRoster::Entry Item = TeamRoster::Make(Head, Uuid, QStringLiteral("delete"), FAccount, QJsonObject(), FIdentity);
    FSync->Client().Api("POST", QStringLiteral("/v1/teams/%1/roster").arg(Uuid), QJsonObject{{QStringLiteral("entry"), TeamRoster::ToWire(Item)}},
      [this, Uuid, Gone, Callback, Attempt](const CloudClient::Reply & Deleted)
    {
      if ((Deleted.ErrorCode() == QStringLiteral("roster_conflict")) && (Attempt + 1 < MaxAttempts))
      {
        SignDelete(Uuid, Callback, Attempt + 1);
      }
      else if (Deleted.Ok())
      {
        Gone();
      }
      else
      {
        Callback(false, ApiError(Deleted, QStringLiteral("Cannot delete the team")));
      }
    });
  });
}

void TeamSync::Forget(const QString & Uuid, bool KeepSites, const Done & Callback)
{
  const State * S = FindState(Uuid);
  if (S == nullptr)
  {
    Callback(false, QStringLiteral("This team is not on this computer."));
    return;
  }
  QString Folder = S->Folder;
  QString Name = S->Name.isEmpty() ? Placeholder(Uuid) : S->Name;
  if (!Folder.isEmpty())
  {
    if (KeepSites)
    {
      // A plain folder from now on: not "Team: ...", so it syncs like any other
      QString Plain = Name;
      Plain.replace(QLatin1Char('/'), QLatin1Char('-'));
      Plain.replace(QLatin1Char('\\'), QLatin1Char('-'));
      MoveFolder(Folder, UniqueFolderLike(QStringLiteral("%1 (former team)").arg(Plain), Uuid));
    }
    else
    {
      RemoveFolderSites(Folder);
    }
    if (!SaveSites())
    {
      Callback(false, QStringLiteral("The sites could not be saved: %1").arg(FConfig->LastSaveError()));
      return;
    }
  }
  ForgetTeam(Uuid, true);
  SaveStates();
  Callback(true, QString());
  emit Changed();
}

void TeamSync::Leave(const QString & Uuid, const Done & Callback)
{
  AppendAtHead(Uuid, QStringLiteral("leave"), QJsonObject(), [this, Uuid, Callback](bool Ok, const QString & Error)
  {
    if (!Ok)
    {
      Callback(false, Error);
      return;
    }
    RemoveTeamLocally(Uuid);
    ForgetTeam(Uuid);
    SaveStates();
    Callback(true, QString());
    emit Changed();
  });
}

void TeamSync::ShareFolder(const QString & Uuid, const QString & Folder, const Done & Callback)
{
  QString Leaf = Folder.section(QLatin1Char('/'), -1);
  QStringList Names;
  for (const auto & Data : FConfig->StoredSessions().Sessions())
  {
    if (Data->Name.startsWith(Folder + QLatin1Char('/')) && !Data->IsWorkspace)
    {
      Names.append(Data->Name);
    }
  }
  const Team * T = Find(Uuid);
  if ((T == nullptr) || !T->CanEdit() || T->Key.IsEmpty())
  {
    Callback(false, QStringLiteral("Only owners and admins can add sites to a team."));
    return;
  }
  // The folder's structure stays: each site keeps its path below Leaf
  StoredSessionList & Sites = FConfig->StoredSessions();
  QString Target = EnsureFolder(Uuid) + QLatin1Char('/') + Leaf;
  State & S = StateOf(Uuid);
  QString TeamFolder = S.Folder;
  int Count = 0;
  for (const QString & Name : Names)
  {
    std::shared_ptr<SessionData> Data = Sites.FindByName(Name);
    if ((Data == nullptr) || Name.startsWith(CloudSettings::TeamFolderPrefix))
    {
      continue;
    }
    auto Copy = std::make_shared<SessionData>(*Data);
    Copy->Name = Target + Name.mid(Folder.size());
    Copy->MakeUniqueIn(Sites);
    Sites.Add(Copy);
    S.Moves.insert(Name, Relative(Copy->Name, TeamFolder));
    Count++;
  }
  if (Count == 0)
  {
    Callback(false, QStringLiteral("There is no site to share in this folder."));
    return;
  }
  if (!SaveSites())
  {
    Callback(false, QStringLiteral("The sites could not be saved: %1").arg(FConfig->LastSaveError()));
    return;
  }
  SaveStates();
  SyncFor(Uuid, Callback);
}

QStringList TeamSync::SharedSites(const QString & Uuid) const
{
  QStringList Result;
  const Team * T = Find(Uuid);
  if ((T == nullptr) || T->Folder.isEmpty())
  {
    return Result;
  }
  for (const auto & Data : FConfig->StoredSessions().Sessions())
  {
    if (Belongs(Data->Name, T->Folder) && !Data->IsWorkspace)
    {
      Result.append(Relative(Data->Name, T->Folder));
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
  if ((T == nullptr) || !T->CanEdit() || T->Key.IsEmpty())
  {
    Callback(false, QStringLiteral("Only owners and admins can add sites to a team."));
    return;
  }
  QString TeamFolder = EnsureFolder(Uuid);
  QString Target = TeamFolder;
  QString Sub = Subfolder.trimmed();
  while (Sub.startsWith(QLatin1Char('/'))) Sub.remove(0, 1);
  while (Sub.endsWith(QLatin1Char('/'))) Sub.chop(1);
  if (!Sub.isEmpty())
  {
    Target += QLatin1Char('/') + Sub;
  }
  StoredSessionList & Sites = FConfig->StoredSessions();
  State & S = StateOf(Uuid);
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
    Sites.Add(Copy);
    if (Move)
    {
      // The personal site goes when the commit succeeded, not before
      S.Moves.insert(Name, Relative(Copy->Name, TeamFolder));
    }
    Count++;
  }
  if (Count == 0)
  {
    Callback(false, QStringLiteral("There is no site to share (sites of a team or workspaces cannot be shared)."));
    return;
  }
  if (!SaveSites())
  {
    Callback(false, QStringLiteral("The sites could not be saved: %1").arg(FConfig->LastSaveError()));
    return;
  }
  SaveStates();
  SyncFor(Uuid, Callback);
}

void TeamSync::Unshare(const QString & Uuid, const QStringList & Names, bool KeepCopy, const Done & Callback)
{
  const Team * T = Find(Uuid);
  if ((T == nullptr) || !T->CanEdit() || T->Key.IsEmpty())
  {
    Callback(false, QStringLiteral("Only owners and admins can stop sharing a team's sites."));
    return;
  }
  QString Folder = T->Folder;
  StoredSessionList & Sites = FConfig->StoredSessions();
  QStringList Selected;
  for (const auto & Data : Sites.Sessions())
  {
    if (!Belongs(Data->Name, Folder))
    {
      continue;
    }
    QString Site = Relative(Data->Name, Folder);
    for (const QString & Name : Names)
    {
      if ((Site == Name) || (Name.endsWith(QLatin1Char('/')) && Site.startsWith(Name)))
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
      Copy->Name = Relative(Name, Folder);
      Sites.Remove(Name);
      Copy->MakeUniqueIn(Sites);
      Sites.Add(Copy);
      emit SiteMoved(Name, Copy->Name);
    }
    else
    {
      Sites.Remove(Name);
    }
  }
  if (!SaveSites())
  {
    Callback(false, QStringLiteral("The sites could not be saved: %1").arg(FConfig->LastSaveError()));
    return;
  }
  // The user asked for it, even when nothing is left
  StateOf(Uuid).AllowEmpty = true;
  SaveStates();
  SyncFor(Uuid, Callback);
}

void TeamSync::CreateInvite(const QString & Uuid, const QString & Role, int Days,
  const std::function<void(bool, const QString &, const QString &)> & Callback)
{
  const Team * T = Find(Uuid);
  if ((T == nullptr) || T->Key.IsEmpty())
  {
    Callback(false, QStringLiteral("The team key is not available on this computer yet."), QString());
    return;
  }
  SendInvite(Uuid, Role, Days, Callback, 0);
}

void TeamSync::SendInvite(const QString & Uuid, const QString & Role, int Days,
  const std::function<void(bool, const QString &, const QString &)> & Callback, int Attempt)
{
  RefreshRoster(Uuid, [this, Uuid, Role, Days, Callback, Attempt](const QString & Error)
  {
    const Team * T = Find(Uuid);
    if (!Error.isEmpty() || (T == nullptr))
    {
      Callback(false, Error.isEmpty() ? QStringLiteral("The team no longer exists.") : Error, QString());
      return;
    }
    TeamRoster::State Next = FRosters[Uuid];
    if (Next.NeedsRotation)
    {
      // The removed member knows this key: first the new one
      Callback(false, QStringLiteral("The team key is being renewed; try again after the next sync."), QString());
      return;
    }
    TeamCrypto::Invite Code = TeamCrypto::NewInvite();
    QString Expires = QDateTime::currentDateTimeUtc().addDays(Days).toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss.zzzZ"));
    QJsonObject Data{{QStringLiteral("id"), Code.Id}, {QStringLiteral("role"), Role}, {QStringLiteral("expires"), Expires},
      {QStringLiteral("public"), B64(TeamCrypto::InviteSignKey(Code).Public)}, {QStringLiteral("key_version"), Next.KeyVersion}};
    TeamRoster::Entry Item = TeamRoster::Make(Next, Uuid, QStringLiteral("invite"), FAccount, Data, FIdentity);
    QString Why;
    if (!TeamRoster::Apply(Next, Item, &Why))
    {
      Callback(false, QStringLiteral("Your role in the team does not allow this (%1).").arg(Why), QString());
      return;
    }
    TeamCrypto::InviteContent Content;
    Content.TeamKey = T->Key.Copy();
    Content.TeamName = T->Name;
    Content.RosterSeq = Item.Seq;
    Content.RosterHash = Item.Hash;
    QJsonObject Body{
      {QStringLiteral("id"), Code.Id},
      {QStringLiteral("entry"), TeamRoster::ToWire(Item)},
      {QStringLiteral("wrapped_team_key"), B64(TeamCrypto::WrapForInvite(Code, Content, Uuid, Next.KeyVersion))},
      {QStringLiteral("key_version"), Next.KeyVersion},
      {QStringLiteral("verifier_hash"), TeamCrypto::InviteVerifierHash(Code)},
    };
    FSync->Client().Api("POST", QStringLiteral("/v1/teams/%1/invites").arg(Uuid), Body,
      [this, Uuid, Role, Days, Code, Next, Callback, Attempt](const CloudClient::Reply & Reply)
    {
      QString ErrorCode = Reply.ErrorCode();
      if (((ErrorCode == QStringLiteral("roster_conflict")) || (ErrorCode == QStringLiteral("invite_id_taken"))) && (Attempt + 1 < MaxAttempts))
      {
        SendInvite(Uuid, Role, Days, Callback, Attempt + 1);
        return;
      }
      if (!Reply.Ok())
      {
        Callback(false, ApiError(Reply, QStringLiteral("Cannot create the invite")), QString());
        return;
      }
      FRosters.insert(Uuid, Next);
      State & S = StateOf(Uuid);
      S.RosterSeq = Next.Seq;
      S.RosterHash = Next.Hash;
      SaveStates();
      Callback(true, QString(), Code.Code());
    });
  });
}

void TeamSync::Invites(const QString & Uuid, const ListDone & Callback)
{
  FSync->Client().Api("GET", QStringLiteral("/v1/teams/%1/invites").arg(Uuid), [Callback](const CloudClient::Reply & Reply)
  {
    Callback(Reply.Ok(), Reply.Ok() ? QString() : ApiError(Reply, QStringLiteral("Cannot list the invites")),
      QJsonDocument::fromJson(Reply.Data).array());
  });
}

void TeamSync::RevokeInvite(const QString & Uuid, const QString & Id, const Done & Callback)
{
  AppendEntry(Uuid, QStringLiteral("revoke"), QJsonObject{{QStringLiteral("invite"), Id}}, Callback);
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
    Join(Code, Callback, 0);
  });
}

void TeamSync::Join(const TeamCrypto::Invite & Code, const Done & Callback, int Attempt)
{
  QString Verifier = B64(TeamCrypto::InviteVerifier(Code));
  FSync->Client().Api("POST", QStringLiteral("/v1/invites/%1/open").arg(Code.Id), QJsonObject{{QStringLiteral("verifier"), Verifier}},
    [this, Code, Verifier, Callback, Attempt](const CloudClient::Reply & Reply)
  {
    if (!Reply.Ok())
    {
      Callback(false, ApiError(Reply, QStringLiteral("Cannot join the team")));
      return;
    }
    QString Uuid = Reply.Json.value(QStringLiteral("team")).toObject().value(QStringLiteral("uuid")).toString();
    qint64 Version = Reply.Json.value(QStringLiteral("key_version")).toInteger();
    TeamCrypto::InviteContent Content;
    if (!TeamRoster::IsUuid(Uuid) || !TeamCrypto::UnwrapForInvite(Code, UnB64(Reply.Json.value(QStringLiteral("wrapped_team_key"))), Uuid, Version, Content))
    {
      Callback(false, QStringLiteral("The invite code does not fit the team's key."));
      return;
    }
    // The code is the anchor: it names an entry, and the hash chain fixes
    // everything before it, the genesis key included
    TeamRoster::State Next;
    QString Why;
    if (!TeamRoster::Replay(Reply.Json.value(QStringLiteral("entries")).toArray(), Uuid, Next, nullptr, &Why))
    {
      Callback(false, QStringLiteral("The team failed verification: %1.").arg(Why));
      return;
    }
    // Joining again: the floor of the earlier membership holds for an invite too
    const State * Floor = FindState(Uuid);
    if ((Floor != nullptr) && !AboveFloor(*Floor, Next, Why))
    {
      Callback(false, QStringLiteral("The team failed verification: %1.").arg(Why));
      return;
    }
    TeamCrypto::SignKey InviteKey = TeamCrypto::InviteSignKey(Code);
    auto Invite = Next.Invites.constFind(Code.Id);
    if ((Next.Hashes.value(Content.RosterSeq) != Content.RosterHash) || (Invite == Next.Invites.cend()) ||
        (Invite->Public != InviteKey.Public) || (Invite->KeyVersion != Version) || (Next.KeyVersion != Version))
    {
      Callback(false, QStringLiteral("The team failed verification: this is not the team the invite code was made for."));
      return;
    }
    QByteArray Wrap = TeamCrypto::WrapForMember(FKeys.Public, Content.TeamKey, Uuid, Version);
    TeamRoster::Entry Item = TeamRoster::Make(Next, Uuid, QStringLiteral("join"), FAccount,
      TeamRoster::JoinData(Code.Id, FSync->Email(), FIdentity, FAccount, FKeys.Public, Wrap), FIdentity, &InviteKey);
    if (!TeamRoster::Apply(Next, Item, &Why))
    {
      Callback(false, QStringLiteral("Cannot join the team: %1.").arg(Why));
      return;
    }
    QJsonObject Body{{QStringLiteral("verifier"), Verifier}, {QStringLiteral("entry"), TeamRoster::ToWire(Item)},
      {QStringLiteral("wrapped_team_key"), B64(Wrap)}};
    QString Name = Content.TeamName;
    FSync->Client().Api("POST", QStringLiteral("/v1/invites/%1/accept").arg(Code.Id), Body,
      [this, Code, Uuid, Name, Next, Callback, Attempt](const CloudClient::Reply & Accepted)
    {
      if ((Accepted.ErrorCode() == QStringLiteral("roster_conflict")) && (Attempt + 1 < MaxAttempts))
      {
        Join(Code, Callback, Attempt + 1);
        return;
      }
      if (!Accepted.Ok())
      {
        Callback(false, ApiError(Accepted, QStringLiteral("Cannot join the team")));
        return;
      }
      FRosters.insert(Uuid, Next);
      FGone.remove(Uuid);
      State & S = StateOf(Uuid);
      // Joined again: a new start; the pins of the previous membership stay valid
      S.Detached = false;
      S.Ignored = false;
      S.RosterSeq = Next.Seq;
      S.RosterHash = Next.Hash;
      S.Name = TeamContent::NormalizeName(Name);
      if (S.Name.isEmpty())
      {
        S.Name = Placeholder(Uuid);
      }
      S.Folder = UniqueFolder(S.Name, Uuid);
      S.Hash = TeamContent::Hash(IniFile());
      S.Base = 0;
      SaveStates();
      SyncFor(Uuid, Callback);
    });
  });
}

void TeamSync::Members(const QString & Uuid, const ListDone & Callback)
{
  auto Roster = FRosters.constFind(Uuid);
  if (Roster == FRosters.cend())
  {
    Callback(false, QStringLiteral("The team's member list is not available yet."), QJsonArray());
    return;
  }
  FSync->Client().Api("GET", QStringLiteral("/v1/teams/%1/members").arg(Uuid), [this, Uuid, Callback](const CloudClient::Reply & Reply)
  {
    auto Roster = FRosters.constFind(Uuid);
    if (Roster == FRosters.cend())
    {
      Callback(false, QStringLiteral("The team's member list is not available."), QJsonArray());
      return;
    }
    // Who is in the team is the roster's business; the server only knows who still exists
    QSet<QString> Accounts;
    for (const QJsonValue & V : QJsonDocument::fromJson(Reply.Data).array())
    {
      Accounts.insert(V.toObject().value(QStringLiteral("user_uuid")).toString());
    }
    QList<TeamRoster::Member> List = Roster->Members.values();
    auto Rank = [](const TeamRoster::Member & M)
    {
      return (M.Role == QStringLiteral("owner")) ? 0 : ((M.Role == QStringLiteral("admin")) ? 1 : 2);
    };
    std::sort(List.begin(), List.end(), [&](const TeamRoster::Member & A, const TeamRoster::Member & B)
    {
      return (Rank(A) != Rank(B)) ? (Rank(A) < Rank(B)) : (A.JoinedSeq < B.JoinedSeq);
    });
    QJsonArray Result;
    for (const TeamRoster::Member & M : List)
    {
      Result.append(QJsonObject{{QStringLiteral("user_uuid"), M.Uuid}, {QStringLiteral("email"), M.Email}, {QStringLiteral("role"), M.Role},
        {QStringLiteral("joined_at"), M.Joined}, {QStringLiteral("fingerprint"), TeamCrypto::Fingerprint(M.Identity)},
        {QStringLiteral("gone"), Reply.Ok() && !Accounts.contains(M.Uuid)}, {QStringLiteral("unusable"), !M.Usable}});
    }
    Callback(true, QString(), Result);
  });
}

void TeamSync::SetRole(const QString & Uuid, const QString & UserUuid, const QString & Role, const Done & Callback)
{
  AppendAtHead(Uuid, QStringLiteral("role"), QJsonObject{{QStringLiteral("user"), UserUuid}, {QStringLiteral("role"), Role}},
    [this, Uuid, Callback](bool Ok, const QString & Error)
  {
    if (!Ok)
    {
      Callback(false, Error);
      return;
    }
    SyncFor(Uuid, Callback);
  });
}

void TeamSync::RemoveMember(const QString & Uuid, const QString & UserUuid, const Done & Callback)
{
  AppendAtHead(Uuid, QStringLiteral("remove"), QJsonObject{{QStringLiteral("user"), UserUuid}},
    [this, Uuid, Callback](bool Ok, const QString & Error)
  {
    if (!Ok)
    {
      Callback(false, Error);
      return;
    }
    // the signed removal makes the next sync renew the key and push the sites again
    SyncFor(Uuid, Callback);
  });
}

void TeamSync::ResolveConflict(const QString & Uuid, bool KeepLocal, const Done & Callback)
{
  auto It = FTeams.find(Uuid);
  if ((It == FTeams.end()) || !It->CanEdit())
  {
    Callback(false, QStringLiteral("Only owners and admins can do this."));
    return;
  }
  State & S = StateOf(Uuid);
  It->Conflict.clear();
  S.ConflictVersion = 0;
  S.EmptyConflict = false;
  if (KeepLocal)
  {
    // On top of the verified head, which this person has decided about
    S.Base = It->HeadVersion;
    S.AllowEmpty = true;
    S.NeedsPush = true;
  }
  else
  {
    FForcePull.insert(Uuid);
    S.Moves.clear();
    S.PendingName.clear();
  }
  SaveStates();
  SyncFor(Uuid, Callback);
}

void TeamSync::History(const QString & Uuid, const ListDone & Callback)
{
  FSync->Client().Api("GET", QStringLiteral("/v1/teams/%1/sync/history").arg(Uuid), [Callback](const CloudClient::Reply & Reply)
  {
    Callback(Reply.Ok(), Reply.Ok() ? QString() : ApiError(Reply, QStringLiteral("Cannot read the team's history")),
      QJsonDocument::fromJson(Reply.Data).array());
  });
}

void TeamSync::Restore(const QString & Uuid, qint64 Version, const Done & Callback)
{
  const Team * T = Find(Uuid);
  if ((T == nullptr) || !T->CanEdit() || T->Key.IsEmpty())
  {
    Callback(false, QStringLiteral("Only owners and admins can restore a team's sites."));
    return;
  }
  Fetch(Uuid, Version, false, [this, Uuid, Callback](bool Ok, const QString & Error, const Verified & Snapshot)
  {
    auto It = FTeams.find(Uuid);
    if (!Ok || (It == FTeams.end()))
    {
      Callback(false, Ok ? QStringLiteral("The team no longer exists.") : QStringLiteral("This version cannot be restored: %1.").arg(Error.startsWith(QLatin1Char('!')) ? Error.mid(1) : Error));
      return;
    }
    State & S = StateOf(Uuid);
    QString Name = S.Name;
    Verified Old = Snapshot;
    Old.Version = It->HeadVersion;
    QString Failed;
    // Not a pull: the sites become the old ones and go up as a new version
    if (!Apply(Uuid, Old, Failed))
    {
      Callback(false, Failed);
      return;
    }
    if (!Snapshot.Name.isEmpty() && (Snapshot.Name != Name))
    {
      S.PendingName = Snapshot.Name;
    }
    // A local change like any other: it needs the head to be where it was
    S.Hash.clear();
    S.AllowEmpty = true;
    if (It->NeedsRotation)
    {
      FRestoring.insert(Uuid, Snapshot);
    }
    SaveStates();
    SyncFor(Uuid, Callback);
  });
}
