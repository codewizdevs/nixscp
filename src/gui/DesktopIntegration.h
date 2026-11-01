// Linux desktop integration replacing the Windows shell features of WinSCP:
// - Explore Directory (Ctrl+Alt+E): windows/Tools.cpp OpenFolderInExplorer /
//   OpenFileInExplorer -> org.freedesktop.FileManager1 ShowFolders/ShowItems,
//   falling back to xdg-open (QDesktopServices)
// - Change Drive (Alt+F1) drive list: forms/ScpCommander.cpp
//   LocalPathComboBox (drives and special folders) -> "Places": XDG user
//   directories (QStandardPaths), GTK bookmarks and mounted volumes (QStorageInfo)
// - Minimize to tray (MinimizeToTray), tray balloons (BalloonNotifications,
//   NotificationsTimeout), FlashOnBackground (FlashTaskbar), the completion
//   beep (BeepOnFinish) and the taskbar progress (ITaskbarList3) of
//   forms/CustomScpExplorer.cpp / windows/UserInterface.cpp -> QSystemTrayIcon,
//   org.freedesktop.Notifications, QApplication::alert and the
//   com.canonical.Unity.LauncherEntry progress signal (KDE, Dock, Plank...)
// - Open in PuTTY (windows/GUITools.cpp OpenSessionInPutty) -> the terminal
//   client command of Preferences > Integration > Applications with all
//   custom command patterns (!@ !U !P !# !/ !S !E !N !K !?prompt?default! !`cmd`)
//   and the OpenSSH equivalents of the PuTTY session settings (-p -l -i -J -o
//   -A -C -4/-6), telnet for FTP sessions (TelnetForFtpInPutty)
// - Desktop Icon (forms/Login.cpp DesktopIconActionExecute, windows/Tools.cpp
//   CreateDesktopSessionShortCut) -> a .desktop launcher in the XDG desktop folder
// - local copy/move and local Properties run with progress on a worker thread
//   (core/LocalFileOperations)
//
// D-Bus calls use QtDBus when built with NIXSCP_HAVE_QTDBUS (Qt6::DBus linked),
// otherwise the gdbus command line tool (glib), which every desktop has.
#pragma once

#include "core/LocalFileOperations.h"
#include "core/SessionData.h"

#include <QIcon>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <functional>

class QMainWindow;
class QMenu;
class QSystemTrayIcon;
class QWidget;
class UserInterface;

namespace DesktopIntegration
{

// ---- Test hook: every external program started by this module (gdbus,
// xdg-open, terminals) goes through it when set; return true = started.
// Pass an empty function to reset.
using CommandRunner = std::function<bool(const QString & Program, const QStringList & Arguments)>;
void SetCommandRunner(const CommandRunner & Runner);

// ---- Explore Directory (Ctrl+Alt+E) ----
// Shows the folder in the desktop file manager (FileManager1.ShowFolders),
// or the file selected in its folder (ShowItems); without the D-Bus service
// the folder is opened with xdg-open.
void ExploreDirectory(const QString & Path);
void ExploreFile(const QString & FileName);

// ---- Places (Change Drive, Alt+F1) ----
struct Place
{
  enum Kind { pkStandard, pkBookmark, pkVolume };
  Kind Type = pkStandard;
  QString Caption;
  QString Path;
  QString IconName;           // freedesktop icon name
};
// Home, Desktop, Documents, Downloads, Music, Pictures, Videos, File System,
// then GTK bookmarks (~/.config/gtk-3.0/bookmarks), then mounted volumes
// (removable media, /mnt, /media, gvfs/network mounts); only existing ones
QList<Place> Places();
// Parses a GTK bookmarks file ("file:///path optional label" per line);
// only local, existing directories
QList<Place> BookmarkPlaces(const QString & BookmarksFile);
// Adds the places to Menu (separated groups); Selected gets the path
void FillPlacesMenu(QMenu * Menu, const std::function<void(const QString & Path)> & Selected);
// Pops the places menu up below Anchor (or at the cursor without an anchor)
void ShowPlacesMenu(QWidget * Anchor, const std::function<void(const QString & Path)> & Selected);

// ---- Attention ----
enum NotificationType { ntInformation, ntWarning, ntError };
// TCustomScpExplorerForm::PopupTrayBalloon / Notify: a desktop notification
// (or the tray balloon while minimized to the tray) when BalloonNotifications
// is on, otherwise FlashOnBackground. Message may contain WinSCP's
// "**main instruction**" markup (used as the title for information).
// TimeoutSeconds 0 = NotificationsTimeout (Configuration\Interface, default 10).
// Returns true when a notification was shown.
bool Notify(const QString & Title, const QString & Message, NotificationType Type = ntInformation,
  int TimeoutSeconds = 0);
// FlashOnBackground (FlashTaskbar): the window asks for attention
// (urgency hint / taskbar flash) when the application is not active
void FlashOnBackground(QWidget * Window = nullptr);
// TCustomScpExplorerForm::OperationComplete for operations without a
// progress window (queue, synchronization...): beep after BeepOnFinishAfter
// (BeepOnFinish), BALLOON_OPERATION_COMPLETE when the window is minimized or
// in the background, and flash. ElapsedMs: duration of the operation.
void OperationComplete(QWidget * Window, qint64 ElapsedMs, const QString & Message = QString());

// ---- Taskbar progress ----
// Percent 0..100, or -1 = no progress (hides the progress bar of the dock entry)
void SetTaskbarProgress(int Percent);
// "NN% Uploading - ..." progress title (ProgressDialog::ProgressTitleChanged):
// updates the taskbar progress and the tray icon tooltip; empty = finished
void SetProgressTitle(const QString & Title);
// Percent of a progress title, -1 when it has none
int ProgressTitlePercent(const QString & Title);

// ---- Main window (minimize to tray) ----
// Installs the tray icon handling on the main window: with MinimizeToTray,
// minimizing hides the window and shows the tray icon (tooltip = window
// title); clicking the icon restores the window (TrayIconClick / RestoreApp).
// Call once; the integration lives as long as the window.
void InstallMainWindow(QMainWindow * Window);
// The window is minimized to the tray (FTrayIcon->Visible)
bool IsMinimizedToTray();
// Restores the window from the tray (RestoreApp)
void RestoreFromTray();

// ---- Terminal client (Open in PuTTY) ----
struct TerminalCommand
{
  QString Program;
  QStringList Arguments;
  QString AskPassFile;        // one-time password file read by nixscp as SSH_ASKPASS
};
// Builds the command line of OpenSessionInPutty for Data. Password: session
// password (or passphrase) when not stored in Data; it is passed (as
// SSH_ASKPASS) only with "Remember session password and pass it to PuTTY"
// (PuttyPassword). RemoteDirectory: !/ and, when ChangeDirectory, the directory
// the remote shell starts in. Prompts answers !?prompt?default! patterns
// (all at once); without it the defaults are used. Returns false with Error
// (pattern errors, cancelled prompts).
bool BuildTerminalCommand(const SessionData & Data, const QString & Password, const QString & RemoteDirectory,
  bool ChangeDirectory, TerminalCommand & Command, QString & Error,
  const std::function<bool(const QStringList & Prompts, const QStringList & Defaults, QStringList & Values)> & Prompts = nullptr,
  const QString & CommandTemplate = QString());
// OpenSessionInPutty: builds and starts the command (asking for prompt
// patterns with the custom command prompts dialog). Errors are shown.
bool OpenSessionInTerminal(QWidget * Parent, const SessionData & Data, const QString & Password = QString(),
  const QString & RemoteDirectory = QString(), bool ChangeDirectory = false);
// Terminal emulator fallback when the configured program (x-terminal-emulator)
// does not exist (non-Debian distributions): $TERMINAL, xdg-terminal-exec,
// the terminal of the current desktop (XDG_CURRENT_DESKTOP: ptyxis/kgx/
// gnome-terminal on GNOME, konsole on KDE, xfce4-terminal on Xfce...), then
// any known one; a leading "-e" of Arguments is replaced in place by the
// emulator's "execute" switch ("--", "-x", none...)
QString ResolveTerminalProgram(const QString & Program, QStringList & Arguments);

// ---- Desktop icon (Login dialog: Tools > Desktop Icon) ----
enum ShortCutKind { sscSite, sscFolder, sscWorkspace };
// Contents of the .desktop launcher (Exec=<nixscp> "<site>" [/workspace])
QString DesktopEntry(const QString & SessionName, ShortCutKind Kind, const QString & InfoTip = QString(),
  const QString & Executable = QString());
// Asks CONFIRM_CREATE_SHORTCUT(_FOLDER/_WORKSPACE) and writes
// <XDG desktop>/<name>.desktop (executable, trusted where gio is available).
// Directory: target folder (tests); empty = QStandardPaths::DesktopLocation.
// Confirm false skips the question. Returns the file name, empty on failure/No.
QString CreateDesktopIcon(QWidget * Parent, const QString & SessionName, ShortCutKind Kind,
  const QString & InfoTip = QString(), bool Confirm = true, const QString & Directory = QString());

// ---- Local file operations with progress (worker thread) ----
// Copy/move (drag & drop within/between local panels, paste) or Properties:
// runs LocalFileOperations on a worker thread with the progress window
// (Cancel, Skip this file); prompts through UI (GuiUserInterface).
// Errors are shown; Done (optional) gets the result on the GUI thread.
void RunLocalFileOperation(QWidget * Parent, UserInterface * UI, const LocalFileOperations::Params & Params,
  const std::function<void(const LocalOperationResult &)> & Done = nullptr);
void RunLocalProperties(QWidget * Parent, UserInterface * UI, const LocalFileOperations::PropertiesParams & Params,
  const std::function<void(const LocalOperationResult &)> & Done = nullptr);

} // namespace DesktopIntegration
