#include "mobile/RemoteSession.h"
#include "mobile/AndroidFiles.h"
#include "mobile/KeyStore.h"
#include "mobile/MobileSettings.h"
#include "mobile/MobileUserInterface.h"
#include "mobile/MobileQueue.h"
#include "mobile/PrivateState.h"
#include "mobile/TextFile.h"

#include "core/Bookmarks.h"
#include "core/FileFind.h"

#include "core/Configuration.h"
#include "core/SessionData.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QThread>
#include <QTimer>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>
#include <QtCore/private/qzipwriter_p.h>

#include <atomic>

namespace
{
QList<FileListModel::Entry> Entries(const RemoteFileList & Files)
{
  QList<FileListModel::Entry> Result;
  for (const RemoteFile & F : Files)
  {
    if ((F.FileName == QStringLiteral(".")) || F.IsParentDirectory || (F.FileName == QStringLiteral("..")))
    {
      continue;
    }
    FileListModel::Entry E;
    E.Name = F.FileName;
    E.IsDir = F.IsDirectory;
    E.IsLink = F.IsSymLink;
    E.Size = F.Size;
    E.Modified = F.Modification;
    E.Rights = F.RightsText();
    E.Mode = F.Permissions & 07777;
    E.Owner = F.Owner;
    E.Group = F.Group;
    Result.append(E);
  }
  return Result;
}

QString DataSize(qint64 Bytes)
{
  return QLocale().formattedDataSize(Bytes, 1, QLocale::DataSizeTraditionalFormat);
}

int NextCacheId = 0;
std::atomic<int> ClosingConnections{0};
// QZipWriter holds a file in memory while it adds it
const qint64 MaxZippedFile = 256 * 1024 * 1024;
}

//---------------------------------------------------------------------------
RemoteSession::RemoteSession(MobileUserInterface * UI, MobileSettings * Settings, MobileQueue * Queue, QObject * Parent) :
  QObject(Parent), FUI(UI), FSettings(Settings), FQueue(Queue), FCacheId(++NextCacheId)
{
  FProgressTimer.setInterval(250);
  connect(&FProgressTimer, &QTimer::timeout, this, &RemoteSession::UpdateProgress);
  if (FQueue != nullptr)
  {
    connect(FQueue, &MobileQueue::added, this, [this](RemoteSession * Owner)
    {
      if (Owner == this)
      {
        RunNext();
      }
    });
    connect(FQueue, &MobileQueue::cancelRequested, this, [this](int Id)
    {
      if ((Id == FBackItem) && (FBack != nullptr))
      {
        FBack->Progress().Cancel();
      }
    });
    // a file came into the folder shown here (from any tab)
    connect(FQueue, &MobileQueue::uploadEnded, this, [this](const QString & Directory)
    {
      if (connected() && (Directory == FDirectory))
      {
        if (FBusy)
        {
          FStale = true;
        }
        else
        {
          refresh();
        }
      }
    });
  }
  // the running queue item's progress
  FBackTimer.setInterval(500);
  connect(&FBackTimer, &QTimer::timeout, this, [this]()
  {
    if ((FBack == nullptr) || (FBackItem == 0))
    {
      return;
    }
    FileOperationProgress::Snapshot P = FBack->Progress().Get();
    qint64 Size = (P.TotalSize > 0) ? P.TotalSize : P.FileSize;
    qint64 Done = (P.TotalSize > 0) ? P.TotalTransferred : P.FileTransferred;
    double Progress = (Size > 0) ? std::min(1.0, double(Done) / double(Size)) : -1;
    QString Detail = (Size > 0) ? QStringLiteral("%1 of %2").arg(DataSize(Done), DataSize(Size)) : DataSize(Done);
    if (P.ElapsedMs >= 1000)
    {
      Detail += QStringLiteral(" · %1/s").arg(DataSize(P.TotalTransferred * 1000 / P.ElapsedMs));
    }
    FQueue->SetProgress(FBackItem, Progress, Detail);
  });
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
  if (!Busy)
  {
    // what waited for the connection
    if (std::any_of(FEdits.cbegin(), FEdits.cend(), [](const Edit & E) { return E.HasUnsent && !E.Failed; }))
    {
      QTimer::singleShot(0, this, &RemoteSession::SendEdits);
    }
    if (FStale && connected())
    {
      FStale = false;
      QTimer::singleShot(0, this, &RemoteSession::refresh);
    }
  }
}

bool RemoteSession::Failed(const OperationResult & Result)
{
  if (Result.Ok)
  {
    return false;
  }
  // after close(): what was running ends, nobody is waiting for it
  if (!Result.Aborted && (FTerminal != nullptr))
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

//---------------------------------------------------------------------------
void RemoteSession::StartTransfer(const QString & Status)
{
  // transferring before the status: QML starts the transfer service on it
  FTransferring = true;
  FProgress = -1;
  FProgressText.clear();
  FProgressFile.clear();
  SetBusy(true, Status);
  FProgressTimer.start();
  emit progressChanged();
}

void RemoteSession::StopTransfer()
{
  FProgressTimer.stop();
  FTransferring = false;
  emit progressChanged();
  SetBusy(false);
}

void RemoteSession::UpdateProgress()
{
  if (FTerminal == nullptr)
  {
    return;
  }
  FileOperationProgress::Snapshot P = FTerminal->Progress().Get();
  if (!P.InProgress)
  {
    return;
  }
  // the total is known once the files were scanned; until then the file's
  qint64 Size = (P.TotalSize > 0) ? P.TotalSize : P.FileSize;
  qint64 Done = (P.TotalSize > 0) ? P.TotalTransferred : P.FileTransferred;
  FProgress = (Size > 0) ? std::min(1.0, double(Done) / double(Size)) : -1;
  QString Text = (Size > 0) ? QStringLiteral("%1 of %2").arg(DataSize(Done), DataSize(Size)) : DataSize(Done);
  if (P.ElapsedMs >= 1000)
  {
    Text += QStringLiteral(" · %1/s").arg(DataSize(P.TotalTransferred * 1000 / P.ElapsedMs));
  }
  if (P.Count > 1)
  {
    Text += QStringLiteral(" · %1 of %2 files").arg(std::min(P.FilesFinished + 1, P.Count)).arg(P.Count);
  }
  FProgressText = Text;
  FProgressFile = QFileInfo(P.FileName).fileName();
  emit progressChanged();
}

void RemoteSession::cancel()
{
  if (FTerminal == nullptr)
  {
    return;
  }
  if (!connected())
  {
    // still connecting: give up the connection
    close();
    return;
  }
  if (FFind != nullptr)
  {
    stopSearch();
    return;
  }
  FTerminal->Progress().Cancel();
}

//---------------------------------------------------------------------------
void RemoteSession::open(const QString & SiteName)
{
  close();
  std::shared_ptr<SessionData> Data = Configuration::Instance().StoredSessions().FindByName(SiteName);
  if (Data == nullptr)
  {
    emit error(QStringLiteral("The site \"%1\" no longer exists.").arg(SiteName), QString());
    // a shortcut of a deleted site: no tab remains for it
    emit closed();
    return;
  }
  FAsker = FUI->NewAsker();
  FSite = SiteName;
  FData = *Data;
  SessionData Open = *Data;
  // the phone's timeout and keepalive
  FSettings->Apply(Open);
  // always the app's own known_hosts (Android apps cannot write ~/.ssh, and
  // a synced site may carry another device's path)
  QString SshDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/.ssh");
  QDir().mkpath(SshDir);
  QFile::setPermissions(SshDir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
  Open.KnownHostsFile = SshDir + QStringLiteral("/known_hosts");
  // the key: this phone's choice (the core opens an encrypted key in memory);
  // a key that was deleted, or a key file of another device, does not exist
  // here: the password is asked for instead
  QString KeyName = FSettings->siteKey(SiteName);
  QString KeyPath = !KeyName.isEmpty() ? (KeyStore::Folder() + QLatin1Char('/') + KeyName) : Open.PublicKeyFile;
  Open.PublicKeyFile = QFileInfo::exists(KeyPath) ? KeyPath : QString();
  FOpenData = Open;
  Terminal * Term = new Terminal(Open, FAsker.get(), this);
  KeepAsker(Term);
  FTerminal = Term;
  connect(Term, &Terminal::Disconnected, this, [this, Term](const QString & Message, const QString & More)
  {
    if (FTerminal == Term)
    {
      emit error(Message, More);
      emit stateChanged();
    }
  });
  SetBusy(true, QStringLiteral("Connecting to %1...").arg(Data->HostNameExpanded()));
  QPointer<Terminal> Guard(Term);
  Term->Open([this, Guard](const OperationResult & Result)
  {
    // cancelled meanwhile (closed, or another site opened)
    if ((Guard == nullptr) || (FTerminal != Guard))
    {
      return;
    }
    SetBusy(false);
    if (Failed(Result))
    {
      close();
      return;
    }
    // what a new host's key added to known_hosts: the name no longer readable
    KeyStore::HashKnownHosts(FOpenData.KnownHostsFile);
    emit established();
    Load(FTerminal->CurrentDirectory());
  });
}

void RemoteSession::KeepAsker(QObject * Connection)
{
  connect(Connection, &QObject::destroyed, [Asker = FAsker]() {});
}

void RemoteSession::Dispose(Terminal * Term)
{
  // on its own: this session (its parent) may be gone first at the app's end
  disconnect(Term, nullptr, this, nullptr);
  Term->setParent(nullptr);
  Term->Close();
  ClosingConnections++;
  connect(Term, &QObject::destroyed, []() { ClosingConnections--; });
  FClosing++;
  connect(Term, &QObject::destroyed, this, [this]()
  {
    if (--FClosing == 0)
    {
      // what the worker was still writing
      MobileCache::RemoveSession(FCacheId);
      if (FDeleteClosed)
      {
        deleteLater();
      }
    }
  });
  // ~Terminal waits for its worker with touches held back: only when the
  // worker is idle, so that a server that does not answer freezes nothing
  if (Term->Busy())
  {
    connect(Term, &Terminal::BusyChanged, Term, [Term](bool Busy)
    {
      if (!Busy)
      {
        Term->deleteLater();
      }
    });
  }
  else
  {
    Term->deleteLater();
  }
}

void RemoteSession::DeleteWhenClosed()
{
  FDeleteClosed = true;
  if (FClosing == 0)
  {
    deleteLater();
  }
}

int RemoteSession::Closing()
{
  return ClosingConnections + TerminalSession::Ending();
}

void RemoteSession::close()
{
  bool Ended = (FTerminal != nullptr);
  // nothing waits for a dialog of this session any more
  if (FAsker != nullptr)
  {
    FAsker->Cancel();
  }
  if (FTerminal != nullptr)
  {
    Terminal * Term = FTerminal;
    FTerminal = nullptr;
    Dispose(Term);
  }
  FProgressTimer.stop();
  FTransferring = false;
  if ((FQueue != nullptr) && Ended)
  {
    FQueue->Abandon(this);
  }
  FThumbs.clear();
  stopSearch();
  DropBack();
  if (FShell != nullptr)
  {
    disconnect(FShell, nullptr, this, nullptr);
    FShell->setParent(nullptr);
    FShell->Dispose();
    FShell = nullptr;
    emit terminalChanged();
  }
  FFiles.SetEntries({});
  // what another app changed and could not be uploaded is not thrown away
  QStringList Unsent;
  for (const Edit & E : std::as_const(FEdits))
  {
    QFileInfo Info(E.Local);
    if (!E.BuiltIn && Info.exists() && ((Info.lastModified() != E.Modified) || (Info.size() != E.Size)))
    {
      QString Kept = MobileCache::Folder(QStringLiteral("edit"), 0) + QLatin1Char('/') + QString::number(FCacheId) + QLatin1Char('/') +
        QFileInfo(Info.path()).fileName();
      QDir().mkpath(Kept);
      Kept += QLatin1Char('/') + Info.fileName();
      if (QFile::rename(E.Local, Kept))
      {
        Unsent.append(Kept);
      }
    }
  }
  FEdits.clear();
  FUploadingEdit.clear();
  FStale = false;
  // nothing of this session stays in the cache
  MobileCache::RemoveSession(FCacheId);
  // a host accepted on another connection of the session
  if (Ended)
  {
    KeyStore::HashKnownHosts(FOpenData.KnownHostsFile);
  }
  FSite.clear();
  FDirectory.clear();
  FBusy = false;
  FStatus.clear();
  emit progressChanged();
  emit statusChanged();
  emit directoryChanged();
  emit stateChanged();
  for (const QString & Kept : std::as_const(Unsent))
  {
    emit editUnsent(Kept);
  }
  if (Ended)
  {
    emit closed();
  }
}

void RemoteSession::Load(const QString & Path)
{
  if (FTerminal == nullptr)
  {
    return;
  }
  SetBusy(true, QStringLiteral("Reading %1...").arg(Path));
  FTerminal->ReadDirectory(Path, [this, Path](const OperationResult & Result, const QString & Directory, const RemoteFileList & Files)
  {
    // closed meanwhile
    if (FTerminal == nullptr)
    {
      return;
    }
    SetBusy(false);
    if (Failed(Result))
    {
      // the site's start folder does not exist (any more): the home folder
      // rather than an empty pane
      if (FDirectory.isEmpty() && (Path != QStringLiteral(".")) && connected())
      {
        Load(QStringLiteral("."));
      }
      return;
    }
    bool Changed = (Directory != FDirectory);
    FDirectory = Directory;
    FFiles.SetEntries(Entries(Files));
    if (Changed)
    {
      // a new folder starts without the previous one's filter
      FFiles.setFilter(QString());
    }
    FSettings->addRecentFolder(FSite, Directory);
    QueueThumbnails(Files);
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

//---------------------------------------------------------------------------
void RemoteSession::Fetch(const QStringList & Names, const QString & Target, bool Clean, const QString & Status,
  std::function<void(const QStringList &)> Done, bool Exact)
{
  if ((FTerminal == nullptr) || Names.isEmpty())
  {
    return;
  }
  if (Clean)
  {
    QDir(Target).removeRecursively();
  }
  QDir().mkpath(Target);
  QStringList Paths;
  QStringList Local;
  for (const QString & Name : Names)
  {
    Paths.append(RemotePath(Name));
    Local.append(Target + QLatin1Char('/') + Name.mid(Name.lastIndexOf(QLatin1Char('/')) + 1));
  }
  StartTransfer(Status);
  CopyParam Param = Configuration::Instance().DefaultCopyParam();
  if (Exact)
  {
    Param.Mode = CopyParam::tmBinary;
  }
  FTerminal->CopyToLocal(Paths, Target, Param, false,
    [this, Local, Done](const OperationResult & Result)
  {
    StopTransfer();
    if (!Failed(Result))
    {
      Done(Local);
    }
  });
}

void RemoteSession::download(const QStringList & Names, const QString & TargetDir)
{
  if ((FQueue == nullptr) || Names.isEmpty())
  {
    return;
  }
  QString Target = TargetDir.isEmpty() ? downloadFolder() : TargetDir;
  QDir().mkpath(Target);
  FQueue->Add(this, MobileQueue::Download, remotePaths(Names), Target, TargetDir.isEmpty());
}

void RemoteSession::openFile(const QString & Name, bool Share)
{
  QString Target = MobileCache::Folder(QStringLiteral("open"), FCacheId);
  Fetch({Name}, Target, true, QStringLiteral("Opening %1...").arg(Name), [this, Share](const QStringList & Local)
  {
    emit opened(Local.first(), Share);
  });
}

void RemoteSession::shareFiles(const QStringList & Names)
{
  QString Target = MobileCache::Folder(QStringLiteral("open"), FCacheId);
  Fetch(Names, Target, true, QStringLiteral("Preparing %1 files...").arg(Names.size()), [this](const QStringList & Local)
  {
    emit shared(Local);
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
  // the document's own name (shared and picked files are content:// URIs)
  QString Name = AndroidFiles::DisplayName(Path);
  if (Name.isEmpty() || Name.contains(QLatin1Char('/')) || Name.contains(QLatin1Char(':')))
  {
    Name = QStringLiteral("upload-%1").arg(Index + 1);
  }
  // two documents of the same name are two files
  QString Target = CacheDir + QLatin1Char('/') + TextFile::UniqueName(CacheDir, Name);
  // by hand: QFile::copy() takes over the document's permissions, and a
  // content:// document has none (the copy could not be read)
  QFile Source(Path);
  QFile Copy(Target);
  if (!Source.open(QIODevice::ReadOnly) || !Copy.open(QIODevice::WriteOnly))
  {
    return QString();
  }
  // all of it or nothing: a read error is not the end of the document
  if (!TextFile::Copy(Source, Copy) || !Copy.flush())
  {
    Copy.remove();
    return QString();
  }
  Copy.close();
  Copy.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  return Target;
}

void RemoteSession::upload(const QStringList & Sources)
{
  if ((FTerminal == nullptr) || Sources.isEmpty())
  {
    return;
  }
  // a folder per upload: queued uploads keep their copies until they run
  QString CacheDir = MobileCache::Folder(QStringLiteral("upload"), FCacheId) + QLatin1Char('/') + QString::number(++FUploadCounter);
  QDir().mkpath(CacheDir);
  QString Target = FDirectory;
  SetBusy(true, QStringLiteral("Preparing the files..."));
  // documents of other apps may be large or far away: not on the GUI thread
  QPointer<RemoteSession> Self(this);
  QThread * Worker = QThread::create([Self, Sources, CacheDir, Target]()
  {
    QStringList LocalPaths;
    QString Bad;
    for (int I = 0; (I < Sources.size()) && Bad.isEmpty(); I++)
    {
      QString Local = LocalCopy(Sources.at(I), CacheDir, I);
      if (Local.isEmpty())
      {
        Bad = Sources.at(I);
      }
      LocalPaths.append(Local);
    }
    QMetaObject::invokeMethod(QCoreApplication::instance(), [Self, LocalPaths, Bad, CacheDir, Target]()
    {
      if (Self != nullptr)
      {
        Self->CopiesReady(LocalPaths, Bad, CacheDir, Target);
      }
      else
      {
        QDir(CacheDir).removeRecursively();
      }
    }, Qt::QueuedConnection);
  });
  connect(Worker, &QThread::finished, Worker, &QObject::deleteLater);
  Worker->start();
}

void RemoteSession::CopiesReady(const QStringList & LocalPaths, const QString & Bad, const QString & CacheDir, const QString & Target)
{
  if (FTerminal == nullptr)
  {
    QDir(CacheDir).removeRecursively();
    return;
  }
  SetBusy(false);
  if (!Bad.isEmpty())
  {
    QDir(CacheDir).removeRecursively();
    emit error(QStringLiteral("Cannot read the file %1.").arg(Bad), QString());
    return;
  }
  if (FQueue != nullptr)
  {
    FQueue->Add(this, MobileQueue::Upload, LocalPaths, Target, false, CacheDir);
  }
}

void RemoteSession::UploadTo(const QStringList & LocalPaths, const QString & RemoteDir, std::function<void(bool)> Done,
  bool Overwrite)
{
  if (FTerminal == nullptr)
  {
    return;
  }
  StartTransfer((LocalPaths.size() == 1) ? QStringLiteral("Uploading %1...").arg(QFileInfo(LocalPaths.first()).fileName()) :
    QStringLiteral("Uploading %1 files...").arg(LocalPaths.size()));
  CopyParam Param = Configuration::Instance().DefaultCopyParam();
  if (Overwrite)
  {
    Param.ConfirmOverwriting = false;
    // an edited file: its bytes, whatever the transfer settings are
    Param.Mode = CopyParam::tmBinary;
  }
  FTerminal->CopyToRemote(LocalPaths, RemoteDir, Param, false,
    [this, Done](const OperationResult & Result)
  {
    StopTransfer();
    Done(!Failed(Result));
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

//---------------------------------------------------------------------------
namespace
{
QString Quote(const QString & Text)
{
  QString Result = Text;
  Result.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
  return QLatin1Char('\'') + Result + QLatin1Char('\'');
}
}

QString RemoteSession::InDirectory(const QString & Command) const
{
  if ((FTerminal == nullptr) || (FTerminal->CurrentDirectory() == FDirectory) || FDirectory.isEmpty())
  {
    return Command;
  }
  return QStringLiteral("cd %1 && %2").arg(Quote(FDirectory), Command);
}

void RemoteSession::Command(const QString & Cmd, const QString & Status,
  std::function<void(int, const QStringList &)> Done)
{
  if (FTerminal == nullptr)
  {
    return;
  }
  auto Output = std::make_shared<QStringList>();
  SetBusy(true, Status);
  FTerminal->AnyCommand(InDirectory(Cmd), [Output](const QString & Line, bool)
  {
    Output->append(Line);
  },
  [this, Output, Done](const OperationResult & Result, int ExitCode, const QString &)
  {
    SetBusy(false);
    if (Failed(Result))
    {
      Done(-1, *Output);
      return;
    }
    Done(ExitCode, *Output);
  });
}

//---------------------------------------------------------------------------
RemoteSession::Edit * RemoteSession::FindEdit(const QString & Local)
{
  for (Edit & E : FEdits)
  {
    if (E.Local == Local)
    {
      return &E;
    }
  }
  return nullptr;
}

void RemoteSession::FetchEdit(const QString & Name, bool Adopt)
{
  QString Target = MobileCache::Folder(QStringLiteral("edit"), FCacheId) + QLatin1Char('/') + QString::number(++FEditCounter);
  QString Remote = RemotePath(Name);
  QString RemoteDir = Remote.left(Remote.lastIndexOf(QLatin1Char('/')));
  if (RemoteDir.isEmpty())
  {
    RemoteDir = QStringLiteral("/");
  }
  Fetch({Name}, Target, true, QStringLiteral("Opening %1...").arg(Remote.mid(Remote.lastIndexOf(QLatin1Char('/')) + 1)),
    [this, RemoteDir, Remote, Adopt](const QStringList & Local)
  {
    QFileInfo Info(Local.first());
    Edit E;
    E.Local = Local.first();
    E.RemoteDir = RemoteDir;
    E.Modified = Info.lastModified();
    E.Size = Info.size();
    FEdits.append(E);
    if (Adopt)
    {
      emit editAdopted(Local.first(), Remote);
    }
    else
    {
      emit editReady(Local.first(), Remote);
    }
  }, true);
}

void RemoteSession::editFile(const QString & Name)
{
  FetchEdit(Name, false);
}

void RemoteSession::adoptEdit(const QString & RemotePath)
{
  FetchEdit(RemotePath, true);
}

void RemoteSession::checkEdits()
{
  // failed ones get another try now
  for (Edit & E : FEdits)
  {
    E.Failed = false;
  }
  SendEdits();
}

bool RemoteSession::WriteEdit(const Edit & E)
{
  // the file as it was until the new one is complete: full storage must not
  // end as an empty file on the server
  QDir().mkpath(QFileInfo(E.Local).absolutePath());
  QSaveFile File(E.Local);
  if (!File.open(QIODevice::WriteOnly) || (File.write(E.Sending) != E.Sending.size()) || !File.commit())
  {
    emit error(QStringLiteral("Cannot save %1.").arg(QFileInfo(E.Local).fileName()), File.errorString());
    return false;
  }
  return true;
}

void RemoteSession::SendEdits()
{
  if (!connected() || FBusy || !FUploadingEdit.isEmpty())
  {
    return;
  }
  for (Edit & E : FEdits)
  {
    if (E.Failed)
    {
      continue;
    }
    QFileInfo Info(E.Local);
    if (E.BuiltIn)
    {
      if (!E.HasUnsent)
      {
        continue;
      }
      E.Sending = E.Unsent;
      if (!WriteEdit(E))
      {
        E.Failed = true;
        emit editSaved(E.Local, false);
        return;
      }
      Info.refresh();
    }
    else if (!Info.exists() || ((Info.lastModified() == E.Modified) && (Info.size() == E.Size)))
    {
      continue;
    }
    QString Local = E.Local;
    QDateTime Modified = Info.lastModified();
    qint64 Size = Info.size();
    FUploadingEdit = Local;
    UploadTo({Local}, E.RemoteDir, [this, Local, Modified, Size](bool Ok) { EditUploaded(Local, Ok, Modified, Size); },
      true);   // an edited file replaces its original, as WinSCP's editing
    return;   // one upload at a time
  }
}

void RemoteSession::EditUploaded(const QString & Local, bool Ok, const QDateTime & Modified, qint64 Size)
{
  FUploadingEdit.clear();
  Edit * E = FindEdit(Local);
  if (E == nullptr)
  {
    return;
  }
  QString Name = QFileInfo(Local).fileName();
  QString RemoteDir = E->RemoteDir;
  bool BuiltIn = E->BuiltIn;
  if (BuiltIn)
  {
    // no copy stays; the changes wait in memory for another try
    QFile::remove(Local);
  }
  E->Failed = !Ok;
  // saved again during the upload: the page goes on waiting for what it saved last
  bool Newer = false;
  if (Ok)
  {
    E->Modified = Modified;
    E->Size = Size;
    if (BuiltIn)
    {
      E->Original = E->Sending;
      Newer = E->HasUnsent && (E->Unsent != E->Sending);
      if (!Newer)
      {
        E->Unsent.clear();
        E->HasUnsent = false;
      }
    }
    if (E->Closed && !Newer)
    {
      FEdits.removeAt(E - FEdits.data());
      E = nullptr;
    }
    if (!Newer)
    {
      emit notice(QStringLiteral("Changes of %1 uploaded.").arg(Name));
    }
    if (RemoteDir == FDirectory)
    {
      refresh();
    }
  }
  if (E != nullptr)
  {
    E->Sending.clear();
  }
  if (!Newer)
  {
    emit editSaved(Local, Ok);
  }
  // the next changed file, if any (a failed one waits: the same error again and again helps nobody)
  if (Ok)
  {
    QTimer::singleShot(0, this, &RemoteSession::SendEdits);
  }
}

QVariantMap RemoteSession::readEdit(const QString & LocalPath, const QString & Encoding)
{
  Edit * E = FindEdit(LocalPath);
  if (E == nullptr)
  {
    return {{QStringLiteral("error"), QStringLiteral("This file is no longer open.")}};
  }
  QString Name = QFileInfo(LocalPath).fileName();
  if (!E->BuiltIn)
  {
    QFile File(LocalPath);
    if (File.size() > 5 * 1024 * 1024)
    {
      return {{QStringLiteral("error"), QStringLiteral("The file is too large for the editor (over 5 MB).")}};
    }
    if (!File.open(QIODevice::ReadOnly))
    {
      return {{QStringLiteral("error"), QStringLiteral("Cannot read %1.").arg(Name)}};
    }
    E->Original = File.readAll();
    File.close();
    E->BuiltIn = true;
    // from here on in memory only
    QFile::remove(LocalPath);
  }
  if (E->Original.contains('\0'))
  {
    return {{QStringLiteral("error"), QStringLiteral("This is not a text file.")}};
  }
  QString Use = Encoding;
  bool Ask = Use.isEmpty() && !TextFile::IsUtf8(E->Original);
  if (Use.isEmpty())
  {
    Use = Ask ? TextFile::LegacyEncodings().first() : QLatin1String(TextFile::Utf8);
  }
  QString Text;
  if (!TextFile::Decode(E->Original, Use, Text))
  {
    return {{QStringLiteral("error"), QStringLiteral("%1 is not in %2.").arg(Name, Use)}};
  }
  QVariantMap Result{{QStringLiteral("text"), Text}, {QStringLiteral("encoding"), Use},
    {QStringLiteral("lineEnding"), TextFile::LineEnding(E->Original)}, {QStringLiteral("sample"), TextFile::Sample(Text)}};
  if (Ask)
  {
    Result.insert(QStringLiteral("needsEncoding"), true);
    Result.insert(QStringLiteral("encodings"), TextFile::LegacyEncodings());
  }
  return Result;
}

void RemoteSession::saveText(const QString & LocalPath, const QString & Text, const QString & Encoding)
{
  Edit * E = FindEdit(LocalPath);
  if (E == nullptr)
  {
    emit editSaved(LocalPath, false);
    return;
  }
  QByteArray Data;
  QString Problem;
  if (!TextFile::Merge(E->Original, Text, Encoding, Data, Problem))
  {
    emit error(QStringLiteral("Cannot save %1.").arg(QFileInfo(LocalPath).fileName()), Problem);
    emit editSaved(LocalPath, false);
    return;
  }
  // what the server has already, and goes on having: with an upload on its way,
  // even the original text is a change to what that one leaves there
  if ((Data == E->Original) && (FUploadingEdit != LocalPath))
  {
    E->HasUnsent = false;
    E->Unsent.clear();
    emit editSaved(LocalPath, true);
    return;
  }
  E->Unsent = Data;
  E->HasUnsent = true;
  E->Failed = false;
  // now, or as soon as the connection is free
  SendEdits();
}

void RemoteSession::closeEdit(const QString & LocalPath)
{
  Edit * E = FindEdit(LocalPath);
  if (E == nullptr)
  {
    return;
  }
  E->Closed = true;
  // on its way to the server: the end of the upload forgets it
  if (FUploadingEdit != LocalPath)
  {
    QFile::remove(LocalPath);
    FEdits.removeAt(E - FEdits.data());
  }
}

QVariantMap RemoteSession::readText(const QString & LocalPath) const
{
  QFile File(LocalPath);
  if (File.size() > 5 * 1024 * 1024)
  {
    return {{QStringLiteral("error"), QStringLiteral("The file is too large for the editor (over 5 MB).")}};
  }
  if (!File.open(QIODevice::ReadOnly))
  {
    return {{QStringLiteral("error"), QStringLiteral("Cannot read %1.").arg(QFileInfo(LocalPath).fileName())}};
  }
  QByteArray Data = File.readAll();
  if (Data.contains('\0'))
  {
    return {{QStringLiteral("error"), QStringLiteral("This is not a text file.")}};
  }
  // only shown: a file that is not UTF-8 in the first code page offered
  QString Text;
  TextFile::Decode(Data, TextFile::IsUtf8(Data) ? QLatin1String(TextFile::Utf8) : TextFile::LegacyEncodings().first(), Text);
  return {{QStringLiteral("text"), Text}};
}

void RemoteSession::createFile(const QString & Name)
{
  if ((FTerminal == nullptr) || Name.isEmpty() || Name.contains(QLatin1Char('/')) ||
      (Name == QStringLiteral(".")) || (Name == QStringLiteral("..")))
  {
    return;
  }
  if (!FFiles.entry(Name).isEmpty())
  {
    emit error(QStringLiteral("%1 already exists.").arg(Name), QString());
    return;
  }
  QString Target = MobileCache::Folder(QStringLiteral("edit"), FCacheId) + QLatin1Char('/') + QString::number(++FEditCounter);
  QDir().mkpath(Target);
  QString Local = Target + QLatin1Char('/') + Name;
  QFile File(Local);
  if (!File.open(QIODevice::WriteOnly | QIODevice::Truncate))
  {
    emit error(QStringLiteral("Cannot create %1.").arg(Name), File.errorString());
    return;
  }
  File.close();
  QString RemoteDir = FDirectory;
  QString Remote = RemotePath(Name);
  UploadTo({Local}, RemoteDir, [this, Local, RemoteDir, Remote](bool Ok)
  {
    if (!Ok)
    {
      return;
    }
    refresh();
    QFileInfo Info(Local);
    Edit E;
    E.Local = Local;
    E.RemoteDir = RemoteDir;
    E.Modified = Info.lastModified();
    E.Size = Info.size();
    FEdits.append(E);
    emit editReady(Local, Remote);
  }, true);
}

//---------------------------------------------------------------------------
bool RemoteSession::isArchive(const QString & Name) const
{
  static const QStringList Suffixes = {QStringLiteral(".zip"), QStringLiteral(".tar"), QStringLiteral(".tar.gz"),
    QStringLiteral(".tgz"), QStringLiteral(".tar.bz2"), QStringLiteral(".tbz2"), QStringLiteral(".tar.xz"),
    QStringLiteral(".txz"), QStringLiteral(".tar.zst")};
  for (const QString & Suffix : Suffixes)
  {
    if (Name.endsWith(Suffix, Qt::CaseInsensitive))
    {
      return true;
    }
  }
  return false;
}

void RemoteSession::archive(const QStringList & Names, const QString & Format, const QString & TargetDir)
{
  if ((FTerminal == nullptr) || Names.isEmpty())
  {
    return;
  }
  // one item: its name; several: the folder's and the time
  QString Base = (Names.size() == 1) ? Names.first() : QFileInfo(FDirectory).fileName();
  if (Base.isEmpty())
  {
    Base = QStringLiteral("files");
  }
  if (Names.size() > 1)
  {
    Base += QDateTime::currentDateTime().toString(QStringLiteral("-yyyyMMdd-HHmm"));
  }
  if (!canRunCommands())
  {
    ZipLocally(Names, Base + QStringLiteral(".zip"), TargetDir);
    return;
  }
  bool Zip = (Format == QStringLiteral("zip"));
  QString ArchiveName = Base + (Zip ? QStringLiteral(".zip") : QStringLiteral(".tar.gz"));
  QStringList Quoted;
  for (const QString & Name : Names)
  {
    Quoted.append(Quote(Name));
  }
  // in a temporary folder of the server; its path is the last line
  QString Make = Zip ? QStringLiteral("zip -q -r -y \"$D\"/%1 -- %2").arg(Quote(ArchiveName), Quoted.join(QLatin1Char(' ')))
                     : QStringLiteral("tar -czf \"$D\"/%1 -- %2").arg(Quote(ArchiveName), Quoted.join(QLatin1Char(' ')));
  // a failure removes the folder and keeps the exit code (no "exit": SCP
  // runs this in the session's shell)
  QString Cmd = QStringLiteral("D=$(mktemp -d) && if %1; then echo \"$D\"; else S=$?; rm -rf \"$D\"; sh -c \"exit $S\"; fi").arg(Make);
  Command(Cmd, QStringLiteral("Packing %1...").arg(ArchiveName), [this, Names, Base, ArchiveName, TargetDir, Zip](int Exit, const QStringList & Output)
  {
    if ((Exit == 127) && Zip)
    {
      // no zip on the server: packed on the phone instead
      emit notice(QStringLiteral("zip is not installed on the server; packing on the phone."));
      ZipLocally(Names, Base + QStringLiteral(".zip"), TargetDir);
      return;
    }
    if (Exit != 0)
    {
      if (Exit > 0)
      {
        emit error(QStringLiteral("The archive could not be made on the server."), Output.join(QLatin1Char('\n')).trimmed());
      }
      return;
    }
    QString Dir = Output.isEmpty() ? QString() : Output.last().trimmed();
    if (!Dir.startsWith(QLatin1Char('/')))
    {
      emit error(QStringLiteral("The archive could not be made on the server."), Output.join(QLatin1Char('\n')));
      return;
    }
    QString Remote = Dir + QLatin1Char('/') + ArchiveName;
    QString Target = TargetDir.isEmpty() ? downloadFolder() : TargetDir;
    // the temporary folder goes away afterwards, downloaded or not
    auto Cleanup = [this, Dir]()
    {
      if (FTerminal != nullptr)
      {
        FTerminal->AnyCommand(QStringLiteral("rm -rf %1").arg(Quote(Dir)), [](const QString &, bool) {},
          [](const OperationResult &, int, const QString &) {});
      }
    };
    StartTransfer(QStringLiteral("Downloading %1...").arg(ArchiveName));
    QDir().mkpath(Target);
    FTerminal->CopyToLocal({Remote}, Target, Configuration::Instance().DefaultCopyParam(), false,
      [this, Target, ArchiveName, TargetDir, Cleanup](const OperationResult & Result)
    {
      StopTransfer();
      Cleanup();
      if (Failed(Result))
      {
        return;
      }
      QString Local = Target + QLatin1Char('/') + ArchiveName;
      if (TargetDir.isEmpty())
      {
        emit downloaded({Local});
      }
      else
      {
        emit archived(Local);
      }
    });
  });
}

// For servers without commands (FTP, WebDAV, S3): the files are downloaded
// and packed on the phone
void RemoteSession::ZipLocally(const QStringList & Names, const QString & ArchiveName, const QString & TargetDir)
{
  QString Work = MobileCache::Folder(QStringLiteral("archive"), FCacheId);
  Fetch(Names, Work, true, QStringLiteral("Downloading %1 items...").arg(Names.size()),
    [this, Work, ArchiveName, TargetDir](const QStringList &)
  {
    QString Target = TargetDir.isEmpty() ? downloadFolder() : TargetDir;
    QDir().mkpath(Target);
    QString Local = Target + QLatin1Char('/') + ArchiveName;
    SetBusy(true, QStringLiteral("Packing %1...").arg(ArchiveName));
    QPointer<RemoteSession> Self(this);
    // not on the GUI thread: compressing takes its time
    QThread * Worker = QThread::create([Self, Work, Local, TargetDir]()
    {
      QString Problem;
      {
        QZipWriter Zip(Local);
        Zip.setCompressionPolicy(QZipWriter::AutoCompress);
        QDir Root(Work);
        QDirIterator It(Work, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
          QDirIterator::Subdirectories);
        while (It.hasNext() && Problem.isEmpty())
        {
          QString Path = It.next();
          QString Relative = Root.relativeFilePath(Path);
          if (It.fileInfo().isDir())
          {
            Zip.addDirectory(Relative);
          }
          else if (It.fileInfo().size() > MaxZippedFile)
          {
            Problem = QStringLiteral("%1 is too large to be packed on the phone. Download it without an archive.").arg(Relative);
          }
          else
          {
            QFile File(Path);
            if (File.open(QIODevice::ReadOnly))
            {
              Zip.addFile(Relative, &File);
            }
          }
        }
        Zip.close();
        if (Problem.isEmpty() && (Zip.status() != QZipWriter::NoError))
        {
          Problem = QStringLiteral("Cannot write %1.").arg(Local);
        }
      }
      QDir(Work).removeRecursively();
      if (!Problem.isEmpty())
      {
        QFile::remove(Local);
      }
      QMetaObject::invokeMethod(QCoreApplication::instance(), [Self, Local, TargetDir, Problem]()
      {
        if ((Self == nullptr) || (Self->FTerminal == nullptr))
        {
          return;
        }
        Self->SetBusy(false);
        if (!Problem.isEmpty())
        {
          emit Self->error(Problem, QString());
        }
        else if (TargetDir.isEmpty())
        {
          emit Self->downloaded({Local});
        }
        else
        {
          emit Self->archived(Local);
        }
      }, Qt::QueuedConnection);
    });
    connect(Worker, &QThread::finished, Worker, &QObject::deleteLater);
    Worker->start();
  });
}

void RemoteSession::extract(const QString & Name)
{
  if (!canRunCommands())
  {
    return;
  }
  QString Cmd = Name.endsWith(QStringLiteral(".zip"), Qt::CaseInsensitive) ? QStringLiteral("unzip -o -q -- %1").arg(Quote(Name))
                                                                           : QStringLiteral("tar -xf %1").arg(Quote(Name));
  Command(Cmd, QStringLiteral("Extracting %1...").arg(Name), [this, Name](int Exit, const QStringList & Output)
  {
    if (Exit == 0)
    {
      emit notice(QStringLiteral("%1 extracted.").arg(Name));
    }
    else if (Exit > 0)
    {
      emit error((Exit == 127) ? QStringLiteral("The tool to extract %1 is not installed on the server.").arg(Name)
                               : QStringLiteral("%1 could not be extracted.").arg(Name), Output.join(QLatin1Char('\n')).trimmed());
    }
    refresh();
  });
}

//---------------------------------------------------------------------------
TerminalSession * RemoteSession::openTerminal(int Cols, int Rows)
{
  if (!connected())
  {
    return FShell;
  }
  if (FShell == nullptr)
  {
    SessionData Data = FOpenData;
    // a password typed at the prompt is not asked again
    if (Data.Password.isEmpty())
    {
      Data.Password = FTerminal->RememberedPassword();
    }
    FShell = new TerminalSession(Data, FAsker.get(), this);
    KeepAsker(FShell);
    emit terminalChanged();
  }
  if (!FShell->running() && !FShell->connecting())
  {
    FShell->Start(Cols, Rows);
    if (!FDirectory.isEmpty())
    {
      // into the browsed folder (the shell starts in the home folder)
      FShell->sendText(QStringLiteral(" cd %1 && clear\n").arg(Quote(FDirectory)));
    }
  }
  return FShell;
}

//---------------------------------------------------------------------------
void RemoteSession::setProperties(const QStringList & Names, int Set, int Unset, const QString & Owner, const QString & Group,
  bool Recursive)
{
  if ((FTerminal == nullptr) || Names.isEmpty())
  {
    return;
  }
  RemoteProperties Properties;
  if ((Set | Unset) != 0)
  {
    Properties.ValidRights = true;
    Properties.RightsSet = static_cast<unsigned int>(Set) & 07777;
    Properties.RightsUnset = static_cast<unsigned int>(Unset) & 07777;
  }
  Properties.Owner = Owner.trimmed();
  Properties.Group = Group.trimmed();
  Properties.Recursive = Recursive;
  if (Properties.Empty())
  {
    return;
  }
  QStringList Paths;
  for (const QString & Name : Names)
  {
    Paths.append(RemotePath(Name));
  }
  SetBusy(true, QStringLiteral("Changing properties..."));
  FTerminal->ChangeFilesProperties(Paths, Properties, [this](const OperationResult & Result)
  {
    SetBusy(false);
    Failed(Result);
    refresh();
  });
}

//---------------------------------------------------------------------------
QVariantList RemoteSession::bookmarks() const
{
  QVariantList Result;
  if (FSite.isEmpty())
  {
    return Result;
  }
  BookmarkList List = Bookmarks::ConfiguredList(Bookmarks::SessionKey(FData));
  for (const Bookmark & B : List.Items())
  {
    if (!B.Remote.isEmpty())
    {
      Result.append(QVariantMap{{QStringLiteral("name"), B.Name}, {QStringLiteral("path"), B.Remote}});
    }
  }
  return Result;
}

bool RemoteSession::addBookmark(const QString & Name, const QString & Path)
{
  if (FSite.isEmpty() || Name.trimmed().isEmpty())
  {
    return false;
  }
  Configuration & Config = Configuration::Instance();
  Bookmarks All;
  All.Load(Config.Ini());
  QString Key = Bookmarks::SessionKey(FData);
  BookmarkList List = All.List(Key);
  Bookmark B;
  B.Name = Name.trimmed();
  B.Remote = Path;
  try
  {
    List.Add(B);
  }
  catch (const ExtException & E)
  {
    emit error(QString::fromUtf8(E.what()), QString());
    return false;
  }
  All.SetList(Key, List);
  All.Save(Config.Ini());
  if (!Config.Save())
  {
    emit error(QStringLiteral("The bookmark could not be saved."), QString());
    return false;
  }
  return true;
}

void RemoteSession::removeBookmark(const QString & Name)
{
  Configuration & Config = Configuration::Instance();
  Bookmarks All;
  All.Load(Config.Ini());
  QString Key = Bookmarks::SessionKey(FData);
  BookmarkList List = All.List(Key);
  int Index = List.IndexOf(QString(), Name);
  if (Index < 0)
  {
    for (int I = 0; I < List.Count(); I++)
    {
      if (List.At(I).Name == Name)
      {
        Index = I;
        break;
      }
    }
  }
  if (Index >= 0)
  {
    List.Delete(Index);
    All.SetList(Key, List);
    All.Save(Config.Ini());
    if (!Config.Save())
    {
      emit error(QStringLiteral("The bookmark could not be removed."), QString());
    }
  }
}

//---------------------------------------------------------------------------
void RemoteSession::installPublicKey(const QString & Key)
{
  QString Line = Key.trimmed();
  if (!canRunCommands() || Line.isEmpty() || Line.contains(QLatin1Char('\n')))
  {
    return;
  }
  Command(InstallKeyCommand(Line), QStringLiteral("Installing the key..."), [this](int Exit, const QStringList & Output)
  {
    if (Exit == 0)
    {
      emit notice(QStringLiteral("The key was added to ~/.ssh/authorized_keys on %1.").arg(FData.HostNameExpanded()));
    }
    else if (Exit > 0)
    {
      emit error(QStringLiteral("The key could not be installed."), Output.join(QLatin1Char('\n')).trimmed());
    }
  });
}

QString RemoteSession::InstallKeyCommand(const QString & Line)
{
  // a file without a final newline: the key must start a line of its own;
  // the last grep tells whether it is there, whatever came before
  return QStringLiteral(
    "umask 077 && mkdir -p ~/.ssh && touch ~/.ssh/authorized_keys && "
    "(grep -qxF %1 ~/.ssh/authorized_keys || { "
    "if [ -s ~/.ssh/authorized_keys ] && [ -n \"$(tail -c 1 ~/.ssh/authorized_keys)\" ]; then echo >> ~/.ssh/authorized_keys; fi; "
    "echo %1 >> ~/.ssh/authorized_keys; }) && grep -qxF %1 ~/.ssh/authorized_keys").arg(Quote(Line));
}

bool RemoteSession::hasTransfers() const
{
  return FTransferring || ((FQueue != nullptr) && FQueue->HasActive(this));
}

//---------------------------------------------------------------------------
// the background connection: queued transfers first, then thumbnails
void RemoteSession::DropBack()
{
  FBackTimer.stop();
  FBackItem = 0;
  FBackThumb = false;
  FBackOpening = false;
  if (FBack != nullptr)
  {
    Terminal * Back = FBack;
    FBack = nullptr;
    Dispose(Back);
  }
}

void RemoteSession::WithBack(std::function<void(bool)> Ready)
{
  if ((FBack != nullptr) && FBack->Active())
  {
    Ready(true);
    return;
  }
  if (FBackOpening || !connected())
  {
    return;
  }
  DropBack();
  SessionData Data = FOpenData;
  if (Data.Password.isEmpty())
  {
    Data.Password = FTerminal->RememberedPassword();
  }
  Terminal * Back = new Terminal(Data, FAsker.get(), this);
  KeepAsker(Back);
  // errors of single files end the operation with all of them (no
  // Retry/Skip question for every thumbnail)
  Terminal::Settings Settings = Terminal::ConfigurationSettings();
  Settings.ContinueOnError = true;
  Back->SetSettings(Settings);
  FBack = Back;
  FBackOpening = true;
  connect(Back, &Terminal::Disconnected, this, [this, Back](const QString &, const QString &)
  {
    // reopened for the next item
    if ((FBack == Back) && (FBackItem == 0) && !FBackThumb)
    {
      // this one only: the next item may have opened a new one meanwhile
      QTimer::singleShot(0, this, [this, Back]()
      {
        if (FBack == Back)
        {
          DropBack();
        }
      });
    }
  });
  QPointer<Terminal> Guard(Back);
  Back->Open([this, Guard, Ready](const OperationResult & Result)
  {
    if ((Guard == nullptr) || (FBack != Guard))
    {
      return;
    }
    FBackOpening = false;
    if (!Result.Ok)
    {
      QString Message = Result.Message;
      Terminal * Failed = Guard;
      QTimer::singleShot(0, this, [this, Failed]()
      {
        if (FBack == Failed)
        {
          DropBack();
        }
      });
      // the waiting transfers cannot run
      while (const MobileQueue::Item * I = FQueue->Next(this))
      {
        FQueue->SetState(I->Id, MobileQueue::Failed, Message);
      }
      FThumbs.clear();
      return;
    }
    Ready(true);
  });
}

void RemoteSession::RunNext()
{
  if ((FQueue == nullptr) || (FBackItem != 0) || FBackThumb || FBackOpening || !connected())
  {
    return;
  }
  const MobileQueue::Item * I = FQueue->Next(this);
  if (I == nullptr)
  {
    NextThumbnail();
    return;
  }
  int Id = I->Id;
  WithBack([this, Id](bool) { RunItem(Id); });
}

void RemoteSession::RunItem(int Id)
{
  const MobileQueue::Item * I = FQueue->Find(Id);
  if ((I == nullptr) || (I->St != MobileQueue::Waiting) || (FBack == nullptr))
  {
    RunNext();
    return;
  }
  FBackItem = Id;
  FQueue->SetState(Id, MobileQueue::Running);
  FBackTimer.start();
  MobileQueue::Direction Dir = I->Dir;
  QStringList Sources = I->Sources;
  QString Target = I->Target;
  auto Done = [this, Id, Dir, Target](const OperationResult & Result)
  {
    FBackTimer.stop();
    FBackItem = 0;
    if (Result.Ok)
    {
      FQueue->SetState(Id, MobileQueue::Done);
    }
    else
    {
      // an upload keeps its cache copies for Retry
      FQueue->SetState(Id, Result.Aborted ? MobileQueue::Cancelled : MobileQueue::Failed,
        Result.MoreMessages.isEmpty() ? Result.Message : (Result.Message + QLatin1Char('\n') + Result.MoreMessages));
    }
    // partly done is new files too
    if (Dir == MobileQueue::Upload)
    {
      emit FQueue->uploadEnded(Target);
    }
    QTimer::singleShot(0, this, &RemoteSession::RunNext);
  };
  CopyParam Param = Configuration::Instance().DefaultCopyParam();
  // the queue resumes without asking: with ContinueOnError the question
  // would count as an error though the file then arrives (a Retry after a
  // partial transfer would end as Failed)
  Param.ConfirmResume = false;
  if (Dir == MobileQueue::Download)
  {
    FBack->CopyToLocal(Sources, Target, Param, false, Done);
  }
  else
  {
    FBack->CopyToRemote(Sources, Target, Param, false, Done);
  }
}

void RemoteSession::QueueThumbnails(const RemoteFileList & Files)
{
  FThumbs.clear();
  FThumbDirectory = FDirectory;
  if (!FSettings->thumbnails())
  {
    return;
  }
  QString Folder = MobileCache::Thumbnails(FCacheId);
  QDir().mkpath(Folder);
  for (const RemoteFile & F : Files)
  {
    if (F.IsDirectory || (F.Size <= 0) || (F.Size > 3 * 1024 * 1024) ||
        (FileListModel::previewKind(F.FileName) != QStringLiteral("image")))
    {
      continue;
    }
    // the same file (path, size, time) is fetched once
    QString Remote = RemotePath(F.FileName);
    QByteArray Key = (FData.HostNameExpanded() + QLatin1Char('|') + Remote + QLatin1Char('|') + QString::number(F.Size) +
      QLatin1Char('|') + F.Modification.toString(Qt::ISODate)).toUtf8();
    QString Cache = Folder + QLatin1Char('/') +
      QString::fromLatin1(QCryptographicHash::hash(Key, QCryptographicHash::Sha1).toHex().left(24)) +
      QLatin1Char('.') + QFileInfo(F.FileName).suffix().toLower();
    if (QFileInfo::exists(Cache))
    {
      FFiles.SetThumbnail(F.FileName, QUrl::fromLocalFile(Cache).toString());
    }
    else if (FThumbs.size() < 100)
    {
      FThumbs.append(Thumb{F.FileName, Remote, Cache});
    }
  }
  RunNext();
}

void RemoteSession::NextThumbnail()
{
  if (FThumbs.isEmpty() || FBackThumb || (FBackItem != 0))
  {
    return;
  }
  WithBack([this](bool)
  {
    if (FThumbs.isEmpty() || FBackThumb || (FBackItem != 0) || (FBack == nullptr))
    {
      return;
    }
    // a waiting transfer goes first
    if (FQueue->Next(this) != nullptr)
    {
      RunNext();
      return;
    }
    Thumb T = FThumbs.takeFirst();
    QString Temp = MobileCache::Folder(QStringLiteral("thumbfetch"), FCacheId);
    QDir(Temp).removeRecursively();
    QDir().mkpath(Temp);
    FBackThumb = true;
    CopyParam Param = Configuration::Instance().DefaultCopyParam();
    Param.ConfirmOverwriting = false;
    QString Directory = FThumbDirectory;
    FBack->CopyToLocal({T.Remote}, Temp, Param, false, [this, T, Temp, Directory](const OperationResult & Result)
    {
      FBackThumb = false;
      QString Fetched = Temp + QLatin1Char('/') + T.Name;
      if (Result.Ok && QFile::rename(Fetched, T.Cache) && (Directory == FDirectory))
      {
        FFiles.SetThumbnail(T.Name, QUrl::fromLocalFile(T.Cache).toString());
      }
      QTimer::singleShot(0, this, &RemoteSession::RunNext);
    });
  });
}

//---------------------------------------------------------------------------
void RemoteSession::previewFile(const QString & Name)
{
  QString Target = MobileCache::Folder(QStringLiteral("open"), FCacheId);
  Fetch({Name}, Target, true, QStringLiteral("Opening %1...").arg(Name), [this, Name](const QStringList & Local)
  {
    emit previewReady(Local.first(), Name);
  });
}

void RemoteSession::closePreview(const QString & LocalPath)
{
  // the phone's own files are previewed in place
  if (LocalPath.startsWith(MobileCache::Folder(QStringLiteral("open"), FCacheId) + QLatin1Char('/')))
  {
    QFile::remove(LocalPath);
  }
}

bool RemoteSession::findFiles(const QString & Text)
{
  if (!connected() || (FFind != nullptr) || Text.trimmed().isEmpty())
  {
    return false;
  }
  // a part of the name, or a mask of its own
  QString Mask = Text.trimmed();
  if (!Mask.contains(QLatin1Char('*')) && !Mask.contains(QLatin1Char('?')))
  {
    Mask = QLatin1Char('*') + Mask + QLatin1Char('*');
  }
  FileFind * Find = new FileFind(this);
  FFind = Find;
  connect(Find, &FileFind::FilesFound, this, [this](const QList<FoundFile> & Files)
  {
    QVariantList Result;
    for (const FoundFile & F : Files)
    {
      Result.append(QVariantMap{{QStringLiteral("name"), F.File.FileName}, {QStringLiteral("directory"), F.Directory},
        {QStringLiteral("path"), F.FullPath}, {QStringLiteral("isDir"), F.File.IsDirectory},
        {QStringLiteral("size"), F.File.IsDirectory ? QString() : DataSize(F.File.Size)}});
    }
    emit found(Result);
  });
  connect(Find, &FileFind::SearchingDirectory, this, [this](const QString & Directory)
  {
    FStatus = QStringLiteral("Searching %1").arg(Directory);
    emit statusChanged();
  });
  connect(Find, &FileFind::Finished, this, [this, Find](bool Aborted, const QString & Error)
  {
    SetBusy(false);
    emit searchFinished(Aborted, Error);
    Find->deleteLater();
  });
  QString Error;
  if (!Find->Start(FTerminal, FDirectory, Mask, &Error))
  {
    Find->deleteLater();
    emit searchFinished(false, Error);
    return false;
  }
  SetBusy(true, QStringLiteral("Searching..."));
  return true;
}

void RemoteSession::stopSearch()
{
  if (FFind != nullptr)
  {
    FFind->Stop();
  }
}

QStringList RemoteSession::remotePaths(const QStringList & Names) const
{
  QStringList Result;
  for (const QString & Name : Names)
  {
    Result.append(RemotePath(Name));
  }
  return Result;
}

void RemoteSession::paste(const QStringList & Paths, bool Move)
{
  if (!connected() || Paths.isEmpty())
  {
    return;
  }
  SetBusy(true, Move ? QStringLiteral("Moving...") : QStringLiteral("Copying..."));
  auto Done = [this](const OperationResult & Result)
  {
    SetBusy(false);
    Failed(Result);
    refresh();
  };
  // "*.*": the names stay
  if (Move)
  {
    FTerminal->MoveFiles(Paths, FDirectory, QStringLiteral("*.*"), Done);
  }
  else
  {
    FTerminal->CopyFiles(Paths, FDirectory, QStringLiteral("*.*"), Done);
  }
}

QStringList RemoteSession::recentFolders() const
{
  return FSettings->recentFolders(FSite);
}
