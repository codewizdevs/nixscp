// Port of WinSCP source/forms/NonVisual.{cpp,dfm}: the action list
// (captions, 'Short|Long' hints, Commander shortcuts) and the menu builders
#include "gui/AgentAccessDialog.h"
#include "gui/TeamsDialog.h"
#include "gui/TunnelsDialog.h"
#include "gui/CloudSyncUi.h"
#include "gui/CommanderActions.h"
#include "gui/Icons.h"

#include <QAction>
#include <QMenu>
#include <QPointer>
#include <QToolBar>
#include <QToolButton>
#include <QWidget>

static const char * const ProxyTargetProperty = "NixSCPProxyTarget";

QKeySequence CommanderActions::Num(Qt::Key Key, Qt::KeyboardModifiers Modifiers)
{
  return QKeySequence(QKeyCombination(Modifiers | Qt::KeypadModifier, Key));
}

static QKeySequence K(Qt::KeyboardModifiers Modifiers, Qt::Key Key)
{
  return QKeySequence(QKeyCombination(Modifiers, Key));
}

static QKeySequence K(Qt::Key Key)
{
  return QKeySequence(QKeyCombination(Qt::NoModifier, Key));
}

CommanderActions::CommanderActions(QWidget * Window) :
  QObject(Window),
  FWindow(Window)
{
  CreateActions();
  ApplyCommanderShortcuts(false);
}

CommanderActions::~CommanderActions()
{
}

void CommanderActions::SetHint(QAction * Action, const QString & Hint)
{
  int Pos = Hint.indexOf(QLatin1Char('|'));
  QString Short = (Pos >= 0) ? Hint.left(Pos) : Hint;
  QString Long = (Pos >= 0) ? Hint.mid(Pos + 1) : Hint;
  Action->setToolTip(Short);
  Action->setStatusTip(Long);
}

QString CommanderActions::PlainCaption(const QString & Caption)
{
  QString Result = Caption;
  Result.replace(QStringLiteral("&&"), QStringLiteral("\x01"));
  Result.remove(QLatin1Char('&'));
  Result.replace(QStringLiteral("\x01"), QStringLiteral("&"));
  if (Result.endsWith(QStringLiteral("...")))
  {
    Result.chop(3);
  }
  return Result;
}

QAction * CommanderActions::Add(const char * Name, const char * Caption, const char * Hint, ShortCutScope Scope,
  const QList<QKeySequence> & ShortCuts, bool Checkable)
{
  auto * Action = new QAction(QString::fromUtf8(Caption), this);
  QString ActionName = QString::fromLatin1(Name);
  Action->setObjectName(ActionName);
  if ((Hint != nullptr) && (*Hint != '\0'))
  {
    SetHint(Action, QString::fromUtf8(Hint));
  }
  Action->setCheckable(Checkable);
  QIcon Icon = Icons::ForAction(ActionName);
  if (!Icon.isNull())
  {
    Action->setIcon(Icon);
  }
  Action->setShortcuts(ShortCuts);
  Register(ActionName, Action, Scope);
  return Action;
}

void CommanderActions::Register(const QString & Name, QAction * Action, ShortCutScope Scope)
{
  if (Action->objectName().isEmpty())
  {
    Action->setObjectName(Name);
  }
  if (!FActions.contains(Name))
  {
    FOrder.append(Name);
  }
  FActions.insert(Name, Action);
  FScopes.insert(Action, Scope);
  if (Scope != ssDisplay)
  {
    FWindow->addAction(Action);
  }
  UpdateShortCutContext(Action);
}

QAction * CommanderActions::Get(const QString & Name) const
{
  QAction * Action = FActions.value(Name);
  if (Action == nullptr)
  {
    qWarning("CommanderActions: unknown action %s", qPrintable(Name));
    if (FMissing == nullptr)
    {
      FMissing = new QAction(QStringLiteral("?"), const_cast<CommanderActions *>(this));
      FMissing->setEnabled(false);
      FMissing->setVisible(false);
    }
    Action = FMissing;
  }
  return Action;
}

bool CommanderActions::Has(const QString & Name) const
{
  return FActions.contains(Name);
}

QStringList CommanderActions::Names() const
{
  return FOrder;
}

void CommanderActions::On(const char * Name, const std::function<void()> & Func)
{
  QAction * Action = Get(QString::fromLatin1(Name));
  connect(Action, &QAction::triggered, this, [Func]() { Func(); });
}

CommanderActions::ShortCutScope CommanderActions::Scope(QAction * Action) const
{
  return FScopes.value(Action, ssDisplay);
}

void CommanderActions::SetScope(QAction * Action, ShortCutScope AScope)
{
  ShortCutScope Old = Scope(Action);
  FScopes.insert(Action, AScope);
  if ((Old == ssDisplay) && (AScope != ssDisplay))
  {
    FWindow->addAction(Action);
  }
  else if ((Old != ssDisplay) && (AScope == ssDisplay))
  {
    FWindow->removeAction(Action);
  }
  UpdateShortCutContext(Action);
}

void CommanderActions::UpdateShortCutContext(QAction * Action)
{
  // TScpCommanderForm::AllowedAction: Local/Remote actions run from the
  // keyboard only when their side is current
  ShortCutScope S = Scope(Action);
  bool Active =
    (S == ssWindow) ||
    ((S == ssLocal) && !FCurrentRemote) ||
    ((S == ssRemote) && FCurrentRemote);
  Qt::ShortcutContext Context = Active ? Qt::WindowShortcut : Qt::WidgetShortcut;
  if (Action->shortcutContext() != Context)
  {
    Action->setShortcutContext(Context);
  }
}

void CommanderActions::SetCurrentSide(bool Remote)
{
  if (FCurrentRemote == Remote)
  {
    return;
  }
  FCurrentRemote = Remote;
  for (auto It = FScopes.cbegin(); It != FScopes.cend(); ++It)
  {
    if ((It.value() == ssLocal) || (It.value() == ssRemote))
    {
      UpdateShortCutContext(It.key());
    }
  }
}

void CommanderActions::ApplyCommanderShortcuts(bool ExplorerKeyboardShortcuts)
{
  // TNonVisualDataModule::CommanderShortcuts / CloneShortcuts
  QKeySequence Copy = ExplorerKeyboardShortcuts ? K(Qt::ControlModifier, Qt::Key_K) : K(Qt::Key_F5);
  for (const char * Name : {"RemoteCopyAction", "LocalCopyAction", "LocalLocalCopyAction", "LocalOtherCopyAction"})
  {
    Get(QString::fromLatin1(Name))->setShortcut(Copy);
  }
  QKeySequence Refresh = ExplorerKeyboardShortcuts ? K(Qt::Key_F5) : K(Qt::ControlModifier, Qt::Key_R);
  Get(QStringLiteral("RemoteRefreshAction"))->setShortcut(Refresh);
  Get(QStringLiteral("LocalRefreshAction"))->setShortcut(Refresh);
  Get(QStringLiteral("RemoteFindFilesAction2"))->setShortcut(
    ExplorerKeyboardShortcuts ? K(Qt::Key_F3) : K(Qt::AltModifier, Qt::Key_F7));
  QKeySequence CtrlF4 = K(Qt::ControlModifier, Qt::Key_F4);
  for (const char * Name : {"LocalSortByExtAction2", "RemoteSortByExtAction2", "CurrentSortByExtAction"})
  {
    Get(QString::fromLatin1(Name))->setShortcut(ExplorerKeyboardShortcuts ? QKeySequence() : CtrlF4);
  }
  QList<QKeySequence> CloseTab = {K(Qt::ControlModifier | Qt::ShiftModifier, Qt::Key_D), K(Qt::ControlModifier, Qt::Key_W)};
  if (ExplorerKeyboardShortcuts)
  {
    CloseTab.append(CtrlF4);
  }
  Get(QStringLiteral("CloseTabAction"))->setShortcuts(CloseTab);
}

void CommanderActions::CreateActions()
{
  const Qt::KeyboardModifiers Ctrl = Qt::ControlModifier;
  const Qt::KeyboardModifiers Shift = Qt::ShiftModifier;
  const Qt::KeyboardModifiers Alt = Qt::AltModifier;

  // ---- Tab / Session
  Add("NewTabAction", "&New Tab", "Open new tab", ssWindow, {K(Ctrl, Qt::Key_T)});
  Add("NewRemoteTabAction", "&Remote Tab", "Open new tab with one local panel and one remote session panel", ssWindow, {K(Ctrl, Qt::Key_N)});
  Add("NewLocalTabAction", "&Local Tab", "Open new tab with two local panels", ssWindow, {K(Ctrl | Shift, Qt::Key_N)});
  Add("DefaultToNewRemoteTabAction", "&Default to Remote Tab",
    "When turned on, the New Tab command opens a new remote tab. Otherwise it opens a new local tab.", ssWindow, {}, true);
  Add("CloseTabAction", "&Close Tab", "Close the current tab", ssWindow);
  Add("DuplicateTabAction", "Du&plicate Tab", "Duplicate tab|Open new tab with the same folder", ssWindow);
  Add("RenameTabAction", "&Rename Tab", "Rename tab|Change the name of the current tab", ssWindow);
  Add("ColorMenuAction2", "C&olor", "Change color of the current tab", ssDisplay);
  Add("OpenedTabsAction", "&Opened Tabs", "Select tab|Select tab to activate", ssDisplay);
  Add("WorkspacesAction", "&Workspaces", "Open workspace", ssDisplay);
  Add("SaveWorkspaceAction", "Save Wor&kspace...", "Save workspace|Save workspace", ssWindow);
  Add("DisconnectSessionAction", "&Disconnect Session", "Disconnect the current session, but keep the tab opened", ssWindow);
  Add("ReconnectSessionAction", "&Reconnect Session", "Reconnect the current disconnected session", ssWindow, {K(Ctrl | Shift, Qt::Key_R)});
  Add("SaveCurrentSessionAction2", "&Save Session as Site...", "Save session as site|Save current session as site", ssWindow);
  Add("SavedSessionsAction2", "Si&tes", "Open site", ssDisplay);
  Add("SiteManagerAction", "Site &Manager...", "Site Manager|Opens site manager", ssWindow);
  Add("SessionGenerateUrlAction2", "Generate Session &URL/Code...", "Generate URL or code for current session", ssWindow);
  Add("FileSystemInfoAction", "Server/Protocol &Information", "Display server/protocol information", ssWindow);
  // Ctrl+Tab / Ctrl+Shift+Tab (TCustomScpExplorerForm::KeyDown: next/previous tab)
  Add("NextTabAction", "&Next Tab", "Switch to the next tab", ssWindow, {K(Ctrl, Qt::Key_Tab)});
  Add("PreviousTabAction", "&Previous Tab", "Switch to the previous tab", ssWindow, {K(Ctrl | Shift, Qt::Key_Backtab)});

  // ---- Local directory (left panel)
  Add("LocalChangePathAction2", "&Change Drive", "Allows selection of a different place (home, bookmarks, mounted volumes) for panel", ssWindow, {K(Alt, Qt::Key_F1)});
  Add("LocalOpenDirAction", "&Open Directory/Bookmark...", "Open directory/bookmark|Open specified directory or saved bookmark", ssLocal, {K(Ctrl, Qt::Key_O)});
  Add("LocalExploreDirectoryAction", "&Explore Directory", "Opens file manager with the current local directory", ssLocal, {K(Ctrl | Alt, Qt::Key_E)});
  Add("LocalParentDirAction", "&Parent Directory", "Parent directory|Go to parent directory", ssDisplay, {K(Qt::Key_Backspace)});
  Add("LocalRootDirAction", "&Root Directory", "Root directory|Go to root directory", ssLocal, {K(Ctrl, Qt::Key_Backslash)});
  Add("LocalHomeDirAction", "&Home Directory", "Home directory|Go to home directory", ssLocal, {K(Ctrl, Qt::Key_H)});
  Add("LocalOtherDirAction", "Path fro&m Other Panel", "Open the same directory as in the other panel", ssLocal, {K(Ctrl, Qt::Key_Period)});
  Add("LocalBackAction", "&Back", "", ssLocal, {K(Alt, Qt::Key_Left)});
  Add("LocalForwardAction", "&Forward", "", ssLocal, {K(Alt, Qt::Key_Right)});
  Add("LocalRefreshAction", "&Refresh", "Refresh|Refresh directory content", ssLocal);
  Add("LocalAddBookmarkAction2", "&Add Path to Bookmarks", "Add to bookmarks|Add the current directory to bookmark list", ssLocal, {K(Ctrl, Qt::Key_B)});
  Add("LocalPathToClipboardAction2", "Copy &Path to Clipboard", "Copy the current path to the clipboard", ssLocal, {K(Ctrl, Qt::Key_BracketLeft)});
  Add("LocalFilterAction", "&Filter...", "Filter|Filter displayed files", ssLocal, {K(Ctrl | Alt, Qt::Key_F)}, true);

  // ---- Remote directory (right panel)
  Add("RemoteChangePathAction2", "&Change Directory", "Allows selection of different directory for panel", ssWindow, {K(Alt, Qt::Key_F2)});
  Add("RemoteOpenDirAction", "&Open Directory/Bookmark...", "Open directory/bookmark|Open specified directory or saved bookmark", ssRemote, {K(Ctrl, Qt::Key_O)});
  Add("RemoteExploreDirectoryAction", "&Explore Directory", "Opens file manager with the current local directory", ssRemote, {K(Ctrl | Alt, Qt::Key_E)});
  Add("RemoteParentDirAction", "&Parent Directory", "Parent directory|Go to parent directory", ssDisplay, {K(Qt::Key_Backspace)});
  Add("RemoteRootDirAction", "&Root Directory", "Root directory|Go to root directory", ssRemote, {K(Ctrl, Qt::Key_Backslash)});
  Add("RemoteHomeDirAction", "&Home Directory", "Home directory|Go to home directory", ssRemote, {K(Ctrl, Qt::Key_H)});
  Add("RemoteOtherDirAction", "Path fro&m Other Panel", "Open the same directory as in the other panel", ssRemote, {K(Ctrl, Qt::Key_Period)});
  Add("RemoteBackAction", "&Back", "", ssRemote, {K(Alt, Qt::Key_Left)});
  Add("RemoteForwardAction", "&Forward", "", ssRemote, {K(Alt, Qt::Key_Right)});
  Add("RemoteRefreshAction", "&Refresh", "Refresh|Refresh directory content", ssRemote);
  Add("RemoteAddBookmarkAction2", "&Add Path to Bookmarks", "Add to bookmarks|Add the current directory to bookmark list", ssRemote, {K(Ctrl, Qt::Key_B)});
  Add("RemotePathToClipboardAction2", "Copy &Path to Clipboard", "Copy the current path to the clipboard", ssRemote, {K(Ctrl, Qt::Key_BracketRight)});
  Add("RemoteFilterAction", "&Filter...", "Filter|Filter displayed files", ssRemote, {K(Ctrl | Alt, Qt::Key_F)}, true);
  Add("RemoteFindFilesAction2", "&Find Files...", "Find files|Find files and directories", ssWindow);
  // Address bar (GoToAddressAction): Ctrl+L, Alt+D
  Add("GoToAddressAction", "Go to &Address", "Go to the path box of the current panel", ssWindow, {K(Ctrl, Qt::Key_L), K(Alt, Qt::Key_D)});
  Add("IncrementalSearchStartAction", "Incremental Search", "Start searching the files of the current panel as you type", ssWindow, {K(Ctrl, Qt::Key_F)});

  // ---- Sort
  Add("LocalSortAscendingAction2", "&Ascending", "Ascending/descending|Toggle ascending/descending sorting of files in the panel", ssDisplay, {}, true);
  Add("LocalSortByNameAction2", "By &Name", "Sort by name|Sort panel by name", ssLocal, {K(Ctrl, Qt::Key_F3)}, true);
  Add("LocalSortByExtAction2", "By &Extension", "Sort by extension|Sort panel by file name extension", ssLocal, {}, true);
  Add("LocalSortBySizeAction2", "By &Size", "Sort by size|Sort panel by file size", ssLocal, {K(Ctrl, Qt::Key_F6)}, true);
  Add("LocalSortByTypeAction2", "By &Type", "Sort by type|Sort panel by file type", ssLocal, {}, true);
  Add("LocalSortByChangedAction3", "By Date &modified", "Sort by time|Sort panel by last modification time", ssLocal, {K(Ctrl, Qt::Key_F5)}, true);
  Add("LocalSortByAttrAction2", "By A&ttributes", "Sort by attributes|Sort panel by attributes", ssLocal, {K(Ctrl, Qt::Key_F7)}, true);
  Add("RemoteSortAscendingAction2", "&Ascending", "Ascending/descending|Toggle ascending/descending sorting of files in the panel", ssDisplay, {}, true);
  Add("RemoteSortByNameAction2", "By &Name", "Sort by name|Sort panel by name", ssRemote, {K(Ctrl, Qt::Key_F3)}, true);
  Add("RemoteSortByExtAction2", "By &Extension", "Sort by extension|Sort panel by file name extension", ssRemote, {}, true);
  Add("RemoteSortBySizeAction2", "By &Size", "Sort by size|Sort panel by file size", ssRemote, {K(Ctrl, Qt::Key_F6)}, true);
  Add("RemoteSortByTypeAction2", "By &Type", "Sort by type|Sort panel by file type", ssRemote, {}, true);
  Add("RemoteSortByChangedAction3", "By Date &modified", "Sort by time|Sort panel by last modification time", ssRemote, {K(Ctrl, Qt::Key_F5)}, true);
  Add("RemoteSortByRightsAction2", "By &Permissions/Attributes", "Sort by attributes/permissions|Sort panel by attributes/permissions", ssRemote, {K(Ctrl, Qt::Key_F7)}, true);
  Add("RemoteSortByOwnerAction2", "By &Owner", "Sort by owner|Sort panel by file owner", ssRemote, {K(Ctrl, Qt::Key_F8)}, true);
  Add("RemoteSortByGroupAction2", "By &Group", "Sort by group|Sort panel by file group", ssRemote, {K(Ctrl, Qt::Key_F9)}, true);
  Add("CurrentSortAscendingAction", "&Ascending", "Ascending/descending|Toggle ascending/descending sort of current panel", ssDisplay, {}, true);
  Add("CurrentSortByNameAction", "By &Name", "Sort by name|Sort current panel by name", ssDisplay, {K(Ctrl, Qt::Key_F3)}, true);
  Add("CurrentSortByExtAction", "By &Extension", "Sort by extension|Sort current panel by file name extension", ssDisplay, {}, true);
  Add("CurrentSortBySizeAction", "By &Size", "Sort by size|Sort current panel by file size", ssDisplay, {K(Ctrl, Qt::Key_F6)}, true);
  Add("CurrentSortByTypeAction2", "By &Type", "Sort by type|Sort current panel by file type", ssDisplay, {}, true);
  Add("CurrentSortByChangedAction2", "By Date &modified", "Sort by time|Sort current panel by last modification time", ssDisplay, {K(Ctrl, Qt::Key_F5)}, true);
  Add("CurrentSortByRightsAction", "By &Permissions/Attributes", "Sort by attributes/permissions|Sort current panel by attributes/permissions", ssDisplay, {K(Ctrl, Qt::Key_F7)}, true);
  Add("CurrentSortByOwnerAction", "By &Owner", "Sort by owner|Sort current panel by file owner (remote panel only)", ssDisplay, {K(Ctrl, Qt::Key_F8)}, true);
  Add("CurrentSortByGroupAction", "By &Group", "Sort by group|Sort current panel by file group (remote panel only)", ssDisplay, {K(Ctrl, Qt::Key_F9)}, true);

  // ---- Columns
  Add("ShowHideLocalNameColumnAction2", "&Name", "Show/hide name column", ssDisplay, {}, true);
  Add("ShowHideLocalSizeColumnAction2", "&Size", "Show/hide size column", ssDisplay, {}, true);
  Add("ShowHideLocalTypeColumnAction2", "&Type", "Show/hide type column", ssDisplay, {}, true);
  Add("ShowHideLocalChangedColumnAction3", "&Date modified", "Show/hide date modified column", ssDisplay, {}, true);
  Add("ShowHideLocalAttrColumnAction2", "&Attributes", "Show/hide attributes column", ssDisplay, {}, true);
  Add("AutoSizeLocalColumnsAction", "Size &Automatically", "Adjust columns width to fit their contents", ssLocal, {Num(Qt::Key_Plus, Ctrl)});
  Add("ResetLayoutLocalColumnsAction", "&Reset Layout", "Reset to the default layout of file panel columns", ssDisplay);
  Add("ShowHideRemoteNameColumnAction2", "&Name", "Show/hide name column", ssDisplay, {}, true);
  Add("ShowHideRemoteSizeColumnAction2", "&Size", "Show/hide size column", ssDisplay, {}, true);
  Add("ShowHideRemoteTypeColumnAction2", "&Type", "Show/hide type column", ssDisplay, {}, true);
  Add("ShowHideRemoteChangedColumnAction3", "&Date modified", "Show/hide date modified column", ssDisplay, {}, true);
  Add("ShowHideRemoteRightsColumnAction2", "&Permissions", "Show/hide permissions column", ssDisplay, {}, true);
  Add("ShowHideRemoteOwnerColumnAction2", "&Owner", "Show/hide owner column", ssDisplay, {}, true);
  Add("ShowHideRemoteGroupColumnAction2", "&Group", "Show/hide group column", ssDisplay, {}, true);
  Add("ShowHideRemoteLinkTargetColumnAction2", "&Link Target", "Show/hide link target column", ssDisplay, {}, true);
  Add("AutoSizeRemoteColumnsAction", "Size &Automatically", "Adjust columns width to fit their contents", ssRemote, {Num(Qt::Key_Plus, Ctrl)});
  Add("ResetLayoutRemoteColumnsAction", "&Reset Layout", "Reset to the default layout of file panel columns", ssDisplay);

  // ---- Selection (Mark menu)
  Add("SelectOneAction", "&Select/Unselect", "Select|Select/unselect focused file", ssDisplay, {K(Qt::Key_Insert)});
  Add("SelectAction", "Sele&ct Files...", "Select|Select files by mask", ssWindow, {Num(Qt::Key_Plus)});
  Add("UnselectAction", "&Unselect Files...", "Unselect|Unselect files by mask", ssWindow, {Num(Qt::Key_Minus)});
  Add("SelectAllAction", "Select &All", "Select all files", ssWindow, {K(Ctrl, Qt::Key_A)});
  Add("InvertSelectionAction", "&Invert Selection", "Invert selection", ssWindow, {Num(Qt::Key_Asterisk)});
  Add("ClearSelectionAction", "C&lear Selection", "Clear selection", ssWindow, {K(Ctrl | Shift, Qt::Key_L)});
  Add("RestoreSelectionAction", "&Restore Selection", "Restore previous selection", ssWindow, {K(Ctrl | Shift, Qt::Key_S)});
  Add("SelectSameExtAction", "Select Files with Same &Extension", "Select all files with the same extension as currently focused file", ssWindow, {Num(Qt::Key_Plus, Alt)});
  Add("UnselectSameExtAction", "Unselect Files with Same E&xtension", "Unselect all files with the same extension as currently focused file", ssWindow, {Num(Qt::Key_Minus, Alt)});
  Add("LocalSelectAction2", "Sele&ct Files...", "Select|Select files by mask", ssDisplay);
  Add("LocalUnselectAction2", "&Unselect Files...", "Unselect|Unselect files by mask", ssDisplay);
  Add("LocalSelectAllAction2", "Select &All", "Select all files", ssDisplay);
  Add("RemoteSelectAction2", "Sele&ct Files...", "Select|Select files by mask", ssDisplay);
  Add("RemoteUnselectAction2", "&Unselect Files...", "Unselect|Unselect files by mask", ssDisplay);
  Add("RemoteSelectAllAction2", "Select &All", "Select all files", ssDisplay);

  // ---- Files
  Add("NewFileAction", "&File...", "Create file|Create new file and open it in editor", ssWindow, {K(Shift, Qt::Key_F4), K(Ctrl | Shift, Qt::Key_F4)});
  Add("NewDirAction", "&Directory...", "Create directory|Create new directory", ssDisplay, {K(Qt::Key_F7)});
  Add("NewLinkAction", "&Link...", "Create link|Create new link/shortcut", ssDisplay, {K(Alt, Qt::Key_F6)});
  Add("CurrentOpenAction", "&Open", "Open document|Open selected document using application associated with document type", ssWindow);
  Add("CurrentAddEditLinkAction", "Edit &Link...", "Add/edit link|Add new link/shortcut or edit selected link/shortcut", ssWindow, {K(Alt, Qt::Key_F6)});
  Add("CurrentAddEditLinkContextAction", "Edit &Link...", "Edit link|Edit selected link/shortcut", ssDisplay, {K(Alt, Qt::Key_F6)});
  Add("CurrentCreateDirAction", "&Create Directory...", "Create directory|Create new directory", ssWindow, {K(Qt::Key_F7)});
  Add("CurrentRenameAction", "&Rename", "Rename|Rename selected file", ssWindow, {K(Qt::Key_F2)});
  Add("CurrentDeleteAction", "&Delete", "Delete|Delete selected file(s)", ssWindow, {K(Qt::Key_F8), K(Qt::Key_Delete)});
  Add("CurrentDeleteAlternativeAction", "&Delete", "Delete|Delete selected file(s)", ssWindow, {K(Shift, Qt::Key_F8), K(Shift, Qt::Key_Delete)});
  Add("CurrentPropertiesAction", "&Properties", "Properties|Display/change properties of selected file(s)", ssWindow, {K(Qt::Key_F9)});
  Add("CurrentCopyToClipboardAction2", "&Copy to Clipboard", "Copy the selected files to the clipboard", ssWindow, {K(Ctrl, Qt::Key_C)});
  Add("PasteAction3", "&Paste from Clipboard",
    "Paste files from the clipboard to the current directory in the active panel; or open a path from the clipboard in the active panel; or open a session URL from the clipboard",
    ssWindow, {K(Ctrl, Qt::Key_V)});
  Add("CalculateDirectorySizesAction", "&Calculate Directory Sizes", "Calculate sizes of the selected directories and display them in the file panel", ssWindow, {K(Alt | Shift, Qt::Key_Return), K(Alt | Shift, Qt::Key_Enter)});
  Add("CustomCommandsFileAction", "&Custom Commands", "Execute custom commands with selected file(s)", ssDisplay);
  Add("FileListToClipboardAction", "Copy to &Clipboard", "Copy name(s) of selected file(s) to clipboard", ssWindow, {K(Ctrl | Shift, Qt::Key_C)});
  Add("FullFileListToClipboardAction", "Copy to Clipboard (Include &Paths)", "Copy name(s) including path of selected file(s) to clipboard", ssWindow, {K(Ctrl | Alt, Qt::Key_C)});
  Add("FileGenerateUrlAction2", "Generate File &URL...", "Generate URL's of selected file(s)", ssWindow);
  // Upload / download (local / remote panel) and local-local copy
  Add("LocalCopyAction", "Up&load...", "Upload|Upload selected local file(s) to remote directory", ssLocal);
  Add("LocalCopyNonQueueAction", "Up&load...", "Upload|Upload selected local file(s) to remote directory", ssDisplay);
  Add("LocalCopyQueueAction", "Upload in &Background...", "Upload selected local file(s) to remote directory in background", ssDisplay);
  Add("LocalMoveAction", "Upload and Dele&te...", "Upload and Delete|Upload selected local file(s) to remote directory and delete original", ssLocal, {K(Qt::Key_F6)});
  Add("RemoteCopyAction", "Down&load...", "Download|Download selected remote file(s) to local directory", ssRemote);
  Add("RemoteCopyNonQueueAction", "Down&load...", "Download|Download selected remote file(s) to local directory", ssDisplay);
  Add("RemoteCopyQueueAction", "Download in &Background...", "Download selected remote file(s) to local directory in background", ssDisplay);
  Add("RemoteMoveAction", "Download and Dele&te...", "Download and Delete|Download selected remote file(s) to local directory and delete original", ssRemote, {K(Qt::Key_F6)});
  Add("LocalLocalCopyAction", "&Copy...", "Copy the selected file(s) to another directory or another name", ssLocal);
  Add("LocalLocalMoveAction", "&Move...", "Move the selected file(s) to another directory or rename them", ssLocal, {K(Qt::Key_F6)});
  Add("LocalOtherCopyAction", "&Copy...", "Copy the selected file(s) to another directory or another name", ssRemote);
  Add("LocalOtherMoveAction", "&Move...", "Move the selected file(s) to another directory or rename them", ssRemote, {K(Qt::Key_F6)});
  Add("RemoteCopyToAction", "Du&plicate...", "Duplicate|Duplicate selected remote file(s) to another remote directory or another name", ssWindow, {K(Shift, Qt::Key_F5)});
  Add("RemoteMoveToAction", "Mo&ve To...", "Move|Move selected remote file(s) to another remote directory or another name", ssWindow, {K(Shift, Qt::Key_F6)});
  // Panel File toolbars (they act on their own side)
  Add("LocalEditAction2", "&Edit", "Edit|Edit selected file(s)", ssDisplay, {K(Qt::Key_F4)});
  Add("LocalDeleteAction2", "&Delete", "Delete|Delete selected file(s)", ssDisplay, {K(Qt::Key_F8)});
  Add("LocalRenameAction2", "&Rename", "Rename|Rename selected file", ssDisplay, {K(Qt::Key_F2)});
  Add("LocalPropertiesAction2", "&Properties", "Properties|Display/change properties of selected file(s)", ssDisplay, {K(Qt::Key_F9)});
  Add("LocalCalculateDirectorySizesAction", "&Calculate Directory Sizes", "Calculate sizes of the selected directories and display them in the file panel", ssDisplay, {K(Alt | Shift, Qt::Key_Return)});
  Add("LocalNewFileAction", "&File...", "Create file|Create new file and open it in editor", ssDisplay, {K(Shift, Qt::Key_F4)});
  Add("LocalCreateDirAction3", "&Directory...", "Create directory|Create new directory", ssDisplay, {K(Qt::Key_F7)});
  // Windows shortcut (.lnk) -> symbolic link on Linux
  Add("LocalAddEditLinkAction3", "&Link...", "Add/edit link|Add new link or edit selected link", ssDisplay, {K(Alt, Qt::Key_F6)});
  Add("RemoteEditAction2", "&Edit", "Edit|Edit selected file(s)", ssDisplay, {K(Qt::Key_F4)});
  Add("RemoteDeleteAction2", "&Delete", "Delete|Delete selected file(s)", ssDisplay, {K(Qt::Key_F8)});
  Add("RemoteRenameAction2", "&Rename", "Rename|Rename selected file", ssDisplay, {K(Qt::Key_F2)});
  Add("RemotePropertiesAction2", "&Properties", "Properties|Display/change properties of selected file(s)", ssDisplay, {K(Qt::Key_F9)});
  Add("RemoteCalculateDirectorySizesAction", "&Calculate Directory Sizes", "Calculate sizes of the selected directories and display them in the file panel", ssDisplay, {K(Alt | Shift, Qt::Key_Return)});
  Add("RemoteNewFileAction", "&File...", "Create file|Create new file and open it in editor", ssDisplay, {K(Shift, Qt::Key_F4)});
  Add("RemoteCreateDirAction3", "&Directory...", "Create directory|Create new directory", ssDisplay, {K(Qt::Key_F7)});
  Add("RemoteAddEditLinkAction3", "&Link...", "Add/edit link|Add new link/shortcut or edit selected link/shortcut", ssDisplay, {K(Alt, Qt::Key_F6)});

  // ---- Commands
  Add("CompareDirectoriesAction2", "&Compare Directories", "Compare directories|Mark different files between files panels", ssWindow, {K(Shift, Qt::Key_F2)});
  Add("SynchronizeAction", "&Keep Remote Directory up to Date...", "Keep remote directory up to date|Keep remote directory up to date", ssWindow, {K(Ctrl, Qt::Key_U)});
  Add("FullSynchronizeAction2", "&Synchronize...", "Synchronize files between two directories", ssWindow, {K(Ctrl, Qt::Key_S)});
  Add("SynchronizeBrowsingAction2", "Synchronize &Browsing", "Synchronize browsing|Synchronize browsing between both panels", ssWindow, {K(Ctrl | Alt, Qt::Key_B)}, true);
  Add("CustomCommandsNonFileAction", "Static &Custom Commands", "Execute custom commands that do not operate with files", ssDisplay);
  Add("CustomCommandsEnterAction", "&Enter...", "Enter ad hoc custom command", ssWindow);
  Add("CustomCommandsCustomizeAction", "&Customize...", "Customize custom commands", ssWindow);
  Add("ConsoleAction", "Open &Terminal", "Open terminal|Open terminal window that allow executing arbitrary command (with exception of commands that require user input)", ssWindow, {K(Ctrl | Shift, Qt::Key_T)});
  // PuTTY: the terminal client of Preferences > Integration > Applications (OpenSSH in a terminal emulator by default)
  Add("PuttyAction", "Open in &PuTTY", "Open session in PuTTY|Execute the terminal client (PuTTY/terminal client path of Preferences) and open the current session with it", ssWindow, {K(Ctrl, Qt::Key_P)});
  Add("ClearCachesAction", "Clea&r Caches", "Clear directory listing and directory changes caches", ssWindow);
  // NixSCP Agent Access (gui/AgentAccessDialog.h)
  Add("TunnelsAction", "&Manage Tunnels...", "Forward ports through the SSH connection of your sites (start, stop, edit)", ssWindow);
  Add("AgentAccessLogAction", "A&gent Access Log...", "Show which programs used your sites through Agent Access", ssWindow);
  Add("AgentSessionsAction", "Agent &Sessions...", "Show the recorded agent sessions (commands and their output)", ssWindow);
  Add("AgentAccessHelpAction", "Agent Access &Help", "How AI agents and other programs use your sites without their passwords", ssWindow);
  Add("CloseApplicationAction2", "&Quit", "Exit application|Close application (any opened sessions are closed)", ssWindow, {K(Qt::Key_F10)});

  // ---- View / Options
  Add("PreferencesAction", "&Preferences...", "Preferences|Show/change user preferences", ssWindow, {K(Ctrl | Alt, Qt::Key_P)});
  Add("PresetsPreferencesAction", "&Configure...", "Configure transfers settings presets", ssWindow);
  Add("ShowHiddenFilesAction", "Show/Hide &Hidden Files", "Toggle showing hidden files in panel(s)", ssWindow, {K(Ctrl | Alt, Qt::Key_H)}, true);
  Add("AutoReadDirectoryAfterOpAction", "Auto&matically Reload Directory", "Toggle automatic reload of remote directory after operation", ssWindow, {K(Ctrl | Alt, Qt::Key_R)}, true);
  Add("CommanderCommandsBandAction", "&Commands Buttons", "Hide/show commands toolbar", ssDisplay, {}, true);
  Add("CommanderSessionBandAction2", "Sessio&ns and Tabs Buttons", "Hide/show sessions and tabs toolbar", ssDisplay, {}, true);
  Add("CommanderPreferencesBandAction", "&Preferences Buttons", "Hide/show preferences toolbar", ssDisplay, {}, true);
  Add("CommanderSortBandAction", "So&rt Buttons", "Hide/show sort toolbar", ssDisplay, {}, true);
  Add("CommanderTransferBandAction", "&Transfer Settings", "Hide/show transfer settings toolbar", ssDisplay, {}, true);
  Add("CommanderCustomCommandsBandAction", "Custom Co&mmand Buttons", "Hide/show custom commands toolbar", ssDisplay, {}, true);
  Add("ToolBar2Action", "&Hot Keys Toolbar", "Hide/show bottom hot keys toolbar", ssDisplay, {}, true);
  Add("LockToolbarsAction", "&Lock Toolbars", "Prevents moving and (un)docking of all toolbars", ssDisplay, {}, true);
  Add("SelectiveToolbarTextAction", "&Show Selective Text Labels", "Show text labels for selected important commands on toolbars", ssDisplay, {}, true);
  Add("ToolbarIconSizeAction", "T&oolbar Icons Size", "Change toolbar icons size", ssDisplay);
  Add("ToolbarIconSizeNormalAction", "&Normal", "Show normal toolbar icons", ssDisplay, {}, true);
  Add("ToolbarIconSizeLargeAction", "&Large", "Show large toolbar icons", ssDisplay, {}, true);
  Add("ToolbarIconSizeVeryLargeAction", "&Very Large", "Show very large toolbar icons", ssDisplay, {}, true);
  Add("CommanderLocalPanelAction", "&Left Panel", "Change left panel layout", ssDisplay);
  Add("CommanderLocalHistoryBandAction2", "&History Buttons", "Hide/show history toolbar", ssDisplay, {}, true);
  Add("CommanderLocalNavigationBandAction2", "&Navigation Buttons", "Hide/show navigation toolbar", ssDisplay, {}, true);
  Add("CommanderLocalFileBandAction2", "&File Buttons", "Hide/show file toolbar", ssDisplay, {}, true);
  Add("CommanderLocalSelectionBandAction2", "Se&lection Buttons", "Hide/show selection toolbar", ssDisplay, {}, true);
  Add("LocalStatusBarAction2", "Status &Bar", "Hide/show panel status bar", ssDisplay, {}, true);
  Add("CommanderRemotePanelAction", "&Right Panel", "Change right panel layout", ssDisplay);
  Add("CommanderRemoteHistoryBandAction2", "&History Buttons", "Hide/show history toolbar", ssDisplay, {}, true);
  Add("CommanderRemoteNavigationBandAction2", "&Navigation Buttons", "Hide/show navigation toolbar", ssDisplay, {}, true);
  Add("CommanderRemoteFileBandAction2", "&File Buttons", "Hide/show file toolbar", ssDisplay, {}, true);
  Add("CommanderRemoteSelectionBandAction2", "Se&lection Buttons", "Hide/show selection toolbar", ssDisplay, {}, true);
  Add("RemoteStatusBarAction2", "Status &Bar", "Hide/show panel status bar", ssDisplay, {}, true);
  Add("SessionsTabsAction2", "T&abs", "Hide/show tabs", ssDisplay, {}, true);
  Add("StatusBarAction", "Status &Bar", "Hide/show status bar", ssDisplay, {}, true);
}

QAction * CommanderActions::CreateProxy(QObject * Parent, QAction * Target)
{
  auto * Proxy = new QAction(Parent);
  connect(Proxy, &QAction::triggered, Proxy, [Proxy]()
  {
    if (QAction * T = ProxyTarget(Proxy))
    {
      if (T->isEnabled())
      {
        T->trigger();
      }
    }
  });
  SetProxyTarget(Proxy, Target);
  return Proxy;
}

QAction * CommanderActions::ProxyTarget(QAction * Proxy)
{
  return qobject_cast<QAction *>(Proxy->property(ProxyTargetProperty).value<QObject *>());
}

void CommanderActions::SetProxyTarget(QAction * Proxy, QAction * Target)
{
  QAction * Old = ProxyTarget(Proxy);
  if ((Old == Target) && (Old != nullptr))
  {
    return;
  }
  if (Old != nullptr)
  {
    QObject::disconnect(Old, nullptr, Proxy, nullptr);
  }
  Proxy->setProperty(ProxyTargetProperty, QVariant::fromValue<QObject *>(Target));
  if (Target == nullptr)
  {
    Proxy->setEnabled(false);
    return;
  }
  auto Sync = [Proxy, Target]()
  {
    Proxy->setText(Target->text());
    Proxy->setIcon(Target->icon());
    Proxy->setToolTip(Target->toolTip());
    Proxy->setStatusTip(Target->statusTip());
    Proxy->setEnabled(Target->isEnabled());
    Proxy->setCheckable(Target->isCheckable());
    Proxy->setChecked(Target->isChecked());
    // Shows the key only; the target handles it
    Proxy->setShortcutContext(Qt::WidgetShortcut);
    Proxy->setShortcuts(Target->shortcuts());
  };
  QObject::connect(Target, &QAction::changed, Proxy, Sync);
  Sync();
}

void CommanderActions::Populate(QMenu * Menu, const QList<MenuItem> & Items) const
{
  for (const MenuItem & Item : Items)
  {
    if (Item.DirectMenu != nullptr)
    {
      Menu->addMenu(Item.DirectMenu);
    }
    else if (Item.Direct != nullptr)
    {
      Menu->addAction(Item.Direct);
    }
    else if (Item.Action == QStringLiteral("-"))
    {
      Menu->addSeparator();
    }
    else if (!Item.Caption.isEmpty() || !Item.Items.isEmpty() || (Item.Created != nullptr))
    {
      QMenu * Sub;
      if (!Item.Action.isEmpty())
      {
        QAction * Action = Get(Item.Action);
        Sub = Menu->addMenu(Action->icon(), Action->text());
        Sub->menuAction()->setStatusTip(Action->statusTip());
        Sub->menuAction()->setToolTip(Action->toolTip());
        Sub->setObjectName(Item.Action + QStringLiteral("Menu"));
      }
      else
      {
        Sub = Menu->addMenu(Item.Caption);
      }
      Sub->setToolTipsVisible(true);
      Populate(Sub, Item.Items);
      if (Item.Created != nullptr)
      {
        *Item.Created = Sub;
      }
    }
    else
    {
      Menu->addAction(Get(Item.Action));
    }
  }
}

void CommanderActions::AddDropDownButton(QToolBar * Bar, QAction * Action, QMenu * Menu)
{
  auto * Button = new QToolButton(Bar);
  Button->setDefaultAction(Action);
  Button->setMenu(Menu);
  Button->setPopupMode(QToolButton::MenuButtonPopup);
  Button->setToolButtonStyle(Bar->toolButtonStyle());
  Button->setIconSize(Bar->iconSize());
  Button->setAutoRaise(true);
  QObject::connect(Bar, &QToolBar::toolButtonStyleChanged, Button, &QToolButton::setToolButtonStyle);
  QObject::connect(Bar, &QToolBar::iconSizeChanged, Button, &QToolButton::setIconSize);
  Bar->addWidget(Button);
}

//===========================================================================
// CommanderWindow: actions, menus, toolbars and popups
// (forms/NonVisual.cpp TNonVisualDataModule::ExplorerActionsUpdate/Execute,
// forms/ScpCommander.dfm menus and toolbars, ScpCommander.cpp UpdateControls)
//===========================================================================

#include "gui/CommanderWindow.h"
#include "gui/ClipboardTransfer.h"
#include "gui/DesktopIntegration.h"
#include "gui/EditorManager.h"
#include "gui/FileListModel.h"
#include "gui/GuiUserInterface.h"
#include "gui/HelpSystem.h"
#include "gui/LocationProfilesDialog.h"
#include "gui/PreferencesDialog.h"
#include "gui/CopyParamsDialog.h"
#include "gui/ConsoleDialog.h"
#include "gui/CustomCommandRunner.h"
#include "gui/QueuePanel.h"
#include "core/Configuration.h"
#include "core/CopyParamList.h"
#include "core/IniStorage.h"
#include "core/Queue.h"
#include "core/Terminal.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QKeyEvent>
#include <QLabel>
#include <QMainWindow>
#include <QMenuBar>
#include <QMouseEvent>
#include <QScopedValueRollback>
#include <QSplitter>
#include <QStatusBar>
#include <QTabBar>
#include <QTimer>

using CA = CommanderActions;

// Panel band indexes (FBandVisible)
enum { bandHistory, bandNavigation, bandFile, bandSelection };

static const char * const PanelToolbarProperty = "NixSCPPanelToolbar";

void CommanderWindow::CreateActions()
{
  // Editors (EditorManager owns CurrentEditAction F4, CurrentEditInternalAction
  // Ctrl+Alt+F4 and CurrentEditWithAction as window shortcuts)
  // (window scope: ssDisplay would demote their shortcut context to the
  // widget, so F4 / Ctrl+Alt+F4 would never fire)
  FA->Register(QStringLiteral("CurrentEditAction"), FEditorManager->EditAction(), CA::ssWindow);
  FA->Register(QStringLiteral("CurrentEditInternalAction"), FEditorManager->EditInternalAction(), CA::ssWindow);
  FA->Register(QStringLiteral("CurrentEditWithAction"), FEditorManager->EditWithAction(), CA::ssDisplay);
  FA->Register(QStringLiteral("EditorListCustomizeAction"), FEditorManager->EditorConfigureAction(), CA::ssDisplay);
  // Queue (QueuePanel installs Ctrl+Q / Ctrl+Shift+Q itself)
  const struct { QueuePanel::QueueAction Id; const char * Name; } QueueActions[] = {
    {QueuePanel::QueueAction::ToggleShow, "QueueToggleShowAction"},
    {QueuePanel::QueueAction::Show, "QueueShowAction"},
    {QueuePanel::QueueAction::HideWhenEmpty, "QueueHideWhenEmptyAction"},
    {QueuePanel::QueueAction::Hide, "QueueHideAction"},
    {QueuePanel::QueueAction::Toolbar, "QueueToolbarAction"},
    {QueuePanel::QueueAction::FileList, "QueueFileListAction"},
    {QueuePanel::QueueAction::ResetLayoutColumns, "QueueResetLayoutColumnsAction"},
    {QueuePanel::QueueAction::CycleOnceEmpty, "QueueCycleOnceEmptyAction"},
    {QueuePanel::QueueAction::Preferences, "QueuePreferencesAction"},
  };
  for (const auto & Q : QueueActions)
  {
    FA->Register(QString::fromLatin1(Q.Name), FQueuePanel->Action(Q.Id), CA::ssDisplay);
  }
  // Opened tabs: Alt+1..Alt+9, Alt+0 (TNonVisualDataModule::OpenSessionShortCut)
  for (int Index = 0; Index < 10; Index++)
  {
    auto * Action = new QAction(this);
    Action->setCheckable(true);
    Action->setShortcut(QKeySequence(QKeyCombination(Qt::AltModifier, static_cast<Qt::Key>((Index < 9) ? (Qt::Key_1 + Index) : Qt::Key_0))));
    Action->setShortcutContext(Qt::WindowShortcut);
    connect(Action, &QAction::triggered, this, [this, Index]()
    {
      if (Index < FTabs.size()) ActivateTab(FTabs[Index]);
    });
    addAction(Action);
    FSelectTabActions.append(Action);
  }

  // Items whose action follows the active side (ScpCommander.cpp UpdateControls)
  FCurrentCopyItem = FA->CreateProxy(this, (*FA)("RemoteCopyAction"));
  FCurrentCopyNonQueueItem = FA->CreateProxy(this, (*FA)("RemoteCopyNonQueueAction"));
  FCurrentCopyQueueItem = FA->CreateProxy(this, (*FA)("RemoteCopyQueueAction"));
  FCurrentMoveItem = FA->CreateProxy(this, (*FA)("RemoteMoveAction"));
  FCurrentCopyToItem = FA->CreateProxy(this, (*FA)("RemoteCopyToAction"));
  FCurrentMoveToItem = FA->CreateProxy(this, (*FA)("RemoteMoveToAction"));
  FLocalCopyItem = FA->CreateProxy(this, (*FA)("LocalCopyAction"));
  FLocalMoveItem = FA->CreateProxy(this, (*FA)("LocalMoveAction"));
  FRemoteCopyItem = FA->CreateProxy(this, (*FA)("RemoteCopyAction"));
  FRemoteMoveItem = FA->CreateProxy(this, (*FA)("RemoteMoveAction"));
  FHotCopyItem = FA->CreateProxy(this, (*FA)("RemoteCopyAction"));
  FHotMoveItem = FA->CreateProxy(this, (*FA)("RemoteMoveAction"));

  const Configuration & Config = Configuration::Instance();
  (*FA)("ShowHiddenFilesAction")->setChecked(Config.ShowHiddenFiles);
  (*FA)("AutoReadDirectoryAfterOpAction")->setChecked(Config.AutoReadDirectoryAfterOp);
}

void CommanderWindow::ConnectActions()
{
  CommanderActions & A = *FA;
  // ---- Tabs / sessions
  A.On("NewTabAction", [this]() { NewTabFromCurrent(false); });
  A.On("NewRemoteTabAction", [this]() { NewTab(true); });
  A.On("NewLocalTabAction", [this]() { NewTab(false); });
  connect(A("DefaultToNewRemoteTabAction"), &QAction::toggled, this, [this](bool Checked)
  {
    // Configuration\Interface\DefaultToNewRemoteTab
    IniStorage Storage(&Configuration::Instance().Ini());
    if (Storage.OpenSubKeyPath(QStringLiteral("Configuration\\Interface"), true))
    {
      Storage.WriteBool(QStringLiteral("DefaultToNewRemoteTab"), Checked);
      Storage.CloseSubKeyPath();
    }
    (*FA)("NewTabAction")->setIcon(Icons::ForAction(Checked ? QStringLiteral("NewRemoteTabAction") : QStringLiteral("NewLocalTabAction")));
  });
  A.On("CloseTabAction", [this]() { CloseTab(CurrentTab()); });
  A.On("DuplicateTabAction", [this]() { DuplicateTab(); });
  A.On("RenameTabAction", [this]() { RenameTab(); });
  A.On("SaveWorkspaceAction", [this]() { SaveWorkspace(); });
  A.On("DisconnectSessionAction", [this]()
  {
    // TCustomScpExplorerForm::DisconnectSession (CanCloseQueue)
    SessionTab * Tab = CurrentTab();
    if (Connected(Tab) && CanCloseQueue(Tab)) DisconnectTab(Tab);
  });
  A.On("SaveCurrentSessionAction2", [this]() { SaveCurrentSession(); });
  A.On("SiteManagerAction", [this]() { NewSession(); });
  A.On("SessionGenerateUrlAction2", [this]() { GenerateSessionUrl(); });
  A.On("FileSystemInfoAction", [this]() { ShowServerInfo(); });
  A.On("NextTabAction", [this]()
  {
    if (FTabs.size() > 1) ActivateTab(FTabs[(TabIndex(CurrentTab()) + 1) % FTabs.size()]);
  });
  A.On("PreviousTabAction", [this]()
  {
    if (FTabs.size() > 1) ActivateTab(FTabs[(TabIndex(CurrentTab()) + FTabs.size() - 1) % FTabs.size()]);
  });

  // ---- Directories of both sides (Local = left panel, Remote = right panel)
  for (bool Remote : {false, true})
  {
    auto P = [this, Remote]() { return SidePanel(Remote); };
    auto N = [Remote](const char * Suffix) { return QString::fromLatin1(Remote ? "Remote" : "Local") + QString::fromLatin1(Suffix); };
    auto On = [this, &N](const char * Suffix, const std::function<void()> & Func)
    {
      connect(FA->Get(N(Suffix)), &QAction::triggered, this, Func);
    };
    On("OpenDirAction", [this, P]() { OpenDirectory(P(), false); });
    On("ParentDirAction", [this, P]() { if (PanelReady(P())) P()->ParentDirectory(); });
    On("RootDirAction", [this, P]() { if (PanelReady(P())) P()->RootDirectory(); });
    On("HomeDirAction", [this, P]() { if (PanelReady(P())) P()->HomeDirectory(); });
    On("OtherDirAction", [this, P]() { PathFromOtherPanel(P()); });
    On("BackAction", [this, P]() { if (PanelReady(P())) P()->Back(); });
    On("ForwardAction", [this, P]() { if (PanelReady(P())) P()->Forward(); });
    On("RefreshAction", [this, P]() { if (PanelReady(P())) P()->Refresh(); });
    On("AddBookmarkAction2", [this, P]() { OpenDirectory(P(), true); });
    On("PathToClipboardAction2", [this, P]() { if (PanelReady(P())) QApplication::clipboard()->setText(P()->CurrentDirectory()); });
    On("FilterAction", [this, P]() { Filter(P()); });
    On("ExploreDirectoryAction", [this, P, Remote]()
    {
      // Opens the file manager (Windows Explorer in WinSCP) with the local directory
      FilePanel * Panel = (Remote && !P()->IsRemote()) ? P() : LocalPanel();
      DesktopIntegration::ExploreDirectory(Panel->CurrentDirectory());
    });
    On("SortAscendingAction2", [this, P]() { ToggleSortAscending(P()); });
    On("SelectAction2", [this, P]() { SelectByMask(P(), true); });
    On("UnselectAction2", [this, P]() { SelectByMask(P(), false); });
    On("SelectAllAction2", [this, P]() { if (PanelReady(P())) P()->SelectAll(); });
    On("EditAction2", [this, P]()
    {
      FilePanel * Panel = P();
      if (!PanelReady(Panel)) return;
      QList<RemoteFile> Files = Panel->SelectedFiles();
      QStringList Paths = Panel->SelectedPaths();
      QStringList Edit;
      for (int I = 0; (I < Files.size()) && (I < Paths.size()); I++)
      {
        if (!Files[I].IsDirectory) Edit.append(Paths[I]);
      }
      if (!Edit.isEmpty()) FEditorManager->ExecuteFiles(PanelTerminal(Panel), Edit, EditorManager::efDefaultEditor);
    });
    On("DeleteAction2", [this, P]() { DeleteFiles(P(), false); });
    On("RenameAction2", [this, P]() { if (PanelReady(P())) { P()->FocusView(); P()->StartRename(); } });
    On("PropertiesAction2", [this, P]() { Properties(P()); });
    On("CalculateDirectorySizesAction", [this, P]() { CalculateDirectorySizes(P()); });
    On("NewFileAction", [this, P]() { EditNew(P()); });
    On("CreateDirAction3", [this, P]() { CreateDirectory(P()); });
    On("AddEditLinkAction3", [this, P]() { AddEditLink(P(), false); });
    On("StatusBarAction2", [this, Remote]()
    {
      bool Visible = FA->Get(QString::fromLatin1(Remote ? "RemoteStatusBarAction2" : "LocalStatusBarAction2"))->isChecked();
      for (SessionTab * Tab : FTabs)
      {
        (Remote ? Tab->RemotePanel : Tab->LocalPanel)->SetStatusBarVisible(Visible);
      }
    });
    Q_UNUSED(P);
  }
  // Sort and column actions of each side
  const struct { const char * Local; const char * Remote; int Column; } SortActions[] = {
    {"LocalSortByNameAction2", "RemoteSortByNameAction2", FileListModel::ColName},
    {"LocalSortByExtAction2", "RemoteSortByExtAction2", FileListModel::ColExt},
    {"LocalSortBySizeAction2", "RemoteSortBySizeAction2", FileListModel::ColSize},
    {"LocalSortByTypeAction2", "RemoteSortByTypeAction2", FileListModel::ColType},
    {"LocalSortByChangedAction3", "RemoteSortByChangedAction3", FileListModel::ColChanged},
    {"LocalSortByAttrAction2", "RemoteSortByRightsAction2", FileListModel::ColRights},
    {nullptr, "RemoteSortByOwnerAction2", FileListModel::ColOwner},
    {nullptr, "RemoteSortByGroupAction2", FileListModel::ColGroup},
  };
  for (const auto & S : SortActions)
  {
    int Column = S.Column;
    if (S.Local != nullptr)
    {
      A.On(S.Local, [this, Column]() { SortBy(SidePanel(false), Column); });
    }
    A.On(S.Remote, [this, Column]() { SortBy(SidePanel(true), Column); });
  }
  const struct { const char * Name; int Column; } CurrentSortActions[] = {
    {"CurrentSortByNameAction", FileListModel::ColName}, {"CurrentSortByExtAction", FileListModel::ColExt},
    {"CurrentSortBySizeAction", FileListModel::ColSize}, {"CurrentSortByTypeAction2", FileListModel::ColType},
    {"CurrentSortByChangedAction2", FileListModel::ColChanged}, {"CurrentSortByRightsAction", FileListModel::ColRights},
    {"CurrentSortByOwnerAction", FileListModel::ColOwner}, {"CurrentSortByGroupAction", FileListModel::ColGroup},
  };
  for (const auto & S : CurrentSortActions)
  {
    int Column = S.Column;
    A.On(S.Name, [this, Column]() { SortBy(ActivePanel(), Column); });
  }
  A.On("CurrentSortAscendingAction", [this]() { ToggleSortAscending(ActivePanel()); });
  const struct { const char * Name; bool Remote; int Column; } ColumnActions[] = {
    {"ShowHideLocalNameColumnAction2", false, FileListModel::ColName},
    {"ShowHideLocalSizeColumnAction2", false, FileListModel::ColSize},
    {"ShowHideLocalTypeColumnAction2", false, FileListModel::ColType},
    {"ShowHideLocalChangedColumnAction3", false, FileListModel::ColChanged},
    {"ShowHideLocalAttrColumnAction2", false, FileListModel::ColRights},
    {"ShowHideRemoteNameColumnAction2", true, FileListModel::ColName},
    {"ShowHideRemoteSizeColumnAction2", true, FileListModel::ColSize},
    {"ShowHideRemoteTypeColumnAction2", true, FileListModel::ColType},
    {"ShowHideRemoteChangedColumnAction3", true, FileListModel::ColChanged},
    {"ShowHideRemoteRightsColumnAction2", true, FileListModel::ColRights},
    {"ShowHideRemoteOwnerColumnAction2", true, FileListModel::ColOwner},
    {"ShowHideRemoteGroupColumnAction2", true, FileListModel::ColGroup},
    {"ShowHideRemoteLinkTargetColumnAction2", true, FileListModel::ColLinkTarget},
  };
  for (const auto & C : ColumnActions)
  {
    bool Remote = C.Remote;
    int Column = C.Column;
    QAction * Action = A(C.Name);
    connect(Action, &QAction::triggered, this, [this, Remote, Column](bool Checked)
    {
      FilePanel * Panel = SidePanel(Remote);
      if (PanelReady(Panel) && (Column != FileListModel::ColName)) Panel->SetColumnVisible(Column, Checked);
      UpdateSortActions();
    });
  }
  A.On("AutoSizeLocalColumnsAction", [this]() { if (PanelReady(SidePanel(false))) SidePanel(false)->AutoSizeColumns(); });
  A.On("AutoSizeRemoteColumnsAction", [this]() { if (PanelReady(SidePanel(true))) SidePanel(true)->AutoSizeColumns(); });
  A.On("ResetLayoutLocalColumnsAction", [this]() { if (PanelReady(SidePanel(false))) SidePanel(false)->ResetColumnLayout(); });
  A.On("ResetLayoutRemoteColumnsAction", [this]() { if (PanelReady(SidePanel(true))) SidePanel(true)->ResetColumnLayout(); });

  A.On("LocalChangePathAction2", [this]()
  {
    // Change Drive (Alt+F1): the places of the desktop (home, bookmarks, volumes)
    FilePanel * Panel = SidePanel(false);
    DesktopIntegration::ShowPlacesMenu(Panel, [Panel](const QString & Path) { Panel->ChangeDirectory(Path); });
  });
  A.On("RemoteChangePathAction2", [this]()
  {
    FilePanel * Panel = SidePanel(true);
    if (PanelReady(Panel)) Panel->ShowPathComboPopup();
  });
  A.On("RemoteFindFilesAction2", [this]() { FindFiles(); });
  A.On("IncrementalSearchStartAction", [this]() { if (PanelReady(ActivePanel())) ActivePanel()->StartIncrementalSearch(); });
  A.On("GoToAddressAction", [this]() { GoToAddress(); });

  // ---- Selection
  A.On("SelectOneAction", [this]() { if (PanelReady(ActivePanel())) ActivePanel()->SelectCurrentItem(); });
  A.On("SelectAction", [this]() { SelectByMask(ActivePanel(), true); });
  A.On("UnselectAction", [this]() { SelectByMask(ActivePanel(), false); });
  A.On("SelectAllAction", [this]() { if (PanelReady(ActivePanel())) ActivePanel()->SelectAll(); });
  A.On("InvertSelectionAction", [this]() { if (PanelReady(ActivePanel())) ActivePanel()->InvertSelection(); });
  A.On("ClearSelectionAction", [this]() { if (PanelReady(ActivePanel())) ActivePanel()->ClearSelection(); });
  A.On("RestoreSelectionAction", [this]() { if (PanelReady(ActivePanel())) ActivePanel()->RestoreSelectedNames(); });
  A.On("SelectSameExtAction", [this]() { if (PanelReady(ActivePanel())) ActivePanel()->SelectSameExtension(true); });
  A.On("UnselectSameExtAction", [this]() { if (PanelReady(ActivePanel())) ActivePanel()->SelectSameExtension(false); });

  // ---- Files
  A.On("NewFileAction", [this]() { EditNew(ActivePanel()); });
  A.On("NewDirAction", [this]() { CreateDirectory(ActivePanel()); });
  A.On("NewLinkAction", [this]() { AddEditLink(ActivePanel(), true); });
  A.On("CurrentCreateDirAction", [this]() { CreateDirectory(ActivePanel()); });
  A.On("CurrentOpenAction", [this]() { OpenFocused(); });
  A.On("CurrentAddEditLinkAction", [this]() { AddEditLink(ActivePanel(), false); });
  A.On("CurrentAddEditLinkContextAction", [this]() { AddEditLink(ActivePanel(), false); });
  A.On("CurrentRenameAction", [this]() { if (PanelReady(ActivePanel())) ActivePanel()->StartRename(); });
  A.On("CurrentDeleteAction", [this]() { DeleteFiles(ActivePanel(), false); });
  A.On("CurrentDeleteAlternativeAction", [this]() { DeleteFiles(ActivePanel(), true); });
  A.On("CurrentPropertiesAction", [this]() { Properties(ActivePanel()); });
  A.On("CalculateDirectorySizesAction", [this]() { CalculateDirectorySizes(ActivePanel()); });
  A.On("CurrentCopyToClipboardAction2", [this]() { CopyToClipboard(); });
  A.On("PasteAction3", [this]() { PasteFromClipboard(); });
  A.On("FileListToClipboardAction", [this]() { FileNamesToClipboard(false); });
  A.On("FullFileListToClipboardAction", [this]() { FileNamesToClipboard(true); });
  A.On("FileGenerateUrlAction2", [this]() { GenerateFileUrl(); });
  // the F5 hint of the Copy dialog: was the action executed by its shortcut?
  for (const char * Name : {"LocalCopyAction", "RemoteCopyAction"})
  {
    A(Name)->installEventFilter(this);
  }
  A.On("LocalCopyAction", [this]() { CopyFiles(false, false, 0); });
  A.On("LocalCopyNonQueueAction", [this]() { CopyFiles(false, false, -1); });
  A.On("LocalCopyQueueAction", [this]() { CopyFiles(false, false, 1); });
  A.On("LocalMoveAction", [this]() { CopyFiles(false, true, 0); });
  A.On("RemoteCopyAction", [this]() { CopyFiles(true, false, 0); });
  A.On("RemoteCopyNonQueueAction", [this]() { CopyFiles(true, false, -1); });
  A.On("RemoteCopyQueueAction", [this]() { CopyFiles(true, false, 1); });
  A.On("RemoteMoveAction", [this]() { CopyFiles(true, true, 0); });
  A.On("LocalLocalCopyAction", [this]() { LocalLocalCopy(false, false); });
  A.On("LocalLocalMoveAction", [this]() { LocalLocalCopy(false, true); });
  A.On("LocalOtherCopyAction", [this]() { LocalLocalCopy(true, false); });
  A.On("LocalOtherMoveAction", [this]() { LocalLocalCopy(true, true); });
  A.On("RemoteCopyToAction", [this]() { DuplicateOrMove(false); });
  A.On("RemoteMoveToAction", [this]() { DuplicateOrMove(true); });

  // ---- Commands
  A.On("CompareDirectoriesAction2", [this]() { CompareDirectories(); });
  A.On("SynchronizeAction", [this]() { Synchronize(true); });
  A.On("FullSynchronizeAction2", [this]() { Synchronize(false); });
  A.On("SynchronizeBrowsingAction2", [this]() { SynchronizeBrowsingChanged(); });
  A.On("CustomCommandsEnterAction", [this]()
  {
    // Commands > Custom Commands > Enter... (ad hoc command)
    static CustomCommandType LastCommand;
    SessionTab * Tab = CurrentTab();
    CustomCommandRunner::ExecuteAdHoc(this, LastCommand, CustomCommandsContext(), [this, Tab]()
    {
      if (FTabs.contains(Tab)) RefreshTab(Tab);
    });
  });
  A.On("CustomCommandsCustomizeAction", [this]()
  {
    if (PreferencesDialog::Execute(this, QStringLiteral("Commands")))
    {
      ApplyConfiguration();
    }
  });
  A.On("ConsoleAction", [this]()
  {
    // forms/Console
    SessionTab * Tab = CurrentTab();
    if (!Connected(Tab) || !Tab->Term->CanAnyCommand() || !CheckReady()) return;
    Terminal * Term = Tab->Term;
    bool DirectoryChanged = ConsoleDialog::Execute(Term, this);
    if (FTabs.contains(Tab) && Connected(Tab))
    {
      if (DirectoryChanged) Tab->RemotePanel->ChangeDirectory(Term->CurrentDirectory());
      else Tab->RemotePanel->Refresh();
    }
  });
  A.On("PuttyAction", [this]() { OpenInPutty(); });
  A.On("ClearCachesAction", [this]() { ClearCaches(); });
  A.On("TunnelsAction", [this]() { TunnelsDialog::Execute(this); });
  A.On("AgentAccessLogAction", [this]() { AgentAccessDialog::ShowLog(this); });
  A.On("AgentSessionsAction", [this]() { AgentAccessDialog::ShowSessions(this); });
  A.On("AgentAccessHelpAction", [this]() { AgentAccessDialog::ShowHelp(this); });
  A.On("CloseApplicationAction2", [this]() { close(); });

  // ---- Options / View
  A.On("PreferencesAction", [this]()
  {
    if (PreferencesDialog::Execute(this))
    {
      ApplyConfiguration();
    }
  });
  A.On("PresetsPreferencesAction", [this]()
  {
    if (PreferencesDialog::Execute(this, QStringLiteral("Transfer")))
    {
      ApplyConfiguration();
    }
  });
  A.On("ShowHiddenFilesAction", [this]() { ToggleShowHiddenFiles(); });
  connect(A("AutoReadDirectoryAfterOpAction"), &QAction::triggered, this, [this](bool Checked)
  {
    Configuration & Config = Configuration::Instance();
    Config.AutoReadDirectoryAfterOp = Checked;
    // AUTO_READ_DIRECTORY_TOGGLE
    statusBar()->showMessage(QStringLiteral("Automatic reloading of directory after operation was turned %1.")
      .arg(Checked ? QStringLiteral("on") : QStringLiteral("off")), 3000);
  });
  const struct { const char * Action; QToolBar * CommanderWindow::*Bar; } BarActions[] = {
    {"CommanderCommandsBandAction", &CommanderWindow::FCommandsBar},
    {"CommanderSessionBandAction2", &CommanderWindow::FSessionBar},
    {"CommanderPreferencesBandAction", &CommanderWindow::FPreferencesBar},
    {"CommanderSortBandAction", &CommanderWindow::FSortBar},
    {"CommanderTransferBandAction", &CommanderWindow::FTransferBar},
    {"CommanderCustomCommandsBandAction", &CommanderWindow::FCustomCommandsBar},
    {"ToolBar2Action", &CommanderWindow::FHotKeysBar},
  };
  for (const auto & B : BarActions)
  {
    auto Bar = B.Bar;
    connect(A(B.Action), &QAction::triggered, this, [this, Bar](bool Checked)
    {
      if ((this->*Bar) != nullptr) (this->*Bar)->setVisible(Checked);
    });
  }
  const struct { const char * Action; int Side; int Band; } BandActions[] = {
    {"CommanderLocalHistoryBandAction2", 0, bandHistory}, {"CommanderLocalNavigationBandAction2", 0, bandNavigation},
    {"CommanderLocalFileBandAction2", 0, bandFile}, {"CommanderLocalSelectionBandAction2", 0, bandSelection},
    {"CommanderRemoteHistoryBandAction2", 1, bandHistory}, {"CommanderRemoteNavigationBandAction2", 1, bandNavigation},
    {"CommanderRemoteFileBandAction2", 1, bandFile}, {"CommanderRemoteSelectionBandAction2", 1, bandSelection},
  };
  for (const auto & B : BandActions)
  {
    int Side = B.Side;
    int Band = B.Band;
    connect(A(B.Action), &QAction::triggered, this, [this, Side, Band](bool Checked)
    {
      FBandVisible[Side][Band] = Checked;
      UpdatePanelToolbars();
    });
  }
  connect(A("LockToolbarsAction"), &QAction::triggered, this, [this]() { ApplyToolbarStyle(); });
  connect(A("SelectiveToolbarTextAction"), &QAction::triggered, this, [this]() { ApplyToolbarStyle(); });
  A.On("ToolbarIconSizeNormalAction", [this]() { FToolbarIconSize = 16; ApplyToolbarStyle(); });
  A.On("ToolbarIconSizeLargeAction", [this]() { FToolbarIconSize = 24; ApplyToolbarStyle(); });
  A.On("ToolbarIconSizeVeryLargeAction", [this]() { FToolbarIconSize = 32; ApplyToolbarStyle(); });
  connect(A("SessionsTabsAction2"), &QAction::triggered, this, [this](bool Checked) { SessionTabs->setVisible(Checked); });
  connect(A("StatusBarAction"), &QAction::triggered, this, [this](bool Checked) { statusBar()->setVisible(Checked); });
  // Clipboard content decides Paste
  // (the clipboard is examined when it changes only: on X11 asking for its
  // formats is a round trip to the owner, too slow for every UpdateActions)
  auto ClipboardChanged = [this]()
  {
    QAction * Paste = (*FA)("PasteAction3");
    Paste->setProperty("NixSCPCanPaste", ClipboardTransfer::CanPaste());
    Paste->setEnabled(Paste->property("NixSCPCanPaste").toBool() && !FTabs.isEmpty() && PanelReady(ActivePanel()));
  };
  connect(QApplication::clipboard(), &QClipboard::dataChanged, this, ClipboardChanged);
  (*FA)("PasteAction3")->setProperty("NixSCPCanPaste", ClipboardTransfer::CanPaste());
}

//---------------------------------------------------------------------------
// Menus (ScpCommander.dfm MenuToolbar)

void CommanderWindow::CreateMenus()
{
  menuBar()->setObjectName(QStringLiteral("MenuToolbar"));
  auto Side = [this](bool Remote, QMenu * Menu)
  {
    QString S = Remote ? QStringLiteral("Remote") : QStringLiteral("Local");
    auto I = [&S](const char * Suffix) { return CA::Item(S + QString::fromLatin1(Suffix)); };
    QList<CA::MenuItem> Sort = {I("SortAscendingAction2"), CA::Separator(), I("SortByNameAction2"), I("SortByExtAction2"),
      I("SortBySizeAction2"), I("SortByTypeAction2"), I(Remote ? "SortByChangedAction3" : "SortByChangedAction3")};
    QList<CA::MenuItem> Columns;
    if (Remote)
    {
      Sort.append({I("SortByRightsAction2"), I("SortByOwnerAction2"), I("SortByGroupAction2")});
      Columns = {CA::Item(QStringLiteral("ShowHideRemoteNameColumnAction2")), CA::Item(QStringLiteral("ShowHideRemoteSizeColumnAction2")),
        CA::Item(QStringLiteral("ShowHideRemoteTypeColumnAction2")), CA::Item(QStringLiteral("ShowHideRemoteChangedColumnAction3")),
        CA::Item(QStringLiteral("ShowHideRemoteRightsColumnAction2")), CA::Item(QStringLiteral("ShowHideRemoteOwnerColumnAction2")),
        CA::Item(QStringLiteral("ShowHideRemoteGroupColumnAction2")), CA::Item(QStringLiteral("ShowHideRemoteLinkTargetColumnAction2")),
        CA::Separator(), CA::Item(QStringLiteral("AutoSizeRemoteColumnsAction")), CA::Item(QStringLiteral("ResetLayoutRemoteColumnsAction"))};
    }
    else
    {
      Sort.append(I("SortByAttrAction2"));
      Columns = {CA::Item(QStringLiteral("ShowHideLocalNameColumnAction2")), CA::Item(QStringLiteral("ShowHideLocalSizeColumnAction2")),
        CA::Item(QStringLiteral("ShowHideLocalTypeColumnAction2")), CA::Item(QStringLiteral("ShowHideLocalChangedColumnAction3")),
        CA::Item(QStringLiteral("ShowHideLocalAttrColumnAction2")),
        CA::Separator(), CA::Item(QStringLiteral("AutoSizeLocalColumnsAction")), CA::Item(QStringLiteral("ResetLayoutLocalColumnsAction"))};
    }
    QMenu * SortMenu = nullptr;
    QMenu * ColumnsMenu = nullptr;
    FA->Populate(Menu, {
      I("ChangePathAction2"),
      CA::Separator(),
      CA::Submenu(QStringLiteral("&Go To"), {I("OpenDirAction"), I("ExploreDirectoryAction"), CA::Separator(),
        I("ParentDirAction"), I("RootDirAction"), I("HomeDirAction"), I("OtherDirAction"), CA::Separator(),
        I("BackAction"), I("ForwardAction")}),
      I("RefreshAction"),
      I("AddBookmarkAction2"),
      I("PathToClipboardAction2"),
      CA::Separator(),
      CA::Submenu(QStringLiteral("&Sort"), Sort, &SortMenu),
      CA::Submenu(QStringLiteral("&Columns"), Columns, &ColumnsMenu),
      I("FilterAction")});
    connect(Menu, &QMenu::aboutToShow, this, [this]() { UpdateActions(); UpdateSortActions(); });
    for (QMenu * Sub : {SortMenu, ColumnsMenu})
    {
      if (Sub != nullptr) connect(Sub, &QMenu::aboutToShow, this, &CommanderWindow::UpdateSortActions);
    }
  };

  FLocalMenu = menuBar()->addMenu(QStringLiteral("&Local"));
  FLocalMenu->setToolTipsVisible(true);
  Side(false, FLocalMenu);

  QMenu * Mark = menuBar()->addMenu(QStringLiteral("&Mark"));
  FA->Populate(Mark, {CA::Item(QStringLiteral("SelectOneAction")), CA::Item(QStringLiteral("SelectAction")),
    CA::Item(QStringLiteral("UnselectAction")), CA::Item(QStringLiteral("SelectAllAction")), CA::Separator(),
    CA::Item(QStringLiteral("InvertSelectionAction")), CA::Item(QStringLiteral("ClearSelectionAction")),
    CA::Item(QStringLiteral("RestoreSelectionAction")), CA::Separator(),
    CA::Item(QStringLiteral("SelectSameExtAction")), CA::Item(QStringLiteral("UnselectSameExtAction"))});

  // Files: Upload/Download follow the active side (CurrentCopyItem dropdown)
  QMenu * Files = menuBar()->addMenu(QStringLiteral("&Files"));
  Files->setToolTipsVisible(true);
  QMenu * CopyMenu = new QMenu(this);
  CopyMenu->addAction(FCurrentCopyNonQueueItem);
  CopyMenu->addAction(FCurrentCopyQueueItem);
  CopyMenu->addSeparator();
  CopyMenu->addAction(FCurrentMoveItem);
  CopyMenu->menuAction()->setVisible(false);
  QMenu * FileNames = nullptr;
  FA->Populate(Files, {
    CA::Submenu(QStringLiteral("&New"), {CA::Item(QStringLiteral("NewFileAction")), CA::Item(QStringLiteral("NewDirAction")),
      CA::Item(QStringLiteral("NewLinkAction"))}),
    CA::Separator(),
    CA::Item(QStringLiteral("CurrentOpenAction")),
    CA::ExistingMenu(FEditorManager->EditorListMenu()),
    CA::Item(QStringLiteral("CurrentAddEditLinkAction")),
    CA::Separator(),
    CA::Object(FCurrentCopyItem),
    CA::Object(FCurrentCopyNonQueueItem),
    CA::Object(FCurrentCopyQueueItem),
    CA::Separator(),
    CA::Object(FCurrentMoveItem),
    CA::Object(FCurrentCopyToItem),
    CA::Object(FCurrentMoveToItem),
    CA::Item(QStringLiteral("CurrentDeleteAction")),
    CA::Item(QStringLiteral("CurrentRenameAction")),
    CA::Separator(),
    CA::Item(QStringLiteral("CurrentCopyToClipboardAction2")),
    CA::Item(QStringLiteral("PasteAction3")),
    CA::Separator(),
    CA::ActionSubmenu(QStringLiteral("CustomCommandsFileAction"), {}, &FFileCustomCommandsMenu),
    CA::Submenu(QStringLiteral("&File Names"), {CA::Item(QStringLiteral("FileListToClipboardAction")),
      CA::Item(QStringLiteral("FullFileListToClipboardAction")), CA::Item(QStringLiteral("FileGenerateUrlAction2"))}, &FileNames),
    CA::Separator(),
    CA::Item(QStringLiteral("CurrentPropertiesAction")),
    CA::Item(QStringLiteral("CalculateDirectorySizesAction"))});
  delete CopyMenu;
  connect(FFileCustomCommandsMenu, &QMenu::aboutToShow, this, [this]() { FillCustomCommandsMenu(FFileCustomCommandsMenu, true); });
  connect(Files, &QMenu::aboutToShow, this, &CommanderWindow::UpdateActions);

  QMenu * Commands = menuBar()->addMenu(QStringLiteral("&Commands"));
  Commands->setToolTipsVisible(true);
  QMenu * QueueMenu = nullptr;
  FA->Populate(Commands, {
    CA::Item(QStringLiteral("CompareDirectoriesAction2")),
    CA::Item(QStringLiteral("SynchronizeAction")),
    CA::Item(QStringLiteral("FullSynchronizeAction2")),
    CA::Item(QStringLiteral("SynchronizeBrowsingAction2")),
    CA::Item(QStringLiteral("RemoteFindFilesAction2")),
    CA::Submenu(QStringLiteral("Q&ueue"), {}, &QueueMenu),
    CA::ActionSubmenu(QStringLiteral("CustomCommandsNonFileAction"), {}, &FCommandsCustomCommandsMenu),
    CA::Separator(),
    CA::Item(QStringLiteral("ConsoleAction")),
    CA::Item(QStringLiteral("PuttyAction")),
    CA::Separator(),
    CA::Item(QStringLiteral("AgentAccessLogAction")),
    CA::Item(QStringLiteral("AgentSessionsAction")),
    CA::Item(QStringLiteral("AgentAccessHelpAction")),
    CA::Separator(),
    CA::Item(QStringLiteral("ClearCachesAction")),
    CA::Separator(),
    CA::Item(QStringLiteral("CloseApplicationAction2"))});
  FQueuePanel->PopulateQueueMenu(QueueMenu);
  connect(FCommandsCustomCommandsMenu, &QMenu::aboutToShow, this, [this]() { FillCustomCommandsMenu(FCommandsCustomCommandsMenu, false); });
  connect(Commands, &QMenu::aboutToShow, this, &CommanderWindow::UpdateActions);

  QMenu * Tabs = menuBar()->addMenu(QStringLiteral("&Tabs"));
  Tabs->setToolTipsVisible(true);
  FA->Populate(Tabs, {
    CA::ActionSubmenu(QStringLiteral("NewTabAction"), {CA::Item(QStringLiteral("NewRemoteTabAction")),
      CA::Item(QStringLiteral("NewLocalTabAction")), CA::Separator(), CA::Item(QStringLiteral("DefaultToNewRemoteTabAction"))}),
    CA::Item(QStringLiteral("CloseTabAction")),
    CA::Item(QStringLiteral("DuplicateTabAction")),
    CA::Item(QStringLiteral("RenameTabAction")),
    CA::Separator(),
    CA::ActionSubmenu(QStringLiteral("ColorMenuAction2"), {}, &FColorMenu),
    CA::Separator(),
    CA::Item(QStringLiteral("DisconnectSessionAction")),
    CA::Item(QStringLiteral("ReconnectSessionAction")),
    CA::Item(QStringLiteral("SaveCurrentSessionAction2")),
    CA::Separator(),
    CA::Item(QStringLiteral("FileSystemInfoAction")),
    CA::Item(QStringLiteral("SessionGenerateUrlAction2")),
    CA::Separator(),
    CA::ActionSubmenu(QStringLiteral("OpenedTabsAction"), {}, &FOpenedTabsMenu),
    CA::ActionSubmenu(QStringLiteral("WorkspacesAction"), {}, &FWorkspacesMenu),
    CA::Item(QStringLiteral("SaveWorkspaceAction")),
    CA::Separator(),
    CA::ActionSubmenu(QStringLiteral("SavedSessionsAction2"), {}, &FSitesMenu)});
  connect(FColorMenu, &QMenu::aboutToShow, this, [this]() { FillColorMenu(FColorMenu); });
  connect(FOpenedTabsMenu, &QMenu::aboutToShow, this, [this]() { FillOpenedTabsMenu(FOpenedTabsMenu); });
  connect(FWorkspacesMenu, &QMenu::aboutToShow, this, [this]() { FillWorkspacesMenu(FWorkspacesMenu); });
  connect(FSitesMenu, &QMenu::aboutToShow, this, [this]() { FillSitesMenu(FSitesMenu); });
  connect(Tabs, &QMenu::aboutToShow, this, &CommanderWindow::UpdateActions);

  QMenu * Options = menuBar()->addMenu(QStringLiteral("&Options"));
  Options->setToolTipsVisible(true);
  QMenu * QueueOptions = nullptr;
  FA->Populate(Options, {
    CA::Submenu(QStringLiteral("&Toolbars"), {
      CA::Item(QStringLiteral("CommanderCommandsBandAction")), CA::Item(QStringLiteral("CommanderSessionBandAction2")),
      CA::Item(QStringLiteral("CommanderPreferencesBandAction")), CA::Item(QStringLiteral("CommanderSortBandAction")),
      CA::Item(QStringLiteral("CommanderTransferBandAction")), CA::Item(QStringLiteral("CommanderCustomCommandsBandAction")),
      CA::Separator(), CA::Item(QStringLiteral("ToolBar2Action")), CA::Separator(),
      CA::Item(QStringLiteral("LockToolbarsAction")), CA::Item(QStringLiteral("SelectiveToolbarTextAction")),
      CA::ActionSubmenu(QStringLiteral("ToolbarIconSizeAction"), {CA::Item(QStringLiteral("ToolbarIconSizeNormalAction")),
        CA::Item(QStringLiteral("ToolbarIconSizeLargeAction")), CA::Item(QStringLiteral("ToolbarIconSizeVeryLargeAction"))})}),
    CA::ActionSubmenu(QStringLiteral("CommanderLocalPanelAction"), {
      CA::Item(QStringLiteral("CommanderLocalHistoryBandAction2")), CA::Item(QStringLiteral("CommanderLocalNavigationBandAction2")),
      CA::Item(QStringLiteral("CommanderLocalFileBandAction2")), CA::Item(QStringLiteral("CommanderLocalSelectionBandAction2")),
      CA::Separator(), CA::Item(QStringLiteral("LocalStatusBarAction2"))}),
    CA::ActionSubmenu(QStringLiteral("CommanderRemotePanelAction"), {
      CA::Item(QStringLiteral("CommanderRemoteHistoryBandAction2")), CA::Item(QStringLiteral("CommanderRemoteNavigationBandAction2")),
      CA::Item(QStringLiteral("CommanderRemoteFileBandAction2")), CA::Item(QStringLiteral("CommanderRemoteSelectionBandAction2")),
      CA::Separator(), CA::Item(QStringLiteral("RemoteStatusBarAction2"))}),
    CA::Separator(),
    CA::Item(QStringLiteral("SessionsTabsAction2")),
    CA::Item(QStringLiteral("StatusBarAction")),
    CA::Submenu(QStringLiteral("&Queue"), {
      CA::Item(QStringLiteral("QueueShowAction")), CA::Item(QStringLiteral("QueueHideWhenEmptyAction")),
      CA::Item(QStringLiteral("QueueHideAction")), CA::Separator(), CA::Item(QStringLiteral("QueueToolbarAction")),
      CA::Item(QStringLiteral("QueueFileListAction")), CA::Separator(), CA::Item(QStringLiteral("QueueResetLayoutColumnsAction")),
      CA::Separator(), CA::Item(QStringLiteral("QueueCycleOnceEmptyAction")), CA::Item(QStringLiteral("QueuePreferencesAction"))},
      &QueueOptions),
    CA::Separator(),
    CA::Item(QStringLiteral("PreferencesAction"))});
  connect(Options, &QMenu::aboutToShow, this, &CommanderWindow::UpdateActions);
  Q_UNUSED(QueueOptions);

  FRemoteMenu = menuBar()->addMenu(QStringLiteral("&Remote"));
  FRemoteMenu->setToolTipsVisible(true);
  Side(true, FRemoteMenu);

  // NixSCP: saved SSH tunnels, started and stopped from here
  TunnelsUi::SetupMenu(menuBar()->addMenu(QStringLiteral("T&unnels")), FA->Get(QStringLiteral("TunnelsAction")));

  // NixSCP Teams: shared sites (gui/TeamsDialog)
  TeamsDialog::SetupMenu(menuBar()->addMenu(QStringLiteral("Tea&ms")), this, [this]() { return CurrentSiteName(); });

  // NixSCP Pro: "Cloud Sync" when signed out, "Pro" when signed in
  CloudSyncUi::SetupMenu(menuBar()->addMenu(QStringLiteral("Cloud Sync")), this);

  // Help, the last menu: Contents (F1), Homepage, Report a Bug, Send Feedback, History, About
  QMenu * Help = menuBar()->addMenu(QStringLiteral("&Help"));
  HelpSystem::PopulateHelpMenu(Help, this);
  Q_UNUSED(FileNames);
}

//---------------------------------------------------------------------------
// Toolbars (ScpCommander.dfm TopDock, BottomDock; panel docks)

void CommanderWindow::CreateToolbars()
{
  auto MakeToolbar = [this](const QString & Title, const QString & Name)
  {
    QToolBar * Bar = addToolBar(Title);
    Bar->setObjectName(Name);
    Bar->setIconSize(QSize(FToolbarIconSize, FToolbarIconSize));
    Bar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    return Bar;
  };
  // Selective text labels: the important commands show their caption
  auto Selective = [](QAction * Action) { Action->setProperty("NixSCPSelectiveText", true); };

  // Commands (CommandsToolbar)
  FCommandsBar = MakeToolbar(QStringLiteral("Commands"), QStringLiteral("CommandsToolbar"));
  for (const char * Name : {"CompareDirectoriesAction2", "SynchronizeAction", "FullSynchronizeAction2"})
  {
    FCommandsBar->addAction((*FA)(Name));
  }
  FCommandsBar->addSeparator();
  FCommandsBar->addAction((*FA)("ConsoleAction"));
  FCommandsBar->addAction((*FA)("PuttyAction"));
  FCommandsBar->addSeparator();
  FCommandsBar->addAction((*FA)("SynchronizeBrowsingAction2"));
  Selective((*FA)("FullSynchronizeAction2"));

  // Sessions and Tabs (SessionToolbar2): hidden by default
  FSessionBar = MakeToolbar(QStringLiteral("Sessions and Tabs"), QStringLiteral("SessionToolbar"));
  auto * NewTabMenu = new QMenu(this);
  FA->Populate(NewTabMenu, {CA::Item(QStringLiteral("NewRemoteTabAction")), CA::Item(QStringLiteral("NewLocalTabAction")),
    CA::Separator(), CA::Item(QStringLiteral("DefaultToNewRemoteTabAction"))});
  CA::AddDropDownButton(FSessionBar, (*FA)("NewTabAction"), NewTabMenu);
  FSessionBar->addAction((*FA)("SaveCurrentSessionAction2"));
  FSessionBar->addSeparator();
  FSessionBar->addAction((*FA)("DuplicateTabAction"));
  FSessionBar->addAction((*FA)("CloseTabAction"));
  FSessionBar->addSeparator();
  auto * SitesButton = new QToolButton(FSessionBar);
  QAction * Sites = (*FA)("SavedSessionsAction2");
  SitesButton->setText(CA::PlainCaption(Sites->text()));
  SitesButton->setIcon(Sites->icon());
  SitesButton->setToolTip(Sites->toolTip());
  SitesButton->setMenu(FSitesMenu);
  SitesButton->setPopupMode(QToolButton::InstantPopup);
  SitesButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  SitesButton->setAutoRaise(true);
  FSessionBar->addWidget(SitesButton);
  Selective((*FA)("NewTabAction"));

  // Preferences (PreferencesToolbar)
  FPreferencesBar = MakeToolbar(QStringLiteral("Preferences"), QStringLiteral("PreferencesToolbar"));
  FPreferencesBar->addAction((*FA)("PreferencesAction"));
  FPreferencesBar->addSeparator();
  auto * QueueMenu = new QMenu(this);
  FA->Populate(QueueMenu, {CA::Item(QStringLiteral("QueueShowAction")), CA::Item(QStringLiteral("QueueHideWhenEmptyAction")),
    CA::Item(QStringLiteral("QueueHideAction")), CA::Separator(), CA::Item(QStringLiteral("QueueFileListAction"))});
  CA::AddDropDownButton(FPreferencesBar, (*FA)("QueueToggleShowAction"), QueueMenu);
  Selective((*FA)("QueueToggleShowAction"));

  // Sort (SortToolbar): hidden by default
  FSortBar = MakeToolbar(QStringLiteral("Sort"), QStringLiteral("SortToolbar"));
  FSortBar->addAction((*FA)("CurrentSortAscendingAction"));
  FSortBar->addSeparator();
  for (const char * Name : {"CurrentSortByNameAction", "CurrentSortByExtAction", "CurrentSortBySizeAction", "CurrentSortByTypeAction2",
         "CurrentSortByChangedAction2", "CurrentSortByRightsAction", "CurrentSortByOwnerAction", "CurrentSortByGroupAction"})
  {
    FSortBar->addAction((*FA)(Name));
  }

  // Transfer Settings (TransferToolbar): the presets (TransferDropDown)
  FTransferBar = MakeToolbar(QStringLiteral("Transfer Settings"), QStringLiteral("TransferToolbar"));
  auto * TransferLabel = new QLabel(QStringLiteral("Transfer Settings"));
  TransferLabel->setContentsMargins(4, 0, 4, 0);
  FTransferBar->addWidget(TransferLabel);
  FTransferCombo = new QComboBox();
  FTransferCombo->setObjectName(QStringLiteral("TransferDropDown"));
  FTransferCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
  FTransferCombo->setToolTip(QStringLiteral("Select transfer settings preset"));
  FTransferBar->addWidget(FTransferCombo);
  FTransferBar->addSeparator();
  FTransferBar->addAction((*FA)("PresetsPreferencesAction"));
  connect(FTransferCombo, QOverload<int>::of(&QComboBox::activated), this, [this](int Index)
  {
    if (FUpdatingTransferCombo || (Index < 0)) return;
    CopyParamPresets::Instance().SelectByUser(FTransferCombo->itemData(Index).toString());
    // COPY_PARAM_PRESET: the preset's description in the hint
    FTransferCombo->setToolTip(CopyParamPresets::Instance().CurrentCopyParam().GetInfoStr(QStringLiteral("; "), CopyParam::UsableAttrs(true) & CopyParam::UsableAttrs(false)));
  });
  connect(&CopyParamPresets::Instance(), &CopyParamPresets::ListChanged, this, &CommanderWindow::UpdateTransferCombo);
  connect(&CopyParamPresets::Instance(), &CopyParamPresets::CurrentChanged, this, &CommanderWindow::UpdateTransferCombo);
  UpdateTransferCombo();

  // Custom Command Buttons (CustomCommandsToolbar): hidden by default
  addToolBarBreak();
  FCustomCommandsBar = MakeToolbar(QStringLiteral("Custom Commands"), QStringLiteral("CustomCommandsToolbar"));
  FCustomCommandsBar->setToolButtonStyle(Qt::ToolButtonTextOnly);

  // Hot Keys (Toolbar2Toolbar, bottom): captions follow the active side
  FHotKeysBar = new QToolBar(QStringLiteral("Hot Keys"));
  FHotKeysBar->setObjectName(QStringLiteral("Toolbar2Toolbar"));
  FHotKeysBar->setToolButtonStyle(Qt::ToolButtonTextOnly);
  for (QAction * Action : {(*FA)("CurrentRenameAction"), FEditorManager->EditAction(), FHotCopyItem, FHotMoveItem,
         (*FA)("CurrentCreateDirAction"), (*FA)("CurrentDeleteAction"), (*FA)("CurrentPropertiesAction"), (*FA)("CloseApplicationAction2")})
  {
    auto * Button = new QToolButton(FHotKeysBar);
    Button->setAutoRaise(true);
    Button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    Button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    Button->setProperty("NixSCPHotKeyAction", QVariant::fromValue<QObject *>(Action));
    connect(Button, &QToolButton::clicked, Action, &QAction::trigger);
    connect(Action, &QAction::changed, Button, [this]() { UpdateHotKeyCaptions(); });
    FHotKeysBar->addWidget(Button);
  }
  addToolBar(Qt::BottomToolBarArea, FHotKeysBar);
  UpdateHotKeyCaptions();

  // Default visibility (ToolbarsLayout2 defaults: Session=0, Sort=0, CustomCommands=0, Toolbar2=0)
  const struct { QToolBar * Bar; const char * Action; bool Default; } Bars[] = {
    {FCommandsBar, "CommanderCommandsBandAction", true}, {FSessionBar, "CommanderSessionBandAction2", false},
    {FPreferencesBar, "CommanderPreferencesBandAction", true}, {FSortBar, "CommanderSortBandAction", false},
    {FTransferBar, "CommanderTransferBandAction", true}, {FCustomCommandsBar, "CommanderCustomCommandsBandAction", false},
    {FHotKeysBar, "ToolBar2Action", false},
  };
  // WinSCP.ini ToolbarsLayout2 (Interface\Commander), when this configuration came from WinSCP
  QString Layout;
  {
    IniStorage Storage(&Configuration::Instance().Ini());
    if (Storage.OpenSubKeyPath(QStringLiteral("Configuration\\Interface\\Commander"), false))
    {
      Layout = Storage.ReadStringRaw(QStringLiteral("ToolbarsLayout2"), QString());
      Storage.CloseSubKeyPath();
    }
  }
  QHash<QString, bool> LayoutVisible;
  for (const QString & Part : Layout.split(QLatin1Char(','), Qt::SkipEmptyParts))
  {
    int Eq = Part.indexOf(QLatin1Char('='));
    if (Eq > 0)
    {
      LayoutVisible.insert(Part.left(Eq).trimmed(), Part.mid(Eq + 1).startsWith(QLatin1Char('1')));
    }
  }
  const char * LayoutNames[] = {"Commands", "Session", "Preferences", "Sort", "Transfer", "CustomCommands", "Toolbar2"};
  int Index = 0;
  for (const auto & B : Bars)
  {
    QString Name = QString::fromLatin1(LayoutNames[Index++]);
    bool Visible = LayoutVisible.value(Name, B.Default);
    B.Bar->setVisible(Visible);
    QAction * Action = (*FA)(B.Action);
    Action->setChecked(Visible);
    QToolBar * Bar = B.Bar;
    connect(Bar, &QToolBar::visibilityChanged, Action, [Action, Bar, this]()
    {
      if (!FClosing && !isMinimized()) Action->setChecked(Bar->isVisibleTo(this));
    });
  }
}

void CommanderWindow::LoadToolbarSettings()
{
  // Configuration\Interface: DefaultToNewRemoteTab, LockToolbars,
  // SelectiveToolbarText, LargerToolbar; Interface\Commander: SessionsTabs,
  // StatusBar; NixSCP: panel bands
  IniStorage Storage(&Configuration::Instance().Ini());
  bool DefaultToNewRemoteTab = true;
  bool LockToolbars = false;
  bool SelectiveText = true;
  int LargerToolbar = 1;
  bool SessionsTabs = true;
  bool StatusBarVisible = true;
  QString Bands;
  if (Storage.OpenSubKeyPath(QStringLiteral("Configuration\\Interface"), false))
  {
    DefaultToNewRemoteTab = Storage.ReadBool(QStringLiteral("DefaultToNewRemoteTab"), DefaultToNewRemoteTab);
    LockToolbars = Storage.ReadBool(QStringLiteral("LockToolbars"), LockToolbars);
    SelectiveText = Storage.ReadBool(QStringLiteral("SelectiveToolbarText"), SelectiveText);
    LargerToolbar = Storage.ReadInteger(QStringLiteral("LargerToolbar"), LargerToolbar);
    if (Storage.OpenSubKey(QStringLiteral("Commander"), false))
    {
      SessionsTabs = Storage.ReadBool(QStringLiteral("SessionsTabs"), SessionsTabs);
      StatusBarVisible = Storage.ReadBool(QStringLiteral("StatusBar"), StatusBarVisible);
      Storage.CloseSubKey();
    }
    if (Storage.OpenSubKey(QStringLiteral("NixSCP"), false))
    {
      Bands = Storage.ReadStringRaw(QStringLiteral("PanelBands"), QString());
      Storage.CloseSubKey();
    }
    Storage.CloseSubKeyPath();
  }
  {
    QSignalBlocker Blocker((*FA)("DefaultToNewRemoteTabAction"));
    (*FA)("DefaultToNewRemoteTabAction")->setChecked(DefaultToNewRemoteTab);
  }
  (*FA)("NewTabAction")->setIcon(Icons::ForAction(DefaultToNewRemoteTab ? QStringLiteral("NewRemoteTabAction") : QStringLiteral("NewLocalTabAction")));
  (*FA)("LockToolbarsAction")->setChecked(LockToolbars);
  (*FA)("SelectiveToolbarTextAction")->setChecked(SelectiveText);
  FToolbarIconSize = (LargerToolbar <= 0) ? 16 : ((LargerToolbar == 1) ? 24 : 32);
  (*FA)("SessionsTabsAction2")->setChecked(SessionsTabs);
  SessionTabs->setVisible(SessionsTabs);
  (*FA)("StatusBarAction")->setChecked(StatusBarVisible);
  statusBar()->setVisible(StatusBarVisible);
  // "LHNFS;RHNFS": upper case = visible
  QStringList Sides = Bands.split(QLatin1Char(';'));
  for (int Side = 0; (Side < 2) && (Side < Sides.size()); Side++)
  {
    const QString & S = Sides[Side];
    for (int Band = 0; (Band < 4) && (Band + 1 < S.size()); Band++)
    {
      FBandVisible[Side][Band] = S.at(Band + 1).isUpper();
    }
  }
  const char * BandActions[2][4] = {
    {"CommanderLocalHistoryBandAction2", "CommanderLocalNavigationBandAction2", "CommanderLocalFileBandAction2", "CommanderLocalSelectionBandAction2"},
    {"CommanderRemoteHistoryBandAction2", "CommanderRemoteNavigationBandAction2", "CommanderRemoteFileBandAction2", "CommanderRemoteSelectionBandAction2"}};
  for (int Side = 0; Side < 2; Side++)
  {
    for (int Band = 0; Band < 4; Band++)
    {
      (*FA)(BandActions[Side][Band])->setChecked(FBandVisible[Side][Band]);
    }
  }
}

void CommanderWindow::SaveToolbarSettings()
{
  IniStorage Storage(&Configuration::Instance().Ini());
  if (Storage.OpenSubKeyPath(QStringLiteral("Configuration\\Interface"), true))
  {
    Storage.WriteBool(QStringLiteral("LockToolbars"), (*FA)("LockToolbarsAction")->isChecked());
    Storage.WriteBool(QStringLiteral("SelectiveToolbarText"), (*FA)("SelectiveToolbarTextAction")->isChecked());
    Storage.WriteInteger(QStringLiteral("LargerToolbar"), (FToolbarIconSize <= 16) ? 0 : ((FToolbarIconSize <= 24) ? 1 : 2));
    if (Storage.OpenSubKey(QStringLiteral("Commander"), true))
    {
      Storage.WriteBool(QStringLiteral("SessionsTabs"), (*FA)("SessionsTabsAction2")->isChecked());
      Storage.WriteBool(QStringLiteral("StatusBar"), (*FA)("StatusBarAction")->isChecked());
      Storage.CloseSubKey();
    }
    if (Storage.OpenSubKey(QStringLiteral("NixSCP"), true))
    {
      QString Bands;
      for (int Side = 0; Side < 2; Side++)
      {
        Bands += Side ? QStringLiteral(";r") : QStringLiteral("l");
        const char Letters[] = "hnfs";
        for (int Band = 0; Band < 4; Band++)
        {
          QChar C = QLatin1Char(Letters[Band]);
          Bands += FBandVisible[Side][Band] ? C.toUpper() : C;
        }
      }
      Storage.WriteStringRaw(QStringLiteral("PanelBands"), Bands);
      Storage.CloseSubKey();
    }
    Storage.CloseSubKeyPath();
  }
}

void CommanderWindow::ApplyToolbarStyle()
{
  bool Locked = (*FA)("LockToolbarsAction")->isChecked();
  bool SelectiveText = (*FA)("SelectiveToolbarTextAction")->isChecked();
  (*FA)("ToolbarIconSizeNormalAction")->setChecked(FToolbarIconSize <= 16);
  (*FA)("ToolbarIconSizeLargeAction")->setChecked(FToolbarIconSize == 24);
  (*FA)("ToolbarIconSizeVeryLargeAction")->setChecked(FToolbarIconSize >= 32);
  for (QToolBar * Bar : findChildren<QToolBar *>())
  {
    if (Bar == FHotKeysBar)
    {
      Bar->setMovable(!Locked);
      continue;
    }
    Bar->setMovable(!Locked && !Bar->property(PanelToolbarProperty).toBool());
    Bar->setIconSize(QSize(FToolbarIconSize, FToolbarIconSize));
    if (Bar == FCustomCommandsBar)
    {
      continue;
    }
    for (QAction * Action : Bar->actions())
    {
      QWidget * Widget = Bar->widgetForAction(Action);
      auto * Button = qobject_cast<QToolButton *>(Widget);
      if (Button == nullptr) continue;
      QAction * Source = Button->defaultAction() ? Button->defaultAction() : Action;
      QAction * Target = CA::ProxyTarget(Source);
      bool Text = SelectiveText &&
        (Source->property("NixSCPSelectiveText").toBool() || ((Target != nullptr) && Target->property("NixSCPSelectiveText").toBool()));
      Button->setToolButtonStyle(Text ? Qt::ToolButtonTextBesideIcon : Qt::ToolButtonIconOnly);
      Button->setIconSize(QSize(FToolbarIconSize, FToolbarIconSize));
    }
  }
}

void CommanderWindow::CreatePanelToolbars(SessionTab * Tab)
{
  // LocalFileToolbar / RemoteFileToolbar, LocalSelectionToolbar / RemoteSelectionToolbar
  for (bool Remote : {false, true})
  {
    FilePanel * Panel = Remote ? Tab->RemotePanel : Tab->LocalPanel;
    QString S = Remote ? QStringLiteral("Remote") : QStringLiteral("Local");
    auto A = [this, &S](const char * Suffix) { return FA->Get(S + QString::fromLatin1(Suffix)); };

    auto * FileBar = new QToolBar(QStringLiteral("%1 Files").arg(S));
    FileBar->setObjectName(S + QStringLiteral("FileToolbar"));
    FileBar->setProperty(PanelToolbarProperty, true);
    FileBar->setIconSize(QSize(FToolbarIconSize, FToolbarIconSize));
    FileBar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    QAction * CopyItem = Remote ? FRemoteCopyItem : FLocalCopyItem;
    QAction * MoveItem = Remote ? FRemoteMoveItem : FLocalMoveItem;
    auto * CopyMenu = new QMenu(FileBar);
    CopyMenu->addAction(A("CopyNonQueueAction"));
    CopyMenu->addAction(A("CopyQueueAction"));
    CopyMenu->addSeparator();
    CopyMenu->addAction(MoveItem);
    connect(CopyMenu, &QMenu::aboutToShow, this, [this, CopyMenu, Remote]()
    {
      // Local-local tabs have no upload/download variants
      bool LocalBrowser = IsLocalBrowserMode();
      QList<QAction *> Items = CopyMenu->actions();
      for (int I = 0; I < 3; I++) Items[I]->setVisible(!LocalBrowser);
      Q_UNUSED(Remote);
    });
    CA::AddDropDownButton(FileBar, CopyItem, CopyMenu);
    CopyItem->setProperty("NixSCPSelectiveText", true);
    FileBar->addSeparator();
    auto * EditMenu = new QMenu(FileBar);
    connect(EditMenu, &QMenu::aboutToShow, this, [this, EditMenu]()
    {
      // The editor list (EditorManager::EditorListMenu) for this side
      EditMenu->clear();
      QMenu * List = FEditorManager->EditorListMenu();
      emit List->aboutToShow();
      EditMenu->addActions(List->actions());
    });
    QAction * Edit = A("EditAction2");
    Edit->setProperty("NixSCPSelectiveText", true);
    CA::AddDropDownButton(FileBar, Edit, EditMenu);
    FileBar->addAction(A("DeleteAction2"));
    FileBar->addAction(A("RenameAction2"));
    auto * PropertiesMenu = new QMenu(FileBar);
    PropertiesMenu->addAction(A("PropertiesAction2"));
    PropertiesMenu->addSeparator();
    PropertiesMenu->addAction(A("CalculateDirectorySizesAction"));
    QAction * PropertiesAction = A("PropertiesAction2");
    PropertiesAction->setProperty("NixSCPSelectiveText", true);
    CA::AddDropDownButton(FileBar, PropertiesAction, PropertiesMenu);
    FileBar->addSeparator();
    auto * NewMenu = new QMenu(FileBar);
    NewMenu->addAction(A("NewFileAction"));
    NewMenu->addAction(A("CreateDirAction3"));
    NewMenu->addAction(A("AddEditLinkAction3"));
    auto * NewButton = new QToolButton(FileBar);
    NewButton->setText(QStringLiteral("&New"));
    NewButton->setIcon(Icons::ForAction(QStringLiteral("NewFileAction")));
    NewButton->setToolTip(QStringLiteral("New"));
    NewButton->setMenu(NewMenu);
    NewButton->setPopupMode(QToolButton::InstantPopup);
    NewButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    NewButton->setAutoRaise(true);
    FileBar->addWidget(NewButton)->setProperty("NixSCPSelectiveText", true);
    Panel->AddToolbar(FileBar, true);

    auto * SelectionBar = new QToolBar(QStringLiteral("%1 Selection").arg(S));
    SelectionBar->setObjectName(S + QStringLiteral("SelectionToolbar"));
    SelectionBar->setProperty(PanelToolbarProperty, true);
    SelectionBar->setIconSize(QSize(FToolbarIconSize, FToolbarIconSize));
    SelectionBar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    SelectionBar->addAction(A("SelectAction2"));
    SelectionBar->addAction(A("UnselectAction2"));
    SelectionBar->addAction(A("SelectAllAction2"));
    Panel->AddToolbar(SelectionBar, false);
    for (QToolBar * Bar : {FileBar, SelectionBar})
    {
      Bar->setMovable(false);
      // Activating the panel by its toolbar (the buttons act on their side)
      connect(Bar, &QToolBar::actionTriggered, this, [Tab, Panel](QAction *)
      {
        Tab->LastActivePanel = Panel;
      });
    }
  }
}

void CommanderWindow::UpdatePanelToolbars()
{
  for (SessionTab * Tab : FTabs)
  {
    for (int Side = 0; Side < 2; Side++)
    {
      FilePanel * Panel = Side ? Tab->RemotePanel : Tab->LocalPanel;
      Panel->SetHistoryButtonsVisible(FBandVisible[Side][bandHistory]);
      Panel->SetNavigationButtonsVisible(FBandVisible[Side][bandNavigation]);
      QString S = Side ? QStringLiteral("Remote") : QStringLiteral("Local");
      for (QToolBar * Bar : Panel->findChildren<QToolBar *>())
      {
        if (Bar->objectName() == S + QStringLiteral("FileToolbar"))
        {
          Bar->setVisible(FBandVisible[Side][bandFile]);
        }
        else if (Bar->objectName() == S + QStringLiteral("SelectionToolbar"))
        {
          Bar->setVisible(FBandVisible[Side][bandSelection]);
        }
      }
    }
  }
  (*FA)("LocalStatusBarAction2")->setChecked(FTabs.isEmpty() || CurrentTab()->LocalPanel->IsStatusBarVisible());
  (*FA)("RemoteStatusBarAction2")->setChecked(FTabs.isEmpty() || CurrentTab()->RemotePanel->IsStatusBarVisible());
}

void CommanderWindow::UpdateTransferCombo()
{
  // TCustomScpExplorerForm::UpdateTransferList
  if (FTransferCombo == nullptr) return;
  FUpdatingTransferCombo = true;
  CopyParamPresets & Presets = CopyParamPresets::Instance();
  FTransferCombo->clear();
  FTransferCombo->addItem(QStringLiteral("Default"), QString());
  const CopyParamList & List = Presets.List();
  for (int Index = 0; Index < List.Count(); Index++)
  {
    FTransferCombo->addItem(List.Name(Index), List.Name(Index));
  }
  int Current = FTransferCombo->findData(Presets.Current());
  FTransferCombo->setCurrentIndex((Current >= 0) ? Current : 0);
  FTransferCombo->setToolTip(Presets.CurrentCopyParam().GetInfoStr(QStringLiteral("; "), CopyParam::UsableAttrs(true) & CopyParam::UsableAttrs(false)));
  FUpdatingTransferCombo = false;
}

void CommanderWindow::UpdateHotKeyCaptions()
{
  // TScpCommanderForm::SetToolbar2ItemAction: "<shortcut> <caption>"
  if (FHotKeysBar == nullptr) return;
  for (QToolButton * Button : FHotKeysBar->findChildren<QToolButton *>())
  {
    auto * Action = qobject_cast<QAction *>(Button->property("NixSCPHotKeyAction").value<QObject *>());
    if (Action == nullptr) continue;
    QString Key = Action->shortcut().toString(QKeySequence::NativeText);
    QString Caption = CA::PlainCaption(Action->text());
    if (Action == (*FA)("CurrentCreateDirAction"))
    {
      Caption = QStringLiteral("Create Directory");
    }
    Button->setText(Key.isEmpty() ? Caption : QStringLiteral("%1 %2").arg(Key, Caption));
    Button->setToolTip(Action->toolTip());
    Button->setEnabled(Action->isEnabled());
  }
}

void CommanderWindow::UpdateSortActions()
{
  if (FTabs.isEmpty()) return;
  auto Check = [this](FilePanel * Panel, const char * Name, int Column)
  {
    QAction * Action = (*FA)(Name);
    Action->setEnabled(PanelReady(Panel) && Panel->IsColumnAvailable(Column));
    Action->setChecked(PanelReady(Panel) && (Panel->SortColumn() == Column));
  };
  FilePanel * L = SidePanel(false);
  FilePanel * R = SidePanel(true);
  FilePanel * C = ActivePanel();
  const struct { const char * Local; const char * Remote; const char * Current; int Column; } Sorts[] = {
    {"LocalSortByNameAction2", "RemoteSortByNameAction2", "CurrentSortByNameAction", FileListModel::ColName},
    {"LocalSortByExtAction2", "RemoteSortByExtAction2", "CurrentSortByExtAction", FileListModel::ColExt},
    {"LocalSortBySizeAction2", "RemoteSortBySizeAction2", "CurrentSortBySizeAction", FileListModel::ColSize},
    {"LocalSortByTypeAction2", "RemoteSortByTypeAction2", "CurrentSortByTypeAction2", FileListModel::ColType},
    {"LocalSortByChangedAction3", "RemoteSortByChangedAction3", "CurrentSortByChangedAction2", FileListModel::ColChanged},
    {"LocalSortByAttrAction2", "RemoteSortByRightsAction2", "CurrentSortByRightsAction", FileListModel::ColRights},
    {nullptr, "RemoteSortByOwnerAction2", "CurrentSortByOwnerAction", FileListModel::ColOwner},
    {nullptr, "RemoteSortByGroupAction2", "CurrentSortByGroupAction", FileListModel::ColGroup},
  };
  for (const auto & S : Sorts)
  {
    if (S.Local != nullptr) Check(L, S.Local, S.Column);
    Check(R, S.Remote, S.Column);
    Check(C, S.Current, S.Column);
  }
  // Ext is always sortable (name column), Rights/Owner/Group are available on remote panels
  (*FA)("LocalSortByExtAction2")->setEnabled(PanelReady(L));
  (*FA)("RemoteSortByExtAction2")->setEnabled(PanelReady(R));
  (*FA)("CurrentSortByExtAction")->setEnabled(PanelReady(C));
  (*FA)("LocalSortAscendingAction2")->setEnabled(PanelReady(L));
  (*FA)("LocalSortAscendingAction2")->setChecked(PanelReady(L) && L->SortAscending());
  (*FA)("RemoteSortAscendingAction2")->setEnabled(PanelReady(R));
  (*FA)("RemoteSortAscendingAction2")->setChecked(PanelReady(R) && R->SortAscending());
  (*FA)("CurrentSortAscendingAction")->setEnabled(PanelReady(C));
  (*FA)("CurrentSortAscendingAction")->setChecked(PanelReady(C) && C->SortAscending());
  const struct { const char * Name; bool Remote; int Column; } Columns[] = {
    {"ShowHideLocalNameColumnAction2", false, FileListModel::ColName},
    {"ShowHideLocalSizeColumnAction2", false, FileListModel::ColSize},
    {"ShowHideLocalTypeColumnAction2", false, FileListModel::ColType},
    {"ShowHideLocalChangedColumnAction3", false, FileListModel::ColChanged},
    {"ShowHideLocalAttrColumnAction2", false, FileListModel::ColRights},
    {"ShowHideRemoteNameColumnAction2", true, FileListModel::ColName},
    {"ShowHideRemoteSizeColumnAction2", true, FileListModel::ColSize},
    {"ShowHideRemoteTypeColumnAction2", true, FileListModel::ColType},
    {"ShowHideRemoteChangedColumnAction3", true, FileListModel::ColChanged},
    {"ShowHideRemoteRightsColumnAction2", true, FileListModel::ColRights},
    {"ShowHideRemoteOwnerColumnAction2", true, FileListModel::ColOwner},
    {"ShowHideRemoteGroupColumnAction2", true, FileListModel::ColGroup},
    {"ShowHideRemoteLinkTargetColumnAction2", true, FileListModel::ColLinkTarget},
  };
  for (const auto & Col : Columns)
  {
    FilePanel * Panel = Col.Remote ? R : L;
    QAction * Action = (*FA)(Col.Name);
    bool Ready = PanelReady(Panel) && Panel->IsColumnAvailable(Col.Column);
    Action->setEnabled(Ready && (Col.Column != FileListModel::ColName));
    Action->setChecked(Ready && Panel->IsColumnVisible(Col.Column));
  }
}

//---------------------------------------------------------------------------
// Enabled state (TNonVisualDataModule::ExplorerActionsUpdate) and the
// items that follow the active side (TScpCommanderForm::UpdateControls)

void CommanderWindow::UpdateSideItems()
{
  SessionTab * Tab = CurrentTab();
  bool RemoteSide = (ActivePanel() == Tab->RemotePanel);
  FA->SetCurrentSide(RemoteSide);
  if (Tab->LocalBrowser)
  {
    CA::SetProxyTarget(FCurrentCopyItem, (*FA)(RemoteSide ? "LocalOtherCopyAction" : "LocalLocalCopyAction"));
    CA::SetProxyTarget(FCurrentMoveItem, (*FA)(RemoteSide ? "LocalOtherMoveAction" : "LocalLocalMoveAction"));
    CA::SetProxyTarget(FCurrentCopyToItem, (*FA)(RemoteSide ? "LocalOtherCopyAction" : "LocalLocalCopyAction"));
    CA::SetProxyTarget(FCurrentMoveToItem, (*FA)(RemoteSide ? "LocalOtherMoveAction" : "LocalLocalMoveAction"));
    CA::SetProxyTarget(FLocalCopyItem, (*FA)("LocalLocalCopyAction"));
    CA::SetProxyTarget(FLocalMoveItem, (*FA)("LocalLocalMoveAction"));
    CA::SetProxyTarget(FRemoteCopyItem, (*FA)("LocalOtherCopyAction"));
    CA::SetProxyTarget(FRemoteMoveItem, (*FA)("LocalOtherMoveAction"));
  }
  else
  {
    CA::SetProxyTarget(FCurrentCopyItem, (*FA)(RemoteSide ? "RemoteCopyAction" : "LocalCopyAction"));
    CA::SetProxyTarget(FCurrentMoveItem, (*FA)(RemoteSide ? "RemoteMoveAction" : "LocalMoveAction"));
    CA::SetProxyTarget(FCurrentCopyToItem, (*FA)("RemoteCopyToAction"));
    CA::SetProxyTarget(FCurrentMoveToItem, (*FA)("RemoteMoveToAction"));
    CA::SetProxyTarget(FLocalCopyItem, (*FA)("LocalCopyAction"));
    CA::SetProxyTarget(FLocalMoveItem, (*FA)("LocalMoveAction"));
    CA::SetProxyTarget(FRemoteCopyItem, (*FA)("RemoteCopyAction"));
    CA::SetProxyTarget(FRemoteMoveItem, (*FA)("RemoteMoveAction"));
  }
  // CurrentCopyItem->Visible = !IsLocalBrowserMode(): the local copy is the "Copy To" item then
  FCurrentCopyItem->setVisible(!Tab->LocalBrowser);
  CA::SetProxyTarget(FCurrentCopyNonQueueItem, (*FA)(RemoteSide ? "RemoteCopyNonQueueAction" : "LocalCopyNonQueueAction"));
  CA::SetProxyTarget(FCurrentCopyQueueItem, (*FA)(RemoteSide ? "RemoteCopyQueueAction" : "LocalCopyQueueAction"));
  FCurrentCopyNonQueueItem->setVisible(false);
  FCurrentCopyQueueItem->setVisible(!Tab->LocalBrowser);
  CA::SetProxyTarget(FHotCopyItem, CA::ProxyTarget(Tab->LocalBrowser ? FCurrentCopyToItem : FCurrentCopyItem));
  CA::SetProxyTarget(FHotMoveItem, CA::ProxyTarget(Tab->LocalBrowser ? FCurrentMoveToItem : FCurrentMoveItem));
  // LOCAL_MENU_CAPTION / REMOTE_MENU_CAPTION, or LEFT/RIGHT in a local-local tab
  if (FLocalMenu != nullptr)
  {
    FLocalMenu->setTitle(Tab->LocalBrowser ? QStringLiteral("&Left") : QStringLiteral("&Local"));
    FRemoteMenu->setTitle(Tab->LocalBrowser ? QStringLiteral("&Right") : QStringLiteral("&Remote"));
  }
  UpdateHotKeyCaptions();
}

void CommanderWindow::UpdateActions()
{
  if (FTabs.isEmpty() || (FA == nullptr))
  {
    return;
  }
  CommanderActions & A = *FA;
  SessionTab * Tab = CurrentTab();
  FilePanel * Panel = ActivePanel();
  FilePanel * L = Tab->LocalPanel;
  FilePanel * R = Tab->RemotePanel;
  bool IsConnected = Connected(Tab);
  bool LocalBrowser = Tab->LocalBrowser;
  bool Idle = !Tab->OperationRunning;
  bool Ready = PanelReady(Panel);
  bool LReady = PanelReady(L);
  bool RReady = PanelReady(R);
  // EnabledSelectedOperation: marked files or a focused file
  auto HasSelection = [this](FilePanel * P) { return PanelReady(P) && ((P->MarkedCount() > 0) || P->HasFocusedFile()); };
  bool Selected = HasSelection(Panel);
  bool LSelected = HasSelection(L);
  bool RSelected = HasSelection(R);
  bool Focused = Ready && Panel->HasFocusedFile();
  bool FocusedFile = Focused && !Panel->FocusedFile().IsDirectory;
  bool CanTransfer = IsConnected && Idle;

  UpdateSideItems();

  // Tabs / session
  A("DisconnectSessionAction")->setEnabled(IsConnected);
  A("ReconnectSessionAction")->setEnabled(!LocalBrowser && !IsConnected && !Tab->Connecting && !Tab->Data.HostName.isEmpty());
  A("FileSystemInfoAction")->setEnabled(IsConnected);
  A("SaveCurrentSessionAction2")->setEnabled(IsConnected);
  A("SessionGenerateUrlAction2")->setEnabled(!LocalBrowser && !Tab->Data.HostName.isEmpty());
  A("NextTabAction")->setEnabled(FTabs.size() > 1);
  A("PreviousTabAction")->setEnabled(FTabs.size() > 1);
  A("DuplicateTabAction")->setEnabled(LocalBrowser || !Tab->Data.HostName.isEmpty());
  A("SaveWorkspaceAction")->setEnabled(std::any_of(FTabs.cbegin(), FTabs.cend(), [](const SessionTab * T)
  {
    return T->LocalBrowser ? !T->Implicit : !T->Data.HostName.isEmpty();
  }));
  A("ColorMenuAction2")->setEnabled(!LocalBrowser);
  if (FColorMenu != nullptr) FColorMenu->setEnabled(!LocalBrowser);
  for (int Index = 0; Index < FSelectTabActions.size(); Index++)
  {
    FSelectTabActions[Index]->setEnabled(Index < FTabs.size());
  }

  // Directories of both sides
  for (bool Remote : {false, true})
  {
    FilePanel * P = Remote ? R : L;
    bool PR = PanelReady(P);
    QString S = Remote ? QStringLiteral("Remote") : QStringLiteral("Local");
    auto E = [&A, &S](const char * Suffix, bool Enabled) { A.Get(S + QString::fromLatin1(Suffix))->setEnabled(Enabled); };
    E("OpenDirAction", PR);
    E("ParentDirAction", PR && !P->IsRoot());
    E("RootDirAction", PR && !P->IsRoot());
    E("HomeDirAction", PR);
    E("BackAction", PR && P->CanBack());
    E("ForwardAction", PR && P->CanForward());
    E("RefreshAction", PR);
    E("AddBookmarkAction2", PR);
    E("PathToClipboardAction2", PR);
    E("FilterAction", PR);
    {
      // Checked while a mask is on (NonVisual.cpp PANEL_ACTIONS)
      QAction * Filter = A.Get(S + QStringLiteral("FilterAction"));
      QSignalBlocker Blocker(Filter);
      Filter->setChecked(PR && !P->FilterMask().isEmpty());
    }
    // Path from Other Panel: local-local tabs only, the panels differ (NonVisual.cpp:291)
    E("OtherDirAction", PR && LocalBrowser && (QDir::cleanPath(L->CurrentDirectory()) != QDir::cleanPath(R->CurrentDirectory())));
    // Explore Directory: local directories only
    E("ExploreDirectoryAction", PR && !P->IsRemote());
    bool PSelected = HasSelection(P);
    bool PIdle = !P->IsRemote() || Idle;
    E("EditAction2", PSelected && PIdle);
    E("DeleteAction2", PSelected && PIdle);
    E("RenameAction2", PR && P->HasFocusedFile() && PIdle);
    E("PropertiesAction2", PSelected && PIdle);
    E("CalculateDirectorySizesAction", PSelected && PIdle);
    E("NewFileAction", PR && PIdle);
    E("CreateDirAction3", PR && PIdle);
    E("AddEditLinkAction3", PR && PIdle);
    E("SelectAction2", PR);
    E("UnselectAction2", PR);
    E("SelectAllAction2", PR);
    E("SortAscendingAction2", PR);
    A.Get(S + QStringLiteral("StatusBarAction2"))->setEnabled(true);
  }
  A("LocalChangePathAction2")->setEnabled(LReady);
  A("RemoteChangePathAction2")->setEnabled(RReady);
  A("AutoSizeLocalColumnsAction")->setEnabled(LReady);
  A("AutoSizeRemoteColumnsAction")->setEnabled(RReady);
  A("ResetLayoutLocalColumnsAction")->setEnabled(LReady);
  A("ResetLayoutRemoteColumnsAction")->setEnabled(RReady);
  A("RemoteFindFilesAction2")->setEnabled(Ready);
  A("GoToAddressAction")->setEnabled(Ready);
  A("IncrementalSearchStartAction")->setEnabled(Ready);

  // Selection
  for (const char * Name : {"SelectOneAction", "SelectAction", "UnselectAction", "SelectAllAction", "InvertSelectionAction",
         "ClearSelectionAction", "SelectSameExtAction", "UnselectSameExtAction"})
  {
    A(Name)->setEnabled(Ready);
  }
  A("RestoreSelectionAction")->setEnabled(Ready && Panel->SelectedNamesSaved());

  // Files
  bool PanelIdle = Ready && (!Panel->IsRemote() || Idle);
  A("NewFileAction")->setEnabled(PanelIdle);
  A("NewDirAction")->setEnabled(PanelIdle);
  A("CurrentCreateDirAction")->setEnabled(PanelIdle);
  // Links: remote by the protocol's capability, local always (symlink(2))
  bool CanLink = PanelIdle && (!Panel->IsRemote() || Tab->Term->IsCapable(fcSymbolicLink) || Tab->Term->IsCapable(fcHardLink));
  A("NewLinkAction")->setEnabled(CanLink);
  A("CurrentAddEditLinkAction")->setEnabled(CanLink);
  A("CurrentAddEditLinkContextAction")->setEnabled(CanLink && Focused && Panel->FocusedFile().IsSymLink);
  A("CurrentOpenAction")->setEnabled(Focused && PanelIdle);
  A("CurrentRenameAction")->setEnabled(Focused && PanelIdle);
  A("CurrentDeleteAction")->setEnabled(Selected && PanelIdle);
  A("CurrentDeleteAlternativeAction")->setEnabled(Selected && PanelIdle);
  A("CurrentPropertiesAction")->setEnabled(Selected && PanelIdle);
  A("CalculateDirectorySizesAction")->setEnabled(Selected && PanelIdle);
  A("CurrentCopyToClipboardAction2")->setEnabled(Selected && PanelIdle);
  A("PasteAction3")->setEnabled(PanelIdle && A("PasteAction3")->property("NixSCPCanPaste").toBool());
  A("FileListToClipboardAction")->setEnabled(Selected);
  A("FullFileListToClipboardAction")->setEnabled(Selected);
  A("FileGenerateUrlAction2")->setEnabled(Selected && Panel->IsRemote() && IsConnected);
  // Upload / download (HasTerminal && EnabledLocal/RemoteSelectedOperation)
  // and local-local copy; never both enabled (they share F5/F6)
  A("LocalCopyAction")->setEnabled(!LocalBrowser && CanTransfer && LSelected);
  A("LocalCopyNonQueueAction")->setEnabled(!LocalBrowser && CanTransfer && LSelected);
  A("LocalCopyQueueAction")->setEnabled(!LocalBrowser && IsConnected && LSelected && (Tab->Queue != nullptr));
  A("LocalMoveAction")->setEnabled(!LocalBrowser && CanTransfer && LSelected);
  A("RemoteCopyAction")->setEnabled(!LocalBrowser && CanTransfer && RSelected);
  A("RemoteCopyNonQueueAction")->setEnabled(!LocalBrowser && CanTransfer && RSelected);
  A("RemoteCopyQueueAction")->setEnabled(!LocalBrowser && IsConnected && RSelected && (Tab->Queue != nullptr));
  A("RemoteMoveAction")->setEnabled(!LocalBrowser && CanTransfer && RSelected);
  A("LocalLocalCopyAction")->setEnabled(LocalBrowser && LSelected);
  A("LocalLocalMoveAction")->setEnabled(LocalBrowser && LSelected);
  A("LocalOtherCopyAction")->setEnabled(LocalBrowser && RSelected);
  A("LocalOtherMoveAction")->setEnabled(LocalBrowser && RSelected);
  // Duplicate / Move To: the remote panel (any protocol: temporary copy fallback)
  bool RemoteSelected = !LocalBrowser && CanTransfer && (Panel == R) && RSelected;
  A("RemoteCopyToAction")->setEnabled(RemoteSelected);
  A("RemoteMoveToAction")->setEnabled(RemoteSelected && Tab->Term->IsCapable(fcRename));
  A("CustomCommandsFileAction")->setEnabled(Selected);
  if (FFileCustomCommandsMenu != nullptr) FFileCustomCommandsMenu->setEnabled(Selected);

  // Commands
  A("CompareDirectoriesAction2")->setEnabled((IsConnected || LocalBrowser) && LReady && RReady);
  A("SynchronizeAction")->setEnabled(CanTransfer);
  A("FullSynchronizeAction2")->setEnabled(CanTransfer);
  A("SynchronizeBrowsingAction2")->setEnabled(IsConnected);
  {
    QSignalBlocker Blocker(A("SynchronizeBrowsingAction2"));
    A("SynchronizeBrowsingAction2")->setChecked(IsConnected && Tab->SynchronizeBrowsing);
  }
  A("ConsoleAction")->setEnabled(CanTransfer && Tab->Term->CanAnyCommand());
  A("PuttyAction")->setEnabled(!LocalBrowser && !Tab->Data.HostName.isEmpty());
  A("ClearCachesAction")->setEnabled(CanTransfer);
  A("CustomCommandsEnterAction")->setEnabled(true);

  // Toggles
  {
    QSignalBlocker Blocker(A("ShowHiddenFilesAction"));
    A("ShowHiddenFilesAction")->setChecked(Configuration::Instance().ShowHiddenFiles);
  }
  {
    QSignalBlocker Blocker(A("AutoReadDirectoryAfterOpAction"));
    A("AutoReadDirectoryAfterOpAction")->setChecked(Configuration::Instance().AutoReadDirectoryAfterOp);
  }
  Q_UNUSED(FocusedFile);
  FEditorManager->UpdateActions();
  UpdateSortActions();
  UpdateHotKeyCaptions();
}

//---------------------------------------------------------------------------
// Popups (NonVisual.dfm LocalFilePopup, RemoteFilePopup, LocalDirViewPopup,
// RemoteDirViewPopup, LocalPanelPopup, RemotePanelPopup, CommanderBarPopup)

namespace
{
// A TTBXSubmenuItem with DropdownCombo in a popup (RemoteEditMenuItem,
// RemoteCopyMenuItem...): a click on the item or Enter runs its default action,
// the arrow opens the submenu. Qt menus only open it, so the default action is
// picked here and run once the popup has closed (Pending).
class DropdownComboFilter : public QObject
{
public:
  explicit DropdownComboFilter(QMenu * Menu) : QObject(Menu), FMenu(Menu) { Menu->installEventFilter(this); }

  void Add(QMenu * Submenu, QAction * Default) { FDefaults.insert(Submenu->menuAction(), Default); }
  QAction * Pending() const { return FPending; }

protected:
  bool eventFilter(QObject *, QEvent * Event) override
  {
    QAction * Item = nullptr;
    if (Event->type() == QEvent::MouseButtonRelease)
    {
      auto * Mouse = static_cast<QMouseEvent *>(Event);
      QPoint Pos = Mouse->position().toPoint();
      Item = FMenu->actionAt(Pos);
      int ArrowWidth = FMenu->fontMetrics().height() * 3 / 2;
      if ((Mouse->button() != Qt::LeftButton) || (Item == nullptr) || (Pos.x() >= FMenu->actionGeometry(Item).right() - ArrowWidth))
      {
        Item = nullptr;
      }
    }
    else if (Event->type() == QEvent::KeyPress)
    {
      int Key = static_cast<QKeyEvent *>(Event)->key();
      if ((Key == Qt::Key_Return) || (Key == Qt::Key_Enter))
      {
        Item = FMenu->activeAction();
      }
    }
    QAction * Default = FDefaults.value(Item, nullptr);
    if ((Default != nullptr) && Default->isEnabled())
    {
      FPending = Default;
      FMenu->close();
      return true;
    }
    return false;
  }

private:
  QMenu * FMenu;
  QHash<QAction *, QAction *> FDefaults;
  QAction * FPending = nullptr;
};
}

QMenu * CommanderWindow::CreateEditPopupSubmenu(QMenu * Parent, bool Bold)
{
  // CurrentEditFocusedAction with the editor list (EditorManager::EditorListMenu)
  QMenu * Sub = Parent->addMenu(FEditorManager->EditAction()->icon(), FEditorManager->EditAction()->text());
  Sub->setEnabled(FEditorManager->EditAction()->isEnabled());
  QMenu * List = FEditorManager->EditorListMenu();
  emit List->aboutToShow();
  Sub->addActions(List->actions());
  if (Bold)
  {
    QFont Font = Sub->menuAction()->font();
    Font.setBold(true);
    Sub->menuAction()->setFont(Font);
  }
  return Sub;
}

void CommanderWindow::ShowFilePopup(FilePanel * Panel, const QPoint & GlobalPos, bool OnItem)
{
  // LocalFilePopup / RemoteFilePopup hold the *Focused actions: a right click or
  // the Menu key on a file that is not marked is about that file, not the marks
  {
    QScopedValueRollback<bool> OnFocused(FOnFocused, OnItem);
    ExecuteFilePopup(Panel, GlobalPos, OnItem);
  }
  UpdateActions();
}

void CommanderWindow::ExecuteFilePopup(FilePanel * Panel, const QPoint & GlobalPos, bool OnItem)
{
  SessionTab * Tab = CurrentTab();
  if (!PanelReady(Panel))
  {
    return;
  }
  Tab->LastActivePanel = Panel;
  Tab->LocalPanel->SetActive(Panel == Tab->LocalPanel);
  Tab->RemotePanel->SetActive(Panel == Tab->RemotePanel);
  UpdateActions();
  bool Remote = (Panel == Tab->RemotePanel);
  QMenu Menu(this);
  Menu.setToolTipsVisible(true);
  if (!OnItem)
  {
    // LocalDirViewPopup / RemoteDirViewPopup (directory background)
    QString S = Remote ? QStringLiteral("Remote") : QStringLiteral("Local");
    auto I = [&S](const char * Suffix) { return CA::Item(S + QString::fromLatin1(Suffix)); };
    QList<CA::MenuItem> New = {CA::Item(QStringLiteral("NewFileAction")), CA::Item(QStringLiteral("NewDirAction"))};
    if (Panel->IsRemote())
    {
      New.append(CA::Item(QStringLiteral("NewLinkAction")));
    }
    QList<CA::MenuItem> Items = {
      CA::Submenu(QStringLiteral("&Go To"), {I("OpenDirAction"), I("ExploreDirectoryAction"), CA::Separator(),
        I("ParentDirAction"), I("RootDirAction"), I("HomeDirAction"), CA::Separator(), I("BackAction"), I("ForwardAction")}),
      I("RefreshAction")};
    if (Panel->IsRemote())
    {
      Items.append({I("AddBookmarkAction2"), I("FilterAction")});
    }
    else
    {
      Items.append({I("FilterAction"), I("AddBookmarkAction2")});
    }
    Items.append({I("PathToClipboardAction2"), CA::Separator(), CA::Submenu(QStringLiteral("&New"), New),
      CA::Item(QStringLiteral("PasteAction3"))});
    QMenu * NonFile = nullptr;
    if (Panel->IsRemote())
    {
      Items.append(CA::ActionSubmenu(QStringLiteral("CustomCommandsNonFileAction"), {}, &NonFile));
    }
    FA->Populate(&Menu, Items);
    if (NonFile != nullptr)
    {
      FillCustomCommandsMenu(NonFile, false);
    }
    Menu.exec(GlobalPos);
    return;
  }

  // LocalFilePopup / RemoteFilePopup; the DoubleClickAction item is bold
  TDoubleClickAction DoubleClick = Configuration::Instance().DoubleClickAction;
  bool FocusedDirectory = Panel->HasFocusedFile() && Panel->FocusedFile().IsDirectory;
  auto Bold = [](QAction * Action)
  {
    QFont Font = Action->font();
    Font.setBold(true);
    Action->setFont(Font);
  };
  auto AddProxy = [this, &Menu](QAction * Target, bool IsBold)
  {
    QAction * Proxy = FA->CreateProxy(&Menu, Target);
    if (IsBold)
    {
      QFont Font = Proxy->font();
      Font.setBold(true);
      Proxy->setFont(Font);
    }
    Menu.addAction(Proxy);
    return Proxy;
  };
  if (Panel->IsRemote())
  {
    QAction * EditLink = AddProxy((*FA)("CurrentAddEditLinkContextAction"), false);
    EditLink->setVisible(Panel->HasFocusedFile() && Panel->FocusedFile().IsSymLink);
  }
  AddProxy((*FA)("CurrentOpenAction"), FocusedDirectory || (DoubleClick == dcaOpen));
  auto * Combo = new DropdownComboFilter(&Menu);
  Combo->Add(CreateEditPopupSubmenu(&Menu, !FocusedDirectory && (DoubleClick == dcaEdit)), FEditorManager->EditAction());
  if (Tab->LocalBrowser)
  {
    // LocalLocalCopyFocusedAction / LocalLocalMoveFocusedAction
    AddProxy((*FA)(Remote ? "LocalOtherCopyAction" : "LocalLocalCopyAction"), !FocusedDirectory && (DoubleClick == dcaCopy));
    AddProxy((*FA)(Remote ? "LocalOtherMoveAction" : "LocalLocalMoveAction"), false);
  }
  else
  {
    QString S = Remote ? QStringLiteral("Remote") : QStringLiteral("Local");
    QAction * Copy = FA->Get(S + QStringLiteral("CopyAction"));
    QMenu * CopyMenu = Menu.addMenu(Copy->icon(), Copy->text());
    Combo->Add(CopyMenu, Copy);
    CopyMenu->setEnabled(Copy->isEnabled());
    CopyMenu->menuAction()->setStatusTip(Copy->statusTip());
    if (!FocusedDirectory && (DoubleClick == dcaCopy))
    {
      Bold(CopyMenu->menuAction());
    }
    CopyMenu->addAction(FA->CreateProxy(CopyMenu, FA->Get(S + QStringLiteral("CopyNonQueueAction"))));
    CopyMenu->addAction(FA->CreateProxy(CopyMenu, FA->Get(S + QStringLiteral("CopyQueueAction"))));
    CopyMenu->addSeparator();
    CopyMenu->addAction(FA->CreateProxy(CopyMenu, FA->Get(S + QStringLiteral("MoveAction"))));
    if (Panel->IsRemote())
    {
      AddProxy((*FA)("RemoteCopyToAction"), false);
      AddProxy((*FA)("RemoteMoveToAction"), false);
    }
  }
  AddProxy((*FA)("CurrentDeleteAction"), false);
  AddProxy((*FA)("CurrentRenameAction"), false);
  Menu.addSeparator();
  AddProxy((*FA)("CurrentCopyToClipboardAction2"), false);
  Menu.addSeparator();
  QAction * CustomCommands = (*FA)("CustomCommandsFileAction");
  QMenu * CustomMenu = Menu.addMenu(CustomCommands->icon(), CustomCommands->text());
  FillCustomCommandsMenu(CustomMenu, true);
  QMenu * FileNames = Menu.addMenu(QStringLiteral("&File Names"));
  FileNames->addAction((*FA)("FileListToClipboardAction"));
  FileNames->addAction((*FA)("FullFileListToClipboardAction"));
  if (Panel->IsRemote())
  {
    FileNames->addAction((*FA)("FileGenerateUrlAction2"));
  }
  Menu.addSeparator();
  AddProxy((*FA)("CurrentPropertiesAction"), false);
  // CurrentSystemMenuFocusedAction (the Windows shell context menu) has no Linux equivalent
  Menu.exec(GlobalPos);
  if (Combo->Pending() != nullptr)
  {
    Combo->Pending()->trigger();
  }
}

void CommanderWindow::ShowPanelPopup(FilePanel * Panel, const QPoint & GlobalPos)
{
  // LocalPanelPopup / RemotePanelPopup (path label)
  SessionTab * Tab = CurrentTab();
  bool Remote = (Panel == Tab->RemotePanel);
  QString S = Remote ? QStringLiteral("Remote") : QStringLiteral("Local");
  auto I = [&S](const char * Suffix) { return CA::Item(S + QString::fromLatin1(Suffix)); };
  UpdateActions();
  QMenu Menu(this);
  Menu.setToolTipsVisible(true);
  QList<CA::MenuItem> Items = {
    CA::Submenu(QStringLiteral("&Go To"), {I("OpenDirAction"), I("ExploreDirectoryAction"), CA::Separator(),
      I("ParentDirAction"), I("RootDirAction"), I("HomeDirAction"), I("OtherDirAction"), CA::Separator(),
      I("BackAction"), I("ForwardAction")}),
    I("RefreshAction")};
  if (Remote)
  {
    Items.append({I("AddBookmarkAction2"), I("FilterAction")});
  }
  else
  {
    Items.append({I("FilterAction"), I("AddBookmarkAction2")});
  }
  Items.append({I("PathToClipboardAction2"), CA::Separator(),
    CA::Item(QStringLiteral("Commander") + S + QStringLiteral("HistoryBandAction2")),
    CA::Item(QStringLiteral("Commander") + S + QStringLiteral("NavigationBandAction2")),
    CA::Item(QStringLiteral("Commander") + S + QStringLiteral("FileBandAction2")),
    CA::Item(QStringLiteral("Commander") + S + QStringLiteral("SelectionBandAction2")),
    CA::Separator(),
    CA::Item(QStringLiteral("LockToolbarsAction")), CA::Item(QStringLiteral("SelectiveToolbarTextAction")),
    CA::ActionSubmenu(QStringLiteral("ToolbarIconSizeAction"), {CA::Item(QStringLiteral("ToolbarIconSizeNormalAction")),
      CA::Item(QStringLiteral("ToolbarIconSizeLargeAction")), CA::Item(QStringLiteral("ToolbarIconSizeVeryLargeAction"))}),
    CA::Separator(),
    I("StatusBarAction2")});
  FA->Populate(&Menu, Items);
  Menu.exec(GlobalPos);
}

QMenu * CommanderWindow::createPopupMenu()
{
  // CommanderBarPopup (right click on the toolbars / menu bar)
  auto * Menu = new QMenu(this);
  Menu->setToolTipsVisible(true);
  UpdateActions();
  FA->Populate(Menu, {
    CA::Item(QStringLiteral("CommanderCommandsBandAction")), CA::Item(QStringLiteral("CommanderSessionBandAction2")),
    CA::Item(QStringLiteral("CommanderPreferencesBandAction")), CA::Item(QStringLiteral("CommanderSortBandAction")),
    CA::Item(QStringLiteral("CommanderTransferBandAction")), CA::Item(QStringLiteral("CommanderCustomCommandsBandAction")),
    CA::Separator(),
    CA::Item(QStringLiteral("LockToolbarsAction")), CA::Item(QStringLiteral("SelectiveToolbarTextAction")),
    CA::ActionSubmenu(QStringLiteral("ToolbarIconSizeAction"), {CA::Item(QStringLiteral("ToolbarIconSizeNormalAction")),
      CA::Item(QStringLiteral("ToolbarIconSizeLargeAction")), CA::Item(QStringLiteral("ToolbarIconSizeVeryLargeAction"))}),
    CA::Separator(),
    CA::Item(QStringLiteral("SessionsTabsAction2")), CA::Item(QStringLiteral("ToolBar2Action")), CA::Item(QStringLiteral("StatusBarAction")),
    CA::Separator(),
    CA::ActionSubmenu(QStringLiteral("CommanderLocalPanelAction"), {
      CA::Item(QStringLiteral("CommanderLocalHistoryBandAction2")), CA::Item(QStringLiteral("CommanderLocalNavigationBandAction2")),
      CA::Item(QStringLiteral("CommanderLocalFileBandAction2")), CA::Item(QStringLiteral("CommanderLocalSelectionBandAction2")),
      CA::Separator(), CA::Item(QStringLiteral("LocalStatusBarAction2"))}),
    CA::ActionSubmenu(QStringLiteral("CommanderRemotePanelAction"), {
      CA::Item(QStringLiteral("CommanderRemoteHistoryBandAction2")), CA::Item(QStringLiteral("CommanderRemoteNavigationBandAction2")),
      CA::Item(QStringLiteral("CommanderRemoteFileBandAction2")), CA::Item(QStringLiteral("CommanderRemoteSelectionBandAction2")),
      CA::Separator(), CA::Item(QStringLiteral("RemoteStatusBarAction2"))}),
    CA::Submenu(QStringLiteral("&Queue"), {
      CA::Item(QStringLiteral("QueueShowAction")), CA::Item(QStringLiteral("QueueHideWhenEmptyAction")),
      CA::Item(QStringLiteral("QueueHideAction")), CA::Separator(), CA::Item(QStringLiteral("QueueToolbarAction")),
      CA::Item(QStringLiteral("QueueFileListAction")), CA::Separator(), CA::Item(QStringLiteral("QueuePreferencesAction"))})});
  return Menu;
}
