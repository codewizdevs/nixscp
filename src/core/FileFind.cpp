// Port of WinSCP source/core/Terminal.cpp (TTerminal::FilesFind, DoFilesFind,
// FileFind, TLoopDetector) plus a local equivalent
#include "core/FileFind.h"
#include "core/FileMasks.h"
#include "core/Terminal.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QMutex>
#include <QSet>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <functional>
#include <sys/stat.h>

struct FileFind::State
{
  QMutex Mutex;
  QList<FoundFile> Pending;
  QString CurrentDirectory;
  QStringList Errors;
  bool Aborted = false;        // local thread result
  QString FatalError;          // local thread result
  std::atomic<bool> Cancel{false};
  std::atomic<bool> Started{false};
};

namespace
{

using Lister = std::function<RemoteFileList(const QString & Directory)>;

// ::AbsolutePath (Common.cpp) for unix paths
QString AbsoluteUnixPath(const QString & Base, const QString & Path)
{
  QString Full = Path.startsWith(QLatin1Char('/')) ? Path : UnixPath::Combine(Base.isEmpty() ? QStringLiteral("/") : Base, Path);
  QStringList Result;
  for (const QString & Part : Full.split(QLatin1Char('/'), Qt::SkipEmptyParts))
  {
    if (Part == QStringLiteral("."))
    {
      continue;
    }
    if (Part == QStringLiteral(".."))
    {
      if (!Result.isEmpty())
      {
        Result.removeLast();
      }
      continue;
    }
    Result.append(Part);
  }
  return QLatin1Char('/') + Result.join(QLatin1Char('/'));
}

struct SearchContext
{
  FileFind::State * St;
  Lister List;
  FileMasks Mask;
  bool Local = false;
  QSet<QString> Visited;   // TLoopDetector
  std::function<void()> CheckCancel;
};

void CheckCancelled(SearchContext & Context)
{
  if (Context.St->Cancel)
  {
    throw AbortException();
  }
  if (Context.CheckCancel)
  {
    Context.CheckCancel();
  }
}

QString RealPathOf(SearchContext & Context, const QString & RealDirectory, const QString & FullFileName, const RemoteFile & File)
{
  if (Context.Local)
  {
    QString Canonical = QFileInfo(FullFileName).canonicalFilePath();
    return Canonical.isEmpty() ? FullFileName : Canonical;
  }
  if (!File.IsSymLink || File.LinkTo.isEmpty())
  {
    return UnixPath::Combine(RealDirectory, File.FileName);
  }
  return AbsoluteUnixPath(RealDirectory, File.LinkTo);
}

// TTerminal::DoFilesFind + TTerminal::FileFind
void DoFilesFind(SearchContext & Context, const QString & Directory, const QString & RealDirectory, bool Root)
{
  {
    QMutexLocker Lock(&Context.St->Mutex);
    Context.St->CurrentDirectory = Directory;
  }
  CheckCancelled(Context);

  RemoteFileList Files;
  try
  {
    Files = Context.List(Directory);
  }
  catch (const AbortException &)
  {
    throw;
  }
  catch (const FatalException &)
  {
    throw;
  }
  catch (const ExtException & E)
  {
    // FIND_FILE_ERROR
    QString Message = QStringLiteral("Error retrieving file list for \"%1\".").arg(Directory);
    if (Root)
    {
      throw ExtException(Message, E.Message());
    }
    QMutexLocker Lock(&Context.St->Mutex);
    Context.St->Errors.append(QStringLiteral("%1 %2").arg(Message, E.Message()));
    return;
  }

  for (const RemoteFile & File : Files)
  {
    if ((File.FileName == QStringLiteral(".")) || (File.FileName == QStringLiteral("..")) || File.IsParentDirectory)
    {
      continue;
    }
    CheckCancelled(Context);

    QString FullFileName = UnixPath::Combine(Directory, File.FileName);
    FileMasks::Params MaskParams;
    MaskParams.Size = File.Size;
    MaskParams.Modification = File.Modification;
    bool ImplicitMatch = false;
    // Do not use recursive include match
    if (Context.Mask.Matches(FullFileName, Context.Local, File.IsDirectory, &MaskParams, false, ImplicitMatch))
    {
      if (!ImplicitMatch)
      {
        FoundFile Found;
        Found.File = File;
        Found.Directory = Directory;
        Found.FullPath = FullFileName;
        QMutexLocker Lock(&Context.St->Mutex);
        Context.St->Pending.append(Found);
      }

      if (File.IsDirectory)
      {
        QString RealPath = RealPathOf(Context, RealDirectory, FullFileName, File);
        if (!Context.Visited.contains(RealPath))
        {
          Context.Visited.insert(RealPath);
          DoFilesFind(Context, FullFileName, RealPath, false);
        }
        // else: already searched, link loop detected
      }
    }
  }
}

void RunSearch(SearchContext & Context, const QString & Directory)
{
  // TTerminal::FilesFind
  Context.Mask.NoImplicitMatchWithDirExcludeMask = true;
  Context.Mask.AllDirsAreImplicitlyIncluded = true;
  QString Real = Directory;
  if (Context.Local)
  {
    QString Canonical = QFileInfo(Directory).canonicalFilePath();
    if (!Canonical.isEmpty())
    {
      Real = Canonical;
    }
  }
  Context.Visited.insert(Real);
  DoFilesFind(Context, Directory, Real, true);
}

RemoteFileList ListLocalDirectory(const QString & Path)
{
  QFileInfo DirInfo(Path);
  if (!DirInfo.exists())
  {
    throw ExtException(QStringLiteral("No such file or directory."));
  }
  if (!DirInfo.isDir())
  {
    throw ExtException(QStringLiteral("Not a directory."));
  }
  if (!DirInfo.isReadable() || !DirInfo.isExecutable())
  {
    throw ExtException(QStringLiteral("Permission denied."));
  }
  QDir Dir(Path);
  const QFileInfoList Entries = Dir.entryInfoList(
    QDir::AllEntries | QDir::System | QDir::Hidden | QDir::NoDotAndDotDot, QDir::NoSort);
  RemoteFileList Result;
  Result.reserve(Entries.size());
  for (const QFileInfo & Info : Entries)
  {
    RemoteFile File;
    File.FileName = Info.fileName();
    File.IsSymLink = Info.isSymLink();
    File.IsDirectory = Info.isDir();
    File.FileType = File.IsSymLink ? RemoteFile::Symlink :
      (File.IsDirectory ? RemoteFile::Directory : (Info.isFile() ? RemoteFile::File : RemoteFile::Other));
    File.Size = File.IsDirectory ? 0 : Info.size();
    File.Modification = Info.lastModified();
    File.IsHidden = File.FileName.startsWith(QLatin1Char('.'));
    if (File.IsSymLink)
    {
      File.LinkTo = Info.symLinkTarget();
    }
    QByteArray EntryPath = QFile::encodeName(Info.absoluteFilePath());
    struct stat St;
    if ((::stat(EntryPath.constData(), &St) == 0) || (::lstat(EntryPath.constData(), &St) == 0))
    {
      File.Permissions = St.st_mode & 07777;
    }
    File.Owner = Info.owner();
    File.Group = Info.group();
    Result.append(File);
  }
  return Result;
}

QString NormalizeDirectory(QString Directory, bool Local)
{
  Directory = Directory.trimmed();
  if (Local)
  {
    if ((Directory == QStringLiteral("~")) || Directory.startsWith(QStringLiteral("~/")))
    {
      Directory = QDir::homePath() + Directory.mid(1);
    }
    Directory = QDir::cleanPath(QDir(Directory).absolutePath());
  }
  // UnixExcludeTrailingBackslash
  while ((Directory.size() > 1) && Directory.endsWith(QLatin1Char('/')))
  {
    Directory.chop(1);
  }
  return Directory;
}

} // namespace

FileFind::FileFind(QObject * Parent) :
  QObject(Parent),
  FTimer(new QTimer(this))
{
  FTimer->setInterval(100);
  connect(FTimer, &QTimer::timeout, this, &FileFind::Poll);
}

FileFind::~FileFind()
{
  if (FState)
  {
    FState->Cancel = true;
    if (FRemote && FState->Started && !FTerminal.isNull())
    {
      FTerminal->Progress().Cancel();
    }
  }
}

bool FileFind::Start(Terminal * Term, const QString & ADirectory, const QString & Mask, QString * ErrorMessage)
{
  if (FFinding)
  {
    if (ErrorMessage != nullptr)
    {
      *ErrorMessage = QStringLiteral("Search is already running.");
    }
    return false;
  }
  FileMasks Masks;
  try
  {
    Masks = FileMasks(Mask);
  }
  catch (const FileMasksException & E)
  {
    if (ErrorMessage != nullptr)
    {
      *ErrorMessage = E.Message();
    }
    return false;
  }

  bool Local = (Term == nullptr);
  QString Directory = NormalizeDirectory(ADirectory, Local);
  auto St = std::make_shared<State>();
  FState = St;
  FTerminal = Term;
  FRemote = !Local;
  FFinding = true;
  FLastDirectory.clear();
  FErrors.clear();
  FTimer->start();

  QPointer<FileFind> Guard(this);
  if (Local)
  {
    QThread * Thread = QThread::create([St, Directory, Masks]()
    {
      SearchContext Context;
      Context.St = St.get();
      Context.List = ListLocalDirectory;
      Context.Mask = Masks;
      Context.Local = true;
      St->Started = true;
      QString Error;
      bool Aborted = false;
      try
      {
        RunSearch(Context, Directory);
      }
      catch (const AbortException &)
      {
        Aborted = true;
      }
      catch (const ExtException & E)
      {
        Error = E.MoreMessages().isEmpty() ? E.Message() : (E.Message() + QLatin1Char('\n') + E.MoreMessages());
      }
      catch (const std::exception & E)
      {
        Error = QString::fromUtf8(E.what());
      }
      QMutexLocker Lock(&St->Mutex);
      St->Aborted = Aborted;
      St->FatalError = Error;
    });
    connect(Thread, &QThread::finished, this, [this, Guard, St]()
    {
      if (Guard.isNull() || (FState != St))
      {
        return;
      }
      bool Aborted;
      QString Error;
      {
        QMutexLocker Lock(&St->Mutex);
        Aborted = St->Aborted || St->Cancel;
        Error = St->FatalError;
      }
      Finish(Aborted, Error);
    }, Qt::QueuedConnection);
    connect(Thread, &QThread::finished, Thread, &QObject::deleteLater);
    Thread->start();
  }
  else
  {
    Term->RunOnWorker(
      [St, Directory, Masks](FileSystem & Fs, FileOperationProgress & Progress, UserInterface &)
      {
        Progress.Start(FileOperationProgress::Calculate, FileOperationProgress::ToLocal, 0);
        St->Started = true;
        if (St->Cancel)
        {
          throw AbortException();
        }
        SearchContext Context;
        Context.St = St.get();
        Context.List = [&Fs](const QString & Path) { return Fs.ReadDirectory(Path); };
        Context.Mask = Masks;
        Context.Local = false;
        Context.CheckCancel = [&Progress]() { Progress.CheckCancel(); };
        RunSearch(Context, Directory);
      },
      [this, Guard, St](const OperationResult & Result)
      {
        if (Guard.isNull() || (FState != St))
        {
          return;
        }
        QString Error;
        if (!Result.Ok && !Result.Aborted)
        {
          Error = Result.MoreMessages.isEmpty() ? Result.Message : (Result.Message + QLatin1Char('\n') + Result.MoreMessages);
        }
        Finish(Result.Aborted || St->Cancel, Error);
      });
  }
  return true;
}

void FileFind::Stop()
{
  if (!FFinding || !FState)
  {
    return;
  }
  FState->Cancel = true;
  if (FRemote && FState->Started && !FTerminal.isNull())
  {
    FTerminal->Progress().Cancel();
  }
}

void FileFind::Poll()
{
  if (!FState)
  {
    return;
  }
  QList<FoundFile> Files;
  QString Directory;
  {
    QMutexLocker Lock(&FState->Mutex);
    Files.swap(FState->Pending);
    Directory = FState->CurrentDirectory;
  }
  if (!Directory.isEmpty() && (Directory != FLastDirectory) && FFinding)
  {
    FLastDirectory = Directory;
    emit SearchingDirectory(Directory);
  }
  if (!Files.isEmpty())
  {
    emit FilesFound(Files);
  }
  // The terminal was destroyed while searching: its callback will never come
  if (FFinding && FRemote && FTerminal.isNull())
  {
    Finish(true, QString());
  }
}

void FileFind::Finish(bool Aborted, const QString & Error)
{
  if (!FFinding)
  {
    return;
  }
  Poll();
  FTimer->stop();
  FFinding = false;
  {
    QMutexLocker Lock(&FState->Mutex);
    FErrors = FState->Errors;
  }
  emit Finished(Aborted, Error);
}

QList<FoundFile> FileFind::FindLocal(const QString & ADirectory, const QString & Mask, QStringList * Errors)
{
  State St;
  SearchContext Context;
  Context.St = &St;
  Context.List = ListLocalDirectory;
  Context.Mask = FileMasks(Mask);
  Context.Local = true;
  RunSearch(Context, NormalizeDirectory(ADirectory, true));
  if (Errors != nullptr)
  {
    *Errors = St.Errors;
  }
  return St.Pending;
}
