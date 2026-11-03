// NixSCP Agent Access end to end (core/AgentAccess, gui/AgentBroker,
// gui/AgentClient): the broker runs in this process, with a master password,
// a NixSCP Pro account and stored sites; the real "nixscp" client runs as a
// separate process, as an AI agent would run it. Covers listing, commands
// (stdout, stderr, exit codes, stdin, a large output), file upload and
// download, approval prompts, and every refusal.
//
// Skipped unless these are set:
//   NIXSCP_TEST_CLOUD_COOKIE       website session cookie of a test account with an active license (or beta)
//   NIXSCP_TEST_AGENT_SSH_PORT     SSH server on 127.0.0.1 with password authentication
//   NIXSCP_TEST_AGENT_SSH_USER, NIXSCP_TEST_AGENT_SSH_PASSWORD
//   NIXSCP_TEST_AGENT_KNOWN_HOSTS  known_hosts file with the server's host key
//   NIXSCP_AGENT_CLIENT            the nixscp executable
// NIXSCP_GUI_TESTS
#include "core/AgentAccess.h"
#include "core/CloudSync.h"
#include "core/ConfigVault.h"
#include "core/Configuration.h"
#include "core/SessionData.h"
#include "gui/AgentBroker.h"

#include <QAbstractButton>
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QTcpSocket>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrlQuery>

#include <cstdio>
#include <functional>
#include <memory>

static int Failures = 0;

#define CHECK(Cond) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); std::fflush(stdout); Failures++; } } while (0)
#define CHECK_MSG(Cond, Msg) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n  %s\n", __FILE__, __LINE__, #Cond, qPrintable(Msg)); std::fflush(stdout); Failures++; } } while (0)

static bool WaitFor(const std::function<bool()> & Condition, int TimeoutMs = 60000)
{
  QElapsedTimer Timer;
  Timer.start();
  while (!Condition())
  {
    if (Timer.elapsed() > TimeoutMs)
    {
      return false;
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  }
  return true;
}

static bool SignIn()
{
  CloudSync & Sync = CloudSync::Instance();
  CloudSync::SignInRequest Request = Sync.NewSignInRequest(53683);
  QUrlQuery Query(QUrl(Request.Url));
  QJsonObject Body
  {
    {QStringLiteral("client_id"), QStringLiteral("nixscp-desktop")},
    {QStringLiteral("redirect_uri"), Request.RedirectUri},
    {QStringLiteral("code_challenge"), Query.queryItemValue(QStringLiteral("code_challenge"))},
    {QStringLiteral("code_challenge_method"), QStringLiteral("S256")},
    {QStringLiteral("state"), Request.State},
    {QStringLiteral("device_name"), QStringLiteral("agent access test")},
  };
  QNetworkAccessManager Manager;
  QNetworkRequest Http(QUrl(Sync.Client().BaseUrl() + QStringLiteral("/v1/oauth/authorize")));
  Http.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
  Http.setRawHeader("Cookie", "nx_session=" + qgetenv("NIXSCP_TEST_CLOUD_COOKIE"));
  QNetworkReply * Reply = Manager.post(Http, QJsonDocument(Body).toJson());
  WaitFor([Reply]() { return Reply->isFinished(); });
  QUrl Redirect(QJsonDocument::fromJson(Reply->readAll()).object().value(QStringLiteral("redirect_url")).toString());
  delete Reply;
  QString Code = QUrlQuery(Redirect).queryItemValue(QStringLiteral("code"));
  if (Code.isEmpty())
  {
    return false;
  }
  auto Done = std::make_shared<int>(0);
  Sync.CompleteSignIn(Request, Code, [Done](bool Ok, const QString &) { *Done = Ok ? 1 : 2; });
  WaitFor([Done]() { return *Done != 0; });
  return *Done == 1;
}

struct Result
{
  int Code = -1;
  QByteArray Out;
  QByteArray Err;
};

// The client in its own process; the broker in this process keeps running
static Result Agent(const QStringList & Arguments, const QByteArray & Input = QByteArray())
{
  QProcess P;
  P.start(qEnvironmentVariable("NIXSCP_AGENT_CLIENT"), Arguments);
  if (!P.waitForStarted(10000))
  {
    return Result();
  }
  P.write(Input);
  P.closeWriteChannel();
  Result R;
  WaitFor([&P, &R]()
  {
    R.Out += P.readAllStandardOutput();
    R.Err += P.readAllStandardError();
    return P.state() == QProcess::NotRunning;
  }, 120000);
  R.Out += P.readAllStandardOutput();
  R.Err += P.readAllStandardError();
  R.Code = (P.exitStatus() == QProcess::NormalExit) ? P.exitCode() : -2;
  return R;
}

static void AddSite(const QString & Name, int Access, int Permissions, const QString & KnownHosts,
  const QString & AgentName = QString())
{
  SessionData Site;
  Site.Name = Name;
  Site.HostName = QStringLiteral("127.0.0.1");
  Site.PortNumber = qEnvironmentVariableIntValue("NIXSCP_TEST_AGENT_SSH_PORT");
  Site.UserName = qEnvironmentVariable("NIXSCP_TEST_AGENT_SSH_USER");
  Site.Password = qEnvironmentVariable("NIXSCP_TEST_AGENT_SSH_PASSWORD");
  Site.FSProtocol = fsSFTP;
  Site.KnownHostsFile = KnownHosts;
  Site.AgentAccess = Access;
  Site.AgentPermissions = Permissions;
  Site.AgentName = AgentName;
  Configuration::Instance().StoredSessions().Add(std::make_shared<SessionData>(Site));
}

// Answers the next approval prompt with the button captioned Caption
static void AnswerApproval(const QString & Caption)
{
  auto * Timer = new QTimer();
  QObject::connect(Timer, &QTimer::timeout, Timer, [Timer, Caption]()
  {
    for (QWidget * W : QApplication::topLevelWidgets())
    {
      auto * Box = qobject_cast<QMessageBox *>(W);
      if ((Box != nullptr) && Box->isVisible() && (Box->windowTitle() == QStringLiteral("NixSCP Agent Access")))
      {
        for (QAbstractButton * B : Box->buttons())
        {
          if (B->text() == Caption)
          {
            B->click();
            Timer->deleteLater();
            return;
          }
        }
      }
    }
  });
  Timer->start(50);
}

int main(int argc, char ** argv)
{
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
  {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  const char * Required[] = {"NIXSCP_TEST_CLOUD_COOKIE", "NIXSCP_TEST_AGENT_SSH_PORT", "NIXSCP_TEST_AGENT_SSH_USER",
    "NIXSCP_TEST_AGENT_SSH_PASSWORD", "NIXSCP_TEST_AGENT_KNOWN_HOSTS", "NIXSCP_AGENT_CLIENT"};
  for (const char * Name : Required)
  {
    if (qEnvironmentVariableIsEmpty(Name))
    {
      std::printf("SKIP: set %s (see the comment at the top of the test)\n", Name);
      return 0;
    }
  }
  QTemporaryDir Dir;
  // the broker's socket and the client's lookup use this folder
  qputenv("XDG_RUNTIME_DIR", Dir.path().toUtf8());
  QApplication App(argc, argv);
  Configuration & Config = Configuration::Instance();
  Config.SetIniFileName(Dir.path() + QStringLiteral("/nixscp.ini"));
  Config.Load();
  CloudSync::Instance().Initialize(Config);

  QString KnownHosts = qEnvironmentVariable("NIXSCP_TEST_AGENT_KNOWN_HOSTS");
  QString NoKnownHosts = Dir.path() + QStringLiteral("/empty_known_hosts");
  {
    QFile Empty(NoKnownHosts);
    CHECK(Empty.open(QIODevice::WriteOnly));
  }
  AddSite(QStringLiteral("agent-allow"), AgentAccess::amAllow, AgentAccess::apAll, KnownHosts);
  AddSite(QStringLiteral("agent-off"), AgentAccess::amOff, AgentAccess::apAll, KnownHosts);
  AddSite(QStringLiteral("agent-commands-only"), AgentAccess::amAllow, AgentAccess::apCommands, KnownHosts);
  AddSite(QStringLiteral("agent-ask"), AgentAccess::amAsk, AgentAccess::apAll, KnownHosts);
  AddSite(QStringLiteral("agent-new-host"), AgentAccess::amAllow, AgentAccess::apAll, NoKnownHosts);
  // command rules, downloads only, not recorded
  AddSite(QStringLiteral("agent-rules"), AgentAccess::amAllow, AgentAccess::apAll | AgentAccess::apDownloadOnly, KnownHosts);
  {
    auto Rules = Config.StoredSessions().FindByName(QStringLiteral("agent-rules"));
    Rules->AgentAllow = QStringLiteral("echo *\nuptime");
    Rules->AgentRecord = false;
    // a saved tunnel to the server's own SSH port (2222 inside the container)
    Config.StoredSessions().FindByName(QStringLiteral("agent-allow"))->PortForwards = QStringLiteral("ssh|22223|localhost|2222");
  }
  // agents know this one only by its agent name
  AddSite(QStringLiteral("Clients/secret-client"), AgentAccess::amAllow, AgentAccess::apAll, KnownHosts, QStringLiteral("web-alias"));
  CHECK(Config.Save());
  AgentBroker::Start(nullptr);

  // ---- saved passwords only obfuscated (no master password, no NixSCP Pro): refused
  Result R = Agent({QStringLiteral("--list")});
  CHECK_MSG((R.Code == 255) && R.Err.contains("master password"), QString::fromUtf8(R.Err));

  // ---- free: a master password is enough, no NixSCP Pro account needed
  CHECK(Config.ChangeMasterPassword(QStringLiteral("legacy master password 1")));
  CHECK(Config.UseMasterPassword());
  CHECK(!CloudSync::Instance().SignedIn());
  for (const auto & Site : Config.StoredSessions().Sessions())
  {
    Site->KnownHostsFile = (Site->Name == QStringLiteral("agent-new-host")) ? NoKnownHosts : KnownHosts;
  }
  R = Agent({QStringLiteral("--list")});
  CHECK_MSG((R.Code == 0) && R.Out.contains("agent-allow"), QString::fromUtf8(R.Err));
  R = Agent({QStringLiteral("--connect"), QStringLiteral("agent-allow"), QStringLiteral("--cmd"), QStringLiteral("echo free")});
  CHECK_MSG((R.Code == 0) && (R.Out == "free\n"), QString::fromUtf8(R.Err));

  // ---- NixSCP Pro: the NixSCP password replaces the master password
  CHECK(SignIn());
  // the server's host key (KnownHostsFile is not stored, so set it on the loaded sites)
  for (const auto & Site : Config.StoredSessions().Sessions())
  {
    Site->KnownHostsFile = (Site->Name == QStringLiteral("agent-new-host")) ? NoKnownHosts : KnownHosts;
  }

  // ---- the NixSCP password (sync password) encrypts the configuration
  auto KeyDone = std::make_shared<int>(0);
  CloudSync::Instance().CreateKey(QStringLiteral("agent test NixSCP password"), true,
    [KeyDone](bool Ok, const QString &) { *KeyDone = Ok ? 1 : 2; });
  WaitFor([KeyDone]() { return *KeyDone != 0; });
  CHECK(*KeyDone == 1);
  CHECK(ConfigVault::Enabled());
  CHECK(ConfigVault::IsSealedFile(Config.IniFileName()));
  {
    QFile Raw(Config.IniFileName());
    CHECK(Raw.open(QIODevice::ReadOnly) && !Raw.readAll().contains("agent-allow"));
  }
  // the master password is replaced by the NixSCP password; site passwords survive a restart
  CHECK(!Config.UseMasterPassword());
  {
    QString Password = qEnvironmentVariable("NIXSCP_TEST_AGENT_SSH_PASSWORD");
    ConfigVault::Forget();
    CHECK(ConfigVault::Unlock(Config.IniFileName(), QStringLiteral("agent test NixSCP password"), false));
    Config.Load();
    CHECK(!Config.IsLocked() && !Config.UseMasterPassword());
    auto Site = Config.StoredSessions().FindByName(QStringLiteral("agent-allow"));
    CHECK((Site != nullptr) && (Site->Password == Password));
    for (const auto & S : Config.StoredSessions().Sessions())
    {
      S->KnownHostsFile = (S->Name == QStringLiteral("agent-new-host")) ? NoKnownHosts : KnownHosts;
    }
  }

  // ---- -help needs nothing
  R = Agent({QStringLiteral("--help")});
  CHECK((R.Code == 0) && R.Out.contains("nixscp --connect <site> --cmd \"<command>\""));

  // ---- list: only the sites agents may use
  R = Agent({QStringLiteral("--list")});
  CHECK_MSG(R.Code == 0, QString::fromUtf8(R.Err));
  CHECK(R.Out.contains("agent-allow") && R.Out.contains("agent-commands-only") && R.Out.contains("agent-ask"));
  CHECK(!R.Out.contains("agent-off"));
  CHECK(R.Out.contains("web-alias") && !R.Out.contains("secret-client"));
  R = Agent({QStringLiteral("--list"), QStringLiteral("--json")});
  CHECK(QJsonDocument::fromJson(R.Out).isArray());

  // ---- commands: output, error output and the exit code
  R = Agent({QStringLiteral("--connect"), QStringLiteral("agent-allow"), QStringLiteral("--cmd"), QStringLiteral("echo hello; echo oops >&2; exit 7")});
  CHECK_MSG((R.Code == 7) && (R.Out == "hello\n") && (R.Err == "oops\n"), QStringLiteral("%1 [%2] [%3]").arg(R.Code).arg(QString::fromUtf8(R.Out), QString::fromUtf8(R.Err)));
  // standard input reaches the command
  R = Agent({QStringLiteral("--connect"), QStringLiteral("agent-allow"), QStringLiteral("--cmd"), QStringLiteral("tr a-z A-Z")}, "piped input\n");
  CHECK_MSG((R.Code == 0) && (R.Out == "PIPED INPUT\n"), QString::fromUtf8(R.Out + R.Err));
  // a large output arrives completely (flow control)
  R = Agent({QStringLiteral("--connect"), QStringLiteral("agent-allow"), QStringLiteral("--cmd"), QStringLiteral("head -c 20000000 /dev/zero")});
  CHECK_MSG((R.Code == 0) && (R.Out.size() == 20000000), QStringLiteral("%1 bytes, %2").arg(R.Out.size()).arg(QString::fromUtf8(R.Err)));
  // the stored password never shows up
  CHECK(!R.Err.contains(qgetenv("NIXSCP_TEST_AGENT_SSH_PASSWORD")));

  // ---- option forms: --cmd=..., words after --cmd, and mistakes
  R = Agent({QStringLiteral("--connect"), QStringLiteral("agent-allow"), QStringLiteral("--cmd=echo one two")});
  CHECK_MSG((R.Code == 0) && (R.Out == "one two\n"), QString::fromUtf8(R.Err));
  R = Agent({QStringLiteral("--connect"), QStringLiteral("agent-allow"), QStringLiteral("--cmd"), QStringLiteral("echo"), QStringLiteral("three")});
  CHECK_MSG((R.Code == 0) && (R.Out == "three\n"), QString::fromUtf8(R.Err));
  R = Agent({QStringLiteral("--connect"), QStringLiteral("agent-allow"), QStringLiteral("uptime")});
  CHECK((R.Code == 255) && R.Err.contains("--cmd"));
  R = Agent({QStringLiteral("--list"), QStringLiteral("--verbose")});
  CHECK((R.Code == 255) && R.Err.contains("Unknown option"));
  R = Agent({QStringLiteral("--connect"), QStringLiteral("agent-allow"), QStringLiteral("--cmd")});
  CHECK((R.Code == 255) && R.Err.contains("Missing command"));

  // ---- files: upload and download back
  QByteArray Content(3 * 1024 * 1024 + 123, Qt::Uninitialized);
  for (char & C : Content)
  {
    C = static_cast<char>(QRandomGenerator::global()->bounded(256));
  }
  QString Local = Dir.path() + QStringLiteral("/upload.bin");
  {
    QFile F(Local);
    CHECK(F.open(QIODevice::WriteOnly) && (F.write(Content) == Content.size()));
  }
  R = Agent({QStringLiteral("--put"), QStringLiteral("agent-allow"), Local, QStringLiteral("/tmp/nixscp-agent-test.bin")});
  CHECK_MSG(R.Code == 0, QString::fromUtf8(R.Err));
  QString Back = Dir.path() + QStringLiteral("/download.bin");
  R = Agent({QStringLiteral("--get"), QStringLiteral("agent-allow"), QStringLiteral("/tmp/nixscp-agent-test.bin"), Back});
  CHECK_MSG(R.Code == 0, QString::fromUtf8(R.Err));
  {
    QFile F(Back);
    CHECK(F.open(QIODevice::ReadOnly) && (F.readAll() == Content));
  }
  R = Agent({QStringLiteral("--connect"), QStringLiteral("agent-allow"), QStringLiteral("--cmd"), QStringLiteral("rm /tmp/nixscp-agent-test.bin")});
  CHECK(R.Code == 0);
  // a missing remote file fails cleanly and leaves no local file behind
  QString Missing = Dir.path() + QStringLiteral("/missing.bin");
  R = Agent({QStringLiteral("--get"), QStringLiteral("agent-allow"), QStringLiteral("/nonexistent/file"), Missing});
  CHECK((R.Code == 255) && !QFile::exists(Missing));

  // ---- agent names: the alias works, the real site name does not
  R = Agent({QStringLiteral("--connect"), QStringLiteral("web-alias"), QStringLiteral("--cmd"), QStringLiteral("echo via-alias")});
  CHECK_MSG((R.Code == 0) && (R.Out == "via-alias\n"), QString::fromUtf8(R.Err));
  R = Agent({QStringLiteral("--connect"), QStringLiteral("Clients/secret-client"), QStringLiteral("--cmd"), QStringLiteral("true")});
  CHECK_MSG((R.Code == 255) && R.Err.contains("not available to agents"), QString::fromUtf8(R.Err));

  // ---- command rules and downloads only
  R = Agent({QStringLiteral("--connect"), QStringLiteral("agent-rules"), QStringLiteral("--cmd"), QStringLiteral("echo ruled")});
  CHECK_MSG((R.Code == 0) && (R.Out == "ruled\n"), QString::fromUtf8(R.Err));
  R = Agent({QStringLiteral("--connect"), QStringLiteral("agent-rules"), QStringLiteral("--cmd"), QStringLiteral("echo a; id")});
  CHECK_MSG((R.Code == 255) && R.Err.contains("not among the commands allowed"), QString::fromUtf8(R.Err));
  R = Agent({QStringLiteral("--connect"), QStringLiteral("agent-rules"), QStringLiteral("--cmd"), QStringLiteral("echo $(id)")});
  CHECK((R.Code == 255) && R.Err.contains("substitution"));
  R = Agent({QStringLiteral("--put"), QStringLiteral("agent-rules"), Local, QStringLiteral("/tmp/x.bin")});
  CHECK_MSG((R.Code == 255) && R.Err.contains("downloads only"), QString::fromUtf8(R.Err));
  R = Agent({QStringLiteral("--get"), QStringLiteral("agent-rules"), QStringLiteral("/etc/hostname"), Dir.path() + QStringLiteral("/hostname.txt")});
  CHECK_MSG(R.Code == 0, QString::fromUtf8(R.Err));
  R = Agent({QStringLiteral("--list")});
  CHECK(R.Out.contains("downloads") && R.Out.contains("restricted commands"));

  // ---- recordings: transcripts of recorded sites only
  {
    QDir Recordings(AgentAccess::RecordingDirectory());
    QStringList Files = Recordings.entryList({QStringLiteral("*.log")}, QDir::Files);
    bool Hello = false;
    for (const QString & File : Files)
    {
      QByteArray Text = ConfigVault::ReadFile(Recordings.filePath(File));
      bool IsHello = Text.contains("Command: echo hello; echo oops >&2; exit 7") && Text.contains("hello\noops") &&
        Text.contains("[exit code 7]");
      Hello = Hello || IsHello;
      CHECK(!File.contains(QStringLiteral("agent-rules")));
      if (IsHello)
      {
        // made after the NixSCP password was set up: encrypted on disk like the configuration
        QFile Raw(Recordings.filePath(File));
        CHECK(Raw.open(QIODevice::ReadOnly) && ConfigVault::IsSealed(Raw.readAll()));
      }
    }
    CHECK(Hello);
  }

  // ---- MCP server
  {
    QProcess Mcp;
    Mcp.start(qEnvironmentVariable("NIXSCP_AGENT_CLIENT"), {QStringLiteral("--mcp")});
    CHECK(Mcp.waitForStarted(10000));
    Mcp.write("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},"
      "\"clientInfo\":{\"name\":\"test\",\"version\":\"1\"}}}\n"
      "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"
      "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"run_command\",\"arguments\":"
      "{\"site\":\"agent-allow\",\"command\":\"echo via-mcp; exit 3\"}}}\n"
      "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"list_sites\",\"arguments\":{}}}\n");
    QByteArray Out;
    WaitFor([&Mcp, &Out]() { Out += Mcp.readAllStandardOutput(); return Out.count('\n') >= 3; }, 60000);
    Mcp.closeWriteChannel();
    Mcp.waitForFinished(10000);
    QList<QByteArray> Lines = Out.trimmed().split('\n');
    CHECK(Lines.size() == 3);
    QJsonObject Init = QJsonDocument::fromJson(Lines.value(0)).object();
    CHECK(Init.value(QStringLiteral("result")).toObject().value(QStringLiteral("protocolVersion")).toString() == QStringLiteral("2025-06-18"));
    QJsonObject Call = QJsonDocument::fromJson(Lines.value(1)).object().value(QStringLiteral("result")).toObject();
    CHECK_MSG(!Call.value(QStringLiteral("isError")).toBool() &&
      (Call.value(QStringLiteral("structuredContent")).toObject().value(QStringLiteral("exit_code")).toInt() == 3) &&
      (Call.value(QStringLiteral("structuredContent")).toObject().value(QStringLiteral("stdout")).toString() == QStringLiteral("via-mcp\n")),
      QString::fromUtf8(Lines.value(1)));
    CHECK(Lines.value(2).contains("agent-allow"));
  }

  // ---- tunnels: a spec and a saved tunnel; the SSH banner comes through
  for (const QString & Forward : {QStringLiteral("22222:localhost:2222"), QStringLiteral("ssh")})
  {
    int LocalPort = (Forward == QStringLiteral("ssh")) ? 22223 : 22222;
    QProcess Tunnel;
    Tunnel.start(qEnvironmentVariable("NIXSCP_AGENT_CLIENT"), {QStringLiteral("--tunnel"), QStringLiteral("agent-allow"), Forward});
    CHECK(Tunnel.waitForStarted(10000));
    QByteArray Err;
    CHECK(WaitFor([&Tunnel, &Err]() { Err += Tunnel.readAllStandardError(); return Err.contains("Tunnel open"); }, 30000));
    QTcpSocket Socket;
    Socket.connectToHost(QStringLiteral("127.0.0.1"), static_cast<quint16>(LocalPort));
    QByteArray Banner;
    WaitFor([&Socket, &Banner]() { Banner += Socket.readAll(); return Banner.contains("\n"); }, 15000);
    CHECK_MSG(Banner.startsWith("SSH-2.0"), QString::fromUtf8(Banner + Err));
    Socket.close();
    Tunnel.kill();
    Tunnel.waitForFinished(5000);
    // closed with its client
    WaitFor([LocalPort]() { QTcpSocket Probe; Probe.connectToHost(QStringLiteral("127.0.0.1"), static_cast<quint16>(LocalPort));
      return !Probe.waitForConnected(300); }, 10000);
    QTcpSocket Probe;
    Probe.connectToHost(QStringLiteral("127.0.0.1"), static_cast<quint16>(LocalPort));
    CHECK(!Probe.waitForConnected(500));
  }
  R = Agent({QStringLiteral("--tunnel"), QStringLiteral("agent-commands-only"), QStringLiteral("22224:localhost:2222")});
  CHECK_MSG((R.Code == 255) && R.Err.contains("does not allow"), QString::fromUtf8(R.Err));
  R = Agent({QStringLiteral("--tunnel"), QStringLiteral("agent-allow"), QStringLiteral("no-such-tunnel")});
  CHECK((R.Code == 255) && R.Err.contains("no saved tunnel"));

  // ---- refusals
  R = Agent({QStringLiteral("--connect"), QStringLiteral("agent-off"), QStringLiteral("--cmd"), QStringLiteral("true")});
  CHECK_MSG((R.Code == 255) && R.Err.contains("not available to agents"), QString::fromUtf8(R.Err));
  R = Agent({QStringLiteral("--connect"), QStringLiteral("no-such-site"), QStringLiteral("--cmd"), QStringLiteral("true")});
  CHECK((R.Code == 255) && R.Err.contains("not available to agents"));
  R = Agent({QStringLiteral("--get"), QStringLiteral("agent-commands-only"), QStringLiteral("/etc/hostname"), Dir.path()});
  CHECK_MSG((R.Code == 255) && R.Err.contains("file transfers"), QString::fromUtf8(R.Err));
  R = Agent({QStringLiteral("--connect"), QStringLiteral("agent-commands-only"), QStringLiteral("--cmd"), QStringLiteral("true")});
  CHECK(R.Code == 0);
  R = Agent({QStringLiteral("--connect"), QStringLiteral("agent-new-host"), QStringLiteral("--cmd"), QStringLiteral("true")});
  CHECK_MSG((R.Code == 255) && R.Err.contains("host key"), QString::fromUtf8(R.Err));
  R = Agent({QStringLiteral("--connect"), QStringLiteral("agent-allow")}, QByteArray());
  CHECK_MSG((R.Code == 255) && R.Err.contains("needs a terminal"), QString::fromUtf8(R.Err));

  // ---- interactive shell in a terminal ("script" provides one)
  if (!QStandardPaths::findExecutable(QStringLiteral("script")).isEmpty())
  {
    QProcess P;
    P.start(QStringLiteral("script"), {QStringLiteral("-qec"),
      QStringLiteral("%1 --connect agent-allow").arg(qEnvironmentVariable("NIXSCP_AGENT_CLIENT")), QStringLiteral("/dev/null")});
    CHECK(P.waitForStarted(10000));
    P.write("echo shell-$((6*7))\nexit 3\n");
    QByteArray Out;
    WaitFor([&P, &Out]() { Out += P.readAll(); return P.state() == QProcess::NotRunning; }, 60000);
    Out += P.readAll();
    CHECK_MSG(Out.contains("shell-42") && (P.exitCode() == 3), QString::fromUtf8(Out));
  }

  // ---- approval
  AnswerApproval(QStringLiteral("Allow once"));
  R = Agent({QStringLiteral("--connect"), QStringLiteral("agent-ask"), QStringLiteral("--cmd"), QStringLiteral("echo approved")});
  CHECK_MSG((R.Code == 0) && (R.Out == "approved\n"), QString::fromUtf8(R.Err));
  AnswerApproval(QStringLiteral("Deny"));
  R = Agent({QStringLiteral("--connect"), QStringLiteral("agent-ask"), QStringLiteral("--cmd"), QStringLiteral("echo nope")});
  CHECK_MSG((R.Code == 255) && R.Err.contains("denied"), QString::fromUtf8(R.Err));
  AnswerApproval(QStringLiteral("1 hour"));
  R = Agent({QStringLiteral("--connect"), QStringLiteral("agent-ask"), QStringLiteral("--cmd"), QStringLiteral("true")});
  CHECK(R.Code == 0);
  // remembered: no prompt this time
  R = Agent({QStringLiteral("--connect"), QStringLiteral("agent-ask"), QStringLiteral("--cmd"), QStringLiteral("true")});
  CHECK(R.Code == 0);
  // "Always" turns the site into allowed-without-asking, stored in the configuration
  AddSite(QStringLiteral("agent-ask-2"), AgentAccess::amAsk, AgentAccess::apAll, KnownHosts);
  AnswerApproval(QStringLiteral("Always"));
  R = Agent({QStringLiteral("--connect"), QStringLiteral("agent-ask-2"), QStringLiteral("--cmd"), QStringLiteral("true")});
  CHECK(R.Code == 0);
  CHECK(Config.StoredSessions().FindByName(QStringLiteral("agent-ask-2"))->AgentAccess == AgentAccess::amAllow);

  // ---- the audit log records requests, allowed and denied
  QFile Log(AgentAccess::AuditLogFileName());
  CHECK(Log.open(QIODevice::ReadOnly));
  QByteArray LogText = Log.readAll();
  CHECK(LogText.contains("agent-allow\texec\tallowed\techo hello"));
  CHECK(LogText.contains("Clients/secret-client [agent name: web-alias]\texec\tallowed\techo via-alias"));
  CHECK(LogText.contains("agent-off\texec\tdenied: not allowed"));
  CHECK(LogText.contains("agent-ask\texec\tdenied: by user"));
  CHECK(!LogText.contains(qgetenv("NIXSCP_TEST_AGENT_SSH_PASSWORD")));
  CHECK((Log.permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther)) == 0);

  CloudSync::Instance().SignOut();
  WaitFor([]() { return !CloudSync::Instance().SignedIn(); }, 10000);
  if (Failures == 0)
  {
    std::printf("All agent access tests passed\n");
  }
  return (Failures == 0) ? 0 : 1;
}
