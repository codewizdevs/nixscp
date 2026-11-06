// Port of WinSCP source/core/Terminal.cpp (SynchronizeCollect, DoSynchronizeCollectDirectory,
// DoSynchronizeCollectFile, SynchronizedFileCheckModified, SynchronizedFileNewOrModified,
// SameFileChecksum, SynchronizeApply, SynchronizeLocalTimestamp, SynchronizeRemoteTimestamp,
// SynchronizeToQueue, SynchronizeChecklistCalculateSize),
// source/core/RemoteFiles.cpp (TSynchronizeChecklist, TSynchronizeProgress),
// source/windows/SynchronizeController.cpp, source/forms/CustomScpExplorer.cpp
// (DoSynchronizeMove) and source/packages/filemng/CustomDirView.pas (ProcessChangedFiles)
#include "core/Synchronize.h"
#include "core/ActionsLog.h"
#include "core/Platform.h"

#include "core/FtpFileSystem.h"
#include "core/LocalFileOperations.h"
#include "core/Queue.h"
#include "core/SecureShell.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMetaMethod>
#include <QPointer>
#include <QSocketNotifier>
#include <QTemporaryDir>

#include <algorithm>
#include <limits>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <QFileSystemWatcher>

#ifndef Q_OS_WIN
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
// Watching local directories: inotify on Linux; QFileSystemWatcher elsewhere
// (ReadDirectoryChangesW on Windows, FSEvents/kqueue on macOS)
#ifdef Q_OS_LINUX
#define NIXSCP_INOTIFY 1
#include <sys/inotify.h>
#endif

//---------------------------------------------------------------------------
// Helpers

static QString NormalizeLocalDirectory(const QString & Directory)
{
  return QDir::cleanPath(QDir(Directory).absolutePath());
}

static QString NormalizeRemoteDirectory(const QString & Directory)
{
  QString Result = Directory;
  while ((Result.size() > 1) && Result.endsWith(QLatin1Char('/')))
  {
    Result.chop(1);
  }
  return Result;
}

static QString LocalCombine(const QString & Directory, const QString & FileName)
{
  return Directory.endsWith(QLatin1Char('/')) ? Directory + FileName : Directory + QLatin1Char('/') + FileName;
}

// Common.cpp CompareFileTime: "FAT" time precision (2 seconds)
static int CompareFileTime(const QDateTime & T1, const QDateTime & T2)
{
  qint64 Diff = T1.msecsTo(T2);
  if (Diff >= 2000)
  {
    return -1;
  }
  else if (Diff <= -2000)
  {
    return 1;
  }
  return 0;
}

// ReduceDateTimePrecision to seconds (SFTP mfFull)
static QDateTime ReduceToSeconds(const QDateTime & Time)
{
  return Time.isValid() ? QDateTime::fromSecsSinceEpoch(Time.toSecsSinceEpoch()) : Time;
}

static FileMasks::Params MaskParams(qint64 Size, const QDateTime & Modification)
{
  FileMasks::Params Result;
  Result.Size = Size;
  Result.Modification = Modification;
  return Result;
}

// TCustomFileSystem::TemporaryTransferFile
static bool TemporaryTransferFile(const QString & FileName)
{
  return FileName.endsWith(PartialExt, Qt::CaseInsensitive);
}

static void SetLocalModificationTime(const QString & Path, const QDateTime & Time)
{
  QString Error;
  if (!Platform::SetFileTimes(Path, Time.toSecsSinceEpoch(), -1, &Error))
  {
    throw ExtException(QStringLiteral("Can't set attributes of file '%1'.").arg(Path), Error);
  }
}

static QFileInfoList ListLocalDirectory(const QString & Directory)
{
  QDir Dir(Directory);
  if (!Dir.exists() || !Dir.isReadable())
  {
    throw ExtException(QStringLiteral("Error listing directory '%1'.").arg(Directory),
      QString::fromLocal8Bit(strerror(Dir.exists() ? EACCES : ENOENT)));
  }
  return Dir.entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
}

static bool ChecksumAlgorithm(const QString & Alg, QCryptographicHash::Algorithm & Algorithm)
{
  // WinSCP names (Sha256ChecksumAlg = "sha256", FTP "sha-256", ...)
  QString A = Alg.toLower();
  A.remove(QLatin1Char('-'));
  if (A == QStringLiteral("sha256")) { Algorithm = QCryptographicHash::Sha256; return true; }
  if (A == QStringLiteral("sha1")) { Algorithm = QCryptographicHash::Sha1; return true; }
  if (A == QStringLiteral("sha512")) { Algorithm = QCryptographicHash::Sha512; return true; }
  if (A == QStringLiteral("sha384")) { Algorithm = QCryptographicHash::Sha384; return true; }
  if (A == QStringLiteral("sha224")) { Algorithm = QCryptographicHash::Sha224; return true; }
  if (A == QStringLiteral("md5")) { Algorithm = QCryptographicHash::Md5; return true; }
  return false;
}

QString LocalFileChecksum(const QString & FileName, const QString & Alg)
{
  // TTerminal::CalculateLocalFileChecksum
  QCryptographicHash::Algorithm Algorithm;
  if (!ChecksumAlgorithm(Alg, Algorithm))
  {
    // UNKNOWN_CHECKSUM
    throw ExtException(QStringLiteral("Checksum algorithm '%1' is not supported.").arg(Alg));
  }
  QFile File(FileName);
  QCryptographicHash Hash(Algorithm);
  if (!File.open(QIODevice::ReadOnly) || !Hash.addData(&File))
  {
    // CHECKSUM_ERROR
    throw ExtException(QStringLiteral("Error calculating checksum for file '%1'.").arg(FileName), File.errorString());
  }
  return QString::fromLatin1(Hash.result().toHex());
}

QHash<QString, QString> ParseChecksumOutput(const QStringList & Lines)
{
  // "<hex>  <name>" or "<hex> *<name>" (binary mode); with a leading "\" the
  // name has "\\", "\n" (and "\r") escaped (GNU coreutils). "<hex> <name>" with
  // one space: "md5 -r" of macOS and the BSDs
  QHash<QString, QString> Result;
  for (const QString & ALine : Lines)
  {
    QString Line = ALine;
    if (Line.endsWith(QLatin1Char('\r')))
    {
      Line.chop(1);
    }
    bool Escaped = Line.startsWith(QLatin1Char('\\'));
    if (Escaped)
    {
      Line.remove(0, 1);
    }
    int Space = Line.indexOf(QLatin1Char(' '));
    if ((Space < 8) || (Space + 2 > Line.size()))
    {
      continue;
    }
    QString Hash = Line.left(Space);
    bool Hex = true;
    for (QChar C : Hash)
    {
      if (!C.isDigit() && !((C.toLower() >= QLatin1Char('a')) && (C.toLower() <= QLatin1Char('f'))))
      {
        Hex = false;
        break;
      }
    }
    if (!Hex)
    {
      continue;
    }
    QChar Mode = Line.at(Space + 1);
    QString Name = Line.mid(Space + (((Mode == QLatin1Char(' ')) || (Mode == QLatin1Char('*'))) ? 2 : 1));
    if (Escaped)
    {
      QString Unescaped;
      for (int Index = 0; Index < Name.size(); Index++)
      {
        QChar C = Name.at(Index);
        if ((C == QLatin1Char('\\')) && (Index + 1 < Name.size()))
        {
          QChar N = Name.at(++Index);
          Unescaped += (N == QLatin1Char('n')) ? QChar(QLatin1Char('\n')) : ((N == QLatin1Char('r')) ? QChar(QLatin1Char('\r')) : N);
        }
        else
        {
          Unescaped += C;
        }
      }
      Name = Unescaped;
    }
    Result.insert(Name, Hash.toLower());
  }
  return Result;
}

//---------------------------------------------------------------------------

QString SynchronizeModeStr(SynchronizeMode Mode)
{
  switch (Mode)
  {
    case smRemote: return QStringLiteral("Remote");
    case smLocal: return QStringLiteral("Local");
    case smBoth: return QStringLiteral("Both");
    default: return QStringLiteral("Unknown");
  }
}

static void AddFlagName(QString & ParamsStr, int & Params, int Param, const QString & Name)
{
  if ((Params & Param) != 0)
  {
    if (!ParamsStr.isEmpty())
    {
      ParamsStr += QStringLiteral(", ");
    }
    ParamsStr += Name;
  }
  Params &= ~Param;
}

QString SynchronizeParamsStr(int Params)
{
  QString ParamsStr;
  AddFlagName(ParamsStr, Params, spDelete, QStringLiteral("Delete"));
  AddFlagName(ParamsStr, Params, spNoConfirmation, QStringLiteral("NoConfirmation"));
  AddFlagName(ParamsStr, Params, spExistingOnly, QStringLiteral("ExistingOnly"));
  AddFlagName(ParamsStr, Params, spNoRecurse, QStringLiteral("NoRecurse"));
  AddFlagName(ParamsStr, Params, spUseCache, QStringLiteral("UseCache"));
  AddFlagName(ParamsStr, Params, spDelayProgress, QStringLiteral("DelayProgress"));
  AddFlagName(ParamsStr, Params, spPreviewChanges, QStringLiteral("*PreviewChanges"));
  AddFlagName(ParamsStr, Params, spSubDirs, QStringLiteral("SubDirs"));
  AddFlagName(ParamsStr, Params, spTimestamp, QStringLiteral("Timestamp"));
  AddFlagName(ParamsStr, Params, spNotByTime, QStringLiteral("NotByTime"));
  AddFlagName(ParamsStr, Params, spBySize, QStringLiteral("BySize"));
  AddFlagName(ParamsStr, Params, spByChecksum, QStringLiteral("ByChecksum"));
  AddFlagName(ParamsStr, Params, spCaseSensitive, QStringLiteral("CaseSensitive"));
  AddFlagName(ParamsStr, Params, spSelectedOnly, QStringLiteral("*SelectedOnly"));
  AddFlagName(ParamsStr, Params, spMirror, QStringLiteral("Mirror"));
  if (Params > 0)
  {
    if (!ParamsStr.isEmpty())
    {
      ParamsStr += QStringLiteral(", ");
    }
    ParamsStr += QStringLiteral("0x%1").arg(Params, 0, 16);
  }
  return ParamsStr;
}

bool SynchronizeOptions::MatchesFilter(const QString & FileName) const
{
  if (!UseFilter)
  {
    return true;
  }
  for (const QString & Name : Filter)
  {
    if (Name.compare(FileName, Qt::CaseInsensitive) == 0)
    {
      return true;
    }
  }
  return false;
}

//---------------------------------------------------------------------------
// SynchronizeListingCache

bool SynchronizeListingCache::Get(const QString & Directory, RemoteFileList & Files) const
{
  QMutexLocker Lock(&FMutex);
  auto It = FListings.constFind(Directory);
  if (It == FListings.constEnd())
  {
    return false;
  }
  Files = It.value();
  return true;
}

void SynchronizeListingCache::Put(const QString & Directory, const RemoteFileList & Files)
{
  QMutexLocker Lock(&FMutex);
  FListings.insert(Directory, Files);
}

void SynchronizeListingCache::Invalidate(const QString & Directory)
{
  QMutexLocker Lock(&FMutex);
  QString Prefix = Directory.endsWith(QLatin1Char('/')) ? Directory : Directory + QLatin1Char('/');
  for (auto It = FListings.begin(); It != FListings.end(); )
  {
    if ((It.key() == Directory) || It.key().startsWith(Prefix))
    {
      It = FListings.erase(It);
    }
    else
    {
      ++It;
    }
  }
}

void SynchronizeListingCache::Clear()
{
  QMutexLocker Lock(&FMutex);
  FListings.clear();
}

int SynchronizeListingCache::Count() const
{
  QMutexLocker Lock(&FMutex);
  return FListings.size();
}

//---------------------------------------------------------------------------
// TSynchronizeChecklist

QString SynchronizeChecklist::Item::GetFileName() const
{
  return !Info2.FileName.isEmpty() ? Info2.FileName : Info1.FileName;
}

qint64 SynchronizeChecklist::Item::GetSize(ActionType AAction) const
{
  return IsItemSizeIrrelevant(AAction) ? 0 : GetBaseSize(AAction);
}

qint64 SynchronizeChecklist::Item::GetBaseSize(ActionType AAction) const
{
  switch (AAction)
  {
    case saUploadNew:
    case saUploadUpdate:
    case saDeleteLocal:
      return Info1.Size;

    case saDownloadNew:
    case saDownloadUpdate:
    case saDeleteRemote:
      return Info2.Size;

    default:
      return 0;
  }
}

QString SynchronizeChecklist::Item::GetLocalPath() const
{
  return LocalCombine(Info1.Directory, Info1.FileName);
}

QString SynchronizeChecklist::Item::ForceGetLocalPath() const
{
  return LocalCombine(Info1.Directory, Info1.FileName.isEmpty() ? Info2.FileName : Info1.FileName);
}

QString SynchronizeChecklist::Item::GetRemotePath() const
{
  return UnixPath::Combine(Info2.Directory, Info2.FileName);
}

QString SynchronizeChecklist::Item::ForceGetRemotePath() const
{
  return UnixPath::Combine(Info2.Directory, GetFileName());
}

QString SynchronizeChecklist::Item::GetFilePath() const
{
  switch (Action)
  {
    case saDownloadNew:
    case saDownloadUpdate:
    case saDeleteRemote:
      return GetRemotePath();
    default:
      return GetLocalPath();
  }
}

int SynchronizeChecklist::CheckedCount() const
{
  int Result = 0;
  for (const auto & I : FList)
  {
    if (I->Checked)
    {
      Result++;
    }
  }
  return Result;
}

bool SynchronizeChecklist::GetNextChecked(int & Index, const Item *& AItem) const
{
  while (Index < Count())
  {
    const Item * TheItem = GetItem(Index);
    Index++;
    if (TheItem->Checked)
    {
      AItem = TheItem;
      return true;
    }
  }
  AItem = nullptr;
  return false;
}

SynchronizeChecklist::Item * SynchronizeChecklist::Mutable(const Item * AItem)
{
  for (auto & I : FList)
  {
    if (I.get() == AItem)
    {
      return I.get();
    }
  }
  return nullptr;
}

void SynchronizeChecklist::Update(const Item * AItem, bool Check, ActionType Action)
{
  Item * MutableItem = Mutable(AItem);
  if (MutableItem != nullptr)
  {
    MutableItem->Checked = Check;
    MutableItem->Action = Action;
  }
}

void SynchronizeChecklist::UpdateDirectorySize(const Item * AItem, qint64 Size)
{
  Item * MutableItem = Mutable(AItem);
  if ((MutableItem != nullptr) && MutableItem->IsDirectory)
  {
    MutableItem->FDirectoryHasSize = true;
    if (MutableItem->IsRemoteOnly())
    {
      MutableItem->Info2.Size = Size;
    }
    else if (MutableItem->IsLocalOnly())
    {
      MutableItem->Info1.Size = Size;
    }
  }
}

void SynchronizeChecklist::Delete(const Item * AItem)
{
  auto It = std::find_if(FList.begin(), FList.end(), [AItem](const std::unique_ptr<Item> & I) { return I.get() == AItem; });
  if (It != FList.end())
  {
    FList.erase(It);
  }
}

SynchronizeChecklist::ActionType SynchronizeChecklist::Reverse(ActionType Action)
{
  switch (Action)
  {
    case saUploadNew: return saDeleteLocal;
    case saDownloadNew: return saDeleteRemote;
    case saUploadUpdate: return saDownloadUpdate;
    case saDownloadUpdate: return saUploadUpdate;
    case saDeleteRemote: return saDownloadNew;
    case saDeleteLocal: return saUploadNew;
    default: return saNone;
  }
}

SynchronizeChecklist::ActionType SynchronizeChecklist::OppositeMoveAction(ActionType Action)
{
  switch (Action)
  {
    case saUploadNew: return saDeleteRemote;
    case saDownloadNew: return saDeleteLocal;
    case saDeleteRemote: return saUploadNew;
    case saDeleteLocal: return saDownloadNew;
    default: return saNone;
  }
}

bool SynchronizeChecklist::IsItemSizeIrrelevant(ActionType Action)
{
  switch (Action)
  {
    case saNone:
    case saDeleteRemote:
    case saDeleteLocal:
      return true;
    default:
      return false;
  }
}

int SynchronizeChecklist::Compare(const Item * Item1, const Item * Item2)
{
  if (Item1 == Item2)
  {
    return 0;
  }
  int Result;
  if (!Item1->Info1.Directory.isEmpty())
  {
    Result = (Item1->Info1.Directory == Item2->Info1.Directory) ? 0 :
      Item1->Info1.Directory.compare(Item2->Info1.Directory, Qt::CaseInsensitive);
  }
  else
  {
    Result = (Item1->Info2.Directory == Item2->Info2.Directory) ? 0 :
      Item1->Info2.Directory.compare(Item2->Info2.Directory, Qt::CaseInsensitive);
  }
  if (Result == 0)
  {
    Result = Item1->GetFileName().compare(Item2->GetFileName(), Qt::CaseInsensitive);
  }
  return Result;
}

void SynchronizeChecklist::Sort()
{
  std::stable_sort(FList.begin(), FList.end(),
    [](const std::unique_ptr<Item> & A, const std::unique_ptr<Item> & B) { return Compare(A.get(), B.get()) < 0; });
}

//---------------------------------------------------------------------------

void SynchronizeStatus::SetDirectories(const QString & LocalDirectory, const QString & RemoteDirectory, bool Collect)
{
  QMutexLocker Lock(&FMutex);
  FLocalDirectory = LocalDirectory;
  FRemoteDirectory = RemoteDirectory;
  FCollect = Collect;
}

SynchronizeStatus::Snapshot SynchronizeStatus::Get() const
{
  QMutexLocker Lock(&FMutex);
  Snapshot Result;
  Result.LocalDirectory = FLocalDirectory;
  Result.RemoteDirectory = FRemoteDirectory;
  Result.Collect = FCollect;
  Result.Files = FFiles;
  return Result;
}

//---------------------------------------------------------------------------
// TSynchronizeProgress

qint64 SynchronizeProgress::ItemSize(const SynchronizeChecklist::Item * ChecklistItem) const
{
  switch (ChecklistItem->Action)
  {
    case SynchronizeChecklist::saDeleteRemote:
    case SynchronizeChecklist::saDeleteLocal:
      return ChecklistItem->IsDirectory ? 1024 * 1024 : 100 * 1024;

    default:
      if (ChecklistItem->HasSize())
      {
        return ChecklistItem->GetSize();
      }
      return 1024 * 1024;
  }
}

void SynchronizeProgress::ItemProcessed(const SynchronizeChecklist::Item * ChecklistItem)
{
  FProcessedSize += ItemSize(ChecklistItem);
}

int SynchronizeProgress::Progress(qint64 CurrentTransferred) const
{
  if (FTotalSize < 0)
  {
    FTotalSize = 0;
    int Index = 0;
    const SynchronizeChecklist::Item * ChecklistItem;
    while (FChecklist->GetNextChecked(Index, ChecklistItem))
    {
      FTotalSize += ItemSize(ChecklistItem);
    }
  }
  if (FTotalSize <= 0)
  {
    return 0;
  }
  qint64 Processed = FProcessedSize + CurrentTransferred;
  return static_cast<int>(std::min<qint64>(100, (Processed * 100) / FTotalSize));
}

//---------------------------------------------------------------------------

CopyParam GetSynchronizeCopyParam(const CopyParam & Param, int Params)
{
  CopyParam Result = Param;
  // when synchronizing by time, we force preserving time,
  // otherwise it does not make any sense
  if ((Params & spNotByTime) == 0)
  {
    Result.PreserveTime = true;
  }
  // GetSynchronizeCopyParams: cpNoConfirmation
  Result.ConfirmOverwriting = false;
  Result.NewerOnly = false;
  return Result;
}

//---------------------------------------------------------------------------
// Collect (runs on the worker thread)

class SynchronizeCollector
{
public:
  SynchronizeCollector(Terminal * ATerm, FileOperationProgress & AProgress, SynchronizeMode AMode,
    const CopyParam & AParam, int AParams, const SynchronizeOptions & AOptions, SynchronizeStatus * AStatus,
    bool AFollowDirectorySymlinks, SynchronizeChecklist * AChecklist) :
    FTerm(ATerm), FProgress(AProgress), FMode(AMode), FParam(AParam), FParams(AParams), FOptions(AOptions),
    FStatus(AStatus), FFollowDirectorySymlinks(AFollowDirectorySymlinks), FChecklist(AChecklist)
  {
    FParam.TrimVMSVersions = ATerm->Data().TrimVMSVersions;
  }

  void CollectDirectory(const QString & Directory1, const QString & Directory2, bool FirstLevel);

private:
  struct LeftFile
  {
    bool Modified = false;
    bool New = true;
    bool IsDirectory = false;
    SynchronizeChecklist::FileInfo Info;
    SynchronizeChecklist::FileInfo MatchingFile2;
    bool HasMatchingRemoteFile2 = false;
    RemoteFile MatchingRemoteFile2;
    QDateTime LocalLastWriteTime;
  };

  // SameFileChecksum is deferred until all files of the directory are known,
  // so that the server calculates the checksums of the directory at once
  struct PendingChecksum
  {
    std::unique_ptr<SynchronizeChecklist::Item> ChecklistItem;
    size_t LeftIndex = 0;
    QString FullLeftFileName;
    QString FullRightFileName;
    RemoteFile RightFile;
  };

  struct Level
  {
    QString Directory1;
    QString Directory2;
    bool FirstLevel = false;
    std::vector<LeftFile> LeftFiles;
    QHash<QString, size_t> LeftIndex;
    std::vector<PendingChecksum> Checksums;
  };

  // The session's file system: replaced when a lost connection is reopened
  // while reading a directory (TTerminal::CustomReadDirectory)
  Terminal * FTerm;
  FileSystem & Fs() { return FTerm->WorkerFileSystem(); }
  FileOperationProgress & FProgress;
  SynchronizeMode FMode;
  CopyParam FParam;
  int FParams;
  const SynchronizeOptions & FOptions;
  SynchronizeStatus * FStatus;
  bool FFollowDirectorySymlinks;
  SynchronizeChecklist * FChecklist;
  // Checksum command of the shell that works (index to ShellChecksumCommands),
  // -1 = not known yet, -2 = none (download the files instead)
  int FShellChecksumCommand = -1;

  bool Flag(int F) const { return (FParams & F) != 0; }
  void CheckCancel() const
  {
    if ((FStatus != nullptr) && FStatus->Cancelled())
    {
      throw AbortException();
    }
    FProgress.CheckCancel();
  }
  QString Key(const QString & FileName) const
  {
#ifdef Q_OS_MACOS
    // both Unicode forms of a name are the same file on the disk of macOS, where
    // a name often has the decomposed one, and a server the composed one
    QString Name = FileName.normalized(QString::NormalizationForm_C);
#else
    const QString & Name = FileName;
#endif
    return Flag(spCaseSensitive) ? Name : Name.toLower();
  }
  bool MatchesFilter(const Level & Data, const QString & FileName) const
  {
    return !Data.FirstLevel || FOptions.MatchesFilter(FileName);
  }
  LeftFile * MatchLeftFile(Level & Data, const QString & FileName)
  {
    auto It = Data.LeftIndex.constFind(Key(FileName));
    if (It == Data.LeftIndex.constEnd())
    {
      return nullptr;
    }
    LeftFile * Result = &Data.LeftFiles[It.value()];
    Result->New = false;
    return Result;
  }
  bool CanRecurseToDirectory(const RemoteFile & File) const
  {
    return !File.IsSymLink || FFollowDirectorySymlinks;
  }
  bool AllowLocalFileTransfer(const QFileInfo & Info, bool DisallowTemporaryTransferFiles);
  bool AllowRemoteFileTransfer(const QString & FullFileName, const RemoteFile & File, bool DisallowTemporaryTransferFiles);
  bool IsEmptyLocalDirectory(const QString & Path, int Depth);
  bool IsEmptyRemoteDirectory(const QString & Path, int Depth);
  bool DownloadSameFileChecksum(const QString & LeftFileName, const QString & RightFileName);
  QHash<QString, QString> RemoteChecksums(const QStringList & Paths, QString & Alg);
  void ResolveChecksums(Level & Data);
  RemoteFileList ReadRemoteDirectory(const QString & Directory);
  void CollectFile(Level & Data, const RemoteFile & File);
  void FileCheckModified(Level & Data, std::unique_ptr<SynchronizeChecklist::Item> & ChecklistItem,
    const QString & FullLeftFileName, LeftFile * LocalData, const QString & FullRightFileName, const RemoteFile & RightFile);
  void FileNewOrModified(std::unique_ptr<SynchronizeChecklist::Item> & ChecklistItem, const RemoteFile * File, bool New);
  bool FilesMatch(const LeftFile * LeftItem, SynchronizeChecklist::Item * RightItem);
};

bool SynchronizeCollector::IsEmptyLocalDirectory(const QString & Path, int Depth)
{
  if (Depth > 64)
  {
    return false;
  }
  QDir Dir(Path);
  const QFileInfoList Entries = Dir.entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
  for (const QFileInfo & Info : Entries)
  {
    if (!Info.exists())
    {
      continue;
    }
    if (FParam.AllowTransfer(Info.filePath(), true, Info.isDir(), MaskParams(Info.isDir() ? 0 : Info.size(), Info.lastModified()),
          Info.fileName().startsWith(QLatin1Char('.'))) &&
        !TemporaryTransferFile(Info.fileName()))
    {
      if (!Info.isDir() || !IsEmptyLocalDirectory(Info.filePath(), Depth + 1))
      {
        return false;
      }
    }
  }
  return true;
}

bool SynchronizeCollector::IsEmptyRemoteDirectory(const QString & Path, int Depth)
{
  if (Depth > 64)
  {
    return false;
  }
  try
  {
    for (const RemoteFile & File : FTerm->ReadDirectoryOnWorker(Path))
    {
      if (File.IsParentDirectory || !IsValidListingName(File.FileName))
      {
        continue;
      }
      QString FullFileName = UnixPath::Combine(Path, File.FileName);
      if (FParam.AllowTransfer(FullFileName, false, File.IsDirectory, MaskParams(File.Size, File.Modification), File.IsHidden) &&
          !TemporaryTransferFile(File.FileName))
      {
        if (!File.IsDirectory ||
            (CanRecurseToDirectory(File) && !IsEmptyRemoteDirectory(FullFileName, Depth + 1)))
        {
          return false;
        }
      }
    }
  }
  catch (const FatalException &)
  {
    throw;
  }
  catch (const ExtException &)
  {
    // csIgnoreErrors
    return false;
  }
  return true;
}

bool SynchronizeCollector::AllowLocalFileTransfer(const QFileInfo & Info, bool DisallowTemporaryTransferFiles)
{
  // TTerminal::DoAllowLocalFileTransfer
  bool Directory = Info.isDir();
  return
    FParam.AllowTransfer(Info.filePath(), true, Directory, MaskParams(Directory ? 0 : Info.size(), Info.lastModified()),
      Info.fileName().startsWith(QLatin1Char('.'))) &&
    (!DisallowTemporaryTransferFiles || !TemporaryTransferFile(Info.fileName())) &&
    (!Directory || !FParam.ExcludeEmptyDirectories || !IsEmptyLocalDirectory(Info.filePath(), 0));
}

bool SynchronizeCollector::AllowRemoteFileTransfer(const QString & FullFileName, const RemoteFile & File, bool DisallowTemporaryTransferFiles)
{
  // TTerminal::DoAllowRemoteFileTransfer
  return
    FParam.AllowTransfer(FullFileName, false, File.IsDirectory, MaskParams(File.Size, File.Modification), File.IsHidden) &&
    (!DisallowTemporaryTransferFiles || !TemporaryTransferFile(File.FileName)) &&
    (!File.IsDirectory || !FParam.ExcludeEmptyDirectories ||
     !CanRecurseToDirectory(File) || !IsEmptyRemoteDirectory(FullFileName, 0));
}

bool SynchronizeCollector::DownloadSameFileChecksum(const QString & LeftFileName, const QString & RightFileName)
{
  // The server cannot calculate checksums (no FTP HASH/XSHA*, no shell):
  // the remote file is downloaded and hashed locally
  QTemporaryDir Temp;
  if (!Temp.isValid())
  {
    throw ExtException(QStringLiteral("Error calculating checksum for file '%1'.").arg(RightFileName), Temp.errorString());
  }
  Platform::MakeDirectoryPrivate(Temp.path());
  QString TempFileName = Temp.filePath(QStringLiteral("checksum"));
  Fs().Download(RightFileName, TempFileName, FProgress);
  QString Alg = QStringLiteral("sha-256");
  return (LocalFileChecksum(TempFileName, Alg) == LocalFileChecksum(LeftFileName, Alg));
}

namespace
{
struct ShellChecksumCommand
{
  const char * Command;
  const char * Alg;
};
// SynchronizationChecksumAlgs default "sha256,sha1"
const ShellChecksumCommand ShellChecksumCommands[] = {
  { "sha256sum", "sha-256" },
  { "shasum -a 256", "sha-256" },
  { "sha1sum", "sha-1" },
};
const int ShellChecksumBatch = 64;
}

QHash<QString, QString> SynchronizeCollector::RemoteChecksums(const QStringList & Paths, QString & Alg)
{
  // TTerminal::SameFileChecksum + CalculateFilesChecksum, for all files of a directory
  QHash<QString, QString> Result;
  Alg.clear();
  if (auto * Ftp = dynamic_cast<FtpFileSystem *>(&Fs()); (Ftp != nullptr) && Ftp->CanCalculateChecksum())
  {
    const QStringList Supported = Ftp->ChecksumAlgorithms();
    for (const QString & A : {QStringLiteral("sha-256"), QStringLiteral("sha-1")})
    {
      if (Supported.contains(A, Qt::CaseInsensitive))
      {
        Alg = A;
        break;
      }
    }
    if (!Alg.isEmpty())
    {
      for (const QString & Path : Paths)
      {
        CheckCancel();
        FProgress.SetFile(Path, 0);
        try
        {
          Result.insert(Path, Ftp->CalculateFileChecksum(Path, Alg).toLower());
        }
        catch (const AbortException &)
        {
          throw;
        }
        catch (const FatalException &)
        {
          throw;
        }
        catch (const ExtException &)
        {
          // downloaded instead
        }
      }
      return Result;
    }
  }

  auto * Shell = dynamic_cast<AnyCommandFileSystem *>(&Fs());
  if ((Shell != nullptr) && (FShellChecksumCommand != -2))
  {
    int First = std::max(FShellChecksumCommand, 0);
    int Last = (FShellChecksumCommand >= 0) ? FShellChecksumCommand : static_cast<int>(std::size(ShellChecksumCommands)) - 1;
    for (int Index = 0; (Index < Paths.size()) && (FShellChecksumCommand != -2); Index += ShellChecksumBatch)
    {
      CheckCancel();
      QString Arguments;
      for (const QString & Path : Paths.mid(Index, ShellChecksumBatch))
      {
        Arguments += QLatin1Char(' ') + ShellQuoteStr(Path);
      }
      FProgress.SetFile(Paths.at(Index), 0);
      bool Done = false;
      for (int CommandIndex = First; !Done && (CommandIndex <= Last); CommandIndex++)
      {
        const ShellChecksumCommand & Command = ShellChecksumCommands[CommandIndex];
        QStringList Lines;
        int ExitCode;
        try
        {
          ExitCode = Shell->AnyCommand(
            QStringLiteral("%1 --%2 2>/dev/null").arg(QString::fromLatin1(Command.Command), Arguments), QString(),
            [&Lines](const QString & Line, bool StdError) { if (!StdError) Lines.append(Line); },
            FProgress, nullptr);
        }
        catch (const AbortException &)
        {
          throw;
        }
        catch (const FatalException &)
        {
          throw;
        }
        catch (const ExtException &)
        {
          // no shell access (e.g. SFTP only account)
          FShellChecksumCommand = -2;
          break;
        }
        // 126/127: the command does not exist or cannot be executed
        if ((ExitCode == 126) || (ExitCode == 127))
        {
          continue;
        }
        QHash<QString, QString> Parsed = ParseChecksumOutput(Lines);
        if (Parsed.isEmpty() && (ExitCode != 0))
        {
          continue;
        }
        for (auto It = Parsed.cbegin(); It != Parsed.cend(); ++It)
        {
          Result.insert(It.key(), It.value());
        }
        FShellChecksumCommand = CommandIndex;
        First = Last = CommandIndex;
        Alg = QString::fromLatin1(Command.Alg);
        Done = true;
      }
      if (!Done && (FShellChecksumCommand != -2) && (FShellChecksumCommand < 0))
      {
        FShellChecksumCommand = -2;
      }
    }
    if (!Alg.isEmpty())
    {
      return Result;
    }
  }
  return QHash<QString, QString>();
}

void SynchronizeCollector::ResolveChecksums(Level & Data)
{
  if (Data.Checksums.empty())
  {
    return;
  }
  QStringList Paths;
  for (const PendingChecksum & Pending : Data.Checksums)
  {
    Paths.append(Pending.FullRightFileName);
  }
  QString Alg;
  QHash<QString, QString> Remote = RemoteChecksums(Paths, Alg);
  for (PendingChecksum & Pending : Data.Checksums)
  {
    CheckCancel();
    QString RightChecksum = Remote.value(Pending.FullRightFileName);
    bool Same;
    if (!RightChecksum.isEmpty())
    {
      FProgress.SetFile(Pending.FullLeftFileName, 0);
      Same = (RightChecksum.compare(LocalFileChecksum(Pending.FullLeftFileName, Alg), Qt::CaseInsensitive) == 0);
    }
    else
    {
      Same = DownloadSameFileChecksum(Pending.FullLeftFileName, Pending.FullRightFileName);
    }
    if (!Same)
    {
      // Modified = LeftModified = true
      LeftFile * LocalData = &Data.LeftFiles[Pending.LeftIndex];
      LocalData->Modified = true;
      LocalData->MatchingFile2 = Pending.ChecklistItem->Info2;
      LocalData->HasMatchingRemoteFile2 = true;
      LocalData->MatchingRemoteFile2 = Pending.RightFile;
      FileNewOrModified(Pending.ChecklistItem, &Pending.RightFile, false);
    }
  }
  Data.Checksums.clear();
}

RemoteFileList SynchronizeCollector::ReadRemoteDirectory(const QString & Directory)
{
  // spUseCache: TTerminal::DirectoryFileList with the directory cache
  bool UseCache = Flag(spUseCache) && FOptions.Cache;
  RemoteFileList Result;
  if (UseCache && FOptions.Cache->Get(Directory, Result))
  {
    return Result;
  }
  Result = FTerm->ReadDirectoryOnWorker(Directory);
  if (UseCache)
  {
    FOptions.Cache->Put(Directory, Result);
  }
  return Result;
}

void SynchronizeCollector::CollectDirectory(const QString & Directory1, const QString & Directory2, bool FirstLevel)
{
  // TTerminal::DoSynchronizeCollectDirectory
  CheckCancel();
  Level Data;
  Data.Directory1 = Directory1;
  Data.Directory2 = Directory2;
  Data.FirstLevel = FirstLevel;

  if (FStatus != nullptr)
  {
    FStatus->SetDirectories(Directory1, Directory2, true);
  }
  FProgress.SetFile(Directory1, 0);

  const QFileInfoList LocalEntries = ListLocalDirectory(Directory1);
  for (const QFileInfo & Info : LocalEntries)
  {
    CheckCancel();
    if (!Info.exists())
    {
      continue; // broken symlink
    }
    QString FileName = Info.fileName();
    QString RemoteFileName = CopyParam::RestoreChars(FParam.ChangeFileName(FileName));
    if (AllowLocalFileTransfer(Info, true) &&
        (MatchesFilter(Data, FileName) || MatchesFilter(Data, RemoteFileName)))
    {
      LeftFile FileData;
      FileData.IsDirectory = Info.isDir();
      FileData.Info.FileName = FileName;
      FileData.Info.Directory = Directory1;
      FileData.Info.Modification = Info.lastModified();
      FileData.Info.Size = FileData.IsDirectory ? 0 : Info.size();
      FileData.LocalLastWriteTime = Info.lastModified();
      QString K = Key(FileName);
      if (!Data.LeftIndex.contains(K))
      {
        Data.LeftIndex.insert(K, Data.LeftFiles.size());
        Data.LeftFiles.push_back(FileData);
      }
    }
  }

  // ProcessDirectory(Data.Directory2, SynchronizeCollectFile)
  const RemoteFileList RemoteFiles = ReadRemoteDirectory(Directory2);
  for (const RemoteFile & File : RemoteFiles)
  {
    CheckCancel();
    if (File.IsParentDirectory || !IsValidListingName(File.FileName))
    {
      continue;
    }
    CollectFile(Data, File);
  }
  ResolveChecksums(Data);

  for (LeftFile & FileData : Data.LeftFiles)
  {
    // add local file either if we are going to upload it
    // (i.e. if it is updated or we want to upload even new files)
    // or if we are going to delete it (i.e. all "new"=obsolete files)
    bool Modified = (FileData.Modified && ((FMode == smBoth) || (FMode == smRemote)));
    bool New = (FileData.New &&
      ((FMode == smLocal) ||
       (((FMode == smBoth) || (FMode == smRemote)) && !Flag(spTimestamp))));

    if (Modified || New)
    {
      auto ChecklistItem = std::make_unique<SynchronizeChecklist::Item>();
      ChecklistItem->IsDirectory = FileData.IsDirectory;
      ChecklistItem->Info1 = FileData.Info;
      ChecklistItem->FLocalLastWriteTime = FileData.LocalLastWriteTime;

      if (Modified)
      {
        ChecklistItem->Info2 = FileData.MatchingFile2;
        ChecklistItem->HasRemoteFile = FileData.HasMatchingRemoteFile2;
        ChecklistItem->Remote = FileData.MatchingRemoteFile2;
      }
      else
      {
        ChecklistItem->Info2.Directory = Directory2;
      }

      if ((FMode == smBoth) || (FMode == smRemote))
      {
        ChecklistItem->Action =
          (Modified ? SynchronizeChecklist::saUploadUpdate : SynchronizeChecklist::saUploadNew);
        ChecklistItem->Checked =
          (Modified || !Flag(spExistingOnly)) &&
          (!ChecklistItem->IsDirectory || !Flag(spNoRecurse) || Flag(spSubDirs));
      }
      else if ((FMode == smLocal) && !Flag(spTimestamp))
      {
        ChecklistItem->Action = SynchronizeChecklist::saDeleteLocal;
        ChecklistItem->Checked =
          Flag(spDelete) &&
          (!ChecklistItem->IsDirectory || !Flag(spNoRecurse) || Flag(spSubDirs));
      }

      if (ChecklistItem->Action != SynchronizeChecklist::saNone)
      {
        FChecklist->Add(std::move(ChecklistItem));
      }
    }
  }
}

bool SynchronizeCollector::FilesMatch(const LeftFile * LeftItem, SynchronizeChecklist::Item * RightItem)
{
  // TTerminal::DoFilesMatch
  bool Result = (LeftItem->IsDirectory == RightItem->IsDirectory);
  if (Result && !RightItem->IsDirectory)
  {
    RightItem->Info1 = LeftItem->Info;
  }
  return Result;
}

void SynchronizeCollector::CollectFile(Level & Data, const RemoteFile & File)
{
  // TTerminal::DoSynchronizeCollectFile
  if (FStatus != nullptr)
  {
    FStatus->IncFiles();
  }

  QString LocalFileName = CopyParam::ValidLocalFileName(FParam.ChangeFileName(File.FileName));
  QString FullRemoteFileName = UnixPath::Combine(Data.Directory2, File.FileName);
  if (AllowRemoteFileTransfer(FullRemoteFileName, File, true) &&
      (MatchesFilter(Data, File.FileName) || MatchesFilter(Data, LocalFileName)))
  {
    auto ChecklistItem = std::make_unique<SynchronizeChecklist::Item>();
    ChecklistItem->IsDirectory = File.IsDirectory;
    ChecklistItem->Info2.FileName = File.FileName;
    ChecklistItem->Info2.Directory = Data.Directory2;
    ChecklistItem->Info2.Modification = File.Modification;
    ChecklistItem->Info2.Size = File.IsDirectory ? 0 : File.Size;

    if (File.IsDirectory && !CanRecurseToDirectory(File))
    {
      // Skipping symlink to directory
    }
    else
    {
      LeftFile * LocalData = MatchLeftFile(Data, LocalFileName);
      if ((LocalData != nullptr) && FilesMatch(LocalData, ChecklistItem.get()))
      {
        QString FullLocalFileName = LocalCombine(LocalData->Info.Directory, LocalData->Info.FileName);
        if (!ChecklistItem->IsDirectory)
        {
          // TTerminal::DoSynchronizeCollectFile: as precise as the remote listing is
          ChecklistItem->Info1.Modification = ReduceDateTimePrecision(ChecklistItem->Info1.Modification, File.ModificationFmt);
          FileCheckModified(Data, ChecklistItem, FullLocalFileName, LocalData, FullRemoteFileName, File);
        }
        else if (!Flag(spNoRecurse))
        {
          CollectDirectory(FullLocalFileName, FullRemoteFileName, false);
        }
      }
      else if (LocalData == nullptr)
      {
        // SynchronizedFileNew
        ChecklistItem->Info1.Directory = Data.Directory1;
        FileNewOrModified(ChecklistItem, &File, true);
      }
    }
  }
}

void SynchronizeCollector::FileCheckModified(Level & Data, std::unique_ptr<SynchronizeChecklist::Item> & ChecklistItem,
  const QString & FullLeftFileName, LeftFile * LocalData, const QString & FullRightFileName, const RemoteFile & RightFile)
{
  // TTerminal::SynchronizedFileCheckModified
  bool Modified = false;
  bool LeftModified = false;
  // for spTimestamp+spBySize require that the file sizes are the same
  // before comparing file time
  int TimeCompare;
  if (!Flag(spNotByTime) &&
      (!Flag(spTimestamp) || !Flag(spBySize) ||
       (ChecklistItem->Info1.Size == ChecklistItem->Info2.Size)))
  {
    TimeCompare = CompareFileTime(ChecklistItem->Info1.Modification, ChecklistItem->Info2.Modification);
  }
  else
  {
    TimeCompare = 0;
  }
  if (TimeCompare < 0)
  {
    if ((!Flag(spTimestamp) && !Flag(spMirror)) ||
        (FMode == smBoth) || (FMode == smLocal))
    {
      Modified = true;
    }
    else
    {
      LeftModified = true;
    }
  }
  else if (TimeCompare > 0)
  {
    if ((!Flag(spTimestamp) && !Flag(spMirror)) ||
        (FMode == smBoth) || (FMode == smRemote))
    {
      LeftModified = true;
    }
    else
    {
      Modified = true;
    }
  }
  else if (Flag(spBySize) &&
           (ChecklistItem->Info1.Size != ChecklistItem->Info2.Size) &&
           !Flag(spTimestamp))
  {
    Modified = true;
    LeftModified = true;
  }
  else if (Flag(spByChecksum) && !Flag(spTimestamp))
  {
    // !SameFileChecksum => Modified = LeftModified = true (ResolveChecksums)
    PendingChecksum Pending;
    Pending.LeftIndex = static_cast<size_t>(LocalData - Data.LeftFiles.data());
    Pending.FullLeftFileName = FullLeftFileName;
    Pending.FullRightFileName = FullRightFileName;
    Pending.RightFile = RightFile;
    Pending.ChecklistItem = std::move(ChecklistItem);
    Data.Checksums.push_back(std::move(Pending));
    return;
  }

  if (LeftModified)
  {
    LocalData->Modified = true;
    LocalData->MatchingFile2 = ChecklistItem->Info2;
    LocalData->HasMatchingRemoteFile2 = true;
    LocalData->MatchingRemoteFile2 = RightFile;
  }

  if (Modified)
  {
    FileNewOrModified(ChecklistItem, &RightFile, false);
  }
}

void SynchronizeCollector::FileNewOrModified(std::unique_ptr<SynchronizeChecklist::Item> & ChecklistItem, const RemoteFile * File, bool New)
{
  // TTerminal::SynchronizedFileNewOrModified
  // download the file if it changed or is new and we want to have it locally
  if ((FMode == smBoth) || (FMode == smLocal))
  {
    if (!Flag(spTimestamp) || !New)
    {
      ChecklistItem->Action =
        (!New ? SynchronizeChecklist::saDownloadUpdate : SynchronizeChecklist::saDownloadNew);
      ChecklistItem->Checked =
        (!New || !Flag(spExistingOnly)) &&
        (!ChecklistItem->IsDirectory || !Flag(spNoRecurse) || Flag(spSubDirs));
    }
  }
  else if ((FMode == smRemote) && New)
  {
    if (!Flag(spTimestamp))
    {
      ChecklistItem->Action = SynchronizeChecklist::saDeleteRemote;
      ChecklistItem->Checked =
        Flag(spDelete) &&
        (!ChecklistItem->IsDirectory || !Flag(spNoRecurse) || Flag(spSubDirs));
    }
  }

  if (ChecklistItem->Action != SynchronizeChecklist::saNone)
  {
    if (File != nullptr)
    {
      ChecklistItem->HasRemoteFile = true;
      ChecklistItem->Remote = *File;
    }
    FChecklist->Add(std::move(ChecklistItem));
  }
}

void SynchronizeCollect(Terminal * Term, const QString & LocalDirectory, const QString & RemoteDirectory,
  SynchronizeMode Mode, const CopyParam & Param, int Params, const SynchronizeOptions & Options,
  SynchronizeStatusPtr Status, SynchronizeCollectCallback Done)
{
  auto Checklist = std::make_shared<SynchronizeChecklist>();
  QString Directory1 = NormalizeLocalDirectory(LocalDirectory);
  QString Directory2 = NormalizeRemoteDirectory(RemoteDirectory);
  bool FollowDirectorySymlinks = Term->Data().FollowDirectorySymlinks;
  Term->Progress().Start(FileOperationProgress::Calculate, FileOperationProgress::ToRemote, 0);
  Term->RunOnWorker(
    [Term, Checklist, Directory1, Directory2, Mode, Param, Params, Options, Status, FollowDirectorySymlinks]
    (FileSystem &, FileOperationProgress & Progress, UserInterface &)
    {
      SynchronizeCollector Collector(Term, Progress, Mode, Param, Params, Options, Status.get(), FollowDirectorySymlinks, Checklist.get());
      Collector.CollectDirectory(Directory1, Directory2, true);
      Checklist->Sort();
    },
    [Checklist, Done](const OperationResult & Result)
    {
      if (Done)
      {
        Done(Result, Result.Ok ? Checklist : SynchronizeChecklistPtr());
      }
    });
}

//---------------------------------------------------------------------------
// Apply

namespace
{

class SynchronizeApplier : public std::enable_shared_from_this<SynchronizeApplier>
{
public:
  QPointer<Terminal> Term;
  SynchronizeChecklistPtr Checklist;
  CopyParam SyncCopyParam;
  int Params = 0;
  SynchronizeStatusPtr Status;
  SynchronizeProcessedItemEvent OnProcessedItem;
  SynchronizeApplyCallback Done;
  SynchronizeStatistics Statistics;
  int Index = 0;
  bool DeleteLocalToTrash = false;

  void Next();

private:
  void Finish(const OperationResult & Result);
  void Processed(const std::vector<const SynchronizeChecklist::Item *> & Items);
  void ApplyTimestamps();
  void DeleteLocal(const std::vector<const SynchronizeChecklist::Item *> & Items);
};

void SynchronizeApplier::Finish(const OperationResult & Result)
{
  SynchronizeApplyCallback ADone = std::move(Done);
  Done = nullptr;
  if (ADone)
  {
    ADone(Result, Statistics);
  }
}

void SynchronizeApplier::Processed(const std::vector<const SynchronizeChecklist::Item *> & Items)
{
  if (OnProcessedItem)
  {
    for (const SynchronizeChecklist::Item * ChecklistItem : Items)
    {
      OnProcessedItem(ChecklistItem);
    }
  }
}

static bool Cancelled(const SynchronizeStatusPtr & Status, Terminal * Term)
{
  return (Status && Status->Cancelled()) || Term->Progress().Cancelled();
}

void SynchronizeApplier::ApplyTimestamps()
{
  // SynchronizeLocalTimestamp / SynchronizeRemoteTimestamp for all items in one job
  auto Items = std::make_shared<std::vector<const SynchronizeChecklist::Item *>>();
  int I = 0;
  const SynchronizeChecklist::Item * ChecklistItem;
  while (Checklist->GetNextChecked(I, ChecklistItem))
  {
    Items->push_back(ChecklistItem);
  }
  Index = Checklist->Count();
  auto Self = shared_from_this();
  SynchronizeChecklistPtr AChecklist = Checklist;
  SynchronizeStatusPtr AStatus = Status;
  Term->Progress().Start(FileOperationProgress::SetProperties, FileOperationProgress::ToRemote, static_cast<int>(Items->size()));
  Term->RunOnWorker([Items, AChecklist, AStatus](FileSystem & Fs, FileOperationProgress & Progress, UserInterface &)
  {
    for (const SynchronizeChecklist::Item * Item : *Items)
    {
      if (AStatus && AStatus->Cancelled())
      {
        throw AbortException();
      }
      Progress.CheckCancel();
      if (AStatus)
      {
        AStatus->SetDirectories(Item->Info1.Directory, Item->Info2.Directory, false);
      }
      switch (Item->Action)
      {
        case SynchronizeChecklist::saDownloadUpdate:
          Progress.SetFile(Item->GetRemotePath(), 0);
          SetLocalModificationTime(Item->GetLocalPath(), Item->Info2.Modification);
          break;

        case SynchronizeChecklist::saUploadUpdate:
          Progress.SetFile(Item->GetLocalPath(), 0);
          try
          {
            SessionAction TouchAction(laTouch, Item->GetRemotePath());
            TouchAction.Modification(Item->LocalLastWriteTime());
            Fs.SetModificationTime(Item->GetRemotePath(), Item->LocalLastWriteTime());
            TouchAction.Commit();
          }
          catch (const FatalException &)
          {
            throw;
          }
          catch (const ExtException & E)
          {
            throw ExtException(QStringLiteral("Can't set attributes of file '%1'.").arg(Item->GetRemotePath()),
              E.MoreMessages().isEmpty() ? E.Message() : E.MoreMessages());
          }
          break;

        default:
          break;
      }
      Progress.FileFinished();
    }
  },
  [Self, Items](const OperationResult & Result)
  {
    if (Result.Ok)
    {
      Self->Processed(*Items);
    }
    Self->Finish(Result);
  });
}

static bool RemoveLocal(const QString & Path, bool ToTrash)
{
  QFileInfo Info(Path);
  if (ToTrash && (Info.exists() || Info.isSymLink()))
  {
    // TTerminalManager::TerminalDeleteLocalFile with DeleteToRecycleBin
    return LocalFileOperations::MoveToTrash(Path);
  }
  if (Info.isDir() && !Info.isSymLink())
  {
    return QDir(Path).removeRecursively();
  }
  return QFile::remove(Path) || (!Info.exists() && !Info.isSymLink());
}

void SynchronizeApplier::DeleteLocal(const std::vector<const SynchronizeChecklist::Item *> & Items)
{
  // TTerminal::DeleteLocalFiles (OnDeleteLocalFile: to the trash with DeleteToRecycleBin)
  QStringList Paths;
  for (const SynchronizeChecklist::Item * Item : Items)
  {
    Paths.append(Item->GetLocalPath());
  }
  auto Count = std::make_shared<int>(0);
  auto Self = shared_from_this();
  Term->Progress().Start(FileOperationProgress::Delete, FileOperationProgress::ToLocal, Paths.size());
  bool ToTrash = DeleteLocalToTrash;
  Term->RunOnWorker([Paths, Count, ToTrash](FileSystem &, FileOperationProgress & Progress, UserInterface &)
  {
    for (const QString & Path : Paths)
    {
      Progress.CheckCancel();
      Progress.SetFile(Path, 0);
      if (!RemoveLocal(Path, ToTrash))
      {
        throw ExtException(QStringLiteral("Error deleting file '%1'.").arg(Path));
      }
      (*Count)++;
      Progress.FileFinished();
    }
  },
  [Self, Items, Count](const OperationResult & Result)
  {
    Self->Statistics.FilesDeletedLocal += *Count;
    if (!Result.Ok)
    {
      Self->Finish(Result);
      return;
    }
    Self->Processed(Items);
    Self->Next();
  });
}

void SynchronizeApplier::Next()
{
  if (Term.isNull())
  {
    return;
  }
  if (Cancelled(Status, Term))
  {
    OperationResult Result;
    Result.Ok = false;
    Result.Aborted = true;
    Finish(Result);
    return;
  }

  if ((Params & spTimestamp) != 0)
  {
    if (Index == 0)
    {
      ApplyTimestamps();
    }
    else
    {
      Finish(OperationResult());
    }
    return;
  }

  // Next group of consecutive checked items with the same action and directories
  std::vector<const SynchronizeChecklist::Item *> Items;
  const SynchronizeChecklist::Item * First = nullptr;
  if (!Checklist->GetNextChecked(Index, First))
  {
    Finish(OperationResult());
    return;
  }
  Items.push_back(First);
  int I = Index;
  const SynchronizeChecklist::Item * ChecklistItem;
  while (Checklist->GetNextChecked(I, ChecklistItem) &&
         (ChecklistItem->Action == First->Action) &&
         (ChecklistItem->Info1.Directory == First->Info1.Directory) &&
         (ChecklistItem->Info2.Directory == First->Info2.Directory))
  {
    Items.push_back(ChecklistItem);
    Index = I;
  }

  if (Status)
  {
    Status->SetDirectories(First->GetLocalTarget(), First->GetRemoteTarget(), false);
  }

  QStringList Paths;
  for (const SynchronizeChecklist::Item * Item : Items)
  {
    Paths.append(Item->GetFilePath());
  }

  auto Self = shared_from_this();
  SynchronizeChecklist::ActionType Action = First->Action;
  Terminal::Callback Callback = [Self, Items, Action](const OperationResult & Result)
  {
    if (!Self->Term.isNull())
    {
      FileOperationProgress::Snapshot S = Self->Term->Progress().Get();
      switch (Action)
      {
        case SynchronizeChecklist::saUploadNew:
        case SynchronizeChecklist::saUploadUpdate:
          Self->Statistics.FilesUploaded += S.FilesFinished;
          Self->Statistics.TotalUploaded += S.TotalTransferred;
          break;
        case SynchronizeChecklist::saDownloadNew:
        case SynchronizeChecklist::saDownloadUpdate:
          Self->Statistics.FilesDownloaded += S.FilesFinished;
          Self->Statistics.TotalDownloaded += S.TotalTransferred;
          break;
        case SynchronizeChecklist::saDeleteRemote:
          Self->Statistics.FilesDeletedRemote += S.FilesFinished;
          break;
        default:
          break;
      }
    }
    if (!Result.Ok)
    {
      Self->Finish(Result);
      return;
    }
    Self->Processed(Items);
    Self->Next();
  };

  switch (Action)
  {
    case SynchronizeChecklist::saDownloadNew:
    case SynchronizeChecklist::saDownloadUpdate:
      if (!QDir().mkpath(First->GetLocalTarget()))
      {
        OperationResult Result;
        Result.Ok = false;
        Result.Message = QStringLiteral("Error creating folder '%1'.").arg(First->GetLocalTarget());
        Finish(Result);
        return;
      }
      Term->CopyToLocal(Paths, First->GetLocalTarget(), SyncCopyParam, false, Callback);
      break;

    case SynchronizeChecklist::saUploadNew:
    case SynchronizeChecklist::saUploadUpdate:
      Term->CopyToRemote(Paths, First->GetRemoteTarget(), SyncCopyParam, false, Callback);
      break;

    case SynchronizeChecklist::saDeleteRemote:
      Term->DeleteFiles(Paths, Callback);
      break;

    case SynchronizeChecklist::saDeleteLocal:
      DeleteLocal(Items);
      break;

    default:
      Next();
      break;
  }
}

}

void SynchronizeApply(Terminal * Term, SynchronizeChecklistPtr Checklist, const CopyParam & Param, int Params,
  SynchronizeStatusPtr Status, SynchronizeProcessedItemEvent OnProcessedItem, SynchronizeApplyCallback Done,
  bool DeleteLocalToTrash)
{
  auto Applier = std::make_shared<SynchronizeApplier>();
  Applier->DeleteLocalToTrash = DeleteLocalToTrash;
  Applier->Term = Term;
  Applier->Checklist = std::move(Checklist);
  Applier->SyncCopyParam = GetSynchronizeCopyParam(Param, Params);
  Applier->Params = Params;
  Applier->Status = std::move(Status);
  Applier->OnProcessedItem = std::move(OnProcessedItem);
  Applier->Done = std::move(Done);
  // Asynchronously, so the caller always gets its callback after returning
  QMetaObject::invokeMethod(Term, [Applier]() { Applier->Next(); }, Qt::QueuedConnection);
}

//---------------------------------------------------------------------------
// Start in Background

int SynchronizeToQueue(TransferQueue * Queue, const SynchronizeChecklist & Checklist, const CopyParam & Param,
  int Params, bool DeleteLocalToTrash)
{
  // TCustomScpExplorerForm::DoQueueSynchronize + TTerminal::SynchronizeToQueue
  if ((Queue == nullptr) || ((Params & spTimestamp) != 0))
  {
    return 0;
  }
  CopyParam SyncCopyParam = GetSynchronizeCopyParam(Param, Params);
  // GetSynchronizeCopyParams: cpNoConfirmation
  SyncCopyParam.QueueNoConfirmation = true;
  int Result = 0;
  int Index = 0;
  const SynchronizeChecklist::Item * ChecklistItem;
  while (Checklist.GetNextChecked(Index, ChecklistItem))
  {
    QStringList FileList(ChecklistItem->GetFilePath());
    switch (ChecklistItem->Action)
    {
      case SynchronizeChecklist::saDownloadNew:
      case SynchronizeChecklist::saDownloadUpdate:
        QDir().mkpath(ChecklistItem->GetLocalTarget());
        Queue->AddDownload(FileList, ChecklistItem->GetLocalTarget(), SyncCopyParam, false);
        Result++;
        break;

      case SynchronizeChecklist::saDeleteRemote:
        Queue->AddDelete(FileList, true);
        Result++;
        break;

      case SynchronizeChecklist::saUploadNew:
      case SynchronizeChecklist::saUploadUpdate:
        Queue->AddUpload(FileList, ChecklistItem->GetRemoteTarget(), SyncCopyParam, false);
        Result++;
        break;

      case SynchronizeChecklist::saDeleteLocal:
        Queue->AddDelete(FileList, false, DeleteLocalToTrash);
        Result++;
        break;

      default:
        break;
    }
  }
  return Result;
}

//---------------------------------------------------------------------------
// Calculate (directory sizes in the checklist)

namespace
{

// TTerminal::CalculateLocalFilesSize for one directory (with the masks)
qint64 CalculateLocalDirectorySize(const QString & Directory, const CopyParam & Param, FileOperationProgress & Progress,
  const std::shared_ptr<std::atomic<bool>> & Cancel)
{
  qint64 Result = 0;
  QDirIterator It(Directory, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
  while (It.hasNext())
  {
    if (Cancel->load())
    {
      throw AbortException();
    }
    Progress.CheckCancel();
    It.next();
    QFileInfo Info = It.fileInfo();
    bool Directory2 = Info.isDir();
    if (!Param.AllowTransfer(Info.filePath(), true, Directory2, MaskParams(Directory2 ? 0 : Info.size(), Info.lastModified()),
          Info.fileName().startsWith(QLatin1Char('.'))))
    {
      continue;
    }
    if (Directory2)
    {
      if (!Info.isSymLink())
      {
        Result += CalculateLocalDirectorySize(Info.filePath(), Param, Progress, Cancel);
      }
    }
    else
    {
      Result += Info.size();
    }
  }
  return Result;
}

// TTerminal::CalculateFilesSize (DoCalculateDirectorySize) for one directory
qint64 CalculateRemoteDirectorySize(FileSystem & Fs, const QString & Directory, const CopyParam & Param,
  bool FollowDirectorySymlinks, FileOperationProgress & Progress, int Depth)
{
  qint64 Result = 0;
  if (Depth > 64)
  {
    return Result;
  }
  Progress.SetFile(Directory, 0);
  for (const RemoteFile & File : Fs.ReadDirectory(Directory))
  {
    Progress.CheckCancel();
    if (File.IsParentDirectory || !IsValidListingName(File.FileName))
    {
      continue;
    }
    QString FullFileName = UnixPath::Combine(Directory, File.FileName);
    if (!Param.AllowTransfer(FullFileName, false, File.IsDirectory, MaskParams(File.Size, File.Modification), File.IsHidden))
    {
      continue;
    }
    if (File.IsDirectory)
    {
      if (!File.IsSymLink || FollowDirectorySymlinks)
      {
        Result += CalculateRemoteDirectorySize(Fs, FullFileName, Param, FollowDirectorySymlinks, Progress, Depth + 1);
      }
    }
    else
    {
      Result += File.Size;
    }
  }
  return Result;
}

}

void SynchronizeChecklistCalculateSize(Terminal * Term, SynchronizeChecklistPtr Checklist,
  const std::vector<const SynchronizeChecklist::Item *> & Items, const CopyParam & Param, int Params,
  Terminal::Callback Done)
{
  Q_UNUSED(Params);
  struct SizeJob
  {
    const SynchronizeChecklist::Item * ChecklistItem;
    bool Remote;
    QString Path;
    qint64 Size;
  };
  auto Jobs = std::make_shared<std::vector<SizeJob>>();
  for (const SynchronizeChecklist::Item * ChecklistItem : Items)
  {
    if (ChecklistItem->IsDirectory)
    {
      if (ChecklistItem->IsRemoteOnly())
      {
        Jobs->push_back({ChecklistItem, true, ChecklistItem->GetRemotePath(), -1});
      }
      else if (ChecklistItem->IsLocalOnly())
      {
        Jobs->push_back({ChecklistItem, false, ChecklistItem->GetLocalPath(), -1});
      }
      // "update" actions are not relevant for directories
    }
  }
  bool FollowDirectorySymlinks = Term->Data().FollowDirectorySymlinks;
  auto Cancel = std::make_shared<std::atomic<bool>>(false);
  Term->Progress().Start(FileOperationProgress::Calculate, FileOperationProgress::ToRemote, static_cast<int>(Jobs->size()));
  Term->RunOnWorker(
    [Jobs, Param, FollowDirectorySymlinks, Cancel](FileSystem & Fs, FileOperationProgress & Progress, UserInterface &)
    {
      for (SizeJob & Job : *Jobs)
      {
        Progress.CheckCancel();
        Progress.SetFile(Job.Path, 0);
        Job.Size = Job.Remote ?
          CalculateRemoteDirectorySize(Fs, Job.Path, Param, FollowDirectorySymlinks, Progress, 0) :
          CalculateLocalDirectorySize(Job.Path, Param, Progress, Cancel);
        Progress.FileFinished();
      }
    },
    [Jobs, Checklist, Done](const OperationResult & Result)
    {
      // also the sizes calculated before a failure/cancel are kept (__finally)
      for (const SizeJob & Job : *Jobs)
      {
        if (Job.Size >= 0)
        {
          Checklist->UpdateDirectorySize(Job.ChecklistItem, Job.Size);
        }
      }
      if (Done)
      {
        Done(Result);
      }
    });
}

//---------------------------------------------------------------------------
// Move (checklist)

void SynchronizeMove(Terminal * Term, bool Remote, const QStringList & FileList, const QString & NewFileName,
  bool TargetIsDirectory, Terminal::Callback Done)
{
  // TCustomScpExplorerForm::DoSynchronizeMove
  Term->Progress().Start(FileOperationProgress::Move, Remote ? FileOperationProgress::ToRemote : FileOperationProgress::ToLocal,
    static_cast<int>(FileList.size()));
  Term->RunOnWorker(
    [Remote, FileList, NewFileName, TargetIsDirectory](FileSystem & Fs, FileOperationProgress & Progress, UserInterface &)
    {
      if (Remote)
      {
        if (TargetIsDirectory)
        {
          // CreateTargetDirectory
          RemoteFile Existing;
          if (!Fs.FileExists(NewFileName, &Existing))
          {
            Fs.CreateDirectory(NewFileName, ServerDefaultMode);
          }
        }
        for (const QString & FileName : FileList)
        {
          Progress.CheckCancel();
          Progress.SetFile(FileName, 0);
          QString Target = TargetIsDirectory ? UnixPath::Combine(NewFileName, UnixPath::ExtractFileName(FileName)) : NewFileName;
          Fs.RenameFile(FileName, Target);
          Progress.FileFinished();
        }
      }
      else
      {
        if (TargetIsDirectory && !QDir().mkpath(NewFileName))
        {
          // CREATE_DIR_ERROR
          throw ExtException(QStringLiteral("Cannot create folder '%1'.").arg(NewFileName));
        }
        for (const QString & FileName : FileList)
        {
          Progress.CheckCancel();
          Progress.SetFile(FileName, 0);
          QString Target = TargetIsDirectory ? LocalCombine(NewFileName, QFileInfo(FileName).fileName()) : NewFileName;
          if (std::rename(QFile::encodeName(FileName).constData(), QFile::encodeName(Target).constData()) != 0)
          {
            // RENAME_FILE_ERROR
            throw ExtException(QStringLiteral("Error moving file '%1' to '%2'.").arg(FileName, Target),
              QString::fromLocal8Bit(strerror(errno)));
          }
          Progress.FileFinished();
        }
      }
    },
    std::move(Done));
}

//---------------------------------------------------------------------------
// Compare directories

static void ProcessChangedFiles(const QList<CompareEntry> & Entries, const QList<CompareEntry> & Mirror,
  bool ByTime, bool BySize, bool ExistingOnly, QStringList & Changed)
{
  QHash<QString, const CompareEntry *> MirrorIndex;
  for (const CompareEntry & Entry : Mirror)
  {
    if (!MirrorIndex.contains(Entry.FileName))
    {
      MirrorIndex.insert(Entry.FileName, &Entry);
    }
  }
  for (const CompareEntry & Item : Entries)
  {
    if (Item.Directory)
    {
      continue;
    }
    bool IsChanged = false;
    const CompareEntry * MirrorItem = MirrorIndex.value(Item.FileName, nullptr);
    if (MirrorItem == nullptr)
    {
      IsChanged = !ExistingOnly;
    }
    else
    {
      bool SameTime;
      if (ByTime)
      {
        QDateTime FileTime = Item.Modification;
        QDateTime MirrorFileTime = MirrorItem->Modification;
        bool SecondPrecision = Item.SecondPrecision || MirrorItem->SecondPrecision;
        // ModificationFmt: an FTP LIST may give minutes or days only, or no time at all
        TModificationFmt Precision = LessDateTimePrecision(Item.ModificationFmt, MirrorItem->ModificationFmt);
        if (SecondPrecision)
        {
          FileTime = ReduceDateTimePrecision(FileTime, Precision);
          MirrorFileTime = ReduceDateTimePrecision(MirrorFileTime, Precision);
        }
        SameTime = (FileTime == MirrorFileTime);
        if (SecondPrecision)
        {
          // 1 ms more solves the rounding issues
          MirrorFileTime = MirrorFileTime.addMSecs(1001);
        }
        IsChanged = FileTime.isValid() && MirrorFileTime.isValid() && (FileTime > MirrorFileTime);
      }
      else
      {
        SameTime = true;
      }
      if (!IsChanged && SameTime && BySize)
      {
        IsChanged = (Item.Size != MirrorItem->Size);
      }
    }
    if (IsChanged)
    {
      Changed.append(Item.FileName);
    }
  }
}

void CompareDirectories(const QList<CompareEntry> & Local, const QList<CompareEntry> & Remote,
  bool ByTime, bool BySize, bool ExistingOnly, QStringList & LocalChanged, QStringList & RemoteChanged)
{
  LocalChanged.clear();
  RemoteChanged.clear();
  ProcessChangedFiles(Local, Remote, ByTime, BySize, ExistingOnly, LocalChanged);
  ProcessChangedFiles(Remote, Local, ByTime, BySize, ExistingOnly, RemoteChanged);
}

void CompareDirectories(const QString & LocalDirectory, const RemoteFileList & RemoteFiles,
  bool ByTime, bool BySize, QStringList & LocalChanged, QStringList & RemoteChanged, const QSet<QString> * LocalNames)
{
  QList<CompareEntry> Local;
  const QFileInfoList Entries = QDir(LocalDirectory).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
  for (const QFileInfo & Info : Entries)
  {
    if ((LocalNames != nullptr) && !LocalNames->contains(Info.fileName()))
    {
      continue;
    }
    CompareEntry Entry;
    Entry.FileName = Info.fileName();
    Entry.Directory = Info.isDir();
    Entry.Size = Info.isDir() ? 0 : Info.size();
    Entry.Modification = Info.lastModified();
    Local.append(Entry);
  }
  QList<CompareEntry> Remote;
  for (const RemoteFile & File : RemoteFiles)
  {
    if (File.IsParentDirectory || !IsValidListingName(File.FileName))
    {
      continue;
    }
    CompareEntry Entry;
    Entry.FileName = File.FileName;
    Entry.Directory = File.IsDirectory;
    Entry.Size = File.Size;
    Entry.Modification = File.Modification;
    Entry.ModificationFmt = File.ModificationFmt;
    Entry.SecondPrecision = true;
    Remote.append(Entry);
  }
  CompareDirectories(Local, Remote, ByTime, BySize, false, LocalChanged, RemoteChanged);
}

//---------------------------------------------------------------------------
// TSynchronizeController

static int ReadProcInteger(const char * Path)
{
  QFile File(QString::fromLatin1(Path));
  if (File.open(QIODevice::ReadOnly))
  {
    bool Ok = false;
    int Value = QString::fromLatin1(File.readAll()).trimmed().toInt(&Ok);
    if (Ok)
    {
      return Value;
    }
  }
  return -1;
}

QString InotifyErrorMessage(int Error)
{
  QString Message = QString::fromLocal8Bit(strerror(Error));
  if (Error == ENOSPC)
  {
    // inotify_add_watch: the limit of watches per user was reached
    int Limit = ReadProcInteger("/proc/sys/fs/inotify/max_user_watches");
    Message = QStringLiteral(
      "The system limit of watched directories was reached (fs.inotify.max_user_watches = %1). "
      "Increase the limit, e.g. with 'sudo sysctl fs.inotify.max_user_watches=524288' "
      "(add the setting to /etc/sysctl.conf to keep it), or watch fewer directories.")
      .arg((Limit >= 0) ? QString::number(Limit) : QStringLiteral("?"));
  }
  else if (Error == EMFILE)
  {
    // inotify_init1: the limit of inotify instances per user was reached
    int Limit = ReadProcInteger("/proc/sys/fs/inotify/max_user_instances");
    Message = QStringLiteral(
      "The system limit of directory watchers was reached (fs.inotify.max_user_instances = %1). "
      "Close other applications that watch directories, or increase the limit, "
      "e.g. with 'sudo sysctl fs.inotify.max_user_instances=1024'.")
      .arg((Limit >= 0) ? QString::number(Limit) : QStringLiteral("?"));
  }
  return Message;
}

#ifdef NIXSCP_INOTIFY
static const uint32_t WatchMask =
  IN_CLOSE_WRITE | IN_MODIFY | IN_ATTRIB | IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO |
  IN_DELETE_SELF | IN_MOVE_SELF | IN_DONT_FOLLOW;
#endif

SynchronizeController::SynchronizeController(Terminal * Term, QObject * Parent) :
  QObject(Parent),
  FTerminal(Term),
  FAlive(std::make_shared<bool>(true))
{
  FChangeTimer.setSingleShot(true);
  connect(&FChangeTimer, &QTimer::timeout, this, &SynchronizeController::ProcessPending);
}

SynchronizeController::~SynchronizeController()
{
  Stop();
  *FAlive = false;
}

QString SynchronizeController::RemotePathFor(const QString & LocalDirectory) const
{
  QString Root = FParams.LocalDirectory;
  QString Relative;
  if ((LocalDirectory != Root) && LocalDirectory.startsWith(Root + QLatin1Char('/')))
  {
    Relative = LocalDirectory.mid(Root.size() + 1);
  }
  else if ((Root == QStringLiteral("/")) && (LocalDirectory != Root))
  {
    Relative = LocalDirectory.mid(1);
  }
  return Relative.isEmpty() ? FParams.RemoteDirectory : UnixPath::Combine(FParams.RemoteDirectory, Relative);
}

void SynchronizeController::SetBusy(bool Busy)
{
  if (FBusy != Busy)
  {
    FBusy = Busy;
    emit BusyChanged(Busy);
  }
}

void SynchronizeController::Start(const SynchronizeParamType & Params, const CopyParam & Param, const SynchronizeOptions & Options)
{
  Stop();
  FGeneration++;
  FParams = Params;
  FParams.LocalDirectory = NormalizeLocalDirectory(Params.LocalDirectory);
  FParams.RemoteDirectory = NormalizeRemoteDirectory(Params.RemoteDirectory);
  // TSynchronizeDialog::GetCopyParams
  FCopyParam = Param;
  FCopyParam.PreserveTime = true;
  FCopyParam.NewerOnly = false;
  FOptions = Options;
  FActive = true;
  FDirectoriesLimit = std::max(FMaxWatchDirectories, 1);
  FTooManyAborted = false;
  FCache = std::make_shared<SynchronizeListingCache>();

  if ((FParams.Options & soSynchronize) != 0)
  {
    FInitialSync = true;
    SynchronizeDirectory(FParams.LocalDirectory, true);
  }
  else
  {
    FInitialSync = false;
    QMetaObject::invokeMethod(this, [this, Generation = FGeneration]()
    {
      if (FActive && (Generation == FGeneration))
      {
        Rescan();
      }
    }, Qt::QueuedConnection);
  }
}

bool SynchronizeController::AllowDirectory(const QString & Directory) const
{
  // TSynchronizeController::SynchronizeFilter
  bool Add = true;
  QFileInfo Info(Directory);
  if (FOptions.UseFilter && (NormalizeLocalDirectory(Info.path()) == FParams.LocalDirectory))
  {
    Add = FOptions.MatchesFilter(Info.fileName());
  }
  if (Add && !FCopyParam.AllowAnyTransfer())
  {
    FileMasks::Params MaskParams; // size/time does not matter for directories
    Add = FCopyParam.AllowTransfer(Directory, true, true, MaskParams, Info.fileName().startsWith(QLatin1Char('.')));
  }
  return Add;
}

bool SynchronizeController::AddDirectory(const QString & Directory, bool Recurse, QString & Error)
{
  // TDiscMonitor: OnTooManyDirectories before exceeding MaxDirectories
  bool AlreadyWatched = FWatchedPaths.contains(Directory);
  if (!AlreadyWatched && (FWatches.size() >= FDirectoriesLimit) &&
      !isSignalConnected(QMetaMethod::fromSignal(&SynchronizeController::TooManyDirectories)))
  {
    // nobody to ask: no limit
    FDirectoriesLimit = std::numeric_limits<int>::max();
  }
  if (!AlreadyWatched && (FWatches.size() >= FDirectoriesLimit))
  {
    int MaxDirectories = FDirectoriesLimit;
    bool AbortWatching = false;
    emit TooManyDirectories(MaxDirectories, AbortWatching);
    if (AbortWatching || (MaxDirectories <= FWatches.size()))
    {
      FTooManyAborted = true;
      Error.clear();
      return false;
    }
    FDirectoriesLimit = MaxDirectories;
  }
#ifndef NIXSCP_INOTIFY
  if (!AlreadyWatched)
  {
    if (!FWatcher->addPath(Directory))
    {
      if (!QFileInfo(Directory).isDir() && (Directory != FParams.LocalDirectory))
      {
        return true; // vanished meanwhile
      }
      Error = QStringLiteral("Cannot watch directory '%1' for changes.").arg(Directory);
      return false;
    }
    FWatches.insert(++FNextWatch, Directory);
  }
#else
  int Wd = inotify_add_watch(FInotify, QFile::encodeName(Directory).constData(), WatchMask);
  if (Wd < 0)
  {
    int Err = errno;
    if ((Err == ENOENT) && (Directory != FParams.LocalDirectory))
    {
      return true; // vanished meanwhile
    }
    Error = InotifyErrorMessage(Err);
    return false;
  }
  FWatches.insert(Wd, Directory);
#endif
  FWatchedPaths.insert(Directory);
  if (Recurse)
  {
    const QFileInfoList Entries = QDir(Directory).entryInfoList(QDir::Dirs | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot | QDir::NoSymLinks);
    for (const QFileInfo & Info : Entries)
    {
      if (AllowDirectory(Info.filePath()) && !AddDirectory(Info.filePath(), true, Error))
      {
        return false;
      }
    }
  }
  return true;
}

void SynchronizeController::RemoveWatches()
{
#ifndef NIXSCP_INOTIFY
  if ((FWatcher != nullptr) && !FWatcher->directories().isEmpty())
  {
    FWatcher->removePaths(FWatcher->directories());
  }
#else
  if (FInotify >= 0)
  {
    for (auto It = FWatches.cbegin(); It != FWatches.cend(); ++It)
    {
      inotify_rm_watch(FInotify, It.key());
    }
  }
#endif
  FWatches.clear();
  FWatchedPaths.clear();
}

void SynchronizeController::Rescan()
{
  // DiscMon AddDirectory + Open (initially) / directories change (later)
#ifndef NIXSCP_INOTIFY
  bool Initial = (FWatcher == nullptr);
#else
  bool Initial = (FInotify < 0);
#endif
  bool Recurse = ((FParams.Options & soRecurse) != 0);
  if (Initial)
  {
    if (Recurse)
    {
      emit Log(slScan, QStringLiteral("Scanning '%1' for subdirectories...").arg(FParams.LocalDirectory), QString());
    }
#ifndef NIXSCP_INOTIFY
    FWatcher = new QFileSystemWatcher(this);
    connect(FWatcher, &QFileSystemWatcher::directoryChanged, this, &SynchronizeController::WatchedDirectoryChanged);
  }
#else
    FInotify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (FInotify < 0)
    {
      // WATCH_ERROR_GENERAL
      Abort(false, QStringLiteral("Error watching for changes."), InotifyErrorMessage(errno));
      return;
    }
    FNotifier = new QSocketNotifier(FInotify, QSocketNotifier::Read, this);
    connect(FNotifier, &QSocketNotifier::activated, this, &SynchronizeController::ReadEvents);
  }
#endif
  int Before = FWatches.size();
  QString Error;
  if (!QFileInfo(FParams.LocalDirectory).isDir())
  {
    Abort(false, QStringLiteral("Error watching for changes in directory '%1'.").arg(FParams.LocalDirectory),
      QString::fromLocal8Bit(strerror(ENOENT)));
    return;
  }
  if (!AddDirectory(FParams.LocalDirectory, Recurse, Error))
  {
    if (FTooManyAborted)
    {
      // Abort() in OnTooManyDirectories
      Abort(false, QString(), QString());
    }
    else
    {
      Abort(false, QStringLiteral("Error watching for changes in directory '%1'.").arg(FParams.LocalDirectory), Error);
    }
    return;
  }
  if (Initial)
  {
    emit Log(slStart, QStringLiteral("Watching for changes in %1 directories...").arg(FWatches.size()), QString());
  }
  else if (FWatches.size() != Before)
  {
    emit Log(slDirChange, QStringLiteral("Watching for changes in %1 directories...").arg(FWatches.size()), QString());
  }
}

#ifndef NIXSCP_INOTIFY
// QFileSystemWatcher: "something changed in Directory" (files or subdirectories)
void SynchronizeController::WatchedDirectoryChanged(const QString & Directory)
{
  if (!FActive)
  {
    return;
  }
  if (!QFileInfo(Directory).isDir())
  {
    if (Directory == FParams.LocalDirectory)
    {
      // TSynchronizeController::SynchronizeInvalid
      Abort(false, QStringLiteral("Error watching for changes in directory '%1'.").arg(Directory),
        QString::fromLocal8Bit(strerror(ENOENT)));
      return;
    }
    // A watched subdirectory (and the tree below it) is gone
    QString Prefix = Directory + QLatin1Char('/');
    for (auto W = FWatches.begin(); W != FWatches.end(); )
    {
      if ((W.value() == Directory) || W.value().startsWith(Prefix))
      {
        FWatcher->removePath(W.value());
        FWatchedPaths.remove(W.value());
        W = FWatches.erase(W);
      }
      else
      {
        ++W;
      }
    }
    return;
  }
  bool DirectoriesChanged = false;
  if ((FParams.Options & soRecurse) != 0)
  {
    // New subdirectories are watched too
    const QFileInfoList Entries = QDir(Directory).entryInfoList(QDir::Dirs | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot | QDir::NoSymLinks);
    for (const QFileInfo & Info : Entries)
    {
      QString Path = Info.filePath();
      if (!FWatchedPaths.contains(Path) && AllowDirectory(Path))
      {
        QString Error;
        if (!AddDirectory(Path, true, Error))
        {
          Abort(false, FTooManyAborted ? QString() : QStringLiteral("Error watching for changes in directory '%1'.").arg(Path), Error);
          return;
        }
        DirectoriesChanged = true;
      }
    }
  }
  DirectoryChanged(Directory);
  if (DirectoriesChanged && FActive)
  {
    emit Log(slDirChange, QStringLiteral("Watching for changes in %1 directories...").arg(FWatches.size()), QString());
  }
}

void SynchronizeController::ReadEvents()
{
}
#else
void SynchronizeController::ReadEvents()
{
  alignas(struct inotify_event) char Buffer[16384];
  bool Recurse = ((FParams.Options & soRecurse) != 0);
  bool DirectoriesChanged = false;
  for (;;)
  {
    ssize_t Len = read(FInotify, Buffer, sizeof(Buffer));
    if (Len <= 0)
    {
      break;
    }
    for (char * P = Buffer; P < Buffer + Len; )
    {
      const struct inotify_event * Event = reinterpret_cast<const struct inotify_event *>(P);
      P += sizeof(struct inotify_event) + Event->len;
      if (!FActive)
      {
        continue;
      }
      if ((Event->mask & IN_Q_OVERFLOW) != 0)
      {
        // lost events: synchronize everything watched
        for (const QString & Directory : FWatches)
        {
          DirectoryChanged(Directory);
        }
        continue;
      }
      auto It = FWatches.constFind(Event->wd);
      if (It == FWatches.constEnd())
      {
        continue;
      }
      QString Directory = It.value();
      if ((Event->mask & (IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF)) != 0)
      {
        if (Directory == FParams.LocalDirectory)
        {
          // TSynchronizeController::SynchronizeInvalid
          Abort(false, QStringLiteral("Error watching for changes in directory '%1'.").arg(Directory),
            QString::fromLocal8Bit(strerror(ENOENT)));
          return;
        }
        if ((Event->mask & IN_IGNORED) != 0)
        {
          FWatchedPaths.remove(Directory);
          FWatches.remove(Event->wd);
        }
        continue;
      }
      if (Event->len == 0)
      {
        continue;
      }
      QString Name = QFile::decodeName(Event->name);
      QString Path = LocalCombine(Directory, Name);
      if ((Event->mask & IN_ISDIR) != 0)
      {
        if (Recurse && ((Event->mask & (IN_CREATE | IN_MOVED_TO)) != 0) && AllowDirectory(Path))
        {
          QString Error;
          if (!AddDirectory(Path, true, Error))
          {
            Abort(false, FTooManyAborted ? QString() : QStringLiteral("Error watching for changes in directory '%1'.").arg(Path), Error);
            return;
          }
          DirectoriesChanged = true;
        }
        else if ((Event->mask & IN_MOVED_FROM) != 0)
        {
          // the watches of the moved tree would report wrong paths
          QString Prefix = Path + QLatin1Char('/');
          for (auto W = FWatches.begin(); W != FWatches.end(); )
          {
            if ((W.value() == Path) || W.value().startsWith(Prefix))
            {
              inotify_rm_watch(FInotify, W.key());
              FWatchedPaths.remove(W.value());
              W = FWatches.erase(W);
              DirectoriesChanged = true;
            }
            else
            {
              ++W;
            }
          }
        }
      }
      DirectoryChanged(Directory);
    }
  }
  if (DirectoriesChanged && FActive)
  {
    emit Log(slDirChange, QStringLiteral("Watching for changes in %1 directories...").arg(FWatches.size()), QString());
  }
}
#endif

void SynchronizeController::DirectoryChanged(const QString & Directory)
{
  if (!FPending.contains(Directory))
  {
    FPending.insert(Directory);
    FPendingOrder.append(Directory);
  }
  // KeepUpToDateChangeDelay: wait until the changes settle
  FChangeTimer.start(FChangeDelay);
}

void SynchronizeController::ProcessPending()
{
  if (!FActive || FBusy || FChangeTimer.isActive())
  {
    return;
  }
  while (!FPendingOrder.isEmpty())
  {
    // parents first, so that new directories are created before their contents are examined
    auto Shallowest = std::min_element(FPendingOrder.begin(), FPendingOrder.end(),
      [](const QString & A, const QString & B) { return A.count(QLatin1Char('/')) < B.count(QLatin1Char('/')); });
    QString Directory = *Shallowest;
    FPendingOrder.erase(Shallowest);
    FPending.remove(Directory);
    if (QFileInfo(Directory).isDir())
    {
      SynchronizeDirectory(Directory, false);
      return;
    }
  }
}

void SynchronizeController::SynchronizeDirectory(const QString & LocalDirectory, bool Full)
{
  // TCustomScpExplorerForm::DoSynchronize + Synchronize
  SetBusy(true);
  int Params = FParams.Params;
  if (!Full)
  {
    Params |= spNoRecurse | spUseCache | spDelayProgress;
    if ((FParams.Options & soRecurse) != 0)
    {
      Params |= spSubDirs;
    }
    emit Log(slChange, QStringLiteral("Change in '%1' detected.").arg(LocalDirectory), QString());
  }
  else
  {
    // if keepuptodate is non-recursive,
    // full sync before has to be non-recursive as well
    if ((FParams.Options & soRecurse) == 0)
    {
      Params |= spNoRecurse;
    }
  }
  Params |= spNoConfirmation;
  SynchronizeOptions Options;
  if (LocalDirectory == FParams.LocalDirectory)
  {
    Options = FOptions;
  }
  // spUseCache: the remote listings are cached between the changes
  Options.Cache = FCache;
  FStatus = std::make_shared<SynchronizeStatus>();
  std::weak_ptr<bool> Alive = FAlive;
  quint64 Generation = FGeneration;
  SynchronizeStatusPtr Status = FStatus;
  Terminal * Term = FTerminal;
  CopyParam Param = FCopyParam;
  SynchronizeCollect(FTerminal, LocalDirectory, RemotePathFor(LocalDirectory), smRemote, FCopyParam, Params, Options, Status,
    [this, Alive, Generation, Status, Term, Param, Params, LocalDirectory, Full](const OperationResult & Result, SynchronizeChecklistPtr Checklist)
    {
      if (Alive.expired() || (Generation != FGeneration))
      {
        return;
      }
      if (!Result.Ok || (Checklist->CheckedCount() == 0))
      {
        SynchronizeDone(LocalDirectory, Full, Result, Checklist);
        return;
      }
      SynchronizeApply(Term, Checklist, Param, Params, Status,
        [this, Alive, Generation](const SynchronizeChecklist::Item * ChecklistItem)
        {
          if (Alive.expired() || (Generation != FGeneration))
          {
            return;
          }
          // TSynchronizeController::LogOperation
          if (ChecklistItem->Action == SynchronizeChecklist::saDeleteRemote)
          {
            emit Log(slDelete, QStringLiteral("File '%1' deleted.").arg(ChecklistItem->GetRemotePath()), QString());
          }
          else
          {
            emit Log(slUpload, QStringLiteral("File '%1' uploaded.").arg(ChecklistItem->GetLocalPath()), QString());
          }
        },
        [this, Alive, Generation, LocalDirectory, Full, Checklist](const OperationResult & AResult, const SynchronizeStatistics &)
        {
          if (Alive.expired() || (Generation != FGeneration))
          {
            return;
          }
          SynchronizeDone(LocalDirectory, Full, AResult, Checklist);
        });
    });
}

void SynchronizeController::SynchronizeDone(const QString & LocalDirectory, bool Full, const OperationResult & Result, SynchronizeChecklistPtr Checklist)
{
  Q_UNUSED(LocalDirectory);
  FStatus.reset();
  if (FCache)
  {
    if (!Result.Ok || !Checklist)
    {
      FCache->Clear();
    }
    else
    {
      // the remote directories changed by the synchronization
      for (int Index = 0; Index < Checklist->Count(); Index++)
      {
        const SynchronizeChecklist::Item * Item = Checklist->GetItem(Index);
        if (Item->Checked)
        {
          FCache->Invalidate(Item->Info2.Directory);
          if (Item->IsDirectory)
          {
            FCache->Invalidate(Item->ForceGetRemotePath());
          }
        }
      }
    }
  }
  SetBusy(false);
  if (!Result.Ok)
  {
    if (Result.Fatal || !FTerminal->Active())
    {
      Abort(true, Result.Message, Result.MoreMessages);
      return;
    }
    else if (!Result.Aborted && ((FParams.Options & soContinueOnError) != 0))
    {
      emit Log(slContinuedError, Result.Message, Result.MoreMessages);
    }
    else
    {
      Abort(false, Result.Aborted ? QString() : Result.Message, Result.MoreMessages);
      return;
    }
  }
  if (Full || FInitialSync)
  {
    FInitialSync = false;
    Rescan();
  }
  else if (Result.Ok && ((FParams.Options & soRecurse) != 0) && Checklist)
  {
    // TSynchronizeController::SynchronizeChange: SubdirsChanged
    bool SubdirsChanged = false;
    for (int Index = 0; Index < Checklist->Count(); Index++)
    {
      const SynchronizeChecklist::Item * Item = Checklist->GetItem(Index);
      if (Item->IsDirectory &&
          ((Item->Action == SynchronizeChecklist::saUploadNew) || (Item->Action == SynchronizeChecklist::saDeleteRemote)))
      {
        SubdirsChanged = true;
        break;
      }
    }
    if (SubdirsChanged)
    {
      Rescan();
    }
  }
  if (FActive)
  {
    QMetaObject::invokeMethod(this, &SynchronizeController::ProcessPending, Qt::QueuedConnection);
  }
}

void SynchronizeController::Abort(bool Close, const QString & Message, const QString & MoreMessages)
{
  Stop();
  emit Aborted(Close, Message, MoreMessages);
}

void SynchronizeController::Stop()
{
  bool WasActive = FActive;
  FActive = false;
  FGeneration++;
  FChangeTimer.stop();
  FPending.clear();
  FPendingOrder.clear();
  if (FStatus)
  {
    FStatus->Cancel();
    if (FTerminal != nullptr)
    {
      FTerminal->Progress().Cancel();
    }
    FStatus.reset();
  }
  RemoveWatches();
#ifndef NIXSCP_INOTIFY
  delete FWatcher;
  FWatcher = nullptr;
#else
  if (FNotifier != nullptr)
  {
    delete FNotifier;
    FNotifier = nullptr;
  }
  if (FInotify >= 0)
  {
    close(FInotify);
    FInotify = -1;
  }
#endif
  if (WasActive)
  {
    SetBusy(false);
  }
}
