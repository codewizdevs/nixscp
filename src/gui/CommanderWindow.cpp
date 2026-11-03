// Port of WinSCP source/forms/ScpCommander.cpp, forms/CustomScpExplorer.cpp,
// forms/NonVisual.cpp (command handlers) and windows/TerminalManager.cpp
// (tabs, local browser tabs). Actions, menus, toolbars and popups are built
// in gui/CommanderActions.cpp.
#include "gui/TunnelsDialog.h"
#include "gui/CommanderWindow.h"
#include "gui/Translator.h"
#include "gui/HelpSystem.h"
#include "gui/CommanderActions.h"
#include "gui/ClipboardTransfer.h"
#include "gui/DesktopIntegration.h"
#include "gui/EditorManager.h"
#include "gui/FileOperations.h"
#include "gui/GenerateUrlDialog.h"
#include "gui/ServerInfoDialog.h"
#include "gui/SiteAdvancedDialog.h"
#include "gui/SystemActions.h"
#include "core/LocalFileOperations.h"
#include "gui/CopyDialog.h"
#include "gui/GuiUserInterface.h"
#include "gui/LoginDialog.h"
#include "gui/ProgressDialog.h"
#include "gui/CloudSyncUi.h"
#include "gui/PreferencesDialog.h"
#include "gui/EditMaskDialog.h"
#include "gui/FindFilesDialog.h"
#include "gui/LocationProfilesDialog.h"
#include "gui/SynchronizeDialogs.h"
#include "gui/ConsoleDialog.h"
#include "gui/Icons.h"
#include "gui/HistoryComboBox.h"
#include "gui/SaveWorkspaceDialog.h"
#include "gui/CustomCommandRunner.h"
#include "gui/CustomCommandsPage.h"
#include "core/CopyParamList.h"
#include "core/DirectoryCache.h"
#include "core/Workspaces.h"
#include "core/CustomCommands.h"
#include "core/Bookmarks.h"
#include "gui/QueuePanel.h"
#include "core/Configuration.h"
#include "core/Queue.h"
#include "core/Terminal.h"

#include <QAction>
#include <QToolButton>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QLocale>
#include <QMouseEvent>
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyle>
#include <QTabBar>
#include <QTimer>
#include <QToolBar>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>


// Status bar messages in the UI language (WinSCP's message templates)
static void ShowStatusMessage(QStatusBar * Bar, const QString & Message, int Timeout = 0)
{
  Bar->showMessage(Translator::TrMessage(Message), Timeout);
}

// ---- Reconnect (TTerminalManager::ReconnectActiveTerminal, connection lost handling)
// Per tab state kept as dynamic properties of the tab page:
// the remote directory when the session closed, and when the current series
// of automatic reconnects started (TManagedTerminal::ReopenStart)
static const char * const LastRemoteDirectoryProperty = "NixSCPLastRemoteDirectory";
static const char * const ReopenStartProperty = "NixSCPReopenStart";
static const char * const ReconnectingProperty = "NixSCPReconnecting";
static const char * const EverConnectedProperty = "NixSCPEverConnected";

// TitleSeparator (core/Common.cpp): en dash
static const QString TitleSeparator = QStringLiteral(" – ");

// The session of the tab, opened again in its last remote directory
// (TTerminal::Reopen: PeekCurrentDirectory); OpenSession reuses the remembered password
static SessionData ReconnectData(const SessionTab & Tab)
{
  SessionData Data = Tab.Data;
  QString LastDirectory = Tab.Page->property(LastRemoteDirectoryProperty).toString();
  if (!LastDirectory.isEmpty())
  {
    Data.RemoteDirectory = LastDirectory;
  }
  return Data;
}

// Starts (or continues) reconnecting the tab (TTerminalManager::ReconnectingTerminal)
static SessionData BeginReconnect(SessionTab & Tab)
{
  Tab.Page->setProperty(ReconnectingProperty, true);
  if (Tab.Page->property(ReopenStartProperty).toLongLong() == 0)
  {
    Tab.Page->setProperty(ReopenStartProperty, QDateTime::currentMSecsSinceEpoch());
  }
  return ReconnectData(Tab);
}

// TTerminalManager::ReconnectedTerminal (or reconnecting given up)
static void EndReconnect(SessionTab & Tab)
{
  Tab.Page->setProperty(ReconnectingProperty, QVariant());
  Tab.Page->setProperty(ReopenStartProperty, QVariant());
}

static bool SameSessionTarget(const SessionData & Data1, const SessionData & Data2)
{
  return (Data1.FSProtocol == Data2.FSProtocol) && (Data1.HostName == Data2.HostName) &&
    (Data1.PortNumber == Data2.PortNumber) && (Data1.UserName == Data2.UserName);
}

// Connection lost message with Reconnect (RECONNECT_BUTTON) and OK; with
// TimeoutMs > 0 it reconnects automatically after the countdown shown on the
// button (SessionReopenAutoIdle). Returns true to reconnect.
static bool ConnectionLostDialog(QWidget * Parent, const QString & Message, const QString & MoreMessages, int TimeoutMs)
{
  MessageParams Params;
  MessageButtonAlias Alias;
  Alias.Button = qaRetry;
  Alias.Alias = QStringLiteral("&Reconnect");
  Params.Aliases.append(Alias);
  if (TimeoutMs > 0)
  {
    Params.Timeout = static_cast<unsigned int>(TimeoutMs);
    Params.TimeoutAnswer = qaRetry;
  }
  DesktopIntegration::FlashOnBackground(Parent);
  return (MoreMessageDialog(Parent, Message, MoreMessages, qtError, qaRetry | qaOK, QString(), &Params) == qaRetry);
}

// ExtractShortName (windows/WinInterface): last component of a path
static QString ShortPathName(const QString & Path, bool Unix)
{
  QString Result = Unix ? UnixPath::ExtractFileName(Path) : QFileInfo(Path).fileName();
  return Result.isEmpty() ? Path : Result;
}

static bool SameUnixPath(const QString & Path1, const QString & Path2)
{
  auto Normalize = [](QString P)
  {
    while ((P.size() > 1) && P.endsWith(QLatin1Char('/'))) P.chop(1);
    return P;
  };
  return Normalize(Path1) == Normalize(Path2);
}

static const int WindowStateVersion = 3;

CommanderWindow::CommanderWindow(QWidget * Parent) :
  QMainWindow(Parent),
  FUI(new GuiUserInterface(this))
{
  Configuration & Config = Configuration::Instance();
  setWindowTitle(QStringLiteral("NixSCP"));
  setWindowIcon(Icons::Application());
  resize(1100, 700);

  // Session tabs (forms/CustomScpExplorer SessionsPageControl); last tab is "New Tab"
  SessionTabs = new QTabBar();
  SessionTabs->setObjectName(QStringLiteral("SessionsPageControl"));
  SessionTabs->setDrawBase(false);
  SessionTabs->setExpanding(false);
  SessionTabs->setTabsClosable(true);
  SessionTabs->setMovable(false);
  SessionTabs->setUsesScrollButtons(true);
  SessionTabs->setContextMenuPolicy(Qt::CustomContextMenu);
  SessionTabs->addTab(Icons::ForAction(QStringLiteral("NewTabAction")), QStringLiteral("New Tab"));
  SessionTabs->setTabButton(0, QTabBar::RightSide, nullptr);
  SessionTabs->setTabButton(0, QTabBar::LeftSide, nullptr);
  connect(SessionTabs, &QTabBar::tabBarClicked, this, [this](int Index)
  {
    if (Index == SessionTabs->count() - 1)
    {
      // TCustomScpExplorerForm::SessionsPageControlChange: New Tab (Ctrl reverses DefaultToNewRemoteTab)
      QTimer::singleShot(0, this, [this]() { NewTabFromCurrent(true); });
    }
  });
  connect(SessionTabs, &QTabBar::currentChanged, this, [this](int Index)
  {
    if (FUpdatingTabs)
    {
      return;
    }
    if ((Index < 0) || (Index >= FTabs.size()))
    {
      // "New Tab" pseudo tab must never stay selected
      FUpdatingTabs = true;
      SessionTabs->setCurrentIndex(Pages->currentIndex());
      FUpdatingTabs = false;
      return;
    }
    ActivateTab(FTabs[Index]);
  });
  connect(SessionTabs, &QTabBar::tabCloseRequested, this, [this](int Index)
  {
    if ((Index >= 0) && (Index < FTabs.size()))
    {
      CloseTab(FTabs[Index]);
    }
  });
  connect(SessionTabs, &QWidget::customContextMenuRequested, this, [this](const QPoint & Pos)
  {
    ShowTabMenu(SessionTabs->tabAt(Pos), SessionTabs->mapToGlobal(Pos));
  });
  SessionTabs->installEventFilter(this);

  Pages = new QStackedWidget();
  FQueuePanel = new QueuePanel();
  FQueuePanel->hide();
  connect(FQueuePanel, &QueuePanel::HideRequested, this, &CommanderWindow::UpdateQueueView);
  connect(FQueuePanel, &QueuePanel::VisibilityChanged, this, &CommanderWindow::UpdateQueueView);
  connect(FQueuePanel, &QueuePanel::ConfigurationChanged, this, &CommanderWindow::ApplyConfiguration);
  connect(FQueuePanel, &QueuePanel::OnceEmptyRequested, this, [this](int Operation)
  {
    // TCustomScpExplorerForm::UpdateQueueStatus -> Terminal->CloseOnCompletion
    // of the session whose queue became empty
    SessionTab * Tab = CurrentTab();
    for (SessionTab * T : FTabs)
    {
      if ((T->Queue != nullptr) && (T->Queue == FQueuePanel->Queue()))
      {
        Tab = T;
        break;
      }
    }
    ExecuteOnceDone(Tab, Operation, true);
  });
  QueueSplitter = new QSplitter(Qt::Vertical);
  QueueSplitter->addWidget(Pages);
  QueueSplitter->addWidget(FQueuePanel);
  QueueSplitter->setStretchFactor(0, 1);
  QueueSplitter->setChildrenCollapsible(false);
  QueueSplitter->handle(1)->installEventFilter(this);

  auto * Central = new QWidget();
  auto * Layout = new QVBoxLayout(Central);
  Layout->setContentsMargins(2, 2, 2, 0);
  Layout->setSpacing(2);
  Layout->addWidget(SessionTabs);
  Layout->addWidget(QueueSplitter, 1);
  setCentralWidget(Central);

  // Actions (NonVisual.dfm), the editors (windows/EditorManager.cpp)
  FA = new CommanderActions(this);
  FA->ApplyCommanderShortcuts(Config.ExplorerKeyboardShortcuts);
  FEditorManager = new EditorManager(this);
  EditorManager::Hooks H;
  H.Terminals = [this]()
  {
    QList<Terminal *> R;
    for (SessionTab * T : FTabs)
    {
      if (Connected(T)) R.append(T->Term);
    }
    return R;
  };
  H.CanStartOperation = [this](Terminal * T)
  {
    SessionTab * Tab = TabOfTerminal(T);
    return (Tab != nullptr) && !Tab->OperationRunning;
  };
  H.SetOperationRunning = [this](Terminal * T, bool Running)
  {
    if (SessionTab * Tab = TabOfTerminal(T))
    {
      Tab->OperationRunning = Running;
      UpdateActions();
    }
  };
  H.FileUploaded = [this](Terminal * T, const QString &)
  {
    SessionTab * Tab = TabOfTerminal(T);
    if ((Tab != nullptr) && Connected(Tab))
    {
      Tab->RemotePanel->RefreshAfterOperation();
    }
  };
  H.ShowStatus = [this](const QString & Message, int Ms) { ShowStatusMessage(statusBar(), Message, Ms); };
  H.CurrentSelection = [this]()
  {
    EditorManager::Selection S;
    if (FTabs.isEmpty()) return S;
    SessionTab * Tab = CurrentTab();
    FilePanel * Panel = ActivePanel();
    if (Panel->IsRemote() && !Connected(Tab))
    {
      return S;
    }
    S.Term = Panel->IsRemote() ? Tab->Term : nullptr;
    QList<RemoteFile> Files = Panel->SelectedFiles();
    QStringList Paths = Panel->SelectedPaths();
    for (int I = 0; (I < Files.size()) && (I < Paths.size()); I++)
    {
      if (!Files[I].IsDirectory)
      {
        S.Paths.append(Paths[I]);
      }
    }
    return S;
  };
  H.Queue = [this](Terminal * T) -> TransferQueue *
  {
    SessionTab * Tab = TabOfTerminal(T);
    return (Tab != nullptr) ? Tab->Queue : nullptr;
  };
  FEditorManager->SetHooks(H);

  CreateActions();
  ConnectActions();
  FA->On("ReconnectSessionAction", [this]()
  {
    // ReconnectSessionAction: the last remote directory, the remembered password
    SessionTab * Tab = CurrentTab();
    if (!Tab->LocalBrowser && !Tab->Data.HostName.isEmpty() && !Connected(Tab) && !Tab->Connecting)
    {
      OpenSession(BeginReconnect(*Tab));
    }
  });
  LoadToolbarSettings();
  CreateMenus();
  CreateToolbars();
  CreateStatusBar();
  RebuildCustomCommands();

  SessionTab * Tab = CreateTab(-1, true);
  Tab->Implicit = true;
  if (!Config.WindowGeometry.isEmpty()) restoreGeometry(Config.WindowGeometry);
  // Version bumps when toolbars change, so an older saved layout is not applied
  if (!Config.WindowState.isEmpty()) restoreState(Config.WindowState, WindowStateVersion);
  if (!Config.PanelSplitter.isEmpty()) Tab->Page->restoreState(Config.PanelSplitter);
  ApplyToolbarStyle();
  ActivateTab(Tab);
  Tab->LocalPanel->FocusView();

  // Tray icon, taskbar progress, notifications (WP18)
  DesktopIntegration::InstallMainWindow(this);
  // Shared bookmark shortcuts (TCustomScpExplorerForm::KeyDown, OpenBookmark)
  LocationProfilesDialog::InstallBookmarkShortCuts(this, [this](const Bookmark & B) { OpenBookmark(B); });
  // Synchronize dialogs: queue, "Explore" of the checklist, open in new window
  SynchronizeUiHooks & SyncHooks = SynchronizeHooks();
  SyncHooks.Queue = [this](Terminal * T) -> TransferQueue *
  {
    SessionTab * Tab = TabOfTerminal(T);
    return (Tab != nullptr) ? Tab->Queue : nullptr;
  };
  SyncHooks.ExploreRemote = [this](Terminal * T, const QString & Directory, const QString & FileName)
  {
    if (SessionTab * Tab = TabOfTerminal(T))
    {
      ActivateTab(Tab);
      Tab->RemotePanel->ChangeDirectory(Directory, FileName);
      Tab->RemotePanel->FocusView();
    }
  };
  connect(&Config, &Configuration::Changed, this, &CommanderWindow::ApplyConfiguration);
  // NixSCP Pro: status bar indicator, conflict prompt, open tabs to the cloud
  CloudSyncUi::Attach(this);
  // Stale temporary folders of edited files (TemporaryDirectoryCleanup), once
  // per application start; only in the application itself (main.cpp names it
  // "NixSCP"), not in tools and tests that create a window
  static bool TemporaryDirectoryCleanupDone = false;
  if (!TemporaryDirectoryCleanupDone && (QCoreApplication::applicationName() == QStringLiteral("NixSCP")))
  {
    TemporaryDirectoryCleanupDone = true;
    QTimer::singleShot(0, this, [this]() { EditorManager::TemporaryDirectoryCleanup(this); });
  }
}

CommanderWindow::~CommanderWindow()
{
  // The destructors below run nested event loops (Terminal waits for its
  // worker): nothing may reach the half destroyed tabs any more
  disconnect(&Configuration::Instance(), nullptr, this, nullptr);
  QList<SessionTab *> Tabs = FTabs;
  FTabs.clear();
  for (SessionTab * Tab : Tabs)
  {
    Tab->LocalPanel->SetProvider(nullptr);
    Tab->RemotePanel->SetProvider(nullptr);
    if (Tab->Queue != nullptr)
    {
      Tab->Queue->disconnect(this);
    }
    if (Tab->Term != nullptr)
    {
      Tab->Term->disconnect(this);
    }
    delete Tab->Queue;
    Tab->Queue = nullptr;
    delete Tab->Term;
    Tab->Term = nullptr;
    delete Tab;
  }
  // Sessions and queues of closed tabs still waiting for deleteLater: a
  // Terminal destructor waits for its worker in a nested event loop, which
  // must not run while QWidget's destructor deletes the children
  // (one at a time: each destructor may run deferred deletes of the others)
  // The pending DeferredDelete is dropped first, or the nested event loop
  // of the destructor would delete the object a second time.
  while (auto * Queue = findChild<TransferQueue *>(QString(), Qt::FindDirectChildrenOnly))
  {
    QCoreApplication::removePostedEvents(Queue, QEvent::DeferredDelete);
    delete Queue;
  }
  while (auto * Term = findChild<Terminal *>(QString(), Qt::FindDirectChildrenOnly))
  {
    QCoreApplication::removePostedEvents(Term, QEvent::DeferredDelete);
    delete Term;
  }
}

//---------------------------------------------------------------------------
// Tabs (windows/TerminalManager.cpp, CustomScpExplorer SessionsPageControl)

SessionTab * CommanderWindow::CreateTab(int Index, bool LocalBrowser, const QString & LocalDirectory,
  const QString & OtherLocalDirectory)
{
  Configuration & Config = Configuration::Instance();
  auto * Tab = new SessionTab();
  Tab->LocalBrowser = LocalBrowser;
  Tab->LocalPanel = new FilePanel(false);
  Tab->RemotePanel = new FilePanel(!LocalBrowser);
  Tab->LocalProvider.ShowHiddenFiles = Config.ShowHiddenFiles;
  Tab->OtherLocalProvider.ShowHiddenFiles = Config.ShowHiddenFiles;
  Tab->LocalPanel->SetShowHiddenFiles(Config.ShowHiddenFiles);
  Tab->RemotePanel->SetShowHiddenFiles(Config.ShowHiddenFiles);
  Tab->LocalPanel->SetProvider(&Tab->LocalProvider);
  Tab->RemotePanel->SetProvider(LocalBrowser ? &Tab->OtherLocalProvider : nullptr);
  Tab->LastActivePanel = Tab->LocalPanel;

  Tab->Page = new QSplitter(Qt::Horizontal);
  Tab->Page->addWidget(Tab->LocalPanel);
  Tab->Page->addWidget(Tab->RemotePanel);
  Tab->Page->setChildrenCollapsible(false);
  if (!FTabs.isEmpty())
  {
    Tab->Page->restoreState(CurrentTab()->Page->saveState());
  }
  CreatePanelToolbars(Tab);

  for (FilePanel * Panel : {Tab->LocalPanel, Tab->RemotePanel})
  {
    connect(Panel, &FilePanel::Activated, this, [this, Tab, Panel]()
    {
      Tab->LastActivePanel = Panel;
      Tab->LocalPanel->SetActive(Panel == Tab->LocalPanel);
      Tab->RemotePanel->SetActive(Panel == Tab->RemotePanel);
      UpdateActions();
      UpdateTitle();
    });
    connect(Panel, &FilePanel::DirectoryChanged, this, [this, Tab, Panel]()
    {
      if (!FTabs.contains(Tab)) return;
      UpdateTabCaption(Tab);
      UpdateTitle();
      UpdateActions();
      PanelDirectoryChanged(Tab, Panel);
      if (FTabs.contains(Tab))
      {
        TransferPresetAutoSelect(Tab);
      }
    });
    connect(Panel, &FilePanel::SelectionChanged, this, &CommanderWindow::UpdateActions);
    connect(Panel, &FilePanel::OpenFileRequested, this, [this, Panel](const RemoteFile & File, const QString & FullPath)
    {
      OpenFile(Panel, File, FullPath);
    });
    // "Operation on double-click": Edit (default) and Copy
    connect(Panel, &FilePanel::EditRequested, this, [this, Panel](const RemoteFile & File, const QString & FullPath)
    {
      if (File.IsDirectory)
      {
        Panel->ChangeDirectory(FullPath);
        return;
      }
      Terminal * Term = PanelTerminal(Panel);
      if (Panel->IsRemote() && (Term == nullptr)) return;
      FEditorManager->ExecuteFiles(Term, {FullPath}, EditorManager::efDefaultEditor);
    });
    connect(Panel, &FilePanel::CopyRequested, this, [this, Tab, Panel](const RemoteFile & File, const QString & FullPath, bool NoConfirmation)
    {
      if (File.IsDirectory)
      {
        Panel->ChangeDirectory(FullPath);
        return;
      }
      FilePanel * Other = (Panel == Tab->LocalPanel) ? Tab->RemotePanel : Tab->LocalPanel;
      if (Tab->LocalBrowser)
      {
        LocalLocalCopy(Panel == Tab->RemotePanel, false);
      }
      else if (Connected(Tab))
      {
        TransferFiles({FullPath}, Panel->IsRemote(), Other->CurrentDirectory(), false, !NoConfirmation);
      }
    });
    connect(Panel, &FilePanel::PropertiesRequested, this, [this, Panel]() { Properties(Panel); });
    connect(Panel, &FilePanel::RenameRequested, this, [this, Panel](const QString & OldPath, const QString & NewName)
    {
      RenameFile(Panel, OldPath, NewName);
    });
    connect(Panel, &FilePanel::DropRequested, this,
      [this, Panel](const QStringList & Paths, bool FromRemote, const QString & TargetDirectory, Qt::DropAction Action)
    {
      HandleDrop(Panel, Paths, FromRemote, TargetDirectory, Action);
    });
    connect(Panel, &FilePanel::LoadFailed, this, [this, Tab, Panel](const QString & Error, const QString & More)
    {
      int Side = (Panel == Tab->LocalPanel) ? 0 : 1;
      if (FTabs.contains(Tab) && !Tab->SyncPending[Side].isEmpty())
      {
        // Synchronized browsing could not open the corresponding directory
        QString Path = Tab->SyncPending[Side];
        Tab->SyncPending[Side].clear();
        SynchronizeBrowsingFailed(Tab, Path, Panel->IsRemote(), Error + (More.isEmpty() ? QString() : QStringLiteral("\n") + More));
        return;
      }
      ShowErrorMessage(this, Error, More);
    });
    connect(Panel, &FilePanel::NewSessionRequested, this, &CommanderWindow::NewSession);
    connect(Panel, &FilePanel::FilterRequested, this, [this, Panel]() { Filter(Panel); });
    connect(Panel, &FilePanel::ToggleHiddenFilesRequested, this, &CommanderWindow::ToggleShowHiddenFiles);
    connect(Panel, &FilePanel::ContextMenuRequested, this, [this, Panel](const QPoint & GlobalPos, bool OnItem)
    {
      ShowFilePopup(Panel, GlobalPos, OnItem);
    });
    connect(Panel, &FilePanel::PanelMenuRequested, this, [this, Panel](const QPoint & GlobalPos)
    {
      ShowPanelPopup(Panel, GlobalPos);
    });
    // PathLabelDblClick (ScpCommander.cpp): Open Directory/Bookmark
    connect(Panel, &FilePanel::PathLabelDoubleClicked, this, [this, Panel]() { OpenDirectory(Panel, false); });
    Panel->View()->installEventFilter(this);
  }

  // Remote drag-out: files are downloaded when the drop target asks for them
  Tab->RemotePanel->SetRemoteDragDataFactory([this, Tab](const QStringList & Paths) -> QMimeData *
  {
    if (!Connected(Tab)) return nullptr;
    qint64 Size = 0;
    for (const RemoteFile & F : Tab->RemotePanel->SelectedFiles())
    {
      if (F.IsDirectory)
      {
        Size = -1;
        break;
      }
      Size += F.Size;
    }
    return ClipboardTransfer::CreateRemoteMimeData(Tab->Term, Paths, this, true, Size);
  });

  QString LocalDir = LocalDirectory;
  if (LocalDir.isEmpty())
  {
    LocalDir = FTabs.isEmpty() ? Config.LastLocalDirectory : CurrentTab()->LocalPanel->CurrentDirectory();
  }
  if (LocalDir.isEmpty() || !QFileInfo(LocalDir).isDir())
  {
    LocalDir = QDir::homePath();
  }
  QString OtherDir = OtherLocalDirectory;
  if (LocalBrowser && (OtherDir.isEmpty() || !QFileInfo(OtherDir).isDir()))
  {
    OtherDir = LocalDir;
  }

  if ((Index < 0) || (Index > FTabs.size()))
  {
    Index = FTabs.size();
  }
  FUpdatingTabs = true;
  FTabs.insert(Index, Tab);
  Pages->insertWidget(Index, Tab->Page);
  SessionTabs->insertTab(Index, QString());
  FUpdatingTabs = false;
  // Only once the tab is registered: loading emits DirectoryChanged
  Tab->LocalPanel->ChangeDirectory(LocalDir);
  if (LocalBrowser)
  {
    Tab->RemotePanel->ChangeDirectory(OtherDir);
  }
  UpdatePanelToolbars();
  if (FTabs.size() > 1)
  {
    // Toolbar text labels and icon size for the new panel toolbars
    ApplyToolbarStyle();
  }
  UpdateTabCaption(Tab);
  return Tab;
}

QString CommanderWindow::TabCaption(SessionTab * Tab) const
{
  if (!Tab->Name.isEmpty())
  {
    return Tab->Name;
  }
  const Configuration & Config = Configuration::Instance();
  if (Tab->LocalBrowser)
  {
    // TScpCommanderForm::GetLocalBrowserSessionTitle
    auto PathName = [&Config](const QString & Path)
    {
      QString Result = ShortPathName(Path, false);
      // TTerminalManager::GetPathForSessionTabName
      const int MaxPathLen = 16;
      if ((Config.SessionTabNameFormat == stnfShortPathTrunc) && (Result.size() > MaxPathLen))
      {
        Result = Result.left(MaxPathLen - 2) + QStringLiteral("…");
      }
      return Result;
    };
    return PathName(Tab->LocalPanel->CurrentDirectory()) + TitleSeparator + PathName(Tab->RemotePanel->CurrentDirectory());
  }
  // TTerminalManager::GetSessionTitle(Unique): sessions of the same name get the short path
  QString Result = Tab->Data.SessionName();
  if ((Config.SessionTabNameFormat != stnfNone) && Connected(Tab))
  {
    for (SessionTab * Other : FTabs)
    {
      if ((Other != Tab) && Connected(Other) && (Other->Data.SessionName().compare(Result, Qt::CaseInsensitive) == 0))
      {
        QString Path = ShortPathName(Tab->RemotePanel->CurrentDirectory(), true);
        if (!Path.isEmpty())
        {
          const int MaxPathLen = 16;
          if ((Config.SessionTabNameFormat == stnfShortPathTrunc) && (Path.size() > MaxPathLen))
          {
            Path = Path.left(MaxPathLen - 2) + QStringLiteral("…");
          }
          Result = QStringLiteral("%1 (%2)").arg(Result, Path);
        }
        break;
      }
    }
  }
  return Result;
}

// Session color of a tab (TCustomScpExplorerForm::UpdateSessionColor):
// a colored mark next to the tab icon
static QIcon ColoredTabIcon(const QIcon & Base, int Color)
{
  if (Color == 0)
  {
    return Base;
  }
  QPixmap Pixmap(16, 16);
  Pixmap.fill(Qt::transparent);
  QPainter Painter(&Pixmap);
  Base.paint(&Painter, QRect(0, 0, 16, 16));
  QColor C(Color & 0xFF, (Color >> 8) & 0xFF, (Color >> 16) & 0xFF);
  Painter.setPen(C.darker(130));
  Painter.setBrush(C);
  Painter.drawRoundedRect(QRectF(9.5, 9.5, 6, 6), 1.5, 1.5);
  Painter.end();
  return QIcon(Pixmap);
}

void CommanderWindow::UpdateTabCaption(SessionTab * Tab)
{
  // Connected, disconnected or another directory: the cloud session follows
  CloudSyncUi::SessionsChanged(this);
  int Index = TabIndex(Tab);
  if (Index < 0)
  {
    return;
  }
  QIcon Icon;
  QString Hint;
  if (Tab->LocalBrowser)
  {
    Icon = Icons::ForAction(QStringLiteral("NewLocalTabAction"));
    if (Icon.isNull()) Icon = style()->standardIcon(QStyle::SP_DirIcon);
    // TScpCommanderForm::GetTabHintDetails
    Hint = Tab->LocalPanel->CurrentDirectory() + QLatin1Char('\n') + Tab->RemotePanel->CurrentDirectory();
  }
  else
  {
    Icon = style()->standardIcon(Connected(Tab) ? QStyle::SP_DriveNetIcon : QStyle::SP_MessageBoxWarning);
    Hint = Tab->Data.SessionUrl();
    if (Connected(Tab))
    {
      Hint += QLatin1Char('\n') + Tab->RemotePanel->CurrentDirectory();
    }
  }
  QString Caption = TabCaption(Tab);
  // SessionTabCaptionTruncation: long captions are elided to fit
  SessionTabs->setElideMode(Configuration::Instance().SessionTabCaptionTruncation ? Qt::ElideMiddle : Qt::ElideNone);
  SessionTabs->setTabText(Index, QString(Caption).replace(QLatin1Char('&'), QStringLiteral("&&")));
  SessionTabs->setTabIcon(Index, ColoredTabIcon(Icon, Tab->Color));
  SessionTabs->setTabToolTip(Index, Hint);
}

int CommanderWindow::TabIndex(SessionTab * Tab) const
{
  return FTabs.indexOf(Tab);
}

SessionTab * CommanderWindow::CurrentTab() const
{
  int Index = Pages->currentIndex();
  return ((Index >= 0) && (Index < FTabs.size())) ? FTabs[Index] : FTabs.value(0);
}

SessionTab * CommanderWindow::TabOfTerminal(Terminal * Term) const
{
  for (SessionTab * Tab : FTabs)
  {
    if ((Term != nullptr) && (Tab->Term == Term))
    {
      return Tab;
    }
  }
  return nullptr;
}

void CommanderWindow::ActivateTab(SessionTab * Tab)
{
  int Index = TabIndex(Tab);
  if (Index < 0)
  {
    return;
  }
  FUpdatingTabs = true;
  Pages->setCurrentIndex(Index);
  SessionTabs->setCurrentIndex(Index);
  FUpdatingTabs = false;
  Tab->LocalPanel->SetActive(Tab->LastActivePanel == Tab->LocalPanel);
  Tab->RemotePanel->SetActive(Tab->LastActivePanel == Tab->RemotePanel);
  Tab->LastActivePanel->FocusView();
  FA->Get(QStringLiteral("SynchronizeBrowsingAction2"))->setChecked(Tab->SynchronizeBrowsing);
  UpdatePanelToolbars();
  UpdateQueueView();
  UpdateStatusBar();
  UpdateActions();
  UpdateTitle();
}

bool CommanderWindow::CanCloseQueue(SessionTab * Tab)
{
  // TCustomScpExplorerForm::CanCloseQueue (PENDING_QUEUE_ITEMS2)
  if (!Connected(Tab) || (Tab->Queue == nullptr) || Tab->Queue->IsEmpty() || (Tab->Queue->ActiveCount() == 0))
  {
    return true;
  }
  return (MessageDialog(this,
    QStringLiteral("**There are still some background transfers in queue. Do you want to disconnect anyway?**\n \n"
      "Warning: Pressing 'OK' will terminate all transfers immediately."),
    qtWarning, qaOK | qaCancel) == qaOK);
}

void CommanderWindow::CloseTab(SessionTab * Tab, bool KeepOne)
{
  if (Tab->Connecting || (Connected(Tab) && Tab->OperationRunning))
  {
    ShowStatusMessage(statusBar(), QStringLiteral("Cannot close the tab while an operation is in progress."), 3000);
    QApplication::beep();
    return;
  }
  if (!CanCloseQueue(Tab))
  {
    return;
  }
  DisconnectTab(Tab);
  int Index = TabIndex(Tab);
  if (Index < 0)
  {
    return;
  }
  FUpdatingTabs = true;
  FTabs.removeAt(Index);
  Pages->removeWidget(Tab->Page);
  SessionTabs->removeTab(Index);
  FUpdatingTabs = false;
  Tab->Page->deleteLater();
  delete Tab;
  CloudSyncUi::SessionsChanged(this);
  if (FTabs.isEmpty() && KeepOne)
  {
    // TScpCommanderForm::GetReplacementForLastSession: a local browser
    SessionTab * Replacement = CreateTab(-1, true);
    Replacement->Implicit = true;
    // TCustomScpExplorerForm::NeedSession: without "Keep main window open
    // when the last session is closed" the application terminates
    if (!Configuration::Instance().KeepOpenWhenNoSession)
    {
      QTimer::singleShot(0, this, &QWidget::close);
    }
  }
  if (!FTabs.isEmpty())
  {
    ActivateTab(FTabs[std::min<int>(Index, FTabs.size() - 1)]);
  }
}

void CommanderWindow::NewLocalTab(const QString & LocalDirectory, const QString & OtherLocalDirectory)
{
  // TTerminalManager::NewLocalSession
  SessionTab * Tab = CreateTab(TabIndex(CurrentTab()) + 1, true, LocalDirectory, OtherLocalDirectory);
  ActivateTab(Tab);
}

void CommanderWindow::NewTab(bool Remote)
{
  // TScpCommanderForm::NewTab
  if (Remote)
  {
    NewSession();
  }
  else
  {
    SessionTab * Tab = CurrentTab();
    QString Other = Tab->LocalBrowser ? Tab->RemotePanel->CurrentDirectory() : Tab->LocalPanel->CurrentDirectory();
    NewLocalTab(Tab->LocalPanel->CurrentDirectory(), Other);
  }
}

void CommanderWindow::NewTabFromCurrent(bool AllowReverse)
{
  bool Remote = FA->Get(QStringLiteral("DefaultToNewRemoteTabAction"))->isChecked();
  if (AllowReverse && (QApplication::keyboardModifiers() & Qt::ControlModifier))
  {
    Remote = !Remote;
  }
  NewTab(Remote);
}

void CommanderWindow::DuplicateTab()
{
  // TCustomScpExplorerForm::DuplicateTab
  SessionTab * Tab = CurrentTab();
  if (Tab->LocalBrowser)
  {
    NewLocalTab(Tab->LocalPanel->CurrentDirectory(), Tab->RemotePanel->CurrentDirectory());
    return;
  }
  if (Tab->Data.HostName.isEmpty())
  {
    return;
  }
  SessionData Data = Tab->Data;
  if (Connected(Tab))
  {
    Data.RemoteDirectory = Tab->RemotePanel->CurrentDirectory();
    if (Data.Password.isEmpty())
    {
      Data.Password = !Tab->SessionPassword.isEmpty() ? Tab->SessionPassword : Tab->Term->RememberedPassword();
    }
  }
  Data.LocalDirectory = Tab->LocalPanel->CurrentDirectory();
  SessionTab * New = CreateTab(TabIndex(Tab) + 1);
  ActivateTab(New);
  OpenSession(Data);
}

void CommanderWindow::RenameTab()
{
  // TCustomScpExplorerForm::RenameTab (RENAME_TAB_TITLE, RENAME_TAB_PROMPT)
  SessionTab * Tab = CurrentTab();
  bool Ok = false;
  // HELP_TAB_RENAME
  QString Name = HelpSystem::InputDialog(this, QStringLiteral("Rename tab"), QStringLiteral("&New tab name:"),
    TabCaption(Tab), QStringLiteral("ui_tabs#renaming"), &Ok);
  if (Ok)
  {
    Tab->Name = Name.trimmed();
    UpdateTabCaption(Tab);
  }
}

void CommanderWindow::ShowTabMenu(int Index, const QPoint & GlobalPos)
{
  QMenu Menu(this);
  Menu.setToolTipsVisible(true);
  if ((Index >= 0) && (Index < FTabs.size()))
  {
    ActivateTab(FTabs[Index]);
    if (FTabs[Index]->LocalBrowser)
    {
      // LocalBrowserPopup
      FA->Populate(&Menu, {
        CommanderActions::Item(QStringLiteral("CloseTabAction")),
        CommanderActions::Item(QStringLiteral("DuplicateTabAction")),
        CommanderActions::Item(QStringLiteral("RenameTabAction")),
        CommanderActions::Separator(),
        CommanderActions::ActionSubmenu(QStringLiteral("NewTabAction"), {
          CommanderActions::Item(QStringLiteral("NewRemoteTabAction")),
          CommanderActions::Item(QStringLiteral("NewLocalTabAction")),
          CommanderActions::Separator(),
          CommanderActions::Item(QStringLiteral("DefaultToNewRemoteTabAction"))}),
        CommanderActions::ExistingMenu(FSitesMenu),
        CommanderActions::Separator(),
        CommanderActions::Item(QStringLiteral("SessionsTabsAction2"))});
    }
    else
    {
      // SessionsPopup
      QMenu * Color = nullptr;
      FA->Populate(&Menu, {
        CommanderActions::Item(QStringLiteral("CloseTabAction")),
        CommanderActions::Item(QStringLiteral("DuplicateTabAction")),
        CommanderActions::Item(QStringLiteral("RenameTabAction")),
        CommanderActions::Separator(),
        CommanderActions::ActionSubmenu(QStringLiteral("ColorMenuAction2"), {}, &Color),
        CommanderActions::Separator(),
        CommanderActions::Item(QStringLiteral("DisconnectSessionAction")),
        CommanderActions::Item(QStringLiteral("ReconnectSessionAction")),
        CommanderActions::Item(QStringLiteral("SaveCurrentSessionAction2")),
        CommanderActions::Separator(),
        CommanderActions::Item(QStringLiteral("FileSystemInfoAction")),
        CommanderActions::Item(QStringLiteral("SessionGenerateUrlAction2")),
        CommanderActions::Separator(),
        CommanderActions::ActionSubmenu(QStringLiteral("NewTabAction"), {
          CommanderActions::Item(QStringLiteral("NewRemoteTabAction")),
          CommanderActions::Item(QStringLiteral("NewLocalTabAction")),
          CommanderActions::Separator(),
          CommanderActions::Item(QStringLiteral("DefaultToNewRemoteTabAction"))}),
        CommanderActions::ExistingMenu(FSitesMenu),
        CommanderActions::Separator(),
        CommanderActions::Item(QStringLiteral("SessionsTabsAction2"))});
      if (Color != nullptr)
      {
        connect(Color, &QMenu::aboutToShow, this, [this, Color]() { FillColorMenu(Color); });
        Color->setEnabled(!CurrentTab()->LocalBrowser);
      }
    }
  }
  else
  {
    // NewTabPopup (the "New Tab" pseudo tab)
    FA->Populate(&Menu, {
      CommanderActions::Item(QStringLiteral("NewRemoteTabAction")),
      CommanderActions::Item(QStringLiteral("NewLocalTabAction")),
      CommanderActions::Separator(),
      CommanderActions::Item(QStringLiteral("DefaultToNewRemoteTabAction"))});
  }
  UpdateActions();
  Menu.exec(GlobalPos);
}

void CommanderWindow::FillColorMenu(QMenu * Menu)
{
  Menu->clear();
  SessionTab * Tab = CurrentTab();
  SiteAdvancedDialog::PopulateSessionColorMenu(Menu, Tab->Color, [this, Tab](int Color)
  {
    if (!FTabs.contains(Tab)) return;
    // TCustomScpExplorerForm::SessionColorPick: the color of the tab (not saved to the site)
    Tab->Color = Color;
    UpdateTabCaption(Tab);
  });
}

void CommanderWindow::FillOpenedTabsMenu(QMenu * Menu)
{
  // TNonVisualDataModule::CreateOpenedSessionListMenu (radio items, Alt+1..9, Alt+0)
  Menu->clear();
  for (int Index = 0; Index < FTabs.size(); Index++)
  {
    QAction * Action = (Index < FSelectTabActions.size()) ? FSelectTabActions[Index] : nullptr;
    QString Caption = TabCaption(FTabs[Index]);
    if (Action != nullptr)
    {
      Action->setText(QString(Caption).replace(QLatin1Char('&'), QStringLiteral("&&")));
      Action->setChecked(FTabs[Index] == CurrentTab());
      CommanderActions::SetHint(Action, QStringLiteral("Select tab '%1'").arg(Caption));
      Menu->addAction(Action);
    }
    else
    {
      SessionTab * Tab = FTabs[Index];
      QAction * Item = Menu->addAction(QString(Caption).replace(QLatin1Char('&'), QStringLiteral("&&")), this,
        [this, Tab]() { if (FTabs.contains(Tab)) ActivateTab(Tab); });
      Item->setCheckable(true);
      Item->setChecked(Tab == CurrentTab());
    }
  }
}

void CommanderWindow::FillSitesMenu(QMenu * Menu)
{
  // TNonVisualDataModule::CreateSessionListMenu (Sites): stored sites by folder, Site Manager
  Menu->clear();
  Menu->setToolTipsVisible(true);
  QMap<QString, QMenu *> Folders;
  for (const auto & Data : Configuration::Instance().StoredSessions().Sessions())
  {
    if (Data->IsWorkspace) continue;
    QMenu * Parent = Menu;
    QStringList Parts = Data->Name.split(QLatin1Char('/'));
    QString Path;
    for (int I = 0; I + 1 < Parts.size(); I++)
    {
      Path += Parts[I] + QLatin1Char('/');
      if (!Folders.contains(Path))
      {
        QMenu * Folder = Parent->addMenu(Icons::Get(QStringLiteral("site-folder")), QString(Parts[I]).replace(QLatin1Char('&'), QStringLiteral("&&")));
        QString FolderName = Path.chopped(1);
        Folders.insert(Path, Folder);
        // SAVEDSESSIONFOLDER_THIS_OPEN: open all sites of the folder
        QAction * OpenAll = Folder->addAction(QStringLiteral("Open all sites of the folder"), this, [this, FolderName]()
        {
          OpenWorkspace(FolderName);
        });
        Folder->addSeparator();
        Q_UNUSED(OpenAll);
      }
      Parent = Folders[Path];
    }
    SessionData Copy = *Data;
    QAction * Item = Parent->addAction(LoginDialog::SiteIcon(*Data), QString(Parts.last()).replace(QLatin1Char('&'), QStringLiteral("&&")),
      this, [this, Copy]() { OpenSession(Copy); });
    Item->setToolTip(Data->InfoTip());
  }
  if (Menu->isEmpty())
  {
    Menu->addAction(QStringLiteral("(no sites)"))->setEnabled(false);
  }
  Menu->addSeparator();
  Menu->addAction(FA->Get(QStringLiteral("SiteManagerAction")));
}

void CommanderWindow::FillWorkspacesMenu(QMenu * Menu)
{
  // TNonVisualDataModule::CreateWorkspacesMenu
  Menu->clear();
  QStringList Names = Workspaces::WorkspaceNames(Configuration::Instance().StoredSessions());
  for (const QString & Name : Names)
  {
    Menu->addAction(Icons::Get(QStringLiteral("workspace")), QString(Name).replace(QLatin1Char('&'), QStringLiteral("&&")),
      this, [this, Name]() { OpenWorkspace(Name); });
  }
  if (Names.isEmpty())
  {
    Menu->addAction(QStringLiteral("(no workspaces)"))->setEnabled(false);
  }
}

//---------------------------------------------------------------------------
// Panels and sides

FilePanel * CommanderWindow::ActivePanel() const
{
  SessionTab * Tab = CurrentTab();
  return Tab->LastActivePanel ? Tab->LastActivePanel : Tab->LocalPanel;
}

FilePanel * CommanderWindow::OtherPanel(FilePanel * Panel) const
{
  SessionTab * Tab = CurrentTab();
  return (Panel == Tab->LocalPanel) ? Tab->RemotePanel : Tab->LocalPanel;
}

FilePanel * CommanderWindow::SidePanel(bool Remote) const
{
  return Remote ? CurrentTab()->RemotePanel : CurrentTab()->LocalPanel;
}

bool CommanderWindow::IsLocalBrowserMode() const
{
  return !FTabs.isEmpty() && CurrentTab()->LocalBrowser;
}

Terminal * CommanderWindow::PanelTerminal(FilePanel * Panel) const
{
  for (SessionTab * Tab : FTabs)
  {
    if ((Tab->RemotePanel == Panel) && Panel->IsRemote() && Connected(Tab))
    {
      return Tab->Term;
    }
  }
  return nullptr;
}

bool CommanderWindow::Connected(const SessionTab * Tab) const
{
  if (Tab == nullptr)
  {
    if (FTabs.isEmpty()) return false;
    Tab = CurrentTab();
  }
  return (Tab != nullptr) && (Tab->Term != nullptr) && Tab->Term->Active();
}

bool CommanderWindow::PanelReady(FilePanel * Panel) const
{
  return (Panel != nullptr) && (Panel->Provider() != nullptr) && (!Panel->IsRemote() || (PanelTerminal(Panel) != nullptr));
}

bool CommanderWindow::CheckReady()
{
  if (CurrentTab()->OperationRunning)
  {
    ShowStatusMessage(statusBar(), QStringLiteral("Another operation is in progress."), 3000);
    QApplication::beep();
    return false;
  }
  return true;
}

bool CommanderWindow::eventFilter(QObject * Object, QEvent * Event)
{
  if (Event->type() == QEvent::KeyPress)
  {
    auto * Key = static_cast<QKeyEvent *>(Event);
    if ((Key->key() == Qt::Key_Tab) && (Key->modifiers() == Qt::NoModifier) && !FTabs.isEmpty())
    {
      SessionTab * Tab = CurrentTab();
      FilePanel * Other = nullptr;
      if (Object == Tab->LocalPanel->View()) Other = Tab->RemotePanel;
      else if (Object == Tab->RemotePanel->View()) Other = Tab->LocalPanel;
      if (Other != nullptr)
      {
        if (Other->Provider() != nullptr)
        {
          Other->FocusView();
        }
        return true;
      }
    }
  }
  else if ((Event->type() == QEvent::Shortcut) && (qobject_cast<QAction *>(Object) != nullptr))
  {
    // TNonVisualDataModule::ExplorerActionsExecute ExecutedByShortcut
    // (ActionComponent == NULL): the Copy dialog offers the F5 hint
    FExecutedByShortcut = true;
    QTimer::singleShot(0, this, [this]() { FExecutedByShortcut = false; });
  }
  else if ((Event->type() == QEvent::MouseButtonDblClick) && (QueueSplitter->count() > 1) && (Object == QueueSplitter->handle(1)))
  {
    // Double click on the queue splitter: ToggleQueueVisibility
    FQueuePanel->ToggleVisibility();
    UpdateQueueView();
    return true;
  }
  else if ((Event->type() == QEvent::MouseButtonPress) && (Object == SessionTabs))
  {
    // Middle click closes the tab (SessionsPageControlMouseDown)
    auto * Mouse = static_cast<QMouseEvent *>(Event);
    if (Mouse->button() == Qt::MiddleButton)
    {
      int Index = SessionTabs->tabAt(Mouse->position().toPoint());
      if ((Index >= 0) && (Index < FTabs.size()))
      {
        SessionTab * Tab = FTabs[Index];
        QTimer::singleShot(0, this, [this, Tab]() { if (FTabs.contains(Tab)) CloseTab(Tab); });
        return true;
      }
    }
  }
  return QMainWindow::eventFilter(Object, Event);
}

void CommanderWindow::changeEvent(QEvent * Event)
{
  QMainWindow::changeEvent(Event);
}

//---------------------------------------------------------------------------
// Status bar, title, queue view

void CommanderWindow::CreateStatusBar()
{
  // SessionStatusBar panels: security, protocol, duration
  StatusSecureLabel = new QLabel();
  StatusProtocolLabel = new QLabel();
  StatusDurationLabel = new QLabel();
  StatusSecureLabel->setObjectName(QStringLiteral("SecurityStatusPanel"));
  StatusProtocolLabel->setObjectName(QStringLiteral("ProtocolStatusPanel"));
  StatusDurationLabel->setObjectName(QStringLiteral("DurationStatusPanel"));
  statusBar()->addPermanentWidget(TunnelsUi::CreateIndicator(this));
  statusBar()->addPermanentWidget(StatusSecureLabel);
  statusBar()->addPermanentWidget(StatusProtocolLabel);
  statusBar()->addPermanentWidget(StatusDurationLabel);
  DurationTimer = new QTimer(this);
  DurationTimer->setInterval(1000);
  connect(DurationTimer, &QTimer::timeout, this, &CommanderWindow::UpdateStatusBar);
  DurationTimer->start();
}

// TSessionInfo::SecurityProtocolName: SSH-2 for SFTP/SCP, the TLS mode for
// FTPS/WebDAV/S3 over TLS; empty = not encrypted
static QString SecurityProtocolName(const SessionData & Data)
{
  switch (Data.FSProtocol)
  {
    case fsSCPonly:
    case fsSFTP:
    case fsSFTPonly:
      return QStringLiteral("SSH-2");
    default:
      if (Data.Ftps == ftpsImplicit)
      {
        return QStringLiteral("TLS/SSL Implicit encryption");
      }
      else if ((Data.Ftps == ftpsExplicitSsl) || (Data.Ftps == ftpsExplicitTls))
      {
        return QStringLiteral("TLS/SSL Explicit encryption");
      }
      return QString();
  }
}

void CommanderWindow::UpdateStatusBar()
{
  // TCustomScpExplorerForm::InitStatusBar
  if (FTabs.isEmpty())
  {
    return;
  }
  SessionTab * Tab = CurrentTab();
  if (Connected(Tab))
  {
    QString Security = SecurityProtocolName(Tab->Data);
    // STATUS_SECURE / STATUS_INSECURE
    QString SecurityText = Security.isEmpty() ? QStringLiteral("Insecure connection") :
      QStringLiteral("Secure connection (%1)").arg(Security);
    StatusSecureLabel->setText(SecurityText);
    StatusSecureLabel->setToolTip(SecurityText);
    StatusSecureLabel->setEnabled(!Security.isEmpty());
    StatusSecureLabel->setVisible(true);
    StatusProtocolLabel->setText(Tab->Term->ProtocolName());
    StatusProtocolLabel->setToolTip(QStringLiteral("Protocol in use"));
    qint64 Secs = Tab->SessionTimer.elapsed() / 1000;
    StatusDurationLabel->setText(QStringLiteral("%1:%2:%3").arg(Secs / 3600)
      .arg((Secs / 60) % 60, 2, 10, QLatin1Char('0')).arg(Secs % 60, 2, 10, QLatin1Char('0')));
    StatusDurationLabel->setToolTip(QStringLiteral("Connection time"));
    StatusDurationLabel->setVisible(true);
  }
  else
  {
    StatusSecureLabel->clear();
    StatusSecureLabel->setVisible(false);
    StatusDurationLabel->clear();
    StatusDurationLabel->setVisible(false);
    if (Tab->LocalBrowser)
    {
      StatusProtocolLabel->clear();
    }
    else
    {
      // STATUS_CONNECTING / STATUS_NOT_CONNECTED2
      StatusProtocolLabel->setText(Tab->Connecting ? QStringLiteral("Connecting...") : QStringLiteral("Not connected"));
    }
    StatusProtocolLabel->setToolTip(QString());
  }
}

void CommanderWindow::UpdateQueueView()
{
  // TCustomScpExplorerForm::UpdateQueueView
  if (FTabs.isEmpty())
  {
    return;
  }
  SessionTab * Tab = CurrentTab();
  TransferQueue * Queue = Connected(Tab) ? Tab->Queue : nullptr;
  if (FQueuePanel->Queue() != Queue)
  {
    FQueuePanel->SetQueue(Queue);
  }
  bool Visible = Connected(Tab) && FQueuePanel->ShouldBeVisible();
  if (Visible && !FQueuePanel->isVisible())
  {
    int Height = QueuePanel::ConfiguredHeight();
    FQueuePanel->setVisible(true);
    QList<int> Sizes = QueueSplitter->sizes();
    if ((Sizes.size() == 2) && (Height > 0))
    {
      int Total = Sizes[0] + Sizes[1];
      QueueSplitter->setSizes({std::max(Total - Height, 0), Height});
    }
  }
  else if (!Visible && FQueuePanel->isVisible())
  {
    FQueuePanel->setVisible(false);
  }
}

void CommanderWindow::TransferPresetAutoSelect(SessionTab * Tab)
{
  if ((Tab != CurrentTab()) || Tab->LocalBrowser || !Connected(Tab))
  {
    return;
  }
  CopyParamPresets & Presets = CopyParamPresets::Instance();
  if (!Presets.List().AnyRule())
  {
    return;
  }
  // GetTransferPresetAutoSelectData (TScpCommanderForm adds the local directory)
  CopyParamRuleData Data;
  Data.HostName = Tab->Term->Data().HostNameExpanded();
  Data.UserName = Tab->Term->Data().UserNameExpanded();
  Data.RemoteDirectory = Tab->RemotePanel->CurrentDirectory();
  Data.LocalDirectory = Tab->LocalPanel->CurrentDirectory();
  // UsableCopyParamAttrs(0).General
  int CopyParamAttrs = CopyParam::UsableAttrs(true, Tab->Term) & CopyParam::UsableAttrs(false, Tab->Term);
  QString Message;
  QString Info;
  if (!Presets.AutoSelect(Data, CopyParamAttrs, &Message, &Info))
  {
    return;
  }
  UpdateTransferCombo();
  Configuration & Config = Configuration::Instance();
  if (Config.CopyParamAutoSelectNotice)
  {
    // TransferPresetNoteMessage
    MessageParams Params;
    Params.Params = qpNeverAskAgainCheck;
    unsigned int Answer = MessageDialog(this, QStringLiteral("**%1**\n\n%2").arg(Message, Info), qtInformation, qaOK,
      QStringLiteral("transfer_settings#presets"), &Params);
    if (Answer == qaNeverAskAgain)
    {
      Config.CopyParamAutoSelectNotice = false;
      Config.Save();
    }
  }
  else
  {
    // PostNote
    ShowStatusMessage(statusBar(), Message, 10000);
  }
}

void CommanderWindow::UpdateTitle()
{
  if (FTabs.isEmpty())
  {
    return;
  }
  // TTerminalManager::UpdateAppTitle: "<path> - <session> - WinSCP"
  SessionTab * Tab = CurrentTab();
  // PathForCaption: Preferences > Window > "Path in window title"
  TPathInCaption PathInCaption = Configuration::Instance().PathInCaption;
  auto PathForCaption = [PathInCaption](const QString & Path, bool Unix)
  {
    switch (PathInCaption)
    {
      case picFull:
        return Path;
      case picShort:
        return ShortPathName(Path, Unix);
      default:
        return QString();
    }
  };
  QString Title;
  if (Connected(Tab))
  {
    QString Dir = PathForCaption(Tab->RemotePanel->CurrentDirectory(), true);
    Title = (Dir.isEmpty() ? QString() : (Dir + TitleSeparator)) + TabCaption(Tab) + TitleSeparator + QStringLiteral("NixSCP");
  }
  else
  {
    QString Dir = PathForCaption(ActivePanel()->CurrentDirectory(), false);
    Title = Dir.isEmpty() ? QStringLiteral("NixSCP") : (Dir + TitleSeparator + QStringLiteral("NixSCP"));
  }
  setWindowTitle(Title);
}

//---------------------------------------------------------------------------
// Sessions

void CommanderWindow::NewSession()
{
  Configuration & Config = Configuration::Instance();
  LoginDialog Dialog(Config.StoredSessions(), this);
  if (Dialog.exec() != QDialog::Accepted)
  {
    return;
  }
  if (!Dialog.WorkspaceToOpen().isEmpty())
  {
    OpenWorkspace(Dialog.WorkspaceToOpen());
    return;
  }
  const QList<SessionData> Sessions = Dialog.SessionsToOpen();
  for (const SessionData & Data : Sessions)
  {
    OpenSession(Data);
  }
}

void CommanderWindow::OpenSession(const SessionData & Data)
{
  // Reuse current tab if it has no session, otherwise open a new tab (as WinSCP)
  SessionTab * Tab = CurrentTab();
  // Reconnecting the session of this tab: reuse the password typed before
  // (TTerminal::GetRememberedPassword)
  QString RememberedPassword;
  if (!Tab->LocalBrowser && !Connected(Tab) && !Tab->Connecting && SameSessionTarget(Tab->Data, Data))
  {
    RememberedPassword = Tab->SessionPassword;
  }
  if (Tab->LocalBrowser)
  {
    // A session never replaces a local browser tab; the implicit one (no
    // session was open) is closed (TTerminalManager::NewSession ReplaceExisting)
    SessionTab * Implicit = Tab->Implicit ? Tab : nullptr;
    Tab = CreateTab(TabIndex(Tab) + 1, false, Tab->LocalPanel->CurrentDirectory());
    if (Implicit != nullptr)
    {
      ActivateTab(Tab);
      CloseTab(Implicit, false);
    }
  }
  else if ((Tab->Term != nullptr) || Tab->Connecting)
  {
    if (!Connected(Tab) && !Tab->Connecting)
    {
      // Disconnected tab (e.g. reconnect): reuse it
      DisconnectTab(Tab);
    }
    else
    {
      Tab = CreateTab(TabIndex(CurrentTab()) + 1);
    }
  }
  ActivateTab(Tab);

  Tab->Data = Data;
  Tab->Color = Data.Color;
  Tab->SynchronizeBrowsing = Data.SynchronizeBrowsing;
  Tab->Connecting = true;
  FUI->TakeLastPassword();
  Terminal * Term = new Terminal(Data, FUI, this);
  // Preferences: Endurance (automatic reconnect), Continue on error, remembering passwords
  Term->SetSettings(Terminal::ConfigurationSettings());
  if (!RememberedPassword.isEmpty())
  {
    Term->SetRememberedPassword(RememberedPassword);
  }
  Tab->Term = Term;
  UpdateTabCaption(Tab);
  // Connection lost while idle, reconnecting during an operation failed, or a
  // reconnect failed: the error with Reconnect (TTerminalManager /
  // ShowExtendedException). Counts down SessionReopenAutoIdle to reconnecting
  // automatically, while the series of reconnects is within SessionReopenTimeout.
  // (Declined: the user refused to reconnect during the operation, no countdown then)
  auto QueryReconnect = [this](SessionTab * Tab, const QString & Message, const QString & More, bool Declined)
  {
    if (!FTabs.contains(Tab) || Connected(Tab) || Tab->Connecting) return;
    const Configuration & Config = Configuration::Instance();
    qint64 ReopenStart = Tab->Page->property(ReopenStartProperty).toLongLong();
    qint64 Now = QDateTime::currentMSecsSinceEpoch();
    bool WithinTimeout = (Config.SessionReopenTimeout <= 0) || (ReopenStart == 0) || (Now - ReopenStart < Config.SessionReopenTimeout);
    int Timeout = (WithinTimeout && Config.SessionReopenAutoIdleOn && !Declined) ? Config.SessionReopenAutoIdle : 0;
    bool Reconnect = ConnectionLostDialog(this, Message, More, Timeout);
    if (!FTabs.contains(Tab) || Connected(Tab) || Tab->Connecting) return;
    if (Reconnect)
    {
      ActivateTab(Tab);
      OpenSession(BeginReconnect(*Tab));
    }
    else
    {
      EndReconnect(*Tab);
    }
  };
  connect(Term, &Terminal::Disconnected, this, [this, Term, QueryReconnect](const QString & Message, const QString & More)
  {
    SessionTab * Tab = TabOfTerminal(Term);
    if (Tab == nullptr) return;
    bool Declined = Term->ReopenDeclined();
    SessionClosed(Tab);
    if (Tab == CurrentTab())
    {
      QueryReconnect(Tab, Message, More, Declined);
    }
    else
    {
      // Inactive tab: the message is shown when the tab gets activated; with
      // SessionReopenAutoInactive it reconnects right away then
      DesktopIntegration::Notify(Tab->Data.SessionName(), Message, DesktopIntegration::ntError);
      auto * Pending = new QObject(this);
      connect(SessionTabs, &QTabBar::currentChanged, Pending, [this, Tab, QueryReconnect, Pending, Message, More, Declined]()
      {
        if (!FTabs.contains(Tab))
        {
          Pending->deleteLater();
          return;
        }
        if (Tab != CurrentTab()) return;
        Pending->deleteLater();
        if (Connected(Tab) || Tab->Connecting) return;
        if (Configuration::Instance().SessionReopenAutoInactive)
        {
          OpenSession(BeginReconnect(*Tab));
        }
        else
        {
          QTimer::singleShot(0, this, [QueryReconnect, Tab, Message, More, Declined]() { QueryReconnect(Tab, Message, More, Declined); });
        }
      });
    }
  });

  // Authenticate window (TTerminalManager::AuthenticateFormCancel): the
  // connection log and the prompts; closing it aborts the connecting
  ShowStatusMessage(statusBar(), QStringLiteral("Connecting..."));
  FUI->BeginConnecting(Data, [this, Term]()
  {
    // The worker cannot be interrupted inside connect(); the session is discarded once it returns
    if (SessionTab * Tab = TabOfTerminal(Term))
    {
      Tab->Term = nullptr;
      Tab->Connecting = false;
      Term->Close();
      ShowStatusMessage(statusBar(), QStringLiteral("Aborted"), 3000);
      UpdateTabCaption(Tab);
      UpdateActions();
      UpdateStatusBar();
    }
  });

  Term->Open([this, Term, QueryReconnect](const OperationResult & Result)
  {
    FUI->EndConnecting();
    SessionTab * Tab = TabOfTerminal(Term);
    if (Tab == nullptr)
    {
      Term->deleteLater();
      return;
    }
    Tab->Connecting = false;
    statusBar()->clearMessage();
    if (!Result.Ok)
    {
      Tab->Term = nullptr;
      Term->deleteLater();
      UpdateTabCaption(Tab);
      if (Result.Aborted)
      {
        EndReconnect(*Tab);
      }
      else if (Tab->Page->property(ReconnectingProperty).toBool())
      {
        // A failed reconnect offers to reconnect again
        QString Message = Result.Message;
        QString More = Result.MoreMessages;
        QTimer::singleShot(0, this, [QueryReconnect, Tab, Message, More]() { QueryReconnect(Tab, Message, More, false); });
      }
      else
      {
        DesktopIntegration::FlashOnBackground(this);
        ShowErrorMessage(this, Result.Message, Result.MoreMessages);
        if (!Tab->Page->property(EverConnectedProperty).toBool() && FTabs.contains(Tab))
        {
          // The tab of a session that never opened is freed (and replaced by a
          // local browser when it was the last one), as WinSCP
          SessionData Failed = Tab->Data;
          CloseTab(Tab);
          Q_UNUSED(Failed);
        }
        // As WinSCP: return to Login dialog after failed connect
        QTimer::singleShot(0, this, &CommanderWindow::NewSession);
      }
      UpdateActions();
      UpdateStatusBar();
      return;
    }
    EndReconnect(*Tab);
    Tab->Page->setProperty(EverConnectedProperty, true);
    // Background transfers use own connections; pass on a password typed at the prompt
    SessionData QueueData = Tab->Data;
    QString Typed = FUI->TakeLastPassword();
    bool RememberThis = FUI->TakeSessionRememberPassword();
    if (Typed.isEmpty())
    {
      // typed before and reused by a reconnect
      Typed = Term->RememberedPassword();
    }
    else if (RememberThis)
    {
      Term->SetRememberedPassword(Typed);
    }
    if (QueueData.Password.isEmpty())
    {
      QueueData.Password = Typed;
    }
    const Configuration & Config = Configuration::Instance();
    if (Config.SessionRememberPassword || Config.PuttyPassword || RememberThis)
    {
      // Kept in memory for the session duration only (reconnect, terminal client)
      Tab->SessionPassword = QueueData.Password;
    }
    Tab->Queue = new TransferQueue(QueueData, FUI, this);
    Tab->Queue->SetTransfersLimit(Config.QueueTransfersLimit);
    if (!QueueData.PublicKeyFile.isEmpty() && !Typed.isEmpty() && Tab->Data.Password.isEmpty())
    {
      // The passphrase typed at login also opens the key for the queue connections
      Tab->Queue->SetRememberedPassword(Typed, UserInterface::pkPassphrase);
    }
    connect(Tab->Queue, &TransferQueue::Changed, this, [this, Tab]()
    {
      if (FTabs.contains(Tab) && (Tab == CurrentTab())) UpdateQueueView();
    });
    connect(Tab->Queue, &TransferQueue::ItemFinished, this, [this, Tab](int, const OperationResult & Result, bool, const QString &, bool)
    {
      if (!FTabs.contains(Tab)) return;
      if (!Result.Ok && !Result.Aborted)
      {
        ShowStatusMessage(statusBar(), QStringLiteral("Background transfer failed: %1").arg(Result.Message), 5000);
      }
      RefreshTab(Tab);
    });
    connect(Tab->Queue, &TransferQueue::UserActionPending, this, [this, Tab](int Id)
    {
      QueueUserActionPending(Tab, Id);
    });
    connect(Tab->Queue, &TransferQueue::Emptied, this, [this, Tab]()
    {
      // BALLOON_QUEUE_EMPTY (TCustomScpExplorerForm::QueueEvent): only when the
      // main window is minimized (or in the tray) or the queue belongs to another
      // tab - not merely when another window (e.g. the external editor) is active
      bool Minimized = isMinimized() || DesktopIntegration::IsMinimizedToTray();
      if (FTabs.contains(Tab) && Configuration::Instance().BalloonNotifications &&
          (Minimized || (Tab != CurrentTab())))
      {
        DesktopIntegration::Notify(QString(), QStringLiteral("**Transfer queue is empty.**"));
      }
    });

    Tab->RemoteProvider = std::make_unique<RemoteDirectoryProvider>(Term);
    Tab->RemoteProvider->ShowHiddenFiles = Configuration::Instance().ShowHiddenFiles;
    Tab->RemotePanel->SetProvider(Tab->RemoteProvider.get());
    Tab->RemotePanel->ChangeDirectory(Term->CurrentDirectory());
    if (!Tab->Data.LocalDirectory.isEmpty() && QFileInfo(Tab->Data.LocalDirectory).isDir())
    {
      Tab->LocalPanel->ChangeDirectory(Tab->Data.LocalDirectory);
    }
    Tab->SessionTimer.start();
    Tab->LastActivePanel = Tab->RemotePanel;
    UpdateTabCaption(Tab);
    if (Tab == CurrentTab())
    {
      ActivateTab(Tab);
    }
    // Preferences > Integration > Applications: Automatically open new sessions in PuTTY
    if (Config.AutoOpenInPutty && (Tab == CurrentTab()))
    {
      OpenInPutty();
    }
  });
  UpdateActions();
  UpdateStatusBar();
}

void CommanderWindow::SessionClosed(SessionTab * Tab)
{
  // Reconnect Session returns to the last remote directory
  if (Tab->RemoteProvider && !Tab->RemotePanel->CurrentDirectory().isEmpty())
  {
    Tab->Page->setProperty(LastRemoteDirectoryProperty, Tab->RemotePanel->CurrentDirectory());
  }
  else if ((Tab->Term != nullptr) && !Tab->Term->CurrentDirectory().isEmpty())
  {
    Tab->Page->setProperty(LastRemoteDirectoryProperty, Tab->Term->CurrentDirectory());
  }
  if (Tab->LocalBrowser)
  {
    return;
  }
  // Panel must not keep using a provider of a dead terminal
  Tab->RemotePanel->SetProvider(nullptr);
  Tab->RemoteProvider.reset();
  if (Tab->Queue != nullptr)
  {
    if (FQueuePanel->Queue() == Tab->Queue)
    {
      FQueuePanel->SetQueue(nullptr);
    }
    Tab->Queue->deleteLater();
    Tab->Queue = nullptr;
  }
  if (Tab->Term != nullptr)
  {
    Tab->Term->deleteLater();
    // Keep Data for Reconnect, drop the terminal
    Tab->Term = nullptr;
  }
  Tab->OperationRunning = false;
  if (Tab->LastActivePanel == Tab->RemotePanel)
  {
    Tab->LastActivePanel = Tab->LocalPanel;
  }
  UpdateTabCaption(Tab);
  if (Tab == CurrentTab())
  {
    Tab->LocalPanel->SetActive(true);
    Tab->RemotePanel->SetActive(false);
    UpdateQueueView();
    UpdateStatusBar();
    UpdateActions();
    UpdateTitle();
  }
}

void CommanderWindow::DisconnectTab(SessionTab * Tab)
{
  if (Tab->Term != nullptr)
  {
    Tab->Term->Close();
  }
  SessionClosed(Tab);
}

void CommanderWindow::RefreshTab(SessionTab * Tab)
{
  // After an operation: AutoReadDirectoryAfterOp is honoured for remote panels
  Tab->LocalPanel->RefreshAfterOperation();
  if (Tab->LocalBrowser || Connected(Tab))
  {
    Tab->RemotePanel->RefreshAfterOperation();
  }
}

void CommanderWindow::SaveCurrentSession()
{
  SessionTab * Tab = CurrentTab();
  if (!Connected(Tab)) return;
  bool Ok = false;
  // HELP_SESSION_SAVE
  QString Name = HelpSystem::InputDialog(this, QStringLiteral("Save session as site"), QStringLiteral("&Site name:"),
    Tab->Data.SessionName(), QStringLiteral("ui_login_save"), &Ok).trimmed();
  if (!Ok || Name.isEmpty())
  {
    return;
  }
  auto Data = std::make_shared<SessionData>(Tab->Data);
  Data->Name = Name;
  Data->Password.clear();
  Data->RemoteDirectory = Tab->RemotePanel->CurrentDirectory();
  // TScpCommanderForm::UpdateSessionData: the state of the Commander
  Data->LocalDirectory = Tab->LocalPanel->CurrentDirectory();
  Data->SynchronizeBrowsing = Tab->SynchronizeBrowsing;
  Data->Color = Tab->Color;
  Configuration::Instance().StoredSessions().Add(Data);
  Configuration::Instance().Save();
}

QString CommanderWindow::HostKeyForCode(SessionTab * Tab) const
{
  // SessionDataForCode: '<Host key algorithm> <Host key fingerprint (SHA-256)>'
  QString Algorithm;
  QString Fingerprint;
  if (Tab->Term != nullptr)
  {
    for (const QString & Line : Tab->Term->ServerInfo().split(QLatin1Char('\n')))
    {
      if (Line.startsWith(QStringLiteral("Host key algorithm: ")))
      {
        Algorithm = Line.mid(20).trimmed();
      }
      else if (Line.startsWith(QStringLiteral("Host key fingerprint (SHA-256): ")))
      {
        Fingerprint = Line.mid(32).trimmed();
      }
    }
  }
  return (Algorithm.isEmpty() || Fingerprint.isEmpty()) ? QString() : (Algorithm + QLatin1Char(' ') + Fingerprint);
}

void CommanderWindow::GenerateSessionUrl()
{
  // TCustomScpExplorerForm::SessionGenerateUrl (SessionDataForCode)
  SessionTab * Tab = CurrentTab();
  if (Tab->LocalBrowser || Tab->Data.HostName.isEmpty()) return;
  SessionData Data = Tab->Data;
  if (Connected(Tab))
  {
    Data.RemoteDirectory = Tab->RemotePanel->CurrentDirectory();
    if (Data.HostKey.isEmpty())
    {
      Data.HostKey = HostKeyForCode(Tab);
    }
  }
  GenerateUrlDialog::Execute(this, Data);
}

void CommanderWindow::GenerateFileUrl()
{
  // TCustomScpExplorerForm::FileGenerateUrl
  SessionTab * Tab = CurrentTab();
  FilePanel * Panel = ActivePanel();
  if (!Connected(Tab) || !Panel->IsRemote()) return;
  QStringList Paths = Panel->SelectedPaths();
  if (Paths.isEmpty()) return;
  SessionData Data = Tab->Data;
  Data.RemoteDirectory = Tab->RemotePanel->CurrentDirectory();
  if (Data.HostKey.isEmpty())
  {
    Data.HostKey = HostKeyForCode(Tab);
  }
  GenerateUrlDialog::Execute(this, Data, Paths);
}

void CommanderWindow::ShowServerInfo()
{
  SessionTab * Tab = CurrentTab();
  if (!Connected(Tab)) return;
  ServerInfoDialog::Execute(this, Tab->Term, Tab->RemotePanel->CurrentDirectory(),
    FileOperations::SpaceAvailableFunc(this, Tab->Term));
}

void CommanderWindow::ClearCaches()
{
  // TCustomScpExplorerForm::ClearCaches -> TTerminal::ClearCaches (worker thread)
  SessionTab * Tab = CurrentTab();
  if (!Connected(Tab) || !CheckReady()) return;
  Tab->Term->RunOnWorker([](FileSystem & Fs, FileOperationProgress &, UserInterface &)
  {
    DirectoryCaching::ClearCaches(Fs);
  }, [this](const OperationResult & Result)
  {
    ShowResultError(Result);
    ShowStatusMessage(statusBar(), QStringLiteral("Caches cleared."), 3000);
  });
}

void CommanderWindow::OpenInPutty()
{
  // TTerminalManager::OpenInPutty -> windows/GUITools.cpp OpenSessionInPutty.
  // On Linux the "PuTTY" is the terminal client command of Preferences >
  // Integration > Applications (default: OpenSSH in the terminal emulator),
  // with all session patterns and the OpenSSH equivalents of the site's SSH
  // settings (DesktopIntegration::BuildTerminalCommand).
  SessionTab * Tab = CurrentTab();
  if (Tab->LocalBrowser) return;
  SessionData Data = Tab->Data;
  QString Password = Tab->SessionPassword;
  if (Password.isEmpty() && (Tab->Term != nullptr))
  {
    // UpdateSessionCredentials: the password typed at login
    Password = Tab->Term->RememberedPassword();
  }
  bool IsConnected = Connected(Tab);
  QString RemoteDirectory = IsConnected ? Tab->RemotePanel->CurrentDirectory() : Data.RemoteDirectory;
  DesktopIntegration::OpenSessionInTerminal(this, Data, Password, RemoteDirectory, IsConnected);
}

void CommanderWindow::OpenBookmark(const Bookmark & B)
{
  // TScpCommanderForm::OpenBookmark (shared bookmark shortcuts)
  SessionTab * Tab = CurrentTab();
  QString Local;
  QString Remote;
  bool RemoteSide = (ActivePanel() == Tab->RemotePanel) && Connected(Tab);
  if (Bookmarks::OpenBookmarkTarget(B, Configuration::Instance().UseLocationProfiles, Connected(Tab), RemoteSide, Local, Remote))
  {
    if (!Local.isEmpty())
    {
      Tab->LocalPanel->ChangeDirectory(Local);
    }
    if (!Remote.isEmpty() && Connected(Tab))
    {
      Tab->RemotePanel->ChangeDirectory(Remote);
    }
  }
}

void CommanderWindow::ExploreFile(const QString & FileName)
{
  // /browse: focuses the file in the remote panel once it is listed
  SessionTab * Tab = CurrentTab();
  if (!Connected(Tab)) return;
  QString Directory = UnixPath::ExtractFileDir(FileName);
  QString Name = UnixPath::ExtractFileName(FileName);
  if (Directory.isEmpty() || (Directory == Tab->RemotePanel->CurrentDirectory()))
  {
    if (!Tab->RemotePanel->FocusFile(Name))
    {
      Tab->RemotePanel->ChangeDirectory(Tab->RemotePanel->CurrentDirectory(), Name);
    }
  }
  else
  {
    Tab->RemotePanel->ChangeDirectory(Directory, Name);
  }
  Tab->LastActivePanel = Tab->RemotePanel;
  Tab->RemotePanel->FocusView();
}

void CommanderWindow::RefreshPanel(const QString & Session, const QString & Path)
{
  // TCustomScpExplorerForm::RefreshPanel (/refresh)
  for (SessionTab * Tab : FTabs)
  {
    if (!Connected(Tab)) continue;
    if (!Session.isEmpty())
    {
      bool Match = (Tab->Data.Name.compare(Session, Qt::CaseInsensitive) == 0) ||
        (Tab->Data.SessionName().compare(Session, Qt::CaseInsensitive) == 0) ||
        (Tab->Data.HostName.compare(Session, Qt::CaseInsensitive) == 0);
      if (!Match)
      {
        // Session URL: compare the host
        SessionData Parsed;
        int Info = 0;
        if (Parsed.ParseUrl(Session, Info))
        {
          Match = SameSessionTarget(Parsed, Tab->Data) ||
            ((Parsed.HostName == Tab->Data.HostName) && (Parsed.UserName.isEmpty() || (Parsed.UserName == Tab->Data.UserName)));
        }
      }
      if (!Match) continue;
    }
    if (Path.isEmpty() || SameUnixPath(Path, Tab->RemotePanel->CurrentDirectory()))
    {
      Tab->RemotePanel->Refresh();
    }
  }
}

//---------------------------------------------------------------------------
// Operations

void CommanderWindow::ShowResultError(const OperationResult & Result)
{
  if (!Result.Ok && !Result.Aborted && !Result.Fatal)
  {
    ShowErrorMessage(this, Result.Message, Result.MoreMessages);
  }
}

void CommanderWindow::RunWithProgress(const std::function<void(std::function<void(const OperationResult &)>)> & Start,
  const std::function<void(const OperationResult &)> & Done, const std::function<void()> & MoveToQueue,
  const QString & TargetLocalPath)
{
  SessionTab * Tab = CurrentTab();
  if (Tab->Term == nullptr)
  {
    // Local operation without a session: no session progress to show
    Start([this, Tab, Done](const OperationResult & Result)
    {
      ShowResultError(Result);
      if (Done) Done(Result);
      if (FTabs.contains(Tab)) RefreshTab(Tab);
      UpdateActions();
    });
    return;
  }
  Tab->OperationRunning = true;
  UpdateActions();
  auto * Progress = new ProgressDialog(Tab->Term->Progress(), this);
  // Window title and taskbar progress (TCustomScpExplorerForm::UpdateProgressTitle)
  connect(Progress, &ProgressDialog::ProgressTitleChanged, this, [this](const QString & Title)
  {
    DesktopIntegration::SetProgressTitle(Title);
    if (Title.isEmpty())
    {
      UpdateTitle();
    }
    else
    {
      setWindowTitle(Title + TitleSeparator + windowTitle().section(TitleSeparator, -2));
    }
  });
  // Proceed in Background (TProgressForm AllowMoveToQueue: FTransferResumeList != NULL)
  bool AllowMoveToQueue = MoveToQueue && (Tab->Queue != nullptr) && Tab->Term->IsCapable(fcBackgroundTransfers);
  Progress->SetAllowMoveToQueue(AllowMoveToQueue);
  // Once Finished > Disconnect Session; Sleep / Shut Down are executed by the
  // dialog itself after this signal (TTerminal::CloseOnCompletion)
  connect(Progress, &ProgressDialog::OnceDoneRequested, this, [this, Tab, TargetLocalPath](SystemActions::OnceDoneOperation Operation)
  {
    ExecuteOnceDone(Tab, Operation, false, TargetLocalPath);
  });
  auto Elapsed = std::make_shared<QElapsedTimer>();
  Elapsed->start();
  Progress->Start();
  Start([this, Tab, Progress, Done, Elapsed, MoveToQueue, AllowMoveToQueue](const OperationResult & Result)
  {
    bool MoveToQueueNow = AllowMoveToQueue && Progress->MoveToQueue();
    Progress->Finish(Result.Ok);
    Progress->deleteLater();
    DesktopIntegration::SetProgressTitle(QString());
    if (!FTabs.contains(Tab))
    {
      return;
    }
    Tab->OperationRunning = false;
    UpdateTitle();
    if (MoveToQueueNow && Connected(Tab))
    {
      // the abort caused by "MoveToQueue" is not an error: the rest goes to the queue
      MoveToQueue();
      if (Done)
      {
        Done(Result);
      }
      RefreshTab(Tab);
      UpdateActions();
      return;
    }
    // BeepOnFinish / balloon of a minimized window (OperationComplete)
    DesktopIntegration::OperationComplete(this, Elapsed->elapsed());
    ShowResultError(Result);
    if (Done)
    {
      Done(Result);
    }
    RefreshTab(Tab);
    UpdateActions();
  });
}

// Top-level items of a transfer stopped by Proceed in Background that were
// not (completely) transferred yet. WinSCP passes the transferred files to the
// queue item as TransferSkipList / TransferResumeFile; the NixSCP transfer
// processes the scanned items in the order of Paths, so the item being
// transferred when the operation stopped tells which top-level items are done.
// A directory in progress is queued whole.
static QStringList RemainingTransferPaths(const QStringList & Paths, const FileOperationProgress::Snapshot & Data)
{
  if ((Data.FilesFinished == 0) && (Data.TotalTransferred == 0))
  {
    // nothing transferred yet (still scanning the sources, which shows them as well)
    return Paths;
  }
  if (Data.FileName.isEmpty())
  {
    // past the transfers (deleting the sources of a move)
    return QStringList();
  }
  for (int Index = 0; Index < Paths.size(); Index++)
  {
    const QString & Path = Paths[Index];
    if (Data.FileName == Path)
    {
      bool Completed = (Data.FileSize > 0) && (Data.FileTransferred >= Data.FileSize);
      return Paths.mid(Completed ? Index + 1 : Index);
    }
    QString Prefix = Path.endsWith(QLatin1Char('/')) ? Path : Path + QLatin1Char('/');
    if (Data.FileName.startsWith(Prefix))
    {
      return Paths.mid(Index);
    }
  }
  return Paths;
}

void CommanderWindow::RunTransfer(SessionTab * Tab, const QStringList & Paths, bool FromRemote, const QString & TargetDirectory,
  const CopyParam & Param, bool Move, const std::function<void(const OperationResult &)> & Done)
{
  Terminal * T = Tab->Term;
  // TCustomScpExplorerForm::ExecuteFileOperation, catch (EAbort) with FMoveToQueue
  auto ToQueue = [this, Tab, T, Paths, FromRemote, TargetDirectory, Param, Move]()
  {
    if (!FTabs.contains(Tab) || (Tab->Queue == nullptr) || (Tab->Term != T))
    {
      return;
    }
    QStringList Remaining = RemainingTransferPaths(Paths, T->Progress().Get());
    if (!Remaining.isEmpty())
    {
      if (FromRemote)
      {
        Tab->Queue->AddDownload(Remaining, TargetDirectory, Param, Move);
      }
      else
      {
        Tab->Queue->AddUpload(Remaining, TargetDirectory, Param, Move);
      }
    }
    // ClearTransferSourceSelection
    (FromRemote ? Tab->RemotePanel : Tab->LocalPanel)->ClearOperationSelection();
    UpdateQueueView();
  };
  RunWithProgress([T, Paths, FromRemote, TargetDirectory, Param, Move](auto ADone)
  {
    if (FromRemote)
    {
      T->CopyToLocal(Paths, TargetDirectory, Param, Move, ADone);
    }
    else
    {
      T->CopyToRemote(Paths, TargetDirectory, Param, Move, ADone);
    }
  }, Done, ToQueue, FromRemote ? TargetDirectory : QString());
}

void CommanderWindow::ExecuteOnceDone(SessionTab * Tab, int Operation, bool ExecuteSystem, const QString & TargetLocalPath)
{
  // TTerminal::CloseOnCompletion + ShowExtendedExceptionEx (ESshTerminate).
  // Deferred, so that the operation (or queue) finishes its own work first
  // and Sleep (executed by the progress dialog) happens before the prompt.
  if ((Operation == SystemActions::odoIdle) || !FTabs.contains(Tab))
  {
    return;
  }
  QTimer::singleShot(0, this, [this, Tab, Operation, ExecuteSystem, TargetLocalPath]()
  {
    if (!FTabs.contains(Tab))
    {
      return;
    }
    auto Op = static_cast<SystemActions::OnceDoneOperation>(Operation);
    bool ActiveSession = (Tab == CurrentTab());
    int OtherSessions = 0;
    for (SessionTab * Other : FTabs)
    {
      if ((Other != Tab) && Connected(Other)) OtherSessions++;
    }
    if (Connected(Tab))
    {
      DisconnectTab(Tab);
    }
    if (ExecuteSystem && ((Op == SystemActions::odoSuspend) || (Op == SystemActions::odoShutDown)))
    {
      // suspend, so that the exit prompt is shown only after resume
      QString Error;
      if (!SystemActions::Execute(Op, &Error) && !Error.isEmpty())
      {
        ShowErrorMessage(this, Error);
      }
    }
    if (SystemActions::ConfirmCloseOnCompletion(this, Op, ActiveSession, OtherSessions, TargetLocalPath) && ActiveSession)
    {
      close();
    }
  });
}

void CommanderWindow::QueueUserActionPending(SessionTab * Tab, int Id)
{
  // TCustomScpExplorerForm::QueueEvent qePendingUserAction
  if (!FTabs.contains(Tab))
  {
    return;
  }
  bool Current = (Tab == CurrentTab());
  if (Current)
  {
    // the queue list shows the item (it is popped up by the panel with QueueAutoPopup)
    UpdateQueueView();
    FQueuePanel->ShowUserActionItem(Id);
  }
  bool QueueInvisible = !FQueuePanel->isVisible() || isMinimized() || DesktopIntegration::IsMinimizedToTray();
  if (!Current || (QueueInvisible && !Configuration::Instance().QueueAutoPopup))
  {
    // BALLOON_QUEUE_USER_ACTION
    DesktopIntegration::Notify(TabCaption(Tab), QStringLiteral("Background transfer requires your attention."));
  }
}

void CommanderWindow::CopyFiles(bool Remote, bool Move, int QueueMode)
{
  // TCustomScpExplorerForm::ExecuteCopyOperationCommand
  SessionTab * Tab = CurrentTab();
  if (Tab->LocalBrowser)
  {
    LocalLocalCopy(Remote, Move);
    return;
  }
  FilePanel * Source = SidePanel(Remote);
  FilePanel * Target = OtherPanel(Source);
  QStringList Paths = Source->SelectedPaths();
  if (Paths.isEmpty() || !Connected(Tab))
  {
    return;
  }
  // ExecuteCopyOperationCommand: cocShortCutHint only in Commander with
  // Commander keyboard shortcuts
  bool ShortCut = FExecutedByShortcut && !Configuration::Instance().ExplorerKeyboardShortcuts;
  FExecutedByShortcut = false;
  TransferFiles(Paths, Source->IsRemote(), Target->CurrentDirectory(), Move, true, QueueMode, ShortCut);
}

void CommanderWindow::TransferFiles(const QStringList & Paths, bool FromRemote, const QString & TargetDirectory, bool Move,
  bool Confirm, int QueueMode, bool ShortCutHint)
{
  SessionTab * Tab = CurrentTab();
  if (!Connected(Tab))
  {
    return;
  }
  QString Target = TargetDirectory;
  // The selected transfer settings preset (Transfer Settings toolbar)
  CopyParam Param = CopyParamPresets::Instance().CurrentCopyParam();
  if (QueueMode != 0)
  {
    Param.Queue = (QueueMode > 0);
  }
  bool UseQueue = Param.Queue;
  bool Background = Tab->Term->IsCapable(fcBackgroundTransfers);
  if (Confirm)
  {
    QStringList Names;
    for (const QString & P : Paths)
    {
      Names.append(FromRemote ? UnixPath::ExtractFileName(P) : QFileInfo(P).fileName());
    }
    CopyDialog Dialog(!FromRemote, Move, Names, TargetDirectory, Param, this);
    Dialog.SetCopyParamAttrs(CopyParam::UsableAttrs(!FromRemote, Tab->Term, Move));
    Dialog.SetOptions(CopyDialog::coDoNotShowAgain | (Background ? 0 : CopyDialog::coDisableQueue) |
      (ShortCutHint ? CopyDialog::coShortCutHint : 0));
    if (Dialog.exec() != QDialog::Accepted)
    {
      return;
    }
    if (!Dialog.TargetDirectory().isEmpty())
    {
      Target = Dialog.TargetDirectory();
    }
    Param = Dialog.Param();
    UseQueue = Dialog.Queue();
  }
  if (UseQueue && Background && (Tab->Queue != nullptr))
  {
    if (FromRemote)
    {
      Tab->Queue->AddDownload(Paths, Target, Param, Move);
    }
    else
    {
      Tab->Queue->AddUpload(Paths, Target, Param, Move);
    }
    UpdateQueueView();
    return;
  }
  if (!CheckReady())
  {
    return;
  }
  FilePanel * SourcePanel = FromRemote ? Tab->RemotePanel : Tab->LocalPanel;
  SourcePanel->SaveSelectedNames();
  RunTransfer(Tab, Paths, FromRemote, Target, Param, Move, [this, Tab, SourcePanel](const OperationResult & Result)
  {
    if (FTabs.contains(Tab) && Result.Ok)
    {
      SourcePanel->ClearOperationSelection();
    }
  });
}

// TCopyLocalDialog (forms/CopyLocal.cpp): target path with the file mask
static bool CopyLocalDialogExecute(QWidget * Parent, bool Move, int Count, const QString & FirstName,
  QString & TargetDirectory, QString & FileMask)
{
  QDialog Dialog(Parent);
  Dialog.setWindowTitle(Move ? QStringLiteral("Move") : QStringLiteral("Copy"));
  auto * Layout = new QVBoxLayout(&Dialog);
  QString Prompt = (Count == 1) ?
    (Move ? QStringLiteral("Move '%1' to:") : QStringLiteral("Copy '%1' to:")).arg(FirstName) :
    (Move ? QStringLiteral("Move %1 files to:") : QStringLiteral("Copy %1 files to:")).arg(Count);
  auto * Label = new QLabel(Prompt);
  Layout->addWidget(Label);
  auto * Row = new QHBoxLayout();
  auto * Edit = new HistoryComboBox(QStringLiteral("LocalTarget"), &Dialog);
  Edit->setEditable(true);
  Edit->setMinimumWidth(420);
  Edit->setCurrentText(QDir(TargetDirectory).filePath(FileMask));
  Label->setBuddy(Edit);
  Row->addWidget(Edit, 1);
  auto * Browse = new QPushButton(QStringLiteral("B&rowse..."));
  Row->addWidget(Browse);
  Layout->addLayout(Row);
  QObject::connect(Browse, &QPushButton::clicked, &Dialog, [&Dialog, Edit]()
  {
    QString Dir = QFileDialog::getExistingDirectory(&Dialog, QStringLiteral("Select target directory"),
      QFileInfo(Edit->currentText()).path());
    if (!Dir.isEmpty())
    {
      Edit->setCurrentText(QDir(Dir).filePath(QFileInfo(Edit->currentText()).fileName()));
    }
  });
  auto * Buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  HelpSystem::AddHelpButton(Buttons, &Dialog, QStringLiteral("ui_copy_local"));
  Layout->addWidget(Buttons);
  QObject::connect(Buttons, &QDialogButtonBox::accepted, &Dialog, &QDialog::accept);
  QObject::connect(Buttons, &QDialogButtonBox::rejected, &Dialog, &QDialog::reject);
  if (Dialog.exec() != QDialog::Accepted)
  {
    return false;
  }
  QString Text = Edit->currentText().trimmed();
  // ValidateDirectoryEdit: an existing directory gets the "all files" mask
  if (QFileInfo(Text).isDir())
  {
    TargetDirectory = Text;
    FileMask = QStringLiteral("*.*");
  }
  else
  {
    TargetDirectory = QFileInfo(Text).path();
    FileMask = QFileInfo(Text).fileName();
  }
  Edit->SaveToHistory();
  return true;
}

void CommanderWindow::LocalLocalCopy(bool Remote, bool Move)
{
  // TCustomScpExplorerForm::LocalLocalCopyCommand / LocalLocalCopy
  SessionTab * Tab = CurrentTab();
  FilePanel * Source = SidePanel(Remote);
  FilePanel * Target = OtherPanel(Source);
  QStringList Paths = Source->SelectedPaths();
  if (Paths.isEmpty())
  {
    return;
  }
  QString TargetDirectory = Target->CurrentDirectory();
  QString FileMask = QStringLiteral("*.*");
  if (Paths.size() == 1)
  {
    FileMask = QFileInfo(Paths.first()).fileName();
    if ((Source->CurrentDirectory() != TargetDirectory) || Move)
    {
      FileMask = QStringLiteral("*.*");
    }
  }
  if (Configuration::Instance().ConfirmTransferring &&
      !CopyLocalDialogExecute(this, Move, Paths.size(), QFileInfo(Paths.first()).fileName(), TargetDirectory, FileMask))
  {
    return;
  }
  LocalFileOperations::Params Params;
  Params.Op = Move ? LocalFileOperations::opMove : LocalFileOperations::opCopy;
  Params.Sources = Paths;
  Params.TargetDirectory = TargetDirectory;
  if ((Paths.size() == 1) && !FileMask.isEmpty() && (FileMask != QStringLiteral("*.*")) && (FileMask != QStringLiteral("*")))
  {
    Params.NewName = FileMask;
  }
  Params.DuplicateIfSame = (QDir(TargetDirectory) == QDir(Source->CurrentDirectory()));
  Params.ConfirmOverwrite = Configuration::Instance().DefaultCopyParam().ConfirmOverwriting;
  Source->SaveSelectedNames();
  DesktopIntegration::RunLocalFileOperation(this, FUI, Params, [this, Tab, Source](const LocalOperationResult &)
  {
    if (FTabs.contains(Tab))
    {
      Source->ClearOperationSelection();
      Tab->LocalPanel->Refresh();
      Tab->RemotePanel->Refresh();
    }
  });
}

void CommanderWindow::HandleDrop(FilePanel * Target, const QStringList & Paths, bool FromRemote, const QString & TargetDirectory,
  Qt::DropAction Action, bool Paste)
{
  // Drop effect (FilePanel::ActionFor, RemoteFileContolDDChooseEffect): copy
  // between panels and from other applications, move within a panel; Ctrl
  // forces copy, Shift forces move
  bool Move = (Action == Qt::MoveAction);
  SessionTab * Tab = CurrentTab();
  if (Paths.isEmpty())
  {
    return;
  }
  Configuration & Config = Configuration::Instance();
  if (Target->IsRemote() != FromRemote)
  {
    // Upload / download (RemoteFileControlDragDropFileOperation,
    // LocalFileControlDDFileOperation): the Copy dialog unless the drag & drop
    // transfer confirmation is off (DDTransferConfirmation; "Do not show
    // this dialog box again" defaults to checked while it is Automatic)
    if (!Connected(Tab))
    {
      return;
    }
    TAutoSwitch ConfirmationBefore = Config.DDTransferConfirmation;
    QStringList Names;
    for (const QString & P : Paths)
    {
      Names.append(FromRemote ? UnixPath::ExtractFileName(P) : QFileInfo(P).fileName());
    }
    DesktopIntegration::FlashOnBackground(this);
    CopyDialog Dialog(!FromRemote, Move, Names, TargetDirectory, CopyParamPresets::Instance().CurrentCopyParam(), this);
    Dialog.SetCopyParamAttrs(CopyParam::UsableAttrs(!FromRemote, Tab->Term, Move));
    Dialog.SetOptions(CopyDialog::coDoNotShowAgain | (Tab->Term->IsCapable(fcBackgroundTransfers) ? 0 : CopyDialog::coDisableQueue));
    // A paste always shows the dialog (as an explicit command)
    Dialog.SetDragDrop(!Paste);
    if (Dialog.exec() != QDialog::Accepted)
    {
      return;
    }
    if (!Paste && !Dialog.Skipped() && (ConfirmationBefore == asAuto) && (Config.DDTransferConfirmation == asOff))
    {
      // DD_TRANSFER_CONFIRM_OFF2 (HandleDoNotShowCopyDialogAgain)
      DesktopIntegration::Notify(QString(), QStringLiteral("**Transfer confirmation turned off**\n\n"
        "You have opted not to see the Transfer options dialog the next time. "
        "It can be turned on again in Preferences > Drag & Drop."));
    }
    QString TargetDir = Dialog.TargetDirectory().isEmpty() ? TargetDirectory : Dialog.TargetDirectory();
    CopyParam Param = Dialog.Param();
    if (Dialog.Queue() && (Tab->Queue != nullptr))
    {
      if (FromRemote)
      {
        Tab->Queue->AddDownload(Paths, TargetDir, Param, Move);
      }
      else
      {
        Tab->Queue->AddUpload(Paths, TargetDir, Param, Move);
      }
      UpdateQueueView();
      return;
    }
    if (!CheckReady())
    {
      return;
    }
    RunTransfer(Tab, Paths, FromRemote, TargetDir, Param, Move);
    return;
  }
  if (Target->IsRemote())
  {
    if (!Connected(Tab) || !CheckReady())
    {
      return;
    }
    Terminal * Term = Tab->Term;
    if (!Move)
    {
      // Ctrl+drag within the remote panel: duplicate to the drop directory
      // (RemoteFileControlFileOperation foRemoteCopy). With the transfer
      // confirmation forced on, the Duplicate dialog with the confirmed target;
      // otherwise directly (TTerminal::CopyFiles, or the temporary copy)
      if (Config.DDTransferConfirmation == asOn)
      {
        QList<RemoteTransferDialog::SessionEntry> Sessions;
        for (SessionTab * T : FTabs)
        {
          if (Connected(T))
          {
            RemoteTransferDialog::SessionEntry E;
            E.Name = TabCaption(T);
            E.Directory = T->RemotePanel->CurrentDirectory();
            E.Session = T->Term;
            Sessions.append(E);
          }
        }
        FileOperations::Runner Run = [this](const auto & Start, const auto & Done) { RunWithProgress(Start, Done); };
        FileOperations::Duplicate(this, Term, Tab->RemotePanel->SelectedFiles(), Paths, TargetDirectory, Sessions, Run,
          [this](Terminal * TargetSession)
          {
            if (SessionTab * T = TabOfTerminal(TargetSession)) RefreshTab(T);
          }, true);
        return;
      }
      if (!ClipboardTransfer::CanDuplicateRemote(Term))
      {
        ShowErrorMessage(this, QStringLiteral("Operation not supported."),
          QStringLiteral("The protocol cannot duplicate remote files."));
        return;
      }
      RunWithProgress([Term, Paths, TargetDirectory](auto Done)
      {
        ClipboardTransfer::DuplicateRemoteFiles(Term, Paths, TargetDirectory, Done);
      });
      return;
    }
    // Move into the directory (rename), as WinSCP does for a drag within the panel
    auto Remaining = std::make_shared<QStringList>(Paths);
    auto Next = std::make_shared<std::function<void()>>();
    *Next = [this, Tab, Term, Remaining, TargetDirectory, Next]()
    {
      if (!FTabs.contains(Tab)) return;
      if (Remaining->isEmpty() || !Connected(Tab))
      {
        RefreshTab(Tab);
        return;
      }
      QString Path = Remaining->takeFirst();
      QString NewPath = UnixPath::Combine(TargetDirectory, UnixPath::ExtractFileName(Path));
      if (NewPath == Path)
      {
        (*Next)();
        return;
      }
      Term->RenameFile(Path, NewPath,
        [this, Tab, Next](const OperationResult & Result)
        {
          ShowResultError(Result);
          if (Result.Ok) (*Next)(); else if (FTabs.contains(Tab)) RefreshTab(Tab);
        });
    };
    (*Next)();
  }
  else
  {
    // Local copy/move (also files dropped from other applications): worker
    // thread with progress and cancel, reflink/copy_file_range, rename(2)
    // within a file system, symlinks and timestamps kept, the target replaced
    // only once the copy is complete (core/LocalFileOperations)
    LocalFileOperations::Params Params;
    Params.Op = Move ? LocalFileOperations::opMove : LocalFileOperations::opCopy;
    Params.Sources = Paths;
    Params.TargetDirectory = TargetDirectory;
    Params.ConfirmOverwrite = Config.DefaultCopyParam().ConfirmOverwriting;
    Params.DuplicateIfSame = Paste;
    DesktopIntegration::RunLocalFileOperation(this, FUI, Params, [this, Tab](const LocalOperationResult &)
    {
      if (FTabs.contains(Tab))
      {
        Tab->LocalPanel->Refresh();
        if (Tab->LocalBrowser) Tab->RemotePanel->Refresh();
      }
    });
  }
}

void CommanderWindow::DeleteFiles(FilePanel * Panel, bool Alternative)
{
  SessionTab * Tab = CurrentTab();
  QStringList Paths = Panel->SelectedPaths();
  if (Paths.isEmpty() || !PanelReady(Panel))
  {
    return;
  }
  Configuration & Config = Configuration::Instance();
  // TCustomScpExplorerForm::DeleteFiles: the alternative delete (Shift+F8,
  // Shift+Del) does the opposite of the recycle bin setting; remote files go
  // to the session's remote recycle bin (Preserve deleted remote files to
  // recycle bin), except for files that are in the recycle bin already
  bool Recycle;
  if (!Panel->IsRemote())
  {
    // Linux: the freedesktop Trash instead of the Recycle Bin
    Recycle = (Config.DeleteToRecycleBin != Alternative);
  }
  else
  {
    QString RecycleBinPath = Tab->Data.RecycleBinPath;
    while ((RecycleBinPath.size() > 1) && RecycleBinPath.endsWith(QLatin1Char('/'))) RecycleBinPath.chop(1);
    Recycle =
      (Tab->Data.DeleteToRecycleBin != Alternative) &&
      !RecycleBinPath.isEmpty() &&
      (UnixPath::ExtractFileDir(Paths.first()) != RecycleBinPath);
  }
  bool Permanently = !Recycle;
  // ConfirmDeleting / ConfirmRecycling (Preferences > Environment > Confirmations)
  if (Recycle ? Config.ConfirmRecycling : Config.ConfirmDeleting)
  {
    QString Name = Panel->IsRemote() ? UnixPath::ExtractFileName(Paths.first()) : QFileInfo(Paths.first()).fileName();
    // CONFIRM_DELETE_FILE / CONFIRM_DELETE_FILES
    QString Query = (Paths.size() == 1) ?
      QStringLiteral("Are you sure you want to delete file '%1'?").arg(Name) :
      QStringLiteral("Are you sure you want to delete %1 selected files?").arg(Paths.size());
    if (Recycle && !Panel->IsRemote())
    {
      Query = (Paths.size() == 1) ?
        QStringLiteral("Are you sure you want to move file '%1' to Trash?").arg(Name) :
        QStringLiteral("Are you sure you want to move %1 selected files to Trash?").arg(Paths.size());
    }
    else if (Recycle)
    {
      // CONFIRM_RECYCLE_FILE / CONFIRM_RECYCLE_FILES
      Query = (Paths.size() == 1) ?
        QStringLiteral("Are you sure you want to move file '%1' to recycle bin?").arg(Name) :
        QStringLiteral("Are you sure you want to move %1 selected files to recycle bin?").arg(Paths.size());
    }
    MessageParams Params;
    Params.Params = qpNeverAskAgainCheck;
    unsigned int Answer = MessageDialog(this, Query, qtConfirmation, qaOK | qaCancel, QStringLiteral("task_delete"), &Params);
    if (Answer == qaNeverAskAgain)
    {
      (Recycle ? Config.ConfirmRecycling : Config.ConfirmDeleting) = false;
      Config.Save();
    }
    else if (Answer != qaOK)
    {
      return;
    }
  }
  Panel->SaveSelectedNames();
  if (Panel->IsRemote())
  {
    if (!Connected(Tab) || !CheckReady()) return;
    Terminal * T = Tab->Term;
    int Params = Alternative ? dfAlternative : 0;
    RunWithProgress([T, Paths, Params](auto Done) { T->DeleteFiles(Paths, Done, Params); },
      [this, Tab, Panel](const OperationResult & Result)
      {
        if (FTabs.contains(Tab) && Result.Ok) Panel->ClearOperationSelection();
      });
  }
  else
  {
    for (const QString & Path : Paths)
    {
      QFileInfo Info(Path);
      bool Ok;
      if (Permanently)
      {
        Ok = (Info.isDir() && !Info.isSymLink()) ? QDir(Path).removeRecursively() : QFile::remove(Path);
      }
      else
      {
        QString TrashError;
        Ok = LocalFileOperations::MoveToTrash(Path, &TrashError);
        if (!Ok)
        {
          // as the Windows shell does for drives without a Recycle Bin
          QString Message = QStringLiteral("Cannot move '%1' to Trash.").arg(Path);
          if (!TrashError.isEmpty())
          {
            Message += QStringLiteral("\n\n") + TrashError;
          }
          Message += QStringLiteral("\n\nDo you want to delete it permanently?");
          if (MessageDialog(this, Message, qtConfirmation, qaYes | qaNo, QStringLiteral("task_delete")) == qaYes)
          {
            Ok = (Info.isDir() && !Info.isSymLink()) ? QDir(Path).removeRecursively() : QFile::remove(Path);
          }
          else
          {
            break;
          }
        }
      }
      if (!Ok)
      {
        ShowErrorMessage(this, QStringLiteral("Error deleting file '%1'.").arg(Path));
        break;
      }
      Panel->UnmarkFile(Path);
    }
    Panel->Refresh();
    if (Tab->LocalBrowser) OtherPanel(Panel)->Refresh();
  }
}

void CommanderWindow::RenameFile(FilePanel * Panel, const QString & OldPath, const QString & NewName)
{
  if (NewName.isEmpty() || NewName.contains(QLatin1Char('/')))
  {
    return;
  }
  SessionTab * Tab = CurrentTab();
  if (Panel->IsRemote())
  {
    Terminal * Term = PanelTerminal(Panel);
    if (Term == nullptr) return;
    QString NewPath = UnixPath::Combine(UnixPath::ExtractFileDir(OldPath), NewName);
    Term->RenameFile(OldPath, NewPath, [this, Tab, NewName](const OperationResult & Result)
    {
      ShowResultError(Result);
      if (FTabs.contains(Tab) && Connected(Tab))
      {
        Tab->RemotePanel->RefreshAfterOperation();
      }
    });
  }
  else
  {
    QString NewPath = QFileInfo(OldPath).dir().filePath(NewName);
    if (QFileInfo::exists(NewPath) || !QFile::rename(OldPath, NewPath))
    {
      ShowErrorMessage(this, QStringLiteral("Error renaming file '%1' to '%2'.").arg(QFileInfo(OldPath).fileName(), NewName));
    }
    Panel->Refresh();
    Panel->FocusFile(NewName);
  }
}

static FileOperations::Runner MakeRunner(CommanderWindow * Window,
  const std::function<void(const std::function<void(Terminal::Callback)> &, const Terminal::Callback &)> & Run)
{
  Q_UNUSED(Window);
  return Run;
}

void CommanderWindow::CreateDirectory(FilePanel * Panel)
{
  // TCustomScpExplorerForm::CreateDirectory (F7)
  if (!PanelReady(Panel)) return;
  SessionTab * Tab = CurrentTab();
  Terminal * Term = PanelTerminal(Panel);
  FileOperations::Runner Run = MakeRunner(this, [this](const auto & Start, const auto & Done) { RunWithProgress(Start, Done); });
  FileOperations::CreateDirectory(this, Term, Panel->CurrentDirectory(), Run, [this, Tab, Panel](const QString & Path)
  {
    if (!FTabs.contains(Tab)) return;
    Panel->ChangeDirectory(Panel->CurrentDirectory(), Panel->IsRemote() ? UnixPath::ExtractFileName(Path) : QFileInfo(Path).fileName());
  });
}

void CommanderWindow::AddEditLink(FilePanel * Panel, bool Add)
{
  // TCustomScpExplorerForm::AddEditLink (Alt+F6): NewLinkAction always adds
  if (!PanelReady(Panel)) return;
  SessionTab * Tab = CurrentTab();
  Terminal * Term = PanelTerminal(Panel);
  RemoteFile Focused;
  bool HasFocused = !Add && Panel->HasFocusedFile();
  if (HasFocused)
  {
    Focused = Panel->FocusedFile();
  }
  FileOperations::AddEditLink(this, Term, Panel->CurrentDirectory(), HasFocused ? &Focused : nullptr, [this, Tab, Panel](bool)
  {
    if (FTabs.contains(Tab)) Panel->Refresh();
  });
}

void CommanderWindow::Properties(FilePanel * Panel)
{
  // F9 / Alt+Enter (TCustomScpExplorerForm::RemoteDirViewProperties / local properties)
  if (!PanelReady(Panel)) return;
  SessionTab * Tab = CurrentTab();
  QList<RemoteFile> Files = Panel->SelectedFiles();
  QStringList Paths = Panel->SelectedPaths();
  if (Files.isEmpty())
  {
    return;
  }
  Terminal * Term = PanelTerminal(Panel);
  if ((Term != nullptr) && !CheckReady()) return;
  FileOperations::Runner Run = MakeRunner(this, [this](const auto & Start, const auto & Done) { RunWithProgress(Start, Done); });
  FileOperations::Properties(this, Term, Files, Paths, Panel->CurrentDirectory(), Run, [this, Tab, Panel](bool Changed)
  {
    if (Changed && FTabs.contains(Tab)) Panel->Refresh();
  });
}

void CommanderWindow::CalculateDirectorySizes(FilePanel * Panel)
{
  // CalculateDirectorySizesAction (Shift+Alt+Enter)
  if (!PanelReady(Panel)) return;
  QStringList Directories;
  QList<RemoteFile> Files = Panel->SelectedFiles();
  QStringList Paths = Panel->SelectedPaths();
  for (int I = 0; (I < Files.size()) && (I < Paths.size()); I++)
  {
    if (Files[I].IsDirectory && !Files[I].IsSymLink)
    {
      Directories.append(Paths[I]);
    }
  }
  if (Directories.isEmpty())
  {
    return;
  }
  Terminal * Term = PanelTerminal(Panel);
  if ((Term != nullptr) && !CheckReady()) return;
  QPointer<FilePanel> GuardPanel(Panel);
  FileOperations::CalculateDirectorySizes(this, Term, Directories, [GuardPanel](const QHash<QString, qint64> & Sizes)
  {
    // DirectorySizeCalculated -> SetItemCalculatedSize: sizes go to the Size column
    if (!GuardPanel) return;
    for (auto It = Sizes.cbegin(); It != Sizes.cend(); ++It)
    {
      GuardPanel->SetItemCalculatedSize(It.key(), It.value());
    }
  });
}

void CommanderWindow::DuplicateOrMove(bool Move)
{
  // RemoteCopyToAction (Shift+F5 Duplicate) / RemoteMoveToAction (Shift+F6 Move To)
  SessionTab * Tab = CurrentTab();
  FilePanel * Panel = ActivePanel();
  if (Tab->LocalBrowser)
  {
    LocalLocalCopy(Panel == Tab->RemotePanel, Move);
    return;
  }
  if (!Connected(Tab) || !Panel->IsRemote() || !CheckReady()) return;
  QStringList Paths = Panel->SelectedPaths();
  if (Paths.isEmpty()) return;
  FileOperations::Runner Run = MakeRunner(this, [this](const auto & Start, const auto & Done) { RunWithProgress(Start, Done); });
  if (Move)
  {
    FileOperations::MoveTo(this, Tab->Term, Paths, Panel->CurrentDirectory(), Run, [this, Tab](bool)
    {
      if (FTabs.contains(Tab)) RefreshTab(Tab);
    });
    return;
  }
  QList<RemoteTransferDialog::SessionEntry> Sessions;
  for (SessionTab * T : FTabs)
  {
    if (Connected(T))
    {
      RemoteTransferDialog::SessionEntry E;
      E.Name = TabCaption(T);
      E.Directory = T->RemotePanel->CurrentDirectory();
      E.Session = T->Term;
      Sessions.append(E);
    }
  }
  FileOperations::Duplicate(this, Tab->Term, Panel->SelectedFiles(), Paths, Panel->CurrentDirectory(), Sessions, Run,
    [this](Terminal * TargetSession)
    {
      if (SessionTab * T = TabOfTerminal(TargetSession)) RefreshTab(T);
    });
}

void CommanderWindow::EditNew(FilePanel * Panel)
{
  // TCustomScpExplorerForm::EditNew (Shift+F4): EDIT_FILE_CAPTION / EDIT_FILE_PROMPT,
  // the focused file name (NEW_FILE otherwise), the EditFile history
  if (!PanelReady(Panel)) return;
  SessionTab * Tab = CurrentTab();
  QString Name = QStringLiteral("New file");
  if (Panel->HasFocusedFile() && !Panel->FocusedFile().IsDirectory)
  {
    Name = Panel->FocusedFile().FileName;
  }
  QDialog Dialog(this);
  Dialog.setWindowTitle(QStringLiteral("Edit file"));
  auto * Layout = new QVBoxLayout(&Dialog);
  auto * Label = new QLabel(QStringLiteral("&Enter file name:"));
  auto * Combo = new HistoryComboBox(QStringLiteral("EditFile"), &Dialog);
  Combo->setEditable(true);
  Combo->setMinimumWidth(320);
  Combo->setCurrentText(Name);
  Label->setBuddy(Combo);
  Layout->addWidget(Label);
  Layout->addWidget(Combo);
  auto * Buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  // HELP_EDIT_NEW
  HelpSystem::AddHelpButton(Buttons, &Dialog, QStringLiteral("task_edit"));
  Layout->addWidget(Buttons);
  connect(Buttons, &QDialogButtonBox::accepted, &Dialog, &QDialog::accept);
  connect(Buttons, &QDialogButtonBox::rejected, &Dialog, &QDialog::reject);
  if (Combo->lineEdit() != nullptr)
  {
    Combo->lineEdit()->selectAll();
  }
  if ((Dialog.exec() != QDialog::Accepted) || Combo->currentText().trimmed().isEmpty())
  {
    return;
  }
  Combo->SaveToHistory();
  // Several names separated by ';' (FileMasksDelimiters)
  const QStringList Names = Combo->currentText().split(QLatin1Char(';'), Qt::SkipEmptyParts);
  for (QString FileName : Names)
  {
    FileName = FileName.trimmed();
    if (FileName.isEmpty()) continue;
    if (!Panel->IsRemote())
    {
      QString LocalFileName = QDir::isAbsolutePath(FileName) ? FileName : QDir(Panel->CurrentDirectory()).filePath(FileName);
      if (!QFileInfo::exists(LocalFileName))
      {
        QFile File(LocalFileName);
        if (!File.open(QIODevice::WriteOnly))
        {
          // CREATE_FILE_ERROR
          ShowErrorMessage(this, QStringLiteral("Cannot create file '%1'.").arg(LocalFileName), File.errorString());
          return;
        }
        File.close();
        Panel->Refresh();
      }
      FEditorManager->ExecuteFiles(nullptr, {LocalFileName}, EditorManager::efDefaultEditor);
      continue;
    }
    Terminal * Term = PanelTerminal(Panel);
    if (Term == nullptr) return;
    QString RemotePath = FileName.startsWith(QLatin1Char('/')) ? FileName : UnixPath::Combine(Panel->CurrentDirectory(), FileName);
    // An existing file is edited (TryReadFile); a new one is edited in an
    // empty temporary file and created on the server when saved
    // (TCustomScpExplorerForm::EditNew, NewFile)
    Term->RunOnWorker([RemotePath](FileSystem & Fs, FileOperationProgress &, UserInterface &)
    {
      RemoteFile Found;
      if (!Fs.FileExists(RemotePath, &Found) || Found.IsDirectory)
      {
        throw ExtException(QStringLiteral("NOTFOUND"));
      }
    }, [this, Tab, Term, RemotePath](const OperationResult & Result)
    {
      if (!FTabs.contains(Tab) || !Connected(Tab)) return;
      if (Result.Ok)
      {
        FEditorManager->ExecuteFiles(Term, {RemotePath}, EditorManager::efDefaultEditor);
      }
      else
      {
        FEditorManager->ExecuteNewRemoteFile(Term, RemotePath);
      }
    });
  }
}

void CommanderWindow::OpenFocused()
{
  // CurrentOpenAction (TCustomScpExplorerForm::ExecuteCurrentFile)
  FilePanel * Panel = ActivePanel();
  if (Panel->HasFocusedFile())
  {
    RemoteFile File = Panel->FocusedFile();
    OpenFile(Panel, File, Panel->FullPath(File.FileName));
  }
}

void CommanderWindow::OpenFile(FilePanel * Panel, const RemoteFile & File, const QString & FullPath)
{
  if (File.IsDirectory)
  {
    Panel->ChangeDirectory(FullPath);
  }
  else if (Panel->IsRemote())
  {
    // WinSCP "Open": download to temporary folder and open with associated application;
    // changes are uploaded back as with Edit
    Terminal * Term = PanelTerminal(Panel);
    if (Term == nullptr) return;
    FEditorManager->ExecuteFiles(Term, {FullPath}, EditorManager::efShell);
  }
  else
  {
    QDesktopServices::openUrl(QUrl::fromLocalFile(FullPath));
  }
}

void CommanderWindow::SelectByMask(FilePanel * Panel, bool Select)
{
  // forms/SelectMask (EditMask) with WinSCP's remembered mask
  if (!PanelReady(Panel)) return;
  QString Mask;
  bool ApplyToDirectories = false;
  if (EditMaskDialog::Execute(this, Select ? EditMaskDialog::Select : EditMaskDialog::Unselect, Mask, ApplyToDirectories))
  {
    Panel->SelectByMask(Mask, Select, ApplyToDirectories);
  }
}

void CommanderWindow::Filter(FilePanel * Panel)
{
  if (!PanelReady(Panel)) return;
  QString Mask = Panel->FilterMask();
  bool ApplyToDirectories = false;
  if (EditMaskDialog::Execute(this, EditMaskDialog::Filter, Mask, ApplyToDirectories))
  {
    Panel->SetFilterMask(Mask);
  }
}

void CommanderWindow::OpenDirectory(FilePanel * Panel, bool AddBookmark)
{
  // forms/LocationProfiles: Open Directory/Bookmark (Ctrl+O), Add Path to Bookmarks (Ctrl+B)
  SessionTab * Tab = CurrentTab();
  if (!PanelReady(Panel)) return;
  if (Tab->LocalBrowser)
  {
    // Local-local: both panels are local; the dialog works on the local side
    QString LocalDir = Panel->CurrentDirectory();
    QString RemoteDir;
    if (LocationProfilesDialog::Execute(this, QString(), false, LocalDir, RemoteDir, AddBookmark, Panel->VisitedDirectories()))
    {
      if (!LocalDir.isEmpty() && (LocalDir != Panel->CurrentDirectory()))
      {
        Panel->ChangeDirectory(LocalDir);
      }
    }
    return;
  }
  bool Remote = Panel->IsRemote() && Connected(Tab);
  QString SessionKey = Connected(Tab) ? Bookmarks::SessionKey(Tab->Data) : QString();
  QString LocalDir = Tab->LocalPanel->CurrentDirectory();
  QString RemoteDir = Connected(Tab) ? Tab->RemotePanel->CurrentDirectory() : QString();
  if (LocationProfilesDialog::Execute(this, SessionKey, Remote, LocalDir, RemoteDir, AddBookmark,
        Tab->LocalPanel->VisitedDirectories(), Connected(Tab) ? Tab->RemotePanel->VisitedDirectories() : QStringList()))
  {
    if (!LocalDir.isEmpty() && (LocalDir != Tab->LocalPanel->CurrentDirectory()))
    {
      Tab->LocalPanel->ChangeDirectory(LocalDir);
    }
    if (Connected(Tab) && !RemoteDir.isEmpty() && (RemoteDir != Tab->RemotePanel->CurrentDirectory()))
    {
      Tab->RemotePanel->ChangeDirectory(RemoteDir);
    }
  }
}

void CommanderWindow::PathFromOtherPanel(FilePanel * Panel)
{
  // LocalOtherDirAction / RemoteOtherDirAction (local-local tabs only)
  SessionTab * Tab = CurrentTab();
  if (!Tab->LocalBrowser) return;
  Panel->ChangeDirectory(OtherPanel(Panel)->CurrentDirectory());
}

void CommanderWindow::GoToAddress()
{
  // GoToAddressAction (Ctrl+L, Alt+D): the path box of the current panel
  FilePanel * Panel = ActivePanel();
  if (PanelReady(Panel))
  {
    Panel->FocusPathCombo();
  }
}

void CommanderWindow::SortBy(FilePanel * Panel, int Column)
{
  if (PanelReady(Panel))
  {
    Panel->SortBy(Column);
    UpdateSortActions();
  }
}

void CommanderWindow::ToggleSortAscending(FilePanel * Panel)
{
  if (PanelReady(Panel))
  {
    Panel->SetSort(Panel->SortColumn(), !Panel->SortAscending());
    UpdateSortActions();
  }
}

void CommanderWindow::FindFiles()
{
  // forms/FileFind: searches the active panel's side
  SessionTab * Tab = CurrentTab();
  FilePanel * Panel = ActivePanel();
  if (!PanelReady(Panel)) return;
  bool Remote = Panel->IsRemote() && Connected(Tab);
  Terminal * Term = Remote ? Tab->Term : nullptr;
  auto * Dialog = new FindFilesDialog(Term, Panel->CurrentDirectory(), this);
  connect(Dialog, &FindFilesDialog::FocusRequested, this, [this, Tab, Panel](const QString & Directory, const QString & FileName)
  {
    if (!FTabs.contains(Tab)) return;
    ActivateTab(Tab);
    Panel->ChangeDirectory(Directory, FileName);
    Panel->FocusView();
  });
  connect(Dialog, &FindFilesDialog::EditRequested, this, [this, Tab, Remote](const QStringList & Paths)
  {
    if (!FTabs.contains(Tab)) return;
    if (Remote && !Connected(Tab)) return;
    ActivateTab(Tab);
    FEditorManager->ExecuteFiles(Remote ? Tab->Term : nullptr, Paths);
  });
  if (Remote)
  {
    connect(Dialog, &FindFilesDialog::DownloadRequested, this, [this, Tab](const QStringList & Paths)
    {
      if (!FTabs.contains(Tab) || !Connected(Tab)) return;
      ActivateTab(Tab);
      TransferFiles(Paths, true, Tab->LocalPanel->CurrentDirectory(), false, true);
    });
  }
  QPointer<FindFilesDialog> DialogPtr(Dialog);
  connect(Dialog, &FindFilesDialog::DeleteRequested, this, [this, Tab, Remote, DialogPtr](const QStringList & Paths)
  {
    if (!FTabs.contains(Tab)) return;
    if (Configuration::Instance().ConfirmDeleting &&
        (MessageDialog(this,
          (Paths.size() == 1) ? QStringLiteral("Are you sure you want to delete file '%1'?").arg(Paths.first())
                              : QStringLiteral("Are you sure you want to delete %1 selected files?").arg(Paths.size()),
          qtConfirmation, qaOK | qaCancel) != qaOK))
    {
      return;
    }
    if (Remote)
    {
      if (!Connected(Tab)) return;
      Tab->Term->DeleteFiles(Paths, [this, Tab, Paths, DialogPtr](const OperationResult & Result)
      {
        ShowResultError(Result);
        if (Result.Ok && DialogPtr) DialogPtr->FilesDeleted(Paths);
        if (FTabs.contains(Tab)) RefreshTab(Tab);
      });
    }
    else
    {
      QStringList Deleted;
      bool Trash = Configuration::Instance().DeleteToRecycleBin;
      for (const QString & Path : Paths)
      {
        if ((Trash && LocalFileOperations::MoveToTrash(Path)) || (!Trash && QFile::remove(Path))) Deleted.append(Path);
      }
      if (DialogPtr) DialogPtr->FilesDeleted(Deleted);
      RefreshTab(Tab);
    }
  });
  Dialog->show();
}

void CommanderWindow::FileNamesToClipboard(bool FullPath)
{
  // FileListToClipboardAction / FullFileListToClipboardAction (PanelExport)
  FilePanel * Panel = ActivePanel();
  if (!PanelReady(Panel)) return;
  QStringList Result;
  for (const QString & Path : Panel->SelectedPaths())
  {
    Result.append(FullPath ? Path : (Panel->IsRemote() ? UnixPath::ExtractFileName(Path) : QFileInfo(Path).fileName()));
  }
  QApplication::clipboard()->setText(Result.join(QLatin1Char('\n')));
}

void CommanderWindow::CopyToClipboard()
{
  // CurrentCopyToClipboardAction2 (Ctrl+C): the files themselves
  SessionTab * Tab = CurrentTab();
  FilePanel * Panel = ActivePanel();
  if (!PanelReady(Panel)) return;
  QStringList Paths = Panel->SelectedPaths();
  if (Paths.isEmpty()) return;
  if (!Panel->IsRemote())
  {
    ClipboardTransfer::CopyLocalFiles(Paths);
    return;
  }
  qint64 Size = 0;
  for (const RemoteFile & F : Panel->SelectedFiles())
  {
    if (F.IsDirectory)
    {
      Size = -1;
      break;
    }
    Size += F.Size;
  }
  ClipboardTransfer::CopyRemoteFiles(Tab->Term, Paths, this, Size);
}

void CommanderWindow::PasteFromClipboard()
{
  // PasteAction3 (Ctrl+V): files to the current directory of the active
  // panel, a path to open in it, or a session URL to open
  SessionTab * Tab = CurrentTab();
  FilePanel * Panel = ActivePanel();
  if (!PanelReady(Panel)) return;
  ClipboardTransfer::Content C = ClipboardTransfer::Read();
  QString Dir = Panel->CurrentDirectory();
  switch (C.Type)
  {
    case ClipboardTransfer::Content::ckLocalFiles:
      HandleDrop(Panel, C.Paths, false, Dir, C.Cut ? Qt::MoveAction : Qt::CopyAction, true);
      break;
    case ClipboardTransfer::Content::ckRemoteFiles:
      if (!C.Source.isNull() && (C.Source.data() == Tab->Term) && Connected(Tab))
      {
        if (!Panel->IsRemote())
        {
          HandleDrop(Panel, C.Paths, true, Dir, Qt::CopyAction, true);
        }
        else if (CheckReady())
        {
          Terminal * T = Tab->Term;
          QStringList Paths = C.Paths;
          RunWithProgress([T, Paths, Dir](auto Done) { ClipboardTransfer::DuplicateRemoteFiles(T, Paths, Dir, Done, true); });
        }
      }
      else
      {
        // Files of another session: downloaded to a temporary folder first
        QStringList Local = !C.Remote.isNull() ? C.Remote->DownloadedFiles() : QStringList();
        if (!Local.isEmpty())
        {
          HandleDrop(Panel, Local, false, Dir, Qt::CopyAction, true);
        }
      }
      break;
    case ClipboardTransfer::Content::ckText:
    {
      QString Value;
      switch (ClipboardTransfer::ClassifyText(C.Text, Panel->IsRemote(), &Value))
      {
        case ClipboardTransfer::tkSessionUrl:
        {
          SessionData Data;
          int Info = 0;
          if (Data.ParseUrl(Value, Info))
          {
            OpenSession(Data);
          }
          break;
        }
        case ClipboardTransfer::tkPath:
          Panel->ChangeDirectory(Value);
          break;
        default:
          break;
      }
      break;
    }
    default:
      break;
  }
}

void CommanderWindow::ToggleShowHiddenFiles()
{
  // TCustomScpExplorerForm::ToggleShowHiddenFiles
  Configuration & Config = Configuration::Instance();
  Config.ShowHiddenFiles = !Config.ShowHiddenFiles;
  FA->Get(QStringLiteral("ShowHiddenFilesAction"))->setChecked(Config.ShowHiddenFiles);
  for (SessionTab * Tab : FTabs)
  {
    Tab->LocalProvider.ShowHiddenFiles = Config.ShowHiddenFiles;
    Tab->OtherLocalProvider.ShowHiddenFiles = Config.ShowHiddenFiles;
    if (Tab->RemoteProvider) Tab->RemoteProvider->ShowHiddenFiles = Config.ShowHiddenFiles;
    Tab->LocalPanel->SetShowHiddenFiles(Config.ShowHiddenFiles);
    Tab->RemotePanel->SetShowHiddenFiles(Config.ShowHiddenFiles);
  }
  // SHOW_HIDDEN_FILES_TOGGLE
  ShowStatusMessage(statusBar(), Config.ShowHiddenFiles ? QStringLiteral("Hidden files are shown.") :
    QStringLiteral("Hidden files are hidden."), 3000);
}

void CommanderWindow::ApplyConfiguration()
{
  // Configuration::Changed (Preferences, Import, another instance): settings
  // that affect open sessions, panels and the window
  RebuildCustomCommands();
  Configuration & Config = Configuration::Instance();
  FA->ApplyCommanderShortcuts(Config.ExplorerKeyboardShortcuts);
  FA->Get(QStringLiteral("ShowHiddenFilesAction"))->setChecked(Config.ShowHiddenFiles);
  FA->Get(QStringLiteral("AutoReadDirectoryAfterOpAction"))->setChecked(Config.AutoReadDirectoryAfterOp);
  for (SessionTab * Tab : FTabs)
  {
    if (Tab->Queue != nullptr)
    {
      Tab->Queue->SetTransfersLimit(Config.QueueTransfersLimit);
      // Background transfer preferences (Preferences > Background, Endurance)
      Tab->Queue->ApplyConfiguration();
    }
    if (Tab->Term != nullptr)
    {
      Tab->Term->SetSettings(Terminal::ConfigurationSettings());
    }
    Tab->LocalProvider.ShowHiddenFiles = Config.ShowHiddenFiles;
    Tab->OtherLocalProvider.ShowHiddenFiles = Config.ShowHiddenFiles;
    if (Tab->RemoteProvider) Tab->RemoteProvider->ShowHiddenFiles = Config.ShowHiddenFiles;
    Tab->LocalPanel->SetShowHiddenFiles(Config.ShowHiddenFiles);
    Tab->RemotePanel->SetShowHiddenFiles(Config.ShowHiddenFiles);
    Tab->LocalPanel->ApplyConfiguration();
    Tab->RemotePanel->ApplyConfiguration();
    UpdateTabCaption(Tab);
  }
  UpdateTransferCombo();
  UpdateQueueView();
  UpdateActions();
  UpdateTitle();
}

void CommanderWindow::Synchronize(bool KeepUpToDate)
{
  // Commands > Synchronize (forms/FullSynchronize) / Keep Remote Directory up to Date (forms/Synchronize)
  SessionTab * Tab = CurrentTab();
  if (!Connected(Tab) || !CheckReady())
  {
    return;
  }
  QStringList SelectedNames;
  for (FilePanel * Panel : {Tab->LocalPanel, Tab->RemotePanel})
  {
    for (const RemoteFile & File : Panel->SelectedFiles())
    {
      if (!SelectedNames.contains(File.FileName)) SelectedNames.append(File.FileName);
    }
  }
  // Selection defaults to the focused file; "Selected files only" only makes sense with real selection
  if ((SelectedNames.size() == 1) && (Tab->LocalPanel->MarkedCount() == 0) &&
      (Tab->RemotePanel->MarkedCount() == 0))
  {
    SelectedNames.clear();
  }
  Tab->OperationRunning = true;
  UpdateActions();
  auto Finished = [this, Tab]()
  {
    if (!FTabs.contains(Tab)) return;
    Tab->OperationRunning = false;
    RefreshTab(Tab);
    UpdateQueueView();
    UpdateActions();
  };
  CopyParam Param = CopyParamPresets::Instance().CurrentCopyParam();
  if (KeepUpToDate)
  {
    RunKeepRemoteUpToDate(this, Tab->Term, Tab->LocalPanel->CurrentDirectory(), Tab->RemotePanel->CurrentDirectory(),
      Param, SelectedNames, Finished);
  }
  else
  {
    RunFullSynchronize(this, Tab->Term, Tab->LocalPanel->CurrentDirectory(), Tab->RemotePanel->CurrentDirectory(),
      ActivePanel() == Tab->LocalPanel, Param, SelectedNames, Finished);
  }
}

void CommanderWindow::CompareDirectories()
{
  // Commands > Compare Directories (Shift+F2): marks files that differ in both panels
  SessionTab * Tab = CurrentTab();
  QString LocalDir = Tab->LocalPanel->CurrentDirectory();
  if (Tab->LocalBrowser)
  {
    // Local-local tab: the right panel's listing
    QString Canonical;
    RemoteFileList Files;
    QString Error;
    QString More;
    if (!LocalDirectoryProvider::ReadDirectory(Tab->RemotePanel->CurrentDirectory(), Canonical, Files, Error, More))
    {
      ShowErrorMessage(this, Error, More);
      return;
    }
    RunCompareDirectories(this, LocalDir, Files, [Tab](const QStringList & LocalNames, const QStringList & RemoteNames)
    {
      Tab->LocalPanel->SelectFiles(LocalNames);
      Tab->RemotePanel->SelectFiles(RemoteNames);
    });
    return;
  }
  if (!Connected(Tab))
  {
    return;
  }
  Tab->Term->ReadDirectory(Tab->RemotePanel->CurrentDirectory(),
    [this, Tab, LocalDir](const OperationResult & Result, const QString &, const RemoteFileList & Files)
    {
      if (!FTabs.contains(Tab)) return;
      if (!Result.Ok)
      {
        ShowResultError(Result);
        return;
      }
      RunCompareDirectories(this, LocalDir, Files, [Tab](const QStringList & LocalNames, const QStringList & RemoteNames)
      {
        Tab->LocalPanel->SelectFiles(LocalNames);
        Tab->RemotePanel->SelectFiles(RemoteNames);
      });
    });
}

//---------------------------------------------------------------------------
// Synchronized browsing (TScpCommanderForm::SynchronizeBrowsing)

void CommanderWindow::SynchronizeBrowsingChanged()
{
  // TCustomScpExplorerForm::SynchronizeBrowsingChanged (SYNC_DIR_BROWSE_TOGGLE)
  SessionTab * Tab = CurrentTab();
  bool On = FA->Get(QStringLiteral("SynchronizeBrowsingAction2"))->isChecked() && Connected(Tab);
  Tab->SynchronizeBrowsing = On;
  Tab->SyncPrevPath[0] = Tab->LocalPanel->CurrentDirectory();
  Tab->SyncPrevPath[1] = Tab->RemotePanel->CurrentDirectory();
  ShowStatusMessage(statusBar(), QStringLiteral("Synchronized browsing was turned %1.").arg(On ? QStringLiteral("on") : QStringLiteral("off")), 3000);
}

// SynchronizeBrowsingLocal / SynchronizeBrowsingRemote: the opposite panel
// follows the same relative change (going up to the common path first)
static bool MapSynchronizedPath(const QString & PrevPath, const QString & NewPath, bool SourceRemote,
  const QString & OtherPath, QString & Result)
{
  auto Split = [](const QString & Path)
  {
    return Path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
  };
  QStringList Prev = Split(PrevPath);
  QStringList New = Split(NewPath);
  int Common = 0;
  while ((Common < Prev.size()) && (Common < New.size()) && (Prev[Common] == New[Common]))
  {
    Common++;
  }
  QStringList Other = Split(OtherPath);
  int Up = Prev.size() - Common;
  if (Up > Other.size())
  {
    return false;
  }
  Other = Other.mid(0, Other.size() - Up);
  Other.append(New.mid(Common));
  Q_UNUSED(SourceRemote);
  Result = QLatin1Char('/') + Other.join(QLatin1Char('/'));
  return true;
}

void CommanderWindow::PanelDirectoryChanged(SessionTab * Tab, FilePanel * Panel)
{
  if (Tab->LocalBrowser)
  {
    return;
  }
  int Side = (Panel == Tab->LocalPanel) ? 0 : 1;
  QString PrevPath = Tab->SyncPrevPath[Side];
  QString NewPath = Panel->CurrentDirectory();
  Tab->SyncPrevPath[Side] = NewPath;
  if (!Tab->SyncPending[Side].isEmpty())
  {
    // The change made by synchronized browsing itself
    Tab->SyncPending[Side].clear();
    return;
  }
  if (!Tab->SynchronizeBrowsing || !Connected(Tab) || PrevPath.isEmpty() || (PrevPath == NewPath))
  {
    return;
  }
  FilePanel * Other = (Side == 0) ? Tab->RemotePanel : Tab->LocalPanel;
  QString Target;
  if (!MapSynchronizedPath(PrevPath, NewPath, Side == 1, Other->CurrentDirectory(), Target))
  {
    // EAbort: no fallback
    Tab->SynchronizeBrowsing = false;
    FA->Get(QStringLiteral("SynchronizeBrowsingAction2"))->setChecked(false);
    MessageDialog(this, QStringLiteral("Cannot open corresponding directory in the opposite panel. "
      "Directory browsing synchronisation failed. It has been turned off."), qtInformation, qaOK);
    return;
  }
  if (Target == Other->CurrentDirectory())
  {
    return;
  }
  if (!Other->IsRemote() && !QFileInfo(Target).isDir())
  {
    SynchronizeBrowsingFailed(Tab, Target, false, QString());
    return;
  }
  Tab->SyncPending[1 - Side] = Target;
  Other->ChangeDirectory(Target);
}

void CommanderWindow::SynchronizeBrowsingFailed(SessionTab * Tab, const QString & Path, bool CreateRemote, const QString & More)
{
  // SYNC_DIR_BROWSE_CREATE2: offer to create the missing directory
  if (MoreMessageDialog(this, QStringLiteral("**Do you want to try to create directory '%1'?**\n\n"
        "Cannot open corresponding directory in the opposite panel.").arg(Path), More, qtConfirmation, qaYes | qaNo) != qaYes)
  {
    Tab->SynchronizeBrowsing = false;
    if (Tab == CurrentTab())
    {
      FA->Get(QStringLiteral("SynchronizeBrowsingAction2"))->setChecked(false);
    }
    return;
  }
  auto Failed = [this, Tab](const QString & Message, const QString & Details)
  {
    if (!FTabs.contains(Tab)) return;
    ShowErrorMessage(this, Message, Details);
    Tab->SynchronizeBrowsing = false;
    if (Tab == CurrentTab())
    {
      FA->Get(QStringLiteral("SynchronizeBrowsingAction2"))->setChecked(false);
    }
    MessageDialog(this, QStringLiteral("Cannot open corresponding directory in the opposite panel. "
      "Directory browsing synchronisation failed. It has been turned off."), qtInformation, qaOK);
  };
  if (!CreateRemote)
  {
    if (!QDir().mkpath(Path))
    {
      Failed(QStringLiteral("Cannot create directory '%1'.").arg(Path), QString());
      return;
    }
    Tab->SyncPending[0] = Path;
    Tab->LocalPanel->ChangeDirectory(Path);
    return;
  }
  if (!Connected(Tab)) return;
  Tab->Term->CreateDirectory(Path, RemoteProperties(), [this, Tab, Path, Failed](const OperationResult & Result)
  {
    if (!FTabs.contains(Tab) || !Connected(Tab)) return;
    if (!Result.Ok)
    {
      Failed(Result.Message, Result.MoreMessages);
      return;
    }
    Tab->SyncPending[1] = Path;
    Tab->RemotePanel->ChangeDirectory(Path);
  });
}

//---------------------------------------------------------------------------
// Closing (TCustomScpExplorerForm::FormCloseQuery)

void CommanderWindow::closeEvent(QCloseEvent * Event)
{
  for (SessionTab * Tab : FTabs)
  {
    if (Connected(Tab) && Tab->OperationRunning)
    {
      // NonVisualDataModule->Busy
      ShowStatusMessage(statusBar(), QStringLiteral("Cannot close the application while an operation is in progress."), 3000);
      QApplication::beep();
      Event->ignore();
      return;
    }
  }
  Configuration & Config = Configuration::Instance();
  int ActiveSessions = 0;
  for (SessionTab * Tab : FTabs)
  {
    ActiveSessions += Connected(Tab) ? 1 : 0;
  }
  // Confirm, when there's at least one active remote session or when there
  // are multiple (inactive or local) sessions and auto workspace saving is not enabled
  if (((ActiveSessions > 0) || ((FTabs.size() > 1) && !Config.AutoSaveWorkspace)) && Config.ConfirmClosingSession &&
      !QCoreApplication::closingDown())
  {
    QString Message;
    unsigned int Answers = qaOK | qaCancel;
    if ((ActiveSessions == 1) && Connected(CurrentTab()))
    {
      // CLOSE_SESSION_WORKSPACE / CLOSE_SESSION
      Message = (!Config.AutoSaveWorkspace ?
        QStringLiteral("Terminate session '%1' and close application without saving a workspace?") :
        QStringLiteral("Terminate session '%1' and close application?")).arg(CurrentTab()->Data.SessionName());
    }
    else if (ActiveSessions > 0)
    {
      // CLOSE_SESSIONS_WORKSPACE3 / CLOSE_SESSIONS
      Message = !Config.AutoSaveWorkspace ?
        QStringLiteral("Terminate all sessions and close application without saving a workspace?") :
        QStringLiteral("Terminate all sessions and close application?");
    }
    else
    {
      // CLOSE_WORKSPACE
      Message = QStringLiteral("Close application without saving a workspace?");
    }
    QString Note;
    if (Config.AutoSaveWorkspace)
    {
      // AUTO_WORKSPACE
      Note = QStringLiteral("Workspace '%1' will be automatically saved.").arg(Workspaces::WorkspaceName(Config));
    }
    else
    {
      // AUTO_WORKSPACE_ENABLE
      Note = QStringLiteral("Press 'No' to enable automatic saving of the workspace.");
      Answers = qaYes | qaNo | qaCancel;
    }
    MessageParams Params;
    Params.Params = qpNeverAskAgainCheck;
    unsigned int Result = MessageDialog(this, QStringLiteral("**%1**\n\n%2").arg(Message, Note), qtConfirmation, Answers,
      QStringLiteral("workspace"), &Params);
    bool CanClose;
    if (Result == qaNeverAskAgain)
    {
      Config.ConfirmClosingSession = false;
      CanClose = true;
    }
    else if (Result == qaNo)
    {
      // Save the workspace and enable its automatic saving
      QList<SessionData> Tabs = WorkspaceTabs();
      CanClose = Tabs.isEmpty() || SaveWorkspaceDialog::SaveWorkspace(this, Tabs, true);
    }
    else
    {
      CanClose = (Result == qaOK) || (Result == qaYes);
    }
    if (!CanClose)
    {
      Event->ignore();
      return;
    }
  }
  for (SessionTab * Tab : FTabs)
  {
    if (!CanCloseQueue(Tab))
    {
      Event->ignore();
      return;
    }
  }
  // Edited files not uploaded yet (PENDING_EDITORS), internal editors with changes
  if (!FEditorManager->CanClose())
  {
    Event->ignore();
    return;
  }
  // Environment > Window > "Automatically save workspace on exit"
  QList<SessionData> Tabs = WorkspaceTabs();
  if (Config.AutoSaveWorkspace && !Tabs.isEmpty())
  {
    Workspaces::SaveWorkspace(Config.StoredSessions(), Workspaces::WorkspaceName(Config), Tabs, Config.AutoSaveWorkspacePasswords);
  }
  FClosing = true;
  SaveToolbarSettings();
  Config.WindowGeometry = saveGeometry();
  Config.WindowState = saveState(WindowStateVersion);
  Config.PanelSplitter = CurrentTab()->Page->saveState();
  Config.LastLocalDirectory = CurrentTab()->LocalPanel->CurrentDirectory();
  Config.Save();
  // NixSCP Pro: settings changed just now and the open tabs reach the cloud
  CloudSyncUi::FlushOnExit(this, Tabs);
  Event->accept();
}

//---------------------------------------------------------------------------
// Workspaces (forms/CustomScpExplorer.cpp SaveWorkspace / TTerminalManager)

QList<SessionData> CommanderWindow::WorkspaceTabs() const
{
  QList<SessionData> Result;
  for (SessionTab * Tab : FTabs)
  {
    if (!Connected(Tab))
    {
      continue;
    }
    SessionData Data = Tab->Data;
    Data.RemoteDirectory = Tab->RemotePanel->CurrentDirectory();
    Data.LocalDirectory = Tab->LocalPanel->CurrentDirectory();
    Data.SynchronizeBrowsing = Tab->SynchronizeBrowsing;
    if (Data.Password.isEmpty())
    {
      Data.Password = Tab->SessionPassword;
    }
    Result.append(Data);
  }
  return Result;
}

void CommanderWindow::SaveWorkspace()
{
  QList<SessionData> Tabs = WorkspaceTabs();
  if (!Tabs.isEmpty())
  {
    SaveWorkspaceDialog::SaveWorkspace(this, Tabs);
  }
}

void CommanderWindow::OpenWorkspace(const QString & Name)
{
  QList<SessionData> Sessions = Workspaces::FolderOrWorkspace(Configuration::Instance().StoredSessions(), Name);
  if (Sessions.isEmpty())
  {
    ShowErrorMessage(this, QStringLiteral("Workspace '%1' does not contain any session.").arg(Name));
    return;
  }
  for (const SessionData & Data : Sessions)
  {
    SessionData Copy = Data;
    Copy.IsWorkspace = false;
    OpenSession(Copy);
  }
  if (Configuration::Instance().StoredSessions().IsWorkspace(Name))
  {
    Workspaces::SetLastWorkspace(Name);
  }
}

//---------------------------------------------------------------------------
// Custom commands (forms/CustomScpExplorer.cpp CustomCommand*, NonVisual CustomCommandsMenu)

CustomCommandContext CommanderWindow::CustomCommandsContext() const
{
  SessionTab * Tab = CurrentTab();
  CustomCommandContext Context;
  if (Connected(Tab))
  {
    Context.Term = Tab->Term;
    Context.SessionPassword = Tab->SessionPassword;
    Context.CurrentRemoteDir = Tab->RemotePanel->CurrentDirectory();
    Context.SelectedRemoteFiles = Tab->RemotePanel->SelectedPaths();
  }
  Context.CurrentLocalDir = Tab->LocalPanel->CurrentDirectory();
  Context.SelectedLocalFiles = Tab->LocalPanel->SelectedPaths();
  Context.LocalSide = (ActivePanel() == Tab->LocalPanel) || Tab->LocalBrowser;
  Context.RemotePanel = !Tab->LocalBrowser;
  return Context;
}

void CommanderWindow::RebuildCustomCommands()
{
  // Actions with the commands' shortcuts live on the window, so shortcuts work without opening a menu
  for (QAction * Action : FCustomCommandActions)
  {
    removeAction(Action);
    delete Action;
  }
  FCustomCommandActions.clear();
  CustomCommandList List = CustomCommandList::Configured();
  for (int Index = 0; Index < List.Count(); Index++)
  {
    const CustomCommandType Command = List.At(Index);
    auto * Action = new QAction(Command.DisplayName(), this);
    if (Command.ShortCut != 0)
    {
      Action->setShortcut(LocationProfilesDialog::ShortCutToKeySequence(Command.ShortCut));
      Action->setShortcutContext(Qt::WindowShortcut);
    }
    connect(Action, &QAction::triggered, this, [this, Command]()
    {
      CustomCommandContext Context = CustomCommandsContext();
      if (CustomCommandState(Command, CustomCommandRunner::Availability(Context)) <= 0)
      {
        QApplication::beep();
        return;
      }
      SessionTab * Tab = CurrentTab();
      CustomCommandRunner::Execute(this, Command, Context, [this, Tab]()
      {
        if (FTabs.contains(Tab)) RefreshTab(Tab);
      });
    });
    addAction(Action);
    FCustomCommandActions.append(Action);
  }
  // Custom Command Buttons toolbar (CustomCommandsToolbar)
  if (FCustomCommandsBar != nullptr)
  {
    FCustomCommandsBar->clear();
    for (QAction * Action : FCustomCommandActions)
    {
      FCustomCommandsBar->addAction(Action);
    }
  }
}

void CommanderWindow::FillCustomCommandsMenu(QMenu * Menu, bool FileCommands)
{
  Menu->clear();
  CustomCommandList List = CustomCommandList::Configured();
  CustomCommandAvailability Availability = CustomCommandRunner::Availability(CustomCommandsContext());
  for (int Index = 0; (Index < List.Count()) && (Index < FCustomCommandActions.size()); Index++)
  {
    int State = CustomCommandState(List.At(Index), Availability, FileCommands ? ccltFile : ccltNonFile);
    if (State >= 0)
    {
      QAction * Action = FCustomCommandActions[Index];
      Action->setEnabled(State > 0);
      Menu->addAction(Action);
    }
  }
  if (!Menu->isEmpty())
  {
    Menu->addSeparator();
  }
  Menu->addAction(FA->Get(QStringLiteral("CustomCommandsEnterAction")));
  Menu->addAction(FA->Get(QStringLiteral("CustomCommandsCustomizeAction")));
}
