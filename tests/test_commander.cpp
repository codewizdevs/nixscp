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
#include "core/Terminal.h"
#include "gui/CommanderActions.h"
#include "gui/CommanderWindow.h"
#include "gui/FilePanel.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
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
  // NixSCP Pro adds its menu after Help ("Cloud Sync" when signed out).
  QStringList Titles = MenuTitles(Bar);
  CHECK_MSG(Titles == QStringList({"&Left", "&Mark", "&Files", "&Commands", "&Tabs", "&Options", "&Right", "T&unnels", "Tea&ms", "&Help", "Cloud Sync"}),
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
  setvbuf(stdout, nullptr, _IONBF, 0);
  {
    CommanderWindow Window;
    Window.resize(1200, 800);
    Window.show();
    QApplication::processEvents();
    TestStructure(Window);
    TestLocalLocal(Window, Scratch.path());
    TestRemote(Window, Scratch.path());
    Window.close();
  }
  std::printf("%d checks, %d failures\n", Checks, Failures);
  return (Failures == 0) ? 0 : 1;
}

#endif
