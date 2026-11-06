// Port of WinSCP source/forms/Login.cpp, forms/ImportSessions.cpp,
// forms/Cleanup.cpp and TSaveSessionDialog/DoSaveSession of forms/Custom.cpp
#include "gui/LoginDialog.h"
#include "core/CloudSettings.h"
#include "gui/TeamsDialog.h"
#include "core/TeamSync.h"
#include "gui/AgentAccessDialog.h"
#include "core/Platform.h"
#include "gui/CloudSyncUi.h"
#include "gui/DesktopIntegration.h"
#include "gui/Translator.h"
#include "gui/HelpSystem.h"
#include "gui/SiteAdvancedDialog.h"
#include "gui/PreferencesDialog.h"
#include "gui/GuiUserInterface.h"
#include "core/Configuration.h"
#include "core/IniStorage.h"
#include "core/S3FileSystem.h"
#include "core/Workspaces.h"
#include "gui/Icons.h"

#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QDrag>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QScreen>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <functional>
#include <unistd.h>

static const int ItemTypeRole = Qt::UserRole + 1;
static const int ItemNameRole = Qt::UserRole + 2;

// FSOrder: order of TransferProtocolCombo items
static const TFSProtocol FSOrder[] = { fsSFTPonly, fsSCPonly, fsFTP, fsWebDAV, fsS3 };
static const int FSOrderCount = static_cast<int>(sizeof(FSOrder) / sizeof(FSOrder[0]));
static const QString S3HostName = QStringLiteral("s3.amazonaws.com");
static const QString AnonymousUserName = QStringLiteral("anonymous");
static const QString AnonymousPassword = QStringLiteral("anonymous@example.com");
static const QString InterfaceKey = QStringLiteral("Interface");
static const QString ConfigurationKey = QStringLiteral("Configuration");

//---------------------------------------------------------------------------
// Session tree with WinSCP's drag & drop of sites into folders
// (SessionTreeStartDrag / SessionTreeProc / SessionTreeDragDrop)

class SessionTreeWidget : public QTreeWidget
{
public:
  explicit SessionTreeWidget(QWidget * Parent = nullptr) : QTreeWidget(Parent) {}

  bool IsEditing() const { return (state() == QAbstractItemView::EditingState); }

  std::function<bool(QTreeWidgetItem *)> CanDrag;
  std::function<QTreeWidgetItem *(QTreeWidgetItem *)> NormalizeTarget;
  std::function<bool(QTreeWidgetItem *)> AllowDrop;
  std::function<void(QTreeWidgetItem *, QTreeWidgetItem *)> Dropped;

protected:
  void startDrag(Qt::DropActions SupportedActions) override
  {
    // neither folders/workspaces, nor New Site, nor special sites can be dragged
    if (CanDrag && CanDrag(currentItem()))
    {
      QTreeWidget::startDrag(SupportedActions);
    }
  }

  void dragEnterEvent(QDragEnterEvent * Event) override
  {
    if (Event->source() == this)
    {
      Event->acceptProposedAction();
    }
    else
    {
      Event->ignore();
    }
  }

  void dragMoveEvent(QDragMoveEvent * Event) override
  {
    if (Event->source() != this)
    {
      Event->ignore();
      return;
    }
    // autoscroll
    QTreeWidget::dragMoveEvent(Event);
    QTreeWidgetItem * Target = NormalizeTarget(itemAt(Event->position().toPoint()));
    SetDropTarget(Target);
    if (AllowDrop(Target))
    {
      Event->setDropAction(Qt::CopyAction);
      Event->accept();
    }
    else
    {
      Event->ignore();
    }
  }

  void dragLeaveEvent(QDragLeaveEvent * Event) override
  {
    SetDropTarget(nullptr);
    QTreeWidget::dragLeaveEvent(Event);
  }

  void dropEvent(QDropEvent * Event) override
  {
    SetDropTarget(nullptr);
    QTreeWidgetItem * Node = currentItem();
    QTreeWidgetItem * Target = NormalizeTarget(itemAt(Event->position().toPoint()));
    // never let QTreeWidget move the items itself
    Event->setDropAction(Qt::CopyAction);
    Event->accept();
    if ((Event->source() == this) && (Node != nullptr) && AllowDrop(Target))
    {
      // the tree gets reloaded, do not do that inside of the drag
      QPersistentModelIndex NodeIndex = indexFromItem(Node);
      QPersistentModelIndex TargetIndex = (Target != nullptr) ? QPersistentModelIndex(indexFromItem(Target)) : QPersistentModelIndex();
      bool RootTarget = (Target == nullptr);
      QTimer::singleShot(0, this, [this, NodeIndex, TargetIndex, RootTarget]()
      {
        if (NodeIndex.isValid() && (RootTarget || TargetIndex.isValid()))
        {
          Dropped(itemFromIndex(NodeIndex), RootTarget ? nullptr : itemFromIndex(TargetIndex));
        }
      });
    }
  }

private:
  QTreeWidgetItem * FDropTarget = nullptr;

  void SetDropTarget(QTreeWidgetItem * Target)
  {
    if (FDropTarget != Target)
    {
      if (FDropTarget != nullptr)
      {
        FDropTarget->setBackground(0, QBrush());
      }
      FDropTarget = Target;
      if (FDropTarget != nullptr)
      {
        QColor Color = palette().color(QPalette::Highlight);
        Color.setAlpha(80);
        FDropTarget->setBackground(0, Color);
      }
    }
  }
};

//---------------------------------------------------------------------------
// Helpers

static QMessageBox::StandardButton Confirm(QWidget * Parent, const QString & Message,
  QMessageBox::StandardButtons Buttons = QMessageBox::Ok | QMessageBox::Cancel)
{
  return QMessageBox::question(Parent, QStringLiteral("Confirm"), Message, Buttons);
}

// TStringList::CommaText
static QString ToCommaText(const QStringList & List)
{
  QStringList Items;
  for (const QString & S : List)
  {
    bool NeedQuote = S.isEmpty();
    for (QChar C : S)
    {
      if ((C <= QLatin1Char(' ')) || (C == QLatin1Char(',')) || (C == QLatin1Char('"')))
      {
        NeedQuote = true;
        break;
      }
    }
    Items.append(NeedQuote ? (QLatin1Char('"') + QString(S).replace(QStringLiteral("\""), QStringLiteral("\"\"")) + QLatin1Char('"')) : S);
  }
  return Items.join(QLatin1Char(','));
}

static QStringList FromCommaText(const QString & Text)
{
  QStringList Result;
  qsizetype P = 0;
  auto SkipWhitespace = [&]()
  {
    while ((P < Text.size()) && (Text[P] <= QLatin1Char(' ')))
    {
      P++;
    }
  };
  SkipWhitespace();
  while (P < Text.size())
  {
    QString Item;
    if (Text[P] == QLatin1Char('"'))
    {
      P++;
      while (P < Text.size())
      {
        if (Text[P] == QLatin1Char('"'))
        {
          if ((P + 1 < Text.size()) && (Text[P + 1] == QLatin1Char('"')))
          {
            Item += QLatin1Char('"');
            P += 2;
            continue;
          }
          P++;
          break;
        }
        Item += Text[P++];
      }
    }
    else
    {
      while ((P < Text.size()) && (Text[P] > QLatin1Char(' ')) && (Text[P] != QLatin1Char(',')))
      {
        Item += Text[P++];
      }
    }
    Result.append(Item);
    SkipWhitespace();
    if ((P < Text.size()) && (Text[P] == QLatin1Char(',')))
    {
      P++;
      SkipWhitespace();
    }
  }
  return Result;
}

// ContainsTextSemiCaseSensitive
static bool ContainsTextSemiCaseSensitive(const QString & Text, const QString & SubText)
{
  if (SubText.toLower() == SubText)
  {
    return Text.contains(SubText, Qt::CaseInsensitive);
  }
  return Text.contains(SubText, Qt::CaseSensitive);
}

static bool OpenInNewWindow()
{
  return (QGuiApplication::keyboardModifiers() & Qt::ShiftModifier) != 0;
}

static void ExecuteNewInstance(const QString & Param)
{
  QProcess::startDetached(QCoreApplication::applicationFilePath(), QStringList{Param});
}

// Linux adaptation of CreateDesktopSessionShortCut: a .desktop launcher on
// the XDG desktop, starting NixSCP with the site/folder/workspace name
static bool CreateDesktopSessionShortCut(const QString & Name, QString * Error)
{
  QString Desktop = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
  if (Desktop.isEmpty())
  {
    Desktop = QDir::homePath() + QStringLiteral("/Desktop");
  }
  QDir().mkpath(Desktop);
  QString Base = Name;
  Base.replace(QLatin1Char('/'), QLatin1Char('-'));
  QString FileName = Desktop + QLatin1Char('/') + Base + QStringLiteral(".desktop");
  // Desktop Entry Specification: quote arguments, escape " ` $ and backslash
  auto Quote = [](const QString & Arg)
  {
    QString Escaped = Arg;
    Escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    Escaped.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    Escaped.replace(QLatin1Char('`'), QStringLiteral("\\`"));
    Escaped.replace(QLatin1Char('$'), QStringLiteral("\\$"));
    Escaped.replace(QLatin1Char('%'), QStringLiteral("%%"));
    return QLatin1Char('"') + Escaped + QLatin1Char('"');
  };
  QString Contents = QStringLiteral(
    "[Desktop Entry]\n"
    "Type=Application\n"
    "Name=%1\n"
    "Comment=NixSCP\n"
    "Exec=%2 %3\n"
    "Icon=nixscp\n"
    "Terminal=false\n").arg(Name, Quote(QCoreApplication::applicationFilePath()), Quote(Name));
  QFile F(FileName);
  if (!F.open(QIODevice::WriteOnly | QIODevice::Truncate))
  {
    if (Error != nullptr)
    {
      *Error = QStringLiteral("Cannot create file \"%1\".").arg(FileName);
    }
    return false;
  }
  F.write(Contents.toUtf8());
  F.close();
  // desktops only start executable launchers
  F.setPermissions(F.permissions() | QFileDevice::ExeOwner | QFileDevice::ExeUser);
  return true;
}

// Testing aid (offscreen GUI checks): NIXSCP_SCREENSHOT_LOGIN=<file.png> saves
// the topmost modal dialog and quits; NIXSCP_SCREENSHOT_LOGIN_ACTION selects
// what to show first (import, saveas, raw, cleanup, search, edit, advanced)
static void ScheduleScreenshot(QWidget * Fallback)
{
  QString FileName = qEnvironmentVariable("NIXSCP_SCREENSHOT_LOGIN");
  QTimer::singleShot(qEnvironmentVariableIntValue("NIXSCP_SCREENSHOT_DELAY") > 0 ? qEnvironmentVariableIntValue("NIXSCP_SCREENSHOT_DELAY") : 700,
    qApp, [Fallback, FileName]()
    {
      QWidget * Widget = QApplication::activeModalWidget();
      if (Widget == nullptr)
      {
        Widget = Fallback;
      }
      Widget->grab().save(FileName);
      QCoreApplication::exit(0);
    });
}

static bool ScreenshotMode()
{
  return !qEnvironmentVariableIsEmpty("NIXSCP_SCREENSHOT_LOGIN") || !qEnvironmentVariableIsEmpty("NIXSCP_SCREENSHOT");
}

static QString ReadInterfaceString(const QString & Name, const QString & Default = QString())
{
  IniStorage Storage(&Configuration::Instance().Ini());
  if (Storage.OpenSubKey(ConfigurationKey, false) && Storage.OpenSubKey(InterfaceKey, false))
  {
    return Storage.ReadString(Name, Default);
  }
  return Default;
}

//---------------------------------------------------------------------------
// LoginDialog

std::function<void(const QList<SessionData> &, QWidget *)> LoginDialog::OpenInPuttyHook;
std::function<void(const SessionData &, QWidget *)> LoginDialog::GenerateUrlHook;
std::function<void(const SessionData &, QWidget *)> LoginDialog::CopyParamRuleHook;

LoginDialog::LoginDialog(StoredSessionList & Sessions, QWidget * Parent) :
  QDialog(Parent),
  FSessions(Sessions),
  FSiteSearch(isName)
{
  setWindowTitle(QStringLiteral("Login"));
  resize(873, 411);
  setMinimumSize(660, 399);
  CreateActions();
  CreateControls();
  UpdateClipboardUrl();
  connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, [this]() { UpdateClipboardUrl(); });
  ResetNewSiteData();
  LoadSessions();
  // NixSCP Teams: sites shared since the last sync show up right away
  if (TeamSync::Instance().Available())
  {
    TeamSync::Instance().SyncAll();
  }
  connect(&Configuration::Instance(), &Configuration::StoredSessionsChanged, this, [this]()
  {
    // another instance saved the sites, or a team's sites changed
    if (!FEditing && !FRenaming)
    {
      ReloadSessions(SessionNodePath(Selected()) + (IsFolderNode(Selected()) ? QStringLiteral("/") : QString()));
    }
  });
}

LoginDialog::~LoginDialog()
{
}

QMenu * LoginDialog::CreateSearchOptionsMenu(QWidget * Parent)
{
  auto * Menu = new QMenu(QStringLiteral("&Options"), Parent);
  Menu->addAction(SearchSiteNameStartOnlyAction);
  Menu->addAction(SearchSiteNameAction);
  Menu->addAction(SearchSiteAction);
  return Menu;
}

void LoginDialog::CreateActions()
{
  auto Action = [this](const QString & Caption, auto Slot)
  {
    auto * A = new QAction(Caption, this);
    connect(A, &QAction::triggered, this, Slot);
    return A;
  };
  EditSessionAction = Action(QStringLiteral("&Edit"), [this]() { EditSessionActionExecute(); });
  SaveAsSessionAction = Action(QStringLiteral("Save &As..."), [this]() { SaveAsSession(true); });
  SaveAsSessionAction->setShortcut(QKeySequence(Qt::ALT | Qt::Key_A));
  SaveAsSessionAction->setShortcutContext(Qt::WindowShortcut);
  addAction(SaveAsSessionAction);
  SaveSessionAction = Action(QStringLiteral("&Save"), [this]() { SaveSessionActionExecute(); });
  DeleteSessionAction = Action(QStringLiteral("&Delete"), [this]() { DeleteSessionActionExecute(); });
  DeleteSessionAction->setIcon(Icons::Get(QStringLiteral("delete")));
  ImportSessionsAction = Action(QStringLiteral("&Import Sites..."), [this]() { ImportSessionsActionExecute(); });
  LoginAction = Action(QStringLiteral("Login"), [this]() { LoginActionExecute(); });
  AboutAction = Action(QStringLiteral("A&bout..."), [this]() { AboutActionExecute(); });
  CleanUpAction = Action(QStringLiteral("&Clean Up..."), [this]() { CleanUpActionExecute(); });
  ResetNewSessionAction = Action(QStringLiteral("&Reset"), [this]() { ResetNewSessionActionExecute(); });
  SetDefaultSessionAction = Action(QStringLiteral("Set De&faults"), [this]() { SetDefaultSessionActionExecute(); });
  DesktopIconAction = Action(QStringLiteral("Desktop &Icon"), [this]() { DesktopIconActionExecute(); });
  DesktopIconAction->setVisible(DesktopIntegration::CanCreateSessionShortcut());
  RenameSessionAction = Action(QStringLiteral("&Rename"), [this]() { RenameSessionActionExecute(); });
  RenameSessionAction->setIcon(Icons::Get(QStringLiteral("rename")));
  NewSessionFolderAction = Action(QStringLiteral("Ne&w Folder..."), [this]() { NewSessionFolderActionExecute(); });
  NewSessionFolderAction->setIcon(Icons::Get(QStringLiteral("new-folder")));
  // Unix PuTTY tools; shown only when installed (FindTool)
  RunPageantAction = Action(QStringLiteral("Run Pagea&nt"), []()
  {
    QProcess::startDetached(QStandardPaths::findExecutable(QStringLiteral("pageant")), QStringList());
  });
  RunPageantAction->setVisible(!QStandardPaths::findExecutable(QStringLiteral("pageant")).isEmpty());
  RunPuttygenAction = Action(QStringLiteral("Run PuTTY&gen"), []()
  {
    QProcess::startDetached(QStandardPaths::findExecutable(QStringLiteral("puttygen")), QStringList());
  });
  RunPuttygenAction->setVisible(!QStandardPaths::findExecutable(QStringLiteral("puttygen")).isEmpty());
  ImportAction = Action(QStringLiteral("Import/&Restore Configuration..."), [this]() { ImportActionExecute(); });
  ExportAction = Action(QStringLiteral("&Export/Backup Configuration..."), [this]() { ExportActionExecute(); });
  PreferencesAction = Action(QStringLiteral("&Preferences..."), [this]()
  {
    PreferencesDialog::Execute(this);
    UpdateControls();
  });
  PreferencesAction->setIcon(Icons::Get(QStringLiteral("preferences")));
  EditCancelAction = Action(QStringLiteral("Cancel"), [this]() { EditCancelActionExecute(); });
  SessionAdvancedAction = Action(QStringLiteral("A&dvanced..."), [this]() { SessionAdvancedActionExecute(false); });
  // NixSCP Agent Access (gui/AgentAccessDialog.h)
  AgentAccessAction = Action(QStringLiteral("A&gent Access..."), [this]() { AgentAccessActionExecute(); });
  ShareWithTeamAction = Action(QStringLiteral("S&hare with Team..."), [this]() { ShareWithTeamActionExecute(); });
  StopSharingAction = Action(QStringLiteral("Stop S&haring with Team..."), [this]() { StopSharingActionExecute(); });
  PreferencesLoggingAction = Action(QStringLiteral("&Logging..."), [this]()
  {
    PreferencesDialog::Execute(this, QStringLiteral("Logging"));
    UpdateControls();
  });
  CloneToNewSiteAction = Action(QStringLiteral("&Clone to New Site"), [this]() { CloneToNewSite(); });
  PasteUrlAction = Action(QStringLiteral("Paste Session &URL"), [this]() { PasteUrlActionExecute(); });
  PasteUrlAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_V));
  auto * SearchGroup = new QActionGroup(this);
  SearchSiteNameStartOnlyAction = Action(QStringLiteral("&Beginning of Site Name Only"), [this]() { SetSiteSearch(isNameStartOnly); });
  SearchSiteNameAction = Action(QStringLiteral("&Any Part of Site Name"), [this]() { SetSiteSearch(isName); });
  SearchSiteAction = Action(QStringLiteral("All &Major Site Fields"), [this]() { SetSiteSearch(isAll); });
  for (QAction * A : {SearchSiteNameStartOnlyAction, SearchSiteNameAction, SearchSiteAction})
  {
    A->setCheckable(true);
    SearchGroup->addAction(A);
  }
  SessionRawAction = Action(QStringLiteral("Edit &Raw Settings..."), [this]() { SessionAdvancedActionExecute(true); });
  SearchSiteStartAction = Action(QStringLiteral("&Find Site"), [this]() { SearchSiteStartActionExecute(); });
  SearchSiteStartAction->setShortcuts({QKeySequence(Qt::CTRL | Qt::Key_F), QKeySequence(Qt::ALT | Qt::Key_F7), QKeySequence(Qt::Key_F3)});
  SearchSiteStartAction->setShortcutContext(Qt::WindowShortcut);
  addAction(SearchSiteStartAction);
  PuttyAction = Action(QStringLiteral("Open in &PuTTY"), [this]() { PuttyActionExecute(); });
  PuttyAction->setIcon(Icons::Get(QStringLiteral("putty")));
  PuttyAction->setShortcuts({QKeySequence(Qt::CTRL | Qt::Key_P), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_P)});
  PuttyAction->setShortcutContext(Qt::WindowShortcut);
  PuttyAction->setVisible(static_cast<bool>(OpenInPuttyHook));
  addAction(PuttyAction);
  GenerateUrlAction2 = Action(QStringLiteral("&Generate Session URL/Code..."), [this]()
  {
    PersistNewSiteIfNeeded();
    GenerateUrlHook(CurrentSessionData(), this);
  });
  GenerateUrlAction2->setVisible(static_cast<bool>(GenerateUrlHook));
  CopyParamRuleAction = Action(QStringLiteral("Transfer Settings &Rule..."), [this]()
  {
    CopyParamRuleHook(CurrentSessionData(), this);
  });
  CopyParamRuleAction->setVisible(static_cast<bool>(CopyParamRuleHook));
}

SessionData LoginDialog::CurrentSessionData()
{
  std::shared_ptr<SessionData> Site = SelectedSession();
  if (Site != nullptr)
  {
    return *Site;
  }
  SessionData Data;
  SaveSession(Data);
  Data.Name.clear();
  return Data;
}

void LoginDialog::PuttyActionExecute()
{
  UpdateActions();
  if (!PuttyAction->isEnabled() || !OpenInPuttyHook)
  {
    return;
  }
  // Shift: keep the Login dialog open
  bool Close = !OpenInNewWindow();
  ParseHostName();
  SaveDataList();
  QList<SessionData> Sessions = FSessionsToOpen;
  FSessionsToOpen.clear();
  FWorkspaceToOpen.clear();
  OpenInPuttyHook(Sessions, this);
  if (Close)
  {
    reject();
  }
}

void LoginDialog::CreateControls()
{
  // ---- SitesPanel ----
  SessionTree = new SessionTreeWidget();
  SessionTree->setHeaderHidden(true);
  SessionTree->setEditTriggers(QAbstractItemView::NoEditTriggers);
  SessionTree->setContextMenuPolicy(Qt::CustomContextMenu);
  SessionTree->setMinimumWidth(260);
  SessionTree->setDragEnabled(true);
  SessionTree->setAcceptDrops(true);
  SessionTree->setDropIndicatorShown(false);
  SessionTree->setDragDropMode(QAbstractItemView::DragDrop);
  SessionTree->setDefaultDropAction(Qt::CopyAction);
  SessionTree->installEventFilter(this);
  SessionTree->CanDrag = [this](QTreeWidgetItem * Node)
  {
    // (the site of a node can be gone: another instance, a team sync)
    std::shared_ptr<SessionData> Data = NodeSession(Node);
    return (Data != nullptr) && !Data->Special && !FEditing && !FRenaming;
  };
  SessionTree->NormalizeTarget = [this](QTreeWidgetItem * Target) { return NormalizeDropTarget(Target); };
  SessionTree->AllowDrop = [this](QTreeWidgetItem * Target) { return SessionAllowDrop(Target); };
  SessionTree->Dropped = [this](QTreeWidgetItem * Node, QTreeWidgetItem * Target) { SessionTreeDropped(Node, Target); };
  // PasteUrlAction (Ctrl+V) does not steal paste from the edit boxes
  PasteUrlAction->setShortcutContext(Qt::WidgetShortcut);
  SessionTree->addAction(PasteUrlAction);

  SitesIncrementalSearchLabel = new QLabel();
  SitesIncrementalSearchLabel->setFrameShape(QFrame::StyledPanel);
  SitesIncrementalSearchLabel->setContextMenuPolicy(Qt::CustomContextMenu);
  SitesIncrementalSearchLabel->setVisible(false);
  connect(SitesIncrementalSearchLabel, &QWidget::customContextMenuRequested, this, [this](const QPoint & Pos)
  {
    QMenu Menu;
    Menu.addAction(SearchSiteNameStartOnlyAction);
    Menu.addAction(SearchSiteNameAction);
    Menu.addAction(SearchSiteAction);
    UpdateActions();
    Menu.exec(SitesIncrementalSearchLabel->mapToGlobal(Pos));
  });

  ToolsMenuButton = new QPushButton(QStringLiteral("&Tools"));
  ManageButton = new QPushButton(QStringLiteral("&Manage"));
  ToolsPopupMenu = new QMenu(this);
  ToolsPopupMenu->addAction(ImportSessionsAction);
  ToolsPopupMenu->addSeparator();
  ToolsPopupMenu->addAction(ImportAction);
  ToolsPopupMenu->addAction(ExportAction);
  ToolsPopupMenu->addAction(CleanUpAction);
  if (RunPageantAction->isVisible() || RunPuttygenAction->isVisible())
  {
    ToolsPopupMenu->addSeparator();
    ToolsPopupMenu->addAction(RunPageantAction);
    ToolsPopupMenu->addAction(RunPuttygenAction);
  }
  ToolsPopupMenu->addSeparator();
  ToolsPopupMenu->addAction(PreferencesAction);
  ToolsPopupMenu->addAction(AboutAction);
  ToolsMenuButton->setMenu(ToolsPopupMenu);
  connect(ToolsPopupMenu, &QMenu::aboutToShow, this, [this]() { UpdateActions(); });
  // ManageButtonClick: menu of the selected node
  connect(ManageButton, &QPushButton::clicked, this, [this]()
  {
    QMenu * Menu = GetSelectedNodePopupMenu();
    Menu->exec(ManageButton->mapToGlobal(QPoint(0, ManageButton->height())));
    Menu->deleteLater();
  });
  connect(SessionTree, &QWidget::customContextMenuRequested, this, [this](const QPoint & Pos)
  {
    ResetSitesIncrementalSearch();
    QTreeWidgetItem * Node = SessionTree->itemAt(Pos);
    if (Node == nullptr)
    {
      return;
    }
    // SessionTreeMouseDown: right click selects
    if (Node != Selected())
    {
      SetSelected(Node);
      // selection may have been refused (editing)
      if (Node != Selected())
      {
        return;
      }
    }
    QMenu * Menu = GetSelectedNodePopupMenu();
    Menu->exec(SessionTree->viewport()->mapToGlobal(Pos));
    Menu->deleteLater();
  });

  auto * SitesButtons = new QHBoxLayout();
  SitesButtons->addWidget(ToolsMenuButton);
  SitesButtons->addStretch();
  SitesButtons->addWidget(ManageButton);
  auto * SitesPanel = new QVBoxLayout();
  SitesPanel->setSpacing(0);
  SitesPanel->addWidget(SessionTree, 1);
  SitesPanel->addWidget(SitesIncrementalSearchLabel);
  SitesPanel->addSpacing(6);
  SitesPanel->addLayout(SitesButtons);

  // ---- BasicGroup ("Session") ----
  BasicGroup = new QGroupBox(QStringLiteral("Session"));
  auto * Grid = new QGridLayout(BasicGroup);
  TransferProtocolCombo = new QComboBox();
  TransferProtocolCombo->addItems({QStringLiteral("SFTP"), QStringLiteral("SCP"), QStringLiteral("FTP"), QStringLiteral("WebDAV"), QStringLiteral("Amazon S3")});
  TransferProtocolView = new QLineEdit();
  TransferProtocolView->setReadOnly(true);
  FtpsCombo = new QComboBox();
  FtpsCombo->addItems({QStringLiteral("No encryption"), QStringLiteral("TLS/SSL Implicit encryption"), QStringLiteral("TLS/SSL Explicit encryption")});
  WebDavsCombo = new QComboBox();
  WebDavsCombo->addItems({QStringLiteral("No encryption"), QStringLiteral("TLS/SSL Implicit encryption")});
  EncryptionView = new QLineEdit();
  EncryptionView->setReadOnly(true);
  EncryptionLabel = new QLabel(QStringLiteral("&Encryption:"));
  HostNameEdit = new QLineEdit();
  PortNumberEdit = new QSpinBox();
  PortNumberEdit->setRange(1, 65535);
  PortNumberEdit->setMinimumWidth(80);
  UserNameEdit = new QLineEdit();
  PasswordEdit = new QLineEdit();
  PasswordEdit->setEchoMode(QLineEdit::Password);
  AnonymousLoginCheck = new QCheckBox(QStringLiteral("A&nonymous login"));

  auto Label = [](const QString & Caption, QWidget * Buddy)
  {
    auto * L = new QLabel(Caption);
    L->setBuddy(Buddy);
    return L;
  };
  for (int Col = 0; Col < 4; Col++)
  {
    Grid->setColumnStretch(Col, 1);
  }
  auto * ProtocolLabel = Label(QStringLiteral("&File protocol:"), TransferProtocolCombo);
  Grid->addWidget(ProtocolLabel, 0, 0, 1, 2);
  Grid->addWidget(EncryptionLabel, 0, 2, 1, 2);
  Grid->addWidget(TransferProtocolCombo, 1, 0, 1, 2);
  Grid->addWidget(TransferProtocolView, 1, 0, 1, 2);
  Grid->addWidget(FtpsCombo, 1, 2, 1, 2);
  Grid->addWidget(WebDavsCombo, 1, 2, 1, 2);
  Grid->addWidget(EncryptionView, 1, 2, 1, 2);
  Grid->addWidget(Label(QStringLiteral("&Hostname:"), HostNameEdit), 2, 0, 1, 3);
  Grid->addWidget(Label(QStringLiteral("Po&rt number:"), PortNumberEdit), 2, 3);
  Grid->addWidget(HostNameEdit, 3, 0, 1, 3);
  Grid->addWidget(PortNumberEdit, 3, 3);
  UserNameLabel = Label(QStringLiteral("&Username:"), UserNameEdit);
  PasswordLabel = Label(QStringLiteral("&Password:"), PasswordEdit);
  Grid->addWidget(UserNameLabel, 4, 0, 1, 2);
  Grid->addWidget(PasswordLabel, 4, 2, 1, 2);
  Grid->addWidget(UserNameEdit, 5, 0, 1, 2);
  Grid->addWidget(PasswordEdit, 5, 2, 1, 2);
  Grid->addWidget(AnonymousLoginCheck, 6, 0, 1, 2);
  // BasicS3Panel: S3CredentialsEnvCheck3 + S3ProfileCombo
  S3CredentialsEnvCheck = new QCheckBox(QStringLiteral("&Credentials from AWS environment:"));
  S3ProfileCombo = new QComboBox();
  S3ProfileCombo->setEditable(true);
  S3ProfileCombo->setInsertPolicy(QComboBox::NoInsert);
  Grid->addWidget(S3CredentialsEnvCheck, 6, 0, 1, 3);
  Grid->addWidget(S3ProfileCombo, 6, 3);

  // Save split button: Save / Save As... / Set Defaults (SaveDropDownMenu)
  // QToolButton would strip "&" and "..." from the action text
  SaveSessionAction->setIconText(SaveSessionAction->text());
  SessionAdvancedAction->setIconText(SessionAdvancedAction->text());
  SaveButton = new QToolButton();
  SaveButton->setDefaultAction(SaveSessionAction);
  SaveButton->setPopupMode(QToolButton::MenuButtonPopup);
  SaveButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
  SaveButton->setMinimumWidth(90);
  SaveDropDownMenu = new QMenu(this);
  SaveDropDownMenu->addAction(SaveSessionAction);
  SaveAsSessionMenuItem = SaveDropDownMenu->addAction(SaveAsSessionAction->text());
  connect(SaveAsSessionMenuItem, &QAction::triggered, SaveAsSessionAction, &QAction::trigger);
  SaveDropDownMenu->addSeparator();
  SaveDropDownMenu->addAction(SetDefaultSessionAction);
  SaveButton->setMenu(SaveDropDownMenu);
  EditCancelButton = new QPushButton();
  EditCancelButton->setText(EditCancelAction->text());
  connect(EditCancelButton, &QPushButton::clicked, EditCancelAction, &QAction::trigger);
  EditButton = new QPushButton(EditSessionAction->text());
  connect(EditButton, &QPushButton::clicked, EditSessionAction, &QAction::trigger);
  // Advanced split button: Advanced... / Edit Raw Settings... / Logging... (SessionAdvancedPopupMenu)
  AdvancedButton = new QToolButton();
  AdvancedButton->setDefaultAction(SessionAdvancedAction);
  AdvancedButton->setPopupMode(QToolButton::MenuButtonPopup);
  AdvancedButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
  AdvancedButton->setMinimumWidth(100);
  SessionAdvancedPopupMenu = new QMenu(this);
  SessionAdvancedPopupMenu->addSection(QStringLiteral("Session"));
  SessionAdvancedPopupMenu->addAction(SessionAdvancedAction);
  SessionAdvancedPopupMenu->addAction(SessionRawAction);
  SessionAdvancedPopupMenu->addAction(CopyParamRuleAction);
  SessionAdvancedPopupMenu->addSection(QStringLiteral("Global Preferences"));
  SessionAdvancedPopupMenu->addAction(PreferencesLoggingAction);
  AdvancedButton->setMenu(SessionAdvancedPopupMenu);
  AgentAccessButton = new QPushButton(AgentAccessAction->text());
  AgentAccessButton->setToolTip(QStringLiteral("Let AI agents and other programs use this site without getting its password"));
  connect(AgentAccessButton, &QPushButton::clicked, AgentAccessAction, &QAction::trigger);
  // NixSCP Teams: share the site with a team, or stop sharing it
  TeamButton = new QPushButton(ShareWithTeamAction->text());
  connect(TeamButton, &QPushButton::clicked, this, [this]()
  {
    QString Uuid, Relative;
    if (SelectedTeamItem(Uuid, Relative)) StopSharingActionExecute(); else ShareWithTeamActionExecute();
  });
  for (QPushButton * B : {EditCancelButton, EditButton, AgentAccessButton, TeamButton})
  {
    B->setAutoDefault(false);
  }
  auto * SessionButtons = new QHBoxLayout();
  SessionButtons->addWidget(SaveButton);
  SessionButtons->addWidget(EditCancelButton);
  SessionButtons->addWidget(EditButton);
  SessionButtons->addStretch();
  SessionButtons->addWidget(TeamButton);
  SessionButtons->addWidget(AgentAccessButton);
  SessionButtons->addWidget(AdvancedButton);
  Grid->addLayout(SessionButtons, 7, 0, 1, 4);

  NoteGroup = new QGroupBox(QStringLiteral("Note"));
  auto * NoteLayout = new QVBoxLayout(NoteGroup);
  NoteMemo = new QPlainTextEdit();
  NoteMemo->setReadOnly(true);
  NoteLayout->addWidget(NoteMemo);

  SitePanel = new QWidget();
  auto * SitePanelLayout = new QVBoxLayout(SitePanel);
  SitePanelLayout->setContentsMargins(0, 0, 0, 0);
  SitePanelLayout->addWidget(BasicGroup);
  SitePanelLayout->addWidget(NoteGroup, 1);
  SitePanelLayout->addStretch();

  // ---- ContentsPanel (folders and workspaces) ----
  ContentsPanel = new QWidget();
  auto * ContentsPanelLayout = new QVBoxLayout(ContentsPanel);
  ContentsPanelLayout->setContentsMargins(0, 0, 0, 0);
  ContentsGroupBox = new QGroupBox(QStringLiteral("Site Folder"));
  auto * ContentsLayout = new QGridLayout(ContentsGroupBox);
  ContentsNameEdit = new QLineEdit();
  ContentsNameEdit->setReadOnly(true);
  ContentsMemo = new QPlainTextEdit();
  ContentsMemo->setReadOnly(true);
  ContentsLayout->addWidget(Label(QStringLiteral("Name:"), ContentsNameEdit), 0, 0);
  ContentsLayout->addWidget(ContentsNameEdit, 0, 1);
  ContentsLayout->addWidget(ContentsMemo, 1, 0, 1, 2);
  ContentsPanelLayout->addWidget(ContentsGroupBox);

  // ---- ButtonPanel ----
  LoginButton = new QPushButton();
  LoginButton->setDefault(true);
  connect(LoginButton, &QPushButton::clicked, LoginAction, &QAction::trigger);
  CloseButton = new QPushButton(QStringLiteral("Close"));
  CloseButton->setAutoDefault(false);
  auto * HelpButton = new QPushButton(QStringLiteral("&Help"));
  HelpButton->setAutoDefault(false);
  HelpSystem::InstallFormHelp(this, QStringLiteral("ui_login"), HelpButton);
  auto * ButtonPanel = new QHBoxLayout();
  ButtonPanel->addStretch();
  ButtonPanel->addWidget(LoginButton);
  ButtonPanel->addWidget(CloseButton);
  ButtonPanel->addWidget(HelpButton);

  auto * RightPanel = new QVBoxLayout();
  RightPanel->addWidget(SitePanel, 1);
  RightPanel->addWidget(ContentsPanel, 1);
  RightPanel->addLayout(ButtonPanel);

  auto * MainPanel = new QHBoxLayout();
  MainPanel->addLayout(SitesPanel, 2);
  MainPanel->addLayout(RightPanel, 3);

  // ---- ShowAgainPanel ----
  ShowAgainPanel = new QWidget();
  auto * ShowAgainLayout = new QHBoxLayout(ShowAgainPanel);
  ShowAgainLayout->setContentsMargins(0, 0, 0, 0);
  ShowAgainCheck = new QCheckBox(QStringLiteral("&Show Login dialog on startup and when the last session is closed"));
  ShowAgainCheck->setChecked(true);
  ShowAgainLayout->addWidget(ShowAgainCheck);
  ShowAgainLayout->addStretch();
  ShowAgainPanel->setVisible(false);
  connect(ShowAgainCheck, &QCheckBox::clicked, this, [this](bool Checked)
  {
    if (!Checked)
    {
      // LOGIN_NOT_SHOWING_AGAIN
      if (Confirm(this, QStringLiteral(
            "<b>Stop showing Login dialog automatically?</b><br><br>Please confirm if you really want NixSCP to stop showing "
            "Login dialog automatically on startup and when the last session is closed.<br><br>If you change your mind later, "
            "you can revert this in Preferences on Environment &gt; Window page.<br><br>To open the Login dialog manually, go "
            "to Tabs &gt; New Tab [&gt; Remote Tab] or use corresponding toolbar button.")) != QMessageBox::Ok)
      {
        ShowAgainCheck->setChecked(true);
      }
    }
  });

  auto * MainLayout = new QVBoxLayout(this);
  MainLayout->addLayout(MainPanel, 1);
  MainLayout->addWidget(ShowAgainPanel);

  connect(SessionTree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem * Current, QTreeWidgetItem * Previous)
  {
    if (FNoUpdate > 0)
    {
      return;
    }
    if (FEditing && (Previous != nullptr) && (Current != Previous))
    {
      // SessionTreeChanging: the user has to decide first; select back and ask
      QString Target = SessionNodePath(Current) + (IsFolderNode(Current) ? QStringLiteral("/") : QString());
      bool TargetNewSite = IsNewSiteNode(Current);
      FNoUpdate++;
      SessionTree->setCurrentItem(Previous);
      FNoUpdate--;
      QTimer::singleShot(0, this, [this, Target, TargetNewSite]()
      {
        if (EnsureNotEditing())
        {
          if (TargetNewSite)
          {
            SetSelected(NewSiteNode());
          }
          else
          {
            ReloadSessions(Target);
          }
        }
      });
      return;
    }
    if (IsNewSiteNode(Previous) && FInitialized)
    {
      // PersistNewSiteIfNeeded (controls still show New Site)
      SaveSession(FNewSiteData);
    }
    if (FIncrementalSearching <= 0)
    {
      ResetSitesIncrementalSearch();
    }
    LoadContents();
  });
  connect(SessionTree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem * Node)
  {
    // SessionTreeDblClick
    if ((Node == Selected()) && EnsureNotEditing())
    {
      if (IsCloneToNewSiteDefault())
      {
        CloneToNewSite();
      }
      else if (CanOpen() && (IsSiteNode(Node) || IsNewSiteNode(Node) || IsWorkspaceNode(Node)))
      {
        LoginActionExecute();
      }
    }
  });
  connect(SessionTree, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem * Node) { UpdateFolderNode(Node); });
  connect(SessionTree, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem * Node) { UpdateFolderNode(Node); });
  connect(SessionTree->itemDelegate(), &QAbstractItemDelegate::closeEditor, this, [this]()
  {
    if (FRenaming)
    {
      FRenaming = false;
      QTreeWidgetItem * Node = SessionTree->currentItem();
      QTimer::singleShot(0, this, [this, Node]() { SessionTreeEdited(Node); });
    }
  });
  connect(TransferProtocolCombo, &QComboBox::currentIndexChanged, this, [this]() { TransferProtocolComboChange(); });
  auto EncryptionComboChange = [this]()
  {
    UpdatePortWithProtocol();
    UpdateControls();
  };
  connect(FtpsCombo, &QComboBox::currentIndexChanged, this, EncryptionComboChange);
  connect(WebDavsCombo, &QComboBox::currentIndexChanged, this, EncryptionComboChange);
  connect(PortNumberEdit, &QSpinBox::valueChanged, this, [this]() { PortNumberEditChange(); });
  connect(AnonymousLoginCheck, &QCheckBox::clicked, this, [this](bool Checked)
  {
    // AnonymousLoginCheckClick
    if (Checked)
    {
      UserNameEdit->setText(AnonymousUserName);
      PasswordEdit->setText(AnonymousPassword);
    }
    UpdateControls();
  });
  connect(HostNameEdit, &QLineEdit::textChanged, this, [this]() { if (FNoUpdate == 0) UpdateControls(); });
  connect(S3CredentialsEnvCheck, &QCheckBox::clicked, this, [this]()
  {
    // S3CredentialsEnvCheck3Click
    UpdateS3Credentials();
    UpdateControls();
  });
  connect(S3ProfileCombo, &QComboBox::currentTextChanged, this, [this]()
  {
    // S3ProfileComboChange
    if (FNoUpdate == 0)
    {
      UpdateS3Credentials();
      UpdateControls();
    }
  });
  connect(UserNameEdit, &QLineEdit::textChanged, this, [this]() { if (FNoUpdate == 0) UpdateControls(); });
  connect(PasswordEdit, &QLineEdit::textChanged, this, [this]() { if (FNoUpdate == 0) UpdateControls(); });
  // HostNameEditExit
  connect(HostNameEdit, &QLineEdit::editingFinished, this, [this]() { if (IsEditable()) ParseHostName(); });
  connect(CloseButton, &QPushButton::clicked, this, &QDialog::reject);
}

//---------------------------------------------------------------------------
// Tree

LoginDialog::ItemType LoginDialog::NodeType(const QTreeWidgetItem * Node)
{
  return static_cast<ItemType>(Node->data(0, ItemTypeRole).toInt());
}

QTreeWidgetItem * LoginDialog::NewSiteNode() const
{
  return (SessionTree->topLevelItemCount() > 0) ? SessionTree->topLevelItem(0) : nullptr;
}

QTreeWidgetItem * LoginDialog::Selected() const
{
  return SessionTree->currentItem();
}

void LoginDialog::SetSelected(QTreeWidgetItem * Node)
{
  if (Node != nullptr)
  {
    for (QTreeWidgetItem * P = Node->parent(); P != nullptr; P = P->parent())
    {
      P->setExpanded(true);
    }
    SessionTree->setCurrentItem(Node);
    SessionTree->scrollToItem(Node);
  }
}

QString LoginDialog::SessionNodePath(const QTreeWidgetItem * Node) const
{
  if ((Node == nullptr) || IsNewSiteNode(Node))
  {
    return QString();
  }
  return Node->data(0, ItemNameRole).toString();
}

std::shared_ptr<SessionData> LoginDialog::NodeSession(const QTreeWidgetItem * Node) const
{
  if (IsSiteNode(Node))
  {
    return FSessions.FindByName(Node->data(0, ItemNameRole).toString());
  }
  return nullptr;
}

std::shared_ptr<SessionData> LoginDialog::SelectedSession() const
{
  return NodeSession(Selected());
}

QIcon LoginDialog::SiteIcon(const SessionData & Data)
{
  // GetSessionImageIndex / GetSessionColorImage: site icon marked with the site color
  static QHash<int, QIcon> Cache;
  QIcon Site = Icons::Get(QStringLiteral("site"));
  if (Data.Color == 0)
  {
    return Site;
  }
  auto It = Cache.constFind(Data.Color);
  if (It != Cache.constEnd())
  {
    return It.value();
  }
  QColor Color = QColor::fromRgb(Data.ColorRgb());
  QIcon Result;
  for (int Size : {16, 24, 32})
  {
    QPixmap Pixmap = Site.pixmap(Size, Size);
    if (Pixmap.isNull())
    {
      Pixmap = QPixmap(Size, Size);
      Pixmap.fill(Qt::transparent);
    }
    QPainter Painter(&Pixmap);
    int Mark = Size / 2;
    QRect Rect(Size - Mark, Size - Mark, Mark - 1, Mark - 1);
    Painter.fillRect(Rect, Color);
    Painter.setPen(Color.darker(160));
    Painter.drawRect(Rect);
    Painter.end();
    Result.addPixmap(Pixmap);
  }
  Cache.insert(Data.Color, Result);
  return Result;
}

QIcon LoginDialog::SessionIcon(const SessionData & Data)
{
  return SiteIcon(Data);
}

void LoginDialog::UpdateNodeImage(QTreeWidgetItem * Node)
{
  if (std::shared_ptr<SessionData> Data = NodeSession(Node))
  {
    Node->setIcon(0, SessionIcon(*Data));
  }
}

void LoginDialog::UpdateFolderNode(QTreeWidgetItem * Node)
{
  if (IsFolderNode(Node))
  {
    Node->setIcon(0, Node->isExpanded() ? style()->standardIcon(QStyle::SP_DirOpenIcon) : Icons::Get(QStringLiteral("site-folder")));
  }
}

QTreeWidgetItem * LoginDialog::AddSessionPath(const QString & Path, bool CanCreate)
{
  QTreeWidgetItem * Parent = nullptr;
  QString Accumulated;
  for (const QString & Folder : Path.split(QLatin1Char('/'), Qt::SkipEmptyParts))
  {
    Accumulated = Accumulated.isEmpty() ? Folder : (Accumulated + QLatin1Char('/') + Folder);
    QTreeWidgetItem * Found = nullptr;
    int Count = (Parent != nullptr) ? Parent->childCount() : SessionTree->topLevelItemCount();
    for (int Index = 0; Index < Count; Index++)
    {
      QTreeWidgetItem * Child = (Parent != nullptr) ? Parent->child(Index) : SessionTree->topLevelItem(Index);
      // a folder may have the same name as a site on the same level
      if (IsFolderNode(Child) && (Child->text(0).compare(Folder, Qt::CaseInsensitive) == 0))
      {
        Found = Child;
        break;
      }
    }
    if (Found == nullptr)
    {
      if (!CanCreate)
      {
        return nullptr;
      }
      Found = (Parent != nullptr) ? new QTreeWidgetItem(Parent) : new QTreeWidgetItem(SessionTree);
      Found->setText(0, Folder);
      Found->setData(0, ItemTypeRole, itFolder);
      Found->setData(0, ItemNameRole, Accumulated);
      UpdateFolderNode(Found);
    }
    Parent = Found;
  }
  return Parent;
}

QTreeWidgetItem * LoginDialog::AddSession(const std::shared_ptr<SessionData> & Data)
{
  QTreeWidgetItem * Parent = AddSessionPath(Data->FolderName(), true);
  auto * Node = (Parent != nullptr) ? new QTreeWidgetItem(Parent) : new QTreeWidgetItem(SessionTree);
  Node->setText(0, Data->LocalName());
  Node->setData(0, ItemTypeRole, itSite);
  Node->setData(0, ItemNameRole, Data->Name);
  Node->setIcon(0, SessionIcon(*Data));
  Node->setToolTip(0, Data->InfoTip());
  if (Data->Special)
  {
    // SessionTreeCustomDrawItem
    QFont Font = Node->font(0);
    Font.setBold(true);
    Font.setUnderline(true);
    Node->setFont(0, Font);
  }
  return Node;
}

static void SortChildren(QTreeWidgetItem * Parent)
{
  // SessionTreeCompare: New Site, workspaces, folders, sites; logical order
  QList<QTreeWidgetItem *> Children = Parent->takeChildren();
  std::stable_sort(Children.begin(), Children.end(), [](QTreeWidgetItem * A, QTreeWidgetItem * B)
  {
    int TypeA = A->data(0, ItemTypeRole).toInt();
    int TypeB = B->data(0, ItemTypeRole).toInt();
    // itNewSite = 1, itSite = 2, itFolder = 3, itWorkspace = 4
    auto Rank = [](int Type) { return (Type == 1) ? 0 : ((Type == 4) ? 1 : ((Type == 3) ? 2 : 3)); };
    if (Rank(TypeA) != Rank(TypeB))
    {
      return Rank(TypeA) < Rank(TypeB);
    }
    return StoredSessionList::CompareLogicalText(A->text(0), B->text(0)) < 0;
  });
  Parent->addChildren(Children);
  for (QTreeWidgetItem * Child : Children)
  {
    if (Child->childCount() > 0)
    {
      SortChildren(Child);
    }
  }
}

void LoginDialog::SortTree()
{
  SortChildren(SessionTree->invisibleRootItem());
}

bool LoginDialog::HasNodeAnySession(const QTreeWidgetItem * Node, bool NeedCanOpen) const
{
  if (!IsFolderOrWorkspaceNode(Node))
  {
    return false;
  }
  bool Workspace = IsWorkspaceNode(Node);
  QString Prefix = SessionNodePath(Node) + QLatin1Char('/');
  for (const auto & Data : FSessions.Sessions())
  {
    if ((Data->IsWorkspace == Workspace) && Data->Name.startsWith(Prefix, Qt::CaseInsensitive) &&
        (!NeedCanOpen || FSessions.CanOpen(*Data)))
    {
      return true;
    }
  }
  return false;
}

void LoginDialog::UpdateFolderNodesStyle()
{
  // Empty folders drawn grayed (SessionTreeCustomDrawItem)
  for (QTreeWidgetItem * Node : AllNodes())
  {
    if (IsFolderOrWorkspaceNode(Node))
    {
      Node->setForeground(0, HasNodeAnySession(Node) ? QBrush() : palette().brush(QPalette::Disabled, QPalette::Text));
    }
  }
}

QList<QTreeWidgetItem *> LoginDialog::AllNodes() const
{
  // pre-order (TTreeNodes order)
  QList<QTreeWidgetItem *> Result;
  std::function<void(QTreeWidgetItem *)> Add = [&](QTreeWidgetItem * Parent)
  {
    for (int Index = 0; Index < Parent->childCount(); Index++)
    {
      Result.append(Parent->child(Index));
      Add(Parent->child(Index));
    }
  };
  Add(SessionTree->invisibleRootItem());
  return Result;
}

void LoginDialog::LoadSessions()
{
  FNoUpdate++;
  SessionTree->clear();
  auto * NewSite = new QTreeWidgetItem(SessionTree);
  NewSite->setText(0, Translator::Tr(QStringLiteral("New Site"), "Login"));
  NewSite->setIcon(0, Icons::Get(QStringLiteral("new-session")));
  NewSite->setData(0, ItemTypeRole, itNewSite);

  for (const QString & Workspace : Workspaces::WorkspaceNames(FSessions))
  {
    auto * Node = new QTreeWidgetItem(SessionTree);
    Node->setText(0, Workspace);
    Node->setIcon(0, Icons::Get(QStringLiteral("workspace")));
    Node->setData(0, ItemTypeRole, itWorkspace);
    Node->setData(0, ItemNameRole, Workspace);
    // WORKSPACE_INFO_TIP
    Node->setToolTip(0, (QStringLiteral("Workspace: %1\n").arg(Workspace) +
      FolderOrWorkspaceContents(Node, QStringLiteral("  "), QString())).trimmed());
  }
  // empty folders (known to the Login dialog only)
  for (const QString & Folder : FSessions.Folders())
  {
    AddSessionPath(Folder, true);
  }
  for (const auto & Data : FSessions.Sessions())
  {
    if (!Data->IsWorkspace)
    {
      AddSession(Data);
    }
  }
  SortTree();
  UpdateFolderNodesStyle();
  SessionTree->setCurrentItem(NewSite);
  FNoUpdate--;
}

QTreeWidgetItem * LoginDialog::FindSessionNode(const QString & Name) const
{
  for (QTreeWidgetItem * Node : AllNodes())
  {
    if (IsSiteNode(Node) && (Node->data(0, ItemNameRole).toString().compare(Name, Qt::CaseInsensitive) == 0))
    {
      return Node;
    }
  }
  return nullptr;
}

QTreeWidgetItem * LoginDialog::SessionFolderNode(QTreeWidgetItem * Node) const
{
  if (IsSiteNode(Node) || IsNewSiteNode(Node))
  {
    return Node->parent();
  }
  if (IsFolderNode(Node))
  {
    return Node;
  }
  return nullptr;
}

QString LoginDialog::FolderOrWorkspaceContents(const QTreeWidgetItem * Node, const QString & Indent, const QString & CommonRoot) const
{
  QString Contents;
  for (QString Name : Workspaces::FolderOrWorkspaceList(FSessions, SessionNodePath(Node)))
  {
    if (!CommonRoot.isEmpty() && Name.startsWith(CommonRoot))
    {
      Name.remove(0, CommonRoot.size());
    }
    Contents += Indent + Name + QLatin1Char('\n');
  }
  return Contents;
}

static QTreeWidgetItem * FindNodeByPath(QTreeWidget * Tree, const QString & APath,
  const std::function<QTreeWidgetItem *(const QString &)> & FolderNode)
{
  // LoadState: LastStoredSession lookup (folders end with a slash)
  QString Path = APath;
  bool Folder = Path.endsWith(QLatin1Char('/'));
  while (Path.endsWith(QLatin1Char('/')))
  {
    Path.chop(1);
  }
  if (Path.isEmpty())
  {
    return nullptr;
  }
  qsizetype P = Path.lastIndexOf(QLatin1Char('/'));
  QString ParentPath = (P >= 0) ? Path.left(P) : QString();
  QString Name = (P >= 0) ? Path.mid(P + 1) : Path;
  QTreeWidgetItem * Parent = ParentPath.isEmpty() ? Tree->invisibleRootItem() : FolderNode(ParentPath);
  if (Parent == nullptr)
  {
    return nullptr;
  }
  for (int Index = 0; Index < Parent->childCount(); Index++)
  {
    QTreeWidgetItem * Node = Parent->child(Index);
    bool IsFolder = (Node->data(0, ItemTypeRole).toInt() == 3);
    if ((Node->data(0, ItemTypeRole).toInt() != 1) && (Node->text(0).compare(Name, Qt::CaseInsensitive) == 0) && (IsFolder == Folder))
    {
      return Node;
    }
  }
  return nullptr;
}

void LoginDialog::ReloadSessions(const QString & SelectSite)
{
  // ReloadSessions: keep expanded folders, select SelectSite (LastStoredSession format)
  QStringList Expanded;
  for (QTreeWidgetItem * Node : AllNodes())
  {
    if (IsFolderNode(Node) && Node->isExpanded())
    {
      Expanded.append(SessionNodePath(Node));
    }
  }
  LoadSessions();
  FNoUpdate++;
  for (QTreeWidgetItem * Node : AllNodes())
  {
    if (IsFolderNode(Node) && Expanded.contains(SessionNodePath(Node), Qt::CaseInsensitive))
    {
      Node->setExpanded(true);
      UpdateFolderNode(Node);
    }
  }
  FNoUpdate--;
  QTreeWidgetItem * Node = FindNodeByPath(SessionTree, SelectSite, [this](const QString & Path) { return AddSessionPath(Path, false); });
  FNoUpdate++;
  SetSelected((Node != nullptr) ? Node : NewSiteNode());
  FNoUpdate--;
  LoadContents();
}

//---------------------------------------------------------------------------
// State

void LoginDialog::SaveState()
{
  Configuration & Config = Configuration::Instance();
  IniStorage Storage(&Config.Ini());
  if (Storage.OpenSubKey(ConfigurationKey, true) && Storage.OpenSubKey(InterfaceKey, true))
  {
    QStringList OpenedStoredSessionFolders;
    for (QTreeWidgetItem * Node : AllNodes())
    {
      if (IsFolderNode(Node) && Node->isExpanded())
      {
        OpenedStoredSessionFolders.append(SessionNodePath(Node));
      }
    }
    OpenedStoredSessionFolders.sort(Qt::CaseInsensitive);
    Storage.WriteString(QStringLiteral("OpenedStoredSessionFolders"), ToCommaText(OpenedStoredSessionFolders));
    QString LastStoredSession = SessionNodePath(Selected());
    if (IsFolderNode(Selected()))
    {
      LastStoredSession += QLatin1Char('/');
    }
    Storage.WriteString(QStringLiteral("LastStoredSession"), LastStoredSession);
    if (Storage.OpenSubKey(QStringLiteral("LoginDialog"), true))
    {
      // StoreFormSize: "Width,Height,PixelsPerInch" (logical pixels = 96 DPI)
      Storage.WriteString(QStringLiteral("WindowSize"), QStringLiteral("%1,%2,96").arg(width()).arg(height()));
      Storage.WriteInteger(QStringLiteral("SiteSearch"), FSiteSearch);
      Storage.CloseSubKey();
    }
  }
}

void LoginDialog::LoadState()
{
  Configuration & Config = Configuration::Instance();
  IniStorage Storage(&Config.Ini());
  QString OpenedStoredSessionFolders;
  QString LastStoredSession;
  if (Storage.OpenSubKey(ConfigurationKey, false) && Storage.OpenSubKey(InterfaceKey, false))
  {
    OpenedStoredSessionFolders = Storage.ReadString(QStringLiteral("OpenedStoredSessionFolders"), QString());
    LastStoredSession = Storage.ReadString(QStringLiteral("LastStoredSession"), QString());
    if (Storage.OpenSubKey(QStringLiteral("LoginDialog"), false))
    {
      // RestoreFormSize
      QStringList Size = Storage.ReadString(QStringLiteral("WindowSize"), QString()).split(QLatin1Char(','));
      if (Size.size() >= 2)
      {
        int W = Size[0].toInt();
        int H = Size[1].toInt();
        int Ppi = (Size.size() >= 3) ? Size[2].toInt() : 96;
        if ((Ppi > 0) && (Ppi != 96))
        {
          W = W * 96 / Ppi;
          H = H * 96 / Ppi;
        }
        if ((W > 0) && (H > 0))
        {
          QSize Available = (screen() != nullptr) ? screen()->availableSize() : QSize(W, H);
          resize(qMin(qMax(W, minimumWidth()), Available.width()), qMin(qMax(H, minimumHeight()), Available.height()));
        }
      }
      int SiteSearch = Storage.ReadInteger(QStringLiteral("SiteSearch"), FSiteSearch);
      if ((SiteSearch >= isNameStartOnly) && (SiteSearch <= isAll))
      {
        FSiteSearch = SiteSearch;
      }
      Storage.CloseSubKey();
    }
  }
  FNoUpdate++;
  QStringList Folders = FromCommaText(OpenedStoredSessionFolders);
  for (QTreeWidgetItem * Node : AllNodes())
  {
    if (IsFolderNode(Node) && Folders.contains(SessionNodePath(Node), Qt::CaseInsensitive))
    {
      Node->setExpanded(true);
      UpdateFolderNode(Node);
    }
  }
  FNoUpdate--;
  if (!FForceNewSite && IsNewSiteNode(Selected()) && !LastStoredSession.isEmpty())
  {
    QTreeWidgetItem * Node = FindNodeByPath(SessionTree, LastStoredSession, [this](const QString & Path) { return AddSessionPath(Path, false); });
    if (Node != nullptr)
    {
      FNoUpdate++;
      SetSelected(Node);
      FNoUpdate--;
    }
  }
}

void LoginDialog::SetInitialSession(const SessionData & Data)
{
  // TLoginDialog::Execute(DataList)
  QTreeWidgetItem * Node = Data.Name.isEmpty() ? nullptr : FindSessionNode(Data.Name);
  if (Node != nullptr)
  {
    FNoUpdate++;
    SetSelected(Node);
    FNoUpdate--;
    FForceNewSite = true; // do not jump to LastStoredSession
  }
  else
  {
    FNewSiteData = Data;
    FNewSiteData.Special = false;
    SessionData Defaults = FSessions.DefaultSettings();
    if (!FNewSiteData.SaveToOptions(&Defaults).isEmpty())
    {
      // we want to start with new site page
      FForceNewSite = true;
    }
    FNoUpdate++;
    SetSelected(NewSiteNode());
    FNoUpdate--;
  }
  if (FInitialized)
  {
    LoadContents();
  }
}

void LoginDialog::SetShowAgainPanel(bool Show)
{
  ShowAgainPanel->setVisible(Show);
}

std::shared_ptr<SessionData> LoginDialog::SessionToOpen() const
{
  if (FSessionsToOpen.isEmpty())
  {
    return nullptr;
  }
  return std::make_shared<SessionData>(FSessionsToOpen.first());
}

void LoginDialog::showEvent(QShowEvent * Event)
{
  QDialog::showEvent(Event);
  if (!FInitialized)
  {
    LoadState();
    FInitialized = true;
    LoadContents();
    // Init: Host name when there are no sites (or forced New Site), else the tree
    if ((SessionTree->topLevelItemCount() == 1) || (FForceNewSite && IsNewSiteNode(Selected())))
    {
      HostNameEdit->setFocus();
    }
    else
    {
      SessionTree->setFocus();
    }
    UpdateControls();
    if (!qEnvironmentVariableIsEmpty("NIXSCP_SCREENSHOT_LOGIN"))
    {
      QString Select = qEnvironmentVariable("NIXSCP_SCREENSHOT_LOGIN_SELECT");
      if (!Select.isEmpty())
      {
        ReloadSessions(Select);
      }
      ScheduleScreenshot(this);
      QString ScreenshotAction = qEnvironmentVariable("NIXSCP_SCREENSHOT_LOGIN_ACTION");
      QTimer::singleShot(100, this, [this, ScreenshotAction]()
      {
        if (ScreenshotAction == QStringLiteral("import")) ImportSessionsActionExecute();
        else if (ScreenshotAction == QStringLiteral("saveas")) SaveAsSession(true);
        else if (ScreenshotAction == QStringLiteral("raw")) { EditSessionActionExecute(); SessionAdvancedActionExecute(true); }
        else if (ScreenshotAction == QStringLiteral("advanced")) { EditSessionActionExecute(); SessionAdvancedActionExecute(false); }
        else if (ScreenshotAction == QStringLiteral("cleanup")) CleanUpActionExecute();
        else if (ScreenshotAction == QStringLiteral("edit")) EditSessionActionExecute();
        else if (ScreenshotAction == QStringLiteral("search"))
        {
          SessionTree->setFocus();
          SitesIncrementalSearch(qEnvironmentVariable("NIXSCP_SCREENSHOT_LOGIN_SEARCH"), false, false, false);
        }
      });
    }
    else if (!ScreenshotMode())
    {
      QTimer::singleShot(0, this, [this]()
      {
        qsizetype Count = FSessions.Sessions().size();
        ImportSitesIfAny(FSessions, this);
        if ((FSessions.Sessions().size() != Count) && !FEditing)
        {
          ReloadSessions(SessionNodePath(Selected()) + (IsFolderNode(Selected()) ? QStringLiteral("/") : QString()));
        }
      });
    }
  }
}

//---------------------------------------------------------------------------
// Session editing

bool LoginDialog::IsEditable() const
{
  return IsNewSiteNode(Selected()) || FEditing;
}

void LoginDialog::LoadContents()
{
  QTreeWidgetItem * Node = Selected();
  bool UseContentsPanel = false;
  if (IsSiteNode(Node))
  {
    if (std::shared_ptr<SessionData> Data = NodeSession(Node))
    {
      LoadSession(*Data);
    }
  }
  else if (IsNewSiteNode(Node) || (Node == nullptr))
  {
    LoadSession(FNewSiteData);
  }
  else
  {
    QString NodePath = SessionNodePath(Node);
    ContentsNameEdit->setText(NodePath);
    QString CommonRoot = IsFolderNode(Node) ? (NodePath + QLatin1Char('/')) : QString();
    ContentsMemo->setPlainText(FolderOrWorkspaceContents(Node, QString(), CommonRoot));
    UseContentsPanel = true;
    // LOGIN_SITE_FOLDER_CAPTION, LOGIN_WORKSPACE_CAPTION
    ContentsGroupBox->setTitle(IsFolderNode(Node) ? QStringLiteral("Site Folder") : QStringLiteral("Workspace"));
  }
  SitePanel->setVisible(!UseContentsPanel);
  ContentsPanel->setVisible(UseContentsPanel);
  UpdateControls();
}

static int FtpsToIndex(TFtps Ftps)
{
  switch (Ftps)
  {
    case ftpsImplicit: return 1;
    case ftpsExplicitTls:
    case ftpsExplicitSsl: return 2;
    default: return 0;
  }
}

static int FSProtocolToIndex(TFSProtocol FSProtocol, bool & AllowScpFallback)
{
  if (FSProtocol == fsSFTP)
  {
    AllowScpFallback = true;
    return 0;
  }
  AllowScpFallback = false;
  for (int Index = 0; Index < FSOrderCount; Index++)
  {
    if (FSOrder[Index] == FSProtocol)
    {
      return Index;
    }
  }
  // SFTP is always present
  AllowScpFallback = true;
  return 0;
}

static TFSProtocol IndexToFSProtocol(int Index, bool AllowScpFallback)
{
  TFSProtocol Result = fsSFTP;
  if ((Index >= 0) && (Index < FSOrderCount))
  {
    Result = FSOrder[Index];
    if ((Result == fsSFTPonly) && AllowScpFallback)
    {
      Result = fsSFTP;
    }
  }
  return Result;
}

void LoginDialog::SetFtpsIndex(int Index)
{
  FtpsCombo->setCurrentIndex(Index);
  WebDavsCombo->setCurrentIndex(qMin(Index, 1));
}

void LoginDialog::LoadSession(const SessionData & Data)
{
  FNoUpdate++;
  PortNumberEdit->setValue(Data.PortNumber);
  int FtpsIndex = FtpsToIndex(Data.Ftps);
  SetFtpsIndex(FtpsIndex);
  EncryptionView->setText(FtpsCombo->itemText(FtpsIndex));
  bool AllowScpFallback;
  TransferProtocolCombo->setCurrentIndex(FSProtocolToIndex(Data.FSProtocol, AllowScpFallback));
  TransferProtocolView->setText(TransferProtocolCombo->currentText());
  HostNameEdit->setText(Data.HostName);
  UserNameEdit->setText(Data.UserName);
  if (IsEditable())
  {
    PasswordEdit->setText(Data.Password);
  }
  else
  {
    // saved password is not loaded into the box
    PasswordEdit->setText(Data.HasPassword() ? QString(16, QLatin1Char('?')) : QString());
  }
  S3CredentialsEnvCheck->setChecked(Data.S3CredentialsEnv);
  if ((Data.FSProtocol == fsS3) && (S3ProfileCombo->count() == 0))
  {
    LoadS3Profiles();
  }
  // LOGIN_S3_GENERAL_CREDENTIALS
  S3ProfileCombo->setEditText(Data.S3Profile.isEmpty() ? QStringLiteral("General") : Data.S3Profile);
  NoteGroup->setVisible(!Data.Note.trimmed().isEmpty());
  NoteMemo->setPlainText(Data.Note);
  if (&Data != &FEditingData)
  {
    FEditingData = Data;
  }
  FNoUpdate--;
  // just in case TransferProtocolComboChange is not triggered
  FDefaultPort = DefaultPort();
  FUpdatePortWithProtocol = true;
  UpdateControls();
}

TFSProtocol LoginDialog::GetFSProtocol(bool RequireScpFallbackDistinction) const
{
  bool AllowScpFallback = false;
  if (RequireScpFallbackDistinction)
  {
    FSProtocolToIndex(FEditingData.FSProtocol, AllowScpFallback);
  }
  return IndexToFSProtocol(TransferProtocolCombo->currentIndex(), AllowScpFallback);
}

TFtps LoginDialog::GetFtps() const
{
  TFSProtocol FSProtocol = GetFSProtocol(false);
  int Index = ((FSProtocol == fsWebDAV) || (FSProtocol == fsS3)) ? WebDavsCombo->currentIndex() : FtpsCombo->currentIndex();
  switch (Index)
  {
    case 1: return ftpsImplicit;
    case 2: return ftpsExplicitTls;
    default: return ftpsNone;
  }
}

int LoginDialog::DefaultPort() const
{
  return SessionData::DefaultPort(GetFSProtocol(false), GetFtps());
}

void LoginDialog::SaveSession(SessionData & Data)
{
  std::shared_ptr<SessionData> Site = SelectedSession();
  if (!IsEditable() && (Site != nullptr))
  {
    // a saved site that is not being edited (the password box holds a placeholder)
    Data = *Site;
    return;
  }
  // advanced
  if (&Data != &FEditingData)
  {
    Data = FEditingData;
  }
  Data.FSProtocol = GetFSProtocol(true);
  Data.S3CredentialsEnv = S3CredentialsEnvCheck->isChecked();
  Data.S3Profile = GetS3Profile();
  Data.UserName = UserNameEdit->text().trimmed();
  Data.Password = PasswordEdit->text();
  Data.PortNumber = PortNumberEdit->value();
  // Must be set after UserName, because HostName may be in format user@host
  QString Host = HostNameEdit->text().trimmed();
  qsizetype At = Host.lastIndexOf(QLatin1Char('@'));
  if ((At >= 0) && !SessionData::IsIPv6Literal(Host))
  {
    Data.UserName = Host.left(At);
    Host = Host.mid(At + 1);
  }
  Data.HostName = Host;
  Data.Ftps = GetFtps();
  // (the edited site may have been removed meanwhile: it is saved under its name again)
  Data.Name = FEditing ? FEditingName : (FNewSiteKeepName ? Data.Name : Data.DefaultSessionName());
}

void LoginDialog::UpdateActions()
{
  // ActionListUpdate
  QTreeWidgetItem * Node = Selected();
  bool SiteSelected = IsSiteNode(Node);
  bool FolderOrWorkspaceSelected = IsFolderOrWorkspaceNode(Node);
  bool WorkspaceSelected = IsWorkspaceNode(Node);
  std::shared_ptr<SessionData> Session = SelectedSession();
  bool Special = (Session != nullptr) && Session->Special;
  bool Editable = IsEditable();

  EditSessionAction->setEnabled(SiteSelected && !FEditing);
  CloneToNewSiteAction->setEnabled(SiteSelected && !FEditing);
  EditCancelAction->setEnabled(FEditing);
  DeleteSessionAction->setEnabled((SiteSelected && !Special && !FEditing) || FolderOrWorkspaceSelected);
  RenameSessionAction->setEnabled((SiteSelected && !Special && !FEditing) || FolderOrWorkspaceSelected);
  DesktopIconAction->setEnabled((SiteSelected && !FEditing) || (FolderOrWorkspaceSelected && HasNodeAnySession(Node)));
  LoginAction->setEnabled(CanOpen());
  // LOGIN_OPEN / LOGIN_LOGIN
  LoginAction->setText(FolderOrWorkspaceSelected ? QStringLiteral("Open") : QStringLiteral("Login"));
  LoginAction->setIcon(FolderOrWorkspaceSelected ?
    (WorkspaceSelected ? Icons::Get(QStringLiteral("workspace")) : style()->standardIcon(QStyle::SP_DirOpenIcon)) :
    style()->standardIcon(QStyle::SP_DialogOkButton));
  SaveSessionAction->setEnabled(Editable);
  SessionAdvancedAction->setEnabled(Editable);
  AgentAccessAction->setEnabled(Editable);
  AgentAccessButton->setEnabled(Editable);
  {
    // NixSCP Teams
    bool Teams = TeamSync::Instance().Available();
    QString TeamUuid, Relative;
    bool InTeam = SelectedTeamItem(TeamUuid, Relative);
    const TeamSync::Team * T = InTeam ? TeamSync::Instance().Find(TeamUuid) : nullptr;
    bool Shareable = Teams && !InTeam && !Special && !FEditing &&
      ((SiteSelected && (Session != nullptr)) || (IsFolderNode(Node) && HasNodeAnySession(Node)));
    bool Stoppable = Teams && InTeam && (T != nullptr) && T->CanEdit() && !FEditing;
    ShareWithTeamAction->setVisible(Teams && !InTeam);
    ShareWithTeamAction->setEnabled(Shareable);
    StopSharingAction->setVisible(Teams && InTeam);
    StopSharingAction->setEnabled(Stoppable);
    TeamButton->setVisible(Teams && SiteSelected && (Session != nullptr) && !Special);
    TeamButton->setText(InTeam ? ((T != nullptr) && T->CanEdit() ? QStringLiteral("Stop S&haring...") : QStringLiteral("Shared by Team"))
                               : ShareWithTeamAction->text());
    TeamButton->setEnabled(InTeam ? Stoppable : Shareable);
    TeamButton->setToolTip(InTeam ? QStringLiteral("Shared in the team \"%1\"").arg((T != nullptr) ? T->Name : QString())
                                  : QStringLiteral("Share this site, with its stored password, with your colleagues in a NixSCP team"));
  }
  SessionRawAction->setEnabled(Editable);
  SaveAsSessionAction->setEnabled(Editable);
  NewSessionFolderAction->setEnabled(!FEditing);
  PasteUrlAction->setEnabled(FClipboardHasUrl);
  SearchSiteStartAction->setEnabled(!FRenaming && !FEditing);
  bool HasHost = (Session != nullptr) ? !Session->HostNameExpanded().isEmpty() :
    (IsNewSiteNode(Node) && !SessionData::ExpandEnvironmentVariables(HostNameEdit->text().trimmed()).isEmpty());
  PuttyAction->setEnabled(
    (IsSiteAndCanOpen() && !((Session != nullptr) ? Session->Tunnel : FEditingData.Tunnel)) ||
    (IsFolderOrWorkspaceAndCanOpen() && IsFolderNode(Node)));
  GenerateUrlAction2->setEnabled(HasHost && !FEditing);
  CopyParamRuleAction->setEnabled(HasHost);
  SearchSiteNameStartOnlyAction->setChecked(FSiteSearch == isNameStartOnly);
  SearchSiteNameAction->setChecked(FSiteSearch == isName);
  SearchSiteAction->setChecked(FSiteSearch == isAll);
}

void LoginDialog::UpdateControls()
{
  if (FNoUpdate > 0)
  {
    return;
  }
  FNoUpdate++;
  bool Editable = IsEditable();
  TFSProtocol FSProtocol = GetFSProtocol(false);
  bool SshProtocol = (FSProtocol == fsSCPonly) || (FSProtocol == fsSFTP) || (FSProtocol == fsSFTPonly);
  bool FtpProtocol = (FSProtocol == fsFTP);
  bool WebDavProtocol = (FSProtocol == fsWebDAV);
  bool S3Protocol = (FSProtocol == fsS3);

  // session
  FtpsCombo->setVisible(Editable && FtpProtocol);
  WebDavsCombo->setVisible(Editable && (WebDavProtocol || S3Protocol));
  EncryptionLabel->setVisible(FtpProtocol || WebDavProtocol || S3Protocol);
  EncryptionLabel->setBuddy(FtpProtocol ? static_cast<QWidget *>(FtpsCombo) : static_cast<QWidget *>(WebDavsCombo));
  EncryptionView->setVisible(!Editable && (FtpProtocol || WebDavProtocol || S3Protocol));
  AnonymousLoginCheck->setVisible(FtpProtocol && Editable);
  AnonymousLoginCheck->setChecked(
    (UserNameEdit->text().compare(AnonymousUserName, Qt::CaseInsensitive) == 0) &&
    (PasswordEdit->text().compare(AnonymousPassword, Qt::CaseInsensitive) == 0));
  TransferProtocolCombo->setVisible(Editable);
  TransferProtocolView->setVisible(!Editable);
  HostNameEdit->setReadOnly(!Editable);
  PortNumberEdit->setReadOnly(!Editable);
  PortNumberEdit->setButtonSymbols(Editable ? QAbstractSpinBox::UpDownArrows : QAbstractSpinBox::NoButtons);
  bool S3CredentialsEnv = S3Protocol && S3CredentialsEnvCheck->isChecked();
  S3CredentialsEnvCheck->setVisible(S3Protocol);
  S3ProfileCombo->setVisible(S3Protocol);
  S3CredentialsEnvCheck->setEnabled(Editable);
  S3ProfileCombo->setEnabled(Editable && S3CredentialsEnv);
  if (S3Protocol && (S3ProfileCombo->count() == 0))
  {
    LoadS3Profiles();
  }
  bool NoAuth = Editable && ((SshProtocol && FEditingData.SshNoUserAuth) || S3CredentialsEnv);
  UserNameEdit->setReadOnly(!Editable);
  UserNameEdit->setEnabled(!NoAuth);
  UserNameLabel->setEnabled(!NoAuth);
  PasswordEdit->setReadOnly(!Editable);
  PasswordEdit->setEnabled(!NoAuth);
  PasswordLabel->setEnabled(!NoAuth);
  UserNameLabel->setText(S3Protocol ? QStringLiteral("A&ccess key ID:") : QStringLiteral("&Username:"));
  PasswordLabel->setText(S3Protocol ? QStringLiteral("Secret access &key:") : QStringLiteral("&Password:"));

  // sites
  SitesIncrementalSearchLabel->setVisible(FSearching);
  if (FSearching)
  {
    // INC_SEARCH, INC_SEARCH_TYPE, INC_NEXT_SEARCH
    SitesIncrementalSearchLabel->setText(
      QStringLiteral("Search: %1").arg(FSearchText.isEmpty() ? QStringLiteral("(start typing)") : FSearchText) +
      ((FSearchHaveNext && !FSearchText.isEmpty()) ? QStringLiteral(" (press Tab for next)") : QString()));
  }

  ManageButton->setEnabled(!FEditing);
  ToolsMenuButton->setEnabled(!FEditing);
  CloseButton->setEnabled(!FEditing);

  UpdateActions();
  LoginButton->setText(LoginAction->text());
  LoginButton->setIcon(LoginAction->icon());
  LoginButton->setEnabled(LoginAction->isEnabled());
  LoginButton->setDefault(!FEditing && !FRenaming && !IsCloneToNewSiteDefault());
  SaveButton->setVisible(SaveSessionAction->isEnabled());
  EditButton->setVisible(EditSessionAction->isEnabled());
  EditCancelButton->setVisible(EditCancelAction->isEnabled());
  SaveAsSessionMenuItem->setVisible(FEditing);
  FNoUpdate--;
}

void LoginDialog::TransferProtocolComboChange()
{
  if (FNoUpdate == 0)
  {
    if (GetFSProtocol(false) == fsS3)
    {
      FNoUpdate++;
      WebDavsCombo->setCurrentIndex(FtpsToIndex(ftpsImplicit));
      HostNameEdit->setText(S3HostName);
      FNoUpdate--;
    }
    else
    {
      FNoUpdate++;
      if (HostNameEdit->text() == S3HostName)
      {
        HostNameEdit->clear();
      }
      // the credentials taken from the AWS environment do not belong to other protocols
      QString S3Profile = GetS3Profile();
      if (S3CredentialsEnvCheck->isChecked() || !UserNameEdit->text().isEmpty())
      {
        if (!UserNameEdit->text().isEmpty() && (UserNameEdit->text() == S3FileSystem::EnvUserName(S3Profile)))
        {
          UserNameEdit->clear();
        }
        if (!PasswordEdit->text().isEmpty() && (PasswordEdit->text() == S3FileSystem::EnvPassword(S3Profile)))
        {
          PasswordEdit->clear();
        }
      }
      if (FEditingData.S3SessionToken == S3FileSystem::EnvSessionToken(S3Profile))
      {
        FEditingData.S3SessionToken.clear();
      }
      if (FEditingData.S3RoleArn == S3FileSystem::EnvRoleArn(S3Profile))
      {
        FEditingData.S3RoleArn.clear();
      }
      FNoUpdate--;
    }
    S3CredentialsEnvCheck->setChecked(false);
  }
  UpdatePortWithProtocol();
  UpdateControls();
}

QString LoginDialog::GetS3Profile() const
{
  QString Result = S3ProfileCombo->currentText().trimmed();
  return (Result == QStringLiteral("General")) ? QString() : Result;
}

void LoginDialog::LoadS3Profiles()
{
  QStringList Profiles = S3FileSystem::Profiles();
  Profiles.sort(Qt::CaseInsensitive);
  Profiles.prepend(QStringLiteral("General"));
  FNoUpdate++;
  QString Text = S3ProfileCombo->currentText();
  S3ProfileCombo->clear();
  S3ProfileCombo->addItems(Profiles);
  S3ProfileCombo->setEditText(Text.isEmpty() ? QStringLiteral("General") : Text);
  FNoUpdate--;
}

void LoginDialog::UpdateS3Credentials()
{
  if (S3CredentialsEnvCheck->isChecked())
  {
    QString S3Profile = GetS3Profile();
    FNoUpdate++;
    UserNameEdit->setText(S3FileSystem::EnvUserName(S3Profile));
    PasswordEdit->setText(S3FileSystem::EnvPassword(S3Profile));
    FNoUpdate--;
    // kept, so that checking and unchecking preserves them as the user name and password
    FEditingData.S3SessionToken = S3FileSystem::EnvSessionToken(S3Profile);
    FEditingData.S3RoleArn = S3FileSystem::EnvRoleArn(S3Profile);
  }
}

void LoginDialog::UpdatePortWithProtocol()
{
  int ADefaultPort = DefaultPort();
  if ((FNoUpdate == 0) && FUpdatePortWithProtocol)
  {
    FNoUpdate++;
    if (PortNumberEdit->value() == FDefaultPort)
    {
      PortNumberEdit->setValue(ADefaultPort);
    }
    FNoUpdate--;
  }
  FDefaultPort = ADefaultPort;
}

void LoginDialog::PortNumberEditChange()
{
  if (FNoUpdate == 0)
  {
    bool WellKnownPort = false;
    TFSProtocol FSProtocol = fsSFTP;
    TFtps Ftps = ftpsNone;
    // For ambiguous port numbers, keep the current protocol, even if it is
    // not the default protocol for the port (e.g. HTTPS vs S3 or FTP vs FTPES)
    TFSProtocol CurrentFSProtocol = GetFSProtocol(false);
    int PortNumber = PortNumberEdit->value();
    if (PortNumber == SshPortNumber)
    {
      FSProtocol = fsSFTP;
      WellKnownPort = true;
    }
    else if (PortNumber == FtpPortNumber)
    {
      FSProtocol = fsFTP;
      if ((CurrentFSProtocol == FSProtocol) && (GetFtps() == ftpsExplicitTls))
      {
        Ftps = ftpsExplicitTls;
      }
      WellKnownPort = true;
    }
    else if (PortNumber == FtpsImplicitPortNumber)
    {
      FSProtocol = fsFTP;
      Ftps = ftpsImplicit;
      WellKnownPort = true;
    }
    else if (PortNumber == HTTPPortNumber)
    {
      FSProtocol = (CurrentFSProtocol == fsS3) ? fsS3 : fsWebDAV;
      WellKnownPort = true;
    }
    else if (PortNumber == HTTPSPortNumber)
    {
      FSProtocol = (CurrentFSProtocol == fsS3) ? fsS3 : fsWebDAV;
      Ftps = ftpsImplicit;
      WellKnownPort = true;
    }

    if (WellKnownPort)
    {
      bool AllowScpFallback;
      int ProtocolIndex = FSProtocolToIndex(FSProtocol, AllowScpFallback);
      // SCP keeps its port
      if ((CurrentFSProtocol == fsSCPonly) && (FSProtocol == fsSFTP))
      {
        ProtocolIndex = TransferProtocolCombo->currentIndex();
      }
      if ((TransferProtocolCombo->currentIndex() == ProtocolIndex) && (GetFtps() == Ftps))
      {
        FUpdatePortWithProtocol = true;
      }
      else
      {
        FUpdatePortWithProtocol = false;
        FNoUpdate++;
        TransferProtocolCombo->setCurrentIndex(ProtocolIndex);
        SetFtpsIndex(FtpsToIndex(Ftps));
        FNoUpdate--;
      }
    }
  }
  UpdateControls();
}

bool LoginDialog::IsSiteAndCanOpen() const
{
  QTreeWidgetItem * Node = Selected();
  if (FEditing)
  {
    return false;
  }
  if (IsNewSiteNode(Node))
  {
    return !HostNameEdit->text().trimmed().isEmpty();
  }
  std::shared_ptr<SessionData> Data = NodeSession(Node);
  return (Data != nullptr) && FSessions.CanOpen(*Data);
}

bool LoginDialog::IsFolderOrWorkspaceAndCanOpen() const
{
  return IsFolderOrWorkspaceNode(Selected()) && HasNodeAnySession(Selected(), true);
}

bool LoginDialog::CanOpen() const
{
  return IsSiteAndCanOpen() || IsFolderOrWorkspaceAndCanOpen();
}

bool LoginDialog::IsCloneToNewSiteDefault() const
{
  std::shared_ptr<SessionData> Data = SelectedSession();
  return !FEditing && !FRenaming && (Data != nullptr) && !FSessions.CanOpen(*Data);
}

void LoginDialog::ResetNewSiteData()
{
  FNewSiteData = FSessions.DefaultSettings();
  FNewSiteData.Name.clear();
}

void LoginDialog::NewSite()
{
  QTreeWidgetItem * Node = NewSiteNode();
  if (Selected() != Node)
  {
    SetSelected(Node);
  }
  LoadContents();
}

void LoginDialog::Default()
{
  ResetNewSiteData();
  NewSite();
}

void LoginDialog::EditSession()
{
  HostNameEdit->setFocus();
}

bool LoginDialog::EnsureNotEditing()
{
  bool Result = !FEditing;
  if (!Result)
  {
    // LOGIN_SAVE_EDITING
    QMessageBox::StandardButton Answer = Confirm(this, QStringLiteral("You are editing a site. Do you want to save your changes?"),
      QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
    if (Answer == QMessageBox::Yes)
    {
      SaveAsSession(false);
      Result = !FEditing;
    }
    else if (Answer == QMessageBox::No)
    {
      CancelEditing();
      LoadContents();
      Result = true;
    }
  }
  return Result;
}

void LoginDialog::CancelEditing()
{
  FEditing = false;
  // reset back the color
  UpdateNodeImage(Selected());
}

void LoginDialog::PersistNewSiteIfNeeded()
{
  if (isVisible() && IsNewSiteNode(Selected()))
  {
    SaveSession(FNewSiteData);
  }
}

bool LoginDialog::ParseUrl(const QString & Url)
{
  SessionData Data;
  SaveSession(Data);
  if (SessionData::IsIPv6Literal(Url))
  {
    QString IPv6Literal = Url;
    if (IPv6Literal.startsWith(QLatin1Char('[')) && IPv6Literal.endsWith(QLatin1Char(']')))
    {
      IPv6Literal = IPv6Literal.mid(1, IPv6Literal.size() - 2);
    }
    Data.HostName = IPv6Literal;
  }
  else
  {
    int ParsedInfo = 0;
    SessionData Parsed = Data;
    Parsed.ParseUrl(Url, ParsedInfo);
    // UNSAFE_SESSION
    if (((ParsedInfo & SessionData::ParsedUnsafeSettings) != 0) &&
        (Confirm(this, QStringLiteral("<b>Use potentially unsafe session settings?</b><br>The provided session URL contains potentially "
          "unsafe settings. Proceed only if you trust its source.")) != QMessageBox::Ok))
    {
      return false;
    }
    Data = Parsed;
  }
  LoadSession(Data);
  return true;
}

void LoginDialog::ParseHostName()
{
  QString HostName = HostNameEdit->text().trimmed();
  if (HostName.isEmpty())
  {
    return;
  }
  bool NeedParse =
    SessionData::IsIPv6Literal(HostName) ? HostName.startsWith(QLatin1Char('[')) :
    (HostName.contains(QLatin1Char('@')) || HostName.contains(QLatin1Char(':')) || HostName.contains(QLatin1Char('/')) ||
     HostName.contains(QLatin1Char(';')));
  if (NeedParse)
  {
    ParseUrl(HostName);
  }
}

void LoginDialog::EditSessionActionExecute()
{
  if (SelectedSession() != nullptr)
  {
    FEditing = true;
    FEditingName = SelectedSession()->Name;
    EditSession();
    // reload the session, to load the password
    LoadContents();
    UpdateControls();
  }
}

void LoginDialog::EditCancelActionExecute()
{
  CancelEditing();
  // reset back to saved settings
  LoadContents();
  UpdateControls();
  SessionTree->setFocus();
}

void LoginDialog::SaveSessionActionExecute()
{
  // for New Site, the "save" command is actually "save as"
  SaveAsSession(IsNewSiteNode(Selected()));
}

bool LoginDialog::SessionNameValidate(const QString & Text, const QString & OriginalName)
{
  std::shared_ptr<SessionData> Data = FSessions.FindByName(Text);
  if ((Data != nullptr) && Data->Special)
  {
    // CANNOT_OVERWRITE_SPECIAL_SESSION
    ShowErrorMessage(this, QStringLiteral("Cannot overwrite special site '%1'.").arg(Text));
    return false;
  }
  if ((Data != nullptr) && (Data->Name.compare(OriginalName, Qt::CaseInsensitive) != 0) &&
      // CONFIRM_OVERWRITE_SESSION
      (Confirm(this, QStringLiteral("Site with name '%1' already exists. Overwrite?").arg(Text),
        QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes))
  {
    return false;
  }
  return true;
}

std::shared_ptr<SessionData> LoginDialog::DoSaveSession(SessionData & Data, const SessionData * OriginalSession, bool ForceDialog,
  const QStringList & AdditionalFolders)
{
  // DoSaveSession (forms/Custom.cpp)
  Configuration & Config = Configuration::Instance();
  bool UseMasterPassword = Config.UseMasterPassword();
  bool NotRecommendedSavingPassword = !UseMasterPassword && (Data.UserName.compare(AnonymousUserName, Qt::CaseInsensitive) != 0);
  bool CanSavePassword = !Config.DisablePasswordStoring() && Data.HasAnySessionPassword();
  bool SavePassword = false;
  if (CanSavePassword)
  {
    SavePassword = ((OriginalSession != nullptr) && OriginalSession->HasAnySessionPassword()) || !NotRecommendedSavingPassword;
  }
  QString SessionName = Data.SessionName();
  bool CreateShortcut = false;
  if (ForceDialog || (CanSavePassword && !SavePassword))
  {
    // TSaveSessionDialog
    QDialog Dialog(this);
    Dialog.setWindowTitle(QStringLiteral("Save session as site"));
    auto * Layout = new QVBoxLayout(&Dialog);
    auto * SessionNameEdit = new QLineEdit(Data.LocalName().isEmpty() ? SessionName : SessionName.mid(SessionName.lastIndexOf(QLatin1Char('/')) + 1));
    auto * NameLabel = new QLabel(QStringLiteral("&Site name:"));
    NameLabel->setBuddy(SessionNameEdit);
    // SAVE_SESSION_ROOT_FOLDER2
    const QString RootFolder = QStringLiteral("<none>");
    QStringList Folders = AdditionalFolders;
    for (const auto & S : FSessions.Sessions())
    {
      if (!S->Special && !S->IsWorkspace)
      {
        QString Folder = S->FolderName();
        if (!Folder.isEmpty() && !Folders.contains(Folder, Qt::CaseInsensitive))
        {
          Folders.append(Folder);
        }
      }
    }
    std::sort(Folders.begin(), Folders.end(), [](const QString & A, const QString & B) { return A.compare(B, Qt::CaseInsensitive) < 0; });
    auto * FolderCombo = new QComboBox();
    FolderCombo->setEditable(true);
    FolderCombo->setInsertPolicy(QComboBox::NoInsert);
    FolderCombo->addItem(RootFolder);
    FolderCombo->addItems(Folders);
    QString Folder = SessionName.contains(QLatin1Char('/')) ? SessionName.left(SessionName.lastIndexOf(QLatin1Char('/'))) : QString();
    FolderCombo->setCurrentText(Folder.isEmpty() ? RootFolder : Folder);
    auto * FolderLabel = new QLabel(QStringLiteral("&Folder:"));
    FolderLabel->setBuddy(FolderCombo);
    auto * SavePasswordCheck = new QCheckBox(
      NotRecommendedSavingPassword ? QStringLiteral("Save &password (not recommended)") :
        (UseMasterPassword ? QStringLiteral("Save &password (protected by master password)") : QStringLiteral("Save &password")));
    SavePasswordCheck->setChecked(SavePassword);
    SavePasswordCheck->setEnabled(CanSavePassword);
    // SAVE_SITE_WORKSPACE_SHORTCUT
    auto * CreateShortcutCheck = new QCheckBox(QStringLiteral("&Create desktop shortcut"));
    if (!DesktopIntegration::CanCreateSessionShortcut())
    {
      CreateShortcutCheck->hide();
    }
    Layout->addWidget(NameLabel);
    Layout->addWidget(SessionNameEdit);
    Layout->addWidget(FolderLabel);
    Layout->addWidget(FolderCombo);
    Layout->addWidget(SavePasswordCheck);
    Layout->addWidget(CreateShortcutCheck);
    auto * Buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    // TSaveSessionDialog: TCustomDialog(HELP_SESSION_SAVE)
    HelpSystem::AddHelpButton(Buttons, &Dialog, QStringLiteral("ui_login_save"));
    QPushButton * OkButton = Buttons->button(QDialogButtonBox::Ok);
    connect(SessionNameEdit, &QLineEdit::textChanged, &Dialog, [OkButton](const QString & Text) { OkButton->setEnabled(!Text.isEmpty()); });
    OkButton->setEnabled(!SessionNameEdit->text().isEmpty());
    auto GetSessionName = [FolderCombo, SessionNameEdit, RootFolder]()
    {
      QString AFolder = FolderCombo->currentText().trimmed();
      while (AFolder.endsWith(QLatin1Char('/')))
      {
        AFolder.chop(1);
      }
      return SessionData::ComposePath((AFolder == RootFolder) ? QString() : AFolder, SessionNameEdit->text());
    };
    QString OriginalName = Data.Name;
    connect(Buttons, &QDialogButtonBox::accepted, &Dialog, [&]()
    {
      // DoValidate
      QString Error;
      if (!SessionData::IsValidName(SessionNameEdit->text(), &Error))
      {
        ShowErrorMessage(&Dialog, Error);
        return;
      }
      QString Name = GetSessionName();
      if (!SessionNameValidate(Name, OriginalName))
      {
        return;
      }
      QString AFolder = Name.contains(QLatin1Char('/')) ? Name.left(Name.lastIndexOf(QLatin1Char('/'))) : QString();
      if (!AFolder.isEmpty() && FSessions.IsWorkspace(AFolder))
      {
        // WORKSPACE_NOT_FOLDER
        ShowErrorMessage(&Dialog, QStringLiteral("'%1' is workspace, not site folder.").arg(AFolder));
        return;
      }
      Dialog.accept();
    });
    connect(Buttons, &QDialogButtonBox::rejected, &Dialog, &QDialog::reject);
    Layout->addWidget(Buttons);
    SessionNameEdit->selectAll();
    Dialog.resize(380, Dialog.sizeHint().height());
    if (Dialog.exec() != QDialog::Accepted)
    {
      return nullptr;
    }
    SessionName = GetSessionName();
    SavePassword = SavePasswordCheck->isChecked();
    CreateShortcut = CreateShortcutCheck->isChecked();
  }
  if ((CanSavePassword && !SavePassword) || Config.DisablePasswordStoring())
  {
    Workspaces::ClearSessionPasswords(Data);
  }
  std::shared_ptr<SessionData> NewSession = FSessions.NewSession(SessionName, Data);
  Config.Save();
  if (CreateShortcut)
  {
    QString Error;
    if (!CreateDesktopSessionShortCut(SessionName, &Error))
    {
      ShowErrorMessage(this, Error);
    }
  }
  return NewSession;
}

void LoginDialog::SaveAsSession(bool ForceDialog)
{
  // Parse hostname before saving
  ParseHostName();
  SessionData Data;
  SaveSession(Data);
  std::shared_ptr<SessionData> EditingSessionData = FEditing ? SelectedSession() : nullptr;
  // empty folders (not persistent, known to the dialog only)
  QStringList NewFolders;
  for (QTreeWidgetItem * Node : AllNodes())
  {
    if (IsFolderNode(Node) && (Node->childCount() == 0))
    {
      NewFolders.append(SessionNodePath(Node));
    }
  }
  std::shared_ptr<SessionData> NewSession = DoSaveSession(Data, EditingSessionData.get(), ForceDialog, NewFolders);
  if (NewSession != nullptr)
  {
    FEditing = false;
    FNewSiteKeepName = false;
    ResetNewSiteData();
    // reloads fake password, clears the password box, if not saved
    ReloadSessions(NewSession->Name);
    SessionTree->setFocus();
  }
}

void LoginDialog::CloneToNewSite()
{
  std::shared_ptr<SessionData> Session = SelectedSession();
  if (Session == nullptr)
  {
    return;
  }
  FNewSiteData = *Session;
  FNewSiteData.Special = false;
  // a new site, usually another server: agents must not reach it unasked, or
  // under the name they use for the original (AgentAccess::FindAgentSite)
  FNewSiteData.AgentAccess = 0;
  FNewSiteData.AgentName.clear();
  FNewSiteData.MakeUniqueIn(FSessions);
  FNewSiteKeepName = true;
  NewSite();
  EditSession();
}

void LoginDialog::ResetNewSessionActionExecute()
{
  Default();
  EditSession();
  FNewSiteKeepName = false;
}

void LoginDialog::SetDefaultSessionActionExecute()
{
  // SET_DEFAULT_SESSION_SETTINGS
  if (Confirm(this, QStringLiteral("Set current session settings as default?")) == QMessageBox::Ok)
  {
    SessionData Data;
    SaveSession(Data);
    FSessions.SetDefaultSettings(Data);
    Configuration::Instance().Save();
    Default();
  }
}

bool LoginDialog::SelectedTeamItem(QString & TeamUuid, QString & Relative) const
{
  QTreeWidgetItem * Node = Selected();
  QString Path = SessionNodePath(Node);
  if (Path.isEmpty() || !Path.startsWith(CloudSettings::TeamFolderPrefix))
  {
    return false;
  }
  if (IsFolderNode(Node))
  {
    // the team's folder itself: everything; a subfolder: "sub/"
    for (const TeamSync::Team & T : TeamSync::Instance().Teams())
    {
      QString Folder = T.Folder;
      if (Folder.isEmpty())
      {
        continue;
      }
      if (Path == Folder)
      {
        TeamUuid = T.Uuid;
        Relative = QString();
        return true;
      }
      if (Path.startsWith(Folder + QLatin1Char('/')))
      {
        TeamUuid = T.Uuid;
        Relative = Path.mid(Folder.size() + 1) + QLatin1Char('/');
        return true;
      }
    }
    return false;
  }
  return TeamsDialog::TeamSite(Path, TeamUuid, Relative);
}

void LoginDialog::ShareWithTeamActionExecute()
{
  UpdateActions();
  if (!ShareWithTeamAction->isEnabled())
  {
    return;
  }
  QTreeWidgetItem * Node = Selected();
  QString Path = SessionNodePath(Node);
  QStringList Names;
  QString Subfolder;
  if (IsFolderNode(Node))
  {
    for (const auto & Data : FSessions.Sessions())
    {
      if (Data->Name.startsWith(Path + QLatin1Char('/')) && !Data->IsWorkspace)
      {
        Names.append(Data->Name);
      }
    }
    Subfolder = Path.section(QLatin1Char('/'), -1);
  }
  else
  {
    Names.append(Path);
  }
  if (!Names.isEmpty() && TeamsDialog::ShareSites(this, Names, Subfolder))
  {
    ReloadSessions(QString());
  }
}

void LoginDialog::StopSharingActionExecute()
{
  QString TeamUuid, Relative;
  if (!SelectedTeamItem(TeamUuid, Relative))
  {
    return;
  }
  QStringList Names;
  if (Relative.isEmpty())
  {
    // the whole team folder: everything shared
    Names = TeamSync::Instance().SharedSites(TeamUuid);
  }
  else
  {
    Names.append(Relative);
  }
  if (TeamsDialog::StopSharing(this, TeamUuid, Names))
  {
    ReloadSessions(QString());
  }
}

void LoginDialog::AgentAccessActionExecute()
{
  if (!IsEditable())
  {
    return;
  }
  // as Advanced: the dialog works on the site being edited, kept by Save
  ParseHostName();
  SaveSession(FEditingData);
  AgentAccessDialog Dialog(FEditingData, this);
  if (Dialog.exec() == QDialog::Accepted)
  {
    LoadSession(FEditingData);
  }
}

void LoginDialog::SessionAdvancedActionExecute(bool Raw)
{
  if (!IsEditable())
  {
    return;
  }
  // parse hostname (it may change protocol particularly) before opening advanced settings
  ParseHostName();
  SaveSession(FEditingData);
  if (Raw)
  {
    SiteRawDialog Dialog(FEditingData, this);
    Dialog.exec();
  }
  else
  {
    SiteAdvancedDialog Dialog(FEditingData, this);
    Dialog.exec();
  }
  // Needed for Note (and the protocol)
  LoadSession(FEditingData);
  if (IsSiteNode(Selected()))
  {
    Selected()->setIcon(0, SessionIcon(FEditingData));
  }
}

void LoginDialog::DeleteSessionActionExecute()
{
  QTreeWidgetItem * Node = Selected();
  if (Node == nullptr)
  {
    return;
  }
  // next node to select: sibling below, above or the parent
  QTreeWidgetItem * Parent = Node->parent() ? Node->parent() : SessionTree->invisibleRootItem();
  int Index = Parent->indexOfChild(Node);
  QTreeWidgetItem * Next = (Index + 1 < Parent->childCount()) ? Parent->child(Index + 1) : ((Index > 0) ? Parent->child(Index - 1) : Node->parent());
  QString NextPath = (Next != nullptr) ? (SessionNodePath(Next) + (IsFolderNode(Next) ? QStringLiteral("/") : QString())) : QString();
  if (IsSiteNode(Node))
  {
    std::shared_ptr<SessionData> Session = SelectedSession();
    if ((Session == nullptr) || Session->Special || FEditing)
    {
      return;
    }
    // CONFIRM_DELETE_SESSION
    if (Confirm(this, QStringLiteral("Are you sure you want to delete site '%1'?").arg(Session->SessionName())) == QMessageBox::Ok)
    {
      FSessions.Remove(Session->Name);
      Configuration::Instance().Save();
      ReloadSessions(NextPath);
    }
  }
  else if (IsFolderOrWorkspaceNode(Node))
  {
    QString Path = SessionNodePath(Node);
    bool Workspace = IsWorkspaceNode(Node);
    int Sessions = 0;
    for (const auto & Data : FSessions.Sessions())
    {
      if ((Data->IsWorkspace == Workspace) && Data->Name.startsWith(Path + QLatin1Char('/'), Qt::CaseInsensitive))
      {
        if (Data->Special)
        {
          // LOGIN_DELETE_SPECIAL_SESSION
          ShowErrorMessage(this, QStringLiteral("Cannot delete special site '%1'.").arg(Data->SessionName()));
          return;
        }
        Sessions++;
      }
    }
    // LOGIN_DELETE_SESSION_FOLDER, LOGIN_DELETE_WORKSPACE
    QString Prompt = Workspace ?
      QStringLiteral("Are you sure you want to delete workspace '%1' with %2 session(s)?") :
      QStringLiteral("Are you sure you want to delete site folder '%1' with %2 site(s)?");
    if ((Sessions == 0) || (Confirm(this, Prompt.arg(Path).arg(Sessions)) == QMessageBox::Ok))
    {
      if (Workspace)
      {
        Workspaces::DeleteWorkspace(FSessions, Path);
      }
      else
      {
        FSessions.RemoveFolder(Path);
      }
      Configuration::Instance().Save();
      ReloadSessions(NextPath);
    }
  }
}

void LoginDialog::RenameSessionActionExecute()
{
  QTreeWidgetItem * Node = Selected();
  if ((Node == nullptr) || !RenameSessionAction->isEnabled())
  {
    return;
  }
  // SessionTreeEditing
  ResetSitesIncrementalSearch();
  SessionTree->setFocus();
  FRenamingOriginal = Node->text(0);
  Node->setFlags(Node->flags() | Qt::ItemIsEditable);
  FRenaming = true;
  UpdateControls();
  SessionTree->editItem(Node);
}

bool LoginDialog::CheckDuplicateFolder(QTreeWidgetItem * Parent, const QString & Text, QTreeWidgetItem * Node)
{
  QTreeWidgetItem * AParent = (Parent != nullptr) ? Parent : SessionTree->invisibleRootItem();
  for (int Index = 0; Index < AParent->childCount(); Index++)
  {
    QTreeWidgetItem * ANode = AParent->child(Index);
    // a folder may have the same name as a site on the same level
    if ((ANode != Node) && IsFolderOrWorkspaceNode(ANode) && (ANode->text(0).compare(Text, Qt::CaseInsensitive) == 0))
    {
      // LOGIN_DUPLICATE_SESSION_FOLDER_WORKSPACE
      ShowErrorMessage(this, QStringLiteral("Site folder or workspace with name '%1' already exists.").arg(Text));
      return false;
    }
  }
  return true;
}

bool LoginDialog::CheckIsSessionFolder(QTreeWidgetItem * Node)
{
  for (QTreeWidgetItem * ANode = Node; ANode != nullptr; ANode = ANode->parent())
  {
    if (IsWorkspaceNode(ANode))
    {
      ShowErrorMessage(this, QStringLiteral("'%1' is workspace, not site folder.").arg(SessionNodePath(ANode)));
      return false;
    }
  }
  return true;
}

void LoginDialog::SessionTreeEdited(QTreeWidgetItem * Node)
{
  // SessionTreeEdited
  bool Exists = false;
  for (QTreeWidgetItem * ANode : AllNodes())
  {
    Exists = Exists || (ANode == Node);
  }
  if (!Exists || (Node == nullptr))
  {
    UpdateControls();
    return;
  }
  Node->setFlags(Node->flags() & ~Qt::ItemIsEditable);
  QString S = Node->text(0).trimmed();
  QString Original = FRenamingOriginal;
  auto Revert = [&]()
  {
    Node->setText(0, Original);
    UpdateControls();
  };
  if ((S == Original) || S.isEmpty())
  {
    Revert();
    return;
  }
  QString Error;
  if (!SessionData::IsValidName(S, &Error))
  {
    ShowErrorMessage(this, Error);
    Revert();
    return;
  }
  if (IsSiteNode(Node))
  {
    std::shared_ptr<SessionData> Session = NodeSession(Node);
    if (Session == nullptr)
    {
      Revert();
      return;
    }
    QString Path = SessionData::ComposePath(Session->FolderName(), S);
    if (!SessionNameValidate(Path, Session->Name))
    {
      Revert();
      return;
    }
    SessionData Copy = *Session;
    FSessions.Remove(Session->Name);
    FSessions.NewSession(Path, Copy);
    Workspaces::UpdateLinks(FSessions, Session->Name, Path);
    Configuration::Instance().Save();
    ReloadSessions(Path);
  }
  else
  {
    if (!CheckDuplicateFolder(Node->parent(), S, Node))
    {
      Revert();
      return;
    }
    QString ParentPath = SessionNodePath(Node->parent());
    QString OldRoot = SessionData::ComposePath(ParentPath, Original);
    QString NewRoot = SessionData::ComposePath(ParentPath, S);
    FSessions.RenameFolder(OldRoot, NewRoot);
    Workspaces::UpdateLinks(FSessions, OldRoot, NewRoot, true);
    Configuration::Instance().Save();
    ReloadSessions(NewRoot + (IsFolderNode(Node) ? QStringLiteral("/") : QString()));
  }
}

void LoginDialog::NewSessionFolderActionExecute()
{
  QTreeWidgetItem * ParentNode = SessionFolderNode(Selected());
  QString ParentPath = SessionNodePath(ParentNode);
  // NEW_FOLDER
  QString Name = (ParentPath.isEmpty() ? QString() : (ParentPath + QLatin1Char('/'))) + QStringLiteral("New folder");
  QInputDialog Dialog(this);
  // LOGIN_NEW_SESSION_FOLDER_CAPTION, LOGIN_NEW_SESSION_FOLDER_PROMPT
  Dialog.setWindowTitle(QStringLiteral("Create Site Folder"));
  Dialog.setLabelText(QStringLiteral("New folder name:"));
  Dialog.setTextValue(Name);
  // HELP_NEW_SESSION_FOLDER (F1; QInputDialog has no Help button)
  HelpSystem::InstallFormHelp(&Dialog, QStringLiteral("ui_login#management"));
  // NewSessionFolderInputDialogInitialize: select the last path component
  QTimer::singleShot(0, &Dialog, [&Dialog, Name]()
  {
    if (QLineEdit * Edit = Dialog.findChild<QLineEdit *>())
    {
      qsizetype P = Name.lastIndexOf(QLatin1Char('/'));
      Edit->setSelection(static_cast<int>(P + 1), static_cast<int>(Name.size() - P - 1));
    }
  });
  if (Dialog.exec() != QDialog::Accepted)
  {
    return;
  }
  Name = Dialog.textValue().trimmed();
  while (Name.endsWith(QLatin1Char('/')))
  {
    Name.chop(1);
  }
  if (Name.isEmpty())
  {
    return;
  }
  qsizetype P = Name.lastIndexOf(QLatin1Char('/'));
  QString ParentFolder = (P >= 0) ? Name.left(P) : QString();
  QString LocalName = (P >= 0) ? Name.mid(P + 1) : Name;
  QTreeWidgetItem * Parent = ParentFolder.isEmpty() ? nullptr :
    FindNodeByPath(SessionTree, ParentFolder, [this](const QString & Path) { return AddSessionPath(Path, false); });
  if ((Parent != nullptr) && !CheckIsSessionFolder(Parent))
  {
    return;
  }
  if (!ParentFolder.isEmpty() && FSessions.IsWorkspace(ParentFolder))
  {
    ShowErrorMessage(this, QStringLiteral("'%1' is workspace, not site folder.").arg(ParentFolder));
    return;
  }
  QTreeWidgetItem * ExistingParent = ParentFolder.isEmpty() ? nullptr : AddSessionPath(ParentFolder, false);
  if ((ParentFolder.isEmpty() || (ExistingParent != nullptr)) && !CheckDuplicateFolder(ExistingParent, LocalName, nullptr))
  {
    return;
  }
  FSessions.AddFolder(Name);
  ReloadSessions(Name + QLatin1Char('/'));
}

void LoginDialog::SessionTreeDropped(QTreeWidgetItem * Node, QTreeWidgetItem * DropTarget)
{
  // SessionTreeDragDrop
  if ((Node != Selected()) || !SessionAllowDrop(DropTarget) || !EnsureNotEditing())
  {
    return;
  }
  std::shared_ptr<SessionData> Session = SelectedSession();
  if (Session == nullptr)
  {
    return;
  }
  QString Path = SessionData::ComposePath(SessionNodePath(DropTarget), Session->LocalName());
  if (!SessionNameValidate(Path, Session->SessionName()))
  {
    return;
  }
  SessionData Copy = *Session;
  FSessions.Remove(Session->Name);
  FSessions.NewSession(Path, Copy);
  Workspaces::UpdateLinks(FSessions, Session->Name, Path);
  Configuration::Instance().Save();
  ReloadSessions(Path);
}

QTreeWidgetItem * LoginDialog::NormalizeDropTarget(QTreeWidgetItem * DropTarget) const
{
  return IsWorkspaceNode(DropTarget) ? DropTarget : SessionFolderNode(DropTarget);
}

bool LoginDialog::SessionAllowDrop(QTreeWidgetItem * DropTarget) const
{
  QTreeWidgetItem * Node = Selected();
  return (Node != nullptr) && IsSiteNode(Node) && (Node->parent() != DropTarget) && ((DropTarget == nullptr) || IsFolderNode(DropTarget));
}

void LoginDialog::DesktopIconActionExecute()
{
  QTreeWidgetItem * Node = Selected();
  QString Name;
  QString Message;
  if (IsSiteNode(Node))
  {
    std::shared_ptr<SessionData> Session = SelectedSession();
    if (Session == nullptr)
    {
      return;
    }
    Name = Session->Name;
    // CONFIRM_CREATE_SHORTCUT
    Message = QStringLiteral("Create desktop shortcut for site '%1'?").arg(Name);
  }
  else if (IsFolderNode(Node))
  {
    Name = SessionNodePath(Node);
    Message = QStringLiteral("Create desktop shortcut for site folder '%1'?").arg(Name);
  }
  else if (IsWorkspaceNode(Node))
  {
    Name = SessionNodePath(Node);
    Message = QStringLiteral("Create desktop shortcut for workspace '%1'?").arg(Name);
  }
  else
  {
    return;
  }
  if (Confirm(this, Message, QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes)
  {
    QString Error;
    if (!CreateDesktopSessionShortCut(Name, &Error))
    {
      ShowErrorMessage(this, Error);
    }
  }
}

void LoginDialog::UpdateClipboardUrl()
{
  QString ClipboardText = QGuiApplication::clipboard()->text().trimmed();
  FClipboardHasUrl = false;
  if (!ClipboardText.isEmpty() && !ClipboardText.contains(QLatin1Char('\n')))
  {
    SessionData Probe;
    int ParsedInfo = 0;
    FClipboardHasUrl = Probe.ParseUrl(ClipboardText, ParsedInfo) && ((ParsedInfo & SessionData::ParsedProtocolDefined) != 0);
  }
  PasteUrlAction->setEnabled(FClipboardHasUrl);
}

void LoginDialog::PasteUrlActionExecute()
{
  QString ClipboardUrl = QGuiApplication::clipboard()->text().trimmed();
  UpdateClipboardUrl();
  UpdateActions();
  if (ClipboardUrl.isEmpty() || !PasteUrlAction->isEnabled())
  {
    return;
  }
  if (!IsEditable())
  {
    // select new site node, when other node is selected and not in editing mode
    SetSelected(NewSiteNode());
  }
  if (IsEditable())
  {
    ParseUrl(ClipboardUrl);
  }
  // visualize the pasting
  HostNameEdit->setFocus();
  HostNameEdit->selectAll();
}

void LoginDialog::AboutActionExecute()
{
  QMessageBox::about(this, QStringLiteral("About NixSCP"),
    QStringLiteral("<b>NixSCP %1</b><br>Native Linux port of WinSCP.<br><br>"
      "Based on WinSCP by Martin Prikryl, licensed under GNU GPL 3.<br>"
      "Uses Qt %2 and libssh.").arg(QApplication::applicationVersion(), QString::fromLatin1(qVersion())));
}

void LoginDialog::ImportSessionsActionExecute()
{
  QStringList Imported;
  // Can be empty when imported known_hosts
  if (ImportSites(FSessions, this, &Imported) && !Imported.isEmpty())
  {
    // Focus the first imported session
    ReloadSessions(Imported.first());
    SessionTree->setFocus();
  }
}

void LoginDialog::ImportActionExecute()
{
  // IMPORT_CONFIGURATION2
  if (QMessageBox::warning(this, QStringLiteral("Warning"),
        QStringLiteral("<b>Restore configuration from a backup?</b><br><br>Importing configuration will overwrite all your preferences "
          "and sites. Consider saving your work and creating backup of your current configuration."),
        QMessageBox::Ok | QMessageBox::Cancel) != QMessageBox::Ok)
  {
    return;
  }
  Configuration & Config = Configuration::Instance();
  // IMPORT_CONF_TITLE, EXPORT_CONF_FILTER
  QString FileName = QFileDialog::getOpenFileName(this, QStringLiteral("Select file to import configuration from"),
    Config.DefaultIniFileExportPath(), QStringLiteral("INI files (*.ini);;All files (*)"));
  if (FileName.isEmpty())
  {
    return;
  }
  QString Error;
  if (!CloudSyncUi::ImportConfiguration(this, FileName, &Error))
  {
    if (!Error.isEmpty())  // empty: cancelled
    {
      ShowErrorMessage(this, Error);
    }
    return;
  }
  // Configuration::Import reloads everything (no restart needed)
  FEditing = false;
  ResetNewSiteData();
  ReloadSessions(QString());
}

void LoginDialog::ExportActionExecute()
{
  Configuration & Config = Configuration::Instance();
  // EXPORT_CONF_TITLE
  QString FileName = QFileDialog::getSaveFileName(this, QStringLiteral("Select file to export configuration to"),
    Config.DefaultIniFileExportPath(), QStringLiteral("INI files (*.ini);;All files (*)"));
  if (FileName.isEmpty())
  {
    return;
  }
  if (QFileInfo(FileName).suffix().isEmpty())
  {
    FileName += QStringLiteral(".ini");
  }
  SaveState();
  QString Error;
  if (!CloudSyncUi::ExportConfiguration(this, FileName, &Error))
  {
    if (!Error.isEmpty())  // empty: cancelled
    {
      ShowErrorMessage(this, Error);
    }
  }
}

// Temporary folders of NixSCP (TWinConfiguration::DoFindTemporaryFolders)
static QStringList FindTemporaryFolders()
{
  QStringList Result;
  QDir Dir(Configuration::Instance().ExpandedTemporaryDirectory());
  for (const QFileInfo & Info : Dir.entryInfoList({QStringLiteral("nixscp-*")}, QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden))
  {
    if (Info.isDir() && !Info.isSymLink() && Platform::OwnedByCurrentUser(Info.ownerId()))
    {
      Result.append(Info.absoluteFilePath());
    }
  }
  return Result;
}

void LoginDialog::CleanUpActionExecute()
{
  // TCleanupDialog (Linux: configuration INI file and temporary folders)
  Configuration & Config = Configuration::Instance();
  struct TLocation
  {
    QString Caption;
    QString Location;
    std::function<QString()> Cleanup;
  };
  QList<TLocation> Locations;
  QString IniFileName = Config.IniFileName();
  if (!Config.IsNulStorage() && QFileInfo::exists(IniFileName))
  {
    Locations.append({QStringLiteral("Configuration INI file"), IniFileName, [IniFileName]()
    {
      if (QFileInfo::exists(IniFileName) && !QFile::remove(IniFileName))
      {
        return QStringLiteral("Error cleaning up INI file.");
      }
      // TConfiguration::DontSave
      Configuration::Instance().SetIniFileName(QStringLiteral("nul"));
      return QString();
    }});
  }
  QStringList TemporaryFolders = FindTemporaryFolders();
  if (!TemporaryFolders.isEmpty())
  {
    Locations.append({QStringLiteral("Temporary folders"), Config.ExpandedTemporaryDirectory(), [TemporaryFolders]()
    {
      QStringList Errors;
      for (const QString & Folder : TemporaryFolders)
      {
        if (!QDir(Folder).removeRecursively())
        {
          Errors.append(Folder);
        }
      }
      // CLEANUP_TEMP_ERROR
      return Errors.isEmpty() ? QString() :
        (QStringLiteral("Some of the temporary folders have not been deleted. If you have some files stored in them still opened, "
          "close them and try again.\n\n") + Errors.join(QLatin1Char('\n')));
    }});
  }

  QDialog Dialog(this);
  Dialog.setWindowTitle(QStringLiteral("Cleanup application data"));
  Dialog.resize(489, 323);
  auto * Layout = new QVBoxLayout(&Dialog);
  auto * Label = new QLabel(QStringLiteral(
    "Following list contains all application data possibly stored on this computer. Check those you want to remove.\n\n"
    "If another instance of application is running, please close it before cleaning data.\n\n"
    "Notice: Opening session and/or next execution of application will recreate some of data."));
  Label->setWordWrap(true);
  Layout->addWidget(Label);
  auto * DataListView = new QTreeWidget();
  DataListView->setHeaderLabels({QStringLiteral("Data"), QStringLiteral("Location")});
  DataListView->setProperty("NixSCPTranslateItems", QVariantList{ 0 });
  DataListView->setRootIsDecorated(false);
  for (const TLocation & Location : Locations)
  {
    auto * Item = new QTreeWidgetItem(DataListView, {Location.Caption, Location.Location});
    Item->setFlags(Item->flags() | Qt::ItemIsUserCheckable);
    Item->setCheckState(0, Qt::Unchecked);
    Item->setToolTip(0, QStringLiteral("%1\nLocation: %2").arg(Location.Caption, Location.Location));
  }
  DataListView->resizeColumnToContents(0);
  Layout->addWidget(DataListView, 1);
  auto * CheckAllButton = new QPushButton(QStringLiteral("Un/check &all"));
  auto * Buttons = new QDialogButtonBox();
  QPushButton * OKButton = Buttons->addButton(QDialogButtonBox::Ok);
  Buttons->addButton(QStringLiteral("Close"), QDialogButtonBox::RejectRole);
  HelpSystem::AddHelpButton(Buttons, &Dialog, QStringLiteral("ui_cleanup"));
  auto * ButtonsLayout = new QHBoxLayout();
  ButtonsLayout->addWidget(CheckAllButton);
  ButtonsLayout->addStretch();
  ButtonsLayout->addWidget(Buttons);
  Layout->addLayout(ButtonsLayout);
  auto AnyChecked = [DataListView]()
  {
    for (int Index = 0; Index < DataListView->topLevelItemCount(); Index++)
    {
      if (DataListView->topLevelItem(Index)->checkState(0) == Qt::Checked)
      {
        return true;
      }
    }
    return false;
  };
  OKButton->setEnabled(false);
  CheckAllButton->setEnabled(DataListView->topLevelItemCount() > 0);
  connect(DataListView, &QTreeWidget::itemChanged, &Dialog, [OKButton, AnyChecked]() { OKButton->setEnabled(AnyChecked()); });
  connect(CheckAllButton, &QPushButton::clicked, &Dialog, [DataListView]()
  {
    bool AllChecked = true;
    for (int Index = 0; Index < DataListView->topLevelItemCount(); Index++)
    {
      AllChecked = AllChecked && (DataListView->topLevelItem(Index)->checkState(0) == Qt::Checked);
    }
    for (int Index = 0; Index < DataListView->topLevelItemCount(); Index++)
    {
      DataListView->topLevelItem(Index)->setCheckState(0, AllChecked ? Qt::Unchecked : Qt::Checked);
    }
  });
  connect(Buttons, &QDialogButtonBox::accepted, &Dialog, &QDialog::accept);
  connect(Buttons, &QDialogButtonBox::rejected, &Dialog, &QDialog::reject);
  if (Dialog.exec() == QDialog::Accepted)
  {
    for (int Index = 0; Index < DataListView->topLevelItemCount(); Index++)
    {
      if (DataListView->topLevelItem(Index)->checkState(0) == Qt::Checked)
      {
        QString Error = Locations[Index].Cleanup();
        if (!Error.isEmpty())
        {
          ShowErrorMessage(this, Error);
        }
      }
    }
    ReloadSessions(QString());
  }
}

//---------------------------------------------------------------------------
// Menus

QMenu * LoginDialog::GetSelectedNodePopupMenu()
{
  UpdateActions();
  QTreeWidgetItem * Node = Selected();
  auto * Menu = new QMenu(this);
  auto AddSearch = [this, Menu]()
  {
    Menu->addSection(QStringLiteral("Search"));
    Menu->addAction(SearchSiteStartAction);
    Menu->addMenu(CreateSearchOptionsMenu(Menu));
  };
  if (IsNewSiteNode(Node))
  {
    // ManageNewSitePopupMenu
    Menu->addSection(QStringLiteral("New Site"));
    Menu->addAction(LoginAction);
    Menu->addAction(PuttyAction);
    Menu->addSeparator();
    Menu->addAction(SaveAsSessionAction);
    Menu->addAction(ResetNewSessionAction);
    Menu->addAction(PasteUrlAction);
    Menu->addAction(GenerateUrlAction2);
    Menu->addSeparator();
    Menu->addAction(SetDefaultSessionAction);
    Menu->addSeparator();
    Menu->addAction(NewSessionFolderAction);
    AddSearch();
  }
  else if (IsSiteNode(Node))
  {
    // ManageSitePopupMenu
    Menu->addSection(QStringLiteral("Site"));
    Menu->addAction(LoginAction);
    Menu->addAction(PuttyAction);
    Menu->addSeparator();
    Menu->addAction(EditSessionAction);
    Menu->addAction(DeleteSessionAction);
    Menu->addAction(RenameSessionAction);
    Menu->addAction(CloneToNewSiteAction);
    Menu->addAction(GenerateUrlAction2);
    Menu->addSeparator();
    Menu->addAction(SetDefaultSessionAction);
    Menu->addSeparator();
    Menu->addAction(NewSessionFolderAction);
    if (ShareWithTeamAction->isVisible() || StopSharingAction->isVisible())
    {
      Menu->addSection(QStringLiteral("Team"));
      Menu->addAction(ShareWithTeamAction);
      Menu->addAction(StopSharingAction);
    }
    Menu->addSection(QStringLiteral("Site Shell Icon"));
    Menu->addAction(DesktopIconAction);
    AddSearch();
    // SiteLoginMenuItem / SiteClonetoNewSiteMenuItem default
    Menu->setDefaultAction(IsCloneToNewSiteDefault() ? CloneToNewSiteAction : LoginAction);
  }
  else if (IsFolderNode(Node))
  {
    // ManageFolderPopupMenu
    Menu->addSection(QStringLiteral("Site Folder"));
    Menu->addAction(LoginAction);
    Menu->addAction(PuttyAction);
    Menu->addSeparator();
    Menu->addAction(DeleteSessionAction);
    Menu->addAction(RenameSessionAction);
    Menu->addSeparator();
    Menu->addAction(NewSessionFolderAction);
    if (ShareWithTeamAction->isVisible() || StopSharingAction->isVisible())
    {
      Menu->addSection(QStringLiteral("Team"));
      Menu->addAction(ShareWithTeamAction);
      Menu->addAction(StopSharingAction);
    }
    Menu->addSection(QStringLiteral("Site Folder Shell Icon"));
    Menu->addAction(DesktopIconAction);
    AddSearch();
  }
  else if (IsWorkspaceNode(Node))
  {
    // ManageWorkspacePopupMenu
    Menu->addSection(QStringLiteral("Workspace"));
    Menu->addAction(LoginAction);
    Menu->addSeparator();
    Menu->addAction(DeleteSessionAction);
    Menu->addAction(RenameSessionAction);
    Menu->addSection(QStringLiteral("Workspace Shell Icon"));
    Menu->addAction(DesktopIconAction);
    AddSearch();
  }
  return Menu;
}

//---------------------------------------------------------------------------
// Incremental site search

void LoginDialog::ResetSitesIncrementalSearch()
{
  FSearching = false;
  FSearchText.clear();
  FSearchHaveNext = false;
  UpdateControls();
}

QTreeWidgetItem * LoginDialog::GetNextNode(QTreeWidgetItem * Node, bool Reverse) const
{
  QList<QTreeWidgetItem *> Nodes = AllNodes();
  if (Nodes.isEmpty())
  {
    return nullptr;
  }
  qsizetype Index = Nodes.indexOf(Node);
  if (Reverse)
  {
    Index = (Index <= 0) ? (Nodes.size() - 1) : (Index - 1);
  }
  else
  {
    Index = ((Index < 0) || (Index + 1 >= Nodes.size())) ? 0 : (Index + 1);
  }
  return Nodes[Index];
}

QTreeWidgetItem * LoginDialog::SearchSite(const QString & Text, bool AllowExpanding, bool SkipCurrent, bool Reverse) const
{
  QTreeWidgetItem * CurrentNode = (Selected() != nullptr) ? Selected() : NewSiteNode();
  if (CurrentNode == nullptr)
  {
    return nullptr;
  }
  QTreeWidgetItem * Node = CurrentNode;
  if (SkipCurrent)
  {
    Node = GetNextNode(Node, Reverse);
  }
  while (Node != nullptr)
  {
    bool Eligible = true;
    for (QTreeWidgetItem * Parent = Node->parent(); Eligible && (Parent != nullptr); Parent = Parent->parent())
    {
      Eligible = IsFolderNode(Parent) && (Parent->isExpanded() || AllowExpanding);
    }
    if (Eligible)
    {
      bool Matches = false;
      switch (FSiteSearch)
      {
        case isNameStartOnly:
          Matches = ContainsTextSemiCaseSensitive(Node->text(0).left(Text.size()), Text);
          break;
        case isName:
          Matches = ContainsTextSemiCaseSensitive(Node->text(0), Text);
          break;
        case isAll:
          Matches = ContainsTextSemiCaseSensitive(Node->text(0), Text);
          if (!Matches)
          {
            if (std::shared_ptr<SessionData> Data = NodeSession(Node))
            {
              Matches =
                ContainsTextSemiCaseSensitive(Data->HostName, Text) ||
                ContainsTextSemiCaseSensitive(Data->UserName, Text) ||
                ContainsTextSemiCaseSensitive(Data->Note, Text);
            }
          }
          break;
      }
      if (Matches)
      {
        return Node;
      }
    }
    Node = GetNextNode(Node, Reverse);
    if (Node == CurrentNode)
    {
      if (SkipCurrent)
      {
        // the current node is a candidate too (single match)
        break;
      }
      return nullptr;
    }
  }
  return nullptr;
}

bool LoginDialog::SitesIncrementalSearch(const QString & Text, bool SkipCurrent, bool Reverse, bool Expanding)
{
  QTreeWidgetItem * Node = nullptr;
  if (!Expanding)
  {
    Node = SearchSite(Text, false, SkipCurrent, Reverse);
  }
  if (Node == nullptr)
  {
    Node = SearchSite(Text, true, SkipCurrent, Reverse);
    if (Node != nullptr)
    {
      for (QTreeWidgetItem * Parent = Node->parent(); Parent != nullptr; Parent = Parent->parent())
      {
        Parent->setExpanded(true);
      }
    }
  }
  bool Result = (Node != nullptr);
  if (Result)
  {
    FIncrementalSearching++;
    SetSelected(Node);
    FIncrementalSearching--;
    // the selection may have been refused (editing)
    if (Selected() != Node)
    {
      return false;
    }
    FSearching = true;
    FSearchText = Text;
    // Tab always searches even in collapsed nodes
    QTreeWidgetItem * NextNode = SearchSite(Text, true, true, Reverse);
    FSearchHaveNext = (NextNode != nullptr) && (NextNode != Node);
    UpdateControls();
    SessionTree->scrollToItem(Node);
  }
  return Result;
}

void LoginDialog::SetSiteSearch(int SiteSearch)
{
  if (FSiteSearch != SiteSearch)
  {
    FSiteSearch = SiteSearch;
    if (!FSearchText.isEmpty() && !SitesIncrementalSearch(FSearchText, false, false, false))
    {
      ResetSitesIncrementalSearch();
    }
  }
  UpdateActions();
}

void LoginDialog::SearchSiteStartActionExecute()
{
  if (!SearchSiteStartAction->isEnabled())
  {
    return;
  }
  SessionTree->setFocus();
  if (!FSearching)
  {
    FSearching = true;
    FSearchHaveNext = false;
    FSearchText.clear();
    UpdateControls();
  }
}

bool LoginDialog::SessionTreeKeyPress(QKeyEvent * Event)
{
  if (FRenaming || SessionTree->IsEditing())
  {
    return false;
  }
  int Key = Event->key();
  Qt::KeyboardModifiers Modifiers = Event->modifiers() & ~(Qt::KeypadModifier | Qt::ShiftModifier);
  if ((Key == Qt::Key_Delete) && (Modifiers == Qt::NoModifier))
  {
    if (DeleteSessionAction->isEnabled())
    {
      DeleteSessionActionExecute();
    }
    return true;
  }
  if ((Key == Qt::Key_F2) && (Modifiers == Qt::NoModifier))
  {
    RenameSessionActionExecute();
    return true;
  }
  if ((Key == Qt::Key_Tab) || (Key == Qt::Key_Backtab))
  {
    if (!FSearchText.isEmpty())
    {
      bool Reverse = (Key == Qt::Key_Backtab) || (Event->modifiers() & Qt::ShiftModifier);
      if (!SitesIncrementalSearch(FSearchText, true, Reverse, true))
      {
        QApplication::beep();
      }
      return true;
    }
    return false;
  }
  if (Key == Qt::Key_Escape)
  {
    if (FSearching)
    {
      ResetSitesIncrementalSearch();
      return true;
    }
    return false;
  }
  if (Key == Qt::Key_Backspace)
  {
    if (FSearching)
    {
      if (FSearchText.size() <= 1)
      {
        ResetSitesIncrementalSearch();
      }
      else
      {
        SitesIncrementalSearch(FSearchText.left(FSearchText.size() - 1), false, false, false);
      }
    }
    return true;
  }
  if ((Key == Qt::Key_Return) || (Key == Qt::Key_Enter))
  {
    if (FEditing)
    {
      // SaveButton is the default button while editing
      SaveSessionActionExecute();
    }
    else if (IsCloneToNewSiteDefault())
    {
      CloneToNewSite();
    }
    else if (CanOpen())
    {
      LoginActionExecute();
    }
    return true;
  }
  QString Text = Event->text();
  if ((Modifiers == Qt::NoModifier) && !Text.isEmpty() && (Text[0] >= QLatin1Char(' ')) && Text[0].isPrint())
  {
    if (!SitesIncrementalSearch(FSearchText + Text, false, false, false))
    {
      QApplication::beep();
    }
    return true;
  }
  return false;
}

bool LoginDialog::eventFilter(QObject * Object, QEvent * Event)
{
  if (Object == SessionTree)
  {
    if (Event->type() == QEvent::KeyPress)
    {
      if (SessionTreeKeyPress(static_cast<QKeyEvent *>(Event)))
      {
        return true;
      }
    }
    else if (Event->type() == QEvent::ShortcutOverride)
    {
      // keep Tab/Esc/Backspace for the incremental search
      auto * KeyEvent = static_cast<QKeyEvent *>(Event);
      if (FSearching && ((KeyEvent->key() == Qt::Key_Escape) || (KeyEvent->key() == Qt::Key_Tab) || (KeyEvent->key() == Qt::Key_Backtab)))
      {
        Event->accept();
        return true;
      }
    }
    else if (Event->type() == QEvent::FocusOut)
    {
      // SessionTreeExit
      if (FSearching && !SitesIncrementalSearchLabel->underMouse())
      {
        ResetSitesIncrementalSearch();
      }
    }
  }
  return QDialog::eventFilter(Object, Event);
}

void LoginDialog::keyPressEvent(QKeyEvent * Event)
{
  if (FRenaming || SessionTree->IsEditing())
  {
    // Enter/Esc of the in-place rename editor
    Event->accept();
    return;
  }
  if ((Event->key() == Qt::Key_Return) || (Event->key() == Qt::Key_Enter))
  {
    if (FEditing)
    {
      // SaveButton is the default button while editing
      SaveSessionActionExecute();
    }
    else if (LoginAction->isEnabled())
    {
      LoginActionExecute();
    }
    return;
  }
  if (Event->key() == Qt::Key_Escape)
  {
    if (FSearching)
    {
      ResetSitesIncrementalSearch();
    }
    else if (FEditing)
    {
      // EditCancelButton is the cancel button while editing
      EditCancelActionExecute();
    }
    else
    {
      reject();
    }
    return;
  }
  QDialog::keyPressEvent(Event);
}

//---------------------------------------------------------------------------
// Login

void LoginDialog::LoginActionExecute()
{
  UpdateActions();
  if (!LoginAction->isEnabled())
  {
    return;
  }
  if (OpenInNewWindow() && !IsNewSiteNode(Selected()))
  {
    ExecuteNewInstance(SessionNodePath(Selected()));
    return;
  }
  accept();
}

void LoginDialog::SaveDataList()
{
  // SaveDataList / CloneSelectedSession
  FSessionsToOpen.clear();
  FWorkspaceToOpen.clear();
  QTreeWidgetItem * Node = Selected();
  if (IsFolderOrWorkspaceNode(Node))
  {
    QString Name = SessionNodePath(Node);
    FWorkspaceToOpen = Name;
    for (SessionData Data : Workspaces::FolderOrWorkspace(FSessions, Name))
    {
      Data.IsWorkspace = false;
      FSessionsToOpen.append(Data);
    }
  }
  else
  {
    ParseHostName();
    SessionData Data;
    if (std::shared_ptr<SessionData> Site = SelectedSession())
    {
      Data = *Site;
    }
    else
    {
      SaveSession(FNewSiteData);
      Data = FNewSiteData;
      // ad-hoc session: the name of an edited stored site does not leave the dialog
      Data.Name.clear();
    }
    FSessionsToOpen.append(Data);
  }
  for (SessionData & Data : FSessionsToOpen)
  {
    Data.ExpandEnvironmentVariables();
  }
}

void LoginDialog::SaveConfiguration()
{
  if (IsWorkspaceNode(Selected()))
  {
    Workspaces::SetLastWorkspace(SessionNodePath(Selected()));
  }
}

void LoginDialog::done(int Result)
{
  // FormCloseQuery
  if (!EnsureNotEditing())
  {
    return;
  }
  if (Result == QDialog::Accepted)
  {
    if (!CanOpen())
    {
      return;
    }
    SaveDataList();
    SaveConfiguration();
  }
  else if (IsNewSiteNode(Selected()))
  {
    PersistNewSiteIfNeeded();
  }
  Configuration & Config = Configuration::Instance();
  SaveState();
  if (!ShowAgainCheck->isChecked())
  {
    Config.ShowLoginOnStartup = false;
  }
  Config.Save();
  QDialog::done(Result);
}

//---------------------------------------------------------------------------
// Import

bool LoginDialog::ImportSites(StoredSessionList & Sessions, QWidget * Parent, QStringList * Imported)
{
  // DoImportSessionsDialog
  ImportSessionsDialog Dialog(Sessions, Parent);
  Dialog.Init();
  if (Dialog.exec() != QDialog::Accepted)
  {
    return false;
  }
  QList<std::shared_ptr<SessionData>> Sites = Dialog.Import();
  if (Imported != nullptr)
  {
    for (const auto & Site : Sites)
    {
      Imported->append(Site->Name);
    }
  }
  Configuration::Instance().Save();
  return true;
}

void LoginDialog::ImportSitesIfAny(StoredSessionList & Sessions, QWidget * Parent)
{
  // ImportSitesIfAny (WinMain.cpp), OpenSSH added as the common Linux source
  Configuration & Config = Configuration::Instance();
  if (ReadInterfaceString(QStringLiteral("AutoImportedFromPuttyOrFilezilla"), QStringLiteral("0")).toInt() != 0)
  {
    return;
  }
  QStringList Sources;
  if (SiteImport::AnyPuttySessionForImport(Sessions))
  {
    Sources.append(QStringLiteral("PuTTY SSH client"));
  }
  if (SiteImport::AnyFilezillaSessionForImport(Sessions))
  {
    Sources.append(QStringLiteral("FileZilla FTP client"));
  }
  if (SiteImport::AnyOpensshSessionForImport(Sessions))
  {
    Sources.append(QStringLiteral("OpenSSH configuration"));
  }
  if (Sources.isEmpty())
  {
    return;
  }
  QString Source = Sources.last();
  if (Sources.size() > 1)
  {
    // "%s and %s"
    Source = QStringLiteral("%1 and %2").arg(QStringList(Sources.mid(0, Sources.size() - 1)).join(QStringLiteral(", ")), Sources.last());
  }
  {
    IniStorage Storage(&Config.Ini());
    if (Storage.OpenSubKey(ConfigurationKey, true) && Storage.OpenSubKey(InterfaceKey, true))
    {
      Storage.WriteBool(QStringLiteral("AutoImportedFromPuttyOrFilezilla"), true);
    }
  }
  Config.Save();
  // IMPORT_SESSIONS2
  if (QMessageBox::question(Parent, QStringLiteral("Confirm"),
        QStringLiteral("<b>You have stored sessions/sites in %1.<br><br>Do you want to import them into NixSCP?</b><br><br>"
          "(You can import them anytime later from Login dialog)").arg(Source.toHtmlEscaped()),
        QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes)
  {
    ImportSites(Sessions, Parent);
  }
}

//---------------------------------------------------------------------------
// ImportSessionsDialog

ImportSessionsDialog::ImportSessionsDialog(StoredSessionList & Sessions, QWidget * Parent) :
  QDialog(Parent),
  FSessions(Sessions)
{
  // IMPORT_CAPTION
  setWindowTitle(QStringLiteral("Import sites"));
  resize(418, 307);
  auto * Layout = new QVBoxLayout(this);
  auto * SourceLayout = new QHBoxLayout();
  auto * Label = new QLabel(QStringLiteral("&Import from:"));
  SourceComboBox = new QComboBox();
  // KiTTY is Windows only
  SourceComboBox->addItems({QStringLiteral("PuTTY"), QStringLiteral("FileZilla"), QStringLiteral("OpenSSH"), QStringLiteral("INI file"), QStringLiteral("known_hosts")});
  Label->setBuddy(SourceComboBox);
  BrowseButton = new QPushButton(QStringLiteral("B&rowse..."));
  PasteButton = new QPushButton(QStringLiteral("&Paste"));
  SourceLayout->addWidget(Label);
  SourceLayout->addWidget(SourceComboBox);
  SourceLayout->addWidget(BrowseButton);
  SourceLayout->addWidget(PasteButton);
  SourceLayout->addStretch();
  Layout->addLayout(SourceLayout);

  ListStack = new QStackedWidget();
  auto * Stack = ListStack;
  SessionListView = new QListWidget();
  SessionListView->setObjectName(QStringLiteral("SessionListView"));
  ErrorLabel = new QLabel();
  ErrorLabel->setAlignment(Qt::AlignCenter);
  ErrorLabel->setWordWrap(true);
  ErrorLabel->setAutoFillBackground(true);
  ErrorLabel->setBackgroundRole(QPalette::Base);
  ErrorLabel->setFrameShape(QFrame::StyledPanel);
  ErrorLabel->setTextFormat(Qt::PlainText);
  Stack->addWidget(SessionListView);
  Stack->addWidget(ErrorLabel);
  Layout->addWidget(Stack, 1);

  CheckAllButton = new QPushButton(QStringLiteral("Un/check &all"));
  auto * Buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  OKButton = Buttons->button(QDialogButtonBox::Ok);
  HelpSystem::AddHelpButton(Buttons, this, QStringLiteral("ui_import"));
  auto * ButtonsLayout = new QHBoxLayout();
  ButtonsLayout->addWidget(CheckAllButton);
  ButtonsLayout->addStretch();
  ButtonsLayout->addWidget(Buttons);
  Layout->addLayout(ButtonsLayout);

  connect(Buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(Buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(SourceComboBox, &QComboBox::activated, this, [this]()
  {
    // SourceComboBoxSelect
    LoadSessions();
  });
  connect(BrowseButton, &QPushButton::clicked, this, [this]() { BrowseButtonClick(); });
  connect(PasteButton, &QPushButton::clicked, this, [this]() { PasteButtonClick(); });
  connect(CheckAllButton, &QPushButton::clicked, this, [this]() { CheckAllButtonClick(); });
  connect(SessionListView, &QListWidget::itemChanged, this, [this]() { SaveSelection(); UpdateControls(); });
  connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, [this]() { UpdateControls(); });
  connect(Stack, &QStackedWidget::currentChanged, this, [this]() { UpdateControls(); });
}

ImportSessionsDialog::~ImportSessionsDialog()
{
}

void ImportSessionsDialog::Init()
{
  FLists.clear();
  for (int Index = 0; Index < SiteImport::isCount; Index++)
  {
    FLists.append(ImportSessionList());
  }
  FLists[SiteImport::isPutty] = SiteImport::SelectPuttySessionsForImport(FSessions);
  FLists[SiteImport::isFilezilla] = SiteImport::SelectFilezillaSessionsForImport(FSessions);
  FLists[SiteImport::isOpenssh] = SiteImport::SelectOpensshSessionsForImport(FSessions);
  FLists[SiteImport::isIni] = SiteImport::SelectSessionsForImport(FSessions, FIniFileName);
  FLists[SiteImport::isKnownHosts] = SiteImport::SelectKnownHostsSessionsForImportFromFile(FSessions);
  int Source = -1;
  for (int Index = 0; Index < FLists.size(); Index++)
  {
    if ((Source < 0) && (FLists[Index].Count() > 0))
    {
      Source = Index;
    }
  }
  SourceComboBox->setCurrentIndex((Source < 0) ? 0 : Source);
  LoadSessions();
}

int ImportSessionsDialog::SourceIndex() const
{
  return SourceComboBox->currentIndex();
}

void ImportSessionsDialog::SetSourceIndex(int Index)
{
  SourceComboBox->setCurrentIndex(Index);
  LoadSessions();
}

void ImportSessionsDialog::SetIniFileName(const QString & FileName)
{
  FIniFileName = FileName;
  if (FLists.size() > SiteImport::isIni)
  {
    FLists[SiteImport::isIni] = SiteImport::SelectSessionsForImport(FSessions, FIniFileName);
    if (SourceComboBox->currentIndex() == SiteImport::isIni)
    {
      LoadSessions();
    }
  }
}

void ImportSessionsDialog::LoadSessions()
{
  int Source = SourceComboBox->currentIndex();
  if ((Source < 0) || (Source >= FLists.size()))
  {
    return;
  }
  const ImportSessionList & List = FLists[Source];
  SessionListView->blockSignals(true);
  SessionListView->clear();
  for (int Index = 0; Index < List.Count(); Index++)
  {
    const SessionData & Data = *List.Sessions[Index];
    auto * Item = new QListWidgetItem(Data.Name, SessionListView);
    Item->setFlags((Item->flags() | Qt::ItemIsUserCheckable) & ~Qt::ItemIsEditable);
    Item->setCheckState(List.Selected[Index] ? Qt::Checked : Qt::Unchecked);
    // SessionListView2InfoTip (IMPORT_KNOWNHOSTS_INFO_TIP)
    Item->setToolTip((Source == SiteImport::isKnownHosts) ?
      QStringLiteral("Host: %1\nHost key: %2").arg(Data.HostName, Data.HostKey) : Data.InfoTip());
  }
  SessionListView->blockSignals(false);
  QStackedWidget * Stack = ListStack;
  if ((List.Count() > 0) || List.Error.isEmpty())
  {
    Stack->setCurrentWidget(SessionListView);
  }
  else
  {
    ErrorLabel->setText(List.Error);
    Stack->setCurrentWidget(ErrorLabel);
  }
  UpdateControls();
}

void ImportSessionsDialog::SaveSelection()
{
  int Source = SourceComboBox->currentIndex();
  if ((Source < 0) || (Source >= FLists.size()))
  {
    return;
  }
  ImportSessionList & List = FLists[Source];
  for (int Index = 0; (Index < SessionListView->count()) && (Index < List.Count()); Index++)
  {
    List.Selected[Index] = (SessionListView->item(Index)->checkState() == Qt::Checked);
  }
}

void ImportSessionsDialog::UpdateControls()
{
  int Source = SourceComboBox->currentIndex();
  BrowseButton->setVisible(Source == SiteImport::isIni);
  PasteButton->setVisible(Source == SiteImport::isKnownHosts);
  PasteButton->setEnabled(!QGuiApplication::clipboard()->text().trimmed().isEmpty());
  bool AnyChecked = false;
  for (int Index = 0; Index < SessionListView->count(); Index++)
  {
    AnyChecked = AnyChecked || (SessionListView->item(Index)->checkState() == Qt::Checked);
  }
  OKButton->setEnabled(AnyChecked);
  CheckAllButton->setEnabled(SessionListView->count() > 0);
}

void ImportSessionsDialog::CheckAllButtonClick()
{
  // ListViewCheckAll(caToggle)
  bool AllChecked = true;
  for (int Index = 0; Index < SessionListView->count(); Index++)
  {
    AllChecked = AllChecked && (SessionListView->item(Index)->checkState() == Qt::Checked);
  }
  SessionListView->blockSignals(true);
  for (int Index = 0; Index < SessionListView->count(); Index++)
  {
    SessionListView->item(Index)->setCheckState(AllChecked ? Qt::Unchecked : Qt::Checked);
  }
  SessionListView->blockSignals(false);
  SaveSelection();
  UpdateControls();
}

void ImportSessionsDialog::BrowseButtonClick()
{
  // IMPORT_INI_TITLE, EXPORT_CONF_FILTER (+ registry exports of WinSCP)
  QString FileName = QFileDialog::getOpenFileName(this, QStringLiteral("Select file to import sites from"),
    FIniFileName.isEmpty() ? Configuration::Instance().DefaultIniFileExportPath() : FIniFileName,
    QStringLiteral("INI files (*.ini);;Registry exports (*.reg);;All files (*)"));
  if (!FileName.isEmpty())
  {
    SetIniFileName(FileName);
  }
}

void ImportSessionsDialog::PasteButtonClick()
{
  // Proceed even when retrieving from clipboard fails, "no host keys" error will show
  QStringList Lines = QGuiApplication::clipboard()->text().split(QLatin1Char('\n'));
  for (QString & Line : Lines)
  {
    Line = Line.trimmed();
  }
  FLists[SiteImport::isKnownHosts] = SiteImport::SelectKnownHostsSessionsForImport(FSessions, Lines);
  LoadSessions();
}

QList<std::shared_ptr<SessionData>> ImportSessionsDialog::Import()
{
  // TImportSessionsDialog::Execute + DoImportSessionsDialog: only the
  // sites checked in the current source are imported
  SaveSelection();
  int Source = SourceComboBox->currentIndex();
  QList<std::shared_ptr<SessionData>> Result;
  if ((Source < 0) || (Source >= FLists.size()))
  {
    return Result;
  }
  ImportSessionList & List = FLists[Source];
  if (Source == SiteImport::isKnownHosts)
  {
    SiteImport::ImportSelectedKnownHosts(List);
    return Result;
  }
  QList<std::shared_ptr<SessionData>> Selected = List.SelectedSessions();
  // Windows paths (C:\...) in imported sites
  bool AnyWindowsPaths = false;
  for (const auto & Data : Selected)
  {
    AnyWindowsPaths = AnyWindowsPaths || SiteImport::HasWindowsPaths(*Data);
  }
  if (AnyWindowsPaths &&
      (QMessageBox::question(this, QStringLiteral("Confirm"),
         QStringLiteral("Some of the selected sites refer to Windows paths (like C:\\...) of private key files or local directories.\n\n"
           "Do you want to map them to your home folder? Paths that cannot be mapped will be cleared."),
         QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes))
  {
    for (const auto & Data : Selected)
    {
      SiteImport::RemapWindowsPaths(*Data);
    }
  }
  Result = FSessions.Import(Selected);
  switch (Source)
  {
    case SiteImport::isPutty:
    case SiteImport::isFilezilla:
      // FileZilla uses PuTTY's host key store
      SiteImport::ImportHostKeys(SiteImport::LoadPuttyHostKeys(), Selected);
      break;
    case SiteImport::isIni:
      SiteImport::ImportHostKeys(SiteImport::LoadIniHostKeys(FIniFileName), Selected);
      break;
    case SiteImport::isOpenssh:
    {
      // keys of the imported hosts from known_hosts (no-op for NixSCP's own file)
      ImportSessionList & KnownHosts = FLists[SiteImport::isKnownHosts];
      for (int Index = 0; Index < KnownHosts.Count(); Index++)
      {
        KnownHosts.Selected[Index] = false;
      }
      SiteImport::SelectKnownHostsForSelectedSessions(KnownHosts, List);
      SiteImport::ImportSelectedKnownHosts(KnownHosts);
      break;
    }
    default:
      break;
  }
  return Result;
}
