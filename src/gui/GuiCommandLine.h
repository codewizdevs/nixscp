// Port of the GUI command line of WinSCP: source/windows/WinMain.cpp
// (Execute, GetLoginData, GetFolderOrWorkspaceName, Upload, Download,
// FullSynchronize, Synchronize, Refresh), windows/UserInterface.cpp
// (CreateConfiguration /ini /rawconfig, CheckSafe), core/SessionData.cpp
// (TStoredSessionList::ParseUrl with pufAllowStoredSiteWithProtocol and
// pufUnsafe, the ";save" URL parameter and the "URL points to a file" part),
// forms/Custom.cpp (TSaveSessionDialog, DoSaveSession),
// forms/CustomScpExplorer.cpp (CommandLineFromAnotherInstance, RefreshPanel,
// ExploreFile, LastTerminalClosed/NeedSession) and windows/Setup.cpp
// (RegisterForDefaultProtocols, UnregisterForProtocols).
//
// The URL parser itself is the one of nixscp-cli (src/cli/SessionUrl.cpp),
// compiled into the GUI by GuiCommandLine.cpp.
//
// Linux adaptations:
//  - WM_COPYDATA to another instance -> gui/SingleInstance (QLocalServer)
//  - "Open in PuTTY" for ssh:// URLs -> the terminal client command of
//    Preferences > Integration > Applications (PuttyPath, OpenSSH in a terminal)
//  - URL handler registration in the registry -> freedesktop x-scheme-handler
//    desktop entry + xdg-mime default, started with /unsafe as WinSCP's handler
//  - /console, /script, /command, /help, /info, /keygen, /copyid,
//    /fingerprintscan, /batchsettings -> run nixscp-cli (winscp.com)
// Removed (no Linux equivalent): embedded sessions (sites stored as resources
// of the executable), /Usage, /Update, /UninstallCleanup, /AddSearchPath,
// /RemoveSearchPath, /ImportSitesIfAny maintenance switches (installer only),
// /JumpList, /Desktop, /SendToHook usage counters, /Console=<ver> /DotNet=<ver>.
#pragma once

#include "core/SessionData.h"

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <functional>
#include <memory>

class Options;
class CommanderWindow;
class GuiUserInterface;
class Terminal;
class QWidget;
class QTimer;

namespace GuiCommandLine
{
  // TLoginNeed
  enum LoginNeed { lnNone, lnSession, lnTerminal };
  // ParamCommand of Execute
  enum ParamCommand { pcNone, pcUpload, pcFullSynchronize, pcSynchronize, pcEdit, pcRefresh };

  // CheckSafe: dangerous switches (/ini, /rawconfig, /log, /xmllog,
  // /defaults, maintenance tasks) and unsafe raw session settings are ignored
  // when the URL handler started us (/Unsafe)
  bool CheckSafe(Options & Params);

  // Result of TStoredSessionList::ParseUrl for the GUI command line
  struct ParsedUrl
  {
    SessionData Data;
    int ParsedInfo = 0;          // piProtocolDefined, piDefaultsOnly (cli/SessionUrl.h)
    QString DownloadFile;        // URL path does not end with '/': name of the file to download
    bool SaveOnly = false;       // ";save" session parameter
    bool PuttyProtocol = false;  // ssh:// (open in terminal, not as SFTP session)
    QString Passphrase;          // -passphrase / Passphrase raw setting
  };
  // Unsafe (pufUnsafe): raw settings in the URL or /rawsettings must not set
  // the shell, listing command, return variable, post login commands, recycle
  // bin, SFTP server or proxy commands. Throws ExtException on invalid switch values.
  ParsedUrl ParseUrl(const QString & Url, Options * Params, StoredSessionList & Stored, bool Unsafe);

  // GetFolderOrWorkspaceName: the argument names a site folder or a workspace
  // (decoded); empty when it is a site or URL
  QString FolderOrWorkspaceName(const QString & SessionName, Options * Params, bool PreferSite,
    const StoredSessionList & Stored);

  // Result of GetLoginData
  struct LoginData
  {
    QList<SessionData> Sessions;   // one per tab
    QString Workspace;             // Sessions are the tabs of this workspace
    QString DownloadFile;
    bool Aborted = false;          // nothing to do (dialog cancelled, opened in terminal...)
  };
  // GetLoginData: folder/workspace, URL or site; handles ";save" (Save session
  // dialog), ssh:// (terminal) and shows the Login dialog when a session is
  // needed but not given (or cannot be opened)
  LoginData GetLoginData(const QString & SessionName, Options & Params, LoginNeed Need,
    QWidget * LinkedForm, bool Unsafe);

  // DoSaveSession (TSaveSessionDialog "Save session as site"): saves Data as
  // a stored site; nullptr when cancelled
  std::shared_ptr<SessionData> DoSaveSession(const SessionData & Data, QWidget * Parent);

  // DoLoginDialog. Initial: preselected site / New Site data (may be null).
  // ShowAgain: the ShowAgainPanel (dialog opened automatically).
  // Returns false when cancelled.
  bool DoLoginDialog(QWidget * Parent, const SessionData * Initial, bool ShowAgain, LoginData & Result);

  // /privatekey=<file> (WinConfiguration->DefaultKeyFile): key used by
  // sessions with no key file of their own; not saved
  void SetDefaultKeyFile(const QString & FileName);
  QString DefaultKeyFile();
  // Session about to be opened: default key, environment variables
  SessionData PrepareSession(const SessionData & Data);

  // OpenSessionInPutty for ssh:// URLs: the configured terminal client
  bool OpenSessionInTerminal(const SessionData & Data);

  // /ini=<file|nul> (before Configuration::Load); FILE_NOT_EXISTS is
  // reported by the caller when the file does not exist (IniFileMissing)
  void ApplyIniSwitch(Options & Params, QString * IniFileMissing = nullptr);
  // /rawconfig (after Load, CheckSafe): the configuration is loaded from a
  // temporary copy with the settings applied (not saved to the real file)
  void ApplyRawConfig(Options & Params);
  // /loglevel=<-1..2>[*[-]], /logsize=[<count>*]<size>, /rawtransfersettings
  void ApplyLoggingAndTransferSwitches(Options & Params);
  // /log=<file>, /xmllog=<file> (CheckSafe): session logging for this run
  void ApplyLogSwitches(Options & Params);

  // Console modes of WinMain.cpp Execute (help, batchsettings, keygen,
  // copyid, fingerprintscan, info, console, script, command): runs
  // nixscp-cli with the same arguments and returns its exit code;
  // -1 when the command line is not a console one
  int RunConsoleMode(const QStringList & Arguments);

  // Explore (/browse=<file>, download URL with /browse): focuses FileName
  // in the remote panel of the session being opened in Window
  void ExploreFile(CommanderWindow * Window, const QString & FileName);

  // Opens the sessions in Window (TTerminalManager::NewSessions)
  void OpenSessions(CommanderWindow * Window, const LoginData & Data);

  // TCustomScpExplorerForm::CommandLineFromAnotherInstance / CanCommandLineFromAnotherInstance
  bool CanCommandLineFromAnotherInstance(CommanderWindow * Window);
  void CommandLineFromAnotherInstance(CommanderWindow * Window, const QStringList & Arguments,
    const QString & WorkingDirectory, const QString & ActivationToken);
  // TCustomScpExplorerForm::RefreshPanel (/refresh [session] [path])
  void RefreshPanel(CommanderWindow * Window, const QString & Session, const QString & Path);

  // RegisterForDefaultProtocols / UnregisterForProtocols: desktop entry
  // ~/.local/share/applications/nixscp-url-handler.desktop with
  // Exec=<nixscp> /unsafe %u, x-scheme-handler MIME types, xdg-mime default
  QStringList UrlSchemes();
  QString UrlHandlerDesktopFileName();
  bool RegisterForDefaultProtocols(QString * Error = nullptr);
  bool UnregisterForProtocols(QString * Error = nullptr);

  // Default local directory of command line downloads (GetPersonalFolder)
  QString PersonalFolder();
}

// /upload, URL pointing to a file, /synchronize, /keepuptodate: the session is
// opened without the main window, the operation runs with its dialogs
// (StandaloneOperation), the session is disconnected after and Finished is
// called (the application then quits, as WinSCP does with odoDisconnect).
class CommandLineOperation : public QObject
{
  Q_OBJECT

public:
  enum Kind { okUpload, okDownload, okFullSynchronize, okKeepUpToDate };

  // Params: files to upload, the remote file to download (full path),
  // or [local directory [remote directory]] of synchronization.
  // UseDefaults (/defaults[=n]): -1 show dialogs, 0 skip them, n > 0 accept
  // them automatically after n seconds
  CommandLineOperation(Kind AKind, const SessionData & Data, const QStringList & Params, int UseDefaults,
    QObject * Parent = nullptr);
  ~CommandLineOperation() override;

  void Start();
  // ExitCode: 0 when done or cancelled by the user, 1 on error
  std::function<void(int ExitCode)> Finished;

private:
  Kind FKind;
  SessionData FData;
  QStringList FParams;
  int FUseDefaults;
  GuiUserInterface * FUI = nullptr;
  Terminal * FTerm = nullptr;
  bool FDone = false;

  void Connected();
  void Upload();
  void Download();
  void Synchronize(bool KeepUpToDate);
  void Transfer(bool ToRemote, const QStringList & Paths, const QStringList & DisplayNames, const QString & TargetDirectory);
  void Finish(int ExitCode);
  int ExecCopyDialog(class CopyDialog & Dialog);
};

// LastTerminalClosed -> NeedSession: when the last session of the window is
// closed (or disconnected / lost and not reconnected) and "Show Login dialog
// on startup and when the last session is closed" is on, the Login dialog is
// shown. Watches the sessions (Terminal objects) of the window. Disabled when
// the window sets the dynamic property "HandlesLastSessionClosed" to true.
class LastSessionWatcher : public QObject
{
  Q_OBJECT

public:
  explicit LastSessionWatcher(CommanderWindow * Window);

  // Tests: poll interval
  void SetInterval(int Ms);

protected:
  bool eventFilter(QObject * Object, QEvent * Event) override;

private:
  QPointer<CommanderWindow> FWindow;
  QTimer * FTimer;
  int FLastCount = 0;
  bool FPending = false;
  bool FLoginShown = false;

  void Check();
  int SessionCount() const;
};
