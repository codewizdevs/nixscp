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
#include "core/IniStorage.h"
#include "core/Terminal.h"
#include "gui/AuthenticateDialog.h"
#include "core/Workspaces.h"
#include "gui/CommanderWindow.h"
#include "gui/FilePanel.h"
#include "gui/GuiUserInterface.h"
#include "gui/GuiCommandLine.h"
#include "gui/LoginDialog.h"
#include "gui/SingleInstance.h"

#include <QSettings>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QAction>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

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

  // Started by a link: its URL and nothing else, however the arguments got there
  CHECK(LinkArguments({QStringLiteral("sftp://host/")}).isEmpty());
  CHECK(LinkArguments({QStringLiteral("/ini=x"), QStringLiteral("-platform"), QStringLiteral("offscreen")}).isEmpty());
  const QStringList PlainLink{QStringLiteral("/unsafe"), QStringLiteral("sftp://host/")};
  CHECK_EQ(LinkArguments(PlainLink), PlainLink);
  CHECK_EQ(LinkArguments({QStringLiteral("-UNSAFE"), QStringLiteral("sftp://host/")}), PlainLink);
  // what a link with a quote adds on Windows
  CHECK_EQ(LinkArguments({QStringLiteral("/unsafe"), QStringLiteral("sftp://host/"), QStringLiteral("-platformpluginpath"),
    QStringLiteral("\\\\evil\\share"), QStringLiteral("-platform"), QStringLiteral("x")}), PlainLink);
  CHECK_EQ(LinkArguments({QStringLiteral("/unsafe"), QStringLiteral("sftp://host/"), QStringLiteral("/script=\\\\evil\\s.txt")}), PlainLink);
  CHECK_EQ(LinkArguments({QStringLiteral("/unsafe"), QStringLiteral("sftp://host/"), QStringLiteral("/upload"), QStringLiteral("C:\\secret")}), PlainLink);
  CHECK_EQ(LinkArguments({QStringLiteral("/unsafe"), QStringLiteral("sftp://host/"), QStringLiteral("/hostkey=*"), QStringLiteral("/ini=x")}), PlainLink);
  CHECK_EQ(LinkArguments({QStringLiteral("/unsafe"), QStringLiteral("sftp://host/"), QStringLiteral("other://host/")}), PlainLink);
  // a switch for Qt is not a link
  CHECK_EQ(LinkArguments({QStringLiteral("/unsafe"), QStringLiteral("--"), QStringLiteral("-platformpluginpath")}), QStringList{QStringLiteral("/unsafe")});
  CHECK_EQ(LinkArguments({QStringLiteral("/unsafe")}), QStringList{QStringLiteral("/unsafe")});

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
  // no switch applies to an unsafe command line at all, harmless or not
  CHECK(!P.Data.Compression);
  Options Raw2 = MakeOptions({QStringLiteral("sftp://host/"), QStringLiteral("/rawsettings"), QStringLiteral("SftpServer=/bin/sftp")});
  P = ParseUrl(QStringLiteral("sftp://host/"), &Raw2, Stored, false);
  CHECK_EQ(P.Data.SftpServer, QStringLiteral("/bin/sftp"));

  // A link sets only what is on the list: whatever it holds about trust, the
  // target, commands or local files is gone, known setting or not
  P = ParseUrl(QStringLiteral("sftp://user;x-SshHostKey=*;x-TunnelHostKey=*;x-AgentAccess=1;x-AgentPermissions=7;x-AgentName=evil;x-AgentAllow=*;"
    "x-AgentDeny=x;x-AgentRecord=1;x-PortForwards=L1%20h%202;x-AgentFwd=1;x-GSSAPIFwdTGT=1;x-HostName=evil.example;x-PortNumber=2222;x-Ftps=0;"
    "x-PublicKeyFile=%2Fhome%2Fu%2Fkey;x-LocalDirectory=%2Fetc;x-RemoteDirectory=%7E%3Bid;x-Tunnel=1;x-TunnelHostName=evil.example;"
    "x-ProxyMethod=cmd;x-ProxyTelnetCommand=id;x-ProxyHost=evil.example;x-Compression=1;x-S3CredentialsEnv=1;x-S3Profile=default;"
    "x-Passphrase=p;x-WebDavUnencryptedRedirects=1;x-Cipher=des,WARN;x-Timeout=33@host/"), nullptr, Stored, true);
  {
    SessionData Factory = Stored.DefaultSettings();
    const SessionData & D = P.Data;
    CHECK(D.HostKey.isEmpty() && D.TunnelHostKey.isEmpty() && !D.HostKeyFromUrl && !D.TunnelHostKeyFromUrl);
    CHECK((D.AgentAccess == Factory.AgentAccess) && (D.AgentPermissions == Factory.AgentPermissions) && D.AgentName.isEmpty());
    CHECK((D.AgentAllow == Factory.AgentAllow) && (D.AgentDeny == Factory.AgentDeny) && (D.AgentRecord == Factory.AgentRecord));
    CHECK(D.PortForwards.isEmpty() && !D.AgentFwd && !D.GSSAPIFwdTGT);
    CHECK_EQ(D.HostName, QStringLiteral("host"));
    CHECK(D.PortNumber == SshPortNumber);
    CHECK(D.PublicKeyFile.isEmpty() && D.LocalDirectory.isEmpty() && D.RemoteDirectory.isEmpty());
    CHECK(!D.Tunnel && D.TunnelHostName.isEmpty());
    CHECK((D.ProxyMethod == Factory.ProxyMethod) && (D.ProxyHost == Factory.ProxyHost) && (D.ProxyTelnetCommand == Factory.ProxyTelnetCommand) &&
      (D.ProxyLocalCommand == Factory.ProxyLocalCommand));
    CHECK(!D.S3CredentialsEnv && D.S3Profile.isEmpty());
    CHECK(D.CipherList == Factory.CipherList);
    CHECK(P.Passphrase.isEmpty());
    CHECK(D.Compression);
    CHECK(D.Timeout == 33);
    // nothing of it is kept for saving either
    IniFile File;
    IniStorage Storage(&File);
    Storage.OpenSubKey(QStringLiteral("Site"), true);
    D.Save(Storage);
    SessionData Back;
    Back.Load(Storage);
    CHECK((Back.AgentAccess == Factory.AgentAccess) && Back.HostKey.isEmpty() && Back.TunnelHostKey.isEmpty());
    CHECK(!File.Keys(QStringLiteral("Site")).contains(QStringLiteral("WebDavUnencryptedRedirects"), Qt::CaseInsensitive));
  }
  // the scheme of a link stays what it says
  P = ParseUrl(QStringLiteral("ftpes://user;x-Ftps=0@host/"), nullptr, Stored, true);
  CHECK(P.Data.Ftps == ftpsExplicitTls);
  // ";fingerprint=" of a clicked link is its author's: the user is still asked.
  // Elsewhere it goes on as a URL's: an unknown key is compared with it, a known one is not
  P = ParseUrl(QStringLiteral("sftp://user;fingerprint=abc@host/"), nullptr, Stored, true);
  CHECK(P.Data.HostKey.isEmpty());
  CHECK(!P.Data.HostKeyFromUrl);
  P = ParseUrl(QStringLiteral("sftp://user;fingerprint=abc@host/"), nullptr, Stored, false);
  CHECK_EQ(P.Data.HostKey, QStringLiteral("abc"));
  CHECK(P.Data.HostKeyFromUrl);
  for (const char * Url : {"sftp://user;fingerprint=*@host/", "sftp://user;fingerprint=acceptnew@host/", "ftpes://user;fingerprint=*@host/", "davs://user;fingerprint=*@host/"})
  {
    for (bool AUnsafe : {false, true})
    {
      P = ParseUrl(QString::fromLatin1(Url), nullptr, Stored, AUnsafe);
      CHECK(P.Data.HostKey.isEmpty());
    }
  }
  // a command line of one's own has more to say, not about keys and agents though
  P = ParseUrl(QStringLiteral("sftp://user;x-SshHostKey=abc;x-TunnelHostKey=*;x-AgentAccess=1;x-PortForwards=L1%20h%202;x-Tunnel=1;x-TunnelHostName=gw@host/"),
    nullptr, Stored, false);
  CHECK_EQ(P.Data.HostKey, QStringLiteral("abc"));
  CHECK(P.Data.HostKeyFromUrl);
  CHECK(P.Data.TunnelHostKey.isEmpty());
  CHECK(P.Data.AgentAccess == Stored.DefaultSettings().AgentAccess);
  CHECK(P.Data.PortForwards.isEmpty());
  CHECK_EQ(P.Data.TunnelHostName, QStringLiteral("gw"));
  // S14: from another process (Unsafe) a stored site keeps its host and security
  Options Redirect = MakeOptions({QStringLiteral("myhost.example.com"), QStringLiteral("/rawsettings"),
    QStringLiteral("HostName=evil.example"), QStringLiteral("PortNumber=21"), QStringLiteral("/hostkey=*")});
  P = ParseUrl(QStringLiteral("myhost.example.com"), &Redirect, Stored, true);
  CHECK_EQ(P.Data.HostName, QStringLiteral("myhost.example.com"));
  CHECK_EQ(P.Data.PortNumber, 2200);
  CHECK(P.Data.HostKey.isEmpty());
  // nor does a link take a stored site to another protocol
  SessionData SftpSite;
  SftpSite.HostName = QStringLiteral("protocol.example");
  SftpSite.FSProtocol = fsSFTP;
  Stored.NewSession(QStringLiteral("protocol.example"), SftpSite);
  P = ParseUrl(QStringLiteral("ftp://protocol.example/"), nullptr, Stored, true);
  CHECK(P.Data.FSProtocol == fsSFTP);
  P = ParseUrl(QStringLiteral("ftp://protocol.example/"), nullptr, Stored, false);
  CHECK(P.Data.FSProtocol == fsFTP);
  Stored.Remove(QStringLiteral("protocol.example"));
  // the command line stays with a new instance rather than losing its switches
  CHECK(CanForwardCommandLine(MakeOptions({QStringLiteral("/unsafe"), QStringLiteral("sftp://host/")})));
  CHECK(CanForwardCommandLine(MakeOptions({QStringLiteral("mysite"), QStringLiteral("/browse=x")})));
  CHECK(!CanForwardCommandLine(MakeOptions({QStringLiteral("mysite"), QStringLiteral("/rawsettings"), QStringLiteral("HostName=x")})));
  CHECK(!CanForwardCommandLine(MakeOptions({QStringLiteral("mysite"), QStringLiteral("/hostkey=*")})));
  CHECK(!CanForwardCommandLine(MakeOptions({QStringLiteral("mysite"), QStringLiteral("/explicit=0")})));

  // S3: nothing of a URL is expanded, in any of its forms
  qputenv("NIXSCP_TEST_SECRET", "wJalrFAKEsecret");
  for (bool AUnsafe : {false, true})
  {
    P = ParseUrl(QStringLiteral("ftp://$NIXSCP_TEST_SECRET@evil.example/"), nullptr, Stored, AUnsafe);
    CHECK_EQ(PrepareSession(P.Data).UserName, QStringLiteral("$NIXSCP_TEST_SECRET"));
    P = ParseUrl(QStringLiteral("ftp://$%7BNIXSCP_TEST_SECRET%7D@evil.example/"), nullptr, Stored, AUnsafe);
    CHECK_EQ(PrepareSession(P.Data).UserName, QStringLiteral("${NIXSCP_TEST_SECRET}"));
    P = ParseUrl(QStringLiteral("ftp://%25NIXSCP_TEST_SECRET%25@evil.example/"), nullptr, Stored, AUnsafe);
    CHECK_EQ(PrepareSession(P.Data).UserName, QStringLiteral("%NIXSCP_TEST_SECRET%"));
    P = ParseUrl(QStringLiteral("ftp://u@$NIXSCP_TEST_SECRET.evil.example/"), nullptr, Stored, AUnsafe);
    CHECK_EQ(PrepareSession(P.Data).HostName, QStringLiteral("$NIXSCP_TEST_SECRET.evil.example"));
    // (a link does not get to choose a tunnel at all)
    P = ParseUrl(QStringLiteral("sftp://u;x-TunnelHostName=$NIXSCP_TEST_SECRET.evil.example;x-TunnelUserName=$NIXSCP_TEST_SECRET@h/"), nullptr, Stored, AUnsafe);
    CHECK_EQ(PrepareSession(P.Data).TunnelHostName, AUnsafe ? QString() : QStringLiteral("$NIXSCP_TEST_SECRET.evil.example"));
    CHECK_EQ(PrepareSession(P.Data).TunnelUserName, AUnsafe ? QString() : QStringLiteral("$NIXSCP_TEST_SECRET"));
  }
  // the switches of the local command line and stored sites are expanded as ever
  Options UserSwitch = MakeOptions({QStringLiteral("sftp://host/"), QStringLiteral("/username=$NIXSCP_TEST_SECRET")});
  P = ParseUrl(QStringLiteral("sftp://host/"), &UserSwitch, Stored, false);
  CHECK_EQ(PrepareSession(P.Data).UserName, QStringLiteral("wJalrFAKEsecret"));
  SessionData Expanding;
  Expanding.HostName = QStringLiteral("expanding.example");
  Expanding.UserName = QStringLiteral("$NIXSCP_TEST_SECRET");
  Stored.NewSession(QStringLiteral("Expanding"), Expanding);
  P = ParseUrl(QStringLiteral("Expanding"), nullptr, Stored, true);
  CHECK_EQ(PrepareSession(P.Data).UserName, QStringLiteral("wJalrFAKEsecret"));
  // saved as a site, a URL stays literal
  P = ParseUrl(QStringLiteral("ftp://$NIXSCP_TEST_SECRET@evil.example/"), nullptr, Stored, true);
  std::shared_ptr<SessionData> FromUrl = Stored.NewSession(QStringLiteral("FromUrl"), P.Data);
  IniFile Ini;
  Stored.Save(Ini);
  StoredSessionList Reloaded;
  Reloaded.Load(Ini);
  CHECK_EQ(PrepareSession(*Reloaded.FindByName(QStringLiteral("FromUrl"))).UserName, QStringLiteral("$NIXSCP_TEST_SECRET"));
  Stored.Remove(QStringLiteral("FromUrl"));
  Stored.Remove(QStringLiteral("Expanding"));
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
#ifdef Q_OS_MACOS
  // not available on macOS yet: refused, nothing written
  Q_UNUSED(Scratch);
  QString MacError;
  CHECK(!RegisterForDefaultProtocols(&MacError) && MacError.contains(QStringLiteral("macOS")));
  CHECK(!UnregisterForProtocols(&MacError));
  CHECK(!QFileInfo::exists(UrlHandlerDesktopFileName()));
  return;
#endif
#ifdef _WIN32
  // HKCU\\Software\\Classes\\<scheme>, as WinSCP
  Q_UNUSED(Scratch);
  QSettings Classes(QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes"), QSettings::NativeFormat);
  QString Error;
  CHECK(RegisterForDefaultProtocols(&Error));
  for (const QString & Scheme : UrlSchemes())
  {
    CHECK(Classes.contains(Scheme + QStringLiteral("/URL Protocol")));
    QString Command = Classes.value(Scheme + QStringLiteral("/shell/open/command/Default")).toString();
    CHECK(Command.contains(QStringLiteral("\" /unsafe \"%1\"")));
  }
  CHECK(UnregisterForProtocols(&Error));
  CHECK(!Classes.contains(QStringLiteral("sftp/shell/open/command/Default")));
#else
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
#endif
}

static void TestServerNameLength(const QString & Scratch)
{
#ifndef _WIN32
  // Qt binds "<name>XXXXXX/s" first: with a name that still fits sockaddr_un,
  // but not with that, the instances would silently run on their own
  QString Run = Scratch + QStringLiteral("/longrun");
  QByteArray Saved = qgetenv("XDG_RUNTIME_DIR");
  QString Name = SingleInstance::DefaultServerName(QStringLiteral("nul"));
  qsizetype Fixed = QFileInfo(Name).fileName().size() + 1;
  for (qsizetype Length = 90; Length <= 110; Length++)
  {
    QString Dir = Run + QLatin1Char('/');
    if (Dir.size() + 1 < Length - Fixed)
    {
      Dir += QString(Length - Fixed - Dir.size(), QLatin1Char('d'));
    }
    QDir().mkpath(Dir);
    QFile::setPermissions(Dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    qputenv("XDG_RUNTIME_DIR", QFile::encodeName(Dir));
    QString Server = SingleInstance::DefaultServerName(QStringLiteral("nul"));
    SingleInstance Instance(Server);
    CHECK(Instance.Listen());
  }
  qputenv("XDG_RUNTIME_DIR", Saved);
  QDir(Run).removeRecursively();
#else
  Q_UNUSED(Scratch);
#endif
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

// T16: the site of a node can go while the dialog is open (another instance, a
// team sync); the tree is not reloaded while a site is being edited
static void TestSiteRemovedInLoginDialog()
{
  auto NewSites = []()
  {
    auto Sites = std::make_shared<StoredSessionList>();
    auto Data = std::make_shared<SessionData>();
    Data->Name = QStringLiteral("Folder/Doomed");
    Data->HostName = QStringLiteral("doomed.example.com");
    Data->UserName = QStringLiteral("deploy");
    Sites->Add(Data);
    return Sites;
  };
  auto SiteNode = [](QTreeWidget * Tree)
  {
    QTreeWidgetItem * Result = nullptr;
    for (QTreeWidgetItemIterator It(Tree); *It; ++It)
    {
      if ((*It)->text(0) == QStringLiteral("Doomed")) Result = *It;
    }
    return Result;
  };
  auto Trigger = [](QDialog & Dialog, const QString & Text)
  {
    for (QAction * Action : Dialog.findChildren<QAction *>())
    {
      if (Action->text() == Text)
      {
        Action->trigger();
        return true;
      }
    }
    return false;
  };

  // a drag of the stale node
  {
    auto Sites = NewSites();
    LoginDialog Dialog(*Sites);
    Dialog.show();
    QApplication::processEvents();
    auto * Tree = Dialog.findChild<QTreeWidget *>();
    QTreeWidgetItem * Node = SiteNode(Tree);
    CHECK(Node != nullptr);
    if (Node == nullptr) return;
    Tree->expandAll();
    Tree->setCurrentItem(Node);
    QApplication::processEvents();
    Sites->Remove(QStringLiteral("Folder/Doomed"));
    QPoint Pos = Tree->visualItemRect(Node).center();
    QWidget * Viewport = Tree->viewport();
    QMouseEvent Press(QEvent::MouseButtonPress, Pos, Viewport->mapToGlobal(Pos), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(Viewport, &Press);
    QPoint Far = Pos + QPoint(QApplication::startDragDistance() * 4, 0);
    QMouseEvent Move(QEvent::MouseMove, Far, Viewport->mapToGlobal(Far), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(Viewport, &Move);
    QMouseEvent Release(QEvent::MouseButtonRelease, Far, Viewport->mapToGlobal(Far), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(Viewport, &Release);
    // the Desktop Icon command of the stale node
    Trigger(Dialog, QStringLiteral("Desktop &Icon"));
  }
  // saved while editing: under its name, not a default one
  {
    auto Sites = NewSites();
    LoginDialog Dialog(*Sites);
    Dialog.show();
    QApplication::processEvents();
    auto * Tree = Dialog.findChild<QTreeWidget *>();
    QTreeWidgetItem * Node = SiteNode(Tree);
    CHECK(Node != nullptr);
    if (Node == nullptr) return;
    Tree->setCurrentItem(Node);
    QApplication::processEvents();
    CHECK(Trigger(Dialog, QStringLiteral("&Edit")));
    QApplication::processEvents();
    Sites->Remove(QStringLiteral("Folder/Doomed"));
    CHECK(Trigger(Dialog, QStringLiteral("&Save")));
    QApplication::processEvents();
    CHECK(Sites->FindByName(QStringLiteral("Folder/Doomed")) != nullptr);
    CHECK_EQ(Sites->Sessions().size(), static_cast<qsizetype>(1));
  }
  // renamed: the workspace follows (L37)
  {
    auto Sites = NewSites();
    SessionData Tab = *Sites->FindByName(QStringLiteral("Folder/Doomed"));
    Workspaces::SaveWorkspace(*Sites, QStringLiteral("WS"), {Tab}, false);
    LoginDialog Dialog(*Sites);
    Dialog.show();
    QApplication::processEvents();
    auto * Tree = Dialog.findChild<QTreeWidget *>();
    QTreeWidgetItem * Node = SiteNode(Tree);
    CHECK(Node != nullptr);
    if (Node == nullptr) return;
    Tree->expandAll();
    Tree->setCurrentItem(Node);
    QApplication::processEvents();
    CHECK(Trigger(Dialog, QStringLiteral("&Rename")));
    QApplication::processEvents();
    auto * Editor = Tree->findChild<QLineEdit *>();
    CHECK(Editor != nullptr);
    if (Editor != nullptr)
    {
      Editor->setText(QStringLiteral("Survivor"));
      QKeyEvent Press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
      QApplication::sendEvent(Editor, &Press);
      CHECK(WaitUntil([&Sites]() { return Sites->FindByName(QStringLiteral("Folder/Survivor")) != nullptr; }, 3000));
      QList<SessionData> Tabs = Workspaces::LoadWorkspace(*Sites, QStringLiteral("WS"));
      CHECK_EQ(Tabs.size(), static_cast<qsizetype>(1));
      CHECK_EQ(Tabs.value(0).Name, QStringLiteral("Folder/Survivor"));
    }
  }
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
        // the error, not the connecting dialog (shown by the "*" warning, ANY_HOSTKEY)
        auto * Dialog = qobject_cast<QDialog *>(W);
        if ((Dialog != nullptr) && (qobject_cast<AuthenticateDialog *>(W) == nullptr))
        {
          // only the error: the connection has a modal dialog too on a busy machine
          QString Text;
          for (QLabel * Label : Dialog->findChildren<QLabel *>())
          {
            Text += Label->text();
          }
          if (Text.contains(QStringLiteral("does not exist")))
          {
            ErrorText = Text;
            Dialog->reject();
          }
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
  // S14: what comes from another process is a stored site or a plain URL. A
  // site is opened as it is stored, whatever the command line tries.
  StoredSessionList & Stored = Configuration::Instance().StoredSessions();
  SessionData Site = Data;
  Site.HostKey = QStringLiteral("*");
  Site.TryAgent = false;
  Site.AuthKI = false;
  Site.RemoteDirectory = Dir;
  Stored.NewSession(QStringLiteral("instance site"), Site);
  QStringList Args{QStringLiteral("instance site"), QStringLiteral("/rawsettings"), QStringLiteral("HostName=127.0.0.2"),
    QStringLiteral("PortNumber=1"), QStringLiteral("/browse=") + Dir + QStringLiteral("/down load.txt")};
  CommandLineFromAnotherInstance(&Window, Args, Scratch, QString());
  CHECK(WaitUntil([&Window, &Site]()
  {
    for (Terminal * T : Window.findChildren<Terminal *>(Qt::FindDirectChildrenOnly))
    {
      if (T->Active()) return (T->Data().HostName == Site.HostName) && (T->Data().PortNumber == Site.PortNumber);
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
  TestServerNameLength(Scratch.path());
  TestSiteRemovedInLoginDialog();

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
