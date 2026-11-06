// Tests of the core operation loop of core/Terminal (port of WinSCP
// core/Terminal.cpp FileOperationLoop, TRobustOperationLoop, TRetryOperationLoop,
// ConfirmFileOverwrite, DoRenameOrCopyFile, RecycleFile, Reopen):
// per-file Retry / Skip / Skip all / Abort, Continue on error, Move deleting
// only transferred sources, overwrite answers (No, Yes to All, Newer only,
// New name, Append, Resume), rename/move confirmation, remote recycle bin,
// "Add X to directories", capabilities, missing local target directory,
// remembered password and automatic reconnect.
//
// The unit parts always run. The SFTP parts are skipped unless
// NIXSCP_TEST_SSH_PORT and NIXSCP_TEST_SSH_KEY are set (see test_sftp.cpp);
// the server must run on this machine (remote paths are checked locally).
// The reconnect part additionally needs NIXSCP_TEST_SSH_KILL_PORT and
// NIXSCP_TEST_SSH_KILL_PID: port and pid of a private sshd (same keys) whose
// connections the test may kill. NIXSCP_TEST_SSH_PASSKEY/PASSPHRASE are used
// there to check that the passphrase typed once is reused by the reconnect.
#include "core/Terminal.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QMutex>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTimer>

#include <signal.h>
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

struct QueryRecord
{
  QString Query;
  unsigned int Answers = 0;
  QueryUserParams Params;
  bool Extended = false;
};

class TestUserInterface : public UserInterface
{
public:
  QMutex Mutex;
  QStringList PromptAnswers;
  QList<unsigned int> QueryAnswers;
  QList<QueryRecord> Queries;
  QList<PromptKind> PromptKinds;

  void Reset()
  {
    QMutexLocker Lock(&Mutex);
    PromptAnswers.clear();
    QueryAnswers.clear();
    Queries.clear();
    PromptKinds.clear();
  }
  int QueryCount()
  {
    QMutexLocker Lock(&Mutex);
    return Queries.size();
  }
  QueryRecord Query(int Index)
  {
    QMutexLocker Lock(&Mutex);
    return Queries.value(Index);
  }

  HostKeyAnswer VerifyHostKey(const QString &, int, const QString &, const QString &, const QString &, HostKeyState) override
  {
    return HostKeyOnce;
  }
  bool PromptUser(PromptKind Kind, const QString & Name, const QString &, QList<Prompt> & Prompts) override
  {
    QMutexLocker Lock(&Mutex);
    PromptKinds.append(Kind);
    Log(QStringLiteral("PromptUser kind=%1 name=%2").arg(Kind).arg(Name));
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
  unsigned int Answer(const QString & Query, unsigned int Answers, const QueryUserParams & Params, bool Extended)
  {
    QMutexLocker Lock(&Mutex);
    Queries.append(QueryRecord{Query, Answers, Params, Extended});
    unsigned int Result = QueryAnswers.isEmpty() ? static_cast<unsigned int>(qaCancel) : QueryAnswers.takeFirst();
    Log(QStringLiteral("Query \"%1\" answers=%2 -> %3").arg(Query.left(70).replace(QLatin1Char('\n'), QLatin1Char(' '))).arg(Answers).arg(Result));
    return Result;
  }
  unsigned int QueryUser(const QString & Query, const QString &, unsigned int Answers) override
  {
    return Answer(Query, Answers, QueryUserParams(), false);
  }
  unsigned int QueryUserEx(const QString & Query, const QString &, unsigned int Answers, const QueryUserParams & Params) override
  {
    return Answer(Query, Answers, Params, true);
  }
};

// UI without QueryUserEx: checks the fallback of UserInterface::QueryUserEx
class PlainUserInterface : public UserInterface
{
public:
  unsigned int LastAnswers = 0;
  HostKeyAnswer VerifyHostKey(const QString &, int, const QString &, const QString &, const QString &, HostKeyState) override { return HostKeyCancel; }
  bool PromptUser(PromptKind, const QString &, const QString &, QList<Prompt> &) override { return false; }
  void DisplayBanner(const QString &, const QString &) override {}
  unsigned int QueryUser(const QString &, const QString &, unsigned int Answers) override
  {
    LastAnswers = Answers;
    return qaCancel;
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
    return [this](const OperationResult & R) { Result = R; Called = true; Loop.quit(); };
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
  OperationResult Wait(int TimeoutMs = 120000)
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
  return QStringLiteral("ok=%1 aborted=%2 fatal=%3 msg=\"%4\" more=\"%5\"").arg(R.Ok).arg(R.Aborted).arg(R.Fatal).arg(R.Message, R.MoreMessages);
}

static void WriteFile(const QString & FileName, const QByteArray & Data, qint64 MTime = 0)
{
  QFile F(FileName);
  if (!F.open(QIODevice::WriteOnly | QIODevice::Truncate) || (F.write(Data) != Data.size()))
  {
    std::printf("FAIL cannot write %s\n", qPrintable(FileName));
    Failures++;
  }
  F.close();
  if (MTime > 0)
  {
    struct timespec Times[2] = {{static_cast<time_t>(MTime), 0}, {static_cast<time_t>(MTime), 0}};
    utimensat(AT_FDCWD, QFile::encodeName(FileName).constData(), Times, 0);
  }
}

static QByteArray ReadFile(const QString & FileName)
{
  QFile F(FileName);
  return F.open(QIODevice::ReadOnly) ? F.readAll() : QByteArray("<missing>");
}

static unsigned int LocalMode(const QString & Path)
{
  struct stat St;
  return (stat(QFile::encodeName(Path).constData(), &St) == 0) ? (St.st_mode & 07777) : 0xFFFFFFFF;
}

static QByteArray RandomData(qint64 Size)
{
  QByteArray Data(Size, Qt::Uninitialized);
  QRandomGenerator Gen(4242);
  Gen.fillRange(reinterpret_cast<quint32 *>(Data.data()), Size / 4);
  return Data;
}

static QStringList Entries(const QString & Dir)
{
  return QDir(Dir).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot, QDir::Name);
}

// Kills the per-connection processes of the private sshd (all its
// descendants: the session monitor, the network child, sftp-server)
static int KillConnections(qint64 ParentPid)
{
  QMap<qint64, qint64> Parents;
  const QStringList Pids = QDir(QStringLiteral("/proc")).entryList(QDir::Dirs);
  for (const QString & PidStr : Pids)
  {
    bool Ok = false;
    qint64 Pid = PidStr.toLongLong(&Ok);
    if (!Ok)
    {
      continue;
    }
    QFile Status(QStringLiteral("/proc/%1/status").arg(Pid));
    if (!Status.open(QIODevice::ReadOnly))
    {
      continue;
    }
    for (const QByteArray & Line : Status.readAll().split('\n'))
    {
      if (Line.startsWith("PPid:"))
      {
        Parents.insert(Pid, Line.mid(5).trimmed().toLongLong());
      }
    }
  }
  QList<qint64> Victims;
  for (auto It = Parents.cbegin(); It != Parents.cend(); ++It)
  {
    qint64 Ancestor = It.value();
    for (int Depth = 0; (Ancestor > 1) && (Depth < 10); Depth++)
    {
      if (Ancestor == ParentPid)
      {
        Victims.append(It.key());
        break;
      }
      Ancestor = Parents.value(Ancestor, 0);
    }
  }
  int Killed = 0;
  for (qint64 Pid : Victims)
  {
    if (kill(static_cast<pid_t>(Pid), SIGKILL) == 0)
    {
      Killed++;
    }
  }
  return Killed;
}

static void UnitTests()
{
  std::printf("[capabilities]\n");
  CHECK(Terminal::ProtocolCapability(fsSFTP, fcModeChanging));
  CHECK(Terminal::ProtocolCapability(fsSFTP, fcRename));
  CHECK(Terminal::ProtocolCapability(fsSFTP, fcResumeOverwrite));
  CHECK(!Terminal::ProtocolCapability(fsSFTP, fcMoveOverExistingFile));
  CHECK(Terminal::ProtocolCapability(fsSFTP, fcAppendUpload));
  CHECK(Terminal::ProtocolCapability(fsSCPonly, fcMoveOverExistingFile));
  CHECK(Terminal::ProtocolCapability(fsSCPonly, fcShellAnyCommand));
  CHECK(!Terminal::ProtocolCapability(fsSCPonly, fcResumeSupport));
  CHECK(Terminal::ProtocolCapability(fsFTP, fcModeChanging));
  CHECK(!Terminal::ProtocolCapability(fsFTP, fcModeChangingUpload));
  CHECK(Terminal::ProtocolCapability(fsFTP, fcNewerOnlyUpload));
  CHECK(!Terminal::ProtocolCapability(fsWebDAV, fcModeChanging));
  CHECK(!Terminal::ProtocolCapability(fsWebDAV, fcSymbolicLink));
  CHECK(!Terminal::ProtocolCapability(fsS3, fcModeChanging));
  CHECK(Terminal::ProtocolCapability(fsS3, fcRemoteMove));
  {
    SessionData Data;
    Data.FSProtocol = fsWebDAV;
    PlainUserInterface UI;
    Terminal T(Data, &UI);
    // before Open: the protocol's static answer
    CHECK(!T.IsCapable(fcModeChanging));
    CHECK(T.IsCapable(fcRename));
    CHECK(!T.IsCapable(static_cast<FSCapability>(fcCount)));
  }

  std::printf("[query fallback]\n");
  {
    PlainUserInterface UI;
    QueryUserParams Params;
    Params.Aliases.append(QueryButtonAlias{qaRetry, QStringLiteral("A&ppend"), qaNo, Qt::AltModifier});
    Params.Aliases.append(QueryButtonAlias::AllAsYesToNewerGroupedWithYes());
    Params.Aliases.append(QueryButtonAlias::IgnoreAsRenameGroupedWithNo());
    Params.Aliases.append(QueryButtonAlias::YesToAllGroupedWithYes());
    UserInterface & Base = UI;
    Base.QueryUserEx(QStringLiteral("q"), QString(), qaYes | qaNo | qaCancel | qaYesToAll | qaAll | qaIgnore | qaRetry, Params);
    CHECK(UI.LastAnswers == (qaYes | qaNo | qaCancel | qaYesToAll));
    // an alias that is not grouped keeps the answer (Reconnect = Retry)
    QueryUserParams Reconnect;
    Reconnect.Aliases.append(QueryButtonAlias{qaRetry, QStringLiteral("&Reconnect")});
    Base.QueryUserEx(QStringLiteral("q"), QString(), qaRetry | qaAbort, Reconnect);
    CHECK(UI.LastAnswers == (qaRetry | qaAbort));
    QueryUserParams SkipAll;
    SkipAll.Aliases.append(QueryButtonAlias{qaAll, QStringLiteral("Ski&p all")});
    Base.QueryUserEx(QStringLiteral("q"), QString(), qaRetry | qaSkip | qaAll | qaAbort, SkipAll);
    CHECK(UI.LastAnswers == (qaRetry | qaSkip | qaAbort));
  }

  std::printf("[progress skip]\n");
  {
    FileOperationProgress Progress;
    Progress.Start(FileOperationProgress::Copy, FileOperationProgress::ToRemote, 1);
    Progress.SetFile(QStringLiteral("a"), 1);
    Progress.CancelFile();
    bool Skipped = false;
    try
    {
      Progress.CheckCancel();
    }
    catch (const SkipFileException &)
    {
      Skipped = true;
    }
    CHECK(Skipped);
    // only once
    bool Again = false;
    try
    {
      Progress.CheckCancel();
    }
    catch (const AbortException &)
    {
      Again = true;
    }
    CHECK(!Again);
    CHECK(!Progress.SkipToAll());
    Progress.SetSkipToAll();
    CHECK(Progress.SkipToAll());
    Progress.Start(FileOperationProgress::Copy, FileOperationProgress::ToRemote, 1);
    CHECK(!Progress.SkipToAll());
  }
}

int main(int argc, char ** argv)
{
  QCoreApplication App(argc, argv);

  UnitTests();

  QByteArray PortStr = qgetenv("NIXSCP_TEST_SSH_PORT");
  QString KeyFile = qEnvironmentVariable("NIXSCP_TEST_SSH_KEY");
  if (PortStr.isEmpty() || KeyFile.isEmpty())
  {
    std::printf("test_operations: SFTP parts SKIPPED (set NIXSCP_TEST_SSH_PORT, NIXSCP_TEST_SSH_KEY)\n");
    std::printf(Failures ? "test_operations: %d FAILURES\n" : "test_operations: OK\n", Failures);
    return Failures ? 1 : 0;
  }

  QTemporaryDir Scratch;
  CHECK(Scratch.isValid());
  SessionData Data;
  Data.HostName = qEnvironmentVariable("NIXSCP_TEST_SSH_HOST", QStringLiteral("127.0.0.1"));
  Data.PortNumber = PortStr.toInt();
  Data.UserName = qEnvironmentVariable("NIXSCP_TEST_SSH_USER", qEnvironmentVariable("USER"));
  Data.PublicKeyFile = KeyFile;
  Data.TryAgent = false;
  Data.AuthKI = false;
  Data.KnownHostsFile = Scratch.filePath(QStringLiteral("known_hosts"));

  const QString Base = Scratch.filePath(QStringLiteral("remote"));
  const QString Local = Scratch.filePath(QStringLiteral("local"));
  QDir().mkpath(Base);
  QDir().mkpath(Local);

  TestUserInterface UI;
  CopyParam Param;
  Terminal T(Data, &UI);
  {
    Waiter W;
    T.Open(W.Cb());
    OperationResult R = W.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    if (!R.Ok)
    {
      return 1;
    }
    Waiter L;
    T.ReadDirectory(Base, L.ListCb());
    if (!L.Wait().Ok)
    {
      std::printf("test_operations: the server does not run on this machine, SFTP parts SKIPPED\n");
      return Failures ? 1 : 0;
    }
  }

  std::printf("[capabilities of the open session]\n");
  CHECK(T.IsCapable(fcRename));
  CHECK(T.IsCapable(fcModeChanging));
  CHECK(T.IsCapable(fcAnyCommand));
  CHECK(!T.IsCapable(fcMoveOverExistingFile));

  // ---------------------------------------------------------------- per-file errors
  const QString L1 = Local + QStringLiteral("/L1");
  QDir().mkpath(L1);
  WriteFile(L1 + QStringLiteral("/a.txt"), "aaa");
  WriteFile(L1 + QStringLiteral("/b.txt"), "bbb");
  WriteFile(L1 + QStringLiteral("/c.txt"), "ccc");
  chmod(QFile::encodeName(L1 + QStringLiteral("/b.txt")).constData(), 0);
  const QString L2 = Local + QStringLiteral("/L2");
  QDir().mkpath(L2);
  WriteFile(L2 + QStringLiteral("/x1.txt"), "x1");
  WriteFile(L2 + QStringLiteral("/x2.txt"), "x2");
  WriteFile(L2 + QStringLiteral("/x3.txt"), "x3");
  chmod(QFile::encodeName(L2 + QStringLiteral("/x1.txt")).constData(), 0);
  chmod(QFile::encodeName(L2 + QStringLiteral("/x2.txt")).constData(), 0);

  std::printf("[per-file error: Retry, then Skip]\n");
  {
    UI.Reset();
    UI.QueryAnswers = {qaRetry, qaSkip};
    QDir().mkpath(Base + QStringLiteral("/skip"));
    Waiter W;
    T.CopyToRemote({L1}, Base + QStringLiteral("/skip"), Param, false, W.Cb());
    OperationResult R = W.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(UI.QueryCount() == 2);
    QueryRecord Q = UI.Query(0);
    CHECK_MSG(Q.Query.contains(QStringLiteral("b.txt")), Q.Query);
    CHECK(Q.Answers == (qaRetry | qaSkip | qaAll | qaAbort));
    CHECK(Q.Params.Type == qtError);
    CHECK(Q.Params.Params & qpAllowContinueOnError);
    CHECK((Q.Params.Aliases.size() == 1) && (Q.Params.Aliases[0].Button == qaAll) && (Q.Params.Aliases[0].Alias == QStringLiteral("Ski&p all")));
    CHECK(Entries(Base + QStringLiteral("/skip/L1")) == QStringList({QStringLiteral("a.txt"), QStringLiteral("c.txt")}));
  }

  std::printf("[per-file error: Skip all]\n");
  {
    UI.Reset();
    UI.QueryAnswers = {qaAll};
    QDir().mkpath(Base + QStringLiteral("/skipall"));
    Waiter W;
    T.CopyToRemote({L2}, Base + QStringLiteral("/skipall"), Param, false, W.Cb());
    OperationResult R = W.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(UI.QueryCount() == 1);
    CHECK(Entries(Base + QStringLiteral("/skipall/L2")) == QStringList({QStringLiteral("x3.txt")}));
  }

  std::printf("[per-file error: Abort]\n");
  {
    UI.Reset();
    UI.QueryAnswers = {qaAbort};
    QDir().mkpath(Base + QStringLiteral("/abort"));
    Waiter W;
    T.CopyToRemote({L2}, Base + QStringLiteral("/abort"), Param, false, W.Cb());
    OperationResult R = W.Wait();
    CHECK_MSG(!R.Ok && R.Aborted, Describe(R));
    // the error stays in the result (it was shown as the query)
    CHECK_MSG(R.Message.contains(QStringLiteral("x1.txt")), R.Message);
    CHECK(UI.QueryCount() == 1);
    CHECK(!QFile::exists(Base + QStringLiteral("/abort/L2/x3.txt")));
  }

  std::printf("[continue on error]\n");
  {
    UI.Reset();
    Terminal::Settings Settings;
    Settings.ContinueOnError = true;
    T.SetSettings(Settings);
    QDir().mkpath(Base + QStringLiteral("/coe"));
    Waiter W;
    T.CopyToRemote({L2}, Base + QStringLiteral("/coe"), Param, false, W.Cb());
    OperationResult R = W.Wait();
    T.SetSettings(Terminal::Settings());
    CHECK_MSG(!R.Ok && !R.Aborted && !R.Fatal, Describe(R));
    CHECK_MSG(R.Message == QStringLiteral("2 error(s) occurred during last operation."), R.Message);
    CHECK(R.MoreMessages.contains(QStringLiteral("x1.txt")) && R.MoreMessages.contains(QStringLiteral("x2.txt")));
    CHECK(UI.QueryCount() == 0);
    CHECK(Entries(Base + QStringLiteral("/coe/L2")) == QStringList({QStringLiteral("x3.txt")}));
  }

  std::printf("[move deletes only transferred sources]\n");
  {
    UI.Reset();
    UI.QueryAnswers = {qaSkip};
    QDir().mkpath(Base + QStringLiteral("/move"));
    Waiter W;
    T.CopyToRemote({L1}, Base + QStringLiteral("/move"), Param, true, W.Cb());
    OperationResult R = W.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(Entries(L1) == QStringList({QStringLiteral("b.txt")}));
    CHECK(Entries(Base + QStringLiteral("/move/L1")) == QStringList({QStringLiteral("a.txt"), QStringLiteral("c.txt")}));
  }
  chmod(QFile::encodeName(L1 + QStringLiteral("/b.txt")).constData(), 0644);
  chmod(QFile::encodeName(L2 + QStringLiteral("/x1.txt")).constData(), 0644);
  chmod(QFile::encodeName(L2 + QStringLiteral("/x2.txt")).constData(), 0644);

  // ---------------------------------------------------------------- overwrite
  const QString R1 = Base + QStringLiteral("/ow");
  const QString D1 = Local + QStringLiteral("/ow");
  QDir().mkpath(R1);
  QDir().mkpath(D1);
  const QByteArray RemoteData = RandomData(4000);
  qint64 Now = QDateTime::currentSecsSinceEpoch();
  WriteFile(R1 + QStringLiteral("/a.txt"), RemoteData, Now - 100);
  const QString RA = R1 + QStringLiteral("/a.txt");
  const QString DA = D1 + QStringLiteral("/a.txt");

  std::printf("[overwrite: No]\n");
  {
    WriteFile(DA, "local", Now - 50);
    UI.Reset();
    UI.QueryAnswers = {qaNo};
    Waiter W;
    T.CopyToLocal({RA}, D1, Param, false, W.Cb());
    OperationResult R = W.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(ReadFile(DA) == "local");
    CHECK(UI.QueryCount() == 1);
    QueryRecord Q = UI.Query(0);
    CHECK_MSG(Q.Query.startsWith(QStringLiteral("**Overwrite local file 'a.txt'?**\n\nDestination directory already contains file 'a.txt'.")), Q.Query);
    CHECK_MSG(Q.Query.contains(QStringLiteral("New:      \t")) && Q.Query.contains(QStringLiteral("Existing: \t")), Q.Query);
    CHECK((Q.Answers & (qaYes | qaNo | qaCancel | qaYesToAll | qaNoToAll | qaAll | qaIgnore | qaRetry)) ==
      (qaYes | qaNo | qaCancel | qaYesToAll | qaNoToAll | qaAll | qaIgnore | qaRetry));
    CHECK(Q.Params.Params & qpNeverAskAgainCheck);
    bool NewerOnly = false, NewName = false, Append = false;
    for (const QueryButtonAlias & A : Q.Params.Aliases)
    {
      NewerOnly |= (A.Button == qaAll) && (A.Alias == QStringLiteral("Ne&wer only")) && (A.GroupWith == qaYes);
      NewName |= (A.Button == qaIgnore) && (A.Alias == QStringLiteral("New na&me")) && (A.GroupWith == qaNo);
      Append |= (A.Button == qaRetry) && (A.Alias == QStringLiteral("A&ppend"));
    }
    CHECK(NewerOnly && NewName && Append);
  }

  std::printf("[overwrite: New name]\n");
  {
    UI.Reset();
    UI.QueryAnswers = {qaIgnore};
    UI.PromptAnswers = {QStringLiteral("renamed.txt")};
    Waiter W;
    T.CopyToLocal({RA}, D1, Param, false, W.Cb());
    OperationResult R = W.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(ReadFile(DA) == "local");
    CHECK(ReadFile(D1 + QStringLiteral("/renamed.txt")) == RemoteData);
    CHECK((UI.PromptKinds.size() == 1) && (UI.PromptKinds[0] == UserInterface::pkFileName));
  }

  std::printf("[overwrite: Append, target smaller -> Append]\n");
  {
    WriteFile(DA, "local", Now - 50);
    UI.Reset();
    UI.QueryAnswers = {qaRetry, qaYes};
    Waiter W;
    T.CopyToLocal({RA}, D1, Param, false, W.Cb());
    OperationResult R = W.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(ReadFile(DA) == QByteArray("local") + RemoteData);
    CHECK(UI.QueryCount() == 2);
    CHECK_MSG(UI.Query(1).Query.startsWith(QStringLiteral("**Do you want to append file '")), UI.Query(1).Query);
    CHECK(UI.Query(1).Answers == (qaYes | qaNo | qaNoToAll | qaCancel));
    CHECK(!QFile::exists(DA + QStringLiteral(".append.filepart")));
  }

  std::printf("[overwrite: Append, target smaller -> Resume]\n");
  {
    WriteFile(DA, RemoteData.left(1500), Now - 50);
    UI.Reset();
    UI.QueryAnswers = {qaRetry, qaNo};
    Waiter W;
    T.CopyToLocal({RA}, D1, Param, false, W.Cb());
    OperationResult R = W.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(ReadFile(DA) == RemoteData);
    CHECK(UI.QueryCount() == 2);
  }

  std::printf("[overwrite: Append, target bigger]\n");
  {
    QByteArray Big = RandomData(6000);
    WriteFile(DA, Big, Now - 50);
    UI.Reset();
    UI.QueryAnswers = {qaRetry};
    Waiter W;
    T.CopyToLocal({RA}, D1, Param, false, W.Cb());
    OperationResult R = W.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(ReadFile(DA) == Big + RemoteData);
    CHECK(UI.QueryCount() == 1);
  }

  std::printf("[overwrite: Newer only, Yes to All]\n");
  {
    WriteFile(R1 + QStringLiteral("/n1.txt"), "remote-new", Now - 10);
    WriteFile(R1 + QStringLiteral("/n2.txt"), "remote-old", Now - 1000);
    WriteFile(D1 + QStringLiteral("/n1.txt"), "local-old", Now - 1000);
    WriteFile(D1 + QStringLiteral("/n2.txt"), "local-new", Now - 10);
    UI.Reset();
    UI.QueryAnswers = {qaAll};
    Waiter W;
    T.CopyToLocal({R1 + QStringLiteral("/n1.txt"), R1 + QStringLiteral("/n2.txt")}, D1, Param, false, W.Cb());
    OperationResult R = W.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(UI.QueryCount() == 1);
    CHECK(ReadFile(D1 + QStringLiteral("/n1.txt")) == "remote-new");
    CHECK(ReadFile(D1 + QStringLiteral("/n2.txt")) == "local-new");

    UI.Reset();
    UI.QueryAnswers = {qaYesToAll};
    Waiter W2;
    T.CopyToLocal({R1 + QStringLiteral("/n1.txt"), R1 + QStringLiteral("/n2.txt")}, D1, Param, false, W2.Cb());
    R = W2.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(UI.QueryCount() == 1);
    CHECK(ReadFile(D1 + QStringLiteral("/n2.txt")) == "remote-old");
  }

  std::printf("[overwrite: upload, New name]\n");
  {
    WriteFile(Local + QStringLiteral("/u.txt"), "upload");
    WriteFile(R1 + QStringLiteral("/u.txt"), "existing");
    UI.Reset();
    UI.QueryAnswers = {qaIgnore};
    UI.PromptAnswers = {QStringLiteral("u2.txt")};
    Waiter W;
    T.CopyToRemote({Local + QStringLiteral("/u.txt")}, R1, Param, false, W.Cb());
    OperationResult R = W.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(ReadFile(R1 + QStringLiteral("/u.txt")) == "existing");
    CHECK(ReadFile(R1 + QStringLiteral("/u2.txt")) == "upload");
    CHECK_MSG(UI.Query(0).Query.startsWith(QStringLiteral("**Overwrite remote file 'u.txt'?**")), UI.Query(0).Query);
  }

  std::printf("[missing local target directory]\n");
  {
    UI.Reset();
    UI.QueryAnswers = {qaCancel};
    QString Missing = Local + QStringLiteral("/missing/sub");
    Waiter W;
    T.CopyToLocal({RA}, Missing, Param, false, W.Cb());
    OperationResult R = W.Wait();
    CHECK_MSG(R.Aborted, Describe(R));
    CHECK(!QFileInfo::exists(Local + QStringLiteral("/missing")));
    CHECK_MSG(UI.Query(0).Query == QStringLiteral("Directory '%1' doesn't exist. Create?").arg(Missing), UI.Query(0).Query);
    CHECK(UI.Query(0).Answers == (qaOK | qaCancel));

    UI.Reset();
    UI.QueryAnswers = {qaOK};
    Waiter W2;
    T.CopyToLocal({RA}, Missing, Param, false, W2.Cb());
    R = W2.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(ReadFile(Missing + QStringLiteral("/a.txt")) == RemoteData);
  }

  // ---------------------------------------------------------------- rename / move
  std::printf("[rename onto existing file]\n");
  {
    WriteFile(Base + QStringLiteral("/r1.txt"), "one");
    WriteFile(Base + QStringLiteral("/r2.txt"), "two");
    UI.Reset();
    UI.QueryAnswers = {qaNo};
    Waiter W;
    T.RenameFile(Base + QStringLiteral("/r1.txt"), Base + QStringLiteral("/r2.txt"), W.Cb());
    OperationResult R = W.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(UI.QueryCount() == 1);
    CHECK_MSG(UI.Query(0).Query == QStringLiteral("**File 'r2.txt' already exists. Overwrite?**"), UI.Query(0).Query);
    CHECK(UI.Query(0).Answers == (qaYes | qaNo));
    CHECK(ReadFile(Base + QStringLiteral("/r1.txt")) == "one");
    CHECK(ReadFile(Base + QStringLiteral("/r2.txt")) == "two");

    UI.Reset();
    UI.QueryAnswers = {qaYes};
    Waiter W2;
    T.RenameFile(Base + QStringLiteral("/r1.txt"), Base + QStringLiteral("/r2.txt"), W2.Cb());
    R = W2.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(!QFile::exists(Base + QStringLiteral("/r1.txt")));
    CHECK(ReadFile(Base + QStringLiteral("/r2.txt")) == "one");

    // missing source: no overwrite question, the error of the rename (Retry / Skip / Abort)
    WriteFile(Base + QStringLiteral("/r4.txt"), "four");
    UI.Reset();
    UI.QueryAnswers = {qaAbort};
    Waiter W4;
    T.RenameFile(Base + QStringLiteral("/missing.txt"), Base + QStringLiteral("/r4.txt"), W4.Cb());
    R = W4.Wait();
    CHECK_MSG(!R.Ok && R.Aborted && !R.Message.isEmpty(), Describe(R));
    CHECK(UI.QueryCount() == 1);
    CHECK(UI.Query(0).Answers == (qaRetry | qaSkip | qaAbort));
    CHECK(ReadFile(Base + QStringLiteral("/r4.txt")) == "four");

    // no target: no question
    UI.Reset();
    Waiter W3;
    T.RenameFile(Base + QStringLiteral("/r2.txt"), Base + QStringLiteral("/r3.txt"), W3.Cb());
    R = W3.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(UI.QueryCount() == 0);
    CHECK(ReadFile(Base + QStringLiteral("/r3.txt")) == "one");
  }

  std::printf("[move onto existing directory]\n");
  {
    QDir().mkpath(Base + QStringLiteral("/d1"));
    QDir().mkpath(Base + QStringLiteral("/d2"));
    WriteFile(Base + QStringLiteral("/d1/in1.txt"), "1");
    WriteFile(Base + QStringLiteral("/d2/in2.txt"), "2");
    UI.Reset();
    UI.QueryAnswers = {qaNo};
    Waiter W;
    T.RenameFile(Base + QStringLiteral("/d1"), Base + QStringLiteral("/d2"), W.Cb());
    OperationResult R = W.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    CHECK_MSG(UI.Query(0).Query.contains(QStringLiteral("All files in the target directory will be deleted!")), UI.Query(0).Query);
    CHECK(UI.Query(0).Params.Type == qtWarning);
    CHECK(QFile::exists(Base + QStringLiteral("/d2/in2.txt")));

    UI.Reset();
    UI.QueryAnswers = {qaYes};
    Waiter W2;
    T.RenameFile(Base + QStringLiteral("/d1"), Base + QStringLiteral("/d2"), W2.Cb());
    R = W2.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(Entries(Base + QStringLiteral("/d2")) == QStringList({QStringLiteral("in1.txt")}));
    CHECK(!QFileInfo::exists(Base + QStringLiteral("/d1")));
  }

  // ---------------------------------------------------------------- Add X to directories
  std::printf("[add X to directories]\n");
  {
    QString Dir = Base + QStringLiteral("/ax");
    QDir().mkpath(Dir);
    WriteFile(Dir + QStringLiteral("/f.txt"), "f");
    UI.Reset();
    Waiter W;
    T.ChangeFileProperties({Dir}, 0644, true, W.Cb(), false);
    OperationResult R = W.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    CHECK_MSG(LocalMode(Dir) == 0644, QString::number(LocalMode(Dir), 8));
    chmod(QFile::encodeName(Dir).constData(), 0700);
    CHECK(LocalMode(Dir + QStringLiteral("/f.txt")) == 0644);
    Waiter W2;
    T.ChangeFileProperties({Dir}, 0644, true, W2.Cb(), true);
    R = W2.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    CHECK_MSG(LocalMode(Dir) == 0755, QString::number(LocalMode(Dir), 8));
    CHECK(LocalMode(Dir + QStringLiteral("/f.txt")) == 0644);
  }

  // ---------------------------------------------------------------- recycle bin
  std::printf("[remote recycle bin]\n");
  {
    QString Bin = Base + QStringLiteral("/bin");
    QDir().mkpath(Bin);
    SessionData RecycleData = Data;
    RecycleData.DeleteToRecycleBin = true;
    RecycleData.OverwrittenToRecycleBin = true;
    RecycleData.RecycleBinPath = Bin;
    Terminal T2(RecycleData, &UI);
    Waiter WO;
    T2.Open(WO.Cb());
    CHECK_MSG(WO.Wait().Ok, Describe(WO.Result));

    WriteFile(Base + QStringLiteral("/del.txt"), "deleted");
    UI.Reset();
    Waiter W;
    T2.DeleteFiles({Base + QStringLiteral("/del.txt")}, W.Cb());
    OperationResult R = W.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(!QFile::exists(Base + QStringLiteral("/del.txt")));
    QStringList InBin = Entries(Bin);
    CHECK_MSG((InBin.size() == 1) && QRegularExpression(QStringLiteral("^del-\\d{8}-\\d{6}\\.txt$")).match(InBin.value(0)).hasMatch(),
      InBin.join(QLatin1Char(',')));
    CHECK(ReadFile(Bin + QLatin1Char('/') + InBin.value(0)) == "deleted");

    // deleting in the recycle bin is permanent
    Waiter W2;
    T2.DeleteFiles({Bin + QLatin1Char('/') + InBin.value(0)}, W2.Cb());
    CHECK_MSG(W2.Wait().Ok, Describe(W2.Result));
    CHECK(Entries(Bin).isEmpty());

    // alternative delete (Shift) is permanent
    WriteFile(Base + QStringLiteral("/perm.txt"), "p");
    Waiter W3;
    T2.DeleteFiles({Base + QStringLiteral("/perm.txt")}, W3.Cb(), dfAlternative);
    CHECK_MSG(W3.Wait().Ok, Describe(W3.Result));
    CHECK(!QFile::exists(Base + QStringLiteral("/perm.txt")));
    CHECK(Entries(Bin).isEmpty());

    // directories go as a whole
    QDir().mkpath(Base + QStringLiteral("/deldir/sub"));
    WriteFile(Base + QStringLiteral("/deldir/sub/x"), "x");
    Waiter W4;
    T2.DeleteFiles({Base + QStringLiteral("/deldir")}, W4.Cb());
    CHECK_MSG(W4.Wait().Ok, Describe(W4.Result));
    InBin = Entries(Bin);
    CHECK_MSG((InBin.size() == 1) && InBin.value(0).startsWith(QStringLiteral("deldir-")), InBin.join(QLatin1Char(',')));
    CHECK(QFile::exists(Bin + QLatin1Char('/') + InBin.value(0) + QStringLiteral("/sub/x")));
    QDir(Bin + QLatin1Char('/') + InBin.value(0)).removeRecursively();

    // overwritten files are preserved too (SFTP)
    WriteFile(Base + QStringLiteral("/ow.txt"), "old");
    WriteFile(Local + QStringLiteral("/ow.txt"), "new");
    CopyParam NoConfirm = Param;
    NoConfirm.ConfirmOverwriting = false;
    Waiter W5;
    T2.CopyToRemote({Local + QStringLiteral("/ow.txt")}, Base, NoConfirm, false, W5.Cb());
    CHECK_MSG(W5.Wait().Ok, Describe(W5.Result));
    CHECK(ReadFile(Base + QStringLiteral("/ow.txt")) == "new");
    InBin = Entries(Bin);
    CHECK_MSG((InBin.size() == 1) && InBin.value(0).startsWith(QStringLiteral("ow-")), InBin.join(QLatin1Char(',')));
    CHECK(ReadFile(Bin + QLatin1Char('/') + InBin.value(0)) == "old");
  }

  // ---------------------------------------------------------------- reopen
  std::printf("[reopen in the last directory]\n");
  {
    QDir().mkpath(Base + QStringLiteral("/last"));
    Waiter L;
    T.ReadDirectory(Base + QStringLiteral("/last"), L.ListCb());
    CHECK(L.Wait().Ok);
    T.Close();
    CHECK(!T.Active());
    Waiter W;
    T.Reopen(W.Cb());
    OperationResult R = W.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(T.Active());
    CHECK_MSG(T.CurrentDirectory() == Base + QStringLiteral("/last"), T.CurrentDirectory());
  }

  std::printf("[reopen when the last directory is gone]\n");
  {
    // not a reason to lose the session, whatever the rule of Open is
    SessionData Strict[2] = {Data, Data};
    Strict[0].RequireDirectories = true;
    Strict[1].UpdateDirectories = false;
    for (const SessionData & D : Strict)
    {
      QString Gone = Base + QStringLiteral("/gone");
      QDir().mkpath(Gone);
      Terminal TG(D, &UI);
      Waiter O;
      TG.Open(O.Cb());
      CHECK_MSG(O.Wait().Ok, Describe(O.Result));
      QString HomeDir = TG.CurrentDirectory();
      Waiter L;
      TG.ReadDirectory(Gone, L.ListCb());
      CHECK(L.Wait().Ok);
      TG.Close();
      CHECK(QDir(Gone).removeRecursively());
      Waiter W;
      TG.Reopen(W.Cb());
      OperationResult R = W.Wait();
      CHECK_MSG(R.Ok, Describe(R));
      CHECK(TG.Active());
      CHECK_MSG(TG.CurrentDirectory() == HomeDir, TG.CurrentDirectory());
      TG.Close();
      // opening it is still the error of the site or the URL
      SessionData Opening = D;
      Opening.RemoteDirectory = Gone;
      Terminal TO(Opening, &UI);
      Waiter F;
      TO.Open(F.Cb());
      CHECK_MSG(!F.Wait().Ok, Gone);
      CHECK(F.Result.Message.contains(QStringLiteral("Error changing directory")));
    }
  }

  // ---------------------------------------------------------------- automatic reconnect
  QByteArray KillPortStr = qgetenv("NIXSCP_TEST_SSH_KILL_PORT");
  qint64 KillPid = qEnvironmentVariable("NIXSCP_TEST_SSH_KILL_PID").toLongLong();
  if (KillPortStr.isEmpty() || (KillPid <= 0))
  {
    std::printf("[automatic reconnect] SKIPPED (set NIXSCP_TEST_SSH_KILL_PORT and NIXSCP_TEST_SSH_KILL_PID)\n");
  }
  else
  {
    std::printf("[automatic reconnect during upload]\n");
    SessionData KillData = Data;
    KillData.PortNumber = KillPortStr.toInt();
    QString PassKey = qEnvironmentVariable("NIXSCP_TEST_SSH_PASSKEY");
    QString Passphrase = qEnvironmentVariable("NIXSCP_TEST_SSH_PASSPHRASE");
    bool UsePassphrase = !PassKey.isEmpty() && !Passphrase.isEmpty();
    if (UsePassphrase)
    {
      KillData.PublicKeyFile = PassKey;
    }
    UI.Reset();
    if (UsePassphrase)
    {
      // only once: the reconnect must reuse it
      UI.PromptAnswers = {Passphrase};
    }
    Terminal TK(KillData, &UI);
    Terminal::Settings Settings;
    Settings.ReopenAuto = 1500;
    TK.SetSettings(Settings);
    int ReconnectedSignals = 0;
    QObject::connect(&TK, &Terminal::Reconnected, [&ReconnectedSignals]() { ReconnectedSignals++; });
    Waiter WO;
    TK.Open(WO.Cb());
    CHECK_MSG(WO.Wait().Ok, Describe(WO.Result));

    QByteArray Big = RandomData(3 * 1024 * 1024);
    WriteFile(Local + QStringLiteral("/big.bin"), Big);
    QDir().mkpath(Base + QStringLiteral("/rc"));
    CopyParam Slow = Param;
    Slow.CPSLimit = 768 * 1024;
    Slow.ResumeSupport = CopyParam::rsOn;
    UI.QueryAnswers = {qaRetry};
    Waiter W;
    TK.CopyToRemote({Local + QStringLiteral("/big.bin")}, Base + QStringLiteral("/rc"), Slow, false, W.Cb());
    QTimer::singleShot(1500, [KillPid]() { Log(QStringLiteral("killed %1 connection process(es)").arg(KillConnections(KillPid))); });
    OperationResult R = W.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(UI.QueryCount() == 1);
    QueryRecord Q = UI.Query(0);
    CHECK(Q.Extended);
    CHECK(Q.Answers == (qaRetry | qaAbort));
    CHECK(Q.Params.Timeout == 1500);
    CHECK(Q.Params.TimeoutAnswer == qaRetry);
    CHECK((Q.Params.Aliases.size() == 1) && (Q.Params.Aliases[0].Alias == QStringLiteral("&Reconnect")));
    CHECK(ReadFile(Base + QStringLiteral("/rc/big.bin")) == Big);
    CHECK(!QFile::exists(Base + QStringLiteral("/rc/big.bin.filepart")));
    QCoreApplication::processEvents();
    CHECK(ReconnectedSignals == 1);
    CHECK(TK.Active());
    if (UsePassphrase)
    {
      CHECK_MSG(UI.PromptKinds.size() == 1, QString::number(UI.PromptKinds.size()));
    }

    std::printf("[automatic reconnect of a listing]\n");
    UI.QueryAnswers = {qaRetry};
    int Queries = UI.QueryCount();
    Log(QStringLiteral("killed %1 connection process(es)").arg(KillConnections(KillPid)));
    QThread::msleep(300);
    Waiter L;
    TK.ReadDirectory(Base + QStringLiteral("/rc"), L.ListCb());
    CHECK_MSG(L.Wait().Ok, Describe(L.Result));
    CHECK(UI.QueryCount() == Queries + 1);
    CHECK(L.Files.size() >= 1);

    std::printf("[reconnect declined]\n");
    UI.QueryAnswers = {qaAbort};
    Log(QStringLiteral("killed %1 connection process(es)").arg(KillConnections(KillPid)));
    QThread::msleep(300);
    Waiter L2;
    TK.ReadDirectory(Base + QStringLiteral("/rc"), L2.ListCb());
    CHECK_MSG(!L2.Wait().Ok && L2.Result.Fatal, Describe(L2.Result));
    CHECK(!TK.Active());
  }

  std::printf(Failures ? "test_operations: %d FAILURES\n" : "test_operations: OK\n", Failures);
  return Failures ? 1 : 0;
}
