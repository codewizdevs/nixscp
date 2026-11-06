#include "mobile/TeamsModel.h"

#include "core/CloudSettings.h"
#include "core/Configuration.h"
#include "core/SessionData.h"
#include "core/TeamSync.h"

#include <QSet>

namespace
{
TeamSync & Teams() { return TeamSync::Instance(); }
}

TeamsModel::TeamsModel(QObject * Parent) : QObject(Parent)
{
}

void TeamsModel::Start()
{
  connect(&Teams(), &TeamSync::Changed, this, &TeamsModel::changed);
  connect(&Teams(), &TeamSync::SitesReceived, this, &TeamsModel::sitesReceived);
}

QVariantList TeamsModel::teams() const
{
  QVariantList Result;
  QList<TeamSync::Team> List = Teams().Teams();
  std::sort(List.begin(), List.end(), [](const TeamSync::Team & A, const TeamSync::Team & B)
  {
    return A.Name.compare(B.Name, Qt::CaseInsensitive) < 0;
  });
  for (const TeamSync::Team & T : List)
  {
    Result.append(QVariantMap{{QStringLiteral("uuid"), T.Uuid}, {QStringLiteral("name"), T.Name}, {QStringLiteral("folder"), T.Folder},
      {QStringLiteral("role"), T.Role}, {QStringLiteral("canEdit"), T.CanEdit()},
      {QStringLiteral("members"), T.MemberCount}, {QStringLiteral("needsRotation"), T.NeedsRotation},
      {QStringLiteral("keyReady"), !T.Key.IsEmpty()}, {QStringLiteral("error"), T.Error}, {QStringLiteral("conflict"), T.Conflict}});
  }
  return Result;
}

bool TeamsModel::available() const
{
  return Teams().Available();
}

std::function<void(bool, const QString &)> TeamsModel::Done(const QString & Op)
{
  FBusy++;
  emit busyChanged();
  return [this, Op](bool Ok, const QString & Error)
  {
    FBusy--;
    emit busyChanged();
    emit changed();
    emit finished(Op, Ok, Error, QVariant());
  };
}

void TeamsModel::refresh() { Teams().SyncAll(Done(QStringLiteral("refresh"))); }
void TeamsModel::create(const QString & Name) { Teams().CreateTeam(Name.trimmed(), Done(QStringLiteral("create"))); }
void TeamsModel::rename(const QString & Uuid, const QString & Name) { Teams().Rename(Uuid, Name.trimmed(), Done(QStringLiteral("rename"))); }
void TeamsModel::remove(const QString & Uuid) { Teams().DeleteTeam(Uuid, Done(QStringLiteral("delete"))); }
void TeamsModel::leave(const QString & Uuid) { Teams().Leave(Uuid, Done(QStringLiteral("leave"))); }
void TeamsModel::forget(const QString & Uuid, bool KeepSites) { Teams().Forget(Uuid, KeepSites, Done(QStringLiteral("forget"))); }
void TeamsModel::join(const QString & Code) { Teams().JoinWithCode(Code.trimmed(), Done(QStringLiteral("join"))); }
void TeamsModel::shareFolder(const QString & Uuid, const QString & Folder) { Teams().ShareFolder(Uuid, Folder, Done(QStringLiteral("share"))); }
void TeamsModel::revokeInvite(const QString & Uuid, const QString & Id) { Teams().RevokeInvite(Uuid, Id, Done(QStringLiteral("revoke"))); }
void TeamsModel::setRole(const QString & Uuid, const QString & User, const QString & Role) { Teams().SetRole(Uuid, User, Role, Done(QStringLiteral("role"))); }
void TeamsModel::removeMember(const QString & Uuid, const QString & User) { Teams().RemoveMember(Uuid, User, Done(QStringLiteral("removeMember"))); }

void TeamsModel::invite(const QString & Uuid, const QString & Role, int Days)
{
  FBusy++;
  emit busyChanged();
  Teams().CreateInvite(Uuid, Role, Days, [this](bool Ok, const QString & Error, const QString & Code)
  {
    FBusy--;
    emit busyChanged();
    emit finished(QStringLiteral("invite"), Ok, Error, Code);
  });
}

void TeamsModel::invites(const QString & Uuid)
{
  Teams().Invites(Uuid, [this, Uuid](bool Ok, const QString & Error, const QJsonArray & List)
  {
    emit finished(QStringLiteral("invites"), Ok, Error, QVariantMap{{QStringLiteral("uuid"), Uuid}, {QStringLiteral("list"), List.toVariantList()}});
  });
}

void TeamsModel::members(const QString & Uuid)
{
  Teams().Members(Uuid, [this, Uuid](bool Ok, const QString & Error, const QJsonArray & List)
  {
    emit finished(QStringLiteral("members"), Ok, Error, QVariantMap{{QStringLiteral("uuid"), Uuid}, {QStringLiteral("list"), List.toVariantList()}});
  });
}

void TeamsModel::history(const QString & Uuid)
{
  Teams().History(Uuid, [this, Uuid](bool Ok, const QString & Error, const QJsonArray & List)
  {
    emit finished(QStringLiteral("history"), Ok, Error, QVariantMap{{QStringLiteral("uuid"), Uuid}, {QStringLiteral("list"), List.toVariantList()}});
  });
}

void TeamsModel::restore(const QString & Uuid, double Version)
{
  Teams().Restore(Uuid, static_cast<qint64>(Version), Done(QStringLiteral("restore")));
}

void TeamsModel::resolveConflict(const QString & Uuid, bool KeepLocal)
{
  Teams().ResolveConflict(Uuid, KeepLocal, Done(QStringLiteral("resolve")));
}

QString TeamsModel::fingerprint() const
{
  return Teams().Fingerprint();
}

QStringList TeamsModel::localFolders() const
{
  QSet<QString> Folders;
  for (const auto & Data : Configuration::Instance().StoredSessions().Sessions())
  {
    QString Folder = Data->FolderName();
    if (Data->IsWorkspace || Folder.isEmpty() || Folder.startsWith(CloudSettings::TeamFolderPrefix))
    {
      continue;
    }
    // the folder and the ones above it (a team gets a folder with its subfolders)
    for (int Depth = 1; Depth <= Folder.count(QLatin1Char('/')) + 1; Depth++)
    {
      Folders.insert(Folder.section(QLatin1Char('/'), 0, Depth - 1));
    }
  }
  QStringList Result(Folders.begin(), Folders.end());
  Result.sort(Qt::CaseInsensitive);
  return Result;
}

QStringList TeamsModel::localSites() const
{
  QStringList Result;
  for (const auto & Data : Configuration::Instance().StoredSessions().Sessions())
  {
    if (!Data->IsWorkspace && !Data->Name.startsWith(CloudSettings::TeamFolderPrefix))
    {
      Result.append(Data->Name);
    }
  }
  std::sort(Result.begin(), Result.end(), [](const QString & A, const QString & B)
  {
    return StoredSessionList::CompareLogicalText(A, B) < 0;
  });
  return Result;
}

QStringList TeamsModel::sharedSites(const QString & Uuid) const
{
  return Teams().SharedSites(Uuid);
}

void TeamsModel::shareSite(const QString & Uuid, const QString & Site, const QString & Subfolder, bool Move)
{
  Teams().ShareSites(Uuid, {Site}, Subfolder, Move, Done(QStringLiteral("shareSite")));
}

void TeamsModel::unshare(const QString & Uuid, const QStringList & Names, bool KeepCopy)
{
  Teams().Unshare(Uuid, Names, KeepCopy, Done(QStringLiteral("unshare")));
}

QVariantMap TeamsModel::siteTeam(const QString & Site) const
{
  const TeamSync::Team * T = Teams().TeamOfSite(Site);
  if (T == nullptr)
  {
    return QVariantMap();
  }
  return QVariantMap{{QStringLiteral("uuid"), T->Uuid}, {QStringLiteral("name"), T->Name},
    {QStringLiteral("relative"), Site.mid(T->Folder.size() + 1)}, {QStringLiteral("canEdit"), T->CanEdit()}};
}
