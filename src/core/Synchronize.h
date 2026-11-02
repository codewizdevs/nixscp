// Port of WinSCP synchronization:
//   source/core/Terminal.{h,cpp}     SynchronizeCollect, DoSynchronizeCollectDirectory, SynchronizeApply,
//                                    TSynchronizeOptions, sp* flags, GetSynchronizeCopyParam,
//                                    SameFileChecksum, SynchronizeToQueue,
//                                    SynchronizeChecklistCalculateSize
//   source/core/RemoteFiles.{h,cpp}  TSynchronizeChecklist, TSynchronizeProgress
//   source/windows/SynchronizeController.{h,cpp}  keep remote directory up to date
//   source/forms/CustomScpExplorer.cpp            DoSynchronizeMove, DoSynchronizeTooManyDirectories
//   source/packages/filemng/CustomDirView.pas     ProcessChangedFiles (Compare Directories)
//
// All functions/classes here are used from the GUI thread; the remote
// work runs on the Terminal's worker thread via Terminal::RunOnWorker and the
// regular Terminal operations (so transfers honour CopyParam exactly as
// regular transfers do: masks, resume, speed limit, rights, times).
//
// Linux adaptations: "Case sensitive" is part of spDefault (Linux file
// systems are case sensitive); local deletes go to the desktop trash
// (QFile::moveToTrash) instead of the Recycle Bin; directory changes are
// watched with inotify instead of ReadDirectoryChangesW; checksums are
// calculated on the server (FTP HASH/XSHA*, sha256sum/sha1sum over the shell
// of SFTP/SCP sessions), only servers without any of these download the file.
//
// Not ported: local-local synchronization (spLocalLocal; NixSCP has no
// local-local panel mode yet).
#pragma once

#include "core/Terminal.h"

#include <QDateTime>
#include <QHash>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <atomic>
#include <functional>
#include <memory>
#include <vector>

class QSocketNotifier;
class QFileSystemWatcher;
class TransferQueue;

// TTerminal::TSynchronizeMode; TScript::SynchronizeProc and WinSCP.ini rely on the order
enum SynchronizeMode { smRemote, smLocal, smBoth };

// TTerminal::sp* (stored in WinSCP.ini Configuration\Interface\SynchronizeParams)
const int spDelete = 0x01;          // cannot be combined with spTimestamp
const int spNoConfirmation = 0x02;  // has no effect for spTimestamp
const int spExistingOnly = 0x04;    // is implicit for spTimestamp
const int spNoRecurse = 0x08;
const int spUseCache = 0x10;        // remote listings from SynchronizeOptions::Cache
const int spDelayProgress = 0x20;   // no effect in NixSCP
const int spPreviewChanges = 0x40;  // not used by core
const int spSubDirs = 0x80;         // cannot be combined with spTimestamp
const int spTimestamp = 0x100;
const int spNotByTime = 0x200;      // cannot be combined with spTimestamp and smBoth
const int spBySize = 0x400;         // cannot be combined with smBoth, has opposite meaning for spTimestamp
const int spSelectedOnly = 0x800;   // not used by core
const int spMirror = 0x1000;
const int spCaseSensitive = 0x2000;
const int spByChecksum = 0x4000;    // cannot be combined with spTimestamp and smBoth
// WinSCP: spNoConfirmation | spPreviewChanges (Windows file systems are case
// insensitive); Linux: case sensitive by default
const int spDefault = spNoConfirmation | spPreviewChanges | spCaseSensitive;

QString SynchronizeModeStr(SynchronizeMode Mode);
QString SynchronizeParamsStr(int Params);

// Remote directory listings kept between synchronizations of "Keep remote
// directory up to date" (the role of the directory cache of TTerminal with
// spUseCache). Thread safe.
class SynchronizeListingCache
{
public:
  bool Get(const QString & Directory, RemoteFileList & Files) const;
  void Put(const QString & Directory, const RemoteFileList & Files);
  // Forgets Directory and everything below it
  void Invalidate(const QString & Directory);
  void Clear();
  int Count() const;

private:
  mutable QMutex FMutex;
  QHash<QString, RemoteFileList> FListings;
};

using SynchronizeListingCachePtr = std::shared_ptr<SynchronizeListingCache>;

// TSynchronizeOptions
struct SynchronizeOptions
{
  // "Selected files only": names allowed in the top level directories
  // (case insensitive, as WinSCP); used only when UseFilter is set
  bool UseFilter = false;
  QStringList Filter;
  // Number of files seen while collecting (statistics)
  int Files = 0;
  // Remote listings used with spUseCache (optional)
  SynchronizeListingCachePtr Cache;

  bool MatchesFilter(const QString & FileName) const;
};

// TSynchronizeChecklist
class SynchronizeChecklist
{
public:
  enum ActionType { saNone, saUploadNew, saDownloadNew, saUploadUpdate, saDownloadUpdate, saDeleteRemote, saDeleteLocal };
  static const int ActionCount = saDeleteLocal;

  struct FileInfo
  {
    QString FileName;
    QString Directory;          // without trailing slash
    QDateTime Modification;
    qint64 Size = 0;
  };

  class Item
  {
  public:
    ActionType Action = saNone;
    bool IsDirectory = false;
    FileInfo Info1;             // local
    FileInfo Info2;             // remote
    bool Checked = true;
    bool HasRemoteFile = false;
    RemoteFile Remote;          // remote file the item refers to (if any)

    QString GetFileName() const;
    bool IsRemoteOnly() const { return (Action == saDownloadNew) || (Action == saDeleteRemote); }
    bool IsLocalOnly() const { return (Action == saUploadNew) || (Action == saDeleteLocal); }
    bool HasSize() const { return !IsDirectory || FDirectoryHasSize; }
    qint64 GetBaseSize() const { return GetBaseSize(Action); }
    qint64 GetBaseSize(ActionType AAction) const;
    qint64 GetSize() const { return GetSize(Action); }
    qint64 GetSize(ActionType AAction) const;
    QString GetLocalPath() const;
    QString ForceGetLocalPath() const;
    QString GetRemotePath() const;
    QString ForceGetRemotePath() const;
    QString GetLocalTarget() const { return Info1.Directory; }
    QString GetRemoteTarget() const { return Info2.Directory; }
    // Full path of the source of the action (remote path for downloads and
    // remote deletes, local path otherwise)
    QString GetFilePath() const;
    // Local modification time with full (millisecond) precision
    QDateTime LocalLastWriteTime() const { return FLocalLastWriteTime; }

  private:
    friend class SynchronizeChecklist;
    friend class SynchronizeCollector;
    QDateTime FLocalLastWriteTime;
    bool FDirectoryHasSize = false;
  };

  SynchronizeChecklist() = default;
  SynchronizeChecklist(const SynchronizeChecklist &) = delete;
  SynchronizeChecklist & operator=(const SynchronizeChecklist &) = delete;

  int Count() const { return static_cast<int>(FList.size()); }
  int CheckedCount() const;
  const Item * GetItem(int Index) const { return FList[static_cast<size_t>(Index)].get(); }
  bool GetNextChecked(int & Index, const Item *& AItem) const;

  void Update(const Item * AItem, bool Check, ActionType Action);
  void UpdateDirectorySize(const Item * AItem, qint64 Size);
  void Delete(const Item * AItem);

  static ActionType Reverse(ActionType Action);
  // SynchronizeChecklist.cpp IsTransferNewAction / GetOppositeMoveAction
  static bool IsTransferNewAction(ActionType Action) { return (Action == saUploadNew) || (Action == saDownloadNew); }
  static ActionType OppositeMoveAction(ActionType Action);
  static bool IsItemSizeIrrelevant(ActionType Action);
  static int Compare(const Item * Item1, const Item * Item2);

  void Add(std::unique_ptr<Item> AItem) { FList.push_back(std::move(AItem)); }
  void Sort();

private:
  std::vector<std::unique_ptr<Item>> FList;
  Item * Mutable(const Item * AItem);
};

using SynchronizeChecklistPtr = std::shared_ptr<SynchronizeChecklist>;

// Thread safe state of a running synchronization, polled by the progress
// window (TSynchronizeProgressForm::SetData / TerminalSynchronizeDirectory).
// Cancel() aborts both the comparison and the synchronization.
class SynchronizeStatus
{
public:
  void SetDirectories(const QString & LocalDirectory, const QString & RemoteDirectory, bool Collect);
  void IncFiles() { FFiles++; }
  int Files() const { return FFiles; }
  void Cancel() { FCancel = true; }
  bool Cancelled() const { return FCancel; }

  struct Snapshot
  {
    QString LocalDirectory;
    QString RemoteDirectory;
    bool Collect = true;
    int Files = 0;
  };
  Snapshot Get() const;

private:
  mutable QMutex FMutex;
  QString FLocalDirectory;
  QString FRemoteDirectory;
  bool FCollect = true;
  std::atomic<int> FFiles{0};
  std::atomic<bool> FCancel{false};
};

using SynchronizeStatusPtr = std::shared_ptr<SynchronizeStatus>;

// TSynchronizeProgress: overall progress of SynchronizeApply
class SynchronizeProgress
{
public:
  explicit SynchronizeProgress(SynchronizeChecklistPtr Checklist) : FChecklist(std::move(Checklist)) {}
  void ItemProcessed(const SynchronizeChecklist::Item * ChecklistItem);
  // Percent 0..100, CurrentTransferred = bytes done by the running operation
  int Progress(qint64 CurrentTransferred) const;

private:
  SynchronizeChecklistPtr FChecklist;
  mutable qint64 FTotalSize = -1;
  qint64 FProcessedSize = 0;
  qint64 ItemSize(const SynchronizeChecklist::Item * ChecklistItem) const;
};

// TFileOperationStatistics
struct SynchronizeStatistics
{
  int FilesUploaded = 0;
  qint64 TotalUploaded = 0;
  int FilesDownloaded = 0;
  qint64 TotalDownloaded = 0;
  int FilesDeletedLocal = 0;
  int FilesDeletedRemote = 0;
};

// TTerminal::GetSynchronizeCopyParam (+ no overwrite confirmations, cpNoConfirmation,
// and no "newer only", which the WinSCP dialogs reset)
CopyParam GetSynchronizeCopyParam(const CopyParam & Param, int Params);

using SynchronizeCollectCallback = std::function<void(const OperationResult & Result, SynchronizeChecklistPtr Checklist)>;
using SynchronizeProcessedItemEvent = std::function<void(const SynchronizeChecklist::Item * ChecklistItem)>;
using SynchronizeApplyCallback = std::function<void(const OperationResult & Result, const SynchronizeStatistics & Statistics)>;

// TTerminal::SynchronizeCollect: compares LocalDirectory with RemoteDirectory
// (recursively unless spNoRecurse) on the worker thread and returns the
// sorted checklist. Status (optional) receives the directories being compared
// and can cancel; Options (filter) as TSynchronizeOptions; Options.Files is
// not reported back (use Status->Files()).
void SynchronizeCollect(Terminal * Term, const QString & LocalDirectory, const QString & RemoteDirectory,
  SynchronizeMode Mode, const CopyParam & Param, int Params, const SynchronizeOptions & Options,
  SynchronizeStatusPtr Status, SynchronizeCollectCallback Done);

// TTerminal::SynchronizeApply: performs the checked items, in checklist order;
// consecutive items with the same action and directories are processed by one
// Terminal operation. OnProcessedItem (optional) is called for every finished item.
// DeleteLocalToTrash: local files are moved to the desktop trash
// (TerminalManager's OnDeleteLocalFile with DeleteToRecycleBin); scripting
// deletes permanently, as WinSCP.
void SynchronizeApply(Terminal * Term, SynchronizeChecklistPtr Checklist, const CopyParam & Param, int Params,
  SynchronizeStatusPtr Status, SynchronizeProcessedItemEvent OnProcessedItem, SynchronizeApplyCallback Done,
  bool DeleteLocalToTrash = false);

// TTerminal::SynchronizeToQueue for all checked items (Start in Background):
// one queue item per checklist item. Returns the number of queued items.
// spTimestamp checklists cannot be queued (returns 0).
int SynchronizeToQueue(TransferQueue * Queue, const SynchronizeChecklist & Checklist, const CopyParam & Param,
  int Params, bool DeleteLocalToTrash);

// TTerminal::SynchronizeChecklistCalculateSize: calculates the sizes of the
// directory items of Items that exist on one side only (local ones with
// CopyParam masks, remote ones on the worker thread) and stores them with
// UpdateDirectorySize (on the GUI thread, before Done is called).
void SynchronizeChecklistCalculateSize(Terminal * Term, SynchronizeChecklistPtr Checklist,
  const std::vector<const SynchronizeChecklist::Item *> & Items, const CopyParam & Param, int Params,
  Terminal::Callback Done);

// TCustomScpExplorerForm::DoSynchronizeMove (checklist "Move"): moves
// FileList (remote paths when Remote, local paths otherwise) to NewFileName,
// which is the new name of a single file, or with TargetIsDirectory the
// directory (created when missing) to move the files into.
void SynchronizeMove(Terminal * Term, bool Remote, const QStringList & FileList, const QString & NewFileName,
  bool TargetIsDirectory, Terminal::Callback Done);

// Checksums of SameFileChecksum (SynchronizationChecksumAlgs "sha256,sha1")
// Local file checksum with WinSCP algorithm name ("sha256", "sha-1", "md5", ...),
// lower case hex. Throws ExtException.
QString LocalFileChecksum(const QString & FileName, const QString & Alg);
// Parses the output lines of "sha256sum -- file..." (also sha1sum/md5sum,
// "shasum -a 256"; GNU escaped names "\hash  name" are decoded) into
// file name -> lower case hex checksum
QHash<QString, QString> ParseChecksumOutput(const QStringList & Lines);

// Commands > Compare Directories (TCustomDirView::ProcessChangedFiles):
// names of files (not directories) in each panel, that are missing in the
// other panel (unless ExistingOnly) or newer than their counterpart, or with
// the same time but different size (BySize). Time is compared with the
// precision of the less precise side (seconds for SFTP).
struct CompareEntry
{
  QString FileName;
  bool Directory = false;
  qint64 Size = 0;
  QDateTime Modification;
  bool SecondPrecision = false;
};
void CompareDirectories(const QList<CompareEntry> & Local, const QList<CompareEntry> & Remote,
  bool ByTime, bool BySize, bool ExistingOnly, QStringList & LocalChanged, QStringList & RemoteChanged);
// Convenience variant listing LocalDirectory (non-recursive)
void CompareDirectories(const QString & LocalDirectory, const RemoteFileList & RemoteFiles,
  bool ByTime, bool BySize, QStringList & LocalChanged, QStringList & RemoteChanged);

// ---------------------------------------------------------------------------
// Keep remote directory up to date (windows/SynchronizeController.cpp)

// TSynchronizeParamType
struct SynchronizeParamType
{
  QString LocalDirectory;
  QString RemoteDirectory;
  int Params = 0;
  int Options = 0;
};

// so* (WinSCP.ini Configuration\Interface\SynchronizeOptions)
const int soRecurse = 0x01;
const int soSynchronize = 0x02;
const int soSynchronizeAsk = 0x04;
const int soContinueOnError = 0x08;

enum SynchronizeLogEntry { slScan, slStart, slChange, slUpload, slDelete, slDirChange, slContinuedError };

// Message of a failed inotify_init1/inotify_add_watch: ENOSPC/EMFILE are
// explained with the fs.inotify.max_user_watches/max_user_instances limits
QString InotifyErrorMessage(int Error);

// Watches the local directory tree (inotify) and uploads changes (and with
// spDelete deletes remote files/directories deleted locally) to the remote
// directory. Changes are collected for ChangeDelay ms
// (KeepUpToDateChangeDelay) and synchronized directory by directory with
// SynchronizeCollect/SynchronizeApply (smRemote, spNoRecurse, spSubDirs with
// soRecurse). The Terminal must not be used for other operations while active.
class SynchronizeController : public QObject
{
  Q_OBJECT

public:
  explicit SynchronizeController(Terminal * Term, QObject * Parent = nullptr);
  ~SynchronizeController() override;

  // TSynchronizeController::StartStop(Start = true). With soSynchronize
  // full synchronization of the remote directory runs first (asynchronously).
  // Failure to watch the directory emits Aborted.
  void Start(const SynchronizeParamType & Params, const CopyParam & Param, const SynchronizeOptions & Options);
  // StartStop(Start = false); a running synchronization is cancelled
  void Stop();
  bool Active() const { return FActive; }
  bool Busy() const { return FBusy; }
  int Directories() const { return FWatches.size(); }
  void SetChangeDelay(int Ms) { FChangeDelay = Ms; }
  // GUIConfiguration MaxWatchDirectories (default 500): TooManyDirectories
  // is emitted before watching more directories
  void SetMaxWatchDirectories(int Value) { FMaxWatchDirectories = Value; }
  int MaxWatchDirectories() const { return FMaxWatchDirectories; }
  // Status of the running synchronization (directories, cancel), null when idle
  SynchronizeStatusPtr Status() const { return FStatus; }

signals:
  void Log(SynchronizeLogEntry Entry, const QString & Message, const QString & MoreMessages);
  // Watching stopped because of an error (Message empty when cancelled by
  // user); Close = the session was lost (TSynchronizeAbortEvent)
  void Aborted(bool Close, const QString & Message, const QString & MoreMessages);
  void BusyChanged(bool Busy);
  // TSynchronizeController OnTooManyDirectories (connect with a direct
  // connection): MaxDirectories directories are watched already; increase
  // MaxDirectories to continue, set Abort to stop (silently)
  void TooManyDirectories(int & MaxDirectories, bool & Abort);

private:
  Terminal * FTerminal;
  SynchronizeParamType FParams;
  CopyParam FCopyParam;
  SynchronizeOptions FOptions;
  bool FActive = false;
  bool FBusy = false;
  bool FInitialSync = false;
  int FChangeDelay = 500;
  int FMaxWatchDirectories = 500;
  int FDirectoriesLimit = 0;
  bool FTooManyAborted = false;
  SynchronizeListingCachePtr FCache;
  int FInotify = -1;
  QSocketNotifier * FNotifier = nullptr;
  // Windows: QFileSystemWatcher (ReadDirectoryChangesW); FWatches keys are then counters
  QFileSystemWatcher * FWatcher = nullptr;
  int FNextWatch = 0;
  QHash<int, QString> FWatches;       // watch descriptor -> directory
  QSet<QString> FWatchedPaths;        // directories of FWatches
  QSet<QString> FPending;             // changed directories
  QStringList FPendingOrder;
  QTimer FChangeTimer;
  SynchronizeStatusPtr FStatus;
  std::shared_ptr<bool> FAlive;
  quint64 FGeneration = 0;

  bool AddDirectory(const QString & Directory, bool Recurse, QString & Error);
  bool AllowDirectory(const QString & Directory) const;
  void RemoveWatches();
  void Rescan();
  void ReadEvents();
  void WatchedDirectoryChanged(const QString & Directory);
  void DirectoryChanged(const QString & Directory);
  void ProcessPending();
  void SynchronizeDirectory(const QString & LocalDirectory, bool Full);
  void SynchronizeDone(const QString & LocalDirectory, bool Full, const OperationResult & Result, SynchronizeChecklistPtr Checklist);
  void SetBusy(bool Busy);
  void Abort(bool Close, const QString & Message, const QString & MoreMessages);
  QString RemotePathFor(const QString & LocalDirectory) const;
};
