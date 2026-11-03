#include "mobile/RemoteSession.h"
#include "mobile/MobileUserInterface.h"

#include "core/Configuration.h"
#include "core/SessionData.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QStandardPaths>
#include <QUrl>

#include <algorithm>

//---------------------------------------------------------------------------
void FilesModel::SetFiles(const RemoteFileList & Files)
{
  beginResetModel();
  FFiles.clear();
  for (const RemoteFile & F : Files)
  {
    if ((F.FileName != QStringLiteral(".")) && !F.IsParentDirectory && (F.FileName != QStringLiteral("..")))
    {
      FFiles.append(F);
    }
  }
  // folders first, natural order (as the desktop panels)
  std::sort(FFiles.begin(), FFiles.end(), [](const RemoteFile & A, const RemoteFile & B)
  {
    if (A.IsDirectory != B.IsDirectory)
    {
      return A.IsDirectory;
    }
    return StoredSessionList::CompareLogicalText(A.FileName, B.FileName) < 0;
  });
  endResetModel();
}

int FilesModel::rowCount(const QModelIndex & Parent) const
{
  return Parent.isValid() ? 0 : static_cast<int>(FFiles.size());
}

QVariant FilesModel::data(const QModelIndex & Index, int Role) const
{
  if (!Index.isValid() || (Index.row() >= FFiles.size()))
  {
    return QVariant();
  }
  const RemoteFile & F = FFiles.at(Index.row());
  switch (Role)
  {
    case NameRole: return F.FileName;
    case IsDirRole: return F.IsDirectory;
    case IsLinkRole: return F.IsSymLink;
    case SizeRole: return F.IsDirectory ? QString() : QLocale().formattedDataSize(F.Size, 1, QLocale::DataSizeTraditionalFormat);
    case DateRole: return QLocale().toString(F.Modification, QLocale::ShortFormat);
    case RightsRole: return F.RightsText();
    default: return QVariant();
  }
}

QHash<int, QByteArray> FilesModel::roleNames() const
{
  return {{NameRole, "name"}, {IsDirRole, "isDir"}, {SizeRole, "size"}, {DateRole, "date"},
    {RightsRole, "rights"}, {IsLinkRole, "isLink"}};
}

//---------------------------------------------------------------------------
RemoteSession::RemoteSession(MobileUserInterface * UI, QObject * Parent) : QObject(Parent), FUI(UI)
{
}

RemoteSession::~RemoteSession()
{
  close();
}

bool RemoteSession::connected() const
{
  return (FTerminal != nullptr) && FTerminal->Active();
}

QString RemoteSession::downloadFolder() const
{
  QString Dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
  if (Dir.isEmpty())
  {
    Dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/Downloads");
  }
  return Dir + QStringLiteral("/NixSCP");
}

void RemoteSession::SetBusy(bool Busy, const QString & Status)
{
  FBusy = Busy;
  if (!Status.isEmpty() || !Busy)
  {
    FStatus = Status;
    emit statusChanged();
  }
  emit stateChanged();
}

bool RemoteSession::Failed(const OperationResult & Result)
{
  if (Result.Ok)
  {
    return false;
  }
  if (!Result.Aborted)
  {
    emit error(Result.Message, Result.MoreMessages);
  }
  return true;
}

QString RemoteSession::RemotePath(const QString & Name) const
{
  if (Name.startsWith(QLatin1Char('/')))
  {
    return Name;
  }
  return FDirectory.endsWith(QLatin1Char('/')) ? (FDirectory + Name) : (FDirectory + QLatin1Char('/') + Name);
}

void RemoteSession::open(const QString & SiteName)
{
  close();
  std::shared_ptr<SessionData> Data = Configuration::Instance().StoredSessions().FindByName(SiteName);
  if (Data == nullptr)
  {
    emit error(QStringLiteral("The site \"%1\" no longer exists.").arg(SiteName), QString());
    return;
  }
  FSite = SiteName;
  SessionData Open = *Data;
  if (Open.KnownHostsFile.isEmpty())
  {
    // the app's own known_hosts (Android apps cannot write ~/.ssh)
    QString SshDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/.ssh");
    QDir().mkpath(SshDir);
    QFile::setPermissions(SshDir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    Open.KnownHostsFile = SshDir + QStringLiteral("/known_hosts");
  }
  FTerminal = new Terminal(Open, FUI, this);
  connect(FTerminal, &Terminal::Disconnected, this, [this](const QString & Message, const QString & More)
  {
    emit error(Message, More);
    emit stateChanged();
  });
  SetBusy(true, QStringLiteral("Connecting to %1...").arg(Data->HostNameExpanded()));
  FTerminal->Open([this](const OperationResult & Result)
  {
    SetBusy(false);
    if (Failed(Result))
    {
      close();
      return;
    }
    Load(FTerminal->CurrentDirectory());
  });
}

void RemoteSession::close()
{
  if (FTerminal != nullptr)
  {
    FTerminal->Close();
    FTerminal->deleteLater();
    FTerminal = nullptr;
  }
  FFiles.SetFiles({});
  FSite.clear();
  FDirectory.clear();
  FBusy = false;
  emit directoryChanged();
  emit stateChanged();
}

void RemoteSession::Load(const QString & Path)
{
  if (FTerminal == nullptr)
  {
    return;
  }
  SetBusy(true, QStringLiteral("Reading %1...").arg(Path));
  FTerminal->ReadDirectory(Path, [this](const OperationResult & Result, const QString & Directory, const RemoteFileList & Files)
  {
    SetBusy(false);
    if (Failed(Result))
    {
      return;
    }
    FDirectory = Directory;
    FFiles.SetFiles(Files);
    emit directoryChanged();
  });
}

void RemoteSession::cd(const QString & Path)
{
  Load(Path);
}

void RemoteSession::enter(const QString & Name)
{
  if (Name == QStringLiteral(".."))
  {
    if (FDirectory == QStringLiteral("/"))
    {
      return;
    }
    QString Parent = FDirectory.left(FDirectory.lastIndexOf(QLatin1Char('/')));
    Load(Parent.isEmpty() ? QStringLiteral("/") : Parent);
    return;
  }
  Load(RemotePath(Name));
}

void RemoteSession::refresh()
{
  Load(FDirectory);
}

void RemoteSession::download(const QStringList & Names)
{
  if ((FTerminal == nullptr) || Names.isEmpty())
  {
    return;
  }
  QString Target = downloadFolder();
  QDir().mkpath(Target);
  QStringList Paths;
  QStringList Local;
  for (const QString & Name : Names)
  {
    Paths.append(RemotePath(Name));
    Local.append(Target + QLatin1Char('/') + Name);
  }
  SetBusy(true, (Names.size() == 1) ? QStringLiteral("Downloading %1...").arg(Names.first()) :
    QStringLiteral("Downloading %1 files...").arg(Names.size()));
  FTerminal->CopyToLocal(Paths, Target, Configuration::Instance().DefaultCopyParam(), false,
    [this, Local](const OperationResult & Result)
  {
    SetBusy(false);
    if (!Failed(Result))
    {
      emit downloaded(Local);
    }
  });
}

// A local file for the core: file:// URLs as paths; content:// documents
// (Android's file picker) copied into the cache first, under their names
static QString LocalCopy(const QString & Path, const QString & CacheDir, int Index)
{
  QUrl Url(Path);
  if (Url.isLocalFile())
  {
    return Url.toLocalFile();
  }
  if (!Path.startsWith(QStringLiteral("content:")))
  {
    return Path;
  }
  QString Name = QFileInfo(Path).fileName();
  if (Name.isEmpty() || Name.contains(QLatin1Char('/')) || Name.contains(QLatin1Char(':')))
  {
    Name = QStringLiteral("upload-%1").arg(Index + 1);
  }
  QString Target = CacheDir + QLatin1Char('/') + Name;
  QFile::remove(Target);
  return QFile::copy(Path, Target) ? Target : QString();
}

void RemoteSession::openFile(const QString & Name, bool Share)
{
  if ((FTerminal == nullptr) || Name.isEmpty())
  {
    return;
  }
  QString Target = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/open");
  QDir(Target).removeRecursively();
  QDir().mkpath(Target);
  SetBusy(true, QStringLiteral("Opening %1...").arg(Name));
  QString Local = Target + QLatin1Char('/') + Name;
  FTerminal->CopyToLocal({RemotePath(Name)}, Target, Configuration::Instance().DefaultCopyParam(), false,
    [this, Local, Share](const OperationResult & Result)
  {
    SetBusy(false);
    if (!Failed(Result))
    {
      emit opened(Local, Share);
    }
  });
}

void RemoteSession::upload(const QStringList & Sources)
{
  if ((FTerminal == nullptr) || Sources.isEmpty())
  {
    return;
  }
  QString CacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/upload");
  QDir(CacheDir).removeRecursively();
  QDir().mkpath(CacheDir);
  QStringList LocalPaths;
  for (int I = 0; I < Sources.size(); I++)
  {
    QString Local = LocalCopy(Sources.at(I), CacheDir, I);
    if (Local.isEmpty())
    {
      emit error(QStringLiteral("Cannot read the file %1.").arg(Sources.at(I)), QString());
      return;
    }
    LocalPaths.append(Local);
  }
  SetBusy(true, (LocalPaths.size() == 1) ? QStringLiteral("Uploading %1...").arg(QFileInfo(LocalPaths.first()).fileName()) :
    QStringLiteral("Uploading %1 files...").arg(LocalPaths.size()));
  FTerminal->CopyToRemote(LocalPaths, FDirectory, Configuration::Instance().DefaultCopyParam(), false,
    [this](const OperationResult & Result)
  {
    SetBusy(false);
    Failed(Result);
    refresh();
  });
}

void RemoteSession::remove(const QStringList & Names)
{
  if ((FTerminal == nullptr) || Names.isEmpty())
  {
    return;
  }
  QStringList Paths;
  for (const QString & Name : Names)
  {
    Paths.append(RemotePath(Name));
  }
  SetBusy(true, QStringLiteral("Deleting..."));
  FTerminal->DeleteFiles(Paths, [this](const OperationResult & Result)
  {
    SetBusy(false);
    Failed(Result);
    refresh();
  });
}

void RemoteSession::rename(const QString & From, const QString & To)
{
  if ((FTerminal == nullptr) || To.isEmpty() || (From == To))
  {
    return;
  }
  SetBusy(true, QStringLiteral("Renaming..."));
  FTerminal->RenameFile(RemotePath(From), RemotePath(To), [this](const OperationResult & Result)
  {
    SetBusy(false);
    Failed(Result);
    refresh();
  });
}

void RemoteSession::createFolder(const QString & Name)
{
  if ((FTerminal == nullptr) || Name.isEmpty())
  {
    return;
  }
  SetBusy(true, QStringLiteral("Creating %1...").arg(Name));
  FTerminal->CreateDirectory(RemotePath(Name), 0755, [this](const OperationResult & Result)
  {
    SetBusy(false);
    Failed(Result);
    refresh();
  });
}
