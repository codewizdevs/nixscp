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
#include <QRandomGenerator>

#include "core/LocalFileSystem.h"

#include <exception>

namespace
{

using LocalFs::Stat;

QString ErrnoText(int Error)
{
  return LocalFs::ErrorText(Error);
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

QStringList ListDirectory(const QString & Path)
{
  return LocalFs::ListDirectory(Path);
}

// LStat(Path, Stat): LocalFs::LStat (found through the Stat argument)
using LocalFs::LStat;

QString FormatStat(const Stat & St)
{
  QDateTime Modified = QDateTime::fromSecsSinceEpoch(St.ModifiedSec);
  return QStringLiteral("%1 bytes, %2").arg(QLocale().toString(St.Size),
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
      LocalFs::RemoveFile(FPath);
    }
  }
  void Release() { FPath.clear(); }
private:
  QString FPath;
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
  quint64 FTargetDevice = 0;

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
    Stat St;
    if (!LStat(Path, St))
    {
      return;
    }
    if (St.IsDirectory())
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
      if (St.IsRegular())
      {
        Size += St.Size;
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
  bool ConfirmOverwrite(const QString & Target, const Stat & SourceSt, const Stat & TargetSt)
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
    bool Directory = TargetSt.IsDirectory();
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

  unsigned int TargetMode(const Stat & St, bool Directory) const
  {
    return FParams.PreserveRights ? St.Mode : LocalFs::DefaultMode(Directory);
  }

  // Owner, permissions and times of a copy (before it is renamed to its target)
  void ApplyMetadata(const QString & Path, const QString & Target, const Stat & St, bool Link, bool MustSucceed)
  {
    // chown before chmod: chown clears set-user-ID bits
    if (FParams.PreserveOwner)
    {
      LocalFs::CopyOwner(Path, St, Link);
    }
    if (!Link && !LocalFs::SetMode(Path, TargetMode(St, false)) && MustSucceed)
    {
      int Error = LocalFs::LastError();
      throw ExtException(QStringLiteral("Cannot set permissions of file '%1'.").arg(Target), ErrnoText(Error));
    }
    if (FParams.PreserveTime && !LocalFs::SetTimes(Path, St, Link) && MustSucceed)
    {
      int Error = LocalFs::LastError();
      throw ExtException(QStringLiteral("Cannot set timestamp of file '%1'.").arg(Target), ErrnoText(Error));
    }
  }

  void CopyRegular(const QString & Source, const Stat & St, const QString & Target)
  {
    // Cancel, pause and "skip file" of the progress, from within the copy
    std::exception_ptr Stopped;
    auto Progress = [&](qint64 Delta) -> bool
    {
      try
      {
        if ((Delta > 0) && (FProgress != nullptr))
        {
          FProgress->AddTransferred(Delta);
        }
        CheckCancel();
        return true;
      }
      catch (...)
      {
        Stopped = std::current_exception();
        return false;
      }
    };
    QString Temp;
    bool Ok = false;
    bool Cancelled = false;
    LocalFs::TCopyStep Step = LocalFs::csOpenSource;
    int Error = 0;
    for (int Attempt = 0; Attempt < 10; Attempt++)
    {
      Temp = TemporaryName(Target);
      Ok = LocalFs::CopyFileData(Source, Temp, St.Size, FParams.UseReflink, Progress, Cancelled, Step);
      Error = Ok ? 0 : LocalFs::LastError();
      if (Ok || (Step != LocalFs::csCreateTarget) || !LocalFs::IsExistsError(Error))
      {
        break;
      }
    }
    // A file created by the attempt is removed unless renamed to its target
    TemporaryGuard TempGuard((Step == LocalFs::csCopy) ? Temp : QString());
    if (!Ok)
    {
      if (Cancelled && Stopped)
      {
        std::rethrow_exception(Stopped);
      }
      switch (Step)
      {
        case LocalFs::csOpenSource:
          throw ExtException(QStringLiteral("Cannot open file '%1'.").arg(Source), ErrnoText(Error));
        case LocalFs::csCreateTarget:
          throw ExtException(QStringLiteral("Cannot create file '%1'.").arg(Target), ErrnoText(Error));
        default:
          throw ExtException(QStringLiteral("Error copying file '%1' to '%2'.").arg(Source, Target), ErrnoText(Error));
      }
    }
    ApplyMetadata(Temp, Target, St, false, true);
    // Atomically replaces an existing target, only now that the copy is complete
    if (!LocalFs::Rename(Temp, Target))
    {
      int RenameError = LocalFs::LastError();
      throw ExtException(QStringLiteral("Error copying file '%1' to '%2'.").arg(Source, Target), ErrnoText(RenameError));
    }
    TempGuard.Release();
  }

  void CopySpecial(const QString & Source, const Stat & St, const QString & Target)
  {
    QString Temp;
    bool Ok = false;
    int Error = 0;
    for (int Attempt = 0; Attempt < 10; Attempt++)
    {
      Temp = TemporaryName(Target);
      Ok = LocalFs::CopySpecial(Source, St, Temp);
      Error = Ok ? 0 : LocalFs::LastError();
      if (Ok || !LocalFs::IsExistsError(Error))
      {
        break;
      }
    }
    if (!Ok)
    {
      throw ExtException(QStringLiteral("Error copying file '%1' to '%2'.").arg(Source, Target), ErrnoText(Error));
    }
    TemporaryGuard TempGuard(Temp);
    ApplyMetadata(Temp, Target, St, St.IsSymlink(), false);
    if (!LocalFs::Rename(Temp, Target))
    {
      int RenameError = LocalFs::LastError();
      throw ExtException(QStringLiteral("Error copying file '%1' to '%2'.").arg(Source, Target), ErrnoText(RenameError));
    }
    TempGuard.Release();
    if (FProgress != nullptr)
    {
      FProgress->AddTransferred(0);
    }
  }

  void FinishDirectory(const QString & Target, const Stat & St)
  {
    Stat Current;
    if (LStat(Target, Current))
    {
      if (FParams.PreserveOwner)
      {
        LocalFs::CopyOwner(Target, St, false);
      }
      (void)LocalFs::SetMode(Target, TargetMode(St, true));
      if (FParams.PreserveTime)
      {
        (void)LocalFs::SetTimes(Target, St, false);
      }
    }
  }

  // Copies Source to Target; false when something was skipped.
  // Confirmed: overwriting an existing Target was already confirmed.
  bool CopyItem(const QString & Source, const QString & Target, bool Confirmed, bool DeleteSource)
  {
    CheckCancel();
    Stat St;
    if (!LStat(Source, St))
    {
      int Error = LocalFs::LastError();
      throw ExtException(QStringLiteral("File or folder '%1' does not exist.").arg(Source), ErrnoText(Error));
    }
    Stat TargetSt;
    bool Exists = LStat(Target, TargetSt);
    if (Exists && TargetSt.SameFile(St))
    {
      // the same file (hard link or the item itself)
      return true;
    }
    if (St.IsDirectory())
    {
      bool Created = false;
      if (Exists)
      {
        if (!TargetSt.IsDirectory())
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
        if (!LocalFs::MakeDirectory(Target))
        {
          int Error = LocalFs::LastError();
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
        if (!LocalFs::RemoveDirectory(Source))
        {
          int Error = LocalFs::LastError();
          throw ExtException(QStringLiteral("Cannot delete folder '%1'.").arg(Source), ErrnoText(Error));
        }
      }
      return Complete;
    }

    if (Exists)
    {
      if (TargetSt.IsDirectory())
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
      FProgress->SetFile(Source, St.IsRegular() ? St.Size : 0);
    }
    if (St.IsRegular())
    {
      CopyRegular(Source, St, Target);
    }
    else
    {
      CopySpecial(Source, St, Target);
    }
    if (DeleteSource)
    {
      if (!LocalFs::RemoveFile(Source))
      {
        int Error = LocalFs::LastError();
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
    Stat St;
    if (!LStat(Source, St))
    {
      int Error = LocalFs::LastError();
      throw ExtException(QStringLiteral("File or folder '%1' does not exist.").arg(Source), ErrnoText(Error));
    }
    Stat TargetSt;
    bool Exists = LStat(Target, TargetSt);
    if (Exists && TargetSt.SameFile(St))
    {
      return true;
    }
    bool Directory = St.IsDirectory();
    if (Exists)
    {
      if (Directory != TargetSt.IsDirectory())
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
        if (Complete && !LocalFs::RemoveDirectory(Source))
        {
          int Error = LocalFs::LastError();
          throw ExtException(QStringLiteral("Cannot delete folder '%1'.").arg(Source), ErrnoText(Error));
        }
        return Complete;
      }
    }
    if (FProgress != nullptr)
    {
      FProgress->SetFile(Source, 0);
    }
    if (LocalFs::Rename(Source, Target))
    {
      if (FProgress != nullptr)
      {
        FProgress->FileFinished();
      }
      return true;
    }
    int Error = LocalFs::LastError();
    if (!LocalFs::IsCrossDeviceError(Error))
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
  Stat TargetDirSt;
  if (!LStat(TargetDirectory, TargetDirSt) || !TargetDirSt.IsDirectory())
  {
    if (!LocalFs::StatFollow(TargetDirectory, TargetDirSt) || !TargetDirSt.IsDirectory())
    {
      FResult.Ok = false;
      FResult.Message = QStringLiteral("Target directory '%1' does not exist.").arg(TargetDirectory);
      return FResult;
    }
  }
  FTargetDevice = TargetDirSt.Device;

  // Totals for the progress (moves within the file system are renames, not scanned)
  qint64 TotalSize = 0;
  int Count = 0;
  try
  {
    for (const QString & Source : FParams.Sources)
    {
      Stat St;
      if (IsMove() && LStat(Source, St) && (St.Device == FTargetDevice))
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
      Stat St;
      bool IsDir = LStat(Source, St) && St.IsDirectory();
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
  // Linux: the freedesktop Trash (the home trash for files on the home volume,
  // $topdir/.Trash/$uid or $topdir/.Trash-$uid on other volumes); Windows: the Recycle Bin
  if (QFile::moveToTrash(Path))
  {
    return true;
  }
  QString Output;
  if (LocalFs::MoveToTrashFallback(Path, Output))
  {
    return true;
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
    Stat St;
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
  // Windows paths are case insensitive
  Qt::CaseSensitivity Case = LocalFs::HasUnixPermissions() ? Qt::CaseSensitive : Qt::CaseInsensitive;
  if (P.compare(D, Case) == 0)
  {
    return true;
  }
  return P.startsWith(D.endsWith(QLatin1Char('/')) ? D : (D + QLatin1Char('/')), Case);
}

LocalOperationResult LocalFileOperations::ChangeProperties(const PropertiesParams & AParams, FileOperationProgress * Progress,
  UserInterface * UI)
{
  LocalOperationResult Result;
  unsigned int Uid = 0;
  unsigned int Gid = 0;
  bool ChangeUid = !AParams.Owner.isEmpty();
  bool ChangeGid = !AParams.Group.isEmpty();
  if (ChangeUid && !LocalFs::ResolveUser(AParams.Owner, Uid))
  {
    Result.Ok = false;
    Result.Message = QStringLiteral("User '%1' does not exist.").arg(AParams.Owner);
    return Result;
  }
  if (ChangeGid && !LocalFs::ResolveGroup(AParams.Group, Gid))
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
        Stat St;
        if (!LStat(Path, St))
        {
          int Error = LocalFs::LastError();
          throw ExtException(QStringLiteral("File or folder '%1' does not exist.").arg(Path), ErrnoText(Error));
        }
        bool Link = St.IsSymlink();
        if ((ChangeUid && (Uid != St.Uid)) || (ChangeGid && (Gid != St.Gid)))
        {
          if (!LocalFs::ChangeOwner(Path, Uid, Gid, ChangeUid, ChangeGid))
          {
            int Error = LocalFs::LastError();
            throw ExtException(QStringLiteral("Cannot change owner of file '%1'.").arg(Path), ErrnoText(Error));
          }
        }
        // permissions of symbolic links cannot be changed on Linux (and do not matter)
        if (AParams.ChangeMode && !Link)
        {
          unsigned int Mode = AParams.ModeMasks ?
            ((St.Mode & ~AParams.ClearMask) | AParams.SetMask) :
            (AParams.Mode & 07777);
          if (AParams.AddXToDirectories && St.IsDirectory())
          {
            if (Mode & 0400) Mode |= 0100;
            if (Mode & 0040) Mode |= 0010;
            if (Mode & 0004) Mode |= 0001;
          }
          if ((Mode != St.Mode) && !LocalFs::SetMode(Path, Mode))
          {
            int Error = LocalFs::LastError();
            throw ExtException(QStringLiteral("Cannot change permissions of file '%1'.").arg(Path), ErrnoText(Error));
          }
        }
        if (Progress != nullptr)
        {
          Progress->FileFinished();
        }
        if (AParams.Recursive && St.IsDirectory())
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
  return LocalFs::UserNames();
}

QStringList LocalFileOperations::GroupNames()
{
  return LocalFs::GroupNames();
}

QString LocalFileOperations::OwnerName(unsigned int Uid)
{
  return LocalFs::OwnerName(Uid);
}

QString LocalFileOperations::GroupName(unsigned int Gid)
{
  return LocalFs::GroupName(Gid);
}
