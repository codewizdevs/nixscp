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
    Result.append(QVariantMap{{QStringLiteral("uuid"), T.Uuid}, {QStringLiteral("name"), T.Name},
      {QStringLiteral("role"), T.Role}, {QStringLiteral("canEdit"), T.CanEdit()},
      {QStringLiteral("members"), T.MemberCount}, {QStringLiteral("needsRotation"), T.NeedsRotation},
      {QStringLiteral("keyReady"), !T.Key.isEmpty()}});
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
  Teams().Invites(Uuid, [this](bool Ok, const QString & Error, const QJsonArray & List)
  {
    emit finished(QStringLiteral("invites"), Ok, Error, List.toVariantList());
  });
}

void TeamsModel::members(const QString & Uuid)
{
  Teams().Members(Uuid, [this](bool Ok, const QString & Error, const QJsonArray & List)
  {
    emit finished(QStringLiteral("members"), Ok, Error, List.toVariantList());
  });
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
