// End-to-end verification of the Commander main window (gui/CommanderWindow,
// CommanderActions, FilePanel, FileListModel, QueuePanel, ProgressDialog,
// EditorManager, FileOperations, ClipboardTransfer) against WinSCP
// forms/ScpCommander + forms/NonVisual.dfm: the real window is driven
// through its keyboard shortcuts (delivered as the platform would: shortcut
// override first, then the key press to the focus widget), actions, popup
// menus, simulated drag & drop and the clipboard, and the effect on the
// remote (test sshd serving the local file system) and local files and on
// the panels is checked.
//
// Covered: navigation (Enter, Backspace, Alt+Left/Right, Ctrl+\, Ctrl+H,
// Ctrl+R), marking (Insert, Space, Ctrl+A, Num*, Num+/Num- masks, Alt+Num+,
// Ctrl+Shift+L, Ctrl+Shift+S), F2 rename, F4 edit + save (upload), Shift+F4
// new file, F5/F6 upload/download/move, Shift+F5 duplicate, Shift+F6 move to,
// Alt+F6 link, F7 directory, F8/Shift+Del delete, F9/Alt+Enter properties,
// Ctrl+C/Ctrl+V file clipboard, Ctrl+Shift+C/Ctrl+Alt+C names, Ctrl+[ / Ctrl+]
// path, drag & drop between panels and onto a directory, tabs (Ctrl+T,
// duplicate, rename, Ctrl+W), queue (Upload in Background), Shift+F2 compare,
// Ctrl+Alt+F filter, Alt+F7 find files, Ctrl+B bookmark + Ctrl+O, Ctrl+Shift+T
// console, custom commands (Touch, ad hoc), workspaces (save + open),
// Ctrl+S synchronize, Ctrl+U keep remote directory up to date, file popup
// menu, Ctrl+Alt+H hidden files, Ctrl+L address bar, Ctrl+Alt+B synchronized
// browsing, disconnect + Ctrl+Shift+R reconnect, F10 quit.
// NIXSCP_VERIFY_ONLY=<section,...> runs some sections only (see main).
//
// GUI test: built with NIXSCP_GUI_TESTS and the GUI objects, runs offscreen.
// Needs NIXSCP_TEST_SSH_PORT and NIXSCP_TEST_SSH_KEY (NIXSCP_TEST_SSH_HOST,
// NIXSCP_TEST_SSH_USER optional), skips otherwise.
// NIXSCP_TEST_SCREENSHOT=<dir> saves screenshots of key states.
#ifndef NIXSCP_GUI_TESTS

#include <cstdio>

int main()
{
  std::printf("SKIP: test_verify_gui needs NIXSCP_GUI_TESTS (Qt Widgets + the GUI sources)\n");
  return 0;
}

#else

#include "core/Configuration.h"
#include "core/Queue.h"
#include "gui/CommanderActions.h"
#include "gui/CommanderWindow.h"
#include "gui/ConsoleDialog.h"
#include "gui/CopyDialog.h"
#include "gui/CustomCommandDialog.h"
#include "gui/EditMaskDialog.h"
#include "gui/EditorManager.h"
#include "gui/EditorWindow.h"
#include "gui/FileListModel.h"
#include "gui/FilePanel.h"
#include "gui/FindFilesDialog.h"
#include "gui/GuiUserInterface.h"
#include "gui/LinkDialog.h"
#include "gui/LocationProfilesDialog.h"
#include "gui/OpenDirectoryDialog.h"
#include "gui/ProgressDialog.h"
#include "gui/PropertiesDialog.h"
#include "gui/QueuePanel.h"
#include "gui/RemoteTransferDialog.h"
#include "gui/SaveWorkspaceDialog.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QStatusBar>
#include <QTabBar>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QTreeView>
#include <QTreeWidget>

#include <cstdio>
#include <functional>
#include <sys/stat.h>

// QtGui test hooks (qtestkeyboard.h): the platform's key path
Q_GUI_EXPORT bool qt_sendShortcutOverrideEvent(QObject * o, ulong timestamp, int k, Qt::KeyboardModifiers mods,
  const QString & text = QString(), bool autorep = false, ushort count = 1);

static int Failures = 0;
static int Checks = 0;
static QString ShotDir;

#define CHECK(Cond) \
  do { Checks++; if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); std::fflush(stdout); Failures++; } } while (0)
#define CHECK_MSG(Cond, Msg) \
  do { Checks++; if (!(Cond)) { std::printf("FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, #Cond, qPrintable(Msg)); std::fflush(stdout); Failures++; } } while (0)

static bool WaitFor(const std::function<bool()> & Condition, int Ms = 15000)
{
  QElapsedTimer Timer;
  Timer.start();
  while (!Condition())
  {
    if (Timer.elapsed() > Ms) return false;
    QApplication::processEvents(QEventLoop::AllEvents, 20);
    QThread::msleep(5);
  }
  return true;
}

static void Pump(int Ms = 100)
{
  QElapsedTimer Timer;
  Timer.start();
  while (Timer.elapsed() < Ms)
  {
    QApplication::processEvents(QEventLoop::AllEvents, 20);
    QThread::msleep(5);
  }
}

//---------------------------------------------------------------------------
// Modal dialogs: scripted handlers by class name; message boxes answered
// with MessageAnswer; anything else is recorded and rejected

struct ModalStep
{
  QString ClassName;
  std::function<void(QWidget *)> Handle;
};
static QList<ModalStep> ModalScript;
static QStringList Messages;            // texts of the message forms shown
static QStringList Unexpected;          // other modals (class:title)
static unsigned int MessageAnswer = qaOK;
static QList<QPointer<QWidget>> Handled;   // dialogs already answered (may still be closing or wait for a nested one)

static bool WasHandled(QWidget * W)
{
  for (const QPointer<QWidget> & H : Handled) if (H == W) return true;
  return false;
}

static QString ClassOf(QObject * O)
{
  return QString::fromLatin1(O->metaObject()->className());
}

static void InstallModalHandler()
{
  auto * Timer = new QTimer(qApp);
  Timer->setInterval(40);
  QObject::connect(Timer, &QTimer::timeout, []()
  {
    QWidget * Modal = QApplication::activeModalWidget();
    if ((Modal == nullptr) || !Modal->isVisible()) return;
    if (WasHandled(Modal)) return;   // still closing, or waiting for a nested dialog
    Handled.removeAll(QPointer<QWidget>());
    QString Class = ClassOf(Modal);
    // the connection log window of a connecting session closes itself
    if (Class == QStringLiteral("AuthenticateDialog")) return;
    if (!ModalScript.isEmpty() && ((ModalScript.first().ClassName == Class) ||
        (ModalScript.first().ClassName == Class + QLatin1Char(':') + Modal->windowTitle())))
    {
      ModalStep Step = ModalScript.takeFirst();
      Handled.append(Modal);
      // outside of this timer's slot: the step may open nested dialogs that
      // this timer has to answer
      QPointer<QWidget> Guard(Modal);
      QTimer::singleShot(0, Modal, [Step, Guard]() { if (Guard) Step.Handle(Guard); });
      return;
    }
    if (auto * Form = qobject_cast<MessageForm *>(Modal))
    {
      QString Text = Form->FormText();
      Messages.append(Text);
      std::printf("  [message] %s\n", qPrintable(Text.left(200).replace(QLatin1Char('\n'), QLatin1Char(' '))));
      Handled.append(Modal);
      if (Form->AnswerButton(MessageAnswer) != nullptr) Form->SubmitAnswer(MessageAnswer);
      else if (Form->AnswerButton(qaOK) != nullptr) Form->SubmitAnswer(qaOK);
      else if (Form->AnswerButton(qaYes) != nullptr) Form->SubmitAnswer(qaYes);
      else Form->reject();
      return;
    }
    if (auto * Box = qobject_cast<QMessageBox *>(Modal))
    {
      Messages.append(Box->text());
      std::printf("  [message box] %s\n", qPrintable(Box->text().left(200).replace(QLatin1Char('\n'), QLatin1Char(' '))));
      Handled.append(Modal);
      QAbstractButton * Button = Box->button(QMessageBox::Ok);
      if (Button == nullptr) Button = Box->button(QMessageBox::Yes);
      if (Button == nullptr) Button = Box->defaultButton();
      if (Button != nullptr) Button->click();
      else Box->reject();
      return;
    }
    if (auto * Dialog = qobject_cast<QDialog *>(Modal))
    {
      Unexpected.append(Class + QLatin1Char(':') + Dialog->windowTitle());
      std::printf("  [unexpected modal] %s:%s\n", qPrintable(Class), qPrintable(Dialog->windowTitle()));
      Handled.append(Modal);
      Dialog->reject();
    }
  });
  Timer->start();
}

static void Expect(const QString & ClassName, const std::function<void(QWidget *)> & Handle)
{
  ModalScript.append({ClassName, Handle});
}

static bool ScriptDone(int Ms = 10000)
{
  bool Result = WaitFor([]() { return ModalScript.isEmpty(); }, Ms);
  if (!Result)
  {
    std::printf("  modal script not consumed: next %s\n", qPrintable(ModalScript.first().ClassName));
    ModalScript.clear();
  }
  return Result;
}

// Popup menus (QMenu::exec): the visible items are recorded, the item with
// PopupChoice (plain caption) is triggered, otherwise the popup is closed
static QStringList PopupItems;
static QString PopupChoice;

static QString Plain(const QString & Caption)
{
  QString R = Caption;
  R.remove(QLatin1Char('&'));
  int Tab = R.indexOf(QLatin1Char('\t'));
  if (Tab >= 0) R = R.left(Tab);
  return R;
}

static void InstallPopupHandler()
{
  auto * Timer = new QTimer(qApp);
  Timer->setInterval(40);
  QObject::connect(Timer, &QTimer::timeout, []()
  {
    auto * Menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
    if ((Menu == nullptr) || !Menu->isVisible()) return;
    PopupItems.clear();
    QAction * Choice = nullptr;
    for (QAction * A : Menu->actions())
    {
      if (A->isSeparator() || !A->isVisible()) continue;
      PopupItems.append(Plain(A->text()));
      if (!PopupChoice.isEmpty() && (Plain(A->text()) == PopupChoice) && A->isEnabled()) Choice = A;
    }
    PopupChoice.clear();
    if (Choice != nullptr)
    {
      Menu->setActiveAction(Choice);
      QKeyEvent Press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
      QApplication::sendEvent(Menu, &Press);
    }
    else
    {
      Menu->close();
    }
  });
  Timer->start();
}

//---------------------------------------------------------------------------
// Window helpers

static FilePanel * Panel(CommanderWindow & Window, bool Right)
{
  for (QSplitter * Splitter : Window.findChildren<QSplitter *>())
  {
    if (!Splitter->isVisibleTo(&Window) || (Splitter->count() != 2)) continue;
    auto * Left = qobject_cast<FilePanel *>(Splitter->widget(0));
    auto * RightPanel = qobject_cast<FilePanel *>(Splitter->widget(1));
    if ((Left != nullptr) && (RightPanel != nullptr)) return Right ? RightPanel : Left;
  }
  return nullptr;
}

static QTabBar * Tabs(CommanderWindow & Window)
{
  for (QTabBar * Bar : Window.findChildren<QTabBar *>())
  {
    if (Bar->objectName() == QStringLiteral("SessionsPageControl")) return Bar;
  }
  // the session tab bar is the first direct tab bar that is not part of a tab widget
  for (QTabBar * Bar : Window.findChildren<QTabBar *>())
  {
    if (ClassOf(Bar->parent()) != QStringLiteral("QTabWidget")) return Bar;
  }
  return nullptr;
}

// Delivers a key as the platform does: ShortcutOverride / shortcut map
// first, then the key press to the focus widget
static QPointer<QWidget> MainWindow;

static void Key(int KeyCode, Qt::KeyboardModifiers Mods = Qt::NoModifier, const QString & Text = QString())
{
  // a window manager re-activates the main window when a dialog closes; offscreen does not
  if (MainWindow && (QApplication::activeWindow() == nullptr) && (QApplication::activeModalWidget() == nullptr))
  {
    QWidget * Focus = MainWindow->focusWidget();
    MainWindow->activateWindow();
    WaitFor([]() { return QApplication::activeWindow() != nullptr; }, 2000);
    if (Focus != nullptr) Focus->setFocus(Qt::ActiveWindowFocusReason);
  }
  QWidget * Target = QApplication::focusWidget();
  if (Target == nullptr) Target = QApplication::activeWindow();
  if (Target == nullptr) return;
  QPointer<QWidget> Guard(Target);
  if (!qt_sendShortcutOverrideEvent(Target, 0, KeyCode, Mods, Text))
  {
    if (Guard)
    {
      QKeyEvent Press(QEvent::KeyPress, KeyCode, Mods, Text);
      QApplication::sendEvent(Target, &Press);
    }
  }
  if (Guard)
  {
    QKeyEvent Release(QEvent::KeyRelease, KeyCode, Mods, Text);
    QApplication::sendEvent(Target, &Release);
  }
  Pump(30);
}

static void Activate(CommanderWindow & Window, FilePanel * P)
{
  Window.activateWindow();
  WaitFor([&]() { return Window.isActiveWindow(); }, 2000);
  P->FocusView();
  P->View()->setFocus(Qt::OtherFocusReason);
  emit P->Activated();
  Pump(30);
}

static bool Loaded(FilePanel * P, const QString & Dir = QString())
{
  return WaitFor([&]() { return !P->IsLoading() && (Dir.isEmpty() || (P->CurrentDirectory() == Dir)); });
}

static QStringList Names(FilePanel * P)
{
  QStringList Result;
  QAbstractItemModel * Model = P->View()->model();
  for (int Row = 0; Row < Model->rowCount(); Row++)
  {
    QString Name = Model->index(Row, FileListModel::ColName).data().toString();
    if (Name != QStringLiteral("..")) Result.append(Name);
  }
  return Result;
}

static bool Listed(FilePanel * P, const QString & Name)
{
  return Names(P).contains(Name);
}

static void Refresh(FilePanel * P)
{
  P->Refresh();
  Loaded(P);
}

static void WriteFile(const QString & FileName, const QByteArray & Data)
{
  QDir().mkpath(QFileInfo(FileName).path());
  QFile F(FileName);
  (void)F.open(QIODevice::WriteOnly);
  F.write(Data);
}

static QByteArray ReadFile(const QString & FileName)
{
  QFile F(FileName);
  if (!F.open(QIODevice::ReadOnly)) return QByteArray("<missing>");
  return F.readAll();
}

static unsigned int Mode(const QString & FileName)
{
  struct stat St;
  if (::lstat(QFile::encodeName(FileName).constData(), &St) != 0) return 0;
  return St.st_mode & 07777;
}

static void Shot(CommanderWindow & Window, const QString & Name)
{
  if (ShotDir.isEmpty()) return;
  Pump(100);
  Window.grab().save(QDir(ShotDir).filePath(Name + QStringLiteral(".png")));
}

static QAction * A(CommanderWindow & Window, const char * Name)
{
  return Window.Actions()->Get(QString::fromLatin1(Name));
}

// No dialog, no foreground operation (its progress window is deleted when
// it finishes), no panel loading
static bool Idle(CommanderWindow & Window)
{
  return WaitFor([&]() { return (QApplication::activeModalWidget() == nullptr) && Window.findChildren<ProgressDialog *>().isEmpty() &&
    Panel(Window, true) && !Panel(Window, true)->IsLoading() && !Panel(Window, false)->IsLoading(); }, 20000);
}

// Sets the editable combo / line edit of a dialog found by object name (or the first editable combo)
static void SetComboText(QWidget * Dialog, const QString & ObjectName, const QString & Text)
{
  QComboBox * Combo = ObjectName.isEmpty() ? nullptr : Dialog->findChild<QComboBox *>(ObjectName);
  if (Combo == nullptr)
  {
    for (QComboBox * C : Dialog->findChildren<QComboBox *>())
    {
      if (C->isEditable() && C->isVisible()) { Combo = C; break; }
    }
  }
  if (Combo != nullptr) Combo->setCurrentText(Text);
  else std::printf("  no combo '%s' in %s\n", qPrintable(ObjectName), qPrintable(ClassOf(Dialog)));
}

static void Accept(QWidget * Dialog)
{
  if (auto * D = qobject_cast<QDialog *>(Dialog)) D->accept();
}

//---------------------------------------------------------------------------

struct Env
{
  QString Root;
  QString R;     // remote directory
  QString L;     // local directory
};

static SessionData TestSession(const QString & RemoteDir, const QString & LocalDir)
{
  SessionData Data;
  Data.FSProtocol = fsSFTP;
  Data.HostName = qEnvironmentVariable("NIXSCP_TEST_SSH_HOST", QStringLiteral("127.0.0.1"));
  Data.PortNumber = qEnvironmentVariable("NIXSCP_TEST_SSH_PORT").toInt();
  Data.UserName = qEnvironmentVariable("NIXSCP_TEST_SSH_USER", qEnvironmentVariable("USER"));
  Data.PublicKeyFile = qEnvironmentVariable("NIXSCP_TEST_SSH_KEY");
  Data.HostKey = QStringLiteral("*");
  Data.RemoteDirectory = RemoteDir;
  Data.LocalDirectory = LocalDir;
  return Data;
}

static bool Connect(CommanderWindow & Window, const Env & E)
{
  Window.OpenSession(TestSession(E.R, E.L));
  bool Result = WaitFor([&]()
  {
    FilePanel * R = Panel(Window, true);
    FilePanel * L = Panel(Window, false);
    return (R != nullptr) && R->IsRemote() && (R->Provider() != nullptr) && !R->IsLoading() && (R->CurrentDirectory() == E.R) &&
      (L->CurrentDirectory() == E.L) && !L->IsLoading();
  }, 30000);
  if (!Result) std::printf("  connect failed; unexpected: %s\n", qPrintable(Unexpected.join(QLatin1Char(','))));
  return Result;
}

static void ResetDirs(CommanderWindow & Window, const Env & E)
{
  FilePanel * R = Panel(Window, true);
  FilePanel * L = Panel(Window, false);
  if (R->CurrentDirectory() != E.R) R->ChangeDirectory(E.R);
  if (L->CurrentDirectory() != E.L) L->ChangeDirectory(E.L);
  Loaded(R, E.R);
  Loaded(L, E.L);
  Refresh(R);
  Refresh(L);
  R->ClearSelection();
  L->ClearSelection();
  R->SetFilterMask(QString());
  L->SetFilterMask(QString());
}

//---------------------------------------------------------------------------

static void TestNavigation(CommanderWindow & Window, const Env & E)
{
  std::printf("[navigation]\n");
  FilePanel * R = Panel(Window, true);
  Activate(Window, R);
  CHECK(R->FocusFile(QStringLiteral("sub")));
  Key(Qt::Key_Return);
  CHECK_MSG(Loaded(R, E.R + QStringLiteral("/sub")), R->CurrentDirectory());
  CHECK(Listed(R, QStringLiteral("deep.txt")));
  Key(Qt::Key_Backspace);
  CHECK_MSG(Loaded(R, E.R), R->CurrentDirectory());
  // the directory we came from is focused
  CHECK(R->HasFocusedFile() && (R->FocusedFile().FileName == QStringLiteral("sub")));
  Key(Qt::Key_Left, Qt::AltModifier);
  CHECK_MSG(Loaded(R, E.R + QStringLiteral("/sub")), R->CurrentDirectory());
  Key(Qt::Key_Right, Qt::AltModifier);
  CHECK_MSG(Loaded(R, E.R), R->CurrentDirectory());
  Key(Qt::Key_Backslash, Qt::ControlModifier);
  CHECK_MSG(Loaded(R, QStringLiteral("/")), R->CurrentDirectory());
  Key(Qt::Key_H, Qt::ControlModifier);
  CHECK_MSG(WaitFor([&]() { return !R->IsLoading() && (R->CurrentDirectory() != QStringLiteral("/")); }), R->CurrentDirectory());
  std::printf("  Ctrl+H -> %s\n", qPrintable(R->CurrentDirectory()));
  R->ChangeDirectory(E.R);
  Loaded(R, E.R);
  // Ctrl+R reloads: a file created behind the panel's back appears
  WriteFile(E.R + QStringLiteral("/appeared.txt"), "x");
  Activate(Window, R);
  Key(Qt::Key_R, Qt::ControlModifier);
  CHECK(WaitFor([&]() { return Listed(R, QStringLiteral("appeared.txt")); }));
  // local side: Enter / Backspace
  FilePanel * L = Panel(Window, false);
  Activate(Window, L);
  CHECK(L->FocusFile(QStringLiteral("ldir")));
  Key(Qt::Key_Return);
  CHECK_MSG(Loaded(L, E.L + QStringLiteral("/ldir")), L->CurrentDirectory());
  Key(Qt::Key_Backspace);
  CHECK_MSG(Loaded(L, E.L), L->CurrentDirectory());
  // Path to clipboard: Ctrl+[ (local), Ctrl+] (remote)
  Key(Qt::Key_BracketLeft, Qt::ControlModifier);
  CHECK_MSG(QApplication::clipboard()->text() == E.L, QApplication::clipboard()->text());
  Activate(Window, R);
  Key(Qt::Key_BracketRight, Qt::ControlModifier);
  CHECK_MSG(QApplication::clipboard()->text() == E.R, QApplication::clipboard()->text());
  // Tab switches the active panel
  Key(Qt::Key_Tab);
  CHECK_MSG(L->View()->hasFocus(), ClassOf(QApplication::focusWidget()));
}

static void TestMarking(CommanderWindow & Window, const Env & E)
{
  std::printf("[marking]\n");
  FilePanel * R = Panel(Window, true);
  ResetDirs(Window, E);
  Activate(Window, R);
  CHECK(R->FocusFile(QStringLiteral("a.txt")));
  Key(Qt::Key_Insert);
  CHECK(R->MarkedNames() == QStringList{QStringLiteral("a.txt")});
  // Insert moves to the next item
  CHECK(R->HasFocusedFile() && (R->FocusedFile().FileName != QStringLiteral("a.txt")));
  QString Next = R->FocusedFile().FileName;
  Key(Qt::Key_Space, Qt::NoModifier, QStringLiteral(" "));
  CHECK_MSG(R->MarkedCount() == 2, R->MarkedNames().join(QLatin1Char(',')));
  std::printf("  Space after Insert: marked %s (focused %s)\n", qPrintable(R->MarkedNames().join(QLatin1Char(','))), qPrintable(Next));
  Key(Qt::Key_L, Qt::ControlModifier | Qt::ShiftModifier);
  CHECK(R->MarkedCount() == 0);
  Key(Qt::Key_A, Qt::ControlModifier);
  int All = Names(R).size();
  CHECK_MSG(R->MarkedCount() == All, QStringLiteral("%1 of %2").arg(R->MarkedCount()).arg(All));
  Key(Qt::Key_Asterisk, Qt::KeypadModifier, QStringLiteral("*"));
  CHECK(R->MarkedCount() == 0);
  // Num+ "*.txt"
  Expect(QStringLiteral("EditMaskDialog"), [](QWidget * D) { SetComboText(D, QString(), QStringLiteral("*.txt")); Accept(D); });
  Key(Qt::Key_Plus, Qt::KeypadModifier, QStringLiteral("+"));
  CHECK(ScriptDone());
  QStringList Txt;
  for (const QString & N : Names(R)) if (N.endsWith(QStringLiteral(".txt"))) Txt.append(N);
  CHECK_MSG(R->MarkedNames() == Txt, R->MarkedNames().join(QLatin1Char(',')) + QStringLiteral(" vs ") + Txt.join(QLatin1Char(',')));
  // Num- "a*"
  Expect(QStringLiteral("EditMaskDialog"), [](QWidget * D) { SetComboText(D, QString(), QStringLiteral("a*")); Accept(D); });
  Key(Qt::Key_Minus, Qt::KeypadModifier, QStringLiteral("-"));
  CHECK(ScriptDone());
  CHECK(!R->MarkedNames().contains(QStringLiteral("a.txt")) && R->MarkedNames().contains(QStringLiteral("b.txt")));
  // Restore selection after clearing
  R->SaveSelectedNames();
  QStringList Saved = R->MarkedNames();
  Key(Qt::Key_L, Qt::ControlModifier | Qt::ShiftModifier);
  Key(Qt::Key_S, Qt::ControlModifier | Qt::ShiftModifier);
  CHECK_MSG(R->MarkedNames() == Saved, R->MarkedNames().join(QLatin1Char(',')));
  Key(Qt::Key_L, Qt::ControlModifier | Qt::ShiftModifier);
  // Alt+Num+: same extension as the focused file
  CHECK(R->FocusFile(QStringLiteral("c.log")));
  Key(Qt::Key_Plus, Qt::AltModifier | Qt::KeypadModifier, QStringLiteral("+"));
  QStringList Logs;
  for (const QString & N : Names(R)) if (N.endsWith(QStringLiteral(".log"))) Logs.append(N);
  CHECK_MSG(R->MarkedNames() == Logs, R->MarkedNames().join(QLatin1Char(',')));
  Shot(Window, QStringLiteral("marking"));
  Key(Qt::Key_L, Qt::ControlModifier | Qt::ShiftModifier);
  CHECK(R->MarkedCount() == 0);
}

static void TestCreateRenameDelete(CommanderWindow & Window, const Env & E)
{
  std::printf("[F7 / F2 / F8]\n");
  FilePanel * R = Panel(Window, true);
  FilePanel * L = Panel(Window, false);
  ResetDirs(Window, E);
  for (FilePanel * P : {R, L})
  {
    QString Dir = (P == R) ? E.R : E.L;
    Activate(Window, P);
    Expect(QStringLiteral("CreateDirectoryDialog"), [](QWidget * D)
    {
      qobject_cast<CreateDirectoryDialog *>(D)->SetDirectoryName(QStringLiteral("made by F7"));
      Accept(D);
    });
    Key(Qt::Key_F7);
    CHECK(ScriptDone());
    CHECK_MSG(WaitFor([&]() { return QFileInfo(Dir + QStringLiteral("/made by F7")).isDir(); }), Dir);
    CHECK(WaitFor([&]() { return Listed(P, QStringLiteral("made by F7")) && P->HasFocusedFile() &&
      (P->FocusedFile().FileName == QStringLiteral("made by F7")); }));
    Idle(Window);

    // F2: inline rename
    CHECK(P->FocusFile(QStringLiteral("torename.txt")));
    Key(Qt::Key_F2);
    QLineEdit * Editor = nullptr;
    CHECK(WaitFor([&]()
    {
      for (QLineEdit * Ed : P->View()->findChildren<QLineEdit *>()) if (Ed->isVisible()) Editor = Ed;
      return Editor != nullptr;
    }, 3000));
    if (Editor != nullptr)
    {
      // the base name is selected (as WinSCP)
      CHECK_MSG(Editor->selectedText() == QStringLiteral("torename"), Editor->selectedText());
      Editor->setText(QStringLiteral("renamed.txt"));
      Key(Qt::Key_Return);
      CHECK_MSG(WaitFor([&]() { return QFileInfo::exists(Dir + QStringLiteral("/renamed.txt")); }), Dir);
      CHECK(!QFileInfo::exists(Dir + QStringLiteral("/torename.txt")));
      CHECK(WaitFor([&]() { return Listed(P, QStringLiteral("renamed.txt")) && !Listed(P, QStringLiteral("torename.txt")); }));
    }
    Idle(Window);

    // F8 with confirmation (Cancel keeps the file)
    Activate(Window, P);
    CHECK(P->FocusFile(QStringLiteral("todelete.txt")));
    Messages.clear();
    MessageAnswer = qaCancel;
    Key(Qt::Key_F8);
    CHECK(WaitFor([&]() { return !Messages.isEmpty(); }));
    Idle(Window);
    CHECK(QFileInfo::exists(Dir + QStringLiteral("/todelete.txt")));
    std::printf("  F8 asks: %s\n", qPrintable(Messages.value(0)));
    MessageAnswer = qaOK;
    Messages.clear();
    Activate(Window, P);
    CHECK(P->FocusFile(QStringLiteral("todelete.txt")));
    Key(Qt::Key_F8);
    CHECK(WaitFor([&]() { return !QFileInfo::exists(Dir + QStringLiteral("/todelete.txt")); }));
    CHECK(WaitFor([&]() { return !Listed(P, QStringLiteral("todelete.txt")); }));
    CHECK(Messages.size() == 1);
    Idle(Window);
    // Shift+Del: the opposite of "Delete to recycle bin" (off here): local
    // files go to the Trash, remote ones (no recycle bin) are deleted
    Activate(Window, P);
    P->SelectFiles({QStringLiteral("made by F7")});
    WriteFile(Dir + QStringLiteral("/made by F7/inner.txt"), "inner");
    Messages.clear();
    Key(Qt::Key_Delete, Qt::ShiftModifier);
    CHECK_MSG(WaitFor([&]() { return !QFileInfo::exists(Dir + QStringLiteral("/made by F7")); }), Messages.join(QLatin1Char('|')));
    CHECK(WaitFor([&]() { return !Listed(P, QStringLiteral("made by F7")); }));
    Idle(Window);
  }
  Shot(Window, QStringLiteral("after-f7-f2-f8"));
}

static void TestTransfers(CommanderWindow & Window, const Env & E)
{
  std::printf("[F5 / F6 transfers]\n");
  FilePanel * R = Panel(Window, true);
  FilePanel * L = Panel(Window, false);
  ResetDirs(Window, E);
  WriteFile(E.L + QStringLiteral("/up1.bin"), QByteArray(5000, 'u'));
  WriteFile(E.L + QStringLiteral("/up2.bin"), QByteArray(7000, 'v'));
  WriteFile(E.L + QStringLiteral("/upmove.bin"), QByteArray(300, 'm'));
  WriteFile(E.R + QStringLiteral("/down.bin"), QByteArray(4000, 'd'));
  WriteFile(E.R + QStringLiteral("/downmove.bin"), QByteArray(400, 'e'));
  Refresh(L);
  Refresh(R);
  // F5 upload of two marked files
  Activate(Window, L);
  L->SelectFiles({QStringLiteral("up1.bin"), QStringLiteral("up2.bin")});
  QString DialogCaption;
  int Options = 0;
  Expect(QStringLiteral("CopyDialog"), [&](QWidget * D) { DialogCaption = D->windowTitle(); Options = qobject_cast<CopyDialog *>(D)->Options(); Accept(D); });
  Key(Qt::Key_F5);
  CHECK(ScriptDone());
  std::printf("  F5 dialog: %s\n", qPrintable(DialogCaption));
  CHECK(DialogCaption == QStringLiteral("Upload"));
  // executed by the F5 shortcut: the Copy dialog offers the shortcut hint (cocShortCutHint)
  CHECK((Options & CopyDialog::coShortCutHint) != 0);
  CHECK(WaitFor([&]() { return ReadFile(E.R + QStringLiteral("/up2.bin")) == QByteArray(7000, 'v') &&
    ReadFile(E.R + QStringLiteral("/up1.bin")) == QByteArray(5000, 'u'); }));
  CHECK(WaitFor([&]() { return Listed(R, QStringLiteral("up1.bin")) && Listed(R, QStringLiteral("up2.bin")); }));
  // the transferred files are unmarked
  CHECK(WaitFor([&]() { return L->MarkedCount() == 0; }));
  Idle(Window);
  // F6 upload and delete
  Activate(Window, L);
  CHECK(L->FocusFile(QStringLiteral("upmove.bin")));
  Expect(QStringLiteral("CopyDialog"), [&](QWidget * D) { DialogCaption = D->windowTitle(); Options = qobject_cast<CopyDialog *>(D)->Options(); Accept(D); });
  Key(Qt::Key_F6);
  CHECK(ScriptDone());
  CHECK_MSG(DialogCaption == QStringLiteral("Upload and Delete"), DialogCaption);
  CHECK((Options & CopyDialog::coShortCutHint) == 0);
  CHECK(WaitFor([&]() { return QFileInfo::exists(E.R + QStringLiteral("/upmove.bin")) && !QFileInfo::exists(E.L + QStringLiteral("/upmove.bin")); }));
  CHECK(WaitFor([&]() { return !Listed(L, QStringLiteral("upmove.bin")); }));
  Idle(Window);
  // F5 download (focused file)
  Activate(Window, R);
  CHECK(R->FocusFile(QStringLiteral("down.bin")));
  Expect(QStringLiteral("CopyDialog"), [&](QWidget * D) { DialogCaption = D->windowTitle(); Accept(D); });
  Key(Qt::Key_F5);
  CHECK(ScriptDone());
  CHECK_MSG(DialogCaption == QStringLiteral("Download"), DialogCaption);
  CHECK(WaitFor([&]() { return ReadFile(E.L + QStringLiteral("/down.bin")) == QByteArray(4000, 'd'); }));
  CHECK(WaitFor([&]() { return Listed(L, QStringLiteral("down.bin")); }));
  Idle(Window);
  // Download from the menu (not by shortcut): no hint
  Activate(Window, R);
  CHECK(R->FocusFile(QStringLiteral("down.bin")));
  Expect(QStringLiteral("CopyDialog"), [&](QWidget * D) { Options = qobject_cast<CopyDialog *>(D)->Options(); qobject_cast<QDialog *>(D)->reject(); });
  A(Window, "RemoteCopyAction")->trigger();
  CHECK(ScriptDone());
  CHECK((Options & CopyDialog::coShortCutHint) == 0);
  // F6 download and delete
  Activate(Window, R);
  CHECK(R->FocusFile(QStringLiteral("downmove.bin")));
  Expect(QStringLiteral("CopyDialog"), [](QWidget * D) { Accept(D); });
  Key(Qt::Key_F6);
  CHECK(ScriptDone());
  CHECK(WaitFor([&]() { return QFileInfo::exists(E.L + QStringLiteral("/downmove.bin")) && !QFileInfo::exists(E.R + QStringLiteral("/downmove.bin")); }));
  CHECK(WaitFor([&]() { return !Listed(R, QStringLiteral("downmove.bin")); }));
  Idle(Window);
  // overwrite confirmation: F5 of an existing file, answered "No" (skip)
  WriteFile(E.L + QStringLiteral("/up1.bin"), QByteArray(10, 'n'));
  Refresh(L);
  Activate(Window, L);
  CHECK(L->FocusFile(QStringLiteral("up1.bin")));
  Messages.clear();
  MessageAnswer = qaNo;
  Expect(QStringLiteral("CopyDialog"), [](QWidget * D) { Accept(D); });
  Key(Qt::Key_F5);
  CHECK(ScriptDone());
  CHECK(WaitFor([&]() { return !Messages.isEmpty(); }));
  Idle(Window);
  CHECK(ReadFile(E.R + QStringLiteral("/up1.bin")) == QByteArray(5000, 'u'));
  MessageAnswer = qaOK;
  std::printf("  overwrite query: %s\n", qPrintable(Messages.value(0).left(120)));
  // target typed in the Copy dialog with a new name
  Activate(Window, L);
  CHECK(L->FocusFile(QStringLiteral("up2.bin")));
  Expect(QStringLiteral("CopyDialog"), [&E](QWidget * D)
  {
    SetComboText(D, QString(), E.R + QStringLiteral("/sub/renamed-up2.bin"));
    Accept(D);
  });
  Key(Qt::Key_F5);
  CHECK(ScriptDone());
  CHECK_MSG(WaitFor([&]() { return ReadFile(E.R + QStringLiteral("/sub/renamed-up2.bin")) == QByteArray(7000, 'v'); }),
    QDir(E.R + QStringLiteral("/sub")).entryList(QDir::Files).join(QLatin1Char(',')) + QStringLiteral(" | ") + Messages.join(QLatin1Char('|')));
  Idle(Window);
}

static void TestRemoteOps(CommanderWindow & Window, const Env & E)
{
  std::printf("[Shift+F5 / Shift+F6 / Alt+F6 / F9]\n");
  FilePanel * R = Panel(Window, true);
  FilePanel * L = Panel(Window, false);
  ResetDirs(Window, E);
  WriteFile(E.R + QStringLiteral("/dup.txt"), "duplicate me");
  WriteFile(E.R + QStringLiteral("/mv.txt"), "move me");
  WriteFile(E.R + QStringLiteral("/perm.txt"), "perm");
  Refresh(R);
  Activate(Window, R);
  // Shift+F5 duplicate to a new name in the same directory
  CHECK(R->FocusFile(QStringLiteral("dup.txt")));
  Expect(QStringLiteral("RemoteTransferDialog"), [&E](QWidget * D)
  {
    SetComboText(D, QStringLiteral("DirectoryEdit"), E.R + QStringLiteral("/dup-copy.txt"));
    Accept(D);
  });
  Key(Qt::Key_F5, Qt::ShiftModifier);
  CHECK(ScriptDone());
  CHECK(WaitFor([&]() { return ReadFile(E.R + QStringLiteral("/dup-copy.txt")) == "duplicate me"; }));
  CHECK(QFileInfo::exists(E.R + QStringLiteral("/dup.txt")));
  CHECK(WaitFor([&]() { return Listed(R, QStringLiteral("dup-copy.txt")); }));
  Idle(Window);
  // Shift+F6 move to a subdirectory
  Activate(Window, R);
  CHECK(R->FocusFile(QStringLiteral("mv.txt")));
  Expect(QStringLiteral("RemoteMoveDialog"), [&E](QWidget * D)
  {
    SetComboText(D, QStringLiteral("DirectoryEdit"), E.R + QStringLiteral("/sub/"));
    Accept(D);
  });
  Key(Qt::Key_F6, Qt::ShiftModifier);
  CHECK(ScriptDone());
  CHECK(WaitFor([&]() { return QFileInfo::exists(E.R + QStringLiteral("/sub/mv.txt")) && !QFileInfo::exists(E.R + QStringLiteral("/mv.txt")); }));
  CHECK(WaitFor([&]() { return !Listed(R, QStringLiteral("mv.txt")); }));
  Idle(Window);
  // Alt+F6 symbolic link (remote and local)
  for (FilePanel * P : {R, L})
  {
    QString Dir = (P == R) ? E.R : E.L;
    Activate(Window, P);
    P->FocusFile(QStringLiteral("a.txt"));
    Expect(QStringLiteral("LinkDialog"), [](QWidget * D)
    {
      auto * Dialog = qobject_cast<LinkDialog *>(D);
      Dialog->SetFileName(QStringLiteral("link-to-a"));
      Dialog->SetPointTo(QStringLiteral("a.txt"));
      Dialog->SetSymbolicLink(true);
      Accept(D);
    });
    Key(Qt::Key_F6, Qt::AltModifier);
    CHECK(ScriptDone());
    CHECK_MSG(WaitFor([&]() { return QFileInfo(Dir + QStringLiteral("/link-to-a")).isSymLink(); }), Dir);
    CHECK(QFileInfo(Dir + QStringLiteral("/link-to-a")).symLinkTarget() == Dir + QStringLiteral("/a.txt"));
    CHECK(WaitFor([&]() { return Listed(P, QStringLiteral("link-to-a")); }));
    Idle(Window);
  }
  // F9 properties: permissions 0640 (remote), Alt+Enter (local) 0604
  ::chmod(QFile::encodeName(E.R + QStringLiteral("/perm.txt")).constData(), 0644);
  ::chmod(QFile::encodeName(E.L + QStringLiteral("/a.txt")).constData(), 0644);
  Refresh(R);
  Refresh(L);
  for (FilePanel * P : {R, L})
  {
    bool Remote = (P == R);
    QString File = Remote ? (E.R + QStringLiteral("/perm.txt")) : (E.L + QStringLiteral("/a.txt"));
    unsigned int NewMode = Remote ? 0640 : 0604;
    Activate(Window, P);
    CHECK(P->FocusFile(QFileInfo(File).fileName()));
    QString Caption;
    Expect(QStringLiteral("PropertiesDialog"), [&Caption, NewMode](QWidget * D)
    {
      Caption = D->windowTitle();
      auto * Rights = D->findChild<RightsWidget *>();
      if (Rights != nullptr) Rights->SetRights(NewMode);
      // SetRights does not emit Changed: toggle a bit twice to mark the change
      for (QCheckBox * Box : Rights->findChildren<QCheckBox *>())
      {
        if (Box->isVisible() && Box->isEnabled() && (Box->objectName().isEmpty() || true))
        {
          Box->click();
          Box->click();
          break;
        }
      }
      Accept(D);
    });
    if (Remote) Key(Qt::Key_F9);
    else Key(Qt::Key_Return, Qt::AltModifier);
    CHECK(ScriptDone());
    std::printf("  properties caption: %s\n", qPrintable(Caption));
    CHECK_MSG(WaitFor([&]() { return Mode(File) == NewMode; }), QString::number(Mode(File), 8));
    Idle(Window);
  }
  // the panel shows the new permissions
  CHECK(WaitFor([&]()
  {
    QAbstractItemModel * Model = R->View()->model();
    for (int Row = 0; Row < Model->rowCount(); Row++)
    {
      if (Model->index(Row, FileListModel::ColName).data().toString() == QStringLiteral("perm.txt"))
        return Model->index(Row, FileListModel::ColRights).data().toString() == QStringLiteral("rw-r-----");
    }
    return false;
  }));
}

static void TestClipboard(CommanderWindow & Window, const Env & E)
{
  std::printf("[clipboard]\n");
  FilePanel * R = Panel(Window, true);
  FilePanel * L = Panel(Window, false);
  ResetDirs(Window, E);
  WriteFile(E.L + QStringLiteral("/clip-up.txt"), "clip up");
  WriteFile(E.R + QStringLiteral("/clip-down.txt"), "clip down");
  Refresh(L);
  Refresh(R);
  // names
  Activate(Window, L);
  L->SelectFiles({QStringLiteral("a.txt"), QStringLiteral("b.txt")});
  Key(Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier);
  CHECK_MSG(QApplication::clipboard()->text() == QStringLiteral("a.txt\nb.txt"), QApplication::clipboard()->text());
  Key(Qt::Key_C, Qt::ControlModifier | Qt::AltModifier);
  CHECK_MSG(QApplication::clipboard()->text() == E.L + QStringLiteral("/a.txt\n") + E.L + QStringLiteral("/b.txt"), QApplication::clipboard()->text());
  L->ClearSelection();
  // Ctrl+C local file, Ctrl+V in the remote panel = upload (Copy dialog)
  CHECK(L->FocusFile(QStringLiteral("clip-up.txt")));
  Key(Qt::Key_C, Qt::ControlModifier);
  const QMimeData * Mime = QApplication::clipboard()->mimeData();
  CHECK((Mime != nullptr) && Mime->hasUrls());
  Activate(Window, R);
  CHECK(A(Window, "PasteAction3")->isEnabled());
  QString Caption;
  Expect(QStringLiteral("CopyDialog"), [&Caption](QWidget * D) { Caption = D->windowTitle(); Accept(D); });
  Key(Qt::Key_V, Qt::ControlModifier);
  CHECK(ScriptDone());
  CHECK_MSG(Caption == QStringLiteral("Upload"), Caption);
  CHECK(WaitFor([&]() { return ReadFile(E.R + QStringLiteral("/clip-up.txt")) == "clip up"; }));
  CHECK(WaitFor([&]() { return Listed(R, QStringLiteral("clip-up.txt")); }));
  Idle(Window);
  // Ctrl+C remote file, Ctrl+V in the local panel = download
  Activate(Window, R);
  CHECK(R->FocusFile(QStringLiteral("clip-down.txt")));
  Key(Qt::Key_C, Qt::ControlModifier);
  Pump(200);
  Activate(Window, L);
  Expect(QStringLiteral("CopyDialog"), [&Caption](QWidget * D) { Caption = D->windowTitle(); Accept(D); });
  Key(Qt::Key_V, Qt::ControlModifier);
  CHECK(ScriptDone());
  CHECK_MSG(Caption == QStringLiteral("Download"), Caption);
  CHECK(WaitFor([&]() { return ReadFile(E.L + QStringLiteral("/clip-down.txt")) == "clip down"; }));
  CHECK(WaitFor([&]() { return Listed(L, QStringLiteral("clip-down.txt")); }));
  Idle(Window);
  // Ctrl+C remote, Ctrl+V into a remote subdirectory = duplicate there
  Activate(Window, R);
  CHECK(R->FocusFile(QStringLiteral("clip-down.txt")));
  Key(Qt::Key_C, Qt::ControlModifier);
  Pump(200);
  R->ChangeDirectory(E.R + QStringLiteral("/sub"));
  Loaded(R, E.R + QStringLiteral("/sub"));
  Activate(Window, R);
  Key(Qt::Key_V, Qt::ControlModifier);
  CHECK(WaitFor([&]() { return ReadFile(E.R + QStringLiteral("/sub/clip-down.txt")) == "clip down"; }));
  Idle(Window);
  // Ctrl+C local, Ctrl+V into the local panel's other directory = local copy
  Activate(Window, L);
  CHECK(L->FocusFile(QStringLiteral("clip-up.txt")));
  Key(Qt::Key_C, Qt::ControlModifier);
  L->ChangeDirectory(E.L + QStringLiteral("/ldir"));
  Loaded(L, E.L + QStringLiteral("/ldir"));
  Activate(Window, L);
  Key(Qt::Key_V, Qt::ControlModifier);
  CHECK(WaitFor([&]() { return ReadFile(E.L + QStringLiteral("/ldir/clip-up.txt")) == "clip up"; }));
  CHECK(WaitFor([&]() { return Listed(L, QStringLiteral("clip-up.txt")); }));
  Idle(Window);
  // a path in the clipboard opens the directory
  QApplication::clipboard()->setText(E.R + QStringLiteral("/sub"));
  Pump(100);
  R->ChangeDirectory(E.R);
  Loaded(R, E.R);
  Activate(Window, R);
  Key(Qt::Key_V, Qt::ControlModifier);
  CHECK_MSG(Loaded(R, E.R + QStringLiteral("/sub")), R->CurrentDirectory());
}

static void Drop(FilePanel * Target, const QStringList & Paths, bool FromRemote, const QString & OnRowName, Qt::KeyboardModifiers Mods = Qt::NoModifier)
{
  QTreeView * View = Target->View();
  QPoint Pos(View->viewport()->width() / 2, View->viewport()->height() - 5);
  if (!OnRowName.isEmpty())
  {
    QAbstractItemModel * Model = View->model();
    for (int Row = 0; Row < Model->rowCount(); Row++)
    {
      QModelIndex Index = Model->index(Row, FileListModel::ColName);
      if (Index.data().toString() == OnRowName)
      {
        View->scrollTo(Index);
        Pos = View->visualRect(Index).center();
      }
    }
  }
  auto * Mime = new QMimeData();
  QJsonObject Obj;
  Obj.insert(QStringLiteral("remote"), FromRemote);
  Obj.insert(QStringLiteral("paths"), QJsonArray::fromStringList(Paths));
  Mime->setData(QStringLiteral("application/x-nixscp-files"), QJsonDocument(Obj).toJson(QJsonDocument::Compact));
  if (!FromRemote)
  {
    QList<QUrl> Urls;
    for (const QString & P : Paths) Urls.append(QUrl::fromLocalFile(P));
    Mime->setUrls(Urls);
  }
  QDragEnterEvent Enter(Pos, Qt::CopyAction | Qt::MoveAction, Mime, Qt::LeftButton, Mods);
  QApplication::sendEvent(View->viewport(), &Enter);
  QDragMoveEvent Move(Pos, Qt::CopyAction | Qt::MoveAction, Mime, Qt::LeftButton, Mods);
  QApplication::sendEvent(View->viewport(), &Move);
  QDropEvent DropEvent(QPointF(Pos), Qt::CopyAction | Qt::MoveAction, Mime, Qt::LeftButton, Mods);
  QApplication::sendEvent(View->viewport(), &DropEvent);
  delete Mime;
  Pump(50);
}

static void TestDragDrop(CommanderWindow & Window, const Env & E)
{
  std::printf("[drag & drop]\n");
  FilePanel * R = Panel(Window, true);
  FilePanel * L = Panel(Window, false);
  ResetDirs(Window, E);
  WriteFile(E.L + QStringLiteral("/dd-up.txt"), "dd up");
  WriteFile(E.R + QStringLiteral("/dd-down.txt"), "dd down");
  WriteFile(E.Root + QStringLiteral("/external/ext.txt"), "external");
  Refresh(L);
  Refresh(R);
  Configuration::Instance().DDTransferConfirmation = asOn;
  QString Mime = QStringLiteral("application/x-nixscp-files");
  // the format name used by FilePanel
  // local -> remote panel background: upload with the Copy dialog
  Expect(QStringLiteral("CopyDialog"), [](QWidget * D) { Accept(D); });
  Drop(R, {E.L + QStringLiteral("/dd-up.txt")}, false, QString());
  CHECK(ScriptDone());
  CHECK(WaitFor([&]() { return ReadFile(E.R + QStringLiteral("/dd-up.txt")) == "dd up"; }));
  Idle(Window);
  // local -> remote directory row "sub"
  Expect(QStringLiteral("CopyDialog"), [](QWidget * D) { Accept(D); });
  Drop(R, {E.L + QStringLiteral("/dd-up.txt")}, false, QStringLiteral("sub"));
  CHECK(ScriptDone());
  CHECK(WaitFor([&]() { return ReadFile(E.R + QStringLiteral("/sub/dd-up.txt")) == "dd up"; }));
  Idle(Window);
  // remote -> local panel: download
  Expect(QStringLiteral("CopyDialog"), [](QWidget * D) { Accept(D); });
  Drop(L, {E.R + QStringLiteral("/dd-down.txt")}, true, QString());
  CHECK(ScriptDone());
  CHECK(WaitFor([&]() { return ReadFile(E.L + QStringLiteral("/dd-down.txt")) == "dd down"; }));
  Idle(Window);
  // a file from another application (text/uri-list only) onto the local panel: copy
  {
    QTreeView * View = L->View();
    QPoint Pos(10, View->viewport()->height() - 5);
    QMimeData Data;
    Data.setUrls({QUrl::fromLocalFile(E.Root + QStringLiteral("/external/ext.txt"))});
    QDragEnterEvent Enter(Pos, Qt::CopyAction | Qt::MoveAction, &Data, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(View->viewport(), &Enter);
    QDropEvent DropEvent(QPointF(Pos), Qt::CopyAction | Qt::MoveAction, &Data, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(View->viewport(), &DropEvent);
    CHECK(WaitFor([&]() { return ReadFile(E.L + QStringLiteral("/ext.txt")) == "external"; }));
    CHECK(QFileInfo::exists(E.Root + QStringLiteral("/external/ext.txt")));
    Idle(Window);
  }
  // external file onto the remote panel: upload
  Expect(QStringLiteral("CopyDialog"), [](QWidget * D) { Accept(D); });
  {
    QTreeView * View = R->View();
    QPoint Pos(10, View->viewport()->height() - 5);
    QMimeData Data;
    Data.setUrls({QUrl::fromLocalFile(E.Root + QStringLiteral("/external/ext.txt"))});
    QDragEnterEvent Enter(Pos, Qt::CopyAction | Qt::MoveAction, &Data, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(View->viewport(), &Enter);
    QDropEvent DropEvent(QPointF(Pos), Qt::CopyAction | Qt::MoveAction, &Data, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(View->viewport(), &DropEvent);
  }
  CHECK(ScriptDone());
  CHECK(WaitFor([&]() { return ReadFile(E.R + QStringLiteral("/ext.txt")) == "external"; }));
  Idle(Window);
  // Shift = move (remote -> local)
  WriteFile(E.R + QStringLiteral("/dd-move.txt"), "dd move");
  Refresh(R);
  Expect(QStringLiteral("CopyDialog"), [](QWidget * D) { Accept(D); });
  Drop(L, {E.R + QStringLiteral("/dd-move.txt")}, true, QString(), Qt::ShiftModifier);
  CHECK(ScriptDone());
  CHECK(WaitFor([&]() { return QFileInfo::exists(E.L + QStringLiteral("/dd-move.txt")) && !QFileInfo::exists(E.R + QStringLiteral("/dd-move.txt")); }));
  Idle(Window);
  // within the remote panel (the view's own drag: emitted as the view does):
  // drop onto "sub" moves (rename), Ctrl+drop duplicates
  WriteFile(E.R + QStringLiteral("/dd-inner.txt"), "inner move");
  WriteFile(E.R + QStringLiteral("/dd-dup.txt"), "inner dup");
  Refresh(R);
  emit R->DropRequested({E.R + QStringLiteral("/dd-inner.txt")}, true, E.R + QStringLiteral("/sub"), Qt::MoveAction);
  CHECK(WaitFor([&]() { return (ReadFile(E.R + QStringLiteral("/sub/dd-inner.txt")) == "inner move") &&
    !QFileInfo::exists(E.R + QStringLiteral("/dd-inner.txt")); }));
  CHECK(WaitFor([&]() { return !Listed(R, QStringLiteral("dd-inner.txt")); }));
  Idle(Window);
  Expect(QStringLiteral("RemoteTransferDialog"), [](QWidget * D) { Accept(D); });
  emit R->DropRequested({E.R + QStringLiteral("/dd-dup.txt")}, true, E.R + QStringLiteral("/sub"), Qt::CopyAction);
  CHECK(ScriptDone());
  CHECK(WaitFor([&]() { return (ReadFile(E.R + QStringLiteral("/sub/dd-dup.txt")) == "inner dup") &&
    QFileInfo::exists(E.R + QStringLiteral("/dd-dup.txt")); }));
  Idle(Window);
  Configuration::Instance().DDTransferConfirmation = asAuto;
  // Automatic confirmation: Ctrl+drop within the panel duplicates directly
  QDir().mkpath(E.R + QStringLiteral("/sub2"));
  Unexpected.clear();
  emit R->DropRequested({E.R + QStringLiteral("/dd-dup.txt")}, true, E.R + QStringLiteral("/sub2"), Qt::CopyAction);
  CHECK_MSG(WaitFor([&]() { return ReadFile(E.R + QStringLiteral("/sub2/dd-dup.txt")) == "inner dup"; }),
    Window.statusBar()->currentMessage() + QStringLiteral(" | ") + Messages.join(QLatin1Char('|')) + QStringLiteral(" | ") +
    QDir(E.R + QStringLiteral("/sub2")).entryList(QDir::Files).join(QLatin1Char(',')));
  CHECK(Unexpected.isEmpty());
  Idle(Window);
}

static void TestTabs(CommanderWindow & Window, const Env & E)
{
  std::printf("[tabs]\n");
  QTabBar * Bar = Tabs(Window);
  CHECK(Bar != nullptr);
  if (Bar == nullptr) return;
  int Before = Bar->count();
  std::printf("  tab bar: %d tabs: '%s'\n", Before, qPrintable(Bar->tabText(0)));
  FilePanel * R0 = Panel(Window, true);
  // Duplicate tab: same session, same directories
  A(Window, "DuplicateTabAction")->trigger();
  CHECK(WaitFor([&]() { return Panel(Window, true) != R0; }));
  FilePanel * R1 = Panel(Window, true);
  CHECK_MSG(WaitFor([&]() { return (R1->Provider() != nullptr) && !R1->IsLoading() && (R1->CurrentDirectory() == R0->CurrentDirectory()); }, 30000),
    R1->CurrentDirectory());
  int AfterDuplicate = Bar->count();
  CHECK(AfterDuplicate > Before);
  Shot(Window, QStringLiteral("duplicated-tab"));
  // Rename tab
  Expect(QStringLiteral("QInputDialog"), [](QWidget * D)
  {
    if (auto * Edit = D->findChild<QLineEdit *>()) Edit->setText(QStringLiteral("Renamed tab"));
    Accept(D);
  });
  A(Window, "RenameTabAction")->trigger();
  CHECK(ScriptDone(3000));
  bool Renamed = false;
  for (int I = 0; I < Bar->count(); I++) if (Bar->tabText(I).contains(QStringLiteral("Renamed tab"))) Renamed = true;
  CHECK(Renamed);
  // Ctrl+Tab cycles back to the first tab
  Activate(Window, R1);
  Key(Qt::Key_Tab, Qt::ControlModifier);
  CHECK(WaitFor([&]() { return Panel(Window, true) == R0; }, 3000));
  Key(Qt::Key_Backtab, Qt::ControlModifier | Qt::ShiftModifier);
  CHECK(WaitFor([&]() { return Panel(Window, true) == R1; }, 3000));
  // Ctrl+W closes the duplicate
  Activate(Window, Panel(Window, true));
  MessageAnswer = qaOK;
  Key(Qt::Key_W, Qt::ControlModifier);
  CHECK(WaitFor([&]() { return Bar->count() == Before; }, 5000));
  CHECK(Panel(Window, true) == R0);
  // Ctrl+Shift+N: a new local tab (two local panels)
  Activate(Window, R0);
  Key(Qt::Key_N, Qt::ControlModifier | Qt::ShiftModifier);
  CHECK(WaitFor([&]() { return (Bar->count() == Before + 1) && (Panel(Window, true) != R0); }, 5000));
  FilePanel * Other = Panel(Window, true);
  CHECK((Other != nullptr) && !Other->IsRemote());
  Shot(Window, QStringLiteral("local-tab"));
  // F5 in a local tab copies locally (Copy dialog of local-local)
  FilePanel * LL = Panel(Window, false);
  LL->ChangeDirectory(E.L);
  Other->ChangeDirectory(E.Root + QStringLiteral("/external"));
  Loaded(LL, E.L);
  Loaded(Other, E.Root + QStringLiteral("/external"));
  Activate(Window, LL);
  CHECK(LL->FocusFile(QStringLiteral("b.txt")));
  Expect(QStringLiteral("QDialog"), [](QWidget * D) { Accept(D); });
  Key(Qt::Key_F5);
  CHECK(ScriptDone(5000));
  CHECK(WaitFor([&]() { return ReadFile(E.Root + QStringLiteral("/external/b.txt")) == ReadFile(E.L + QStringLiteral("/b.txt")); }));
  Idle(Window);
  A(Window, "CloseTabAction")->trigger();
  CHECK(WaitFor([&]() { return Bar->count() == Before; }, 5000));
  CHECK(WaitFor([&]() { return Panel(Window, true) == R0; }, 3000));
}

static void TestQueue(CommanderWindow & Window, const Env & E)
{
  std::printf("[queue]\n");
  FilePanel * R = Panel(Window, true);
  FilePanel * L = Panel(Window, false);
  ResetDirs(Window, E);
  WriteFile(E.L + QStringLiteral("/queued.bin"), QByteArray(100000, 'q'));
  Refresh(L);
  Activate(Window, L);
  CHECK(L->FocusFile(QStringLiteral("queued.bin")));
  bool QueueChecked = false;
  Expect(QStringLiteral("CopyDialog"), [&QueueChecked](QWidget * D)
  {
    QueueChecked = qobject_cast<CopyDialog *>(D)->Queue();
    Accept(D);
  });
  A(Window, "LocalCopyQueueAction")->trigger();
  CHECK(ScriptDone());
  CHECK(QueueChecked);
  CHECK(WaitFor([&]() { return ReadFile(E.R + QStringLiteral("/queued.bin")).size() == 100000; }));
  QueuePanel * Queue = Window.findChild<QueuePanel *>();
  CHECK((Queue != nullptr) && Queue->isVisible());
  CHECK(WaitFor([&]() { return Listed(R, QStringLiteral("queued.bin")); }));
  Shot(Window, QStringLiteral("queue"));
  Idle(Window);
}

static void TestCompareFilter(CommanderWindow & Window, const Env & E)
{
  std::printf("[compare / filter]\n");
  FilePanel * R = Panel(Window, true);
  FilePanel * L = Panel(Window, false);
  QString CR = E.Root + QStringLiteral("/cmp-r");
  QString CL = E.Root + QStringLiteral("/cmp-l");
  WriteFile(CR + QStringLiteral("/same.txt"), "same");
  WriteFile(CL + QStringLiteral("/same.txt"), "same");
  WriteFile(CR + QStringLiteral("/only-remote.txt"), "r");
  WriteFile(CL + QStringLiteral("/only-local.txt"), "l");
  QDateTime T = QDateTime::currentDateTime().addSecs(-3600);
  for (const QString & F : {CR + QStringLiteral("/same.txt"), CL + QStringLiteral("/same.txt")})
  {
    QFile File(F);
    (void)File.open(QIODevice::ReadWrite);
    File.setFileTime(T, QFileDevice::FileModificationTime);
  }
  R->ChangeDirectory(CR);
  L->ChangeDirectory(CL);
  Loaded(R, CR);
  Loaded(L, CL);
  Activate(Window, L);
  Key(Qt::Key_F2, Qt::ShiftModifier);
  CHECK(WaitFor([&]() { return (L->MarkedCount() > 0) || (R->MarkedCount() > 0); }, 5000));
  Idle(Window);
  CHECK_MSG(L->MarkedNames() == QStringList{QStringLiteral("only-local.txt")}, L->MarkedNames().join(QLatin1Char(',')));
  CHECK_MSG(R->MarkedNames() == QStringList{QStringLiteral("only-remote.txt")}, R->MarkedNames().join(QLatin1Char(',')));
  Shot(Window, QStringLiteral("compare"));
  L->ClearSelection();
  R->ClearSelection();
  // Ctrl+Alt+F filter
  Activate(Window, R);
  Expect(QStringLiteral("EditMaskDialog"), [](QWidget * D) { SetComboText(D, QString(), QStringLiteral("same*")); Accept(D); });
  Key(Qt::Key_F, Qt::ControlModifier | Qt::AltModifier);
  CHECK(ScriptDone());
  CHECK_MSG(WaitFor([&]() { return Names(R) == QStringList{QStringLiteral("same.txt")}; }, 3000), Names(R).join(QLatin1Char(',')));
  CHECK(R->FilterMask() == QStringLiteral("same*"));
  Shot(Window, QStringLiteral("filter"));
  R->SetFilterMask(QString());
  CHECK(WaitFor([&]() { return Names(R).size() == 2; }, 3000));
}

static void TestFindFiles(CommanderWindow & Window, const Env & E)
{
  std::printf("[find files]\n");
  FilePanel * R = Panel(Window, true);
  ResetDirs(Window, E);
  Activate(Window, R);
  Key(Qt::Key_F7, Qt::AltModifier);
  FindFilesDialog * Dialog = nullptr;
  CHECK(WaitFor([&]() { Dialog = Window.findChild<FindFilesDialog *>(); return (Dialog != nullptr) && Dialog->isVisible(); }, 3000));
  if (Dialog == nullptr) return;
  QList<QComboBox *> Combos = Dialog->findChildren<QComboBox *>();
  CHECK(Combos.size() >= 2);
  Combos[0]->setCurrentText(QStringLiteral("deep*"));
  Combos[1]->setCurrentText(E.R);
  QPushButton * Start = nullptr;
  QPushButton * Focus = nullptr;
  for (QPushButton * B : Dialog->findChildren<QPushButton *>())
  {
    if (B->text() == QStringLiteral("&Start")) Start = B;
    if (B->text() == QStringLiteral("Fo&cus")) Focus = B;
  }
  CHECK(Start != nullptr);
  if (Start == nullptr) return;
  Start->click();
  auto * View = Dialog->findChild<QTreeWidget *>();
  CHECK(WaitFor([&]() { return (View->topLevelItemCount() > 0) && (Start->text() == QStringLiteral("&Start")); }, 15000));
  CHECK(View->topLevelItemCount() == 1);
  if (View->topLevelItemCount() > 0)
  {
    CHECK(View->topLevelItem(0)->text(0) == QStringLiteral("deep.txt"));
    View->setCurrentItem(View->topLevelItem(0));
    if (!ShotDir.isEmpty()) Dialog->grab().save(QDir(ShotDir).filePath(QStringLiteral("find-files.png")));
    if (Focus != nullptr) Focus->click();
    CHECK_MSG(Loaded(R, E.R + QStringLiteral("/sub")), R->CurrentDirectory());
    CHECK(WaitFor([&]() { return R->HasFocusedFile() && (R->FocusedFile().FileName == QStringLiteral("deep.txt")); }, 3000));
  }
  Dialog->close();
  Pump(100);
}

static void TestEditor(CommanderWindow & Window, const Env & E)
{
  std::printf("[editor]\n");
  FilePanel * R = Panel(Window, true);
  ResetDirs(Window, E);
  WriteFile(E.R + QStringLiteral("/edit.txt"), "original\n");
  Refresh(R);
  Activate(Window, R);
  CHECK(R->FocusFile(QStringLiteral("edit.txt")));
  // "Do you want to use '<detected editor>' instead of default internal
  // editor?" (first edit): Cancel keeps the internal editor
  MessageAnswer = qaCancel;
  Key(Qt::Key_F4);
  EditorWindow * Editor = nullptr;
  CHECK(WaitFor([&]()
  {
    for (QWidget * W : QApplication::topLevelWidgets())
    {
      if (auto * Ed = qobject_cast<EditorWindow *>(W)) if (Ed->isVisible() && Ed->IsLoaded()) Editor = Ed;
    }
    return Editor != nullptr;
  }));
  MessageAnswer = qaOK;
  if (Editor == nullptr) return;
  auto * Memo = Editor->findChild<QPlainTextEdit *>();
  CHECK(Memo != nullptr);
  CHECK_MSG(Memo->toPlainText().trimmed() == QStringLiteral("original"), Memo->toPlainText());
  // typed text marks the document modified; Ctrl+S saves and uploads
  Memo->selectAll();
  Memo->insertPlainText(QStringLiteral("changed in editor"));
  CHECK(Editor->IsModified());
  if (!ShotDir.isEmpty()) Editor->grab().save(QDir(ShotDir).filePath(QStringLiteral("editor.png")));
  Editor->activateWindow();
  WaitFor([&]() { return Editor->isActiveWindow(); }, 2000);
  Memo->setFocus();
  Key(Qt::Key_S, Qt::ControlModifier);
  CHECK_MSG(WaitFor([&]() { return ReadFile(E.R + QStringLiteral("/edit.txt")).trimmed() == "changed in editor"; }), QString::fromUtf8(ReadFile(E.R + QStringLiteral("/edit.txt"))));
  CHECK(!Editor->IsModified());
  // the original line ending is kept
  CHECK(ReadFile(E.R + QStringLiteral("/edit.txt")) == "changed in editor\n");
  Editor->close();
  CHECK(WaitFor([&]() { return !Editor->isVisible(); }, 3000));
  Idle(Window);
  // Shift+F4: new remote file, created empty and opened in the editor
  Activate(Window, R);
  Expect(QStringLiteral("QDialog"), [](QWidget * D) { SetComboText(D, QString(), QStringLiteral("brand-new.txt")); Accept(D); });
  Key(Qt::Key_F4, Qt::ShiftModifier);
  CHECK(ScriptDone());
  Editor = nullptr;
  CHECK(WaitFor([&]()
  {
    for (QWidget * W : QApplication::topLevelWidgets())
    {
      if (auto * Ed = qobject_cast<EditorWindow *>(W)) if (Ed->isVisible() && Ed->IsLoaded()) Editor = Ed;
    }
    return Editor != nullptr;
  }));
  if (Editor == nullptr)
  {
    for (QWidget * W : QApplication::topLevelWidgets())
      if (W->isVisible()) std::printf("  visible top-level: %s '%s'\n", qPrintable(ClassOf(W)), qPrintable(W->windowTitle()));
    std::printf("  messages: %s\n", qPrintable(Messages.join(QLatin1Char('|'))));
  }
  // nothing is created on the server before the file is saved (WinSCP NewFile)
  CHECK(!QFileInfo::exists(E.R + QStringLiteral("/brand-new.txt")));
  if (Editor != nullptr)
  {
    Editor->findChild<QPlainTextEdit *>()->insertPlainText(QStringLiteral("new content"));
    CHECK(Editor->Save());
    CHECK(WaitFor([&]() { return ReadFile(E.R + QStringLiteral("/brand-new.txt")) == "new content"; }));
    Editor->close();
  }
  CHECK(WaitFor([&]() { return Listed(R, QStringLiteral("brand-new.txt")); }));
  Idle(Window);
  // Shift+F4 and closing the editor unsaved: no remote file
  Activate(Window, R);
  Expect(QStringLiteral("QDialog"), [](QWidget * D) { SetComboText(D, QString(), QStringLiteral("never-saved.txt")); Accept(D); });
  Key(Qt::Key_F4, Qt::ShiftModifier);
  CHECK(ScriptDone());
  Editor = nullptr;
  CHECK(WaitFor([&]()
  {
    for (QWidget * W : QApplication::topLevelWidgets())
    {
      if (auto * Ed = qobject_cast<EditorWindow *>(W)) if (Ed->isVisible() && Ed->IsLoaded()) Editor = Ed;
    }
    return Editor != nullptr;
  }));
  if (Editor != nullptr) Editor->close();
  Pump(500);
  CHECK(!QFileInfo::exists(E.R + QStringLiteral("/never-saved.txt")));
  Idle(Window);
}

static void TestConsoleCustomCommands(CommanderWindow & Window, const Env & E)
{
  std::printf("[console / custom commands]\n");
  FilePanel * R = Panel(Window, true);
  ResetDirs(Window, E);
  Activate(Window, R);
  QString Output;
  Expect(QStringLiteral("ConsoleDialog"), [&Output](QWidget * D)
  {
    QComboBox * Command = D->findChild<QComboBox *>();
    Command->setCurrentText(QStringLiteral("touch console-made.txt; echo console-ok"));
    for (QPushButton * B : D->findChildren<QPushButton *>()) if (B->text() == QStringLiteral("&Execute")) B->click();
    auto * Memo = D->findChild<QPlainTextEdit *>();
    WaitFor([&]() { return Memo->toPlainText().contains(QStringLiteral("console-ok")) && !qobject_cast<ConsoleDialog *>(D)->Executing(); }, 10000);
    Output = Memo->toPlainText();
    if (!ShotDir.isEmpty()) D->grab().save(QDir(ShotDir).filePath(QStringLiteral("console.png")));
    qobject_cast<QDialog *>(D)->reject();
  });
  Key(Qt::Key_T, Qt::ControlModifier | Qt::ShiftModifier);
  CHECK(ScriptDone(15000));
  CHECK_MSG(Output.contains(QStringLiteral("console-ok")), Output);
  CHECK(QFileInfo::exists(E.R + QStringLiteral("/console-made.txt")));
  CHECK(WaitFor([&]() { return Listed(R, QStringLiteral("console-made.txt")); }));
  Idle(Window);

  // Files > Custom Commands > Touch on a marked file (remote command)
  QDateTime Old = QDateTime::currentDateTime().addDays(-10);
  {
    QFile F(E.R + QStringLiteral("/b.txt"));
    (void)F.open(QIODevice::ReadWrite);
    F.setFileTime(Old, QFileDevice::FileModificationTime);
  }
  Refresh(R);
  Activate(Window, R);
  CHECK(R->FocusFile(QStringLiteral("b.txt")));
  QMenu * FilesMenu = nullptr;
  for (QAction * Top : Window.menuBar()->actions())
  {
    if (Plain(Top->text()) == QStringLiteral("Files")) FilesMenu = Top->menu();
  }
  CHECK(FilesMenu != nullptr);
  QAction * Touch = nullptr;
  if (FilesMenu != nullptr)
  {
    emit FilesMenu->aboutToShow();
    for (QAction * Item : FilesMenu->actions())
    {
      if ((Item->menu() != nullptr) && (Plain(Item->text()) == QStringLiteral("Custom Commands")))
      {
        emit Item->menu()->aboutToShow();
        for (QAction * C : Item->menu()->actions()) if (Plain(C->text()) == QStringLiteral("Touch")) Touch = C;
        QStringList Captions;
        for (QAction * C : Item->menu()->actions()) if (!C->isSeparator()) Captions.append(Plain(C->text()));
        std::printf("  custom commands: %s\n", qPrintable(Captions.join(QStringLiteral(", "))));
      }
    }
  }
  CHECK(Touch != nullptr);
  if (Touch != nullptr)
  {
    CHECK(Touch->isEnabled());
    Touch->trigger();
    CHECK_MSG(WaitFor([&]() { return QFileInfo(E.R + QStringLiteral("/b.txt")).lastModified() > Old.addDays(5); }),
      QFileInfo(E.R + QStringLiteral("/b.txt")).lastModified().toString());
    Idle(Window);
  }
  // Commands > Custom Commands > Enter... (ad hoc), remote command with !
  Activate(Window, R);
  CHECK(R->FocusFile(QStringLiteral("a.txt")));
  Expect(QStringLiteral("CustomCommandDialog"), [](QWidget * D)
  {
    SetComboText(D, QString(), QStringLiteral("cp \"!\" \"!-adhoc\""));
    Accept(D);
  });
  A(Window, "CustomCommandsEnterAction")->trigger();
  CHECK(ScriptDone());
  CHECK(WaitFor([&]() { return ReadFile(E.R + QStringLiteral("/a.txt-adhoc")) == ReadFile(E.R + QStringLiteral("/a.txt")); }));
  CHECK(WaitFor([&]() { return Listed(R, QStringLiteral("a.txt-adhoc")); }));
  Idle(Window);
}

static void TestBookmarks(CommanderWindow & Window, const Env & E)
{
  std::printf("[bookmarks / open directory]\n");
  FilePanel * R = Panel(Window, true);
  ResetDirs(Window, E);
  R->ChangeDirectory(E.R + QStringLiteral("/sub"));
  Loaded(R, E.R + QStringLiteral("/sub"));
  Activate(Window, R);
  // Ctrl+B: Add Path to Bookmarks (ADD_BOOKMARK_CONFIRM, then the "Manage bookmarks" dialog)
  QStringList Items;
  Messages.clear();
  MessageAnswer = qaOK;
  Expect(QStringLiteral("OpenDirectoryDialog"), [&Items](QWidget * D)
  {
    auto * Dialog = qobject_cast<OpenDirectoryDialog *>(D);
    for (bool Shared : {false, true})
    {
      QListWidget * List = Dialog->BookmarksList(Shared);
      for (int I = 0; (List != nullptr) && (I < List->count()); I++) Items.append(List->item(I)->text());
    }
    if (!ShotDir.isEmpty()) D->grab().save(QDir(ShotDir).filePath(QStringLiteral("bookmarks.png")));
    qobject_cast<QDialog *>(D)->reject();
  });
  Key(Qt::Key_B, Qt::ControlModifier);
  CHECK(ScriptDone());
  std::printf("  confirmation: %s\n  bookmarks: %s\n", qPrintable(Messages.value(0).left(100)), qPrintable(Items.join(QStringLiteral(", "))));
  CHECK(Messages.value(0).contains(E.R + QStringLiteral("/sub")));
  CHECK(Items.contains(E.R + QStringLiteral("/sub")));
  Idle(Window);
  // Ctrl+O: open a bookmark / typed directory
  Activate(Window, R);
  Expect(QStringLiteral("OpenDirectoryDialog"), [&E](QWidget * D)
  {
    qobject_cast<OpenDirectoryDialog *>(D)->DirectoryEdit()->setCurrentText(E.R);
    Accept(D);
  });
  Key(Qt::Key_O, Qt::ControlModifier);
  CHECK(ScriptDone());
  CHECK_MSG(Loaded(R, E.R), R->CurrentDirectory());
}

static void TestPopup(CommanderWindow & Window, const Env & E)
{
  std::printf("[file popup]\n");
  FilePanel * R = Panel(Window, true);
  ResetDirs(Window, E);
  WriteFile(E.R + QStringLiteral("/popup-del.txt"), "x");
  Refresh(R);
  Activate(Window, R);
  CHECK(R->FocusFile(QStringLiteral("popup-del.txt")));
  PopupChoice = QStringLiteral("Delete");
  MessageAnswer = qaOK;
  Key(Qt::Key_F10, Qt::ShiftModifier);
  CHECK(WaitFor([&]() { return !QFileInfo::exists(E.R + QStringLiteral("/popup-del.txt")); }));
  std::printf("  popup: %s\n", qPrintable(PopupItems.join(QStringLiteral(" | "))));
  for (const char * Expected : {"Open", "Edit", "Download...", "Delete", "Rename", "Properties"})
  {
    CHECK_MSG(PopupItems.contains(QString::fromLatin1(Expected)), QString::fromLatin1(Expected));
  }
  Idle(Window);
}

static void TestSynchronize(CommanderWindow & Window, const Env & E)
{
  std::printf("[synchronize]\n");
  FilePanel * R = Panel(Window, true);
  FilePanel * L = Panel(Window, false);
  QString SR = E.Root + QStringLiteral("/sync-r");
  QString SL = E.Root + QStringLiteral("/sync-l");
  WriteFile(SL + QStringLiteral("/s1.txt"), "s1");
  WriteFile(SL + QStringLiteral("/sd/s2.txt"), "s2");
  QDir().mkpath(SR);
  R->ChangeDirectory(SR);
  L->ChangeDirectory(SL);
  Loaded(R, SR);
  Loaded(L, SL);
  Activate(Window, L);
  QStringList Seen;
  // Synchronize dialog -> checklist -> done
  Expect(QStringLiteral("QDialog:Synchronize"), [&Seen](QWidget * D) { Seen.append(D->windowTitle()); Accept(D); });
  Expect(QStringLiteral("QDialog:Synchronization checklist"), [&Seen](QWidget * D)
  {
    Seen.append(D->windowTitle());
    if (!ShotDir.isEmpty()) D->grab().save(QDir(ShotDir).filePath(QStringLiteral("sync-checklist.png")));
    Accept(D);
  });
  Key(Qt::Key_S, Qt::ControlModifier);
  ScriptDone(15000);
  std::printf("  dialogs: %s; unexpected: %s\n", qPrintable(Seen.join(QStringLiteral(", "))), qPrintable(Unexpected.join(QStringLiteral(", "))));
  CHECK(WaitFor([&]() { return (ReadFile(SR + QStringLiteral("/s1.txt")) == "s1") && (ReadFile(SR + QStringLiteral("/sd/s2.txt")) == "s2"); }, 20000));
  CHECK(WaitFor([&]() { return Listed(R, QStringLiteral("s1.txt")) && Listed(R, QStringLiteral("sd")); }));
  Idle(Window);
}

static void TestWorkspace(CommanderWindow & Window, const Env & E)
{
  std::printf("[workspace]\n");
  ResetDirs(Window, E);
  Expect(QStringLiteral("SaveWorkspaceDialog"), [](QWidget * D) { SetComboText(D, QString(), QStringLiteral("verify-ws")); Accept(D); });
  A(Window, "SaveWorkspaceAction")->trigger();
  CHECK(ScriptDone());
  QFile Ini(Configuration::Instance().IniFileName());
  CHECK(Ini.open(QIODevice::ReadOnly));
  QString Content = QString::fromUtf8(Ini.readAll());
  CHECK_MSG(Content.contains(QStringLiteral("verify-ws")), QStringLiteral("no workspace in ini"));
  QTabBar * Bar = Tabs(Window);
  int Before = Bar->count();
  Window.OpenWorkspace(QStringLiteral("verify-ws"));
  CHECK_MSG(WaitFor([&]() { return Bar->count() == Before + 1; }, 10000), QString::number(Bar->count()));
  FilePanel * R = Panel(Window, true);
  CHECK_MSG(WaitFor([&]() { R = Panel(Window, true); return (R->Provider() != nullptr) && !R->IsLoading() && (R->CurrentDirectory() == E.R); }, 30000),
    R->CurrentDirectory());
  CHECK(Panel(Window, false)->CurrentDirectory() == E.L);
  Shot(Window, QStringLiteral("workspace"));
  A(Window, "CloseTabAction")->trigger();
  CHECK(WaitFor([&]() { return Bar->count() == Before; }, 5000));
}

static QPushButton * ButtonByText(QWidget * Parent, const QString & Text)
{
  for (QPushButton * B : Parent->findChildren<QPushButton *>())
  {
    if (B->text() == Text) return B;
  }
  return nullptr;
}

static void TestMisc(CommanderWindow & Window, const Env & E)
{
  std::printf("[hidden files / address bar / synchronized browsing / keep up to date / reconnect]\n");
  FilePanel * R = Panel(Window, true);
  FilePanel * L = Panel(Window, false);
  ResetDirs(Window, E);
  WriteFile(E.R + QStringLiteral("/.hidden-file"), "h");
  Refresh(R);
  bool Shown = Configuration::Instance().ShowHiddenFiles;
  CHECK(Listed(R, QStringLiteral(".hidden-file")) == Shown);
  Activate(Window, R);
  Key(Qt::Key_H, Qt::ControlModifier | Qt::AltModifier);
  CHECK(WaitFor([&]() { return !R->IsLoading() && (Listed(R, QStringLiteral(".hidden-file")) != Shown); }));
  CHECK(A(Window, "ShowHiddenFilesAction")->isChecked() != Shown);
  Key(Qt::Key_H, Qt::ControlModifier | Qt::AltModifier);
  CHECK(WaitFor([&]() { return !R->IsLoading() && (Listed(R, QStringLiteral(".hidden-file")) == Shown); }));

  std::printf("  step: %s\n", "Ctrl+L: the path box");
  // Ctrl+L: the path box of the current panel; a typed path + Enter opens it
  Activate(Window, R);
  Key(Qt::Key_L, Qt::ControlModifier);
  QWidget * Focus = QApplication::focusWidget();
  auto * PathEdit = qobject_cast<QLineEdit *>(Focus);
  if ((PathEdit == nullptr) && (qobject_cast<QComboBox *>(Focus) != nullptr)) PathEdit = qobject_cast<QComboBox *>(Focus)->lineEdit();
  CHECK_MSG((PathEdit != nullptr) && R->isAncestorOf(PathEdit), (Focus != nullptr) ? ClassOf(Focus) : QStringLiteral("none"));
  if (PathEdit != nullptr)
  {
    PathEdit->setText(E.R + QStringLiteral("/sub"));
    Key(Qt::Key_Return);
    CHECK_MSG(Loaded(R, E.R + QStringLiteral("/sub")), R->CurrentDirectory());
  }
  R->ChangeDirectory(E.R);
  Loaded(R, E.R);

  std::printf("  step: %s\n", "Ctrl+Alt+B synchronize");
  // Ctrl+Alt+B synchronized browsing: entering "sub" remotely enters "sub" locally
  QDir().mkpath(E.L + QStringLiteral("/sub"));
  Refresh(L);
  Activate(Window, R);
  Key(Qt::Key_B, Qt::ControlModifier | Qt::AltModifier);
  CHECK(A(Window, "SynchronizeBrowsingAction2")->isChecked());
  CHECK(R->FocusFile(QStringLiteral("sub")));
  Activate(Window, R);
  Key(Qt::Key_Return);
  CHECK(Loaded(R, E.R + QStringLiteral("/sub")));
  CHECK_MSG(WaitFor([&]() { return L->CurrentDirectory() == E.L + QStringLiteral("/sub"); }), L->CurrentDirectory());
  Key(Qt::Key_Backspace);
  CHECK_MSG(WaitFor([&]() { return (R->CurrentDirectory() == E.R) && (L->CurrentDirectory() == E.L); }), L->CurrentDirectory());
  Key(Qt::Key_B, Qt::ControlModifier | Qt::AltModifier);
  CHECK(!A(Window, "SynchronizeBrowsingAction2")->isChecked());
  Idle(Window);

  std::printf("  step: %s\n", "Ctrl+U Keep remote");
  // Ctrl+U Keep remote directory up to date: a local change is uploaded
  QString KR = E.Root + QStringLiteral("/keep-r");
  QString KL = E.Root + QStringLiteral("/keep-l");
  QDir().mkpath(KR);
  QDir().mkpath(KL);
  R->ChangeDirectory(KR);
  L->ChangeDirectory(KL);
  Loaded(R, KR);
  Loaded(L, KL);
  Activate(Window, L);
  bool Uploaded = false;
  Expect(QStringLiteral("QDialog:Keep remote directory up to date"), [&](QWidget * D)
  {
    QPushButton * Start = ButtonByText(D, QStringLiteral("&Start"));
    CHECK(Start != nullptr);
    if (Start == nullptr) { qobject_cast<QDialog *>(D)->reject(); return; }
    std::printf("  keep: start\n");
    Start->click();
    std::printf("  keep: clicked, title '%s'\n", qPrintable(D->windowTitle()));
    WaitFor([&]() { return D->windowTitle().startsWith(QStringLiteral("Keeping")); }, 10000);
    std::printf("  keep: title '%s'\n", qPrintable(D->windowTitle()));
    Pump(1500);
    WriteFile(KL + QStringLiteral("/kept.txt"), "kept");
    Uploaded = WaitFor([&]() { return ReadFile(KR + QStringLiteral("/kept.txt")) == "kept"; }, 20000);
    if (!ShotDir.isEmpty()) D->grab().save(QDir(ShotDir).filePath(QStringLiteral("keep-up-to-date.png")));
    std::printf("  keep: uploaded %d\n", int(Uploaded));
    if (QPushButton * Stop = ButtonByText(D, QStringLiteral("&Stop"))) Stop->click();
    std::printf("  keep: stopped\n");
    Pump(300);
    if (QPushButton * Close = ButtonByText(D, QStringLiteral("Close"))) Close->click();
    else qobject_cast<QDialog *>(D)->reject();
  });
  Key(Qt::Key_U, Qt::ControlModifier);
  CHECK(ScriptDone(40000));
  CHECK(Uploaded);
  Idle(Window);

  std::printf("  step: %s\n", "Disconnect + Ctrl+Shif");
  // Disconnect + Ctrl+Shift+R reconnect
  ResetDirs(Window, E);
  A(Window, "DisconnectSessionAction")->trigger();
  CHECK(WaitFor([&]() { return Panel(Window, true)->Provider() == nullptr; }));
  Shot(Window, QStringLiteral("disconnected"));
  Activate(Window, Panel(Window, false));
  Key(Qt::Key_R, Qt::ControlModifier | Qt::ShiftModifier);
  CHECK_MSG(WaitFor([&]() { R = Panel(Window, true); return (R->Provider() != nullptr) && !R->IsLoading() && (R->CurrentDirectory() == E.R); }, 30000),
    Panel(Window, true)->CurrentDirectory());
}

int main(int argc, char ** argv)
{
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
  {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  // never start anything (external editor, file manager) on the user's desktop
  qunsetenv("DISPLAY");
  qunsetenv("WAYLAND_DISPLAY");
  QTemporaryDir Scratch;
  QString ConfigHome = Scratch.filePath(QStringLiteral("xdg-config"));
  QDir().mkpath(ConfigHome);
  qputenv("XDG_CONFIG_HOME", ConfigHome.toUtf8());
  qputenv("XDG_DATA_HOME", Scratch.filePath(QStringLiteral("xdg-data")).toUtf8());
  QDir().mkpath(Scratch.filePath(QStringLiteral("xdg-data")));
  QApplication App(argc, argv);
  setvbuf(stdout, nullptr, _IONBF, 0);
  ShotDir = qEnvironmentVariable("NIXSCP_TEST_SCREENSHOT");
  if (!ShotDir.isEmpty()) QDir().mkpath(ShotDir);
  if (qEnvironmentVariableIsEmpty("NIXSCP_TEST_SSH_PORT") || qEnvironmentVariableIsEmpty("NIXSCP_TEST_SSH_KEY"))
  {
    std::printf("SKIPPED (set NIXSCP_TEST_SSH_PORT, NIXSCP_TEST_SSH_KEY)\n");
    return 0;
  }
  Configuration & Config = Configuration::Instance();
  Config.SetIniFileName(Scratch.filePath(QStringLiteral("nixscp.ini")));
  Config.Load();
  Config.ConfirmClosingSession = false;
  Config.TemporaryDirectoryCleanup = false;
  Config.BeepOnFinish = false;
  Config.DeleteToRecycleBin = false;
  Config.QueueAutoPopup = false;
  InstallModalHandler();
  InstallPopupHandler();

  Env E;
  E.Root = QDir(Scratch.path()).filePath(QStringLiteral("files"));
  E.R = E.Root + QStringLiteral("/remote");
  E.L = E.Root + QStringLiteral("/local");
  for (const QString & Dir : {E.R, E.L})
  {
    for (const char * Name : {"a.txt", "b.txt", "c.log", "d.log", "torename.txt", "todelete.txt"})
    {
      WriteFile(Dir + QLatin1Char('/') + QString::fromLatin1(Name), QByteArray(Name));
    }
  }
  WriteFile(E.R + QStringLiteral("/sub/deep.txt"), "deep");
  WriteFile(E.L + QStringLiteral("/ldir/inner.txt"), "inner");

  CommanderWindow Window;
  MainWindow = &Window;
  Window.resize(1200, 800);
  Window.show();
  Pump(200);
  if (!Connect(Window, E))
  {
    CHECK(false);
    std::printf("%d checks, %d failures\n", Checks, Failures);
    return 1;
  }
  Shot(Window, QStringLiteral("connected"));
  const QStringList Only = qEnvironmentVariable("NIXSCP_VERIFY_ONLY").split(QLatin1Char(','), Qt::SkipEmptyParts);
  auto Run = [&](const char * Name, const std::function<void()> & Func)
  {
    if (!Only.isEmpty() && !Only.contains(QString::fromLatin1(Name))) return;
    Unexpected.clear();
    Func();
    ModalScript.clear();
    MessageAnswer = qaOK;
    if (!Unexpected.isEmpty()) std::printf("  unexpected dialogs in %s: %s\n", Name, qPrintable(Unexpected.join(QStringLiteral(", "))));
  };
  Run("navigation", [&]() { TestNavigation(Window, E); });
  Run("marking", [&]() { TestMarking(Window, E); });
  Run("crd", [&]() { TestCreateRenameDelete(Window, E); });
  Run("transfers", [&]() { TestTransfers(Window, E); });
  Run("remoteops", [&]() { TestRemoteOps(Window, E); });
  Run("clipboard", [&]() { TestClipboard(Window, E); });
  Run("dragdrop", [&]() { TestDragDrop(Window, E); });
  Run("tabs", [&]() { TestTabs(Window, E); });
  Run("queue", [&]() { TestQueue(Window, E); });
  Run("compare", [&]() { TestCompareFilter(Window, E); });
  Run("find", [&]() { TestFindFiles(Window, E); });
  Run("editor", [&]() { TestEditor(Window, E); });
  Run("console", [&]() { TestConsoleCustomCommands(Window, E); });
  Run("bookmarks", [&]() { TestBookmarks(Window, E); });
  Run("popup", [&]() { TestPopup(Window, E); });
  Run("synchronize", [&]() { TestSynchronize(Window, E); });
  Run("workspace", [&]() { TestWorkspace(Window, E); });
  Run("misc", [&]() { TestMisc(Window, E); });
  Shot(Window, QStringLiteral("final"));
  if (Only.isEmpty() || Only.contains(QStringLiteral("quit")))
  {
    // F10 Quit (ConfirmClosingSession is off here)
    std::printf("[quit]\n");
    Activate(Window, Panel(Window, false));
    Key(Qt::Key_F10);
    CHECK(WaitFor([&]() { return !Window.isVisible(); }, 10000));
  }
  std::printf("%d checks, %d failures\n", Checks, Failures);
  Window.close();
  Pump(200);
  return (Failures == 0) ? 0 : 1;
}

#endif
