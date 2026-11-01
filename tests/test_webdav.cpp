// End-to-end test of core/Terminal + core/WebDavFileSystem against WebDAV servers.
//
// Skipped (exit 0) unless NIXSCP_TEST_DAV_URL is set. Environment:
//   NIXSCP_TEST_DAV_URL          http:// URL of a server with Basic authentication
//                                (required to run); its path is the session's
//                                remote directory
//   NIXSCP_TEST_DAV_USER         default "test"
//   NIXSCP_TEST_DAV_PASSWORD     default "secret"
//   NIXSCP_TEST_DAV_ROOT         optional: local path of the server's root
//                                (server on this machine), uploads are then
//                                verified on the server side too
//   NIXSCP_TEST_DAVS_URL         optional: https:// URL of a server with a
//                                self-signed certificate (O=NixSCP Test)
//   NIXSCP_TEST_DAV_DIGEST_URL   optional: server that offers Basic and Digest
//                                but accepts Digest only, with expiring nonces
// The test servers expect the helper paths /redirect/<path> (301 to /<path>) and
// /cross-redirect/<path> (302 to another host). A Python server providing all of
// this without root is described in the NixSCP WebDAV notes.
#include "core/Terminal.h"
#include "core/WebDavFileSystem.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QPointer>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>

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
  QStringList PromptAnswers;
  QList<unsigned int> QueryAnswers;
  QList<PromptKind> PromptKinds;
  QStringList PromptInstructions;
  QStringList Queries;

  void Reset()
  {
    QMutexLocker Lock(&Mutex);
    PromptAnswers.clear();
    QueryAnswers.clear();
    PromptKinds.clear();
    PromptInstructions.clear();
    Queries.clear();
  }

  HostKeyAnswer VerifyHostKey(const QString &, int, const QString &, const QString &, const QString &, HostKeyState) override
  {
    return HostKeyCancel;
  }

  bool PromptUser(PromptKind Kind, const QString & Name, const QString & Instructions, QList<Prompt> & Prompts) override
  {
    QMutexLocker Lock(&Mutex);
    PromptKinds.append(Kind);
    PromptInstructions.append(Instructions);
    Log(QStringLiteral("PromptUser kind=%1 name=%2 instructions=\"%3\"").arg(Kind).arg(Name, Instructions));
    for (Prompt & P : Prompts)
    {
      if (PromptAnswers.isEmpty())
      {
        return false;
      }
      P.Result = PromptAnswers.takeFirst();
    }
    return true;
  }

  void DisplayBanner(const QString &, const QString &) override
  {
  }

  unsigned int QueryUser(const QString & Query, const QString &, unsigned int Answers) override
  {
    QMutexLocker Lock(&Mutex);
    Queries.append(Query);
    unsigned int Answer = QueryAnswers.isEmpty() ? static_cast<unsigned int>(qaCancel) : QueryAnswers.takeFirst();
    Log(QStringLiteral("QueryUser \"%1...\" -> %2").arg(Query.left(60).replace(QLatin1Char('\n'), QLatin1Char(' '))).arg(Answer));
    if ((Answer & Answers) == 0)
    {
      Log(QStringLiteral("  answer not among offered answers %1").arg(Answers));
    }
    return Answer;
  }
};

struct Waiter
{
  QEventLoop Loop;
  OperationResult Result;
  QString Directory;
  RemoteFileList Files;
  bool Called = false;

  Terminal::Callback Cb()
  {
    return [this](const OperationResult & R)
    {
      Result = R;
      Called = true;
      Loop.quit();
    };
  }
  Terminal::ListCallback ListCb()
  {
    return [this](const OperationResult & R, const QString & Dir, const RemoteFileList & List)
    {
      Result = R;
      Directory = Dir;
      Files = List;
      Called = true;
      Loop.quit();
    };
  }
  OperationResult Wait(int TimeoutMs = 300000)
  {
    if (!Called)
    {
      QTimer::singleShot(TimeoutMs, &Loop, [this]() { std::printf("FAIL: operation timed out\n"); Failures++; Loop.quit(); });
      Loop.exec();
    }
    return Result;
  }
};

static QString Describe(const OperationResult & R)
{
  return QStringLiteral("ok=%1 aborted=%2 fatal=%3 msg=\"%4\" more=\"%5\"")
    .arg(R.Ok).arg(R.Aborted).arg(R.Fatal).arg(R.Message, R.MoreMessages);
}

static OperationResult DoOpen(Terminal & T)
{
  Waiter W;
  T.Open(W.Cb());
  return W.Wait();
}

static OperationResult DoList(Terminal & T, const QString & Path, RemoteFileList * Files = nullptr, QString * Dir = nullptr)
{
  Waiter W;
  T.ReadDirectory(Path, W.ListCb());
  W.Wait();
  if (Files != nullptr) *Files = W.Files;
  if (Dir != nullptr) *Dir = W.Directory;
  return W.Result;
}

static OperationResult DoJob(Terminal & T, Terminal::WorkerJob Job)
{
  Waiter W;
  T.RunOnWorker(std::move(Job), W.Cb());
  return W.Wait();
}

static const RemoteFile * Find(const RemoteFileList & Files, const QString & Name)
{
  for (const RemoteFile & F : Files)
  {
    if (F.FileName == Name)
    {
      return &F;
    }
  }
  return nullptr;
}

static QByteArray Sha256(const QString & FileName)
{
  QFile F(FileName);
  if (!F.open(QIODevice::ReadOnly))
  {
    return QByteArray();
  }
  QCryptographicHash Hash(QCryptographicHash::Sha256);
  Hash.addData(&F);
  return Hash.result().toHex();
}

static void WriteFile(const QString & FileName, const QByteArray & Data, qint64 MTime)
{
  QFile F(FileName);
  if (!F.open(QIODevice::WriteOnly | QIODevice::Truncate) || (F.write(Data) != Data.size()))
  {
    std::printf("FAIL cannot write %s\n", qPrintable(FileName));
    Failures++;
  }
  F.close();
  struct timespec Times[2] = {{static_cast<time_t>(MTime), 0}, {static_cast<time_t>(MTime), 0}};
  utimensat(AT_FDCWD, QFile::encodeName(FileName).constData(), Times, 0);
}

static QByteArray RandomData(qint64 Size, quint32 Seed = 12345)
{
  QByteArray Data(Size, Qt::Uninitialized);
  QRandomGenerator Gen(Seed);
  Gen.fillRange(reinterpret_cast<quint32 *>(Data.data()), Size / 4);
  return Data;
}

static QMap<QString, QFileInfo> Tree(const QString & Root)
{
  QMap<QString, QFileInfo> Result;
  QDirIterator It(Root, QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
  while (It.hasNext())
  {
    It.next();
    Result.insert(QDir(Root).relativeFilePath(It.filePath()), It.fileInfo());
  }
  return Result;
}

static void CompareTrees(const QString & Expected, const QString & Actual, bool CheckTimes)
{
  QMap<QString, QFileInfo> A = Tree(Expected);
  QMap<QString, QFileInfo> B = Tree(Actual);
  CHECK_MSG(A.keys() == B.keys(), QStringLiteral("%1 vs %2").arg(A.keys().join(QLatin1Char(',')), B.keys().join(QLatin1Char(','))));
  for (auto It = A.cbegin(); It != A.cend(); ++It)
  {
    if (!B.contains(It.key()))
    {
      continue;
    }
    const QFileInfo & Other = B[It.key()];
    CHECK_MSG(It->isDir() == Other.isDir(), It.key());
    if (It->isDir())
    {
      continue;
    }
    CHECK_MSG(It->size() == Other.size(), It.key());
    CHECK_MSG(Sha256(It->filePath()) == Sha256(Other.filePath()), It.key());
    if (CheckTimes)
    {
      CHECK_MSG(It->lastModified().toSecsSinceEpoch() == Other.lastModified().toSecsSinceEpoch(),
        QStringLiteral("%1: %2 vs %3").arg(It.key()).arg(It->lastModified().toSecsSinceEpoch()).arg(Other.lastModified().toSecsSinceEpoch()));
    }
  }
}

// Cancels the terminal's operation once the current file transferred MinBytes
static QTimer * CancelWhenStarted(Terminal & T, qint64 & TransferredAtCancel, qint64 MinBytes = 1)
{
  auto Timer = new QTimer();
  Timer->setInterval(0);
  QObject::connect(Timer, &QTimer::timeout, Timer, [&T, Timer, &TransferredAtCancel, MinBytes]()
  {
    FileOperationProgress::Snapshot S = T.Progress().Get();
    if (S.FileTransferred >= MinBytes)
    {
      TransferredAtCancel = S.FileTransferred;
      T.Progress().Cancel();
      Timer->stop();
      Timer->deleteLater();
    }
  });
  Timer->start();
  return Timer;
}

static SessionData SessionFromUrl(const QString & UrlStr)
{
  QUrl Url(UrlStr);
  SessionData Data;
  Data.FSProtocol = fsWebDAV;
  bool Https = (Url.scheme() == QStringLiteral("https"));
  Data.Ftps = Https ? ftpsImplicit : ftpsNone;
  Data.HostName = Url.host();
  Data.PortNumber = Url.port(Https ? HTTPSPortNumber : HTTPPortNumber);
  QString Path = Url.path();
  Data.RemoteDirectory = ((Path.isEmpty()) || (Path == QStringLiteral("/"))) ? QString() : Path;
  Data.UserName = qEnvironmentVariable("NIXSCP_TEST_DAV_USER", QStringLiteral("test"));
  Data.Password = qEnvironmentVariable("NIXSCP_TEST_DAV_PASSWORD", QStringLiteral("secret"));
  Data.Timeout = 10;
  return Data;
}

struct Context
{
  TestUserInterface * UI;
  QString Root;       // local path of the server root, may be empty
  QString Work;       // local scratch
};

// Common suite run against each server flavour
static void RunSuite(const SessionData & Data, const QString & Label, Context & C, bool Full)
{
  std::printf("[%s: connect]\n", qPrintable(Label));
  C.UI->Reset();
  C.UI->QueryAnswers = {qaYes};   // certificate (https)
  Terminal T(Data, C.UI);
  OperationResult R = DoOpen(T);
  CHECK_MSG(R.Ok, Describe(R));
  if (!R.Ok)
  {
    return;
  }
  Log(QStringLiteral("protocol %1, home %2").arg(T.ProtocolName(), T.CurrentDirectory()));
  Log(T.ServerInfo().replace(QLatin1Char('\n'), QStringLiteral(" | ")));
  CHECK(T.ProtocolName() == QStringLiteral("WebDAV"));
  CHECK(T.ServerInfo().contains(QStringLiteral("The server supports these WebDAV extensions:")));
  CHECK(C.UI->PromptKinds.isEmpty());
  QString Home = T.CurrentDirectory();
  QString Base = UnixPath::Combine(Home, QStringLiteral("nixscp-test-%1-%2").arg(getpid()).arg(Label));
  QString LocalBase = C.Root.isEmpty() ? QString() : QDir(C.Root).filePath(QDir(QStringLiteral("/")).relativeFilePath(Base));

  {
    Waiter W;
    T.CreateDirectory(Base, 0755, W.Cb());
    R = W.Wait();
    CHECK_MSG(R.Ok, Describe(R));
  }

  // Local source tree
  QString Src = QDir(C.Work).filePath(Label + QStringLiteral("-src"));
  QString OddName = QStringLiteral("odd #?%+&;=@:[]'\" name.txt");
  QString Utf8Name = QString::fromUtf8("\xC5\xBElu\xC5\xA5ou\xC4\x8Dk\xC3\xBD k\xC5\xAF\xC5\x88.txt");
  QDir().mkpath(Src + QStringLiteral("/sub/deeper"));
  QDir().mkpath(Src + QStringLiteral("/empty dir"));
  qint64 T0 = 1600000000;
  WriteFile(Src + QStringLiteral("/small.txt"), "hello dav\n", T0 + 1);
  WriteFile(Src + QStringLiteral("/empty.bin"), QByteArray(), T0 + 2);
  WriteFile(Src + QStringLiteral("/with space.dat"), RandomData(100000, 1), T0 + 3);
  WriteFile(Src + QLatin1Char('/') + Utf8Name, "utf8\n", T0 + 4);
  WriteFile(Src + QStringLiteral("/.hidden"), "hidden\n", T0 + 5);
  WriteFile(Src + QStringLiteral("/sub/mid.bin"), RandomData(1234567, 2), T0 + 6);
  WriteFile(Src + QStringLiteral("/sub/deeper/deep.txt"), "deep\n", T0 + 7);
  WriteFile(Src + QLatin1Char('/') + OddName, "odd\n", T0 + 8);

  CopyParam Param;
  Param.PreserveTime = true;
  Param.ConfirmOverwriting = false;

  std::printf("[%s: upload tree]\n", qPrintable(Label));
  {
    Waiter W;
    T.CopyToRemote(QStringList{Src}, Base, Param, false, W.Cb());
    R = W.Wait();
    CHECK_MSG(R.Ok, Describe(R));
  }
  QString RSrc = UnixPath::Combine(Base, QFileInfo(Src).fileName());
  if (!LocalBase.isEmpty())
  {
    // the test server supports setting DAV:getlastmodified with PROPPATCH
    CompareTrees(Src, QDir(LocalBase).filePath(QFileInfo(Src).fileName()), true);
  }

  std::printf("[%s: listing]\n", qPrintable(Label));
  RemoteFileList Files;
  QString Dir;
  {
    R = DoList(T, RSrc, &Files, &Dir);
    CHECK_MSG(R.Ok, Describe(R));
    CHECK_MSG(Dir == RSrc, Dir);
    const RemoteFile * F = Find(Files, QStringLiteral("with space.dat"));
    CHECK(F && !F->IsDirectory && (F->Size == 100000));
    CHECK(F && (F->Modification.toSecsSinceEpoch() == T0 + 3));
    F = Find(Files, QStringLiteral("sub"));
    CHECK(F && F->IsDirectory);
    F = Find(Files, QStringLiteral("empty dir"));
    CHECK(F && F->IsDirectory);
    F = Find(Files, Utf8Name);
    CHECK(F && (F->Size == 5));
    F = Find(Files, OddName);
    CHECK(F && (F->Size == 4) && !F->IsDirectory);
    F = Find(Files, QStringLiteral(".hidden"));
    CHECK(F && F->IsHidden);
    F = Find(Files, QStringLiteral("empty.bin"));
    CHECK(F && (F->Size == 0) && !F->IsDirectory);
    CHECK_MSG(Files.size() == 8, QString::number(Files.size()));
    for (const RemoteFile & File : Files)
    {
      Log(QStringLiteral("%1 %2 %3 ").arg(File.TypeChar()).arg(File.Size)
        .arg(File.Modification.toString(Qt::ISODate)) + File.FileName);
    }
  }

  std::printf("[%s: download tree]\n", qPrintable(Label));
  QString Dl = QDir(C.Work).filePath(Label + QStringLiteral("-dl"));
  QDir().mkpath(Dl);
  {
    Waiter W;
    T.CopyToLocal(QStringList{RSrc}, Dl, Param, false, W.Cb());
    R = W.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    CompareTrees(Src, QDir(Dl).filePath(QFileInfo(Src).fileName()), true);
  }

  std::printf("[%s: file system calls]\n", qPrintable(Label));
  {
    auto Exists = std::make_shared<int>(0);
    auto DirExists = std::make_shared<int>(0);
    auto Missing = std::make_shared<int>(0);
    auto File = std::make_shared<RemoteFile>();
    auto DirFile = std::make_shared<RemoteFile>();
    auto Canonical = std::make_shared<QString>();
    auto CanonicalFile = std::make_shared<QString>();
    auto Redirected = std::make_shared<QString>();
    R = DoJob(T, [=](FileSystem & Fs, FileOperationProgress &, UserInterface &)
    {
      *Exists = Fs.FileExists(UnixPath::Combine(RSrc, QStringLiteral("small.txt")), File.get()) ? 1 : 0;
      *DirExists = Fs.FileExists(UnixPath::Combine(RSrc, QStringLiteral("sub")), DirFile.get()) ? 1 : 0;
      *Missing = Fs.FileExists(UnixPath::Combine(RSrc, QStringLiteral("nonexistent")), nullptr) ? 1 : 0;
      *Missing += Fs.FileExists(UnixPath::Combine(RSrc, QStringLiteral("nodir/nonexistent")), nullptr) ? 1 : 0;
      *Canonical = Fs.CanonicalizePath(UnixPath::Combine(RSrc, QStringLiteral("sub/deeper/../..")));
      *CanonicalFile = Fs.CanonicalizePath(UnixPath::Combine(RSrc, QStringLiteral("small.txt")));
      // PROPFIND redirect (one step, IsValidRedirect)
      *Redirected = Fs.CanonicalizePath(QStringLiteral("/redirect") + RSrc);
    });
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(*Exists == 1);
    CHECK(*DirExists == 1);
    CHECK(*Missing == 0);
    CHECK(File->FileName == QStringLiteral("small.txt"));
    CHECK(File->Size == 10);
    CHECK(!File->IsDirectory);
    CHECK(File->Modification.toSecsSinceEpoch() == T0 + 1);
    CHECK(DirFile->IsDirectory && (DirFile->FileName == QStringLiteral("sub")));
    CHECK_MSG(*Canonical == RSrc, *Canonical);
    CHECK_MSG(*CanonicalFile == UnixPath::Combine(RSrc, QStringLiteral("small.txt")), *CanonicalFile);
    CHECK_MSG(*Redirected == RSrc, *Redirected);
    // ReadFile of a missing file is an error, not fatal
    R = DoJob(T, [=](FileSystem & Fs, FileOperationProgress &, UserInterface &)
    {
      Fs.ReadFile(UnixPath::Combine(RSrc, QStringLiteral("nonexistent")));
    });
    CHECK_MSG(!R.Ok && !R.Fatal && R.Message.startsWith(QStringLiteral("Can't get attributes of file")) &&
      R.MoreMessages.contains(QStringLiteral("404")), Describe(R));
    // listing through a redirect
    R = DoList(T, QStringLiteral("/redirect") + RSrc, &Files, &Dir);
    CHECK_MSG(R.Ok && (Dir == RSrc) && (Files.size() == 8), Describe(R) + QStringLiteral(" ") + Dir);
    // redirect to another host is refused
    R = DoList(T, QStringLiteral("/cross-redirect") + RSrc);
    CHECK_MSG(!R.Ok && !R.Fatal && R.MoreMessages.contains(QStringLiteral("Redirect to other host encountered")), Describe(R));
    // permissions and links are not supported (as in WinSCP)
    R = DoJob(T, [=](FileSystem & Fs, FileOperationProgress &, UserInterface &)
    {
      Fs.ChangeMode(UnixPath::Combine(RSrc, QStringLiteral("small.txt")), 0600);
    });
    CHECK_MSG(!R.Ok && !R.Fatal && (R.MoreMessages == QStringLiteral("Operation not supported.")), Describe(R));
    Waiter WL;
    T.CreateLink(UnixPath::Combine(RSrc, QStringLiteral("link")), QStringLiteral("small.txt"), WL.Cb());
    R = WL.Wait();
    CHECK_MSG(!R.Ok && !R.Fatal && (R.MoreMessages == QStringLiteral("Operation not supported.")), Describe(R));
    auto Caps = std::make_shared<QStringList>();
    R = DoJob(T, [=](FileSystem & Fs, FileOperationProgress &, UserInterface &)
    {
      *Caps = dynamic_cast<WebDavFileSystem &>(Fs).Capabilities();
    });
    CHECK_MSG(R.Ok && Caps->contains(QStringLiteral("1")), Caps->join(QLatin1Char(',')));
  }

  std::printf("[%s: rename, mkdir, delete]\n", qPrintable(Label));
  {
    QString From = UnixPath::Combine(RSrc, QStringLiteral("small.txt"));
    QString To = UnixPath::Combine(RSrc, QStringLiteral("renamed small.txt"));
    Waiter W;
    T.RenameFile(From, To, W.Cb());
    R = W.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    // directory rename
    Waiter W1;
    T.RenameFile(UnixPath::Combine(RSrc, QStringLiteral("empty dir")), UnixPath::Combine(RSrc, QStringLiteral("empty dir 2")), W1.Cb());
    R = W1.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    R = DoList(T, RSrc, &Files);
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(!Find(Files, QStringLiteral("small.txt")));
    CHECK(Find(Files, QStringLiteral("renamed small.txt")) != nullptr);
    const RemoteFile * D = Find(Files, QStringLiteral("empty dir 2"));
    CHECK(D && D->IsDirectory);
    // rename of a missing source -> error, session stays usable
    Waiter W3;
    T.RenameFile(From, To, W3.Cb());
    R = W3.Wait();
    CHECK_MSG(!R.Ok && !R.Fatal && R.Message.startsWith(QStringLiteral("Error renaming file")), Describe(R));
    Log(Describe(R));
    // rename onto an existing file asks first (TTerminal::DoRenameOrCopyFile);
    // "No" keeps both files
    {
      QMutexLocker Lock(&C.UI->Mutex);
      C.UI->Queries.clear();
      C.UI->QueryAnswers = {qaNo};
    }
    Waiter W3b;
    T.RenameFile(To, UnixPath::Combine(RSrc, QStringLiteral("with space.dat")), W3b.Cb());
    R = W3b.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    {
      QMutexLocker Lock(&C.UI->Mutex);
      CHECK_MSG((C.UI->Queries.size() == 1) && C.UI->Queries[0].contains(QStringLiteral("already exists. Overwrite?")), C.UI->Queries.join(QLatin1Char('|')));
    }
    R = DoList(T, RSrc, &Files);
    CHECK(Find(Files, QStringLiteral("renamed small.txt")) && Find(Files, QStringLiteral("with space.dat")));
    // mkdir existing -> error with server message
    Waiter W4;
    T.CreateDirectory(UnixPath::Combine(RSrc, QStringLiteral("sub")), 0755, W4.Cb());
    R = W4.Wait();
    CHECK_MSG(!R.Ok && !R.Fatal && R.Message.startsWith(QStringLiteral("Error creating folder")) && !R.MoreMessages.isEmpty(), Describe(R));
    // recursive delete
    Waiter W5;
    T.DeleteFiles(QStringList{UnixPath::Combine(RSrc, QStringLiteral("sub")), To}, W5.Cb());
    R = W5.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    R = DoList(T, RSrc, &Files);
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(!Find(Files, QStringLiteral("sub")));
    CHECK(!Find(Files, QStringLiteral("renamed small.txt")));
    CHECK_MSG(Files.size() == 6, QString::number(Files.size()));
    // delete of a missing file -> error
    R = DoJob(T, [=](FileSystem & Fs, FileOperationProgress &, UserInterface &)
    {
      Fs.DeleteFile(UnixPath::Combine(RSrc, QStringLiteral("nonexistent")));
    });
    CHECK_MSG(!R.Ok && !R.Fatal && R.Message.startsWith(QStringLiteral("Error deleting file")), Describe(R));
    // listing a missing directory -> error, not fatal
    R = DoList(T, UnixPath::Combine(Base, QStringLiteral("nonexistent")));
    CHECK_MSG(!R.Ok && !R.Fatal, Describe(R));
    R = DoList(T, Base);
    CHECK_MSG(R.Ok, Describe(R));
  }

  if (Full)
  {
    // big file for cancel/resume; speed limited so that it can be cancelled
    qint64 BigSize = 6 * 1024 * 1024;
    QString BigDir = QDir(C.Work).filePath(Label + QStringLiteral("-big"));
    QDir().mkpath(BigDir);
    QString BigLocal = QDir(BigDir).filePath(QStringLiteral("big.bin"));
    WriteFile(BigLocal, RandomData(BigSize, 3), T0 + 10);
    QString RBig = UnixPath::Combine(Base, QStringLiteral("big.bin"));
    CopyParam Slow = Param;
    Slow.CPSLimit = 2 * 1024 * 1024;
    Slow.ResumeSupport = CopyParam::rsOff;

    std::printf("[%s: speed limited upload]\n", qPrintable(Label));
    {
      Waiter W;
      QElapsedTimer Timer;
      Timer.start();
      T.CopyToRemote(QStringList{BigLocal}, Base, Slow, false, W.Cb());
      R = W.Wait();
      CHECK_MSG(R.Ok, Describe(R));
      Log(QStringLiteral("%1 bytes in %2 ms").arg(BigSize).arg(Timer.elapsed()));
      // per second budget: 3 seconds' worth may pass in just over 1 s
      CHECK(Timer.elapsed() >= 1000);
      CHECK(T.Progress().Get().TotalTransferred == BigSize);
      if (!LocalBase.isEmpty())
      {
        CHECK(Sha256(QDir(LocalBase).filePath(QStringLiteral("big.bin"))) == Sha256(BigLocal));
      }
    }

    std::printf("[%s: upload into a missing directory]\n", qPrintable(Label));
    {
      // the server answers 409 before reading the body: the upload stops early
      Waiter W;
      QElapsedTimer Timer;
      Timer.start();
      T.CopyToRemote(QStringList{BigLocal}, UnixPath::Combine(Base, QStringLiteral("missing")), Slow, false, W.Cb());
      R = W.Wait();
      Log(QStringLiteral("%1 ms: %2").arg(Timer.elapsed()).arg(Describe(R)));
      CHECK_MSG(!R.Ok && !R.Fatal && R.Message.startsWith(QStringLiteral("Error transferring file")) &&
        R.MoreMessages.contains(QStringLiteral("409")), Describe(R));
      // (at most the wait for the next second of the speed limit budget)
      CHECK(Timer.elapsed() < 1500);
      R = DoList(T, Base, &Files);
      CHECK_MSG(R.Ok, Describe(R));
    }

    std::printf("[%s: speed limited download]\n", qPrintable(Label));
    {
      QString Dl2 = QDir(C.Work).filePath(Label + QStringLiteral("-dl2"));
      QDir().mkpath(Dl2);
      Waiter W;
      QElapsedTimer Timer;
      Timer.start();
      T.CopyToLocal(QStringList{RBig}, Dl2, Slow, false, W.Cb());
      R = W.Wait();
      CHECK_MSG(R.Ok, Describe(R));
      Log(QStringLiteral("%1 bytes in %2 ms").arg(BigSize).arg(Timer.elapsed()));
      CHECK(Timer.elapsed() >= 1000);
      CHECK(T.Progress().Get().TotalTransferred == BigSize);
      CHECK(Sha256(Dl2 + QStringLiteral("/big.bin")) == Sha256(BigLocal));
      CHECK(QFileInfo(Dl2 + QStringLiteral("/big.bin")).lastModified().toSecsSinceEpoch() == T0 + 10);
    }

    std::printf("[%s: cancel download]\n", qPrintable(Label));
    {
      QString Dl3 = QDir(C.Work).filePath(Label + QStringLiteral("-dl3"));
      QDir().mkpath(Dl3);
      qint64 At = -1;
      QPointer<QTimer> Timer = CancelWhenStarted(T, At);
      Waiter W;
      T.CopyToLocal(QStringList{RBig}, Dl3, Slow, false, W.Cb());
      R = W.Wait();
      delete Timer.data();
      Log(QStringLiteral("cancelled at %1: %2").arg(At).arg(Describe(R)));
      CHECK_MSG(!R.Ok && R.Aborted && !R.Fatal, Describe(R));
      CHECK(!QFile::exists(Dl3 + QStringLiteral("/big.bin")));
      // the session must stay usable
      R = DoList(T, Base, &Files);
      CHECK_MSG(R.Ok, Describe(R));
      const RemoteFile * F = Find(Files, QStringLiteral("big.bin"));
      CHECK(F && (F->Size == BigSize));
      Waiter W2;
      T.CopyToLocal(QStringList{RBig}, Dl3, Param, false, W2.Cb());
      R = W2.Wait();
      CHECK_MSG(R.Ok, Describe(R));
      CHECK(Sha256(Dl3 + QStringLiteral("/big.bin")) == Sha256(BigLocal));
    }

    std::printf("[%s: cancel upload]\n", qPrintable(Label));
    {
      qint64 At = -1;
      QPointer<QTimer> Timer = CancelWhenStarted(T, At);
      Waiter W;
      T.CopyToRemote(QStringList{BigLocal}, UnixPath::Combine(RSrc, QStringLiteral("empty dir 2")), Slow, false, W.Cb());
      R = W.Wait();
      delete Timer.data();
      Log(QStringLiteral("cancelled at %1: %2").arg(At).arg(Describe(R)));
      CHECK_MSG(!R.Ok && R.Aborted && !R.Fatal, Describe(R));
      R = DoList(T, UnixPath::Combine(RSrc, QStringLiteral("empty dir 2")), &Files);
      CHECK_MSG(R.Ok, Describe(R));
      // the server discards an incomplete PUT
      CHECK(!Find(Files, QStringLiteral("big.bin")));
    }

    CopyParam Resume = Slow;
    Resume.ResumeSupport = CopyParam::rsOn;
    Resume.ConfirmResume = false;
    std::printf("[%s: resume download]\n", qPrintable(Label));
    {
      QString Dl5 = QDir(C.Work).filePath(Label + QStringLiteral("-dl5"));
      QDir().mkpath(Dl5);
      QString Target = Dl5 + QStringLiteral("/big.bin");
      QString Partial = Target + QStringLiteral(".filepart");
      qint64 At = -1;
      QPointer<QTimer> Timer = CancelWhenStarted(T, At);
      Waiter W;
      T.CopyToLocal(QStringList{RBig}, Dl5, Resume, false, W.Cb());
      R = W.Wait();
      delete Timer.data();
      CHECK_MSG(!R.Ok && R.Aborted && !R.Fatal, Describe(R));
      CHECK(!QFile::exists(Target));
      qint64 PartialSize = QFileInfo(Partial).size();
      Log(QStringLiteral("partial file: %1 bytes").arg(PartialSize));
      CHECK((PartialSize > 0) && (PartialSize < BigSize));
      Resume.CPSLimit = 0;
      Waiter W2;
      T.CopyToLocal(QStringList{RBig}, Dl5, Resume, false, W2.Cb());
      R = W2.Wait();
      CHECK_MSG(R.Ok, Describe(R));
      CHECK(T.Progress().Get().TotalTransferred == BigSize);
      CHECK(!QFile::exists(Partial));
      CHECK(Sha256(Target) == Sha256(BigLocal));
      CHECK(QFileInfo(Target).lastModified().toSecsSinceEpoch() == T0 + 10);
      // a complete partial file (Range not satisfiable)
      QFile::rename(Target, Partial);
      Waiter W3;
      T.CopyToLocal(QStringList{RBig}, Dl5, Resume, false, W3.Cb());
      R = W3.Wait();
      CHECK_MSG(R.Ok, Describe(R));
      CHECK(!QFile::exists(Partial));
      CHECK(Sha256(Target) == Sha256(BigLocal));
      Resume.CPSLimit = Slow.CPSLimit;
    }

    std::printf("[%s: resume upload]\n", qPrintable(Label));
    {
      // a PUT cannot append: a left over partial file is replaced by a complete upload
      QString PartialName = QStringLiteral("big.bin.filepart");
      QString TargetDir = UnixPath::Combine(RSrc, QStringLiteral("empty dir 2"));
      QString PartialLocal = QDir(BigDir).filePath(PartialName);
      WriteFile(PartialLocal, RandomData(BigSize, 3).left(1000000), T0);
      Waiter W0;
      T.CopyToRemote(QStringList{PartialLocal}, TargetDir, Param, false, W0.Cb());
      R = W0.Wait();
      CHECK_MSG(R.Ok, Describe(R));
      Resume.CPSLimit = 0;
      Waiter W2;
      T.CopyToRemote(QStringList{BigLocal}, TargetDir, Resume, false, W2.Cb());
      R = W2.Wait();
      CHECK_MSG(R.Ok, Describe(R));
      CHECK_MSG(T.Progress().Get().TotalTransferred == BigSize, QString::number(T.Progress().Get().TotalTransferred));
      R = DoList(T, TargetDir, &Files);
      CHECK(!Find(Files, PartialName));
      const RemoteFile * F = Find(Files, QStringLiteral("big.bin"));
      CHECK(F && (F->Size == BigSize));
      CHECK(F && (F->Modification.toSecsSinceEpoch() == T0 + 10));
      QString Dl6 = QDir(C.Work).filePath(Label + QStringLiteral("-dl6"));
      QDir().mkpath(Dl6);
      Waiter W3;
      T.CopyToLocal(QStringList{UnixPath::Combine(TargetDir, QStringLiteral("big.bin"))}, Dl6, Param, false, W3.Cb());
      R = W3.Wait();
      CHECK_MSG(R.Ok, Describe(R));
      CHECK(Sha256(Dl6 + QStringLiteral("/big.bin")) == Sha256(BigLocal));
    }
  }

  std::printf("[%s: cleanup]\n", qPrintable(Label));
  {
    Waiter W;
    T.DeleteFiles(QStringList{Base}, W.Cb());
    R = W.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    R = DoList(T, Home, &Files);
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(!Find(Files, UnixPath::ExtractFileName(Base)));
  }
}

int main(int argc, char ** argv)
{
  QCoreApplication App(argc, argv);

  QString UrlStr = qEnvironmentVariable("NIXSCP_TEST_DAV_URL");
  if (UrlStr.isEmpty())
  {
    std::printf("test_webdav: SKIPPED (set NIXSCP_TEST_DAV_URL [, NIXSCP_TEST_DAV_USER, NIXSCP_TEST_DAV_PASSWORD, "
      "NIXSCP_TEST_DAV_ROOT, NIXSCP_TEST_DAVS_URL, NIXSCP_TEST_DAV_DIGEST_URL])\n");
    return 0;
  }
  QTemporaryDir Scratch;
  CHECK(Scratch.isValid());
  TestUserInterface UI;
  Context C{&UI, qEnvironmentVariable("NIXSCP_TEST_DAV_ROOT"), Scratch.path()};
  SessionData Data = SessionFromUrl(UrlStr);

  std::printf("[path escaping]\n");
  {
    CHECK(WebDavFileSystem::PathEscape(QStringLiteral("/a b/c#d?e%f/")) == "/a%20b/c%23d%3fe%25f/");
    CHECK(WebDavFileSystem::PathEscape(QStringLiteral("/!$&'()*+,;=:@-._~")) == "/!$&'()*+,;=:@-._~");
    CHECK(WebDavFileSystem::PathEscape(QString::fromUtf8("/\xC5\xBE[]\"")) == "/%c5%be%5b%5d%22");
    CHECK(WebDavFileSystem::PathUnescape("/a%20b/%C5%BE") == QString::fromUtf8("/a b/\xC5\xBE"));
    CHECK(WebDavFileSystem::PathUnescape("/100%") == QStringLiteral("/100%"));
  }

  // ---------------------------------------------------------------- authentication
  std::printf("[wrong password]\n");
  {
    UI.Reset();
    SessionData Wrong = Data;
    Wrong.Password = QStringLiteral("wrong password");
    Terminal T(Wrong, &UI);
    OperationResult R = DoOpen(T);
    CHECK_MSG(!R.Ok && R.Fatal, Describe(R));
    CHECK_MSG((R.Message == QStringLiteral("Connection failed.")) && R.MoreMessages.startsWith(QStringLiteral("Authentication failed.")), Describe(R));
    // the stored password is tried first, then the user is asked
    CHECK((UI.PromptKinds.size() == 1) && (UI.PromptKinds.value(0) == UserInterface::pkPassword));
    CHECK(UI.PromptInstructions.value(0) == QStringLiteral("Access denied."));
    CHECK(!T.Active());
  }
  std::printf("[wrong password, then correct one]\n");
  {
    UI.Reset();
    UI.PromptAnswers = {QStringLiteral("still wrong"), Data.Password};
    SessionData Wrong = Data;
    Wrong.Password = QStringLiteral("wrong password");
    Terminal T(Wrong, &UI);
    OperationResult R = DoOpen(T);
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(UI.PromptKinds.size() == 2);
    R = DoList(T, T.CurrentDirectory());
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(UI.PromptKinds.size() == 2);
  }
  std::printf("[password prompt]\n");
  {
    UI.Reset();
    UI.PromptAnswers = {Data.Password};
    SessionData NoPassword = Data;
    NoPassword.Password.clear();
    Terminal T(NoPassword, &UI);
    OperationResult R = DoOpen(T);
    CHECK_MSG(R.Ok, Describe(R));
    CHECK((UI.PromptKinds.size() == 1) && (UI.PromptKinds.value(0) == UserInterface::pkPassword));
    CHECK(UI.PromptInstructions.value(0).isEmpty());
  }
  std::printf("[username and password prompt]\n");
  {
    UI.Reset();
    UI.PromptAnswers = {Data.UserName, Data.Password};
    SessionData NoCredentials = Data;
    NoCredentials.UserName.clear();
    NoCredentials.Password.clear();
    Terminal T(NoCredentials, &UI);
    OperationResult R = DoOpen(T);
    CHECK_MSG(R.Ok, Describe(R));
    CHECK((UI.PromptKinds.size() == 2) && (UI.PromptKinds.value(0) == UserInterface::pkUserName) &&
      (UI.PromptKinds.value(1) == UserInterface::pkPassword));
  }
  std::printf("[password prompt cancelled]\n");
  {
    UI.Reset();
    SessionData NoPassword = Data;
    NoPassword.Password.clear();
    Terminal T(NoPassword, &UI);
    OperationResult R = DoOpen(T);
    CHECK_MSG(!R.Ok && R.Fatal && R.MoreMessages.contains(QStringLiteral("Authentication failed.")), Describe(R));
  }
  std::printf("[connection refused]\n");
  {
    UI.Reset();
    SessionData Refused = Data;
    Refused.PortNumber = 1;
    Terminal T(Refused, &UI);
    OperationResult R = DoOpen(T);
    CHECK_MSG(!R.Ok && R.Fatal && (R.Message == QStringLiteral("Connection failed.")) &&
      R.MoreMessages.startsWith(QStringLiteral("Could not connect to server")), Describe(R));
  }
  std::printf("[missing remote directory]\n");
  {
    UI.Reset();
    SessionData Missing = Data;
    Missing.RemoteDirectory = QStringLiteral("/nixscp-nonexistent-%1").arg(getpid());
    Terminal T(Missing, &UI);
    OperationResult R = DoOpen(T);
    // OPTIONS succeeds, the directory cannot be opened: the session starts in "/"
    CHECK_MSG(R.Ok, Describe(R));
    CHECK_MSG(T.CurrentDirectory() == QStringLiteral("/"), T.CurrentDirectory());
  }
  std::printf("[redirected remote directory]\n");
  {
    UI.Reset();
    SessionData Redirected = Data;
    Redirected.RemoteDirectory = QStringLiteral("/redirect") + (Data.RemoteDirectory.isEmpty() ? QStringLiteral("/") : Data.RemoteDirectory);
    Terminal T(Redirected, &UI);
    OperationResult R = DoOpen(T);
    CHECK_MSG(R.Ok, Describe(R));
    QString Expected = Data.RemoteDirectory.isEmpty() ? QStringLiteral("/") : Data.RemoteDirectory;
    CHECK_MSG(T.CurrentDirectory() == Expected, T.CurrentDirectory());
  }

  // ---------------------------------------------------------------- suites
  RunSuite(Data, QStringLiteral("plain"), C, true);

  QString DigestUrl = qEnvironmentVariable("NIXSCP_TEST_DAV_DIGEST_URL");
  if (!DigestUrl.isEmpty())
  {
    SessionData Digest = SessionFromUrl(DigestUrl);
    std::printf("[digest: wrong password]\n");
    {
      UI.Reset();
      UI.PromptAnswers = {Digest.Password};
      SessionData Wrong = Digest;
      Wrong.Password = QStringLiteral("wrong password");
      Terminal T(Wrong, &UI);
      OperationResult R = DoOpen(T);
      CHECK_MSG(R.Ok, Describe(R));
      CHECK(UI.PromptKinds.size() == 1);
    }
    // the server refuses Basic (preferred Digest) and expires nonces (stale=true)
    RunSuite(Digest, QStringLiteral("digest"), C, true);
  }

  QString HttpsUrl = qEnvironmentVariable("NIXSCP_TEST_DAVS_URL");
  if (!HttpsUrl.isEmpty())
  {
    SessionData Https = SessionFromUrl(HttpsUrl);
    std::printf("[certificate: cancel]\n");
    {
      UI.Reset();
      Terminal T(Https, &UI);
      OperationResult R = DoOpen(T);
      CHECK_MSG(!R.Ok && R.Fatal && (R.Message == QStringLiteral("Connection failed.")) &&
        R.MoreMessages.contains(QStringLiteral("Server certificate verification failed")), Describe(R));
      CHECK(UI.Queries.size() == 1);
      QString Query = UI.Queries.value(0);
      CHECK(Query.contains(QStringLiteral("The server's certificate is not known")));
      CHECK(Query.contains(QStringLiteral("SHA-256: ")));
      CHECK(Query.contains(QStringLiteral("Certificate not trusted.")));
      CHECK((Query.contains(QStringLiteral("NixSCP Test")) || Query.contains(QStringLiteral("LinSCP Test"))));
      Log(Query.mid(Query.indexOf(QStringLiteral("Summary"))).replace(QLatin1Char('\n'), QLatin1Char(' ')));
    }
    std::printf("[certificate: accept once]\n");
    {
      UI.Reset();
      UI.QueryAnswers = {qaNo};
      Terminal T(Https, &UI);
      OperationResult R = DoOpen(T);
      CHECK_MSG(R.Ok, Describe(R));
      CHECK(UI.Queries.size() == 1);
      CHECK(T.ServerInfo().contains(QStringLiteral("TLS/SSL Implicit encryption")));
      // reconnections of the same session do not ask again
      R = DoList(T, T.CurrentDirectory());
      CHECK_MSG(R.Ok, Describe(R));
      CHECK(UI.Queries.size() == 1);
    }
    std::printf("[certificate: accept and remember]\n");
    {
      UI.Reset();
      UI.QueryAnswers = {qaYes};
      Terminal T(Https, &UI);
      OperationResult R = DoOpen(T);
      CHECK_MSG(R.Ok, Describe(R));
      CHECK(UI.Queries.size() == 1);
      UI.Reset();
      Terminal T2(Https, &UI);
      R = DoOpen(T2);
      CHECK_MSG(R.Ok, Describe(R));
      CHECK(UI.Queries.isEmpty());
    }
    std::printf("[https: connection closed by the server while idle]\n");
    {
      // the test server closes connections idle for 3 s: the next request reconnects
      UI.Reset();
      Terminal T(Https, &UI);
      OperationResult R = DoOpen(T);
      CHECK_MSG(R.Ok, Describe(R));
      QEventLoop Loop;
      QTimer::singleShot(4000, &Loop, &QEventLoop::quit);
      Loop.exec();
      R = DoList(T, T.CurrentDirectory());
      CHECK_MSG(R.Ok, Describe(R));
      CHECK(UI.Queries.isEmpty());
    }
    std::printf("[https: plain http to https port]\n");
    {
      UI.Reset();
      SessionData Wrong = Https;
      Wrong.Ftps = ftpsNone;
      Wrong.Timeout = 3;
      Terminal T(Wrong, &UI);
      OperationResult R = DoOpen(T);
      CHECK_MSG(!R.Ok && R.Fatal, Describe(R));
      Log(Describe(R));
    }
    RunSuite(Https, QStringLiteral("https"), C, true);
  }

  std::printf("%s\n", (Failures == 0) ? "test_webdav: all tests passed" : "test_webdav: FAILURES");
  return (Failures == 0) ? 0 : 1;
}
