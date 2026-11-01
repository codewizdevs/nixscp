// Port of WinSCP source/core/Terminal.{h,cpp}
// A Terminal is one session. All protocol work runs on its own worker thread;
// the public API is asynchronous and must be called from the GUI thread.
#pragma once

#include "core/FileSystem.h"
#include "core/SessionData.h"
#include "core/CopyParam.h"

#include <QElapsedTimer>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QThread>
#include <QVector>
#include <atomic>
#include <functional>
#include <memory>

struct OperationResult
{
  bool Ok = true;
  bool Aborted = false;
  bool Fatal = false;
  QString Message;
  QString MoreMessages;
};

// DeleteFiles Params (TTerminal::DeleteFiles)
const int dfAlternative = 0x02;   // Shift+Delete: the opposite of the session's "Preserve deleted remote files to recycle bin"
const int dfForceDelete = 0x04;   // never move to the remote recycle bin

class Terminal : public QObject
{
  Q_OBJECT

public:
  using Callback = std::function<void(const OperationResult &)>;
  using ListCallback = std::function<void(const OperationResult &, const QString & Directory, const RemoteFileList &)>;

  Terminal(const SessionData & Data, UserInterface * UI, QObject * Parent = nullptr);
  ~Terminal() override;

  const SessionData & Data() const { return FData; }
  bool Active() const { return FActive; }
  bool Busy() const { return FBusy > 0; }
  QString CurrentDirectory() const { return FCurrentDirectory; }
  QString ProtocolName() const { return FProtocolName; }
  QString ServerInfo() const { return FServerInfo; }
  FileOperationProgress & Progress() { return FProgress; }

  void Open(Callback Done);
  void Close();
  // Opens a lost (or closed) session again (TTerminal::Reopen): reconnects
  // in the last remote directory, answering the first password prompt with
  // the remembered password. Active() again on success.
  void Reopen(Callback Done);
  void ReadDirectory(const QString & Path, ListCallback Done);
  // TTerminal::ReadDirectoryListing (script "ls"): lists Path, reopening a
  // lost connection (CustomReadDirectory); neither the directory cache nor
  // the current directory is used or changed
  void ReadDirectoryListing(const QString & Path, ListCallback Done);
  void CopyToLocal(const QStringList & RemotePaths, const QString & TargetDir, const CopyParam & Param, bool Move, Callback Done);
  void CopyToRemote(const QStringList & LocalPaths, const QString & TargetDir, const CopyParam & Param, bool Move, Callback Done);
  // Deletes recursively; files are moved to the remote recycle bin instead
  // when the session has "Preserve deleted remote files to recycle bin" on
  // (Params: dfAlternative, dfForceDelete)
  void DeleteFiles(const QStringList & Paths, Callback Done, int Params = 0);
  // Renames/moves; asks before replacing an existing target (DoRenameOrCopyFile)
  void RenameFile(const QString & From, const QString & To, Callback Done);
  void CreateDirectory(const QString & Path, int Mode, Callback Done);
  // AddXToDirectories: "Add X to directories" of the Properties dialog
  // (TRights::AddExecute for directories: x where r)
  void ChangeFileProperties(const QStringList & Paths, unsigned int Mode, bool Recursive, Callback Done,
    bool AddXToDirectories = false);
  void CreateLink(const QString & Path, const QString & Target, Callback Done);

  // Generic hook for features built on top of the session (synchronization,
  // find files, scripting...): Job runs on the worker thread with exclusive
  // access to the file system; throw ExtException/FatalException/AbortException
  // as the built-in operations do. Done is called on the GUI thread.
  // The job must not capture GUI objects it touches; pass results via shared_ptr.
  using WorkerJob = std::function<void(FileSystem & Fs, FileOperationProgress & Progress, UserInterface & UI)>;
  void RunOnWorker(WorkerJob Job, Callback Done);
  // Worker side, inside a RunOnWorker job (TTerminal::CustomReadDirectory):
  // lists Path, reopening a lost connection (TRobustOperationLoop). A reopen
  // replaces the file system, so the job has to use WorkerFileSystem()
  // afterwards instead of the FileSystem it was given.
  RemoteFileList ReadDirectoryOnWorker(const QString & Path);
  FileSystem & WorkerFileSystem();

  // Console (TTerminal::AnyCommand, forms/Console): runs Command in
  // CurrentDirectory() - in the session shell for SCP, in a separate exec
  // channel of the same connection for SFTP. Output is called on the GUI
  // thread for each line (StdError: line came from stderr). Done gets the
  // exit code (-1 = unknown) and the working directory after the command
  // ("cd" is tracked), which also becomes CurrentDirectory(). The command
  // can be interrupted with Progress().Cancel().
  using CommandOutputCallback = std::function<void(const QString & Line, bool StdError)>;
  using CommandCallback = std::function<void(const OperationResult & Result, int ExitCode, const QString & Directory)>;
  void AnyCommand(const QString & Command, CommandOutputCallback Output, CommandCallback Done);
  // Whether the protocol supports AnyCommand (fcAnyCommand); valid after Open
  bool CanAnyCommand() const { return FCanAnyCommand; }

  // TTerminal::IsCapable; valid after Open (before Open: the protocol's static answer)
  bool IsCapable(FSCapability Capability) const;
  // Static capabilities of a protocol: the IsCapable tables of WinSCP's
  // protocol classes, reduced to what NixSCP's implementations support
  static bool ProtocolCapability(TFSProtocol Protocol, FSCapability Capability);

  // Password or passphrase typed at a prompt of this session
  // (TTerminal::FRememberedPassword), kept only with Settings::RememberPassword.
  // A reconnect answers the first password prompt with it. Thread safe.
  QString RememberedPassword() const;
  void SetRememberedPassword(const QString & Password);

  // Behaviour settings (Preferences; TConfiguration / TGUIConfiguration).
  // The defaults are WinSCP's defaults; the GUI applies the user's
  // preferences with SetSettings(Terminal::ConfigurationSettings()).
  struct Settings
  {
    int ReopenAuto = 5000;           // SessionReopenAuto: reconnect automatically after (ms) when the
                                     // connection breaks during an operation; 0 = ask without countdown
    int ReopenTimeout = 0;           // SessionReopenTimeout: keep reconnecting for (ms), 0 = forever
    int BackgroundReopen = 0;        // background (queue) sessions: reconnect without asking after (ms), 0 = ask
    bool ContinueOnError = false;    // ContinueOnError (GUI): skip failing files, report all errors at the end
    bool ConfirmOverwriting = true;  // ConfirmOverwriting, for rename/move onto an existing file
    bool RememberPassword = true;    // QueueRememberPassword || PuttyPassword
  };
  void SetSettings(const Settings & ASettings);
  const Settings & GetSettings() const { return FSettings; }
  // The user answered Abort to reconnecting in the last operation (its
  // Disconnected signal then needs no automatic reconnect countdown)
  bool ReopenDeclined() const { return FReopenDeclined; }
  // Settings from Configuration::Instance() (GUI thread)
  static Settings ConfigurationSettings();

  // ---- Properties and server side operations (WP05) ----

  // TTerminal::ChangeFilesProperties: owner, group and tri-state rights of
  // Paths (only the bits defined in Properties change on each file),
  // recursively with Properties.Recursive. Symbolic links are skipped.
  void ChangeFilesProperties(const QStringList & Paths, const RemoteProperties & Properties, Callback Done);
  // TCreateDirectory with properties: mkdir, then the properties (Terminal::CreateDirectory
  // + DoChangeFileProperties when the protocol can change them)
  void CreateDirectory(const QString & Path, const RemoteProperties & Properties, Callback Done);

  // TCalculateSizeStats
  struct CalculateSizeStats
  {
    int Files = 0;
    int Directories = 0;
    int SymLinks = 0;
  };
  // TTerminal::CalculateFilesSize: total size of Paths (directories
  // recursively; symlinked directories only with FollowDirectorySymlinks).
  // Sizes has the size of each Paths item (Calculate Directory Sizes).
  // Cancel with Progress().Cancel().
  using CalculateSizeCallback = std::function<void(const OperationResult & Result, qint64 Size,
    const CalculateSizeStats & Stats, const QList<qint64> & Sizes)>;
  void CalculateFilesSize(const QStringList & Paths, CalculateSizeCallback Done);

  // TTerminal::GetSupportedChecksumAlgs (valid after Open): the protocol's
  // own algorithms (SFTP check-file, FTP HASH/X*), otherwise those of the
  // shell commands (SCP, SFTP secondary shell), WinSCP names
  QStringList SupportedChecksumAlgs() const;
  // TTerminal::GetShellChecksumAlgDefs: "sha-256" -> "sha256sum" ... (empty when unknown)
  static QString ShellChecksumCommand(const QString & Alg);
  static QStringList ShellChecksumAlgs();
  // TTerminal::CalculateFilesChecksum: OnChecksum (GUI thread) gets each
  // file's name (directories are processed recursively, names relative to
  // the selected directory), the normalized algorithm and the lower case hex
  // checksum. Cancel with Progress().Cancel().
  using ChecksumCallback = std::function<void(const QString & FileName, const QString & Alg, const QString & Checksum)>;
  void CalculateFilesChecksum(const QString & Alg, const QStringList & Paths, ChecksumCallback OnChecksum, Callback Done);

  // TDirectRemoteCopy: how Duplicate can copy Paths on this session
  enum DirectRemoteCopy { drcDisallow, drcAllow, drcConfirmCommandSession, drcConfirmCommandSessionDirs };
  DirectRemoteCopy AllowDirectRemoteCopy(bool AnyDirectory) const;
  // TTerminal::CopyFiles (Duplicate): copies each of Paths to
  // TargetDir/MaskFileName(name, FileMask) on the server (fcRemoteCopy, or
  // "cp -p -r -f" in a separate shell channel); asks before overwriting
  void CopyFiles(const QStringList & Paths, const QString & TargetDir, const QString & FileMask, Callback Done);
  // TTerminal::MoveFiles (Move To): renames each of Paths to TargetDir/MaskFileName(name, FileMask)
  void MoveFiles(const QStringList & Paths, const QString & TargetDir, const QString & FileMask, Callback Done);
  // TTerminal::CreateLink: symbolic (fcSymbolicLink) or hard (fcHardLink) link Path pointing to Target
  void CreateLink(const QString & Path, const QString & Target, bool Symbolic, Callback Done);
  // TTerminal::SpaceAvailable (fcCheckingSpaceAvailable)
  using SpaceAvailableCallback = std::function<void(const OperationResult & Result, const SpaceAvailableInfo & Space)>;
  void SpaceAvailable(const QString & Path, SpaceAvailableCallback Done);
  // TTerminal::LookupUsersGroups: user and group names the session knows
  // (SFTP: seen in listings; SCP: "groups" with LookupUserGroups)
  using UsersGroupsCallback = std::function<void(const OperationResult & Result, const QStringList & Users, const QStringList & Groups)>;
  void LookupUsersGroups(UsersGroupsCallback Done);

signals:
  void Disconnected(const QString & Message, const QString & MoreMessages);
  void BusyChanged(bool Busy);
  // The connection was lost during an operation and has been reopened
  void Reconnected();

private:
  struct TransferItem
  {
    QString Source;
    QString Target;
    bool Directory = false;
    qint64 Size = 0;
    QDateTime Modification;
    unsigned int Mode = 0;
  };

  // TBatchOverwrite
  enum BatchOverwrite { boNo, boAll, boNone, boOlder, boAlternateResume, boAppend, boResume };
  // TSFTPOverwriteMode
  enum OverwriteMode { omOverwrite, omAppend, omResume };
  // Worker side state of one operation (batch answers of TFileOperationProgressType,
  // settings snapshot taken on the GUI thread when the operation is posted)
  struct OperationState
  {
    Settings Options;              // snapshot of FSettings
    BatchOverwrite Batch = boNo;
    bool AnySkipped = false;
    bool ReopenDeclined = false;   // the user did not want to reconnect (do not ask again in outer loops)
    QStringList Errors;            // collected with ContinueOnError
  };
  // Decisions about one file kept across retries (FileOperationLoop / reconnect)
  struct FileAttempt
  {
    bool Retrying = false;         // after Retry or reconnect: no confirmations, resume automatically
    bool Confirmed = false;        // overwrite confirmed, Mode applies
    OverwriteMode Mode = omOverwrite;
    QString Target;                // target, possibly renamed by "New name"
  };

  SessionData FData;
  // Forwards prompts to the real UI, but answers "cancel" without asking once
  // the terminal is being destroyed (so the destructor never waits on a prompt).
  // Also implements the remembered password (TTerminal::DoPromptUser).
  class UIProxy : public UserInterface
  {
  public:
    UIProxy(Terminal * ATerminal, UserInterface * AUI, const std::atomic<bool> & ADestroying) :
      FTerminal(ATerminal), FUI(AUI), FDestroying(ADestroying) {}
    HostKeyAnswer VerifyHostKey(const QString & Host, int Port, const QString & KeyType,
      const QString & FingerprintSHA256, const QString & FingerprintMD5, HostKeyState State) override
    {
      return FDestroying ? HostKeyCancel : FUI->VerifyHostKey(Host, Port, KeyType, FingerprintSHA256, FingerprintMD5, State);
    }
    bool PromptUser(PromptKind Kind, const QString & Name, const QString & Instructions, QList<Prompt> & Prompts) override;
    void DisplayBanner(const QString & SessionName, const QString & Banner) override
    {
      if (!FDestroying) FUI->DisplayBanner(SessionName, Banner);
    }
    unsigned int QueryUser(const QString & Query, const QString & MoreMessages, unsigned int Answers) override
    {
      return FDestroying ? static_cast<unsigned int>(qaCancel) : FUI->QueryUser(Query, MoreMessages, Answers);
    }
    unsigned int QueryUserEx(const QString & Query, const QString & MoreMessages, unsigned int Answers,
      const QueryUserParams & Params) override
    {
      return FDestroying ? static_cast<unsigned int>(qaCancel) : FUI->QueryUserEx(Query, MoreMessages, Answers, Params);
    }
  private:
    Terminal * FTerminal;
    UserInterface * FUI;
    const std::atomic<bool> & FDestroying;
  };

  std::atomic<bool> FDestroying{false};
  UIProxy FUIProxy;
  UserInterface * FUI;
  QThread FThread;
  QObject * FWorker;
  std::unique_ptr<FileSystem> FFileSystem;  // touched only on worker thread
  FileOperationProgress FProgress;
  bool FActive = false;
  bool FCanAnyCommand = false;
  int FBusy = 0;
  QString FCurrentDirectory;
  QString FProtocolName;
  QString FServerInfo;
  // Capabilities of the opened file system (cached on the worker after Open)
  std::atomic<bool> FCapabilitiesKnown{false};
  bool FCapabilities[fcCount] = {};
  // Remembered password (FRememberedPassword/Kind/Tried) and the last remote
  // directory for reconnects; shared between threads
  mutable QMutex FMutex;
  QString FRememberedPassword;
  int FRememberedPasswordKind = -1;
  bool FRememberedPasswordTried = false;
  bool FRememberedPasswordUsed = false;
  bool FRememberPassword = true;
  QString FReopenDirectory;
  // GUI thread; copied into OperationState when an operation is posted
  Settings FSettings;
  std::atomic<bool> FReopenDeclined{false};

  void Run(std::function<void()> Job, Callback Done);
  // RequireConnection: fail with "Connection has been unexpectedly closed."
  // when there is no file system (all operations but Reopen)
  void RunOperation(std::function<void(OperationState &)> Job, Callback Done, bool RequireConnection = true);
  void SetCurrentDirectory(const QString & Directory);
  void UpdateCapabilities();

  // worker thread helpers (TTerminal equivalents)
  bool WorkerCapable(FSCapability Capability) const;
  void DoOpen(FileSystem & Fs);
  void DoReopen();
  bool ContinueReopen(const QElapsedTimer & Start, const OperationState & State) const;
  bool TryReopen(const FatalException & E, OperationState & State, QElapsedTimer & Start, qint64 & LastTransferred);
  void RollbackFileProgress();
  // "Never ask again" answers of the overwrite / resume prompts (Configuration, posted to the GUI thread)
  void NeverAskAgainOverwriting(OperationState & State);
  void NeverAskAgainResume();
  bool DoQueryReopen(const ExtException & E, OperationState * State);
  bool QueryReopen(const ExtException & E, OperationState * State);
  unsigned int QueryUser(const QString & Query, const QString & MoreMessages, unsigned int Answers,
    const QueryUserParams & Params, OperationState * State);
  void FileOperationLoopQuery(const ExtException & E, OperationState & State, const QString & Message, bool AllowSkip);
  // FILE_OPERATION_LOOP: runs Step; an error asks Retry / Skip / Skip all / Abort.
  // Returns normally after success; throws SkipFileException when skipped.
  void FileOperationLoop(OperationState & State, const QString & Message, const std::function<void()> & Step, bool AllowSkip = true);
  // TRobustOperationLoop + FILE_OPERATION_LOOP around one file of a transfer:
  // reconnects when the connection breaks, retries on request.
  // Returns false when the file was skipped.
  bool ProcessFile(OperationState & State, FileAttempt & Attempt, const std::function<bool(FileAttempt &)> & Step);
  // TRetryOperationLoop for single commands: Retry / Skip / Abort; returns false when skipped
  bool RetryLoop(OperationState & State, const QString & Message, const std::function<void()> & Step);
  // Directory listing with reconnect (TTerminal::CustomReadDirectory)
  RemoteFileList ReadDirectoryRobust(const QString & Path, OperationState & State);
  // State of the operation running on the worker (ReadDirectoryOnWorker)
  OperationState * FWorkerState = nullptr;
  BatchOverwrite EffectiveBatchOverwrite(const CopyParam & Param, const OperationState & State, bool ToRemote, bool Special) const;
  // TTerminal::ConfirmFileOverwrite + the protocol specific parts of
  // TSFTPFileSystem::SFTPConfirmOverwrite / TFTPFileSystem::ConfirmOverwrite.
  // Returns false to skip the file; NewName is set for "New name".
  bool ConfirmOverwrite(const QString & SourceFullFileName, const QString & TargetFileName, bool ToRemote,
    qint64 SourceSize, const QDateTime & SourceTime, qint64 DestSize, const QDateTime & DestTime,
    bool AsciiTransfer, CopyParam & Param, OperationState & State, OverwriteMode & Mode, QString & NewName);
  bool ConfirmResume(const QString & FileName, bool PartialBiggerThanSource, CopyParam & Param, OperationState & State,
    const FileAttempt & Attempt);
  void ScanRemote(const QString & Path, const RemoteFile & File, const QString & Target, QList<TransferItem> & Items,
    const CopyParam & Param, OperationState & State);
  void ScanLocal(const QString & Path, const QString & Target, QList<TransferItem> & Items, QSet<QString> & Visited,
    const CopyParam & Param, OperationState & State);
  bool DownloadFile(const TransferItem & Item, CopyParam & Param, OperationState & State, FileAttempt & Attempt);
  bool UploadFile(const TransferItem & Item, CopyParam & Param, OperationState & State, FileAttempt & Attempt);
  // State of each item of a transfer: SCP sessions transfer the items in one
  // scp batch (tsBatch), anything the batch cannot do goes one by one
  // (tsPerFile: UploadFile / DownloadFile, also all items of other protocols)
  enum TransferState { tsPerFile, tsBatch, tsDone, tsSkipped };
  // TSCPFileSystem::CopyToRemote / CopyToLocal: overwrite confirmations
  // against the target listings (local files) before the batch starts
  void ScpPrepareUpload(const QList<TransferItem> & Items, CopyParam & Param, OperationState & State,
    QVector<TransferState> & States, QVector<FileAttempt> & Attempts);
  void ScpPrepareDownload(const QList<TransferItem> & Items, CopyParam & Param, OperationState & State,
    QVector<TransferState> & States, QVector<FileAttempt> & Attempts);
  // Transfers the tsBatch items with ScpFileSystem::UploadBatch / DownloadBatch:
  // per file errors ask Retry / Skip / Skip all / Abort (Retry continues the
  // file one by one after the batch), "skip file" and reconnects start a new
  // batch for the rest. Items the batch did not finish become tsPerFile.
  // TimesSet: directories whose time the batch set (upload).
  void ScpBatchTransfer(bool ToRemote, const QStringList & Sources, const QString & TargetDir,
    const QList<TransferItem> & Items, CopyParam & Param, OperationState & State,
    QVector<TransferState> & States, QVector<FileAttempt> & Attempts, QVector<bool> & TimesSet);
  // Remote recycle bin (TTerminal::IsRecycledFile / RecycleFile)
  bool IsRecycledFile(const QString & Path) const;
  void RecycleFile(const QString & Path, OperationState & State);
  void DeleteRemoteRecursive(const QString & Path, const RemoteFile & File, OperationState & State);
  // TTerminal::DoRenameOrCopyFile (Rename only); returns false when not renamed
  bool DoRenameFile(const QString & From, const QString & To, OperationState & State, bool Move, bool Batch);
  void ChangeModeRecursive(const QString & Path, unsigned int Mode, bool Recursive, bool AddXToDirectories, OperationState & State);
  // WP05 worker helpers
  mutable QMutex FChecksumAlgsMutex;
  QStringList FChecksumAlgs;
  void UpdateChecksumAlgs();
  void ChangePropertiesRecursive(const QString & Path, const RemoteFile & File, const RemoteProperties & Properties,
    bool TopLevel, OperationState & State);
  qint64 CalculateSizeRecursive(const QString & Path, const RemoteFile & File, CalculateSizeStats & Stats, OperationState & State);
  // TTerminal::DoRenameOrCopyFile; returns false when not renamed/copied
  bool DoRenameOrCopyFile(bool Rename, const QString & From, const RemoteFile * File, const QString & To,
    OperationState & State, bool Move, bool Batch);
  void CopyFileOnServer(const QString & From, const RemoteFile & File, const QString & To);
  void ChecksumFiles(const QString & Alg, const QString & Path, const RemoteFile & File, const QString & DisplayName,
    const std::function<void(const QString &, const QString &)> & Report, OperationState & State);
};
