// Port of WinSCP source/core/Terminal.cpp: TTerminal (operations,
// FileOperationLoop, TRobustOperationLoop, TRetryOperationLoop, Reopen,
// ConfirmFileOverwrite, DoRenameOrCopyFile, RecycleFile), the overwrite and
// resume prompts of source/core/SftpFileSystem.cpp (SFTPConfirmOverwrite,
// SFTPConfirmResume) and source/core/FtpFileSystem.cpp (ConfirmOverwrite),
// and the IsCapable tables of the protocol classes.
#include "core/Terminal.h"
#include "core/Platform.h"
#include "core/ActionsLog.h"
#include "core/LocalFileSystem.h"
#include "core/SessionLog.h"
#include "core/Configuration.h"
#include "core/DirectoryCache.h"
#include "core/FileMasks.h"
#include "core/FileSystemFactory.h"
#include "core/ScpFileSystem.h"
#include "core/SecureShell.h"
#include "core/SftpFileSystem.h"
#include "core/Synchronize.h"

#include <QCoreApplication>
#include <QDir>
#include <QThread>
#include <QEventLoop>
#include <QPointer>
#include <QFileInfo>
#include <QSet>
#include <QLocale>
#include <sys/stat.h>
#include <fcntl.h>
#include <cerrno>
#include <cstdio>
#include <cstring>

static void SetLocalModificationTime(const QString & Path, const QDateTime & Time)
{
  Platform::SetFileTimes(Path, Time.toSecsSinceEpoch());
}

// "**main instruction**" markup of WinSCP messages (MainInstructions)
static QString MainInstructions(const QString & S)
{
  return QStringLiteral("**%1**").arg(S);
}

static QString StripMainInstructions(QString S)
{
  return S.remove(QStringLiteral("**"));
}

// FormatSize ("#,##0")
static QString FormatSize(qint64 Size)
{
  return QLocale().toString(Size);
}

// UserModificationStr (mfFull) / TIME_UNKNOWN
static QString UserModificationStr(const QDateTime & Time)
{
  if (!Time.isValid())
  {
    return QStringLiteral("Unknown");
  }
  QDateTime Local = Time.toLocalTime();
  return QLocale().toString(Local.date(), QLocale::ShortFormat) + QLatin1Char(' ') + Local.time().toString(QStringLiteral("HH:mm:ss"));
}

static bool UnixSamePath(QString Path1, QString Path2)
{
  while ((Path1.size() > 1) && Path1.endsWith(QLatin1Char('/'))) Path1.chop(1);
  while ((Path2.size() > 1) && Path2.endsWith(QLatin1Char('/'))) Path2.chop(1);
  return (Path1 == Path2);
}

// The user answered Abort to an error query: the operation is aborted, the
// result keeps the error (already shown to the user as the query)
class QueryAbortException : public AbortException
{
public:
  explicit QueryAbortException(const ExtException & E) : AbortException(E.Message(), E.MoreMessages()) {}
};

// CheckCancel between files (outside of the per-file loops): a pending
// "Skip this file" (FileOperationProgress::CancelFile) clicked after the
// previous file finished must not abort the whole operation; it is dropped
// (SetFile of the next file resets it anyway)
static void CheckOperationCancel(const FileOperationProgress & Progress)
{
  try
  {
    Progress.CheckCancel();
  }
  catch (const SkipFileException &)
  {
  }
}

// ContinueAnswer (core/Common.cpp): the answer given automatically with "Continue on error"
static unsigned int ContinueOnErrorAnswer(unsigned int Answers)
{
  for (unsigned int Answer : {qaSkip, qaIgnore, qaYes, qaOK, qaRetry, qaCancel, qaNo, qaAbort})
  {
    if (Answers & Answer)
    {
      return Answer;
    }
  }
  return qaCancel;
}

// Message of an error with its details, as one entry of the error list
static QString ErrorText(const QString & Message, const QString & MoreMessages)
{
  QString Result = StripMainInstructions(Message);
  if (!MoreMessages.isEmpty())
  {
    Result += QLatin1Char('\n') + MoreMessages;
  }
  return Result;
}

// Applies a "never ask again" answer to the configuration on the GUI thread
static void PostConfigurationChange(std::function<void(Configuration &)> Change)
{
  if (QCoreApplication * App = QCoreApplication::instance())
  {
    QMetaObject::invokeMethod(App, [Change]() { Change(Configuration::Instance()); }, Qt::QueuedConnection);
  }
}

//---------------------------------------------------------------------------

bool Terminal::UIProxy::PromptUser(PromptKind Kind, const QString & Name, const QString & Instructions, QList<Prompt> & Prompts)
{
  if (FDestroying)
  {
    return false;
  }
  // TTerminal::DoPromptUser: the first password prompt of a (re)connect is
  // answered with the remembered password
  bool PassphrasePrompt = (Kind == pkPassphrase);
  bool PasswordPrompt =
    PassphrasePrompt || (Kind == pkPassword) ||
    ((Kind == pkKeybInteractive) && (Prompts.size() == 1) && !Prompts[0].Echo);
  if (PasswordPrompt)
  {
    QMutexLocker Lock(&FTerminal->FMutex);
    if (!FTerminal->FRememberedPasswordTried)
    {
      FTerminal->FRememberedPasswordTried = true;
      bool RememberedPassphrase = (FTerminal->FRememberedPasswordKind == pkPassphrase);
      if (!FTerminal->FRememberedPassword.isEmpty() && (RememberedPassphrase == PassphrasePrompt))
      {
        FTerminal->FRememberedPasswordUsed = true;
        Prompts[0].Result = FTerminal->FRememberedPassword;
        return true;
      }
    }
  }
  bool Result = FUI->PromptUser(Kind, Name, Instructions, Prompts);
  if (Result && PasswordPrompt && !Prompts.isEmpty())
  {
    QMutexLocker Lock(&FTerminal->FMutex);
    if (FTerminal->FRememberPassword)
    {
      FTerminal->FRememberedPassword = Prompts[0].Result;
      FTerminal->FRememberedPasswordKind = Kind;
    }
  }
  return Result;
}

//---------------------------------------------------------------------------

Terminal::Terminal(const SessionData & Data, UserInterface * UI, QObject * Parent) :
  QObject(Parent),
  FData(Data),
  FUIProxy(this, UI, FDestroying),
  FUI(&FUIProxy),
  FWorker(new QObject())
{
  FWorker->moveToThread(&FThread);
  connect(&FThread, &QThread::finished, FWorker, &QObject::deleteLater);
  FThread.setObjectName(QStringLiteral("Terminal %1").arg(Data.SessionName()));
  FThread.start();
  SessionLog::AttachToWorker(FWorker, FData);
}

Terminal::~Terminal()
{
  FDestroying = true;
  FProgress.Cancel();
  // Wait for the worker while still processing events: a worker blocked in a
  // GUI prompt (queued to this thread) must be able to finish, or both threads
  // would wait for each other. Own queued callbacks are ignored (FDestroying).
  QEventLoop Loop;
  QMetaObject::invokeMethod(FWorker, [this, &Loop]()
  {
    if (FFileSystem)
    {
      FFileSystem->Close();
      FFileSystem.reset();
    }
    SessionLog::DetachThread();
    QMetaObject::invokeMethod(&Loop, &QEventLoop::quit, Qt::QueuedConnection);
  }, Qt::QueuedConnection);
  Loop.exec(QEventLoop::ExcludeUserInputEvents);
  FThread.quit();
  FThread.wait();
}

void Terminal::SetSettings(const Settings & ASettings)
{
  FSettings = ASettings;
  QMutexLocker Lock(&FMutex);
  FRememberPassword = ASettings.RememberPassword;
  if (!FRememberPassword)
  {
    FRememberedPassword.clear();
    FRememberedPasswordKind = -1;
  }
}

Terminal::Settings Terminal::ConfigurationSettings()
{
  const Configuration & Config = Configuration::Instance();
  Settings Result;
  Result.ReopenAuto = Config.SessionReopenAuto;
  Result.ReopenTimeout = Config.SessionReopenTimeout;
  Result.ContinueOnError = Config.ContinueOnError;
  Result.ConfirmOverwriting = Configuration::Instance().DefaultCopyParam().ConfirmOverwriting;
  // TGUIConfiguration::GetRememberPassword
  Result.RememberPassword = Config.SessionRememberPassword || Config.PuttyPassword;
  return Result;
}

QString Terminal::RememberedPassword() const
{
  QMutexLocker Lock(&FMutex);
  return (FRememberedPasswordKind == UserInterface::pkPassphrase) ? QString() : FRememberedPassword;
}

void Terminal::SetRememberedPassword(const QString & Password)
{
  QMutexLocker Lock(&FMutex);
  FRememberedPassword = Password;
  FRememberedPasswordKind = UserInterface::pkPassword;
}

void Terminal::SetCurrentDirectory(const QString & Directory)
{
  FCurrentDirectory = Directory;
  QMutexLocker Lock(&FMutex);
  FReopenDirectory = Directory;
}

//---------------------------------------------------------------------------
// Capabilities

bool Terminal::ProtocolCapability(TFSProtocol Protocol, FSCapability Capability)
{
  switch (Protocol)
  {
    case fsSFTP:
    case fsSFTPonly:
      // TSFTPFileSystem::IsCapable (SFTP 3, as with OpenSSH)
      switch (Capability)
      {
        case fcNewerOnlyUpload:
        case fcTimestampChanging:
        case fcIgnorePermErrors:
        case fcPreservingTimestampUpload:
        case fcSecondaryShell:
        case fcRemoveCtrlZUpload:
        case fcRemoveBOMUpload:
        case fcPreservingTimestampDirs:
        case fcMoveToQueue:
        case fcResumeSupport:
        case fcSkipTransfer:
        case fcParallelTransfers:
        case fcParallelFileTransfers:
        case fcBackgroundTransfers:
        case fcRename:
        case fcRemoteMove:
        case fcSymbolicLink:
        case fcResolveSymlink:
        case fcModeChanging:
        case fcModeChangingUpload:
        case fcTextMode:
        case fcResumeOverwrite:
        case fcAppendUpload:
          return true;
        default:
          // incl. fcMoveOverExistingFile: SFTP 3 rename fails on an existing target
          return false;
      }

    case fsSCPonly:
      // TSCPFileSystem::IsCapable
      switch (Capability)
      {
        case fcModeChanging:
        case fcModeChangingUpload:
        case fcPreservingTimestampUpload:
        case fcShellAnyCommand:
        case fcSecondaryShell: // has fcShellAnyCommand
        case fcSymbolicLink:
        case fcResolveSymlink:
        case fcRename:
        case fcRemoteMove:
        case fcRemoveCtrlZUpload:
        case fcRemoveBOMUpload:
        case fcMoveOverExistingFile:
        case fcTextMode:
        case fcBackgroundTransfers:
        // NixSCP compares timestamps itself (TTerminal level)
        case fcNewerOnlyUpload:
        // NixSCP's SCP sets timestamps with touch
        case fcTimestampChanging:
        case fcPreservingTimestampDirs:
        // chgrp/chown, "ln", "cp -p -r -f", sha*sum/md5sum
        case fcUserGroupListing:
        case fcGroupChanging:
        case fcOwnerChanging:
        case fcHardLink:
        case fcRemoteCopy:
        case fcCalculatingChecksum:
          return true;
        default:
          return false;
      }

    case fsFTP:
      // TFTPFileSystem::IsCapable
      switch (Capability)
      {
        case fcResolveSymlink:
        case fcTextMode:
        case fcModeChanging:
        case fcNewerOnlyUpload:
        case fcRename:
        case fcRemoteMove:
        case fcRemoveBOMUpload:
        case fcMoveToQueue:
        case fcSkipTransfer:
        case fcParallelTransfers:
        case fcBackgroundTransfers:
        case fcPreservingTimestampUpload:
        case fcMoveOverExistingFile:
        case fcResumeSupport:
        case fcResumeOverwrite:
          return true;
        default:
          return false;
      }

    case fsWebDAV:
      // TWebDAVFileSystem::IsCapable
      switch (Capability)
      {
        case fcRename:
        case fcRemoteMove:
        case fcMoveToQueue:
        case fcPreservingTimestampUpload:
        case fcResolveSymlink:
        case fcSkipTransfer:
        case fcParallelTransfers:
        case fcBackgroundTransfers:
        case fcResumeSupport:
        // NixSCP compares timestamps itself (TTerminal level)
        case fcNewerOnlyUpload:
          return true;
        default:
          // incl. fcMoveOverExistingFile: NixSCP moves without Overwrite: T
          return false;
      }

    case fsS3:
      // TS3FileSystem::IsCapable
      switch (Capability)
      {
        case fcResolveSymlink:
        case fcRename:
        case fcRemoteMove:
        case fcMoveToQueue:
        case fcSkipTransfer:
        case fcParallelTransfers:
        case fcBackgroundTransfers:
        case fcMoveOverExistingFile:
        // NixSCP compares timestamps itself (TTerminal level)
        case fcNewerOnlyUpload:
          return true;
        default:
          return false;
      }
  }
  return false;
}

bool Terminal::WorkerCapable(FSCapability Capability) const
{
  bool Default = ProtocolCapability(FData.FSProtocol, Capability);
  if (Capability == fcAnyCommand)
  {
    Default = (dynamic_cast<AnyCommandFileSystem *>(FFileSystem.get()) != nullptr);
  }
  return FFileSystem ? FFileSystem->IsCapable(Capability, Default) : Default;
}

void Terminal::UpdateCapabilities()
{
  for (int Index = 0; Index < fcCount; Index++)
  {
    FCapabilities[Index] = WorkerCapable(static_cast<FSCapability>(Index));
  }
  FCapabilitiesKnown = true;
  UpdateChecksumAlgs();
}

bool Terminal::IsCapable(FSCapability Capability) const
{
  if ((static_cast<int>(Capability) < 0) || (static_cast<int>(Capability) >= fcCount))
  {
    return false;
  }
  if (FCapabilitiesKnown)
  {
    return FCapabilities[Capability];
  }
  if (Capability == fcAnyCommand)
  {
    return FCanAnyCommand;
  }
  return ProtocolCapability(FData.FSProtocol, Capability);
}

//---------------------------------------------------------------------------
// Operation framework

void Terminal::Run(std::function<void()> Job, Callback Done)
{
  RunOperation([Job = std::move(Job)](OperationState &) { Job(); }, std::move(Done));
}

void Terminal::RunOperation(std::function<void(OperationState &)> Job, Callback Done, bool RequireConnection)
{
  FBusy++;
  if (FBusy == 1)
  {
    emit BusyChanged(true);
  }
  OperationState InitialState;
  InitialState.Options = FSettings;
  FReopenDeclined = false;
  QMetaObject::invokeMethod(FWorker, [this, Job = std::move(Job), Done = std::move(Done), InitialState, RequireConnection]() mutable
  {
    OperationResult Result;
    OperationState & State = InitialState;
    try
    {
      if (!FFileSystem && RequireConnection)
      {
        // NOT_CONNECTED
        throw FatalException(QStringLiteral("Connection has been unexpectedly closed."));
      }
      FWorkerState = &State;
      Job(State);
      FWorkerState = nullptr;
    }
    catch (const AbortException & E)
    {
      Result.Ok = false;
      Result.Aborted = true;
      if (dynamic_cast<const QueryAbortException *>(&E) != nullptr)
      {
        Result.Message = E.Message();
        Result.MoreMessages = E.MoreMessages();
      }
    }
    catch (const FatalException & E)
    {
      SessionLog::LogException(E.Message(), E.MoreMessages());
      Result.Ok = false;
      Result.Fatal = true;
      Result.Message = E.Message();
      Result.MoreMessages = E.MoreMessages();
      if (FFileSystem)
      {
        FFileSystem->Close();
      }
    }
    catch (const ExtException & E)
    {
      SessionLog::LogException(E.Message(), E.MoreMessages());
      Result.Ok = false;
      Result.Message = E.Message();
      Result.MoreMessages = E.MoreMessages();
    }
    catch (const std::exception & E)
    {
      Result.Ok = false;
      Result.Message = QString::fromUtf8(E.what());
    }
    FWorkerState = nullptr;
    if (!State.Errors.isEmpty() && !Result.Fatal)
    {
      // "Continue on error": the skipped files are reported together
      // (TCustomScpExplorerForm::HandleErrorList, ERROR_LIST_COUNT)
      QString Summary = QStringLiteral("%1 error(s) occurred during last operation.").arg(State.Errors.size());
      QString List = State.Errors.join(QStringLiteral("\n\n"));
      if (!Result.Ok && !Result.Aborted)
      {
        List = ErrorText(Result.Message, Result.MoreMessages) + QStringLiteral("\n\n") + List;
      }
      Result.Ok = false;
      Result.Aborted = false;
      Result.Message = Summary;
      Result.MoreMessages = List;
    }
    FProgress.Stop();
    QMetaObject::invokeMethod(this, [this, Result, Done]()
    {
      if (FDestroying)
      {
        return;
      }
      FBusy--;
      bool WasActive = FActive;
      if (Result.Fatal)
      {
        FActive = false;
      }
      if (Done)
      {
        // The callback may delete this terminal
        QPointer<Terminal> Self(this);
        Done(Result);
        if (!Self || FDestroying)
        {
          return;
        }
      }
      if (FBusy == 0)
      {
        emit BusyChanged(false);
      }
      if (Result.Fatal && WasActive)
      {
        emit Disconnected(Result.Message, Result.MoreMessages);
      }
    }, Qt::QueuedConnection);
  }, Qt::QueuedConnection);
}

unsigned int Terminal::QueryUser(const QString & Query, const QString & MoreMessages, unsigned int Answers,
  const QueryUserParams & Params, OperationState * State)
{
  if ((State != nullptr) && State->Options.ContinueOnError && (Params.Params & qpAllowContinueOnError))
  {
    // TCustomScpExplorerForm::MoreMessageDialog with ContinueOnError
    State->Errors.append(ErrorText(Query, MoreMessages));
    return ContinueOnErrorAnswer(Answers);
  }
  SessionLog::LogQuery(Query, MoreMessages);
  unsigned int Answer = FUI->QueryUserEx(Query, MoreMessages, Answers, Params);
  SessionLog::LogAnswer(Answer);
  return Answer;
}

void Terminal::FileOperationLoopQuery(const ExtException & E, OperationState & State, const QString & Message, bool AllowSkip)
{
  // TTerminal::FileOperationLoopQuery
  unsigned int Answer;
  if (AllowSkip && FProgress.SkipToAll())
  {
    Answer = qaSkip;
  }
  else
  {
    unsigned int Answers = qaRetry | qaAbort | (AllowSkip ? (qaSkip | qaAll) : 0);
    QueryUserParams Params;
    Params.Params = qpAllowContinueOnError | (AllowSkip ? 0 : qpFatalAbort);
    Params.Type = qtError;
    if (AllowSkip)
    {
      // SKIP_ALL_BUTTON
      Params.Aliases.append(QueryButtonAlias{qaAll, QStringLiteral("Ski&p all")});
    }
    QString Query = Message.isEmpty() ? E.Message() : Message;
    QString More = Message.isEmpty() ? E.MoreMessages() : ErrorText(E.Message(), E.MoreMessages());
    Answer = QueryUser(Query, More, Answers, Params, &State);
    if (Answer == qaAll)
    {
      FProgress.SetSkipToAll();
      Answer = qaSkip;
    }
  }
  if (Answer == qaRetry)
  {
    return;
  }
  if ((Answer == qaSkip) && AllowSkip)
  {
    State.AnySkipped = true;
    throw SkipFileException();
  }
  FProgress.Cancel();
  throw QueryAbortException(E);
}

void Terminal::FileOperationLoop(OperationState & State, const QString & Message, const std::function<void()> & Step, bool AllowSkip)
{
  // FILE_OPERATION_LOOP_BEGIN / FILE_OPERATION_LOOP_END_EX
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
    catch (const FatalException &)
    {
      throw;
    }
    catch (const ExtException & E)
    {
      SessionAction::NoteError(E);
      FileOperationLoopQuery(E, State, Message, AllowSkip);
    }
  }
}

bool Terminal::RetryLoop(OperationState & State, const QString & Message, const std::function<void()> & Step)
{
  // TRetryOperationLoop with TTerminal::CommandError (qaRetry | qaSkip | qaAbort)
  for (;;)
  {
    try
    {
      Step();
      return true;
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
      SessionAction::NoteError(E);
      QueryUserParams Params;
      Params.Params = qpAllowContinueOnError;
      Params.Type = qtError;
      QString Query = Message.isEmpty() ? E.Message() : Message;
      QString More = Message.isEmpty() ? E.MoreMessages() : ErrorText(E.Message(), E.MoreMessages());
      unsigned int Answer = QueryUser(Query, More, qaRetry | qaSkip | qaAbort, Params, &State);
      if (Answer == qaRetry)
      {
        continue;
      }
      if (Answer == qaSkip)
      {
        State.AnySkipped = true;
        return false;
      }
      FProgress.Cancel();
      throw QueryAbortException(E);
    }
  }
}

void Terminal::RollbackFileProgress()
{
  // TFileOperationProgressType::RollbackTransfer
  FileOperationProgress::Snapshot S = FProgress.Get();
  if (S.FileTransferred != 0)
  {
    FProgress.AddTransferred(-S.FileTransferred);
  }
}

bool Terminal::ProcessFile(OperationState & State, FileAttempt & Attempt, const std::function<bool(FileAttempt &)> & Step)
{
  // TTerminal::SourceRobust / SinkRobust (TRobustOperationLoop) with the
  // FILE_OPERATION_LOOP of the individual steps
  QElapsedTimer Start;
  Start.start();
  qint64 LastTransferred = FProgress.Get().TotalTransferred;
  for (;;)
  {
    try
    {
      return Step(Attempt);
    }
    catch (const SkipFileException &)
    {
      State.AnySkipped = true;
      return false;
    }
    catch (const AbortException &)
    {
      throw;
    }
    catch (const FatalException & E)
    {
      SessionAction::NoteError(E);
      if (!TryReopen(E, State, Start, LastTransferred))
      {
        throw;
      }
    }
    catch (const ExtException & E)
    {
      SessionAction::NoteError(E);
      try
      {
        FileOperationLoopQuery(E, State, QString(), true);
      }
      catch (const SkipFileException &)
      {
        return false;
      }
    }
    // Retry: prevent overwrite and resume confirmations, resume the partial file
    RollbackFileProgress();
    Attempt.Retrying = true;
  }
}

RemoteFileList Terminal::ReadDirectoryRobust(const QString & Path, OperationState & State)
{
  // TTerminal::CustomReadDirectory
  QElapsedTimer Start;
  Start.start();
  qint64 LastTransferred = FProgress.Get().TotalTransferred;
  for (;;)
  {
    try
    {
      return FFileSystem->ReadDirectory(Path);
    }
    catch (const FatalException & E)
    {
      if (!TryReopen(E, State, Start, LastTransferred))
      {
        throw;
      }
    }
  }
}

//---------------------------------------------------------------------------
// Reconnect

void Terminal::DoOpen(FileSystem & Fs)
{
  {
    QMutexLocker Lock(&FMutex);
    FRememberedPasswordTried = false;
    FRememberedPasswordUsed = false;
  }
  SessionLog::LogStartup(FData);
  try
  {
    Fs.Open();
  }
  catch (...)
  {
    QMutexLocker Lock(&FMutex);
    if (FRememberedPasswordUsed)
    {
      // Particularly to prevent reusing a wrong password in the next attempt
      FRememberedPassword.clear();
      FRememberedPasswordKind = -1;
    }
    throw;
  }
}

void Terminal::DoReopen()
{
  // TTerminal::Reopen: the same session, in the last remote directory
  SessionData Data = FData;
  {
    QMutexLocker Lock(&FMutex);
    if (!FReopenDirectory.isEmpty())
    {
      Data.RemoteDirectory = FReopenDirectory;
    }
  }
  if (FFileSystem)
  {
    try
    {
      FFileSystem->Close();
    }
    catch (...)
    {
    }
    FFileSystem.reset();
  }
  std::unique_ptr<FileSystem> Fs = CreateFileSystem(Data, FUI);
  if (!Fs)
  {
    throw ExtException(QStringLiteral("%1 protocol is not supported yet by NixSCP.").arg(Data.ProtocolStr()));
  }
  DoOpen(*Fs);
  FFileSystem = std::move(Fs);
  UpdateCapabilities();
  QMetaObject::invokeMethod(this, [this]()
  {
    if (!FDestroying)
    {
      emit Reconnected();
    }
  }, Qt::QueuedConnection);
}

bool Terminal::ContinueReopen(const QElapsedTimer & Start, const OperationState & State) const
{
  // TTerminal::ContinueReopen
  return (State.Options.ReopenTimeout <= 0) || (Start.elapsed() < State.Options.ReopenTimeout);
}

bool Terminal::DoQueryReopen(const ExtException & E, OperationState * State)
{
  // TTerminal::DoQueryReopen ("Connection lost" with Reconnect / Abort and a countdown)
  int AutoDelay = 0;
  if (State != nullptr)
  {
    if (State->Options.BackgroundReopen > 0)
    {
      // TBackgroundTerminal::DoQueryReopen: wait and reconnect without asking
      AutoDelay = State->Options.BackgroundReopen;
    }
    else if (State->Options.ContinueOnError)
    {
      // the query would be answered "Reconnect" right away; wait as its countdown would
      AutoDelay = std::max(State->Options.ReopenAuto, 1000);
    }
  }
  if (AutoDelay > 0)
  {
    QElapsedTimer Wait;
    Wait.start();
    while (Wait.elapsed() < AutoDelay)
    {
      if (FProgress.Cancelled() || FDestroying)
      {
        return false;
      }
      QThread::msleep(50);
    }
    return true;
  }
  QueryUserParams Params;
  Params.Params = qpAllowContinueOnError;
  Params.Type = qtError;
  Params.Timeout = (State != nullptr) ? static_cast<unsigned int>(std::max(State->Options.ReopenAuto, 0)) : 0;
  Params.TimeoutAnswer = qaRetry;
  // RECONNECT_BUTTON
  Params.Aliases.append(QueryButtonAlias{qaRetry, QStringLiteral("&Reconnect")});
  bool Result = (QueryUser(E.Message(), E.MoreMessages(), qaRetry | qaAbort, Params, State) == qaRetry);
  if (!Result)
  {
    FReopenDeclined = true;
  }
  return Result;
}

bool Terminal::QueryReopen(const ExtException & E, OperationState * State)
{
  // TTerminal::QueryReopen
  if (!DoQueryReopen(E, State))
  {
    if (State != nullptr)
    {
      State->ReopenDeclined = true;
    }
    return false;
  }
  QElapsedTimer Start;
  Start.start();
  for (;;)
  {
    CheckOperationCancel(FProgress);
    try
    {
      DoReopen();
      return true;
    }
    catch (const AbortException &)
    {
      // e.g. password prompt cancelled
      if (State != nullptr)
      {
        State->ReopenDeclined = true;
      }
      return false;
    }
    catch (const ExtException & E2)
    {
      if (((State != nullptr) && !ContinueReopen(Start, *State)) || !DoQueryReopen(E2, State))
      {
        if (State != nullptr)
        {
          State->ReopenDeclined = true;
        }
        return false;
      }
    }
  }
}

bool Terminal::TryReopen(const FatalException & E, OperationState & State, QElapsedTimer & Start, qint64 & LastTransferred)
{
  // TRobustOperationLoop::TryReopen
  if (State.ReopenDeclined || FDestroying || FProgress.Cancelled())
  {
    return false;
  }
  qint64 Transferred = FProgress.Get().TotalTransferred;
  if (Transferred != LastTransferred)
  {
    // there was a transfer since the last attempt: restart the retry interval
    Start.restart();
    LastTransferred = Transferred;
  }
  else if (!ContinueReopen(Start, State))
  {
    // Retry interval expired, will not retry transfer
    return false;
  }
  return QueryReopen(E, &State);
}

//---------------------------------------------------------------------------
// Session

QString Terminal::InitialDirectory(const QString & Directory, bool Reopening)
{
  QString Dir = Directory.isEmpty() ? FFileSystem->HomeDirectory() : Directory;
  try
  {
    Dir = FFileSystem->CanonicalizePath(Dir);
    if (!Directory.isEmpty())
    {
      FFileSystem->CheckDirectory(Dir);
    }
    return Dir;
  }
  catch (const FatalException &)
  {
    throw;
  }
  catch (const AbortException &)
  {
    throw;
  }
  catch (const ExtException & E)
  {
    // TTerminal::DoStartup. A reconnect is not about the directory: the one the
    // session was in may be gone by now, and that must not cost the session
    if (!Reopening && !Directory.isEmpty() && (FData.RequireDirectories || !FData.UpdateDirectories))
    {
      QString More = E.Message();
      if (!E.MoreMessages().isEmpty())
      {
        More += QLatin1Char('\n') + E.MoreMessages();
      }
      // CHANGE_DIR_ERROR
      throw ExtException(QStringLiteral("Error changing directory to '%1'.").arg(Directory), More);
    }
    SessionLog::LogEvent(Reopening ?
      QStringLiteral("Directory '%1' cannot be opened after reconnecting, staying in the home directory.").arg(Directory) :
      QStringLiteral("Configured initial remote directory cannot be opened, staying in the home directory."));
    return FFileSystem->HomeDirectory();
  }
}

void Terminal::Open(Callback Done)
{
  auto Info = std::make_shared<QStringList>();
  bool Created = false;
  if (FData.Tunnel && !FData.IsSsh())
  {
    // Only SSH sessions tunnel (Connection > Tunnel); connecting directly
    // instead, past the tunnel the site asks for, is not an option
    QMetaObject::invokeMethod(this, [Done]()
    {
      OperationResult Result;
      Result.Ok = false;
      Result.Message = QStringLiteral("Connecting through an SSH tunnel is supported for the SFTP and SCP protocols only.");
      Done(Result);
    }, Qt::QueuedConnection);
    return;
  }
  QMetaObject::invokeMethod(FWorker, [this, &Created]()
  {
    FFileSystem = CreateFileSystem(FData, FUI);
    Created = (FFileSystem != nullptr);
    FCanAnyCommand = (dynamic_cast<AnyCommandFileSystem *>(FFileSystem.get()) != nullptr);
  }, Qt::BlockingQueuedConnection);
  if (!Created)
  {
    QString Protocol = FData.ProtocolStr();
    QMetaObject::invokeMethod(this, [Done, Protocol]()
    {
      OperationResult Result;
      Result.Ok = false;
      Result.Message = QStringLiteral("%1 protocol is not supported yet by NixSCP.").arg(Protocol);
      Done(Result);
    }, Qt::QueuedConnection);
    return;
  }
  Run([this, Info]()
  {
    DoOpen(*FFileSystem);
    QString Dir;
    try
    {
      Dir = InitialDirectory(FData.RemoteDirectory);
    }
    catch (...)
    {
      // the session did not open: the connection must not stay behind
      FFileSystem->Close();
      throw;
    }
    UpdateCapabilities();
    *Info << Dir << FFileSystem->ProtocolName() << FFileSystem->ServerInfo();
  },
  [this, Info, Done](const OperationResult & Result)
  {
    if (Result.Ok)
    {
      FActive = true;
      SetCurrentDirectory(Info->value(0));
      FProtocolName = Info->value(1);
      FServerInfo = Info->value(2);
    }
    Done(Result);
  });
}

void Terminal::Reopen(Callback Done)
{
  auto Info = std::make_shared<QStringList>();
  RunOperation([this, Info](OperationState &)
  {
    FProgress.ResetCancel();
    DoReopen();
    QString Dir;
    {
      QMutexLocker Lock(&FMutex);
      Dir = FReopenDirectory;
    }
    Dir = InitialDirectory(Dir.isEmpty() ? FData.RemoteDirectory : Dir, true);
    FCanAnyCommand = (dynamic_cast<AnyCommandFileSystem *>(FFileSystem.get()) != nullptr);
    *Info << Dir << FFileSystem->ProtocolName() << FFileSystem->ServerInfo();
  },
  [this, Info, Done](const OperationResult & Result)
  {
    if (Result.Ok)
    {
      FActive = true;
      SetCurrentDirectory(Info->value(0));
      FProtocolName = Info->value(1);
      FServerInfo = Info->value(2);
    }
    if (Done)
    {
      Done(Result);
    }
  }, false);
}

void Terminal::Close()
{
  FProgress.Cancel();
  FActive = false;
  QMetaObject::invokeMethod(FWorker, [this]()
  {
    if (FFileSystem)
    {
      FFileSystem->Close();
    }
  }, Qt::QueuedConnection);
}

RemoteFileList Terminal::ReadDirectoryOnWorker(const QString & Path)
{
  Q_ASSERT(QThread::currentThread() == FWorker->thread());
  if (FWorkerState == nullptr)
  {
    return FFileSystem->ReadDirectory(Path);
  }
  return ReadDirectoryRobust(Path, *FWorkerState);
}

FileSystem & Terminal::WorkerFileSystem()
{
  if (!FFileSystem)
  {
    throw FatalException(QStringLiteral("Connection has been unexpectedly closed."));
  }
  return *FFileSystem;
}

void Terminal::ReadDirectoryListing(const QString & Path, ListCallback Done)
{
  auto Files = std::make_shared<RemoteFileList>();
  RunOperation([this, Path, Files](OperationState & State)
  {
    // not a progress operation: a cancel of a previous operation must not prevent reconnecting
    FProgress.ResetCancel();
    *Files = ReadDirectoryRobust(Path, State);
  },
  [Path, Files, Done](const OperationResult & Result)
  {
    Done(Result, Path, *Files);
  });
}

void Terminal::ReadDirectory(const QString & Path, ListCallback Done)
{
  auto Dir = std::make_shared<QString>();
  auto Files = std::make_shared<RemoteFileList>();
  QString Current = FCurrentDirectory;
  RunOperation([this, Path, Current, Dir, Files](OperationState & State)
  {
    // not a progress operation: a cancel of a previous operation must not prevent reconnecting
    FProgress.ResetCancel();
    // CustomReadDirectory: reopen the lost connection, then list again
    QElapsedTimer Start;
    Start.start();
    qint64 LastTransferred = 0;
    for (;;)
    {
      try
      {
        // canonicalize + list, through the directory cache (WP06)
        DirectoryCaching::ReadDirectory(*FFileSystem, Path, Current, *Dir, *Files);
        return;
      }
      catch (const FatalException & E)
      {
        if (!TryReopen(E, State, Start, LastTransferred))
        {
          throw;
        }
      }
    }
  },
  [this, Dir, Files, Done](const OperationResult & Result)
  {
    if (Result.Ok)
    {
      SetCurrentDirectory(*Dir);
    }
    Done(Result, *Dir, *Files);
  });
}

//---------------------------------------------------------------------------
// Overwrite / resume confirmations

void Terminal::NeverAskAgainOverwriting(OperationState & State)
{
  State.Options.ConfirmOverwriting = false;
  QMetaObject::invokeMethod(this, [this]() { FSettings.ConfirmOverwriting = false; }, Qt::QueuedConnection);
  PostConfigurationChange([](Configuration & Config) { Config.DefaultCopyParam().ConfirmOverwriting = false; });
}

void Terminal::NeverAskAgainResume()
{
  PostConfigurationChange([](Configuration & Config) { Config.DefaultCopyParam().ConfirmResume = false; });
}

Terminal::BatchOverwrite Terminal::EffectiveBatchOverwrite(const CopyParam & Param, const OperationState & State,
  bool ToRemote, bool Special) const
{
  // TTerminal::EffectiveBatchOverwrite (NixSCP compares the timestamps
  // itself, so "newer only" works for uploads of every protocol)
  Q_UNUSED(ToRemote);
  if (Param.NewerOnly)
  {
    // no way to change batch overwrite mode when NewerOnly is on
    return boOlder;
  }
  if (!Param.ConfirmOverwriting)
  {
    // no way to change batch overwrite mode when overwrite confirmations are off
    return boAll;
  }
  BatchOverwrite Result = State.Batch;
  if (!Special && ((Result == boOlder) || (Result == boAlternateResume) || (Result == boResume)))
  {
    Result = boNo;
  }
  return Result;
}

bool Terminal::ConfirmOverwrite(const QString & SourceFullFileName, const QString & TargetFileName, bool ToRemote,
  qint64 SourceSize, const QDateTime & SourceTime, qint64 DestSize, const QDateTime & DestTime, TModificationFmt Precision,
  bool AsciiTransfer, CopyParam & Param, OperationState & State, OverwriteMode & Mode, QString & NewName)
{
  Mode = omOverwrite;
  NewName.clear();
  if (Param.Append)
  {
    // cpAppend (put -append, get -append): no question
    Mode = omAppend;
    return true;
  }
  TFSProtocol Protocol = FData.FSProtocol;
  bool Sftp = (Protocol == fsSFTP) || (Protocol == fsSFTPonly);
  bool Ftp = (Protocol == fsFTP);
  bool CanResumeInPlace = WorkerCapable(fcResumeOverwrite);
  bool CanAlternateResume = (DestSize < SourceSize) && !AsciiTransfer && CanResumeInPlace;
  // SFTP "Append": downloads append locally, uploads need the file system's support
  bool CanAppend = Sftp && (!ToRemote || WorkerCapable(fcAppendUpload));
  // FTP "Resume" (target is smaller)
  bool CanResume = Ftp && CanAlternateResume;

  // TTerminal::ConfirmFileOverwrite
  BatchOverwrite Batch = EffectiveBatchOverwrite(Param, State, ToRemote, true);
  if (((Batch == boAlternateResume) || (Batch == boResume)) && !CanAlternateResume)
  {
    Batch = EffectiveBatchOverwrite(Param, State, ToRemote, false);
  }

  unsigned int Answer = qaCancel;
  if (Batch == boNo)
  {
    if (FProgress.Cancelled())
    {
      throw AbortException();
    }
    unsigned int Answers = qaYes | qaNo | qaCancel | qaYesToAll | qaNoToAll;
    QueryUserParams Params;
    Params.Params = qpNeverAskAgainCheck;
    Params.HelpKeyword = QStringLiteral("ui_overwrite");
    if (CanAppend || CanResume)
    {
      Answers |= qaRetry;
      // APPEND_BUTTON / RESUME_BUTTON
      Params.Aliases.append(QueryButtonAlias{qaRetry, CanAppend ? QStringLiteral("A&ppend") : QStringLiteral("&Resume"), qaNo, Qt::AltModifier});
    }
    if (Protocol != fsS3)
    {
      Answers |= qaAll;
      Params.Aliases.append(QueryButtonAlias::AllAsYesToNewerGroupedWithYes());
    }
    if (Sftp || Ftp)
    {
      Answers |= qaIgnore;
      Params.Aliases.append(QueryButtonAlias::IgnoreAsRenameGroupedWithNo());
    }
    Params.Aliases.append(QueryButtonAlias::YesToAllGroupedWithYes());
    Params.Aliases.append(QueryButtonAlias::NoToAllGroupedWithNo());
    Params.NoBatchAnswers = qaIgnore | qaRetry | qaAll;
    // Side refers to destination side here: LOCAL_FILE_OVERWRITE2 / REMOTE_FILE_OVERWRITE2
    QString Message = (ToRemote ?
      QStringLiteral("**Overwrite remote file '%1'?**\n\nDestination directory already contains file '%2'.\n"
        "Choose, if you want to overwrite the file or skip this transfer and keep existing file.") :
      QStringLiteral("**Overwrite local file '%1'?**\n\nDestination directory already contains file '%2'.\n"
        "Choose, if you want to overwrite the file or skip this transfer and keep existing file."))
      .arg(TargetFileName, TargetFileName);
    // FILE_OVERWRITE_DETAILS
    Message = QStringLiteral("%1\n \nNew:      \t%2 bytes, %3\nExisting: \t%4 bytes, %5")
      .arg(Message, FormatSize(SourceSize), UserModificationStr(SourceTime), FormatSize(DestSize), UserModificationStr(DestTime));
    Answer = QueryUser(Message, QString(), Answers, Params, &State);
    switch (Answer)
    {
      case qaNeverAskAgain:
        Param.ConfirmOverwriting = false;
        NeverAskAgainOverwriting(State);
        Answer = qaYes;
        break;
      case qaYesToAll:
        Batch = boAll;
        break;
      case qaAll:
        Batch = boOlder;
        break;
      case qaNoToAll:
        Batch = boNone;
        break;
      default:
        break;
    }
    // if user has not selected another batch overwrite mode, keep the current one
    if (Batch != boNo)
    {
      State.Batch = Batch;
    }
  }

  switch (Batch)
  {
    case boAll:
      Answer = qaYes;
      break;
    case boNone:
      Answer = qaNo;
      break;
    case boOlder:
      // with the precision of the less precise side: the seconds of an FTP
      // LIST are made up, so a local file would always look newer
      Answer = (SourceTime.isValid() && DestTime.isValid() && (Precision != mfNone) &&
        IsNewerModification(SourceTime, Precision, DestTime, Precision)) ? qaYes : qaNo;
      break;
    case boAlternateResume:
      Answer = qaSkip;
      break;
    case boAppend:
    case boResume:
      Answer = qaRetry;
      break;
    default:
      break;
  }

  // TSFTPFileSystem::SFTPConfirmOverwrite
  if (CanAppend && ((Answer == qaRetry) || (Answer == qaSkip)))
  {
    BatchOverwrite ABatch = EffectiveBatchOverwrite(Param, State, ToRemote, true);
    // when mode is forced by batch, never query user
    if (ABatch == boAppend)
    {
      Mode = omAppend;
    }
    else if (CanAlternateResume && ((ABatch == boResume) || (ABatch == boAlternateResume)))
    {
      Mode = omResume;
    }
    // no other option, but append
    else if (!CanAlternateResume)
    {
      Mode = omAppend;
    }
    else
    {
      QueryUserParams Params;
      Params.HelpKeyword = QStringLiteral("resume#manual");
      // APPEND_OR_RESUME2
      unsigned int AppendAnswer = QueryUser(
        QStringLiteral("**Do you want to append file '%1' at the end of existing file?**\n\nPress 'No' to resume file transfer instead.").arg(SourceFullFileName),
        QString(), qaYes | qaNo | qaNoToAll | qaCancel, Params, &State);
      switch (AppendAnswer)
      {
        case qaYes:
          Mode = omAppend;
          break;
        case qaNo:
          Mode = omResume;
          break;
        case qaNoToAll:
          Mode = omResume;
          State.Batch = boAlternateResume;
          break;
        default:
          FProgress.Cancel();
          throw AbortException();
      }
    }
    return true;
  }
  // TFTPFileSystem::ConfirmOverwrite (resume) / alternate resume without append
  if ((Answer == qaRetry) || (Answer == qaSkip))
  {
    if (CanAlternateResume)
    {
      Mode = omResume;
      return true;
    }
    Answer = qaYes;
  }
  if (Answer == qaIgnore)
  {
    // RENAME_TITLE / RENAME_PROMPT2
    QList<Prompt> Prompts;
    Prompt NamePrompt;
    NamePrompt.Text = QStringLiteral("&New name:");
    NamePrompt.Echo = true;
    NamePrompt.Result = TargetFileName;
    Prompts.append(NamePrompt);
    if (!FUI->PromptUser(UserInterface::pkFileName, QStringLiteral("Transfer under different name"), QString(), Prompts) ||
        Prompts[0].Result.isEmpty())
    {
      FProgress.Cancel();
      throw AbortException();
    }
    NewName = Prompts[0].Result;
    return true;
  }
  switch (Answer)
  {
    case qaYes:
      return true;
    case qaNo:
      return false;
    default:
      FProgress.Cancel();
      throw AbortException();
  }
}

bool Terminal::ConfirmResume(const QString & FileName, bool PartialBiggerThanSource, CopyParam & Param, OperationState & State,
  const FileAttempt & Attempt)
{
  // TSFTPFileSystem::SFTPConfirmResume
  if (PartialBiggerThanSource)
  {
    // cpNoConfirmation: restarted without asking (TSFTPFileSystem::Source, Sink)
    if (Attempt.Retrying || !Param.ConfirmOverwriting)
    {
      return false;
    }
    QueryUserParams Params;
    Params.Params = qpAllowContinueOnError;
    Params.Type = qtWarning;
    // PARTIAL_BIGGER_THAN_SOURCE
    unsigned int Answer = QueryUser(
      QStringLiteral("Target directory contains partially transferred file '%1', which is bigger than a source file. The file will be deleted.").arg(FileName),
      QString(), qaOK | qaAbort, Params, &State);
    if (Answer != qaOK)
    {
      FProgress.Cancel();
      throw AbortException();
    }
    return false;
  }
  if (Attempt.Retrying || !Param.ConfirmResume)
  {
    return true;
  }
  QueryUserParams Params;
  Params.Params = qpAllowContinueOnError | qpNeverAskAgainCheck;
  // RESUME_TRANSFER2 ("abort" replaced with "cancel" to unify with "append/resume" query)
  unsigned int Answer = QueryUser(
    QStringLiteral("**Do you want to resume file transfer?**\n\nTarget directory contains partially transferred file '%1'.\n\n"
      "Note: Answering 'No' would delete partially transferred file and restart transfer.").arg(FileName),
    QString(), qaYes | qaNo | qaCancel, Params, &State);
  switch (Answer)
  {
    case qaNeverAskAgain:
      Param.ConfirmResume = false;
      NeverAskAgainResume();
      return true;
    case qaYes:
      return true;
    case qaNo:
      return false;
    default:
      FProgress.Cancel();
      throw AbortException();
  }
}

//---------------------------------------------------------------------------
// Transfers

static FileMasks::Params MaskParams(qint64 Size, const QDateTime & Modification)
{
  FileMasks::Params Result;
  Result.Size = Size;
  Result.Modification = Modification;
  return Result;
}

// Any file (not directory) in Items from Index on?
template <typename T>
static bool AnyFileFrom(const QList<T> & Items, qsizetype Index)
{
  for (qsizetype I = Index; I < Items.size(); I++)
  {
    if (!Items[I].Directory)
    {
      return true;
    }
  }
  return false;
}

// Is Path inside one of the directories (which could not be created)?
static bool InsideAny(const QString & Path, const QStringList & Directories)
{
  for (const QString & Directory : Directories)
  {
    QString Prefix = Directory.endsWith(QLatin1Char('/')) ? Directory : Directory + QLatin1Char('/');
    if (Path.startsWith(Prefix))
    {
      return true;
    }
  }
  return false;
}

void Terminal::ScanRemote(const QString & Path, const RemoteFile & File, const QString & Target, QList<TransferItem> & Items,
  const CopyParam & Param, OperationState & State)
{
  CheckOperationCancel(FProgress);
  // TTerminal::DoAllowRemoteFileTransfer (empty directories are checked after recursion)
  if (!Param.AllowTransfer(Path, false, File.IsDirectory, MaskParams(File.Size, File.Modification), File.IsHidden))
  {
    return;
  }
  qsizetype Index = Items.size();
  Items.append(TransferItem{Path, Target, File.IsDirectory, File.IsDirectory ? 0 : File.Size, File.Modification, File.Permissions, File.ModificationFmt});
  if (File.IsDirectory)
  {
    // shown as "Calculating directory size" by the progress dialog
    FProgress.SetFile(Path, 0);
    RemoteFileList Children;
    FileAttempt Attempt;
    if (!ProcessFile(State, Attempt, [&](FileAttempt &) { Children = FFileSystem->ReadDirectory(Path); return true; }))
    {
      // directory skipped: neither created nor transferred
      Items.resize(Index);
      return;
    }
    for (const RemoteFile & Child : Children)
    {
      // never let a hostile listing escape the target directory
      if (Child.IsParentDirectory || !IsValidListingName(Child.FileName))
      {
        continue;
      }
      // Symlinked directories are followed, as WinSCP does
      ScanRemote(UnixPath::Combine(Path, Child.FileName), Child,
        QDir(Target).filePath(CopyParam::ValidLocalFileName(Param.ChangeFileName(Child.FileName))), Items, Param, State);
    }
    if (Param.ExcludeEmptyDirectories && !AnyFileFrom(Items, Index + 1))
    {
      Items.resize(Index);
    }
  }
  else
  {
    FProgress.AddTotalSize(File.Size);
  }
}

void Terminal::ScanLocal(const QString & Path, const QString & Target, QList<TransferItem> & Items, QSet<QString> & Visited,
  const CopyParam & Param, OperationState & State)
{
  CheckOperationCancel(FProgress);
  QFileInfo Info(Path);
  try
  {
    FileOperationLoop(State, QString(), [&]()
    {
      Info.refresh();
      if (!Info.exists() && !Info.isSymLink())
      {
        throw ExtException(QStringLiteral("File or folder '%1' does not exist.").arg(Path));
      }
      if (Info.isDir() && !Info.isReadable())
      {
        // LIST_DIR_ERROR
        throw ExtException(QStringLiteral("Error listing directory '%1'.").arg(Path), QString::fromLocal8Bit(strerror(EACCES)));
      }
    });
  }
  catch (const SkipFileException &)
  {
    return;
  }
  TransferItem Item{Path, Target, Info.isDir(), Info.isDir() ? 0 : Info.size(), Info.lastModified(), 0};
  // TTerminal::DoAllowLocalFileTransfer
  if (!Param.AllowTransfer(Path, true, Item.Directory, MaskParams(Item.Size, Item.Modification),
        Info.fileName().startsWith(QLatin1Char('.'))))
  {
    return;
  }
  struct stat St;
  if (stat(QFile::encodeName(Path).constData(), &St) == 0)
  {
    Item.Mode = St.st_mode & 07777;
  }
  qsizetype Index = Items.size();
  Items.append(Item);
  if (Info.isDir())
  {
    QString Canonical = Info.canonicalFilePath();
    if (Visited.contains(Canonical))
    {
      return; // symlink loop
    }
    Visited.insert(Canonical);
    FProgress.SetFile(Path, 0);
    QDir Dir(Path);
    const QFileInfoList Entries = Dir.entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo & Child : Entries)
    {
      ScanLocal(Child.filePath(), UnixPath::Combine(Target, CopyParam::RestoreChars(Param.ChangeFileName(Child.fileName()))), Items, Visited, Param, State);
    }
    if (Param.ExcludeEmptyDirectories && !AnyFileFrom(Items, Index + 1))
    {
      Items.resize(Index);
    }
  }
  else
  {
    FProgress.AddTotalSize(Info.size());
  }
}

static void CheckRename(const QString & From, const QString & To)
{
  if (rename(QFile::encodeName(From).constData(), QFile::encodeName(To).constData()) != 0)
  {
    // RENAME_AFTER_RESUME_ERROR
    throw ExtException(QStringLiteral("Transfer was successfully finished, but temporary transfer file '%1' could not be renamed to target file name '%2'. "
      "If the problem persists, you may try to turn off transfer resume support.").arg(QFileInfo(From).fileName(), QFileInfo(To).fileName()),
      QString::fromLocal8Bit(strerror(errno)));
  }
}

// Appends the downloaded Source at the end of Target and deletes Source
// (local part of the SFTP "Append" overwrite answer)
static void AppendLocalFile(const QString & Source, const QString & Target)
{
  QFile In(Source);
  QFile Out(Target);
  if (!In.open(QIODevice::ReadOnly))
  {
    throw ExtException(QStringLiteral("Can't open file '%1'.").arg(Source), In.errorString());
  }
  if (!Out.open(QIODevice::WriteOnly | QIODevice::Append))
  {
    throw ExtException(QStringLiteral("Can't write to file '%1'.").arg(Target), Out.errorString());
  }
  QByteArray Buffer;
  while (!(Buffer = In.read(256 * 1024)).isEmpty())
  {
    if (Out.write(Buffer) != Buffer.size())
    {
      throw ExtException(QStringLiteral("Can't write to file '%1'.").arg(Target), Out.errorString());
    }
  }
  if (In.error() != QFileDevice::NoError)
  {
    throw ExtException(QStringLiteral("Can't read file '%1'.").arg(Source), In.errorString());
  }
  Out.close();
  In.close();
  QFile::remove(Source);
}

bool Terminal::DownloadFile(const TransferItem & Item, CopyParam & Param, OperationState & State, FileAttempt & Attempt)
{
  // TSFTPFileSystem::Sink
  QString DestFullName = Attempt.Target.isEmpty() ? Item.Target : Attempt.Target;
  bool AsciiTransfer = Param.UseAsciiTransfer(Item.Source, false, MaskParams(Item.Size, Item.Modification));
  OverwriteMode OMode = omOverwrite;
  bool ResumeAllowed = false;
  bool ResumeTransfer = false;
  TransferParams Params;
  QString LocalFileName;
  qint64 ExistingSize = 0;
  for (;;)
  {
    QString DestFileName = QFileInfo(DestFullName).fileName();
    // .filepart only with protocols that can resume (TSFTPFileSystem::Sink;
    // IsCapable(fcResumeSupport) is off for SCP and S3)
    ResumeAllowed = !AsciiTransfer && WorkerCapable(fcResumeSupport) && Param.AllowResume(Item.Size, DestFileName) && !Param.Append;
    LocalFileName = DestFullName;
    Params = TransferParams();
    Params.KeepPartial = ResumeAllowed;
    ResumeTransfer = false;
    OMode = omOverwrite;
    if (ResumeAllowed)
    {
      QString DestPartialFullName = DestFullName + PartialExt;
      LocalFileName = DestPartialFullName;
      QFileInfo Partial(DestPartialFullName);
      if (Partial.exists() && !Partial.isDir())
      {
        qint64 ResumeOffset = Partial.size();
        ResumeTransfer = ConfirmResume(DestFileName, (ResumeOffset > Item.Size), Param, State, Attempt);
        if (!ResumeTransfer)
        {
          if (!QFile::remove(DestPartialFullName))
          {
            throw ExtException(QStringLiteral("Error deleting file '%1'.").arg(DestPartialFullName));
          }
        }
        else
        {
          Params.ResumeOffset = ResumeOffset;
        }
      }
    }

    QFileInfo Existing(DestFullName);
    if (!ResumeTransfer && Existing.exists())
    {
      ExistingSize = Existing.size();
      if (Attempt.Confirmed || Attempt.Retrying)
      {
        OMode = Attempt.Mode;
      }
      else
      {
        QString NewName;
        if (!ConfirmOverwrite(Item.Source, DestFileName, false, Item.Size, Item.Modification, ExistingSize, Existing.lastModified(),
              Item.ModificationFmt, AsciiTransfer, Param, State, OMode, NewName))
        {
          return false;
        }
        if (!NewName.isEmpty())
        {
          // "New name": transfer under a different name, check that one
          DestFullName = QDir(QFileInfo(DestFullName).path()).filePath(NewName);
          Attempt.Target = DestFullName;
          continue;
        }
        Attempt.Confirmed = true;
        Attempt.Mode = OMode;
      }
    }
    break;
  }
  Attempt.Target = DestFullName;

  if (OMode == omResume)
  {
    // continue the existing smaller target in place (alternate resume)
    ResumeAllowed = false;
    LocalFileName = DestFullName;
    Params = TransferParams();
    Params.KeepPartial = true;
    Params.ResumeOffset = ExistingSize;
  }
  else if (OMode == omAppend)
  {
    ResumeAllowed = false;
    LocalFileName = DestFullName + QStringLiteral(".append") + PartialExt;
    Params = TransferParams();
  }

  if (Params.ResumeOffset > 0)
  {
    FProgress.AddResumed(Params.ResumeOffset);
  }
  Params.AsciiTransfer = AsciiTransfer;
  FFileSystem->Download(Item.Source, LocalFileName, Params, FProgress);
  if (OMode == omAppend)
  {
    AppendLocalFile(LocalFileName, DestFullName);
    LocalFileName = DestFullName;
  }
  if (Param.PreserveTime)
  {
    SetLocalModificationTime(LocalFileName, Item.Modification);
  }
  if (ResumeAllowed)
  {
    // rename() replaces an existing target atomically
    CheckRename(LocalFileName, DestFullName);
  }
  LocalFs::Stat St;
  if (Param.PreserveReadOnly && (Item.Mode != 0) && LocalFs::StatFollow(DestFullName, St))
  {
    // TCopyParamType::LocalFileAttrs: a remote file with no owner write permission
    LocalFs::SetMode(DestFullName, PreserveReadOnlyMode(St.Mode, Item.Mode, true));
  }
  return true;
}

bool Terminal::UploadFile(const TransferItem & Item, CopyParam & Param, OperationState & State, FileAttempt & Attempt)
{
  // TSFTPFileSystem::Source
  QString DestFullName = Attempt.Target.isEmpty() ? Item.Target : Attempt.Target;
  QString DestFileName;
  QString DestPartialFullName;
  bool AsciiTransfer = Param.UseAsciiTransfer(Item.Source, true, MaskParams(Item.Size, Item.Modification));
  bool ResumeAllowed = false;
  RemoteFile Existing;
  bool DestFileExists = false;
  TransferParams Params;
  OverwriteMode OMode = omOverwrite;
  for (;;)
  {
    DestFileName = UnixPath::ExtractFileName(DestFullName);
    DestPartialFullName = DestFullName + PartialExt;
    // .filepart + rename only with protocols that can resume (TSFTPFileSystem::Source;
    // IsCapable(fcResumeSupport) is off for SCP and S3)
    ResumeAllowed = !AsciiTransfer && WorkerCapable(fcResumeSupport) && Param.AllowResume(Item.Size, DestFileName) &&
      WorkerCapable(fcRename) && !Param.Append;
    Existing = RemoteFile();
    DestFileExists = FFileSystem->FileExists(DestFullName, &Existing);
    if (ResumeAllowed && DestFileExists)
    {
      // If destination file is symlink, never do resumable transfer, as it would delete the symlink.
      // Also never do resumable transfer for file owned by other user,
      // as deleting and recreating the file would change ownership.
      if (Existing.IsSymLink ||
          (!Existing.Owner.isEmpty() && (Existing.Owner != FData.UserName) && !Existing.Owner.at(0).isDigit()))
      {
        ResumeAllowed = false;
      }
    }

    Params = TransferParams();
    Params.KeepPartial = ResumeAllowed;
    Params.RemoveBOM = AsciiTransfer && Param.RemoveBOM;
    Params.RemoveCtrlZ = AsciiTransfer && Param.RemoveCtrlZ;
    Params.AsciiTransfer = AsciiTransfer;
    OMode = omOverwrite;
    bool Confirm = DestFileExists;
    if (ResumeAllowed)
    {
      RemoteFile Partial;
      if (FFileSystem->FileExists(DestPartialFullName, &Partial))
      {
        Confirm = false;
        qint64 ResumeOffset = Partial.Size;
        if (ConfirmResume(DestFileName, (ResumeOffset > Item.Size), Param, State, Attempt))
        {
          Params.ResumeOffset = ResumeOffset;
        }
        else
        {
          FFileSystem->DeleteFile(DestPartialFullName);
        }
      }
    }
    if (Confirm)
    {
      if (Attempt.Confirmed || Attempt.Retrying)
      {
        OMode = Attempt.Mode;
      }
      else
      {
        QString NewName;
        if (!ConfirmOverwrite(Item.Source, DestFileName, true, Item.Size, Item.Modification, Existing.Size, Existing.Modification,
              Existing.ModificationFmt, AsciiTransfer, Param, State, OMode, NewName))
        {
          return false;
        }
        if (!NewName.isEmpty())
        {
          DestFullName = UnixPath::Combine(UnixPath::ExtractFileDir(DestFullName), NewName);
          Attempt.Target = DestFullName;
          continue;
        }
        Attempt.Confirmed = true;
        Attempt.Mode = OMode;
      }
    }
    break;
  }
  Attempt.Target = DestFullName;

  bool Sftp = (FData.FSProtocol == fsSFTP) || (FData.FSProtocol == fsSFTPonly);
  // "Preserve overwritten remote files to recycle bin" (SFTP only)
  bool RecycleOverwritten = Sftp && FData.OverwrittenToRecycleBin && !FData.RecycleBinPath.isEmpty() &&
    DestFileExists && (OMode == omOverwrite) && !IsRecycledFile(DestFullName);
  bool Recycled = false;
  QString RemoteFileName = ResumeAllowed ? DestPartialFullName : DestFullName;
  if (OMode == omResume)
  {
    // continue the existing smaller target in place (alternate resume / FTP resume)
    ResumeAllowed = false;
    RemoteFileName = DestFullName;
    Params.KeepPartial = true;
    Params.ResumeOffset = Existing.Size;
  }
  else if (OMode == omAppend)
  {
    ResumeAllowed = false;
    RemoteFileName = DestFullName;
    Params.KeepPartial = true;
    Params.ResumeOffset = 0;
    Params.Append = true;
  }
  else if (RecycleOverwritten && !ResumeAllowed)
  {
    RecycleFile(DestFullName, State);
    Recycled = true;
  }
  // The rights of a new file are the server's unless they are set (as WinSCP);
  // a file that replaces another (.filepart) starts with the rights of that one
  int Mode = ServerDefaultMode;
  if (Param.PreserveRights)
  {
    Mode = static_cast<int>(Param.RemoteFileRights(false));
  }
  else if (DestFileExists && (Existing.Permissions != 0))
  {
    Mode = static_cast<int>(Existing.Permissions);
  }
  if (Params.ResumeOffset > 0)
  {
    FProgress.AddResumed(Params.ResumeOffset);
  }
  bool DeletedForOverwrite = false;
  for (;;)
  {
    try
    {
      FFileSystem->Upload(Item.Source, RemoteFileName, Mode, Params, FProgress);
      break;
    }
    catch (const CannotCreateFileException & E)
    {
      // TSFTPFileSystem::SFTPOpenRemote
      if (ResumeAllowed && (Params.ResumeOffset == 0) && (Param.ResumeSupport == CopyParam::rsSmart))
      {
        // The target may be writable in a folder where no new file can be created.
        // Writing to it takes the protection of .filepart away: whatever goes wrong
        // from there, the old contents are gone. WinSCP goes on, and even deletes
        // what is left; here that is for the user to decide, unless the recycle bin
        // keeps the old file. Skip is the answer when nobody can be asked.
        if (DestFileExists && !RecycleOverwritten)
        {
          QueryUserParams QueryParams;
          QueryParams.Params = qpAllowContinueOnError;
          QueryParams.Type = qtWarning;
          QueryParams.Aliases.append(QueryButtonAlias{qaYes, QStringLiteral("&Overwrite")});
          unsigned int Answer = QueryUser(QStringLiteral("Cannot create temporary file '%1' for the upload.\n \n"
            "Press 'Overwrite' to write directly to the existing file '%2'. If the upload then fails, the file stays incomplete and its old contents are lost.")
              .arg(UnixPath::ExtractFileName(RemoteFileName), DestFileName),
            E.MoreMessages(), qaYes | qaSkip | qaAbort, QueryParams, &State);
          if (Answer == qaSkip)
          {
            throw SkipFileException();
          }
          if (Answer != qaYes)
          {
            FProgress.Cancel();
            throw AbortException();
          }
        }
        SessionLog::LogEvent(QStringLiteral("Cannot create new partial file \"%1\", trying to create target file \"%2\"").arg(RemoteFileName, DestFullName));
        ResumeAllowed = false;
        RemoteFileName = DestFullName;
        // A failed upload deletes only a file of its own
        Params.KeepPartial = DestFileExists && !RecycleOverwritten;
        if (RecycleOverwritten)
        {
          RecycleFile(DestFullName, State);
          Recycled = true;
        }
      }
      else if (Sftp && DestFileExists && !Recycled && !DeletedForOverwrite && (OMode == omOverwrite) && (RemoteFileName == DestFullName))
      {
        // SFTP_OVERWRITE_FILE_ERROR2, SFTP_OVERWRITE_DELETE_BUTTON
        QueryUserParams QueryParams;
        QueryParams.Params = qpAllowContinueOnError;
        QueryParams.Type = qtError;
        QueryParams.Aliases.append(QueryButtonAlias{qaRetry, QStringLiteral("&Delete")});
        unsigned int Answer = QueryUser(QStringLiteral("Cannot overwrite remote file '%1'.\n \nPress 'Delete' to delete the file and create new one instead of overwriting it.").arg(RemoteFileName),
          E.MoreMessages(), qaRetry | qaSkip | qaAbort, QueryParams, &State);
        if (Answer == qaSkip)
        {
          throw SkipFileException();
        }
        if (Answer != qaRetry)
        {
          FProgress.Cancel();
          throw AbortException();
        }
        FFileSystem->DeleteFile(RemoteFileName);
        DeletedForOverwrite = true;
      }
      else
      {
        throw;
      }
    }
  }

  if (ResumeAllowed)
  {
    if (DestFileExists)
    {
      try
      {
        if (RecycleOverwritten)
        {
          RecycleFile(DestFullName, State);
          Recycled = true;
        }
        else
        {
          FFileSystem->DeleteFile(DestFullName);
        }
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
        // DELETE_ON_RESUME_ERROR
        throw ExtException(QStringLiteral("Error deleting file '%1'. After resumable file upload the existing destination file must be deleted. "
          "If you do not have permissions to delete the destination file, you need to disable resumable file transfers.").arg(DestFileName),
          E.MoreMessages());
      }
    }
    try
    {
      FFileSystem->RenameFile(RemoteFileName, DestFullName);
    }
    catch (const FatalException &)
    {
      throw;
    }
    catch (const ExtException & E)
    {
      throw ExtException(QStringLiteral("Transfer was successfully finished, but temporary transfer file '%1' could not be renamed to target file name '%2'. "
        "If the problem persists, you may try to turn off transfer resume support.").arg(UnixPath::ExtractFileName(RemoteFileName), DestFileName),
        E.MoreMessages());
    }
  }

  bool PreserveExistingRights = (ResumeAllowed && DestFileExists) || Recycled;
  SftpFileSystem * SftpFs = dynamic_cast<SftpFileSystem *>(FFileSystem.get());
  if ((SftpFs != nullptr) && !SftpFs->UsingScpFallback())
  {
    // TSFTPFileSystem::SFTPSource: one SETSTAT with the rights and the time
    int SetMode = -1;
    if (Param.PreserveRights)
    {
      SetMode = static_cast<int>(Param.RemoteFileRights(false));
    }
    else if (PreserveExistingRights && (Existing.Permissions != 0))
    {
      SetMode = static_cast<int>(Existing.Permissions);
    }
    // TSFTPFileSystem::SFTPSource: TTouchSessionAction, TChmodSessionAction
    SessionAction TouchAction(laTouch, Param.PreserveTime ? ActionsLog::Current() : nullptr);
    TouchAction.FileName(DestFullName);
    TouchAction.Modification(Item.Modification);
    SessionAction ChmodAction(laChmod, Param.PreserveRights ? ActionsLog::Current() : nullptr);
    ChmodAction.FileName(DestFullName);
    ChmodAction.Rights(Param.RemoteFileRights(false));
    try
    {
      SftpFs->SetFileProperties(DestFullName, SetMode, Param.PreserveTime ? Item.Modification : QDateTime());
      TouchAction.Commit();
      ChmodAction.Commit();
    }
    catch (const FatalException &)
    {
      throw;
    }
    catch (const ExtException &)
    {
      // Only the permissions of the overwritten file: "Ignoring error
      // preserving permissions of overwritten file"
      if ((Param.PreserveRights || Param.PreserveTime) && !Param.IgnorePermErrors)
      {
        throw;
      }
    }
    return true;
  }
  try
  {
    if (Param.PreserveRights)
    {
      SessionAction ChmodAction(laChmod, DestFullName);
      ChmodAction.Rights(Param.RemoteFileRights(false));
      FFileSystem->ChangeMode(DestFullName, Param.RemoteFileRights(false));
      ChmodAction.Commit();
    }
    else if (PreserveExistingRights && (Existing.Permissions != 0))
    {
      // 0 = server did not report permissions (e.g. FTP without UNIX listing)
      try
      {
        FFileSystem->ChangeMode(DestFullName, Existing.Permissions);
      }
      catch (const FatalException &)
      {
        throw;
      }
      catch (const ExtException &)
      {
        // Ignoring error preserving permissions of overwritten file
      }
    }
    if (Param.PreserveTime)
    {
      SessionAction TouchAction(laTouch, DestFullName);
      TouchAction.Modification(Item.Modification);
      FFileSystem->SetModificationTime(DestFullName, Item.Modification);
      TouchAction.Commit();
    }
  }
  catch (const FatalException &)
  {
    throw;
  }
  catch (const ExtException &)
  {
    if (!Param.IgnorePermErrors)
    {
      throw;
    }
  }
  return true;
}

void Terminal::ScpPrepareUpload(const QList<TransferItem> & Items, CopyParam & Param, OperationState & State,
  QVector<TransferState> & States, QVector<FileAttempt> & Attempts)
{
  // TSCPFileSystem::CopyToRemote confirms overwriting before each file is
  // sent. The targets are looked up in the listings of the target
  // directories (one "ls" per directory), only directories with a few
  // files get one "ls -d" per file.
  const int ListingThreshold = 4;
  QHash<QString, int> FilesPerDirectory;
  for (const TransferItem & Item : Items)
  {
    if (!Item.Directory)
    {
      FilesPerDirectory[UnixPath::ExtractFileDir(Item.Target)]++;
    }
  }
  QSet<QString> NewDirectories;
  QHash<QString, QHash<QString, RemoteFile>> Listings;
  for (qsizetype Index = 0; Index < Items.size(); Index++)
  {
    const TransferItem & Item = Items[Index];
    States[Index] = tsBatch;
    CheckOperationCancel(FProgress);
    QString Directory = UnixPath::ExtractFileDir(Item.Target);
    QString Name = UnixPath::ExtractFileName(Item.Target);
    RemoteFile Existing;
    bool Exists = false;
    FileAttempt Attempt;
    bool Checked = ProcessFile(State, Attempt, [&](FileAttempt &)
    {
      Exists = false;
      if (NewDirectories.contains(Directory))
      {
        return true;
      }
      auto Listing = Listings.constFind(Directory);
      if ((Listing == Listings.constEnd()) && (FilesPerDirectory.value(Directory) > ListingThreshold))
      {
        QHash<QString, RemoteFile> Files;
        try
        {
          const RemoteFileList List = FFileSystem->ReadDirectory(Directory);
          for (const RemoteFile & File : List)
          {
            Files.insert(File.FileName, File);
          }
          Listing = Listings.insert(Directory, Files);
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
          // cannot list (e.g. no read permission): ask for each file
        }
      }
      if (Listing != Listings.constEnd())
      {
        auto File = Listing->constFind(Name);
        Exists = (File != Listing->constEnd());
        if (Exists)
        {
          Existing = File.value();
        }
      }
      else
      {
        Exists = FFileSystem->FileExists(Item.Target, &Existing);
      }
      return true;
    });
    if (!Checked)
    {
      // skipped (an error of the check): neither created nor transferred
      FProgress.AddTransferred(Item.Size);
      States[Index] = tsSkipped;
      if (Item.Directory)
      {
        QString Prefix = Item.Source + QLatin1Char('/');
        for (qsizetype Child = Index + 1; (Child < Items.size()) && Items[Child].Source.startsWith(Prefix); Child++)
        {
          FProgress.AddTransferred(Items[Child].Size);
          States[Child] = tsSkipped;
        }
      }
      continue;
    }
    if (Item.Directory)
    {
      if (!Exists)
      {
        NewDirectories.insert(Item.Target);
      }
      continue;
    }
    if (!Exists)
    {
      continue;
    }
    bool AsciiTransfer = Param.UseAsciiTransfer(Item.Source, true, MaskParams(Item.Size, Item.Modification));
    OverwriteMode OMode = omOverwrite;
    QString NewName;
    if (!ConfirmOverwrite(Item.Source, Name, true, Item.Size, Item.Modification, Existing.Size, Existing.Modification, Existing.ModificationFmt,
          AsciiTransfer, Param, State, OMode, NewName))
    {
      FProgress.AddTransferred(Item.Size);
      State.AnySkipped = true;
      States[Index] = tsSkipped;
      continue;
    }
    if (!NewName.isEmpty())
    {
      // "New name": UploadFile checks the new name
      Attempts[Index].Target = UnixPath::Combine(Directory, NewName);
      States[Index] = tsPerFile;
      continue;
    }
    Attempts[Index].Confirmed = true;
    Attempts[Index].Mode = OMode;
    if (OMode != omOverwrite)
    {
      States[Index] = tsPerFile;
    }
  }
}

void Terminal::ScpPrepareDownload(const QList<TransferItem> & Items, CopyParam & Param, OperationState & State,
  QVector<TransferState> & States, QVector<FileAttempt> & Attempts)
{
  // TSCPFileSystem::SCPSink confirms overwriting each existing local file
  for (qsizetype Index = 0; Index < Items.size(); Index++)
  {
    const TransferItem & Item = Items[Index];
    States[Index] = tsBatch;
    if (Item.Directory)
    {
      continue;
    }
    CheckOperationCancel(FProgress);
    QFileInfo Existing(Item.Target);
    if (!Existing.exists())
    {
      continue;
    }
    if (Existing.isDir())
    {
      // DownloadFile reports it
      States[Index] = tsPerFile;
      continue;
    }
    bool AsciiTransfer = Param.UseAsciiTransfer(Item.Source, false, MaskParams(Item.Size, Item.Modification));
    OverwriteMode OMode = omOverwrite;
    QString NewName;
    if (!ConfirmOverwrite(Item.Source, Existing.fileName(), false, Item.Size, Item.Modification, Existing.size(),
          Existing.lastModified(), Item.ModificationFmt, AsciiTransfer, Param, State, OMode, NewName))
    {
      FProgress.AddTransferred(Item.Size);
      State.AnySkipped = true;
      States[Index] = tsSkipped;
      continue;
    }
    if (!NewName.isEmpty())
    {
      Attempts[Index].Target = QDir(Existing.path()).filePath(NewName);
      States[Index] = tsPerFile;
      continue;
    }
    Attempts[Index].Confirmed = true;
    Attempts[Index].Mode = OMode;
    if (OMode != omOverwrite)
    {
      States[Index] = tsPerFile;
    }
  }
}

void Terminal::ScpBatchTransfer(bool ToRemote, const QStringList & Sources, const QString & TargetDir,
  const QList<TransferItem> & Items, CopyParam & Param, OperationState & State,
  QVector<TransferState> & States, QVector<FileAttempt> & Attempts, QVector<bool> & TimesSet)
{
  QHash<QString, qsizetype> BySource;
  for (qsizetype Index = 0; Index < Items.size(); Index++)
  {
    BySource.insert(Items[Index].Source, Index);
  }
  auto Find = [&](const QString & Path) -> qsizetype
  {
    auto It = BySource.constFind(Path);
    return (It == BySource.constEnd()) ? -1 : It.value();
  };
  auto ParentOf = [&](qsizetype Index) -> qsizetype
  {
    const QString & Source = Items[Index].Source;
    QString Parent = ToRemote ? QFileInfo(Source).path() : UnixPath::ExtractFileDir(Source);
    return (Parent == Source) ? -1 : Find(Parent);
  };
  // Items are in pre-order: a directory is followed by its contents
  auto ForSubtree = [&](qsizetype Index, const std::function<void(qsizetype)> & Func)
  {
    Func(Index);
    if (Items[Index].Directory)
    {
      QString Prefix = Items[Index].Source + QLatin1Char('/');
      for (qsizetype Child = Index + 1; (Child < Items.size()) && Items[Child].Source.startsWith(Prefix); Child++)
      {
        Func(Child);
      }
    }
  };

  // The file being transferred by the batch and the progress when it started
  qsizetype Current = -1;
  qint64 CurrentStart = 0;
  // Progress of a file leaving the batch: the part transferred so far is
  // rolled back (transferred again) or completed to its size (skipped)
  auto Settle = [&](qsizetype Index, bool Skipped)
  {
    qint64 Counted = (Index == Current) ? (FProgress.Get().TotalTransferred - CurrentStart) : 0;
    FProgress.AddTransferred((Skipped ? Items[Index].Size : 0) - Counted);
    if (Index == Current)
    {
      Current = -1;
    }
  };
  QSet<qsizetype> Needed;   // directories containing items of the batch

  ScpTransferOptions Options;
  Options.PreserveTime = Param.PreserveTime;
  Options.PreserveTimeDirs = Param.PreserveTimeDirs;
  Options.RemoveBOM = Param.RemoveBOM;
  Options.RemoveCtrlZ = Param.RemoveCtrlZ;
  if (ToRemote)
  {
    // "-p" with Set permissions; the rights of TCopyParamType::RemoteFileRights
    Options.PreserveRights = Param.PreserveRights;
    Options.Mode = [&](const QString &, bool Directory) -> unsigned int
    {
      return Param.PreserveRights ? Param.RemoteFileRights(Directory) : (Directory ? 0755u : 0644u);
    };
  }
  Options.Include = [&](const QString & Path, bool Directory) -> bool
  {
    qsizetype Index = Find(Path);
    if ((Index < 0) || (Items[Index].Directory != Directory))
    {
      // excluded by a mask, or appeared / changed since the files were collected
      return false;
    }
    if (Directory)
    {
      return (States[Index] == tsBatch) || Needed.contains(Index);
    }
    if (States[Index] != tsBatch)
    {
      return false;
    }
    Current = Index;
    CurrentStart = FProgress.Get().TotalTransferred;
    return true;
  };
  Options.TargetName = [&](const QString & Path, bool) -> QString
  {
    qsizetype Index = Find(Path);
    if (Index < 0)
    {
      return QString();
    }
    return ToRemote ? UnixPath::ExtractFileName(Items[Index].Target) : QFileInfo(Items[Index].Target).fileName();
  };
  Options.AsciiTransfer = [&](const QString & Path) -> bool
  {
    qsizetype Index = Find(Path);
    return (Index >= 0) &&
      Param.UseAsciiTransfer(Path, ToRemote, MaskParams(Items[Index].Size, Items[Index].Modification));
  };
  Options.Finished = [&](const QString & Path, const QString &, bool Directory)
  {
    qsizetype Index = Find(Path);
    if (Index < 0)
    {
      return;
    }
    if (Directory)
    {
      if (States[Index] == tsBatch)
      {
        States[Index] = tsDone;
      }
      if (Param.PreserveTime && Param.PreserveTimeDirs)
      {
        TimesSet[Index] = true;
      }
      return;
    }
    States[Index] = tsDone;
    SessionAction(ToRemote ? laUpload : laDownload, Items[Index].Source, Items[Index].Target).Commit(true, Items[Index].Size);
    // TSCPFileSystem::SCPSource: SCP cannot tell which part failed, so touch
    // and chmod are logged only after a successful upload
    if (ToRemote && Param.PreserveTime)
    {
      SessionAction TouchAction(laTouch, Items[Index].Target);
      TouchAction.Modification(Items[Index].Modification);
      TouchAction.Commit();
    }
    if (ToRemote && Param.PreserveRights)
    {
      SessionAction ChmodAction(laChmod, Items[Index].Target);
      ChmodAction.Rights(Param.RemoteFileRights(false));
      ChmodAction.Commit();
    }
    if (Index == Current)
    {
      Current = -1;
    }
    FProgress.FileFinished();
  };
  Options.Error = [&](const QString & Path, const ExtException & E) -> bool
  {
    qsizetype Index = Find(Path);
    if ((Index < 0) || ((States[Index] != tsBatch) && !Items[Index].Directory))
    {
      // Not one of the files (COPY_ERROR with qaOK | qaAbort as TSCPFileSystem)
      QueryUserParams Params;
      Params.Params = qpAllowContinueOnError;
      Params.Type = qtError;
      unsigned int Answer = QueryUser(QStringLiteral("Copying file '%1' failed.").arg(Path),
        ErrorText(E.Message(), E.MoreMessages()), qaOK | qaAbort, Params, &State);
      if ((Answer != qaOK) && (Answer != qaSkip))
      {
        FProgress.Cancel();
        throw QueryAbortException(E);
      }
      State.AnySkipped = true;
      return true;
    }
    try
    {
      // FILE_OPERATION_LOOP: Retry / Skip / Skip all / Abort
      FileOperationLoopQuery(E, State, QString(), true);
      // Retry: the file (directory) is transferred one by one after the batch
      ForSubtree(Index, [&](qsizetype Item)
      {
        if (States[Item] == tsBatch)
        {
          Settle(Item, false);
          States[Item] = tsPerFile;
          Attempts[Item].Retrying = true;
        }
      });
    }
    catch (const SkipFileException &)
    {
      SessionAction(ToRemote ? laUpload : laDownload, Items[Index].Source, Items[Index].Target).Rollback(E);
      ForSubtree(Index, [&](qsizetype Item)
      {
        if (States[Item] == tsBatch)
        {
          Settle(Item, true);
          States[Item] = tsSkipped;
        }
      });
      State.AnySkipped = true;
    }
    return true;
  };

  QElapsedTimer Start;
  Start.start();
  qint64 LastTransferred = FProgress.Get().TotalTransferred;
  for (;;)
  {
    // What is left for the batch (a new batch after a skipped file or a reconnect)
    Needed.clear();
    bool Any = false;
    for (qsizetype Index = 0; Index < Items.size(); Index++)
    {
      if (States[Index] != tsBatch)
      {
        continue;
      }
      Any = true;
      for (qsizetype Parent = ParentOf(Index); (Parent >= 0) && !Needed.contains(Parent); Parent = ParentOf(Parent))
      {
        Needed.insert(Parent);
      }
    }
    if (!Any)
    {
      break;
    }
    QStringList Roots;
    for (const QString & Source : Sources)
    {
      qsizetype Index = Find(Source);
      if ((Index >= 0) && ((States[Index] == tsBatch) || Needed.contains(Index)))
      {
        Roots.append(Source);
      }
    }
    ScpFileSystem * Scp = ScpFileSystem::Of(*FFileSystem);
    if (Roots.isEmpty() || (Scp == nullptr))
    {
      break;
    }
    Current = -1;
    try
    {
      if (ToRemote)
      {
        Scp->UploadBatch(Roots, TargetDir, Options, FProgress);
      }
      else
      {
        Scp->DownloadBatch(Roots, TargetDir, Options, FProgress);
      }
      break;
    }
    catch (const SkipFileException &)
    {
      // "Skip this file" of the progress dialog: the rest in a new batch
      if ((Current >= 0) && (States[Current] == tsBatch))
      {
        qsizetype Index = Current;
        Settle(Index, true);
        States[Index] = tsSkipped;
        State.AnySkipped = true;
      }
    }
    catch (const AbortException &)
    {
      throw;
    }
    catch (const FatalException & E)
    {
      if (Current >= 0)
      {
        Settle(Current, false);
      }
      if (!TryReopen(E, State, Start, LastTransferred))
      {
        throw;
      }
    }
    catch (const ExtException &)
    {
      // The batch as a whole failed (e.g. the target directory does not
      // exist): the files one by one report the problem
      if (Current >= 0)
      {
        Settle(Current, false);
      }
      break;
    }
  }
  // Not transferred by the batch (vanished, changed, the batch failed): one by one
  for (qsizetype Index = 0; Index < Items.size(); Index++)
  {
    if (States[Index] == tsBatch)
    {
      States[Index] = tsPerFile;
    }
  }
}

void Terminal::CopyToLocal(const QStringList & RemotePaths, const QString & TargetDir, const CopyParam & Param, bool Move, Callback Done)
{
  FProgress.Start(Move ? FileOperationProgress::Move : FileOperationProgress::Copy, FileOperationProgress::ToLocal, RemotePaths.size());
  FProgress.SetCPSLimit(Param.CPSLimit);
  RunOperation([this, RemotePaths, TargetDir, Param, Move](OperationState & State)
  {
    CopyParam AParam = Param;
    AParam.TrimVMSVersions = FData.TrimVMSVersions;
    if (!TargetDir.isEmpty() && !QFileInfo(TargetDir).isDir())
    {
      // CREATE_LOCAL_DIRECTORY (forms/Copy.cpp)
      QueryUserParams Params;
      unsigned int Answer = QueryUser(QStringLiteral("Directory '%1' doesn't exist. Create?").arg(TargetDir), QString(),
        qaOK | qaCancel, Params, nullptr);
      if (Answer != qaOK)
      {
        throw AbortException();
      }
      FileOperationLoop(State, QString(), [&]()
      {
        if (!QDir().mkpath(TargetDir))
        {
          // CREATE_LOCAL_DIR_ERROR
          throw ExtException(QStringLiteral("Can't create folder '%1'.").arg(TargetDir));
        }
      }, false);
    }

    QList<TransferItem> Items;
    for (const QString & Path : RemotePaths)
    {
      RemoteFile File;
      FileAttempt Attempt;
      if (ProcessFile(State, Attempt, [&](FileAttempt &)
          {
            // The target is named after the last component: "x/.." from a hostile
            // listing, selected by the user, would leave the target directory
            if (!UnixPath::HasValidFileName(Path))
            {
              throw ExtException(QStringLiteral("Invalid file name '%1'.").arg(Path));
            }
            File = FFileSystem->ReadFile(Path);
            return true;
          }))
      {
        ScanRemote(Path, File, QDir(TargetDir).filePath(CopyParam::ValidLocalFileName(AParam.ChangeFileName(UnixPath::ExtractFileName(Path), true))), Items, AParam, State);
      }
    }
    if (!AParam.AllowAnyTransfer())
    {
      State.AnySkipped = true;
    }
    QVector<TransferState> States(Items.size(), tsPerFile);
    QVector<FileAttempt> Attempts(Items.size());
    if (!Items.isEmpty() && (ScpFileSystem::Of(*FFileSystem) != nullptr))
    {
      // SCP: one "scp -f" per selected item (TSCPFileSystem::CopyToLocal)
      QVector<bool> TimesSet(Items.size(), false);
      ScpPrepareDownload(Items, AParam, State, States, Attempts);
      ScpBatchTransfer(false, RemotePaths, TargetDir, Items, AParam, State, States, Attempts, TimesSet);
    }
    QStringList SkippedDirectories;
    for (qsizetype Index = 0; Index < Items.size(); Index++)
    {
      const TransferItem & Item = Items[Index];
      if (States[Index] != tsPerFile)
      {
        continue;
      }
      CheckOperationCancel(FProgress);
      if (InsideAny(Item.Source, SkippedDirectories))
      {
        FProgress.AddTransferred(Item.Size);
        States[Index] = tsSkipped;
        continue;
      }
      FProgress.SetFile(Item.Source, Item.Size);
      FileAttempt Attempt = Attempts[Index];
      if (Item.Directory)
      {
        bool Created = ProcessFile(State, Attempt, [&](FileAttempt &)
        {
          if (!QDir().mkpath(Item.Target))
          {
            throw ExtException(QStringLiteral("Can't create folder '%1'.").arg(Item.Target));
          }
          return true;
        });
        if (!Created)
        {
          SkippedDirectories.append(Item.Source);
          State.AnySkipped = true;
          States[Index] = tsSkipped;
          continue;
        }
        States[Index] = tsDone;
        continue;
      }
      SessionLog::LogEvent(QStringLiteral("Copying \"%1\" to local directory started.").arg(Item.Source));
      SessionAction Action(laDownload, Item.Source, Item.Target);
      if (!Action.Commit(ProcessFile(State, Attempt, [&](FileAttempt & A) { return DownloadFile(Item, AParam, State, A); }), Item.Size))
      {
        // skipped: counts as processed
        RollbackFileProgress();
        FProgress.AddTransferred(Item.Size);
        State.AnySkipped = true;
        States[Index] = tsSkipped;
        continue;
      }
      SessionLog::LogEvent(QStringLiteral("Transfer done: '%1' => '%2' [%3]").arg(Item.Source, Item.Target).arg(Item.Size));
      FProgress.FileFinished();
      States[Index] = tsDone;
    }
    QList<TransferItem> DoneItems;
    for (qsizetype Index = 0; Index < Items.size(); Index++)
    {
      if (States[Index] == tsDone)
      {
        DoneItems.append(Items[Index]);
      }
    }
    if (AParam.PreserveTime && AParam.PreserveTimeDirs)
    {
      // after the contents, so that writing files does not change the time again
      for (auto It = DoneItems.crbegin(); It != DoneItems.crend(); ++It)
      {
        if (It->Directory)
        {
          SetLocalModificationTime(It->Target, It->Modification);
        }
      }
    }
    if (Move)
    {
      FProgress.SetFile(QString(), 0);
      // Delete only the transferred sources, in reverse order, so directories are empty when reached
      for (auto It = DoneItems.crbegin(); It != DoneItems.crend(); ++It)
      {
        CheckOperationCancel(FProgress);
        const QString Source = It->Source;
        if (It->Directory)
        {
          if (State.AnySkipped)
          {
            try
            {
              FFileSystem->RemoveDirectory(Source);
            }
            catch (const FatalException &)
            {
              throw;
            }
            catch (const ExtException &)
            {
              // directory still contains files that were not transferred
            }
          }
          else
          {
            RetryLoop(State, QString(), [&]() { FFileSystem->RemoveDirectory(Source); });
          }
        }
        else
        {
          RetryLoop(State, QString(), [&]() { FFileSystem->DeleteFile(Source); });
        }
      }
    }
  }, Done);
}

void Terminal::CopyToRemote(const QStringList & LocalPaths, const QString & TargetDir, const CopyParam & Param, bool Move, Callback Done)
{
  FProgress.Start(Move ? FileOperationProgress::Move : FileOperationProgress::Copy, FileOperationProgress::ToRemote, LocalPaths.size());
  FProgress.SetCPSLimit(Param.CPSLimit);
  RunOperation([this, LocalPaths, TargetDir, Param, Move](OperationState & State)
  {
    CopyParam AParam = Param;
    AParam.TrimVMSVersions = FData.TrimVMSVersions;
    QList<TransferItem> Items;
    QSet<QString> Visited;
    for (const QString & Path : LocalPaths)
    {
      ScanLocal(Path, UnixPath::Combine(TargetDir, CopyParam::RestoreChars(AParam.ChangeFileName(QFileInfo(Path).fileName(), true))), Items, Visited, AParam, State);
    }
    if (!AParam.AllowAnyTransfer())
    {
      State.AnySkipped = true;
    }
    QVector<TransferState> States(Items.size(), tsPerFile);
    QVector<FileAttempt> Attempts(Items.size());
    // Directories whose time the SCP batch set (T record) and that did not
    // change afterwards
    QVector<bool> TimesSet(Items.size(), false);
    QHash<QString, qsizetype> DirectoryIndexes;
    if (!Items.isEmpty() && (ScpFileSystem::Of(*FFileSystem) != nullptr))
    {
      // SCP: everything in one "scp -t" (TSCPFileSystem::CopyToRemote)
      ScpPrepareUpload(Items, AParam, State, States, Attempts);
      ScpBatchTransfer(true, LocalPaths, TargetDir, Items, AParam, State, States, Attempts, TimesSet);
      for (qsizetype Index = 0; Index < Items.size(); Index++)
      {
        if (Items[Index].Directory)
        {
          DirectoryIndexes.insert(Items[Index].Source, Index);
        }
      }
    }
    QStringList SkippedDirectories;
    for (qsizetype Index = 0; Index < Items.size(); Index++)
    {
      const TransferItem & Item = Items[Index];
      if (States[Index] != tsPerFile)
      {
        continue;
      }
      CheckOperationCancel(FProgress);
      if (InsideAny(Item.Source, SkippedDirectories))
      {
        FProgress.AddTransferred(Item.Size);
        States[Index] = tsSkipped;
        continue;
      }
      // writing into a directory changes its time again
      for (QString Parent = QFileInfo(Item.Source).path(); DirectoryIndexes.contains(Parent); Parent = QFileInfo(Parent).path())
      {
        TimesSet[DirectoryIndexes.value(Parent)] = false;
        if (Parent == QFileInfo(Parent).path())
        {
          break;
        }
      }
      FProgress.SetFile(Item.Source, Item.Size);
      FileAttempt Attempt = Attempts[Index];
      if (Item.Directory)
      {
        // TTerminal::CreateTargetDirectory
        bool Created = ProcessFile(State, Attempt, [&](FileAttempt &)
        {
          RemoteFile Existing;
          if (!FFileSystem->FileExists(Item.Target, &Existing))
          {
            int Mode = AParam.PreserveRights ? static_cast<int>(AParam.RemoteFileRights(true)) : ServerDefaultMode;
            SessionAction Action(laMkdir, Item.Target);
            FFileSystem->CreateDirectory(Item.Target, Mode);
            Action.Commit();
          }
          return true;
        });
        if (!Created)
        {
          SkippedDirectories.append(Item.Source);
          State.AnySkipped = true;
          States[Index] = tsSkipped;
          continue;
        }
        States[Index] = tsDone;
        continue;
      }
      SessionLog::LogEvent(QStringLiteral("Copying \"%1\" to remote directory started.").arg(Item.Source));
      SessionAction Action(laUpload, Item.Source, Item.Target);
      if (!Action.Commit(ProcessFile(State, Attempt, [&](FileAttempt & A) { return UploadFile(Item, AParam, State, A); }), Item.Size))
      {
        RollbackFileProgress();
        FProgress.AddTransferred(Item.Size);
        State.AnySkipped = true;
        States[Index] = tsSkipped;
        continue;
      }
      SessionLog::LogEvent(QStringLiteral("Transfer done: '%1' => '%2' [%3]").arg(Item.Source, Item.Target).arg(Item.Size));
      FProgress.FileFinished();
      States[Index] = tsDone;
    }
    QList<TransferItem> DoneItems;
    QList<bool> DoneTimesSet;
    for (qsizetype Index = 0; Index < Items.size(); Index++)
    {
      if (States[Index] == tsDone)
      {
        DoneItems.append(Items[Index]);
        DoneTimesSet.append(TimesSet[Index]);
      }
    }
    if (AParam.PreserveTime && AParam.PreserveTimeDirs)
    {
      for (qsizetype DoneIndex = DoneItems.size() - 1; DoneIndex >= 0; DoneIndex--)
      {
        auto It = DoneItems.cbegin() + DoneIndex;
        if (It->Directory && !DoneTimesSet[DoneIndex])
        {
          try
          {
            FFileSystem->SetModificationTime(It->Target, It->Modification);
          }
          catch (const FatalException &)
          {
            throw;
          }
          catch (const ExtException &)
          {
            if (!AParam.IgnorePermErrors)
            {
              throw;
            }
          }
        }
      }
    }
    if (Move)
    {
      FProgress.SetFile(QString(), 0);
      // Delete only the transferred sources
      for (auto It = DoneItems.crbegin(); It != DoneItems.crend(); ++It)
      {
        CheckOperationCancel(FProgress);
        const QString Source = It->Source;
        if (It->Directory)
        {
          if (!QDir().rmdir(Source) && !State.AnySkipped)
          {
            RetryLoop(State, QString(), [&]()
            {
              if (QFileInfo::exists(Source) && !QDir().rmdir(Source))
              {
                // DELETE_LOCAL_FILE_ERROR
                throw ExtException(QStringLiteral("Error deleting file '%1'.").arg(Source));
              }
            });
          }
        }
        else
        {
          RetryLoop(State, QString(), [&]()
          {
            if (!QFile::remove(Source))
            {
              throw ExtException(QStringLiteral("Error deleting file '%1'.").arg(Source));
            }
          });
        }
      }
    }
  }, Done);
}

//---------------------------------------------------------------------------
// Delete, rename, other commands

bool Terminal::IsRecycledFile(const QString & Path) const
{
  // TTerminal::IsRecycledFile
  if (FData.RecycleBinPath.isEmpty())
  {
    return false;
  }
  QString Dir = UnixPath::ExtractFileDir(Path);
  return UnixSamePath(Dir.isEmpty() ? QStringLiteral("/") : Dir, FData.RecycleBinPath);
}

void Terminal::DeleteRemoteRecursive(const QString & Path, const RemoteFile & File, OperationState & State)
{
  CheckOperationCancel(FProgress);
  FProgress.SetFile(Path, 0);
  SessionLog::LogEvent(QStringLiteral("Deleting file \"%1\".").arg(Path));
  SessionAction Action(laRm, Path);
  if (File.IsDirectory && !File.IsSymLink)
  {
    RemoteFileList Children;
    if (!RetryLoop(State, QString(), [&]() { Children = FFileSystem->ReadDirectory(Path); }))
    {
      return;
    }
    for (const RemoteFile & Child : Children)
    {
      DeleteRemoteRecursive(UnixPath::Combine(Path, Child.FileName), Child, State);
    }
    Action.Commit(RetryLoop(State, QString(), [&]() { FFileSystem->RemoveDirectory(Path); }));
  }
  else
  {
    Action.Commit(RetryLoop(State, QString(), [&]() { FFileSystem->DeleteFile(Path); }));
  }
  FProgress.FileFinished();
}

void Terminal::DeleteFiles(const QStringList & Paths, Callback Done, int Params)
{
  FProgress.Start(FileOperationProgress::Delete, FileOperationProgress::ToRemote, Paths.size());
  RunOperation([this, Paths, Params](OperationState & State)
  {
    // TTerminal::DeleteFile
    bool Recycle =
      !(Params & dfForceDelete) &&
      (FData.DeleteToRecycleBin != bool(Params & dfAlternative)) &&
      !FData.RecycleBinPath.isEmpty();
    for (const QString & Path : Paths)
    {
      CheckOperationCancel(FProgress);
      FProgress.SetFile(Path, 0);
      if (Recycle && !IsRecycledFile(Path))
      {
        RecycleFile(Path, State);
        FProgress.FileFinished();
      }
      else
      {
        RemoteFile File;
        if (RetryLoop(State, QString(), [&]() { File = FFileSystem->ReadFile(Path); }))
        {
          DeleteRemoteRecursive(Path, File, State);
        }
      }
    }
  }, Done);
}

bool Terminal::DoRenameFile(const QString & From, const QString & To, OperationState & State, bool Move, bool Batch)
{
  return DoRenameOrCopyFile(true, From, nullptr, To, State, Move, Batch);
}

bool Terminal::DoRenameOrCopyFile(bool Rename, const QString & From, const RemoteFile * File, const QString & To,
  OperationState & State, bool Move, bool Batch)
{
  // TTerminal::DoRenameOrCopyFile
  if (UnixSamePath(From, To))
  {
    // Target is same as source - skipping
    return false;
  }
  bool Result = true;
  bool ExistenceKnown = false;
  RemoteFile Duplicate;
  bool DuplicateExists = false;
  BatchOverwrite ABatch = Batch ? State.Batch : boNo;
  if (ABatch == boNone)
  {
    DuplicateExists = FFileSystem->FileExists(To, &Duplicate);
    ExistenceKnown = true;
    Result = !DuplicateExists;
  }
  else if (ABatch == boAll)
  {
    // noop
  }
  else if (State.Options.ConfirmOverwriting)
  {
    DuplicateExists = FFileSystem->FileExists(To, &Duplicate);
    ExistenceKnown = true;
    if (DuplicateExists && !FFileSystem->FileExists(From, nullptr))
    {
      // Missing source (WinSCP renames listed files only): no question,
      // the rename itself reports the error
      DuplicateExists = false;
    }
    else if (DuplicateExists)
    {
      QString Name = Move ? To : UnixPath::ExtractFileName(To);
      QueryUserParams Params;
      Params.Params = qpNeverAskAgainCheck;
      QString Question;
      if (Duplicate.IsDirectory)
      {
        // DIRECTORY_OVERWRITE + DIRECTORY_OVERWRITE_WARNING
        Question = MainInstructions(QStringLiteral("Directory '%1' already exists. Overwrite?").arg(Name)) +
          QStringLiteral("\n\nAll files in the target directory will be deleted!");
        Params.Type = qtWarning;
      }
      else
      {
        // FILE_OVERWRITE
        Question = MainInstructions(QStringLiteral("File '%1' already exists. Overwrite?").arg(Name));
        Params.Type = qtConfirmation;
      }
      unsigned int Answers = qaYes | qaNo | (Batch ? (qaCancel | qaYesToAll | qaNoToAll) : 0);
      unsigned int Answer = QueryUser(Question, QString(), Answers, Params, &State);
      switch (Answer)
      {
        case qaNeverAskAgain:
          NeverAskAgainOverwriting(State);
          Result = true;
          break;
        case qaYes:
          Result = true;
          break;
        case qaYesToAll:
          Result = true;
          State.Batch = boAll;
          break;
        case qaNoToAll:
          Result = false;
          State.Batch = boNone;
          break;
        case qaCancel:
          FProgress.Cancel();
          throw AbortException();
        default:
          Result = false;
          break;
      }
    }
  }

  if (!Result)
  {
    return false;
  }
  // deliberately after confirmation, so that the user can see what files are being overwritten
  QString ToPrefix = To.endsWith(QLatin1Char('/')) ? To : To + QLatin1Char('/');
  if (From.startsWith(ToPrefix))
  {
    // ANCESTOR_TARGET_ERROR
    throw ExtException(MainInstructions(QStringLiteral("Target of copy or move cannot be an ancestor directory of source.")),
      QStringLiteral("%1 => %2").arg(From, To));
  }
  SessionLog::LogEvent((Rename ? QStringLiteral("Moving file \"%1\" to \"%2\".") : QStringLiteral("Copying file \"%1\" to \"%2\".")).arg(From, To));
  SessionAction Action(Rename ? laMv : laCp, From, To);
  if (!WorkerCapable(fcMoveOverExistingFile))
  {
    if (!ExistenceKnown)
    {
      DuplicateExists = FFileSystem->FileExists(To, &Duplicate);
    }
    if (DuplicateExists && !Rename)
    {
      // a copy never replaces an existing target (SFTP copy-data opens it with EXCL)
      DeleteRemoteRecursive(To, Duplicate, State);
      DuplicateExists = false;
    }
    if (DuplicateExists)
    {
      if (!Duplicate.IsDirectory || Duplicate.IsSymLink)
      {
        // the server may still replace a file atomically (e.g. OpenSSH posix-rename)
        try
        {
          FFileSystem->RenameFile(From, To);
          return Action.Commit(true);
        }
        catch (const FatalException &)
        {
          throw;
        }
        catch (const ExtException &)
        {
        }
      }
      // delete the target only when the source is there to replace it
      if (FFileSystem->FileExists(From, nullptr))
      {
        DeleteRemoteRecursive(To, Duplicate, State);
      }
    }
  }
  if (!Rename)
  {
    // COPY_FILE_ERROR
    return Action.Commit(RetryLoop(State, QStringLiteral("Error copying file '%1' to '%2'.").arg(From, To), [&]()
    {
      RemoteFile Source = (File != nullptr) ? *File : FFileSystem->ReadFile(From);
      CopyFileOnServer(From, Source, To);
    }));
  }
  return Action.Commit(RetryLoop(State, QString(), [&]() { FFileSystem->RenameFile(From, To); }));
}

void Terminal::RecycleFile(const QString & Path, OperationState & State)
{
  // TTerminal::RecycleFile: move to RecycleBinPath as name-yyyymmdd-hhnnss.ext
  if (IsRecycledFile(Path))
  {
    return;
  }
  QString Mask = QStringLiteral("*-%1.*").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
  QString Target = UnixPath::Combine(FData.RecycleBinPath, MaskFileName(UnixPath::ExtractFileName(Path), Mask));
  DoRenameFile(Path, Target, State, true, false);
}

void Terminal::RenameFile(const QString & From, const QString & To, Callback Done)
{
  RunOperation([this, From, To](OperationState & State)
  {
    // no progress operation: a cancel of a previous operation does not apply
    FProgress.ResetCancel();
    DoRenameFile(From, To, State, false, false);
  }, Done);
}

void Terminal::CreateDirectory(const QString & Path, int Mode, Callback Done)
{
  RunOperation([this, Path, Mode](OperationState & State)
  {
    FProgress.ResetCancel();
    SessionLog::LogEvent(QStringLiteral("Creating directory \"%1\".").arg(Path));
    SessionAction Action(laMkdir, Path);
    Action.Commit(RetryLoop(State, QString(), [&]() { FFileSystem->CreateDirectory(Path, Mode); }));
  }, Done);
}

void Terminal::CreateLink(const QString & Path, const QString & Target, Callback Done)
{
  RunOperation([this, Path, Target](OperationState & State)
  {
    FProgress.ResetCancel();
    RetryLoop(State, QString(), [&]() { FFileSystem->CreateLink(Path, Target); });
  }, Done);
}

void Terminal::ChangeModeRecursive(const QString & Path, unsigned int Mode, bool Recursive, bool AddXToDirectories, OperationState & State)
{
  CheckOperationCancel(FProgress);
  FProgress.SetFile(Path, 0);
  RemoteFile File;
  if (!RetryLoop(State, QString(), [&]() { File = FFileSystem->ReadFile(Path); }))
  {
    return;
  }
  if (File.IsSymLink)
  {
    return; // chmod on symlink would change the target
  }
  // TSFTPFileSystem::ChangeFileProperties: the contents first, so that the
  // directory is still accessible while they are processed
  if (Recursive && File.IsDirectory)
  {
    RemoteFileList Children;
    if (RetryLoop(State, QString(), [&]() { Children = FFileSystem->ReadDirectory(Path); }))
    {
      for (const RemoteFile & Child : Children)
      {
        ChangeModeRecursive(UnixPath::Combine(Path, Child.FileName), Mode, true, AddXToDirectories, State);
      }
    }
    FProgress.SetFile(Path, 0);
  }
  unsigned int Effective = Mode;
  if (File.IsDirectory && AddXToDirectories)
  {
    // TRights::AddExecute for directories ("Add X to directories")
    Effective = AddExecute(Mode);
  }
  SessionLog::LogEvent(QStringLiteral("Changing properties of \"%1\" (mode: \"%2\")").arg(Path, Rights::ToText(Effective)));
  SessionAction Action(laChmod, Path);
  Action.Rights(Effective);
  Action.Commit(RetryLoop(State, QString(), [&]() { FFileSystem->ChangeMode(Path, Effective); }));
  FProgress.FileFinished();
}

void Terminal::ChangeFileProperties(const QStringList & Paths, unsigned int Mode, bool Recursive, Callback Done, bool AddXToDirectories)
{
  FProgress.Start(FileOperationProgress::SetProperties, FileOperationProgress::ToRemote, Paths.size());
  RunOperation([this, Paths, Mode, Recursive, AddXToDirectories](OperationState & State)
  {
    for (const QString & Path : Paths)
    {
      ChangeModeRecursive(Path, Mode, Recursive, AddXToDirectories, State);
    }
  }, Done);
}

void Terminal::RunOnWorker(WorkerJob Job, Callback Done)
{
  Run([this, Job = std::move(Job)]() { Job(*FFileSystem, FProgress, *FUI); }, std::move(Done));
}

void Terminal::AnyCommand(const QString & Command, CommandOutputCallback Output, CommandCallback Done, bool RecordAction)
{
  auto ExitCode = std::make_shared<int>(-1);
  auto Directory = std::make_shared<QString>();
  QString StartDirectory = FCurrentDirectory;
  FProgress.Start(FileOperationProgress::Calculate, FileOperationProgress::ToRemote, 1);
  Run([this, Command, Output, ExitCode, Directory, StartDirectory, RecordAction]()
  {
    AnyCommandFileSystem * Fs = dynamic_cast<AnyCommandFileSystem *>(FFileSystem.get());
    if (Fs == nullptr)
    {
      throw ExtException(QStringLiteral("Operation not supported."));
    }
    AnyCommandFileSystem::OutputEvent Event = [this, Output](const QString & Line, bool StdError)
    {
      if (!Output)
      {
        return;
      }
      // Queued before the completion, so lines arrive before Done
      QMetaObject::invokeMethod(this, [this, Output, Line, StdError]()
      {
        if (!FDestroying)
        {
          Output(Line, StdError);
        }
      }, Qt::QueuedConnection);
    };
    SessionLog::LogEvent(QStringLiteral("Executing user defined command."));
    SessionAction Action(laCall, RecordAction ? ActionsLog::Current() : nullptr);
    Action.Command(Command);
    Action.Destination(StartDirectory);
    AnyCommandFileSystem::OutputEvent LoggedEvent = [&Action, Event](const QString & Line, bool StdError) { Action.AddOutput(Line, StdError); Event(Line, StdError); };
    *ExitCode = Fs->AnyCommand(Command, StartDirectory, LoggedEvent, FProgress, Directory.get());
    Action.ExitCode(*ExitCode);
    Action.Commit();
  },
  [this, ExitCode, Directory, Done](const OperationResult & Result)
  {
    if (Result.Ok && !Directory->isEmpty())
    {
      SetCurrentDirectory(*Directory);
    }
    if (Done)
    {
      Done(Result, *ExitCode, Directory->isEmpty() ? FCurrentDirectory : *Directory);
    }
  });
}

//---------------------------------------------------------------------------
// Properties and server side operations: TTerminal::ChangeFilesProperties,
// CalculateFilesSize, CalculateFilesChecksum, CopyFiles, MoveFiles,
// CreateLink, SpaceAvailable, LookupUsersGroups

static bool IsRealChild(const RemoteFile & File)
{
  return !File.IsParentDirectory && (File.FileName != QLatin1String(".")) && (File.FileName != QLatin1String(".."));
}

void Terminal::ChangePropertiesRecursive(const QString & Path, const RemoteFile & File, const RemoteProperties & Properties,
  bool TopLevel, OperationState & State)
{
  CheckOperationCancel(FProgress);
  FProgress.SetFile(Path, 0);
  if (File.IsSymLink)
  {
    // chmod/chown of a symbolic link would change its target
    if (TopLevel)
    {
      FProgress.FileFinished();
    }
    return;
  }
  // CHANGE_PROPERTIES_ERROR
  const QString Message = QStringLiteral("Cannot change properties of file '%1'.").arg(Path);
  if (Properties.Recursive && File.IsDirectory && !FFileSystem->ChangesPropertiesRecursively())
  {
    // TSFTPFileSystem::ChangeFileProperties: the contents first, so that the
    // directory is still accessible while they are processed
    RemoteFileList Children;
    if (RetryLoop(State, Message, [&]() { Children = FFileSystem->ReadDirectory(Path); }))
    {
      for (const RemoteFile & Child : Children)
      {
        if (IsRealChild(Child))
        {
          ChangePropertiesRecursive(UnixPath::Combine(Path, Child.FileName), Child, Properties, false, State);
        }
      }
    }
    FProgress.SetFile(Path, 0);
  }
  SessionLog::LogEvent(QStringLiteral("Changing properties of \"%1\"").arg(Path));
  SessionAction Action(laChmod, Path);
  if (Properties.ValidRights) Action.Rights(Properties.CombineRights(File.Permissions, File.IsDirectory));
  Action.Commit(RetryLoop(State, Message, [&]() { FFileSystem->ChangeFileProperties(Path, File, Properties); }));
  if (TopLevel)
  {
    FProgress.FileFinished();
  }
}

void Terminal::ChangeFilesProperties(const QStringList & Paths, const RemoteProperties & Properties, Callback Done)
{
  FProgress.Start(FileOperationProgress::SetProperties, FileOperationProgress::ToRemote, Paths.size());
  RunOperation([this, Paths, Properties](OperationState & State)
  {
    for (const QString & Path : Paths)
    {
      CheckOperationCancel(FProgress);
      RemoteFile File;
      if (RetryLoop(State, QStringLiteral("Cannot change properties of file '%1'.").arg(Path),
            [&]() { File = FFileSystem->ReadFile(Path); }))
      {
        ChangePropertiesRecursive(Path, File, Properties, true, State);
      }
    }
  }, Done);
}

void Terminal::CreateDirectory(const QString & Path, const RemoteProperties & Properties, Callback Done)
{
  RunOperation([this, Path, Properties](OperationState & State)
  {
    FProgress.ResetCancel();
    // the mode of SSH_FXP_MKDIR is subject to the server's umask; the
    // properties are applied afterwards anyway
    int Mode = Properties.ValidRights ? static_cast<int>(Properties.CombineRights(0755, true)) : ServerDefaultMode;
    // CREATE_DIR_ERROR
    SessionLog::LogEvent(QStringLiteral("Creating directory \"%1\".").arg(Path));
    SessionAction Action(laMkdir, Path);
    if (!Action.Commit(RetryLoop(State, QStringLiteral("Cannot create remote folder '%1'.").arg(Path),
          [&]() { FFileSystem->CreateDirectory(Path, Mode); })))
    {
      return;
    }
    RemoteProperties DirProperties = Properties;
    DirProperties.Recursive = false;
    if (!WorkerCapable(fcModeChanging))
    {
      DirProperties.ValidRights = false;
    }
    if (!WorkerCapable(fcOwnerChanging))
    {
      DirProperties.Owner.clear();
    }
    if (!WorkerCapable(fcGroupChanging))
    {
      DirProperties.Group.clear();
    }
    if (!DirProperties.Empty())
    {
      RetryLoop(State, QStringLiteral("Cannot change properties of file '%1'.").arg(Path), [&]()
      {
        RemoteFile File = FFileSystem->ReadFile(Path);
        FFileSystem->ChangeFileProperties(Path, File, DirProperties);
      });
    }
  }, Done);
}

qint64 Terminal::CalculateSizeRecursive(const QString & Path, const RemoteFile & File, CalculateSizeStats & Stats,
  OperationState & State)
{
  // TTerminal::CalculateFileSize / DoCalculateDirectorySize
  CheckOperationCancel(FProgress);
  qint64 Size = 0;
  if (File.IsDirectory)
  {
    // TTerminal::CanRecurseToDirectory
    if (!File.IsSymLink || FData.FollowDirectorySymlinks)
    {
      FProgress.SetFile(Path, 0);
      RemoteFileList Children;
      // CALCULATE_SIZE_ERROR
      if (RetryLoop(State, QStringLiteral("Error calculating size of directory '%1'.").arg(Path),
            [&]() { Children = FFileSystem->ReadDirectory(Path); }))
      {
        for (const RemoteFile & Child : Children)
        {
          if (IsRealChild(Child))
          {
            Size += CalculateSizeRecursive(UnixPath::Combine(Path, Child.FileName), Child, Stats, State);
          }
        }
      }
    }
    Stats.Directories++;
  }
  else
  {
    Size = File.Size;
    Stats.Files++;
  }
  if (File.IsSymLink)
  {
    Stats.SymLinks++;
  }
  return Size;
}

void Terminal::CalculateFilesSize(const QStringList & Paths, CalculateSizeCallback Done)
{
  struct Totals
  {
    qint64 Size = 0;
    CalculateSizeStats Stats;
    QList<qint64> Sizes;
  };
  auto Result = std::make_shared<Totals>();
  FProgress.Start(FileOperationProgress::Calculate, FileOperationProgress::ToRemote, Paths.size());
  RunOperation([this, Paths, Result](OperationState & State)
  {
    for (const QString & Path : Paths)
    {
      CheckOperationCancel(FProgress);
      FProgress.SetFile(Path, 0);
      RemoteFile File;
      qint64 Size = -1;
      if (RetryLoop(State, QStringLiteral("Error calculating size of directory '%1'.").arg(Path),
            [&]() { File = FFileSystem->ReadFile(Path); }))
      {
        Size = CalculateSizeRecursive(Path, File, Result->Stats, State);
        Result->Size += Size;
      }
      Result->Sizes.append(Size);
      FProgress.FileFinished();
    }
  },
  [Result, Done](const OperationResult & R)
  {
    if (Done)
    {
      Done(R, Result->Size, Result->Stats, Result->Sizes);
    }
  });
}

QString Terminal::ShellChecksumCommand(const QString & Alg)
{
  // TTerminal::GetShellChecksumAlgDefs (Configuration ChecksumCommands default)
  static const char * const Defs[][2] = {
    {"sha-512", "sha512sum"}, {"sha-384", "sha384sum"}, {"sha-256", "sha256sum"},
    {"sha-224", "sha224sum"}, {"sha-1", "sha1sum"}, {"md5", "md5sum"}};
  QString Normalized = QString(Alg).remove(QLatin1Char('-'));
  for (const auto & Def : Defs)
  {
    QString Name = QString::fromLatin1(Def[0]);
    if (QString(Name).remove(QLatin1Char('-')).compare(Normalized, Qt::CaseInsensitive) == 0)
    {
      return QString::fromLatin1(Def[1]);
    }
  }
  return QString();
}

QString Terminal::ShellChecksumFallback(const QString & Command)
{
  static const char * const Fallbacks[][2] = {
    {"sha512sum", "shasum -a 512"}, {"sha384sum", "shasum -a 384"}, {"sha256sum", "shasum -a 256"},
    {"sha224sum", "shasum -a 224"}, {"sha1sum", "shasum -a 1"}, {"md5sum", "md5 -r"}};
  for (const auto & Fallback : Fallbacks)
  {
    if (Command == QLatin1String(Fallback[0]))
    {
      return QString::fromLatin1(Fallback[1]);
    }
  }
  return QString();
}

QStringList Terminal::ShellChecksumAlgs()
{
  return {QStringLiteral("sha-512"), QStringLiteral("sha-384"), QStringLiteral("sha-256"),
    QStringLiteral("sha-224"), QStringLiteral("sha-1"), QStringLiteral("md5")};
}

void Terminal::UpdateChecksumAlgs()
{
  // TTerminal::GetSupportedChecksumAlgs
  QStringList Algs;
  if (FFileSystem && WorkerCapable(fcCalculatingChecksum) && !WorkerCapable(fcShellAnyCommand))
  {
    Algs = FFileSystem->ChecksumAlgorithms();
  }
  else if (FFileSystem && (WorkerCapable(fcShellAnyCommand) || WorkerCapable(fcSecondaryShell)) &&
           (dynamic_cast<AnyCommandFileSystem *>(FFileSystem.get()) != nullptr))
  {
    Algs = ShellChecksumAlgs();
  }
  QMutexLocker Lock(&FChecksumAlgsMutex);
  FChecksumAlgs = Algs;
}

QStringList Terminal::SupportedChecksumAlgs() const
{
  QMutexLocker Lock(&FChecksumAlgsMutex);
  return FChecksumAlgs;
}

// FindIdent: the algorithm as the session names it ("SHA256" -> "sha-256"),
// or Alg itself when the session does not know it (user specified algorithm)
static QString NormalizeChecksumAlg(const QString & Alg, const QStringList & Algs)
{
  QString Normalized = QString(Alg).remove(QLatin1Char('-'));
  for (const QString & Known : Algs)
  {
    if (QString(Known).remove(QLatin1Char('-')).compare(Normalized, Qt::CaseInsensitive) == 0)
    {
      return Known;
    }
  }
  return Alg;
}

void Terminal::ChecksumFiles(const QString & Alg, const QString & Path, const RemoteFile & File, const QString & DisplayName,
  const std::function<void(const QString &, const QString &)> & Report, OperationState & State)
{
  // TTerminal::CalculateSubFoldersChecksum + TCustomFileSystem::CalculateFilesChecksum
  // for the protocol's own checksum support (one file at a time)
  CheckOperationCancel(FProgress);
  FProgress.SetFile(Path, 0);
  if (File.IsDirectory)
  {
    if (!File.IsSymLink || FData.FollowDirectorySymlinks)
    {
      RemoteFileList Children;
      if (RetryLoop(State, QStringLiteral("Error calculating checksum for file '%1'.").arg(Path),
            [&]() { Children = FFileSystem->ReadDirectory(Path); }))
      {
        for (const RemoteFile & Child : Children)
        {
          if (IsRealChild(Child))
          {
            ChecksumFiles(Alg, UnixPath::Combine(Path, Child.FileName), Child,
              DisplayName + QLatin1Char('/') + Child.FileName, Report, State);
          }
        }
      }
    }
    return;
  }
  QString Checksum;
  // CHECKSUM_ERROR
  if (RetryLoop(State, QStringLiteral("Error calculating checksum for file '%1'.").arg(Path),
        [&]() { Checksum = FFileSystem->CalculateFileChecksum(Path, Alg); }))
  {
    Report(DisplayName, Checksum);
  }
}

void Terminal::CalculateFilesChecksum(const QString & Alg, const QStringList & Paths, ChecksumCallback OnChecksum, Callback Done)
{
  QString NormalizedAlg = NormalizeChecksumAlg(Alg, SupportedChecksumAlgs());
  FProgress.Start(FileOperationProgress::Calculate, FileOperationProgress::ToRemote, Paths.size());
  RunOperation([this, NormalizedAlg, Paths, OnChecksum](OperationState & State)
  {
    auto Report = [this, NormalizedAlg, OnChecksum](const QString & Name, const QString & Checksum)
    {
      if (OnChecksum)
      {
        // Queued before the completion, so all results arrive before Done
        QMetaObject::invokeMethod(this, [this, OnChecksum, Name, NormalizedAlg, Checksum]()
        {
          if (!FDestroying)
          {
            OnChecksum(Name, NormalizedAlg, Checksum);
          }
        }, Qt::QueuedConnection);
      }
    };

    // the protocol's own support (SFTP check-file, FTP HASH); SCP calculates
    // checksums with shell commands (fcCalculatingChecksum by fcShellAnyCommand)
    if (WorkerCapable(fcCalculatingChecksum) && !WorkerCapable(fcShellAnyCommand))
    {
      for (const QString & Path : Paths)
      {
        RemoteFile File;
        if (RetryLoop(State, QStringLiteral("Error calculating checksum for file '%1'.").arg(Path),
              [&]() { File = FFileSystem->ReadFile(Path); }))
        {
          ChecksumFiles(NormalizedAlg, Path, File, UnixPath::ExtractFileName(Path), Report, State);
        }
        FProgress.FileFinished();
      }
      return;
    }

    // TSCPFileSystem::CalculateFilesChecksum: "sha256sum file1 file2 ..." in
    // the session shell (SCP) or a separate exec channel (SFTP secondary shell)
    AnyCommandFileSystem * Shell = dynamic_cast<AnyCommandFileSystem *>(FFileSystem.get());
    if ((Shell == nullptr) || !(WorkerCapable(fcShellAnyCommand) || WorkerCapable(fcSecondaryShell)))
    {
      throw ExtException(QStringLiteral("The server does not support the operation."));
    }
    QString Command = ShellChecksumCommand(NormalizedAlg);
    if (Command.isEmpty())
    {
      // user-specified algorithm used as the command
      Command = NormalizedAlg;
    }

    // Files to process (directories recursively), with their display names
    struct Item
    {
      QString Path;
      QString Name;
      qint64 Size;
    };
    QList<Item> Items;
    std::function<void(const QString &, const RemoteFile &, const QString &)> Collect =
      [&](const QString & Path, const RemoteFile & File, const QString & Name)
    {
      CheckOperationCancel(FProgress);
      if (File.IsDirectory)
      {
        if (!File.IsSymLink || FData.FollowDirectorySymlinks)
        {
          FProgress.SetFile(Path, 0);
          RemoteFileList Children;
          if (RetryLoop(State, QStringLiteral("Error calculating checksum for file '%1'.").arg(Path),
                [&]() { Children = FFileSystem->ReadDirectory(Path); }))
          {
            for (const RemoteFile & Child : Children)
            {
              if (IsRealChild(Child))
              {
                Collect(UnixPath::Combine(Path, Child.FileName), Child, Name + QLatin1Char('/') + Child.FileName);
              }
            }
          }
        }
      }
      else
      {
        Items.append({Path, Name, File.Size});
      }
    };
    for (const QString & Path : Paths)
    {
      RemoteFile File;
      if (RetryLoop(State, QStringLiteral("Error calculating checksum for file '%1'.").arg(Path),
            [&]() { File = FFileSystem->ReadFile(Path); }))
      {
        Collect(Path, File, UnixPath::ExtractFileName(Path));
      }
    }

    QString Fallback = ShellChecksumFallback(Command);
    auto Run = [&](const QString & Arguments, QStringList & Output) -> bool
    {
      while (true)
      {
        Output.clear();
        AnyCommandFileSystem::OutputEvent Event = [&Output](const QString & Line, bool StdError)
        {
          if (!StdError)
          {
            Output.append(Line);
          }
        };
        int ExitCode = Shell->AnyCommand(Command + Arguments, QString(), Event, FProgress, nullptr);
        // 126/127: the command does not exist or cannot be executed
        if (((ExitCode != 126) && (ExitCode != 127)) || Fallback.isEmpty())
        {
          return (ExitCode == 0);
        }
        Command = Fallback;
        Fallback.clear();
      }
    };

    qsizetype Index = 0;
    while (Index < Items.size())
    {
      CheckOperationCancel(FProgress);
      // Batches of up to 2048 characters / 1 GB
      QString Arguments;
      qint64 BatchSize = 0;
      qsizetype First = Index;
      while (Index < Items.size())
      {
        QString Arg = QLatin1Char(' ') + ShellQuoteStr(Items[Index].Path);
        if ((Index > First) && (((Command.size() + Arguments.size() + Arg.size()) > 2048) || ((BatchSize + Items[Index].Size) > 1024LL * 1024 * 1024)))
        {
          break;
        }
        Arguments += Arg;
        BatchSize += Items[Index].Size;
        Index++;
      }
      FProgress.SetFile(Items[First].Path, 0);
      QStringList Output;
      QHash<QString, QString> Checksums;
      bool BatchOk = false;
      try
      {
        BatchOk = Run(Arguments, Output);
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
        BatchOk = false;
      }
      if (BatchOk)
      {
        Checksums = ParseChecksumOutput(Output);
        for (qsizetype I = First; BatchOk && (I < Index); I++)
        {
          BatchOk = Checksums.contains(Items[I].Path);
        }
      }
      for (qsizetype I = First; I < Index; I++)
      {
        const Item & It = Items[I];
        FProgress.SetFile(It.Path, 0);
        QString Checksum;
        if (BatchOk)
        {
          Checksum = Checksums.value(It.Path);
        }
        else
        {
          // Batch checksum calculation failed, falling back to calculating
          // checksum individually for each file
          if (!RetryLoop(State, QStringLiteral("Error calculating checksum for file '%1'.").arg(It.Path), [&]()
              {
                QStringList Single;
                QString SingleArgument = QLatin1Char(' ') + ShellQuoteStr(It.Path);
                bool Ok = Run(SingleArgument, Single);
                QHash<QString, QString> Parsed = ParseChecksumOutput(Single);
                if (!Ok || !Parsed.contains(It.Path))
                {
                  // INVALID_OUTPUT_ERROR
                  throw ExtException(QStringLiteral("Unexpected output of command '%1'.").arg(Command + SingleArgument),
                    Single.join(QLatin1Char('\n')));
                }
                Checksum = Parsed.value(It.Path);
              }))
          {
            continue;
          }
        }
        Report(It.Name, Checksum);
      }
    }
  }, Done);
}

Terminal::DirectRemoteCopy Terminal::AllowDirectRemoteCopy(bool AnyDirectory) const
{
  // TCustomScpExplorerForm::RemoteTransferDialog / NeedSecondarySessionForRemoteCopy
  if (IsCapable(fcShellAnyCommand))
  {
    // the session shell copies anything ("cp -p -r -f")
    return drcAllow;
  }
  bool RemoteCopy = IsCapable(fcRemoteCopy);
  bool SecondaryShell = IsCapable(fcSecondaryShell) && CanAnyCommand();
  if (RemoteCopy && (!AnyDirectory || !SecondaryShell))
  {
    return drcAllow;
  }
  if (SecondaryShell)
  {
    return RemoteCopy ? drcConfirmCommandSessionDirs : drcConfirmCommandSession;
  }
  return drcDisallow;
}

void Terminal::CopyFileOnServer(const QString & From, const RemoteFile & File, const QString & To)
{
  // The protocol decides: SFTP copy-data/copy-file for files and the
  // secondary shell for directories, SCP "cp", FTP SITE CPFR/CPTO
  Q_UNUSED(File);
  FFileSystem->CopyFile(From, To);
}

void Terminal::CopyFiles(const QStringList & Paths, const QString & TargetDir, const QString & FileMask, Callback Done)
{
  FProgress.Start(FileOperationProgress::Copy, FileOperationProgress::ToRemote, Paths.size());
  RunOperation([this, Paths, TargetDir, FileMask](OperationState & State)
  {
    for (const QString & Path : Paths)
    {
      CheckOperationCancel(FProgress);
      FProgress.SetFile(Path, 0);
      RemoteFile File;
      if (RetryLoop(State, QStringLiteral("Error copying file '%1' to '%2'.").arg(Path, TargetDir),
            [&]() { File = FFileSystem->ReadFile(Path); }))
      {
        QString NewName = UnixPath::Combine(TargetDir,
          MaskFileName(UnixPath::ExtractFileName(Path), FileMask.isEmpty() ? QStringLiteral("*.*") : FileMask));
        try
        {
          DoRenameOrCopyFile(false, Path, &File, NewName, State, false, true);
        }
        catch (const SkipFileException &)
        {
        }
      }
      FProgress.FileFinished();
    }
  }, Done);
}

void Terminal::MoveFiles(const QStringList & Paths, const QString & TargetDir, const QString & FileMask, Callback Done)
{
  FProgress.Start(FileOperationProgress::Move, FileOperationProgress::ToRemote, Paths.size());
  RunOperation([this, Paths, TargetDir, FileMask](OperationState & State)
  {
    for (const QString & Path : Paths)
    {
      CheckOperationCancel(FProgress);
      FProgress.SetFile(Path, 0);
      QString NewName = UnixPath::Combine(TargetDir,
        MaskFileName(UnixPath::ExtractFileName(Path), FileMask.isEmpty() ? QStringLiteral("*.*") : FileMask));
      try
      {
        DoRenameFile(Path, NewName, State, true, true);
      }
      catch (const SkipFileException &)
      {
      }
      FProgress.FileFinished();
    }
  }, Done);
}

void Terminal::CreateLink(const QString & Path, const QString & Target, bool Symbolic, Callback Done)
{
  RunOperation([this, Path, Target, Symbolic](OperationState & State)
  {
    FProgress.ResetCancel();
    // CREATE_LINK_ERROR
    RetryLoop(State, QStringLiteral("Cannot create link '%1'.").arg(Path), [&]()
    {
      if (Symbolic)
      {
        FFileSystem->CreateLink(Path, Target);
      }
      else
      {
        FFileSystem->CreateHardLink(Path, Target);
      }
    });
  }, Done);
}

void Terminal::SpaceAvailable(const QString & Path, SpaceAvailableCallback Done)
{
  auto Space = std::make_shared<SpaceAvailableInfo>();
  RunOperation([this, Path, Space](OperationState &)
  {
    FProgress.ResetCancel();
    try
    {
      *Space = FFileSystem->SpaceAvailable(Path);
    }
    catch (const FatalException &)
    {
      throw;
    }
    catch (const AbortException &)
    {
      throw;
    }
    catch (const ExtException & E)
    {
      // SPACE_AVAILABLE_ERROR
      throw ExtException(QStringLiteral("Error checking space available for path '%1'.").arg(Path),
        ErrorText(E.Message(), E.MoreMessages()));
    }
  },
  [Space, Done](const OperationResult & Result)
  {
    if (Done)
    {
      Done(Result, *Space);
    }
  });
}

void Terminal::LookupUsersGroups(UsersGroupsCallback Done)
{
  auto Users = std::make_shared<QStringList>();
  auto Groups = std::make_shared<QStringList>();
  RunOperation([this, Users, Groups](OperationState &)
  {
    FProgress.ResetCancel();
    if (FData.LookupUserGroups == asOff)
    {
      return;
    }
    try
    {
      FFileSystem->LookupUsersGroups(*Users, *Groups);
    }
    catch (const FatalException &)
    {
      throw;
    }
    catch (const ExtException & E)
    {
      if (FData.LookupUserGroups == asOn)
      {
        // LOOKUP_GROUPS_ERROR
        throw ExtException(QStringLiteral("Error looking up user groups."), ErrorText(E.Message(), E.MoreMessages()));
      }
    }
  },
  [Users, Groups, Done](const OperationResult & Result)
  {
    if (Done)
    {
      Done(Result, *Users, *Groups);
    }
  });
}
