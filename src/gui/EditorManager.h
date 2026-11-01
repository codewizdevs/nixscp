// Port of WinSCP source/windows/EditorManager.cpp (TEditorManager) and the
// edit/open parts of forms/CustomScpExplorer.cpp (ExecuteFile,
// CustomExecuteFile, ExecuteCurrentFileWith, ExecutedFileChanged,
// ExecutedFileReload, ExecutedFileEarlyClosed, EditorAutoConfig,
// TemporaryDirectoryForRemoteFiles, the FormCloseQuery editor checks),
// forms/NonVisual.cpp (CreateEditorListMenu, the Edit actions),
// forms/EditorPreferences (ad-hoc "Edit With" mode) and the temporary folder
// cleanup of windows/Setup.cpp / WinConfiguration.cpp (FindTemporaryFolders).
//
// Opens local and remote files in the internal editor, an external editor or
// the associated application (xdg-open), and uploads remote files back
// whenever they are saved:
//  - remote files are downloaded into a private folder per file (mkdtemp,
//    mode 0700) under the custom temporary directory (Preferences > Storage,
//    DDTemporaryDirectory), else $XDG_RUNTIME_DIR, else the system temporary
//    directory; TemporaryDirectoryAppendSession/AppendPath/Deterministic are
//    honoured. Each folder holds a lock file of the owning instance, so the
//    startup cleanup (TemporaryDirectoryCleanup) skips folders of other
//    running instances.
//  - external editors are started without a shell: the configured command is
//    split into arguments and "!.!" (or "%1") is replaced in the arguments
//    with the file name, so no file name can inject shell commands.
//  - "External editor opens each file in separate window (process)"
//    (SDIExternalEditor): the editor process is tracked (pidfd); when it
//    exits, the file is checked for changes one last time and its temporary
//    folder is deleted. Editors that exit too early (Editor\EarlyClose,
//    single-instance editors) are detected as WinSCP does. Other external
//    editors and xdg-open are "no process" (OPEN_FILE_NO_PROCESS2): the file
//    is watched until NixSCP exits.
//  - an already opened file is not downloaded again: its internal editor is
//    activated (CanAddFile, ALREADY_EDITED_EXTERNALLY_OR_UPLOADED).
//  - uploads wait while the session is busy; when the session was closed,
//    the file can be attached to another open session of the same site
//    (EDIT_SESSION_REATTACH) or the session reconnected (EDIT_SESSION_RECONNECT);
//    with EditorCheckNotModified the remote file is checked for changes
//    first (EDIT_CHANGED_EXTERNALLY).
//  - internal editor Reload downloads the file again (ExecutedFileReload).
//
// Windows-only parts removed: process handle waiting (replaced by pidfd),
// ShellExecute (xdg-open), Notepad/FindExecutable detection (replaced by the
// text/plain default application of xdg-mime).
//
// Integration (main window):
//   FEditorManager = new EditorManager(this);
//   EditorManager::Hooks H;  H.Terminals = ...;  H.Selection = ...;  ...
//   FEditorManager->SetHooks(H);
//   menus: FEditorManager->EditAction() (F4), EditInternalAction() (Ctrl+Alt+F4),
//          EditWithAction(), EditorListMenu() ("Edit" submenu / toolbar drop-down)
//   open:  FEditorManager->ExecuteFiles(Term, Paths, EditorManager::efShell)
//   closeEvent: if (!FEditorManager->CanClose()) { Event->ignore(); return; }
//   startup: EditorManager::TemporaryDirectoryCleanup(MainWindow)
#pragma once

#include "core/Configuration.h"
#include "core/Terminal.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QStringList>

#include <functional>
#include <memory>

class TransferQueue;

class EditorWindow;
class QAction;
class QFileSystemWatcher;
class QLockFile;
class QMenu;
class QSocketNotifier;
class QTimer;
class QWidget;

class EditorManager : public QObject
{
  Q_OBJECT

public:
  // TExecuteFileBy
  enum ExecuteFileBy { efShell, efInternalEditor, efExternalEditor, efDefaultEditor };

  // Files the Edit actions work on (selected files of the current panel)
  struct Selection
  {
    Terminal * Term = nullptr;     // remote panel's session; nullptr = local files
    QStringList Paths;             // full paths of the selected files (directories are skipped)
  };

  // What the manager needs from the main window; all optional
  struct Hooks
  {
    // Connected sessions (to re-attach edited files of a closed session)
    std::function<QList<Terminal *>()> Terminals;
    // false while the window runs another operation on the session
    // (in addition to Terminal::Busy)
    std::function<bool(Terminal *)> CanStartOperation;
    // The manager starts/finished an operation on the session (download or upload)
    std::function<void(Terminal *, bool Running)> SetOperationRunning;
    // An edited file was uploaded to RemoteDirectory (refresh the remote panel)
    std::function<void(Terminal *, const QString & RemoteDirectory)> FileUploaded;
    // Status bar message (Timeout in ms, 0 = until replaced)
    std::function<void(const QString & Message, int Timeout)> ShowStatus;
    // Selected files for the Edit actions
    std::function<Selection()> CurrentSelection;
    // Background queue of the session (fcBackgroundTransfers): saved edited
    // files are uploaded through it, as TEditorUploadQueueItem in WinSCP
    std::function<TransferQueue *(Terminal *)> Queue;
  };

  explicit EditorManager(QWidget * Window);
  ~EditorManager() override;

  void SetHooks(const Hooks & AHooks);
  const Hooks & GetHooks() const { return FHooks; }

  // TCustomScpExplorerForm::ExecuteFile: opens each (non-directory) file of
  // Paths; Term = session of remote files (nullptr = local files).
  // External is the editor for efExternalEditor (nullptr = by file mask).
  void ExecuteFiles(Terminal * Term, const QStringList & Paths, ExecuteFileBy By = efDefaultEditor,
    const EditorPreferences * External = nullptr);
  // TCustomScpExplorerForm::EditNew for a remote file that does not exist
  // yet: an empty local temporary file is opened in the default editor and
  // uploaded to RemotePath when saved (nothing is created on the server
  // when the editor is closed without saving)
  void ExecuteNewRemoteFile(Terminal * Term, const QString & RemotePath);
  // The Edit actions on the current selection (Hooks::CurrentSelection)
  void ExecuteSelection(ExecuteFileBy By, const EditorPreferences * External = nullptr);
  // ExecuteCurrentFileWith: "Editor" dialog (ad-hoc mode, "Remember this editor")
  void ExecuteSelectionWith();

  // Actions (forms/NonVisual.dfm) for menus and toolbars; enabled while
  // Hooks::CurrentSelection has files (UpdateActions)
  QAction * EditAction() const { return FEditAction; }                   // CurrentEditAction "&Edit" F4
  QAction * EditInternalAction() const { return FEditInternalAction; }   // CurrentEditInternalAction "&Internal editor" Ctrl+Alt+F4
  QAction * EditWithAction() const { return FEditWithAction; }           // CurrentEditWithAction "Edit &With..."
  QAction * EditorConfigureAction() const { return FConfigureAction; }   // EditorListCustomizeAction "&Configure..."
  // CreateEditorListMenu: Edit, Internal editor, configured external editors,
  // Edit With..., Configure... (rebuilt whenever shown)
  QMenu * EditorListMenu() const { return FEditorListMenu; }
  void UpdateActions();

  // TCustomScpExplorerForm::FormCloseQuery (editor part): closes internal
  // editors (they ask to save), forgets files without editor process and,
  // when files are still open or uploading, asks PENDING_EDITORS
  // (Ignore = exit anyway; their temporary folders are then kept).
  bool CanClose();
  // TEditorManager::Empty: no edited remote files
  bool Empty() const;
  int Count() const;
  // Edited remote files (local temporary paths)
  QStringList EditedFiles() const;
  // Uploads changed files now (TEditorManager::Check)
  void Check();

  // ---- helpers (static, tested separately)

  // Arguments to execute external editor Command for FileName, without a
  // shell: the command is split like a shell would (quotes, backslash),
  // "!.!" (or "%1") is replaced inside each argument, otherwise FileName is
  // appended; environment variables in the program are expanded
  static QStringList EditorCommandArguments(const QString & Command, const QString & FileName);
  // Folder for temporary copies (custom directory, $XDG_RUNTIME_DIR or system temp)
  static QString TemporaryBaseDirectory();
  // Base folders searched for stale temporary folders
  static QStringList TemporaryBaseDirectories();
  // TWinConfiguration::FindTemporaryFolders: folders left by instances that
  // are not running any more
  static QStringList FindTemporaryFolders();
  // TWinConfiguration::CleanupTemporaryFolders; false if some were not deleted
  static bool CleanupTemporaryFolders(const QStringList & Folders);
  // windows/Setup.cpp TemporaryDirectoryCleanup: at startup, when
  // TemporaryDirectoryCleanup is on; asks first when ConfirmTemporaryDirectoryCleanup
  static void TemporaryDirectoryCleanup(QWidget * Parent);
  // DetectSystemExternalEditor (Linux: xdg-mime default for text/plain);
  // Command gets the "!.!" pattern; false when none/terminal-only
  static bool DetectSystemExternalEditor(QString & Command, QString & Description);
  // Creates a private (0700) unique folder "nixscp-XXXXXX" in Base, holding a
  // lock file of this instance; empty on error
  static QString CreateUniqueTemporaryDirectory(const QString & Base, std::unique_ptr<QLockFile> * Lock = nullptr);
  // Name of the lock file in each temporary folder
  static QString LockFileName();

signals:
  // An edited file was uploaded / its editor closed (for tests and status)
  void FileUploaded(const QString & LocalFileName, bool Ok);
  void FileClosed(const QString & LocalFileName);

private:
  struct EditedFile;

  QWidget * FWindow;
  Hooks FHooks;
  QList<EditedFile *> FFiles;
  QFileSystemWatcher * FWatcher;
  QTimer * FCheckTimer;
  QAction * FEditAction;
  QAction * FEditInternalAction;
  QAction * FEditWithAction;
  QAction * FConfigureAction;
  QMenu * FEditorListMenu;
  QList<EditorWindow *> FLocalEditors;
  quint64 FNextId = 1;

  void ExecuteNext(QPointer<Terminal> Term, QStringList Paths, ExecuteFileBy By, EditorPreferences External, bool HasExternal);
  void ExecuteRemoteFile(Terminal * Term, const QString & RemotePath, ExecuteFileBy By, const EditorPreferences * External,
    const std::function<void()> & Next);
  void ExecuteLocalFile(const QString & Path, ExecuteFileBy By, const EditorPreferences * External);
  bool ExecuteFileNormalize(ExecuteFileBy & By, const EditorPreferences *& External, const QString & FileName);
  void CustomExecuteFile(EditedFile * File, ExecuteFileBy By, const EditorPreferences * External);
  static bool StartExternal(const QString & Command, const QString & FileName, qint64 * Pid);
  bool CanAddFile(Terminal * Term, const QString & RemoteDirectory, const QString & OriginalFileName,
    QString & ExistingLocalRootDirectory, QString & ExistingLocalDirectory, std::unique_ptr<QLockFile> & ExistingLock);
  bool TemporaryDirectoryForRemoteFiles(Terminal * Term, const QString & RemoteDirectory, QString & Directory,
    QString & RootDirectory, std::unique_ptr<QLockFile> & Lock);
  EditedFile * FindFile(quint64 Id) const;
  void EditorAutoConfig();
  void RunOperation(Terminal * Term, const std::function<void(Terminal::Callback)> & Start,
    const Terminal::Callback & Done, bool ShowProgress);
  bool TerminalIdle(Terminal * Term) const;
  bool TerminalAvailable(Terminal * Term) const;
  void ShowStatus(const QString & Message, int Timeout = 0);

  // TEditorManager
  void AddFile(EditedFile * File);
  void WatchFile(EditedFile * File);
  void FileChanged(EditedFile * File);
  void CheckFileChange(EditedFile * File, bool Force);
  void UploadFile(EditedFile * File);
  void UploadComplete(EditedFile * File, bool Ok);
  void FileReload(EditedFile * File, const std::function<void(bool)> & Done);
  void InternalEditorClosed(EditedFile * File, bool Forced);
  void ProcessFinished(EditedFile * File);
  void TrackProcess(EditedFile * File);
  bool EarlyClose(EditedFile * File);
  void CloseProcess(EditedFile * File);
  void CloseFile(EditedFile * File, bool Delete);
  static bool FileStamp(const QString & FileName, qint64 & Time, qint64 & Size);
  bool ReattachSession(EditedFile * File);
  void RebuildEditorListMenu();
};
