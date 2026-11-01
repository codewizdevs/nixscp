// End-to-end test of core/Terminal + core/FtpFileSystem against real FTP servers.
//
// Skipped (exit 0) unless NIXSCP_TEST_FTP_PORT is set. Environment:
//   NIXSCP_TEST_FTP_PORT            plain FTP server port (required to run)
//   NIXSCP_TEST_FTP_HOST            default 127.0.0.1
//   NIXSCP_TEST_FTP_USER            default "test"
//   NIXSCP_TEST_FTP_PASSWORD        default "secret"
//   NIXSCP_TEST_FTP_ROOT            optional: local path of the user's FTP root
//                                   (server on this machine), remote results are
//                                   then verified with stat() too
//   NIXSCP_TEST_FTP_LIST_PORT       optional: server without MLSD/MLST (LIST parsing)
//   NIXSCP_TEST_FTPS_PORT           optional: explicit FTPS (AUTH TLS) server
//   NIXSCP_TEST_FTPS_IMPLICIT_PORT  optional: implicit FTPS server
//   NIXSCP_TEST_FTP_ANONYMOUS       set to 0 if the server does not allow anonymous login
// The pyftpdlib based servers for all of these can be started without root,
// see the start script referenced in the NixSCP FTP notes.
#include "core/Terminal.h"
#include "core/Configuration.h"
#include "core/FtpFileSystem.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QPointer>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

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
  int HostKeyCalls = 0;

  void Reset()
  {
    QMutexLocker Lock(&Mutex);
    PromptAnswers.clear();
    QueryAnswers.clear();
    PromptKinds.clear();
    PromptInstructions.clear();
    Queries.clear();
    HostKeyCalls = 0;
  }

  HostKeyAnswer VerifyHostKey(const QString &, int, const QString &, const QString &, const QString &, HostKeyState) override
  {
    QMutexLocker Lock(&Mutex);
    HostKeyCalls++;
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

static unsigned int LocalMode(const QString & Path)
{
  struct stat St;
  if (stat(QFile::encodeName(Path).constData(), &St) != 0)
  {
    return 0xFFFFFFFF;
  }
  return St.st_mode & 07777;
}

// Cancels the terminal's operation once the current file started transferring
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

struct Context
{
  TestUserInterface * UI;
  QString Root;       // local path of the FTP root, may be empty
  QString Work;       // local scratch
};

// Common suite run against each server flavour
static void RunSuite(const SessionData & Data, const QString & Label, Context & C, bool Full)
{
  std::printf("[%s: connect]\n", qPrintable(Label));
  C.UI->Reset();
  C.UI->QueryAnswers = {qaYes};   // certificate (FTPS)
  Terminal T(Data, C.UI);
  OperationResult R = DoOpen(T);
  CHECK_MSG(R.Ok, Describe(R));
  if (!R.Ok)
  {
    return;
  }
  Log(QStringLiteral("protocol %1, home %2").arg(T.ProtocolName(), T.CurrentDirectory()));
  Log(T.ServerInfo().replace(QLatin1Char('\n'), QStringLiteral(" | ")));
  CHECK(T.ProtocolName() == ((Data.Ftps == ftpsNone) ? QStringLiteral("FTP") : QStringLiteral("FTPS")));
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
  QDir().mkpath(Src + QStringLiteral("/sub/deeper"));
  QDir().mkpath(Src + QStringLiteral("/empty dir"));
  qint64 T0 = 1600000000;
  WriteFile(Src + QStringLiteral("/small.txt"), "hello ftp\n", T0 + 1);
  WriteFile(Src + QStringLiteral("/empty.bin"), QByteArray(), T0 + 2);
  WriteFile(Src + QStringLiteral("/with space.dat"), RandomData(100000, 1), T0 + 3);
  WriteFile(Src + QString::fromUtf8("/\xC5\xBElu\xC5\xA5ou\xC4\x8Dk\xC3\xBD k\xC5\xAF\xC5\x88.txt"), "utf8\n", T0 + 4);
  WriteFile(Src + QStringLiteral("/.hidden"), "hidden\n", T0 + 5);
  WriteFile(Src + QStringLiteral("/sub/mid.bin"), RandomData(1234567, 2), T0 + 6);
  WriteFile(Src + QStringLiteral("/sub/deeper/deep.txt"), "deep\n", T0 + 7);

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
    CompareTrees(Src, QDir(LocalBase).filePath(QFileInfo(Src).fileName()), true);
  }

  std::printf("[%s: listing]\n", qPrintable(Label));
  RemoteFileList Files;
  QString Dir;
  {
    R = DoList(T, RSrc, &Files, &Dir);
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(Dir == RSrc);
    const RemoteFile * F = Find(Files, QStringLiteral("with space.dat"));
    CHECK(F && !F->IsDirectory && (F->Size == 100000));
    CHECK(F && (F->Modification.toSecsSinceEpoch() == T0 + 3));
    F = Find(Files, QStringLiteral("sub"));
    CHECK(F && F->IsDirectory);
    F = Find(Files, QStringLiteral("empty dir"));
    CHECK(F && F->IsDirectory);
    F = Find(Files, QString::fromUtf8("\xC5\xBElu\xC5\xA5ou\xC4\x8Dk\xC3\xBD k\xC5\xAF\xC5\x88.txt"));
    CHECK(F && (F->Size == 5));
    F = Find(Files, QStringLiteral(".hidden"));
    CHECK(F && F->IsHidden);
    CHECK(Files.size() == 7);
    for (const RemoteFile & File : Files)
    {
      Log(QStringLiteral("%1 %2 %3 %4 %5/%6").arg(File.TypeChar(), Rights::ToOctal(File.Permissions)).arg(File.Size)
        .arg(File.Modification.toString(Qt::ISODate), File.Owner, File.Group) + QStringLiteral(" ") + File.FileName);
    }
  }

  std::printf("[%s: recent timestamp]\n", qPrintable(Label));
  {
    // LIST shows recent files with time (server local time, minutes only):
    // the timezone difference is detected with MDTM
    QString RecentDir = QDir(C.Work).filePath(Label + QStringLiteral("-recent"));
    QDir().mkpath(RecentDir);
    qint64 Now = QDateTime::currentSecsSinceEpoch() - 3600;
    WriteFile(RecentDir + QStringLiteral("/recent.txt"), "recent\n", Now);
    Waiter W;
    T.CopyToRemote(QStringList{RecentDir + QStringLiteral("/recent.txt")}, Base, Param, false, W.Cb());
    R = W.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    // new session: no remembered upload times
    Terminal T2(Data, C.UI);
    R = DoOpen(T2);
    CHECK_MSG(R.Ok, Describe(R));
    R = DoList(T2, Base, &Files);
    CHECK_MSG(R.Ok, Describe(R));
    const RemoteFile * F = Find(Files, QStringLiteral("recent.txt"));
    CHECK(F != nullptr);
    if (F != nullptr)
    {
      qint64 Diff = F->Modification.toSecsSinceEpoch() - Now;
      CHECK_MSG((Diff <= 0) && (Diff > -60), QStringLiteral("%1 vs %2").arg(F->Modification.toString(Qt::ISODate)).arg(Now));
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
    auto Missing = std::make_shared<int>(0);
    auto File = std::make_shared<RemoteFile>();
    auto Canonical = std::make_shared<QString>();
    R = DoJob(T, [=](FileSystem & Fs, FileOperationProgress &, UserInterface &)
    {
      *Exists = Fs.FileExists(UnixPath::Combine(RSrc, QStringLiteral("small.txt")), File.get()) ? 1 : 0;
      *Missing = Fs.FileExists(UnixPath::Combine(RSrc, QStringLiteral("nonexistent")), nullptr) ? 1 : 0;
      *Missing += Fs.FileExists(UnixPath::Combine(RSrc, QStringLiteral("nodir/nonexistent")), nullptr) ? 1 : 0;
      *Canonical = Fs.CanonicalizePath(UnixPath::Combine(RSrc, QStringLiteral("sub/deeper/../..")));
    });
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(*Exists == 1);
    CHECK(*Missing == 0);
    CHECK(File->FileName == QStringLiteral("small.txt"));
    CHECK(File->Size == 10);
    CHECK(!File->IsDirectory);
    CHECK_MSG(*Canonical == RSrc, *Canonical);
    // ReadFile of a missing file is an error, not fatal
    R = DoJob(T, [=](FileSystem & Fs, FileOperationProgress &, UserInterface &)
    {
      Fs.ReadFile(UnixPath::Combine(RSrc, QStringLiteral("nonexistent")));
    });
    CHECK_MSG(!R.Ok && !R.Fatal && R.Message.contains(QStringLiteral("does not exist")), Describe(R));
    // raw command
    auto Lines = std::make_shared<QStringList>();
    R = DoJob(T, [=](FileSystem & Fs, FileOperationProgress &, UserInterface &)
    {
      *Lines = dynamic_cast<FtpFileSystem &>(Fs).AnyCommand(QStringLiteral("NOOP"));
    });
    CHECK_MSG(R.Ok && (Lines->size() == 1) && Lines->value(0).startsWith(QStringLiteral("200")), Lines->join(QLatin1Char('|')));
  }

  std::printf("[%s: rename, chmod, mkdir, delete]\n", qPrintable(Label));
  {
    QString From = UnixPath::Combine(RSrc, QStringLiteral("small.txt"));
    QString To = UnixPath::Combine(RSrc, QStringLiteral("renamed small.txt"));
    Waiter W;
    T.RenameFile(From, To, W.Cb());
    R = W.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    Waiter W2;
    T.ChangeFileProperties(QStringList{To}, 0600, false, W2.Cb());
    R = W2.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    R = DoList(T, RSrc, &Files);
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(!Find(Files, QStringLiteral("small.txt")));
    const RemoteFile * F = Find(Files, QStringLiteral("renamed small.txt"));
    CHECK(F != nullptr);
    if ((F != nullptr) && (F->Permissions != 0))
    {
      CHECK_MSG(F->Permissions == 0600, Rights::ToOctal(F->Permissions));
    }
    if (!LocalBase.isEmpty())
    {
      QString LocalTo = QDir(LocalBase).filePath(QFileInfo(Src).fileName() + QStringLiteral("/renamed small.txt"));
      CHECK_MSG(LocalMode(LocalTo) == 0600, Rights::ToOctal(LocalMode(LocalTo)));
    }
    // rename onto missing source -> error, session stays usable
    Waiter W3;
    T.RenameFile(From, To, W3.Cb());
    R = W3.Wait();
    CHECK_MSG(!R.Ok && !R.Fatal && R.Message.startsWith(QStringLiteral("Error renaming file")), Describe(R));
    Log(Describe(R));
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
    CHECK(Files.size() == 5);
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
      CHECK(Timer.elapsed() >= 1000);
      if (!LocalBase.isEmpty())
      {
        CHECK(Sha256(QDir(LocalBase).filePath(QStringLiteral("big.bin"))) == Sha256(BigLocal));
      }
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
      T.CopyToRemote(QStringList{BigLocal}, UnixPath::Combine(RSrc, QStringLiteral("empty dir")), Slow, false, W.Cb());
      R = W.Wait();
      delete Timer.data();
      Log(QStringLiteral("cancelled at %1: %2").arg(At).arg(Describe(R)));
      CHECK_MSG(!R.Ok && R.Aborted && !R.Fatal, Describe(R));
      R = DoList(T, UnixPath::Combine(RSrc, QStringLiteral("empty dir")), &Files);
      CHECK_MSG(R.Ok, Describe(R));
      // No partial file left behind (as WinSCP without resume)
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
      Resume.CPSLimit = Slow.CPSLimit;
    }

    std::printf("[%s: resume upload]\n", qPrintable(Label));
    {
      QString Target = UnixPath::Combine(RSrc, QStringLiteral("empty dir/big.bin"));
      qint64 At = -1;
      QPointer<QTimer> Timer = CancelWhenStarted(T, At, 1024 * 1024);
      Waiter W;
      T.CopyToRemote(QStringList{BigLocal}, UnixPath::Combine(RSrc, QStringLiteral("empty dir")), Resume, false, W.Cb());
      R = W.Wait();
      delete Timer.data();
      CHECK_MSG(!R.Ok && R.Aborted && !R.Fatal, Describe(R));
      R = DoList(T, UnixPath::Combine(RSrc, QStringLiteral("empty dir")), &Files);
      CHECK_MSG(R.Ok, Describe(R));
      const RemoteFile * P = Find(Files, QStringLiteral("big.bin.filepart"));
      CHECK(P && (P->Size > 0) && (P->Size < BigSize));
      CHECK(!Find(Files, QStringLiteral("big.bin")));
      if (P != nullptr)
      {
        Log(QStringLiteral("remote partial file: %1 bytes").arg(P->Size));
      }
      Resume.CPSLimit = 0;
      Waiter W2;
      T.CopyToRemote(QStringList{BigLocal}, UnixPath::Combine(RSrc, QStringLiteral("empty dir")), Resume, false, W2.Cb());
      R = W2.Wait();
      CHECK_MSG(R.Ok, Describe(R));
      CHECK(T.Progress().Get().TotalTransferred == BigSize);
      R = DoList(T, UnixPath::Combine(RSrc, QStringLiteral("empty dir")), &Files);
      CHECK(!Find(Files, QStringLiteral("big.bin.filepart")));
      const RemoteFile * F = Find(Files, QStringLiteral("big.bin"));
      CHECK(F && (F->Size == BigSize));
      CHECK(F && (F->Modification.toSecsSinceEpoch() == T0 + 10));
      QString Dl6 = QDir(C.Work).filePath(Label + QStringLiteral("-dl6"));
      QDir().mkpath(Dl6);
      Waiter W3;
      T.CopyToLocal(QStringList{Target}, Dl6, Param, false, W3.Cb());
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

  QByteArray PortStr = qgetenv("NIXSCP_TEST_FTP_PORT");
  if (PortStr.isEmpty())
  {
    std::printf("test_ftp: SKIPPED (set NIXSCP_TEST_FTP_PORT [, NIXSCP_TEST_FTP_HOST, NIXSCP_TEST_FTP_USER, NIXSCP_TEST_FTP_PASSWORD, "
      "NIXSCP_TEST_FTP_ROOT, NIXSCP_TEST_FTP_LIST_PORT, NIXSCP_TEST_FTPS_PORT, NIXSCP_TEST_FTPS_IMPLICIT_PORT])\n");
    return 0;
  }
  QTemporaryDir Scratch;
  CHECK(Scratch.isValid());
  // Accepted FTPS certificates are stored in the configuration file
  // ([FtpsCertificates]); never touch the user's one
  QStandardPaths::setTestModeEnabled(true);
  Configuration::Instance().SetIniFileName(Scratch.filePath(QStringLiteral("nixscp.ini")));
  TestUserInterface UI;
  Context C{&UI, qEnvironmentVariable("NIXSCP_TEST_FTP_ROOT"), Scratch.path()};

  SessionData Data;
  Data.FSProtocol = fsFTP;
  Data.HostName = qEnvironmentVariable("NIXSCP_TEST_FTP_HOST", QStringLiteral("127.0.0.1"));
  Data.PortNumber = PortStr.toInt();
  Data.UserName = qEnvironmentVariable("NIXSCP_TEST_FTP_USER", QStringLiteral("test"));
  Data.Password = qEnvironmentVariable("NIXSCP_TEST_FTP_PASSWORD", QStringLiteral("secret"));
  Data.Timeout = 10;

  // ---------------------------------------------------------------- authentication
  std::printf("[wrong password]\n");
  {
    UI.Reset();
    SessionData Wrong = Data;
    Wrong.Password = QStringLiteral("wrong password");
    Terminal T(Wrong, &UI);
    OperationResult R = DoOpen(T);
    CHECK_MSG(!R.Ok && R.Fatal, Describe(R));
    CHECK_MSG(R.Message == QStringLiteral("Authentication failed."), Describe(R));
    // the retry asks for a new password (TFTPFileSystem::Open)
    CHECK((UI.PromptKinds.size() == 1) && (UI.PromptKinds.value(0) == UserInterface::pkPassword));
    CHECK(UI.PromptInstructions.value(0) == QStringLiteral("Access denied."));
    CHECK(!T.Active());
  }
  std::printf("[wrong password, then correct one]\n");
  {
    UI.Reset();
    UI.PromptAnswers = {Data.Password};
    SessionData Wrong = Data;
    Wrong.Password = QStringLiteral("wrong password");
    Terminal T(Wrong, &UI);
    OperationResult R = DoOpen(T);
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(UI.PromptKinds.size() == 1);
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
  }
  std::printf("[password prompt cancelled]\n");
  {
    UI.Reset();
    SessionData NoPassword = Data;
    NoPassword.Password.clear();
    Terminal T(NoPassword, &UI);
    OperationResult R = DoOpen(T);
    CHECK_MSG(!R.Ok && R.Fatal, Describe(R));
  }
  if (qEnvironmentVariable("NIXSCP_TEST_FTP_ANONYMOUS") != QStringLiteral("0"))
  {
    std::printf("[anonymous]\n");
    UI.Reset();
    SessionData Anonymous = Data;
    Anonymous.UserName = QStringLiteral("anonymous");
    Anonymous.Password.clear();
    Terminal T(Anonymous, &UI);
    OperationResult R = DoOpen(T);
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(UI.PromptKinds.isEmpty());
    R = DoList(T, T.CurrentDirectory());
    CHECK_MSG(R.Ok, Describe(R));
  }
  std::printf("[connection refused]\n");
  {
    UI.Reset();
    SessionData Refused = Data;
    Refused.PortNumber = 1;
    Terminal T(Refused, &UI);
    OperationResult R = DoOpen(T);
    CHECK_MSG(!R.Ok && R.Fatal && (R.Message == QStringLiteral("Connection failed.")), Describe(R));
  }

  // ---------------------------------------------------------------- suites
  RunSuite(Data, QStringLiteral("plain"), C, true);

  std::printf("[active mode]\n");
  {
    SessionData Active = Data;
    Active.FtpPasvMode = false;
    RunSuite(Active, QStringLiteral("active"), C, false);
  }

  QByteArray ListPort = qgetenv("NIXSCP_TEST_FTP_LIST_PORT");
  if (!ListPort.isEmpty())
  {
    SessionData List = Data;
    List.PortNumber = ListPort.toInt();
    RunSuite(List, QStringLiteral("list"), C, true);
  }

  QByteArray FtpsPort = qgetenv("NIXSCP_TEST_FTPS_PORT");
  if (!FtpsPort.isEmpty())
  {
    SessionData Ftps = Data;
    Ftps.Ftps = ftpsExplicitTls;
    Ftps.PortNumber = FtpsPort.toInt();

    std::printf("[certificate: cancel]\n");
    {
      UI.Reset();
      Terminal T(Ftps, &UI);
      OperationResult R = DoOpen(T);
      CHECK_MSG(!R.Ok && R.Fatal && (R.Message == QStringLiteral("Peer certificate rejected.")), Describe(R));
      CHECK(UI.Queries.size() == 1);
      QString Query = UI.Queries.value(0);
      CHECK(Query.contains(QStringLiteral("The server's certificate is not known")));
      CHECK(Query.contains(QStringLiteral("SHA-256: ")));
      CHECK(Query.contains(QStringLiteral("Self-signed certificate.")));
      CHECK(Query.contains(QStringLiteral("When connecting using an IP address")));
      CHECK((Query.contains(QStringLiteral("NixSCP Test")) || Query.contains(QStringLiteral("LinSCP Test"))));
    }
    std::printf("[certificate: accept once]\n");
    {
      UI.Reset();
      UI.QueryAnswers = {qaNo};
      Terminal T(Ftps, &UI);
      OperationResult R = DoOpen(T);
      CHECK_MSG(R.Ok, Describe(R));
      CHECK(UI.Queries.size() == 1);
      CHECK(T.ServerInfo().contains(QStringLiteral("TLS/SSL Explicit encryption")));
    }
    std::printf("[certificate: accept and remember]\n");
    {
      UI.Reset();
      UI.QueryAnswers = {qaYes};
      Terminal T(Ftps, &UI);
      OperationResult R = DoOpen(T);
      CHECK_MSG(R.Ok, Describe(R));
      CHECK(UI.Queries.size() == 1);
      UI.Reset();
      Terminal T2(Ftps, &UI);
      R = DoOpen(T2);
      CHECK_MSG(R.Ok, Describe(R));
      CHECK(UI.Queries.isEmpty());
    }
    RunSuite(Ftps, QStringLiteral("explicit"), C, true);
  }

  QByteArray ImplicitPort = qgetenv("NIXSCP_TEST_FTPS_IMPLICIT_PORT");
  if (!ImplicitPort.isEmpty())
  {
    SessionData Implicit = Data;
    Implicit.Ftps = ftpsImplicit;
    Implicit.PortNumber = ImplicitPort.toInt();
    RunSuite(Implicit, QStringLiteral("implicit"), C, false);
    std::printf("[implicit: wrong protocol]\n");
    {
      // implicit TLS against a plain FTP server
      UI.Reset();
      SessionData Wrong = Implicit;
      Wrong.PortNumber = Data.PortNumber;
      Wrong.Timeout = 3;
      Terminal T(Wrong, &UI);
      OperationResult R = DoOpen(T);
      CHECK_MSG(!R.Ok && R.Fatal, Describe(R));
      Log(Describe(R));
    }
  }

  std::printf("%s\n", (Failures == 0) ? "test_ftp: all tests passed" : "test_ftp: FAILURES");
  return (Failures == 0) ? 0 : 1;
}
