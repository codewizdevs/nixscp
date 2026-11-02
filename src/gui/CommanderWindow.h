// Port of WinSCP source/forms/ScpCommander.{cpp,dfm}, forms/CustomScpExplorer.cpp
// and forms/NonVisual.{cpp,dfm} (actions, menus, toolbars, popups: see
// gui/CommanderActions.h), windows/TerminalManager.cpp (tabs, local browser)
#pragma once

#include "core/SessionData.h"
#include "core/RemoteFile.h"
#include "gui/FilePanel.h"

#include <QMainWindow>
#include <QMap>
#include <QElapsedTimer>
#include <QPointer>
#include <memory>

class QAction;
class QComboBox;
class QLabel;
class QMenu;
class QSplitter;
class QStackedWidget;
class QTabBar;
class QTimer;
class QToolBar;
class Terminal;
class TransferQueue;
class GuiUserInterface;
class QueuePanel;
class CommanderActions;
class EditorManager;
class Bookmark;
struct OperationResult;
struct CustomCommandContext;
struct CopyParam;

// One session tab (TManagedTerminal in WinSCP): own panels, connection and queue.
// A local browser tab (TManagedTerminal::LocalBrowser) has two local panels:
// RemotePanel is then the right (other) local panel.
struct SessionTab
{
  QSplitter * Page = nullptr;
  FilePanel * LocalPanel = nullptr;
  FilePanel * RemotePanel = nullptr;     // right panel (remote, or other local in a local browser)
  FilePanel * LastActivePanel = nullptr;
  LocalDirectoryProvider LocalProvider;
  LocalDirectoryProvider OtherLocalProvider;
  std::unique_ptr<RemoteDirectoryProvider> RemoteProvider;
  Terminal * Term = nullptr;
  TransferQueue * Queue = nullptr;
  SessionData Data;
  QString SessionPassword;          // password typed at login, if remembering is enabled
  QElapsedTimer SessionTimer;
  bool OperationRunning = false;
  bool Connecting = false;
  bool LocalBrowser = false;        // two local panels (New Local Tab)
  bool Implicit = false;            // the local browser shown when no session is open (replaced by the first session)
  QString Name;                     // Rename Tab (empty = automatic caption)
  int Color = 0;                    // tab color (BGR TColor, 0 = none); from the site
  // Synchronized browsing (TScpCommanderForm::SynchronizeBrowsing)
  bool SynchronizeBrowsing = false;
  QString SyncPrevPath[2];          // [0] local, [1] right panel: the previous directory
  QString SyncPending[2];           // directory the panel is being changed to by synchronized browsing
  bool SyncCreateAsked = false;
};

class CommanderWindow : public QMainWindow
{
  Q_OBJECT

public:
  explicit CommanderWindow(QWidget * Parent = nullptr);
  ~CommanderWindow() override;

  void NewSession();
  void OpenSession(const SessionData & Data);
  void OpenWorkspace(const QString & Name);
  // TTerminalManager::NewLocalSession (Tabs > New Tab > Local Tab)
  void NewLocalTab(const QString & LocalDirectory = QString(), const QString & OtherLocalDirectory = QString());

  CommanderActions * Actions() const { return FA; }
  // The connected tabs with their current directories (workspace / cloud session)
  QList<SessionData> WorkspaceTabs() const;
  EditorManager * Editors() const { return FEditorManager; }

public slots:
  // /browse (TCustomScpExplorerForm::BrowseFile): focuses the file in the remote panel
  void ExploreFile(const QString & FileName);
  // /refresh (TCustomScpExplorerForm::RefreshPanel): reloads remote panels of
  // sessions matching Session (empty = any) whose directory is Path (empty = any)
  void RefreshPanel(const QString & Session, const QString & Path);

protected:
  void closeEvent(QCloseEvent * Event) override;
  bool eventFilter(QObject * Object, QEvent * Event) override;
  QMenu * createPopupMenu() override;
  void changeEvent(QEvent * Event) override;

private:
  CommanderActions * FA = nullptr;
  EditorManager * FEditorManager = nullptr;
  QList<SessionTab *> FTabs;           // in tab bar order (excluding "New Tab")
  QTabBar * SessionTabs;
  QStackedWidget * Pages;
  QSplitter * QueueSplitter;
  QueuePanel * FQueuePanel;
  QLabel * StatusSecureLabel;
  QLabel * StatusProtocolLabel;
  QLabel * StatusDurationLabel;
  QTimer * DurationTimer;
  GuiUserInterface * FUI;
  bool FUpdatingTabs = false;
  bool FClosing = false;
  bool FExecutedByShortcut = false;   // the action being executed was triggered by its shortcut

  // Menus and toolbars
  QMenu * FLocalMenu = nullptr;
  QMenu * FRemoteMenu = nullptr;
  QMenu * FFileCustomCommandsMenu = nullptr;
  QMenu * FCommandsCustomCommandsMenu = nullptr;
  QMenu * FSitesMenu = nullptr;
  QMenu * FWorkspacesMenu = nullptr;
  QMenu * FOpenedTabsMenu = nullptr;
  QMenu * FColorMenu = nullptr;
  QToolBar * FCommandsBar = nullptr;
  QToolBar * FSessionBar = nullptr;
  QToolBar * FPreferencesBar = nullptr;
  QToolBar * FSortBar = nullptr;
  QToolBar * FTransferBar = nullptr;
  QToolBar * FCustomCommandsBar = nullptr;
  QToolBar * FHotKeysBar = nullptr;
  QComboBox * FTransferCombo = nullptr;
  bool FUpdatingTransferCombo = false;
  // Items whose action follows the active side (ScpCommander.cpp UpdateControls)
  QAction * FCurrentCopyItem = nullptr;         // Files > Upload/Download (dropdown)
  QAction * FCurrentCopyNonQueueItem = nullptr;
  QAction * FCurrentCopyQueueItem = nullptr;
  QAction * FCurrentMoveItem = nullptr;
  QAction * FCurrentCopyToItem = nullptr;       // Duplicate / local Copy
  QAction * FCurrentMoveToItem = nullptr;       // Move To / local Move
  QAction * FLocalCopyItem = nullptr;           // left panel File toolbar
  QAction * FLocalMoveItem = nullptr;
  QAction * FRemoteCopyItem = nullptr;          // right panel File toolbar
  QAction * FRemoteMoveItem = nullptr;
  QAction * FHotCopyItem = nullptr;             // Hot Keys toolbar F5 / F6
  QAction * FHotMoveItem = nullptr;
  QList<QAction *> FSelectTabActions;           // Alt+1..9, Alt+0
  QList<QAction *> FCustomCommandActions;
  // Panel band visibility (ToolbarsLayout2 Local*/Remote* toolbars)
  bool FBandVisible[2][4] = {{true, true, true, true}, {true, true, true, true}};
  int FToolbarIconSize = 16;

  void CreateActions();
  void ConnectActions();
  void CreateMenus();
  void CreateToolbars();
  void CreateStatusBar();
  void CreatePanelToolbars(SessionTab * Tab);
  void UpdatePanelToolbars();
  void LoadToolbarSettings();
  void SaveToolbarSettings();
  void ApplyToolbarStyle();
  void UpdateActions();
  void UpdateSideItems();
  void UpdateSortActions();
  void UpdateTitle();
  void UpdateStatusBar();
  void UpdateQueueView();
  void UpdateTransferCombo();
  void UpdateHotKeyCaptions();

  // Tabs
  SessionTab * CreateTab(int Index = -1, bool LocalBrowser = false, const QString & LocalDirectory = QString(),
    const QString & OtherLocalDirectory = QString());
  void CloseTab(SessionTab * Tab, bool KeepOne = true);
  void ActivateTab(SessionTab * Tab);
  void UpdateTabCaption(SessionTab * Tab);
  QString TabCaption(SessionTab * Tab) const;
  SessionTab * CurrentTab() const;
  SessionTab * TabOfTerminal(Terminal * Term) const;
  int TabIndex(SessionTab * Tab) const;
  void NewTab(bool Remote);
  void NewTabFromCurrent(bool AllowReverse);
  void DuplicateTab();
  void RenameTab();
  void ShowTabMenu(int Index, const QPoint & GlobalPos);
  void FillOpenedTabsMenu(QMenu * Menu);
  void FillSitesMenu(QMenu * Menu);
  void FillWorkspacesMenu(QMenu * Menu);
  void FillColorMenu(QMenu * Menu);

  FilePanel * LocalPanel() const { return CurrentTab()->LocalPanel; }
  FilePanel * RemotePanel() const { return CurrentTab()->RemotePanel; }
  FilePanel * ActivePanel() const;
  FilePanel * OtherPanel(FilePanel * Panel) const;
  FilePanel * SidePanel(bool Remote) const;
  bool IsRemotePanel(const FilePanel * Panel) const { return Panel->IsRemote(); }
  bool IsLocalBrowserMode() const;
  Terminal * CurrentTerminal() const { return CurrentTab()->Term; }
  Terminal * PanelTerminal(FilePanel * Panel) const;
  bool Connected(const SessionTab * Tab = nullptr) const;
  bool PanelReady(FilePanel * Panel) const;
  bool CheckReady();
  void SessionClosed(SessionTab * Tab);
  void DisconnectTab(SessionTab * Tab);
  bool CanCloseQueue(SessionTab * Tab);

  // MoveToQueue: offers "Proceed in Background"; called after the operation
  // stopped on that request (TCustomScpExplorerForm::FMoveToQueue).
  // TargetLocalPath: local target of a download, for the "closed on completion" prompt.
  void RunWithProgress(const std::function<void(std::function<void(const OperationResult &)>)> & Start,
    const std::function<void(const OperationResult &)> & Done = nullptr, const std::function<void()> & MoveToQueue = nullptr,
    const QString & TargetLocalPath = QString());
  // Foreground transfer with Proceed in Background (ExecuteFileOperation)
  void RunTransfer(SessionTab * Tab, const QStringList & Paths, bool FromRemote, const QString & TargetDirectory,
    const CopyParam & Param, bool Move, const std::function<void(const OperationResult &)> & Done = nullptr);
  // Once Finished / Once Empty: TTerminal::CloseOnCompletion and the
  // ESshTerminate handling (disconnects the tab, may close the application);
  // ExecuteSystem also suspends / shuts down the computer
  void ExecuteOnceDone(SessionTab * Tab, int Operation, bool ExecuteSystem, const QString & TargetLocalPath = QString());
  // qePendingUserAction of a tab's queue (TCustomScpExplorerForm::QueueEvent)
  void QueueUserActionPending(SessionTab * Tab, int Id);
  void ShowResultError(const OperationResult & Result);
  void RefreshTab(SessionTab * Tab);

  // Operations on a side (Remote = right panel); Focused: popup command
  void CopyFiles(bool Remote, bool Move, int QueueMode = 0);
  void LocalLocalCopy(bool Remote, bool Move);
  void TransferFiles(const QStringList & Paths, bool FromRemote, const QString & TargetDirectory, bool Move, bool Confirm,
    int QueueMode = 0, bool ShortCutHint = false);
  void HandleDrop(FilePanel * Target, const QStringList & Paths, bool FromRemote, const QString & TargetDirectory,
    Qt::DropAction Action, bool Paste = false);
  void DeleteFiles(FilePanel * Panel, bool Alternative);
  void RenameFile(FilePanel * Panel, const QString & OldPath, const QString & NewName);
  void CreateDirectory(FilePanel * Panel);
  void AddEditLink(FilePanel * Panel, bool Add);
  void Properties(FilePanel * Panel);
  void CalculateDirectorySizes(FilePanel * Panel);
  void DuplicateOrMove(bool Move);
  void EditNew(FilePanel * Panel);
  void OpenFocused();
  void OpenFile(FilePanel * Panel, const RemoteFile & File, const QString & FullPath);
  void FindFiles();
  void OpenDirectory(FilePanel * Panel, bool AddBookmark);
  void Filter(FilePanel * Panel);
  void Synchronize(bool KeepUpToDate);
  void CompareDirectories();
  void SelectByMask(FilePanel * Panel, bool Select);
  void FileNamesToClipboard(bool FullPath);
  void CopyToClipboard();
  void PasteFromClipboard();
  void GenerateSessionUrl();
  void GenerateFileUrl();
  void ClearCaches();
  void ToggleShowHiddenFiles();
  void ApplyConfiguration();
  void SortBy(FilePanel * Panel, int Column);
  void ToggleSortAscending(FilePanel * Panel);
  void PathFromOtherPanel(FilePanel * Panel);
  void GoToAddress();
  // Synchronized browsing
  void SynchronizeBrowsingChanged();
  void PanelDirectoryChanged(SessionTab * Tab, FilePanel * Panel);
  // TCustomScpExplorerForm::TransferPresetAutoSelect (transfer settings preset rules)
  void TransferPresetAutoSelect(SessionTab * Tab);
  void SynchronizeBrowsingFailed(SessionTab * Tab, const QString & Path, bool CreateRemote, const QString & More);
  // Context menus (NonVisual.dfm popups)
  void ShowFilePopup(FilePanel * Panel, const QPoint & GlobalPos, bool OnItem);
  void ShowPanelPopup(FilePanel * Panel, const QPoint & GlobalPos);
  QMenu * CreateEditPopupSubmenu(QMenu * Parent, bool Bold);
  // Workspaces / custom commands
  void SaveWorkspace();
  CustomCommandContext CustomCommandsContext() const;
  void RebuildCustomCommands();
  void FillCustomCommandsMenu(QMenu * Menu, bool FileCommands);
  void OpenInPutty();
  void ShowServerInfo();
  void SaveCurrentSession();
  void OpenBookmark(const Bookmark & B);
  QString HostKeyForCode(SessionTab * Tab) const;
};
