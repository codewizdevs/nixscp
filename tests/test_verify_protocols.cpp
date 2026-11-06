// End-to-end verification of the protocols through nixscp-cli and the core
// (protocols-cli verification). Each part runs only when its server is
// configured, otherwise it is SKIPPED:
//   SSH (SFTP, SCP):  NIXSCP_TEST_SSH_PORT, NIXSCP_TEST_SSH_KEY [, _HOST, _USER,
//                     NIXSCP_TEST_SSH_PASSKEY + NIXSCP_TEST_SSH_PASSPHRASE]
//                     (the server must run on this machine)
//   FTP:              NIXSCP_TEST_FTP_PORT (plain; +1 explicit FTPS), NIXSCP_TEST_FTPS_IMPLICIT_PORT,
//                     NIXSCP_TEST_FTP_ROOT (user test/secret, tests/test_ftp.cpp servers)
//   WebDAV:           NIXSCP_TEST_DAV_URL, NIXSCP_TEST_DAVS_URL, NIXSCP_TEST_DAV_DIGEST_URL,
//                     NIXSCP_TEST_DAV_ROOT (user test/secret, tests/test_webdav.cpp servers)
//   S3:               NIXSCP_TEST_S3_ENDPOINT (host:port, plain HTTP), NIXSCP_TEST_S3_ACCESS_KEY,
//                     NIXSCP_TEST_S3_SECRET_KEY, NIXSCP_TEST_S3_BUCKET (an existing bucket)
// The cli binary is NIXSCP_CLI or nixscp-cli next to this test.
//
// Regressions covered:
//  - "get file newname" / "put file newname" go through the core transfer
//    (CopyParam.FileMask on the first level, TCopyParamType::ChangeFileName):
//    renamed transfers keep resume, reconnect and the full overwrite query;
//    the GUI (copy dialog target name, editor upload) relies on it too
//  - script "ls" and "synchronize" reopen a lost connection (CustomReadDirectory)
//  - davs:// and S3 over TLS honor -certificate=* / the configured fingerprint
//  - a cancelled authentication prompt fails "open" with "Authentication failed." /
//    "Credentials were not specified." instead of silently
//  - the parent directory entry without a timestamp (FTP) lists an empty
//    time (mfNone), not "Jan  0 -1:-1:-1"
//  - FTPS (TLS 1.3): an upload is not truncated by a reset of the data
//    connection (unread session tickets when the socket is closed)
//  - SFTP: a cancel with a speed limit stops at once (the pipeline is not
//    filled with the rest of the file first)
//  - the queue transfers with parallel connections over every protocol
#include "core/Configuration.h"
#include "core/CopyParam.h"
#include "core/FileSystem.h"
#include "core/Queue.h"
#include "core/SessionData.h"
#include "core/Terminal.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>
#include <unistd.h>

static int Failures = 0;

#define CHECK(Cond) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); std::fflush(stdout); Failures++; } } while (0)
#define CHECK_MSG(Cond, Msg) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n----\n%s\n----\n", __FILE__, __LINE__, #Cond, qPrintable(Msg)); \
    std::fflush(stdout); Failures++; } } while (0)

static QString ScratchDir;
static QString CliPath;

static void Section(const QString & Name)
{
  std::printf("== %s\n", qPrintable(Name));
  std::fflush(stdout);
}

static void WriteFile(const QString & FileName, const QByteArray & Data, const QDateTime & Time = QDateTime())
{
  QDir().mkpath(QFileInfo(FileName).path());
  QFile F(FileName);
  if (!F.open(QIODevice::WriteOnly | QIODevice::Truncate) || (F.write(Data) != Data.size()))
  {
    std::printf("FAIL: cannot write %s\n", qPrintable(FileName));
    Failures++;
    return;
  }
  F.close();
  if (Time.isValid())
  {
    if (!F.open(QIODevice::ReadWrite) || !F.setFileTime(Time, QFileDevice::FileModificationTime))
    {
      std::printf("FAIL: cannot set time of %s\n", qPrintable(FileName));
      Failures++;
    }
  }
}

static QByteArray RandomData(qint64 Size, quint32 Seed)
{
  QByteArray Data(Size, Qt::Uninitialized);
  QRandomGenerator Gen(Seed);
  for (qint64 I = 0; I < Size; I++)
  {
    Data[I] = static_cast<char>(Gen.generate() & 0xFF);
  }
  return Data;
}

static QByteArray Sha256(const QString & FileName)
{
  QFile F(FileName);
  if (!F.open(QIODevice::ReadOnly))
  {
    return QByteArray("missing");
  }
  QCryptographicHash Hash(QCryptographicHash::Sha256);
  Hash.addData(&F);
  return Hash.result().toHex();
}

// Relative file paths of a tree with their sha256 and (optionally) mtime
static QMap<QString, QString> TreeDigest(const QString & Root, bool Times)
{
  QMap<QString, QString> Result;
  QDirIterator It(Root, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden, QDirIterator::Subdirectories);
  while (It.hasNext())
  {
    QString Path = It.next();
    QFileInfo Info(Path);
    QString Rel = Path.mid(Root.size() + 1);
    if (Info.isDir())
    {
      Result.insert(Rel + QLatin1Char('/'), QString());
      continue;
    }
    QString Value = QString::fromLatin1(Sha256(Path));
    if (Times)
    {
      Value += QLatin1Char(' ') + QString::number(Info.lastModified().toSecsSinceEpoch());
    }
    Result.insert(Rel, Value);
  }
  return Result;
}

static QString DigestStr(const QMap<QString, QString> & Digest)
{
  QStringList Lines;
  for (auto It = Digest.cbegin(); It != Digest.cend(); ++It)
  {
    Lines << (It.key() + QLatin1Char(' ') + It.value());
  }
  return Lines.join(QLatin1Char('\n'));
}

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
    QTimer::singleShot(10, &Loop, &QEventLoop::quit);
    Loop.exec();
  }
  return true;
}

// ------------------------------------------------------------------ TCP proxy

// Forwards a local port to a target port; Drop() closes all live connections
// (a connection lost in the middle of a session). Runs in its own thread, as
// the test blocks in QProcess::waitFor*.
class DropProxy : public QObject
{
public:
  DropProxy(quint16 TargetPort) : FTargetPort(TargetPort)
  {
    moveToThread(&FThread);
    FThread.start();
    QMetaObject::invokeMethod(this, [this]()
    {
      FServer = new QTcpServer(this);
      FServer->listen(QHostAddress::LocalHost, 0);
      FPort = FServer->serverPort();
      QObject::connect(FServer, &QTcpServer::newConnection, this, [this]() { Accept(); });
    }, Qt::BlockingQueuedConnection);
  }
  ~DropProxy() override
  {
    QMetaObject::invokeMethod(this, [this]() { DropNow(); delete FServer; FServer = nullptr; }, Qt::BlockingQueuedConnection);
    FThread.quit();
    FThread.wait();
  }
  quint16 Port() const { return FPort; }
  void Drop()
  {
    QMetaObject::invokeMethod(this, [this]() { DropNow(); }, Qt::BlockingQueuedConnection);
  }

private:
  QThread FThread;
  QTcpServer * FServer = nullptr;
  quint16 FTargetPort;
  quint16 FPort = 0;
  QList<QTcpSocket *> FSockets;

  void Accept()
  {
    while (QTcpSocket * Client = FServer->nextPendingConnection())
    {
      QTcpSocket * Upstream = new QTcpSocket(this);
      Upstream->connectToHost(QHostAddress::LocalHost, FTargetPort);
      if (!Upstream->waitForConnected(5000))
      {
        Client->abort();
        delete Client;
        delete Upstream;
        continue;
      }
      FSockets << Client << Upstream;
      auto Pipe = [](QTcpSocket * From, QTcpSocket * To)
      {
        QObject::connect(From, &QTcpSocket::readyRead, To, [From, To]() { To->write(From->readAll()); });
        QObject::connect(From, &QTcpSocket::disconnected, To, [To]() { To->disconnectFromHost(); });
      };
      Pipe(Client, Upstream);
      Pipe(Upstream, Client);
      Upstream->write(Client->readAll());
    }
  }
  void DropNow()
  {
    for (QTcpSocket * Socket : std::as_const(FSockets))
    {
      Socket->abort();
      Socket->deleteLater();
    }
    FSockets.clear();
  }
};

// ------------------------------------------------------------------ targets

struct Target
{
  QString Label;
  QString Url;            // session URL for nixscp-cli "open" and SessionData::ParseUrl
  QString Switches;       // additional "open" switches
  QString RemoteRoot;     // writable remote directory (absolute)
  QString LocalRoot;      // the same directory on this machine (empty: not visible)
  bool ExactTimes = true; // listing has full precision timestamps (not LIST, not S3)
  bool PreserveTimes = true;
  quint16 ProxyTarget = 0; // port for a DropProxy (0: no reconnect test)
};

static QString OpenCommand(const Target & T, const QString & Url = QString())
{
  return QStringLiteral("open %1 %2").arg(Url.isEmpty() ? T.Url : Url, T.Switches);
}

struct RunResult
{
  int ExitCode = -1;
  QString Output;
};

static QProcessEnvironment CliEnvironment()
{
  QProcessEnvironment Env = QProcessEnvironment::systemEnvironment();
  Env.insert(QStringLiteral("XDG_CONFIG_HOME"), ScratchDir + QStringLiteral("/xdg"));
  Env.insert(QStringLiteral("NIXSCP_KNOWN_HOSTS"), ScratchDir + QStringLiteral("/known_hosts"));
#ifdef Q_OS_MACOS
  // no C.UTF-8 there, and Qt says so on stderr
  Env.insert(QStringLiteral("LC_ALL"), QStringLiteral("en_US.UTF-8"));
#else
  Env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C.UTF-8"));
#endif
  Env.remove(QStringLiteral("SSH_AUTH_SOCK"));
  return Env;
}

static RunResult RunCli(const QStringList & Commands, const QString & WorkDir = QString())
{
  QProcess P;
  P.setProgram(CliPath);
  QStringList Args{QStringLiteral("/ini=nul"), QStringLiteral("/command"), QStringLiteral("option batch abort"),
    QStringLiteral("option confirm off")};
  Args << Commands << QStringLiteral("exit");
  P.setArguments(Args);
  P.setProcessChannelMode(QProcess::MergedChannels);
  P.setProcessEnvironment(CliEnvironment());
  P.setWorkingDirectory(WorkDir.isEmpty() ? ScratchDir : WorkDir);
  P.start();
  P.closeWriteChannel();
  RunResult Result;
  if (!P.waitForFinished(300000))
  {
    P.kill();
    P.waitForFinished();
  }
  Result.ExitCode = (P.exitStatus() == QProcess::NormalExit) ? P.exitCode() : -1;
  Result.Output = QString::fromUtf8(P.readAll());
  return Result;
}

// ------------------------------------------------------------------ cli scenario

static void TestCliTransfers(const Target & T)
{
  Section(T.Label + QStringLiteral(": cli tree transfer, sha256 + mtime"));
  QString Work = ScratchDir + QStringLiteral("/cli-") + T.Label;
  QString Src = Work + QStringLiteral("/src");
  QString Dl = Work + QStringLiteral("/dl");
  QDir().mkpath(Dl);
  QDateTime T1 = QDateTime::fromSecsSinceEpoch(1577934245);  // 2020-01-02
  QDateTime T2 = QDateTime::fromSecsSinceEpoch(1557126489);  // 2019-05-06
  WriteFile(Src + QStringLiteral("/big.bin"), RandomData(300000, 1), T1);
  WriteFile(Src + QStringLiteral("/a.txt"), "hello\n", T1);
  WriteFile(Src + QStringLiteral("/sp ace.txt"), "space name\n", T2);
  WriteFile(Src + QStringLiteral("/empty.txt"), QByteArray(), T2);
  WriteFile(Src + QStringLiteral("/sub/b.txt"), QByteArray(1000, 'x'), T2);
  WriteFile(Src + QStringLiteral("/sub/deep/c.txt"), "deep\n", T1);
  QString Remote = T.RemoteRoot + QStringLiteral("/vp-%1-%2").arg(T.Label).arg(getpid());

  RunResult R = RunCli({OpenCommand(T), QStringLiteral("mkdir \"%1\"").arg(Remote), QStringLiteral("cd \"%1\"").arg(Remote),
    QStringLiteral("lcd \"%1\"").arg(Src), QStringLiteral("put -preservetime *"),
    QStringLiteral("lcd \"%1\"").arg(Dl), QStringLiteral("get -preservetime *"), QStringLiteral("ls")});
  CHECK_MSG(R.ExitCode == 0, R.Output);
  // mfNone parent directory entry
  CHECK_MSG(!R.Output.contains(QStringLiteral("-1:-1")), R.Output);
  CHECK_MSG(R.Output.contains(QStringLiteral("sp ace.txt")), R.Output);
  bool Times = T.ExactTimes && T.PreserveTimes;
  QMap<QString, QString> Expected = TreeDigest(Src, Times);
  CHECK_MSG(TreeDigest(Dl, Times) == Expected, DigestStr(Expected) + QStringLiteral("\n--\n") + DigestStr(TreeDigest(Dl, Times)));
  if (!T.LocalRoot.isEmpty())
  {
    QString RemoteLocal = T.LocalRoot + Remote.mid(T.RemoteRoot.size());
    QMap<QString, QString> OnServer = TreeDigest(RemoteLocal, T.PreserveTimes);
    QMap<QString, QString> ExpectedOnServer = TreeDigest(Src, T.PreserveTimes);
    CHECK_MSG(OnServer == ExpectedOnServer, DigestStr(ExpectedOnServer) + QStringLiteral("\n--\n") + DigestStr(OnServer));
  }

  Section(T.Label + QStringLiteral(": cli transfer under a new name"));
  WriteFile(Work + QStringLiteral("/part.bin"), RandomData(300000, 1).left(100000));
  R = RunCli({OpenCommand(T), QStringLiteral("cd \"%1\"").arg(Remote), QStringLiteral("lcd \"%1\"").arg(Work),
    QStringLiteral("put -preservetime src/a.txt renamed.txt"), QStringLiteral("put src/big.bin renamed.bin"),
    QStringLiteral("get renamed.txt back.txt"), QStringLiteral("get -resume renamed.bin part.bin"),
    QStringLiteral("put src/sub subren"), QStringLiteral("get subren backsub"), QStringLiteral("ls")});
  CHECK_MSG(R.ExitCode == 0, R.Output);
  CHECK_MSG(R.Output.contains(QStringLiteral(" renamed.txt\n")) && R.Output.contains(QStringLiteral(" subren\n")), R.Output);
  CHECK(Sha256(Work + QStringLiteral("/back.txt")) == Sha256(Src + QStringLiteral("/a.txt")));
  CHECK(Sha256(Work + QStringLiteral("/part.bin")) == Sha256(Src + QStringLiteral("/big.bin")));
  CHECK(TreeDigest(Work + QStringLiteral("/backsub"), false) == TreeDigest(Src + QStringLiteral("/sub"), false));
  if (!T.LocalRoot.isEmpty() && T.PreserveTimes)
  {
    QFileInfo Renamed(T.LocalRoot + Remote.mid(T.RemoteRoot.size()) + QStringLiteral("/renamed.txt"));
    CHECK_MSG(Renamed.lastModified() == T1, Renamed.lastModified().toString(Qt::ISODate));
  }

  Section(T.Label + QStringLiteral(": cli synchronize"));
  QString SyncL = Work + QStringLiteral("/syncl");
  QString SyncL2 = Work + QStringLiteral("/syncl2");
  WriteFile(SyncL + QStringLiteral("/one.txt"), "one\n");
  WriteFile(SyncL + QStringLiteral("/d/two.txt"), "two\n");
  QDir().mkpath(SyncL2);
  QString RSync = Remote + QStringLiteral("/sync");
  R = RunCli({OpenCommand(T), QStringLiteral("mkdir \"%1\"").arg(RSync),
    QStringLiteral("synchronize remote \"%1\" \"%2\"").arg(SyncL, RSync),
    QStringLiteral("synchronize local \"%1\" \"%2\"").arg(SyncL2, RSync)});
  CHECK_MSG(R.ExitCode == 0, R.Output);
  CHECK(TreeDigest(SyncL2, false) == TreeDigest(SyncL, false));
  QFile::remove(SyncL + QStringLiteral("/one.txt"));
  WriteFile(SyncL + QStringLiteral("/three.txt"), "three\n");
  R = RunCli({OpenCommand(T), QStringLiteral("synchronize remote -delete -preview \"%1\" \"%2\"").arg(SyncL, RSync),
    QStringLiteral("synchronize remote -delete \"%1\" \"%2\"").arg(SyncL, RSync),
    QStringLiteral("synchronize local -delete \"%1\" \"%2\"").arg(SyncL2, RSync),
    QStringLiteral("synchronize both \"%1\" \"%2\"").arg(SyncL, RSync)});
  CHECK_MSG((R.ExitCode == 0) && R.Output.contains(QStringLiteral("Orphan remote file")), R.Output);
  CHECK_MSG(TreeDigest(SyncL2, false) == TreeDigest(SyncL, false), DigestStr(TreeDigest(SyncL2, false)));
  CHECK(R.Output.contains(QStringLiteral("Nothing to synchronize.")));

  RunResult Cleanup = RunCli({OpenCommand(T), QStringLiteral("rm \"%1\"").arg(Remote)});
  CHECK_MSG(Cleanup.ExitCode == 0, Cleanup.Output);
}

// Commands written to stdin one by one; the connection is dropped by the
// proxy after the first block, the second block must reopen it
static void TestCliReconnect(const Target & T)
{
  if (T.ProxyTarget == 0)
  {
    return;
  }
  Section(T.Label + QStringLiteral(": cli reconnect after a lost connection"));
  DropProxy Proxy(T.ProxyTarget);
  QUrl Url(T.Url);
  Url.setPort(Proxy.Port());
  QString Work = ScratchDir + QStringLiteral("/rc-") + T.Label;
  WriteFile(Work + QStringLiteral("/up.bin"), RandomData(200000, 5));
  WriteFile(Work + QStringLiteral("/sync/s.txt"), "s\n");
  QString Remote = T.RemoteRoot + QStringLiteral("/rc-%1-%2").arg(T.Label).arg(getpid());

  struct Step
  {
    QString Command;   // run after the connection is dropped
    QString Check;     // expected in the output afterwards
  };
  const QList<Step> Steps{
    {QStringLiteral("ls"), QStringLiteral(" up.bin\n")},
    {QStringLiteral("put up.bin new.bin"), QString()},
    {QStringLiteral("synchronize remote \"%1/sync\" .").arg(Work), QString()},
    {QStringLiteral("put up.bin"), QString()},
  };
  for (const Step & S : Steps)
  {
    QProcess P;
    P.setProgram(CliPath);
    P.setArguments({QStringLiteral("/ini=nul"), QStringLiteral("/command"), OpenCommand(T, Url.toString())});
    P.setProcessChannelMode(QProcess::MergedChannels);
    P.setProcessEnvironment(CliEnvironment());
    P.setWorkingDirectory(Work);
    P.start();
    CHECK(P.waitForStarted(10000));
    P.write(QStringLiteral("option batch abort\noption confirm off\nmkdir \"%1\"\ncd \"%1\"\nput up.bin\necho marker1\n").arg(Remote).toUtf8());
    QByteArray Out;
    QElapsedTimer Timer;
    Timer.start();
    while (!Out.contains("marker1") && (Timer.elapsed() < 60000) && (P.state() == QProcess::Running))
    {
      P.waitForReadyRead(200);
      Out += P.readAll();
    }
    CHECK_MSG(Out.contains("marker1"), QString::fromUtf8(Out));
    Proxy.Drop();
    QThread::msleep(500);
    P.write(QStringLiteral("%1\nls\necho marker2\nrm \"%2\"\nexit\n").arg(S.Command, Remote).toUtf8());
    P.closeWriteChannel();
    if (!P.waitForFinished(120000))
    {
      P.kill();
      P.waitForFinished();
    }
    Out += P.readAll();
    QString Output = QString::fromUtf8(Out);
    CHECK_MSG((P.exitStatus() == QProcess::NormalExit) && (P.exitCode() == 0), S.Command + QStringLiteral("\n") + Output);
    CHECK_MSG(Output.contains(QStringLiteral("marker2")) && !Output.contains(QStringLiteral("' closed.")), S.Command + QStringLiteral("\n") + Output);
    CHECK_MSG(S.Check.isEmpty() || Output.contains(S.Check), Output);
  }
}

static void TestCliAuthentication(const Target & T, const QString & PassKey)
{
  Section(QStringLiteral("cli: cancelled authentication prompts"));
  QString Url = T.Url;
  QString Base = QStringLiteral("open %1 -hostkey=* -privatekey=\"%2\"").arg(Url, PassKey);
  RunResult R = RunCli({Base + QStringLiteral(" -passphrase=wrong"), QStringLiteral("pwd")});
  CHECK_MSG((R.ExitCode == 1) && R.Output.contains(QStringLiteral("Authentication failed.")), R.Output);
  R = RunCli({Base, QStringLiteral("pwd")});
  CHECK_MSG((R.ExitCode == 1) && R.Output.contains(QStringLiteral("Credentials were not specified.")), R.Output);
  R = RunCli({Base + QStringLiteral(" -passphrase=") + qEnvironmentVariable("NIXSCP_TEST_SSH_PASSPHRASE", QStringLiteral("secret")),
    QStringLiteral("pwd")});
  CHECK_MSG(R.ExitCode == 0, R.Output);
}

// ------------------------------------------------------------------ core

class TestUserInterface : public UserInterface
{
public:
  HostKeyAnswer VerifyHostKey(const QString &, int, const QString &, const QString &, const QString &, HostKeyState) override
  {
    return HostKeyOnce;
  }
  bool PromptUser(PromptKind, const QString &, const QString &, QList<Prompt> &) override { return false; }
  void DisplayBanner(const QString &, const QString &) override {}
  unsigned int QueryUser(const QString & Query, const QString &, unsigned int) override
  {
    // certificates must be accepted through SessionData::HostKey ("*")
    std::printf("FAIL unexpected query: %s\n", qPrintable(Query.left(200)));
    Failures++;
    return qaCancel;
  }
};

static SessionData CoreSessionData(const Target & T)
{
  SessionData Data;
  int Parsed = 0;
  Data.ParseUrl(T.Url, Parsed);
  if (T.Switches.contains(QStringLiteral("-privatekey=")))
  {
    QString Key = T.Switches.mid(T.Switches.indexOf(QStringLiteral("-privatekey=")) + 12);
    Data.PublicKeyFile = Key.section(QLatin1Char(' '), 0, 0).remove(QLatin1Char('"'));
  }
  Data.TryAgent = false;
  Data.AuthKI = false;
  Data.HostKey = QStringLiteral("*");
  Data.KnownHostsFile = ScratchDir + QStringLiteral("/known_hosts");
  Data.Timeout = 15;
  return Data;
}

static OperationResult Sync(Terminal & Term, const std::function<void(Terminal::Callback)> & Start)
{
  OperationResult Result;
  bool Done = false;
  Start([&](const OperationResult & R) { Result = R; Done = true; });
  if (!WaitUntil([&]() { return Done; }, 180000))
  {
    Result.Ok = false;
    Result.Message = QStringLiteral("timeout");
  }
  return Result;
}

static void TestCore(const Target & T)
{
  Section(T.Label + QStringLiteral(": core FileMask on the first level, queue with parallel connections"));
  TestUserInterface UI;
  SessionData Data = CoreSessionData(T);
  QString Work = ScratchDir + QStringLiteral("/core-") + T.Label;
  QString Remote = T.RemoteRoot + QStringLiteral("/core-%1-%2").arg(T.Label).arg(getpid());
  WriteFile(Work + QStringLiteral("/src/file.txt"), "masked\n");
  {
    Terminal Term(Data, &UI);
    OperationResult R = Sync(Term, [&](Terminal::Callback Done) { Term.Open(Done); });
    CHECK_MSG(R.Ok, R.Message + QLatin1Char('\n') + R.MoreMessages);
    if (!R.Ok)
    {
      return;
    }
    R = Sync(Term, [&](Terminal::Callback Done) { Term.CreateDirectory(Remote, 0755, Done); });
    CHECK_MSG(R.Ok, R.Message);
    CopyParam Param;
    Param.FileMask = QStringLiteral("other.txt");
    R = Sync(Term, [&](Terminal::Callback Done) { Term.CopyToRemote({Work + QStringLiteral("/src/file.txt")}, Remote, Param, false, Done); });
    CHECK_MSG(R.Ok, R.Message);
    Param.FileMask = QStringLiteral("*.bak");
    R = Sync(Term, [&](Terminal::Callback Done) { Term.CopyToLocal({Remote + QStringLiteral("/other.txt")}, Work, Param, false, Done); });
    CHECK_MSG(R.Ok, R.Message);
    CHECK_MSG(QFile::exists(Work + QStringLiteral("/other.bak")), QDir(Work).entryList().join(QLatin1Char(' ')));
    CHECK(Sha256(Work + QStringLiteral("/other.bak")) == Sha256(Work + QStringLiteral("/src/file.txt")));
    // the listing is reopened after a lost connection too: not testable without
    // a proxy here, covered by TestCliReconnect

    if (!T.LocalRoot.isEmpty() && T.Label.startsWith(QStringLiteral("ftp")))
    {
      // TLS 1.3 session tickets arriving after the whole file was written
      // used to make the close of the data connection a reset: the server
      // truncated the upload (at a multiple of 16 KB), reported as complete
      WriteFile(Work + QStringLiteral("/trunc/t.bin"), RandomData(300000, 77));
      CopyParam Plain;
      Plain.ConfirmOverwriting = false;
      int Bad = 0;
      for (int Attempt = 0; Attempt < 12; Attempt++)
      {
        R = Sync(Term, [&](Terminal::Callback Done) { Term.CopyToRemote({Work + QStringLiteral("/trunc/t.bin")}, Remote, Plain, false, Done); });
        QString OnServer = T.LocalRoot + Remote.mid(T.RemoteRoot.size()) + QStringLiteral("/t.bin");
        if (!R.Ok || (Sha256(OnServer) != Sha256(Work + QStringLiteral("/trunc/t.bin"))))
        {
          Bad++;
        }
      }
      CHECK_MSG(Bad == 0, QString::number(Bad));
    }
  }

  QString Tree = Work + QStringLiteral("/tree");
  for (int Dir = 0; Dir < 3; Dir++)
  {
    for (int Index = 0; Index < 6; Index++)
    {
      WriteFile(Tree + QStringLiteral("/d%1/f%2.bin").arg(Dir).arg(Index), RandomData(200000 + Index * 777, 10 * Dir + Index));
    }
  }
  QDir().mkpath(Tree + QStringLiteral("/empty"));
  TransferQueue Q(Data, &UI);
  Q.SetKeepDoneItemsFor(-1);
  QList<OperationResult> Results;
  int MaxConnections = 0;
  QObject::connect(&Q, &TransferQueue::ItemFinished, &Q, [&](int, const OperationResult & R, bool, const QString &, bool)
  {
    Results.append(R);
  });
  auto Sample = [&]()
  {
    for (const QueueItemInfo & Info : Q.Items())
    {
      MaxConnections = std::max(MaxConnections, Info.Connections);
    }
    return false;
  };
  QObject::connect(&Q, &TransferQueue::Changed, &Q, [&]() { Sample(); });
  // two connections opened and idle first: the second one joins the
  // parallel transfer at once (TryAddParallelOperation with FFreeTerminals > 0)
  WriteFile(Work + QStringLiteral("/warm1.bin"), RandomData(3000000, 21));
  WriteFile(Work + QStringLiteral("/warm2.bin"), RandomData(3000000, 22));
  CopyParam Plain;
  Q.AddUpload({Work + QStringLiteral("/warm1.bin")}, Remote, Plain, false);
  Q.AddUpload({Work + QStringLiteral("/warm2.bin")}, Remote, Plain, false);
  CHECK(WaitUntil([&]() { Sample(); return Results.size() == 2; }, 180000));
  CHECK_MSG(std::all_of(Results.cbegin(), Results.cend(), [](const OperationResult & R) { return R.Ok; }), Results.value(0).Message);
  Results.clear();
  MaxConnections = 0;
  CopyParam Parallel;
  Parallel.QueueParallel = true;
  Q.AddUpload({Tree}, Remote, Parallel, false);
  CHECK(WaitUntil([&]() { Sample(); return Results.size() == 1; }, 180000));
  CHECK_MSG(!Results.isEmpty() && Results.first().Ok, Results.isEmpty() ? QString() : Results.first().Message);
  // SCP cannot transfer in parallel (fcParallelTransfers / TSCPFileSystem)
  bool CanParallel = (T.Label != QStringLiteral("scp"));
  CHECK_MSG(MaxConnections == (CanParallel ? 2 : 1), QString::number(MaxConnections));
  int UploadConnections = MaxConnections;
  MaxConnections = 0;
  QString Back = Work + QStringLiteral("/back");
  QDir().mkpath(Back);
  Q.AddDownload({Remote + QStringLiteral("/tree")}, Back, Parallel, false);
  CHECK(WaitUntil([&]() { Sample(); return Results.size() == 2; }, 180000));
  CHECK_MSG((Results.size() == 2) && Results[1].Ok, (Results.size() == 2) ? Results[1].Message : QString());
  CHECK_MSG(TreeDigest(Back + QStringLiteral("/tree"), false) == TreeDigest(Tree, false), DigestStr(TreeDigest(Back + QStringLiteral("/tree"), false)));
  std::printf("  queue: %d/%d connection(s) at most (upload/download)\n", UploadConnections, MaxConnections);
  CHECK_MSG(MaxConnections == (CanParallel ? 2 : 1), QString::number(MaxConnections));
  Q.AddDelete({Remote}, true);
  CHECK(WaitUntil([&]() { return Results.size() == 3; }, 60000));
  CHECK((Results.size() == 3) && Results[2].Ok);
}

static void TestSftpCancelWithSpeedLimit(const Target & T)
{
  Section(T.Label + QStringLiteral(": cancel with a speed limit stops at once"));
  TestUserInterface UI;
  SessionData Data = CoreSessionData(T);
  QString Work = ScratchDir + QStringLiteral("/cancel-") + T.Label;
  WriteFile(Work + QStringLiteral("/big.bin"), RandomData(3000000, 9));
  QString Remote = T.RemoteRoot + QStringLiteral("/cancel-%1-%2").arg(T.Label).arg(getpid());
  QDir().mkpath(T.LocalRoot + Remote.mid(T.RemoteRoot.size()));
  Terminal Term(Data, &UI);
  OperationResult R = Sync(Term, [&](Terminal::Callback Done) { Term.Open(Done); });
  CHECK_MSG(R.Ok, R.Message);
  if (!R.Ok)
  {
    return;
  }
  CopyParam Param;
  Param.CPSLimit = 200 * 1024;
  bool Done = false;
  Term.CopyToRemote({Work + QStringLiteral("/big.bin")}, Remote, Param, false, [&](const OperationResult & Res) { R = Res; Done = true; });
  WaitUntil([]() { return false; }, 1500);
  Term.Progress().Cancel();
  CHECK(WaitUntil([&]() { return Done; }, 30000));
  CHECK(!R.Ok);
  qint64 Uploaded = 0;
  for (const QFileInfo & Info : QDir(T.LocalRoot + Remote.mid(T.RemoteRoot.size())).entryInfoList(QDir::Files))
  {
    Uploaded = std::max(Uploaded, Info.size());
  }
  CHECK_MSG(Uploaded < 1500000, QString::number(Uploaded));
  QDir(T.LocalRoot + Remote.mid(T.RemoteRoot.size())).removeRecursively();
}

// ------------------------------------------------------------------ main

int main(int argc, char ** argv)
{
  QCoreApplication App(argc, argv);
  QTemporaryDir Scratch;
  CHECK(Scratch.isValid());
  ScratchDir = QDir(Scratch.path()).canonicalPath();
  QDir(ScratchDir).mkpath(QStringLiteral("xdg"));
  Configuration::Instance().SetIniFileName(QStringLiteral("nul"));

  CliPath = qEnvironmentVariable("NIXSCP_CLI");
  if (CliPath.isEmpty())
  {
    CliPath = QCoreApplication::applicationDirPath() + QStringLiteral("/nixscp-cli");
  }
  bool HaveCli = QFileInfo(CliPath).isExecutable();
  if (!HaveCli)
  {
    std::printf("test_verify_protocols: nixscp-cli not found at %s, cli parts SKIPPED (set NIXSCP_CLI)\n", qPrintable(CliPath));
  }

  QList<Target> Targets;
  QString SshPort = qEnvironmentVariable("NIXSCP_TEST_SSH_PORT");
  QString SshKey = qEnvironmentVariable("NIXSCP_TEST_SSH_KEY");
  QString SshHost = qEnvironmentVariable("NIXSCP_TEST_SSH_HOST", QStringLiteral("127.0.0.1"));
  QString SshUser = qEnvironmentVariable("NIXSCP_TEST_SSH_USER", qEnvironmentVariable("USER"));
  Target Sftp;
  if (!SshPort.isEmpty() && !SshKey.isEmpty() && ((SshHost == QStringLiteral("127.0.0.1")) || (SshHost == QStringLiteral("localhost"))))
  {
    QString Root = ScratchDir + QStringLiteral("/sshroot");
    QDir().mkpath(Root);
    for (const QString & Protocol : {QStringLiteral("sftp"), QStringLiteral("scp")})
    {
      Target T;
      T.Label = Protocol;
      T.Url = QStringLiteral("%1://%2@%3:%4/").arg(Protocol, SshUser, SshHost, SshPort);
      T.Switches = QStringLiteral("-hostkey=* -privatekey=\"%1\"").arg(SshKey);
      T.RemoteRoot = Root;
      T.LocalRoot = Root;
      T.ProxyTarget = static_cast<quint16>(SshPort.toInt());
      Targets << T;
    }
    Sftp = Targets.first();
  }
  else
  {
    std::printf("test_verify_protocols: SSH part SKIPPED (set NIXSCP_TEST_SSH_PORT, NIXSCP_TEST_SSH_KEY, local server)\n");
  }

  QString FtpPort = qEnvironmentVariable("NIXSCP_TEST_FTP_PORT");
  QString FtpRoot = qEnvironmentVariable("NIXSCP_TEST_FTP_ROOT");
  if (!FtpPort.isEmpty() && !FtpRoot.isEmpty())
  {
    int Port = FtpPort.toInt();
    QString Implicit = qEnvironmentVariable("NIXSCP_TEST_FTPS_IMPLICIT_PORT", QString::number(Port + 2));
    QString ListPort = qEnvironmentVariable("NIXSCP_TEST_FTP_LIST_PORT");
    struct { const char * Label; QString Url; QString Switches; bool Exact; } Ftps[] = {
      {"ftp", QStringLiteral("ftp://test:secret@127.0.0.1:%1/").arg(Port), QString(), true},
      {"ftpes", QStringLiteral("ftpes://test:secret@127.0.0.1:%1/").arg(qEnvironmentVariable("NIXSCP_TEST_FTPS_PORT", QString::number(Port + 1))),
        QStringLiteral("-certificate=*"), true},
      {"ftps", QStringLiteral("ftps://test:secret@127.0.0.1:%1/").arg(Implicit), QStringLiteral("-certificate=*"), true},
      {"ftplist", ListPort.isEmpty() ? QString() : QStringLiteral("ftp://test:secret@127.0.0.1:%1/").arg(ListPort), QString(), false},
    };
    for (const auto & F : Ftps)
    {
      if (F.Url.isEmpty())
      {
        continue;
      }
      Target T;
      T.Label = QString::fromLatin1(F.Label);
      T.Url = F.Url;
      T.Switches = F.Switches;
      T.RemoteRoot = QString();
      T.LocalRoot = QDir(FtpRoot).canonicalPath();
      T.ExactTimes = F.Exact;
      T.ProxyTarget = (T.Label == QStringLiteral("ftp")) ? static_cast<quint16>(Port) : 0;
      Targets << T;
    }
  }
  else
  {
    std::printf("test_verify_protocols: FTP part SKIPPED (set NIXSCP_TEST_FTP_PORT, NIXSCP_TEST_FTP_ROOT)\n");
  }

  QString DavRoot = qEnvironmentVariable("NIXSCP_TEST_DAV_ROOT");
  if (!qEnvironmentVariable("NIXSCP_TEST_DAV_URL").isEmpty() && !DavRoot.isEmpty())
  {
    struct { const char * Label; const char * Env; } Davs[] = {
      {"dav", "NIXSCP_TEST_DAV_URL"}, {"davs", "NIXSCP_TEST_DAVS_URL"}, {"davdigest", "NIXSCP_TEST_DAV_DIGEST_URL"}};
    for (const auto & D : Davs)
    {
      QUrl Url(qEnvironmentVariable(D.Env));
      if (!Url.isValid() || Url.host().isEmpty())
      {
        continue;
      }
      bool Https = (Url.scheme() == QStringLiteral("https"));
      Target T;
      T.Label = QString::fromLatin1(D.Label);
      T.Url = QStringLiteral("%1://test:secret@%2:%3/").arg(Https ? QStringLiteral("davs") : QStringLiteral("dav"), Url.host()).arg(Url.port());
      T.Switches = Https ? QStringLiteral("-certificate=*") : QString();
      T.RemoteRoot = QString();
      T.LocalRoot = QDir(DavRoot).canonicalPath();
      T.ProxyTarget = (T.Label == QStringLiteral("dav")) ? static_cast<quint16>(Url.port()) : 0;
      Targets << T;
    }
  }
  else
  {
    std::printf("test_verify_protocols: WebDAV part SKIPPED (set NIXSCP_TEST_DAV_URL, NIXSCP_TEST_DAV_ROOT)\n");
  }

  QString S3Endpoint = qEnvironmentVariable("NIXSCP_TEST_S3_ENDPOINT");
  QString S3Bucket = qEnvironmentVariable("NIXSCP_TEST_S3_BUCKET");
  if (!S3Endpoint.isEmpty() && !S3Bucket.isEmpty())
  {
    Target T;
    T.Label = QStringLiteral("s3");
    T.Url = QStringLiteral("s3plain://%1:%2@%3/").arg(
      QString::fromLatin1(QUrl::toPercentEncoding(qEnvironmentVariable("NIXSCP_TEST_S3_ACCESS_KEY", QStringLiteral("minioadmin")))),
      QString::fromLatin1(QUrl::toPercentEncoding(qEnvironmentVariable("NIXSCP_TEST_S3_SECRET_KEY", QStringLiteral("minioadmin")))),
      S3Endpoint);
    T.RemoteRoot = QLatin1Char('/') + S3Bucket;
    T.ExactTimes = false;
    T.PreserveTimes = false;
    T.ProxyTarget = static_cast<quint16>(S3Endpoint.section(QLatin1Char(':'), -1).toInt());
    Targets << T;
  }
  else
  {
    std::printf("test_verify_protocols: S3 part SKIPPED (set NIXSCP_TEST_S3_ENDPOINT, NIXSCP_TEST_S3_BUCKET)\n");
  }

  QString Only = qEnvironmentVariable("NIXSCP_TEST_VERIFY_ONLY");
  for (const Target & T : std::as_const(Targets))
  {
    if (!Only.isEmpty() && !Only.split(QLatin1Char(',')).contains(T.Label))
    {
      continue;
    }
    if (HaveCli)
    {
      TestCliTransfers(T);
      TestCliReconnect(T);
    }
    TestCore(T);
  }
  if (!Sftp.Label.isEmpty() && (Only.isEmpty() || Only.split(QLatin1Char(',')).contains(QStringLiteral("sftp"))))
  {
    TestSftpCancelWithSpeedLimit(Sftp);
    QString PassKey = qEnvironmentVariable("NIXSCP_TEST_SSH_PASSKEY");
    if (HaveCli && !PassKey.isEmpty())
    {
      TestCliAuthentication(Sftp, PassKey);
    }
  }

  if (Failures > 0)
  {
    std::printf("test_verify_protocols: %d FAILURE(S)\n", Failures);
    return 1;
  }
  std::printf("test_verify_protocols: all tests passed\n");
  return 0;
}
