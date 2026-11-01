// Port of WinSCP source/core/Script.{h,cpp} (TScript, TManagementScript):
// the scripting language of winscp.com, running NixSCP Terminals
// synchronously from the main thread.
#pragma once

#include "cli/Options.h"
#include "cli/SessionUrl.h"
#include "core/CopyParam.h"
#include "core/SessionData.h"
#include "core/Terminal.h"

#include <QFile>
#include <QMutex>
#include <QString>
#include <QStringList>
#include <atomic>
#include <functional>
#include <memory>
#include <vector>

class ConsoleUserInterface;
class SessionLog;

// TScriptProcParams: parameters of a script command; only '-' marks switches
class ScriptProcParams : public Options
{
public:
  ScriptProcParams(const QString & AFullCommand, const QString & AParamsStr);
  QString FullCommand;
  QString ParamsStr;
};

class Script
{
public:
  enum BatchMode { BatchOff, BatchOn, BatchAbort, BatchContinue };

  explicit Script(StoredSessionList * StoredSessions);
  ~Script();

  // /stdout[=binary|chunked] (TConsoleCommStruct::TInitEvent::STDINOUT)
  enum StdInOutMode { StdInOutOff, StdInOutBinary, StdInOutChunked };

  // Setup (TConsoleRunner::Run)
  void SetCopyParam(const CopyParam & Value);
  void SetWantsProgress(bool Value) { FWantsProgress = Value; }
  void SetUsageWarnings(bool Value) { FUsageWarnings = Value; }
  // TScript::Init: Configuration->SessionReopenTimeout (ms, 0 = default)
  void SetConfigurationReopenTimeout(int Value);
  // /log=<file> with WinSCP's ! patterns, /logsize=[<count>*]<size>
  bool OpenLog(const QString & FileName, QString & Error, qint64 MaxSize = 0, int MaxCount = 0);
  // /loglevel=<-1..2>[*]: 0 normal (default), 1 debug (+ command options,
  // exit code), 2 debug 2 (+ all script output); -1 reduced (no script output); * logs passwords
  void SetLogLevel(int Level, bool LogPasswords);
  int LogLevel() const { return FLogLevel; }
  // /xmllog=<file> [/xmlgroups] (TActionLog); false when the file cannot be created
  bool OpenXmlLog(const QString & FileName, bool Groups, QString & Error);
  // Closes the XML log (</session>); called at exit
  void CloseXmlLog();
  // /stdout and /stdin: "get ... -" and "put - ..." stream file contents
  void SetStdOut(StdInOutMode Value) { FStdOut = Value; }
  void SetStdIn(bool Value) { FStdIn = Value; }
  // Expands WinSCP's log file name patterns (!Y !M !D !T !P !@ !S !! !-)
  static QString ExpandLogFileName(const QString & FileName, const SessionData * Data);
  // MinimizeName (core/RemoteFiles.cpp): shortens a path to MaxLen with ".../"
  static QString MinimizeName(const QString & FileName, int MaxLen, bool Unix);

  // Execution
  void Command(const QString & Cmd);
  void Connect(const QString & Session, Options & Opts, bool CheckParams);
  void StartInteractive();
  bool Continue() const { return FContinue; }
  bool HasSession() const { return (FSession != nullptr); }

  // TConsoleRunner state shared with the console UI (any thread)
  BatchMode Batch() const { return FBatch; }
  unsigned int InputTimeout() const;
  void SetBatchScript(bool Value) { FBatchScript = Value; }
  bool BatchScript() const { return FBatchScript; }
  void NotifyAbort();
  bool Aborted() const { return FAborted; }
  void SetCommandError() { FCommandError = true; }
  void ResetCommandError() { FCommandError = false; }
  bool CommandError() const { return FCommandError; }

  // Output (any thread); everything printed is logged
  void Print(const QString & Str);
  void PrintLine(const QString & Str, bool Error = false);
  void PrintMessage(const QString & Str, bool Error = false);
  // Log types as in WinSCP's session log: '>' input, '<' output, '.' message, '!' exception
  void Log(QChar Type, const QString & Str);
  void ShowException(const ExtException & E);

  // Masks passwords in "open" commands for logs/echo (MaskPasswordInCommand)
  QString MaskPasswordInCommand(const QString & FullCommand, const QString & Command);
  static QString MaskPasswordInCommandLine(const QString & CommandLine);

private:
  struct Session
  {
    SessionData Data;
    SessionExtras Extras;
    std::unique_ptr<ConsoleUserInterface> UI;
    std::unique_ptr<Terminal> Term;   // destroyed before UI
    QString CurrentDirectory;
    QString HomeDirectory;
  };

  using CommandProc = void (Script::*)(ScriptProcParams & Params);
  struct CommandDef
  {
    QString Name;
    QString Description;
    const char * Help;
    CommandProc Proc;
    int MinParams;
    int MaxParams;
    bool Switches;
  };

  enum FileListType { fltDefault = 0, fltDirectories = 0x01, fltQueryServer = 0x02, fltMask = 0x04, fltLatest = 0x08, fltOnlyFile = 0x10 };
  struct RemoteItem
  {
    QString Name;          // as specified/matched (possibly relative)
    QString Path;          // absolute
    RemoteFile File;
    bool HaveFile = false;
  };
  struct LocalItem
  {
    QString Path;          // absolute
    bool Exists = false;
    bool Directory = false;
    qint64 Size = 0;
    QDateTime Modification;
  };

  StoredSessionList * FStoredSessions;
  std::vector<CommandDef> FCommands;
  std::vector<std::unique_ptr<Session>> FSessions;
  Session * FSession = nullptr;
  CopyParam FCopyParam;
  bool FWantsProgress = false;
  bool FUsageWarnings = true;
  bool FContinue = true;
  bool FEcho = false;
  bool FFailOnNoMatch = false;
  bool FIncludeFileMaskOptionUsed = false;
  bool FWarnNonDefaultCopyParam = false;
  std::atomic<BatchMode> FBatch{BatchAbort};
  BatchMode FInteractiveBatch = BatchOff;
  bool FConfirm = false;
  bool FInteractiveConfirm = true;
  int FSessionReopenTimeout = 0;            // ms, option reconnecttime: Terminal::Settings::ReopenTimeout of the sessions
  int FInteractiveSessionReopenTimeout = 0;
  std::atomic<bool> FBatchScript{false};
  std::atomic<bool> FAborted{false};
  std::atomic<bool> FCommandError{false};
  bool FInterrupted = false;
  int FSynchronizeParams = 0;
  StdInOutMode FStdOut = StdInOutOff;
  bool FStdIn = false;

  // The session log (core/SessionLog), shared with the sessions (SessionLog::SetProcessLog)
  std::shared_ptr<SessionLog> FLog;
  int FLogLevel = 0;
  bool FLogPasswords = false;

  // XML log (TActionLog)
  QFile FXmlFile;
  bool FXmlGroups = false;
  bool FXmlStarted = false;
  bool FXmlInGroup = false;
  QString FXmlIndent;
  struct XmlParam
  {
    QString Name;
    QString Value;
  };
  void XmlAdd(const QString & Line);
  void XmlStart();
  // TSessionActionRecord::Record
  void XmlRecord(const QString & Action, const std::vector<XmlParam> & Params, bool Success,
    const QStringList & Errors = QStringList(), bool Recursive = false,
    const RemoteFileList * Files = nullptr, const RemoteFile * File = nullptr);
  void XmlRecordFile(const QString & Indent, const RemoteFile & File, bool IncludeFileName);
  // TActionLog::AddFailure
  void XmlFailure(const ExtException & E);
  void XmlBeginGroup(const QString & Name);
  void XmlEndGroup();
  bool XmlLogging() const { return FXmlFile.isOpen(); }
  static QStringList ExceptionMessages(const ExtException & E);

  // Transfer progress (TManagementScript::TerminalOperationProgress)
  QString FLastProgressFile;
  QString FLastProgressMessage;

  void Register(const QString & Name, const QString & Description, const char * Help, CommandProc Proc,
    int MinParams, int MaxParams, bool Switches);
  int FindCommand(const QString & Command, QString * Matches) const;
  static int FindToggle(const QStringList & Names, const QString & Value);
  void Execute(const QString & Command, const QString & Params);
  void HandleException(const ExtException & E, bool Fatal);

  // Synchronous Terminal operations (main thread waits in an event loop)
  OperationResult Wait(Session & S, const std::function<void(Terminal::Callback)> & Start, bool Transfer = false,
    const std::function<void()> & OnInterrupt = nullptr);
  void Check(const OperationResult & Result);
  void RunWorker(Session & S, Terminal::WorkerJob Job, bool Transfer = false);
  void PrintTransferProgress(Session & S, bool Final);
  void ResetTransfer();

  void CheckSession();
  void CheckParams(ScriptProcParams & Params);
  void RequireParams(ScriptProcParams & Params, int MinParams);
  [[noreturn]] void NotSupported();
  void NoMatch(const QString & Message);
  void NoMatch(const QString & Mask, const QString & Error);
  void CheckDefaultCopyParam();
  void CheckMultiFilesToOne(int Count, const QString & Target, bool Unix);
  QString AbsolutePath(const QString & Path) const;
  std::vector<RemoteItem> CreateFileList(ScriptProcParams & Params, int Start, int End, int ListType);
  std::vector<LocalItem> CreateLocalFileList(ScriptProcParams & Params, int Start, int End, int ListType);
  void CopyParamParams(CopyParam & Param, ScriptProcParams & Params);
  void TransferParamParams(ScriptProcParams & Params, bool & Move, bool & Resume);
  CopyParam::TransferMode ParseTransferModeName(const QString & Name);
  bool RemoteIsDirectory(const QString & Path);
  void Download(const std::vector<RemoteItem> & Files, const QString & TargetDir, const QString & FileMask,
    CopyParam Param, bool Move, bool Resume);
  void Upload(const std::vector<LocalItem> & Files, const QString & TargetDir, const QString & FileMask,
    CopyParam Param, bool Move, bool Resume);
  void PrintActiveSession();
  // TConsoleRunner::UpdateTitle
  void UpdateTitle();
  // Script options that the session's Terminal applies (option reconnecttime)
  void ApplySessionSettings(Session & S);
  // "get dir newname" / "put dir newname": the contents of the directory
  // go to a target directory of a different name
  void DownloadDirectoryAs(const RemoteItem & Item, const QString & Target, const CopyParam & Param, bool Move);
  void UploadDirectoryAs(const LocalItem & Item, const QString & Target, const CopyParam & Param, bool Move);
  // "get ... -" with /stdout, "put - target" with /stdin
  void StreamDownload(const RemoteItem & Item, const CopyParam & Param);
  void StreamUpload(const QString & Target, const CopyParam & Param);
  void DoClose(Session * S);
  void DoChangeLocalDirectory(const QString & Directory);
  int SessionIndex(const Session * S) const;
  Session * FindSession(const QString & Index);
  void Input(const QString & Prompt, QString & Str, bool AllowEmpty);
  static QString ListingStr(const RemoteFile & File);

  // Commands
  void HelpProc(ScriptProcParams & Params);
  void CallProc(ScriptProcParams & Params);
  void EchoProc(ScriptProcParams & Params);
  void StatProc(ScriptProcParams & Params);
  void ChecksumProc(ScriptProcParams & Params);
  void PwdProc(ScriptProcParams & Params);
  void CdProc(ScriptProcParams & Params);
  void LsProc(ScriptProcParams & Params);
  void RmProc(ScriptProcParams & Params);
  void RmDirProc(ScriptProcParams & Params);
  void MvProc(ScriptProcParams & Params);
  void CpProc(ScriptProcParams & Params);
  void ChModProc(ScriptProcParams & Params);
  void LnProc(ScriptProcParams & Params);
  void MkDirProc(ScriptProcParams & Params);
  void GetProc(ScriptProcParams & Params);
  void PutProc(ScriptProcParams & Params);
  void OptionProc(ScriptProcParams & Params);
  void AsciiProc(ScriptProcParams & Params);
  void BinaryProc(ScriptProcParams & Params);
  void SynchronizeProc(ScriptProcParams & Params);
  void KeepUpToDateProc(ScriptProcParams & Params);
  void ExitProc(ScriptProcParams & Params);
  void OpenProc(ScriptProcParams & Params);
  void CloseProc(ScriptProcParams & Params);
  void SessionProc(ScriptProcParams & Params);
  void LPwdProc(ScriptProcParams & Params);
  void LCdProc(ScriptProcParams & Params);
  void LLsProc(ScriptProcParams & Params);
  void OptionImpl(QString OptionName, QString ValueName);
  void SynchronizeDirectories(ScriptProcParams & Params, QString & LocalDirectory, QString & RemoteDirectory, int FirstParam);
};
