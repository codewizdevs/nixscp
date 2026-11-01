// Linux counterparts of the Windows system calls of WinSCP:
// - SuspendWindows / ShutDownWindows (source/windows/Tools.cpp) -> systemd-logind
//   org.freedesktop.login1.Manager.Suspend / PowerOff (interactive=true, polkit decides)
// - TFileOperationProgressType::SystemRequired (source/core/FileOperationProgress.cpp,
//   SetThreadExecutionState(ES_SYSTEM_REQUIRED)) -> logind Inhibit("sleep:idle", ..., "block")
// - OpenFolderInExplorer / OpenFileInExplorer (source/windows/Tools.cpp) ->
//   org.freedesktop.FileManager1.ShowFolders / ShowItems
// - the "closed on completion" prompt of ShowExtendedExceptionEx
//   (source/windows/UserInterface.cpp, ESshTerminate, ConfirmExitOnCompletion)
//
// Uses QtDBus when built with NIXSCP_HAVE_QTDBUS (and linked with Qt6::DBus);
// otherwise the same D-Bus calls go through the systemd command line tools
// (busctl, systemd-inhibit), and notify-send / QDesktopServices.
#pragma once

#include <QString>

#include <functional>

class QWidget;

namespace SystemActions
{

// TOnceDoneOperation (source/core/Exceptions.h); same values as QueuePanel::OnceEmptyOperation
enum OnceDoneOperation { odoIdle, odoDisconnect, odoSuspend, odoShutDown };

// Menu captions of the operation ("&Stay Idle", "&Disconnect Session",
// "Slee&p Computer", "&Shut Down Computer"), Progress.dfm / NonVisual.dfm
QString OnceDoneCaption(OnceDoneOperation Operation);
// Icon name for Icons::Get (queue-idle-once-empty, ...)
QString OnceDoneIconName(OnceDoneOperation Operation);

// SuspendWindows: logind Manager.Suspend(true). Returns false when the request
// could not even be sent (Error says why); the result of the polkit decision
// arrives asynchronously and is reported through the error callback, if set.
bool Suspend(QString * Error = nullptr);
// ShutDownWindows: logind Manager.PowerOff(true)
bool ShutDown(QString * Error = nullptr);
// Suspend / ShutDown for those operations; odoIdle and odoDisconnect do nothing
// here (disconnecting is the main window's job) and return true.
bool Execute(OnceDoneOperation Operation, QString * Error = nullptr);

// Errors of asynchronous requests (polkit refusal, logind missing); the
// default prints a warning.
void SetErrorHandler(const std::function<void(const QString &)> & Handler);

// Test hook: when set, Suspend/ShutDown call it instead of logind
// (so tests never power the machine off). Pass an empty function to reset.
void SetSystemActionHandler(const std::function<bool(OnceDoneOperation)> & Handler);

// OpenFolderInExplorer / OpenFileInExplorer: shows the folder, or the file
// selected in its folder, in the desktop file manager
void ShowFolder(const QString & Path);
void ShowFile(const QString & FileName);
// ExecuteShellChecked on a downloaded file: opens it with its default application
bool OpenFile(const QString & FileName);

// Desktop notification (org.freedesktop.Notifications), e.g. the completion
// balloon of a minimized application (TCustomScpExplorerForm::OperationComplete)
void Notify(const QString & Title, const QString & Message);

// ESshTerminate handling of ShowExtendedExceptionEx after the session was
// closed on completion (Once Finished: Disconnect/Sleep). Shows
// CLOSED_ON_COMPLETION and, when this was the active session, asks
// EXIT_ON_COMPLETION / DISCONNECT_ON_COMPLETION; with a local target an
// "Open" menu button offers "Open &Target Folder" / "Open &Downloaded File".
// Honours and updates Configuration::ConfirmExitOnCompletion (never ask again).
// Returns true when the application should exit (Yes, or no confirmation).
bool ConfirmCloseOnCompletion(QWidget * Parent, OnceDoneOperation Operation, bool ForActiveSession,
  int OtherSessions, const QString & TargetLocalPath = QString(), const QString & DestLocalFileName = QString());

// Sleep inhibitor while operations or the queue run. Reference counted, GUI
// thread only: the first Acquire takes logind's "sleep:idle" block lock
// ("NixSCP", "Transfer in progress"), the last Release drops it.
class SleepInhibitor
{
public:
  static void Acquire();
  static void Release();
  static bool Held();          // reference count > 0
  static bool LockActive();    // the logind lock is actually held (or being requested)
  // Test hook: false = count references only, do not talk to logind
  static void SetBackendEnabled(bool Enabled);
};

// RAII holder of a SleepInhibitor reference
class SleepInhibitLock
{
public:
  SleepInhibitLock() { SleepInhibitor::Acquire(); }
  ~SleepInhibitLock() { SleepInhibitor::Release(); }
  SleepInhibitLock(const SleepInhibitLock &) = delete;
  SleepInhibitLock & operator=(const SleepInhibitLock &) = delete;
};

}
