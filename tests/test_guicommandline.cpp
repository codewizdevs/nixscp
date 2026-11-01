// Offscreen GUI test of the GUI command line (gui/GuiCommandLine, port of
// WinSCP windows/WinMain.cpp + ParseUrl flags) and the single instance
// channel (gui/SingleInstance): URL schemes, URL pointing to a file, ";save",
// stored site lookup with protocol, CheckSafe / pufUnsafe, site folder and
// workspace arguments, logging and configuration switches, URL handler
// registration, the Save session dialog, "Show Login dialog when the last
// session is closed" and - with NIXSCP_TEST_SSH_PORT (+ _KEY, _USER, _HOST) -
// the standalone download / upload and sessions from another instance.
// NIXSCP_GUI_TESTS.
#ifndef NIXSCP_GUI_TESTS

#include <cstdio>

int main()
{
  std::printf("SKIP: test_guicommandline needs NIXSCP_GUI_TESTS (Qt Widgets + the GUI sources)\n");
  return 0;
}

#else

#include "cli/Options.h"
#include "cli/SessionUrl.h"
#include "core/Configuration.h"
#include "core/FileSystem.h"
#include "core/Terminal.h"
#include "gui/CommanderWindow.h"
#include "gui/FilePanel.h"
#include "gui/GuiUserInterface.h"
#include "gui/GuiCommandLine.h"
#include "gui/LoginDialog.h"
#include "gui/SingleInstance.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QTemporaryDir>
#include <QTimer>

#include <atomic>
#include <cstdio>
#include <functional>
#include <thread>

using namespace GuiCommandLine;

static int Failures = 0;

#define CHECK(Cond) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); std::fflush(stdout); Failures++; } } while (0)
#define CHECK_EQ(A, B) \
  do { auto VA = (A); auto VB = (B); if (!(VA == VB)) { std::printf("FAIL %s:%d: %s == %s (%s vs %s)\n", __FILE__, __LINE__, #A, #B, qPrintable(QVariant(VA).toString()), qPrintable(QVariant(VB).toString())); std::fflush(stdout); Failures++; } } while (0)

static bool WaitUntil(const std::function<bool()> & Condition, int TimeoutMs)
{
  QElapsedTimer Timer;
  Timer.start();
  while (!Condition())
  {
    if (Timer.elapsed() > TimeoutMs)
    {
      return false;
    }
    QEventLoop Loop;
    QTimer::singleShot(20, &Loop, &QEventLoop::quit);
    Loop.exec();
  }
  return true;
}

static void WriteFile(const QString & Path, const QByteArray & Data)
{
  QDir().mkpath(QFileInfo(Path).absolutePath());
  QFile F(Path);
  CHECK(F.open(QIODevice::WriteOnly));
  F.write(Data);
}

static QByteArray ReadFile(const QString & Path)
{
  QFile F(Path);
  if (!F.open(QIODevice::ReadOnly))
  {
    return QByteArray();
  }
  return F.readAll();
}

static Options MakeOptions(const QStringList & Args)
{
  Options Result;
  for (const QString & Arg : Args)
  {
    Result.Add(Arg);
  }
  return Result;
}

static LoginDialog * VisibleLoginDialog()
{
  for (QWidget * W : QApplication::topLevelWidgets())
  {
    if (auto * Dialog = qobject_cast<LoginDialog *>(W))
    {
      if (Dialog->isVisible())
      {
        return Dialog;
      }
    }
  }
  return nullptr;
}

// Rejects the next Login dialog (when it appears); counts them
static int RejectedLoginDialogs = 0;
static void RejectLoginDialogsFor(int Ms)
{
  auto * Timer = new QTimer(qApp);
  auto Elapsed = std::make_shared<QElapsedTimer>();
  Elapsed->start();
  QObject::connect(Timer, &QTimer::timeout, Timer, [Timer, Elapsed, Ms]()
  {
    if (LoginDialog * Dialog = VisibleLoginDialog())
    {
      if (!qEnvironmentVariableIsEmpty("NIXSCP_SCREENSHOT_DIR"))
      {
        Dialog->grab().save(qEnvironmentVariable("NIXSCP_SCREENSHOT_DIR") + QStringLiteral("/guicommandline-login%1.png").arg(RejectedLoginDialogs));
      }
      RejectedLoginDialogs++;
      Dialog->reject();
    }
    if (Elapsed->elapsed() > Ms)
    {
      Timer->deleteLater();
    }
  });
  Timer->start(50);
}

static void SetupSites()
{
  StoredSessionList & Stored = Configuration::Instance().StoredSessions();
  SessionData Host;
  Host.HostName = QStringLiteral("myhost.example.com");
  Host.UserName = QStringLiteral("siteuser");
  Host.PortNumber = 2200;
  Stored.NewSession(QStringLiteral("myhost.example.com"), Host);
  SessionData Server;
  Server.HostName = QStringLiteral("server.example.com");
  Server.UserName = QStringLiteral("martin");
  Server.RemoteDirectory = QStringLiteral("/home/martin");
  Stored.NewSession(QStringLiteral("Work/Server"), Server);
  SessionData Other;
  Other.HostName = QStringLiteral("other.example.com");
  Stored.NewSession(QStringLiteral("Work/Other"), Other);
  SessionData Ws;
  Ws.HostName = QStringLiteral("ws.example.com");
  Ws.IsWorkspace = true;
  Stored.NewSession(QStringLiteral("My WS/0000"), Ws);
}

static void TestParseUrl()
{
  StoredSessionList & Stored = Configuration::Instance().StoredSessions();

  ParsedUrl P = ParseUrl(QStringLiteral("ftpes://user@host.example.com/dir/"), nullptr, Stored, false);
  CHECK(P.Data.FSProtocol == fsFTP);
  CHECK(P.Data.Ftps == ftpsExplicitTls);
  CHECK_EQ(P.Data.PortNumber, 21);
  CHECK_EQ(P.Data.HostName, QStringLiteral("host.example.com"));
  CHECK_EQ(P.Data.UserName, QStringLiteral("user"));
  CHECK_EQ(P.Data.RemoteDirectory, QStringLiteral("/dir/"));
  CHECK(P.DownloadFile.isEmpty());
  CHECK(!P.SaveOnly && !P.PuttyProtocol);
  CHECK((P.ParsedInfo & piProtocolDefined) != 0);

  P = ParseUrl(QStringLiteral("dav://host/"), nullptr, Stored, false);
  CHECK(P.Data.FSProtocol == fsWebDAV);
  CHECK(P.Data.Ftps == ftpsNone);
  CHECK_EQ(P.Data.PortNumber, 80);
  P = ParseUrl(QStringLiteral("davs://host:8443/x/"), nullptr, Stored, false);
  CHECK(P.Data.FSProtocol == fsWebDAV);
  CHECK(P.Data.Ftps == ftpsImplicit);
  CHECK_EQ(P.Data.PortNumber, 8443);
  P = ParseUrl(QStringLiteral("s3://s3.amazonaws.com/bucket/"), nullptr, Stored, false);
  CHECK(P.Data.FSProtocol == fsS3);
  CHECK_EQ(P.Data.PortNumber, 443);
  P = ParseUrl(QStringLiteral("scp://host/"), nullptr, Stored, false);
  CHECK(P.Data.FSProtocol == fsSCPonly);
  P = ParseUrl(QStringLiteral("ftps://host/"), nullptr, Stored, false);
  CHECK((P.Data.FSProtocol == fsFTP) && (P.Data.Ftps == ftpsImplicit) && (P.Data.PortNumber == 990));
  P = ParseUrl(QStringLiteral("ssh://host/"), nullptr, Stored, false);
  CHECK(P.PuttyProtocol);
  P = ParseUrl(QStringLiteral("winscp-sftp://host:2022/"), nullptr, Stored, false);
  CHECK(P.Data.FSProtocol == fsSFTPonly);
  CHECK_EQ(P.Data.PortNumber, 2022);
  CHECK(!P.PuttyProtocol);

  // URL pointing to a file
  P = ParseUrl(QStringLiteral("sftp://host/path/file%20x.txt"), nullptr, Stored, false);
  CHECK_EQ(P.DownloadFile, QStringLiteral("file x.txt"));
  CHECK_EQ(P.Data.RemoteDirectory, QStringLiteral("/path/"));
  P = ParseUrl(QStringLiteral("sftp://host/file"), nullptr, Stored, false);
  CHECK_EQ(P.DownloadFile, QStringLiteral("file"));
  CHECK_EQ(P.Data.RemoteDirectory, QStringLiteral("/"));
  P = ParseUrl(QStringLiteral("sftp://host"), nullptr, Stored, false);
  CHECK(P.DownloadFile.isEmpty());

  // ;save
  P = ParseUrl(QStringLiteral("sftp://user@host/;save"), nullptr, Stored, false);
  CHECK(P.SaveOnly);
  CHECK(P.DownloadFile.isEmpty());
  P = ParseUrl(QStringLiteral("sftp://user@host/dir/;save=0"), nullptr, Stored, false);
  CHECK(!P.SaveOnly);

  // Stored site with protocol (pufAllowStoredSiteWithProtocol) and site/path
  P = ParseUrl(QStringLiteral("sftp://myhost.example.com/"), nullptr, Stored, false);
  CHECK_EQ(P.Data.UserName, QStringLiteral("siteuser"));
  CHECK_EQ(P.Data.PortNumber, 2200);
  CHECK(P.Data.FSProtocol == fsSFTPonly);
  P = ParseUrl(QStringLiteral("Work/Server/sub/file.txt"), nullptr, Stored, false);
  CHECK_EQ(P.Data.HostName, QStringLiteral("server.example.com"));
  CHECK_EQ(P.DownloadFile, QStringLiteral("file.txt"));
  CHECK_EQ(P.Data.RemoteDirectory, QStringLiteral("/sub/"));
  P = ParseUrl(QStringLiteral("Work/Server"), nullptr, Stored, false);
  CHECK_EQ(P.Data.RemoteDirectory, QStringLiteral("/home/martin"));
  CHECK(P.DownloadFile.isEmpty());

  // Switches
  Options Opts = MakeOptions({QStringLiteral("ftp://host/"), QStringLiteral("/passive=off"), QStringLiteral("/hostkey=SHA256:abc"),
    QStringLiteral("/username=switchuser"), QStringLiteral("/timeout=33")});
  P = ParseUrl(QStringLiteral("ftp://host/"), &Opts, Stored, false);
  CHECK(!P.Data.FtpPasvMode);
  CHECK_EQ(P.Data.HostKey, QStringLiteral("SHA256:abc"));
  CHECK_EQ(P.Data.UserName, QStringLiteral("switchuser"));
  CHECK_EQ(P.Data.Timeout, 33);

  // CheckSafe / pufUnsafe
  Options Safe = MakeOptions({QStringLiteral("sftp://host/")});
  CHECK(CheckSafe(Safe));
  Options UnsafeOpts = MakeOptions({QStringLiteral("/Unsafe"), QStringLiteral("sftp://host/")});
  CHECK(!CheckSafe(UnsafeOpts));
  Options UnsafeLower = MakeOptions({QStringLiteral("-unsafe")});
  CHECK(!CheckSafe(UnsafeLower));
  QString Default = Stored.DefaultSettings().Shell;
  P = ParseUrl(QStringLiteral("sftp://user;x-Shell=evil;x-Compression=1@host/"), nullptr, Stored, false);
  CHECK_EQ(P.Data.Shell, QStringLiteral("evil"));
  CHECK(P.Data.Compression);
  P = ParseUrl(QStringLiteral("sftp://user;x-Shell=evil;x-Compression=1@host/"), nullptr, Stored, true);
  CHECK_EQ(P.Data.Shell, Default);
  CHECK(P.Data.Compression);
  CHECK_EQ(P.Data.UserName, QStringLiteral("user"));
  Options Raw = MakeOptions({QStringLiteral("sftp://host/"), QStringLiteral("/rawsettings"), QStringLiteral("SftpServer=/bin/evil"),
    QStringLiteral("PostLoginCommands=rm"), QStringLiteral("Compression=1")});
  P = ParseUrl(QStringLiteral("sftp://host/"), &Raw, Stored, true);
  CHECK(P.Data.SftpServer != QStringLiteral("/bin/evil"));
  CHECK(P.Data.PostLoginCommands.isEmpty());
  CHECK(P.Data.Compression);
  Options Raw2 = MakeOptions({QStringLiteral("sftp://host/"), QStringLiteral("/rawsettings"), QStringLiteral("SftpServer=/bin/sftp")});
  P = ParseUrl(QStringLiteral("sftp://host/"), &Raw2, Stored, false);
  CHECK_EQ(P.Data.SftpServer, QStringLiteral("/bin/sftp"));
}

static void TestFolderAndLoginData()
{
  StoredSessionList & Stored = Configuration::Instance().StoredSessions();
  CHECK_EQ(FolderOrWorkspaceName(QStringLiteral("Work"), nullptr, false, Stored), QStringLiteral("Work"));
  CHECK(FolderOrWorkspaceName(QStringLiteral("Work/Server"), nullptr, false, Stored).isEmpty());
  CHECK_EQ(FolderOrWorkspaceName(QStringLiteral("My%20WS"), nullptr, false, Stored), QStringLiteral("My WS"));
  CHECK(FolderOrWorkspaceName(QStringLiteral("sftp://host/"), nullptr, false, Stored).isEmpty());

  // Site folder opens all its sites
  Options Opts = MakeOptions({QStringLiteral("Work")});
  LoginData Data = GetLoginData(QStringLiteral("Work"), Opts, lnNone, nullptr, false);
  CHECK(!Data.Aborted);
  CHECK_EQ(static_cast<int>(Data.Sessions.size()), 2);
  CHECK(Data.Workspace.isEmpty());
  // Workspace
  Options WsOpts = MakeOptions({QStringLiteral("My WS")});
  Data = GetLoginData(QStringLiteral("My WS"), WsOpts, lnNone, nullptr, false);
  CHECK_EQ(Data.Workspace, QStringLiteral("My WS"));
  // URL
  Options UrlOpts = MakeOptions({QStringLiteral("davs://user@dav.example.com/folder/file.bin")});
  Data = GetLoginData(QStringLiteral("davs://user@dav.example.com/folder/file.bin"), UrlOpts, lnNone, nullptr, false);
  CHECK_EQ(static_cast<int>(Data.Sessions.size()), 1);
  CHECK_EQ(Data.DownloadFile, QStringLiteral("file.bin"));
  CHECK(Data.Sessions.value(0).FSProtocol == fsWebDAV);
  // No session, nothing needed: noop
  Options Empty;
  Data = GetLoginData(QString(), Empty, lnNone, nullptr, false);
  CHECK(Data.Sessions.isEmpty() && !Data.Aborted);
  // No session but one is needed: Login dialog (cancelled)
  RejectedLoginDialogs = 0;
  RejectLoginDialogsFor(3000);
  Data = GetLoginData(QString(), Empty, lnTerminal, nullptr, false);
  CHECK(Data.Aborted);
  CHECK_EQ(RejectedLoginDialogs, 1);

  // ";save": Save session dialog, then the Login dialog
  RejectedLoginDialogs = 0;
  bool SaveDialogSeen = false;
  auto * Timer = new QTimer(qApp);
  QObject::connect(Timer, &QTimer::timeout, Timer, [&SaveDialogSeen, Timer]()
  {
    for (QWidget * W : QApplication::topLevelWidgets())
    {
      if ((W->objectName() == QStringLiteral("SaveSessionDialog")) && W->isVisible())
      {
        auto * Dialog = qobject_cast<QDialog *>(W);
        SaveDialogSeen = true;
        CHECK_EQ(Dialog->windowTitle(), QStringLiteral("Save session as site"));
        auto * Name = Dialog->findChild<QLineEdit *>(QStringLiteral("SessionNameEdit"));
        auto * Folder = Dialog->findChild<QComboBox *>(QStringLiteral("FolderCombo"));
        auto * SavePassword = Dialog->findChild<QCheckBox *>(QStringLiteral("SavePasswordCheck"));
        CHECK(Name && Folder && SavePassword);
        if (Name && Folder && SavePassword)
        {
          CHECK_EQ(Name->text(), QStringLiteral("saver@save.example.com"));
          CHECK_EQ(Folder->currentText(), QStringLiteral("<none>"));
          CHECK(SavePassword->isEnabled());
          Name->setText(QStringLiteral("Saved site"));
          Folder->setCurrentText(QStringLiteral("Work"));
          SavePassword->setChecked(true);
          if (!qEnvironmentVariableIsEmpty("NIXSCP_SCREENSHOT_DIR"))
          {
            Dialog->grab().save(qEnvironmentVariable("NIXSCP_SCREENSHOT_DIR") + QStringLiteral("/guicommandline-savesession.png"));
          }
          Dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        }
        Timer->deleteLater();
        return;
      }
    }
  });
  Timer->start(50);
  RejectLoginDialogsFor(5000);
  Options SaveOpts = MakeOptions({QStringLiteral("sftp://saver:pw@save.example.com/;save")});
  Data = GetLoginData(QStringLiteral("sftp://saver:pw@save.example.com/;save"), SaveOpts, lnNone, nullptr, false);
  CHECK(SaveDialogSeen);
  CHECK(Data.Aborted);
  CHECK_EQ(RejectedLoginDialogs, 1);
  auto Saved = Stored.FindByName(QStringLiteral("Work/Saved site"));
  CHECK(Saved != nullptr);
  if (Saved)
  {
    CHECK_EQ(Saved->HostName, QStringLiteral("save.example.com"));
    CHECK_EQ(Saved->Password, QStringLiteral("pw"));
  }
  // persisted in the configuration file
  CHECK(ReadFile(Configuration::Instance().IniFileName()).contains("Saved%20site"));
}

static void TestSwitches(const QString & Scratch)
{
  Configuration & Config = Configuration::Instance();
  Options Opts = MakeOptions({QStringLiteral("/loglevel=2*"), QStringLiteral("/logsize=5*10M"),
    QStringLiteral("/rawtransfersettings"), QStringLiteral("PreserveTime=0")});
  Config.DefaultCopyParam().PreserveTime = true;
  ApplyLoggingAndTransferSwitches(Opts);
  CHECK_EQ(Config.LogProtocol, 2);
  CHECK(Config.LogSensitive);
  CHECK_EQ(Config.LogMaxCount, 5);
  CHECK_EQ(Config.LogMaxSize, 10LL * 1024 * 1024);
  CHECK(!Config.DefaultCopyParam().PreserveTime);
  Options Level = MakeOptions({QStringLiteral("/loglevel=-1*-")});
  ApplyLoggingAndTransferSwitches(Level);
  CHECK_EQ(Config.LogProtocol, -1);
  CHECK(!Config.LogSensitive);

  Config.Logging = false;
  Options UnsafeLog = MakeOptions({QStringLiteral("/unsafe"), QStringLiteral("/log=/tmp/evil.log")});
  ApplyLogSwitches(UnsafeLog);
  CHECK(!Config.Logging);
  Options Log = MakeOptions({QStringLiteral("/log=") + Scratch + QStringLiteral("/s.log"), QStringLiteral("/xmllog=x.xml")});
  ApplyLogSwitches(Log);
  CHECK(Config.Logging);
  CHECK_EQ(Config.LogFileName, Scratch + QStringLiteral("/s.log"));
  CHECK(Config.LogActions);
  CHECK_EQ(Config.ActionsLogFileName, QStringLiteral("x.xml"));
  Config.Logging = false;
  Config.LogActions = false;

  // /ini: ignored with /unsafe (except nul), reported when missing
  QString Ini = Config.IniFileName();
  Options UnsafeIni = MakeOptions({QStringLiteral("/unsafe"), QStringLiteral("/ini=") + Scratch + QStringLiteral("/other.ini")});
  QString Missing;
  ApplyIniSwitch(UnsafeIni, &Missing);
  CHECK_EQ(Config.IniFileName(), Ini);
  CHECK(Missing.isEmpty());
  Options OtherIni = MakeOptions({QStringLiteral("/ini=") + Scratch + QStringLiteral("/other.ini")});
  ApplyIniSwitch(OtherIni, &Missing);
  CHECK_EQ(Config.IniFileName(), Scratch + QStringLiteral("/other.ini"));
  CHECK_EQ(Missing, Scratch + QStringLiteral("/other.ini"));
  Options NulIni = MakeOptions({QStringLiteral("/unsafe"), QStringLiteral("/ini=nul")});
  ApplyIniSwitch(NulIni);
  CHECK(Config.IsNulStorage());
  Config.SetIniFileName(Ini);
  Config.Load();
  CHECK(!Config.IsNulStorage());
  CHECK(Config.StoredSessions().FindByName(QStringLiteral("Work/Saved site")) != nullptr);

  // /privatekey: default key of sessions without one
  SetDefaultKeyFile(QStringLiteral("/keys/id"));
  SessionData Data;
  Data.HostName = QStringLiteral("h");
  CHECK_EQ(PrepareSession(Data).PublicKeyFile, QStringLiteral("/keys/id"));
  Data.PublicKeyFile = QStringLiteral("/own");
  CHECK_EQ(PrepareSession(Data).PublicKeyFile, QStringLiteral("/own"));
  SetDefaultKeyFile(QString());

  // Console modes are recognized (not run here)
  CHECK_EQ(RunConsoleMode({QStringLiteral("sftp://host/")}), -1);
  CHECK_EQ(RunConsoleMode({QStringLiteral("/console=6.0")}), -1);
}

static void TestUrlHandler(const QString & Scratch)
{
  // fake xdg-mime / update-desktop-database: never touch the real desktop
  QString Bin = Scratch + QStringLiteral("/bin");
  QString LogFile = Scratch + QStringLiteral("/xdg-mime.log");
  WriteFile(Bin + QStringLiteral("/xdg-mime"), ("#!/bin/sh\necho \"$@\" >> " + LogFile + "\n").toUtf8());
  WriteFile(Bin + QStringLiteral("/update-desktop-database"), QByteArray("#!/bin/sh\nexit 0\n"));
  QFile::setPermissions(Bin + QStringLiteral("/xdg-mime"), QFileDevice::ReadOwner | QFileDevice::ExeOwner);
  QFile::setPermissions(Bin + QStringLiteral("/update-desktop-database"), QFileDevice::ReadOwner | QFileDevice::ExeOwner);
  QByteArray OldPath = qgetenv("PATH");
  qputenv("PATH", Bin.toUtf8() + ":/usr/bin:/bin");
  QString Error;
  CHECK(RegisterForDefaultProtocols(&Error));
  QString FileName = UrlHandlerDesktopFileName();
  CHECK(FileName.startsWith(Scratch));
  QByteArray Content = ReadFile(FileName);
  CHECK(Content.contains("\" /unsafe %u"));
  CHECK(Content.contains("x-scheme-handler/ftpes;"));
  CHECK(Content.contains("x-scheme-handler/s3;"));
  QByteArray Log = ReadFile(LogFile);
  CHECK(Log.contains("default nixscp-url-handler.desktop x-scheme-handler/sftp"));
  CHECK(UnregisterForProtocols(&Error));
  CHECK(!QFile::exists(FileName));
  qputenv("PATH", OldPath);
}

static void TestSingleInstance(const QString & Scratch)
{
  QString Name = Scratch + QStringLiteral("/instance.sock");
  SingleInstance Server(Name);
  CHECK(Server.Listen());
  SingleInstance Second(Name);
  CHECK(!Second.Listen());

  QStringList ReceivedArgs;
  QString ReceivedCwd;
  QString ReceivedSession;
  QString ReceivedPath;
  QObject::connect(&Server, &SingleInstance::CommandLineReceived, [&](const QStringList & A, const QString & Cwd, const QString &)
  {
    ReceivedArgs = A;
    ReceivedCwd = Cwd;
  });
  QObject::connect(&Server, &SingleInstance::RefreshRequested, [&](const QString & S, const QString & P)
  {
    ReceivedSession = S;
    ReceivedPath = P;
  });
  bool Accept = true;
  Server.SetCanAcceptCommandLine([&Accept]() { return Accept; });

  auto SendAsync = [](std::function<bool()> Send, std::atomic<int> & Result)
  {
    Result = -1;
    std::thread([Send, &Result]() { Result = Send() ? 1 : 0; }).detach();
  };
  std::atomic<int> Result{-1};
  QStringList Args{QStringLiteral("ftpes://user@host/dir/"), QStringLiteral("/privatekey=key file")};
  SendAsync([Name, Args]() { return SingleInstance::SendCommandLine(Name, Args, QStringLiteral("/some/dir")); }, Result);
  CHECK(WaitUntil([&]() { return (Result >= 0) && !ReceivedArgs.isEmpty(); }, 5000));
  CHECK_EQ(Result.load(), 1);
  CHECK(ReceivedArgs == Args);
  CHECK_EQ(ReceivedCwd, QStringLiteral("/some/dir"));

  // refused while busy (modal dialog in the running instance)
  Accept = false;
  ReceivedArgs.clear();
  SendAsync([Name, Args]() { return SingleInstance::SendCommandLine(Name, Args, QString()); }, Result);
  CHECK(WaitUntil([&]() { return Result >= 0; }, 5000));
  CHECK_EQ(Result.load(), 0);
  WaitUntil([]() { return false; }, 100);
  CHECK(ReceivedArgs.isEmpty());

  SendAsync([Name]() { return SingleInstance::SendRefresh(Name, QStringLiteral("mysite"), QStringLiteral("/var/www")); }, Result);
  CHECK(WaitUntil([&]() { return (Result >= 0) && !ReceivedSession.isEmpty(); }, 5000));
  CHECK_EQ(Result.load(), 1);
  CHECK_EQ(ReceivedSession, QStringLiteral("mysite"));
  CHECK_EQ(ReceivedPath, QStringLiteral("/var/www"));

  // garbage does not crash the server
  SendAsync([Name]() { return SingleInstance::SendCommandLine(Name, QStringList(), QString()); }, Result);
  CHECK(WaitUntil([&]() { return Result >= 0; }, 5000));
  CHECK_EQ(Result.load(), 0);

  // nobody listening
  CHECK(!SingleInstance::SendCommandLine(Scratch + QStringLiteral("/none.sock"), Args, QString(), 500));

  // per configuration file names
  CHECK(SingleInstance::DefaultServerName(QStringLiteral("/a/nixscp.ini")) != SingleInstance::DefaultServerName(QStringLiteral("/b/nixscp.ini")));
  CHECK(SingleInstance::DefaultServerName(QStringLiteral("nul")).contains(QStringLiteral("nixscp-")));

  // a socket left by a crashed instance is replaced
  WriteFile(Scratch + QStringLiteral("/stale2.sock"), QByteArray());
  SingleInstance Stale(Scratch + QStringLiteral("/stale2.sock"));
  CHECK(Stale.Listen());
}

static SessionData TestSession(const QString & Scratch)
{
  SessionData Data;
  Data.FSProtocol = fsSFTPonly;
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

static Terminal * OpenTerminal(const SessionData & Data, UserInterface * UI, QObject * Parent)
{
  auto * Term = new Terminal(Data, UI, Parent);
  bool Done = false;
  Term->Open([&Done](const OperationResult &) { Done = true; });
  CHECK(WaitUntil([&Done]() { return Done; }, 30000));
  CHECK(Term->Active());
  return Term;
}

static void TestLastSessionWatcher(const QString & Scratch)
{
  Configuration & Config = Configuration::Instance();
  Config.ShowLoginOnStartup = true;
  CommanderWindow Window;
  Window.show();
  auto * Watcher = new LastSessionWatcher(&Window);
  Watcher->SetInterval(50);
  SessionData Data = TestSession(Scratch);
  GuiUserInterface UI(&Window);

  // last session closed: Login dialog (with the ShowAgainPanel)
  RejectedLoginDialogs = 0;
  Terminal * Term = OpenTerminal(Data, &UI, &Window);
  RejectLoginDialogsFor(3000);
  delete Term;
  CHECK(WaitUntil([]() { return RejectedLoginDialogs == 1; }, 3000));
  WaitUntil([]() { return false; }, 300);
  CHECK_EQ(RejectedLoginDialogs, 1);

  // two sessions, one closed: nothing
  RejectedLoginDialogs = 0;
  Terminal * T1 = OpenTerminal(Data, &UI, &Window);
  Terminal * T2 = OpenTerminal(Data, &UI, &Window);
  RejectLoginDialogsFor(1000);
  delete T1;
  WaitUntil([]() { return false; }, 500);
  CHECK_EQ(RejectedLoginDialogs, 0);

  // the window shows a Login dialog itself after the session failed (as
  // CommanderWindow does after a failed connect; the dead session is deleted
  // only after the dialog): not twice
  RejectedLoginDialogs = 0;
  T2->Close();
  WaitUntil([T2]() { return !T2->Active(); }, 5000);
  RejectLoginDialogsFor(3000);
  QTimer::singleShot(0, &Window, [&Window, T2]()
  {
    T2->deleteLater();
    LoginDialog Dialog(Configuration::Instance().StoredSessions(), &Window);
    Dialog.exec();
  });
  CHECK(WaitUntil([]() { return RejectedLoginDialogs >= 1; }, 3000));
  WaitUntil([]() { return false; }, 800);
  CHECK_EQ(RejectedLoginDialogs, 1);

  // option off: nothing
  Config.ShowLoginOnStartup = false;
  RejectedLoginDialogs = 0;
  Term = OpenTerminal(Data, &UI, &Window);
  RejectLoginDialogsFor(800);
  delete Term;
  WaitUntil([]() { return false; }, 600);
  CHECK_EQ(RejectedLoginDialogs, 0);
  Config.ShowLoginOnStartup = true;

  // the window handles it itself
  Window.setProperty("HandlesLastSessionClosed", true);
  Term = OpenTerminal(Data, &UI, &Window);
  RejectLoginDialogsFor(800);
  delete Term;
  WaitUntil([]() { return false; }, 600);
  CHECK_EQ(RejectedLoginDialogs, 0);
}

static void TestStandaloneOperations(const QString & Scratch)
{
  // URL pointing to a file: download (Copy dialog accepted by /defaults=1)
  QString RemoteDir = Scratch + QStringLiteral("/remote");
  WriteFile(RemoteDir + QStringLiteral("/down load.txt"), QByteArray("downloaded content"));
  SessionData Data = TestSession(Scratch);
  Data.LocalDirectory = Scratch + QStringLiteral("/local");
  QDir().mkpath(Data.LocalDirectory);
  {
    int ExitCode = -1;
    CommandLineOperation Op(CommandLineOperation::okDownload, Data, {RemoteDir + QStringLiteral("/down load.txt")}, 1);
    Op.Finished = [&ExitCode](int Code) { ExitCode = Code; };
    Op.Start();
    CHECK(WaitUntil([&]() { return ExitCode >= 0; }, 30000));
    CHECK_EQ(ExitCode, 0);
    CHECK_EQ(ReadFile(Data.LocalDirectory + QStringLiteral("/down load.txt")), QByteArray("downloaded content"));
  }
  // missing file: FILE_NOT_EXISTS error, exit code 1
  {
    int ExitCode = -1;
    QString ErrorText;
    auto * Timer = new QTimer(qApp);
    QObject::connect(Timer, &QTimer::timeout, Timer, [&ErrorText]()
    {
      if (QWidget * W = QApplication::activeModalWidget())
      {
        if (auto * Dialog = qobject_cast<QDialog *>(W))
        {
          for (QLabel * Label : Dialog->findChildren<QLabel *>())
          {
            ErrorText += Label->text();
          }
          Dialog->reject();
        }
      }
    });
    Timer->start(50);
    CommandLineOperation Op(CommandLineOperation::okDownload, Data, {RemoteDir + QStringLiteral("/missing.txt")}, 0);
    Op.Finished = [&ExitCode](int Code) { ExitCode = Code; };
    Op.Start();
    CHECK(WaitUntil([&]() { return ExitCode >= 0; }, 30000));
    delete Timer;
    CHECK_EQ(ExitCode, 1);
    CHECK(ErrorText.contains(QStringLiteral("does not exist")));
  }
  // /upload file1 file2 into the session's remote directory (no dialog: /defaults)
  {
    WriteFile(Scratch + QStringLiteral("/up/a.txt"), QByteArray("A"));
    WriteFile(Scratch + QStringLiteral("/up/b.txt"), QByteArray("BB"));
    QString Target = Scratch + QStringLiteral("/uptarget");
    QDir().mkpath(Target);
    SessionData UpData = TestSession(Scratch);
    UpData.RemoteDirectory = Target;
    int ExitCode = -1;
    CommandLineOperation Op(CommandLineOperation::okUpload, UpData,
      {Scratch + QStringLiteral("/up/a.txt"), Scratch + QStringLiteral("/up/b.txt")}, 0);
    Op.Finished = [&ExitCode](int Code) { ExitCode = Code; };
    Op.Start();
    CHECK(WaitUntil([&]() { return ExitCode >= 0; }, 30000));
    CHECK_EQ(ExitCode, 0);
    CHECK_EQ(ReadFile(Target + QStringLiteral("/a.txt")), QByteArray("A"));
    CHECK_EQ(ReadFile(Target + QStringLiteral("/b.txt")), QByteArray("BB"));
  }
}

static void TestSessionsFromAnotherInstance(const QString & Scratch)
{
  CommanderWindow Window;
  Window.resize(1000, 700);
  Window.show();
  CHECK(CanCommandLineFromAnotherInstance(&Window));
  SessionData Data = TestSession(Scratch);
  QString Dir = Scratch + QStringLiteral("/remote");
  QString Url = QStringLiteral("sftp://%1@%2:%3%4/").arg(Data.UserName, Data.HostName).arg(Data.PortNumber).arg(Dir);
  QStringList Args{Url, QStringLiteral("/privatekey=") + Data.PublicKeyFile, QStringLiteral("/hostkey=*"),
    QStringLiteral("/rawsettings"), QStringLiteral("TryAgent=0"), QStringLiteral("AuthKI=0"),
    QStringLiteral("/browse=") + Dir + QStringLiteral("/down load.txt")};
  CommandLineFromAnotherInstance(&Window, Args, Scratch, QString());
  CHECK(WaitUntil([&Window]()
  {
    for (Terminal * T : Window.findChildren<Terminal *>(Qt::FindDirectChildrenOnly))
    {
      if (T->Active()) return true;
    }
    return false;
  }, 30000));
  // /refresh: the remote panel in the directory gets reloaded
  RefreshPanel(&Window, QString(), Dir);
  RefreshPanel(&Window, Url, QString());
  WaitUntil([]() { return false; }, 500);
  // ExploreFile focuses the file once listed
  ExploreFile(&Window, Dir + QStringLiteral("/down load.txt"));
  bool Focused = WaitUntil([&Window]()
  {
    for (FilePanel * Panel : Window.findChildren<FilePanel *>())
    {
      if (Panel->IsRemote() && Panel->isVisible() && Panel->HasFocusedFile() && (Panel->FocusedFile().FileName == QStringLiteral("down load.txt")))
      {
        return true;
      }
    }
    return false;
  }, 10000);
  CHECK(Focused);
  // busy (modal dialog): refused
  QDialog Modal(&Window);
  QTimer::singleShot(0, &Modal, [&]() { CHECK(!CanCommandLineFromAnotherInstance(&Window)); Modal.reject(); });
  Modal.exec();
  QString Shot = qEnvironmentVariable("NIXSCP_SCREENSHOT_DIR");
  if (!Shot.isEmpty())
  {
    Window.grab().save(Shot + QStringLiteral("/guicommandline-instance.png"));
  }
  Window.setProperty("HandlesLastSessionClosed", true);
}

int main(int argc, char * argv[])
{
  QTemporaryDir Scratch;
  qputenv("XDG_CONFIG_HOME", Scratch.filePath(QStringLiteral("config")).toUtf8());
  qputenv("XDG_DATA_HOME", Scratch.filePath(QStringLiteral("data")).toUtf8());
  qputenv("XDG_RUNTIME_DIR", Scratch.filePath(QStringLiteral("run")).toUtf8());
  QDir().mkpath(Scratch.filePath(QStringLiteral("run")));
  QFile::setPermissions(Scratch.filePath(QStringLiteral("run")), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
  // private temp dir: leftover nixscp-dd-* folders of other runs in the shared
  // /tmp would trigger the "delete past temporary folders" confirmation
  QDir().mkpath(Scratch.filePath(QStringLiteral("tmp")));
  qputenv("TMPDIR", Scratch.filePath(QStringLiteral("tmp")).toUtf8());
  QApplication App(argc, argv);
  Configuration::Instance().SetIniFileName(Scratch.filePath(QStringLiteral("nixscp.ini")));
  Configuration::Instance().Load();
  Configuration::Instance().ConfirmClosingSession = false;

  std::printf("- %s\n", "SetupSites"); std::fflush(stdout);
  if (qEnvironmentVariableIsSet("GCL_DEBUG"))
  {
    auto * Dbg = new QTimer(qApp);
    QObject::connect(Dbg, &QTimer::timeout, []()
    {
      QWidget * M = QApplication::activeModalWidget();
      std::printf("modal: %s %s\n", M ? M->metaObject()->className() : "-", M ? qPrintable(M->windowTitle()) : "");
      if (M) for (QLabel * L : M->findChildren<QLabel *>()) std::printf("  label: %s\n", qPrintable(L->text()));
      std::fflush(stdout);
    });
    Dbg->start(1000);
  }
  SetupSites();
  std::printf("- %s\n", "TestParseUrl"); std::fflush(stdout);
  TestParseUrl();
  std::printf("- %s\n", "TestFolderAndLoginData"); std::fflush(stdout);
  TestFolderAndLoginData();
  std::printf("- %s\n", "TestSwitches"); std::fflush(stdout);
  TestSwitches(Scratch.path());
  std::printf("- %s\n", "TestUrlHandler"); std::fflush(stdout);
  TestUrlHandler(Scratch.path());
  std::printf("- %s\n", "TestSingleInstance"); std::fflush(stdout);
  TestSingleInstance(Scratch.path());

  if (!qEnvironmentVariableIsEmpty("NIXSCP_TEST_SSH_PORT") && !qEnvironmentVariableIsEmpty("NIXSCP_TEST_SSH_KEY"))
  {
    std::printf("- %s\n", "TestLastSessionWatcher"); std::fflush(stdout);
    TestLastSessionWatcher(Scratch.path());
    std::printf("- %s\n", "TestStandaloneOperations"); std::fflush(stdout);
    TestStandaloneOperations(Scratch.path());
    std::printf("- %s\n", "TestSessionsFromAnotherInstance"); std::fflush(stdout);
    TestSessionsFromAnotherInstance(Scratch.path());
  }
  else
  {
    std::printf("test_guicommandline: session tests SKIPPED (set NIXSCP_TEST_SSH_PORT, NIXSCP_TEST_SSH_KEY)\n");
  }

  std::printf(Failures ? "test_guicommandline: %d FAILURES\n" : "test_guicommandline: OK\n", Failures);
  return Failures ? 1 : 0;
}

#endif
