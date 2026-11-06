#include "mobile/SitesModel.h"

#include "core/Configuration.h"
#include "core/SessionData.h"
#include "mobile/KeyStore.h"
#include "mobile/MobileSettings.h"

#include <QFileInfo>
#include <QUrl>

#include <algorithm>

namespace
{
const QString FavouritesSection = QStringLiteral("\x01" "favourites");
const QString RecentSection = QStringLiteral("\x01" "recent");
const int RecentCount = 5;
}

SitesModel::SitesModel(MobileSettings * Settings, QObject * Parent) :
  QAbstractListModel(Parent),
  FMobile(Settings)
{
  reload();
}

void SitesModel::LoadState()
{
  PrivateState & State = FMobile->Private();
  FFavourites = State.Strings(QStringLiteral("favourites"));
  FRecent = State.Strings(QStringLiteral("recent"));
  const QStringList Collapsed = State.Strings(QStringLiteral("collapsed"));
  FCollapsed = QSet<QString>(Collapsed.begin(), Collapsed.end());
  Rebuild();
}

void SitesModel::SaveState()
{
  PrivateState & State = FMobile->Private();
  bool Ok = State.SetStrings(QStringLiteral("favourites"), FFavourites);
  Ok = State.SetStrings(QStringLiteral("recent"), FRecent) && Ok;
  Ok = State.SetStrings(QStringLiteral("collapsed"), QStringList(FCollapsed.begin(), FCollapsed.end())) && Ok;
  if (!Ok)
  {
    emit failed(QStringLiteral("The list of sites could not be saved."));
  }
}

void SitesModel::reload()
{
  int Before = count();
  QList<Site> Sites;
  for (const auto & Data : Configuration::Instance().StoredSessions().Sessions())
  {
    if (Data->IsWorkspace)
    {
      continue;
    }
    Sites.append(Site{Data->Name, Data->LocalName(), Data->FolderName(), Data->HostNameExpanded(),
      Data->UserNameExpanded(), Data->ProtocolStr()});
  }
  // folders in natural order (the sites without a folder first), then the site names
  std::sort(Sites.begin(), Sites.end(), [](const Site & A, const Site & B)
  {
    int Folder = StoredSessionList::CompareLogicalText(A.Folder, B.Folder);
    return (Folder != 0) ? (Folder < 0) : (StoredSessionList::CompareLogicalText(A.Title, B.Title) < 0);
  });
  // a sync that brought nothing new (every minute with teams) leaves the
  // list as it is: a reset would scroll it to the top
  if (FReloaded && (Sites == FSites))
  {
    return;
  }
  FReloaded = true;
  FSites = Sites;
  Rebuild();
  if (count() != Before)
  {
    emit countChanged();
  }
}

void SitesModel::setFilter(const QString & Value)
{
  if (Value != FFilter)
  {
    FFilter = Value;
    Rebuild();
    emit filterChanged();
  }
}

void SitesModel::Rebuild()
{
  beginResetModel();
  FRows.clear();
  FMatches = 0;
  // every word of the search in the name, host, user or protocol
  const QStringList Words = FFilter.split(QLatin1Char(' '), Qt::SkipEmptyParts);
  auto Matches = [&Words](const Site & S)
  {
    QString Text = S.Name + QLatin1Char(' ') + S.Host + QLatin1Char(' ') + S.User + QLatin1Char('@') + S.Host +
      QLatin1Char(' ') + S.Protocol;
    for (const QString & Word : Words)
    {
      if (!Text.contains(Word, Qt::CaseInsensitive))
      {
        return false;
      }
    }
    return true;
  };
  auto Find = [this](const QString & Name) -> const Site *
  {
    for (const Site & S : FSites)
    {
      if (S.Name == Name)
      {
        return &S;
      }
    }
    return nullptr;
  };
  auto AddSection = [this](const QString & Key, const QString & Title, const QList<const Site *> & Sites, bool Collapsible)
  {
    if (Sites.isEmpty())
    {
      return;
    }
    bool Collapsed = Collapsible && FCollapsed.contains(Key);
    Row Header;
    Header.Header = true;
    Header.Section = Key;
    Header.Title = Title;
    Header.Count = static_cast<int>(Sites.size());
    FRows.append(Header);
    if (!Collapsed)
    {
      for (const Site * S : Sites)
      {
        Row R;
        R.Section = Key;
        R.S = S;
        FRows.append(R);
      }
    }
  };

  bool Searching = !Words.isEmpty();
  if (!Searching)
  {
    QList<const Site *> Favourites;
    for (const QString & Name : FFavourites)
    {
      if (const Site * S = Find(Name))
      {
        Favourites.append(S);
      }
    }
    std::sort(Favourites.begin(), Favourites.end(), [](const Site * A, const Site * B)
    {
      return StoredSessionList::CompareLogicalText(A->Title, B->Title) < 0;
    });
    AddSection(FavouritesSection, QStringLiteral("Favourites"), Favourites, true);
    QList<const Site *> Recent;
    for (const QString & Name : FRecent)
    {
      const Site * S = Find(Name);
      if ((S != nullptr) && (Recent.size() < RecentCount))
      {
        Recent.append(S);
      }
    }
    AddSection(RecentSection, QStringLiteral("Recent"), Recent, true);
  }
  // the folders (in a search: not collapsed, only matches)
  int I = 0;
  while (I < FSites.size())
  {
    QString Folder = FSites.at(I).Folder;
    QList<const Site *> Sites;
    for (; (I < FSites.size()) && (FSites.at(I).Folder == Folder); I++)
    {
      if (!Searching || Matches(FSites.at(I)))
      {
        Sites.append(&FSites.at(I));
      }
    }
    FMatches += static_cast<int>(Sites.size());
    AddSection(Folder, Folder.isEmpty() ? QStringLiteral("Sites") : Folder, Sites, !Searching);
  }
  endResetModel();
  emit rowsChanged();
}

void SitesModel::MigrateKeys()
{
  Configuration & Config = Configuration::Instance();
  bool Changed = false;
  for (const auto & Data : Config.StoredSessions().Sessions())
  {
    if (Data->PublicKeyFile.startsWith(KeyStore::Folder() + QLatin1Char('/')))
    {
      FMobile->setSiteKey(Data->Name, QFileInfo(Data->PublicKeyFile).fileName());
      Data->PublicKeyFile.clear();
      Changed = true;
    }
  }
  if (Changed)
  {
    if (!Config.Save())
    {
      emit failed(QStringLiteral("The sites could not be saved."));
    }
    reload();
  }
}

int SitesModel::importWinScpIni(const QString & FileName)
{
  Configuration & Config = Configuration::Instance();
  QString Error;
  QUrl Url(FileName);
  int Count = Config.StoredSessions().ImportFromWinSCPIni(Url.isLocalFile() ? Url.toLocalFile() : FileName, &Error);
  // an unreadable file comes back as 0 with an error
  if ((Count < 0) || ((Count == 0) && !Error.isEmpty()))
  {
    return -1;
  }
  bool Saved = Config.Save();
  reload();
  return Saved ? Count : -1;
}

QVariantMap SitesModel::siteData(const QString & Name) const
{
  QVariantMap Result;
  std::shared_ptr<SessionData> Data = Configuration::Instance().StoredSessions().FindByName(Name);
  if (Data == nullptr)
  {
    return Result;
  }
  Result[QStringLiteral("name")] = Data->Name;
  Result[QStringLiteral("protocol")] = Data->ProtocolStr();
  Result[QStringLiteral("host")] = Data->HostName;
  Result[QStringLiteral("port")] = (Data->PortNumber == Data->DefaultPort()) ? 0 : Data->PortNumber;
  Result[QStringLiteral("user")] = Data->UserName;
  Result[QStringLiteral("hasPassword")] = Data->HasPassword();
  Result[QStringLiteral("directory")] = Data->RemoteDirectory;
  // this phone's key for the site (another device's key file does not apply here)
  QString Key = FMobile->siteKey(Name);
  QString KeyFile = !Key.isEmpty() ? (KeyStore::Folder() + QLatin1Char('/') + Key)
    : (Data->PublicKeyFile.startsWith(KeyStore::Folder() + QLatin1Char('/')) ? Data->PublicKeyFile : QString());
  // a key that was deleted is no longer the site's
  Result[QStringLiteral("keyFile")] = QFileInfo::exists(KeyFile) ? KeyFile : QString();
  return Result;
}

QString SitesModel::saveSite(const QString & Original, const QString & Name, const QString & Protocol,
  const QString & Host, int Port, const QString & User, const QString & Password, bool KeepPassword,
  const QString & Directory, const QString & KeyFile)
{
  if (Host.trimmed().isEmpty())
  {
    return QStringLiteral("Enter the host name.");
  }
  Configuration & Config = Configuration::Instance();
  StoredSessionList & Sessions = Config.StoredSessions();
  std::shared_ptr<SessionData> Existing = Original.isEmpty() ? nullptr : Sessions.FindByName(Original);
  if (!Original.isEmpty() && (Existing == nullptr))
  {
    return QStringLiteral("The site \"%1\" no longer exists.").arg(Original);
  }
  SessionData Data = Existing ? *Existing : Sessions.DefaultSettings();
  QString OldPassword = Data.Password;
  Data.HostName = Host.trimmed();
  Data.UserName = User.trimmed();
  Data.Password = (Password.isEmpty() && KeepPassword) ? OldPassword : Password;
  Data.RemoteDirectory = Directory.trimmed();
  // the key is this phone's choice; the site keeps another device's key file
  if (Data.PublicKeyFile.startsWith(KeyStore::Folder() + QLatin1Char('/')))
  {
    Data.PublicKeyFile.clear();
  }
  // the protocol only when it changed (keeps e.g. SFTP-only or implicit TLS)
  if (!Existing || (Existing->ProtocolStr() != Protocol))
  {
    Data.Ftps = ftpsNone;
    if (Protocol == QStringLiteral("SCP")) Data.FSProtocol = fsSCPonly;
    else if (Protocol == QStringLiteral("FTP")) Data.FSProtocol = fsFTP;
    else if (Protocol == QStringLiteral("FTPS")) { Data.FSProtocol = fsFTP; Data.Ftps = ftpsExplicitTls; }
    else if (Protocol == QStringLiteral("WebDAV")) Data.FSProtocol = fsWebDAV;
    else if (Protocol == QStringLiteral("WebDAVS")) { Data.FSProtocol = fsWebDAV; Data.Ftps = ftpsImplicit; }
    else if (Protocol == QStringLiteral("S3")) { Data.FSProtocol = fsS3; Data.Ftps = ftpsImplicit; }
    else Data.FSProtocol = fsSFTP;
  }
  Data.PortNumber = (Port > 0) ? Port : Data.DefaultPort();
  QString SiteName = Name.trimmed();
  while (SiteName.contains(QStringLiteral("//")))
  {
    SiteName.replace(QStringLiteral("//"), QStringLiteral("/"));
  }
  if (SiteName.startsWith(QLatin1Char('/'))) SiteName.remove(0, 1);
  if (SiteName.endsWith(QLatin1Char('/'))) SiteName.chop(1);
  if (SiteName.isEmpty())
  {
    SiteName = Data.DefaultSessionName();
  }
  if ((SiteName != Original) && Sessions.FindByName(SiteName))
  {
    return QStringLiteral("A site named \"%1\" already exists.").arg(SiteName);
  }
  if (Existing && (SiteName != Original))
  {
    Sessions.Remove(Original);
    RenameState(Original, SiteName);
  }
  Sessions.NewSession(SiteName, Data);
  if (!Config.Save())
  {
    return QStringLiteral("The site could not be saved.");
  }
  if (!Original.isEmpty() && (SiteName != Original))
  {
    FMobile->setSiteKey(Original, QString());
  }
  FMobile->setSiteKey(SiteName, KeyFile.startsWith(KeyStore::Folder() + QLatin1Char('/')) ? QFileInfo(KeyFile).fileName() : QString());
  reload();
  return QString();
}

QString SitesModel::duplicateSite(const QString & Name)
{
  Configuration & Config = Configuration::Instance();
  StoredSessionList & Sessions = Config.StoredSessions();
  std::shared_ptr<SessionData> Data = Sessions.FindByName(Name);
  if (Data == nullptr)
  {
    return QString();
  }
  int Index = 2;
  QString Copy;
  do
  {
    Copy = QStringLiteral("%1 (%2)").arg(Name).arg(Index++);
  }
  while (Sessions.FindByName(Copy));
  Sessions.NewSession(Copy, *Data);
  if (!Config.Save())
  {
    emit failed(QStringLiteral("The copy of the site could not be saved."));
  }
  FMobile->setSiteKey(Copy, FMobile->siteKey(Name));
  reload();
  return Copy;
}

void SitesModel::removeSite(const QString & Name)
{
  Configuration & Config = Configuration::Instance();
  Config.StoredSessions().Remove(Name);
  if (!Config.Save())
  {
    emit failed(QStringLiteral("The site could not be deleted."));
  }
  FMobile->setSiteKey(Name, QString());
  FFavourites.removeAll(Name);
  FRecent.removeAll(Name);
  SaveState();
  reload();
}

void SitesModel::RenameState(const QString & From, const QString & To)
{
  for (QStringList * List : {&FFavourites, &FRecent})
  {
    for (QString & Name : *List)
    {
      if (Name == From)
      {
        Name = To;
      }
    }
  }
  SaveState();
}

void SitesModel::SiteMoved(const QString & From, const QString & To)
{
  RenameState(From, To);
  QString Key = FMobile->siteKey(From);
  if (!Key.isEmpty())
  {
    FMobile->setSiteKey(To, Key);
    FMobile->setSiteKey(From, QString());
  }
}

void SitesModel::toggleFavourite(const QString & Name)
{
  if (!FFavourites.removeAll(Name))
  {
    FFavourites.append(Name);
  }
  SaveState();
  Rebuild();
}

void SitesModel::toggleSection(const QString & Section)
{
  if (!FCollapsed.remove(Section))
  {
    FCollapsed.insert(Section);
  }
  SaveState();
  Rebuild();
}

void SitesModel::addRecent(const QString & Name)
{
  FRecent.removeAll(Name);
  FRecent.prepend(Name);
  while (FRecent.size() > 20)
  {
    FRecent.removeLast();
  }
  SaveState();
  if (FFilter.trimmed().isEmpty())
  {
    Rebuild();
  }
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
    case IsHeaderRole: return R.Header;
    case SectionRole: return R.Section;
    case CollapsedRole: return R.Header && FCollapsed.contains(R.Section) && FFilter.trimmed().isEmpty();
    case SiteCountRole: return R.Count;
    case TitleRole: return R.Header ? R.Title : R.S->Title;
    case NameRole: return R.S ? R.S->Name : QString();
    case FolderRole: return R.S ? R.S->Folder : QString();
    case HostRole: return R.S ? R.S->Host : QString();
    case UserRole: return R.S ? R.S->User : QString();
    case ProtocolRole: return R.S ? R.S->Protocol : QString();
    case FavouriteRole: return R.S && FFavourites.contains(R.S->Name);
    default: return QVariant();
  }
}

QHash<int, QByteArray> SitesModel::roleNames() const
{
  return {{NameRole, "name"}, {TitleRole, "title"}, {FolderRole, "folder"}, {HostRole, "host"},
    {UserRole, "user"}, {ProtocolRole, "protocol"}, {IsHeaderRole, "isHeader"}, {SectionRole, "section"},
    {CollapsedRole, "collapsed"}, {SiteCountRole, "siteCount"}, {FavouriteRole, "favourite"}};
}
