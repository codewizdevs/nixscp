// End-to-end test of core/Terminal + core/SftpFileSystem against a real SSH server.
//
// Skipped (exit 0) unless NIXSCP_TEST_SSH_PORT is set. Environment:
//   NIXSCP_TEST_SSH_PORT        server port (required to run)
//   NIXSCP_TEST_SSH_HOST        default 127.0.0.1
//   NIXSCP_TEST_SSH_USER        default $USER
//   NIXSCP_TEST_SSH_KEY         private key accepted by the server (required)
//   NIXSCP_TEST_SSH_PASSKEY     optional passphrase protected key accepted by the server
//   NIXSCP_TEST_SSH_PASSPHRASE  its passphrase
// The user's ~/.ssh/known_hosts is never touched: a temporary known_hosts
// file is passed via SessionData::KnownHostsFile.
// When the server runs on the same machine (detected automatically), remote
// results are additionally verified with stat() on the local filesystem.
#include "core/Terminal.h"

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

#include <libssh/libssh.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>

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
  // Configuration (set from main thread before an operation)
  HostKeyAnswer HostKeyResponse = HostKeyAccept;
  QStringList PromptAnswers;
  QList<unsigned int> QueryAnswers;

  // Records
  QMutex Mutex;
  int HostKeyCalls = 0;
  HostKeyState LastHostKeyState = HostKeyUnknown;
  QString LastFingerprint;
  QList<PromptKind> PromptKinds;
  QStringList Queries;
  QStringList Banners;

  void Reset()
  {
    QMutexLocker Lock(&Mutex);
    HostKeyResponse = HostKeyAccept;
    PromptAnswers.clear();
    QueryAnswers.clear();
    HostKeyCalls = 0;
    PromptKinds.clear();
    Queries.clear();
    Banners.clear();
  }

  HostKeyAnswer VerifyHostKey(const QString & Host, int Port, const QString & KeyType,
    const QString & FingerprintSHA256, const QString & FingerprintMD5, HostKeyState State) override
  {
    QMutexLocker Lock(&Mutex);
    HostKeyCalls++;
    LastHostKeyState = State;
    LastFingerprint = FingerprintSHA256;
    Log(QStringLiteral("VerifyHostKey %1:%2 %3 %4 %5 state=%6")
      .arg(Host).arg(Port).arg(KeyType, FingerprintSHA256, FingerprintMD5).arg(State));
    return HostKeyResponse;
  }

  bool PromptUser(PromptKind Kind, const QString & Name, const QString &, QList<Prompt> & Prompts) override
  {
    QMutexLocker Lock(&Mutex);
    PromptKinds.append(Kind);
    Log(QStringLiteral("PromptUser kind=%1 name=%2 prompts=%3").arg(Kind).arg(Name).arg(Prompts.size()));
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

  void DisplayBanner(const QString &, const QString & Banner) override
  {
    QMutexLocker Lock(&Mutex);
    Banners.append(Banner);
  }

  unsigned int QueryUser(const QString & Query, const QString &, unsigned int Answers) override
  {
    QMutexLocker Lock(&Mutex);
    Queries.append(Query);
    unsigned int Answer = QueryAnswers.isEmpty() ? static_cast<unsigned int>(qaCancel) : QueryAnswers.takeFirst();
    Log(QStringLiteral("QueryUser \"%1\" -> %2").arg(Query).arg(Answer));
    if ((Answer & Answers) == 0)
    {
      Log(QStringLiteral("  answer not among offered answers %1").arg(Answers));
    }
    return Answer;
  }
};

// Runs an async Terminal call and spins the event loop until its callback
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
      CHECK(QThread::currentThread() == QCoreApplication::instance()->thread());
      Result = R;
      Called = true;
      Loop.quit();
    };
  }
  Terminal::ListCallback ListCb()
  {
    return [this](const OperationResult & R, const QString & Dir, const RemoteFileList & List)
    {
      CHECK(QThread::currentThread() == QCoreApplication::instance()->thread());
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

static QByteArray ReadFile(const QString & FileName)
{
  QFile F(FileName);
  return F.open(QIODevice::ReadOnly) ? F.readAll() : QByteArray();
}

static QByteArray RandomData(qint64 Size)
{
  QByteArray Data(Size, Qt::Uninitialized);
  QRandomGenerator Gen(12345);
  Gen.fillRange(reinterpret_cast<quint32 *>(Data.data()), Size / 4);
  return Data;
}

// Relative path -> file info for regular files and dirs below Root
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

static bool GenerateKey(const QString & FileName, QString * PublicBase64 = nullptr)
{
  ssh_key Key = nullptr;
  if (ssh_pki_generate(SSH_KEYTYPE_ED25519, 0, &Key) != SSH_OK)
  {
    return false;
  }
  bool Ok = (ssh_pki_export_privkey_file(Key, nullptr, nullptr, nullptr, QFile::encodeName(FileName).constData()) == SSH_OK);
  if (Ok && (PublicBase64 != nullptr))
  {
    char * B64 = nullptr;
    Ok = (ssh_pki_export_pubkey_base64(Key, &B64) == SSH_OK);
    *PublicBase64 = QString::fromLatin1(B64);
    ssh_string_free_char(B64);
  }
  ssh_key_free(Key);
  return Ok;
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

// B8, L13, B9: new files and folders, .filepart that cannot be created,
// a file that cannot be overwritten; L: SFTPRealPath, TrimVMSVersions
static void TestUploadTargets(SessionData Data, TestUserInterface & UI, CopyParam Param, const QString & Scratch)
{
  QString Dir = QDir(Scratch).filePath(QStringLiteral("uptargets"));
  QDir().mkpath(Dir);
  QString Local = QDir(Scratch).filePath(QStringLiteral("uplocal"));
  QDir().mkpath(Local);
  // above the 100 KB of the smart resume: goes through .filepart
  QString Big = Local + QStringLiteral("/big.bin");
  WriteFile(Big, RandomData(300000), 1600000000);

  auto Upload = [&](Terminal & T, const QStringList & Files, const QString & Target, const CopyParam & P)
  {
    Waiter W;
    T.CopyToRemote(Files, Target, P, false, W.Cb());
    return W.Wait();
  };

  std::printf("[upload: .filepart cannot be created]\n");
  {
    UI.Reset();
    Terminal T(Data, &UI);
    OperationResult R = DoOpen(T);
    CHECK_MSG(R.Ok, Describe(R));
    // a folder where the existing file can be written, but no file created
    QString NoCreate = Dir + QStringLiteral("/nocreate");
    QDir().mkpath(NoCreate);
    WriteFile(NoCreate + QStringLiteral("/big.bin"), "old", 1500000000);
    QFile::setPermissions(NoCreate, QFile::ReadOwner | QFile::ExeOwner | QFile::ReadUser | QFile::ExeUser);
    CopyParam Overwrite = Param;
    Overwrite.ConfirmOverwriting = false;
    // writing over the target is no longer safe from a failed upload, so
    // nobody asked (script in batch mode, background queue) means no
    QString Target = NoCreate + QStringLiteral("/big.bin");
    for (unsigned int Answer : {qaSkip, qaAbort})
    {
      UI.Reset();
      UI.QueryAnswers = {Answer};
      R = Upload(T, {Big}, NoCreate, Overwrite);
      CHECK_MSG(!UI.Queries.isEmpty() && UI.Queries.first().contains(QStringLiteral("big.bin.filepart")), UI.Queries.join(QLatin1Char('|')));
      CHECK(ReadFile(Target) == "old");
    }
    UI.Reset();
    UI.QueryAnswers = {qaYes};
    R = Upload(T, {Big}, NoCreate, Overwrite);
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(UI.Queries.size() == 1);
    CHECK(Sha256(Target) == Sha256(Big));
    CHECK(!QFile::exists(NoCreate + QStringLiteral("/big.bin.filepart")));
    // the upload fails after the answer: what there is stays, even if it is not much
    {
      QString Huge = Local + QStringLiteral("/huge/big.bin");
      QDir().mkpath(Local + QStringLiteral("/huge"));
      WriteFile(Huge, RandomData(32 * 1024 * 1024), 1600000000);
      UI.Reset();
      UI.QueryAnswers = {qaYes};
      auto Timer = new QTimer();
      Timer->setInterval(0);
      QObject::connect(Timer, &QTimer::timeout, Timer, [&T, Timer]()
      {
        if (T.Progress().Get().FileTransferred > 0)
        {
          T.Progress().Cancel();
          Timer->stop();
        }
      });
      Timer->start();
      CopyParam Slow = Overwrite;
      Slow.CPSLimit = 2 * 1024 * 1024;
      R = Upload(T, {Huge}, NoCreate, Slow);
      delete Timer;
      CHECK_MSG(!R.Ok, Describe(R));
      CHECK(QFile::exists(Target));
      QFile::remove(Huge);
    }
    QFile::setPermissions(NoCreate, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ReadUser | QFile::WriteUser | QFile::ExeUser);
    UI.Reset();
    WriteFile(NoCreate + QStringLiteral("/big.bin"), "old", 1500000000);
    // with resume forced there is no fallback (rsOn, as WinSCP)
    QFile::setPermissions(NoCreate, QFile::ReadOwner | QFile::ExeOwner | QFile::ReadUser | QFile::ExeUser);
    CopyParam Forced = Overwrite;
    Forced.ResumeSupport = CopyParam::rsOn;
    R = Upload(T, {Big}, NoCreate, Forced);
    QFile::setPermissions(NoCreate, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ReadUser | QFile::WriteUser | QFile::ExeUser);
    CHECK_MSG(!R.Ok && !R.Fatal, Describe(R));
    // no such target either: the error of the file itself
    QString NoFile = Dir + QStringLiteral("/nofile");
    QDir().mkpath(NoFile);
    QFile::setPermissions(NoFile, QFile::ReadOwner | QFile::ExeOwner | QFile::ReadUser | QFile::ExeUser);
    R = Upload(T, {Big}, NoFile, Param);
    QFile::setPermissions(NoFile, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ReadUser | QFile::WriteUser | QFile::ExeUser);
    CHECK_MSG(!R.Ok && !R.Fatal, Describe(R));
  }

  std::printf("[upload: target cannot be overwritten]\n");
  {
    UI.Reset();
    Terminal T(Data, &UI);
    OperationResult R = DoOpen(T);
    CHECK_MSG(R.Ok, Describe(R));
    QString Small = Local + QStringLiteral("/small.txt");
    WriteFile(Small, "new contents", 1600000000);
    QString Target = Dir + QStringLiteral("/small.txt");
    WriteFile(Target, "old", 1500000000);
    QFile::setPermissions(Target, QFile::ReadOwner | QFile::ReadUser);
    CopyParam NoConfirm = Param;
    NoConfirm.ConfirmOverwriting = false;
    // Abort keeps it
    UI.QueryAnswers = {qaAbort};
    R = Upload(T, {Small}, Dir, NoConfirm);
    CHECK_MSG(!R.Ok, Describe(R));
    CHECK_MSG((UI.Queries.size() == 1) && UI.Queries.value(0).contains(QStringLiteral("Cannot overwrite remote file")) &&
      UI.Queries.value(0).contains(QStringLiteral("Press 'Delete'")), UI.Queries.join(QLatin1Char('|')));
    QFile Old(Target);
    CHECK(Old.open(QIODevice::ReadOnly) && (Old.readAll() == "old"));
    Old.close();
    // Delete replaces it
    UI.Reset();
    UI.QueryAnswers = {qaRetry};
    R = Upload(T, {Small}, Dir, NoConfirm);
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(Sha256(Target) == Sha256(Small));
  }

  std::printf("[new files and folders: the rights are the server's]\n");
  {
    // a server with umask 002, as a shared group folder: 0664/0775 unless set
    SessionData Group = Data;
    QString SftpServer = qEnvironmentVariable("NIXSCP_TEST_SFTP_SERVER");
    for (const char * Candidate : {"/usr/lib/openssh/sftp-server", "/usr/libexec/openssh/sftp-server", "/usr/libexec/sftp-server"})
    {
      if (SftpServer.isEmpty() && QFileInfo(QString::fromLatin1(Candidate)).isExecutable())
      {
        SftpServer = QString::fromLatin1(Candidate);
      }
    }
    if (SftpServer.isEmpty())
    {
      Log(QStringLiteral("SKIPPED: no sftp-server to run with its own umask (set NIXSCP_TEST_SFTP_SERVER)"));
    }
    else
    {
      Group.SftpServer = SftpServer + QStringLiteral(" -u 002");
      UI.Reset();
      Terminal T(Group, &UI);
      OperationResult R = DoOpen(T);
      CHECK_MSG(R.Ok, Describe(R));
      QString Target = Dir + QStringLiteral("/rights");
      QDir().mkpath(Target);
      QString Tree = Local + QStringLiteral("/tree");
      QDir().mkpath(Tree + QStringLiteral("/sub"));
      WriteFile(Tree + QStringLiteral("/sub/f.txt"), "x", 1600000000);
      WriteFile(Local + QStringLiteral("/plain.txt"), "x", 1600000000);
      QFile::setPermissions(Local + QStringLiteral("/plain.txt"), QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser | QFile::WriteUser);
      R = Upload(T, {Tree, Local + QStringLiteral("/plain.txt"), Big}, Target, Param);
      CHECK_MSG(R.Ok, Describe(R));
      CHECK_MSG(LocalMode(Target + QStringLiteral("/plain.txt")) == 0664, QString::number(LocalMode(Target + QStringLiteral("/plain.txt")), 8));
      CHECK_MSG(LocalMode(Target + QStringLiteral("/big.bin")) == 0664, QString::number(LocalMode(Target + QStringLiteral("/big.bin")), 8));
      CHECK_MSG(LocalMode(Target + QStringLiteral("/tree")) == 0775, QString::number(LocalMode(Target + QStringLiteral("/tree")), 8));
      CHECK_MSG(LocalMode(Target + QStringLiteral("/tree/sub")) == 0775, QString::number(LocalMode(Target + QStringLiteral("/tree/sub")), 8));
      CHECK_MSG(LocalMode(Target + QStringLiteral("/tree/sub/f.txt")) == 0664, QString::number(LocalMode(Target + QStringLiteral("/tree/sub/f.txt")), 8));
      {
        Waiter W;
        T.CreateDirectory(Target + QStringLiteral("/made"), ServerDefaultMode, W.Cb());
        R = W.Wait();
        CHECK_MSG(R.Ok, Describe(R));
        CHECK(LocalMode(Target + QStringLiteral("/made")) == 0775);
      }
      // "Set permissions" still rules
      CopyParam Set = Param;
      Set.PreserveRights = true;
      Set.Rights = 0600;
      WriteFile(Local + QStringLiteral("/set.txt"), "x", 1600000000);
      R = Upload(T, {Local + QStringLiteral("/set.txt")}, Target, Set);
      CHECK_MSG(R.Ok, Describe(R));
      CHECK(LocalMode(Target + QStringLiteral("/set.txt")) == 0600);
      // an overwritten file keeps its rights
      QFile::setPermissions(Target + QStringLiteral("/plain.txt"), QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser | QFile::WriteUser);
      CopyParam Overwrite = Param;
      Overwrite.ConfirmOverwriting = false;
      R = Upload(T, {Local + QStringLiteral("/plain.txt")}, Target, Overwrite);
      CHECK_MSG(R.Ok, Describe(R));
      CHECK_MSG(LocalMode(Target + QStringLiteral("/plain.txt")) == 0600, QString::number(LocalMode(Target + QStringLiteral("/plain.txt")), 8));
    }
  }

  std::printf("[SFTPRealPath off: no SSH_FXP_REALPATH]\n");
  {
    SessionData NoReal = Data;
    NoReal.SFTPRealPath = asOff;
    NoReal.RemoteDirectory = Dir;
    UI.Reset();
    Terminal T(NoReal, &UI);
    OperationResult R = DoOpen(T);
    CHECK_MSG(R.Ok, Describe(R));
    CHECK_MSG(T.CurrentDirectory() == Dir, T.CurrentDirectory());
    // resolved locally, "/./" and ".." and all
    QString Result;
    R = DoJob(T, [&](FileSystem & Fs, FileOperationProgress &, UserInterface &)
    {
      Result = Fs.CanonicalizePath(Dir + QStringLiteral("/./rights/../rights"));
    });
    CHECK_MSG(R.Ok, Describe(R));
    CHECK_MSG(Result == Dir + QStringLiteral("/rights"), Result);
  }

  std::printf("[PreserveReadOnly]\n");
  {
    QString Remote = Dir + QStringLiteral("/readonly.txt");
    WriteFile(Remote, "ro", 1600000000);
    QFile::setPermissions(Remote, QFile::ReadOwner | QFile::ReadUser | QFile::ReadGroup);
    QString Target = QDir(Scratch).filePath(QStringLiteral("rodown"));
    QDir().mkpath(Target);
    UI.Reset();
    Terminal T(Data, &UI);
    CHECK_MSG(DoOpen(T).Ok, QStringLiteral("open"));
    CopyParam ReadOnly = Param;
    ReadOnly.PreserveReadOnly = true;
    Waiter W;
    T.CopyToLocal({Remote}, Target, ReadOnly, false, W.Cb());
    OperationResult R = W.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    CHECK_MSG((LocalMode(Target + QStringLiteral("/readonly.txt")) & 0222) == 0, QString::number(LocalMode(Target + QStringLiteral("/readonly.txt")), 8));
    // without the option the file stays writable
    QFile::setPermissions(Target + QStringLiteral("/readonly.txt"), QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser | QFile::WriteUser);
    QFile::remove(Target + QStringLiteral("/readonly.txt"));
    Waiter W2;
    T.CopyToLocal({Remote}, Target, Param, false, W2.Cb());
    R = W2.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    CHECK((LocalMode(Target + QStringLiteral("/readonly.txt")) & 0200) != 0);
  }

  std::printf("[TrimVMSVersions]\n");
  {
    SessionData Trim = Data;
    Trim.TrimVMSVersions = true;
    QString Remote = Dir + QStringLiteral("/vms");
    QDir().mkpath(Remote);
    WriteFile(Remote + QStringLiteral("/report.txt;12"), "vms", 1600000000);
    QString Target = QDir(Scratch).filePath(QStringLiteral("vmsdown"));
    QDir().mkpath(Target);
    UI.Reset();
    Terminal T(Trim, &UI);
    OperationResult R = DoOpen(T);
    CHECK_MSG(R.Ok, Describe(R));
    Waiter W;
    T.CopyToLocal({Remote}, Target, Param, false, W.Cb());
    R = W.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(QFile::exists(Target + QStringLiteral("/vms/report.txt")));
    CHECK(!QFile::exists(Target + QStringLiteral("/vms/report.txt;12")));
  }
}

int main(int argc, char ** argv)
{
  QCoreApplication App(argc, argv);

  QByteArray PortStr = qgetenv("NIXSCP_TEST_SSH_PORT");
  if (PortStr.isEmpty())
  {
    std::printf("test_sftp: SKIPPED (set NIXSCP_TEST_SSH_PORT, NIXSCP_TEST_SSH_KEY [, NIXSCP_TEST_SSH_HOST, NIXSCP_TEST_SSH_USER])\n");
    return 0;
  }
  QString Host = qEnvironmentVariable("NIXSCP_TEST_SSH_HOST", QStringLiteral("127.0.0.1"));
  QString User = qEnvironmentVariable("NIXSCP_TEST_SSH_USER", qEnvironmentVariable("USER"));
  QString KeyFile = qEnvironmentVariable("NIXSCP_TEST_SSH_KEY");
  QString PassKeyFile = qEnvironmentVariable("NIXSCP_TEST_SSH_PASSKEY");
  QString Passphrase = qEnvironmentVariable("NIXSCP_TEST_SSH_PASSPHRASE");
  if (KeyFile.isEmpty())
  {
    std::printf("test_sftp: NIXSCP_TEST_SSH_KEY not set\n");
    return 1;
  }

  QTemporaryDir Scratch;
  CHECK(Scratch.isValid());
  // In a not yet existing directory, as ~/.ssh may be missing on a fresh account
  QString KnownHosts = Scratch.filePath(QStringLiteral("dotssh/known_hosts"));
  QString RealKnownHosts = QDir::homePath() + QStringLiteral("/.ssh/known_hosts");
  QDateTime RealKnownHostsTime = QFileInfo(RealKnownHosts).lastModified();

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

  {
    SessionData TargetsData = Data;
    TargetsData.HostKey = QStringLiteral("*");
    TestUploadTargets(TargetsData, UI, Param, Scratch.path());
  }

  std::printf("[initial directory]\n");
  {
    SessionData Missing = Data;
    Missing.HostKey = QStringLiteral("*");
    Missing.RemoteDirectory = Scratch.filePath(QStringLiteral("no/such/directory"));
    // a stored site with UpdateDirectories falls back to the home directory
    {
      UI.Reset();
      Terminal T(Missing, &UI);
      OperationResult R = DoOpen(T);
      CHECK_MSG(R.Ok, Describe(R));
      CHECK_MSG(T.CurrentDirectory() != Missing.RemoteDirectory, T.CurrentDirectory());
    }
    // OpenSSH canonicalizes a missing last component: still an error
    Missing.RemoteDirectory = Scratch.filePath(QStringLiteral("missing"));
    Missing.RequireDirectories = true;
    {
      UI.Reset();
      Terminal T(Missing, &UI);
      OperationResult R = DoOpen(T);
      CHECK_MSG(!R.Ok && R.Message.contains(QStringLiteral("Error changing directory")), Describe(R));
      CHECK(!T.Active());
    }
    Missing.RequireDirectories = false;
    Missing.UpdateDirectories = false;
    {
      UI.Reset();
      Terminal T(Missing, &UI);
      OperationResult R = DoOpen(T);
      CHECK_MSG(!R.Ok, Describe(R));
    }
    // a file is no directory
    QString File = Scratch.filePath(QStringLiteral("afile"));
    WriteFile(File, "x", 1600000000);
    Missing.RemoteDirectory = File;
    Missing.RequireDirectories = true;
    Missing.UpdateDirectories = true;
    {
      UI.Reset();
      Terminal T(Missing, &UI);
      OperationResult R = DoOpen(T);
      CHECK_MSG(!R.Ok, Describe(R));
    }
    // an existing one is simply used
    Missing.RemoteDirectory = Scratch.path();
    {
      UI.Reset();
      Terminal T(Missing, &UI);
      OperationResult R = DoOpen(T);
      CHECK_MSG(R.Ok, Describe(R));
      CHECK_MSG(T.CurrentDirectory() == QDir(Scratch.path()).canonicalPath(), T.CurrentDirectory());
    }
  }

  // ---------------------------------------------------------------- host keys
  std::printf("[host key: cancel]\n");
  {
    UI.Reset();
    UI.HostKeyResponse = UserInterface::HostKeyCancel;
    Terminal T(Data, &UI);
    OperationResult R = DoOpen(T);
    CHECK_MSG(!R.Ok && R.Fatal, Describe(R));
    CHECK(R.Message == QStringLiteral("Host key wasn't verified!"));
    CHECK(UI.HostKeyCalls == 1);
    CHECK(!T.Active());
    CHECK(!QFile::exists(KnownHosts));
  }

  std::printf("[host key: changed]\n");
  {
    QString FakePub;
    CHECK(GenerateKey(Scratch.filePath(QStringLiteral("fakehost")), &FakePub));
    QString HostEntry = (Data.PortNumber == 22) ? Host : QStringLiteral("[%1]:%2").arg(Host).arg(Data.PortNumber);
    QDir().mkpath(QFileInfo(KnownHosts).path());
    QFile F(KnownHosts);
    CHECK(F.open(QIODevice::WriteOnly));
    F.write(QStringLiteral("%1 ssh-ed25519 %2\n").arg(HostEntry, FakePub).toUtf8());
    F.close();
    {
      UI.Reset();
      UI.HostKeyResponse = UserInterface::HostKeyCancel;
      Terminal T(Data, &UI);
      OperationResult R = DoOpen(T);
      CHECK_MSG(!R.Ok && R.Fatal, Describe(R));
      CHECK(UI.HostKeyCalls == 1);
      CHECK(UI.LastHostKeyState == UserInterface::HostKeyChanged);
    }
    // Accepting a changed key must replace it, so it is not asked again
    {
      UI.Reset();
      UI.HostKeyResponse = UserInterface::HostKeyAccept;
      Terminal T(Data, &UI);
      OperationResult R = DoOpen(T);
      CHECK_MSG(R.Ok, Describe(R));
      CHECK(UI.HostKeyCalls == 1);
    }
    {
      UI.Reset();
      Terminal T(Data, &UI);
      OperationResult R = DoOpen(T);
      CHECK_MSG(R.Ok, Describe(R));
      CHECK_MSG(UI.HostKeyCalls == 0, QStringLiteral("asked again, state %1").arg(UI.LastHostKeyState));
    }
    QFile KH(KnownHosts);
    CHECK(KH.open(QIODevice::ReadOnly));
    Log(QStringLiteral("known_hosts after accepting changed key:\n    ") + QString::fromUtf8(KH.readAll()).trimmed().replace(QLatin1Char('\n'), QStringLiteral("\n    ")));
    KH.close();
    QDir(QFileInfo(KnownHosts).path()).removeRecursively();
  }

  std::printf("[host key: unknown -> accept, then known]\n");
  {
    UI.Reset();
    Terminal T(Data, &UI);
    OperationResult R = DoOpen(T);
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(UI.HostKeyCalls == 1);
    CHECK(UI.LastHostKeyState == UserInterface::HostKeyUnknown);
    CHECK(QFileInfo(KnownHosts).size() > 0);
    CHECK(T.Active());
    Log(QStringLiteral("Protocol: %1").arg(T.ProtocolName()));
    Log(QStringLiteral("Server info: %1").arg(T.ServerInfo().replace(QLatin1Char('\n'), QStringLiteral("; "))));
  }
  {
    UI.Reset();
    Terminal T(Data, &UI);
    OperationResult R = DoOpen(T);
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(UI.HostKeyCalls == 0);
  }
  CHECK(QFileInfo(RealKnownHosts).lastModified() == RealKnownHostsTime);

  // --------------------------------------------------------------- auth fails
  std::printf("[auth: wrong key]\n");
  {
    QString WrongKey = Scratch.filePath(QStringLiteral("wrong_key"));
    CHECK(GenerateKey(WrongKey));
    SessionData Wrong = Data;
    Wrong.PublicKeyFile = WrongKey;
    UI.Reset();
    Terminal T(Wrong, &UI);
    OperationResult R = DoOpen(T);
    CHECK_MSG(!R.Ok, Describe(R));
    // a server that takes passwords too, as the one of macOS, is asked for no less
    // and the question is answered with Cancel; one that takes keys only says no
    if (UI.PromptKinds.isEmpty())
    {
      CHECK_MSG(R.Fatal, Describe(R));
      CHECK(R.Message == QStringLiteral("Authentication failed."));
    }
    else
    {
      CHECK_MSG(R.Aborted, Describe(R));
    }
    Log(Describe(R));
  }
  std::printf("[auth: missing key file]\n");
  {
    SessionData Wrong = Data;
    Wrong.PublicKeyFile = Scratch.filePath(QStringLiteral("does-not-exist"));
    UI.Reset();
    Terminal T(Wrong, &UI);
    OperationResult R = DoOpen(T);
    CHECK_MSG(!R.Ok && R.Fatal, Describe(R));
  }
  std::printf("[auth: connection refused]\n");
  {
    SessionData Wrong = Data;
    Wrong.PortNumber = 1;
    UI.Reset();
    Terminal T(Wrong, &UI);
    OperationResult R = DoOpen(T);
    CHECK_MSG(!R.Ok && R.Fatal, Describe(R));
    Log(Describe(R));
  }
  if (!PassKeyFile.isEmpty())
  {
    std::printf("[auth: passphrase key]\n");
    SessionData Pass = Data;
    Pass.PublicKeyFile = PassKeyFile;
    {
      UI.Reset();
      UI.PromptAnswers = QStringList{QStringLiteral("wrong"), Passphrase};
      Terminal T(Pass, &UI);
      OperationResult R = DoOpen(T);
      CHECK_MSG(R.Ok, Describe(R));
      CHECK(UI.PromptKinds.size() == 2);
      CHECK(UI.PromptKinds.value(0) == UserInterface::pkPassphrase);
    }
    {
      UI.Reset();  // no answers -> prompt cancelled
      Terminal T(Pass, &UI);
      OperationResult R = DoOpen(T);
      CHECK_MSG(!R.Ok && R.Aborted, Describe(R));
    }
  }

  // ---------------------------------------------------------------- main session
  std::printf("[session]\n");
  UI.Reset();
  Terminal T(Data, &UI);
  OperationResult R = DoOpen(T);
  CHECK_MSG(R.Ok, Describe(R));
  if (!R.Ok)
  {
    std::printf("test_sftp: cannot connect, giving up\n");
    return 1;
  }
  QString Home = T.CurrentDirectory();
  Log(QStringLiteral("Home: %1").arg(Home));
  CHECK(Home.startsWith(QLatin1Char('/')));

  RemoteFileList Files;
  QString Dir;
  R = DoList(T, Home, &Files, &Dir);
  CHECK_MSG(R.Ok, Describe(R));
  CHECK(Dir == Home);
  CHECK(!Find(Files, QStringLiteral(".")) && !Find(Files, QStringLiteral("..")));
  R = DoList(T, QStringLiteral("/"), &Files, &Dir);
  CHECK_MSG(R.Ok, Describe(R));
  CHECK(Dir == QStringLiteral("/"));
  CHECK(Find(Files, QStringLiteral("tmp")) && Find(Files, QStringLiteral("tmp"))->IsDirectory);
  CHECK(Find(Files, QStringLiteral("usr")) && Find(Files, QStringLiteral("usr"))->IsDirectory);
  if (const RemoteFile * Bin = Find(Files, QStringLiteral("bin")))
  {
    Log(QStringLiteral("/bin: symlink=%1 dir=%2 -> %3").arg(Bin->IsSymLink).arg(Bin->IsDirectory).arg(Bin->LinkTo));
    if (Bin->IsSymLink)
    {
      CHECK(Bin->IsDirectory);
      CHECK(!Bin->LinkTo.isEmpty());
    }
  }
  // relative path resolves against home
  R = DoList(T, QStringLiteral("."), &Files, &Dir);
  CHECK_MSG(R.Ok && (Dir == Home), Describe(R) + Dir);

  // Work directory
  QString Base = UnixPath::Combine(Home, QStringLiteral("nixscp-test-%1").arg(QCoreApplication::applicationPid()));
  {
    Waiter W;
    T.CreateDirectory(Base, 0755, W.Cb());
    R = W.Wait();
    CHECK_MSG(R.Ok, Describe(R));
  }
  {
    Waiter W;
    T.CreateDirectory(Base, 0755, W.Cb());
    R = W.Wait();
    CHECK_MSG(!R.Ok && !R.Fatal, Describe(R));
    Log(QStringLiteral("mkdir existing: ") + Describe(R));
    // the error was offered as Retry / Skip / Abort (TRetryOperationLoop)
    QMutexLocker Lock(&UI.Mutex);
    CHECK((UI.Queries.size() == 1) && UI.Queries[0].startsWith(QStringLiteral("Error creating folder")));
    UI.Queries.clear();
  }
  // Server shares our filesystem?
  bool Local = QFileInfo(Base).isDir();
  Log(QStringLiteral("Server filesystem is local: %1").arg(Local));
  if (Local)
  {
    CHECK(LocalMode(Base) == (0755 & ~[]() { mode_t M = umask(0); umask(M); return M; }()));
  }

  // ---------------------------------------------------------------- local tree
  QTemporaryDir Work;
  QString Src = Work.filePath(QStringLiteral("src"));
  QString UnicodeName = QStringLiteral("file with spaces ünïcødé €.txt");
  const qint64 BigSize = 20 * 1024 * 1024 + 123;
  QDir().mkpath(Src + QStringLiteral("/sub/deeper"));
  QDir().mkpath(Src + QStringLiteral("/emptydir"));
  const qint64 T0 = 1577934245; // 2020-01-02 03:04:05 UTC
  WriteFile(Src + QStringLiteral("/a.txt"), "hello\n", T0);
  WriteFile(Src + QStringLiteral("/empty.bin"), QByteArray(), T0 + 1);
  WriteFile(Src + QStringLiteral("/") + UnicodeName, "unicode\n", T0 + 2);
  WriteFile(Src + QStringLiteral("/.hidden"), "hidden\n", T0 + 3);
  WriteFile(Src + QStringLiteral("/sub/nested.txt"), RandomData(100000), T0 + 4);
  WriteFile(Src + QStringLiteral("/sub/deeper/x.dat"), RandomData(32768 * 3), T0 + 5);
  QString BigDir = Work.filePath(QStringLiteral("bigsrc"));
  QDir().mkpath(BigDir);
  QString BigLocal = BigDir + QStringLiteral("/big.bin");
  WriteFile(BigLocal, RandomData(BigSize), T0 + 6);

  // ---------------------------------------------------------------- upload
  std::printf("[upload tree]\n");
  {
    Waiter W;
    T.CopyToRemote(QStringList{Src}, Base, Param, false, W.Cb());
    R = W.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(UI.Queries.isEmpty());
    FileOperationProgress::Snapshot S = T.Progress().Get();
    CHECK(!S.InProgress);
    CHECK(S.TotalSize == 6 + 0 + 8 + 7 + 100000 + 32768 * 3);
    CHECK(S.TotalTransferred == S.TotalSize);
  }
  QString RSrc = UnixPath::Combine(Base, QStringLiteral("src"));
  R = DoList(T, RSrc, &Files);
  CHECK_MSG(R.Ok, Describe(R));
  {
    CHECK(Files.size() == 6);
    const RemoteFile * F = Find(Files, QStringLiteral("a.txt"));
    CHECK(F && (F->Size == 6) && (F->Modification.toSecsSinceEpoch() == T0) && !F->IsDirectory);
    F = Find(Files, QStringLiteral("empty.bin"));
    CHECK(F && (F->Size == 0) && (F->Modification.toSecsSinceEpoch() == T0 + 1));
    F = Find(Files, UnicodeName);
    CHECK(F && (F->Size == 8) && (F->Modification.toSecsSinceEpoch() == T0 + 2));
    F = Find(Files, QStringLiteral(".hidden"));
    CHECK(F && F->IsHidden);
    F = Find(Files, QStringLiteral("sub"));
    CHECK(F && F->IsDirectory && (F->TypeChar() == QStringLiteral("d")));
    F = Find(Files, QStringLiteral("emptydir"));
    CHECK(F && F->IsDirectory);
    F = Find(Files, QStringLiteral("a.txt"));
    if (F)
    {
      CHECK(F->Permissions == 0644);
      CHECK(!F->Owner.isEmpty());
      Log(QStringLiteral("a.txt: %1%2 %3 %4").arg(F->TypeChar(), F->RightsText(), F->Owner, F->Group));
    }
  }
  R = DoList(T, UnixPath::Combine(RSrc, QStringLiteral("sub/deeper")), &Files);
  CHECK_MSG(R.Ok, Describe(R));
  CHECK((Files.size() == 1) && (Files[0].Size == 32768 * 3) && (Files[0].Modification.toSecsSinceEpoch() == T0 + 5));
  if (Local)
  {
    CompareTrees(Src, RSrc, true);
  }

  std::printf("[upload 20 MB]\n");
  double UploadMBs = 0, DownloadMBs = 0;
  {
    Waiter W;
    QElapsedTimer Timer;
    Timer.start();
    T.CopyToRemote(QStringList{BigLocal}, Base, Param, false, W.Cb());
    R = W.Wait();
    qint64 Ms = std::max<qint64>(1, Timer.elapsed());
    CHECK_MSG(R.Ok, Describe(R));
    UploadMBs = (BigSize / 1048576.0) / (Ms / 1000.0);
    Log(QStringLiteral("upload: %1 ms, %2 MB/s").arg(Ms).arg(UploadMBs, 0, 'f', 1));
  }
  QString RBig = UnixPath::Combine(Base, QStringLiteral("big.bin"));
  R = DoList(T, Base, &Files);
  CHECK(Find(Files, QStringLiteral("big.bin")) && (Find(Files, QStringLiteral("big.bin"))->Size == BigSize));
  if (Local)
  {
    CHECK(Sha256(RBig) == Sha256(BigLocal));
  }

  std::printf("[many small files]\n");
  double SmallUp = 0, SmallDown = 0;
  const int SmallCount = 300;
  {
    QString Many = Work.filePath(QStringLiteral("many"));
    QDir().mkpath(Many);
    for (int I = 0; I < SmallCount; I++)
    {
      WriteFile(Many + QStringLiteral("/f%1.txt").arg(I), QByteArray::number(I).repeated(10), T0);
    }
    Waiter W;
    QElapsedTimer Timer;
    Timer.start();
    T.CopyToRemote(QStringList{Many}, Base, Param, false, W.Cb());
    R = W.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    SmallUp = SmallCount * 1000.0 / std::max<qint64>(1, Timer.elapsed());
    QString DlMany = Work.filePath(QStringLiteral("dlmany"));
    QDir().mkpath(DlMany);
    Waiter W2;
    Timer.restart();
    T.CopyToLocal(QStringList{UnixPath::Combine(Base, QStringLiteral("many"))}, DlMany, Param, false, W2.Cb());
    R = W2.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    SmallDown = SmallCount * 1000.0 / std::max<qint64>(1, Timer.elapsed());
    CompareTrees(Many, DlMany + QStringLiteral("/many"), true);
    Log(QStringLiteral("%1 small files: upload %2 files/s, download %3 files/s").arg(SmallCount).arg(SmallUp, 0, 'f', 0).arg(SmallDown, 0, 'f', 0));
  }

  // ---------------------------------------------------------------- download
  std::printf("[download tree]\n");
  QString Dl = Work.filePath(QStringLiteral("dl"));
  QDir().mkpath(Dl);
  {
    Waiter W;
    T.CopyToLocal(QStringList{RSrc}, Dl, Param, false, W.Cb());
    R = W.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    CompareTrees(Src, Dl + QStringLiteral("/src"), true);
  }
  std::printf("[download 20 MB]\n");
  {
    Waiter W;
    QElapsedTimer Timer;
    Timer.start();
    T.CopyToLocal(QStringList{RBig}, Dl, Param, false, W.Cb());
    R = W.Wait();
    qint64 Ms = std::max<qint64>(1, Timer.elapsed());
    CHECK_MSG(R.Ok, Describe(R));
    DownloadMBs = (BigSize / 1048576.0) / (Ms / 1000.0);
    Log(QStringLiteral("download: %1 ms, %2 MB/s").arg(Ms).arg(DownloadMBs, 0, 'f', 1));
    CHECK(Sha256(Dl + QStringLiteral("/big.bin")) == Sha256(BigLocal));
    CHECK(QFileInfo(Dl + QStringLiteral("/big.bin")).lastModified().toSecsSinceEpoch() == T0 + 6);
  }

  // ---------------------------------------------------------------- overwrite
  std::printf("[overwrite confirmation, upload: No then Yes to all]\n");
  {
    // Change all small files, so overwritten / skipped files can be told apart
    for (const QString & Name : {QStringLiteral("a.txt"), QStringLiteral("empty.bin"), UnicodeName, QStringLiteral(".hidden"),
           QStringLiteral("sub/nested.txt"), QStringLiteral("sub/deeper/x.dat")})
    {
      QFile F(Src + QLatin1Char('/') + Name);
      CHECK(F.open(QIODevice::Append));
      F.write("v2");
      F.close();
    }
    UI.Reset();
    UI.QueryAnswers = {qaNo, qaYesToAll};
    Waiter W;
    T.CopyToRemote(QStringList{Src}, Base, Param, false, W.Cb());
    R = W.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    CHECK_MSG(UI.Queries.size() == 2, UI.Queries.join(QLatin1Char('|')));
    QString Skipped = UI.Queries.value(0).section(QLatin1Char('\''), 1, 1);
    Log(QStringLiteral("skipped: %1").arg(Skipped));
    // Verify by downloading to a fresh dir
    QString Dl2 = Work.filePath(QStringLiteral("dl2"));
    QDir().mkpath(Dl2);
    Waiter W2;
    T.CopyToLocal(QStringList{RSrc}, Dl2, Param, false, W2.Cb());
    R = W2.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    for (auto It : Tree(Src).asKeyValueRange())
    {
      if (It.second.isDir())
      {
        continue;
      }
      QString Down = Dl2 + QStringLiteral("/src/") + It.first;
      bool Same = (Sha256(Down) == Sha256(It.second.filePath()));
      if (It.second.fileName() == Skipped)
      {
        CHECK_MSG(!Same && Sha256(Down) == Sha256(Dl + QStringLiteral("/src/") + It.first), It.first);
      }
      else
      {
        CHECK_MSG(Same, It.first);
      }
    }
  }
  std::printf("[overwrite confirmation, download: Cancel]\n");
  {
    UI.Reset();
    UI.QueryAnswers = {qaCancel};
    Waiter W;
    T.CopyToLocal(QStringList{RSrc}, Dl, Param, false, W.Cb());
    R = W.Wait();
    CHECK_MSG(!R.Ok && R.Aborted && !R.Fatal, Describe(R));
    CHECK(UI.Queries.size() == 1);
  }
  std::printf("[overwrite, download: confirmations off]\n");
  {
    UI.Reset();
    CopyParam NoConfirm = Param;
    NoConfirm.ConfirmOverwriting = false;
    Waiter W;
    T.CopyToLocal(QStringList{RSrc}, Dl, NoConfirm, false, W.Cb());
    R = W.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(UI.Queries.isEmpty());
  }

  // ---------------------------------------------------------------- rename
  std::printf("[rename]\n");
  {
    Waiter W;
    T.RenameFile(UnixPath::Combine(RSrc, QStringLiteral("a.txt")), UnixPath::Combine(RSrc, QStringLiteral("renamed ä.txt")), W.Cb());
    R = W.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    DoList(T, RSrc, &Files);
    CHECK(!Find(Files, QStringLiteral("a.txt")) && Find(Files, QStringLiteral("renamed ä.txt")));
    Waiter W2;
    T.RenameFile(UnixPath::Combine(RSrc, QStringLiteral("nonexistent")), UnixPath::Combine(RSrc, QStringLiteral("x")), W2.Cb());
    R = W2.Wait();
    CHECK_MSG(!R.Ok && !R.Fatal, Describe(R));
    Log(QStringLiteral("rename nonexistent: ") + Describe(R));
  }

  // ---------------------------------------------------------------- chmod
  std::printf("[change properties]\n");
  {
    QString File = UnixPath::Combine(RSrc, QStringLiteral("renamed ä.txt"));
    Waiter W;
    T.ChangeFileProperties(QStringList{File}, 0600, false, W.Cb());
    R = W.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    DoList(T, RSrc, &Files);
    CHECK(Find(Files, QStringLiteral("renamed ä.txt")) && (Find(Files, QStringLiteral("renamed ä.txt"))->Permissions == 0600));
    if (Local)
    {
      CHECK(LocalMode(File) == 0600);
    }
    QString Sub = UnixPath::Combine(RSrc, QStringLiteral("sub"));
    Waiter W2;
    // "Add X to directories" checked
    T.ChangeFileProperties(QStringList{Sub}, 0640, true, W2.Cb(), true);
    R = W2.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    if (Local)
    {
      CHECK_MSG(LocalMode(Sub) == 0750, Rights::ToOctal(LocalMode(Sub)));
      CHECK_MSG(LocalMode(Sub + QStringLiteral("/deeper")) == 0750, Rights::ToOctal(LocalMode(Sub + QStringLiteral("/deeper"))));
      CHECK_MSG(LocalMode(Sub + QStringLiteral("/nested.txt")) == 0640, Rights::ToOctal(LocalMode(Sub + QStringLiteral("/nested.txt"))));
      CHECK_MSG(LocalMode(Sub + QStringLiteral("/deeper/x.dat")) == 0640, Rights::ToOctal(LocalMode(Sub + QStringLiteral("/deeper/x.dat"))));
    }
    R = DoList(T, UnixPath::Combine(Sub, QStringLiteral("deeper")), &Files);
    CHECK_MSG(R.Ok, Describe(R));
    CHECK((Files.size() == 1) && (Files[0].Permissions == 0640));
  }

  // ---------------------------------------------------------------- symlink
  std::printf("[symlink]\n");
  {
    QString Link = UnixPath::Combine(Base, QStringLiteral("link-to-sub"));
    Waiter W;
    T.CreateLink(Link, UnixPath::Combine(RSrc, QStringLiteral("sub")), W.Cb());
    R = W.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    DoList(T, Base, &Files);
    const RemoteFile * L = Find(Files, QStringLiteral("link-to-sub"));
    CHECK(L && L->IsSymLink && L->IsDirectory && (L->LinkTo == UnixPath::Combine(RSrc, QStringLiteral("sub"))));
    if (Local)
    {
      CHECK(QFileInfo(Link).isSymLink() && (QFileInfo(Link).symLinkTarget() == UnixPath::Combine(RSrc, QStringLiteral("sub"))));
    }
  }

  std::printf("[symlink loop download]\n");
  {
    QString LoopDir = UnixPath::Combine(Base, QStringLiteral("loop"));
    Waiter W;
    T.CreateDirectory(LoopDir, 0755, W.Cb());
    CHECK(W.Wait().Ok);
    Waiter W2;
    T.CreateLink(UnixPath::Combine(LoopDir, QStringLiteral("self")), LoopDir, W2.Cb());
    CHECK(W2.Wait().Ok);
    QString DlLoop = Work.filePath(QStringLiteral("dlloop"));
    QDir().mkpath(DlLoop);
    Waiter W3;
    T.CopyToLocal(QStringList{LoopDir}, DlLoop, Param, false, W3.Cb());
    R = W3.Wait(60000);
    CHECK_MSG(!R.Fatal, Describe(R));
    int Depth = 0;
    for (QString P = DlLoop + QStringLiteral("/loop"); QFileInfo(P).isDir(); P += QStringLiteral("/self"))
    {
      Depth++;
    }
    Log(QStringLiteral("loop download: depth %1, %2").arg(Depth).arg(Describe(R)));
    R = DoList(T, Base, &Files);
    CHECK_MSG(R.Ok, Describe(R));
  }

  if (Local)
  {
    // Recursive delete must remove a symlink to a directory, not the directory contents
    QDir().mkpath(Work.filePath(QStringLiteral("keep")));
    WriteFile(Work.filePath(QStringLiteral("keep/file")), "keep", T0);
    Waiter W;
    T.CreateLink(UnixPath::Combine(Base, QStringLiteral("link-to-keep")), Work.filePath(QStringLiteral("keep")), W.Cb());
    CHECK(W.Wait().Ok);
  }

  // ---------------------------------------------------------------- errors
  std::printf("[errors]\n");
  {
    R = DoList(T, UnixPath::Combine(Base, QStringLiteral("nonexistent")), &Files);
    CHECK_MSG(!R.Ok && !R.Fatal && !R.Aborted, Describe(R));
    CHECK(!R.Message.isEmpty());
    CHECK_MSG(R.MoreMessages.contains(QStringLiteral("No such file")), R.MoreMessages);
    Log(QStringLiteral("list nonexistent: ") + Describe(R));
    CHECK(T.Active());
    // A file is not a directory
    R = DoList(T, RBig, &Files);
    CHECK_MSG(!R.Ok && !R.Fatal, Describe(R));
    Log(QStringLiteral("list file: ") + Describe(R));
    // Session still works
    R = DoList(T, Base, &Files);
    CHECK_MSG(R.Ok, Describe(R));
    // Download of nonexistent file
    Waiter W;
    T.CopyToLocal(QStringList{UnixPath::Combine(Base, QStringLiteral("nonexistent"))}, Dl, Param, false, W.Cb());
    R = W.Wait();
    CHECK_MSG(!R.Ok && !R.Fatal, Describe(R));
    Log(QStringLiteral("download nonexistent: ") + Describe(R));
    // Upload into nonexistent dir
    Waiter W2;
    T.CopyToRemote(QStringList{Src + QStringLiteral("/empty.bin")}, UnixPath::Combine(Base, QStringLiteral("nonexistent")), Param, false, W2.Cb());
    R = W2.Wait();
    CHECK_MSG(!R.Ok && !R.Fatal, Describe(R));
    Log(QStringLiteral("upload to nonexistent dir: ") + Describe(R));
  }

  // ---------------------------------------------------------------- cancel
  auto CancelWhenStarted = [&T](qint64 & TransferredAtCancel)
  {
    auto Timer = new QTimer();
    Timer->setInterval(0);
    QObject::connect(Timer, &QTimer::timeout, Timer, [&T, Timer, &TransferredAtCancel]()
    {
      FileOperationProgress::Snapshot S = T.Progress().Get();
      if (S.FileTransferred > 0)
      {
        TransferredAtCancel = S.FileTransferred;
        T.Progress().Cancel();
        Timer->stop();
        Timer->deleteLater();
      }
    });
    Timer->start();
    return Timer;
  };
  // Plain (non-resumable) transfers: partial target is deleted on cancel
  CopyParam NoResume = Param;
  NoResume.ResumeSupport = CopyParam::rsOff;
  std::printf("[cancel download]\n");
  {
    QString Dl3 = Work.filePath(QStringLiteral("dl3"));
    QDir().mkpath(Dl3);
    qint64 At = -1;
    QPointer<QTimer> Timer = CancelWhenStarted(At);
    Waiter W;
    T.CopyToLocal(QStringList{RBig}, Dl3, NoResume, false, W.Cb());
    R = W.Wait();
    delete Timer.data();
    Log(QStringLiteral("cancelled at %1 of %2 bytes: %3").arg(At).arg(BigSize).arg(Describe(R)));
    if (At >= 0 && At < BigSize)
    {
      CHECK_MSG(!R.Ok && R.Aborted && !R.Fatal, Describe(R));
      CHECK(!QFile::exists(Dl3 + QStringLiteral("/big.bin")));
    }
    else
    {
      Log(QStringLiteral("WARNING: transfer finished before it could be cancelled"));
    }
    // Session must stay usable (outstanding aio requests drained)
    R = DoList(T, Base, &Files);
    CHECK_MSG(R.Ok, Describe(R));
    Waiter W2;
    T.CopyToLocal(QStringList{RBig}, Dl3, Param, false, W2.Cb());
    R = W2.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(Sha256(Dl3 + QStringLiteral("/big.bin")) == Sha256(BigLocal));
  }
  std::printf("[cancel upload]\n");
  {
    qint64 At = -1;
    QPointer<QTimer> Timer = CancelWhenStarted(At);
    UI.Reset();
    UI.QueryAnswers = {qaYes};
    Waiter W;
    T.CopyToRemote(QStringList{BigLocal}, Base, NoResume, false, W.Cb());
    R = W.Wait();
    delete Timer.data();
    Log(QStringLiteral("cancelled at %1 of %2 bytes: %3").arg(At).arg(BigSize).arg(Describe(R)));
    if (At >= 0 && At < BigSize)
    {
      CHECK_MSG(!R.Ok && R.Aborted && !R.Fatal, Describe(R));
    }
    R = DoList(T, Base, &Files);
    CHECK_MSG(R.Ok, Describe(R));
    if (At >= 0 && At < BigSize)
    {
      // No partial file left behind (as WinSCP without resume)
      CHECK(!Find(Files, QStringLiteral("big.bin")));
    }
  }

  // ---------------------------------------------------------------- resume
  CopyParam Resume = Param;
  Resume.ResumeSupport = CopyParam::rsOn;
  Resume.ConfirmResume = false;
  // big.bin was removed by the cancelled non-resumable upload above
  {
    UI.Reset();
    UI.QueryAnswers = {qaYes};
    Waiter W0;
    T.CopyToRemote(QStringList{BigLocal}, Base, Param, false, W0.Cb());
    R = W0.Wait();
    CHECK_MSG(R.Ok, Describe(R));
  }
  std::printf("[resume download]\n");
  {
    QString Dl5 = Work.filePath(QStringLiteral("dl5"));
    QDir().mkpath(Dl5);
    QString Target = Dl5 + QStringLiteral("/big.bin");
    QString Partial = Target + QStringLiteral(".filepart");
    qint64 At = -1;
    QPointer<QTimer> Timer = CancelWhenStarted(At);
    Waiter W;
    T.CopyToLocal(QStringList{RBig}, Dl5, Resume, false, W.Cb());
    R = W.Wait();
    delete Timer.data();
    Log(QStringLiteral("cancelled at %1 of %2 bytes: %3").arg(At).arg(BigSize).arg(Describe(R)));
    if (At >= 0 && At < BigSize)
    {
      CHECK_MSG(!R.Ok && R.Aborted && !R.Fatal, Describe(R));
      CHECK(!QFile::exists(Target));
      CHECK(QFile::exists(Partial));
      qint64 PartialSize = QFileInfo(Partial).size();
      Log(QStringLiteral("partial file: %1 bytes").arg(PartialSize));
      CHECK((PartialSize > 0) && (PartialSize < BigSize));
      // Resume, confirming the resume prompt
      UI.Reset();
      UI.QueryAnswers = {qaYes};
      CopyParam Confirm = Resume;
      Confirm.ConfirmResume = true;
      Waiter W2;
      T.CopyToLocal(QStringList{RBig}, Dl5, Confirm, false, W2.Cb());
      R = W2.Wait();
      CHECK_MSG(R.Ok, Describe(R));
      CHECK_MSG((UI.Queries.size() == 1) && UI.Queries.value(0).contains(QStringLiteral("resume")), UI.Queries.join(QLatin1Char('|')));
      CHECK(T.Progress().Get().TotalTransferred == BigSize);
      CHECK(!QFile::exists(Partial));
      CHECK(Sha256(Target) == Sha256(BigLocal));
      CHECK(QFileInfo(Target).lastModified().toSecsSinceEpoch() == T0 + 6);
    }
    else
    {
      Log(QStringLiteral("WARNING: transfer finished before it could be cancelled"));
    }
    // Partial file bigger than source is deleted after a warning
    QString Nested = UnixPath::Combine(RSrc, QStringLiteral("sub/nested.txt"));
    WriteFile(Dl5 + QStringLiteral("/nested.txt.filepart"), RandomData(300000), T0);
    UI.Reset();
    UI.QueryAnswers = {qaOK};
    Waiter W3;
    T.CopyToLocal(QStringList{Nested}, Dl5, Resume, false, W3.Cb());
    R = W3.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    CHECK_MSG((UI.Queries.size() == 1) && UI.Queries.value(0).contains(QStringLiteral("bigger")), UI.Queries.join(QLatin1Char('|')));
    CHECK(!QFile::exists(Dl5 + QStringLiteral("/nested.txt.filepart")));
    CHECK(Sha256(Dl5 + QStringLiteral("/nested.txt")) == Sha256(Src + QStringLiteral("/sub/nested.txt")));
    // Smart resume: small files are transferred directly (no .filepart), big ones via .filepart
    CopyParam Smart = Param;
    CHECK(Smart.ResumeSupport == CopyParam::rsSmart);
    CHECK(!Smart.AllowResume(100000 + 2, QStringLiteral("nested.txt")));
    CHECK(Smart.AllowResume(BigSize, QStringLiteral("big.bin")));
  }

  std::printf("[resume upload]\n");
  {
    QString RBigPartial = RBig + QStringLiteral(".filepart");
    // Upload over existing file: cancelled resumable upload keeps the original target
    qint64 At = -1;
    QPointer<QTimer> Timer = CancelWhenStarted(At);
    UI.Reset();
    UI.QueryAnswers = {qaYes};
    Waiter W;
    T.CopyToRemote(QStringList{BigLocal}, Base, Resume, false, W.Cb());
    R = W.Wait();
    delete Timer.data();
    Log(QStringLiteral("cancelled at %1 of %2 bytes: %3").arg(At).arg(BigSize).arg(Describe(R)));
    R = DoList(T, Base, &Files);
    CHECK_MSG(R.Ok, Describe(R));
    const RemoteFile * Big = Find(Files, QStringLiteral("big.bin"));
    if (At >= 0 && At < BigSize)
    {
      CHECK(UI.Queries.size() == 1);   // overwrite confirmation
      CHECK(Big && (Big->Size == BigSize));
      const RemoteFile * Part = Find(Files, QStringLiteral("big.bin.filepart"));
      CHECK(Part && (Part->Size > 0) && (Part->Size <= BigSize));
      if (Part)
      {
        Log(QStringLiteral("remote partial file: %1 bytes").arg(Part->Size));
      }
      // Resume: no overwrite confirmation, as the partial file exists
      UI.Reset();
      Waiter W2;
      T.CopyToRemote(QStringList{BigLocal}, Base, Resume, false, W2.Cb());
      R = W2.Wait();
      CHECK_MSG(R.Ok, Describe(R));
      CHECK_MSG(UI.Queries.isEmpty(), UI.Queries.join(QLatin1Char('|')));
      CHECK(T.Progress().Get().TotalTransferred == BigSize);
      DoList(T, Base, &Files);
      CHECK(!Find(Files, QStringLiteral("big.bin.filepart")));
      Big = Find(Files, QStringLiteral("big.bin"));
      CHECK(Big && (Big->Size == BigSize) && (Big->Modification.toSecsSinceEpoch() == T0 + 6));
      if (Local)
      {
        CHECK(Sha256(RBig) == Sha256(BigLocal));
        CHECK(!QFile::exists(RBigPartial));
      }
      else
      {
        QString Dl6 = Work.filePath(QStringLiteral("dl6"));
        QDir().mkpath(Dl6);
        Waiter W3;
        T.CopyToLocal(QStringList{RBig}, Dl6, Param, false, W3.Cb());
        CHECK(W3.Wait().Ok);
        CHECK(Sha256(Dl6 + QStringLiteral("/big.bin")) == Sha256(BigLocal));
      }
    }
    else
    {
      Log(QStringLiteral("WARNING: transfer finished before it could be cancelled"));
    }
  }

  // ---------------------------------------------------------------- masks
  std::printf("[upload with file mask]\n");
  {
    QString Masked = UnixPath::Combine(Base, QStringLiteral("masked"));
    Waiter W;
    T.CreateDirectory(Masked, 0755, W.Cb());
    CHECK(W.Wait().Ok);
    CopyParam MaskParam = Param;
    MaskParam.IncludeFileMask = QStringLiteral("*.txt | deeper/");
    MaskParam.ExcludeHiddenFiles = true;
    MaskParam.ExcludeEmptyDirectories = true;
    UI.Reset();
    Waiter W2;
    T.CopyToRemote(QStringList{Src}, Masked, MaskParam, false, W2.Cb());
    R = W2.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(UI.Queries.isEmpty());
    FileOperationProgress::Snapshot S = T.Progress().Get();
    qint64 Expected = QFileInfo(Src + QStringLiteral("/a.txt")).size() + QFileInfo(Src + QLatin1Char('/') + UnicodeName).size() +
      QFileInfo(Src + QStringLiteral("/sub/nested.txt")).size();
    CHECK_MSG(S.TotalSize == Expected, QString::number(S.TotalSize));
    R = DoList(T, UnixPath::Combine(Masked, QStringLiteral("src")), &Files);
    CHECK_MSG(R.Ok, Describe(R));
    QStringList Names;
    for (const RemoteFile & F : Files) Names << F.FileName;
    Names.sort();
    QStringList ExpectedNames{QStringLiteral("a.txt"), UnicodeName, QStringLiteral("sub")};
    ExpectedNames.sort();
    CHECK_MSG(Names == ExpectedNames, Names.join(QLatin1Char('|')));
    R = DoList(T, UnixPath::Combine(Masked, QStringLiteral("src/sub")), &Files);
    CHECK_MSG(R.Ok && (Files.size() == 1) && (Files.value(0).FileName == QStringLiteral("nested.txt")), Describe(R));
  }
  std::printf("[download with size mask]\n");
  {
    QString Dl7 = Work.filePath(QStringLiteral("dl7"));
    QDir().mkpath(Dl7);
    CopyParam MaskParam = Param;
    MaskParam.IncludeFileMask = QStringLiteral(">50K");
    MaskParam.ExcludeEmptyDirectories = true;
    Waiter W;
    T.CopyToLocal(QStringList{RSrc}, Dl7, MaskParam, false, W.Cb());
    R = W.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    QStringList Got = Tree(Dl7).keys();
    Got.sort();
    QStringList ExpectedTree{QStringLiteral("src"), QStringLiteral("src/sub"), QStringLiteral("src/sub/deeper"),
      QStringLiteral("src/sub/deeper/x.dat"), QStringLiteral("src/sub/nested.txt")};
    CHECK_MSG(Got == ExpectedTree, Got.join(QLatin1Char('|')));
    CHECK(Sha256(Dl7 + QStringLiteral("/src/sub/deeper/x.dat")) == Sha256(Src + QStringLiteral("/sub/deeper/x.dat")));
    // Excluding everything leaves nothing
    QString Dl8 = Work.filePath(QStringLiteral("dl8"));
    QDir().mkpath(Dl8);
    MaskParam.IncludeFileMask = QStringLiteral("| */");
    MaskParam.ExcludeEmptyDirectories = false;
    Waiter W2;
    T.CopyToLocal(QStringList{RSrc}, Dl8, MaskParam, false, W2.Cb());
    R = W2.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(Tree(Dl8).isEmpty());
  }

  // ---------------------------------------------------------------- newer only
  std::printf("[newer only]\n");
  {
    QString NewerDir = Work.filePath(QStringLiteral("newer"));
    QDir().mkpath(NewerDir);
    WriteFile(NewerDir + QStringLiteral("/f1"), "old1", T0 + 100);
    WriteFile(NewerDir + QStringLiteral("/f2"), "old2", T0 + 100);
    Waiter W;
    T.CopyToRemote(QStringList{NewerDir}, Base, Param, false, W.Cb());
    CHECK(W.Wait().Ok);
    WriteFile(NewerDir + QStringLiteral("/f1"), "new1", T0 + 200);   // newer than remote
    WriteFile(NewerDir + QStringLiteral("/f2"), "new2", T0);         // older than remote
    CopyParam Newer = Param;
    Newer.NewerOnly = true;
    UI.Reset();
    Waiter W2;
    T.CopyToRemote(QStringList{NewerDir}, Base, Newer, false, W2.Cb());
    R = W2.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(UI.Queries.isEmpty());
    QString Dl9 = Work.filePath(QStringLiteral("dl9"));
    QDir().mkpath(Dl9);
    Waiter W3;
    T.CopyToLocal(QStringList{UnixPath::Combine(Base, QStringLiteral("newer"))}, Dl9, Param, false, W3.Cb());
    CHECK(W3.Wait().Ok);
    QFile F1(Dl9 + QStringLiteral("/newer/f1")), F2(Dl9 + QStringLiteral("/newer/f2"));
    CHECK(F1.open(QIODevice::ReadOnly) && (F1.readAll() == "new1"));
    CHECK(F2.open(QIODevice::ReadOnly) && (F2.readAll() == "old2"));
    // Download newer only: f1 has the same time on both sides -> skipped; local f2 is older -> downloaded
    WriteFile(Dl9 + QStringLiteral("/newer/f2"), "local", T0 - 100);
    Waiter W4;
    T.CopyToLocal(QStringList{UnixPath::Combine(Base, QStringLiteral("newer"))}, Dl9, Newer, false, W4.Cb());
    CHECK(W4.Wait().Ok);
    CHECK(UI.Queries.isEmpty());
    QFile F2b(Dl9 + QStringLiteral("/newer/f2"));
    CHECK(F2b.open(QIODevice::ReadOnly) && (F2b.readAll() == "old2"));
  }

  // ---------------------------------------------------------------- misc options
  std::printf("[directory timestamps, file name case, text mode]\n");
  {
    QString Opt = Work.filePath(QStringLiteral("Opt"));
    QDir().mkpath(Opt + QStringLiteral("/SubDir"));
    WriteFile(Opt + QStringLiteral("/SubDir/MixedCase.TXT"), "\xEF\xBB\xBFline1\nline2\n\x1A", T0 + 7);
    WriteFile(Opt + QStringLiteral("/Binary.dat"), "\xEF\xBB\xBF\x1A", T0 + 8);
    struct timespec Times[2];
    Times[0].tv_sec = Times[1].tv_sec = T0 + 9;
    Times[0].tv_nsec = Times[1].tv_nsec = 0;
    utimensat(AT_FDCWD, QFile::encodeName(Opt + QStringLiteral("/SubDir")).constData(), Times, 0);
    Times[0].tv_sec = Times[1].tv_sec = T0 + 10;
    utimensat(AT_FDCWD, QFile::encodeName(Opt).constData(), Times, 0);
    CopyParam Opts = Param;
    Opts.PreserveTimeDirs = true;
    Opts.FileNameCase = CopyParam::ncLowerCase;
    Opts.Mode = CopyParam::tmAutomatic;
    Opts.RemoveBOM = true;
    Opts.RemoveCtrlZ = true;
    Waiter W;
    T.CopyToRemote(QStringList{Opt}, Base, Opts, false, W.Cb());
    R = W.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    QString ROpt = UnixPath::Combine(Base, QStringLiteral("opt"));
    DoList(T, Base, &Files);
    const RemoteFile * OptDir = Find(Files, QStringLiteral("opt"));
    CHECK(OptDir && OptDir->IsDirectory && (OptDir->Modification.toSecsSinceEpoch() == T0 + 10));
    DoList(T, ROpt, &Files);
    const RemoteFile * SubDir = Find(Files, QStringLiteral("subdir"));
    CHECK(SubDir && (SubDir->Modification.toSecsSinceEpoch() == T0 + 9));
    const RemoteFile * Bin = Find(Files, QStringLiteral("binary.dat"));
    CHECK(Bin && (Bin->Size == 4));   // not in text mode mask: unchanged
    DoList(T, UnixPath::Combine(ROpt, QStringLiteral("subdir")), &Files);
    const RemoteFile * Text = Find(Files, QStringLiteral("mixedcase.txt"));
    CHECK(Text && (Text->Size == 12));   // BOM and EOF mark removed
    // Download back, keeping directory timestamps
    QString Dl10 = Work.filePath(QStringLiteral("dl10"));
    QDir().mkpath(Dl10);
    CopyParam Back = Param;
    Back.PreserveTimeDirs = true;
    Waiter W2;
    T.CopyToLocal(QStringList{ROpt}, Dl10, Back, false, W2.Cb());
    CHECK(W2.Wait().Ok);
    QFile F(Dl10 + QStringLiteral("/opt/subdir/mixedcase.txt"));
    CHECK(F.open(QIODevice::ReadOnly) && (F.readAll() == "line1\nline2\n"));
    CHECK(QFileInfo(Dl10 + QStringLiteral("/opt/subdir")).lastModified().toSecsSinceEpoch() == T0 + 9);
    CHECK(QFileInfo(Dl10 + QStringLiteral("/opt")).lastModified().toSecsSinceEpoch() == T0 + 10);
  }

  // ---------------------------------------------------------------- speed limit
  std::printf("[speed limit]\n");
  {
    const unsigned long Limit = 512 * 1024;
    const qint64 LimitedSize = 5 * Limit;
    QString LimitedDir = Work.filePath(QStringLiteral("limited"));
    QDir().mkpath(LimitedDir);
    WriteFile(LimitedDir + QStringLiteral("/limited.bin"), RandomData(LimitedSize), T0);
    CopyParam Limited = Param;
    Limited.CPSLimit = Limit;
    for (int Down = 0; Down < 2; Down++)
    {
      QElapsedTimer Timer;
      Timer.start();
      Waiter W;
      if (Down == 0)
      {
        T.CopyToRemote(QStringList{LimitedDir + QStringLiteral("/limited.bin")}, Base, Limited, false, W.Cb());
      }
      else
      {
        QString Dl11 = Work.filePath(QStringLiteral("dl11"));
        QDir().mkpath(Dl11);
        T.CopyToLocal(QStringList{UnixPath::Combine(Base, QStringLiteral("limited.bin"))}, Dl11, Limited, false, W.Cb());
      }
      R = W.Wait();
      CHECK_MSG(R.Ok, Describe(R));
      double Rate = LimitedSize / (std::max<qint64>(1, Timer.elapsed()) / 1000.0);
      Log(QStringLiteral("%1 at limit %2 KB/s: %3 KB/s (%4 ms)").arg(Down ? QStringLiteral("download") : QStringLiteral("upload"))
        .arg(Limit / 1024).arg(Rate / 1024, 0, 'f', 0).arg(Timer.elapsed()));
      // Per-second budget (as WinSCP): the first, partial second gets a full budget,
      // so N budgets take between N-2 and N seconds: rate < 5/3 of the limit here
      CHECK_MSG((Rate > 0.6 * Limit) && (Rate < 1.8 * Limit), QString::number(Rate / 1024));
    }
    CHECK(Sha256(Work.filePath(QStringLiteral("dl11/limited.bin"))) == Sha256(LimitedDir + QStringLiteral("/limited.bin")));
  }

  // ---------------------------------------------------------------- move
  std::printf("[move to local]\n");
  {
    QString Dl4 = Work.filePath(QStringLiteral("dl4"));
    QDir().mkpath(Dl4);
    Waiter W;
    T.CopyToLocal(QStringList{UnixPath::Combine(RSrc, QStringLiteral("sub"))}, Dl4, Param, true, W.Cb());
    R = W.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(QFileInfo::exists(Dl4 + QStringLiteral("/sub/deeper/x.dat")));
    DoList(T, RSrc, &Files);
    CHECK(!Find(Files, QStringLiteral("sub")));
  }

  // ---------------------------------------------------------------- delete
  std::printf("[delete recursive]\n");
  {
    Waiter W;
    T.DeleteFiles(QStringList{Base}, W.Cb());
    R = W.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    DoList(T, Home, &Files);
    CHECK(!Find(Files, UnixPath::ExtractFileName(Base)));
    if (Local)
    {
      CHECK(!QFileInfo::exists(Base));
      CHECK(QFileInfo::exists(Work.filePath(QStringLiteral("keep/file"))));
    }
  }

  QByteArray KillCmd = qgetenv("NIXSCP_TEST_SSH_KILL_CMD");
  if (!KillCmd.isEmpty())
  {
    std::printf("[connection lost]\n");
    UI.Reset();
    Terminal T2(Data, &UI);
    R = DoOpen(T2);
    CHECK_MSG(R.Ok, Describe(R));
    QString DisconnectMessage;
    int Disconnects = 0;
    QObject::connect(&T2, &Terminal::Disconnected, [&](const QString & Message, const QString &)
    {
      Disconnects++;
      DisconnectMessage = Message;
    });
    // Kills the server side of all test sessions (T is closed already or will be reopened)
    CHECK(std::system(KillCmd.constData()) == 0);
    R = DoList(T2, Home, &Files);
    CHECK_MSG(!R.Ok && R.Fatal, Describe(R));
    Log(QStringLiteral("after kill: ") + Describe(R));
    CHECK(Disconnects == 1);
    CHECK(!T2.Active());
  }

  // ---------------------------------------------------------------- disconnect
  std::printf("[close]\n");
  {
    T.Close();
    CHECK(!T.Active());
    Waiter W;
    T.ReadDirectory(Home, W.ListCb());
    R = W.Wait();
    CHECK_MSG(!R.Ok && R.Fatal, Describe(R));
  }
  CHECK(QFileInfo(RealKnownHosts).lastModified() == RealKnownHostsTime);

  std::printf("THROUGHPUT upload %.1f MB/s, download %.1f MB/s (20 MB, %s); small files: upload %.0f/s, download %.0f/s\n",
    UploadMBs, DownloadMBs, qPrintable(Host), SmallUp, SmallDown);
  if (Failures == 0)
  {
    std::printf("test_sftp: all tests passed\n");
    return 0;
  }
  std::printf("test_sftp: %d failure(s)\n", Failures);
  return 1;
}
