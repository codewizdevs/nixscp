// Commander window tests (WP02): src/gui/CommanderWindow + CommanderActions
// (ports of WinSCP forms/ScpCommander, forms/NonVisual, forms/CustomScpExplorer):
// menu structure of ScpCommander.dfm, captions, 'Short|Long' hints, Commander
// keyboard shortcuts (no key bound twice for the active side), local-local
// tabs (New Local Tab, F5 copy between two local panels), the items that
// follow the active side (Upload/Download, Hot Keys), tab commands
// (Duplicate, Rename, Alt+N), file and directory popups, and with the test
// SSH server: secure connection status, Download captions, synchronized
// browsing, Clear Caches, the remote file popup.
//
// GUI test: built with NIXSCP_GUI_TESTS and the GUI objects, runs offscreen.
// The remote part needs NIXSCP_TEST_SSH_PORT and NIXSCP_TEST_SSH_KEY
// (NIXSCP_TEST_SSH_HOST, NIXSCP_TEST_SSH_USER optional).
// NIXSCP_TEST_SCREENSHOT=<dir> saves screenshots of the window.
#ifndef NIXSCP_GUI_TESTS

#include <cstdio>

int main()
{
  std::printf("SKIP: test_commander needs NIXSCP_GUI_TESTS (Qt Widgets + the GUI sources)\n");
  return 0;
}

#else

#include "core/Configuration.h"
#include "core/IniStorage.h"
#include "core/Terminal.h"
#include "core/Workspaces.h"
#include "gui/CommanderActions.h"
#include "gui/CommanderWindow.h"
#include "gui/EditorManager.h"
#include "gui/FileOperations.h"
#include "gui/FilePanel.h"
#include "gui/PreferencesDialog.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMimeData>
#include <QMouseEvent>
#include <QPushButton>
#include <QStandardPaths>
#include <QSplitter>
#include <QStatusBar>
#include <QTabBar>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeView>

#include <cstdio>
#include <functional>

static int Failures = 0;
static int Checks = 0;
static QStringList Warnings;

#define CHECK(Cond) \
  do { Checks++; if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); std::fflush(stdout); Failures++; } } while (0)
#define CHECK_MSG(Cond, Msg) \
  do { Checks++; if (!(Cond)) { std::printf("FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, #Cond, qPrintable(Msg)); std::fflush(stdout); Failures++; } } while (0)

static bool WaitFor(const std::function<bool()> & Condition, int Ms = 10000)
{
  QElapsedTimer Timer;
  Timer.start();
  while (!Condition())
  {
    if (Timer.elapsed() > Ms) return false;
    QApplication::processEvents(QEventLoop::AllEvents, 50);
    QThread::msleep(10);
  }
  return true;
}

static void MessageHandler(QtMsgType Type, const QMessageLogContext &, const QString & Message)
{
  if ((Type == QtWarningMsg) && (Message.contains(QStringLiteral("CommanderActions")) ||
        Message.contains(QStringLiteral("Ambiguous shortcut"))))
  {
    Warnings.append(Message);
  }
  std::fprintf(stderr, "%s\n", qPrintable(Message));
}

static bool StartsWith(const QStringList & List, const QStringList & Prefix)
{
  return (List.size() >= Prefix.size()) && (List.mid(0, Prefix.size()) == Prefix);
}

static QStringList MenuTitles(QMenuBar * Bar)
{
  QStringList Result;
  for (QAction * Action : Bar->actions())
  {
    Result.append(Action->text());
  }
  return Result;
}

static QMenu * TopMenu(QMenuBar * Bar, const QString & Title)
{
  for (QAction * Action : Bar->actions())
  {
    if (Action->text() == Title) return Action->menu();
  }
  return nullptr;
}

static QStringList MenuItems(QMenu * Menu)
{
  QStringList Result;
  if (Menu == nullptr) return Result;
  emit Menu->aboutToShow();
  for (QAction * Action : Menu->actions())
  {
    if (Action->isSeparator()) Result.append(QStringLiteral("-"));
    else if (Action->isVisible()) Result.append(Action->text());
  }
  return Result;
}

static QMenu * SubMenu(QMenu * Menu, const QString & Title)
{
  for (QAction * Action : Menu->actions())
  {
    if ((Action->text() == Title) && (Action->menu() != nullptr)) return Action->menu();
  }
  return nullptr;
}

// Window-active shortcuts bound to more than one enabled action
static QStringList DuplicateShortcuts(QWidget * Window)
{
  QHash<QString, QStringList> Keys;
  QList<QAction *> All = Window->findChildren<QAction *>();
  for (QAction * Action : Window->actions())
  {
    if (!All.contains(Action)) All.append(Action);
  }
  for (QAction * Action : All)
  {
    if ((Action->shortcutContext() != Qt::WindowShortcut) || !Action->isEnabled() || !Action->isVisible()) continue;
    bool Associated = false;
    for (QObject * Object : Action->associatedObjects())
    {
      if (qobject_cast<QWidget *>(Object) == Window) Associated = true;
      else if (auto * Menu = qobject_cast<QMenu *>(Object))
      {
        Q_UNUSED(Menu);
        Associated = true;
      }
    }
    if (!Associated) continue;
    for (const QKeySequence & Key : Action->shortcuts())
    {
      if (!Key.isEmpty()) Keys[Key.toString()].append(Action->objectName().isEmpty() ? Action->text() : Action->objectName());
    }
  }
  QStringList Result;
  for (auto It = Keys.cbegin(); It != Keys.cend(); ++It)
  {
    QStringList Names = It.value();
    Names.removeDuplicates();
    if (Names.size() > 1) Result.append(It.key() + QStringLiteral(": ") + Names.join(QStringLiteral(", ")));
  }
  return Result;
}

// Shows a popup by emitting the panel signal, returns its visible item captions
static QStringList PopupItems(const std::function<void()> & Show, QStringList * SubItems = nullptr, const QString & Sub = QString())
{
  QStringList Result;
  QTimer::singleShot(150, [&Result, SubItems, Sub]()
  {
    auto * Menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
    if (Menu == nullptr) return;
    Result = MenuItems(Menu);
    for (QAction * Action : Menu->actions())
    {
      if ((Action->menu() != nullptr) && (Action->text() == Sub) && (SubItems != nullptr))
      {
        *SubItems = MenuItems(Action->menu());
      }
      if (Action->isVisible() && Action->font().bold())
      {
        Result.append(QStringLiteral("BOLD:") + Action->text());
      }
    }
    Menu->close();
  });
  Show();
  return Result;
}

static void Screenshot(QWidget * Window, const QString & Name)
{
  QString Dir = qEnvironmentVariable("NIXSCP_TEST_SCREENSHOT");
  if (!Dir.isEmpty())
  {
    QDir().mkpath(Dir);
    Window->grab().save(QDir(Dir).filePath(Name + QStringLiteral(".png")));
  }
}

static FilePanel * Panel(CommanderWindow & Window, bool Right)
{
  // Panels of the current tab (visible page): the splitter's widgets in order
  for (QSplitter * Splitter : Window.findChildren<QSplitter *>())
  {
    if (!Splitter->isVisibleTo(&Window) || (Splitter->count() != 2)) continue;
    auto * Left = qobject_cast<FilePanel *>(Splitter->widget(0));
    auto * RightPanel = qobject_cast<FilePanel *>(Splitter->widget(1));
    if ((Left != nullptr) && (RightPanel != nullptr)) return Right ? RightPanel : Left;
  }
  return nullptr;
}

static void TestStructure(CommanderWindow & Window)
{
  std::printf("[structure]\n");
  CommanderActions & A = *Window.Actions();
  QMenuBar * Bar = Window.menuBar();
  // ScpCommander.dfm MenuToolbar; a local browser tab says Left/Right.
  // NixSCP Pro adds its menu before Help ("Cloud Sync" when signed out).
  QStringList Titles = MenuTitles(Bar);
  CHECK_MSG(Titles == QStringList({"&Left", "&Mark", "&Files", "&Commands", "&Tabs", "&Options", "&Right", "T&unnels", "Tea&ms", "Cloud Sync", "&Help"}),
    Titles.join(QStringLiteral("|")));
  QStringList Files = MenuItems(TopMenu(Bar, QStringLiteral("&Files")));
  CHECK_MSG(StartsWith(Files, QStringList({"&New", "-", "&Open", "&Edit", "Edit &Link...", "-"})), Files.join(QStringLiteral("|")));
  CHECK_MSG(Files.contains(QStringLiteral("&Copy to Clipboard")) && Files.contains(QStringLiteral("&Paste from Clipboard")) &&
    Files.contains(QStringLiteral("&Calculate Directory Sizes")) && Files.contains(QStringLiteral("&Properties")), Files.join(QStringLiteral("|")));
  QStringList New = MenuItems(SubMenu(TopMenu(Bar, QStringLiteral("&Files")), QStringLiteral("&New")));
  CHECK_MSG(New == QStringList({"&File...", "&Directory...", "&Link..."}), New.join(QStringLiteral("|")));
  QStringList Commands = MenuItems(TopMenu(Bar, QStringLiteral("&Commands")));
  CHECK_MSG(Commands == QStringList({"&Compare Directories", "&Keep Remote Directory up to Date...", "&Synchronize...",
    "Synchronize &Browsing", "&Find Files...", "Q&ueue", "Static &Custom Commands", "-", "Open &Terminal", "Open in &PuTTY",
    "-", "A&gent Access Log...", "Agent &Sessions...", "Agent Access &Help", "-", "Clea&r Caches", "-", "&Quit"}), Commands.join(QStringLiteral("|")));
  QStringList Tabs = MenuItems(TopMenu(Bar, QStringLiteral("&Tabs")));
  CHECK_MSG(StartsWith(Tabs, QStringList({"&New Tab", "&Close Tab", "Du&plicate Tab", "&Rename Tab", "-", "C&olor", "-",
    "&Disconnect Session", "&Reconnect Session", "&Save Session as Site...", "-", "Server/Protocol &Information",
    "Generate Session &URL/Code..."})), Tabs.join(QStringLiteral("|")));
  QStringList NewTab = MenuItems(SubMenu(TopMenu(Bar, QStringLiteral("&Tabs")), QStringLiteral("&New Tab")));
  CHECK_MSG(NewTab == QStringList({"&Remote Tab", "&Local Tab", "-", "&Default to Remote Tab"}), NewTab.join(QStringLiteral("|")));
  QStringList Mark = MenuItems(TopMenu(Bar, QStringLiteral("&Mark")));
  CHECK_MSG(Mark.first() == QStringLiteral("&Select/Unselect") && Mark.contains(QStringLiteral("&Restore Selection")), Mark.join(QStringLiteral("|")));
  QStringList Left = MenuItems(TopMenu(Bar, QStringLiteral("&Left")));
  CHECK_MSG(StartsWith(Left, QStringList({"&Change Drive", "-", "&Go To", "&Refresh", "&Add Path to Bookmarks", "Copy &Path to Clipboard"})),
    Left.join(QStringLiteral("|")));
  QStringList Help = MenuItems(TopMenu(Bar, QStringLiteral("&Help")));
  CHECK_MSG(Help.contains(QStringLiteral("&Contents")) && Help.contains(QStringLiteral("&About...")), Help.join(QStringLiteral("|")));

  // Shortcuts (Commander keyboard: NonVisual.cpp CommanderShortcuts)
  auto Key = [&A](const char * Name) { return A(Name)->shortcut().toString(); };
  CHECK(Key("NewTabAction") == QStringLiteral("Ctrl+T"));
  CHECK(Key("ConsoleAction") == QStringLiteral("Ctrl+Shift+T"));
  CHECK(Key("NewRemoteTabAction") == QStringLiteral("Ctrl+N"));
  CHECK(Key("NewLocalTabAction") == QStringLiteral("Ctrl+Shift+N"));
  CHECK(A("CloseTabAction")->shortcuts().size() == 2);
  CHECK(Key("CloseTabAction") == QStringLiteral("Ctrl+Shift+D"));
  CHECK(Key("GoToAddressAction") == QStringLiteral("Ctrl+L"));
  CHECK(A("GoToAddressAction")->shortcuts().value(1).toString() == QStringLiteral("Alt+D"));
  CHECK(Key("ClearSelectionAction") == QStringLiteral("Ctrl+Shift+L"));
  CHECK(Key("LocalHomeDirAction") == QStringLiteral("Ctrl+H"));
  CHECK(Key("LocalRootDirAction") == QStringLiteral("Ctrl+\\"));
  CHECK(Key("LocalRefreshAction") == QStringLiteral("Ctrl+R"));
  CHECK(Key("CurrentDeleteAlternativeAction") == QStringLiteral("Shift+F8"));
  CHECK(A("CurrentDeleteAlternativeAction")->shortcuts().value(1).toString() == QStringLiteral("Shift+Del"));
  CHECK(Key("NewFileAction") == QStringLiteral("Shift+F4"));
  CHECK(Key("RemoteCopyToAction") == QStringLiteral("Shift+F5"));
  CHECK(Key("RemoteMoveToAction") == QStringLiteral("Shift+F6"));
  CHECK(Key("CurrentAddEditLinkAction") == QStringLiteral("Alt+F6"));
  CHECK(Key("CompareDirectoriesAction2") == QStringLiteral("Shift+F2"));
  CHECK(Key("SynchronizeBrowsingAction2") == QStringLiteral("Ctrl+Alt+B"));
  CHECK(Key("AutoReadDirectoryAfterOpAction") == QStringLiteral("Ctrl+Alt+R"));
  CHECK(Key("CloseApplicationAction2") == QStringLiteral("F10"));
  CHECK(Key("LocalSortByNameAction2") == QStringLiteral("Ctrl+F3"));
  CHECK(Key("RemoteSortByGroupAction2") == QStringLiteral("Ctrl+F9"));
  CHECK(Key("SelectAction") == QStringLiteral("Num++"));
  CHECK(Key("InvertSelectionAction") == QStringLiteral("Num+*"));
  CHECK(Key("CurrentCopyToClipboardAction2") == QStringLiteral("Ctrl+C"));
  CHECK(Key("PasteAction3") == QStringLiteral("Ctrl+V"));
  CHECK(Key("LocalBackAction") == QStringLiteral("Alt+Left"));
  CHECK(A("CalculateDirectorySizesAction")->shortcut() == QKeySequence(QKeyCombination(Qt::AltModifier | Qt::ShiftModifier, Qt::Key_Return)));
  // Hints: 'Short|Long'
  CHECK(A("CompareDirectoriesAction2")->toolTip() == QStringLiteral("Compare directories"));
  CHECK(A("CompareDirectoriesAction2")->statusTip() == QStringLiteral("Mark different files between files panels"));
  CHECK(A("ClearCachesAction")->statusTip() == QStringLiteral("Clear directory listing and directory changes caches"));

  // Toolbars (ScpCommander.dfm TopDock / BottomDock)
  QStringList Bars;
  for (QToolBar * T : Window.findChildren<QToolBar *>())
  {
    Bars.append(T->objectName());
  }
  for (const char * Name : {"CommandsToolbar", "SessionToolbar", "PreferencesToolbar", "SortToolbar", "TransferToolbar",
         "CustomCommandsToolbar", "Toolbar2Toolbar", "LocalFileToolbar", "RemoteFileToolbar", "LocalSelectionToolbar"})
  {
    CHECK_MSG(Bars.contains(QString::fromLatin1(Name)), QString::fromLatin1(Name));
  }
  CHECK(Window.findChild<QToolBar *>(QStringLiteral("CommandsToolbar"))->isVisibleTo(&Window));
  CHECK(!Window.findChild<QToolBar *>(QStringLiteral("SortToolbar"))->isVisibleTo(&Window));
  // Every name used by the menus exists
  CHECK_MSG(Warnings.isEmpty(), Warnings.join(QStringLiteral("; ")));
}

static void TestLocalLocal(CommanderWindow & Window, const QString & Root)
{
  std::printf("[local-local]\n");
  CommanderActions & A = *Window.Actions();
  auto * Tabs = Window.findChild<QTabBar *>(QStringLiteral("SessionsPageControl"));
  CHECK(Tabs != nullptr);
  // The implicit tab is a local browser: two local panels
  FilePanel * L = Panel(Window, false);
  FilePanel * R = Panel(Window, true);
  CHECK((L != nullptr) && (R != nullptr));
  if ((L == nullptr) || (R == nullptr)) return;
  CHECK(!L->IsRemote() && !R->IsRemote());

  QDir(Root).mkpath(QStringLiteral("left/sub"));
  QDir(Root).mkpath(QStringLiteral("right"));
  QFile F(QDir(Root).filePath(QStringLiteral("left/hello.txt")));
  (void)F.open(QIODevice::WriteOnly);
  F.write("hello");
  F.close();
  int Count = Tabs->count();
  A("NewLocalTabAction")->trigger();
  CHECK(Tabs->count() == Count + 1);
  L = Panel(Window, false);
  R = Panel(Window, true);
  L->ChangeDirectory(QDir(Root).filePath(QStringLiteral("left")));
  R->ChangeDirectory(QDir(Root).filePath(QStringLiteral("right")));
  CHECK(WaitFor([&]() { return L->FocusFile(QStringLiteral("hello.txt")) && R->CurrentDirectory().endsWith(QStringLiteral("right")); }));
  L->FocusView();
  L->View()->setFocus();
  emit L->Activated();
  QApplication::processEvents();
  // Local-local mode: Copy/Move instead of Upload/Download, F5/F6
  CHECK(A("LocalLocalCopyAction")->isEnabled());
  CHECK(!A("LocalCopyAction")->isEnabled());
  CHECK(A("LocalLocalCopyAction")->shortcut().toString() == QStringLiteral("F5"));
  CHECK(A("LocalOtherDirAction")->isEnabled());
  QStringList Dups = DuplicateShortcuts(&Window);
  CHECK_MSG(Dups.isEmpty(), Dups.join(QStringLiteral("; ")));
  // Hot Keys captions follow the active side
  QStringList HotKeys;
  for (QToolButton * B : Window.findChild<QToolBar *>(QStringLiteral("Toolbar2Toolbar"))->findChildren<QToolButton *>())
  {
    HotKeys.append(B->text());
  }
  CHECK_MSG(HotKeys.contains(QStringLiteral("F5 Copy")) && HotKeys.contains(QStringLiteral("F6 Move")) &&
    HotKeys.contains(QStringLiteral("F7 Create Directory")) && HotKeys.contains(QStringLiteral("F10 Quit")), HotKeys.join(QStringLiteral("|")));
  // F5 without the dialog (ConfirmTransferring off)
  Configuration::Instance().ConfirmTransferring = false;
  A("LocalLocalCopyAction")->trigger();
  CHECK(WaitFor([&]() { return QFile::exists(QDir(Root).filePath(QStringLiteral("right/hello.txt"))); }));
  // The panels are refreshed when the operation completes
  CHECK(WaitFor([&]() { return R->FocusFile(QStringLiteral("hello.txt")); }));
  QApplication::processEvents();
  Configuration::Instance().ConfirmTransferring = true;
  // Path from Other Panel (Ctrl+.)
  A("LocalOtherDirAction")->trigger();
  CHECK(WaitFor([&]() { return L->CurrentDirectory().endsWith(QStringLiteral("right")); }));
  // Tab caption: both short paths
  CHECK_MSG(Tabs->tabText(Tabs->currentIndex()).contains(QStringLiteral("right")), Tabs->tabText(Tabs->currentIndex()));
  // Popups: directory background and file
  QStringList NewItems;
  QStringList Background = PopupItems([&]() { emit L->ContextMenuRequested(L->mapToGlobal(QPoint(5, 5)), false); },
    &NewItems, QStringLiteral("&New"));
  CHECK_MSG(StartsWith(Background, QStringList({"&Go To", "&Refresh", "&Filter...", "&Add Path to Bookmarks", "Copy &Path to Clipboard", "-",
    "&New", "&Paste from Clipboard"})), Background.join(QStringLiteral("|")));
  CHECK_MSG(NewItems == QStringList({"&File...", "&Directory..."}), NewItems.join(QStringLiteral("|")));
  L->FocusFile(QStringLiteral("hello.txt"));
  QStringList FileItems = PopupItems([&]() { emit L->ContextMenuRequested(L->mapToGlobal(QPoint(5, 5)), true); });
  CHECK_MSG(StartsWith(FileItems, QStringList({"&Open", "&Edit", "&Copy...", "&Move...", "&Delete", "&Rename", "-", "&Copy to Clipboard"})),
    FileItems.join(QStringLiteral("|")));
  // Default double-click action (Edit) is bold
  CHECK_MSG(FileItems.contains(QStringLiteral("BOLD:&Edit")), FileItems.join(QStringLiteral("|")));
  // Rename tab, Duplicate tab, Alt+1
  int Index = Tabs->currentIndex();
  A("DuplicateTabAction")->trigger();
  CHECK(Tabs->count() == Count + 2);
  A("CloseTabAction")->trigger();
  CHECK(Tabs->count() == Count + 1);
  CHECK(Tabs->currentIndex() == Index);
  QAction * Alt1 = nullptr;
  for (QAction * Action : Window.actions())
  {
    if (Action->shortcut() == QKeySequence(QStringLiteral("Alt+1"))) Alt1 = Action;
  }
  CHECK(Alt1 != nullptr);
  if (Alt1 != nullptr)
  {
    Alt1->trigger();
    CHECK(Tabs->currentIndex() == 0);
  }
  Screenshot(&Window, QStringLiteral("commander-local"));
}

// Runs Show, in which a popup opens, and activates its item Item (of the
// submenu Sub), as a click would
static void PopupTrigger(const std::function<void()> & Show, const QString & Item, const QString & Sub = QString())
{
  bool Found = false;
  QTimer::singleShot(150, [&Found, Item, Sub]()
  {
    auto * Menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
    if (Menu == nullptr) return;
    QMenu * Target = Sub.isEmpty() ? Menu : SubMenu(Menu, Sub);
    for (QAction * Action : (Target != nullptr) ? Target->actions() : QList<QAction *>())
    {
      if (Action->text() == Item)
      {
        Found = true;
        Menu->close();
        Action->trigger();
        return;
      }
    }
    Menu->close();
  });
  Show();
  CHECK_MSG(Found, Item);
}

// The modal dialog that comes up while Run runs: closed, its texts returned
static QStringList ModalTexts(const std::function<void()> & Run)
{
  QStringList Result;
  auto * Timer = new QTimer();
  Timer->setInterval(50);
  QObject::connect(Timer, &QTimer::timeout, [&Result, Timer]()
  {
    auto * Dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
    if (Dialog == nullptr) return;
    Timer->stop();
    Result.append(Dialog->windowTitle());
    for (QLabel * Label : Dialog->findChildren<QLabel *>()) Result.append(Label->text());
    for (QLineEdit * Edit : Dialog->findChildren<QLineEdit *>()) Result.append(Edit->text());
    Dialog->reject();
  });
  Timer->start();
  Run();
  WaitFor([&Result]() { return !Result.isEmpty(); }, 3000);
  delete Timer;
  return Result;
}

static void WriteTestFile(const QString & Path, const QByteArray & Data = QByteArray("x"))
{
  QFile F(Path);
  CHECK(F.open(QIODevice::WriteOnly));
  F.write(Data);
}

static void TestFocusedCommands(CommanderWindow & Window, const QString & Root)
{
  std::printf("[file popup and marks]\n");
  CommanderActions & A = *Window.Actions();
  Configuration & Config = Configuration::Instance();
  QString Dir = QDir(Root).filePath(QStringLiteral("focused"));
  QDir().mkpath(Dir);
  for (const char * Name : {"a.txt", "b.txt", "c.txt", "d.txt"})
  {
    WriteTestFile(Dir + QLatin1Char('/') + QString::fromLatin1(Name));
  }
  FilePanel * L = Panel(Window, false);
  L->ChangeDirectory(Dir);
  CHECK(WaitFor([&]() { return L->FocusFile(QStringLiteral("c.txt")) && !L->IsLoading(); }));
  L->FocusView();
  emit L->Activated();
  auto Mark = [&]()
  {
    L->SelectFiles({QStringLiteral("a.txt"), QStringLiteral("b.txt")});
    L->FocusFile(QStringLiteral("c.txt"));
  };
  auto ShowPopup = [&]() { emit L->ContextMenuRequested(L->mapToGlobal(QPoint(5, 5)), true); };
  QString APath = L->FullPath(QStringLiteral("a.txt"));
  QString BPath = L->FullPath(QStringLiteral("b.txt"));
  QString CPath = L->FullPath(QStringLiteral("c.txt"));

  // Copy to Clipboard of the popup: the focused file. File Names: the marks (not a Focused action)
  Mark();
  PopupTrigger(ShowPopup, A("CurrentCopyToClipboardAction2")->text());
  CHECK(QApplication::clipboard()->mimeData()->urls() == QList<QUrl>{QUrl::fromLocalFile(CPath)});
  Mark();
  PopupTrigger(ShowPopup, A("FileListToClipboardAction")->text(), QStringLiteral("&File Names"));
  CHECK_MSG(QApplication::clipboard()->text() == QStringLiteral("a.txt\nb.txt"), QApplication::clipboard()->text());
  // the commands of the menus and the keys stay with the marks
  Mark();
  A("CurrentCopyToClipboardAction2")->trigger();
  CHECK((QApplication::clipboard()->mimeData()->urls() == QList<QUrl>{QUrl::fromLocalFile(APath), QUrl::fromLocalFile(BPath)}));

  // Properties
  Mark();
  QStringList Texts = ModalTexts([&]() { PopupTrigger(ShowPopup, A("CurrentPropertiesAction")->text()); });
  CHECK_MSG(Texts.join(QLatin1Char('|')).contains(QStringLiteral("c.txt")), Texts.join(QLatin1Char('|')));
  Mark();
  // Alt+Enter on the panel
  Texts = ModalTexts([&]() { emit L->PropertiesRequested(); });
  CHECK_MSG(Texts.join(QLatin1Char('|')).contains(QStringLiteral("c.txt")), Texts.join(QLatin1Char('|')));
  Mark();
  Texts = ModalTexts([&]() { A("CurrentPropertiesAction")->trigger(); });
  CHECK_MSG(!Texts.join(QLatin1Char('|')).contains(QStringLiteral("c.txt")), Texts.join(QLatin1Char('|')));

  // Edit, a combo item: a click runs the default action (F-27)
  Mark();
  int Edits = 0;
  auto Counter = QObject::connect(Window.Editors()->EditAction(), &QAction::triggered, [&Edits]() { Edits++; });
  QTimer::singleShot(150, []()
  {
    auto * Menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
    if (Menu == nullptr) return;
    for (QAction * Action : Menu->actions())
    {
      if ((Action->text() == QStringLiteral("&Edit")) && (Action->menu() != nullptr))
      {
        QPoint Pos = Menu->actionGeometry(Action).center();
        QMouseEvent Release(QEvent::MouseButtonRelease, Pos, Menu->mapToGlobal(Pos), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(Menu, &Release);
        break;
      }
    }
    if (Menu->isVisible())
    {
      std::printf("the popup stayed open\n");
      Menu->close();
    }
  });
  ShowPopup();
  CHECK(Edits == 1);
  QObject::disconnect(Counter);
  for (QWidget * Widget : QApplication::topLevelWidgets())
  {
    if (Widget->inherits("EditorWindow")) Widget->close();
  }
  QApplication::processEvents();

  // Delete, permanently: of the popup the focused file, on a worker thread
  bool Confirm = Config.ConfirmDeleting;
  bool Trash = Config.DeleteToRecycleBin;
  Config.ConfirmDeleting = false;
  Config.DeleteToRecycleBin = false;
  Mark();
  PopupTrigger(ShowPopup, A("CurrentDeleteAction")->text());
  CHECK(WaitFor([&]() { return !QFile::exists(CPath); }));
  CHECK(QFile::exists(APath) && QFile::exists(BPath));
  CHECK(WaitFor([&]() { return !L->IsLoading() && !L->FocusFile(QStringLiteral("c.txt")); }));
  // the marks stay marked; F8 takes them
  CHECK(L->MarkedCount() == 2);
  L->FocusFile(QStringLiteral("d.txt"));
  A("CurrentDeleteAction")->trigger();
  CHECK(WaitFor([&]() { return !QFile::exists(APath) && !QFile::exists(BPath); }));
  CHECK(QFile::exists(L->FullPath(QStringLiteral("d.txt"))));
  CHECK(WaitFor([&]() { return (L->MarkedCount() == 0) && !L->FocusFile(QStringLiteral("a.txt")); }));
  // a directory: contents, and a link is not followed
  QDir().mkpath(Dir + QStringLiteral("/tree/sub"));
  WriteTestFile(Dir + QStringLiteral("/tree/sub/file"));
  QDir().mkpath(Dir + QStringLiteral("/kept"));
  WriteTestFile(Dir + QStringLiteral("/kept/file"));
  QFile::link(Dir + QStringLiteral("/kept"), Dir + QStringLiteral("/tree/link"));
  L->Refresh();
  CHECK(WaitFor([&]() { return !L->IsLoading() && L->FocusFile(QStringLiteral("tree")); }));
  L->ClearSelection();
  A("CurrentDeleteAction")->trigger();
  CHECK(WaitFor([&]() { return !QFileInfo::exists(Dir + QStringLiteral("/tree")); }));
  CHECK(QFile::exists(Dir + QStringLiteral("/kept/file")));
  Config.ConfirmDeleting = Confirm;
  Config.DeleteToRecycleBin = Trash;
  CHECK(WaitFor([&]() { return !L->IsLoading() && !L->FocusFile(QStringLiteral("tree")); }));
}

static void TestActionStates(CommanderWindow & Window, const QString & Root)
{
  std::printf("[action states, compare, rename]\n");
  CommanderActions & A = *Window.Actions();
  Configuration & Config = Configuration::Instance();
  FilePanel * L = Panel(Window, false);
  FilePanel * R = Panel(Window, true);
  QString Left = QDir(Root).filePath(QStringLiteral("compare/left"));
  QString Right = QDir(Root).filePath(QStringLiteral("compare/right"));
  QDir().mkpath(Left);
  QDir().mkpath(Right);
  L->FocusView();
  emit L->Activated();

  // F-32: Parent and Root at the root, Path from Other Panel, Filter
  L->ChangeDirectory(QDir::rootPath());
  CHECK(WaitFor([&]() { return !L->IsLoading() && L->IsRoot(); }));
  emit L->SelectionChanged();
  CHECK(!A("LocalParentDirAction")->isEnabled());
  CHECK(!A("LocalRootDirAction")->isEnabled());
  L->ChangeDirectory(Left);
  R->ChangeDirectory(Left);
  CHECK(WaitFor([&]() { return !L->IsLoading() && !R->IsLoading() && L->CurrentDirectory().endsWith(QStringLiteral("left")) && R->CurrentDirectory().endsWith(QStringLiteral("left")); }));
  emit L->SelectionChanged();
  CHECK(A("LocalParentDirAction")->isEnabled());
  CHECK(A("LocalRootDirAction")->isEnabled());
  CHECK(!A("LocalOtherDirAction")->isEnabled());
  R->ChangeDirectory(Right);
  CHECK(WaitFor([&]() { return !R->IsLoading() && R->CurrentDirectory().endsWith(QStringLiteral("right")); }));
  emit L->SelectionChanged();
  CHECK(A("LocalOtherDirAction")->isEnabled());
  CHECK(!A("LocalFilterAction")->isChecked());
  L->SetFilterMask(QStringLiteral("*.txt"));
  emit L->SelectionChanged();
  CHECK(A("LocalFilterAction")->isCheckable() && A("LocalFilterAction")->isChecked());
  L->SetFilterMask(QString());
  emit L->SelectionChanged();
  CHECK(!A("LocalFilterAction")->isChecked());

  // F-09: Ctrl+F
  CHECK(A("IncrementalSearchStartAction")->shortcut() == QKeySequence(QStringLiteral("Ctrl+F")));
  A("IncrementalSearchStartAction")->trigger();
  bool Searching = false;
  for (QLabel * Label : L->findChildren<QLabel *>())
  {
    Searching = Searching || Label->text().startsWith(QStringLiteral("Search:"));
  }
  CHECK(Searching);
  L->FocusView();
  QKeyEvent Escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
  QApplication::sendEvent(L->View(), &Escape);

  // F-23: what the panels show is compared
  WriteTestFile(Left + QStringLiteral("/same.txt"));
  WriteTestFile(Right + QStringLiteral("/same.txt"));
  WriteTestFile(Left + QStringLiteral("/.hidden-left"));
  WriteTestFile(Right + QStringLiteral("/.hidden-right"));
  QDateTime Time = QDateTime::currentDateTimeUtc().addDays(-1);
  for (const QString & Name : {Left + QStringLiteral("/same.txt"), Right + QStringLiteral("/same.txt")})
  {
    QFile File(Name);
    CHECK(File.open(QIODevice::ReadWrite));
    CHECK(File.setFileTime(Time, QFileDevice::FileModificationTime));
  }
  bool Hidden = Config.ShowHiddenFiles;
  Config.ShowHiddenFiles = false;
  L->SetShowHiddenFiles(false);
  R->SetShowHiddenFiles(false);
  L->Refresh();
  R->Refresh();
  CHECK(WaitFor([&]() { return !L->IsLoading() && !R->IsLoading() && L->FocusFile(QStringLiteral("same.txt")) && R->FocusFile(QStringLiteral("same.txt")); }));
  CHECK(L->VisibleFiles().size() == 1);
  QStringList Texts = ModalTexts([&]() { A("CompareDirectoriesAction2")->trigger(); });
  CHECK_MSG(Texts.join(QLatin1Char('|')).contains(QStringLiteral("No differences found")), Texts.join(QLatin1Char('|')));
  CHECK((L->MarkedCount() == 0) && (R->MarkedCount() == 0));
  // a visible difference is still found; its mirror is filtered out: it counts as missing
  WriteTestFile(Left + QStringLiteral("/only-left.txt"));
  L->Refresh();
  CHECK(WaitFor([&]() { return !L->IsLoading() && L->FocusFile(QStringLiteral("only-left.txt")); }));
  A("CompareDirectoriesAction2")->trigger();
  QApplication::processEvents();
  CHECK(L->MarkedNames() == QStringList{QStringLiteral("only-left.txt")});
  L->ClearSelection();
  Config.ShowHiddenFiles = Hidden;
  L->SetShowHiddenFiles(Hidden);
  R->SetShowHiddenFiles(Hidden);

  // D-07, D-08: no control without a feature behind it
  {
    PreferencesDialog Dialog;
    QStringList Captions;
    for (QCheckBox * Check : Dialog.findChildren<QCheckBox *>())
    {
      Captions.append(Check->text());
    }
    CHECK(Captions.contains(QStringLiteral("&Remember panels' state when switching sessions")));
    CHECK(!Captions.contains(QStringLiteral("Show &directory tree left of file list")));
    CHECK(!Captions.contains(QStringLiteral("Use &system file context menu")));
  }

  // F-24: the helpers of the local in-place rename
  auto NoListing = []() -> QStringList { CHECK(false); return QStringList(); };
  // nothing there
  CHECK(!FileOperations::LocalRenameTargetTaken(QStringLiteral("foo"), QStringLiteral("bar"), false, NoListing));
  // another file
  CHECK(FileOperations::LocalRenameTargetTaken(QStringLiteral("foo"), QStringLiteral("bar"), true, NoListing));
  // Windows, macOS: the file itself under another case
  CHECK(!FileOperations::LocalRenameTargetTaken(QStringLiteral("foo"), QStringLiteral("Foo"), true, []() { return QStringList{QStringLiteral("foo")}; }));
  // a case-sensitive volume with both names
  CHECK(FileOperations::LocalRenameTargetTaken(QStringLiteral("foo"), QStringLiteral("Foo"), true,
    []() { return QStringList{QStringLiteral("foo"), QStringLiteral("Foo")}; }));
  CHECK(FileOperations::InvalidNameCharacter(QStringLiteral("a/b"), false) == QStringLiteral("/"));
  CHECK(FileOperations::InvalidNameCharacter(QStringLiteral("a\\b"), false).isEmpty());
  CHECK(FileOperations::InvalidNameCharacter(QStringLiteral("a\\b"), true) == QStringLiteral("\\"));
  CHECK(FileOperations::InvalidNameCharacter(QStringLiteral("name.txt"), true).isEmpty());
  // the rename itself, in the panel
  L->FocusFile(QStringLiteral("same.txt"));
  emit L->RenameRequested(L->FullPath(QStringLiteral("same.txt")), QStringLiteral("Same.txt"));
  // by the names in the folder: on a disk that ignores case both spellings exist
  CHECK(WaitFor([&]() { QStringList Names = QDir(Left).entryList(QDir::Files); return Names.contains(QStringLiteral("Same.txt")) && !Names.contains(QStringLiteral("same.txt")); }));
  CHECK(WaitFor([&]() { return !L->IsLoading() && L->FocusFile(QStringLiteral("Same.txt")); }));
  // the target taken: an error, nothing is lost
  Texts = ModalTexts([&]() { emit L->RenameRequested(L->FullPath(QStringLiteral("Same.txt")), QStringLiteral("only-left.txt")); });
  CHECK_MSG(Texts.join(QLatin1Char('|')).contains(QStringLiteral("Error renaming")), Texts.join(QLatin1Char('|')));
  CHECK(QFile::exists(Left + QStringLiteral("/Same.txt")));
}

static void TestRemote(CommanderWindow & Window, const QString & Root)
{
  QByteArray PortStr = qgetenv("NIXSCP_TEST_SSH_PORT");
  QString KeyFile = qEnvironmentVariable("NIXSCP_TEST_SSH_KEY");
  if (PortStr.isEmpty() || KeyFile.isEmpty())
  {
    std::printf("[remote] SKIPPED (set NIXSCP_TEST_SSH_PORT, NIXSCP_TEST_SSH_KEY)\n");
    return;
  }
  std::printf("[remote]\n");
  CommanderActions & A = *Window.Actions();
  auto * Tabs = Window.findChild<QTabBar *>(QStringLiteral("SessionsPageControl"));
  QString RemoteRoot = QDir(Root).filePath(QStringLiteral("remote"));
  QDir(RemoteRoot).mkpath(QStringLiteral("sync/sub"));
  QDir(Root).mkpath(QStringLiteral("localsync/sub"));
  QFile F(QDir(RemoteRoot).filePath(QStringLiteral("file.txt")));
  (void)F.open(QIODevice::WriteOnly);
  F.write("remote");
  F.close();

  SessionData Data;
  Data.FSProtocol = fsSFTP;
  Data.HostName = qEnvironmentVariable("NIXSCP_TEST_SSH_HOST", QStringLiteral("127.0.0.1"));
  Data.PortNumber = PortStr.toInt();
  Data.UserName = qEnvironmentVariable("NIXSCP_TEST_SSH_USER", qEnvironmentVariable("USER"));
  Data.PublicKeyFile = KeyFile;
  Data.HostKey = QStringLiteral("*");
  Data.RemoteDirectory = RemoteRoot;
  Data.LocalDirectory = QDir(Root).filePath(QStringLiteral("localsync"));
  // A session replaces the implicit local browser tab (the first one) when it is current
  Tabs->setCurrentIndex(1);
  QApplication::processEvents();
  int Count = Tabs->count();
  Window.OpenSession(Data);
  FilePanel * R = nullptr;
  CHECK(WaitFor([&]() { R = Panel(Window, true); return (R != nullptr) && R->IsRemote() && R->FocusFile(QStringLiteral("file.txt")); }, 20000));
  if ((R == nullptr) || !R->IsRemote()) return;
  CHECK(Tabs->count() == Count + 1);
  FilePanel * L = Panel(Window, false);
  // Status bar: STATUS_SECURE
  auto * Secure = Window.statusBar()->findChild<QLabel *>(QStringLiteral("SecurityStatusPanel"));
  CHECK((Secure != nullptr) && (Secure->text() == QStringLiteral("Secure connection (SSH-2)")));
  // Menus say Local/Remote; captions follow the active side
  CHECK(MenuTitles(Window.menuBar()).contains(QStringLiteral("&Remote")));
  R->FocusView();
  emit R->Activated();
  QApplication::processEvents();
  QStringList Files = MenuItems(TopMenu(Window.menuBar(), QStringLiteral("&Files")));
  CHECK_MSG(Files.contains(QStringLiteral("Down&load...")) && Files.contains(QStringLiteral("Download and Dele&te...")) &&
    Files.contains(QStringLiteral("Du&plicate...")) && Files.contains(QStringLiteral("Mo&ve To...")), Files.join(QStringLiteral("|")));
  CHECK(A("RemoteCopyAction")->isEnabled());
  CHECK(A("RemoteCopyToAction")->isEnabled());
  QStringList Dups = DuplicateShortcuts(&Window);
  CHECK_MSG(Dups.isEmpty(), Dups.join(QStringLiteral("; ")));
  L->FocusView();
  emit L->Activated();
  QApplication::processEvents();
  Files = MenuItems(TopMenu(Window.menuBar(), QStringLiteral("&Files")));
  CHECK_MSG(Files.contains(QStringLiteral("Up&load...")) && Files.contains(QStringLiteral("Upload and Dele&te...")), Files.join(QStringLiteral("|")));
  Dups = DuplicateShortcuts(&Window);
  CHECK_MSG(Dups.isEmpty(), Dups.join(QStringLiteral("; ")));
  QStringList HotKeys;
  for (QToolButton * B : Window.findChild<QToolBar *>(QStringLiteral("Toolbar2Toolbar"))->findChildren<QToolButton *>())
  {
    HotKeys.append(B->text());
  }
  CHECK_MSG(HotKeys.contains(QStringLiteral("F5 Upload")), HotKeys.join(QStringLiteral("|")));
  // Remote file popup
  R->FocusView();
  emit R->Activated();
  R->FocusFile(QStringLiteral("file.txt"));
  QStringList CopySub;
  QStringList Popup = PopupItems([&]() { emit R->ContextMenuRequested(R->mapToGlobal(QPoint(5, 5)), true); }, &CopySub,
    QStringLiteral("Down&load..."));
  CHECK_MSG(Popup.contains(QStringLiteral("Down&load...")) && Popup.contains(QStringLiteral("Du&plicate...")) &&
    Popup.contains(QStringLiteral("Mo&ve To...")) && Popup.contains(QStringLiteral("&File Names")), Popup.join(QStringLiteral("|")));
  CHECK_MSG(CopySub == QStringList({"Down&load...", "Download in &Background...", "-", "Download and Dele&te..."}), CopySub.join(QStringLiteral("|")));
  // F-10 on a session: the popup deletes the focused file, the other marks stay
  {
    Configuration & Config = Configuration::Instance();
    bool Confirm = Config.ConfirmDeleting;
    Config.ConfirmDeleting = false;
    QString MarkedPath = QDir(RemoteRoot).filePath(QStringLiteral("del-marked.txt"));
    QString FocusedPath = QDir(RemoteRoot).filePath(QStringLiteral("del-focused.txt"));
    WriteTestFile(MarkedPath);
    WriteTestFile(FocusedPath);
    R->Refresh();
    CHECK(WaitFor([&]() { return !R->IsLoading() && R->FocusFile(QStringLiteral("del-focused.txt")); }));
    R->SelectFiles({QStringLiteral("del-marked.txt")});
    PopupTrigger([&]() { emit R->ContextMenuRequested(R->mapToGlobal(QPoint(5, 5)), true); }, A("CurrentDeleteAction")->text());
    CHECK(WaitFor([&]() { return !QFile::exists(FocusedPath); }));
    CHECK(QFile::exists(MarkedPath));
    CHECK(WaitFor([&]() { return A("CurrentDeleteAction")->isEnabled() && !R->IsLoading() && !R->FocusFile(QStringLiteral("del-focused.txt")); }));
    CHECK(R->MarkedNames() == QStringList{QStringLiteral("del-marked.txt")});
    R->ClearSelection();
    QFile::remove(MarkedPath);
    Config.ConfirmDeleting = Confirm;
    R->Refresh();
    CHECK(WaitFor([&]() { return !R->IsLoading() && R->FocusFile(QStringLiteral("file.txt")); }));
  }
  // Synchronized browsing: the other panel follows
  L->ChangeDirectory(QDir(Root).filePath(QStringLiteral("localsync")));
  R->ChangeDirectory(QDir(RemoteRoot).filePath(QStringLiteral("sync")));
  CHECK(WaitFor([&]() { return L->CurrentDirectory().endsWith(QStringLiteral("localsync")) && R->CurrentDirectory().endsWith(QStringLiteral("/sync")); }));
  A("SynchronizeBrowsingAction2")->setChecked(true);
  A("SynchronizeBrowsingAction2")->triggered(true);
  L->ChangeDirectory(QDir(Root).filePath(QStringLiteral("localsync/sub")));
  CHECK_MSG(WaitFor([&]() { return R->CurrentDirectory().endsWith(QStringLiteral("/sync/sub")); }), R->CurrentDirectory());
  L->ParentDirectory();
  CHECK_MSG(WaitFor([&]() { return R->CurrentDirectory().endsWith(QStringLiteral("/sync")); }), R->CurrentDirectory());
  A("SynchronizeBrowsingAction2")->setChecked(false);
  A("SynchronizeBrowsingAction2")->triggered(false);
  // F-28: a double click on the session panels of the status bar
  {
    QStringList Texts = ModalTexts([&]()
    {
      QMouseEvent Double(QEvent::MouseButtonDblClick, QPointF(2, 2), Secure->mapToGlobal(QPointF(2, 2)), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
      QApplication::sendEvent(Secure, &Double);
    });
    CHECK_MSG(!Texts.isEmpty(), QStringLiteral("no server information"));
  }
  // Clear Caches runs on the session
  CHECK(A("ClearCachesAction")->isEnabled());
  A("ClearCachesAction")->trigger();
  CHECK(WaitFor([&]() { return Window.statusBar()->currentMessage().contains(QStringLiteral("Caches cleared")); }));
  Screenshot(&Window, QStringLiteral("commander-remote"));
  // Disconnect keeps the tab
  A("DisconnectSessionAction")->trigger();
  CHECK(WaitFor([&]() { return !A("DisconnectSessionAction")->isEnabled(); }));
  CHECK(A("ReconnectSessionAction")->isEnabled());
  CHECK(Tabs->count() == Count + 1);
  // F-28: the panel of the disconnected tab offers Reconnect
  {
    FilePanel * Right = Panel(Window, true);
    auto * Reconnect = Right->findChild<QPushButton *>(QStringLiteral("ReconnectItem"));
    CHECK((Reconnect != nullptr) && Reconnect->isVisible());
  }

  // F-04: a disconnected tab is part of the workspace, with its last directory
  QList<SessionData> Saved = Window.CollectWorkspace();
  bool Found = false;
  for (const SessionData & Tab : Saved)
  {
    Found = Found || ((Tab.HostName == Data.HostName) && (Tab.RemoteDirectory == R->CurrentDirectory() || Tab.RemoteDirectory.startsWith(RemoteRoot)));
  }
  CHECK(Found);
  CHECK(Window.WorkspaceTabs().isEmpty());
}

static QList<SessionData> TwoSessions(const QString & Root)
{
  SessionData Data;
  Data.FSProtocol = fsSFTP;
  Data.HostName = qEnvironmentVariable("NIXSCP_TEST_SSH_HOST", QStringLiteral("127.0.0.1"));
  Data.PortNumber = qgetenv("NIXSCP_TEST_SSH_PORT").toInt();
  Data.UserName = qEnvironmentVariable("NIXSCP_TEST_SSH_USER", qEnvironmentVariable("USER"));
  Data.PublicKeyFile = qEnvironmentVariable("NIXSCP_TEST_SSH_KEY");
  Data.HostKey = QStringLiteral("*");
  Data.RemoteDirectory = Root;
  Data.LocalDirectory = Root;
  return {Data, Data};
}

static int ActiveSessions(CommanderWindow & Window)
{
  int Result = 0;
  for (Terminal * Term : Window.findChildren<Terminal *>(Qt::FindDirectChildrenOnly))
  {
    Result += Term->Active() ? 1 : 0;
  }
  return Result;
}

// F-03, F-04, D-01, D-02, D-04, D-06 with a window of their own
static void TestWorkspaces(const QString & Root)
{
  std::printf("[workspaces]\n");
  Configuration & Config = Configuration::Instance();
  StoredSessionList & Sessions = Config.StoredSessions();
  QString Left = QDir(Root).filePath(QStringLiteral("ws/left"));
  QString Right = QDir(Root).filePath(QStringLiteral("ws/right"));
  QDir().mkpath(Left);
  QDir().mkpath(Right);
  Config.ConfirmClosingSession = false;

  // nothing but the implicit local browser: no workspace
  {
    CommanderWindow Window;
    CHECK(Window.CollectWorkspace().isEmpty());
    CHECK(!Window.Actions()->Get(QStringLiteral("SaveWorkspaceAction"))->isEnabled());
    Window.NewLocalTab(Left, Right);
    QApplication::processEvents();
    QList<SessionData> Tabs = Window.CollectWorkspace();
    CHECK(Tabs.size() == 1);
    if (Tabs.size() == 1)
    {
      CHECK(Tabs[0].IsLocalBrowser() && !Tabs[0].CanLogin());
      CHECK(Tabs[0].LocalDirectory.endsWith(QStringLiteral("left")));
      CHECK(Tabs[0].OtherLocalDirectory.endsWith(QStringLiteral("right")));
    }
    CHECK(Window.Actions()->Get(QStringLiteral("SaveWorkspaceAction"))->isEnabled());
    // auto-saved when the window closes, and remembered as the one to reopen
    Config.AutoSaveWorkspace = true;
    Config.AutoWorkspace = QStringLiteral("auto-ws");
    Workspaces::SetLastWorkspace(QString());
    CHECK(Window.close());
    CHECK(Config.LastStoredSession() == QStringLiteral("auto-ws"));
    CHECK(Workspaces::LoadWorkspace(Sessions, QStringLiteral("auto-ws")).size() == 1);
  }

  // F-03: reopened only when the Login dialog does not come first, as in WinSCP
  Config.ShowLoginOnStartup = true;
  {
    CommanderWindow Window;
    CHECK(!Window.OpenAutoWorkspace());
  }
  Config.ShowLoginOnStartup = false;
  {
    Config.SetLastStoredSession(QStringLiteral("something else"));
    CommanderWindow Window;
    CHECK(!Window.OpenAutoWorkspace());
    Config.SetLastStoredSession(QStringLiteral("auto-ws"));
    CHECK(Window.OpenAutoWorkspace());
    QApplication::processEvents();
    auto * Tabs = Window.findChild<QTabBar *>(QStringLiteral("SessionsPageControl"));
    CHECK(Tabs->count() == 2);   // the tab and "New Tab"; the implicit local browser gave way
    FilePanel * L = Panel(Window, false);
    FilePanel * R = Panel(Window, true);
    CHECK((L != nullptr) && (R != nullptr) && !L->IsRemote() && !R->IsRemote());
    CHECK(WaitFor([&]() { return L->CurrentDirectory().endsWith(QStringLiteral("left")) && R->CurrentDirectory().endsWith(QStringLiteral("right")); }));
    CHECK(Window.CollectWorkspace().size() == 1);
  }
  // nothing open when closed: not reopened next time
  {
    CommanderWindow Window;
    CHECK(Window.close());
    CHECK(Config.LastStoredSession().isEmpty());
    CHECK(!Window.OpenAutoWorkspace());
  }
  Config.AutoSaveWorkspace = false;
  Config.AutoWorkspace.clear();
  Workspaces::DeleteWorkspace(Sessions, QStringLiteral("auto-ws"));
  Workspaces::SetLastWorkspace(QString());

  // D-01: panels forget marks, focus and history when the tab is switched
  {
    CommanderWindow Window;
    Window.NewLocalTab(Left, Right);
    QApplication::processEvents();
    auto * Tabs = Window.findChild<QTabBar *>(QStringLiteral("SessionsPageControl"));
    WriteTestFile(Left + QStringLiteral("/marked.txt"));
    FilePanel * L = Panel(Window, false);
    L->Refresh();
    CHECK(WaitFor([&]() { return !L->IsLoading() && L->FocusFile(QStringLiteral("marked.txt")); }));
    L->SelectFiles({QStringLiteral("marked.txt")});
    Tabs->setCurrentIndex(0);
    QApplication::processEvents();
    Tabs->setCurrentIndex(1);
    QApplication::processEvents();
    CHECK(L->MarkedCount() == 1);  // the default: each tab keeps its state
    Config.PreservePanelState = false;
    Tabs->setCurrentIndex(0);
    QApplication::processEvents();
    Tabs->setCurrentIndex(1);
    QApplication::processEvents();
    CHECK(L->MarkedCount() == 0);
    Config.PreservePanelState = true;
  }

  if (qgetenv("NIXSCP_TEST_SSH_PORT").isEmpty() || qEnvironmentVariableIsEmpty("NIXSCP_TEST_SSH_KEY"))
  {
    std::printf("[workspaces, remote] SKIPPED\n");
    return;
  }
  QList<SessionData> Two = TwoSessions(Left);
  Workspaces::SaveWorkspace(Sessions, QStringLiteral("two"), Two, false);

  // D-06: only the first session connects, the other when activated
  Config.WorkspaceConnectAll = false;
  {
    CommanderWindow Window;
    Window.OpenWorkspace(QStringLiteral("two"));
    auto * Tabs = Window.findChild<QTabBar *>(QStringLiteral("SessionsPageControl"));
    CHECK(Tabs->count() == 3);
    CHECK(WaitFor([&]() { return ActiveSessions(Window) == 1; }, 20000));
    WaitFor([]() { return false; }, 1000);
    CHECK(ActiveSessions(Window) == 1);
    CHECK(Tabs->currentIndex() == 0);
    // both are part of the workspace, connected or not
    CHECK(Window.CollectWorkspace().size() == 2);
    Tabs->setCurrentIndex(1);
    CHECK(WaitFor([&]() { return ActiveSessions(Window) == 2; }, 20000));
    CHECK(WaitFor([&]() { FilePanel * R = Panel(Window, true); return (R != nullptr) && (R->Provider() != nullptr) && !R->IsLoading(); }, 10000));

    // D-02: the local panel does not change with the tab
    FilePanel * L1 = Panel(Window, false);
    L1->ChangeDirectory(Right);
    CHECK(WaitFor([&]() { return !L1->IsLoading() && L1->CurrentDirectory().endsWith(QStringLiteral("right")); }));
    Tabs->setCurrentIndex(0);
    QApplication::processEvents();
    FilePanel * L0 = Panel(Window, false);
    CHECK(L0 != L1);
    CHECK(WaitFor([&]() { return !L0->IsLoading() && L0->CurrentDirectory().endsWith(QStringLiteral("left")); }));
    Config.PreserveLocalDirectory = true;
    Tabs->setCurrentIndex(1);
    CHECK_MSG(WaitFor([&]() { return !L1->IsLoading() && L1->CurrentDirectory().endsWith(QStringLiteral("left")); }), L1->CurrentDirectory());
    Config.PreserveLocalDirectory = false;
  }

  // D-06: all connect, the others by themselves, one by one
  Config.WorkspaceConnectAll = true;
  {
    CommanderWindow Window;
    Window.OpenWorkspace(QStringLiteral("two"));
    auto * Tabs = Window.findChild<QTabBar *>(QStringLiteral("SessionsPageControl"));
    CHECK(WaitFor([&]() { return ActiveSessions(Window) == 2; }, 30000));
    CHECK(Tabs->currentIndex() == 0);
  }
  Config.WorkspaceConnectAll = false;

  // F-03: the first tab of the auto-saved workspace waits, with Reconnect
  {
    Config.AutoSaveWorkspace = true;
    Config.AutoWorkspace = QStringLiteral("two");
    Config.SetLastStoredSession(QStringLiteral("two"));
    CommanderWindow Window;
    CHECK(Window.OpenAutoWorkspace());
    QApplication::processEvents();
    WaitFor([]() { return false; }, 500);
    CHECK(ActiveSessions(Window) == 0);
    FilePanel * R = Panel(Window, true);
    auto * Reconnect = R->findChild<QPushButton *>(QStringLiteral("ReconnectItem"));
    CHECK((Reconnect != nullptr) && !Reconnect->isHidden());
    if (Reconnect != nullptr)
    {
      Reconnect->click();
    }
    CHECK(WaitFor([&]() { return ActiveSessions(Window) == 1; }, 20000));
    Config.AutoSaveWorkspace = false;
    Config.AutoWorkspace.clear();
  }

  // D-04: the lost idle session: no message box, the message in the panel
  {
    Config.SessionSilentDisconnect = true;
    bool AutoIdle = Config.SessionReopenAutoIdleOn;
    Config.SessionReopenAutoIdleOn = false;
    CommanderWindow Window;
    Window.OpenWorkspace(QStringLiteral("two"));
    FilePanel * R = nullptr;
    CHECK(WaitFor([&]() { R = Panel(Window, true); return (ActiveSessions(Window) == 1) && (R->Provider() != nullptr); }, 20000));
    Terminal * Term = Window.findChild<Terminal *>(QString(), Qt::FindDirectChildrenOnly);
    bool Dialog = false;
    QTimer::singleShot(300, [&Dialog]()
    {
      if (auto * Box = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
      {
        Dialog = true;
        Box->reject();
      }
    });
    emit Term->Disconnected(QStringLiteral("Connection has been unexpectedly closed."), QString());
    WaitFor([]() { return false; }, 500);
    CHECK(!Dialog);
    auto * Message = R->findChild<QLabel *>(QStringLiteral("ReconnectLabel"));
    auto * Reconnect = R->findChild<QPushButton *>(QStringLiteral("ReconnectItem"));
    CHECK((Message != nullptr) && !Message->isHidden() && Message->text().contains(QStringLiteral("unexpectedly closed")));
    CHECK((Reconnect != nullptr) && !Reconnect->isHidden());
    CHECK(R->Provider() == nullptr);
    if (Reconnect != nullptr)
    {
      Reconnect->click();
      CHECK(WaitFor([&]() { return R->Provider() != nullptr; }, 20000));
      CHECK((Message != nullptr) && Message->isHidden());
    }
    // with the automatic reconnect it comes back by itself
    Config.SessionReopenAutoIdleOn = true;
    int Idle = Config.SessionReopenAutoIdle;
    Config.SessionReopenAutoIdle = 200;
    // (the session before it is waiting for its deferred deletion)
    Term = Window.findChildren<Terminal *>(QString(), Qt::FindDirectChildrenOnly).last();
    emit Term->Disconnected(QStringLiteral("Connection has been unexpectedly closed."), QString());
    CHECK(R->Provider() == nullptr);
    CHECK(WaitFor([&]() { return (R->Provider() != nullptr) && !R->IsLoading(); }, 20000));
    Config.SessionReopenAutoIdle = Idle;
    Config.SessionReopenAutoIdleOn = AutoIdle;
    Config.SessionSilentDisconnect = false;
  }
  Workspaces::DeleteWorkspace(Sessions, QStringLiteral("two"));
}

int main(int argc, char ** argv)
{
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
  {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  qInstallMessageHandler(MessageHandler);
  QApplication App(argc, argv);
  QStandardPaths::setTestModeEnabled(true);
  QTemporaryDir Scratch;
  Configuration::Instance().SetIniFileName(Scratch.filePath(QStringLiteral("nixscp.ini")));
  Configuration::Instance().Load();
  Configuration::Instance().ConfirmClosingSession = false;
  // No question about stale temporary folders of other test runs
  Configuration::Instance().TemporaryDirectoryCleanup = false;
  // Nor about the text editor of the desktop
  {
    IniStorage Storage(&Configuration::Instance().Ini());
    if (Storage.OpenSubKey(QStringLiteral("Configuration"), true) && Storage.OpenSubKey(QStringLiteral("Interface"), true))
    {
      Storage.WriteBool(QStringLiteral("OfferedEditorAutoConfig"), true);
    }
  }
  setvbuf(stdout, nullptr, _IONBF, 0);
  // An unexpected modal dialog must fail the test, not hang it
  QTimer Watchdog;
  QObject::connect(&Watchdog, &QTimer::timeout, []()
  {
    auto * Dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
    // (the window of a session being opened is no reason to fail, on a busy machine)
    if ((Dialog != nullptr) && !Dialog->inherits("AuthenticateDialog"))
    {
      QStringList Texts{Dialog->windowTitle()};
      for (QLabel * Label : Dialog->findChildren<QLabel *>()) Texts.append(Label->text());
      CHECK_MSG(false, QStringLiteral("unexpected dialog: ") + Texts.join(QLatin1Char('|')));
      Dialog->reject();
    }
  });
  Watchdog.start(8000);
  {
    CommanderWindow Window;
    Window.resize(1200, 800);
    Window.show();
    QApplication::processEvents();
    TestStructure(Window);
    TestLocalLocal(Window, Scratch.path());
    TestFocusedCommands(Window, Scratch.path());
    TestActionStates(Window, Scratch.path());
    TestRemote(Window, Scratch.path());
    Window.close();
  }
  TestWorkspaces(Scratch.path());
  std::printf("%d checks, %d failures\n", Checks, Failures);
  return (Failures == 0) ? 0 : 1;
}

#endif
