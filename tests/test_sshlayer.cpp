// Tests of the SSH connection layer (core/SecureShell, WP08): proxies
// (SOCKS 4/4A, SOCKS 5, HTTP CONNECT, Telnet, through in-process test
// proxies), algorithm warning threshold, logical host name, host key info,
// keepalives, agent forwarding, key certificates and Install Public Key.
//
// Offline parts always run. Server parts are skipped unless
// NIXSCP_TEST_SSH_PORT is set (see test_sftp.cpp for NIXSCP_TEST_SSH_HOST,
// _USER, _KEY). Optional, for certificates and Install Public Key:
//   NIXSCP_TEST_SSH_CA_PORT    server trusting a user CA (TrustedUserCAKeys)
//                              whose sftp-server starts in a scratch
//                              directory (sftp-server -d <dir>)
//   NIXSCP_TEST_SSH_CA_HOME    that scratch directory (the test refuses to
//                              install a key anywhere else)
//   NIXSCP_TEST_SSH_CERT_KEY   ECDSA/RSA private key, not in authorized_keys,
//                              with "<key>-cert.pub" signed by the CA
// The user's ~/.ssh is never touched.
#include "core/Configuration.h"
#include "core/SecureShell.h"
#include "core/SessionLog.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>

#include <libssh/sftp.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pwd.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

// a closed peer is an error result, not SIGPIPE (macOS: SO_NOSIGPIPE / ignored signal)
#ifdef MSG_NOSIGNAL
static const int NoSigPipe = MSG_NOSIGNAL;
#else
static const int NoSigPipe = 0;
#endif

static int Failures = 0;

#define CHECK(Cond) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); std::fflush(stdout); Failures++; } } while (0)
#define CHECK_MSG(Cond, Msg) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, #Cond, qPrintable(Msg)); std::fflush(stdout); Failures++; } } while (0)

static void Log(const QString & Message)
{
  std::printf("  %s\n", qPrintable(Message));
  std::fflush(stdout);
}

class TestUserInterface : public UserInterface
{
public:
  QMutex Mutex;
  HostKeyAnswer HostKeyResponse = HostKeyAccept;
  QString LastHostKeyHost;
  QString LastFingerprint;
  int HostKeyCalls = 0;
  HostKeyState LastHostKeyState = HostKeyUnknown;
  QStringList PromptAnswers;
  QStringList PromptTexts;
  QList<unsigned int> QueryAnswers;
  QStringList Queries;
  QStringList Informations;
  unsigned int LastQueryTimeout = 0;
  unsigned int LastQueryTimeoutAnswer = 0;


  HostKeyAnswer VerifyHostKey(const QString & Host, int, const QString &, const QString & FingerprintSHA256,
    const QString &, HostKeyState State) override
  {
    QMutexLocker Lock(&Mutex);
    HostKeyCalls++;
    LastHostKeyState = State;
    LastHostKeyHost = Host;
    LastFingerprint = FingerprintSHA256;
    return HostKeyResponse;
  }

  bool PromptUser(PromptKind, const QString & Name, const QString &, QList<Prompt> & Prompts) override
  {
    QMutexLocker Lock(&Mutex);
    for (Prompt & P : Prompts)
    {
      PromptTexts.append(Name + QLatin1Char('|') + P.Text);
      if (PromptAnswers.isEmpty())
      {
        return false;
      }
      P.Result = PromptAnswers.takeFirst();
    }
    return true;
  }

  void DisplayBanner(const QString &, const QString &) override {}

  void Information(const QString & Message) override
  {
    QMutexLocker Lock(&Mutex);
    Informations.append(Message);
  }

  unsigned int QueryUserEx(const QString & Query, const QString & MoreMessages, unsigned int Answers,
    const QueryUserParams & Params) override
  {
    {
      QMutexLocker Lock(&Mutex);
      LastQueryTimeout = Params.Timeout;
      LastQueryTimeoutAnswer = Params.TimeoutAnswer;
    }
    return QueryUser(Query, MoreMessages, Answers);
  }

  unsigned int QueryUser(const QString & Query, const QString &, unsigned int) override
  {
    QMutexLocker Lock(&Mutex);
    Queries.append(Query);
    Log(QStringLiteral("QueryUser: %1").arg(Query));
    return QueryAnswers.isEmpty() ? static_cast<unsigned int>(qaNo) : QueryAnswers.takeFirst();
  }
};

// ---------------------------------------------------------------- test proxy

static bool SendAll(int S, const QByteArray & Data)
{
  qsizetype Done = 0;
  while (Done < Data.size())
  {
    ssize_t N = ::send(S, Data.constData() + Done, static_cast<size_t>(Data.size() - Done), NoSigPipe);
    if (N <= 0)
    {
      return false;
    }
    Done += N;
  }
  return true;
}

static QByteArray RecvExact(int S, qsizetype Len)
{
  QByteArray Result;
  while (Result.size() < Len)
  {
    char Buffer[512];
    ssize_t N = ::recv(S, Buffer, static_cast<size_t>(std::min<qsizetype>(sizeof(Buffer), Len - Result.size())), 0);
    if (N <= 0)
    {
      return QByteArray();
    }
    Result.append(Buffer, N);
  }
  return Result;
}

static QByteArray RecvUntil(int S, const QByteArray & Terminator)
{
  QByteArray Result;
  while (!Result.endsWith(Terminator))
  {
    QByteArray C = RecvExact(S, 1);
    if (C.isEmpty())
    {
      return QByteArray();
    }
    Result += C;
  }
  return Result;
}

static int ConnectTo(const QByteArray & Host, int Port)
{
  int S = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in A{};
  A.sin_family = AF_INET;
  A.sin_port = htons(static_cast<uint16_t>(Port));
  inet_pton(AF_INET, Host.constData(), &A.sin_addr);
  if (::connect(S, reinterpret_cast<sockaddr *>(&A), sizeof(A)) != 0)
  {
    ::close(S);
    return -1;
  }
  return S;
}

static void Relay(int A, int B)
{
  pollfd P[2] = {{A, POLLIN, 0}, {B, POLLIN, 0}};
  char Buffer[16384];
  for (;;)
  {
    if (poll(P, 2, 30000) <= 0)
    {
      return;
    }
    for (int I = 0; I < 2; I++)
    {
      if (P[I].revents & (POLLIN | POLLHUP | POLLERR))
      {
        ssize_t N = ::recv(P[I].fd, Buffer, sizeof(Buffer), 0);
        if (N <= 0)
        {
          return;
        }
        if (!SendAll(P[1 - I].fd, QByteArray(Buffer, N)))
        {
          return;
        }
      }
    }
  }
}

// Minimal proxy servers, one connection at a time, relaying to the SSH
// server whatever destination the client asks for (the request is recorded)
class TestProxy
{
public:
  enum Mode { Socks4, Socks5, Http, Telnet };

  TestProxy(Mode AMode, const QByteArray & TargetHost, int TargetPort) :
    FMode(AMode), FTargetHost(TargetHost), FTargetPort(TargetPort)
  {
    FListen = socket(AF_INET, SOCK_STREAM, 0);
    int One = 1;
    setsockopt(FListen, SOL_SOCKET, SO_REUSEADDR, &One, sizeof(One));
    sockaddr_in A{};
    A.sin_family = AF_INET;
    A.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(FListen, reinterpret_cast<sockaddr *>(&A), sizeof(A));
    ::listen(FListen, 4);
    socklen_t Len = sizeof(A);
    getsockname(FListen, reinterpret_cast<sockaddr *>(&A), &Len);
    Port = ntohs(A.sin_port);
    FThread = std::thread([this]() { Run(); });
  }

  ~TestProxy()
  {
    FStop = true;
    ::shutdown(FListen, SHUT_RDWR);
    ::close(FListen);
    FThread.join();
  }

  int Port = 0;
  QByteArray RequiredUser;
  QByteArray RequiredPassword;
  std::atomic<int> Connections{0};
  std::atomic<int> Relayed{0};
  QMutex Mutex;
  QByteArray LastRequest; // destination as requested (host/IP string + port)

private:
  Mode FMode;
  QByteArray FTargetHost;
  int FTargetPort;
  int FListen;
  std::atomic<bool> FStop{false};
  std::thread FThread;

  void Record(const QByteArray & Request)
  {
    QMutexLocker Lock(&Mutex);
    LastRequest = Request;
  }

  void Run()
  {
    while (!FStop)
    {
      int Client = ::accept(FListen, nullptr, nullptr);
      if (Client < 0)
      {
        return;
      }
      Connections++;
      if (Handshake(Client))
      {
        int Target = ConnectTo(FTargetHost, FTargetPort);
        if (Target >= 0)
        {
          if (FMode == Telnet)
          {
            // Noise before the SSH identification, as real telnet proxies print
            SendAll(Client, "Trying 127.0.0.1...\r\nConnected to target.\r\nEscape character is '^]'.\r\n");
          }
          Relayed++;
          Relay(Client, Target);
          ::close(Target);
        }
      }
      ::close(Client);
    }
  }

  bool Handshake(int S)
  {
    switch (FMode)
    {
      case Socks4:
      {
        QByteArray Head = RecvExact(S, 8);
        if ((Head.size() != 8) || (Head[0] != 4) || (Head[1] != 1))
        {
          return false;
        }
        int DestPort = (static_cast<unsigned char>(Head[2]) << 8) | static_cast<unsigned char>(Head[3]);
        QByteArray User = RecvUntil(S, QByteArray(1, '\0'));
        User.chop(1);
        QByteArray Dest;
        if ((Head[4] == 0) && (Head[5] == 0) && (Head[6] == 0) && (Head[7] != 0))
        {
          Dest = RecvUntil(S, QByteArray(1, '\0'));
          Dest.chop(1);
          Dest = "4a:" + Dest;
        }
        else
        {
          char Buffer[INET_ADDRSTRLEN];
          inet_ntop(AF_INET, Head.constData() + 4, Buffer, sizeof(Buffer));
          Dest = Buffer;
        }
        Record(Dest + ':' + QByteArray::number(DestPort) + " user=" + User);
        return SendAll(S, QByteArray("\x00\x5A\x00\x00\x00\x00\x00\x00", 8));
      }
      case Socks5:
      {
        QByteArray Head = RecvExact(S, 2);
        if (Head.size() != 2 || Head[0] != 5)
        {
          return false;
        }
        QByteArray Methods = RecvExact(S, static_cast<unsigned char>(Head[1]));
        bool WantAuth = !RequiredUser.isEmpty();
        if (WantAuth)
        {
          if (!Methods.contains('\x02'))
          {
            SendAll(S, QByteArray("\x05\xFF", 2));
            return false;
          }
          SendAll(S, QByteArray("\x05\x02", 2));
          QByteArray Ver = RecvExact(S, 2);
          QByteArray User = RecvExact(S, static_cast<unsigned char>(Ver[1]));
          QByteArray PLen = RecvExact(S, 1);
          QByteArray Password = RecvExact(S, static_cast<unsigned char>(PLen[0]));
          bool Ok = (User == RequiredUser) && (Password == RequiredPassword);
          SendAll(S, Ok ? QByteArray("\x01\x00", 2) : QByteArray("\x01\x01", 2));
          if (!Ok)
          {
            return false;
          }
        }
        else
        {
          SendAll(S, QByteArray("\x05\x00", 2));
        }
        QByteArray Req = RecvExact(S, 4);
        QByteArray Dest;
        if (Req[3] == 1)
        {
          QByteArray Addr = RecvExact(S, 4);
          char Buffer[INET_ADDRSTRLEN];
          inet_ntop(AF_INET, Addr.constData(), Buffer, sizeof(Buffer));
          Dest = Buffer;
        }
        else if (Req[3] == 3)
        {
          QByteArray L = RecvExact(S, 1);
          Dest = "name:" + RecvExact(S, static_cast<unsigned char>(L[0]));
        }
        else
        {
          RecvExact(S, 16);
          Dest = "ipv6";
        }
        QByteArray PortBytes = RecvExact(S, 2);
        int DestPort = (static_cast<unsigned char>(PortBytes[0]) << 8) | static_cast<unsigned char>(PortBytes[1]);
        Record(Dest + ':' + QByteArray::number(DestPort));
        return SendAll(S, QByteArray("\x05\x00\x00\x01\x7f\x00\x00\x01\x00\x00", 10));
      }
      case Http:
      {
        QByteArray Header = RecvUntil(S, "\r\n\r\n");
        QByteArray First = Header.left(Header.indexOf("\r\n"));
        Record(First);
        if (!RequiredUser.isEmpty())
        {
          QByteArray Expected = "Proxy-Authorization: Basic " + (RequiredUser + ':' + RequiredPassword).toBase64() + "\r\n";
          if (!Header.contains(Expected))
          {
            SendAll(S, "HTTP/1.1 407 Proxy Authentication Required\r\nProxy-Authenticate: Basic realm=\"test\"\r\nContent-Length: 0\r\n\r\n");
            return false;
          }
        }
        return SendAll(S, "HTTP/1.1 200 Connection established\r\nProxy-Agent: test\r\n\r\n");
      }
      case Telnet:
      {
        QByteArray Command = RecvUntil(S, "\n");
        Record(Command.trimmed());
        return Command.startsWith("connect ");
      }
    }
    return false;
  }
};

// ---------------------------------------------------------------- helpers

static bool TryOpen(SecureShell & Shell, QString * Error = nullptr)
{
  try
  {
    Shell.Open();
    return true;
  }
  catch (const AbortException &)
  {
    if (Error != nullptr)
    {
      *Error = QStringLiteral("aborted");
    }
    Log(QStringLiteral("Open aborted"));
  }
  catch (const ExtException & E)
  {
    if (Error != nullptr)
    {
      *Error = E.Message() + QLatin1Char('\n') + E.MoreMessages();
    }
    Log(QStringLiteral("Open failed: %1 / %2").arg(E.Message(), E.MoreMessages()));
  }
  return false;
}

static QString ExecOutput(SecureShell & Shell, const QString & Command)
{
  SshChannel Channel(Shell);
  Channel.OpenExec(Command);
  Channel.SendEof();
  QString Output;
  QString Line;
  while (Channel.ReceiveLineOrEof(Line))
  {
    Output += Line + QLatin1Char('\n');
  }
  return Output;
}

static QString InfoValue(const QStringList & Lines, const QString & Key)
{
  for (const QString & Line : Lines)
  {
    if (Line.startsWith(Key + QStringLiteral(": ")))
    {
      return Line.mid(Key.size() + 2);
    }
  }
  return QString();
}

// ---------------------------------------------------------------- offline

static void TestOffline(const QString & ScratchPath)
{
  Log(QStringLiteral("offline: certificates, public key lines, proxy options"));
  QDir Scratch(ScratchPath);
  // Matching certificate detection
  QString Key = Scratch.filePath(QStringLiteral("id_test"));
  (void)QFile(Key).open(QIODevice::WriteOnly);
  CHECK(SecureShell::MatchingCertificateFile(Key).isEmpty());
  (void)QFile(Key + QStringLiteral(".pub-aadcert.pub")).open(QIODevice::WriteOnly);
  CHECK(SecureShell::MatchingCertificateFile(Key) == Key + QStringLiteral(".pub-aadcert.pub"));
  (void)QFile(Key + QStringLiteral("-cert.pub")).open(QIODevice::WriteOnly);
  CHECK(SecureShell::MatchingCertificateFile(Key) == Key + QStringLiteral("-cert.pub"));
  CHECK(SecureShell::MatchingCertificateFile(Key + QStringLiteral(".ppk")) == Key + QStringLiteral("-cert.pub"));
  CHECK(SecureShell::MatchingCertificateFile(Key + QStringLiteral(".PPK")) == Key + QStringLiteral("-cert.pub"));
  CHECK(SecureShell::MatchingCertificateFile(QString()).isEmpty());

  // Public key line: .pub next to the key
  QFile Pub(Scratch.filePath(QStringLiteral("k1.pub")));
  (void)Pub.open(QIODevice::WriteOnly);
  Pub.write("ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIL0CDQbAk9WUv26IMj1Xdq+KK/Q434dyU34mAAAAAAAA my comment\n");
  Pub.close();
  (void)QFile(Scratch.filePath(QStringLiteral("k1"))).open(QIODevice::WriteOnly);
  QString Comment;
  QString Line = SecureShell::PublicKeyLine(Scratch.filePath(QStringLiteral("k1")), &Comment);
  CHECK_MSG(Line == QStringLiteral("ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIL0CDQbAk9WUv26IMj1Xdq+KK/Q434dyU34mAAAAAAAA my comment"), Line);
  CHECK(Comment == QStringLiteral("my comment"));

  // Public key line from a .ppk (public part is never encrypted)
  // As test_puttykey: NIXSCP_TEST_DATA_DIR (the ppk directory) or the source tree
  QString PpkDir = qEnvironmentVariable("NIXSCP_TEST_DATA_DIR",
    QFileInfo(QStringLiteral(__FILE__)).absolutePath() + QStringLiteral("/data/ppk"));
  QString Ppk = PpkDir + QStringLiteral("/ecdsa256-v3-enc.ppk");
  if (QFileInfo::exists(Ppk))
  {
    Line = SecureShell::PublicKeyLine(Ppk, &Comment);
    QFile Expected(PpkDir + QStringLiteral("/ecdsa256.pub"));
    (void)Expected.open(QIODevice::ReadOnly);
    QStringList ExpectedParts = QString::fromUtf8(Expected.readAll()).trimmed().split(QLatin1Char(' '));
    CHECK_MSG(Line.section(QLatin1Char(' '), 0, 1) == ExpectedParts.mid(0, 2).join(QLatin1Char(' ')), Line);
    CHECK(!Comment.isEmpty());
  }

  // Unknown file
  QFile Junk(Scratch.filePath(QStringLiteral("junk")));
  (void)Junk.open(QIODevice::WriteOnly);
  Junk.write("not a key");
  Junk.close();
  bool Thrown = false;
  try
  {
    SecureShell::PublicKeyLine(Junk.fileName());
  }
  catch (const ExtException &)
  {
    Thrown = true;
  }
  CHECK(Thrown);

  // Only a "Local" proxy produces a command; localhost is excluded unless
  // ProxyLocalhost is on
  SessionData Data;
  Data.HostName = QStringLiteral("example.com");
  Data.PortNumber = 2200;
  Data.ProxyMethod = pmCmd;
  Data.ProxyHost = QStringLiteral("proxy.example");
  Data.ProxyPort = 3128;
  Data.ProxyUsername = QStringLiteral("u");
  Data.ProxyLocalCommand = QStringLiteral("connect %host %port via %proxyhost:%proxyport as %user\\t100%%");
  CHECK_MSG(SecureShell::ProxyCommandOption(Data) == QStringLiteral("connect example.com 2200 via proxy.example:3128 as u\t100%"),
    SecureShell::ProxyCommandOption(Data));
  // the host may be from a URL, and the command goes to a shell
  for (const char * Host : {"$(id)", "`id`", "a;id", "a b", "a|id", "a&id", "a\nid", "a\"b", "a'b", "a\\b", "a>b"})
  {
    Data.HostName = QString::fromLatin1(Host);
    bool Refused = false;
    try
    {
      SecureShell::ProxyCommandOption(Data);
    }
    catch (const FatalException &)
    {
      Refused = true;
    }
    CHECK_MSG(Refused, QString::fromLatin1(Host));
  }
  Data.ProxyLocalhost = true;
  for (const char * Host : {"example.com", "my-host_1.example", "10.0.0.1", "::1", "[fe80::1%eth0]"})
  {
    Data.HostName = QString::fromLatin1(Host);
    CHECK_MSG(SecureShell::ProxyCommandOption(Data).contains(QString::fromLatin1(Host)), QString::fromLatin1(Host));
  }
  Data.ProxyLocalhost = false;
  Data.HostName = QStringLiteral("example.com");
  Data.ProxyMethod = pmSocks5;
  CHECK(SecureShell::ProxyCommandOption(Data).isEmpty());
  Data.ProxyMethod = pmCmd;
  Data.HostName = QStringLiteral("localhost");
  Data.ProxyLocalhost = false;
  CHECK(SecureShell::ProxyCommandOption(Data).isEmpty());
  Data.ProxyLocalhost = true;
  CHECK(!SecureShell::ProxyCommandOption(Data).isEmpty());

  // Autodetect from the environment
  QByteArray Saved = qgetenv("all_proxy");
  qputenv("all_proxy", "socks5://socks.example:1081");
  TProxyMethod Method = pmNone;
  QString Host;
  int Port = 0;
  CHECK(SecureShell::AutodetectProxy(Method, Host, Port));
  CHECK((Method == pmSocks5) && (Host == QStringLiteral("socks.example")) && (Port == 1081));
  qputenv("all_proxy", "http://webproxy.example:8080/");
  CHECK(SecureShell::AutodetectProxy(Method, Host, Port));
  CHECK((Method == pmHTTP) && (Host == QStringLiteral("webproxy.example")) && (Port == 8080));
  if (Saved.isEmpty())
  {
    qunsetenv("all_proxy");
  }
  else
  {
    qputenv("all_proxy", Saved);
  }

  // A proxy that does not listen fails instead of silently going direct
  SessionData Direct;
  Direct.HostName = QStringLiteral("127.0.0.1");
  Direct.PortNumber = 1;
  Direct.UserName = QStringLiteral("nobody");
  Direct.ProxyMethod = pmTelnet;
  Direct.ProxyLocalhost = true;
  Direct.ProxyHost = QStringLiteral("127.0.0.1");
  Direct.ProxyPort = 1;
  Direct.Timeout = 3;
  TestUserInterface UI;
  SecureShell Shell(Direct, &UI);
  QString Error;
  CHECK(!TryOpen(Shell, &Error));
  CHECK_MSG(Error.contains(QStringLiteral("127.0.0.1")), Error);
}

// ---------------------------------------------------------------- server

static SessionData BaseData(const QString & Host, int Port, const QString & User, const QString & KeyFile, const QString & KnownHosts)
{
  SessionData Data;
  Data.HostName = Host;
  Data.PortNumber = Port;
  Data.UserName = User;
  Data.PublicKeyFile = KeyFile;
  Data.TryAgent = false;
  Data.AuthKI = false;
  Data.KnownHostsFile = KnownHosts;
  Data.Timeout = 15;
  return Data;
}

static void TestServerInfo(SessionData Data)
{
  Log(QStringLiteral("server info, host key scan, keepalives"));
  TestUserInterface UI;
  SecureShell Scan(Data, &UI);
  SshHostKeyInfo Scanned = Scan.ScanHostKey();
  CHECK(!Scanned.KeyType.isEmpty());
  CHECK(Scanned.SHA256.startsWith(QStringLiteral("SHA256:")));
  CHECK(Scanned.MD5.count(QLatin1Char(':')) == 15);
  CHECK(!Scan.Active());
  CHECK(UI.HostKeyCalls == 0);

  Data.Bug[sbIgnore2] = asOn;
  SecureShell Shell(Data, &UI);
  if (!TryOpen(Shell))
  {
    CHECK(false);
    return;
  }
  QStringList Info = Shell.SessionInfo();
  Log(Info.join(QStringLiteral(" | ")));
  CHECK(InfoValue(Info, QStringLiteral("Host key algorithm")) == Scanned.KeyType);
  CHECK(InfoValue(Info, QStringLiteral("Host key fingerprint (SHA-256)")) == Scanned.SHA256);
  CHECK(InfoValue(Info, QStringLiteral("Host key fingerprint (MD5)")) == Scanned.MD5);
  CHECK(!InfoValue(Info, QStringLiteral("Encryption algorithm")).isEmpty());
  CHECK(InfoValue(Info, QStringLiteral("Compression")) == QStringLiteral("No"));
  CHECK(Shell.HostKey().SHA256 == Scanned.SHA256);
  // Keepalive with the "chokes on ignore" workaround, then the connection works
  Shell.KeepAlive();
  CHECK(ExecOutput(Shell, QStringLiteral("echo alive")).trimmed() == QStringLiteral("alive"));
}

static void TestWeakAlgorithms(SessionData Data)
{
  Log(QStringLiteral("algorithm warning threshold"));
  // 3des-cbc first, but OpenSSH no longer offers it: the negotiated AES is
  // below WARN
  Data.CipherList = QStringLiteral("3des,WARN,aes,chacha20,aesgcm,des,blowfish,arcfour");
  {
    TestUserInterface UI;
    UI.QueryAnswers = {qaNo};
    SecureShell Shell(Data, &UI);
    QString Error;
    CHECK(!TryOpen(Shell, &Error));
    CHECK_MSG(Error.contains(QStringLiteral("was not verified")), Error);
    CHECK(UI.Queries.size() == 1);
    CHECK(!UI.Queries.isEmpty() && UI.Queries[0].contains(QStringLiteral("The first cipher supported by the server is aes")));
    CHECK(!UI.Queries.isEmpty() && UI.Queries[0].contains(QStringLiteral("below the configured warning threshold")));
  }
  {
    TestUserInterface UI;
    UI.QueryAnswers = {qaYes};
    SecureShell Shell(Data, &UI);
    CHECK(TryOpen(Shell));
    CHECK(UI.Queries.size() == 1);
  }
  // Default lists: no question
  Data.CipherList = SessionData::DefaultCipherList();
  {
    TestUserInterface UI;
    SecureShell Shell(Data, &UI);
    CHECK(TryOpen(Shell));
    CHECK(UI.Queries.isEmpty());
  }
}

// S11: the OpenSSH configuration files of the user and the system are not
// read for a site. libssh takes the home directory from the passwd entry, so
// the file is put in place with bwrap, in a child process of this test.
static int ConfigProbe(SessionData Data)
{
  TestUserInterface UI;
  SecureShell Shell(Data, &UI);
  QString Error;
  bool Opened = TryOpen(Shell, &Error);
  std::printf("probe: opened=%d %s\n", Opened ? 1 : 0, qPrintable(Error));
  return Opened ? 3 : 0;
}

static void TestSshConfigNotRead(const QString & ScratchPath, int Port)
{
  Log(QStringLiteral("~/.ssh/config"));
  QString Bwrap = QStandardPaths::findExecutable(QStringLiteral("bwrap"));
  struct passwd * Pw = getpwuid(getuid());
  if (Bwrap.isEmpty() || (Pw == nullptr) || !QFileInfo::exists(QFile::decodeName(Pw->pw_dir) + QStringLiteral("/.ssh")))
  {
    Log(QStringLiteral("SKIPPED (no bwrap, or no ~/.ssh to bind over)"));
    return;
  }
  QString Dir = QDir(ScratchPath).filePath(QStringLiteral("dotssh-config"));
  QDir().mkpath(Dir);
  QString Marker = QDir(ScratchPath).filePath(QStringLiteral("proxycommand-ran"));
  QFile Config(Dir + QStringLiteral("/config"));
  CHECK(Config.open(QIODevice::WriteOnly));
  Config.write(QStringLiteral("Host cfgalias.test\n  HostName 127.0.0.1\n  Port %1\n  ProxyCommand sh -c 'touch %2; nc 127.0.0.1 %1'\n").arg(Port).arg(Marker).toUtf8());
  Config.close();
  QProcess Probe;
  Probe.setProcessChannelMode(QProcess::MergedChannels);
  QProcessEnvironment Env = QProcessEnvironment::systemEnvironment();
  Env.insert(QStringLiteral("NIXSCP_TEST_CONFIG_PROBE"), QStringLiteral("1"));
  Probe.setProcessEnvironment(Env);
  Probe.start(Bwrap, {QStringLiteral("--dev-bind"), QStringLiteral("/"), QStringLiteral("/"), QStringLiteral("--bind"), Dir,
    QFile::decodeName(Pw->pw_dir) + QStringLiteral("/.ssh"), QCoreApplication::applicationFilePath()});
  CHECK(Probe.waitForFinished(60000));
  QString Output = QString::fromUtf8(Probe.readAll());
  Log(Output.trimmed());
  CHECK_MSG(Probe.exitCode() == 0, Output);
  CHECK(!QFileInfo::exists(Marker));
}

// B16: a proxy command that does not exit when its input closes
static void TestProxyCommandExit(SessionData Data)
{
  Log(QStringLiteral("local proxy command that does not exit"));
  QString Nc = QStandardPaths::findExecutable(QStringLiteral("nc"));
  if (Nc.isEmpty())
  {
    Log(QStringLiteral("SKIPPED (no nc)"));
    return;
  }
  Data.ProxyMethod = pmCmd;
  Data.ProxyLocalhost = true;
  // what makes nc stay: it keeps waiting for the server, whatever happens to its input
  Data.ProxyLocalCommand = QStringLiteral("nc %host %port");
  TestUserInterface UI;
  QElapsedTimer Timer;
  {
    SecureShell Shell(Data, &UI);
    CHECK(TryOpen(Shell));
    CHECK(ExecOutput(Shell, QStringLiteral("echo through-nc")).trimmed() == QStringLiteral("through-nc"));
    Timer.start();
    Shell.Close();
  }
  Log(QStringLiteral("closed in %1 ms").arg(Timer.elapsed()));
  CHECK(Timer.elapsed() < 5000);
  // the command that fails at once: an error, not a hang
  Data.ProxyLocalCommand = QStringLiteral("false");
  SecureShell Failing(Data, &UI);
  QString Error;
  CHECK(!TryOpen(Failing, &Error));

  // a compound command reaches its last part, "exec" would end with the first
  for (const char * Command : {"sleep 0; nc %host %port", "true && nc %host %port", "NIXSCP_TEST=1 nc %host %port", "(nc %host %port)"})
  {
    Data.ProxyLocalCommand = QString::fromLatin1(Command);
    TestUserInterface CompoundUI;
    SecureShell Shell(Data, &CompoundUI);
    QString CompoundError;
    bool Opened = TryOpen(Shell, &CompoundError);
    CHECK_MSG(Opened, QString::fromLatin1(Command) + QLatin1Char(' ') + CompoundError);
    if (!Opened)
    {
      continue;
    }
    CHECK(ExecOutput(Shell, QStringLiteral("echo compound")).trimmed() == QStringLiteral("compound"));
    Timer.restart();
    Shell.Close();
    CHECK(Timer.elapsed() < 5000);
  }
  // and leaves nothing behind when the session closes
  const char * Marker = "31337.5";
  Data.ProxyLocalCommand = QStringLiteral("sleep %1 & nc %host %port").arg(QString::fromLatin1(Marker));
  auto Running = [Marker]()
  {
    QProcess List;
    List.start(QStringLiteral("ps"), {QStringLiteral("-A"), QStringLiteral("-ww"), QStringLiteral("-o"), QStringLiteral("args=")});
    return List.waitForFinished(10000) && List.readAllStandardOutput().contains(Marker);
  };
  {
    TestUserInterface GroupUI;
    SecureShell Shell(Data, &GroupUI);
    CHECK(TryOpen(Shell));
    CHECK(Running());
    Shell.Close();
  }
  CHECK(!Running());
}

// B18, M14: the tunnel goes through the proxy of the session and uses the
// algorithms of the session; its key may be given with a passphrase
static void TestTunnel(SessionData Data, const QString & PassKey, const QString & Passphrase)
{
  Log(QStringLiteral("tunnel through a proxy, TunnelPassphrase"));
  QByteArray TargetHost = Data.HostName.toUtf8();
  Data.Tunnel = true;
  Data.TunnelHostName = Data.HostName;
  Data.TunnelPortNumber = Data.PortNumber;
  Data.TunnelUserName = Data.UserName;
  Data.TunnelPublicKeyFile = PassKey.isEmpty() ? Data.PublicKeyFile : PassKey;
  Data.TunnelPassphrase = Passphrase;
  Data.TunnelHostKey = QStringLiteral("*");
  Data.HostKey = QStringLiteral("*");
  {
    TestProxy Proxy(TestProxy::Socks5, TargetHost, Data.PortNumber);
    SessionData D = Data;
    D.ProxyMethod = pmSocks5;
    D.ProxyLocalhost = true;
    D.ProxyHost = QStringLiteral("127.0.0.1");
    D.ProxyPort = Proxy.Port;
    TestUserInterface UI;
    SecureShell Shell(D, &UI);
    QString Error;
    CHECK_MSG(TryOpen(Shell, &Error), Error);
    CHECK(ExecOutput(Shell, QStringLiteral("echo tunneled")).trimmed() == QStringLiteral("tunneled"));
    CHECK_MSG(Proxy.Relayed == 1, QString::number(Proxy.Relayed.load()));
    CHECK_MSG(UI.PromptTexts.isEmpty(), UI.PromptTexts.join(QLatin1Char('|')));
  }
  {
    // the passphrase, wrong: asked for
    if (!PassKey.isEmpty())
    {
      SessionData D = Data;
      D.TunnelPassphrase = QStringLiteral("wrong passphrase");
      TestUserInterface UI;
      UI.PromptAnswers = {Passphrase};
      SecureShell Shell(D, &UI);
      QString Error;
      CHECK_MSG(TryOpen(Shell, &Error), Error);
      CHECK_MSG(UI.PromptTexts.size() == 1, UI.PromptTexts.join(QLatin1Char('|')));
    }
  }
  {
    // a local proxy command takes the tunnel host too
    SessionData D = Data;
    D.ProxyMethod = pmCmd;
    D.ProxyLocalhost = true;
    D.ProxyLocalCommand = QStringLiteral("nc %host %port");
    if (!QStandardPaths::findExecutable(QStringLiteral("nc")).isEmpty())
    {
      TestUserInterface UI;
      SecureShell Shell(D, &UI);
      QString Error;
      CHECK_MSG(TryOpen(Shell, &Error), Error);
      CHECK(ExecOutput(Shell, QStringLiteral("echo tunneled2")).trimmed() == QStringLiteral("tunneled2"));
    }
  }
  {
    // the algorithms of the session go to the tunnel too: the key exchange
    // offer of the jump connection, read on its way to the tunnel host
    int Listen = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in A{};
    A.sin_family = AF_INET;
    A.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(::bind(Listen, reinterpret_cast<sockaddr *>(&A), sizeof(A)) == 0);
    CHECK(::listen(Listen, 1) == 0);
    socklen_t Len = sizeof(A);
    getsockname(Listen, reinterpret_cast<sockaddr *>(&A), &Len);
    QByteArray Offer;
    std::thread Relay([&]()
    {
      int Client = ::accept(Listen, nullptr, nullptr);
      int Target = (Client >= 0) ? ConnectTo(TargetHost, Data.PortNumber) : -1;
      if (Target >= 0)
      {
        pollfd P[2] = {{Client, POLLIN, 0}, {Target, POLLIN, 0}};
        char Buffer[4096];
        bool Open = true;
        while (Open && (::poll(P, 2, 5000) > 0))
        {
          for (int I = 0; Open && (I < 2); I++)
          {
            if (P[I].revents != 0)
            {
              ssize_t N = ::recv(P[I].fd, Buffer, sizeof(Buffer), 0);
              Open = (N > 0) && SendAll(P[1 - I].fd, QByteArray(Buffer, N));
              if (Open && (I == 0) && (Offer.size() < 4096))
              {
                Offer.append(Buffer, N);
              }
            }
          }
        }
        ::close(Target);
      }
      if (Client >= 0)
      {
        ::close(Client);
      }
    });
    {
      SessionData D = Data;
      D.TunnelHostName = QStringLiteral("127.0.0.1");
      D.TunnelPortNumber = ntohs(A.sin_port);
      D.KexList = QStringLiteral("dh-group14-sha1,ecdh,WARN");
      TestUserInterface UI;
      SecureShell Shell(D, &UI);
      QString Error;
      CHECK_MSG(TryOpen(Shell, &Error), Error);
      Shell.Close();
    }
    Relay.join();
    ::close(Listen);
    qsizetype Chosen = Offer.indexOf("diffie-hellman-group14-sha256");
    qsizetype Default = Offer.indexOf("curve25519-sha256");
    CHECK_MSG((Chosen >= 0) && ((Default < 0) || (Chosen < Default)), QString::fromLatin1(Offer.left(400).replace('\0', '.')));
  }
}

// M13: the fingerprint of a URL, the administrator's policy, -hostkey=acceptnew
static void TestHostKeyPolicies(SessionData Data, const QString & ScratchPath)
{
  Log(QStringLiteral("host key policies"));
  QString KnownHosts = QDir(ScratchPath).filePath(QStringLiteral("policy_known_hosts"));
  Data.KnownHostsFile = KnownHosts;
  TestUserInterface Scan;
  SecureShell Scanner(Data, &Scan);
  QString Fingerprint = Scanner.ScanHostKey().SHA256;
  QString Entry = (Data.PortNumber == 22) ? Data.HostName : QStringLiteral("[%1]:%2").arg(Data.HostName).arg(Data.PortNumber);
  auto WriteKnownHosts = [&](const char * Key)
  {
    QFile File(KnownHosts);
    CHECK(File.open(QIODevice::WriteOnly | QIODevice::Truncate));
    File.write(Entry.toUtf8() + " ssh-ed25519 " + Key + "\n");
  };
  {
    // acceptnew stores an unknown key, and asks nobody
    QFile::remove(KnownHosts);
    SessionData D = Data;
    D.HostKey = QStringLiteral("acceptnew");
    TestUserInterface UI;
    SecureShell Shell(D, &UI);
    CHECK(TryOpen(Shell));
    CHECK(UI.HostKeyCalls == 0);
    QFile File(KnownHosts);
    CHECK(File.open(QIODevice::ReadOnly) && !File.readAll().isEmpty());
  }
  {
    // a changed key is no new key
    WriteKnownHosts("AAAAC3NzaC1lZDI1NTE5AAAAIJ7JYDOxW+0ViCHOQNLEaFcv4UUxbM+AYD+kUYfMdHAh");
    SessionData D = Data;
    D.HostKey = QStringLiteral("acceptnew");
    TestUserInterface UI;
    UI.HostKeyResponse = UserInterface::HostKeyCancel;
    SecureShell Shell(D, &UI);
    CHECK(!TryOpen(Shell));
    CHECK(UI.HostKeyCalls == 1);
  }
  {
    // ";fingerprint=" of the very key the server has does not silence a
    // mismatch with the known one, -hostkey does
    SessionData D = Data;
    D.HostKey = Fingerprint;
    D.HostKeyFromUrl = true;
    TestUserInterface UI;
    UI.HostKeyResponse = UserInterface::HostKeyCancel;
    SecureShell Shell(D, &UI);
    CHECK(!TryOpen(Shell));
    CHECK(UI.HostKeyCalls == 1);
    D.HostKeyFromUrl = false;
    TestUserInterface UI2;
    SecureShell Shell2(D, &UI2);
    CHECK(TryOpen(Shell2));
    CHECK(UI2.HostKeyCalls == 0);
  }
  {
    // with nothing known, it is checked as any configured key
    QFile::remove(KnownHosts);
    SessionData D = Data;
    D.HostKey = QStringLiteral("SHA256:AAAAshZP/98IzuhyeJScnasKJpGTQS8VxhfaqpVwPm8");
    D.HostKeyFromUrl = true;
    TestUserInterface UI;
    SecureShell Shell(D, &UI);
    QString Error;
    CHECK(!TryOpen(Shell, &Error));
    CHECK_MSG(Error.contains(QStringLiteral("does not match")), Error);
    CHECK(UI.HostKeyCalls == 0);
    D.HostKey = Fingerprint;
    SecureShell Shell2(D, &UI);
    CHECK(TryOpen(Shell2));
  }
  {
    // a URL's key known already connects, whatever the fingerprint says
    SessionData First = Data;
    TestUserInterface UI;
    SecureShell Shell(First, &UI);
    CHECK(TryOpen(Shell));
    Shell.Close();
    SessionData D = Data;
    D.HostKey = QStringLiteral("SHA256:AAAAshZP/98IzuhyeJScnasKJpGTQS8VxhfaqpVwPm8");
    D.HostKeyFromUrl = true;
    TestUserInterface UI2;
    SecureShell Shell2(D, &UI2);
    CHECK(TryOpen(Shell2));
  }
  // "*" and "acceptnew" of a URL give up nothing, known host or not, in any spelling
  for (const char * Keys : {"*", " * ", "acceptnew", "AcceptNew", "*;acceptnew", ";*;"})
  {
    QFile::remove(KnownHosts);
    SessionData D = Data;
    D.HostKey = QString::fromLatin1(Keys);
    D.HostKeyFromUrl = true;
    TestUserInterface Unknown;
    Unknown.HostKeyResponse = UserInterface::HostKeyCancel;
    {
      SecureShell Shell(D, &Unknown);
      CHECK_MSG(!TryOpen(Shell), QString::fromLatin1(Keys));
    }
    CHECK(Unknown.HostKeyCalls == 1);
    CHECK(Unknown.LastHostKeyState == UserInterface::HostKeyUnknown);
    CHECK_MSG(!QFile::exists(KnownHosts) || (QFileInfo(KnownHosts).size() == 0), QString::fromLatin1(Keys));
    CHECK(Unknown.Informations.isEmpty());
    WriteKnownHosts("AAAAC3NzaC1lZDI1NTE5AAAAIJ7JYDOxW+0ViCHOQNLEaFcv4UUxbM+AYD+kUYfMdHAh");
    TestUserInterface Changed;
    Changed.HostKeyResponse = UserInterface::HostKeyCancel;
    {
      SecureShell Shell(D, &Changed);
      CHECK_MSG(!TryOpen(Shell), QString::fromLatin1(Keys));
    }
    CHECK(Changed.HostKeyCalls == 1);
    CHECK(Changed.LastHostKeyState == UserInterface::HostKeyChanged);
  }
  {
    // in a list too: the real fingerprint is what is left of it
    QFile::remove(KnownHosts);
    SessionData D = Data;
    D.HostKey = QStringLiteral("SHA256:AAAAshZP/98IzuhyeJScnasKJpGTQS8VxhfaqpVwPm8;*");
    D.HostKeyFromUrl = true;
    TestUserInterface UI;
    SecureShell Shell(D, &UI);
    QString Error;
    CHECK(!TryOpen(Shell, &Error));
    CHECK_MSG(Error.contains(QStringLiteral("does not match")), Error);
    CHECK(UI.HostKeyCalls == 0);
  }
  {
    // a switch or a site says what it means: "*" with a changed key connects
    WriteKnownHosts("AAAAC3NzaC1lZDI1NTE5AAAAIJ7JYDOxW+0ViCHOQNLEaFcv4UUxbM+AYD+kUYfMdHAh");
    SessionData D = Data;
    D.HostKey = QStringLiteral("*");
    TestUserInterface UI;
    SecureShell Shell(D, &UI);
    CHECK(TryOpen(Shell));
    CHECK(UI.HostKeyCalls == 0);
  }
  {
    // the jump host: the same, through TunnelHostKey
    WriteKnownHosts("AAAAC3NzaC1lZDI1NTE5AAAAIJ7JYDOxW+0ViCHOQNLEaFcv4UUxbM+AYD+kUYfMdHAh");
    SessionData D = Data;
    D.HostKey = Fingerprint;
    D.Tunnel = true;
    D.TunnelHostName = Data.HostName;
    D.TunnelPortNumber = Data.PortNumber;
    D.TunnelUserName = Data.UserName;
    D.TunnelPublicKeyFile = Data.PublicKeyFile;
    for (const QString & Keys : {QStringLiteral("*"), Fingerprint})
    {
      D.TunnelHostKey = Keys;
      D.TunnelHostKeyFromUrl = true;
      TestUserInterface UI;
      UI.HostKeyResponse = UserInterface::HostKeyCancel;
      SecureShell Shell(D, &UI);
      CHECK_MSG(!TryOpen(Shell), Keys);
      CHECK_MSG(UI.HostKeyCalls == 1, Keys);
      CHECK(UI.LastHostKeyState == UserInterface::HostKeyChanged);
    }
    D.TunnelHostKey = QStringLiteral("*");
    D.TunnelHostKeyFromUrl = false;
    TestUserInterface UI;
    SecureShell Shell(D, &UI);
    QString Error;
    CHECK_MSG(TryOpen(Shell, &Error), Error);
    CHECK(UI.HostKeyCalls == 0);
  }
  {
    // ANY_HOSTKEY reaches the user in every front end
    SessionData D = Data;
    D.HostKey = QStringLiteral("*");
    TestUserInterface UI;
    SecureShell Shell(D, &UI);
    CHECK(TryOpen(Shell));
    CHECK_MSG(UI.Informations.contains(QStringLiteral("WARNING! Giving up security and accepting any host key as configured!")), UI.Informations.join(QLatin1Char('|')));
  }
}

// D-05: "When SFTP connection is rejected, knock FTP port"
static void TestTryFtp(const QString & Host)
{
  Log(QStringLiteral("FTP suggestion"));
  // Needs port 22 closed and 21 open on the host, which a test cannot have
  // here: what it can check is that other sites do not knock at all
  SessionData Data;
  Data.HostName = Host;
  Data.PortNumber = 1;
  Data.FSProtocol = fsSFTPonly;
  Data.TryAgent = false;
  Data.Timeout = 3;
  TestUserInterface UI;
  SecureShell Shell(Data, &UI);
  QString Error;
  CHECK(!TryOpen(Shell, &Error));
  CHECK_MSG(!Error.contains(QStringLiteral("listens for FTP")), Error);
}

// D-03: a stalled session is given up after SessionReopenAutoStall
static void TestStall(SessionData Data)
{
  Log(QStringLiteral("stalled session"));
  Data.Timeout = 1;
  Configuration::Instance().SessionReopenAutoStall = 12345;
  TestUserInterface UI;
  UI.QueryAnswers = {qaAbort};
  SecureShell Shell(Data, &UI);
  CHECK(TryOpen(Shell));
  bool Fatal = false;
  SshChannel Channel(Shell);
  Channel.OpenExec(QStringLiteral("sleep 30"));
  try
  {
    Channel.ReceiveLine();
  }
  catch (const FatalException & E)
  {
    Fatal = true;
    CHECK_MSG(E.MoreMessages().contains(QStringLiteral("not communicating")), E.MoreMessages());
  }
  CHECK(Fatal);
  CHECK(UI.Queries.size() == 1);
  CHECK(UI.LastQueryTimeout == 12345);
  CHECK(UI.LastQueryTimeoutAnswer == qaAbort);
  Configuration::Instance().SessionReopenAutoStall = 0;
  Channel.Close();
  Shell.Close();
  TestUserInterface UI2;
  UI2.QueryAnswers = {qaAbort};
  SecureShell Shell2(Data, &UI2);
  CHECK(TryOpen(Shell2));
  SshChannel Channel2(Shell2);
  Channel2.OpenExec(QStringLiteral("sleep 30"));
  try
  {
    Channel2.ReceiveLine();
  }
  catch (const FatalException &)
  {
  }
  CHECK(UI2.LastQueryTimeout == 0);
}

// L14: without a key file, only the keys of the agent are offered
// L14: libssh looks in the home directory of the passwd entry, so the key is
// put in place with bwrap, in a child process, as for TestSshConfigNotRead
static void TestDefaultKeys(const SessionData & Data, const QString & ScratchPath)
{
  Log(QStringLiteral("no default keys"));
  QString Bwrap = QStandardPaths::findExecutable(QStringLiteral("bwrap"));
  struct passwd * Pw = getpwuid(getuid());
  if (Bwrap.isEmpty() || (Pw == nullptr) || !QFileInfo::exists(QFile::decodeName(Pw->pw_dir) + QStringLiteral("/.ssh")))
  {
    Log(QStringLiteral("SKIPPED (no bwrap, or no ~/.ssh to bind over)"));
    return;
  }
  QString Dir = QDir(ScratchPath).filePath(QStringLiteral("dotssh-keys"));
  QDir().mkpath(Dir);
  // the key would log in; it must not be touched
  CHECK(QFile::copy(Data.PublicKeyFile, Dir + QStringLiteral("/id_ed25519")));
  QFile::setPermissions(Dir + QStringLiteral("/id_ed25519"), QFile::ReadOwner | QFile::WriteOwner);
  QProcess Probe;
  Probe.setProcessChannelMode(QProcess::MergedChannels);
  QProcessEnvironment Env = QProcessEnvironment::systemEnvironment();
  Env.insert(QStringLiteral("NIXSCP_TEST_CONFIG_PROBE"), QStringLiteral("keys"));
  Env.remove(QStringLiteral("SSH_AUTH_SOCK"));
  Probe.setProcessEnvironment(Env);
  Probe.start(Bwrap, {QStringLiteral("--dev-bind"), QStringLiteral("/"), QStringLiteral("/"), QStringLiteral("--bind"), Dir,
    QFile::decodeName(Pw->pw_dir) + QStringLiteral("/.ssh"), QCoreApplication::applicationFilePath()});
  CHECK(Probe.waitForFinished(60000));
  QString Output = QString::fromUtf8(Probe.readAll());
  Log(Output.trimmed());
  CHECK_MSG(Probe.exitCode() == 0, Output);
}

// L14: the password prompt that follows is not left without a reason
static void TestDefaultKeysLogged(SessionData Data, const QString & ScratchPath)
{
  Log(QStringLiteral("default keys not tried: logged"));
  QString Home = QDir(ScratchPath).filePath(QStringLiteral("logged-home"));
  QDir().mkpath(Home + QStringLiteral("/.ssh"));
  QByteArray OldHome = qgetenv("HOME");
  qputenv("HOME", Home.toUtf8());
  QString LogFile = QDir(ScratchPath).filePath(QStringLiteral("default-keys.log"));
  SessionLogOptions Options;
  Options.Logging = true;
  Options.FileName = LogFile;
  Data.PublicKeyFile.clear();
  Data.TryAgent = false;
  Data.HostKey = QStringLiteral("*");
  auto Attempt = [&]()
  {
    QFile::remove(LogFile);
    auto SessionLogger = std::make_shared<SessionLog>(Options);
    SessionLog::SetCurrent(SessionLogger);
    TestUserInterface UI;
    SecureShell Shell(Data, &UI);
    CHECK(!TryOpen(Shell));
    SessionLog::SetCurrent(nullptr);
    SessionLogger.reset();
    QFile File(LogFile);
    (void)File.open(QIODevice::ReadOnly);
    return QString::fromUtf8(File.readAll());
  };
  // no such key: nothing to explain
  QString Content = Attempt();
  CHECK_MSG(!Content.contains(QStringLiteral("default keys")), Content);
  QFile Key(Home + QStringLiteral("/.ssh/id_ed25519"));
  CHECK(Key.open(QIODevice::WriteOnly));
  Key.close();
  Content = Attempt();
  CHECK_MSG(Content.contains(QStringLiteral("default keys of OpenSSH are not offered")) && Content.contains(QStringLiteral("id_ed25519")), Content);
  qputenv("HOME", OldHome);
}

static void TestLogicalHostName(SessionData Data, const QString & ScratchPath)
{
  Log(QStringLiteral("logical host name"));
  QString KnownHosts = QDir(ScratchPath).filePath(QStringLiteral("logical_known_hosts"));
  Data.KnownHostsFile = KnownHosts;
  Data.LogicalHostName = QStringLiteral("logical.example.invalid");
  TestUserInterface UI;
  SecureShell Shell(Data, &UI);
  CHECK(TryOpen(Shell));
  CHECK(UI.HostKeyCalls == 1);
  CHECK_MSG(UI.LastHostKeyHost == QStringLiteral("logical.example.invalid"), UI.LastHostKeyHost);
  Shell.Close();
  QFile File(KnownHosts);
  (void)File.open(QIODevice::ReadOnly);
  QByteArray Content = File.readAll();
  CHECK_MSG(Content.contains("logical.example.invalid"), QString::fromUtf8(Content));
  // Known now under the logical name: no question
  TestUserInterface UI2;
  SecureShell Shell2(Data, &UI2);
  CHECK(TryOpen(Shell2));
  CHECK(UI2.HostKeyCalls == 0);
}

static void TestProxies(SessionData Data)
{
  Log(QStringLiteral("proxies"));
  QByteArray TargetHost = Data.HostName.toUtf8();
  Data.ProxyLocalhost = true;
  Data.ProxyHost = QStringLiteral("127.0.0.1");
  Data.HostName = QStringLiteral("localhost");

  auto Run = [&](TestProxy & Proxy, SessionData D, const char * What) -> bool
  {
    D.ProxyPort = Proxy.Port;
    TestUserInterface UI;
    UI.PromptAnswers = {QStringLiteral("puser"), QStringLiteral("ppass")};
    SecureShell Shell(D, &UI);
    QString Error;
    bool Ok = TryOpen(Shell, &Error);
    CHECK_MSG(Ok, QStringLiteral("%1: %2").arg(QString::fromLatin1(What), Error));
    if (Ok)
    {
      CHECK(ExecOutput(Shell, QStringLiteral("echo via-proxy")).trimmed() == QStringLiteral("via-proxy"));
    }
    QMutexLocker Lock(&Proxy.Mutex);
    Log(QStringLiteral("%1: request \"%2\", connections %3").arg(QString::fromLatin1(What), QString::fromUtf8(Proxy.LastRequest)).arg(Proxy.Connections.load()));
    return Ok;
  };

  {
    // SOCKS 4: local resolution (ProxyDNS Auto)
    TestProxy Proxy(TestProxy::Socks4, TargetHost, Data.PortNumber);
    SessionData D = Data;
    D.ProxyMethod = pmSocks4;
    D.ProxyUsername = QStringLiteral("s4user");
    Run(Proxy, D, "SOCKS4");
    CHECK(Proxy.Relayed == 1);
    CHECK(Proxy.LastRequest == "127.0.0.1:" + QByteArray::number(Data.PortNumber) + " user=s4user");
  }
  {
    // SOCKS 4A: DNS at proxy forced on
    TestProxy Proxy(TestProxy::Socks4, TargetHost, Data.PortNumber);
    SessionData D = Data;
    D.ProxyMethod = pmSocks4;
    D.ProxyDNS = asOn;
    Run(Proxy, D, "SOCKS4A");
    CHECK(Proxy.LastRequest.startsWith("4a:localhost:"));
  }
  {
    // SOCKS 5: name sent to the proxy (Auto), with authentication
    TestProxy Proxy(TestProxy::Socks5, TargetHost, Data.PortNumber);
    Proxy.RequiredUser = "s5user";
    Proxy.RequiredPassword = "s5pass";
    SessionData D = Data;
    D.ProxyMethod = pmSocks5;
    D.ProxyUsername = QStringLiteral("s5user");
    D.ProxyPassword = QStringLiteral("s5pass");
    Run(Proxy, D, "SOCKS5");
    CHECK(Proxy.LastRequest == "name:localhost:" + QByteArray::number(Data.PortNumber));
  }
  {
    // SOCKS 5: wrong password
    TestProxy Proxy(TestProxy::Socks5, TargetHost, Data.PortNumber);
    Proxy.RequiredUser = "s5user";
    Proxy.RequiredPassword = "s5pass";
    SessionData D = Data;
    D.ProxyMethod = pmSocks5;
    D.ProxyPort = Proxy.Port;
    D.ProxyUsername = QStringLiteral("s5user");
    D.ProxyPassword = QStringLiteral("wrong");
    TestUserInterface UI;
    SecureShell Shell(D, &UI);
    QString Error;
    CHECK(!TryOpen(Shell, &Error));
    CHECK_MSG(Error.contains(QStringLiteral("Proxy authentication failed.")), Error);
  }
  {
    // SOCKS 5 with ProxyDNS off: resolved locally
    TestProxy Proxy(TestProxy::Socks5, TargetHost, Data.PortNumber);
    SessionData D = Data;
    D.ProxyMethod = pmSocks5;
    D.ProxyDNS = asOff;
    D.AddressFamily = afIPv4;
    Run(Proxy, D, "SOCKS5 local DNS");
    CHECK(Proxy.LastRequest == "127.0.0.1:" + QByteArray::number(Data.PortNumber));
  }
  {
    // HTTP: 407 -> credentials prompted -> retried
    TestProxy Proxy(TestProxy::Http, TargetHost, Data.PortNumber);
    Proxy.RequiredUser = "puser";
    Proxy.RequiredPassword = "ppass";
    SessionData D = Data;
    D.ProxyMethod = pmHTTP;
    Run(Proxy, D, "HTTP");
    CHECK(Proxy.Connections == 2);
    CHECK(Proxy.LastRequest == "CONNECT localhost:" + QByteArray::number(Data.PortNumber) + " HTTP/1.1");
  }
  {
    // Telnet: command patterns, noise before the SSH banner is skipped
    TestProxy Proxy(TestProxy::Telnet, TargetHost, Data.PortNumber);
    SessionData D = Data;
    D.ProxyMethod = pmTelnet;
    D.ProxyTelnetCommand = QStringLiteral("connect %host %port\\n");
    Run(Proxy, D, "Telnet");
    CHECK(Proxy.LastRequest == "connect localhost " + QByteArray::number(Data.PortNumber));
  }
  {
    // localhost not proxied unless ProxyLocalhost: the proxy sees nothing
    TestProxy Proxy(TestProxy::Telnet, TargetHost, Data.PortNumber);
    SessionData D = Data;
    D.ProxyMethod = pmTelnet;
    D.ProxyLocalhost = false;
    D.ProxyPort = Proxy.Port;
    TestUserInterface UI;
    SecureShell Shell(D, &UI);
    CHECK(TryOpen(Shell));
    CHECK(Proxy.Connections == 0);
  }
}

static void TestAgent(SessionData Data, const QString & KeyFile, const QString & WrongKey, const QString & ScratchPath)
{
  Log(QStringLiteral("ssh-agent: authentication and forwarding"));
  if (QStandardPaths::findExecutable(QStringLiteral("ssh-agent")).isEmpty() ||
      QStandardPaths::findExecutable(QStringLiteral("ssh-add")).isEmpty())
  {
    Log(QStringLiteral("SKIPPED (no ssh-agent/ssh-add)"));
    return;
  }
  QString Socket = QDir(ScratchPath).filePath(QStringLiteral("agent.sock"));
  QProcess Agent;
  Agent.start(QStringLiteral("ssh-agent"), {QStringLiteral("-D"), QStringLiteral("-a"), Socket});
  for (int I = 0; (I < 50) && !QFileInfo::exists(Socket); I++)
  {
    QThread::msleep(100);
  }
  CHECK(QFileInfo::exists(Socket));
  QByteArray SavedSock = qgetenv("SSH_AUTH_SOCK");
  qputenv("SSH_AUTH_SOCK", QFile::encodeName(Socket));
  QProcess Add;
  Add.start(QStringLiteral("ssh-add"), {KeyFile});
  Add.waitForFinished(10000);
  CHECK(Add.exitCode() == 0);

  // TryAgent: the agent is tried before the configured (wrong) key file
  Data.PublicKeyFile = WrongKey;
  Data.TryAgent = true;
  Data.AgentFwd = true;
  {
    TestUserInterface UI;
    SecureShell Shell(Data, &UI);
    CHECK(TryOpen(Shell));
    if (Shell.Active())
    {
      QString Output = ExecOutput(Shell, QStringLiteral("ssh-add -l; echo rc=$?"));
      Log(QStringLiteral("remote ssh-add -l: %1").arg(Output.trimmed()));
      CHECK_MSG(Output.contains(QStringLiteral("rc=0")), Output);
      CHECK(Shell.AgentChannelsOpened() >= 1);
      // A second command reuses the forwarding
      Output = ExecOutput(Shell, QStringLiteral("ssh-add -l >/dev/null; echo rc=$?"));
      CHECK_MSG(Output.contains(QStringLiteral("rc=0")), Output);
    }
  }
  // Without AgentFwd the remote side has no agent
  Data.AgentFwd = false;
  {
    TestUserInterface UI;
    SecureShell Shell(Data, &UI);
    CHECK(TryOpen(Shell));
    if (Shell.Active())
    {
      QString Output = ExecOutput(Shell, QStringLiteral("ssh-add -l >/dev/null 2>&1; echo rc=$?"));
      CHECK_MSG(Output.contains(QStringLiteral("rc=2")), Output);
      CHECK(Shell.AgentChannelsOpened() == 0);
    }
  }
  // TryAgent off: the wrong key file alone fails
  Data.TryAgent = false;
  {
    TestUserInterface UI;
    SecureShell Shell(Data, &UI);
    CHECK(!TryOpen(Shell));
  }

  if (SavedSock.isEmpty())
  {
    qunsetenv("SSH_AUTH_SOCK");
  }
  else
  {
    qputenv("SSH_AUTH_SOCK", SavedSock);
  }
  Agent.terminate();
  Agent.waitForFinished(5000);
}

static void TestCertificates(SessionData Data, const QString & CertKey, const QString & ScratchPath)
{
  Log(QStringLiteral("key certificates"));
  QString AutoCert = SecureShell::MatchingCertificateFile(CertKey);
  CHECK(!AutoCert.isEmpty());
  Data.PublicKeyFile = CertKey;
  {
    // auto-detected <key>-cert.pub
    TestUserInterface UI;
    SecureShell Shell(Data, &UI);
    CHECK(TryOpen(Shell));
  }
  // Key copied elsewhere: no certificate next to it -> rejected
  QString Copy = QDir(ScratchPath).filePath(QStringLiteral("certkey_copy"));
  QFile::remove(Copy);
  QFile::copy(CertKey, Copy);
  QFile::setPermissions(Copy, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  Data.PublicKeyFile = Copy;
  {
    TestUserInterface UI;
    SecureShell Shell(Data, &UI);
    CHECK(!TryOpen(Shell));
  }
  // ... but accepted with an explicit DetachedCertificate
  Data.DetachedCertificate = AutoCert;
  {
    TestUserInterface UI;
    SecureShell Shell(Data, &UI);
    CHECK(TryOpen(Shell));
  }
  // Explicit certificate that is not one: error
  Data.DetachedCertificate = Copy;
  {
    TestUserInterface UI;
    SecureShell Shell(Data, &UI);
    QString Error;
    CHECK(!TryOpen(Shell, &Error));
    CHECK_MSG(Error.contains(QStringLiteral("certificate"), Qt::CaseInsensitive), Error);
  }
}

static void TestInstallPublicKey(SessionData Data, const QString & CaHome, const QString & ScratchPath)
{
  Log(QStringLiteral("install public key into server"));
  TestUserInterface UI;
  SecureShell Shell(Data, &UI);
  if (!TryOpen(Shell))
  {
    CHECK(false);
    return;
  }
  // Safety: only ever into the scratch home of the test server
  sftp_session Sftp = sftp_new(Shell.Session());
  CHECK(sftp_init(Sftp) == SSH_OK);
  char * Home = sftp_canonicalize_path(Sftp, ".");
  QString HomeStr = QString::fromUtf8(Home);
  ssh_string_free_char(Home);
  sftp_free(Sftp);
  if (QDir(HomeStr).canonicalPath() != QDir(CaHome).canonicalPath())
  {
    Log(QStringLiteral("SKIPPED: server home %1 is not %2").arg(HomeStr, CaHome));
    return;
  }
  QDir(CaHome).removeRecursively();
  QDir().mkpath(CaHome);

  QString NewKey = QDir(ScratchPath).filePath(QStringLiteral("install_key"));
  QProcess::execute(QStringLiteral("ssh-keygen"), {QStringLiteral("-q"), QStringLiteral("-t"), QStringLiteral("ed25519"),
    QStringLiteral("-N"), QString(), QStringLiteral("-C"), QStringLiteral("installed@test"), QStringLiteral("-f"), NewKey});
  QStringList Messages;
  QString Result = Shell.InstallPublicKey(NewKey, [&](const QString & M) { Messages.append(M); });
  Log(Messages.join(QStringLiteral(" | ")));
  CHECK_MSG(Result.startsWith(QStringLiteral("**Public key \"installed@test\" was installed.**")), Result);
  QString AuthorizedKeys = CaHome + QStringLiteral("/.ssh/authorized_keys");
  CHECK(QFileInfo(CaHome + QStringLiteral("/.ssh")).isDir());
  CHECK((QFileInfo(CaHome + QStringLiteral("/.ssh")).permissions() & (QFileDevice::WriteGroup | QFileDevice::WriteOther | QFileDevice::ReadOther)) == 0);
  CHECK((QFileInfo(AuthorizedKeys).permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther | QFileDevice::WriteGroup)) == 0);
  QFile File(AuthorizedKeys);
  (void)File.open(QIODevice::ReadOnly);
  QByteArray Content = File.readAll();
  File.close();
  QString Line = SecureShell::PublicKeyLine(NewKey);
  CHECK(Content == Line.toUtf8() + '\n');

  // Second install: already there, file unchanged
  Messages.clear();
  Shell.InstallPublicKey(NewKey, [&](const QString & M) { Messages.append(M); });
  (void)File.open(QIODevice::ReadOnly);
  CHECK(File.readAll() == Content);
  File.close();
  CHECK(Messages.join(QLatin1Char('\n')).contains(QStringLiteral("already contains")));

  // Existing file without trailing new line, with other keys: appended
  (void)File.open(QIODevice::WriteOnly);
  File.write("ssh-rsa AAAAB3otherkey other@host");
  File.close();
  Shell.InstallPublicKey(NewKey);
  (void)File.open(QIODevice::ReadOnly);
  CHECK(File.readAll() == "ssh-rsa AAAAB3otherkey other@host\n" + Line.toUtf8() + '\n');
  File.close();

  // Wrong permissions are reported
  QFile::setPermissions(AuthorizedKeys, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::WriteGroup | QFileDevice::ReadGroup);
  Result = Shell.InstallPublicKey(NewKey);
  CHECK(Result.contains(QStringLiteral("wrong permissions")));
  QDir(CaHome).removeRecursively();
  QDir().mkpath(CaHome);
}

int main(int argc, char ** argv)
{
  QCoreApplication App(argc, argv);
  QTemporaryDir Scratch;
  CHECK(Scratch.isValid());

  if (qEnvironmentVariableIsSet("NIXSCP_TEST_CONFIG_PROBE"))
  {
    SessionData Probe = BaseData(QString(), qEnvironmentVariableIntValue("NIXSCP_TEST_SSH_PORT"),
      qEnvironmentVariable("NIXSCP_TEST_SSH_USER", qEnvironmentVariable("USER")), qEnvironmentVariable("NIXSCP_TEST_SSH_KEY"),
      Scratch.filePath(QStringLiteral("known_hosts")));
    Probe.HostKey = QStringLiteral("*");
    Probe.Timeout = 5;
    if (qEnvironmentVariable("NIXSCP_TEST_CONFIG_PROBE") == QStringLiteral("keys"))
    {
      Probe.HostName = qEnvironmentVariable("NIXSCP_TEST_SSH_HOST", QStringLiteral("127.0.0.1"));
      Probe.PublicKeyFile.clear();
      Probe.TryAgent = true;
    }
    else
    {
      Probe.HostName = QStringLiteral("cfgalias.test");
    }
    return ConfigProbe(Probe);
  }

  TestOffline(Scratch.path());

  QByteArray PortStr = qgetenv("NIXSCP_TEST_SSH_PORT");
  QString KeyFile = qEnvironmentVariable("NIXSCP_TEST_SSH_KEY");
  if (PortStr.isEmpty() || KeyFile.isEmpty())
  {
    std::printf("test_sshlayer: server tests SKIPPED (set NIXSCP_TEST_SSH_PORT, NIXSCP_TEST_SSH_KEY)\n");
  }
  else
  {
    QString Host = qEnvironmentVariable("NIXSCP_TEST_SSH_HOST", QStringLiteral("127.0.0.1"));
    QString User = qEnvironmentVariable("NIXSCP_TEST_SSH_USER", qEnvironmentVariable("USER"));
    QString KnownHosts = Scratch.filePath(QStringLiteral("known_hosts"));
    SessionData Data = BaseData(Host, PortStr.toInt(), User, KeyFile, KnownHosts);
    QString WrongKey = QFileInfo(KeyFile).dir().filePath(QStringLiteral("wrong_client_key"));
    if (!QFileInfo::exists(WrongKey))
    {
      WrongKey = Scratch.filePath(QStringLiteral("wrong_key"));
      QProcess::execute(QStringLiteral("ssh-keygen"), {QStringLiteral("-q"), QStringLiteral("-t"), QStringLiteral("ed25519"),
        QStringLiteral("-N"), QString(), QStringLiteral("-f"), WrongKey});
    }

    TestServerInfo(Data);
    TestWeakAlgorithms(Data);
    TestLogicalHostName(Data, Scratch.path());
    TestProxies(Data);
    TestSshConfigNotRead(Scratch.path(), PortStr.toInt());
    TestProxyCommandExit(Data);
    TestTunnel(Data, qEnvironmentVariable("NIXSCP_TEST_SSH_PASSKEY"), qEnvironmentVariable("NIXSCP_TEST_SSH_PASSPHRASE"));
    TestHostKeyPolicies(Data, Scratch.path());
    TestTryFtp(Host);
    TestStall(Data);
    TestDefaultKeys(Data, Scratch.path());
    TestDefaultKeysLogged(Data, Scratch.path());
    TestAgent(Data, KeyFile, WrongKey, Scratch.path());

    QByteArray CaPort = qgetenv("NIXSCP_TEST_SSH_CA_PORT");
    QString CertKey = qEnvironmentVariable("NIXSCP_TEST_SSH_CERT_KEY");
    QString CaHome = qEnvironmentVariable("NIXSCP_TEST_SSH_CA_HOME");
    if (!CaPort.isEmpty())
    {
      SessionData CaData = BaseData(Host, CaPort.toInt(), User, KeyFile, KnownHosts);
      if (!CertKey.isEmpty())
      {
        TestCertificates(CaData, CertKey, Scratch.path());
      }
      if (!CaHome.isEmpty())
      {
        TestInstallPublicKey(CaData, CaHome, Scratch.path());
      }
    }
    else
    {
      std::printf("test_sshlayer: certificate/install tests SKIPPED (set NIXSCP_TEST_SSH_CA_PORT, _CERT_KEY, _CA_HOME)\n");
    }
  }

  if (Failures > 0)
  {
    std::printf("test_sshlayer: %d FAILURE(S)\n", Failures);
    return 1;
  }
  std::printf("test_sshlayer: OK\n");
  return 0;
}
