// Port of WinSCP custom command execution: forms/CustomScpExplorer.cpp
// (CustomCommand, RemoteCustomCommand, LocalCustomCommand, LocalCustomCommandPure,
// LocalCustomCommandWithLocalFiles, AdHocCustomCommand, TerminalCaptureLog),
// core/Terminal.cpp (CustomCommandOnFiles, CustomCommandOnFile,
// DoCustomCommandOnFile), core/ScpFileSystem.cpp (CustomCommandOnFile) and
// windows/WinInterface.cpp (TWinInteractiveCustomCommand, TCustomCommandPromptsDialog)
#include "gui/CustomCommandRunner.h"
#include "gui/HelpSystem.h"
#include "gui/CustomCommandDialog.h"
#include "gui/EditMaskDialog.h"

#include "core/Configuration.h"
#include "core/CopyParam.h"
#include "core/Platform.h"
#include "core/SecureShell.h"
#include "core/Terminal.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QTemporaryDir>
#include <QVBoxLayout>

#include <memory>

namespace
{

QString ImpossibleMessage(const QString & Command)
{
  // CUSTOM_COMMAND_IMPOSSIBLE2
  return QStringLiteral("Custom command '%1' cannot be executed right now.\n\n"
    "You may need to select files for the command or open a session first.").arg(Command);
}

QString AbsoluteRemotePath(const QString & Directory, const QString & Name)
{
  return UnixPath::Combine(Directory, Name);
}

// TTerminal::MakeLocalFileList over a directory (ProcessLocalDirectory):
// entries in name order, directories before their contents
void AddLocalDirectory(const QString & Directory, bool IncludeDirs, QStringList & List)
{
  const QFileInfoList Entries = QDir(Directory).entryInfoList(
    QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name);
  for (const QFileInfo & Entry : Entries)
  {
    if (Entry.isDir())
    {
      if (IncludeDirs)
      {
        List.append(Entry.filePath());
      }
      if (!Entry.isSymLink())
      {
        AddLocalDirectory(Entry.filePath(), IncludeDirs, List);
      }
    }
    else
    {
      List.append(Entry.filePath());
    }
  }
}

// Remote part of TTerminal::CustomCommandOnFiles, running on the worker thread
struct RemoteFilesJob
{
  CustomCommandData Data;
  QString Command;              // after the interactive pass
  QString Directory;
  QStringList Names;
  int Params = 0;
  bool ExitCode1IsError = false;
  bool FollowDirectorySymlinks = false;
  bool Capture = false;
  std::shared_ptr<QStringList> Captured;

  FileSystem * Fs = nullptr;
  AnyCommandFileSystem * AnyFs = nullptr;
  FileOperationProgress * Progress = nullptr;
  UserInterface * UI = nullptr;

  // TTerminal::CanRecurseToDirectory
  bool IsDirectory(const RemoteFile & File) const
  {
    return File.IsDirectory && (!File.IsSymLink || FollowDirectorySymlinks);
  }

  RemoteFile Stat(const QString & Name) const
  {
    RemoteFile File;
    try
    {
      File = Fs->ReadFile(AbsoluteRemotePath(Directory, Name));
    }
    catch (const FatalException &)
    {
      throw;
    }
    catch (const ExtException &)
    {
      // let the command itself report a missing file
      File.FileName = UnixPath::ExtractFileName(Name);
    }
    return File;
  }

  void Run()
  {
    if (RemoteCustomCommand().IsFileListCommand(Command))
    {
      QStringList FileList;
      for (const QString & Name : Names)
      {
        if (!IsDirectory(Stat(Name)) || ((Params & ccApplyToDirectories) != 0))
        {
          FileList.append(Name);
        }
      }
      QString Cmd = RemoteCustomCommand(Data, Directory, QString(), MakeShellFileList(FileList)).Complete(Command, true);
      Execute(Cmd);
    }
    else
    {
      for (const QString & Name : Names)
      {
        Progress->CheckCancel();
        CustomCommandOnFile(Name, Stat(Name));
        Progress->FileFinished();
      }
    }
  }

  // TSCPFileSystem::CustomCommandOnFile
  void CustomCommandOnFile(const QString & FileName, const RemoteFile & File)
  {
    bool Dir = IsDirectory(File);
    if (Dir && ((Params & ccRecursive) != 0))
    {
      // TTerminal::ProcessDirectory
      RemoteFileList Files = Fs->ReadDirectory(AbsoluteRemotePath(Directory, FileName));
      QString Prefix = FileName.endsWith(QLatin1Char('/')) ? FileName : (FileName + QLatin1Char('/'));
      for (const RemoteFile & Child : Files)
      {
        if (Child.IsParentDirectory || (Child.FileName == QStringLiteral(".")) || (Child.FileName == QStringLiteral("..")))
        {
          continue;
        }
        Progress->CheckCancel();
        CustomCommandOnFile(Prefix + Child.FileName, Child);
      }
    }

    if (!Dir || ((Params & ccApplyToDirectories) != 0))
    {
      Progress->SetFile(FileName, 0);
      QString Cmd = RemoteCustomCommand(Data, Directory, FileName, QString()).Complete(Command, true);
      // TTerminal::DoCustomCommandOnFile with TRetryOperationLoop
      while (true)
      {
        try
        {
          Execute(Cmd);
          break;
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
          // CUSTOM_COMMAND_ERROR
          QString Message = QStringLiteral("Error executing custom command '%1' on file '%2'.").arg(Command, FileName);
          QString More = E.Message();
          if (!E.MoreMessages().isEmpty())
          {
            More += QLatin1Char('\n') + E.MoreMessages();
          }
          unsigned int Answer = UI->QueryUser(Message, More, qaAbort | qaRetry | qaSkip);
          if (Answer == qaRetry)
          {
            continue;
          }
          if (Answer == qaSkip)
          {
            break;
          }
          throw AbortException();
        }
      }
    }
  }

  // TSCPFileSystem::AnyCommand: exit code 1 is a warning, unless ExitCode1IsError
  void Execute(const QString & Cmd)
  {
    QStringList ErrorLines;
    QString NewDirectory;
    int ExitCode = AnyFs->AnyCommand(Cmd, Directory,
      [this, &ErrorLines](const QString & Line, bool StdError)
      {
        // TerminalCaptureLog: output and error lines
        if (Capture)
        {
          Captured->append(Line);
        }
        if (StdError)
        {
          ErrorLines.append(Line);
        }
      },
      *Progress, &NewDirectory);
    if ((ExitCode > 1) || ((ExitCode == 1) && ExitCode1IsError))
    {
      if (ErrorLines.isEmpty())
      {
        throw ExtException(QStringLiteral("Command failed with return code %1.").arg(ExitCode));
      }
      throw ExtException(QStringLiteral("Command '%1'\nfailed with return code %2 and the following error message.").arg(Cmd).arg(ExitCode),
        ErrorLines.join(QLatin1Char('\n')));
    }
  }
};

// One execution of a custom command; deletes itself when finished
class Runner : public QObject
{
public:
  Runner(QWidget * Parent, const CustomCommandType & Command, const CustomCommandContext & Context, std::function<void()> Done) :
    FParent(Parent),
    FCommand(Command),
    FContext(Context),
    FTerm(Context.Term),
    FDone(std::move(Done))
  {
    if (FTerm)
    {
      // the tab was closed meanwhile: no callbacks will come anymore
      connect(FTerm.data(), &QObject::destroyed, this, [this]()
      {
        FDone = nullptr;
        FFinished = true;
        deleteLater();
      });
    }
  }

  void Start();

private:
  QPointer<QWidget> FParent;
  CustomCommandType FCommand;
  CustomCommandContext FContext;
  QPointer<Terminal> FTerm;
  std::function<void()> FDone;
  bool FFinished = false;
  CustomCommandData FData;
  QString FCmd;

  // local commands
  bool FCaptureOutput = false;
  QString FOutput;
  QStringList FQueue;
  std::function<void()> FThen;
  std::unique_ptr<QTemporaryDir> FTempDir;
  QMap<QString, qint64> FFileTimes;          // downloaded file -> modification (ms)
  QMap<QString, QString> FTempToRemote;      // top level temporary name -> remote path

  bool SessionAvailable() const { return FTerm && FTerm->Active(); }
  QString WorkingDirectory() const;
  void Finish();
  void Fail();
  void Error(const QString & Message, const QString & More = QString());
  bool CommandFailed(int ExitCode) const;

  void RemoteCommand();
  void RemoteFileCommand();
  void RemoteResults(const QStringList & Lines);

  void LocalCommand();
  void LocalCommandWithLocalFiles(bool FileListCommand);
  void LocalCommandPure(bool FileListCommand, bool LocalFileCommand);
  void RunPure(const QStringList & RemoteFileList, const QStringList & LocalFileList,
    bool FileListCommand, bool LocalFileCommand, bool RemoteFiles);
  void UploadModified();
  void UploadNext(QStringList Files);
  void RunLocal(const QStringList & Commands, bool Wait, std::function<void()> Then);
  void RunNext();
  void LocalResults();
};

void Runner::Finish()
{
  if (!FFinished)
  {
    FFinished = true;
    FTempDir.reset();
    if (FDone)
    {
      FDone();
    }
    deleteLater();
  }
}

void Runner::Fail()
{
  FQueue.clear();
  Finish();
}

void Runner::Error(const QString & Message, const QString & More)
{
  QMessageBox Box(QMessageBox::Critical, QStringLiteral("Error"), Message, QMessageBox::Ok, FParent);
  if (!More.isEmpty())
  {
    if (More.size() < 400)
    {
      Box.setInformativeText(More);
    }
    else
    {
      Box.setDetailedText(More);
    }
  }
  Box.exec();
}

bool Runner::CommandFailed(int ExitCode) const
{
  return (ExitCode > 1) || ((ExitCode == 1) && FTerm && FTerm->Data().ExitCode1IsError);
}

QString Runner::WorkingDirectory() const
{
  if (!FContext.CurrentLocalDir.isEmpty() && QFileInfo(FContext.CurrentLocalDir).isDir())
  {
    return FContext.CurrentLocalDir;
  }
  return QDir::homePath();
}

void Runner::Start()
{
  if (SessionAvailable())
  {
    FData = CustomCommandData::FromSession(FTerm->Data(), FContext.SessionPassword);
  }

  bool Local = FCommand.IsLocal();
  try
  {
    // TWinInteractiveCustomCommand
    RemoteCustomCommand RemoteCommand(FData, FContext.CurrentRemoteDir);
    LocalCustomCommand LocalCommand(FData, FContext.CurrentRemoteDir, FContext.CurrentLocalDir);
    FileCustomCommand * Child = Local ? static_cast<FileCustomCommand *>(&LocalCommand) : &RemoteCommand;
    QString Name = FCommand.PlainName();
    QPointer<QWidget> Parent = FParent;
    InteractiveCustomCommand Interactive(Child,
      [Parent, Name](const QStringList & Prompts, const QStringList & Defaults, QStringList & Values)
      {
        return CustomCommandRunner::PromptValues(Parent, Name, Prompts, Defaults, Values);
      });
    FCmd = Interactive.Complete(FCommand.Command, false);
  }
  catch (const AbortException &)
  {
    Finish();
    return;
  }
  catch (const ExtException & E)
  {
    Error(E.Message(), E.MoreMessages());
    Finish();
    return;
  }

  if (Local)
  {
    LocalCommand();
  }
  else
  {
    RemoteCommand();
  }
}

//---------------------------------------------------------------------------
// Remote commands

void Runner::RemoteCommand()
{
  // CustomCommandRemoteAllowed / EnsureCommandSessionFallback
  if (!SessionAvailable() || !FTerm->CanAnyCommand() ||
      // CustomScpExplorer.cpp: fcSecondaryShell || fcShellAnyCommand (FTP has raw commands only)
      !(FTerm->IsCapable(fcShellAnyCommand) || FTerm->IsCapable(fcSecondaryShell)))
  {
    Error(ImpossibleMessage(FCommand.Command));
    Finish();
    return;
  }

  bool FileCommand = false;
  QString Final;
  try
  {
    RemoteCustomCommand Analyzer;
    FileCommand = Analyzer.IsFileCommand(FCmd);
    if (!FileCommand)
    {
      Final = RemoteCustomCommand(FData, FContext.CurrentRemoteDir).Complete(FCmd, true);
    }
  }
  catch (const ExtException & E)
  {
    Error(E.Message(), E.MoreMessages());
    Finish();
    return;
  }

  if (FileCommand)
  {
    RemoteFileCommand();
    return;
  }

  bool Capture = (FCommand.Params & (ccShowResults | ccCopyResults | ccShowResultsInMsgBox)) != 0;
  auto Lines = std::make_shared<QStringList>();
  auto ErrorLines = std::make_shared<QStringList>();
  FTerm->AnyCommand(Final,
    [Capture, Lines, ErrorLines](const QString & Line, bool StdError)
    {
      if (Capture)
      {
        Lines->append(Line);
      }
      if (StdError)
      {
        ErrorLines->append(Line);
      }
    },
    [this, Lines, ErrorLines, Final](const OperationResult & Result, int ExitCode, const QString &)
    {
      if (!Result.Ok)
      {
        if (!Result.Aborted)
        {
          Error(Result.Message, Result.MoreMessages);
        }
      }
      else if (CommandFailed(ExitCode))
      {
        if (ErrorLines->isEmpty())
        {
          Error(QStringLiteral("Command failed with return code %1.").arg(ExitCode));
        }
        else
        {
          Error(QStringLiteral("Command '%1'\nfailed with return code %2 and the following error message.").arg(Final).arg(ExitCode),
            ErrorLines->join(QLatin1Char('\n')));
        }
      }
      else
      {
        RemoteResults(*Lines);
      }
      Finish();
    });
}

void Runner::RemoteFileCommand()
{
  if (FContext.SelectedRemoteFiles.isEmpty())
  {
    Error(ImpossibleMessage(FCommand.Command));
    Finish();
    return;
  }

  auto Job = std::make_shared<RemoteFilesJob>();
  Job->Data = FData;
  Job->Command = FCmd;
  Job->Directory = FContext.CurrentRemoteDir;
  Job->Names = FContext.SelectedRemoteFiles;
  Job->Params = FCommand.Params;
  Job->ExitCode1IsError = FTerm->Data().ExitCode1IsError;
  Job->FollowDirectorySymlinks = FTerm->Data().FollowDirectorySymlinks;
  Job->Capture = (FCommand.Params & (ccShowResults | ccCopyResults | ccShowResultsInMsgBox)) != 0;
  Job->Captured = std::make_shared<QStringList>();
  std::shared_ptr<QStringList> Captured = Job->Captured;

  FTerm->RunOnWorker(
    [Job](FileSystem & Fs, FileOperationProgress & Progress, UserInterface & UI)
    {
      Job->Fs = &Fs;
      Job->AnyFs = dynamic_cast<AnyCommandFileSystem *>(&Fs);
      if (Job->AnyFs == nullptr)
      {
        throw ExtException(QStringLiteral("Operation not supported."));
      }
      Job->Progress = &Progress;
      Job->UI = &UI;
      Progress.Start(FileOperationProgress::Calculate, FileOperationProgress::ToRemote, Job->Names.size());
      Job->Run();
    },
    [this, Captured](const OperationResult & Result)
    {
      if (!Result.Ok)
      {
        if (!Result.Aborted)
        {
          Error(Result.Message, Result.MoreMessages);
        }
      }
      else
      {
        RemoteResults(*Captured);
      }
      Finish();
    });
}

void Runner::RemoteResults(const QStringList & Lines)
{
  if (Lines.isEmpty())
  {
    return;
  }
  if ((FCommand.Params & ccCopyResults) != 0)
  {
    // CopyToClipboard(TStrings *): every line terminated
    QApplication::clipboard()->setText(Lines.join(QLatin1Char('\n')) + QLatin1Char('\n'));
  }
  if ((FCommand.Params & ccShowResults) != 0)
  {
    CustomCommandRunner::ShowResults(FParent, FCommand.PlainName(), Lines);
  }
  else if ((FCommand.Params & ccShowResultsInMsgBox) != 0)
  {
    QMessageBox::information(FParent, FCommand.PlainName(), Lines.join(QLatin1Char('\n')));
  }
}

//---------------------------------------------------------------------------
// Local commands

void Runner::LocalCommand()
{
  bool FileCommand = false;
  bool FileListCommand = false;
  bool LocalFileCommand = false;
  bool SessionCommand = false;
  QString Final;
  try
  {
    LocalCustomCommand Analyzer(FData, FContext.CurrentRemoteDir, FContext.CurrentLocalDir);
    FileCommand = Analyzer.IsFileCommand(FCmd);
    FileListCommand = Analyzer.IsFileListCommand(FCmd);
    LocalFileCommand = Analyzer.HasLocalFileName(FCmd);
    SessionCommand = Analyzer.IsSessionCommand(FCmd);
    if (!FileCommand)
    {
      Final = Analyzer.Complete(FCmd, true);
    }
  }
  catch (const ExtException & E)
  {
    Error(E.Message(), E.MoreMessages());
    Finish();
    return;
  }

  if (SessionCommand && !SessionAvailable())
  {
    Error(ImpossibleMessage(FCommand.Command));
    Finish();
    return;
  }

  FCaptureOutput = (FCommand.Params & (ccShowResultsInMsgBox | ccCopyResults)) != 0;

  if (!FileCommand)
  {
    RunLocal({Final}, false, [this]() { LocalResults(); Finish(); });
  }
  // remote files? (IsSideLocalBrowser: the local panel or a local-local tab)
  else if (!(FContext.LocalSide || !FContext.RemotePanel) || LocalFileCommand)
  {
    LocalCommandPure(FileListCommand, LocalFileCommand);
  }
  // local files
  else
  {
    LocalCommandWithLocalFiles(FileListCommand);
  }
}

void Runner::LocalCommandWithLocalFiles(bool FileListCommand)
{
  QStringList LocalFileList;
  for (const QString & FileName : FContext.SelectedLocalFiles)
  {
    QFileInfo Info(FileName);
    if (Info.isDir())
    {
      if ((FCommand.Params & ccApplyToDirectories) != 0)
      {
        LocalFileList.append(FileName);
      }
      if ((FCommand.Params & ccRecursive) != 0)
      {
        AddLocalDirectory(FileName, (FCommand.Params & ccApplyToDirectories) != 0, LocalFileList);
      }
    }
    else
    {
      LocalFileList.append(FileName);
    }
  }

  QStringList Commands;
  try
  {
    if (FileListCommand)
    {
      Commands.append(LocalCustomCommand(FData, FContext.CurrentRemoteDir, FContext.CurrentLocalDir,
        QString(), QString(), MakeLocalFileList(LocalFileList)).Complete(FCmd, true));
    }
    else
    {
      for (const QString & FileName : LocalFileList)
      {
        Commands.append(LocalCustomCommand(FData, FContext.CurrentRemoteDir, FContext.CurrentLocalDir,
          FileName, QString(), QString()).Complete(FCmd, true));
      }
    }
  }
  catch (const ExtException & E)
  {
    Error(E.Message(), E.MoreMessages());
    Finish();
    return;
  }

  // the file list command does not wait (ExecuteProcessChecked)
  RunLocal(Commands, !FileListCommand, [this]() { LocalResults(); Finish(); });
}

void Runner::LocalCommandPure(bool FileListCommand, bool LocalFileCommand)
{
  QStringList LocalFileList;
  if (LocalFileCommand)
  {
    LocalFileList = FContext.SelectedLocalFiles;
    if (FileListCommand)
    {
      if (LocalFileList.size() != 1)
      {
        // CUSTOM_COMMAND_SELECTED_UNMATCH1
        Error(QStringLiteral("To use selected custom command only one file must be selected in local panel."));
        Finish();
        return;
      }
    }
    else if ((LocalFileList.size() != 1) && (FContext.SelectedRemoteFiles.size() != 1) &&
             (LocalFileList.size() != FContext.SelectedRemoteFiles.size()))
    {
      // CUSTOM_COMMAND_SELECTED_UNMATCH
      Error(QStringLiteral("To use selected custom command only one file must be selected in one panel to execute the command "
        "with the file for each selected file in an opposite panel. Alternatively same number of files can be selected in both "
        "panels to execute the command for file pairs."));
      Finish();
      return;
    }
  }

  bool RemoteFiles = (FCommand.Params & ccRemoteFiles) != 0;
  if (RemoteFiles)
  {
    RunPure(FContext.SelectedRemoteFiles, LocalFileList, FileListCommand, LocalFileCommand, true);
    return;
  }

  // TemporarilyDownloadFiles
  if (!SessionAvailable())
  {
    Error(ImpossibleMessage(FCommand.Command));
    Finish();
    return;
  }
  if (FContext.SelectedRemoteFiles.isEmpty())
  {
    RunPure(QStringList(), LocalFileList, FileListCommand, LocalFileCommand, false);
    return;
  }
  FTempDir = std::make_unique<QTemporaryDir>(QDir::tempPath() + QStringLiteral("/nixscp-XXXXXX"));
  if (!FTempDir->isValid())
  {
    Error(QStringLiteral("Cannot create temporary directory."), FTempDir->errorString());
    Finish();
    return;
  }
  Platform::MakeDirectoryPrivate(FTempDir->path());
  QStringList RemotePaths;
  for (const QString & Name : FContext.SelectedRemoteFiles)
  {
    QString RemotePath = AbsoluteRemotePath(FContext.CurrentRemoteDir, Name);
    RemotePaths.append(RemotePath);
    FTempToRemote.insert(CopyParam::ValidLocalFileName(UnixPath::ExtractFileName(RemotePath)), RemotePath);
  }
  CopyParam Param = Configuration::Instance().DefaultCopyParam().TemporaryFileCopyParam();
  Param.ConfirmOverwriting = false;
  Param.Queue = false;
  FTerm->CopyToLocal(RemotePaths, FTempDir->path(), Param, false,
    [this, RemotePaths, LocalFileList, FileListCommand, LocalFileCommand](const OperationResult & Result)
    {
      if (!Result.Ok)
      {
        if (!Result.Aborted)
        {
          Error(Result.Message, Result.MoreMessages);
        }
        Finish();
        return;
      }

      QString TempDir = FTempDir->path();
      bool IncludeDirs = (FCommand.Params & ccApplyToDirectories) != 0;
      bool Recursive = ((FCommand.Params & ccRecursive) != 0) && !FileListCommand;
      QStringList RemoteFileList;
      for (const QString & RemotePath : RemotePaths)
      {
        // as the download named it: on Windows the name of the server is not a local one
        QString Path = TempDir + QLatin1Char('/') + CopyParam::ValidLocalFileName(UnixPath::ExtractFileName(RemotePath));
        QFileInfo Info(Path);
        if (!Info.exists())
        {
          continue;
        }
        if (Info.isDir())
        {
          if (IncludeDirs)
          {
            RemoteFileList.append(Path);
          }
          if (Recursive)
          {
            AddLocalDirectory(Path, IncludeDirs, RemoteFileList);
          }
        }
        else
        {
          RemoteFileList.append(Path);
        }
      }

      // modification times of all downloaded files, to upload the changed ones
      QStringList AllFiles;
      AddLocalDirectory(TempDir, false, AllFiles);
      for (const QString & File : AllFiles)
      {
        FFileTimes.insert(File, QFileInfo(File).lastModified().toMSecsSinceEpoch());
      }

      RunPure(RemoteFileList, LocalFileList, FileListCommand, LocalFileCommand, false);
    });
}

void Runner::RunPure(const QStringList & RemoteFileList, const QStringList & LocalFileList,
  bool FileListCommand, bool LocalFileCommand, bool RemoteFiles)
{
  QStringList Commands;
  QString RemoteDir = FContext.CurrentRemoteDir;
  QString LocalDir = FContext.CurrentLocalDir;
  try
  {
    if (FileListCommand)
    {
      QString LocalFile = LocalFileCommand ? LocalFileList.value(0) : QString();
      // MakeFileList does not delimit filenames
      Commands.append(LocalCustomCommand(FData, RemoteDir, LocalDir, QString(), LocalFile, MakeLocalFileList(RemoteFileList))
        .Complete(FCmd, true));
    }
    else if (LocalFileCommand)
    {
      if (LocalFileList.size() == 1)
      {
        for (const QString & FileName : RemoteFileList)
        {
          Commands.append(LocalCustomCommand(FData, RemoteDir, LocalDir, FileName, LocalFileList[0], QString()).Complete(FCmd, true));
        }
      }
      else if (RemoteFileList.size() == 1)
      {
        for (const QString & LocalFileName : LocalFileList)
        {
          Commands.append(LocalCustomCommand(FData, RemoteDir, LocalDir, RemoteFileList[0], LocalFileName, QString()).Complete(FCmd, true));
        }
      }
      else
      {
        if (LocalFileList.size() != RemoteFileList.size())
        {
          // CUSTOM_COMMAND_PAIRS_DOWNLOAD_FAILED
          throw ExtException(QStringLiteral("Some of the selected remote files were not downloaded. The selected custom command "
            "must be executed for pairs of files, which is currently not possible."));
        }
        for (int Index = 0; Index < LocalFileList.size(); Index++)
        {
          Commands.append(LocalCustomCommand(FData, RemoteDir, LocalDir, RemoteFileList[Index], LocalFileList[Index], QString())
            .Complete(FCmd, true));
        }
      }
    }
    else
    {
      for (const QString & FileName : RemoteFileList)
      {
        Commands.append(LocalCustomCommand(FData, RemoteDir, LocalDir, FileName, QString(), QString()).Complete(FCmd, true));
      }
    }
  }
  catch (const ExtException & E)
  {
    Error(E.Message(), E.MoreMessages());
    Finish();
    return;
  }

  bool NonBlocking = FileListCommand && RemoteFiles && !FCaptureOutput;
  RunLocal(Commands, !NonBlocking, [this, RemoteFiles]()
  {
    if (RemoteFiles)
    {
      LocalResults();
      Finish();
    }
    else
    {
      UploadModified();
    }
  });
}

void Runner::UploadModified()
{
  QStringList Changed;
  for (auto It = FFileTimes.cbegin(); It != FFileTimes.cend(); ++It)
  {
    QFileInfo Info(It.key());
    if (Info.exists() && !Info.isDir() && (Info.lastModified().toMSecsSinceEpoch() != It.value()))
    {
      Changed.append(It.key());
    }
  }
  UploadNext(Changed);
}

void Runner::UploadNext(QStringList Files)
{
  if (Files.isEmpty() || !SessionAvailable() || !FTempDir)
  {
    LocalResults();
    Finish();
    return;
  }
  QString FileName = Files.takeFirst();
  // temporary name -> remote path (TempNameMap)
  QString RelPath = QDir(FTempDir->path()).relativeFilePath(FileName);
  QString TopName = RelPath.section(QLatin1Char('/'), 0, 0);
  QString RemotePath = FTempToRemote.value(TopName);
  if (RemotePath.isEmpty())
  {
    UploadNext(Files);
    return;
  }
  RemotePath += RelPath.mid(TopName.size());
  QString RemoteDir = UnixPath::ExtractFileDir(RemotePath);
  CopyParam Param = Configuration::Instance().DefaultCopyParam().TemporaryFileCopyParam();
  Param.ConfirmOverwriting = false;
  Param.Queue = false;
  Param.NewerOnly = false;
  FTerm->CopyToRemote({FileName}, RemoteDir, Param, false, [this, Files](const OperationResult & Result)
  {
    if (!Result.Ok)
    {
      if (!Result.Aborted)
      {
        Error(Result.Message, Result.MoreMessages);
      }
      Finish();
      return;
    }
    UploadNext(Files);
  });
}

void Runner::RunLocal(const QStringList & Commands, bool Wait, std::function<void()> Then)
{
  if (!Wait && !FCaptureOutput)
  {
    // ExecuteShellChecked: started, not waited for
    for (const QString & Command : Commands)
    {
      QProcess Detached;
      Platform::SetShellCommand(Detached, Command);
      Detached.setWorkingDirectory(WorkingDirectory());
      if (!Detached.startDetached())
      {
        // EXECUTE_APP_ERROR
        Error(QStringLiteral("Cannot execute '%1'.").arg(Command));
        Fail();
        return;
      }
    }
    Then();
    return;
  }
  FQueue = Commands;
  FThen = std::move(Then);
  RunNext();
}

void Runner::RunNext()
{
  if (FQueue.isEmpty())
  {
    std::function<void()> Then = FThen;
    FThen = nullptr;
    if (Then)
    {
      Then();
    }
    return;
  }
  QString Command = FQueue.takeFirst();
  QProcess * Process = new QProcess(this);
  Process->setWorkingDirectory(WorkingDirectory());
  // ExecuteProcessAndReadOutput reads stderr too
  Process->setProcessChannelMode(FCaptureOutput ? QProcess::MergedChannels : QProcess::ForwardedOutputChannel);
  connect(Process, &QProcess::finished, this, [this, Process, Command](int ExitCode, QProcess::ExitStatus Status)
  {
    QString Output = QString::fromLocal8Bit(FCaptureOutput ? Process->readAllStandardOutput() : Process->readAllStandardError());
    Process->deleteLater();
    if (FCaptureOutput)
    {
      FOutput += Output;
      if ((Status != QProcess::NormalExit) || (ExitCode != 0))
      {
        // DoExecuteProcessAndReadOutput
        Error(QStringLiteral("Command failed with return code %1.").arg(ExitCode), Output);
        Fail();
        return;
      }
    }
    else if ((Status == QProcess::NormalExit) && ((ExitCode == 126) || (ExitCode == 127)))
    {
      // the shell could not run the program
      Error(QStringLiteral("Cannot execute '%1'.").arg(Command), Output.trimmed());
      Fail();
      return;
    }
    RunNext();
  });
  connect(Process, &QProcess::errorOccurred, this, [this, Process, Command](QProcess::ProcessError ProcessError)
  {
    if (ProcessError == QProcess::FailedToStart)
    {
      Process->deleteLater();
      Error(QStringLiteral("Cannot execute '%1'.").arg(Command), Process->errorString());
      Fail();
    }
  });
  Platform::SetShellCommand(*Process, Command);
  Process->start();
  Process->closeWriteChannel();
}

void Runner::LocalResults()
{
  if (!FCaptureOutput)
  {
    return;
  }
  // If the output is single-line, we do not want the trailing line break
  int P = FOutput.indexOf(QLatin1Char('\n'));
  if ((P >= 0) && (P == FOutput.size() - 1))
  {
    FOutput.chop(1);
  }
  // Copy even empty output to clipboard
  if ((FCommand.Params & ccCopyResults) != 0)
  {
    QApplication::clipboard()->setText(FOutput);
  }
  // But do not show an empty message box
  if (((FCommand.Params & ccShowResultsInMsgBox) != 0) && !FOutput.isEmpty())
  {
    QMessageBox Box(QMessageBox::Information, FCommand.PlainName(), FOutput, QMessageBox::Ok, FParent);
    QPushButton * CopyButton = Box.addButton(QStringLiteral("&Copy"), QMessageBox::ActionRole);
    Box.exec();
    if (Box.clickedButton() == CopyButton)
    {
      QApplication::clipboard()->setText(FOutput);
    }
  }
}

// TCustomWinConfiguration::GetValidHistoryKey
QString ValidHistoryKey(QString Key)
{
  for (QChar & C : Key)
  {
    if (!C.isLetterOrNumber())
    {
      C = QLatin1Char('_');
    }
  }
  while (Key.startsWith(QLatin1Char('_')))
  {
    Key.remove(0, 1);
  }
  while (Key.endsWith(QLatin1Char('_')))
  {
    Key.chop(1);
  }
  return Key;
}

}

//---------------------------------------------------------------------------

void CustomCommandRunner::Execute(QWidget * Parent, const CustomCommandType & Command, const CustomCommandContext & Context,
  std::function<void()> Done)
{
  Runner * R = new Runner(Parent, Command, Context, std::move(Done));
  R->Start();
}

bool CustomCommandRunner::ExecuteAdHoc(QWidget * Parent, CustomCommandType & LastCommand, const CustomCommandContext & Context,
  std::function<void()> Done)
{
  // TCustomScpExplorerForm::AdHocCustomCommand
  CustomCommandAvailability A = Availability(Context);
  CustomCommandType Command;
  // make sure we use local custom command when remote are not supported
  if (A.RemoteAllowed || LastCommand.IsLocal())
  {
    Command = LastCommand;
  }
  else
  {
    Command.Params |= ccLocal;
  }
  // CUSTOM_COMMAND_AD_HOC_NAME
  Command.Name = QStringLiteral("Ad Hoc");
  bool LocalSide = Context.LocalSide || !Context.RemotePanel;
  int Options =
    ((!A.RemoteAllowed || LocalSide) ? CustomCommandDialog::ccoDisableRemote : 0) |
    (LocalSide ? CustomCommandDialog::ccoDisableRemoteFiles : 0);
  auto Validate = [A](const CustomCommandType & ACommand) -> QString
  {
    if (CustomCommandState(ACommand, A, ccltAll) <= 0)
    {
      return ImpossibleMessage(ACommand.Command);
    }
    return QString();
  };
  if (!CustomCommandDialog::Execute(Parent, Command, CustomCommandList::Configured(), CustomCommandDialog::ccmAdHoc,
        Options, QList<int>(), Validate))
  {
    return false;
  }
  LastCommand = Command;
  Execute(Parent, Command, Context, std::move(Done));
  return true;
}

CustomCommandAvailability CustomCommandRunner::Availability(const CustomCommandContext & Context)
{
  CustomCommandAvailability A;
  A.SessionAvailable = (Context.Term != nullptr) && Context.Term->Active();
  A.RemoteAllowed = A.SessionAvailable && Context.Term->CanAnyCommand() &&
    (Context.Term->IsCapable(fcShellAnyCommand) || Context.Term->IsCapable(fcSecondaryShell));
  A.LocalSideActive = Context.LocalSide;
  A.RemotePanel = Context.RemotePanel;
  A.AnySelectedLocal = !Context.SelectedLocalFiles.isEmpty();
  A.AnySelectedRemote = !Context.SelectedRemoteFiles.isEmpty();
  A.AnySelectedCurrent = Context.LocalSide ? A.AnySelectedLocal : A.AnySelectedRemote;
  return A;
}

bool CustomCommandRunner::PromptValues(QWidget * Parent, const QString & CommandName, const QStringList & Prompts,
  const QStringList & Defaults, QStringList & Values)
{
  // TCustomCommandPromptsDialog
  QDialog Dialog(Parent);
  // CUSTOM_COMMANDS_PARAMS_TITLE
  Dialog.setWindowTitle(QStringLiteral("'%1' command parameters").arg(CommandName));
  Dialog.setMinimumWidth(380);
  QVBoxLayout * Layout = new QVBoxLayout(&Dialog);
  QList<QComboBox *> Edits;
  QStringList HistoryKeys;
  for (int Index = 0; Index < Prompts.size(); Index++)
  {
    QString Prompt = Prompts[Index];
    if (Prompt.isEmpty())
    {
      // CUSTOM_COMMANDS_PARAM_PROMPT2
      Prompt = QStringLiteral("&Parameter value:");
    }
    QComboBox * ComboBox = new QComboBox(&Dialog);
    ComboBox->setEditable(true);
    ComboBox->setInsertPolicy(QComboBox::NoInsert);
    QString HistoryKey = Prompts[Index].isEmpty() ? QString::number(Index) : Prompts[Index];
    HistoryKey = QStringLiteral("CustomCommandParam_") + ValidHistoryKey(QStringLiteral("%1_%2").arg(CommandName, HistoryKey));
    EditMaskDialog::LoadHistoryCombo(ComboBox, HistoryKey);
    ComboBox->setEditText(Defaults.value(Index));
    QLabel * Label = new QLabel(Prompt, &Dialog);
    Label->setBuddy(ComboBox);
    Layout->addWidget(Label);
    Layout->addWidget(ComboBox);
    Edits.append(ComboBox);
    HistoryKeys.append(HistoryKey);
  }
  Layout->addStretch();
  QDialogButtonBox * Buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Help, &Dialog);
  Buttons->button(QDialogButtonBox::Help)->setText(QStringLiteral("&Help"));
  Layout->addWidget(Buttons);
  QObject::connect(Buttons, &QDialogButtonBox::accepted, &Dialog, &QDialog::accept);
  QObject::connect(Buttons, &QDialogButtonBox::rejected, &Dialog, &QDialog::reject);
  // HELP_CUSTOM_COMMAND_PARAM
  HelpSystem::InstallFormHelp(&Dialog, QStringLiteral("custom_command"), Buttons->button(QDialogButtonBox::Help));
  if (!Edits.isEmpty())
  {
    Edits.first()->setFocus();
    Edits.first()->lineEdit()->selectAll();
  }

  bool Result = (Dialog.exec() == QDialog::Accepted);
  if (Result)
  {
    Values.clear();
    for (int Index = 0; Index < Edits.size(); Index++)
    {
      Values.append(Edits[Index]->currentText());
      EditMaskDialog::SaveToHistory(HistoryKeys[Index], Edits[Index]->currentText());
    }
  }
  return Result;
}

void CustomCommandRunner::ShowResults(QWidget * Parent, const QString & Title, const QStringList & Lines)
{
  QDialog Dialog(Parent);
  Dialog.setWindowTitle(Title.isEmpty() ? QStringLiteral("Console") : QStringLiteral("%1 - Console").arg(Title));
  Dialog.resize(600, 400);
  // the output is shown in TConsoleDialog in WinSCP: F1 opens its help
  HelpSystem::InstallFormHelp(&Dialog, QStringLiteral("ui_console"));
  QVBoxLayout * Layout = new QVBoxLayout(&Dialog);
  QPlainTextEdit * OutputMemo = new QPlainTextEdit(&Dialog);
  OutputMemo->setReadOnly(true);
  OutputMemo->setLineWrapMode(QPlainTextEdit::NoWrap);
  OutputMemo->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  OutputMemo->setPlainText(Lines.join(QLatin1Char('\n')));
  Layout->addWidget(OutputMemo, 1);
  QDialogButtonBox * Buttons = new QDialogButtonBox(QDialogButtonBox::Close, &Dialog);
  QPushButton * CopyButton = Buttons->addButton(QStringLiteral("&Copy"), QDialogButtonBox::ActionRole);
  Layout->addWidget(Buttons);
  QObject::connect(Buttons, &QDialogButtonBox::rejected, &Dialog, &QDialog::reject);
  QObject::connect(CopyButton, &QPushButton::clicked, &Dialog, [OutputMemo]()
  {
    QApplication::clipboard()->setText(OutputMemo->toPlainText() + QLatin1Char('\n'));
  });
  Dialog.exec();
}

void ExecuteCustomCommand(QWidget * Parent, Terminal * Term, const CustomCommandType & Command,
  const QStringList & SelectedRemotePaths, const QString & CurrentRemoteDir, std::function<void()> Done)
{
  CustomCommandContext Context;
  Context.Term = Term;
  Context.CurrentRemoteDir = CurrentRemoteDir;
  Context.SelectedRemoteFiles = SelectedRemotePaths;
  Context.CurrentLocalDir = QDir::homePath();
  CustomCommandRunner::Execute(Parent, Command, Context, std::move(Done));
}
