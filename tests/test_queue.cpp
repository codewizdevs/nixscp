// End-to-end test of core/Queue (TransferQueue) against a real SSH server.
//
// Skipped (exit 0) unless NIXSCP_TEST_SSH_PORT is set. Environment as in test_sftp.cpp:
//   NIXSCP_TEST_SSH_PORT        server port (required to run)
//   NIXSCP_TEST_SSH_HOST        default 127.0.0.1
//   NIXSCP_TEST_SSH_USER        default $USER
//   NIXSCP_TEST_SSH_KEY         private key accepted by the server (required)
// The server must run on this machine: the "remote" directory is a local
// temporary directory, which is also used to verify results.
// The user's ~/.ssh/known_hosts is never touched: a temporary known_hosts
// file is passed via SessionData::KnownHostsFile, and host keys are accepted
// "once" only, so each new connection asks again; this counts connections.
// Queries of background connections wait for TransferQueue::ProcessUserAction
// (Show Query / QueueAutoPopup); the Monitor answers them like the auto popup.
// Configuration::Instance() is never loaded (defaults only).
#include "core/Queue.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QPointer>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <cstdio>
#include <functional>

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
  std::atomic<int> HostKeyCalls{0};
  std::atomic<int> QueryCalls{0};
  std::atomic<unsigned int> QueryAnswer{qaYes};

  HostKeyAnswer VerifyHostKey(const QString &, int, const QString &, const QString &, const QString &, HostKeyState) override
  {
    HostKeyCalls++;
    return HostKeyOnce; // not cached: every connection asks, so they can be counted
  }

  bool PromptUser(PromptKind Kind, const QString & Name, const QString &, QList<Prompt> &) override
  {
    Log(QStringLiteral("unexpected PromptUser kind=%1 name=%2").arg(Kind).arg(Name));
    return false;
  }

  void DisplayBanner(const QString &, const QString &) override
  {
  }

  unsigned int QueryUser(const QString & Query, const QString &, unsigned int) override
  {
    QueryCalls++;
    Log(QStringLiteral("QueryUser \"%1\" -> %2").arg(Query).arg(QueryAnswer.load()));
    return QueryAnswer;
  }
};

// Records finished items and the maximal number of items running at once
struct Monitor
{
  struct Finished
  {
    int Id;
    OperationResult Result;
    bool ToRemote;
    QString Target;
    bool Move;
  };
  QList<Finished> Done;
  int MaxRunning = 0;
  int MaxConnections = 0;    // connections of one item at once
  int UserActions = 0;       // UserActionPending signals
  int Emptied = 0;
  bool AutoProcess = true;   // answer user actions at once (QueueAutoPopup)
  TransferQueue * Queue = nullptr;

  explicit Monitor(TransferQueue * AQueue, bool AAutoProcess = true) : AutoProcess(AAutoProcess), Queue(AQueue)
  {
    QObject::connect(Queue, &TransferQueue::ItemFinished, Queue,
      [this](int Id, const OperationResult & Result, bool ToRemote, const QString & Target, bool Move)
      {
        Done.append(Finished{Id, Result, ToRemote, Target, Move});
      });
    QObject::connect(Queue, &TransferQueue::Changed, Queue, [this]() { Sample(); });
    QObject::connect(Queue, &TransferQueue::Emptied, Queue, [this]() { Emptied++; });
    QObject::connect(Queue, &TransferQueue::UserActionPending, Queue, [this](int Id)
    {
      UserActions++;
      if (AutoProcess)
      {
        QPointer<TransferQueue> Guard(Queue);
        QTimer::singleShot(0, Queue, [Guard, Id]()
        {
          if (Guard)
          {
            Guard->ProcessUserAction(Id);
          }
        });
      }
    });
  }

  void Sample()
  {
    int Running = 0;
    for (const QueueItemInfo & Info : Queue->Items())
    {
      if ((Info.State == QueueItemInfo::qsConnecting) || (Info.State == QueueItemInfo::qsProcessing) ||
          ((Info.State == QueueItemInfo::qsPaused) && Info.Progress.InProgress))
      {
        Running++;
      }
      MaxConnections = std::max(MaxConnections, Info.Connections);
    }
    MaxRunning = std::max(MaxRunning, Running);
  }

  const Finished * Find(int Id) const
  {
    for (const Finished & F : Done)
    {
      if (F.Id == Id)
      {
        return &F;
      }
    }
    return nullptr;
  }
};

static bool WaitFor(const std::function<bool()> & Condition, int TimeoutMs = 60000, Monitor * M = nullptr)
{
  QElapsedTimer Timer;
  Timer.start();
  while (!Condition())
  {
    if (Timer.hasExpired(TimeoutMs))
    {
      return false;
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    if (M != nullptr)
    {
      M->Sample();
    }
    QThread::msleep(5);
  }
  return true;
}

static void Spin(int Ms, Monitor * M = nullptr)
{
  WaitFor([]() { return false; }, Ms, M);
}

static QueueItemInfo Info(TransferQueue & Q, int Id)
{
  for (const QueueItemInfo & I : Q.Items())
  {
    if (I.Id == Id)
    {
      return I;
    }
  }
  QueueItemInfo None;
  None.Id = 0;
  return None;
}

static QList<int> Ids(TransferQueue & Q)
{
  QList<int> Result;
  for (const QueueItemInfo & I : Q.Items())
  {
    Result.append(I.Id);
  }
  return Result;
}

static QString IdsStr(const QList<int> & List)
{
  QStringList S;
  for (int Id : List)
  {
    S << QString::number(Id);
  }
  return S.join(QLatin1Char(','));
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

static void WriteRandomFile(const QString & FileName, qint64 Size, quint32 Seed)
{
  QByteArray Data(Size, Qt::Uninitialized);
  QRandomGenerator Gen(Seed);
  Gen.fillRange(reinterpret_cast<quint32 *>(Data.data()), Size / 4);
  QFile F(FileName);
  if (!F.open(QIODevice::WriteOnly | QIODevice::Truncate) || (F.write(Data) != Data.size()))
  {
    std::printf("FAIL cannot write %s\n", qPrintable(FileName));
    Failures++;
  }
}

static QString Describe(const OperationResult & R)
{
  return QStringLiteral("ok=%1 aborted=%2 fatal=%3 msg=\"%4\"").arg(R.Ok).arg(R.Aborted).arg(R.Fatal).arg(R.Message);
}

int main(int argc, char ** argv)
{
  QCoreApplication App(argc, argv);

  QByteArray PortStr = qgetenv("NIXSCP_TEST_SSH_PORT");
  if (PortStr.isEmpty())
  {
    std::printf("test_queue: SKIPPED (set NIXSCP_TEST_SSH_PORT, NIXSCP_TEST_SSH_KEY [, NIXSCP_TEST_SSH_HOST, NIXSCP_TEST_SSH_USER])\n");
    return 0;
  }
  QString Host = qEnvironmentVariable("NIXSCP_TEST_SSH_HOST", QStringLiteral("127.0.0.1"));
  QString User = qEnvironmentVariable("NIXSCP_TEST_SSH_USER", qEnvironmentVariable("USER"));
  QString KeyFile = qEnvironmentVariable("NIXSCP_TEST_SSH_KEY");
  if (KeyFile.isEmpty())
  {
    std::printf("test_queue: NIXSCP_TEST_SSH_KEY not set\n");
    return 1;
  }

  QTemporaryDir Scratch;
  CHECK(Scratch.isValid());
  QString KnownHosts = Scratch.filePath(QStringLiteral("dotssh/known_hosts"));
  QString Local = Scratch.filePath(QStringLiteral("local"));
  QString Remote = Scratch.filePath(QStringLiteral("remote"));
  QString Back = Scratch.filePath(QStringLiteral("back"));
  QDir().mkpath(Local);
  QDir().mkpath(Remote);
  QDir().mkpath(Back);

  SessionData Data;
  Data.HostName = Host;
  Data.PortNumber = PortStr.toInt();
  Data.UserName = User;
  Data.PublicKeyFile = KeyFile;
  Data.TryAgent = false;
  Data.AuthKI = false;
  Data.KnownHostsFile = KnownHosts;

  TestUserInterface UI;
  CopyParam Param;

  const qint64 MB = 1024 * 1024;
  QStringList Small;
  for (int Index = 0; Index < 5; Index++)
  {
    QString Name = Local + QStringLiteral("/file%1.bin").arg(Index);
    WriteRandomFile(Name, 4 * MB + Index * 1000, 100 + Index);
    Small << Name;
  }
  QString Big = Local + QStringLiteral("/big.bin");
  WriteRandomFile(Big, 20 * MB, 7);

  // ------------------------------------------------------------ uploads, limit 2
  std::printf("[5 uploads, limit 2]\n");
  {
    UI.HostKeyCalls = 0;
    TransferQueue Q(Data, &UI);
    Q.SetKeepDoneItemsFor(-1);
    Monitor M(&Q);
    CHECK(Q.TransfersLimit() == 2);
    CHECK(Q.Enabled());
    CHECK(Q.IsEmpty());
    QList<int> Added;
    for (const QString & File : Small)
    {
      Added << Q.AddUpload({File}, Remote, Param, false);
    }
    CHECK(Q.ActiveCount() == 5);
    CHECK(!Q.IsEmpty());
    CHECK(Ids(Q) == Added);
    CHECK(Q.Items().first().State == QueueItemInfo::qsPending);
    CHECK(WaitFor([&]() { return M.Done.size() == 5; }, 120000, &M));
    CHECK_MSG(M.MaxRunning == 2, QString::number(M.MaxRunning));
    CHECK_MSG(UI.HostKeyCalls == 2, QString::number(UI.HostKeyCalls)); // two connections, reused
    for (const Monitor::Finished & F : M.Done)
    {
      CHECK_MSG(F.Result.Ok, Describe(F.Result));
      CHECK(F.ToRemote && (F.Target == Remote) && !F.Move);
    }
    for (const QString & File : Small)
    {
      QString Target = Remote + QLatin1Char('/') + QFileInfo(File).fileName();
      CHECK_MSG(Sha256(File) == Sha256(Target), Target);
    }
    CHECK(Q.ActiveCount() == 0);
    QList<QueueItemInfo> Items = Q.Items();
    CHECK(Items.size() == 5);
    for (const QueueItemInfo & I : Items)
    {
      CHECK(I.State == QueueItemInfo::qsDone);
      CHECK_MSG(I.Progress.TotalTransferred == QFileInfo(I.Sources.first()).size(), QString::number(I.Progress.TotalTransferred));
      CHECK(!I.Progress.InProgress);
    }

    // ---------------------------------------------------------- downloads, reusing the connections
    std::printf("[downloads on reused connections]\n");
    M.Done.clear();
    QStringList RemoteFiles;
    for (const QString & File : Small)
    {
      RemoteFiles << Remote + QLatin1Char('/') + QFileInfo(File).fileName();
    }
    int D1 = Q.AddDownload(RemoteFiles.mid(0, 3), Back, Param, false);
    int D2 = Q.AddDownload(RemoteFiles.mid(3), Back, Param, false);
    CHECK(WaitFor([&]() { return M.Done.size() == 2; }, 120000, &M));
    CHECK(M.Find(D1) && M.Find(D1)->Result.Ok && !M.Find(D1)->ToRemote && (M.Find(D1)->Target == Back));
    CHECK(M.Find(D2) && M.Find(D2)->Result.Ok);
    CHECK_MSG(UI.HostKeyCalls == 2, QString::number(UI.HostKeyCalls));
    for (const QString & File : Small)
    {
      CHECK_MSG(Sha256(File) == Sha256(Back + QLatin1Char('/') + QFileInfo(File).fileName()), File);
    }
    CHECK(Info(Q, D1).Progress.Count == 3);
    CHECK(Info(Q, D1).Progress.FilesFinished == 3);

    // ---------------------------------------------------------- DeleteAllDone keeps errors
    std::printf("[error item, DeleteAllDone]\n");
    M.Done.clear();
    int E = Q.AddUpload({Local + QStringLiteral("/does-not-exist.bin")}, Remote, Param, false);
    CHECK(WaitFor([&]() { return M.Done.size() == 1; }, 60000, &M));
    CHECK(M.Find(E) && !M.Find(E)->Result.Ok && !M.Find(E)->Result.Aborted);
    QueueItemInfo EI = Info(Q, E);
    CHECK(EI.State == QueueItemInfo::qsError);
    CHECK(EI.Finished);
    CHECK_MSG(!EI.Error.isEmpty(), EI.Error);
    Log(QStringLiteral("error: %1 / %2").arg(EI.Error, EI.MoreMessages));
    CHECK(Q.Items().size() == 8);
    Q.DeleteAllDone();
    CHECK_MSG(Ids(Q) == QList<int>{E}, IdsStr(Ids(Q)));
    Q.Delete(E);
    CHECK(Q.IsEmpty());
    // per-file error of the transfer: asked (Retry/Skip/Abort) as an "Error" user action
    std::printf("[per-file error query: Skip, Abort]\n");
    M.Done.clear();
    UI.QueryAnswer = qaSkip;
    int E2 = Q.AddUpload({Small.first()}, Remote + QStringLiteral("/nonexistent/dir"), Param, false);
    CHECK(WaitFor([&]() { return M.Done.size() == 1; }, 60000, &M));
    CHECK(M.Find(E2) && M.Find(E2)->Result.Ok);
    M.Done.clear();
    UI.QueryAnswer = qaAbort;
    int E3 = Q.AddUpload({Small.first()}, Remote + QStringLiteral("/nonexistent/dir"), Param, false);
    CHECK(WaitFor([&]() { return M.Done.size() == 1; }, 60000, &M));
    CHECK(M.Find(E3) && M.Find(E3)->Result.Aborted);
    CHECK(Info(Q, E3).Id == 0);
    Q.DeleteAllDone();
    CHECK(Q.IsEmpty());
    // connection survived the (non-fatal) error
    M.Done.clear();
    int After = Q.AddUpload({Small.first()}, Back, Param, false);
    UI.QueryAnswer = qaYes; // overwrite
    CHECK(WaitFor([&]() { return M.Done.size() == 1; }, 60000, &M));
    CHECK(M.Find(After) && M.Find(After)->Result.Ok);
    CHECK_MSG(UI.HostKeyCalls == 2, QString::number(UI.HostKeyCalls));
  }

  // ------------------------------------------------------------ pause / resume
  std::printf("[pause/resume of a 20 MB item]\n");
  {
    TransferQueue Q(Data, &UI);
    Q.SetKeepDoneItemsFor(-1);
    Monitor M(&Q);
    QString Target = Scratch.filePath(QStringLiteral("remote/big.bin"));
    int Id = Q.AddUpload({Big}, Remote, Param, false);
    // suspend as soon as the transfer starts
    bool Paused = false;
    QObject::connect(&Q, &TransferQueue::Changed, &Q, [&]()
    {
      if (!Paused && (Info(Q, Id).State == QueueItemInfo::qsProcessing))
      {
        Paused = true;
        Q.Pause(Id);
      }
    });
    CHECK(WaitFor([&]() { return Paused || !M.Done.isEmpty(); }, 60000));
    CHECK(Info(Q, Id).State == QueueItemInfo::qsPaused);
    Spin(500);
    qint64 T1 = Info(Q, Id).Progress.TotalTransferred;
    Spin(1000);
    QueueItemInfo PI = Info(Q, Id);
    CHECK_MSG(PI.Progress.TotalTransferred == T1, QStringLiteral("%1 %2").arg(T1).arg(PI.Progress.TotalTransferred));
    CHECK(PI.Progress.TotalTransferred < 20 * MB);
    CHECK(PI.State == QueueItemInfo::qsPaused);
    CHECK(M.Done.isEmpty());
    CHECK(Q.ActiveCount() == 1);
    Log(QStringLiteral("paused at %1 bytes").arg(T1));
    Q.Resume(Id);
    CHECK(Info(Q, Id).State == QueueItemInfo::qsProcessing);
    // suspend again in the middle of the file
    if (WaitFor([&]() { return !M.Done.isEmpty() || (Info(Q, Id).Progress.TotalTransferred > T1 + 2 * MB); }, 60000) &&
        M.Done.isEmpty())
    {
      Q.Pause(Id);
      Spin(500);
      qint64 T2 = Info(Q, Id).Progress.TotalTransferred;
      Spin(1000);
      CHECK_MSG(Info(Q, Id).Progress.TotalTransferred == T2, QStringLiteral("%1 %2").arg(T2).arg(Info(Q, Id).Progress.TotalTransferred));
      CHECK(M.Done.isEmpty());
      Log(QStringLiteral("paused again at %1 bytes").arg(T2));
      Q.Resume(Id);
    }
    else
    {
      Log(QStringLiteral("transfer finished before it could be suspended in the middle"));
    }
    CHECK(WaitFor([&]() { return M.Done.size() == 1; }, 120000));
    CHECK(M.Find(Id) && M.Find(Id)->Result.Ok);
    CHECK(Sha256(Big) == Sha256(Target));

    // pending item suspended before it starts does not start; PauseAll/ResumeAll
    std::printf("[suspended pending item, ResumeAll]\n");
    M.Done.clear();
    UI.QueryAnswer = qaYes;
    Q.SetEnabled(false);
    int P1 = Q.AddUpload({Small[1]}, Remote, Param, false);
    Q.Pause(P1);
    CHECK(Info(Q, P1).State == QueueItemInfo::qsPaused);
    Q.SetEnabled(true);
    Spin(1000);
    CHECK(Info(Q, P1).State == QueueItemInfo::qsPaused);
    CHECK(M.Done.isEmpty());
    Q.ResumeAll();
    CHECK(WaitFor([&]() { return M.Done.size() == 1; }, 60000));
    CHECK(M.Find(P1) && M.Find(P1)->Result.Ok);
  }

  // ------------------------------------------------------------ delete of a processing item
  std::printf("[delete processing item]\n");
  {
    TransferQueue Q(Data, &UI);
    Monitor M(&Q);
    UI.QueryAnswer = qaYes;
    // deterministic: suspended first, then deleted
    int Id = Q.AddUpload({Big}, Remote, Param, false);
    QObject::connect(&Q, &TransferQueue::Changed, &Q, [&]()
    {
      QueueItemInfo I = Info(Q, Id);
      if ((I.Id != 0) && (I.State == QueueItemInfo::qsProcessing))
      {
        Q.Pause(Id);
      }
    });
    CHECK(WaitFor([&]() { return Info(Q, Id).State == QueueItemInfo::qsPaused; }, 60000));
    Q.Delete(Id);
    CHECK(Info(Q, Id).Id == 0);  // gone from the list at once
    CHECK(Q.IsEmpty());
    CHECK(WaitFor([&]() { return M.Done.size() == 1; }, 60000));
    CHECK(M.Find(Id) && M.Find(Id)->Result.Aborted);
    CHECK(Q.IsEmpty());

    // the connection is reused for the next item
    int HostKeyCalls = UI.HostKeyCalls;
    M.Done.clear();
    int Next = Q.AddUpload({Small[2]}, Remote, Param, false);
    CHECK(WaitFor([&]() { return M.Done.size() == 1; }, 60000));
    CHECK(M.Find(Next) && M.Find(Next)->Result.Ok);
    CHECK(UI.HostKeyCalls == HostKeyCalls);

    // running (not suspended) item deleted
    M.Done.clear();
    int Id2 = Q.AddUpload({Big}, Remote, Param, false);
    CHECK(WaitFor([&]() { return Info(Q, Id2).Progress.TotalTransferred > 0; }, 60000));
    Q.Delete(Id2);
    CHECK(Info(Q, Id2).Id == 0);
    CHECK(WaitFor([&]() { return M.Done.size() == 1; }, 60000));
    CHECK(M.Find(Id2) != nullptr);
    if (M.Find(Id2) != nullptr)
    {
      Log(QStringLiteral("running item deleted: %1").arg(Describe(M.Find(Id2)->Result)));
    }
  }

  // ------------------------------------------------------------ order, SetEnabled, Execute Now
  std::printf("[move up/down, SetEnabled(false), execute now]\n");
  {
    TransferQueue Q(Data, &UI);
    Q.SetKeepDoneItemsFor(-1);
    Monitor M(&Q);
    UI.QueryAnswer = qaYes;
    Q.SetTransfersLimit(1);
    Q.SetEnabled(false);
    CHECK(!Q.Enabled());
    int A = Q.AddUpload({Small[0]}, Remote, Param, false);
    int B = Q.AddUpload({Small[1]}, Remote, Param, false);
    int C = Q.AddUpload({Small[2]}, Remote, Param, false);
    CHECK((Ids(Q) == QList<int>{A, B, C}));
    Q.MoveDown(A);
    CHECK_MSG((Ids(Q) == QList<int>{B, A, C}), IdsStr(Ids(Q)));
    Q.MoveUp(C);
    CHECK_MSG((Ids(Q) == QList<int>{B, C, A}), IdsStr(Ids(Q)));
    Q.MoveUp(B);   // already first
    Q.MoveDown(A); // already last
    CHECK_MSG((Ids(Q) == QList<int>{B, C, A}), IdsStr(Ids(Q)));
    Spin(1000);
    CHECK(UI.HostKeyCalls >= 0);
    for (const QueueItemInfo & I : Q.Items())
    {
      CHECK(I.State == QueueItemInfo::qsPending);
    }
    CHECK(M.Done.isEmpty());

    // Execute Now while processing is disabled
    Q.Resume(A);
    CHECK_MSG((Ids(Q) == QList<int>{A, B, C}), IdsStr(Ids(Q)));
    CHECK(WaitFor([&]() { return M.Done.size() == 1; }, 60000));
    CHECK(M.Find(A) && M.Find(A)->Result.Ok);
    Spin(500);
    CHECK(M.Done.size() == 1);
    CHECK(Info(Q, B).State == QueueItemInfo::qsPending);
    // a started item cannot move
    Q.MoveDown(A);
    CHECK_MSG((Ids(Q) == QList<int>{A, B, C}), IdsStr(Ids(Q)));
    Q.MoveUp(B);
    CHECK_MSG((Ids(Q) == QList<int>{A, B, C}), IdsStr(Ids(Q)));
    Q.MoveDown(B);
    CHECK_MSG((Ids(Q) == QList<int>{A, C, B}), IdsStr(Ids(Q)));

    Q.SetEnabled(true);
    CHECK(WaitFor([&]() { return M.Done.size() == 3; }, 120000, &M));
    CHECK(M.Done.size() == 3);
    if (M.Done.size() == 3)
    {
      CHECK(M.Done[1].Id == C);
      CHECK(M.Done[2].Id == B);
    }
    CHECK_MSG(M.MaxRunning == 1, QString::number(M.MaxRunning));
  }

  // ------------------------------------------------------------ connection failure
  std::printf("[connection failure]\n");
  {
    SessionData BadData = Data;
    BadData.PortNumber = 1;
    TransferQueue Q(BadData, &UI);
    Monitor M(&Q);
    int Id1 = Q.AddUpload({Small[0]}, Remote, Param, false);
    int Id2 = Q.AddUpload({Small[1]}, Remote, Param, false);
    CHECK(WaitFor([&]() { return M.Done.size() == 2; }, 60000));
    QueueItemInfo I1 = Info(Q, Id1);
    CHECK(I1.State == QueueItemInfo::qsError);
    CHECK(!I1.Error.isEmpty());
    CHECK(Info(Q, Id2).State == QueueItemInfo::qsError);
    CHECK(M.Find(Id1) && !M.Find(Id1)->Result.Ok);
    Log(QStringLiteral("connection error: %1").arg(I1.Error));
    // the error waits to be acknowledged (Show Error); OK retries the connection
    CHECK(I1.Finished);
    CHECK(Q.UserActionItem() == Id1);
    CHECK(Q.ActiveCount() == 0);
    CHECK_MSG(M.Emptied == 0, QString::number(M.Emptied));
    M.Done.clear();
    UI.QueryAnswer = qaOK;
    CHECK(Q.ProcessUserAction(Id1));
    CHECK(Info(Q, Id1).Id == Id1);
    CHECK(!Info(Q, Id1).Finished);
    CHECK(WaitFor([&]() { return M.Done.size() == 1; }, 60000));
    CHECK(Info(Q, Id1).State == QueueItemInfo::qsError);
    // Cancel removes it
    UI.QueryAnswer = qaCancel;
    CHECK(Q.ProcessUserAction(Id1));
    CHECK(Info(Q, Id1).Id == 0);
    UI.QueryAnswer = qaOK;
    CHECK(Q.ProcessUserAction(Id2));   // OK: retried, fails again
    CHECK(WaitFor([&]() { return Info(Q, Id2).Finished; }, 60000));
    UI.QueryAnswer = qaCancel;
    CHECK(Q.ProcessUserAction(Id2));
    CHECK(Q.IsEmpty());
    CHECK_MSG(M.Emptied == 1, QString::number(M.Emptied));
    UI.QueryAnswer = qaYes;
  }

  // ------------------------------------------------------------ move semantics
  std::printf("[move upload]\n");
  {
    TransferQueue Q(Data, &UI);
    Monitor M(&Q);
    QString Temp = Local + QStringLiteral("/tomove.bin");
    WriteRandomFile(Temp, MB, 99);
    QByteArray Hash = Sha256(Temp);
    int Id = Q.AddUpload({Temp}, Remote, Param, true);
    CHECK(WaitFor([&]() { return M.Done.size() == 1; }, 60000));
    CHECK(M.Find(Id) && M.Find(Id)->Result.Ok && M.Find(Id)->Move);
    CHECK(!QFile::exists(Temp));
    CHECK(Sha256(Remote + QStringLiteral("/tomove.bin")) == Hash);
  }

  // ------------------------------------------------------------ destruction while running
  std::printf("[destroy queue while items run]\n");
  {
    QElapsedTimer Timer;
    Timer.start();
    auto * Q = new TransferQueue(Data, &UI);
    Monitor M(Q);
    for (int Index = 0; Index < 4; Index++)
    {
      Q->AddUpload({Big}, Remote, Param, false);
    }
    CHECK(WaitFor([&]() { return M.MaxRunning == 2; }, 60000, &M));
    Spin(100);
    delete Q;
    Spin(300);
    Log(QStringLiteral("destroyed after %1 ms").arg(Timer.elapsed()));
  }
  {
    // while connecting
    auto * Q = new TransferQueue(Data, &UI);
    Q->AddUpload({Big}, Remote, Param, false);
    Q->AddDownload({Remote + QStringLiteral("/big.bin")}, Back, Param, false);
    QCoreApplication::processEvents();
    CHECK(Q->Items().first().State == QueueItemInfo::qsConnecting);
    delete Q;
    Spin(300);
  }
  {
    // deleted item still cancelling and a suspended one
    auto * Q = new TransferQueue(Data, &UI);
    Q->SetUserActionsDeferred(false);
    int Id = Q->AddUpload({Big}, Remote, Param, false);
    int Id2 = Q->AddUpload({Big}, Remote, Param, false);
    CHECK(WaitFor([&]() { return Info(*Q, Id).Progress.TotalTransferred > 0; }, 60000));
    Q->Pause(Id2);
    Q->Delete(Id);
    delete Q;
    Spin(300);
  }
  {
    // receiver of ItemFinished deletes the queue
    auto * Q = new TransferQueue(Data, &UI);
    Q->SetUserActionsDeferred(false);
    QPointer<TransferQueue> Guard(Q);
    Q->AddUpload({Small[0]}, Remote, Param, false);
    Q->AddUpload({Small[1]}, Remote, Param, false);
    QObject::connect(Q, &TransferQueue::ItemFinished, Q, [Q]() { delete Q; });
    CHECK(WaitFor([&]() { return Guard.isNull(); }, 60000));
    Spin(300);
  }

  // ------------------------------------------------------------ deferred user actions
  std::printf("[deferred host key query, Show Query]\n");
  {
    TransferQueue Q(Data, &UI);
    Q.SetKeepDoneItemsFor(-1);
    Monitor M(&Q, false);
    int HostKeyCalls = UI.HostKeyCalls;
    int Id = Q.AddUpload({Small[0]}, Remote, Param, false);
    CHECK(WaitFor([&]() { return Info(Q, Id).State == QueueItemInfo::qsQuery; }, 30000));
    CHECK(M.UserActions == 1);
    CHECK(Q.UserActionItem() == Id);
    CHECK(!Info(Q, Id).UserActionText.isEmpty());
    CHECK(UI.HostKeyCalls == HostKeyCalls);   // not asked yet
    Spin(300);
    CHECK(Info(Q, Id).State == QueueItemInfo::qsQuery);
    CHECK(M.Done.isEmpty());
    CHECK(Q.ProcessUserAction(Id));
    CHECK(UI.HostKeyCalls == HostKeyCalls + 1);
    CHECK(WaitFor([&]() { return M.Done.size() == 1; }, 60000));
    CHECK(M.Find(Id) && M.Find(Id)->Result.Ok);
    CHECK(Q.UserActionItem() == 0);
    CHECK(!Q.ProcessUserAction(Id));
    CHECK_MSG(M.Emptied == 1, QString::number(M.Emptied));

    // overwrite query of a transfer without QueueNoConfirmation, cancelled by Delete
    std::printf("[overwrite query, delete while waiting]\n");
    CopyParam Confirm = Param;
    Confirm.QueueNoConfirmation = false;
    Confirm.ConfirmOverwriting = true;
    M.Done.clear();
    int Id2 = Q.AddUpload({Small[0]}, Remote, Confirm, false);
    CHECK(WaitFor([&]() { return Info(Q, Id2).State == QueueItemInfo::qsQuery; }, 30000));
    Log(QStringLiteral("query: %1").arg(Info(Q, Id2).UserActionText));
    Q.Delete(Id2);
    CHECK(Info(Q, Id2).Id == 0);
    CHECK(WaitFor([&]() { return M.Done.size() == 1; }, 30000));
    CHECK(M.Find(Id2) && M.Find(Id2)->Result.Aborted);

    // the same answered "No": nothing transferred, item done
    M.Done.clear();
    int QueryCalls = UI.QueryCalls;
    int Id3 = Q.AddUpload({Small[0]}, Remote, Confirm, false);
    CHECK(WaitFor([&]() { return Info(Q, Id3).State == QueueItemInfo::qsQuery; }, 30000));
    UI.QueryAnswer = qaNo;
    CHECK(Q.ProcessUserAction(Id3));
    CHECK(UI.QueryCalls == QueryCalls + 1);
    CHECK(WaitFor([&]() { return M.Done.size() == 1; }, 30000));
    CHECK(M.Find(Id3) && M.Find(Id3)->Result.Ok);
    UI.QueryAnswer = qaYes;

    // QueueNoConfirmation (default): no query at all
    M.Done.clear();
    QueryCalls = UI.QueryCalls;
    int Id4 = Q.AddUpload({Small[0]}, Remote, Param, false);
    CHECK(WaitFor([&]() { return M.Done.size() == 1; }, 30000));
    CHECK(M.Find(Id4) && M.Find(Id4)->Result.Ok);
    CHECK(UI.QueryCalls == QueryCalls);
  }

  // ------------------------------------------------------------ keep done items, cancel all, speed
  std::printf("[keep done items for N s, Cancel All]\n");
  {
    TransferQueue Q(Data, &UI);
    Monitor M(&Q);
    Q.SetKeepDoneItemsFor(1);
    int Id = Q.AddUpload({Small[1]}, Remote, Param, false);
    CHECK(WaitFor([&]() { return M.Done.size() == 1; }, 60000));
    CHECK(Info(Q, Id).State == QueueItemInfo::qsDone);
    CHECK(WaitFor([&]() { return Info(Q, Id).Id == 0; }, 5000));
    Q.SetKeepDoneItemsFor(0);
    M.Done.clear();
    int Id2 = Q.AddUpload({Small[1]}, Remote, Param, false);
    CHECK(WaitFor([&]() { return M.Done.size() == 1; }, 60000));
    CHECK(M.Find(Id2) && M.Find(Id2)->Result.Ok);
    CHECK(Q.IsEmpty());

    Q.SetEnabled(false);
    Q.AddUpload({Small[0]}, Remote, Param, false);
    Q.AddUpload({Small[1]}, Remote, Param, false);
    Q.AddUpload({Small[2]}, Remote, Param, false);
    CHECK(Q.Items().size() == 3);
    Q.DeleteAll();
    CHECK(Q.IsEmpty());
    Q.SetEnabled(true);
  }

  std::printf("[per-item speed limit]\n");
  {
    TransferQueue Q(Data, &UI);
    Q.SetKeepDoneItemsFor(-1);
    Monitor M(&Q);
    CopyParam Limited = Param;
    Limited.CPSLimit = 512 * 1024;
    int Id = Q.AddUpload({Small[2]}, Remote + QStringLiteral("/"), Limited, false);
    CHECK(Info(Q, Id).CPSLimit == 512 * 1024);
    CHECK(WaitFor([&]() { return Info(Q, Id).Progress.TotalTransferred > 0; }, 60000));
    Spin(2000);
    qint64 T1 = Info(Q, Id).Progress.TotalTransferred;
    Log(QStringLiteral("limited to 512 KB/s: %1 bytes after ~2 s").arg(T1));
    CHECK_MSG(M.Done.isEmpty() && (T1 <= 2 * 1024 * 1024), QString::number(T1));
    Q.SetItemCPSLimit(Id, 0);
    CHECK(Info(Q, Id).CPSLimit == 0);
    CHECK(WaitFor([&]() { return M.Done.size() == 1; }, 60000));
    CHECK(M.Find(Id) && M.Find(Id)->Result.Ok);
    CHECK(Sha256(Small[2]) == Sha256(Remote + QStringLiteral("/file2.bin")));
  }

  // ------------------------------------------------------------ background delete
  std::printf("[background delete, remote and local]\n");
  {
    TransferQueue Q(Data, &UI);
    Q.SetKeepDoneItemsFor(-1);
    Monitor M(&Q);
    QString RemoteFile = Remote + QStringLiteral("/todelete.bin");
    QString LocalDir = Local + QStringLiteral("/deldir");
    QDir().mkpath(LocalDir + QStringLiteral("/sub"));
    WriteRandomFile(RemoteFile, 1000, 1);
    WriteRandomFile(LocalDir + QStringLiteral("/sub/a.bin"), 1000, 2);
    int R = Q.AddDelete({RemoteFile}, true);
    int L = Q.AddDelete({LocalDir}, false);
    CHECK(Info(Q, R).Type == QueueItemInfo::ikDelete);
    CHECK(WaitFor([&]() { return M.Done.size() == 2; }, 60000));
    CHECK(M.Find(R) && M.Find(R)->Result.Ok);
    CHECK(M.Find(L) && M.Find(L)->Result.Ok);
    CHECK(!QFile::exists(RemoteFile));
    CHECK(!QFileInfo::exists(LocalDir));
  }

  // ------------------------------------------------------------ parallel transfers
  std::printf("[parallel directory upload/download]\n");
  {
    QString Tree = Local + QStringLiteral("/tree");
    QStringList Files;
    for (int Dir = 0; Dir < 4; Dir++)
    {
      QString Sub = Tree + QStringLiteral("/d%1/s%1").arg(Dir);
      QDir().mkpath(Sub);
      for (int Index = 0; Index < 12; Index++)
      {
        QString Name = ((Index % 2) ? Sub : QFileInfo(Sub).path()) + QStringLiteral("/f%1.bin").arg(Index);
        WriteRandomFile(Name, 32 * 1024 + Index * 333, 1000 + Dir * 100 + Index);
        Files << Name;
      }
    }
    QDir().mkpath(Tree + QStringLiteral("/empty"));
    TransferQueue Q(Data, &UI);
    Q.SetKeepDoneItemsFor(-1);
    Monitor M(&Q);
    // two connections ready: the second joins the transfer at once
    Q.AddUpload({Small[3]}, Remote, Param, false);
    Q.AddUpload({Small[4]}, Remote, Param, false);
    CHECK(WaitFor([&]() { return M.Done.size() == 2; }, 60000, &M));
    M.Done.clear();
    M.MaxConnections = 0;
    CopyParam Parallel = Param;
    Parallel.QueueParallel = true;
    int Up = Q.AddUpload({Tree}, Remote, Parallel, false);
    CHECK(WaitFor([&]() { return M.Done.size() == 1; }, 120000, &M));
    CHECK(M.Find(Up) && M.Find(Up)->Result.Ok);
    if (M.Find(Up) && !M.Find(Up)->Result.Ok)
    {
      Log(Describe(M.Find(Up)->Result));
    }
    CHECK_MSG(M.MaxConnections == 2, QString::number(M.MaxConnections));
    int Bad = 0;
    for (const QString & File : Files)
    {
      QString Target = Remote + QStringLiteral("/tree") + File.mid(Tree.size());
      if (Sha256(File) != Sha256(Target))
      {
        Bad++;
      }
    }
    CHECK_MSG(Bad == 0, QString::number(Bad));
    CHECK(QFileInfo(Remote + QStringLiteral("/tree/empty")).isDir());
    QueueItemInfo UI1 = Info(Q, Up);
    CHECK_MSG(UI1.Progress.FilesFinished == Files.size(), QString::number(UI1.Progress.FilesFinished));
    QList<QPair<QString, QueueFileState>> FileList = Q.ItemFileList(Up);
    CHECK(FileList.size() >= Files.size());
    CHECK(std::all_of(FileList.cbegin(), FileList.cend(), [](const QPair<QString, QueueFileState> & F) { return F.second == qfsProcessed; }));
    Log(QStringLiteral("parallel upload: %1 files, %2 connections").arg(UI1.Progress.FilesFinished).arg(M.MaxConnections));

    M.Done.clear();
    M.MaxConnections = 0;
    QString BackTree = Back + QStringLiteral("/ptree");
    QDir().mkpath(BackTree);
    int Down = Q.AddDownload({Remote + QStringLiteral("/tree")}, BackTree, Parallel, false);
    CHECK(WaitFor([&]() { return M.Done.size() == 1; }, 120000, &M));
    CHECK(M.Find(Down) && M.Find(Down)->Result.Ok);
    CHECK_MSG(M.MaxConnections == 2, QString::number(M.MaxConnections));
    Bad = 0;
    for (const QString & File : Files)
    {
      if (Sha256(File) != Sha256(BackTree + QStringLiteral("/tree") + File.mid(Tree.size())))
      {
        Bad++;
      }
    }
    CHECK_MSG(Bad == 0, QString::number(Bad));
    CHECK(QFileInfo(BackTree + QStringLiteral("/tree/empty")).isDir());

    // segmented download of one large file (ParallelTransferThreshold)
    std::printf("[segmented download]\n");
    M.Done.clear();
    M.MaxConnections = 0;
    Q.SetParallelTransferThreshold(1024);
    QString SegDir = Back + QStringLiteral("/seg");
    QDir().mkpath(SegDir);
    int Seg = Q.AddDownload({Remote + QStringLiteral("/big.bin")}, SegDir, Parallel, false);
    CHECK(WaitFor([&]() { return M.Done.size() == 1; }, 120000, &M));
    CHECK(M.Find(Seg) && M.Find(Seg)->Result.Ok);
    if (M.Find(Seg) && !M.Find(Seg)->Result.Ok)
    {
      Log(Describe(M.Find(Seg)->Result));
    }
    CHECK_MSG(M.MaxConnections == 2, QString::number(M.MaxConnections));
    CHECK(Sha256(Big) == Sha256(SegDir + QStringLiteral("/big.bin")));
    CHECK(QDir(SegDir).entryList(QDir::Files).size() == 1);   // no part files left
    CHECK(Q.ItemFileList(Seg).size() == 1);
    CHECK(Info(Q, Seg).Progress.TotalTransferred == QFileInfo(Big).size());
    // preserved timestamp
    CHECK(QFileInfo(SegDir + QStringLiteral("/big.bin")).lastModified().toSecsSinceEpoch() ==
      QFileInfo(Remote + QStringLiteral("/big.bin")).lastModified().toSecsSinceEpoch());

    // existing target, overwrite declined at the (deferred) query
    std::printf("[segmented download, overwrite declined]\n");
    M.Done.clear();
    CopyParam ConfirmParallel = Parallel;
    ConfirmParallel.QueueNoConfirmation = false;
    UI.QueryAnswer = qaNo;
    QFile::resize(SegDir + QStringLiteral("/big.bin"), 10);
    int Seg2 = Q.AddDownload({Remote + QStringLiteral("/big.bin")}, SegDir, ConfirmParallel, false);
    CHECK(WaitFor([&]() { return M.Done.size() == 1; }, 60000, &M));
    CHECK(M.Find(Seg2) && M.Find(Seg2)->Result.Ok);
    CHECK(QFileInfo(SegDir + QStringLiteral("/big.bin")).size() == 10);
    UI.QueryAnswer = qaYes;

    // non-parallel when QueueParallel is off
    std::printf("[QueueParallel off]\n");
    M.Done.clear();
    M.MaxConnections = 0;
    CopyParam Single = Param;
    Single.QueueParallel = false;
    QString SingleDir = Back + QStringLiteral("/single");
    QDir().mkpath(SingleDir);
    int One = Q.AddDownload({Remote + QStringLiteral("/tree")}, SingleDir, Single, false);
    CHECK(WaitFor([&]() { return M.Done.size() == 1; }, 120000, &M));
    CHECK(M.Find(One) && M.Find(One)->Result.Ok);
    CHECK_MSG(M.MaxConnections == 1, QString::number(M.MaxConnections));
    CHECK(Q.ItemFileList(One).isEmpty());
    CHECK(Sha256(Files.first()) == Sha256(SingleDir + QStringLiteral("/tree") + Files.first().mid(Tree.size())));
  }

  // ------------------------------------------------------------ parallel: cancel and suspend
  std::printf("[parallel transfer cancelled]\n");
  {
    TransferQueue Q(Data, &UI);
    Monitor M(&Q);
    CopyParam Parallel = Param;
    Parallel.QueueParallel = true;
    Q.SetParallelTransferThreshold(1024);
    QString CancelDir = Back + QStringLiteral("/cancel");
    QDir().mkpath(CancelDir);
    Q.AddUpload({Small[0]}, Remote, Param, false);
    Q.AddUpload({Small[1]}, Remote, Param, false);
    CHECK(WaitFor([&]() { return M.Done.size() == 2; }, 60000, &M));
    M.Done.clear();
    int Id = Q.AddDownload({Remote + QStringLiteral("/big.bin")}, CancelDir, Parallel, false);
    CHECK(WaitFor([&]() { return Info(Q, Id).Progress.TotalTransferred > 0; }, 60000, &M));
    Q.Pause(Id);
    Spin(300);
    qint64 T = Info(Q, Id).Progress.TotalTransferred;
    Spin(700);
    CHECK_MSG(Info(Q, Id).Progress.TotalTransferred == T, QStringLiteral("%1 %2").arg(T).arg(Info(Q, Id).Progress.TotalTransferred));
    CHECK(Info(Q, Id).State == QueueItemInfo::qsPaused);
    Q.Delete(Id);
    CHECK(WaitFor([&]() { return M.Done.size() == 1; }, 60000));
    CHECK(M.Find(Id) && M.Find(Id)->Result.Aborted);
    Spin(200);
    CHECK_MSG(QDir(CancelDir).entryList(QDir::Files).isEmpty(), QDir(CancelDir).entryList(QDir::Files).join(QLatin1Char(',')));
    CHECK(Q.ActiveCount() == 0);
  }

  if (Failures == 0)
  {
    std::printf("test_queue: all tests passed\n");
    return 0;
  }
  std::printf("test_queue: %d failure(s)\n", Failures);
  return 1;
}
