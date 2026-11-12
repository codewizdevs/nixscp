// Agent Access end to end without a cloud account: the real broker runs in this
// process with a master password, the real "nixscp" client as a child process,
// against the small SSH server of the test (tests/testsshserver.h) and, for
// file transfers, the OpenSSH test server (NIXSCP_TEST_SSH_PORT and
// NIXSCP_TEST_SSH_KEY; those parts are skipped without them).
// NIXSCP_AGENT_CLIENT: the nixscp executable (default: next to this test).
// NIXSCP_GUI_TESTS
#include "core/AgentAccess.h"
#include "core/CloudCrypto.h"
#include "core/ConfigVault.h"
#include "core/Configuration.h"
#include "core/IniStorage.h"
#include "core/SessionData.h"
#include "gui/AgentAccessDialog.h"
#include "gui/AgentBroker.h"
#include "gui/TunnelsDialog.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCryptographicHash>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLocalSocket>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRandomGenerator>
#include <QScreen>
#include <QTableWidget>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <cstdio>
#include <functional>
#include <memory>

#if (defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)) || defined(Q_OS_MACOS)
#include "testsshserver.h"
#endif
#ifdef Q_OS_MACOS
#include <sys/resource.h>
#endif

using namespace AgentAccess;

static int Failures = 0;

#define CHECK(Cond) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); std::fflush(stdout); Failures++; } } while (0)
#define CHECK_MSG(Cond, Msg) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n  %s\n", __FILE__, __LINE__, #Cond, qPrintable(Msg)); std::fflush(stdout); Failures++; } } while (0)

#if (defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)) || defined(Q_OS_MACOS)

static QString GClient;
static QString GKnownHosts;
static int GPort = 0;

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

static void Pump(int Ms)
{
  QElapsedTimer Timer;
  Timer.start();
  while (Timer.elapsed() < Ms)
  {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  }
}

struct Result
{
  int Code = -1;
  QByteArray Out;
  QByteArray Err;
  QString Text() const { return QStringLiteral("code %1, out [%2], err [%3]").arg(Code).arg(QString::fromUtf8(Out.left(300)), QString::fromUtf8(Err.left(300))); }
};

static Result Run(const QString & Program, const QStringList & Arguments, const QByteArray & Input = QByteArray())
{
  QProcess P;
  P.start(Program, Arguments);
  Result R;
  if (!P.waitForStarted(10000))
  {
    return R;
  }
  P.write(Input);
  P.closeWriteChannel();
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

static Result Agent(const QStringList & Arguments, const QByteArray & Input = QByteArray())
{
  return Run(GClient, Arguments, Input);
}

static Result Connect(const QString & Site, const QString & Command, const QByteArray & Input = QByteArray())
{
  return Agent({QStringLiteral("--connect"), Site, QStringLiteral("--cmd"), Command}, Input);
}

static std::shared_ptr<SessionData> AddSite(const QString & Name, int Access, int Permissions, bool Confirm = true)
{
  auto Site = std::make_shared<SessionData>();
  Site->Name = Name;
  Site->HostName = QStringLiteral("127.0.0.1");
  Site->PortNumber = GPort;
  Site->UserName = QStringLiteral("u");
  Site->Password = QStringLiteral("stored site password");
  Site->FSProtocol = fsSFTP;
  Site->KnownHostsFile = GKnownHosts;
  Site->AgentAccess = Access;
  Site->AgentPermissions = Permissions;
  Configuration::Instance().StoredSessions().Add(Site);
  if (Confirm)
  {
    CHECK(ConfirmSettings(*Site));
  }
  return Site;
}

static QDialog * OpenPrompt()
{
  for (QWidget * W : QApplication::topLevelWidgets())
  {
    auto * Box = qobject_cast<QDialog *>(W);
    if ((Box != nullptr) && Box->isVisible() && (Box->windowTitle() == QStringLiteral("NixSCP Agent Access")))
    {
      return Box;
    }
  }
  return nullptr;
}

static bool Click(QDialog * Box, const QString & Caption)
{
  for (QAbstractButton * Button : Box->findChildren<QAbstractButton *>())
  {
    if (Button->text() == Caption)
    {
      Button->click();
      return true;
    }
  }
  return false;
}

// Answers the next prompt with the button captioned Caption
static void Answer(const QString & Caption)
{
  auto * Timer = new QTimer();
  QObject::connect(Timer, &QTimer::timeout, Timer, [Timer, Caption]()
  {
    if (QDialog * Box = OpenPrompt())
    {
      if (Click(Box, Caption))
      {
        Timer->deleteLater();
      }
    }
  });
  Timer->start(30);
}

static QByteArray AuditLog()
{
  QFile Log(AuditLogFileName());
  return Log.open(QIODevice::ReadOnly) ? Log.readAll() : QByteArray();
}

// A client that speaks the frames itself, for what the real one never sends
class RawClient
{
public:
  RawClient()
  {
    FSocket.connectToServer(SocketName());
    CHECK(FSocket.waitForConnected(3000));
  }
  void Send(TFrame Type, const QByteArray & Payload)
  {
    FSocket.write(Frame(Type, Payload));
  }
  void Request(const QJsonObject & Json)
  {
    Send(frRequest, QJsonDocument(Json).toJson(QJsonDocument::Compact));
  }
  // waits for the end of the request
  bool Finish(QString * Error = nullptr)
  {
    bool Done = false, Failed = false;
    FrameReader Reader;
    WaitFor([&]()
    {
      Reader.Append(FSocket.readAll());
      quint8 Type = 0;
      QByteArray Payload;
      bool Bad = false;
      while (Reader.Next(Type, Payload, Bad))
      {
        if (Type == frError)
        {
          Failed = true;
          Done = true;
          if (Error != nullptr)
          {
            *Error = QString::fromUtf8(Payload);
          }
        }
        else if (Type == frExit)
        {
          Done = true;
        }
        else if (Type == frStdout)
        {
          Output += Payload;
        }
      }
      return Done || (FSocket.state() != QLocalSocket::ConnectedState);
    }, 30000);
    return Done && !Failed;
  }
  QLocalSocket & Socket() { return FSocket; }
  QByteArray Output;
private:
  QLocalSocket FSocket;
};

// X-M7: \u0000 in the JSON of a request
static void TestNul()
{
  auto Site = AddSite(QStringLiteral("nul"), amAllow, apAll);
  for (const char * Field : {"command", "site", "remote", "name", "forward", "term"})
  {
    QJsonObject Request{{QStringLiteral("op"), QStringLiteral("exec")}, {QStringLiteral("site"), QStringLiteral("nul")},
      {QStringLiteral("command"), QStringLiteral("big 5")}};
    QString Text = Request.value(QLatin1String(Field)).toString(QStringLiteral("x"));
    Text.insert(1, QChar(0));
    Request.insert(QLatin1String(Field), Text);
    RawClient Client;
    Client.Request(Request);
    QString Error;
    CHECK(!Client.Finish(&Error));
    CHECK_MSG(Error.contains(QStringLiteral("NUL")), Error);
  }
  CHECK(AuditLog().contains("denied: NUL character in the request"));
  CHECK(AuditLog().contains("\\x00"));
}

// Finding 1 of the review: a lone surrogate (\ud800 in the JSON), which the rules,
// the prompt and the log see and toUtf8() drops. In every text of a request
static void TestInvalidText()
{
  auto Site = AddSite(QStringLiteral("utf16"), amAllow, apAll);
  for (const char * Field : {"command", "site", "remote", "name", "forward", "term", "op"})
  {
    for (char16_t Surrogate : {char16_t(0xD800), char16_t(0xDBFF), char16_t(0xDC00), char16_t(0xDFFF)})
    {
      QJsonObject Request{{QStringLiteral("op"), QStringLiteral("exec")}, {QStringLiteral("site"), QStringLiteral("utf16")},
        {QStringLiteral("command"), QStringLiteral("big 5")}};
      QString Text = Request.value(QLatin1String(Field)).toString(QStringLiteral("x"));
      Text.insert(1, QChar(Surrogate));
      Request.insert(QLatin1String(Field), Text);
      RawClient Client;
      Client.Request(Request);
      QString Error;
      CHECK_MSG(!Client.Finish(&Error), QString::fromLatin1(Field));
      CHECK_MSG(Error.contains(QStringLiteral("Unicode")), Error);
    }
  }
  CHECK(AuditLog().contains("denied: invalid text in the request"));
  // the log says what came, visibly
  CHECK(AuditLog().contains("\\ud800"));
  CHECK(AuditLog().contains("\\udfff"));
  // a byte order mark at the start of the text would not reach the server
  {
    RawClient Client;
    Client.Send(frRequest, R"J({"op":"exec","site":"utf16","command":"\ufeffbig 5"})J");
    QString Error;
    CHECK(!Client.Finish(&Error));
    CHECK_MSG(Error.contains(QStringLiteral("Unicode")), Error);
  }
  // as a raw client writes it, and bytes that are no UTF-8 at all
  for (const char * Json : {R"J({"op":"exec","site":"utf16","command":"bytes \ud800(x)"})J",
    R"J({"op":"exec","site":"utf16","command":"bytes \ud800A"})J",
    "{\"op\":\"exec\",\"site\":\"utf16\",\"command\":\"bytes \xED\xA0\x80\"}",
    "{\"op\":\"exec\",\"site\":\"utf16\",\"command\":\"bytes \xC0\x80\"}"})
  {
    RawClient Client;
    Client.Send(frRequest, Json);
    QString Error;
    CHECK_MSG(!Client.Finish(&Error), QString::fromLatin1(Json));
    CHECK_MSG(Client.Output.isEmpty(), QString::fromLatin1(Json));
  }
  // the first frame only names a file
  {
    RawClient Client;
    Client.Request(QJsonObject{{QStringLiteral("op"), QStringLiteral("put")}, {QStringLiteral("site"), QStringLiteral("utf16")},
      {QStringLiteral("remote"), QStringLiteral("/tmp")}, {QStringLiteral("name"), QStringLiteral("../x")}, {QStringLiteral("size"), 0}});
    QString Error;
    CHECK(!Client.Finish(&Error));
    CHECK_MSG(Error.contains(QStringLiteral("file name")), Error);
  }
}

// What the server gets is the text that was checked, byte for byte: no
// normalization, nothing added or lost, with and without rules
static void TestExactBytes()
{
  auto Plain = AddSite(QStringLiteral("bytes"), amAllow, apCommands);
  auto Rules = AddSite(QStringLiteral("bytes-rules"), amAllow, apCommands, false);
  Rules->AgentAllow = QStringLiteral("bytes *");
  Rules->AgentDeny = QStringLiteral("rm *");
  CHECK(ConfirmSettings(*Rules));
  for (const QString & Tail : {QStringLiteral("caf\u00e9"), QStringLiteral("cafe\u0301"), QStringLiteral("\u1e9e\u212a\u017f"), QStringLiteral("\U0001F600 x"),
    QStringLiteral("a\ufeffb"), QStringLiteral("a\uffffb\ufffe"), QStringLiteral("a\u00a0b\u2003c\u3000d"), QStringLiteral("a\u0085b\u2028c"), QStringLiteral("a\u037eb\u1fefc"),
    QStringLiteral("\ufdd0\ufffd"), QStringLiteral("  two  spaces  ")})
  {
    for (const QString & Name : {QStringLiteral("bytes"), QStringLiteral("bytes-rules")})
    {
      QString Command = QStringLiteral("bytes ") + Tail;
      RawClient Client;
      Client.Request(QJsonObject{{QStringLiteral("op"), QStringLiteral("exec")}, {QStringLiteral("site"), Name}, {QStringLiteral("command"), Command}});
      QString Error;
      CHECK_MSG(Client.Finish(&Error), Error);
      CHECK_MSG(Client.Output == Command.toUtf8(), QStringLiteral("%1: %2").arg(VisibleText(Command, false), QString::fromLatin1(Client.Output.toHex(' '))));
    }
  }
}

// The proof of the review on the way it was found: the real broker, real ssh, a
// real shell, as a raw client and through MCP
static void TestSurrogateOnServer(const QString & Folder, const QString & OpenSshKnownHosts)
{
  if (OpenSshKnownHosts.isEmpty())
  {
    std::printf("SKIP surrogates on a real shell: set NIXSCP_TEST_SSH_PORT and NIXSCP_TEST_SSH_KEY\n");
    return;
  }
  auto Prepare = [&OpenSshKnownHosts](const QString & Name, const QString & Allow, const QString & Deny)
  {
    auto Site = AddSite(Name, amAllow, apAll, false);
    Site->PortNumber = qEnvironmentVariableIntValue("NIXSCP_TEST_SSH_PORT");
    Site->UserName = qEnvironmentVariable("USER", QStringLiteral("user"));
    Site->Password.clear();
    Site->PublicKeyFile = qEnvironmentVariable("NIXSCP_TEST_SSH_KEY");
    Site->KnownHostsFile = OpenSshKnownHosts;
    Site->AgentAllow = Allow;
    Site->AgentDeny = Deny;
    CHECK(ConfirmSettings(*Site));
    return Site;
  };
  auto ReadOnly = Prepare(QStringLiteral("real-readonly"), ReadOnlyAllowPreset(), ReadOnlyDenyPreset());
  auto DenyOnly = Prepare(QStringLiteral("real-deny"), QString(), QStringLiteral("rm\nrm *\n/bin/rm *"));
  auto Free = Prepare(QStringLiteral("real-free"), QString(), QString());
  QString Marker = Folder + QStringLiteral("/substitution-ran");
  QString Victim = Folder + QStringLiteral("/victim");
  auto Request = [](const QString & Site, const QByteArray & EscapedCommand)
  {
    return "{\"op\":\"exec\",\"site\":\"" + Site.toUtf8() + "\",\"command\":\"" + EscapedCommand + "\"}";
  };

  // the payload is alive: without the surrogate the shell runs it. So does a
  // site with the rules gone, which is why the text must not lose it
  Result R = Connect(QStringLiteral("real-free"), QStringLiteral("ls < \"$(touch '%1')\"").arg(Marker));
  CHECK_MSG(QFile::exists(Marker), R.Text());
  QFile::remove(Marker);
  QByteArray Payload = R"J(ls < \"$\ud800(touch )J" + Marker.toUtf8() + R"J()\")J";
  for (const QString & Site : {QStringLiteral("real-readonly"), QStringLiteral("real-deny"), QStringLiteral("real-free")})
  {
    RawClient Client;
    Client.Send(frRequest, Request(Site, Payload));
    QString Error;
    CHECK_MSG(!Client.Finish(&Error), Site);
    CHECK_MSG(Error.contains(QStringLiteral("Unicode")), Error);
    CHECK_MSG(!QFile::exists(Marker), Site);
    QFile::remove(Marker);
  }
  // a denied program: without the surrogate this is /bin/rm x
  {
    QFile F(Victim);
    CHECK(F.open(QIODevice::WriteOnly));
  }
  {
    RawClient Client;
    Client.Send(frRequest, Request(QStringLiteral("real-deny"), R"J(/bin/r\ud800m )J" + Victim.toUtf8()));
    QString Error;
    CHECK(!Client.Finish(&Error));
    CHECK_MSG(QFile::exists(Victim), Error);
  }
  // through the MCP server, what agents use
  QProcess Mcp;
  Mcp.start(GClient, {QStringLiteral("--mcp")});
  CHECK(Mcp.waitForStarted(10000));
  auto Call = [&Mcp](int Id, const QString & Site, const QByteArray & Command)
  {
    QByteArray Line = "{\"jsonrpc\":\"2.0\",\"id\":" + QByteArray::number(Id) + ",\"method\":\"tools/call\",\"params\":{\"name\":\"run_command\",\"arguments\":{\"site\":\"" +
      Site.toUtf8() + "\",\"command\":\"" + Command + "\"}}}\n";
    Mcp.write(Line);
    QByteArray Out;
    CHECK(WaitFor([&]() { Out += Mcp.readAllStandardOutput(); return Out.contains('\n'); }, 30000));
    return Out;
  };
  QByteArray Reply = Call(1, QStringLiteral("real-readonly"), Payload);
  CHECK_MSG(Reply.contains("\"isError\":true") && Reply.contains("Unicode"), QString::fromUtf8(Reply));
  CHECK(!QFile::exists(Marker));
  Reply = Call(2, QStringLiteral("real-deny"), R"J(/bin/r\ud800m )J" + Victim.toUtf8());
  CHECK_MSG(Reply.contains("\"isError\":true"), QString::fromUtf8(Reply));
  CHECK(QFile::exists(Victim));
  Mcp.closeWriteChannel();
  CHECK(Mcp.waitForFinished(10000));
  // the bytes reach the shell as they were sent: a real pair and a mark in the middle
  // (encodeName: what QProcess does to the arguments of the client on macOS)
  QString Text = QStringLiteral("caf\u00e9-cafe\u0301-\U0001F600-a\ufeffb");
  R = Connect(QStringLiteral("real-readonly"), QStringLiteral("echo '%1'").arg(Text));
  CHECK_MSG((R.Code == 0) && (R.Out == QFile::encodeName(Text) + '\n'), R.Text());
  // rules out of order would show here: the same line with a hidden denied option
  QString Dashes(5000, QLatin1Char('-'));
  R = Connect(QStringLiteral("real-readonly"), QStringLiteral("file -C -m /nonexistent/x ") + Dashes);
  CHECK_MSG(R.Code == 255, R.Text());
  QFile::remove(Victim);
}

// X-H2, X-M1: what the channel does not take now waits, and is not lost
static void TestStdin()
{
  auto Site = AddSite(QStringLiteral("stdin"), amAllow, apCommands);
  // the server takes nothing for 2 seconds: more than its window has to wait
  QByteArray Input(20 * 1024 * 1024, 'i');
  Result R = Connect(QStringLiteral("stdin"), QStringLiteral("count 2"), Input);
  CHECK_MSG((R.Code == 0) && (R.Out.trimmed() == QByteArray::number(Input.size())), R.Text());
  // both ways at once: a program that writes while its input is still coming
  QByteArray Random(6 * 1024 * 1024, Qt::Uninitialized);
  for (char & C : Random)
  {
    C = static_cast<char>(QRandomGenerator::global()->bounded(256));
  }
  R = Connect(QStringLiteral("stdin"), QStringLiteral("cat"), Random);
  CHECK_MSG((R.Code == 0) && (R.Out == Random), QStringLiteral("%1 of %2 bytes, %3").arg(R.Out.size()).arg(Random.size()).arg(QString::fromUtf8(R.Err)));
  // empty input ends too
  R = Connect(QStringLiteral("stdin"), QStringLiteral("count 0"));
  CHECK_MSG((R.Code == 0) && (R.Out.trimmed() == "0"), R.Text());
}

static qint64 PeakMemoryKb()
{
#ifdef Q_OS_MACOS
  struct rusage Usage = {};
  getrusage(RUSAGE_SELF, &Usage);
  return Usage.ru_maxrss / 1024;
#endif
  QFile Status(QStringLiteral("/proc/self/status"));
  if (Status.open(QIODevice::ReadOnly))
  {
    for (const QByteArray & Line : Status.readAll().split('\n'))
    {
      if (Line.startsWith("VmHWM:"))
      {
        return Line.mid(6).trimmed().split(' ').first().toLongLong();
      }
    }
  }
  return 0;
}

// X-M1: the broker reached 6.3 GB with a large stdin
static void TestStdinMemory()
{
  qint64 Before = PeakMemoryKb();
  Result R = Run(QStringLiteral("/bin/sh"), {QStringLiteral("-c"),
    QStringLiteral("head -c 400000000 /dev/zero | '%1' --connect stdin --cmd 'count 3'").arg(GClient)});
  CHECK_MSG((R.Code == 0) && (R.Out.trimmed() == "400000000"), R.Text());
  qint64 Grown = (PeakMemoryKb() - Before) / 1024;
  std::printf("broker memory while 400 MB went through stdin: +%lld MB\n", static_cast<long long>(Grown));
  CHECK_MSG(Grown < 64, QStringLiteral("+%1 MB").arg(Grown));
  // a client that ignores the flow control is dropped, not fed
  RawClient Flood;
  Flood.Request(QJsonObject{{QStringLiteral("op"), QStringLiteral("exec")}, {QStringLiteral("site"), QStringLiteral("stdin")},
    {QStringLiteral("command"), QStringLiteral("count 60")}});
  QByteArray Chunk(MaxFrameSize, 'f');
  for (int I = 0; I < 12; I++)
  {
    Flood.Send(frStdin, Chunk);
  }
  QString Error;
  CHECK(!Flood.Finish(&Error));
  // the message is best effort: closing with unread input can drop it on the way
  CHECK_MSG(Error.isEmpty() || Error.contains(QStringLiteral("flow control")), Error);
  CHECK(WaitFor([]() { return AgentBroker::ActiveJobs() == 0; }, 10000));
  // at a pace the job keeps up with: what it took from the queue and cannot send
  // yet counts too, or the same client grows the memory of the broker for ever
  RawClient Paced;
  Paced.Request(QJsonObject{{QStringLiteral("op"), QStringLiteral("exec")}, {QStringLiteral("site"), QStringLiteral("stdin")},
    {QStringLiteral("command"), QStringLiteral("count 60")}});
  CHECK(WaitFor([]() { return AgentBroker::ActiveJobs() == 1; }, 5000));
  Pump(500);
  for (int I = 0; (I < 24) && (Paced.Socket().state() == QLocalSocket::ConnectedState); I++)
  {
    Paced.Send(frStdin, Chunk);
    Pump(100);
  }
  CHECK(WaitFor([&Paced]() { Paced.Socket().readAll(); return Paced.Socket().state() != QLocalSocket::ConnectedState; }, 5000));
  Paced.Socket().abort();
  CHECK(WaitFor([]() { return AgentBroker::ActiveJobs() == 0; }, 10000));
}

// X-H3: a slow reader got the start of the output and a clean exit
static void TestSlowReader()
{
  auto Site = AddSite(QStringLiteral("slow"), amAllow, apCommands);
  // less than the broker buffers: the job ends while the client is not reading,
  // with most of the output still waiting in the socket. More: the job waits
  for (int Size : {6000000, 6000000, 30000000})
  {
    Result R = Run(QStringLiteral("/bin/sh"), {QStringLiteral("-c"),
      QStringLiteral("'%1' --connect slow --cmd 'big %2' | (sleep 3; wc -c)").arg(GClient).arg(Size)});
    CHECK_MSG(R.Out.trimmed() == QByteArray::number(Size), R.Text());
  }
  // the exit status makes it too
  Result R = Run(QStringLiteral("/bin/sh"), {QStringLiteral("-c"),
    QStringLiteral("'%1' --connect slow --cmd 'big 12000000' > '%2/slow.out'; echo $?").arg(GClient, QFileInfo(AuditLogFileName()).absolutePath())});
  CHECK_MSG(R.Out.trimmed() == "0", R.Text());
  CHECK(QFileInfo(QFileInfo(AuditLogFileName()).absolutePath() + QStringLiteral("/slow.out")).size() == 12000000);
}

// L4
static void TestExitStatus()
{
  auto Site = AddSite(QStringLiteral("status"), amAllow, apCommands);
  Result R = Connect(QStringLiteral("status"), QStringLiteral("noexit"));
  CHECK_MSG((R.Code == 255) && R.Err.contains("exit status"), R.Text());
  // the status may come long after the end of the output
  R = Connect(QStringLiteral("status"), QStringLiteral("quiet 2 7"));
  CHECK_MSG(R.Code == 7, R.Text());
  R = Connect(QStringLiteral("status"), QStringLiteral("big 10"));
  CHECK_MSG((R.Code == 0) && (R.Out.size() == 10), R.Text());
}

// L5, L6, X-M12
static void TestPrompts(const QString & Folder)
{
  auto Site = AddSite(QStringLiteral("ask"), amAsk, apAll);
  // the program gives up while the question is open: not the user's "no"
  QProcess Gone;
  Gone.start(GClient, {QStringLiteral("--connect"), QStringLiteral("ask"), QStringLiteral("--cmd"), QStringLiteral("big 1")});
  CHECK(WaitFor([]() { return OpenPrompt() != nullptr; }, 15000));
  Gone.kill();
  Gone.waitForFinished(5000);
  CHECK(WaitFor([]() { return OpenPrompt() == nullptr; }, 10000));
  CHECK(AuditLog().contains("ask\texec\tdenied: the program went away"));
  CHECK(!AuditLog().contains("ask\texec\tdenied: by user"));

  // the settings changed while it was open: the answer is about the old ones
  auto TakeBack = [&](const std::function<void()> & Change, const char * Expected)
  {
    QProcess Client;
    Client.start(GClient, {QStringLiteral("--connect"), QStringLiteral("ask"), QStringLiteral("--cmd"), QStringLiteral("big 1")});
    CHECK(WaitFor([]() { return OpenPrompt() != nullptr; }, 15000));
    Change();
    CHECK(Click(OpenPrompt(), QStringLiteral("Allow once")));
    QByteArray Err;
    CHECK(WaitFor([&]() { Err += Client.readAllStandardError(); return Client.state() == QProcess::NotRunning; }, 15000));
    Err += Client.readAllStandardError();
    CHECK_MSG((Client.exitCode() == 255) && Err.contains(Expected), QString::fromUtf8(Err));
  };
  TakeBack([&]() { Site->AgentAccess = amOff; }, "not available to agents");
  Site->AgentAccess = amAsk;
  TakeBack([&]() { Site->AgentPermissions = apTransfer; }, "does not allow commands");
  Site->AgentPermissions = apAll;
  TakeBack([&]() { Site->AgentAllow = QStringLiteral("uptime"); }, "not among the commands allowed");
  Site->AgentAllow.clear();

  // what is asked can be read, and the buttons can be reached
  QString Hidden = QString(3000, QLatin1Char('\n')) + QStringLiteral("rm -rf /important");
  QString Command = QStringLiteral("echo <b>fine</b>") + QString(20000, QLatin1Char(' ')) + Hidden;
  QProcess Client;
  Client.start(GClient, {QStringLiteral("--connect"), QStringLiteral("ask"), QStringLiteral("--cmd"), Command});
  CHECK(WaitFor([]() { return OpenPrompt() != nullptr; }, 15000));
  Pump(200);
  QDialog * Box = OpenPrompt();
  CHECK(Box != nullptr);
  if (Box != nullptr)
  {
    auto * Field = Box->findChild<QPlainTextEdit *>(QStringLiteral("RequestText"));
    CHECK(Field != nullptr);
    if (Field != nullptr)
    {
      CHECK(Field->toPlainText().contains(QStringLiteral("rm -rf /important")));
      CHECK(Field->toPlainText().contains(QStringLiteral("<b>fine</b>")));
      CHECK(Field->isReadOnly());
    }
    CHECK(Box->findChild<QLabel *>(QStringLiteral("RequestLong")) != nullptr);
    QSize Available = QApplication::primaryScreen()->availableGeometry().size();
    CHECK_MSG((Box->height() <= Available.height()) && (Box->width() <= Available.width()),
      QStringLiteral("%1x%2 on %3x%4").arg(Box->width()).arg(Box->height()).arg(Available.width()).arg(Available.height()));
    for (QAbstractButton * Button : Box->findChildren<QAbstractButton *>())
    {
      QPoint Corner = Button->mapTo(Box, QPoint(Button->width(), Button->height()));
      CHECK(Button->isVisible() && (Corner.y() <= Box->height()) && (Corner.x() <= Box->width()));
    }
    CHECK(Click(Box, QStringLiteral("Deny")));
  }
  CHECK(WaitFor([&Client]() { return Client.state() == QProcess::NotRunning; }, 15000));
  CHECK(Client.exitCode() == 255);
  // text in a prompt is text: no markup, no control characters
  CHECK(VisibleText(QStringLiteral("a\x1b[2Jb"), true) == QStringLiteral("a\\x1b[2Jb"));

  // approval still works: once, and for a while
  Answer(QStringLiteral("Allow once"));
  Result R = Connect(QStringLiteral("ask"), QStringLiteral("big 3"));
  CHECK_MSG((R.Code == 0) && (R.Out.size() == 3), R.Text());
  Answer(QStringLiteral("Deny"));
  R = Connect(QStringLiteral("ask"), QStringLiteral("big 3"));
  CHECK_MSG((R.Code == 255) && R.Err.contains("denied"), R.Text());
  CHECK(AuditLog().contains("ask\texec\tdenied: by user"));
  Answer(QStringLiteral("1 hour"));
  R = Connect(QStringLiteral("ask"), QStringLiteral("big 3"));
  CHECK(R.Code == 0);
  R = Connect(QStringLiteral("ask"), QStringLiteral("big 3"));
  CHECK(R.Code == 0);
  // "Always" stays valid under the seal of its settings
  auto Forever = AddSite(QStringLiteral("forever"), amAsk, apAll);
  Answer(QStringLiteral("Always"));
  R = Connect(QStringLiteral("forever"), QStringLiteral("big 3"));
  CHECK_MSG(R.Code == 0, R.Text());
  CHECK(Forever->AgentAccess == amAllow);
  CHECK(SettingsConfirmed(*Forever));
  R = Connect(QStringLiteral("forever"), QStringLiteral("big 3"));
  CHECK_MSG(R.Code == 0, R.Text());
  Q_UNUSED(Folder);
}

// X-M16: settings an agent wrote into the plain configuration file
// Finding 12 of the review: a field nobody thought of. Every value a saved site
// has, as the ini file holds it, changed one at a time
static void TestEverySettingCounts(const SessionData & Site)
{
  IniFile Ini;
  IniStorage Storage(&Ini);
  CHECK(Storage.OpenSubKey(QStringLiteral("Site"), true));
  Site.Save(Storage);
  auto Loaded = [&Storage, &Site]()
  {
    SessionData Data;
    Data.Name = Site.Name;
    Data.Load(Storage);
    return Data;
  };
  // saved and read back it is the site that was accepted
  CHECK(SettingsConfirmed(Loaded()));
  // also as the configuration file has it, without what equals the default
  {
    IniFile Relative;
    IniStorage Stored(&Relative);
    CHECK(Stored.OpenSubKey(QStringLiteral("Site"), true));
    SessionData Defaults;
    Site.Save(Stored, &Defaults);
    SessionData Data;
    Data.Name = Site.Name;
    Data.Load(Stored);
    CHECK(SettingsConfirmed(Data));
  }
  QStringList Missed;
  const QStringList Keys = Storage.GetValueNames();
  CHECK(Keys.size() > 100);
  // the passwords are not forged without the key, and the rest is what the panels use
  const QStringList NoSeal{QStringLiteral("Password"), QStringLiteral("Color"), QStringLiteral("Note"), QStringLiteral("LocalDirectory"),
    QStringLiteral("OtherLocalDirectory"), QStringLiteral("RemoteDirectory")};
  for (const QString & Key : Keys)
  {
    QString Original = Storage.ReadStringRaw(Key, QString());
    bool Number = false;
    int Value = Original.toInt(&Number);
    Storage.WriteStringRaw(Key, Number ? QString::number((Value == 1) ? 0 : Value + 1) : (Original + QStringLiteral("x")));
    if (SettingsConfirmed(Loaded()) && !NoSeal.contains(Key))
    {
      Missed.append(Key);
    }
    if (NoSeal.contains(Key))
    {
      CHECK_MSG(SettingsConfirmed(Loaded()), Key);
    }
    Storage.WriteStringRaw(Key, Original);
  }
  CHECK_MSG(Missed.isEmpty(), Missed.join(QLatin1Char(' ')));
  // a value this version has no field for (an older file, a future setting)
  Storage.WriteStringRaw(QStringLiteral("SomeFutureSetting"), QStringLiteral("1"));
  CHECK(!SettingsConfirmed(Loaded()));
  Storage.DeleteValue(QStringLiteral("SomeFutureSetting"));
  CHECK(SettingsConfirmed(Loaded()));
  // what the user accepts goes into the log too: it says that a secret changed, not which
  SessionData Secret = Site;
  Secret.S3SessionToken = QStringLiteral("token value");
  CHECK(!SettingsConfirmed(Secret));
  CHECK(SettingsText(Secret).contains(QStringLiteral("S3SessionToken")) && !SettingsText(Secret).contains(QStringLiteral("token value")));
  Storage.WriteStringRaw(QStringLiteral("EncryptKey"), QStringLiteral("key value"));
  CHECK(!SettingsText(Loaded()).contains(QStringLiteral("key value")));
  Storage.DeleteValue(QStringLiteral("EncryptKey"));
  // the password can change without anyone having to accept the settings again
  SessionData Changed = Site;
  Changed.Password = QStringLiteral("another password");
  Changed.Note = QStringLiteral("a note");
  Changed.RemoteDirectory = QStringLiteral("/last/used");
  CHECK(SettingsConfirmed(Changed));
}

static void TestTamperedSettings()
{
  auto Site = AddSite(QStringLiteral("tamper"), amAllow, apCommands);
  CHECK(SettingsConfirmed(*Site));
  Result R = Connect(QStringLiteral("tamper"), QStringLiteral("big 1"));
  CHECK_MSG(R.Code == 0, R.Text());
  // as the reload after an edit of nixscp.ini would leave them
  Site->AgentPermissions = apAll;
  CHECK(!SettingsConfirmed(*Site));
  Answer(QStringLiteral("Deny"));
  R = Connect(QStringLiteral("tamper"), QStringLiteral("big 1"));
  CHECK_MSG((R.Code == 255) && R.Err.contains("did not accept"), R.Text());
  CHECK(AuditLog().contains("denied: settings not accepted"));
  // back as they were: fine again, without a question
  Site->AgentPermissions = apCommands;
  CHECK(SettingsConfirmed(*Site));
  R = Connect(QStringLiteral("tamper"), QStringLiteral("big 1"));
  CHECK(R.Code == 0);
  // everything that decides where the stored password goes counts too
  for (const std::function<void(SessionData &)> & Change : std::initializer_list<std::function<void(SessionData &)>>{
    [](SessionData & D) { D.HostName = QStringLiteral("evil.example"); },
    [](SessionData & D) { D.PortNumber = 2222; },
    [](SessionData & D) { D.UserName = QStringLiteral("other"); },
    [](SessionData & D) { D.ProxyMethod = pmCmd; D.ProxyLocalCommand = QStringLiteral("nc evil.example 22"); },
    [](SessionData & D) { D.Tunnel = true; D.TunnelHostName = QStringLiteral("evil.example"); },
    [](SessionData & D) { D.AgentAccess = amAllow + 1; },
    [](SessionData & D) { D.AgentAllow = QStringLiteral("*"); },
    [](SessionData & D) { D.AgentDeny = QStringLiteral("x"); },
    [](SessionData & D) { D.AgentRecord = !D.AgentRecord; },
    [](SessionData & D) { D.AgentName = QStringLiteral("other"); },
    [](SessionData & D) { D.PortForwards = QStringLiteral("x|1234|h|22"); },
    // or how far the connection can be trusted
    [](SessionData & D) { D.TunnelHostKey = QStringLiteral("ssh-ed25519 256 SHA256:evil"); },
    [](SessionData & D) { D.HostKey = QStringLiteral("ssh-ed25519 256 SHA256:evil"); },
    [](SessionData & D) { D.LogicalHostName = QStringLiteral("trusted.example"); },
    [](SessionData & D) { D.CipherList = QStringLiteral("des,WARN,aes"); },
    [](SessionData & D) { D.KexList = QStringLiteral("dh-group1-sha1,WARN"); },
    [](SessionData & D) { D.HostKeyList = QStringLiteral("rsa,WARN"); },
    [](SessionData & D) { D.Ssh2DES = !D.Ssh2DES; },
    [](SessionData & D) { D.SshNoUserAuth = !D.SshNoUserAuth; },
    [](SessionData & D) { D.AgentFwd = !D.AgentFwd; },
    [](SessionData & D) { D.TryAgent = !D.TryAgent; },
    [](SessionData & D) { D.SourceAddress = QStringLiteral("192.0.2.1"); },
    [](SessionData & D) { D.ProxyDNS = (D.ProxyDNS == asOn) ? asOff : asOn; },
    [](SessionData & D) { D.ProxyLocalhost = !D.ProxyLocalhost; },
    [](SessionData & D) { D.TunnelLocalPortNumber = 4444; },
    [](SessionData & D) { D.SftpServer = QStringLiteral("/tmp/evil"); },
    [](SessionData & D) { D.Shell = QStringLiteral("/tmp/evil"); }})
  {
    SessionData Copy = *Site;
    Change(Copy);
    CHECK(!SettingsConfirmed(Copy));
    // what the user is asked about shows it
    CHECK_MSG(SettingsText(Copy) != SettingsText(*Site), SettingsText(Copy));
  }
  TestEverySettingCounts(*Site);
  // the seal of one site says nothing about another
  SessionData Other = *Site;
  Other.Name = QStringLiteral("tamper2");
  CHECK(!SettingsConfirmed(Other));
  // a seal only a master password makes: plain obfuscation is forged easily
  QFile Seals(QFileInfo(AuditLogFileName()).absolutePath() + QStringLiteral("/nixscp-agent-seals"));
  CHECK(Seals.open(QIODevice::ReadOnly));
  QByteArray Original = Seals.readAll();
  Seals.close();
  CHECK((Seals.permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther)) == 0);
  QByteArray Key = QCryptographicHash::hash(Site->Name.toUtf8(), QCryptographicHash::Sha256).toHex();
  CHECK(Seals.open(QIODevice::WriteOnly));
  Seals.write(Key + ' ' + "A35C4B6E6A1F\n");
  Seals.close();
  CHECK(!SettingsConfirmed(*Site));
  CHECK(Seals.open(QIODevice::WriteOnly));
  Seals.write(Original);
  Seals.close();
  CHECK(SettingsConfirmed(*Site));
  // what the user accepts takes effect
  Site->AgentPermissions = apCommands | apTunnels;
  Answer(QStringLiteral("Accept these settings"));
  R = Connect(QStringLiteral("tamper"), QStringLiteral("big 1"));
  CHECK_MSG(R.Code == 0, R.Text());
  CHECK(SettingsConfirmed(*Site));
  CHECK(AuditLog().contains("accepted by user"));
  // a new master password: all seals are void
  CHECK(Configuration::Instance().ChangeMasterPassword(QStringLiteral("another master password")));
  CHECK(!SettingsConfirmed(*Site));
  for (const auto & Data : Configuration::Instance().StoredSessions().Sessions())
  {
    Data->KnownHostsFile = GKnownHosts;
    if (UsableByAgents(*Data))
    {
      CHECK(ConfirmSettings(*Data));
    }
  }
}

// L2, L9, X-M14, L7, L24
static void TestAccess(const QString & Folder)
{
  // a mode this version does not know
  auto Odd = AddSite(QStringLiteral("mode3"), amAsk + 1, apAll);
  Result R = Agent({QStringLiteral("--list")});
  CHECK_MSG((R.Code == 0) && !R.Out.contains("mode3"), R.Text());
  R = Connect(QStringLiteral("mode3"), QStringLiteral("big 1"));
  CHECK_MSG((R.Code == 255) && R.Err.contains("not available to agents"), R.Text());

  auto Commands = AddSite(QStringLiteral("cmd-only"), amAllow, apCommands);
  R = Agent({QStringLiteral("--tunnel"), QStringLiteral("cmd-only"), QStringLiteral("22990:localhost:3")});
  CHECK_MSG((R.Code == 255) && R.Err.contains("does not allow tunnels"), R.Text());
  R = Agent({QStringLiteral("--get"), QStringLiteral("cmd-only"), QStringLiteral("/etc/hostname"), Folder + QStringLiteral("/x")});
  CHECK_MSG((R.Code == 255) && R.Err.contains("does not allow file transfers"), R.Text());
  auto Files = AddSite(QStringLiteral("files-only"), amAllow, apTransfer);
  R = Connect(QStringLiteral("files-only"), QStringLiteral("big 1"));
  CHECK_MSG((R.Code == 255) && R.Err.contains("does not allow commands"), R.Text());

  // two sites under one name: neither is reached
  auto First = AddSite(QStringLiteral("twin-a"), amAllow, apCommands);
  First->AgentName = QStringLiteral("twin");
  auto Second = AddSite(QStringLiteral("twin-b"), amAllow, apCommands);
  Second->AgentName = QStringLiteral("twin");
  CHECK(ConfirmSettings(*First) && ConfirmSettings(*Second));
  R = Connect(QStringLiteral("twin"), QStringLiteral("big 1"));
  CHECK_MSG((R.Code == 255) && R.Err.contains("More than one site"), R.Text());
  Second->AgentName = QStringLiteral("twin-2");
  CHECK(ConfirmSettings(*Second));
  R = Connect(QStringLiteral("twin"), QStringLiteral("big 1"));
  CHECK_MSG(R.Code == 0, R.Text());

  // no request without its line in the log
  QString Log = AuditLogFileName();
  QByteArray Saved = AuditLog();
  CHECK(QFile::remove(Log));
  CHECK(QDir().mkdir(Log));
  R = Connect(QStringLiteral("cmd-only"), QStringLiteral("big 1"));
  CHECK_MSG((R.Code == 255) && R.Err.contains("log cannot be written") && R.Out.isEmpty(), R.Text());
  R = Agent({QStringLiteral("--list")});
  CHECK_MSG((R.Code == 255) && R.Err.contains("log cannot be written"), R.Text());
  CHECK(QDir().rmdir(Log));
  QFile Restored(Log);
  CHECK(Restored.open(QIODevice::WriteOnly) && (Restored.write(Saved) == Saved.size()));
  Restored.close();
  CHECK(Restored.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner));
  R = Connect(QStringLiteral("cmd-only"), QStringLiteral("big 1"));
  CHECK_MSG(R.Code == 0, R.Text());

  // rules reach the broker
  auto Rules = AddSite(QStringLiteral("rules"), amAllow, apCommands, false);
  Rules->AgentAllow = QStringLiteral("big *");
  CHECK(ConfirmSettings(*Rules));
  R = Connect(QStringLiteral("rules"), QStringLiteral("big 2"));
  CHECK_MSG((R.Code == 0) && (R.Out.size() == 2), R.Text());
  R = Connect(QStringLiteral("rules"), QStringLiteral("big 2 # '\ncat\n#'"));
  CHECK_MSG((R.Code == 255) && R.Err.contains("Comments"), R.Text());
  Rules->AgentDeny = QStringLiteral("big [");
  CHECK(ConfirmSettings(*Rules));
  R = Connect(QStringLiteral("rules"), QStringLiteral("big 2"));
  CHECK_MSG((R.Code == 255) && R.Err.contains("not valid"), R.Text());
}

static QProcess * OpenTunnel(const QString & Site, const QString & Forward)
{
  auto * Tunnel = new QProcess();
  Tunnel->start(GClient, {QStringLiteral("--tunnel"), Site, Forward});
  CHECK(Tunnel->waitForStarted(10000));
  QByteArray Err;
  CHECK_MSG(WaitFor([&]() { Err += Tunnel->readAllStandardError(); return Err.contains("Tunnel open") || (Tunnel->state() == QProcess::NotRunning); }, 20000) &&
    Err.contains("Tunnel open"), QString::fromUtf8(Err));
  return Tunnel;
}

// L10, L24
static void TestTunnels(int LocalBase)
{
  auto Site = AddSite(QStringLiteral("tunnels"), amAllow, apCommands | apTunnels, false);
  Site->PortForwards = QStringLiteral("db:1|%1|127.0.0.1|1").arg(LocalBase);
  CHECK(ConfirmSettings(*Site));
  // a saved tunnel by a name with a colon
  {
    std::unique_ptr<QProcess> Tunnel(OpenTunnel(QStringLiteral("tunnels"), QStringLiteral("db:1")));
    QTcpSocket Socket;
    Socket.connectToHost(QStringLiteral("127.0.0.1"), static_cast<quint16>(LocalBase));
    QByteArray Banner;
    WaitFor([&]() { Banner += Socket.readAll(); return Banner.contains("\n"); }, 10000);
    CHECK_MSG(Banner == "BANNER\r\n", QString::fromLatin1(Banner));
    Socket.close();
    Tunnel->kill();
    Tunnel->waitForFinished(5000);
  }
  CHECK(WaitFor([]() { return AgentBroker::ActiveJobs() == 0; }, 10000));
  // tunnels count in the limit
  QList<QProcess *> Tunnels;
  for (int I = 0; I < 8; I++)
  {
    Tunnels.append(OpenTunnel(QStringLiteral("tunnels"), QStringLiteral("%1:127.0.0.1:3").arg(LocalBase + 1 + I)));
  }
  CHECK(AgentBroker::ActiveJobs() == 8);
  Result R = Connect(QStringLiteral("tunnels"), QStringLiteral("big 1"));
  CHECK_MSG((R.Code == 255) && R.Err.contains("Too many"), R.Text());
  for (QProcess * Tunnel : Tunnels)
  {
    Tunnel->kill();
    Tunnel->waitForFinished(5000);
    delete Tunnel;
  }
  CHECK(WaitFor([]() { return AgentBroker::ActiveJobs() == 0; }, 10000));
  R = Connect(QStringLiteral("tunnels"), QStringLiteral("big 1"));
  CHECK_MSG(R.Code == 0, R.Text());
}

static QPushButton * DialogButton(QDialog * Dialog, const QString & Text)
{
  for (QPushButton * Button : Dialog->findChildren<QPushButton *>())
  {
    if (Button->text().remove(QLatin1Char('&')) == Text)
    {
      return Button;
    }
  }
  return nullptr;
}

// L19, L20, L21, and a name that used to damage the saved tunnel (L18)
static void TestTunnelsDialog(int LocalBase)
{
  QString Name = QStringLiteral("100%2|db");
  auto Site = AddSite(QStringLiteral("dialog"), amOff, 0, false);
  PortForwardSpec Spec;
  Spec.Name = Name;
  Spec.LocalPort = LocalBase + 10;
  Spec.Host = QStringLiteral("127.0.0.1");
  // refused by the server: a failed connection on a running tunnel
  Spec.Port = 4;
  Site->PortForwards = PortForwardSpec::SaveAll({Spec});
  QString Key = TunnelManager::Key(Site->Name, Name);
  TunnelsDialog Dialog;
  Dialog.show();
  auto * Table = Dialog.findChild<QTableWidget *>();
  CHECK(Table != nullptr);
  int Row = -1;
  for (int R = 0; R < Table->rowCount(); R++)
  {
    if (Table->item(R, 0)->text() == Site->Name)
    {
      Row = R;
    }
  }
  CHECK(Row >= 0);
  CHECK(Table->item(Row, 1)->text() == Name);
  Table->selectRow(Row);
  DialogButton(&Dialog, QStringLiteral("Start"))->click();
  auto Status = [Table, Row]() { return Table->item(Row, 4)->text(); };
  CHECK_MSG(WaitFor([&]() { return Status().startsWith(QStringLiteral("Running")); }, 20000), Status());
  QTcpSocket Socket;
  Socket.connectToHost(QStringLiteral("127.0.0.1"), static_cast<quint16>(LocalBase + 10));
  CHECK_MSG(WaitFor([&]() { return Status().startsWith(QStringLiteral("Running; ")); }, 10000), Status());
  Table->selectRow(Row);

  // a double click on a running tunnel: as the disabled Edit button, nothing
  CHECK(!DialogButton(&Dialog, QStringLiteral("Edit..."))->isEnabled());
  bool EditOpened = false;
  QTimer Watch;
  QObject::connect(&Watch, &QTimer::timeout, &Dialog, [&EditOpened]()
  {
    if (auto * Modal = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
    {
      EditOpened = true;
      Modal->reject();
    }
  });
  Watch.start(20);
  emit Table->cellDoubleClicked(Row, 0);
  Pump(200);
  Watch.stop();
  CHECK(!EditOpened);

  // stopped as asked: no "Failed" left of a connection long gone
  DialogButton(&Dialog, QStringLiteral("Stop"))->click();
  CHECK(WaitFor([&Key]() { return TunnelManager::Instance().Find(Key) == nullptr; }, 10000));
  Pump(100);
  CHECK_MSG(Status() == QStringLiteral("Stopped"), Status());
  CHECK(TunnelManager::Instance().LastError(Key).isEmpty());

  // the site went away behind the dialog (a sync, another window)
  Table->selectRow(Row);
  CHECK(DialogButton(&Dialog, QStringLiteral("Edit..."))->isEnabled());
  Configuration::Instance().StoredSessions().Remove(Site->Name);
  DialogButton(&Dialog, QStringLiteral("Edit..."))->click();
  CHECK(Dialog.isVisible());
  Dialog.close();
}

// What the Agent Access dialog refuses, says and stores (with a master password only)
static void TestAccessDialog()
{
  SessionData Data;
  Data.Name = QStringLiteral("dialog-site");
  Data.HostName = QStringLiteral("example.test");
  Data.UserName = QStringLiteral("u");
  Data.FSProtocol = fsSFTP;
  Data.AgentAccess = amAllow;
  Data.AgentRecord = true;
  AgentAccessDialog Dialog(Data);
  QStringList Texts;
  for (QLabel * Label : Dialog.findChildren<QLabel *>())
  {
    Texts.append(Label->text());
  }
  QString Text = Texts.join(QLatin1Char('\n'));
  // S27: what the log and the recordings really are
  CHECK_MSG(Text.contains(QStringLiteral("Sessions will not be recorded")), Text);
  CHECK_MSG(Text.contains(QStringLiteral("a secret typed into a command does land there")), Text);
  CHECK_MSG(Text.contains(QStringLiteral("not a security boundary")), Text);

  // L1: a pattern that cannot work is not stored
  QList<QPlainTextEdit *> Edits = Dialog.findChildren<QPlainTextEdit *>();
  CHECK(Edits.size() == 2);
  Edits.at(1)->setPlainText(QStringLiteral("rm ["));
  bool Warned = false;
  QTimer::singleShot(100, &Dialog, [&Warned]()
  {
    if (QWidget * Modal = QApplication::activeModalWidget())
    {
      Warned = true;
      Modal->close();
    }
  });
  Dialog.accept();
  CHECK(Warned);
  CHECK(Dialog.result() != QDialog::Accepted);
  CHECK(Data.AgentDeny.isEmpty());

  // the preset leaves no tunnels: they reach the services of the server past any rule
  QPushButton * Preset = DialogButton(&Dialog, QStringLiteral("Read-only preset"));
  CHECK(Preset != nullptr);
  Preset->click();
  Dialog.accept();
  CHECK(Dialog.result() == QDialog::Accepted);
  CHECK(Data.AgentAllow == ReadOnlyAllowPreset());
  CHECK(Data.AgentDeny == ReadOnlyDenyPreset());
  CHECK(!(Data.AgentPermissions & apTunnels));
  CHECK(Data.AgentPermissions & apDownloadOnly);
  // set here by the user: no question later
  CHECK(SettingsConfirmed(Data));
}

// S27
static void TestRecordings()
{
  auto Site = AddSite(QStringLiteral("recorded"), amAllow, apCommands);
  CHECK(Site->AgentRecord);
  // a master password only: nothing on disk, and the log says so
  QDir Recordings(RecordingDirectory());
  CHECK(!CanRecord());
  Result R = Connect(QStringLiteral("recorded"), QStringLiteral("big 4"));
  CHECK_MSG(R.Code == 0, R.Text());
  CHECK(Recordings.entryList({QStringLiteral("*.log")}, QDir::Files).isEmpty());
  CHECK(AuditLog().contains("recorded\texec\tallowed; not recorded: recordings need the NixSCP password\tbig 4"));
  // with the NixSCP key: encrypted, and the settings need no seal any more
  QByteArray Dek(CloudCrypto::KeyLength, 'k');
  CloudCrypto::SnapshotHeader Wrap;
  Wrap.User = QStringLiteral("agent@nixscp.test");
  Wrap.DekPassword = QByteArray(48, 'w');
  ConfigVault::SetKey(Dek, Wrap);
  CHECK(CanRecord());
  R = Connect(QStringLiteral("recorded"), QStringLiteral("big 4"));
  CHECK_MSG(R.Code == 0, R.Text());
  CHECK(WaitFor([&Recordings]() { return !Recordings.entryList({QStringLiteral("*.log")}, QDir::Files).isEmpty(); }, 5000));
  QStringList Files = Recordings.entryList({QStringLiteral("*.log")}, QDir::Files);
  CHECK(Files.size() == 1);
  if (!Files.isEmpty())
  {
    QFile Raw(Recordings.filePath(Files.first()));
    CHECK(Raw.open(QIODevice::ReadOnly));
    QByteArray Bytes = Raw.readAll();
    CHECK(ConfigVault::IsSealed(Bytes) && !Bytes.contains("big 4"));
    CHECK(ConfigVault::ReadFile(Recordings.filePath(Files.first())).contains("Command: big 4"));
  }
  CHECK(AuditLog().contains("recorded\texec\tallowed\tbig 4"));
  ConfigVault::Forget();
  // no stored password in anything that was written
  CHECK(!AuditLog().contains("stored site password"));
}

// Site notes: read and replaced by agents, only with their own permissions
static void TestNotes(const QString & Folder)
{
  auto Plain = AddSite(QStringLiteral("note-none"), amAllow, apCommands);
  Plain->Note = QStringLiteral("secret plans");
  Result R = Agent({QStringLiteral("--note"), QStringLiteral("note-none")});
  CHECK_MSG((R.Code == 255) && R.Err.contains("does not allow reading its note") && R.Out.isEmpty(), R.Text());

  auto Read = AddSite(QStringLiteral("note-read"), amAllow, apNoteRead);
  Read->Note = QStringLiteral("Logs: /var/log/shop\nDo not restart MySQL in the daytime.");
  R = Agent({QStringLiteral("--note"), QStringLiteral("note-read")});
  CHECK_MSG((R.Code == 0) && (R.Out == Read->Note.toUtf8() + "\n"), R.Text());
  R = Agent({QStringLiteral("--note-set"), QStringLiteral("note-read")}, "overwritten");
  CHECK_MSG((R.Code == 255) && R.Err.contains("does not allow editing its note"), R.Text());
  CHECK(Read->Note.startsWith(QStringLiteral("Logs:")));
  // the note alone: nothing else on the server
  R = Connect(QStringLiteral("note-read"), QStringLiteral("big 1"));
  CHECK_MSG((R.Code == 255) && R.Err.contains("does not allow commands"), R.Text());
  R = Agent({QStringLiteral("--list")});
  CHECK_MSG((R.Code == 0) && R.Out.contains("note-read") && R.Out.contains("note"), R.Text());

  auto Write = AddSite(QStringLiteral("note-write"), amAllow, apNoteRead | apNoteWrite);
  QByteArray Text = "Checked on 2026-11-12: nginx 1.26, disk 41% used.\n\xc3\x9cmlauts and tabs\tstay.";
  R = Agent({QStringLiteral("--note-set"), QStringLiteral("note-write")}, Text);
  CHECK_MSG(R.Code == 0, R.Text());
  CHECK(Write->Note == QString::fromUtf8(Text));
  R = Agent({QStringLiteral("--note"), QStringLiteral("note-write")});
  CHECK_MSG((R.Code == 0) && (R.Out == Text + "\n"), R.Text());
  // from a file
  QString File = Folder + QStringLiteral("/note.txt");
  QFile Out(File);
  CHECK(Out.open(QIODevice::WriteOnly) && (Out.write("from a file\n") > 0));
  Out.close();
  R = Agent({QStringLiteral("--note-set"), QStringLiteral("note-write"), File});
  CHECK_MSG((R.Code == 0) && (Write->Note == QStringLiteral("from a file\n")), R.Text());
  // the log has the size, not the text
  CHECK(AuditLog().contains("note-set") && AuditLog().contains("12 bytes") && !AuditLog().contains("from a file"));
  // limits: size and control characters
  R = Agent({QStringLiteral("--note-set"), QStringLiteral("note-write")}, QByteArray(MaxNoteBytes + 1, 'x'));
  CHECK_MSG((R.Code != 0) && R.Err.contains("too long"), R.Text());
  R = Agent({QStringLiteral("--note-set"), QStringLiteral("note-write")}, QByteArray(MaxNoteBytes, 'x'));
  CHECK_MSG((R.Code == 0) && (Write->Note.size() == MaxNoteBytes), R.Text());
  R = Agent({QStringLiteral("--note-set"), QStringLiteral("note-write")}, QByteArray("bell\x07here"));
  CHECK_MSG((R.Code == 255) && R.Err.contains("control characters") && (Write->Note.size() == MaxNoteBytes), R.Text());
  // an empty note clears it
  R = Agent({QStringLiteral("--note-set"), QStringLiteral("note-write")}, QByteArray());
  CHECK_MSG((R.Code == 0) && Write->Note.isEmpty(), R.Text());
  R = Agent({QStringLiteral("--note"), QStringLiteral("note-write")});
  CHECK_MSG((R.Code == 0) && R.Out.isEmpty(), R.Text());

  // asking: Deny keeps the old note
  auto Asked = AddSite(QStringLiteral("note-ask"), amAsk, apNoteRead | apNoteWrite);
  Asked->Note = QStringLiteral("old");
  Answer(QStringLiteral("Deny"));
  R = Agent({QStringLiteral("--note-set"), QStringLiteral("note-ask")}, "new");
  CHECK_MSG((R.Code == 255) && R.Err.contains("denied") && (Asked->Note == QStringLiteral("old")), R.Text());
  Answer(QStringLiteral("Allow once"));
  R = Agent({QStringLiteral("--note-set"), QStringLiteral("note-ask")}, "new");
  CHECK_MSG((R.Code == 0) && (Asked->Note == QStringLiteral("new")), R.Text());

  // a note item: no server behind it, whatever its permissions say
  auto Item = AddSite(QStringLiteral("office-note"), amAllow, apAll | apNoteRead | apNoteWrite, false);
  Item->NoteOnly = true;
  Item->HostName.clear();
  Item->Note = QStringLiteral("Office VPN: vpn.example.com");
  CHECK(ConfirmSettings(*Item));
  R = Agent({QStringLiteral("--note"), QStringLiteral("office-note")});
  CHECK_MSG((R.Code == 0) && R.Out.startsWith("Office VPN"), R.Text());
  R = Connect(QStringLiteral("office-note"), QStringLiteral("big 1"));
  CHECK_MSG((R.Code == 255) && R.Err.contains("is a note, not a server"), R.Text());
  R = Agent({QStringLiteral("--list")});
  QByteArray Line;
  for (const QByteArray & L : R.Out.split('\n')) if (L.startsWith("office-note\t")) Line = L;
  CHECK_MSG(Line.contains("(note)") && Line.contains("note-edit") && !Line.contains("commands") && !Line.contains("files"), R.Text());
  R = Agent({QStringLiteral("--note-set"), QStringLiteral("office-note")}, "Office VPN: vpn2.example.com");
  CHECK_MSG((R.Code == 0) && (Item->Note == QStringLiteral("Office VPN: vpn2.example.com")), R.Text());

  // the same through MCP
  QProcess Mcp;
  Mcp.start(GClient, {QStringLiteral("--mcp")});
  CHECK(Mcp.waitForStarted(10000));
  QByteArray Buffer;
  auto Call = [&](int Id, const QString & Tool, const QJsonObject & Arguments) -> QJsonObject
  {
    Mcp.write(QJsonDocument(QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("id"), Id},
      {QStringLiteral("method"), QStringLiteral("tools/call")},
      {QStringLiteral("params"), QJsonObject{{QStringLiteral("name"), Tool}, {QStringLiteral("arguments"), Arguments}}}}).toJson(QJsonDocument::Compact) + '\n');
    QJsonObject Reply;
    WaitFor([&]()
    {
      Buffer += Mcp.readAllStandardOutput();
      qsizetype End = Buffer.indexOf('\n');
      if (End < 0) return false;
      Reply = QJsonDocument::fromJson(Buffer.left(End)).object().value(QStringLiteral("result")).toObject();
      Buffer.remove(0, End + 1);
      return true;
    }, 20000);
    return Reply;
  };
  Mcp.write("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-03-26\"}}\n");
  WaitFor([&]() { Buffer += Mcp.readAllStandardOutput(); return Buffer.contains('\n'); }, 10000);
  Buffer.clear();
  QJsonObject Got = Call(2, QStringLiteral("read_note"), QJsonObject{{QStringLiteral("site"), QStringLiteral("note-read")}});
  CHECK_MSG(!Got.value(QStringLiteral("isError")).toBool() &&
    Got.value(QStringLiteral("structuredContent")).toObject().value(QStringLiteral("note")).toString().startsWith(QStringLiteral("Logs:")),
    QString::fromUtf8(QJsonDocument(Got).toJson()));
  Got = Call(3, QStringLiteral("write_note"), QJsonObject{{QStringLiteral("site"), QStringLiteral("note-read")}, {QStringLiteral("text"), QStringLiteral("x")}});
  CHECK(Got.value(QStringLiteral("isError")).toBool());
  Got = Call(4, QStringLiteral("write_note"), QJsonObject{{QStringLiteral("site"), QStringLiteral("note-write")}, {QStringLiteral("text"), QStringLiteral("by MCP")}});
  CHECK_MSG(!Got.value(QStringLiteral("isError")).toBool() && (Write->Note == QStringLiteral("by MCP")), QString::fromUtf8(QJsonDocument(Got).toJson()));
  Mcp.closeWriteChannel();
  CHECK(Mcp.waitForFinished(10000));
}

// X-M11, L15, L16
static void TestMcp()
{
  auto Site = AddSite(QStringLiteral("mcp"), amAllow, apCommands);
  QProcess Mcp;
  Mcp.start(GClient, {QStringLiteral("--mcp")});
  CHECK(Mcp.waitForStarted(10000));
  QByteArray Buffer;
  auto Send = [&Mcp](const QByteArray & Line) { Mcp.write(Line + '\n'); };
  auto Receive = [&](int TimeoutMs) -> QJsonDocument
  {
    QJsonDocument Doc;
    WaitFor([&]()
    {
      Buffer += Mcp.readAllStandardOutput();
      qsizetype End = Buffer.indexOf('\n');
      if (End >= 0)
      {
        Doc = QJsonDocument::fromJson(Buffer.left(End));
        Buffer.remove(0, End + 1);
        return true;
      }
      return false;
    }, TimeoutMs);
    return Doc;
  };
  auto Call = [](int Id, const QJsonObject & Arguments)
  {
    return QJsonDocument(QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("id"), Id},
      {QStringLiteral("method"), QStringLiteral("tools/call")},
      {QStringLiteral("params"), QJsonObject{{QStringLiteral("name"), QStringLiteral("run_command")}, {QStringLiteral("arguments"), Arguments}}}}).toJson(QJsonDocument::Compact);
  };
  Send("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-03-26\"}}");
  CHECK(Receive(10000).object().value(QStringLiteral("result")).toObject().value(QStringLiteral("protocolVersion")).toString() == QStringLiteral("2025-03-26"));

  // batches, with everything in one reply
  Send("[{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"ping\"},{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"},{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"nope\"},7]");
  QJsonDocument Reply = Receive(10000);
  CHECK(Reply.isArray() && (Reply.array().size() == 3));
  CHECK(Reply.array().at(0).toObject().value(QStringLiteral("id")).toInt() == 2);
  CHECK(Reply.array().at(1).toObject().value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toInt() == -32601);
  CHECK(Reply.array().at(2).toObject().value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toInt() == -32600);
  // a tool call in a batch: the reply waits for it
  Send("[" + Call(4, QJsonObject{{QStringLiteral("site"), QStringLiteral("mcp")}, {QStringLiteral("command"), QStringLiteral("big 5")}}) + ",{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"ping\"}]");
  Reply = Receive(20000);
  CHECK(Reply.isArray() && (Reply.array().size() == 2));
  CHECK(Reply.array().at(0).toObject().value(QStringLiteral("id")).toInt() == 5);
  CHECK(Reply.array().at(1).toObject().value(QStringLiteral("result")).toObject().value(QStringLiteral("structuredContent")).toObject().value(QStringLiteral("stdout")).toString().size() == 5);
  // empty batch, scalar, garbage
  Send("[]");
  CHECK(Receive(10000).object().value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toInt() == -32600);
  Send("42");
  CHECK(Receive(10000).object().contains(QStringLiteral("error")));
  Send("{not json");
  CHECK(Receive(10000).object().value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toInt() == -32700);

  // stdin larger than a frame
  QString Input(3 * 1024 * 1024 + 11, QLatin1Char('m'));
  Send(Call(6, QJsonObject{{QStringLiteral("site"), QStringLiteral("mcp")}, {QStringLiteral("command"), QStringLiteral("count 0")}, {QStringLiteral("stdin"), Input}}));
  Reply = Receive(30000);
  QJsonObject Content = Reply.object().value(QStringLiteral("result")).toObject();
  CHECK_MSG(!Content.value(QStringLiteral("isError")).toBool() &&
    (Content.value(QStringLiteral("structuredContent")).toObject().value(QStringLiteral("stdout")).toString().trimmed() == QString::number(Input.size())),
    QString::fromUtf8(Reply.toJson(QJsonDocument::Compact).left(300)));

  // ping is answered during a long call, and the call can be cancelled
  QElapsedTimer Timer;
  Timer.start();
  Send(Call(7, QJsonObject{{QStringLiteral("site"), QStringLiteral("mcp")}, {QStringLiteral("command"), QStringLiteral("quiet 30 0")}}));
  Pump(500);
  CHECK(AgentBroker::ActiveJobs() == 1);
  Send("{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"ping\"}");
  Reply = Receive(3000);
  CHECK_MSG(Reply.object().value(QStringLiteral("id")).toInt() == 8, QString::fromUtf8(Reply.toJson(QJsonDocument::Compact)));
  CHECK(Timer.elapsed() < 5000);
  // calls wait for their turn; one cancelled before its turn never runs
  Send(Call(9, QJsonObject{{QStringLiteral("site"), QStringLiteral("mcp")}, {QStringLiteral("command"), QStringLiteral("big 9")}}));
  Send("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\",\"params\":{\"requestId\":9}}");
  Send("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\",\"params\":{\"requestId\":7}}");
  CHECK(WaitFor([]() { return AgentBroker::ActiveJobs() == 0; }, 5000));
  CHECK(Timer.elapsed() < 15000);
  Send("{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"ping\"}");
  Reply = Receive(5000);
  // no reply for 7 or 9
  CHECK_MSG(Reply.object().value(QStringLiteral("id")).toInt() == 10, QString::fromUtf8(Reply.toJson(QJsonDocument::Compact)));
  // a piped client gets its answers before the server exits
  Send(Call(11, QJsonObject{{QStringLiteral("site"), QStringLiteral("mcp")}, {QStringLiteral("command"), QStringLiteral("big 2")}}));
  Mcp.closeWriteChannel();
  Reply = Receive(20000);
  CHECK(Reply.object().value(QStringLiteral("id")).toInt() == 11);
  CHECK(WaitFor([&Mcp]() { return Mcp.state() == QProcess::NotRunning; }, 10000) && (Mcp.exitCode() == 0));
}

// X-H4, X-M15, against OpenSSH
static void TestFiles(const QString & Folder, const QString & OpenSshKnownHosts)
{
  auto Refused = AddSite(QStringLiteral("no-files"), amAllow, apCommands);
  QString Existing = Folder + QStringLiteral("/existing.conf");
  auto Write = [&Existing](const QByteArray & Data)
  {
    QFile F(Existing);
    CHECK(F.open(QIODevice::WriteOnly) && (F.write(Data) == Data.size()));
  };
  auto Read = [](const QString & Name)
  {
    QFile F(Name);
    return F.open(QIODevice::ReadOnly) ? F.readAll() : QByteArray("(missing)");
  };
  Write("precious\n");
  // refused: the file stays (it used to be emptied before the request, then deleted)
  Result R = Agent({QStringLiteral("--get"), QStringLiteral("no-files"), QStringLiteral("/etc/hostname"), Existing});
  CHECK_MSG((R.Code == 255) && (Read(Existing) == "precious\n"), R.Text());
  // the same through MCP
  QProcess Mcp;
  Mcp.start(GClient, {QStringLiteral("--mcp")});
  CHECK(Mcp.waitForStarted(10000));
  QJsonObject Call{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("id"), 1}, {QStringLiteral("method"), QStringLiteral("tools/call")},
    {QStringLiteral("params"), QJsonObject{{QStringLiteral("name"), QStringLiteral("download_file")}, {QStringLiteral("arguments"),
      QJsonObject{{QStringLiteral("site"), QStringLiteral("no-files")}, {QStringLiteral("remote_path"), QStringLiteral("/etc/hostname")}, {QStringLiteral("local_path"), Existing}}}}}};
  Mcp.write(QJsonDocument(Call).toJson(QJsonDocument::Compact) + '\n');
  QByteArray Out;
  CHECK(WaitFor([&]() { Out += Mcp.readAllStandardOutput(); return Out.contains('\n'); }, 15000));
  CHECK_MSG(Out.contains("does not allow"), QString::fromUtf8(Out));
  CHECK(Read(Existing) == "precious\n");
  Mcp.closeWriteChannel();
  CHECK(Mcp.waitForFinished(10000));

  if (OpenSshKnownHosts.isEmpty())
  {
    std::printf("SKIP file transfers: set NIXSCP_TEST_SSH_PORT and NIXSCP_TEST_SSH_KEY\n");
    return;
  }
  auto Site = AddSite(QStringLiteral("openssh"), amAllow, apAll, false);
  Site->PortNumber = qEnvironmentVariableIntValue("NIXSCP_TEST_SSH_PORT");
  Site->UserName = qEnvironmentVariable("USER", QStringLiteral("user"));
  Site->Password.clear();
  Site->PublicKeyFile = qEnvironmentVariable("NIXSCP_TEST_SSH_KEY");
  Site->KnownHostsFile = OpenSshKnownHosts;
  CHECK(ConfirmSettings(*Site));
  QByteArray Content(5 * 1024 * 1024 + 7, Qt::Uninitialized);
  for (char & C : Content)
  {
    C = static_cast<char>(QRandomGenerator::global()->bounded(256));
  }
  QString Upload = Folder + QStringLiteral("/upload.bin");
  {
    QFile F(Upload);
    CHECK(F.open(QIODevice::WriteOnly) && (F.write(Content) == Content.size()));
  }
  QString RemoteFile = QStringLiteral("/tmp/nixscp-agentbroker-%1.bin").arg(QCoreApplication::applicationPid());
  R = Agent({QStringLiteral("--put"), QStringLiteral("openssh"), Upload, RemoteFile});
  CHECK_MSG(R.Code == 0, R.Text());

  // a real command, with real stdin
#ifdef Q_OS_MACOS
  R = Connect(QStringLiteral("openssh"), QStringLiteral("md5"), Content);
#else
  R = Connect(QStringLiteral("openssh"), QStringLiteral("md5sum"), Content);
#endif
  CHECK_MSG((R.Code == 0) && R.Out.startsWith(QCryptographicHash::hash(Content, QCryptographicHash::Md5).toHex()), R.Text());
  R = Connect(QStringLiteral("openssh"), QStringLiteral("echo out; echo err >&2; exit 9"));
  CHECK_MSG((R.Code == 9) && (R.Out == "out\n") && (R.Err == "err\n"), R.Text());
  R = Connect(QStringLiteral("openssh"), QStringLiteral("head -c 25000000 /dev/zero"));
  CHECK_MSG((R.Code == 0) && (R.Out.size() == 25000000), R.Text());
  // a program that stops reading its input ends normally, with its own code
  R = Connect(QStringLiteral("openssh"), QStringLiteral("head -c 10 > /dev/null; exit 5"), QByteArray(8 * 1024 * 1024, 'x'));
  CHECK_MSG(R.Code == 5, R.Text());

  // download: replaced only when complete
  R = Agent({QStringLiteral("--get"), QStringLiteral("openssh"), RemoteFile, Existing});
  CHECK_MSG(R.Code == 0, R.Text());
  CHECK(Read(Existing) == Content);
  Write("precious\n");
  // a remote file that is not there: the local one stays
  R = Agent({QStringLiteral("--get"), QStringLiteral("openssh"), QStringLiteral("/nonexistent/file"), Existing});
  CHECK_MSG((R.Code == 255) && (Read(Existing) == "precious\n"), R.Text());
  R = Agent({QStringLiteral("--get"), QStringLiteral("openssh"), QStringLiteral("/nonexistent/file"), Folder + QStringLiteral("/none.bin")});
  CHECK((R.Code == 255) && !QFile::exists(Folder + QStringLiteral("/none.bin")));
  // a folder: refused, nothing written
  R = Agent({QStringLiteral("--get"), QStringLiteral("openssh"), QStringLiteral("/tmp"), Existing});
  CHECK_MSG((R.Code == 255) && (Read(Existing) == "precious\n"), R.Text());
  // X-M15: a full disk is an error
  R = Agent({QStringLiteral("--get"), QStringLiteral("openssh"), RemoteFile, QStringLiteral("/dev/full")});
  CHECK_MSG((R.Code == 255) && R.Err.contains("Cannot write"), R.Text());
  // a folder that cannot be written to: an error, and the file stays
  QString Locked = Folder + QStringLiteral("/locked");
  CHECK(QDir().mkdir(Locked));
  QString Inside = Locked + QStringLiteral("/file");
  {
    QFile F(Inside);
    CHECK(F.open(QIODevice::WriteOnly) && (F.write("kept") == 4));
  }
  QFile::setPermissions(Locked, QFileDevice::ReadOwner | QFileDevice::ExeOwner);
  R = Agent({QStringLiteral("--get"), QStringLiteral("openssh"), RemoteFile, Inside});
  CHECK_MSG((R.Code == 255) && (Read(Inside) == "kept"), R.Text());
  QFile::setPermissions(Locked, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
  // devices: straight through, no temporary file next to them
  R = Agent({QStringLiteral("--get"), QStringLiteral("openssh"), RemoteFile, QStringLiteral("/dev/null")});
  CHECK_MSG(R.Code == 0, R.Text());
  // nothing left behind
  QStringList Left = QDir(Folder).entryList({QStringLiteral("existing.conf*")}, QDir::Files);
  CHECK_MSG(Left.size() == 1, Left.join(QLatin1Char(' ')));
  R = Connect(QStringLiteral("openssh"), QStringLiteral("rm -f '%1'").arg(RemoteFile));
  CHECK(R.Code == 0);
}

// L11, L14: runs as a child: a second broker and what happens to it
static int BrokerChild(int argc, char ** argv, const QString & Folder)
{
  QApplication App(argc, argv);
  Configuration::Instance().SetIniFileName(Folder + QStringLiteral("/child-nixscp.ini"));
  Configuration::Instance().Load();
  AgentBroker::Start(nullptr);
  std::printf("up [%s]\n", qPrintable(AgentBroker::ListenError()));
  std::fflush(stdout);
  return App.exec();
}

static void TestTwoInstances(const QString & Folder)
{
  QString Runtime = Folder + QStringLiteral("/two");
  CHECK(QDir().mkpath(Runtime));
  QFile::setPermissions(Runtime, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
  QProcessEnvironment Environment = QProcessEnvironment::systemEnvironment();
  Environment.insert(QStringLiteral("XDG_RUNTIME_DIR"), Runtime);
  auto Start = [&](QProcess & P)
  {
    P.setProcessEnvironment(Environment);
    P.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--broker-child"), Folder});
    CHECK(P.waitForStarted(10000));
    QByteArray Out;
    CHECK(WaitFor([&]() { Out += P.readAllStandardOutput(); return Out.contains('\n'); }, 15000));
    return Out;
  };
  auto Client = [&]()
  {
    QProcess P;
    P.setProcessEnvironment(Environment);
    P.start(GClient, {QStringLiteral("--list")});
    QByteArray Err;
    WaitFor([&]() { Err += P.readAllStandardError(); return P.state() == QProcess::NotRunning; }, 20000);
    return Err + P.readAllStandardError();
  };
  QProcess First, Second;
  CHECK_MSG(Start(First).contains("up []"), QStringLiteral("first broker"));
  // the broker answered: it asks for a master password
  CHECK(Client().contains("master password"));
  // the second instance is quiet about it (that is no error) and leaves the first alone
  QByteArray Out = Start(Second);
  CHECK_MSG(Out.contains("up []"), QString::fromUtf8(Out));
  CHECK(Client().contains("master password"));
  First.kill();
  First.waitForFinished(5000);
  // a crash leaves socket and lock behind: the second takes over within seconds
  QByteArray Answer;
  CHECK_MSG(WaitFor([&]() { Answer = Client(); return Answer.contains("master password"); }, 15000), QString::fromUtf8(Answer));
  Second.kill();
  Second.waitForFinished(5000);

  // a socket that cannot exist: said, not silent
  QString Long = Folder + QLatin1Char('/') + QString(100, QLatin1Char('l'));
  CHECK(QDir().mkpath(Long));
  QFile::setPermissions(Long, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
  Environment.insert(QStringLiteral("XDG_RUNTIME_DIR"), Long);
  QProcess Failing;
  Out = Start(Failing);
  CHECK_MSG(Out.contains("Cannot listen on"), QString::fromUtf8(Out));
  Failing.kill();
  Failing.waitForFinished(5000);
}

#endif

int main(int argc, char ** argv)
{
#if (defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)) || defined(Q_OS_MACOS)
  TestSshServer::RunIfAsked(argc, argv);
  if ((argc == 3) && (qstrcmp(argv[1], "--broker-child") == 0))
  {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("HOME", argv[2]);
    return BrokerChild(argc, argv, QString::fromLocal8Bit(argv[2]));
  }
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
  {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  QTemporaryDir Dir;
  // the broker's socket and the client's lookup use this folder; libssh and
  // the client read ~/.ssh and the configuration otherwise
  qputenv("XDG_RUNTIME_DIR", Dir.path().toUtf8());
  qputenv("HOME", Dir.path().toUtf8());
  qputenv("XDG_CONFIG_HOME", (Dir.path() + QStringLiteral("/.config")).toUtf8());
  QApplication App(argc, argv);
#ifdef Q_OS_MACOS
  GClient = qEnvironmentVariable("NIXSCP_AGENT_CLIENT", QCoreApplication::applicationDirPath() + QStringLiteral("/NixSCP.app/Contents/MacOS/NixSCP"));
#else
  GClient = qEnvironmentVariable("NIXSCP_AGENT_CLIENT", QCoreApplication::applicationDirPath() + QStringLiteral("/nixscp"));
#endif
  if (!QFile::exists(GClient))
  {
    std::printf("SKIP: no nixscp executable at %s\n", qPrintable(GClient));
    return 0;
  }
  int Base = 22700 + static_cast<int>(QCoreApplication::applicationPid() % 40) * 2;
  int LocalBase = 22600 + static_cast<int>(QCoreApplication::applicationPid() % 8) * 12;
  QDir(Dir.path()).mkdir(QStringLiteral("server"));
  TestSshServer Server;
  CHECK(Server.Start(Dir.path() + QStringLiteral("/server"), Base));
  GPort = Base;
  GKnownHosts = Server.KnownHosts();

  QString OpenSshKnownHosts;
  QString Key = qEnvironmentVariable("NIXSCP_TEST_SSH_KEY");
  if (!qEnvironmentVariableIsEmpty("NIXSCP_TEST_SSH_PORT") && !Key.isEmpty())
  {
    OpenSshKnownHosts = Dir.path() + QStringLiteral("/openssh_known_hosts");
    QFile HostKey(QFileInfo(Key).dir().filePath(QStringLiteral("host_ed25519.pub")));
    if (!HostKey.exists())
    {
      // the server of the system
      HostKey.setFileName(QStringLiteral("/etc/ssh/ssh_host_ed25519_key.pub"));
    }
    QFile Hosts(OpenSshKnownHosts);
    CHECK(HostKey.open(QIODevice::ReadOnly) && Hosts.open(QIODevice::WriteOnly));
    Hosts.write(QStringLiteral("[127.0.0.1]:%1 ").arg(qEnvironmentVariableIntValue("NIXSCP_TEST_SSH_PORT")).toLatin1() + HostKey.readAll());
  }

  Configuration & Config = Configuration::Instance();
  Config.SetIniFileName(Dir.path() + QStringLiteral("/nixscp.ini"));
  Config.Load();
  CHECK(Config.ChangeMasterPassword(QStringLiteral("master password 1")));
  AgentBroker::Start(nullptr);
  CHECK(AgentBroker::ListenError().isEmpty());
  {
    // both ways: the client checks the broker as the broker checks the client
    QLocalSocket Socket;
    Socket.connectToServer(SocketName());
    CHECK(Socket.waitForConnected(3000) && PeerIsSameUser(Socket.socketDescriptor()));
  }
  TestNul();
  TestInvalidText();
  TestExactBytes();
  TestStdin();
  TestStdinMemory();
  TestSlowReader();
  TestExitStatus();
  TestPrompts(Dir.path());
  TestTamperedSettings();
  TestAccess(Dir.path());
  TestTunnels(LocalBase);
  TestTunnelsDialog(LocalBase);
  TestAccessDialog();
  TestRecordings();
  TestMcp();
  TestNotes(Dir.path());
  TestFiles(Dir.path(), OpenSshKnownHosts);
  TestSurrogateOnServer(Dir.path(), OpenSshKnownHosts);
  TestTwoInstances(Dir.path());
#else
  Q_UNUSED(argc);
  Q_UNUSED(argv);
  std::printf("SKIP: needs Linux\n");
#endif
  if (Failures == 0)
  {
    std::printf("All agent broker tests passed\n");
  }
  return (Failures == 0) ? 0 : 1;
}
