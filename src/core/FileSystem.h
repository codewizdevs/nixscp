// Port of WinSCP source/core/FileSystems.h (TCustomFileSystem),
// source/core/Exceptions.h (ExtException), source/core/FileOperationProgress.h
// and the prompt part of source/core/Interface.h.
#pragma once

#include "core/RemoteFile.h"

#include <QString>
#include <QStringList>
#include <QMutex>
#include <QThread>
#include <QElapsedTimer>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>

class ExtException : public std::exception
{
public:
  explicit ExtException(const QString & Message, const QString & MoreMessages = QString()) :
    FMessage(Message), FMoreMessages(MoreMessages), FUtf8(Message.toUtf8()) {}
  const QString & Message() const { return FMessage; }
  const QString & MoreMessages() const { return FMoreMessages; }
  const char * what() const noexcept override { return FUtf8.constData(); }

private:
  QString FMessage;
  QString FMoreMessages;
  QByteArray FUtf8;
};

// Thrown when the connection itself is lost (TTerminal::FatalError)
class FatalException : public ExtException
{
public:
  using ExtException::ExtException;
};

// Thrown when user cancels an operation (EAbort)
class AbortException : public ExtException
{
public:
  AbortException() : ExtException(QStringLiteral("Operation was aborted")) {}

protected:
  explicit AbortException(const QString & Message, const QString & MoreMessages = QString()) :
    ExtException(Message, MoreMessages) {}
};

// The current file of an operation is skipped (ESkipFile): "Skip" answer of
// the per-file error prompt, "No" to an overwrite confirmation, or the
// "Skip this file" button of the progress dialog (FileOperationProgress::CancelFile).
// Derived from AbortException, so that protocol code cleaning up after an
// abort treats it the same; Terminal catches it per file and continues.
class SkipFileException : public AbortException
{
public:
  SkipFileException() : AbortException(QStringLiteral("File skipped")) {}
};

// TFileOperationProgressType; written by worker thread, read by GUI thread
class FileOperationProgress
{
public:
  enum Operation { Copy, Move, Delete, SetProperties, Calculate };
  enum Side { ToRemote, ToLocal };

  void Start(Operation AOperation, Side ASide, int ACount)
  {
    QMutexLocker Lock(&FMutex);
    FOperation = AOperation;
    FSide = ASide;
    FCount = ACount;
    FFilesFinished = 0;
    FTotalSize = 0;
    FTotalTransferred = 0;
    FFileName.clear();
    FFileSize = 0;
    FFileTransferred = 0;
    FTimer.start();
    FCancel = false;
    FCancelFile = false;
    FSkipToAll = false;
    FInProgress = true;
  }
  void Stop() { QMutexLocker Lock(&FMutex); FInProgress = false; }
  void SetFile(const QString & AFileName, qint64 ASize)
  {
    QMutexLocker Lock(&FMutex);
    FFileName = AFileName;
    FFileSize = ASize;
    FFileTransferred = 0;
    FCancelFile = false;
  }
  void AddTotalSize(qint64 Size) { QMutexLocker Lock(&FMutex); FTotalSize += Size; }
  void AddTransferred(qint64 Bytes)
  {
    QMutexLocker Lock(&FMutex);
    FFileTransferred += Bytes;
    FTotalTransferred += Bytes;
  }
  // Offset of a resumed transfer counts as transferred (TFileOperationProgressType::AddResumed)
  void AddResumed(qint64 Bytes) { AddTransferred(Bytes); }
  void FileFinished() { QMutexLocker Lock(&FMutex); FFilesFinished++; }

  // Speed limit (TFileOperationProgressType::CPSLimit), bytes/s, 0 = unlimited;
  // can be changed while the operation runs (from any thread)
  void SetCPSLimit(unsigned long Limit) { FCPSLimit = Limit; }
  unsigned long CPSLimit() const { return FCPSLimit; }
  // Worker thread: returns how many of Size bytes may be transferred now
  // (TFileOperationProgressType::AdjustToCPSLimit: per-second budget).
  // With Wait, blocks until the next second when the budget is exhausted
  // (never returns 0, returns Size when cancelled so the caller gets to
  // CheckCancel); without Wait returns 0 instead of blocking.
  unsigned long AdjustToCPSLimit(unsigned long Size, bool Wait = true)
  {
    for (;;)
    {
      unsigned long Limit = FCPSLimit;
      if ((Limit == 0) || FCancel || (Size == 0))
      {
        return Size;
      }
      qint64 Now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
      qint64 Second = Now / 1000;
      if (Second != FLastSecond)
      {
        FRemainingCPS = Limit;
        FLastSecond = Second;
      }
      if (FRemainingCPS > 0)
      {
        unsigned long Result = std::min(Size, FRemainingCPS);
        FRemainingCPS -= Result;
        return Result;
      }
      if (!Wait)
      {
        return 0;
      }
      QThread::msleep(static_cast<unsigned long>(std::min<qint64>(100, 1000 - (Now % 1000)) + 1));
    }
  }
  void Cancel() { FCancel = true; }
  bool Cancelled() const { return FCancel; }
  // Worker thread, outside of a progress operation (Start resets it too)
  void ResetCancel() { FCancel = false; }
  // Skips the file being processed (csCancelFile): the next CheckCancel
  // throws SkipFileException; reset by SetFile
  void CancelFile() { FCancelFile = true; }
  // "Skip all" answer of the per-file error prompt: further errors of the
  // operation skip their file without asking (TFileOperationProgressType::SetSkipToAll)
  void SetSkipToAll() { FSkipToAll = true; }
  bool SkipToAll() const { return FSkipToAll; }
  // Pause/resume (queue items); worker blocks in CheckCancel while suspended
  void Suspend() { FSuspended = true; }
  void Resume() { FSuspended = false; }
  bool Suspended() const { return FSuspended; }
  void CheckCancel() const
  {
    while (FSuspended && !FCancel)
    {
      QThread::msleep(100);
    }
    if (FCancel) throw AbortException();
    if (FCancelFile.exchange(false)) throw SkipFileException();
  }

  struct Snapshot
  {
    Operation Op;
    Side Dir;
    QString FileName;
    qint64 FileSize, FileTransferred, TotalSize, TotalTransferred;
    int Count, FilesFinished;
    qint64 ElapsedMs;
    bool InProgress;
  };
  Snapshot Get() const
  {
    QMutexLocker Lock(&FMutex);
    return Snapshot{FOperation, FSide, FFileName, FFileSize, FFileTransferred, FTotalSize, FTotalTransferred,
      FCount, FFilesFinished, FTimer.isValid() ? FTimer.elapsed() : 0, FInProgress};
  }

private:
  mutable QMutex FMutex;
  Operation FOperation = Copy;
  Side FSide = ToRemote;
  QString FFileName;
  qint64 FFileSize = 0, FFileTransferred = 0, FTotalSize = 0, FTotalTransferred = 0;
  int FCount = 0, FFilesFinished = 0;
  QElapsedTimer FTimer;
  bool FInProgress = false;
  std::atomic<bool> FCancel{false};
  mutable std::atomic<bool> FCancelFile{false};
  std::atomic<bool> FSkipToAll{false};
  std::atomic<bool> FSuspended{false};
  std::atomic<unsigned long> FCPSLimit{0};
  unsigned long FRemainingCPS = 0;   // worker thread only
  qint64 FLastSecond = -1;
};

// Options of a single file transfer (FileSystem::Download/Upload)
struct TransferParams
{
  // Continue at this offset; the target (local file for download, remote
  // file for upload) already contains the first ResumeOffset bytes
  qint64 ResumeOffset = 0;
  // On failure/cancel keep the partially transferred target (resumable
  // ".filepart" transfer); otherwise it is deleted
  bool KeepPartial = false;
  // Text mode upload conversions (TFileBuffer::Convert cpRemoveBOM/cpRemoveCtrlZ)
  bool RemoveBOM = false;
  bool RemoveCtrlZ = false;
  // Text mode (OperationProgress->AsciiTransfer): SFTP and SCP convert the
  // line endings between LocalEOL and SessionData::EOLType when they differ
  // (TFileBuffer::Convert), FTP transfers with TYPE A. Never combined with
  // ResumeOffset > 0 (Terminal does not resume text transfers).
  bool AsciiTransfer = false;
  // Upload only: write the whole source after the existing contents of the
  // remote target ("Append" answer of the overwrite prompt, omAppend).
  // Terminal sets it only when IsCapable(fcAppendUpload) is true.
  bool Append = false;
};

struct Prompt
{
  QString Text;
  bool Echo = false;
  QString Result;
};

// Answers to TTerminal::QueryUser-style questions
// (qaAll and qaIgnore are always shown with an alias caption, see QueryButtonAlias)
enum QueryAnswer { qaYes = 1, qaNo = 2, qaOK = 4, qaCancel = 8, qaYesToAll = 16, qaNoToAll = 32, qaAbort = 64, qaRetry = 128, qaSkip = 256, qaNeverAskAgain = 512,
  qaIgnore = 1024, qaAll = 2048, qaHelp = 4096 };

// TQueryType (icon / title of the message box)
enum QueryType { qtConfirmation, qtWarning, qtError, qtInformation };

// TQueryParams::Params flags
const unsigned int qpFatalAbort = 0x01;           // Abort closes the session
const unsigned int qpNeverAskAgainCheck = 0x02;   // show "Never ask me again"; checked + positive answer returns qaNeverAskAgain
const unsigned int qpAllowContinueOnError = 0x04; // may be auto-answered when Preferences "Continue on error" is on
const unsigned int qpIgnoreAbort = 0x08;
const unsigned int qpWaitInBatch = 0x10;

// TQueryButtonAlias: custom caption of an answer button, optionally shown as
// an item of another button's drop-down (GroupWith), selected directly by
// clicking that button with GroupedModifiers held (e.g. Ctrl+Yes = "Newer only")
struct QueryButtonAlias
{
  unsigned int Button = 0;
  QString Alias;                       // caption with & accelerator; empty = default caption
  unsigned int GroupWith = 0;          // 0 = own button
  Qt::KeyboardModifiers GroupedModifiers = Qt::NoModifier;

  static QueryButtonAlias YesToAllGroupedWithYes() { return QueryButtonAlias{qaYesToAll, QString(), qaYes, Qt::ShiftModifier}; }
  static QueryButtonAlias NoToAllGroupedWithNo() { return QueryButtonAlias{qaNoToAll, QString(), qaNo, Qt::ShiftModifier}; }
  // YES_TO_NEWER_BUTTON
  static QueryButtonAlias AllAsYesToNewerGroupedWithYes() { return QueryButtonAlias{qaAll, QStringLiteral("Ne&wer only"), qaYes, Qt::ControlModifier}; }
  // RENAME_BUTTON
  static QueryButtonAlias IgnoreAsRenameGroupedWithNo() { return QueryButtonAlias{qaIgnore, QStringLiteral("New na&me"), qaNo, Qt::ControlModifier}; }
};

// TQueryParams (subset)
struct QueryUserParams
{
  unsigned int Params = 0;             // qp* flags
  QList<QueryButtonAlias> Aliases;
  // Countdown: when Timeout (ms) > 0, TimeoutAnswer is chosen automatically
  // after Timeout ms; the default button shows "Caption (N s)"
  unsigned int Timeout = 0;
  unsigned int TimeoutAnswer = 0;
  unsigned int NoBatchAnswers = 0;     // answers not usable in batch (scripting) mode
  QString HelpKeyword;                 // winscp.net/eng/docs/<keyword>
  QueryType Type = qtConfirmation;
};

// Implemented by GUI; all methods are called from a worker thread and block.
class UserInterface
{
public:
  virtual ~UserInterface() = default;

  enum HostKeyState { HostKeyUnknown, HostKeyChanged };
  enum HostKeyAnswer { HostKeyAccept, HostKeyOnce, HostKeyCancel };
  virtual HostKeyAnswer VerifyHostKey(const QString & Host, int Port, const QString & KeyType,
    const QString & FingerprintSHA256, const QString & FingerprintMD5, HostKeyState State) = 0;

  // pkFileName: one echoed prompt for a file name (e.g. "Transfer under different name")
  enum PromptKind { pkPassword, pkPassphrase, pkKeybInteractive, pkUserName, pkFileName };
  virtual bool PromptUser(PromptKind Kind, const QString & Name, const QString & Instructions, QList<Prompt> & Prompts) = 0;

  virtual void DisplayBanner(const QString & SessionName, const QString & Banner) = 0;

  virtual unsigned int QueryUser(const QString & Query, const QString & MoreMessages, unsigned int Answers) = 0;

  // TTerminal::QueryUser with TQueryParams: button aliases, countdown,
  // "never ask again" check box, query type. Query may contain WinSCP's
  // "**main instruction**" markup. Implementations without support for the
  // extras fall back to the plain QueryUser, without the answers that only
  // make sense with their alias caption (qaAll, qaIgnore and captioned
  // drop-down items such as "Append").
  virtual unsigned int QueryUserEx(const QString & Query, const QString & MoreMessages, unsigned int Answers,
    const QueryUserParams & Params)
  {
    unsigned int PlainAnswers = Answers & ~static_cast<unsigned int>(qaAll | qaIgnore);
    for (const QueryButtonAlias & Alias : Params.Aliases)
    {
      if (!Alias.Alias.isEmpty() && (Alias.GroupWith != 0))
      {
        PlainAnswers &= ~Alias.Button;
      }
    }
    return QueryUser(Query, MoreMessages, (PlainAnswers != 0) ? PlainAnswers : Answers);
  }
};

// TFSCapability (core/SessionInfo.h), same order; NixSCP additions before fcCount
enum FSCapability { fcUserGroupListing, fcModeChanging, fcAclChangingFiles, fcGroupChanging,
  fcOwnerChanging, fcGroupOwnerChangingByID, fcAnyCommand, fcHardLink,
  fcSymbolicLink, fcResolveSymlink,
  fcTextMode, fcRename, fcNativeTextMode, fcNewerOnlyUpload, fcRemoteCopy,
  fcTimestampChanging, fcRemoteMove, fcLoadingAdditionalProperties,
  fcCheckingSpaceAvailable, fcIgnorePermErrors, fcCalculatingChecksum,
  fcModeChangingUpload, fcPreservingTimestampUpload, fcShellAnyCommand,
  fcSecondaryShell, fcRemoveCtrlZUpload, fcRemoveBOMUpload, fcMoveToQueue,
  fcLocking, fcPreservingTimestampDirs, fcResumeSupport,
  fcChangePassword, fcSkipTransfer,
  fcParallelTransfers, fcParallelFileTransfers,
  fcBackgroundTransfers,
  fcTransferOut, fcTransferIn,
  fcMoveOverExistingFile,
  fcTags,
  // NixSCP: TransferParams::Append is honoured by Upload (SFTP omAppend)
  fcAppendUpload,
  // NixSCP: Download/Upload with TransferParams::ResumeOffset > 0 directly into
  // the existing target ("Resume" answer of the overwrite prompt, omResume)
  fcResumeOverwrite,
  fcCount };

// TRemoteProperties (core/RemoteFiles.h): the properties the Properties
// dialog changes on a set of files
struct RemoteProperties
{
  // vpRights as tri-state TRights: RightsSet bits are set, RightsUnset bits
  // are cleared, the bits in neither are left as they are on each file
  // (grayed check boxes of a multiple selection, TRights::Combine)
  bool ValidRights = false;
  unsigned int RightsSet = 0;
  unsigned int RightsUnset = 0;
  bool AddXToDirectories = false;
  // vpOwner / vpGroup when not empty: name, numeric id or "name [id]"
  QString Owner;
  QString Group;
  // "Set owner, group and permissions recursively" (directories)
  bool Recursive = false;

  bool Empty() const { return !ValidRights && Owner.isEmpty() && Group.isEmpty(); }
  // All 12 bits are defined
  bool RightsDefined() const { return ((RightsSet | RightsUnset) & 07777) == 07777; }
  // TRights(Mode).Combine(Rights) + AddExecute for directories
  unsigned int CombineRights(unsigned int Mode, bool Directory) const
  {
    unsigned int Result = ((Mode & 07777) & ~RightsUnset) | (RightsSet & 07777);
    if (Directory && AddXToDirectories)
    {
      // TRights::AddExecute: x where r
      Result |= ((Result & 0400) ? 0100 : 0) | ((Result & 0040) ? 0010 : 0) | ((Result & 0004) ? 0001 : 0);
    }
    return Result;
  }
  // TRights::GetChmodStr: symbolic mode for chmod ("u=rw,g=r,o=r" when all
  // bits are defined, otherwise "u+r-w,g+x,..."); empty when nothing changes
  QString ChmodStr(bool Directory) const
  {
    static const char Who[3] = {'u', 'g', 'o'};
    unsigned int Set = RightsSet & 07777;
    unsigned int Unset = RightsUnset & 07777;
    if (Directory && AddXToDirectories)
    {
      unsigned int Combined = Set;
      Set |= ((Combined & 0400) ? 0100 : 0) | ((Combined & 0040) ? 0010 : 0) | ((Combined & 0004) ? 0001 : 0);
      Unset &= ~Set;
    }
    QStringList Parts;
    for (int Index = 0; Index < 3; Index++)
    {
      int Shift = 6 - (Index * 3);
      unsigned int Special = (Index == 0) ? 04000u : ((Index == 1) ? 02000u : 01000u);
      QString Plus, Minus;
      const char * Letters = "rwx";
      for (int Bit = 0; Bit < 3; Bit++)
      {
        unsigned int Flag = 1u << (Shift + 2 - Bit);
        if (Set & Flag) Plus += QLatin1Char(Letters[Bit]);
        else if (Unset & Flag) Minus += QLatin1Char(Letters[Bit]);
      }
      QChar SpecialChar = (Index == 2) ? QLatin1Char('t') : QLatin1Char('s');
      if (Set & Special) Plus += SpecialChar;
      else if (Unset & Special) Minus += SpecialChar;
      QString Part;
      if (!Plus.isEmpty()) Part += QLatin1Char('+') + Plus;
      if (!Minus.isEmpty()) Part += QLatin1Char('-') + Minus;
      if (!Part.isEmpty()) Parts << (QLatin1Char(Who[Index]) + Part);
    }
    return Parts.join(QLatin1Char(','));
  }
};

// TSpaceAvailable (core/FileSystems.h)
struct SpaceAvailableInfo
{
  qint64 BytesOnDevice = 0;
  qint64 UnusedBytesOnDevice = 0;
  qint64 BytesAvailableToUser = 0;
  qint64 UnusedBytesAvailableToUser = 0;
  qint64 BytesPerAllocationUnit = 0;
};

class FileSystem
{
public:
  virtual ~FileSystem() = default;

  // TCustomFileSystem::IsCapable. Terminal::IsCapable passes the static
  // answer for the protocol (Terminal::ProtocolCapability, a port of the
  // IsCapable tables of WinSCP's protocol classes, limited to what NixSCP
  // implements) as Default; protocol classes override this for answers that
  // depend on the session (server version, extensions, features).
  virtual bool IsCapable(FSCapability Capability, bool Default) const
  {
    Q_UNUSED(Capability);
    return Default;
  }

  virtual void Open() = 0;
  virtual void Close() = 0;
  virtual bool IsOpened() const = 0;
  virtual QString ProtocolName() const = 0;
  virtual QString ServerInfo() const = 0;

  virtual QString CanonicalizePath(const QString & Path) = 0;
  virtual QString HomeDirectory() = 0;
  virtual RemoteFileList ReadDirectory(const QString & Path) = 0;
  virtual RemoteFile ReadFile(const QString & Path) = 0;      // lstat; symlinks resolved for IsDirectory
  virtual bool FileExists(const QString & Path, RemoteFile * File) = 0;
  virtual void CreateDirectory(const QString & Path, int Mode) = 0;
  virtual void DeleteFile(const QString & Path) = 0;
  virtual void RemoveDirectory(const QString & Path) = 0;
  virtual void RenameFile(const QString & From, const QString & To) = 0;
  virtual void ChangeMode(const QString & Path, unsigned int Mode) = 0;
  virtual void SetModificationTime(const QString & Path, const QDateTime & Time) = 0;
  virtual void CreateLink(const QString & Path, const QString & Target) = 0;

  virtual void Download(const QString & RemotePath, const QString & LocalPath, FileOperationProgress & Progress) = 0;
  virtual void Upload(const QString & LocalPath, const QString & RemotePath, int Mode, FileOperationProgress & Progress) = 0;

  // Resumable/throttled variants; speed limit is taken from Progress.CPSLimit().
  // Default implementations support plain transfers only.
  virtual void Download(const QString & RemotePath, const QString & LocalPath, const TransferParams & Params, FileOperationProgress & Progress)
  {
    if (Params.ResumeOffset > 0)
    {
      throw ExtException(QStringLiteral("The server does not support the operation."));
    }
    Download(RemotePath, LocalPath, Progress);
  }
  virtual void Upload(const QString & LocalPath, const QString & RemotePath, int Mode, const TransferParams & Params, FileOperationProgress & Progress)
  {
    if (Params.ResumeOffset > 0)
    {
      throw ExtException(QStringLiteral("The server does not support the operation."));
    }
    Upload(LocalPath, RemotePath, Mode, Progress);
  }

  // ---- Server side operations (TCustomFileSystem); the defaults report
  // "not supported", protocols override what their IsCapable announces ----

  // TCustomFileSystem::ChangeFileProperties for one file (Terminal recurses,
  // unless ChangesPropertiesRecursively). File is the current state of Path.
  virtual void ChangeFileProperties(const QString & Path, const RemoteFile & File, const RemoteProperties & Properties)
  {
    if (!Properties.Owner.isEmpty() || !Properties.Group.isEmpty())
    {
      ChangeOwnerGroup(Path, Properties.Owner, Properties.Group);
    }
    if (Properties.ValidRights)
    {
      ChangeMode(Path, Properties.CombineRights(File.Permissions, File.IsDirectory));
    }
  }
  // ChangeFileProperties applies Properties.Recursive itself (SCP "chmod -R")
  virtual bool ChangesPropertiesRecursively() const { return false; }
  // chown/chgrp (fcOwnerChanging / fcGroupChanging); empty = keep
  virtual void ChangeOwnerGroup(const QString & Path, const QString & Owner, const QString & Group)
  {
    Q_UNUSED(Path); Q_UNUSED(Owner); Q_UNUSED(Group);
    throw ExtException(QStringLiteral("The server does not support the operation."));
  }
  // Remote copy of a file or directory (fcRemoteCopy, TCustomFileSystem::CopyFile)
  virtual void CopyFile(const QString & From, const QString & To)
  {
    Q_UNUSED(From); Q_UNUSED(To);
    throw ExtException(QStringLiteral("The server does not support the operation."));
  }
  // Hard link Path to Target (fcHardLink, CreateLink with Symbolic = false)
  virtual void CreateHardLink(const QString & Path, const QString & Target)
  {
    Q_UNUSED(Path); Q_UNUSED(Target);
    throw ExtException(QStringLiteral("The server does not support the operation."));
  }
  // fcCheckingSpaceAvailable
  virtual SpaceAvailableInfo SpaceAvailable(const QString & Path)
  {
    Q_UNUSED(Path);
    throw ExtException(QStringLiteral("The server does not support the operation."));
  }
  // Algorithms of the protocol's own checksum support (fcCalculatingChecksum),
  // WinSCP names ("sha-256", "sha-1", "md5", ...)
  virtual QStringList ChecksumAlgorithms() const { return QStringList(); }
  // Lower case hex checksum of a file (fcCalculatingChecksum)
  virtual QString CalculateFileChecksum(const QString & Path, const QString & Alg)
  {
    Q_UNUSED(Path); Q_UNUSED(Alg);
    throw ExtException(QStringLiteral("The server does not support the operation."));
  }
  // TCustomFileSystem::LookupUsersGroups: names known to the session, for
  // the owner/group combos of the Properties dialog
  virtual void LookupUsersGroups(QStringList & Users, QStringList & Groups)
  {
    Q_UNUSED(Users); Q_UNUSED(Groups);
  }
};
