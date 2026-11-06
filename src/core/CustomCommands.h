// Port of WinSCP custom commands:
//  - source/windows/WinConfiguration.cpp: TCustomCommandType, TCustomCommandList
//    (WinSCP.ini persistence, default command list)
//  - source/core/FileMasks.cpp: TCustomCommand, TInteractiveCustomCommand,
//    TCustomCommandData, TFileCustomCommand (= TRemoteCustomCommand)
//  - source/windows/GUITools.cpp: TLocalCustomCommand
//  - source/forms/CustomScpExplorer.cpp: CustomCommandState
//
// Storage is identical to WinSCP.ini (TCustomCommandList::Save):
//   [Configuration\CustomCommands]          <description>=<command>
//   [Configuration\CustomCommandsParams]    <description>=<params flags>
//   [Configuration\CustomCommandsShortCuts] <description>=<VCL TShortCut>
//   [Configuration] CustomCommandsNone=1    (the list was emptied)
// When none of these exist, the default command list is used (and not saved).
//
// Patterns (https://winscp.net/eng/docs/custom_command#patterns):
//   !!   exclamation mark                !&  list of selected files (quoted, space-delimited)
//   !    file name                       !/  current remote path
//   !S   session URL                     !@  host name
//   !U   user name                       !P  password
//   !#   port number                     !N  session name
//   !K   private key file                !E  session URL (raw settings not included)
//   !?prompt[\]?default!   value entered by the user (\ = do not escape)
//   !`command`             output of a local command
//   local commands only:  !^!  file name from local panel,  !\  current local path
// File names are escaped for the remote shell (DelimitStr) and prefixed with
// "./" when starting with "-"; local commands are never escaped.
#pragma once

#include "core/FileSystem.h"

#include <QList>
#include <QString>
#include <QStringList>
#include <functional>
#include <map>
#include <vector>

class IniFile;
class SessionData;

// TCustomCommandType::Params flags (core/Terminal.h, windows/GUIConfiguration.cpp)
constexpr int ccApplyToDirectories = 0x01;
constexpr int ccRecursive = 0x02;
constexpr int ccUser = 0x100;
constexpr int ccLocal = ccUser;
constexpr int ccShowResults = ccUser << 1;
constexpr int ccCopyResults = ccUser << 2;
constexpr int ccRemoteFiles = ccUser << 3;
constexpr int ccShowResultsInMsgBox = ccUser << 4;

// TCustomCommandType
class CustomCommandType
{
public:
  CustomCommandType() = default;
  CustomCommandType(const QString & AName, const QString & ACommand, int AParams) :
    Name(AName), Command(ACommand), Params(AParams) {}

  QString Name;           // description, with '&' hotkey ("&Touch")
  QString Command;        // command with patterns
  int Params = 0;         // cc* flags
  int ShortCut = 0;       // VCL TShortCut (0 = none), see LocationProfilesDialog::ShortCutToKeySequence

  bool IsLocal() const { return (Params & ccLocal) != 0; }
  // Description without '&' hotkey markers ("&&" becomes "&"), as StripHotkey
  QString DisplayName() const;
  // StripEllipsis(StripHotkey(Name)): caption for prompts ("Grep" for "&Grep...")
  QString PlainName() const;
  bool operator==(const CustomCommandType & Other) const = default;
};

// TCustomCommandList
class CustomCommandList
{
public:
  int Count() const { return FCommands.size(); }
  const CustomCommandType & At(int Index) const { return FCommands.at(Index); }
  const QList<CustomCommandType> & Items() const { return FCommands; }

  void Clear();
  void Add(const QString & Name, const QString & Command, int Params);
  void Add(const CustomCommandType & Command);
  void Insert(int Index, const CustomCommandType & Command);
  void Change(int Index, const CustomCommandType & Command);
  // TList::Move semantics
  void Move(int CurIndex, int NewIndex);
  void Delete(int Index);
  // Exact (case sensitive) match of the description, as TCustomCommandList::Find
  const CustomCommandType * Find(const QString & Name) const;
  const CustomCommandType * FindByShortCut(int ShortCut) const;
  QList<int> ShortCuts() const;
  bool operator==(const CustomCommandList & Other) const { return FCommands == Other.FCommands; }

  bool Modified() const { return FModified; }
  void Reset() { FModified = false; }

  // TWinConfiguration::DefaultLocalized: the default commands (Linux
  // equivalent for "&Print": lp instead of notepad.exe /p)
  static CustomCommandList Defaults();

  // Configuration\CustomCommands* of the file (TWinConfiguration::LoadData);
  // the defaults when the file has no custom commands. IsDefault: whether
  // the defaults were used.
  void Load(IniFile & File, bool * IsDefault = nullptr);
  // TCustomCommandList::Save (+ CustomCommandsNone)
  void Save(IniFile & File) const;

  // Convenience: the list from Configuration::Instance(); SetConfigured writes
  // it there when it differs from the current one (as
  // TWinConfiguration::SetCustomCommandList). Call Configuration::Save() after.
  static CustomCommandList Configured();
  static void SetConfigured(const CustomCommandList & List);

private:
  QList<CustomCommandType> FCommands;
  bool FModified = false;
};

// TCustomCommandData: session values for the session patterns
struct CustomCommandData
{
  bool HasSession = false;  // false: session patterns expand to empty strings
  QString HostName;         // !@
  QString UserName;         // !U
  QString Password;         // !P
  int PortNumber = 0;       // !#
  QString SessionUrl;       // !S
  QString CompleteSessionUrl; // !E: with the settings that differ from the defaults
  QString SessionName;      // !N
  QString PublicKeyFile;    // !K

  // Password: the actual session password when it is not stored in Data
  // (e.g. typed at login); empty = Data.Password
  static CustomCommandData FromSession(const SessionData & Data, const QString & Password = QString());
  // TSessionData::GenerateSessionUrl(sufSession) without the host key part;
  // Complete: sufComplete, that is with the raw settings too
  static QString GenerateSessionUrl(const SessionData & Data, const QString & Password, bool Complete = false);
};

// TCustomCommand: two-pass pattern expansion. Complete(Command, false) keeps
// unknown patterns and escapes replacements ("!" -> "!!") for a further pass;
// Complete(Command, true) produces the final command. Errors throw ExtException.
class CustomCommand
{
  friend class InteractiveCustomCommand;

public:
  virtual ~CustomCommand() = default;

  QString Complete(const QString & Command, bool LastPass);
  virtual void Validate(const QString & Command);
  bool HasAnyPatterns(const QString & Command);

  static QString Escape(const QString & S);
  // DelimitStr (core/Common.cpp): escapes $ \ (and ` " in double quotes) with
  // a backslash, prefixes "./" when Str starts with "-"; Quote is the quote
  // character around the pattern ('"', '\'' or QChar() = none)
  static QString DelimitStr(const QString & Str, QChar Quote);

protected:
  // Index is 1-based (as in WinSCP and in its error messages)
  void GetToken(const QString & Command, int Index, int & Len, QChar & PatternCmd);
  void CustomValidate(const QString & Command, int * Found);
  bool FindPattern(const QString & Command, QChar PatternCmd);

  virtual void ValidatePattern(const QString & Command, int Index, int Len, QChar PatternCmd, int * Found);
  virtual int PatternLen(const QString & Command, int Index) = 0;
  virtual void PatternHint(int Index, const QString & Pattern);
  virtual bool PatternReplacement(int Index, const QString & Pattern, QString & Replacement, bool & Delimit) = 0;
  virtual void DelimitReplacement(QString & Replacement, QChar Quote);

  static QChar CharAt(const QString & Command, int Index);   // 1-based, QChar() past the end
};

// TInteractiveCustomCommand + TWinInteractiveCustomCommand: expands
// !?prompt?default! and !`command`, leaves the other patterns to the child.
class InteractiveCustomCommand : public CustomCommand
{
public:
  // All prompts of the command at once (one dialog, as
  // TCustomCommandPromptsDialog); return false to abort (Complete then
  // throws AbortException). Without callback, the defaults are used.
  using PromptsCallback = std::function<bool(const QStringList & Prompts, const QStringList & Defaults, QStringList & Values)>;
  // Runs a local command and returns its output; default: /bin/sh -c, output
  // with trailing white space removed (TWinInteractiveCustomCommand::Execute)
  using ExecuteCallback = std::function<QString(const QString & Command)>;

  explicit InteractiveCustomCommand(CustomCommand * ChildCustomCommand,
    PromptsCallback OnPrompts = PromptsCallback(), ExecuteCallback OnExecute = ExecuteCallback());

  static QString ExecuteLocalCommand(const QString & Command);

protected:
  int PatternLen(const QString & Command, int Index) override;
  void PatternHint(int Index, const QString & Pattern) override;
  bool PatternReplacement(int Index, const QString & Pattern, QString & Replacement, bool & Delimit) override;
  void ParsePromptPattern(const QString & Pattern, QString & Prompt, QString & Default, bool & Delimit);
  bool IsPromptPattern(const QString & Pattern);

private:
  CustomCommand * FChildCustomCommand;
  PromptsCallback FOnPrompts;
  ExecuteCallback FOnExecute;
  std::map<int, int> FIndexes;
  QStringList FPrompts;
  QStringList FDefaults;
  QStringList FValues;
  bool FPrompted = false;
};

// TFileCustomCommand (TRemoteCustomCommand)
class FileCustomCommand : public CustomCommand
{
public:
  FileCustomCommand() = default;
  FileCustomCommand(const CustomCommandData & Data, const QString & Path);
  // FileList: already quoted list (MakeShellFileList / MakeLocalFileList)
  FileCustomCommand(const CustomCommandData & Data, const QString & Path, const QString & FileName, const QString & FileList);

  void Validate(const QString & Command) override;

  bool IsFileListCommand(const QString & Command);
  virtual bool IsFileCommand(const QString & Command);
  bool IsRemoteFileCommand(const QString & Command);
  bool IsSiteCommand(const QString & Command);
  bool IsSessionCommand(const QString & Command);
  bool IsPasswordCommand(const QString & Command);

protected:
  void ValidatePattern(const QString & Command, int Index, int Len, QChar PatternCmd, int * Found) override;
  int PatternLen(const QString & Command, int Index) override;
  bool PatternReplacement(int Index, const QString & Pattern, QString & Replacement, bool & Delimit) override;
  void DelimitReplacement(QString & Replacement, QChar Quote) override;

private:
  CustomCommandData FData;
  QString FPath;
  QString FFileName;
  QString FFileList;
};

using RemoteCustomCommand = FileCustomCommand;

// TLocalCustomCommand
class LocalCustomCommand : public FileCustomCommand
{
public:
  LocalCustomCommand() = default;
  LocalCustomCommand(const CustomCommandData & Data, const QString & RemotePath, const QString & LocalPath);
  LocalCustomCommand(const CustomCommandData & Data, const QString & RemotePath, const QString & LocalPath,
    const QString & FileName, const QString & LocalFileName, const QString & FileList);

  bool IsFileCommand(const QString & Command) override;
  bool HasLocalFileName(const QString & Command);
  // Shell-quote replacements (default): the command runs through /bin/sh.
  // Off when the caller expands already split arguments (no shell).
  void SetShellQuoting(bool V) { FShellQuoting = V; }

protected:
  int PatternLen(const QString & Command, int Index) override;
  bool PatternReplacement(int Index, const QString & Pattern, QString & Replacement, bool & Delimit) override;
  void DelimitReplacement(QString & Replacement, QChar Quote) override;

private:
  QString FLocalPath;
  QString FLocalFileName;
  bool FShellQuoting = true;
};

// AddToShellFileListCommandLine: "a b" "c" (ShellQuoteStr, space separated) - for remote commands
QString MakeShellFileList(const QStringList & FileNames);
// MakeFileList (core/RemoteFiles.cpp): names in '' (shell quoted), space separated - for local commands
QString MakeLocalFileList(const QStringList & FileNames);

// TCustomCommandDialog::FormCloseQuery validation of the command (patterns and
// file name/file list combination); empty = valid, else the error message
QString ValidateCustomCommand(const QString & Command, bool Local);

// Analysis of a command (after the interactive pass) for the dialog and menus
struct CustomCommandInfo
{
  bool Valid = true;
  bool FileCommand = false;        // operates on selected files
  bool FileListCommand = false;    // !&
  bool RemoteFileCommand = false;  // ! or !& (TFileCustomCommand::IsRemoteFileCommand)
  bool LocalFileName = false;      // !^!
  bool SessionCommand = false;     // uses session patterns
  bool PasswordCommand = false;    // !P
};
CustomCommandInfo AnalyzeCustomCommand(const QString & Command, bool Local);

// TCustomScpExplorerForm::CustomCommandState: -1 hidden, 0 disabled, 1 enabled
enum CustomCommandListType { ccltAll, ccltFile, ccltNonFile, ccltBoth };
struct CustomCommandAvailability
{
  bool RemoteAllowed = false;       // connected session that can execute commands (Terminal::CanAnyCommand)
  bool SessionAvailable = false;    // connected session (for local commands with session patterns)
  bool LocalSideActive = false;     // the current panel is the local one
  bool RemotePanel = true;          // the "other" side is a remote panel (not local-local tab)
  bool AnySelectedCurrent = false;  // files selected (or focused) in the current panel
  bool AnySelectedLocal = false;    // files selected in the local panel
  bool AnySelectedRemote = false;   // files selected in the remote panel
};
int CustomCommandState(const CustomCommandType & Command, const CustomCommandAvailability & Availability,
  CustomCommandListType ListType = ccltAll);
