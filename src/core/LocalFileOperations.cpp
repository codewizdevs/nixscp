// Local file operations (see LocalFileOperations.h): replaces the Windows
// shell copy/move (SHFileOperation) used by WinSCP's packages/filemng/DirView.pas
// and forms/CustomScpExplorer.cpp for local-to-local operations, and the
// shell Properties dialog (chmod/chown here).
#include "core/LocalFileOperations.h"
#include "core/FileSystem.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QProcess>
#include <QRandomGenerator>
#include <QStandardPaths>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <linux/fs.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace
{

QString ErrnoText(int Error)
{
  return QString::fromLocal8Bit(std::strerror(Error));
}

QByteArray Native(const QString & Path)
{
  return QFile::encodeName(Path);
}

QString FromNative(const char * Path)
{
  return QFile::decodeName(QByteArray(Path));
}

QString CleanPath(const QString & Path)
{
  return QDir::cleanPath(QDir(Path).absolutePath());
}

QString Combine(const QString & Directory, const QString & Name)
{
  return (Directory.endsWith(QLatin1Char('/')) ? Directory : (Directory + QLatin1Char('/'))) + Name;
}

// "**main instruction**" markup of WinSCP messages (MainInstructions)
QString MainInstructions(const QString & S)
{
  return QStringLiteral("**%1**").arg(S);
}

// The process umask without changing it (umask(2) cannot be read atomically)
mode_t CurrentUmask()
{
  static mode_t Result = []()
  {
    QFile Status(QStringLiteral("/proc/self/status"));
    if (Status.open(QIODevice::ReadOnly))
    {
      for (const QByteArray & Line : Status.readAll().split('\n'))
      {
        if (Line.startsWith("Umask:"))
        {
          bool Ok = false;
          unsigned int Value = Line.mid(6).trimmed().toUInt(&Ok, 8);
          if (Ok)
          {
            return static_cast<mode_t>(Value);
          }
        }
      }
    }
    return static_cast<mode_t>(022);
  }();
  return Result;
}

QStringList ListDirectory(const QString & Path)
{
  QStringList Result;
  DIR * Dir = opendir(Native(Path).constData());
  if (Dir == nullptr)
  {
    int Error = errno;
    throw ExtException(QStringLiteral("Error listing directory '%1'.").arg(Path), ErrnoText(Error));
  }
  while (dirent * Entry = readdir(Dir))
  {
    if ((std::strcmp(Entry->d_name, ".") == 0) || (std::strcmp(Entry->d_name, "..") == 0))
    {
      continue;
    }
    Result.append(FromNative(Entry->d_name));
  }
  closedir(Dir);
  return Result;
}

bool LStat(const QString & Path, struct stat & St)
{
  return (lstat(Native(Path).constData(), &St) == 0);
}

QString FormatStat(const struct stat & St)
{
  QDateTime Modified = QDateTime::fromSecsSinceEpoch(St.st_mtim.tv_sec);
  return QStringLiteral("%1 bytes, %2").arg(QLocale().toString(static_cast<qint64>(St.st_size)),
    QLocale().toString(Modified, QLocale::ShortFormat));
}

// Removes a temporary file unless released (failed or cancelled copy)
class TemporaryGuard
{
public:
  explicit TemporaryGuard(const QString & Path) : FPath(Path) {}
  ~TemporaryGuard()
  {
    if (!FPath.isEmpty())
    {
      unlink(Native(FPath).constData());
    }
  }
  void Release() { FPath.clear(); }
private:
  QString FPath;
};

class FdGuard
{
public:
  explicit FdGuard(int Fd = -1) : FFd(Fd) {}
  FdGuard(const FdGuard &) = delete;
  FdGuard & operator=(const FdGuard &) = delete;
  ~FdGuard() { if (FFd >= 0) ::close(FFd); }
  int Get() const { return FFd; }
  int Release() { int Fd = FFd; FFd = -1; return Fd; }
private:
  int FFd;
};

class Engine
{
public:
  Engine(const LocalFileOperations::Params & AParams, FileOperationProgress * Progress, UserInterface * UI) :
    FParams(AParams), FProgress(Progress), FUI(UI)
  {
  }

  LocalOperationResult Run();

private:
  enum OverwriteBatch { obNone, obYesToAll, obNoToAll };

  LocalFileOperations::Params FParams;
  FileOperationProgress * FProgress;
  UserInterface * FUI;
  LocalOperationResult FResult;
  OverwriteBatch FBatch = obNone;
  bool FSkipAll = false;
  dev_t FTargetDevice = 0;

  bool IsMove() const { return (FParams.Op == LocalFileOperations::opMove); }

  void CheckCancel()
  {
    if (FProgress != nullptr)
    {
      FProgress->CheckCancel();
    }
  }

  void Scan(const QString & Path, qint64 & Size, int & Count)
  {
    struct stat St;
    if (!LStat(Path, St))
    {
      return;
    }
    if (S_ISDIR(St.st_mode))
    {
      QStringList Names;
      try
      {
        Names = ListDirectory(Path);
      }
      catch (const ExtException &)
      {
        return;   // reported when the directory is processed
      }
      for (const QString & Name : Names)
      {
        CheckCancel();
        Scan(Combine(Path, Name), Size, Count);
      }
    }
    else
    {
      Count++;
      if (S_ISREG(St.st_mode))
      {
        Size += St.st_size;
      }
    }
  }

  // FILE_OPERATION_LOOP: Retry / Skip / Skip all / Abort
  template<typename TStep>
  void FileLoop(const TStep & Step)
  {
    for (;;)
    {
      try
      {
        Step();
        return;
      }
      catch (const AbortException &)
      {
        // incl. SkipFileException
        throw;
      }
      catch (const ExtException & E)
      {
        if (FUI == nullptr)
        {
          throw;
        }
        unsigned int Answer;
        if (FSkipAll || ((FProgress != nullptr) && FProgress->SkipToAll()))
        {
          Answer = qaSkip;
        }
        else
        {
          QueryUserParams QueryParams;
          QueryParams.Params = qpAllowContinueOnError;
          QueryParams.Type = qtError;
          // SKIP_ALL_BUTTON
          QueryParams.Aliases.append(QueryButtonAlias{qaAll, QStringLiteral("Ski&p all")});
          Answer = FUI->QueryUserEx(E.Message(), E.MoreMessages(), qaRetry | qaSkip | qaAll | qaAbort, QueryParams);
          if (Answer == qaAll)
          {
            FSkipAll = true;
            if (FProgress != nullptr)
            {
              FProgress->SetSkipToAll();
            }
            Answer = qaSkip;
          }
        }
        if (Answer == qaRetry)
        {
          continue;
        }
        if (Answer == qaSkip)
        {
          throw SkipFileException();
        }
        if (FProgress != nullptr)
        {
          FProgress->Cancel();
        }
        throw AbortException();
      }
    }
  }

  // Item processed in a loop; false when it (or part of it) was skipped
  template<typename TStep>
  bool ItemLoop(const TStep & Step)
  {
    try
    {
      bool Complete = true;
      FileLoop([&]() { Complete = Step(); });
      return Complete;
    }
    catch (const SkipFileException &)
    {
      FResult.Skipped++;
      return false;
    }
  }

  // true = overwrite, false = skip; Cancel throws AbortException
  bool ConfirmOverwrite(const QString & Target, const struct stat & SourceSt, const struct stat & TargetSt)
  {
    if (!FParams.ConfirmOverwrite)
    {
      return true;
    }
    if (FBatch == obYesToAll)
    {
      return true;
    }
    if (FBatch == obNoToAll)
    {
      return false;
    }
    if (FUI == nullptr)
    {
      return false;
    }
    bool Directory = S_ISDIR(TargetSt.st_mode);
    QString Name = QFileInfo(Target).fileName();
    QString Query = MainInstructions(Directory ?
      QStringLiteral("Directory '%1' already exists. Overwrite?").arg(Name) :
      QStringLiteral("File '%1' already exists. Overwrite?").arg(Name));
    QString More;
    if (!Directory)
    {
      More = QStringLiteral("New:\t%1\nExisting:\t%2").arg(FormatStat(SourceSt), FormatStat(TargetSt));
    }
    QueryUserParams QueryParams;
    QueryParams.Type = qtConfirmation;
    QueryParams.Aliases.append(QueryButtonAlias::YesToAllGroupedWithYes());
    QueryParams.Aliases.append(QueryButtonAlias::NoToAllGroupedWithNo());
    unsigned int Answer = FUI->QueryUserEx(Query, More, qaYes | qaNo | qaYesToAll | qaNoToAll | qaCancel, QueryParams);
    switch (Answer)
    {
      case qaYes:
        return true;
      case qaYesToAll:
        FBatch = obYesToAll;
        return true;
      case qaNo:
        return false;
      case qaNoToAll:
        FBatch = obNoToAll;
        return false;
      default:
        if (FProgress != nullptr)
        {
          FProgress->Cancel();
        }
        throw AbortException();
    }
  }

  // Temporary name next to Target (hidden, unique, short enough for NAME_MAX)
  static QString TemporaryName(const QString & Target)
  {
    QFileInfo Info(Target);
    QString Base = Info.fileName().left(40);
    return Combine(Info.path(), QStringLiteral(".%1.nixscp-%2").arg(Base)
      .arg(QRandomGenerator::global()->generate(), 8, 16, QLatin1Char('0')));
  }

  void SetOwner(int Fd, const QString & Path, const struct stat & St, bool Link)
  {
    if (!FParams.PreserveOwner)
    {
      return;
    }
    if ((St.st_uid == geteuid()) && (St.st_gid == getegid()))
    {
      return;
    }
    // Only root may give files away; a group change works for own groups
    int Result = (Fd >= 0) ? fchown(Fd, St.st_uid, St.st_gid) :
      fchownat(AT_FDCWD, Native(Path).constData(), St.st_uid, St.st_gid, Link ? AT_SYMLINK_NOFOLLOW : 0);
    if (Result != 0)
    {
      if (Fd >= 0)
      {
        if (fchown(Fd, static_cast<uid_t>(-1), St.st_gid) != 0) {}
      }
      else
      {
        if (fchownat(AT_FDCWD, Native(Path).constData(), static_cast<uid_t>(-1), St.st_gid, Link ? AT_SYMLINK_NOFOLLOW : 0) != 0) {}
      }
    }
  }

  mode_t TargetMode(const struct stat & St, bool Directory) const
  {
    if (FParams.PreserveRights)
    {
      return St.st_mode & 07777;
    }
    return (Directory ? 0777 : 0666) & ~CurrentUmask();
  }

  void CopyRegular(const QString & Source, const struct stat & St, const QString & Target)
  {
    FdGuard In(open(Native(Source).constData(), O_RDONLY | O_CLOEXEC | O_NOCTTY));
    if (In.Get() < 0)
    {
      int Error = errno;
      throw ExtException(QStringLiteral("Cannot open file '%1'.").arg(Source), ErrnoText(Error));
    }
    QString Temp;
    int OutFd = -1;
    for (int Attempt = 0; Attempt < 10; Attempt++)
    {
      Temp = TemporaryName(Target);
      OutFd = open(Native(Temp).constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOCTTY, 0600);
      if ((OutFd >= 0) || (errno != EEXIST))
      {
        break;
      }
    }
    FdGuard Out(OutFd);
    if (Out.Get() < 0)
    {
      int Error = errno;
      throw ExtException(QStringLiteral("Cannot create file '%1'.").arg(Target), ErrnoText(Error));
    }
    TemporaryGuard TempGuard(Temp);

    bool Done = false;
    if (FParams.UseReflink && (St.st_size > 0))
    {
      // Copy-on-write clone (btrfs, xfs, bcachefs; same file system only)
      if (ioctl(Out.Get(), FICLONE, In.Get()) == 0)
      {
        Done = true;
        if (FProgress != nullptr)
        {
          FProgress->AddTransferred(St.st_size);
        }
      }
    }
    if (!Done)
    {
      bool UseCopyFileRange = true;
      qint64 Copied = 0;
      std::vector<char> Buffer;
      for (;;)
      {
        CheckCancel();
        ssize_t Count;
        if (UseCopyFileRange)
        {
          Count = copy_file_range(In.Get(), nullptr, Out.Get(), nullptr, 8 * 1024 * 1024, 0);
          if ((Count < 0) && (Copied == 0) &&
              ((errno == EXDEV) || (errno == EINVAL) || (errno == ENOSYS) || (errno == EOPNOTSUPP) ||
               (errno == EBADF) || (errno == EPERM) || (errno == ETXTBSY)))
          {
            UseCopyFileRange = false;
            continue;
          }
        }
        else
        {
          if (Buffer.empty())
          {
            Buffer.resize(1024 * 1024);
          }
          Count = read(In.Get(), Buffer.data(), Buffer.size());
          if (Count > 0)
          {
            ssize_t Written = 0;
            while (Written < Count)
            {
              ssize_t W = write(Out.Get(), Buffer.data() + Written, static_cast<size_t>(Count - Written));
              if (W < 0)
              {
                if (errno == EINTR)
                {
                  continue;
                }
                int Error = errno;
                throw ExtException(QStringLiteral("Error copying file '%1' to '%2'.").arg(Source, Target), ErrnoText(Error));
              }
              Written += W;
            }
          }
        }
        if (Count < 0)
        {
          if (errno == EINTR)
          {
            continue;
          }
          int Error = errno;
          throw ExtException(QStringLiteral("Error copying file '%1' to '%2'.").arg(Source, Target), ErrnoText(Error));
        }
        if (Count == 0)
        {
          break;
        }
        Copied += Count;
        if (FProgress != nullptr)
        {
          FProgress->AddTransferred(Count);
        }
      }
    }

    // chown before chmod: chown clears set-user-ID bits
    SetOwner(Out.Get(), QString(), St, false);
    if (fchmod(Out.Get(), TargetMode(St, false)) != 0)
    {
      int Error = errno;
      throw ExtException(QStringLiteral("Cannot set permissions of file '%1'.").arg(Target), ErrnoText(Error));
    }
    if (FParams.PreserveTime)
    {
      struct timespec Times[2] = {St.st_atim, St.st_mtim};
      if (futimens(Out.Get(), Times) != 0)
      {
        int Error = errno;
        throw ExtException(QStringLiteral("Cannot set timestamp of file '%1'.").arg(Target), ErrnoText(Error));
      }
    }
    if (::close(Out.Release()) != 0)
    {
      int Error = errno;
      throw ExtException(QStringLiteral("Error copying file '%1' to '%2'.").arg(Source, Target), ErrnoText(Error));
    }
    // Atomically replaces an existing target, only now that the copy is complete
    if (rename(Native(Temp).constData(), Native(Target).constData()) != 0)
    {
      int Error = errno;
      throw ExtException(QStringLiteral("Error copying file '%1' to '%2'.").arg(Source, Target), ErrnoText(Error));
    }
    TempGuard.Release();
  }

  void CopySpecial(const QString & Source, const struct stat & St, const QString & Target)
  {
    QString Temp;
    bool Link = S_ISLNK(St.st_mode);
    QByteArray LinkTarget;
    if (Link)
    {
      std::vector<char> Buffer(static_cast<size_t>(std::max<off_t>(St.st_size, 255)) + 1);
      for (;;)
      {
        ssize_t Length = readlink(Native(Source).constData(), Buffer.data(), Buffer.size());
        if (Length < 0)
        {
          int Error = errno;
          throw ExtException(QStringLiteral("Cannot read symbolic link '%1'.").arg(Source), ErrnoText(Error));
        }
        if (static_cast<size_t>(Length) < Buffer.size())
        {
          LinkTarget = QByteArray(Buffer.data(), static_cast<int>(Length));
          break;
        }
        Buffer.resize(Buffer.size() * 2);
      }
    }
    int Result = -1;
    for (int Attempt = 0; Attempt < 10; Attempt++)
    {
      Temp = TemporaryName(Target);
      if (Link)
      {
        Result = symlink(LinkTarget.constData(), Native(Temp).constData());
      }
      else if (S_ISFIFO(St.st_mode))
      {
        Result = mkfifo(Native(Temp).constData(), 0600);
      }
      else
      {
        // character/block devices and sockets: only root can create devices
        Result = mknod(Native(Temp).constData(), (St.st_mode & S_IFMT) | 0600, St.st_rdev);
      }
      if ((Result == 0) || (errno != EEXIST))
      {
        break;
      }
    }
    if (Result != 0)
    {
      int Error = errno;
      throw ExtException(QStringLiteral("Error copying file '%1' to '%2'.").arg(Source, Target), ErrnoText(Error));
    }
    TemporaryGuard TempGuard(Temp);
    SetOwner(-1, Temp, St, Link);
    if (!Link)
    {
      (void)chmod(Native(Temp).constData(), TargetMode(St, false));
    }
    if (FParams.PreserveTime)
    {
      struct timespec Times[2] = {St.st_atim, St.st_mtim};
      (void)utimensat(AT_FDCWD, Native(Temp).constData(), Times, AT_SYMLINK_NOFOLLOW);
    }
    if (rename(Native(Temp).constData(), Native(Target).constData()) != 0)
    {
      int Error = errno;
      throw ExtException(QStringLiteral("Error copying file '%1' to '%2'.").arg(Source, Target), ErrnoText(Error));
    }
    TempGuard.Release();
    if (FProgress != nullptr)
    {
      FProgress->AddTransferred(0);
    }
  }

  void FinishDirectory(const QString & Target, const struct stat & St)
  {
    struct stat Current;
    if (LStat(Target, Current))
    {
      SetOwner(-1, Target, St, false);
      (void)chmod(Native(Target).constData(), TargetMode(St, true));
      if (FParams.PreserveTime)
      {
        struct timespec Times[2] = {St.st_atim, St.st_mtim};
        (void)utimensat(AT_FDCWD, Native(Target).constData(), Times, 0);
      }
    }
  }

  // Copies Source to Target; false when something was skipped.
  // Confirmed: overwriting an existing Target was already confirmed.
  bool CopyItem(const QString & Source, const QString & Target, bool Confirmed, bool DeleteSource)
  {
    CheckCancel();
    struct stat St;
    if (!LStat(Source, St))
    {
      int Error = errno;
      throw ExtException(QStringLiteral("File or folder '%1' does not exist.").arg(Source), ErrnoText(Error));
    }
    struct stat TargetSt;
    bool Exists = LStat(Target, TargetSt);
    if (Exists && (TargetSt.st_dev == St.st_dev) && (TargetSt.st_ino == St.st_ino))
    {
      // the same file (hard link or the item itself)
      return true;
    }
    if (S_ISDIR(St.st_mode))
    {
      bool Created = false;
      if (Exists)
      {
        if (!S_ISDIR(TargetSt.st_mode))
        {
          throw ExtException(QStringLiteral("Cannot overwrite file '%1' with directory '%2'.").arg(Target, Source));
        }
        if (!Confirmed && !ConfirmOverwrite(Target, St, TargetSt))
        {
          FResult.Skipped++;
          return false;
        }
      }
      else
      {
        if (mkdir(Native(Target).constData(), 0700) != 0)
        {
          int Error = errno;
          throw ExtException(QStringLiteral("Cannot create folder '%1'.").arg(Target), ErrnoText(Error));
        }
        Created = true;
      }
      bool Complete = true;
      const QStringList Names = ListDirectory(Source);
      for (const QString & Name : Names)
      {
        QString ChildSource = Combine(Source, Name);
        QString ChildTarget = Combine(Target, Name);
        if (!ItemLoop([&]() { return CopyItem(ChildSource, ChildTarget, false, DeleteSource); }))
        {
          Complete = false;
        }
      }
      if (Created || FParams.PreserveRights || FParams.PreserveTime)
      {
        FinishDirectory(Target, St);
      }
      if (DeleteSource && Complete)
      {
        if (rmdir(Native(Source).constData()) != 0)
        {
          int Error = errno;
          throw ExtException(QStringLiteral("Cannot delete folder '%1'.").arg(Source), ErrnoText(Error));
        }
      }
      return Complete;
    }

    if (Exists)
    {
      if (S_ISDIR(TargetSt.st_mode))
      {
        throw ExtException(QStringLiteral("Cannot overwrite directory '%1' with file '%2'.").arg(Target, Source));
      }
      if (!Confirmed && !ConfirmOverwrite(Target, St, TargetSt))
      {
        FResult.Skipped++;
        return false;
      }
    }
    if (FProgress != nullptr)
    {
      FProgress->SetFile(Source, S_ISREG(St.st_mode) ? St.st_size : 0);
    }
    if (S_ISREG(St.st_mode))
    {
      CopyRegular(Source, St, Target);
    }
    else
    {
      CopySpecial(Source, St, Target);
    }
    if (DeleteSource)
    {
      if (unlink(Native(Source).constData()) != 0)
      {
        int Error = errno;
        throw ExtException(QStringLiteral("Cannot delete file '%1'.").arg(Source), ErrnoText(Error));
      }
    }
    if (FProgress != nullptr)
    {
      FProgress->FileFinished();
    }
    return true;
  }

  bool MoveItem(const QString & Source, const QString & Target, bool Confirmed)
  {
    CheckCancel();
    struct stat St;
    if (!LStat(Source, St))
    {
      int Error = errno;
      throw ExtException(QStringLiteral("File or folder '%1' does not exist.").arg(Source), ErrnoText(Error));
    }
    struct stat TargetSt;
    bool Exists = LStat(Target, TargetSt);
    if (Exists && (TargetSt.st_dev == St.st_dev) && (TargetSt.st_ino == St.st_ino))
    {
      return true;
    }
    bool Directory = S_ISDIR(St.st_mode);
    if (Exists)
    {
      if (Directory != S_ISDIR(TargetSt.st_mode))
      {
        throw ExtException(Directory ?
          QStringLiteral("Cannot overwrite file '%1' with directory '%2'.").arg(Target, Source) :
          QStringLiteral("Cannot overwrite directory '%1' with file '%2'.").arg(Target, Source));
      }
      if (!Confirmed && !ConfirmOverwrite(Target, St, TargetSt))
      {
        FResult.Skipped++;
        return false;
      }
      Confirmed = true;
      if (Directory)
      {
        // Merge into the existing directory
        bool Complete = true;
        const QStringList Names = ListDirectory(Source);
        for (const QString & Name : Names)
        {
          QString ChildSource = Combine(Source, Name);
          QString ChildTarget = Combine(Target, Name);
          if (!ItemLoop([&]() { return MoveItem(ChildSource, ChildTarget, false); }))
          {
            Complete = false;
          }
        }
        if (Complete && (rmdir(Native(Source).constData()) != 0))
        {
          int Error = errno;
          throw ExtException(QStringLiteral("Cannot delete folder '%1'.").arg(Source), ErrnoText(Error));
        }
        return Complete;
      }
    }
    if (FProgress != nullptr)
    {
      FProgress->SetFile(Source, 0);
    }
    if (rename(Native(Source).constData(), Native(Target).constData()) == 0)
    {
      if (FProgress != nullptr)
      {
        FProgress->FileFinished();
      }
      return true;
    }
    int Error = errno;
    if (Error != EXDEV)
    {
      throw ExtException(QStringLiteral("Error moving file '%1' to '%2'.").arg(Source, Target), ErrnoText(Error));
    }
    // Another file system: copy, then delete what was copied
    return CopyItem(Source, Target, Confirmed, true);
  }
};

LocalOperationResult Engine::Run()
{
  QString TargetDirectory = CleanPath(FParams.TargetDirectory);
  struct stat TargetDirSt;
  if (!LStat(TargetDirectory, TargetDirSt) || !S_ISDIR(TargetDirSt.st_mode))
  {
    if (stat(Native(TargetDirectory).constData(), &TargetDirSt) != 0 || !S_ISDIR(TargetDirSt.st_mode))
    {
      FResult.Ok = false;
      FResult.Message = QStringLiteral("Target directory '%1' does not exist.").arg(TargetDirectory);
      return FResult;
    }
  }
  FTargetDevice = TargetDirSt.st_dev;

  // Totals for the progress (moves within the file system are renames, not scanned)
  qint64 TotalSize = 0;
  int Count = 0;
  try
  {
    for (const QString & Source : FParams.Sources)
    {
      struct stat St;
      if (IsMove() && LStat(Source, St) && (St.st_dev == FTargetDevice))
      {
        Count++;
      }
      else
      {
        Scan(Source, TotalSize, Count);
      }
    }
  }
  catch (const AbortException &)
  {
    FResult.Ok = false;
    FResult.Aborted = true;
    return FResult;
  }
  if (FProgress != nullptr)
  {
    FProgress->Start(IsMove() ? FileOperationProgress::Move : FileOperationProgress::Copy,
      FileOperationProgress::ToLocal, Count);
    FProgress->AddTotalSize(TotalSize);
  }

  try
  {
    for (const QString & ASource : FParams.Sources)
    {
      QString Source = CleanPath(ASource);
      QString Name = ((FParams.Sources.size() == 1) && !FParams.NewName.isEmpty()) ? FParams.NewName : QFileInfo(Source).fileName();
      QString Target = Combine(TargetDirectory, Name);
      if (Target == Source)
      {
        if (IsMove() || !FParams.DuplicateIfSame)
        {
          continue;
        }
        Target = Combine(TargetDirectory, LocalFileOperations::DuplicateName(TargetDirectory, Name));
      }
      struct stat St;
      bool IsDir = LStat(Source, St) && S_ISDIR(St.st_mode);
      bool Complete = ItemLoop([&]()
      {
        if (IsDir && LocalFileOperations::IsSubPath(Source, Target))
        {
          throw ExtException(QStringLiteral("Cannot move or copy folder '%1' into itself.").arg(Source));
        }
        return IsMove() ? MoveItem(Source, Target, false) : CopyItem(Source, Target, false, false);
      });
      if (Complete)
      {
        FResult.Processed++;
        FResult.Targets.append(Target);
      }
    }
  }
  catch (const AbortException &)
  {
    FResult.Ok = false;
    FResult.Aborted = true;
  }
  catch (const ExtException & E)
  {
    FResult.Ok = false;
    FResult.Message = E.Message();
    FResult.MoreMessages = E.MoreMessages();
  }
  if (FProgress != nullptr)
  {
    FProgress->Stop();
  }
  return FResult;
}

bool ResolveUser(const QString & Name, uid_t & Uid)
{
  bool Numeric = false;
  unsigned int Value = Name.toUInt(&Numeric);
  if (Numeric)
  {
    Uid = static_cast<uid_t>(Value);
    return true;
  }
  struct passwd * Entry = getpwnam(Name.toLocal8Bit().constData());
  if (Entry == nullptr)
  {
    return false;
  }
  Uid = Entry->pw_uid;
  return true;
}

bool ResolveGroup(const QString & Name, gid_t & Gid)
{
  bool Numeric = false;
  unsigned int Value = Name.toUInt(&Numeric);
  if (Numeric)
  {
    Gid = static_cast<gid_t>(Value);
    return true;
  }
  struct group * Entry = getgrnam(Name.toLocal8Bit().constData());
  if (Entry == nullptr)
  {
    return false;
  }
  Gid = Entry->gr_gid;
  return true;
}

} // namespace

LocalOperationResult LocalFileOperations::Execute(const Params & AParams, FileOperationProgress * Progress, UserInterface * UI)
{
  Engine E(AParams, Progress, UI);
  return E.Run();
}

bool LocalFileOperations::MoveToTrash(const QString & Path, QString * Error)
{
  QFileInfo Info(Path);
  if (!Info.exists() && !Info.isSymLink())
  {
    if (Error != nullptr)
    {
      *Error = QStringLiteral("File or folder '%1' does not exist.").arg(Path);
    }
    return false;
  }
  // freedesktop Trash specification: the home trash for files on the home
  // volume, $topdir/.Trash/$uid or $topdir/.Trash-$uid on other volumes
  // (USB drives, /tmp...)
  if (QFile::moveToTrash(Path))
  {
    return true;
  }
  // GVfs knows more (network and FUSE mounts under /run/user/<uid>/gvfs)
  QString Gio = QStandardPaths::findExecutable(QStringLiteral("gio"));
  QString Output;
  if (!Gio.isEmpty())
  {
    QProcess Process;
    Process.setProcessChannelMode(QProcess::MergedChannels);
    Process.start(Gio, {QStringLiteral("trash"), QStringLiteral("--"), Info.absoluteFilePath()});
    if (Process.waitForFinished(60000) && (Process.exitStatus() == QProcess::NormalExit) && (Process.exitCode() == 0))
    {
      QFileInfo After(Path);
      if (!After.exists() && !After.isSymLink())
      {
        return true;
      }
    }
    else if (Process.state() != QProcess::NotRunning)
    {
      Process.kill();
      Process.waitForFinished(1000);
    }
    Output = QString::fromLocal8Bit(Process.readAll()).trimmed();
  }
  if (Error != nullptr)
  {
    *Error = QStringLiteral("Cannot move '%1' to Trash.").arg(Path);
    if (!Output.isEmpty())
    {
      *Error += QLatin1Char('\n') + Output;
    }
  }
  return false;
}

QString LocalFileOperations::DuplicateName(const QString & Directory, const QString & FileName)
{
  QString Base = FileName;
  QString Extension;
  int Dot = FileName.lastIndexOf(QLatin1Char('.'));
  if (Dot > 0)
  {
    Base = FileName.left(Dot);
    Extension = FileName.mid(Dot);
    // keep compound archive extensions together ("a.tar.gz" -> "a (copy).tar.gz")
    if (Base.endsWith(QStringLiteral(".tar"), Qt::CaseInsensitive) && (Base.size() > 4))
    {
      Base.chop(4);
      Extension = FileName.mid(Base.size());
    }
  }
  for (int Index = 1; ; Index++)
  {
    QString Name = (Index == 1) ?
      QStringLiteral("%1 (copy)%2").arg(Base, Extension) :
      QStringLiteral("%1 (copy %2)%3").arg(Base).arg(Index).arg(Extension);
    struct stat St;
    if (!LStat(Combine(Directory, Name), St))
    {
      return Name;
    }
  }
}

bool LocalFileOperations::IsSubPath(const QString & Directory, const QString & Path)
{
  QString D = CleanPath(Directory);
  QString P = CleanPath(Path);
  if (P == D)
  {
    return true;
  }
  return P.startsWith((D == QStringLiteral("/")) ? D : (D + QLatin1Char('/')));
}

LocalOperationResult LocalFileOperations::ChangeProperties(const PropertiesParams & AParams, FileOperationProgress * Progress,
  UserInterface * UI)
{
  LocalOperationResult Result;
  uid_t Uid = static_cast<uid_t>(-1);
  gid_t Gid = static_cast<gid_t>(-1);
  if (!AParams.Owner.isEmpty() && !ResolveUser(AParams.Owner, Uid))
  {
    Result.Ok = false;
    Result.Message = QStringLiteral("User '%1' does not exist.").arg(AParams.Owner);
    return Result;
  }
  if (!AParams.Group.isEmpty() && !ResolveGroup(AParams.Group, Gid))
  {
    Result.Ok = false;
    Result.Message = QStringLiteral("Group '%1' does not exist.").arg(AParams.Group);
    return Result;
  }
  if (Progress != nullptr)
  {
    Progress->Start(FileOperationProgress::SetProperties, FileOperationProgress::ToLocal, static_cast<int>(AParams.Paths.size()));
  }
  bool SkipAll = false;

  std::function<void(const QString &)> Process = [&](const QString & Path)
  {
    for (;;)
    {
      try
      {
        if (Progress != nullptr)
        {
          Progress->CheckCancel();
          Progress->SetFile(Path, 0);
        }
        struct stat St;
        if (!LStat(Path, St))
        {
          int Error = errno;
          throw ExtException(QStringLiteral("File or folder '%1' does not exist.").arg(Path), ErrnoText(Error));
        }
        bool Link = S_ISLNK(St.st_mode);
        if ((Uid != static_cast<uid_t>(-1)) || (Gid != static_cast<gid_t>(-1)))
        {
          if ((Uid != St.st_uid && Uid != static_cast<uid_t>(-1)) || (Gid != St.st_gid && Gid != static_cast<gid_t>(-1)))
          {
            if (fchownat(AT_FDCWD, Native(Path).constData(), Uid, Gid, AT_SYMLINK_NOFOLLOW) != 0)
            {
              int Error = errno;
              throw ExtException(QStringLiteral("Cannot change owner of file '%1'.").arg(Path), ErrnoText(Error));
            }
          }
        }
        // permissions of symbolic links cannot be changed on Linux (and do not matter)
        if (AParams.ChangeMode && !Link)
        {
          mode_t Mode = AParams.ModeMasks ?
            static_cast<mode_t>(((St.st_mode & 07777) & ~AParams.ClearMask) | AParams.SetMask) :
            static_cast<mode_t>(AParams.Mode & 07777);
          if (AParams.AddXToDirectories && S_ISDIR(St.st_mode))
          {
            if (Mode & S_IRUSR) Mode |= S_IXUSR;
            if (Mode & S_IRGRP) Mode |= S_IXGRP;
            if (Mode & S_IROTH) Mode |= S_IXOTH;
          }
          if ((Mode != (St.st_mode & 07777)) && (chmod(Native(Path).constData(), Mode) != 0))
          {
            int Error = errno;
            throw ExtException(QStringLiteral("Cannot change permissions of file '%1'.").arg(Path), ErrnoText(Error));
          }
        }
        if (Progress != nullptr)
        {
          Progress->FileFinished();
        }
        if (AParams.Recursive && S_ISDIR(St.st_mode))
        {
          for (const QString & Name : ListDirectory(Path))
          {
            Process(Combine(Path, Name));
          }
        }
        return;
      }
      catch (const SkipFileException &)
      {
        Result.Skipped++;
        return;
      }
      catch (const AbortException &)
      {
        throw;
      }
      catch (const ExtException & E)
      {
        if (UI == nullptr)
        {
          throw;
        }
        unsigned int Answer = qaSkip;
        if (!SkipAll)
        {
          QueryUserParams QueryParams;
          QueryParams.Params = qpAllowContinueOnError;
          QueryParams.Type = qtError;
          QueryParams.Aliases.append(QueryButtonAlias{qaAll, QStringLiteral("Ski&p all")});
          Answer = UI->QueryUserEx(E.Message(), E.MoreMessages(), qaRetry | qaSkip | qaAll | qaAbort, QueryParams);
        }
        if (Answer == qaRetry)
        {
          continue;
        }
        if (Answer == qaAll)
        {
          SkipAll = true;
          Answer = qaSkip;
        }
        if (Answer == qaSkip)
        {
          Result.Skipped++;
          return;
        }
        throw AbortException();
      }
    }
  };

  try
  {
    for (const QString & Path : AParams.Paths)
    {
      Process(Path);
      Result.Processed++;
      Result.Targets.append(Path);
    }
  }
  catch (const AbortException &)
  {
    Result.Ok = false;
    Result.Aborted = true;
  }
  catch (const ExtException & E)
  {
    Result.Ok = false;
    Result.Message = E.Message();
    Result.MoreMessages = E.MoreMessages();
  }
  if (Progress != nullptr)
  {
    Progress->Stop();
  }
  return Result;
}

QStringList LocalFileOperations::UserNames()
{
  QStringList Result;
  setpwent();
  while (struct passwd * Entry = getpwent())
  {
    Result.append(QString::fromLocal8Bit(Entry->pw_name));
  }
  endpwent();
  Result.removeDuplicates();
  Result.sort(Qt::CaseInsensitive);
  return Result;
}

QStringList LocalFileOperations::GroupNames()
{
  QStringList Result;
  setgrent();
  while (struct group * Entry = getgrent())
  {
    Result.append(QString::fromLocal8Bit(Entry->gr_name));
  }
  endgrent();
  Result.removeDuplicates();
  Result.sort(Qt::CaseInsensitive);
  return Result;
}

QString LocalFileOperations::OwnerName(unsigned int Uid)
{
  struct passwd * Entry = getpwuid(static_cast<uid_t>(Uid));
  return (Entry != nullptr) ? QString::fromLocal8Bit(Entry->pw_name) : QString::number(Uid);
}

QString LocalFileOperations::GroupName(unsigned int Gid)
{
  struct group * Entry = getgrgid(static_cast<gid_t>(Gid));
  return (Entry != nullptr) ? QString::fromLocal8Bit(Entry->gr_name) : QString::number(Gid);
}
