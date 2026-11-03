// Linux desktop integration (WP18): src/gui/DesktopIntegration (terminal
// client command with all patterns and OpenSSH options, Explore Directory over
// FileManager1, Places, notifications, taskbar progress, desktop icon, local
// operations with progress) and src/gui/ClipboardTransfer (file clipboard
// formats, remote files downloaded on request, paste decoding, remote
// duplicate), plus the drop handling of the Commander window
// (CommanderWindow::HandleDrop: local copy engine, Ctrl+drag = duplicate in
// the remote panel, DDTransferConfirmation).
//
// GUI test: built with NIXSCP_GUI_TESTS and the GUI objects, runs offscreen.
// The remote part needs the test SSH server (NIXSCP_TEST_SSH_PORT,
// NIXSCP_TEST_SSH_KEY, optional NIXSCP_TEST_SSH_HOST/USER); it is skipped otherwise.
// External programs (gdbus, xdg-open, terminals) are intercepted, never started.
// NIXSCP_TEST_SCREENSHOT=<png> saves the Places menu.
#ifndef NIXSCP_GUI_TESTS

#include <cstdio>

int main()
{
  std::printf("SKIP: test_desktopintegration needs NIXSCP_GUI_TESTS (Qt Widgets + the GUI sources)\n");
  return 0;
}

#else

#include "core/Configuration.h"
#include "core/CustomCommands.h"
#include "core/FileSystem.h"
#include "core/LocalFileOperations.h"
#include "core/Terminal.h"
#include "gui/ClipboardTransfer.h"
#include "gui/CommanderWindow.h"
#include "gui/DesktopIntegration.h"
#include "gui/FilePanel.h"
#include "gui/GuiUserInterface.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QSystemTrayIcon>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <cstdio>
#include <functional>
#include <sys/stat.h>
#include <unistd.h>

static int Failures = 0;
static int Checks = 0;

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
    if (Timer.elapsed() > Ms)
    {
      return false;
    }
    QApplication::processEvents(QEventLoop::AllEvents, 10);
    QThread::msleep(5);
  }
  return true;
}

static void WriteFile(const QString & Path, const QByteArray & Data)
{
  QFile F(Path);
  if (F.open(QIODevice::WriteOnly | QIODevice::Truncate))
  {
    F.write(Data);
  }
}

static QByteArray ReadFile(const QString & Path)
{
  QFile F(Path);
  return F.open(QIODevice::ReadOnly) ? F.readAll() : QByteArray();
}

struct Call
{
  QString Program;
  QStringList Arguments;
};
static QList<Call> Calls;
static bool RunnerResult = true;

static void InstallRunner()
{
  DesktopIntegration::SetCommandRunner([](const QString & Program, const QStringList & Arguments)
  {
    Calls.append(Call{Program, Arguments});
    return RunnerResult;
  });
}

// Accepts the message boxes / dialogs shown while Func runs (answer by button text)
static void AnswerDialogs(const QString & ButtonText, int Ms = 3000)
{
  auto * Timer = new QTimer(qApp);
  Timer->setInterval(50);
  QElapsedTimer * Elapsed = new QElapsedTimer();
  Elapsed->start();
  QObject::connect(Timer, &QTimer::timeout, [Timer, Elapsed, ButtonText, Ms]()
  {
    for (QWidget * Widget : QApplication::topLevelWidgets())
    {
      if (Widget->isVisible() && Widget->inherits("QDialog") && !Widget->inherits("ProgressDialog"))
      {
        for (QAbstractButton * Button : Widget->findChildren<QAbstractButton *>())
        {
          if (Button->isVisible() && (Button->text().remove(QLatin1Char('&')) == ButtonText))
          {
            Timer->stop();
            Timer->deleteLater();
            delete Elapsed;
            Button->click();
            return;
          }
        }
      }
    }
    if (Elapsed->elapsed() > Ms)
    {
      Timer->stop();
      Timer->deleteLater();
      delete Elapsed;
    }
  });
  Timer->start();
}

//---------------------------------------------------------------------------

static SessionData SshSession()
{
  SessionData Data;
  Data.Name = QStringLiteral("Folder/My Site");
  Data.FSProtocol = fsSFTP;
  Data.HostName = QStringLiteral("example.com");
  Data.PortNumber = 2222;
  Data.UserName = QStringLiteral("joe");
  Data.Password = QStringLiteral("se cret");
  Data.RemoteDirectory = QStringLiteral("/home/joe");
  return Data;
}

static void TestTerminalCommand(const QString & Scratch)
{
  std::printf("- terminal client command\n");
  Configuration & Config = Configuration::Instance();
  Config.PuttyPath = Configuration::DefaultPuttyPath();
  Config.PuttyPassword = false;
  Config.TelnetForFtpInPutty = true;

  // default command: patterns, nothing added that the command already says
  SessionData Data = SshSession();
  DesktopIntegration::TerminalCommand Command;
  QString Error;
  CHECK(DesktopIntegration::BuildTerminalCommand(Data, QString(), QString(), false, Command, Error));
  CHECK(Command.Program == QStringLiteral("x-terminal-emulator"));
  CHECK_MSG(Command.Arguments == (QStringList{QStringLiteral("-e"), QStringLiteral("ssh"), QStringLiteral("-p"),
    QStringLiteral("2222"), QStringLiteral("joe@example.com")}), Command.Arguments.join(QLatin1Char('|')));
  CHECK(Command.AskPassFile.isEmpty());

  // OpenSSH options from the site settings
  QString Key = Scratch + QStringLiteral("/id_test");
  WriteFile(Key, "key");
  Data.PublicKeyFile = Key;
  Data.AgentFwd = true;
  Data.Compression = true;
  Data.AddressFamily = afIPv4;
  Data.Tunnel = true;
  Data.TunnelHostName = QStringLiteral("jump.example.com");
  Data.TunnelUserName = QStringLiteral("hop");
  Data.TunnelPortNumber = 2200;
  Data.PingType = ptNullPacket;
  Data.PingInterval = 30;
  CHECK(DesktopIntegration::BuildTerminalCommand(Data, QString(), QStringLiteral("/srv/www"), true, Command, Error));
  QStringList A = Command.Arguments;
  CHECK_MSG(A.mid(0, 2) == (QStringList{QStringLiteral("-e"), QStringLiteral("ssh")}), A.join(QLatin1Char('|')));
  CHECK(A.indexOf(QStringLiteral("-i")) >= 0 && A.value(A.indexOf(QStringLiteral("-i")) + 1) == Key);
  CHECK(A.contains(QStringLiteral("-A")));
  CHECK(A.contains(QStringLiteral("-C")));
  CHECK(A.contains(QStringLiteral("-4")));
  CHECK(A.value(A.indexOf(QStringLiteral("-J")) + 1) == QStringLiteral("hop@jump.example.com:2200"));
  CHECK(A.contains(QStringLiteral("ServerAliveInterval=30")));
  CHECK(A.contains(QStringLiteral("joe@example.com")));
  CHECK(A.value(A.size() - 2) == QStringLiteral("-t"));
  CHECK_MSG(A.last() == QStringLiteral("cd '/srv/www' && exec \"$SHELL\" -l"), A.last());
  // options come right after the ssh program, before the destination
  CHECK(A.indexOf(QStringLiteral("-C")) < A.indexOf(QStringLiteral("joe@example.com")));

  Data.TryAgent = false;
  Data.Tunnel = false;
  Data.ProxyMethod = pmCmd;
  Data.ProxyLocalCommand = QStringLiteral("connect-proxy %host %port 100%");
  Data.LogicalHostName = QStringLiteral("alias.example.com");
  CHECK(DesktopIntegration::BuildTerminalCommand(Data, QString(), QString(), false, Command, Error));
  A = Command.Arguments;
  CHECK(A.contains(QStringLiteral("IdentityAgent=none")));
  CHECK(!A.contains(QStringLiteral("-A")));
  CHECK_MSG(A.contains(QStringLiteral("ProxyCommand=connect-proxy %h %p 100%%")), A.join(QLatin1Char('|')));
  CHECK(A.contains(QStringLiteral("HostKeyAlias=alias.example.com")));

  // all patterns: !N !E !K !S !/ !# !U !@ !P !! and the interactive ones
  Data = SshSession();
  Data.PublicKeyFile = Key;
  Config.PuttyPassword = true;
  QString Template = QStringLiteral("term --title !N --url !E --key !K --dir !/ --x !?Your name?bob! --y !`echo hello world` --pw !P -- ssh !U@!@ --bang !!");
  QStringList AskedPrompts;
  CHECK(DesktopIntegration::BuildTerminalCommand(Data, QString(), QStringLiteral("/data"), false, Command, Error,
    [&AskedPrompts](const QStringList & Prompts, const QStringList & Defaults, QStringList & Values)
    {
      AskedPrompts = Prompts;
      Values = QStringList{Defaults.value(0) + QStringLiteral("by")};
      return true;
    }, Template));
  A = Command.Arguments;
  CHECK(Command.Program == QStringLiteral("term"));
  CHECK_MSG(A.value(A.indexOf(QStringLiteral("--title")) + 1) == QStringLiteral("Folder/My Site"), A.join(QLatin1Char('|')));
  CHECK(A.value(A.indexOf(QStringLiteral("--url")) + 1).startsWith(QStringLiteral("sftp://joe")));
  CHECK(A.value(A.indexOf(QStringLiteral("--key")) + 1) == Key);
  CHECK(A.value(A.indexOf(QStringLiteral("--dir")) + 1).startsWith(QStringLiteral("/data")));
  CHECK(AskedPrompts == QStringList{QStringLiteral("Your name")});
  CHECK(A.value(A.indexOf(QStringLiteral("--x")) + 1) == QStringLiteral("bobby"));
  CHECK_MSG(A.value(A.indexOf(QStringLiteral("--y")) + 1) == QStringLiteral("hello"), A.join(QLatin1Char('|')));
  CHECK(A.value(A.indexOf(QStringLiteral("--pw")) + 1) == QStringLiteral("se cret"));
  CHECK(A.contains(QStringLiteral("joe@example.com")));
  CHECK(A.last() == QStringLiteral("!"));
  // !P in the command: no SSH_ASKPASS
  CHECK(Command.AskPassFile.isEmpty());

  // cancelled prompt: nothing to run, no error
  CHECK(!DesktopIntegration::BuildTerminalCommand(Data, QString(), QString(), false, Command, Error,
    [](const QStringList &, const QStringList &, QStringList &) { return false; }, Template));
  CHECK(Error.isEmpty());
  // unterminated pattern: error
  CHECK(!DesktopIntegration::BuildTerminalCommand(Data, QString(), QString(), false, Command, Error, nullptr,
    QStringLiteral("term !?oops")));
  CHECK(!Error.isEmpty());

  // remembered password passed as SSH_ASKPASS (PuttyPassword)
  CHECK(DesktopIntegration::BuildTerminalCommand(Data, QStringLiteral("typed"), QString(), false, Command, Error));
  A = Command.Arguments;
  CHECK(!Command.AskPassFile.isEmpty());
  CHECK(ReadFile(Command.AskPassFile) == "typed");
  CHECK((QFile::permissions(Command.AskPassFile) & (QFileDevice::ReadGroup | QFileDevice::ReadOther)) == 0);
  int Env = A.indexOf(QStringLiteral("env"));
  CHECK(Env == 1);
  CHECK(A.value(Env + 5) == QStringLiteral("ssh"));
  CHECK(A.contains(QStringLiteral("NIXSCP_ASKPASS_FILE=") + Command.AskPassFile));
  QFile::remove(Command.AskPassFile);
  Config.PuttyPassword = false;
  CHECK(DesktopIntegration::BuildTerminalCommand(Data, QStringLiteral("typed"), QString(), false, Command, Error));
  CHECK(Command.AskPassFile.isEmpty());

  // command without site patterns: the destination is added (as PuTTY -load)
  Config.PuttyPath = QStringLiteral("x-terminal-emulator -e ssh");
  CHECK(DesktopIntegration::BuildTerminalCommand(Data, QString(), QString(), false, Command, Error));
  A = Command.Arguments;
  CHECK_MSG(A.join(QLatin1Char(' ')) == QStringLiteral("-e ssh -i %1 -p 2222 -l joe example.com").arg(Key), A.join(QLatin1Char(' ')));
  // no client at all in the command: OpenSSH is appended
  Config.PuttyPath = QStringLiteral("konsole -e");
  CHECK(DesktopIntegration::BuildTerminalCommand(Data, QString(), QString(), false, Command, Error));
  CHECK_MSG(Command.Arguments.join(QLatin1Char(' ')) == QStringLiteral("-e ssh -i %1 -p 2222 -l joe example.com").arg(Key),
    Command.Arguments.join(QLatin1Char(' ')));

  // FTP: telnet (TelnetForFtpInPutty), or ssh on the SSH port
  Config.PuttyPath = Configuration::DefaultPuttyPath();
  SessionData Ftp = SshSession();
  Ftp.FSProtocol = fsFTP;
  Ftp.PortNumber = 21;
  CHECK(DesktopIntegration::BuildTerminalCommand(Ftp, QString(), QString(), false, Command, Error));
  CHECK_MSG(Command.Arguments == (QStringList{QStringLiteral("-e"), QStringLiteral("telnet"), QStringLiteral("example.com")}),
    Command.Arguments.join(QLatin1Char('|')));
  Config.TelnetForFtpInPutty = false;
  CHECK(DesktopIntegration::BuildTerminalCommand(Ftp, QString(), QString(), false, Command, Error));
  CHECK_MSG(Command.Arguments.join(QLatin1Char(' ')) == QStringLiteral("-e ssh -p 22 joe@example.com"), Command.Arguments.join(QLatin1Char(' ')));
  Config.TelnetForFtpInPutty = true;

  // terminal emulator fallback when x-terminal-emulator is missing
  QString Bin = Scratch + QStringLiteral("/bin");
  QDir().mkpath(Bin);
  QByteArray OldPath = qgetenv("PATH");
  QByteArray OldTerminal = qgetenv("TERMINAL");
  qunsetenv("TERMINAL");
  WriteFile(Bin + QStringLiteral("/gnome-terminal"), "#!/bin/sh\n");
  QFile::setPermissions(Bin + QStringLiteral("/gnome-terminal"), QFileDevice::ReadOwner | QFileDevice::ExeOwner);
  qputenv("PATH", Bin.toUtf8());
  QStringList Args{QStringLiteral("-e"), QStringLiteral("ssh"), QStringLiteral("host")};
#ifdef Q_OS_MACOS
  // Terminal.app opens a self-removing .command script running the command line
  Args = QStringList{QStringLiteral("-e"), QStringLiteral("ssh"), QStringLiteral("it's host")};
  CHECK(DesktopIntegration::ResolveTerminalProgram(QStringLiteral("x-terminal-emulator"), Args) == QStringLiteral("open"));
  CHECK((Args.size() == 3) && (Args.value(0) == QStringLiteral("-a")) && (Args.value(1) == QStringLiteral("Terminal")));
  {
    QFile Script(Args.value(2));
    CHECK(Args.value(2).endsWith(QStringLiteral(".command")) && Script.open(QIODevice::ReadOnly));
    QByteArray Content = Script.readAll();
    CHECK(Content.contains("rm -f \"$0\"\nclear\nexec 'ssh' 'it'\\''s host'\n"));
    CHECK((Script.permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther)) == 0);
    Script.remove();
  }
#else
  CHECK(DesktopIntegration::ResolveTerminalProgram(QStringLiteral("x-terminal-emulator"), Args) == QStringLiteral("gnome-terminal"));
  CHECK(Args == (QStringList{QStringLiteral("--"), QStringLiteral("ssh"), QStringLiteral("host")}));
  WriteFile(Bin + QStringLiteral("/xfce4-terminal"), "#!/bin/sh\n");
  QFile::setPermissions(Bin + QStringLiteral("/xfce4-terminal"), QFileDevice::ReadOwner | QFileDevice::ExeOwner);
  qputenv("TERMINAL", "xfce4-terminal");
  Args = QStringList{QStringLiteral("-e"), QStringLiteral("ssh")};
  CHECK(DesktopIntegration::ResolveTerminalProgram(QStringLiteral("x-terminal-emulator"), Args) == QStringLiteral("xfce4-terminal"));
  CHECK(Args.value(0) == QStringLiteral("-x"));
#endif
  Args = QStringList{QStringLiteral("-e"), QStringLiteral("ssh")};
  CHECK(DesktopIntegration::ResolveTerminalProgram(QStringLiteral("myterm"), Args) == QStringLiteral("myterm"));
  qputenv("PATH", OldPath);
  if (OldTerminal.isNull()) qunsetenv("TERMINAL"); else qputenv("TERMINAL", OldTerminal);

  // OpenSessionInTerminal starts it (intercepted)
  Calls.clear();
  Config.PuttyPath = QStringLiteral("myterm -e ssh -p !# !U@!@");
  CHECK(DesktopIntegration::OpenSessionInTerminal(nullptr, SshSession(), QString(), QStringLiteral("/tmp"), true));
  CHECK(Calls.size() == 1);
  CHECK(Calls.value(0).Program == QStringLiteral("myterm"));
  CHECK(Calls.value(0).Arguments.contains(QStringLiteral("joe@example.com")));
  CHECK(Calls.value(0).Arguments.contains(QStringLiteral("-t")));
  Config.PuttyPath = Configuration::DefaultPuttyPath();
}

static void TestExploreAndPlaces(const QString & Scratch)
{
  std::printf("- explore directory, places\n");
  Calls.clear();
  RunnerResult = true;
  DesktopIntegration::ExploreDirectory(Scratch);
#ifdef Q_OS_MACOS
  // Finder: open (the folder), open -R (the item selected)
  CHECK(Calls.size() == 1);
  CHECK(Calls.value(0).Program == QStringLiteral("open"));
  CHECK(Calls.value(0).Arguments == QStringList{Scratch});
  Calls.clear();
  DesktopIntegration::ExploreFile(Scratch + QStringLiteral("/file.txt"));
  CHECK(Calls.size() == 1);
  CHECK(Calls.value(0).Arguments == (QStringList{QStringLiteral("-R"), Scratch + QStringLiteral("/file.txt")}));
#else
  CHECK(Calls.size() == 1);
  CHECK(Calls.value(0).Program == QStringLiteral("gdbus"));
  CHECK(Calls.value(0).Arguments.contains(QStringLiteral("org.freedesktop.FileManager1.ShowFolders")));
  CHECK(Calls.value(0).Arguments.contains(QStringLiteral("['%1']").arg(QUrl::fromLocalFile(Scratch).toString(QUrl::FullyEncoded))));
  // no FileManager1 service: xdg-open
  Calls.clear();
  RunnerResult = false;
  DesktopIntegration::ExploreFile(Scratch + QStringLiteral("/file.txt"));
  CHECK(Calls.size() == 2);
  CHECK(Calls.value(0).Arguments.contains(QStringLiteral("org.freedesktop.FileManager1.ShowItems")));
  CHECK(Calls.value(1).Program == QStringLiteral("xdg-open"));
  CHECK(Calls.value(1).Arguments == QStringList{Scratch});
#endif
  RunnerResult = true;

  // GTK bookmarks
  QString Bookmarked = Scratch + QStringLiteral("/my projects");
  QDir().mkpath(Bookmarked);
  QString File = Scratch + QStringLiteral("/bookmarks");
  WriteFile(File, (QUrl::fromLocalFile(Bookmarked).toEncoded() + " Projects\n" +
    QUrl::fromLocalFile(Scratch).toEncoded() + "\nsftp://host/dir Remote\nfile:///nonexistent/dir Gone\n"));
  QList<DesktopIntegration::Place> Bookmarks = DesktopIntegration::BookmarkPlaces(File);
  CHECK(Bookmarks.size() == 2);
  CHECK(Bookmarks.value(0).Caption == QStringLiteral("Projects"));
  CHECK(Bookmarks.value(0).Path == Bookmarked);
  CHECK(Bookmarks.value(1).Caption == QFileInfo(Scratch).fileName());

  QList<DesktopIntegration::Place> Places = DesktopIntegration::Places();
  CHECK(!Places.isEmpty());
  CHECK(Places.value(0).Path == QDir::cleanPath(QDir::homePath()));
  bool HasRoot = false;
  for (const DesktopIntegration::Place & P : Places)
  {
    HasRoot = HasRoot || (P.Path == QStringLiteral("/"));
    CHECK(QFileInfo(P.Path).isDir());
  }
  CHECK(HasRoot);

  QMenu Menu;
  QString Chosen;
  DesktopIntegration::FillPlacesMenu(&Menu, [&Chosen](const QString & Path) { Chosen = Path; });
  QAction * Root = nullptr;
  for (QAction * Action : Menu.actions())
  {
    if (Action->data().toString() == QStringLiteral("/"))
    {
      Root = Action;
    }
  }
  CHECK(Root != nullptr);
  CHECK(Root && Root->text() == QStringLiteral("File System"));
  if (!qEnvironmentVariable("NIXSCP_TEST_SCREENSHOT").isEmpty())
  {
    // NIXSCP_TEST_SCREENSHOT=<png>: the Places (Change Drive) menu
    Menu.adjustSize();
    Menu.grab().save(qEnvironmentVariable("NIXSCP_TEST_SCREENSHOT"));
  }
  if (Root) Root->trigger();
  CHECK(Chosen == QStringLiteral("/"));
}

static void TestAttention(const QString & Scratch)
{
  std::printf("- notifications, attention, taskbar progress\n");
  Configuration & Config = Configuration::Instance();
  Config.BalloonNotifications = true;
  Calls.clear();
  CHECK(DesktopIntegration::Notify(QString(), QStringLiteral("**Operation was completed**")));
  CHECK(Calls.size() == 1);
  CHECK(Calls.value(0).Program == QStringLiteral("gdbus"));
  QStringList A = Calls.value(0).Arguments;
  CHECK(A.contains(QStringLiteral("org.freedesktop.Notifications.Notify")));
  CHECK(A.contains(QStringLiteral("'Operation was completed'")));   // main instruction = title
  CHECK(A.last() == QStringLiteral("10000"));                      // NotificationsTimeout default 10 s

  // NotificationsTimeout from the configuration file; error title
  Config.Ini().WriteRaw(QStringLiteral("Configuration\\Interface"), QStringLiteral("NotificationsTimeout"), QStringLiteral("3"));
  Calls.clear();
  CHECK(DesktopIntegration::Notify(QStringLiteral("Site"), QStringLiteral("It's broken"), DesktopIntegration::ntError));
  A = Calls.value(0).Arguments;
  CHECK(A.last() == QStringLiteral("3000"));
  CHECK(A.contains(QStringLiteral("'Site'")));
  CHECK(A.contains(QStringLiteral("'It\\'s broken'")));
  CHECK(A.contains(QStringLiteral("'dialog-error'")));

  // BalloonNotifications off: no notification
  Config.BalloonNotifications = false;
  Calls.clear();
  CHECK(!DesktopIntegration::Notify(QString(), QStringLiteral("x")));
  CHECK(Calls.isEmpty());
  Config.BalloonNotifications = true;

  // taskbar progress: LauncherEntry signal, throttled, hidden at the end
  CHECK(DesktopIntegration::ProgressTitlePercent(QStringLiteral("45% Uploading - site")) == 45);
  CHECK(DesktopIntegration::ProgressTitlePercent(QStringLiteral("Uploading")) == -1);
  Calls.clear();
  DesktopIntegration::SetProgressTitle(QStringLiteral("12% Downloading"));
  CHECK(Calls.size() == 1);
  A = Calls.value(0).Arguments;
  CHECK(A.contains(QStringLiteral("com.canonical.Unity.LauncherEntry.Update")));
  CHECK(A.contains(QStringLiteral("'application://nixscp.desktop'")));
  CHECK(A.last() == QStringLiteral("{'progress': <0.12>, 'progress-visible': <true>}"));
  DesktopIntegration::SetProgressTitle(QStringLiteral("13% Downloading"));
  DesktopIntegration::SetProgressTitle(QStringLiteral("14% Downloading"));
  CHECK(Calls.size() == 1);   // within 500 ms: coalesced
  CHECK(WaitFor([]() { return Calls.size() == 2; }, 2000));
  CHECK(Calls.value(1).Arguments.last().contains(QStringLiteral("<0.14>")));
  DesktopIntegration::SetProgressTitle(QString());
  CHECK(Calls.size() == 3);
  CHECK(Calls.value(2).Arguments.last().contains(QStringLiteral("'progress-visible': <false>")));

  // main window integration (no tray on the offscreen platform: nothing breaks)
  QMainWindow Window;
  Window.setWindowTitle(QStringLiteral("site - NixSCP"));
  DesktopIntegration::InstallMainWindow(&Window);
  DesktopIntegration::InstallMainWindow(&Window);
  Config.MinimizeToTray = true;
  Window.show();
  Window.showMinimized();
  QApplication::processEvents();
  CHECK(DesktopIntegration::IsMinimizedToTray() == QSystemTrayIcon::isSystemTrayAvailable());
  DesktopIntegration::RestoreFromTray();
  Config.MinimizeToTray = false;

  // operation complete in the background: notification (beep only after BeepOnFinishAfter)
  Calls.clear();
  Window.hide();
  DesktopIntegration::OperationComplete(&Window, 1000);
  CHECK(Calls.size() == 1);
  CHECK(Calls.value(0).Arguments.contains(QStringLiteral("'Operation was completed'")));
  Q_UNUSED(Scratch);
}

static void TestDesktopIcon(const QString & Scratch)
{
  std::printf("- desktop icon\n");
  QString Entry = DesktopIntegration::DesktopEntry(QStringLiteral("Folder/My \"Site\" 100%"), DesktopIntegration::sscSite,
    QString(), QStringLiteral("/usr/bin/nixscp"));
  CHECK(Entry.startsWith(QStringLiteral("[Desktop Entry]\n")));
  CHECK_MSG(Entry.contains(QStringLiteral("Exec=/usr/bin/nixscp \"Folder/My \\\"Site\\\" 100%%\"\n")), Entry);
  CHECK(Entry.contains(QStringLiteral("Name=My \"Site\" 100%\n")));
  CHECK(Entry.contains(QStringLiteral("Comment=Opens site 'Folder/My \"Site\" 100%'\n")));
  QString Workspace = DesktopIntegration::DesktopEntry(QStringLiteral("Work"), DesktopIntegration::sscWorkspace, QString(),
    QStringLiteral("/opt/my apps/nixscp"));
  CHECK(Workspace.contains(QStringLiteral("Exec=\"/opt/my apps/nixscp\" Work /workspace\n")));
  CHECK(Workspace.contains(QStringLiteral("Comment=Opens workspace 'Work'\n")));

  QString Desktop = Scratch + QStringLiteral("/Desktop");
  Calls.clear();
  QString FileName = DesktopIntegration::CreateDesktopIcon(nullptr, QStringLiteral("Folder/site one"), DesktopIntegration::sscSite,
    QString(), false, Desktop);
  CHECK(FileName == Desktop + QStringLiteral("/site one.desktop"));
  CHECK(ReadFile(FileName).contains("Exec="));
  CHECK((QFile::permissions(FileName) & QFileDevice::ExeOwner) != 0);
  CHECK(!Calls.isEmpty() && (Calls.last().Program == QStringLiteral("gio")));

  // confirmation (CONFIRM_CREATE_SHORTCUT): No creates nothing
  AnswerDialogs(QStringLiteral("No"));
  QString None = DesktopIntegration::CreateDesktopIcon(nullptr, QStringLiteral("other"), DesktopIntegration::sscSite,
    QString(), true, Desktop);
  CHECK(None.isEmpty());
  CHECK(!QFileInfo::exists(Desktop + QStringLiteral("/other.desktop")));
}

static void TestLocalOperations(const QString & Scratch)
{
  std::printf("- local copy with progress (worker thread)\n");
  QString Src = Scratch + QStringLiteral("/lsrc");
  QString Dst = Scratch + QStringLiteral("/ldst");
  QDir().mkpath(Src);
  QDir().mkpath(Dst);
  WriteFile(Src + QStringLiteral("/a.txt"), "aaa");
  QWidget Window;
  GuiUserInterface UI(&Window);
  LocalFileOperations::Params P;
  P.Sources = {Src + QStringLiteral("/a.txt")};
  P.TargetDirectory = Dst;
  bool Done = false;
  LocalOperationResult Result;
  DesktopIntegration::RunLocalFileOperation(&Window, &UI, P, [&](const LocalOperationResult & R) { Result = R; Done = true; });
  CHECK(WaitFor([&Done]() { return Done; }));
  CHECK(Result.Ok);
  CHECK(ReadFile(Dst + QStringLiteral("/a.txt")) == "aaa");

  // overwrite prompt through the GUI user interface (from the worker thread)
  WriteFile(Src + QStringLiteral("/a.txt"), "new");
  Done = false;
  AnswerDialogs(QStringLiteral("Yes"));
  DesktopIntegration::RunLocalFileOperation(&Window, &UI, P, [&](const LocalOperationResult & R) { Result = R; Done = true; });
  CHECK(WaitFor([&Done]() { return Done; }));
  CHECK(Result.Ok);
  CHECK_MSG(ReadFile(Dst + QStringLiteral("/a.txt")) == "new", QStringLiteral("skipped %1 processed %2").arg(Result.Skipped).arg(Result.Processed));

  LocalFileOperations::PropertiesParams Props;
  Props.Paths = {Dst + QStringLiteral("/a.txt")};
  Props.ChangeMode = true;
  Props.Mode = 0600;
  Done = false;
  DesktopIntegration::RunLocalProperties(&Window, &UI, Props, [&](const LocalOperationResult & R) { Result = R; Done = true; });
  CHECK(WaitFor([&Done]() { return Done; }));
  struct stat St;
  CHECK(::stat(QFile::encodeName(Dst + QStringLiteral("/a.txt")).constData(), &St) == 0);
  CHECK((St.st_mode & 0777) == 0600);
}

static void TestClipboard(const QString & Scratch)
{
  std::printf("- clipboard formats\n");
  QString A = Scratch + QStringLiteral("/clip a.txt");
  QString B = Scratch + QStringLiteral("/clip b.txt");
  WriteFile(A, "a");
  WriteFile(B, "b");
  ClipboardTransfer::CopyLocalFiles({A, B});
  const QMimeData * Mime = QApplication::clipboard()->mimeData();
  CHECK(Mime != nullptr);
  if (Mime == nullptr)
  {
    return;
  }
  CHECK(Mime->data(ClipboardTransfer::GnomeCopiedFilesMimeType()) ==
    "copy\n" + QUrl::fromLocalFile(A).toEncoded() + "\n" + QUrl::fromLocalFile(B).toEncoded());
  CHECK(Mime->urls().size() == 2);
  CHECK(Mime->data(ClipboardTransfer::KdeCutSelectionMimeType()) == "0");
  ClipboardTransfer::Content C = ClipboardTransfer::Read();
  CHECK(C.Type == ClipboardTransfer::Content::ckLocalFiles);
  CHECK(C.Paths == (QStringList{A, B}));
  CHECK(!C.Cut);
  CHECK(ClipboardTransfer::CanPaste());

  // cut in GNOME / KDE file managers
  QMimeData Gnome;
  Gnome.setData(ClipboardTransfer::GnomeCopiedFilesMimeType(), "cut\n" + QUrl::fromLocalFile(A).toEncoded() + "\n");
  C = ClipboardTransfer::Read(&Gnome);
  CHECK(C.Type == ClipboardTransfer::Content::ckLocalFiles && C.Cut && C.Paths == QStringList{A});
  QMimeData Kde;
  Kde.setUrls({QUrl::fromLocalFile(B), QUrl(QStringLiteral("https://example.com/x"))});
  Kde.setData(ClipboardTransfer::KdeCutSelectionMimeType(), "1");
  C = ClipboardTransfer::Read(&Kde);
  CHECK(C.Type == ClipboardTransfer::Content::ckLocalFiles && C.Cut && C.Paths == QStringList{B});

  // text
  QMimeData Text;
  Text.setText(QStringLiteral("  sftp://joe@example.com/dir/  "));
  C = ClipboardTransfer::Read(&Text);
  CHECK(C.Type == ClipboardTransfer::Content::ckText);
  QString Value;
  CHECK(ClipboardTransfer::ClassifyText(C.Text, false, &Value) == ClipboardTransfer::tkSessionUrl);
  CHECK(Value == QStringLiteral("sftp://joe@example.com/dir/"));
  CHECK(ClipboardTransfer::ClassifyText(QStringLiteral("/var/www"), true, &Value) == ClipboardTransfer::tkPath);
  CHECK(ClipboardTransfer::ClassifyText(Scratch, false, &Value) == ClipboardTransfer::tkPath && Value == Scratch);
  CHECK(ClipboardTransfer::ClassifyText(QStringLiteral("relative/path"), false) == ClipboardTransfer::tkNone);
  CHECK(ClipboardTransfer::ClassifyText(QStringLiteral("/no/such/dir/x"), false) == ClipboardTransfer::tkNone);
  CHECK(ClipboardTransfer::ClassifyText(QStringLiteral("a\nb"), true) == ClipboardTransfer::tkNone);
  QMimeData TwoLines;
  TwoLines.setText(QStringLiteral("a\nb"));
  CHECK(!ClipboardTransfer::CanPaste(&TwoLines));

  std::printf("- remote files: download on request\n");
  // a session object (never opened): the downloader hook replaces the transfer
  SessionData Data = SshSession();
  QWidget Window;
  GuiUserInterface UI(&Window);
  Terminal Term(Data, &UI);
  int Downloads = 0;
  ClipboardTransfer::RemoteFilesMimeData::SetDownloader(
    [&Downloads](Terminal *, const QStringList & Paths, const QString & Target)
    {
      Downloads++;
      for (const QString & Path : Paths)
      {
        WriteFile(Target + QLatin1Char('/') + UnixPath::ExtractFileName(Path), "remote");
      }
      return true;
    });
  // inactive session: nothing offered
  auto * Inactive = ClipboardTransfer::CreateRemoteMimeData(&Term, {QStringLiteral("/r/x.txt")}, &Window, true);
  CHECK(Inactive->data(ClipboardTransfer::UriListMimeType()).isEmpty());
  CHECK(Downloads == 0);
  delete Inactive;
}

//---------------------------------------------------------------------------
// Remote: real session (test SSH server)

static SessionData TestServerSession(const QString & Scratch)
{
  SessionData Data;
  Data.Name = QStringLiteral("wp18");
  Data.FSProtocol = fsSFTP;
  Data.HostName = qEnvironmentVariable("NIXSCP_TEST_SSH_HOST", QStringLiteral("127.0.0.1"));
  Data.PortNumber = qEnvironmentVariableIntValue("NIXSCP_TEST_SSH_PORT");
  Data.UserName = qEnvironmentVariable("NIXSCP_TEST_SSH_USER", qEnvironmentVariable("USER"));
  Data.PublicKeyFile = qEnvironmentVariable("NIXSCP_TEST_SSH_KEY");
  Data.TryAgent = false;
  Data.AuthKI = false;
  Data.HostKey = QStringLiteral("*");
  Data.KnownHostsFile = Scratch + QStringLiteral("/known_hosts");
  return Data;
}

static void TestRemote(const QString & Scratch)
{
  if (qEnvironmentVariable("NIXSCP_TEST_SSH_PORT").isEmpty() || qEnvironmentVariable("NIXSCP_TEST_SSH_KEY").isEmpty())
  {
    std::printf("- remote: SKIPPED (set NIXSCP_TEST_SSH_PORT, NIXSCP_TEST_SSH_KEY)\n");
    return;
  }
  std::printf("- remote: duplicate, drag-out download, Commander drops\n");
  Configuration & Config = Configuration::Instance();
  Config.ConfirmTransferring = false;
  QString Root = Scratch + QStringLiteral("/remote");
  QDir().mkpath(Root + QStringLiteral("/sub"));
  WriteFile(Root + QStringLiteral("/one.txt"), "one");
  WriteFile(Root + QStringLiteral("/two.txt"), "two");

  QWidget Window;
  GuiUserInterface UI(&Window);
  Terminal Term(TestServerSession(Scratch), &UI);
  bool Opened = false;
  Term.Open([&Opened](const OperationResult &) { Opened = true; });
  CHECK(WaitFor([&Opened]() { return Opened; }, 30000));
  CHECK(Term.Active());
  if (!Term.Active())
  {
    return;
  }

  // duplicate (cp -p -r -f in the session shell)
  CHECK(ClipboardTransfer::CanDuplicateRemote(&Term));
  bool Done = false;
  OperationResult Result;
  ClipboardTransfer::DuplicateRemoteFiles(&Term, {Root + QStringLiteral("/one.txt")}, Root + QStringLiteral("/sub"),
    [&](const OperationResult & R) { Result = R; Done = true; });
  CHECK(WaitFor([&Done]() { return Done; }, 20000));
  CHECK_MSG(Result.Ok, Result.Message + QStringLiteral(" ") + Result.MoreMessages);
  CHECK(ReadFile(Root + QStringLiteral("/sub/one.txt")) == "one");
  CHECK(QFileInfo::exists(Root + QStringLiteral("/one.txt")));
  // into the same directory: "name (copy)"
  Done = false;
  ClipboardTransfer::DuplicateRemoteFiles(&Term, {Root + QStringLiteral("/one.txt")}, Root,
    [&](const OperationResult & R) { Result = R; Done = true; }, true);
  CHECK(WaitFor([&Done]() { return Done; }, 20000));
  CHECK(Result.Ok);
  CHECK(ReadFile(Root + QStringLiteral("/one (copy).txt")) == "one");
  // existing target: overwrite prompt (No keeps it)
  WriteFile(Root + QStringLiteral("/sub/one.txt"), "keep");
  Done = false;
  AnswerDialogs(QStringLiteral("No"));
  ClipboardTransfer::DuplicateRemoteFiles(&Term, {Root + QStringLiteral("/one.txt")}, Root + QStringLiteral("/sub"),
    [&](const OperationResult & R) { Result = R; Done = true; });
  CHECK(WaitFor([&Done]() { return Done; }, 20000));
  CHECK(ReadFile(Root + QStringLiteral("/sub/one.txt")) == "keep");

  // drag-out / clipboard: the real download on the first request
  ClipboardTransfer::RemoteFilesMimeData::SetDownloader(nullptr);
  ClipboardTransfer::CopyRemoteFiles(&Term, {Root + QStringLiteral("/two.txt"), Root + QStringLiteral("/sub")}, &Window, -1);
  const QMimeData * Mime = QApplication::clipboard()->mimeData();
  ClipboardTransfer::Content C = ClipboardTransfer::Read();
  CHECK(C.Type == ClipboardTransfer::Content::ckRemoteFiles);
  CHECK(C.Source == &Term);
  CHECK(C.Paths.size() == 2);
  CHECK(Mime->text().contains(Root + QStringLiteral("/two.txt")));   // the paths, no download
  CHECK(C.Remote && !C.Remote->Downloaded());
  QList<QUrl> Urls = Mime->urls();
  CHECK(Urls.size() == 2);
  QString Local = Urls.value(1).toLocalFile();
  CHECK_MSG(ReadFile(Local) == "two", Local);
  CHECK(QFileInfo(Urls.value(0).toLocalFile()).isDir());
  CHECK(ReadFile(Urls.value(0).toLocalFile() + QStringLiteral("/one.txt")) == "keep");
  CHECK(Mime->data(ClipboardTransfer::GnomeCopiedFilesMimeType()).startsWith("copy\nfile://"));
  QString Downloaded = C.Remote ? C.Remote->TemporaryDirectory() : QString();
  CHECK(!Downloaded.isEmpty() && Local.startsWith(Downloaded));
  Term.Close();

  // Commander window: drops
  std::printf("- remote: Commander window drops\n");
  CommanderWindow Commander;
  Commander.show();
  Commander.OpenSession(TestServerSession(Scratch));
  FilePanel * LocalPanel = nullptr;
  FilePanel * RemotePanel = nullptr;
  CHECK(WaitFor([&]()
  {
    for (FilePanel * Panel : Commander.findChildren<FilePanel *>())
    {
      if (Panel->isVisible())
      {
        (Panel->IsRemote() ? RemotePanel : LocalPanel) = Panel;
      }
    }
    return (RemotePanel != nullptr) && (RemotePanel->Provider() != nullptr) && !RemotePanel->IsLoading() &&
      !RemotePanel->CurrentDirectory().isEmpty();
  }, 30000));
  if ((LocalPanel == nullptr) || (RemotePanel == nullptr))
  {
    return;
  }
  // Open in PuTTY (Ctrl+P): the terminal command for the session, starting in the panel's directory
  QAction * Putty = nullptr;
  for (QAction * Action : Commander.findChildren<QAction *>())
  {
    if (Action->text().contains(QStringLiteral("PuTTY")))
    {
      Putty = Action;
    }
  }
  CHECK(Putty != nullptr);
  if (Putty != nullptr)
  {
    Calls.clear();
    Config.PuttyPath = QStringLiteral("myterm -e ssh -p !# !U@!@");
    Putty->trigger();
    CHECK(Calls.size() == 1);
    QStringList A = Calls.value(0).Arguments;
    CHECK(Calls.value(0).Program == QStringLiteral("myterm"));
    CHECK(A.contains(QString::number(qEnvironmentVariableIntValue("NIXSCP_TEST_SSH_PORT"))));
    CHECK(A.contains(QStringLiteral("IdentityAgent=none")));
    CHECK(A.contains(QStringLiteral("-i")));
    CHECK_MSG(A.last().startsWith(QStringLiteral("cd '%1'").arg(RemotePanel->CurrentDirectory())), A.join(QLatin1Char('|')));
    Config.PuttyPath = Configuration::DefaultPuttyPath();
  }

  // Ctrl+drag within the remote panel: duplicate (was a move before)
  QFile::remove(Root + QStringLiteral("/sub/two.txt"));
  emit RemotePanel->DropRequested({Root + QStringLiteral("/two.txt")}, true, Root + QStringLiteral("/sub"), Qt::CopyAction);
  CHECK(WaitFor([&]() { return QFileInfo::exists(Root + QStringLiteral("/sub/two.txt")); }, 20000));
  CHECK(QFileInfo::exists(Root + QStringLiteral("/two.txt")));
  // plain drag within the remote panel: move
  WaitFor([]() { return false; }, 300);
  emit RemotePanel->DropRequested({Root + QStringLiteral("/one (copy).txt")}, true, Root + QStringLiteral("/sub"), Qt::MoveAction);
  CHECK(WaitFor([&]() { return QFileInfo::exists(Root + QStringLiteral("/sub/one (copy).txt")); }, 20000));
  CHECK(WaitFor([&]() { return !QFileInfo::exists(Root + QStringLiteral("/one (copy).txt")); }, 5000));

  // drop from the remote panel onto the local one with DD confirmation off: download, no dialog
  WaitFor([]() { return false; }, 300);
  Config.DDTransferConfirmation = asOff;
  Config.ConfirmTransferring = true;
  QString LocalTarget = Scratch + QStringLiteral("/downloads");
  QDir().mkpath(LocalTarget);
  emit LocalPanel->DropRequested({Root + QStringLiteral("/two.txt")}, true, LocalTarget, Qt::CopyAction);
  CHECK(WaitFor([&]() { return ReadFile(LocalTarget + QStringLiteral("/two.txt")) == "two"; }, 20000));

  // local panel drop: local copy engine, then move
  WaitFor([]() { return false; }, 300);
  QString LocalSub = Scratch + QStringLiteral("/downloads/sub");
  QDir().mkpath(LocalSub);
  emit LocalPanel->DropRequested({LocalTarget + QStringLiteral("/two.txt")}, false, LocalSub, Qt::CopyAction);
  CHECK(WaitFor([&]() { return ReadFile(LocalSub + QStringLiteral("/two.txt")) == "two"; }, 10000));
  CHECK(QFileInfo::exists(LocalTarget + QStringLiteral("/two.txt")));
  QDir().mkpath(LocalSub + QStringLiteral("/deeper"));
  emit LocalPanel->DropRequested({LocalSub + QStringLiteral("/two.txt")}, false, LocalSub + QStringLiteral("/deeper"), Qt::MoveAction);
  CHECK(WaitFor([&]() { return QFileInfo::exists(LocalSub + QStringLiteral("/deeper/two.txt")); }, 10000));
  CHECK(!QFileInfo::exists(LocalSub + QStringLiteral("/two.txt")));
  Config.DDTransferConfirmation = asAuto;
}

static void TestCommanderLocalDrop(const QString & Scratch)
{
  std::printf("- Commander window: local drop without a session\n");
  CommanderWindow Commander;
  Commander.show();
  FilePanel * LocalPanel = nullptr;
  for (FilePanel * Panel : Commander.findChildren<FilePanel *>())
  {
    if (!Panel->IsRemote())
    {
      LocalPanel = Panel;
    }
  }
  CHECK(LocalPanel != nullptr);
  if (LocalPanel == nullptr)
  {
    return;
  }
  QString Src = Scratch + QStringLiteral("/cw-src");
  QString Dst = Scratch + QStringLiteral("/cw-dst");
  QDir().mkpath(Src + QStringLiteral("/tree"));
  QDir().mkpath(Dst);
  WriteFile(Src + QStringLiteral("/tree/f.txt"), "f");
  if (symlink("f.txt", QFile::encodeName(Src + QStringLiteral("/tree/l")).constData()) != 0) { }
  emit LocalPanel->DropRequested({Src + QStringLiteral("/tree")}, false, Dst, Qt::CopyAction);
  CHECK(WaitFor([&]() { return ReadFile(Dst + QStringLiteral("/tree/f.txt")) == "f"; }));
  CHECK(WaitFor([&]() { return QFileInfo(Dst + QStringLiteral("/tree/l")).isSymLink(); }));
  // into itself: error (Retry/Skip/Abort), nothing moved
  AnswerDialogs(QStringLiteral("Abort"), 5000);
  emit LocalPanel->DropRequested({Src + QStringLiteral("/tree")}, false, Src + QStringLiteral("/tree"), Qt::MoveAction);
  WaitFor([]() { return false; }, 1500);
  CHECK(QFileInfo::exists(Src + QStringLiteral("/tree/f.txt")));
}

int main(int argc, char ** argv)
{
  QTemporaryDir Scratch;
  qputenv("XDG_CONFIG_HOME", Scratch.filePath(QStringLiteral("config")).toUtf8());
  qputenv("XDG_DATA_HOME", Scratch.filePath(QStringLiteral("data")).toUtf8());
  qputenv("XDG_RUNTIME_DIR", Scratch.filePath(QStringLiteral("run")).toUtf8());
  QDir().mkpath(Scratch.filePath(QStringLiteral("run")));
  QFile::setPermissions(Scratch.filePath(QStringLiteral("run")), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
  QApplication App(argc, argv);
  Configuration::Instance().SetIniFileName(Scratch.filePath(QStringLiteral("nixscp.ini")));
  Configuration::Instance().Load();
  Configuration::Instance().ConfirmClosingSession = false;
  Configuration::Instance().ShowLoginOnStartup = false;
  InstallRunner();

  QString Path = Scratch.path();
  TestTerminalCommand(Path);
  TestExploreAndPlaces(Path);
  TestAttention(Path);
  TestDesktopIcon(Path);
  TestLocalOperations(Path);
  TestClipboard(Path);
  TestCommanderLocalDrop(Path);
  TestRemote(Path);

  DesktopIntegration::SetCommandRunner(nullptr);
  std::printf("%s: %d checks, %d failures\n", Failures ? "FAILED" : "OK", Checks, Failures);
  return Failures ? 1 : 0;
}

#endif
