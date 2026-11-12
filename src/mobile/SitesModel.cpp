#include "mobile/SitesModel.h"

#include "core/AgentAccess.h"
#include "core/Configuration.h"
#include "core/SessionData.h"
#include "core/TeamSync.h"
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
    // a note item has no server: no host, user or protocol to show or search
    if (Data->NoteOnly)
    {
      Sites.append(Site{Data->Name, Data->LocalName(), Data->FolderName(), QString(), QString(), QString(), true, Data->Note});
      continue;
    }
    Sites.append(Site{Data->Name, Data->LocalName(), Data->FolderName(), Data->HostNameExpanded(),
      Data->UserNameExpanded(), Data->ProtocolStr(), false, Data->Note});
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

void SitesModel::setConnectOnly(bool Value)
{
  if (Value != FConnectOnly)
  {
    FConnectOnly = Value;
    Rebuild();
    emit connectOnlyChanged();
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
  // a note item is no choice where a site to connect to is picked
  auto Shown = [this](const Site & S) { return !FConnectOnly || !S.NoteOnly; };
  auto Find = [this, &Shown](const QString & Name) -> const Site *
  {
    for (const Site & S : FSites)
    {
      if (S.Name == Name)
      {
        return Shown(S) ? &S : nullptr;
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
      // recent connections: a note item never connects
      if ((S != nullptr) && !S->NoteOnly && (Recent.size() < RecentCount))
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
      if (Shown(FSites.at(I)) && (!Searching || Matches(FSites.at(I))))
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
  Result[QStringLiteral("note")] = Data->Note;
  Result[QStringLiteral("noteItem")] = Data->NoteOnly;
  Result[QStringLiteral("canEditNote")] = canEditNote(Name);
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
  // a note item has no server to set up (renameSite renames it)
  if (Existing && Existing->NoteOnly)
  {
    return QStringLiteral("\"%1\" is a note, not a site.").arg(Original);
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
  QString SiteName = cleanName(Name);
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

QString SitesModel::cleanName(const QString & Name) const
{
  QString Result = Name.trimmed();
  while (Result.contains(QStringLiteral("//")))
  {
    Result.replace(QStringLiteral("//"), QStringLiteral("/"));
  }
  if (Result.startsWith(QLatin1Char('/'))) Result.remove(0, 1);
  if (Result.endsWith(QLatin1Char('/'))) Result.chop(1);
  return Result.trimmed();
}

QString SitesModel::renameSite(const QString & Original, const QString & Name)
{
  Configuration & Config = Configuration::Instance();
  StoredSessionList & Sessions = Config.StoredSessions();
  std::shared_ptr<SessionData> Existing = Sessions.FindByName(Original);
  if (Existing == nullptr)
  {
    return QStringLiteral("\"%1\" no longer exists.").arg(Original);
  }
  QString NewName = cleanName(Name);
  if (NewName.isEmpty())
  {
    return QStringLiteral("Enter a name.");
  }
  if (NewName == Original)
  {
    return QString();
  }
  // names are compared without case: only another site's name is taken
  std::shared_ptr<SessionData> Taken = Sessions.FindByName(NewName);
  if ((Taken != nullptr) && (Taken != Existing))
  {
    return QStringLiteral("\"%1\" already exists.").arg(NewName);
  }
  SessionData Data = *Existing;
  Sessions.Remove(Original);
  Sessions.NewSession(NewName, Data);
  if (!Config.Save())
  {
    // as it was
    Sessions.Remove(NewName);
    Sessions.NewSession(Original, Data);
    return QStringLiteral("The new name could not be saved.");
  }
  // the favourite, recent and key entries follow
  SiteMoved(Original, NewName);
  reload();
  return QString();
}

//---------------------------------------------------------------------------
int SitesModel::maxNoteBytes() const
{
  return AgentAccess::MaxNoteBytes;
}

bool SitesModel::isNote(const QString & Name) const
{
  std::shared_ptr<SessionData> Data = Configuration::Instance().StoredSessions().FindByName(Name);
  return (Data != nullptr) && Data->NoteOnly;
}

bool SitesModel::canEditNote(const QString & Name) const
{
  // as on the desktop (LoginDialog): a member of a team reads its notes
  const TeamSync::Team * Team = TeamSync::Instance().TeamOfSite(Name);
  return (Team == nullptr) || Team->CanEdit();
}

QString SitesModel::saveNote(const QString & Name, const QString & Text)
{
  Configuration & Config = Configuration::Instance();
  std::shared_ptr<SessionData> Data = Config.StoredSessions().FindByName(Name);
  if (Data == nullptr)
  {
    return QStringLiteral("\"%1\" no longer exists, so the note cannot be saved.").arg(Name);
  }
  if (!canEditNote(Name))
  {
    return QStringLiteral("Only the team's owners and admins edit this note.");
  }
  QString Reason;
  if (!AgentAccess::NoteAcceptable(Text, &Reason))
  {
    return Reason;
  }
  if (Data->Note == Text)
  {
    return QString();
  }
  // the note only: the site's other settings stay as they are stored
  QString Before = Data->Note;
  Data->Note = Text;
  if (!Config.Save())
  {
    Data->Note = Before;
    return QStringLiteral("The note could not be saved.");
  }
  reload();
  return QString();
}

QString SitesModel::newNote(const QString & Name)
{
  QString NoteName = cleanName(Name);
  if (NoteName.isEmpty())
  {
    emit failed(QStringLiteral("Enter a name for the note."));
    return QString();
  }
  Configuration & Config = Configuration::Instance();
  StoredSessionList & Sessions = Config.StoredSessions();
  // as on the desktop (LoginDialog::NewNoteActionExecute)
  auto Note = std::make_shared<SessionData>();
  Note->Name = NoteName;
  Note->NoteOnly = true;
  Note->MakeUniqueIn(Sessions);
  Sessions.Add(Note);
  if (!Config.Save())
  {
    Sessions.Remove(Note->Name);
    emit failed(QStringLiteral("The note could not be saved."));
    return QString();
  }
  reload();
  return Note->Name;
}

int SitesModel::noteBytes(const QString & Text) const
{
  return static_cast<int>(Text.toUtf8().size());
}

QString SitesModel::noteSize(const QString & Text) const
{
  int Bytes = noteBytes(Text);
  QString Size = (Bytes < 1024) ? QStringLiteral("%1 bytes").arg(Bytes) : QStringLiteral("%1 KB").arg(Bytes / 1024.0, 0, 'f', 1);
  return QStringLiteral("%1 of %2 KB").arg(Size).arg(AgentAccess::MaxNoteBytes / 1024);
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
  if (isNote(Name))
  {
    return;
  }
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
    case NoteItemRole: return R.S && R.S->NoteOnly;
    case HasNoteRole: return R.S && !R.S->Note.trimmed().isEmpty();
    case NoteLineRole:
    {
      if (!R.S)
      {
        return QString();
      }
      // the first line with text
      for (const QString & Line : R.S->Note.split(QLatin1Char('\n')))
      {
        if (!Line.trimmed().isEmpty())
        {
          return Line.trimmed().left(200);
        }
      }
      return QString();
    }
    default: return QVariant();
  }
}

QHash<int, QByteArray> SitesModel::roleNames() const
{
  return {{NameRole, "name"}, {TitleRole, "title"}, {FolderRole, "folder"}, {HostRole, "host"},
    {UserRole, "user"}, {ProtocolRole, "protocol"}, {IsHeaderRole, "isHeader"}, {SectionRole, "section"},
    {CollapsedRole, "collapsed"}, {SiteCountRole, "siteCount"}, {FavouriteRole, "favourite"},
    {NoteItemRole, "noteItem"}, {HasNoteRole, "hasNote"}, {NoteLineRole, "noteLine"}};
}
