// Tunnels (core/PortForward): the real class against a small SSH server of the
// test (tests/testsshserver.h), which can die, freeze and misbehave on demand,
// and against the OpenSSH test server when NIXSCP_TEST_SSH_PORT and
// NIXSCP_TEST_SSH_KEY are set (a target on this computer, seen from the server).
#include "core/FileSystem.h"
#include "core/PortForward.h"
#include "core/Sockets.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>
#include <cstdio>
#include <functional>
#include <memory>
#include <thread>

#if (defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)) || defined(Q_OS_MACOS)
#include "testsshserver.h"
#include <ctime>
#endif

static int Failures = 0;

#define CHECK(Cond) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); std::fflush(stdout); Failures++; } } while (0)
#define CHECK_MSG(Cond, Msg) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n  %s\n", __FILE__, __LINE__, #Cond, qPrintable(Msg)); std::fflush(stdout); Failures++; } } while (0)

static bool WaitFor(const std::function<bool()> & Condition, int TimeoutMs)
{
  QElapsedTimer Timer;
  Timer.start();
  while (!Condition())
  {
    if (Timer.elapsed() > TimeoutMs)
    {
      return false;
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    QThread::msleep(2);
  }
  return true;
}

// L18, L25: what a name or a typed spec can do to a saved tunnel
static void TestSpecs()
{
  auto RoundTrip = [](const QString & Name, const QString & Host)
  {
    PortForwardSpec Spec;
    Spec.Name = Name;
    Spec.LocalPort = 5433;
    Spec.Host = Host;
    Spec.Port = 5432;
    QList<PortForwardSpec> Back = PortForwardSpec::LoadAll(PortForwardSpec::SaveAll({Spec, Spec}));
    CHECK_MSG(Back.size() == 2, Spec.Save());
    if (Back.size() == 2)
    {
      CHECK_MSG(Back.at(1).Name == Name, Back.at(1).Name);
      CHECK(Back.at(1).LocalPort == 5433);
      CHECK_MSG(Back.at(1).Host == Host, Back.at(1).Host);
      CHECK(Back.at(1).Port == 5432);
    }
  };
  RoundTrip(QStringLiteral("db"), QStringLiteral("localhost"));
  for (const char * Name : {"a%1", "100%2 load", "x%3", "%4", "%", "%%", "%25", "%7C", "a|b", "|", "a:b", "a b"})
  {
    RoundTrip(QString::fromLatin1(Name), QStringLiteral("localhost"));
  }
  RoundTrip(QString::fromUtf8("baza \xC5\xA1"), QStringLiteral("db.example"));
  RoundTrip(QStringLiteral("v6"), QStringLiteral("::1"));
  RoundTrip(QStringLiteral("v6"), QStringLiteral("2001:db8::1"));
  // the existing format stays readable
  QList<PortForwardSpec> Old = PortForwardSpec::LoadAll(QStringLiteral("postgres|15433|localhost|5432\nredis|16379|10.0.0.5|6379"));
  CHECK((Old.size() == 2) && (Old.at(1).Host == QStringLiteral("10.0.0.5")));
  // new lines never split a saved tunnel
  PortForwardSpec Lines;
  Lines.Name = QStringLiteral("a\nb\rc");
  Lines.LocalPort = 1;
  Lines.Host = QStringLiteral("h");
  Lines.Port = 2;
  CHECK(!Lines.Save().contains(QLatin1Char('\n')) && !Lines.Save().contains(QLatin1Char('\r')));
  CHECK(PortForwardSpec::Load(Lines.Save()).Name == Lines.Name);

  auto Parsed = [](const char * Text, const char * Host)
  {
    PortForwardSpec Spec;
    QString Error;
    bool Ok = PortForwardSpec::Parse(QString::fromLatin1(Text), Spec, &Error);
    CHECK_MSG(Ok == (Host != nullptr), QStringLiteral("%1: %2").arg(QString::fromLatin1(Text), Spec.Host));
    if (Host != nullptr)
    {
      CHECK(Spec.Host == QString::fromLatin1(Host));
    }
    else
    {
      CHECK(!Error.isEmpty());
    }
  };
  Parsed("5433:localhost:5432", "localhost");
  Parsed(" 5433:localhost:5432 ", "localhost");
  Parsed("5433:[::1]:5432", "::1");
  Parsed("5433:::1:5432", "::1");
  Parsed("5433:10.0.0.1:22", "10.0.0.1");
  Parsed("0:h:1", nullptr);
  Parsed("70000:h:22", nullptr);
  Parsed("5433:h:0", nullptr);
  Parsed(":h:22", nullptr);
  Parsed("5433: h :22", nullptr);
  Parsed("5433:[h:22", nullptr);
  Parsed("5433:h]:22", nullptr);
  Parsed("1024:h h:22", nullptr);
  Parsed("5433:a:b:22", nullptr);
  Parsed("+5433:h:22", nullptr);
  Parsed("5433:h: 22", nullptr);
  Parsed("5433abc:h:22", nullptr);
  // the server gets the UTF-8 of the host: "h<surrogate>ost" would reach "host",
  // after the prompt and the log showed something else
  {
    PortForwardSpec Spec;
    CHECK(!PortForwardSpec::Parse(QStringLiteral("5433:h") + QChar(0xD800) + QStringLiteral("ost:22"), Spec));
    Spec.LocalPort = 5433;
    Spec.Port = 22;
    Spec.Host = QStringLiteral("h") + QChar(0xDC00) + QStringLiteral("ost");
    CHECK(!Spec.IsValid());
    Spec.Host = QStringLiteral("h\U0001F600ost");
    CHECK(Spec.IsValid());
  }
  Parsed("5433:a|b:22", nullptr);
  PortForwardSpec Text;
  Text.LocalPort = 1;
  Text.Host = QStringLiteral("%3");
  Text.Port = 2;
  CHECK(Text.Text() == QStringLiteral("1 -> %3:2"));
}

#ifndef Q_OS_WIN

class TestUI : public UserInterface
{
public:
  HostKeyAnswer VerifyHostKey(const QString &, int, const QString &, const QString &, const QString &, HostKeyState) override { return HostKeyCancel; }
  bool PromptUser(PromptKind, const QString &, const QString &, QList<Prompt> &) override { return false; }
  void DisplayBanner(const QString &, const QString &) override {}
  unsigned int QueryUser(const QString &, const QString &, unsigned int Answers) override { return Answers; }
};

// A tunnel with what its signals said
struct Tunnel
{
  TestUI UI;
  std::unique_ptr<PortForward> Forward;
  bool Started = false;
  bool StartedOk = false;
  bool Stopped = false;
  QString StopError;
  QStringList ConnectionErrors;
  int Connections = 0;

  Tunnel(const SessionData & Data, int LocalPort, int TargetPort)
  {
    PortForwardSpec Spec;
    Spec.Name = QStringLiteral("test");
    Spec.LocalPort = LocalPort;
    Spec.Host = QStringLiteral("127.0.0.1");
    Spec.Port = TargetPort;
    Forward = std::make_unique<PortForward>(Data, Spec, &UI);
    QObject::connect(Forward.get(), &PortForward::Started, Forward.get(), [this](bool Ok, const QString &) { Started = true; StartedOk = Ok; });
    QObject::connect(Forward.get(), &PortForward::Stopped, Forward.get(), [this](const QString & Error) { Stopped = true; StopError = Error; });
    QObject::connect(Forward.get(), &PortForward::ConnectionFailed, Forward.get(), [this](const QString & Error) { ConnectionErrors.append(Error); });
    QObject::connect(Forward.get(), &PortForward::ConnectionsChanged, Forward.get(), [this](int N) { Connections = N; });
    Forward->Start();
    CHECK(WaitFor([this]() { return Started; }, 20000));
    CHECK_MSG(StartedOk, StopError);
  }
  ~Tunnel()
  {
    Forward->Stop();
    WaitFor([this]() { return Stopped; }, 10000);
  }
};

// A plain blocking client socket, as any program using the tunnel
static int Connect(int Port)
{
  int S = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in Address{};
  Address.sin_family = AF_INET;
  Address.sin_port = htons(static_cast<quint16>(Port));
  Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(S, reinterpret_cast<sockaddr *>(&Address), sizeof(Address)) != 0)
  {
    close(S);
    return -1;
  }
  return S;
}

static QByteArray ReadAll(int S, int TimeoutMs, qint64 Limit = -1)
{
  QByteArray Result;
  char Buffer[65536];
  QElapsedTimer Timer;
  Timer.start();
  while (((Limit < 0) || (Result.size() < Limit)) && (Timer.elapsed() < TimeoutMs))
  {
    pollfd Fd{S, POLLIN, 0};
    if (poll(&Fd, 1, 100) <= 0)
    {
      QCoreApplication::processEvents();
      continue;
    }
    ssize_t N = recv(S, Buffer, sizeof(Buffer), 0);
    if (N <= 0)
    {
      break;
    }
    Result.append(Buffer, N);
  }
  return Result;
}

static QByteArray RandomData(qint64 Size)
{
  QByteArray Data(Size, Qt::Uninitialized);
  for (char & C : Data)
  {
    C = static_cast<char>(QRandomGenerator::global()->bounded(256));
  }
  return Data;
}

// Writes Data and reads what comes back, at the same time
static QByteArray EchoThrough(int Port, const QByteArray & Data)
{
  int S = Connect(Port);
  CHECK(S >= 0);
  std::thread Writer([S, &Data]()
  {
    qint64 Sent = 0;
    while (Sent < Data.size())
    {
      ssize_t N = send(S, Data.constData() + Sent, static_cast<size_t>(qMin<qint64>(Data.size() - Sent, 65536)), MSG_NOSIGNAL);
      if (N <= 0)
      {
        break;
      }
      Sent += N;
    }
    shutdown(S, SHUT_WR);
  });
  QByteArray Back = ReadAll(S, 60000);
  Writer.join();
  close(S);
  return Back;
}

#if (defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)) || defined(Q_OS_MACOS)

static SessionData Site(const TestSshServer & Server)
{
  SessionData Data;
  Data.Name = QStringLiteral("tunnel test");
  Data.HostName = QStringLiteral("127.0.0.1");
  Data.PortNumber = Server.Port();
  Data.UserName = QStringLiteral("u");
  Data.Password = QStringLiteral("p");
  Data.FSProtocol = fsSFTP;
  Data.KnownHostsFile = Server.KnownHosts();
  return Data;
}

static double CpuSeconds()
{
  timespec T;
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &T);
  return static_cast<double>(T.tv_sec) + (static_cast<double>(T.tv_nsec) / 1e9);
}

// X-H1: channels were used after the unwinding stack had freed the session.
// 8 of 8 runs crashed. One connection idle, one busy, as in the report
static void TestServerDies(const QString & Folder, int Port, int LocalPort)
{
  for (int Run = 0; Run < 8; Run++)
  {
    TestSshServer Server;
    CHECK(Server.Start(Folder, Port));
    Tunnel T(Site(Server), LocalPort, 3);
    int Idle = Connect(LocalPort);
    int Busy = Connect(LocalPort);
    CHECK((Idle >= 0) && (Busy >= 0));
    CHECK(WaitFor([&T]() { return T.Connections == 2; }, 5000));
    std::atomic<bool> Go{true};
    std::thread Traffic([&]()
    {
      std::vector<char> Out(65536, 'z'), In(65536);
      while (Go)
      {
        send(Busy, Out.data(), Out.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
        recv(Busy, In.data(), In.size(), MSG_DONTWAIT);
      }
    });
    CHECK(WaitFor([&T]() { return T.Forward->BytesTransferred() > 1000000; }, 10000));
    Server.Kill();
    CHECK(WaitFor([&T]() { return T.Stopped; }, 5000));
    CHECK_MSG(!T.StopError.isEmpty(), QStringLiteral("no error after the server died"));
    CHECK(!T.Forward->Running());
    Go = false;
    Traffic.join();
    close(Idle);
    close(Busy);
  }
}

// X-M5, the part a stopped server shows: nothing else watches an idle session
static void TestServerGoesIdle(const QString & Folder, int Port, int LocalPort)
{
  // gone, with no local connection to notice it
  {
    TestSshServer Server;
    CHECK(Server.Start(Folder, Port));
    Tunnel T(Site(Server), LocalPort, 3);
    Server.Kill();
    CHECK(WaitFor([&T]() { return T.Stopped; }, 3000));
    CHECK(!T.StopError.isEmpty());
  }
  // a link that went dead without a word: TCP says nothing for many minutes
  {
    TestSshServer Server;
    CHECK(Server.Start(Folder, Port));
    SessionData Data = Site(Server);
    Data.PingType = ptNullPacket;
    Data.PingInterval = 1;
    Data.Timeout = 2;
    Tunnel T(Data, LocalPort, 3);
    // answers keepalives while it lives
    QElapsedTimer Timer;
    Timer.start();
    CHECK(!WaitFor([&T]() { return T.Stopped; }, 4000));
    Server.Freeze();
    CHECK(WaitFor([&T]() { return T.Stopped; }, 6000));
    CHECK_MSG(T.StopError.contains(QStringLiteral("no answer")), T.StopError);
    Server.Thaw();
  }
}

// X-M3: a client that does not read must not take the tunnel down with it
static void TestStalledClient(const QString & Folder, int Port, int LocalPort)
{
  TestSshServer Server;
  CHECK(Server.Start(Folder, Port));
  Tunnel T(Site(Server), LocalPort, 2);
  int Flooded = Connect(LocalPort);
  CHECK(Flooded >= 0);
  // the flood fills the socket and the SSH window
  qint64 Before = -1;
  CHECK(WaitFor([&]()
  {
    qint64 Now = T.Forward->BytesTransferred();
    bool Settled = (Now > 100000) && (Now == Before);
    Before = Now;
    QThread::msleep(300);
    return Settled;
  }, 15000));
  // everything else still works: another connection, and Stop
  int Second = Connect(LocalPort);
  CHECK(Second >= 0);
  CHECK_MSG(WaitFor([&T]() { return T.Connections == 2; }, 3000), QString::number(T.Connections));
  QByteArray Flood = ReadAll(Second, 3000, 100000);
  CHECK_MSG(Flood.size() >= 100000, QString::number(Flood.size()));
  QElapsedTimer Timer;
  Timer.start();
  T.Forward->Stop();
  CHECK(WaitFor([&T]() { return T.Stopped; }, 3000));
  CHECK_MSG(Timer.elapsed() < 2000, QString::number(Timer.elapsed()));
  CHECK(T.StopError.isEmpty());
  close(Flooded);
  close(Second);
}

// X-M4: bytes the SSH window had no room for went missing without a word
static void TestShortChannelWrites(const QString & Folder, int Port, int LocalPort)
{
  TestSshServer Server;
  CHECK(Server.Start(Folder, Port));
  // a blocking write gives up after the timeout, with part of the data written:
  // the server takes nothing for 3 seconds, and more than its window waits
  SessionData Data2 = Site(Server);
  Data2.Timeout = 2;
  Tunnel T(Data2, LocalPort, 5);
  QByteArray Data(12 * 1024 * 1024, 'd');
  int S = Connect(LocalPort);
  CHECK(S >= 0);
  std::thread Writer([S, &Data]()
  {
    qint64 Sent = 0;
    while (Sent < Data.size())
    {
      ssize_t N = send(S, Data.constData() + Sent, static_cast<size_t>(qMin<qint64>(Data.size() - Sent, 65536)), MSG_NOSIGNAL);
      if (N <= 0)
      {
        break;
      }
      Sent += N;
    }
    shutdown(S, SHUT_WR);
  });
  QByteArray Expected = "received " + QByteArray::number(Data.size());
  bool Got = WaitFor([&Server, &Expected]() { return Server.Output().contains(Expected); }, 40000);
  CHECK_MSG(Got, QString::fromLatin1(Server.Output()));
  Writer.join();
  close(S);

  // both ways at once, checked to the byte
  Tunnel Echo(Site(Server), LocalPort + 1, 3);
  QByteArray Random = RandomData(12 * 1024 * 1024);
  CHECK(EchoThrough(LocalPort + 1, Random) == Random);
}

// L17: a connection reset after the end of its data made poll report it for ever
static void TestResetDoesNotSpin(const QString & Folder, int Port, int LocalPort)
{
  TestSshServer Server;
  CHECK(Server.Start(Folder, Port));
  Tunnel T(Site(Server), LocalPort, 5);
  int S = Connect(LocalPort);
  CHECK(S >= 0);
  CHECK(WaitFor([&T]() { return T.Connections == 1; }, 3000));
  linger Linger{1, 0};
  setsockopt(S, SOL_SOCKET, SO_LINGER, &Linger, sizeof(Linger));
  close(S);
  double Start = CpuSeconds();
  QThread::msleep(1500);
  double Used = CpuSeconds() - Start;
  CHECK_MSG(Used < 0.5, QString::number(Used));
}

// L22: the channel open blocked every other connection until it timed out
static void TestSlowChannelOpen(const QString & Folder, int Port, int LocalPort)
{
  TestSshServer Server;
  CHECK(Server.Start(Folder, Port));
  SessionData Data = Site(Server);
  Data.Timeout = 3;
  Tunnel T(Data, LocalPort, 6);
  int Stuck = Connect(LocalPort);
  CHECK(Stuck >= 0);
  QThread::msleep(200);
  QElapsedTimer Timer;
  Timer.start();
  int Second = Connect(LocalPort);
  CHECK(Second >= 0);
  CHECK(send(Second, "ping", 4, MSG_NOSIGNAL) == 4);
  QByteArray Echo = ReadAll(Second, 2500, 4);
  CHECK_MSG(Echo == "ping", QString::fromLatin1(Echo));
  CHECK_MSG(Timer.elapsed() < 2500, QString::number(Timer.elapsed()));
  close(Second);
  // and the one nobody answers is given up on
  CHECK(WaitFor([&T]() { return !T.ConnectionErrors.isEmpty(); }, 6000));
  CHECK(T.ConnectionErrors.value(0).contains(QStringLiteral("no answer")));
  CHECK(ReadAll(Stuck, 2000).isEmpty());
  close(Stuck);
  CHECK(!T.Stopped);
}

// L23: end of data in one direction is not the end of the other
static void TestHalfClose(const QString & Folder, int Port, int LocalPort)
{
  TestSshServer Server;
  CHECK(Server.Start(Folder, Port));
  // the client finished: the answer still arrives
  {
    Tunnel T(Site(Server), LocalPort, 3);
    QByteArray Data = RandomData(3 * 1024 * 1024);
    CHECK(EchoThrough(LocalPort, Data) == Data);
    CHECK(WaitFor([&T]() { return T.Connections == 0; }, 5000));
  }
  // the server finished: the client may still send
  {
    Tunnel T(Site(Server), LocalPort, 7);
    int S = Connect(LocalPort);
    CHECK(S >= 0);
    // EOF arrives after the greeting
    QByteArray Greeting = ReadAll(S, 5000);
    CHECK_MSG(Greeting == "HALF\n", QString::fromLatin1(Greeting));
    QByteArray Data(500000, 'h');
    qint64 Sent = 0;
    while (Sent < Data.size())
    {
      ssize_t N = send(S, Data.constData() + Sent, static_cast<size_t>(Data.size() - Sent), MSG_NOSIGNAL);
      CHECK(N > 0);
      if (N <= 0)
      {
        break;
      }
      Sent += N;
    }
    shutdown(S, SHUT_WR);
    QByteArray Expected = "received " + QByteArray::number(Data.size());
    CHECK_MSG(WaitFor([&Server, &Expected]() { return Server.Output().contains(Expected); }, 15000), QString::fromLatin1(Server.Output()));
    close(S);
    CHECK(WaitFor([&T]() { return T.Connections == 0; }, 5000));
  }
}

// X-M2: Fds has an entry per connection of the round; one accepted in it has none
static void TestManyConnections(const QString & Folder, int Port, int LocalPort)
{
  TestSshServer Server;
  CHECK(Server.Start(Folder, Port));
  Tunnel T(Site(Server), LocalPort, 3);
  QList<int> Sockets;
  for (int I = 0; I < 40; I++)
  {
    int S = Connect(LocalPort);
    CHECK(S >= 0);
    Sockets.append(S);
  }
  CHECK_MSG(WaitFor([&T]() { return T.Connections == 40; }, 10000), QString::number(T.Connections));
  for (int S : Sockets)
  {
    QByteArray Text = "hello " + QByteArray::number(S);
    CHECK(send(S, Text.constData(), static_cast<size_t>(Text.size()), MSG_NOSIGNAL) == Text.size());
    QByteArray Echoed = ReadAll(S, 3000, Text.size());
    CHECK_MSG(Echoed == Text, QStringLiteral("socket %1 of %2: [%3] connections=%4 bytes=%5 stopped=%6 error=%7 failed=%8").arg(Sockets.indexOf(S)).arg(Sockets.size()).arg(QString::fromLatin1(Echoed)).arg(T.Connections).arg(T.Forward->BytesTransferred()).arg(T.Stopped).arg(T.StopError, T.ConnectionErrors.join(QLatin1Char(';'))));
    close(S);
  }
  CHECK(WaitFor([&T]() { return T.Connections == 0; }, 10000));
}

// Refused by the server: a message, and the tunnel lives
static void TestRefusedTarget(const QString & Folder, int Port, int LocalPort)
{
  TestSshServer Server;
  CHECK(Server.Start(Folder, Port));
  Tunnel T(Site(Server), LocalPort, 4);
  int S = Connect(LocalPort);
  CHECK(S >= 0);
  CHECK(WaitFor([&T]() { return !T.ConnectionErrors.isEmpty(); }, 5000));
  CHECK(ReadAll(S, 2000).isEmpty());
  close(S);
  CHECK(T.Forward->Running());
}

#endif

// The same against OpenSSH, to a target on this computer
static void TestOpenSsh(const QString & Folder, int LocalPort)
{
  QString Key = qEnvironmentVariable("NIXSCP_TEST_SSH_KEY");
  if (qEnvironmentVariableIsEmpty("NIXSCP_TEST_SSH_PORT") || Key.isEmpty())
  {
    std::printf("SKIP OpenSSH part: set NIXSCP_TEST_SSH_PORT and NIXSCP_TEST_SSH_KEY\n");
    return;
  }
  int SshPort = qEnvironmentVariableIntValue("NIXSCP_TEST_SSH_PORT");
  QFile HostKey(QFileInfo(Key).dir().filePath(QStringLiteral("host_ed25519.pub")));
  if (!HostKey.exists())
  {
    // the server of the system
    HostKey.setFileName(QStringLiteral("/etc/ssh/ssh_host_ed25519_key.pub"));
  }
  CHECK(HostKey.open(QIODevice::ReadOnly));
  QString KnownHosts = Folder + QStringLiteral("/openssh_known_hosts");
  QFile Hosts(KnownHosts);
  CHECK(Hosts.open(QIODevice::WriteOnly));
  Hosts.write(QStringLiteral("[127.0.0.1]:%1 ").arg(SshPort).toLatin1() + HostKey.readAll());
  Hosts.close();
  SessionData Data;
  Data.Name = QStringLiteral("openssh");
  Data.HostName = QStringLiteral("127.0.0.1");
  Data.PortNumber = SshPort;
  Data.UserName = qEnvironmentVariable("USER", QStringLiteral("user"));
  Data.PublicKeyFile = Key;
  Data.FSProtocol = fsSFTP;
  Data.KnownHostsFile = KnownHosts;

  // an echo server as the target
  QTcpServer Target;
  CHECK(Target.listen(QHostAddress::LocalHost));
  QObject::connect(&Target, &QTcpServer::newConnection, &Target, [&Target]()
  {
    while (QTcpSocket * Socket = Target.nextPendingConnection())
    {
      QObject::connect(Socket, &QTcpSocket::readyRead, Socket, [Socket]() { Socket->write(Socket->readAll()); });
      QObject::connect(Socket, &QTcpSocket::disconnected, Socket, &QObject::deleteLater);
    }
  });
  Tunnel T(Data, LocalPort, Target.serverPort());
  if (!T.StartedOk)
  {
    return;
  }
  // the event loop of the target runs here: the data goes through a thread
  QByteArray Random = RandomData(8 * 1024 * 1024);
  QByteArray Back;
  bool Done = false;
  std::thread Worker([&]()
  {
    int S = Connect(LocalPort);
    qint64 Sent = 0;
    char Buffer[65536];
    while (Back.size() < Random.size())
    {
      if (Sent < Random.size())
      {
        ssize_t N = send(S, Random.constData() + Sent, static_cast<size_t>(qMin<qint64>(Random.size() - Sent, 65536)), MSG_NOSIGNAL | MSG_DONTWAIT);
        Sent += qMax<ssize_t>(N, 0);
      }
      pollfd Fd{S, POLLIN, 0};
      if (poll(&Fd, 1, 5) > 0)
      {
        ssize_t N = recv(S, Buffer, sizeof(Buffer), 0);
        if (N <= 0)
        {
          break;
        }
        Back.append(Buffer, N);
      }
    }
    close(S);
    Done = true;
  });
  CHECK(WaitFor([&Done]() { return Done; }, 60000));
  Worker.join();
  CHECK_MSG(Back == Random, QStringLiteral("%1 of %2 bytes came back").arg(Back.size()).arg(Random.size()));
  CHECK(T.Forward->Running());
  CHECK(WaitFor([&T]() { return T.Connections == 0; }, 5000));
  // nothing listens on the target: a message, and the tunnel lives
  {
    int Unused = Target.serverPort();
    Target.close();
    Tunnel Closed(Data, LocalPort + 1, Unused);
    int S = Connect(LocalPort + 1);
    CHECK(S >= 0);
    CHECK(WaitFor([&Closed]() { return !Closed.ConnectionErrors.isEmpty(); }, 10000));
    CHECK(ReadAll(S, 2000).isEmpty());
    close(S);
    CHECK(Closed.Forward->Running());
  }
  // a port already taken is reported, not a crash
  {
    QTcpServer Taken;
    CHECK(Taken.listen(QHostAddress::LocalHost));
    PortForwardSpec Spec;
    Spec.Name = QStringLiteral("taken");
    Spec.LocalPort = Taken.serverPort();
    Spec.Host = QStringLiteral("127.0.0.1");
    Spec.Port = 22;
    TestUI UI;
    PortForward Forward(Data, Spec, &UI);
    bool Started = false, Ok = true;
    QString Error;
    QObject::connect(&Forward, &PortForward::Started, &Forward, [&](bool AOk, const QString & E) { Started = true; Ok = AOk; Error = E; });
    Forward.Start();
    CHECK(WaitFor([&Started]() { return Started; }, 20000));
    CHECK(!Ok && Error.contains(QStringLiteral("Cannot listen")));
  }
}

#endif

int main(int argc, char ** argv)
{
#if (defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)) || defined(Q_OS_MACOS)
  TestSshServer::RunIfAsked(argc, argv);
#endif
  QCoreApplication App(argc, argv);
  QTemporaryDir Dir;
  // libssh reads ~/.ssh otherwise
  qputenv("HOME", Dir.path().toUtf8());
  qputenv("XDG_CONFIG_HOME", (Dir.path() + QStringLiteral("/.config")).toUtf8());
  TestSpecs();
#ifndef Q_OS_WIN
  // a port range of its own, next to the test servers
  int Base = 22800 + static_cast<int>(QCoreApplication::applicationPid() % 40) * 4;
  int LocalBase = 22960 + static_cast<int>(QCoreApplication::applicationPid() % 10) * 4;
#if (defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)) || defined(Q_OS_MACOS)
  // "test_portforward Many" runs the tests with that in their names
  QString Only = (argc > 1) ? QString::fromLocal8Bit(argv[1]) : QString();
  using TTest = void (*)(const QString &, int, int);
  const QList<std::pair<QString, TTest>> Tests{
    {QStringLiteral("ServerDies"), TestServerDies}, {QStringLiteral("ServerGoesIdle"), TestServerGoesIdle},
    {QStringLiteral("StalledClient"), TestStalledClient}, {QStringLiteral("ShortChannelWrites"), TestShortChannelWrites},
    {QStringLiteral("ResetDoesNotSpin"), TestResetDoesNotSpin}, {QStringLiteral("SlowChannelOpen"), TestSlowChannelOpen},
    {QStringLiteral("HalfClose"), TestHalfClose}, {QStringLiteral("ManyConnections"), TestManyConnections},
    {QStringLiteral("RefusedTarget"), TestRefusedTarget}};
  for (const auto & Test : Tests)
  {
    if (Test.first.contains(Only))
    {
      Test.second(Dir.path(), Base, LocalBase);
    }
  }
#endif
  TestOpenSsh(Dir.path(), LocalBase);
#endif
  if (Failures == 0)
  {
    std::printf("All tunnel tests passed\n");
  }
  return (Failures == 0) ? 0 : 1;
}
