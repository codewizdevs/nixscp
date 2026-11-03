#include "mobile/SitesModel.h"

#include "core/Configuration.h"
#include "core/SessionData.h"

#include <QUrl>

#include <algorithm>

SitesModel::SitesModel(QObject * Parent) : QAbstractListModel(Parent)
{
  reload();
}

void SitesModel::reload()
{
  beginResetModel();
  FRows.clear();
  for (const auto & Data : Configuration::Instance().StoredSessions().Sessions())
  {
    if (Data->IsWorkspace)
    {
      continue;
    }
    FRows.append(Row{Data->Name, Data->LocalName(), Data->FolderName(), Data->HostNameExpanded(),
      Data->UserNameExpanded(), Data->ProtocolStr()});
  }
  // folders first in natural order, then the site names
  std::sort(FRows.begin(), FRows.end(), [](const Row & A, const Row & B)
  {
    int Folder = StoredSessionList::CompareLogicalText(A.Folder, B.Folder);
    return (Folder != 0) ? (Folder < 0) : (StoredSessionList::CompareLogicalText(A.Title, B.Title) < 0);
  });
  endResetModel();
  emit countChanged();
}

int SitesModel::importWinScpIni(const QString & FileName)
{
  Configuration & Config = Configuration::Instance();
  QString Error;
  QUrl Url(FileName);
  int Count = Config.StoredSessions().ImportFromWinSCPIni(Url.isLocalFile() ? Url.toLocalFile() : FileName, &Error);
  if (Count < 0)
  {
    return -1;
  }
  Config.Save();
  reload();
  return Count;
}

bool SitesModel::addSite(const QString & Name, const QString & Protocol, const QString & Host, int Port,
  const QString & User, const QString & Password)
{
  if (Host.trimmed().isEmpty())
  {
    return false;
  }
  Configuration & Config = Configuration::Instance();
  SessionData Data = Config.StoredSessions().DefaultSettings();
  Data.HostName = Host.trimmed();
  Data.UserName = User.trimmed();
  Data.Password = Password;
  Data.Ftps = ftpsNone;
  if (Protocol == QStringLiteral("SCP")) Data.FSProtocol = fsSCPonly;
  else if (Protocol == QStringLiteral("FTP")) Data.FSProtocol = fsFTP;
  else if (Protocol == QStringLiteral("FTPS")) { Data.FSProtocol = fsFTP; Data.Ftps = ftpsExplicitTls; }
  else if (Protocol == QStringLiteral("WebDAV")) Data.FSProtocol = fsWebDAV;
  else if (Protocol == QStringLiteral("WebDAVS")) { Data.FSProtocol = fsWebDAV; Data.Ftps = ftpsImplicit; }
  else Data.FSProtocol = fsSFTP;
  Data.PortNumber = (Port > 0) ? Port : Data.DefaultPort();
  QString SiteName = Name.trimmed().isEmpty() ? Data.DefaultSessionName() : Name.trimmed();
  Config.StoredSessions().NewSession(SiteName, Data);
  bool Saved = Config.Save();
  reload();
  return Saved;
}

void SitesModel::removeSite(const QString & Name)
{
  Configuration & Config = Configuration::Instance();
  Config.StoredSessions().Remove(Name);
  Config.Save();
  reload();
}

int SitesModel::rowCount(const QModelIndex & Parent) const
{
  return Parent.isValid() ? 0 : static_cast<int>(FRows.size());
}

QVariant SitesModel::data(const QModelIndex & Index, int Role) const
{
  if (!Index.isValid() || (Index.row() >= FRows.size()))
  {
    return QVariant();
  }
  const Row & R = FRows.at(Index.row());
  switch (Role)
  {
    case NameRole: return R.Name;
    case TitleRole: return R.Title;
    case FolderRole: return R.Folder;
    case HostRole: return R.Host;
    case UserRole: return R.User;
    case ProtocolRole: return R.Protocol;
    default: return QVariant();
  }
}

QHash<int, QByteArray> SitesModel::roleNames() const
{
  return {{NameRole, "name"}, {TitleRole, "title"}, {FolderRole, "folder"}, {HostRole, "host"},
    {UserRole, "user"}, {ProtocolRole, "protocol"}};
}
