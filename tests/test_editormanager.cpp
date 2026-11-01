// Tests of gui/EditorManager (windows/EditorManager.cpp + the edit parts of
// forms/CustomScpExplorer.cpp) and gui/EditorWindow (forms/Editor.cpp):
// editor command arguments without a shell (no command injection through
// file names), shell-safe FormatEditorCommand, private unique temporary
// folders and their startup cleanup, internal editor preferences, shortcuts,
// Reload/Save All/backup on forced close, Edit actions and the editor list
// menu (NIXSCP_GUI_TESTS, offscreen).
//
// The remote part (download, upload on save, reuse of an open editor,
// EditorCheckNotModified, Reload downloading again, tracked external editor
// process, early close, re-attach to another session, PENDING_EDITORS) needs
// the test SSH server: NIXSCP_TEST_SSH_PORT and NIXSCP_TEST_SSH_KEY (see
// tests/test_sftp.cpp); it assumes the server runs on this machine.
#include "core/Configuration.h"
#include "core/IniStorage.h"
#include "core/SessionData.h"
#include "core/Terminal.h"
#include "gui/EditorManager.h"
#include "gui/EditorWindow.h"
#include "gui/GuiUserInterface.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QLineEdit>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

static int Failures = 0;

#define CHECK(Cond) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); std::fflush(stdout); Failures++; } } while (0)
#define CHECK_EQ(A, B) \
  do { auto VA = (A); auto VB = (B); if (!(VA == VB)) { std::printf("FAIL %s:%d: %s == %s (\"%s\" vs \"%s\")\n", __FILE__, __LINE__, #A, #B, \
    qPrintable(QVariant::fromValue(VA).toString()), qPrintable(QVariant::fromValue(VB).toString())); std::fflush(stdout); Failures++; } } while (0)

static void WriteFile(const QString & Path, const QByteArray & Data)
{
  QDir().mkpath(QFileInfo(Path).absolutePath());
  QFile File(Path);
  CHECK(File.open(QIODevice::WriteOnly | QIODevice::Truncate));
  File.write(Data);
  File.close();
}

static QByteArray ReadFile(const QString & Path)
{
  QFile File(Path);
  if (!File.open(QIODevice::ReadOnly))
  {
    return QByteArray();
  }
  return File.readAll();
}

static void SetAge(const QString & Path, int Seconds)
{
  struct timeval Times[2];
  Times[0].tv_sec = time(nullptr) - Seconds;
  Times[0].tv_usec = 0;
  Times[1] = Times[0];
  utimes(QFile::encodeName(Path).constData(), Times);
}

// Processes events until Cond or Timeout (ms)
template<typename T>
static bool WaitFor(T Cond, int Timeout = 15000)
{
  QElapsedTimer Timer;
  Timer.start();
  while (!Cond())
  {
    if (Timer.elapsed() > Timeout)
    {
      return false;
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    usleep(5000);
  }
  return true;
}

// Answers the next message dialog (MessageForm) with Answer; Seen counts the dialogs
struct AutoAnswer
{
  QTimer Timer;
  unsigned int Answer;
  int Seen = 0;
  QString LastText;

  explicit AutoAnswer(unsigned int AAnswer) : Answer(AAnswer)
  {
    Timer.setInterval(30);
    QObject::connect(&Timer, &QTimer::timeout, [this]()
    {
      if (auto * Form = qobject_cast<MessageForm *>(QApplication::activeModalWidget()))
      {
        Seen++;
        LastText = Form->FormText();
        Form->SubmitAnswer(Answer);
      }
    });
    Timer.start();
  }
};

// Replaces the text as typing would (the document becomes modified)
static void SetText(QPlainTextEdit * Memo, const QString & Text)
{
  Memo->selectAll();
  Memo->insertPlainText(Text);
}

static QAction * FindAction(QWidget * Window, const QString & Text)
{
  for (QAction * Action : Window->findChildren<QAction *>())
  {
    if (Action->text() == Text)
    {
      return Action;
    }
  }
  return nullptr;
}

static void SetEditorIni(const QString & Name, const QString & Value)
{
  IniStorage Storage(&Configuration::Instance().Ini());
  Storage.OpenSubKey(QStringLiteral("Configuration"), true);
  Storage.OpenSubKey(QStringLiteral("Interface"), true);
  Storage.OpenSubKey(QStringLiteral("Editor"), true);
  Storage.WriteStringRaw(Name, Value);
}

//---------------------------------------------------------------------------
static void TestCommandArguments(const QString & Scratch)
{
  std::printf("[arguments]\n");
  QString Name = QStringLiteral("/tmp/a$(touch PWNED).txt");
  CHECK_EQ(EditorManager::EditorCommandArguments(QStringLiteral("gedit !.!"), Name), (QStringList{QStringLiteral("gedit"), Name}));
  CHECK_EQ(EditorManager::EditorCommandArguments(QStringLiteral("kate"), QStringLiteral("/x/`id`.txt")),
    (QStringList{QStringLiteral("kate"), QStringLiteral("/x/`id`.txt")}));
  CHECK_EQ(EditorManager::EditorCommandArguments(QStringLiteral("code \"!.!\" -w"), QStringLiteral("/t/a b")),
    (QStringList{QStringLiteral("code"), QStringLiteral("/t/a b"), QStringLiteral("-w")}));
  CHECK_EQ(EditorManager::EditorCommandArguments(QStringLiteral("'/opt/My Ed/ed' --file='!.!' x\\ y"), QStringLiteral("/f")),
    (QStringList{QStringLiteral("/opt/My Ed/ed"), QStringLiteral("--file=/f"), QStringLiteral("x y")}));
  CHECK_EQ(EditorManager::EditorCommandArguments(QStringLiteral("ed %1"), QStringLiteral("/f")),
    (QStringList{QStringLiteral("ed"), QStringLiteral("/f")}));
  CHECK_EQ(EditorManager::EditorCommandArguments(QStringLiteral("$HOME/bin/ed !.!"), QStringLiteral("/f")).first(),
    qEnvironmentVariable("HOME") + QStringLiteral("/bin/ed"));
  CHECK(EditorManager::EditorCommandArguments(QStringLiteral("   "), QStringLiteral("/f")).isEmpty());

  // FormatEditorCommand (display form) is safe for a shell, too
  for (const QString & Evil : {QStringLiteral("a$(touch PWNED).txt"), QStringLiteral("b`touch PWNED`.txt"),
         QStringLiteral("c\"; touch PWNED; \".txt"), QStringLiteral("d'quote'.txt"), QStringLiteral("e\\x!y.txt"), QStringLiteral("plain name.txt")})
  {
    QString Path = Scratch + QLatin1Char('/') + Evil;
    QString Line = Configuration::FormatEditorCommand(QStringLiteral("printf %s !.!"), Path);
    QProcess Shell;
    Shell.setWorkingDirectory(Scratch);
    Shell.start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), Line});
    CHECK(Shell.waitForFinished(5000));
    CHECK_EQ(QString::fromLocal8Bit(Shell.readAllStandardOutput()), Path);
  }
  CHECK(!QFileInfo::exists(Scratch + QStringLiteral("/PWNED")));
  CHECK_EQ(Configuration::FormatEditorCommand(QStringLiteral("gedit !.!"), QStringLiteral("/tmp/a b.txt")), QStringLiteral("gedit \"/tmp/a b.txt\""));
  CHECK_EQ(Configuration::FormatEditorCommand(QStringLiteral("gedit '!.!'"), QStringLiteral("/tmp/$x")), QStringLiteral("gedit '/tmp/$x'"));
  // the name is inserted once, never expanded again
  CHECK_EQ(Configuration::FormatEditorCommand(QStringLiteral("ed !.!"), QStringLiteral("/t/!.!")), QStringLiteral("ed '/t/!.!'"));

  // A local file with a hostile name, opened in an "external editor": no shell runs
  QString Hostile = Scratch + QStringLiteral("/x$(touch PWNED2)`touch PWNED3`.txt");
  WriteFile(Hostile, "data");
  EditorPreferences Touch;
  Touch.Editor = edExternal;
  Touch.ExternalEditor = QStringLiteral("touch !.!.seen");
  QWidget Window;
  EditorManager Manager(&Window);
  Manager.ExecuteFiles(nullptr, {Hostile}, EditorManager::efExternalEditor, &Touch);
  CHECK(WaitFor([&]() { return QFileInfo::exists(Hostile + QStringLiteral(".seen")); }, 5000));
  CHECK(!QFileInfo::exists(Scratch + QStringLiteral("/PWNED2")));
  CHECK(!QFileInfo::exists(Scratch + QStringLiteral("/PWNED3")));
  CHECK(!QFileInfo::exists(QDir::currentPath() + QStringLiteral("/PWNED2")));
}

//---------------------------------------------------------------------------
static void TestTemporaryFolders(const QString & Scratch)
{
  std::printf("[temporary folders]\n");
  QString Runtime = Scratch + QStringLiteral("/runtime");
  QDir().mkpath(Runtime);
  QFile::setPermissions(Runtime, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
  qputenv("XDG_RUNTIME_DIR", Runtime.toUtf8());
  Configuration::Instance().DDTemporaryDirectory.clear();
  CHECK_EQ(EditorManager::TemporaryBaseDirectory(), Runtime);
  CHECK(EditorManager::TemporaryBaseDirectories().contains(QFileInfo(Runtime).canonicalFilePath()));

  std::unique_ptr<QLockFile> Lock;
  QString Dir = EditorManager::CreateUniqueTemporaryDirectory(Runtime, &Lock);
  CHECK(!Dir.isEmpty());
  CHECK(QFileInfo(Dir).fileName().startsWith(QStringLiteral("nixscp-")));
  CHECK(QFileInfo(Dir).fileName().size() == 13);
  CHECK(Lock != nullptr);
  CHECK(QFileInfo::exists(Dir + QLatin1Char('/') + EditorManager::LockFileName()));
  struct stat St;
  CHECK(::stat(QFile::encodeName(Dir).constData(), &St) == 0);
  CHECK_EQ(static_cast<int>(St.st_mode & 0777), 0700);
  QString Dir2 = EditorManager::CreateUniqueTemporaryDirectory(Runtime);
  CHECK(!Dir2.isEmpty() && (Dir2 != Dir));

  // stale: old folder without lock, nixscp-<dead pid>, folder with a stale lock file
  QString Stale = Runtime + QStringLiteral("/nixscp-AbCdEf");
  QDir().mkpath(Stale);
  WriteFile(Stale + QStringLiteral("/edited.txt"), "x");
  SetAge(Stale, 3600);
  QString OldStyle = Runtime + QStringLiteral("/nixscp-999999");
  QDir().mkpath(OldStyle);
  QString StaleLock = Runtime + QStringLiteral("/nixscp-ZZZZZZ");
  QDir().mkpath(StaleLock);
  // QLockFile format: pid, application name, host name
  WriteFile(StaleLock + QLatin1Char('/') + EditorManager::LockFileName(),
    QByteArray("999998\nnixscp\n") + QSysInfo::machineHostName().toUtf8() + "\n");
  QString Foreign = Runtime + QStringLiteral("/other-AbCdEf");
  QDir().mkpath(Foreign);
  SetAge(Foreign, 3600);

  QStringList Found = EditorManager::FindTemporaryFolders();
  CHECK(Found.contains(QFileInfo(Stale).canonicalFilePath()) || Found.contains(Stale));
  auto Contains = [&Found](const QString & Path)
  {
    for (const QString & F : Found)
    {
      if (QFileInfo(F).canonicalFilePath() == QFileInfo(Path).canonicalFilePath())
      {
        return true;
      }
    }
    return false;
  };
  CHECK(Contains(Stale));
  CHECK(Contains(OldStyle));
  CHECK(Contains(StaleLock));
  CHECK(!Contains(Dir));          // locked by this instance
  CHECK(!Contains(Dir2));         // just created (no lock yet)
  CHECK(!Contains(Foreign));
  CHECK(EditorManager::CleanupTemporaryFolders(Found));
  CHECK(!QFileInfo::exists(Stale));
  CHECK(QFileInfo::exists(Dir));

  // startup cleanup: confirmation (No keeps, Yes deletes), switched off
  QDir().mkpath(Stale);
  SetAge(Stale, 3600);
  Configuration::Instance().TemporaryDirectoryCleanup = true;
  Configuration::Instance().ConfirmTemporaryDirectoryCleanup = true;
  {
    AutoAnswer No(qaNo);
    EditorManager::TemporaryDirectoryCleanup(nullptr);
    CHECK_EQ(No.Seen, 1);
    CHECK(No.LastText.contains(QStringLiteral("Do you want to delete past temporary folders?")));
    CHECK(QFileInfo::exists(Stale));
  }
  {
    AutoAnswer Yes(qaYes);
    EditorManager::TemporaryDirectoryCleanup(nullptr);
    CHECK_EQ(Yes.Seen, 1);
    CHECK(!QFileInfo::exists(Stale));
  }
  QDir().mkpath(Stale);
  SetAge(Stale, 3600);
  Configuration::Instance().TemporaryDirectoryCleanup = false;
  EditorManager::TemporaryDirectoryCleanup(nullptr);
  CHECK(QFileInfo::exists(Stale));
  Configuration::Instance().TemporaryDirectoryCleanup = true;
  Configuration::Instance().ConfirmTemporaryDirectoryCleanup = false;
  EditorManager::TemporaryDirectoryCleanup(nullptr);
  CHECK(!QFileInfo::exists(Stale));
  Lock.reset();
  QDir(Dir).removeRecursively();
  QDir(Dir2).removeRecursively();

  // custom temporary directory (Preferences > Storage)
  Configuration::Instance().DDTemporaryDirectory = Scratch + QStringLiteral("/custom/");
  CHECK_EQ(EditorManager::TemporaryBaseDirectory(), Scratch + QStringLiteral("/custom"));
  Configuration::Instance().DDTemporaryDirectory.clear();
}

//---------------------------------------------------------------------------
static void TestEditorWindow(const QString & Scratch)
{
  std::printf("[internal editor]\n");
  Configuration & Config = Configuration::Instance();
  QString Path = Scratch + QStringLiteral("/edit.txt");
  WriteFile(Path, "line1\nline2\n");
  Config.EditorWordWrap = true;
  Config.EditorTabSize = 4;
  Config.EditorFontSize = 15;
  SetEditorIni(QStringLiteral("FontColor"), QString::number(0x0000FF));        // red (BGR)
  SetEditorIni(QStringLiteral("BackgroundColor"), QString::number(0x00FFFF));  // yellow
  auto * Editor = new EditorWindow(Path, QStringLiteral("/remote/edit.txt - site"));
  CHECK(Editor->IsLoaded());
  QPlainTextEdit * Memo = Editor->findChild<QPlainTextEdit *>();
  CHECK(Memo != nullptr);
  CHECK(Memo->lineWrapMode() == QPlainTextEdit::WidgetWidth);
  CHECK_EQ(Memo->font().pointSize(), 15);
  CHECK(Memo->palette().color(QPalette::Text) == QColor(255, 0, 0));
  CHECK(Memo->palette().color(QPalette::Base) == QColor(255, 255, 0));
  CHECK(qAbs(Memo->tabStopDistance() - QFontMetricsF(Memo->font()).horizontalAdvance(QLatin1Char(' ')) * 4) < 0.5);
  // preferences change -> re-applied
  Config.EditorWordWrap = false;
  Config.EditorFontSize = 11;
  Editor->ApplyConfiguration();
  CHECK(Memo->lineWrapMode() == QPlainTextEdit::NoWrap);
  CHECK_EQ(Memo->font().pointSize(), 11);
  // session colour overrides the background
  Editor->SetBackgroundColor(QColor(0, 0, 128));
  CHECK(Memo->palette().color(QPalette::Base) == QColor(0, 0, 128));
  Editor->SetBackgroundColor(QColor());
  CHECK(Memo->palette().color(QPalette::Base) == QColor(255, 255, 0));
  CHECK(Editor->windowTitle().startsWith(QStringLiteral("/remote/edit.txt - site")));

  // shortcuts (Editor.dfm with SecondaryShortCuts)
  auto HasShortcut = [Editor](const QString & Text, const QKeySequence & Key)
  {
    QAction * Action = FindAction(Editor, Text);
    return (Action != nullptr) && Action->shortcuts().contains(Key);
  };
  CHECK(HasShortcut(QStringLiteral("&Save"), QKeySequence(Qt::Key_F2)));
  CHECK(HasShortcut(QStringLiteral("&Save"), QKeySequence(Qt::CTRL | Qt::Key_S)));
  CHECK(HasShortcut(QStringLiteral("Save &All"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S)));
  CHECK(HasShortcut(QStringLiteral("&Reload"), QKeySequence(Qt::Key_F5)));
  CHECK(HasShortcut(QStringLiteral("&Reload"), QKeySequence(Qt::CTRL | Qt::Key_R)));
  CHECK(HasShortcut(QStringLiteral("&Find..."), QKeySequence(Qt::Key_F7)));
  CHECK(HasShortcut(QStringLiteral("&Replace..."), QKeySequence(Qt::CTRL | Qt::Key_F7)));
  CHECK(HasShortcut(QStringLiteral("Find &Next"), QKeySequence(Qt::SHIFT | Qt::Key_F7)));
  CHECK(HasShortcut(QStringLiteral("&Go to Line Number..."), QKeySequence(Qt::ALT | Qt::Key_F8)));
  CHECK(HasShortcut(QStringLiteral("&Go to Line Number..."), QKeySequence(Qt::CTRL | Qt::Key_G)));
  CHECK(FindAction(Editor, QStringLiteral("&Preferences...")) != nullptr);
  CHECK(FindAction(Editor, QStringLiteral("C&olor")) != nullptr);
  CHECK(FindAction(Editor, QStringLiteral("&Help")) != nullptr);

  // Reload with a handler (remote files download again first)
  int ReloadCalls = 0;
  Editor->SetReloadHandler([&](std::function<void(bool)> Done)
  {
    ReloadCalls++;
    WriteFile(Path, "downloaded again\n");
    Done(true);
  });
  Editor->Reload();
  CHECK_EQ(ReloadCalls, 1);
  CHECK_EQ(Memo->toPlainText(), QStringLiteral("downloaded again"));
  // a failed download keeps the text
  Editor->SetReloadHandler([&](std::function<void(bool)> Done) { ReloadCalls++; WriteFile(Path, "other\n"); Done(false); });
  Editor->Reload();
  CHECK_EQ(Memo->toPlainText(), QStringLiteral("downloaded again"));

  // Save All
  QString Path2 = Scratch + QStringLiteral("/edit2.txt");
  WriteFile(Path2, "second\n");
  auto * Editor2 = new EditorWindow(Path2, QString());
  CHECK(EditorWindow::Instances().contains(Editor) && EditorWindow::Instances().contains(Editor2));
  SetText(Memo, QStringLiteral("changed 1"));
  SetText(Editor2->findChild<QPlainTextEdit *>(), QStringLiteral("changed 2"));
  int Saves = 0;
  QObject::connect(Editor, &EditorWindow::Saved, [&]() { Saves++; });
  QObject::connect(Editor2, &EditorWindow::Saved, [&]() { Saves++; });
  QAction * SaveAll = FindAction(Editor, QStringLiteral("Save &All"));
  CHECK(SaveAll != nullptr);
  SaveAll->trigger();
  CHECK_EQ(Saves, 2);
  CHECK_EQ(ReadFile(Path), QByteArray("changed 1\n"));
  CHECK_EQ(ReadFile(Path2), QByteArray("changed 2\n"));

  // Forced close: unsaved changes go to <file>.bak
  SetText(Memo, QStringLiteral("unsaved"));
  WriteFile(Path + QStringLiteral(".bak"), "older backup");
  int Closed = 0;
  QObject::connect(Editor, &EditorWindow::Closed, [&]() { Closed++; });
  Editor->ForceClose();
  CHECK_EQ(Closed, 1);
  CHECK_EQ(ReadFile(Path), QByteArray("changed 1\n"));
  CHECK_EQ(ReadFile(Path + QStringLiteral(".bak1")), QByteArray("unsaved\n"));
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  CHECK(!EditorWindow::Instances().contains(Editor));
  Editor2->close();
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

  // Default encoding (Preferences, Editor\Encoding) and site override (InternalEditorEncoding)
  QString Utf8 = Scratch + QStringLiteral("/utf8.txt");
  WriteFile(Utf8, "\xC5\xA1\n");
  QString Latin = Scratch + QStringLiteral("/latin.txt");
  WriteFile(Latin, "caf\xE9\n");
  SetEditorIni(QStringLiteral("WarnOnEncodingFallback"), QStringLiteral("0"));
  {
    EditorWindow E(Utf8, QString());
    CHECK(E.FileEncoding() == EditorWindow::encUTF8);
  }
  {
    EditorWindow E(Utf8, QString(), nullptr, EditorWindow::CodePageSystem);
    CHECK(E.FileEncoding() == EditorWindow::encSystem);
  }
  {
    EditorWindow E(Latin, QString());
    CHECK(E.IsLoaded());
    CHECK(E.FileEncoding() != EditorWindow::encUTF8);
    CHECK(E.findChild<QPlainTextEdit *>()->toPlainText() == QStringLiteral("café"));
  }
  SetEditorIni(QStringLiteral("Encoding"), QString::number(EditorWindow::CodePageSystem));
  {
    EditorWindow E(Utf8, QString());
    CHECK(E.FileEncoding() == EditorWindow::encSystem);
  }
  {
    EditorWindow E(Utf8, QString(), nullptr, EditorWindow::CodePageUTF8);
    CHECK(E.FileEncoding() == EditorWindow::encUTF8);
  }
  SetEditorIni(QStringLiteral("Encoding"), QString::number(EditorWindow::CodePageUTF8));
  SetEditorIni(QStringLiteral("FontColor"), QStringLiteral("0"));
  SetEditorIni(QStringLiteral("BackgroundColor"), QStringLiteral("0"));

  // large file warning follows Preferences (WarnOrLargeFileSize/LargeFileSize KB)
  QString Large = Scratch + QStringLiteral("/large.txt");
  WriteFile(Large, QByteArray(3 * 1024, 'a'));
  Config.EditorWarnOrLargeFileSize = true;
  Config.EditorLargeFileSize = 2;
  {
    AutoAnswer Cancel(qaCancel);
    QTimer Fallback;
    // QMessageBox (not a MessageForm): cancel it
    Fallback.setInterval(30);
    int Boxes = 0;
    QObject::connect(&Fallback, &QTimer::timeout, [&]()
    {
      if (QWidget * Modal = QApplication::activeModalWidget())
      {
        Boxes++;
        Modal->close();
      }
    });
    Fallback.start();
    EditorWindow E(Large, QString());
    CHECK_EQ(Boxes, 1);
    CHECK(!E.IsLoaded());
  }
  Config.EditorWarnOrLargeFileSize = false;
  {
    EditorWindow E(Large, QString());
    CHECK(E.IsLoaded());
  }
  Config.EditorWarnOrLargeFileSize = true;
  Config.EditorLargeFileSize = 10 * 1024;

  // screenshot for a visual check
  QString Shot = qEnvironmentVariable("NIXSCP_EDITOR_SCREENSHOT");
  if (!Shot.isEmpty())
  {
    EditorWindow E(Utf8, QStringLiteral("/home/user/notes.txt - user@example.com"));
    E.resize(820, 300);
    E.show();
    QCoreApplication::processEvents();
    E.grab().save(Shot);
  }
}

//---------------------------------------------------------------------------
static void TestActions()
{
  std::printf("[actions]\n");
  Configuration & Config = Configuration::Instance();
  EditorPreferences Gedit;
  Gedit.Editor = edExternal;
  Gedit.ExternalEditor = QStringLiteral("gedit !.!");
  EditorPreferences Code;
  Code.Editor = edExternal;
  Code.ExternalEditor = QStringLiteral("code -w !.!");
  Code.FileMask = QStringLiteral("*.c");
  Config.Editors = {Gedit, Code, Gedit, EditorPreferences()};
  QWidget Window;
  EditorManager Manager(&Window);
  EditorManager::Selection Sel;
  EditorManager::Hooks Hooks;
  Hooks.CurrentSelection = [&Sel]() { return Sel; };
  Manager.SetHooks(Hooks);
  CHECK_EQ(Manager.EditAction()->text(), QStringLiteral("&Edit"));
  CHECK(Manager.EditAction()->shortcut() == QKeySequence(Qt::Key_F4));
  CHECK_EQ(Manager.EditInternalAction()->text(), QStringLiteral("&Internal editor"));
  CHECK(Manager.EditInternalAction()->shortcut() == QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_F4));
  CHECK_EQ(Manager.EditWithAction()->text(), QStringLiteral("Edit &With..."));
  CHECK_EQ(Manager.EditorConfigureAction()->text(), QStringLiteral("&Configure..."));
  CHECK(!Manager.EditAction()->isEnabled());
  Sel.Paths = {QStringLiteral("/tmp/x")};
  Manager.UpdateActions();
  CHECK(Manager.EditAction()->isEnabled());
  QMenu * Menu = Manager.EditorListMenu();
  emit Menu->aboutToShow();
  QStringList Captions;
  for (QAction * Action : Menu->actions())
  {
    Captions.append(Action->isSeparator() ? QStringLiteral("-") : Action->text());
  }
  // CreateEditorListMenu: each external editor once
  CHECK_EQ(Captions, (QStringList{QStringLiteral("&Edit"), QStringLiteral("-"), QStringLiteral("&Internal editor"), QStringLiteral("-"),
    QStringLiteral("gedit"), QStringLiteral("code"), QStringLiteral("Edit &With..."), QStringLiteral("-"), QStringLiteral("&Configure...")}));
  Config.Editors = Configuration::DefaultEditors();

  // Edit With... ("Editor" dialog in ad-hoc mode, Remember this editor)
  QString Target = QDir::currentPath() + QStringLiteral("/with.txt");
  WriteFile(Target, "w");
  Sel.Paths = {Target};
  bool DialogSeen = false;
  QTimer Filler;
  Filler.setInterval(30);
  QObject::connect(&Filler, &QTimer::timeout, [&]()
  {
    auto * Dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
    if ((Dialog == nullptr) || (Dialog->objectName() != QStringLiteral("EditorPreferencesDialog")))
    {
      return;
    }
    DialogSeen = true;
    CHECK_EQ(Dialog->windowTitle(), QStringLiteral("Editor"));
    auto * Edit = Dialog->findChild<QLineEdit *>(QStringLiteral("ExternalEditorEdit"));
    auto * Remember = Dialog->findChild<QCheckBox *>(QStringLiteral("RememberCheck"));
    CHECK((Edit != nullptr) && (Remember != nullptr));
    if ((Edit != nullptr) && (Remember != nullptr))
    {
      Edit->setText(QStringLiteral("sh -c 'printf x > \"$1.with\"' sh"));
      Remember->setChecked(true);
      QString Shot = qEnvironmentVariable("NIXSCP_EDITWITH_SCREENSHOT");
      if (!Shot.isEmpty())
      {
        Dialog->grab().save(Shot);
      }
    }
    Dialog->accept();
  });
  Filler.start();
  Manager.EditWithAction()->trigger();
  Filler.stop();
  CHECK(DialogSeen);
  CHECK(WaitFor([&]() { return QFileInfo::exists(Target + QStringLiteral(".with")); }, 5000));
  bool Remembered = false;
  for (const EditorPreferences & E : Config.Editors)
  {
    Remembered = Remembered || ((E.Editor == edExternal) && (E.ExternalEditor == QStringLiteral("sh -c 'printf x > \"$1.with\"' sh !.!")));
  }
  CHECK(Remembered);
  CHECK_EQ(Config.Editors.size(), 2);
  Config.Editors = Configuration::DefaultEditors();
}

//---------------------------------------------------------------------------
// Remote part
//---------------------------------------------------------------------------
class TestUserInterface : public UserInterface
{
public:
  HostKeyAnswer VerifyHostKey(const QString &, int, const QString &, const QString &, const QString &, HostKeyState) override
  {
    return HostKeyOnce;
  }
  bool PromptUser(PromptKind, const QString &, const QString &, QList<Prompt> &) override { return false; }
  void DisplayBanner(const QString &, const QString &) override {}
  unsigned int QueryUser(const QString &, const QString &, unsigned int) override { return qaCancel; }
};

static bool OpenTerminal(Terminal & Term)
{
  bool Done = false;
  bool Ok = false;
  Term.Open([&](const OperationResult & Result)
  {
    Ok = Result.Ok;
    if (!Ok)
    {
      std::printf("open failed: %s\n", qPrintable(Result.Message));
    }
    Done = true;
  });
  WaitFor([&]() { return Done; }, 30000);
  return Ok;
}

static EditorWindow * NewestEditor(const QList<EditorWindow *> & Before)
{
  for (EditorWindow * Editor : EditorWindow::Instances())
  {
    if (!Before.contains(Editor))
    {
      return Editor;
    }
  }
  return nullptr;
}

static void TestRemote(const QString & Scratch)
{
  QByteArray PortStr = qgetenv("NIXSCP_TEST_SSH_PORT");
  QString KeyFile = qEnvironmentVariable("NIXSCP_TEST_SSH_KEY");
  if (PortStr.isEmpty() || KeyFile.isEmpty())
  {
    std::printf("[remote] SKIPPED (set NIXSCP_TEST_SSH_PORT, NIXSCP_TEST_SSH_KEY)\n");
    return;
  }
  std::printf("[remote]\n");
  Configuration & Config = Configuration::Instance();
  Config.EditorCheckNotModified = false;
  SessionData Data;
  Data.Name = QStringLiteral("editsite");
  Data.HostName = qEnvironmentVariable("NIXSCP_TEST_SSH_HOST", QStringLiteral("127.0.0.1"));
  Data.PortNumber = PortStr.toInt();
  Data.UserName = qEnvironmentVariable("NIXSCP_TEST_SSH_USER", qEnvironmentVariable("USER"));
  Data.PublicKeyFile = KeyFile;
  Data.TryAgent = false;
  Data.AuthKI = false;
  Data.KnownHostsFile = Scratch + QStringLiteral("/known_hosts");
  TestUserInterface UI;
  Terminal Term(Data, &UI);
  if (!OpenTerminal(Term))
  {
    Failures++;
    return;
  }
  QString RemoteDir = Scratch + QStringLiteral("/remote dir");
  QString Remote = RemoteDir + QStringLiteral("/notes.txt");
  WriteFile(Remote, "original\n");

  QWidget Window;
  EditorManager Manager(&Window);
  QList<Terminal *> Open = {&Term};
  int Refreshed = 0;
  EditorManager::Hooks Hooks;
  Hooks.Terminals = [&Open]() { return Open; };
  Hooks.FileUploaded = [&Refreshed](Terminal *, const QString &) { Refreshed++; };
  Manager.SetHooks(Hooks);
  int Uploads = 0;
  bool LastUploadOk = false;
  QObject::connect(&Manager, &EditorManager::FileUploaded, [&](const QString &, bool Ok) { Uploads++; LastUploadOk = Ok; });

  std::printf("  - internal editor: download into a private folder\n");
  // --- internal editor: download into a private folder
  QList<EditorWindow *> Before = EditorWindow::Instances();
  Manager.ExecuteFiles(&Term, {Remote}, EditorManager::efInternalEditor);
  CHECK(WaitFor([&]() { return Manager.Count() == 1; }));
  EditorWindow * Editor = NewestEditor(Before);
  CHECK(Editor != nullptr);
  if (Editor == nullptr)
  {
    return;
  }
  QString Local = Manager.EditedFiles().value(0);
  QString Runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
  CHECK(Local.startsWith(Runtime + QStringLiteral("/nixscp-")));
  // TemporaryDirectoryAppendPath (default on): the remote path below the unique folder
  CHECK(Local.endsWith(RemoteDir + QStringLiteral("/notes.txt")));
  QString Root = Local.left(Runtime.size() + 14);
  struct stat St;
  CHECK(::stat(QFile::encodeName(Root).constData(), &St) == 0);
  CHECK_EQ(static_cast<int>(St.st_mode & 0777), 0700);
  CHECK(QFileInfo::exists(Root + QLatin1Char('/') + EditorManager::LockFileName()));
  CHECK(!EditorManager::FindTemporaryFolders().contains(Root));
  CHECK(Editor->windowTitle().startsWith(Remote + QStringLiteral(" - editsite")));
  QPlainTextEdit * Memo = Editor->findChild<QPlainTextEdit *>();
  CHECK_EQ(Memo->toPlainText(), QStringLiteral("original"));

  std::printf("  - the same file again: the open editor is reused (no second download)\n");
  // --- directories are not opened
  Manager.ExecuteFiles(&Term, {RemoteDir}, EditorManager::efInternalEditor);
  CHECK(WaitFor([&]() { return !Term.Busy(); }));
  QCoreApplication::processEvents();
  CHECK_EQ(Manager.Count(), 1);
  CHECK_EQ(EditorWindow::Instances().size(), Before.size() + 1);

  // --- the same file again: the open editor is reused (no second download)
  Manager.ExecuteFiles(&Term, {Remote}, EditorManager::efInternalEditor);
  QCoreApplication::processEvents();
  CHECK_EQ(Manager.Count(), 1);
  CHECK_EQ(EditorWindow::Instances().size(), Before.size() + 1);

  std::printf("  - save uploads\n");
  // --- save uploads
  SetText(Memo, QStringLiteral("edited once"));
  CHECK(Editor->Save());
  CHECK(WaitFor([&]() { return Uploads == 1; }));
  CHECK(LastUploadOk);
  CHECK_EQ(ReadFile(Remote), QByteArray("edited once\n"));
  CHECK_EQ(Refreshed, 1);

  std::printf("  - Reload downloads the remote file again\n");
  // --- Reload downloads the remote file again
  WriteFile(Remote, "changed on server\n");
  Editor->Reload();
  CHECK(WaitFor([&]() { return Memo->toPlainText() == QStringLiteral("changed on server"); }));
  CHECK(!Editor->IsModified());

  std::printf("  - closing the editor deletes the private folder\n");
  // --- closing the editor deletes the private folder
  int ClosedCount = 0;
  QObject::connect(&Manager, &EditorManager::FileClosed, [&]() { ClosedCount++; });
  Editor->close();
  CHECK(WaitFor([&]() { return Manager.Count() == 0; }));
  CHECK(!QFileInfo::exists(Root));

  std::printf("  - EditorCheckNotModified: remote file changed meanwhile\n");
  // --- EditorCheckNotModified: remote file changed meanwhile
  Config.EditorCheckNotModified = true;
  WriteFile(Remote, "base\n");
  SetAge(Remote, 100);
  Before = EditorWindow::Instances();
  Manager.ExecuteFiles(&Term, {Remote}, EditorManager::efInternalEditor);
  CHECK(WaitFor([&]() { return Manager.Count() == 1; }));
  Editor = NewestEditor(Before);
  CHECK(Editor != nullptr);
  if (Editor == nullptr)
  {
    return;
  }
  Memo = Editor->findChild<QPlainTextEdit *>();
  WriteFile(Remote, "server side change\n");
  SetAge(Remote, 10);
  Uploads = 0;
  {
    AutoAnswer Cancel(qaCancel);
    SetText(Memo, QStringLiteral("mine"));
    Editor->Save();
    CHECK(WaitFor([&]() { return Uploads == 1; }));
    CHECK_EQ(Cancel.Seen, 1);
    CHECK(Cancel.LastText.contains(QStringLiteral("The remote file has been changed while you were editing it.")));
    CHECK(!LastUploadOk);
    CHECK_EQ(ReadFile(Remote), QByteArray("server side change\n"));
  }
  {
    AutoAnswer Ok(qaOK);
    SetText(Memo, QStringLiteral("mine again"));
    Editor->Save();
    CHECK(WaitFor([&]() { return Uploads == 2; }));
    CHECK_EQ(Ok.Seen, 1);
    CHECK(LastUploadOk);
    CHECK_EQ(ReadFile(Remote), QByteArray("mine again\n"));
  }
  {
    // after our own upload no conflict is reported
    AutoAnswer Ok(qaOK);
    SetText(Memo, QStringLiteral("third"));
    Editor->Save();
    CHECK(WaitFor([&]() { return Uploads == 3; }));
    CHECK_EQ(Ok.Seen, 0);
    CHECK_EQ(ReadFile(Remote), QByteArray("third\n"));
  }
  Config.EditorCheckNotModified = false;

  std::printf("  - session closed: re-attach to another open session of the same site\n");
  // --- session closed: re-attach to another open session of the same site
  Terminal Term2(Data, &UI);
  CHECK(OpenTerminal(Term2));
  Open = {&Term2};
  {
    AutoAnswer Ok(qaOK);
    SetText(Memo, QStringLiteral("via second session"));
    Editor->Save();
    CHECK(WaitFor([&]() { return Uploads == 4; }));
    CHECK_EQ(Ok.Seen, 1);
    CHECK(Ok.LastText.contains(QStringLiteral("Do you want to attach edited file 'notes.txt' to session")));
    CHECK(LastUploadOk);
    CHECK_EQ(ReadFile(Remote), QByteArray("via second session\n"));
  }
  // no session at all: EDIT_SESSION_CLOSED2
  Open = {};
  {
    AutoAnswer Ok(qaOK);
    SetText(Memo, QStringLiteral("lost"));
    Editor->Save();
    CHECK(WaitFor([&]() { return Uploads == 5; }));
    CHECK_EQ(Ok.Seen, 1);
    CHECK(Ok.LastText.contains(QStringLiteral("Cannot upload edited file 'notes.txt'")));
    CHECK(!LastUploadOk);
  }
  Open = {&Term, &Term2};
  Editor->close();
  CHECK(WaitFor([&]() { return Manager.Count() == 0; }));

  std::printf("  - tracked external editor (SDI): uploads its change, cleans up when it exits\n");
  // --- tracked external editor (SDI): uploads its change, cleans up when it exits
  WriteFile(Remote, "for external\n");
  EditorPreferences Sdi;
  Sdi.Editor = edExternal;
  Sdi.ExternalEditor = QStringLiteral("sh -c 'sleep 1; printf appended >> \"$1\"; sleep 3' sh !.!");
  Sdi.SDIExternalEditor = true;
  Uploads = 0;
  ClosedCount = 0;
  Manager.ExecuteFiles(&Term, {Remote}, EditorManager::efExternalEditor, &Sdi);
  CHECK(WaitFor([&]() { return Manager.Count() == 1; }));
  QString ExternalLocal = Manager.EditedFiles().value(0);
  CHECK(WaitFor([&]() { return Uploads >= 1; }, 15000));
  CHECK_EQ(ReadFile(Remote), QByteArray("for external\nappended"));
  CHECK(WaitFor([&]() { return Manager.Count() == 0; }, 15000));
  CHECK_EQ(ClosedCount, 1);
  CHECK(!QFileInfo::exists(ExternalLocal));

  std::printf("  - early close (editor that hands the file over and exits): \"No\" keeps the file\n");
  // --- early close (editor that hands the file over and exits): "No" keeps the file
  EditorPreferences Early;
  Early.Editor = edExternal;
  Early.ExternalEditor = QStringLiteral("true !.!");
  Early.SDIExternalEditor = true;
  {
    AutoAnswer No(qaNo);
    Manager.ExecuteFiles(&Term, {Remote}, EditorManager::efExternalEditor, &Early);
    CHECK(WaitFor([&]() { return No.Seen == 1; }));
    CHECK(No.LastText.contains(QStringLiteral("was closed too early")));
    QCoreApplication::processEvents();
    CHECK_EQ(Manager.Count(), 1);
  }
  // now a "no process" file: watched; changes are uploaded
  QString EarlyLocal = Manager.EditedFiles().value(0);
  Uploads = 0;
  WriteFile(EarlyLocal, "changed by mdi editor\n");
  CHECK(WaitFor([&]() { return Uploads == 1; }));
  CHECK_EQ(ReadFile(Remote), QByteArray("changed by mdi editor\n"));
  // an MDI file opened again: downloaded again into the same folder
  WriteFile(Remote, "fresh\n");
  EditorPreferences Mdi = Early;
  Mdi.SDIExternalEditor = false;
  Manager.ExecuteFiles(&Term, {Remote}, EditorManager::efExternalEditor, &Mdi);
  CHECK(WaitFor([&]() { return (ReadFile(EarlyLocal) == QByteArray("fresh\n")) && !Term.Busy() && (Manager.Count() == 1); }));
  CHECK_EQ(Manager.Count(), 1);
  CHECK_EQ(Manager.EditedFiles().value(0), EarlyLocal);

  std::printf("  - exit: files without process are closed, running editors ask PENDING_EDITORS\n");
  // --- exit: files without process are closed, running editors ask PENDING_EDITORS
  EditorPreferences Long;
  Long.Editor = edExternal;
  // a token only this process knows, to find the editor process later
  QString Token = QStringLiteral("edtest%1x%2").arg(getpid()).arg(QDateTime::currentMSecsSinceEpoch());
  Long.ExternalEditor = QStringLiteral("sh -c 'sleep 60; : %1' sh !.!").arg(Token);
  Long.SDIExternalEditor = true;
  QString Remote2 = RemoteDir + QStringLiteral("/second.txt");
  WriteFile(Remote2, "2\n");
  Manager.ExecuteFiles(&Term, {Remote2}, EditorManager::efExternalEditor, &Long);
  CHECK(WaitFor([&]() { return Manager.Count() == 2; }));
  QString LongLocal;
  for (const QString & F : Manager.EditedFiles())
  {
    if (F.endsWith(QStringLiteral("second.txt")))
    {
      LongLocal = F;
    }
  }
  {
    AutoAnswer Cancel(qaCancel);
    CHECK(!Manager.CanClose());
    CHECK_EQ(Cancel.Seen, 1);
    CHECK(Cancel.LastText.contains(QStringLiteral("There are some opened files.")));
    CHECK_EQ(Manager.Count(), 1);            // the MDI file was closed
    CHECK(!QFileInfo::exists(EarlyLocal));
  }
  {
    AutoAnswer Ignore(qaIgnore);
    CHECK(Manager.CanClose());
    CHECK_EQ(Ignore.Seen, 1);
    CHECK(QFileInfo::exists(LongLocal));     // kept, the editor still has it open
  }
  {
    // the editor exits: last check and cleanup (an early exit asks first)
    AutoAnswer Yes(qaYes);
    int Count = Manager.Count();
    QProcess::execute(QStringLiteral("pkill"), {QStringLiteral("-f"), Token});
    CHECK(WaitFor([&]() { return Manager.Count() == Count - 1; }));
    CHECK(!QFileInfo::exists(LongLocal));
  }

  std::printf("  - hostile remote file name: no shell is involved anywhere\n");
  // --- hostile remote file name: no shell is involved anywhere
  QString HostileRemote = RemoteDir + QStringLiteral("/h$(touch PWNED4)`touch PWNED5`.txt");
  WriteFile(HostileRemote, "h\n");
  EditorPreferences Touch;
  Touch.Editor = edExternal;
  Touch.ExternalEditor = QStringLiteral("touch !.!.seen");
  int CountBefore = Manager.Count();
  AutoAnswer Unexpected(qaOK);
  Manager.ExecuteFiles(&Term, {HostileRemote}, EditorManager::efExternalEditor, &Touch);
  CHECK(WaitFor([&]() { return Manager.Count() == CountBefore + 1; }));
  QString HostileLocal;
  for (const QString & F : Manager.EditedFiles())
  {
    if (F.endsWith(QStringLiteral("PWNED5`.txt")))
    {
      HostileLocal = F;
    }
  }
  CHECK(!HostileLocal.isEmpty());
  CHECK(WaitFor([&]() { return QFileInfo::exists(HostileLocal + QStringLiteral(".seen")); }, 5000));
  CHECK(!QFileInfo::exists(QDir::currentPath() + QStringLiteral("/PWNED4")));
  CHECK(!QFileInfo::exists(QDir::currentPath() + QStringLiteral("/PWNED5")));
  CHECK(!QFileInfo::exists(RemoteDir + QStringLiteral("/PWNED4")));
  CHECK_EQ(Unexpected.Seen, 0);

  std::printf("  - exit with a modified internal editor: saved, uploaded, then closed\n");
  QString Remote3 = RemoteDir + QStringLiteral("/third.txt");
  WriteFile(Remote3, "3\n");
  QList<EditorWindow *> Before3 = EditorWindow::Instances();
  int Count3 = Manager.Count();
  Manager.ExecuteFiles(&Term, {Remote3}, EditorManager::efInternalEditor);
  CHECK(WaitFor([&]() { return Manager.Count() == Count3 + 1; }));
  EditorWindow * Editor3 = NewestEditor(Before3);
  CHECK(Editor3 != nullptr);
  if (Editor3 != nullptr)
  {
    SetText(Editor3->findChild<QPlainTextEdit *>(), QStringLiteral("saved on exit"));
    QTimer SaveYes;
    SaveYes.setInterval(30);
    QObject::connect(&SaveYes, &QTimer::timeout, [&]()
    {
      if (auto * Box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
      {
        Box->button(QMessageBox::Yes)->click();
      }
    });
    SaveYes.start();
    AutoAnswer Ignore(qaIgnore);
    CHECK(Manager.CanClose());
    CHECK_EQ(ReadFile(Remote3), QByteArray("saved on exit\n"));
    // only the hostile file's "touch" editor is left (no process): closed too
    CHECK_EQ(Manager.Count(), 0);
    CHECK_EQ(Ignore.Seen, 0);
  }
  if (Unexpected.Seen > 0)
  {
    std::printf("unexpected message: %s\n", qPrintable(Unexpected.LastText));
  }
}

int main(int argc, char ** argv)
{
  QApplication App(argc, argv);
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  QStandardPaths::setTestModeEnabled(true);
  QTemporaryDir Scratch;
  CHECK(Scratch.isValid());
  qputenv("XDG_CONFIG_HOME", Scratch.path().toUtf8());
  QDir::setCurrent(Scratch.path());
  Configuration::Instance().SetIniFileName(Scratch.filePath(QStringLiteral("nixscp.ini")));
  Configuration::Instance().Load();
  // no first-use offer of the system editor in tests
  {
    IniStorage Storage(&Configuration::Instance().Ini());
    Storage.OpenSubKey(QStringLiteral("Configuration"), true);
    Storage.OpenSubKey(QStringLiteral("Interface"), true);
    Storage.WriteBool(QStringLiteral("OfferedEditorAutoConfig"), true);
  }

  TestCommandArguments(Scratch.path());
  TestTemporaryFolders(Scratch.path());
  TestEditorWindow(Scratch.path());
  TestActions();
  TestRemote(Scratch.path());

  std::printf(Failures == 0 ? "OK\n" : "%d FAILURE(S)\n", Failures);
  return (Failures == 0) ? 0 : 1;
}
