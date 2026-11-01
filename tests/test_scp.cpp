// Tests of core/ScpFileSystem: "ls -la" listing parser (always run) and an
// end-to-end test of Terminal + ScpFileSystem against a real SSH server, plus
// the console (Terminal::AnyCommand) on SCP and SFTP sessions.
//
// Server part is skipped unless NIXSCP_TEST_SSH_PORT is set; environment as
// for test_sftp (NIXSCP_TEST_SSH_PORT, _HOST, _USER, _KEY). The user's
// ~/.ssh/known_hosts is never touched (SessionData::KnownHostsFile).
#include "core/Terminal.h"
#include "core/ScpFileSystem.h"

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
#include <QTemporaryDir>
#include <QTimeZone>
#include <QTimer>

#include <sys/stat.h>
#include <fcntl.h>
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

// ---------------------------------------------------------------- listing parser

static RemoteFile Parse(const QString & Line, bool * Ok = nullptr, QString * Error = nullptr)
{
  try
  {
    RemoteFile F = ScpFileSystem::ParseListingLine(Line, QDate(2024, 11, 5));
    if (Ok != nullptr) *Ok = true;
    return F;
  }
  catch (const ExtException & E)
  {
    if (Ok != nullptr) *Ok = false;
    if (Error != nullptr) *Error = E.Message();
    return RemoteFile();
  }
}

static QDateTime Local(int Y, int M, int D, int H = 0, int Mi = 0, int S = 0)
{
  return QDateTime(QDate(Y, M, D), QTime(H, Mi, S));
}

static void TestListingParser()
{
  std::printf("[listing parser]\n");
  bool Ok = false;
  QString Error;

  // GNU ls in C locale, recent file (time instead of year)
  RemoteFile F = Parse(QStringLiteral("-rw-r--r--   1 martin users   12345 Oct 31 22:15 file.txt"), &Ok);
  CHECK(Ok);
  CHECK(F.FileName == QStringLiteral("file.txt"));
  CHECK(F.Size == 12345);
  CHECK(F.Permissions == 0644);
  CHECK(F.Owner == QStringLiteral("martin"));
  CHECK(F.Group == QStringLiteral("users"));
  CHECK(F.FileType == RemoteFile::File && !F.IsDirectory && !F.IsSymLink && !F.IsHidden);
  CHECK(F.Modification == Local(2024, 10, 31, 22, 15));

  // Date in the future -> last year
  F = Parse(QStringLiteral("-rw-r--r--   1 martin users   1 Dec 25 10:00 xmas"), &Ok);
  CHECK(Ok && (F.Modification == Local(2023, 12, 25, 10, 0)));
  // Today
  F = Parse(QStringLiteral("-rw-r--r--   1 martin users   1 Nov  5 23:59 today"), &Ok);
  CHECK(Ok && (F.Modification == Local(2024, 11, 5, 23, 59)) && (F.FileName == QStringLiteral("today")));

  // Old file: year instead of time (right aligned)
  F = Parse(QStringLiteral("-rw-------   1 root   root        5 Jan  3  2020 old.txt"), &Ok);
  CHECK(Ok && (F.Modification == Local(2020, 1, 3)) && (F.Permissions == 0600) && (F.FileName == QStringLiteral("old.txt")));
  // Year aligned to left (YYYY_)
  F = Parse(QStringLiteral("-rw-------   1 root   root        5 Jan 13 2020  old2.txt"), &Ok);
  CHECK_MSG(Ok && (F.Modification == Local(2020, 1, 13)) && (F.FileName == QStringLiteral("old2.txt")), F.FileName);

  // --full-time (GNU): exact time with timezone
  F = Parse(QStringLiteral("-rw-r--r-- 1 martin users 6 2020-01-02 03:04:05.123456789 +0000 a.txt"), &Ok);
  CHECK(Ok && (F.Modification.toSecsSinceEpoch() == 1577934245) && (F.FileName == QStringLiteral("a.txt")));
  F = Parse(QStringLiteral("-rw-r--r-- 1 martin users 6 2020-01-02 04:34:05.000000000 +0130 b.txt"), &Ok);
  CHECK(Ok && (F.Modification.toSecsSinceEpoch() == 1577934245));
  F = Parse(QStringLiteral("-rw-r--r-- 1 martin users 6 2020-01-01 22:04:05.000000000 -0500 c.txt"), &Ok);
  CHECK(Ok && (F.Modification.toSecsSinceEpoch() == 1577934245));
  // --full-time name with leading space is kept
  F = Parse(QStringLiteral("-rw-r--r-- 1 martin users 6 2020-01-02 03:04:05.000000000 +0000  lead"), &Ok);
  CHECK(Ok && (F.FileName == QStringLiteral(" lead")));

  // Second --full-time format (busybox): ddd mmm dd hh:nn:ss yyyy
  F = Parse(QStringLiteral("-rw-r--r--    1 root     root            12 Sat Oct 31 22:15:03 2020 bb.txt"), &Ok);
  CHECK(Ok && (F.Modification == Local(2020, 10, 31, 22, 15, 3)) && (F.FileName == QStringLiteral("bb.txt")));

  // dd mmm format
  F = Parse(QStringLiteral("-rw-r--r-- 1 u g 12 31 Oct 22:15 dm1"), &Ok);
  CHECK(Ok && (F.Modification == Local(2024, 10, 31, 22, 15)) && (F.FileName == QStringLiteral("dm1")));
  F = Parse(QStringLiteral("-rw-r--r-- 1 u g 12 31 Oct 2019 dm2"), &Ok);
  CHECK(Ok && (F.Modification == Local(2019, 10, 31)) && (F.FileName == QStringLiteral("dm2")));

  // Directory with ACL '+'
  F = Parse(QStringLiteral("drwxr-xr-x+  2 martin users 4096 Oct 31 22:15 acl dir"), &Ok);
  CHECK(Ok && F.IsDirectory && (F.FileType == RemoteFile::Directory) && (F.FileName == QStringLiteral("acl dir")) && (F.Permissions == 0755));
  // macOS extended attributes '@', SELinux '.', and '+' after a space
  F = Parse(QStringLiteral("-rw-r--r--@ 1 martin staff 6 Oct 31 22:15 mac"), &Ok);
  CHECK(Ok && (F.FileName == QStringLiteral("mac")) && (F.Size == 6));
  F = Parse(QStringLiteral("-rw-r--r--. 1 martin users 7 Oct 31 22:15 selinux"), &Ok);
  CHECK(Ok && (F.FileName == QStringLiteral("selinux")) && (F.Size == 7));
  F = Parse(QStringLiteral("-rw-r--r-- + 1 martin users 8 Oct 31 22:15 spaced"), &Ok);
  CHECK(Ok && (F.FileName == QStringLiteral("spaced")) && (F.Size == 8));

  // Symlinks
  F = Parse(QStringLiteral("lrwxrwxrwx   1 root root    7 Mar  1  2021 bin -> usr/bin"), &Ok);
  CHECK(Ok && F.IsSymLink && (F.FileType == RemoteFile::Symlink) && (F.FileName == QStringLiteral("bin")) && (F.LinkTo == QStringLiteral("usr/bin")));
  F = Parse(QStringLiteral("lrwxrwxrwx   1 root root   20 Mar  1  2021 my link -> /target with spaces"), &Ok);
  CHECK(Ok && (F.FileName == QStringLiteral("my link")) && (F.LinkTo == QStringLiteral("/target with spaces")));
  Parse(QStringLiteral("lrwxrwxrwx   1 root root    7 Mar  1  2021 nolinktarget"), &Ok);
  CHECK(!Ok);

  // Device files: "major, minor" instead of size
  F = Parse(QStringLiteral("crw-rw-rw-   1 root root   1,   3 Oct 31 22:15 null"), &Ok);
  CHECK(Ok && (F.FileType == RemoteFile::Other) && (F.Size == 3) && (F.FileName == QStringLiteral("null")) && (F.Permissions == 0666));
  F = Parse(QStringLiteral("brw-rw----   1 root disk   8,   0 Oct 31 22:15 sda"), &Ok);
  CHECK(Ok && (F.FileType == RemoteFile::Other) && (F.Size == 0) && (F.Group == QStringLiteral("disk")));
  F = Parse(QStringLiteral("prw-r--r--   1 root root      0 Oct 31 22:15 fifo"), &Ok);
  CHECK(Ok && (F.FileType == RemoteFile::Other));
  F = Parse(QStringLiteral("srwxrwxrwx   1 root root      0 Oct 31 22:15 socket"), &Ok);
  CHECK(Ok && (F.FileType == RemoteFile::Other));

  // Special permission bits
  F = Parse(QStringLiteral("-rwsr-sr-t   1 root root      0 Oct 31 22:15 suid"), &Ok);
  CHECK(Ok && (F.Permissions == 07755));
  F = Parse(QStringLiteral("drwxrwxrwT   1 root root      0 Oct 31 22:15 sticky"), &Ok);
  CHECK(Ok && (F.Permissions == 01776));
  F = Parse(QStringLiteral("-rwSr-Sr--   1 root root      0 Oct 31 22:15 bigS"), &Ok);
  CHECK(Ok && (F.Permissions == 06644));

  // Names: spaces, leading space, hidden, unicode
  F = Parse(QStringLiteral("-rw-r--r-- 1 u g 1 Oct 31 22:15 name with  spaces "), &Ok);
  CHECK(Ok && (F.FileName == QStringLiteral("name with  spaces ")));
  F = Parse(QStringLiteral("-rw-r--r-- 1 u g 1 Oct 31 22:15  lead"), &Ok);
  CHECK(Ok && (F.FileName == QStringLiteral(" lead")));
  F = Parse(QStringLiteral("-rw-r--r-- 1 u g 1 Oct 31 22:15 .hidden"), &Ok);
  CHECK(Ok && F.IsHidden);
  F = Parse(QStringLiteral("-rw-r--r-- 1 u g 1 Oct 31 22:15 ünïcødé €.txt"), &Ok);
  CHECK(Ok && (F.FileName == QStringLiteral("ünïcødé €.txt")));
  // "ls -d path" lists the path; only the name is used
  F = Parse(QStringLiteral("drwxr-xr-x 20 root root 4096 2026-06-09 23:18:30.738863822 +0200 /"), &Ok);
  CHECK(Ok && (F.FileName == QStringLiteral("/")) && F.IsDirectory);
  F = Parse(QStringLiteral("drwxr-xr-x 2 root root 4096 Oct 31 22:15 /home/u/dir/"), &Ok);
  CHECK(Ok && (F.FileName == QStringLiteral("dir")));
  F = Parse(QStringLiteral("-rw-r--r-- 1 u g 1 Oct 31 22:15 /home/u/dir/file"), &Ok);
  CHECK(Ok && (F.FileName == QStringLiteral("file")));
  // Tabs
  F = Parse(QStringLiteral("-rw-r--r--\t1 u g\t1 Oct 31 22:15 tab"), &Ok);
  CHECK(Ok && (F.FileName == QStringLiteral("tab")));

  // Group with space (Cygwin), no link count (Android BusyBox), big sizes
  F = Parse(QStringLiteral("-rw-r--r-- 1 martin Domain Users 12 Oct 31 22:15 cyg"), &Ok);
  CHECK(Ok && (F.Owner == QStringLiteral("martin")) && (F.Group == QStringLiteral("Domain Users")) && (F.Size == 12));
  F = Parse(QStringLiteral("-rw-r--r-- root     sdcard_rw   12 Oct 31 22:15 android"), &Ok);
  CHECK(Ok && (F.Owner == QStringLiteral("root")) && (F.Group == QStringLiteral("sdcard_rw")) && (F.Size == 12));
  F = Parse(QStringLiteral("-rw-r--r-- 1 u g 109951162777600 Oct 31 22:15 huge"), &Ok);
  CHECK(Ok && (F.Size == Q_INT64_C(109951162777600)));
  // No space between rights and link count
  F = Parse(QStringLiteral("-rw-r--r--1 u g 5 Oct 31 22:15 glued"), &Ok);
  CHECK(Ok && (F.FileName == QStringLiteral("glued")) && (F.Size == 5));
  // Numeric owner/group
  F = Parse(QStringLiteral("-rw-r--r-- 1 1000 1000 5 Oct 31 22:15 numeric"), &Ok);
  CHECK(Ok && (F.Owner == QStringLiteral("1000")) && (F.Group == QStringLiteral("1000")) && (F.Size == 5));
  // Month names are case-insensitive
  F = Parse(QStringLiteral("-rw-r--r-- 1 u g 5 OCT 31 22:15 upper"), &Ok);
  CHECK(Ok && (F.Modification == Local(2024, 10, 31, 22, 15)));

  // SSH FS (sshfs) entries without metadata
  F = Parse(QStringLiteral("d????????? ? ? ? ? ? broken"), &Ok);
  CHECK(Ok && (F.FileName == QStringLiteral("broken")) && (F.Size == 0) && !F.Modification.isValid() && F.IsDirectory);

  // "total" lines
  CHECK(ScpFileSystem::IsTotalListingLine(QStringLiteral("total 24")));
  CHECK(ScpFileSystem::IsTotalListingLine(QStringLiteral("totalt 24")));
  CHECK(ScpFileSystem::IsTotalListingLine(QStringLiteral("Total 5")));
  CHECK(!ScpFileSystem::IsTotalListingLine(QStringLiteral("-rw-r--r-- 1 u g 1 Oct 31 22:15 total")));

  // Errors
  Parse(QStringLiteral("garbage"), &Ok, &Error);
  CHECK(!Ok && (Error == QStringLiteral("Unexpected directory listing line 'garbage'.")));
  Parse(QString(), &Ok);
  CHECK(!Ok);
  Parse(QStringLiteral("-rw-r--r-- 1 u g 12 Foo 31 22:15 badmonth"), &Ok);
  CHECK(!Ok);
  Parse(QStringLiteral("-rw-r--r-- 1 u g 12 Oct 32 22:15 badday"), &Ok);
  CHECK(!Ok);
  Parse(QStringLiteral("-rw-r--r-- 1 u g 12 Oct 31 25:15 badhour"), &Ok);
  CHECK(!Ok);
  Parse(QStringLiteral("-rw-r--r-- 1 u g 12 Feb 30 2020 baddate"), &Ok);
  CHECK(!Ok);
  Parse(QStringLiteral("-rw-r--r-- 1 u g 12 Oct 31 22:15"), &Ok);
  CHECK(!Ok);
}

static void TestShellQuote()
{
  std::printf("[shell quoting]\n");
  CHECK(ShellQuoteStr(QStringLiteral("a b")) == QStringLiteral("\"a b\""));
  CHECK(ShellQuoteStr(QStringLiteral("$x`y\"z\\")) == QStringLiteral("\"\\$x\\`y\\\"z\\\\\""));
  CHECK(ShellQuoteStr(QStringLiteral("it's")) == QStringLiteral("\"it's\""));
}

// ---------------------------------------------------------------- server tests

class TestUserInterface : public UserInterface
{
public:
  QStringList PromptAnswers;
  QList<unsigned int> QueryAnswers;
  QMutex Mutex;
  int HostKeyCalls = 0;
  QString LastFingerprint;
  QStringList Queries;

  void Reset()
  {
    QMutexLocker Lock(&Mutex);
    PromptAnswers.clear();
    QueryAnswers.clear();
    HostKeyCalls = 0;
    Queries.clear();
  }

  HostKeyAnswer VerifyHostKey(const QString &, int, const QString &, const QString & FingerprintSHA256, const QString &, HostKeyState) override
  {
    QMutexLocker Lock(&Mutex);
    HostKeyCalls++;
    LastFingerprint = FingerprintSHA256;
    return HostKeyAccept;
  }

  bool PromptUser(PromptKind, const QString &, const QString &, QList<Prompt> & Prompts) override
  {
    QMutexLocker Lock(&Mutex);
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

  void DisplayBanner(const QString &, const QString &) override {}

  unsigned int QueryUser(const QString & Query, const QString &, unsigned int) override
  {
    QMutexLocker Lock(&Mutex);
    Queries.append(Query);
    unsigned int Answer = QueryAnswers.isEmpty() ? static_cast<unsigned int>(qaCancel) : QueryAnswers.takeFirst();
    Log(QStringLiteral("QueryUser \"%1\" -> %2").arg(Query).arg(Answer));
    return Answer;
  }
};

struct Waiter
{
  QEventLoop Loop;
  OperationResult Result;
  RemoteFileList Files;
  QString Directory;
  int ExitCode = -2;
  bool Called = false;

  Terminal::Callback Cb()
  {
    return [this](const OperationResult & R) { Result = R; Called = true; Loop.quit(); };
  }
  Terminal::ListCallback ListCb()
  {
    return [this](const OperationResult & R, const QString & Dir, const RemoteFileList & List)
    {
      Result = R; Directory = Dir; Files = List; Called = true; Loop.quit();
    };
  }
  Terminal::CommandCallback CommandCb()
  {
    return [this](const OperationResult & R, int Code, const QString & Dir)
    {
      CHECK(QThread::currentThread() == QCoreApplication::instance()->thread());
      Result = R; ExitCode = Code; Directory = Dir; Called = true; Loop.quit();
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

static OperationResult DoSimple(const std::function<void(Terminal::Callback)> & Call)
{
  Waiter W;
  Call(W.Cb());
  return W.Wait();
}

struct CommandResult
{
  OperationResult Result;
  int ExitCode = -2;
  QString Directory;
  QStringList Output;
  QStringList Errors;
};

static CommandResult DoCommand(Terminal & T, const QString & Command)
{
  CommandResult C;
  Waiter W;
  T.AnyCommand(Command, [&C](const QString & Line, bool StdError)
  {
    CHECK(QThread::currentThread() == QCoreApplication::instance()->thread());
    (StdError ? C.Errors : C.Output).append(Line);
  }, W.CommandCb());
  C.Result = W.Wait();
  C.ExitCode = W.ExitCode;
  C.Directory = W.Directory;
  return C;
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

static QByteArray RandomData(qint64 Size)
{
  QByteArray Data(Size, Qt::Uninitialized);
  QRandomGenerator Gen(4242);
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
  if (lstat(QFile::encodeName(Path).constData(), &St) != 0)
  {
    return 0xFFFFFFFF;
  }
  return St.st_mode & 07777;
}

static void TestConsole(Terminal & T, const QString & Home, const QString & Base, const char * Name)
{
  std::printf("[console %s]\n", Name);
  CHECK(T.CanAnyCommand());
  CommandResult C = DoCommand(T, QStringLiteral("echo hi"));
  CHECK_MSG(C.Result.Ok, Describe(C.Result));
  CHECK_MSG(C.Output == QStringList{QStringLiteral("hi")}, C.Output.join(QLatin1Char('|')));
  CHECK(C.Errors.isEmpty());
  CHECK(C.ExitCode == 0);

  C = DoCommand(T, QStringLiteral("false"));
  CHECK_MSG(C.Result.Ok, Describe(C.Result));
  CHECK_MSG(C.ExitCode == 1, QString::number(C.ExitCode));
  C = DoCommand(T, QStringLiteral("exit 3"));
  if (C.Result.Ok)
  {
    // SFTP: own channel per command
    CHECK_MSG(C.ExitCode == 3, QString::number(C.ExitCode));
  }
  else
  {
    // SCP: exiting the shell closes the session, as in WinSCP
    CHECK_MSG(C.Result.Fatal, Describe(C.Result));
    Log(QStringLiteral("exit: %1").arg(Describe(C.Result)));
    CHECK(DoOpen(T).Ok);
  }

  // pwd in the current directory of the terminal
  QString Start = T.CurrentDirectory();
  C = DoCommand(T, QStringLiteral("pwd"));
  CHECK_MSG(C.Result.Ok && (C.Output == QStringList{Start}), C.Output.join(QLatin1Char('|')) + QStringLiteral(" vs ") + Start);
  CHECK(C.Directory == Start);

  // Output and errors, empty lines, no trailing newline
  C = DoCommand(T, QStringLiteral("echo one; echo; echo two; echo err >&2; printf last"));
  CHECK_MSG(C.Result.Ok, Describe(C.Result));
  CHECK_MSG(C.Output == (QStringList{QStringLiteral("one"), QString(), QStringLiteral("two"), QStringLiteral("last")}), C.Output.join(QLatin1Char('|')));
  CHECK_MSG(C.Errors == QStringList{QStringLiteral("err")}, C.Errors.join(QLatin1Char('|')));
  C = DoCommand(T, QStringLiteral("echo a; echo"));
  CHECK_MSG(C.Output == (QStringList{QStringLiteral("a"), QString()}), C.Output.join(QLatin1Char('|')));

  // cd is tracked
  C = DoCommand(T, QStringLiteral("cd %1").arg(ShellQuoteStr(Base)));
  CHECK_MSG(C.Result.Ok && (C.ExitCode == 0), Describe(C.Result));
  CHECK_MSG(C.Directory == Base, C.Directory);
  CHECK(T.CurrentDirectory() == Base);
  C = DoCommand(T, QStringLiteral("pwd"));
  CHECK_MSG(C.Output == QStringList{Base}, C.Output.join(QLatin1Char('|')));
  C = DoCommand(T, QStringLiteral("cd /nonexistent-nixscp"));
  CHECK(C.Result.Ok && (C.ExitCode != 0) && !C.Errors.isEmpty() && (C.Directory == Base));
  C = DoCommand(T, QStringLiteral("cd .. # comment"));
  CHECK_MSG(C.Result.Ok && (C.Directory == UnixPath::Parent(Base)), C.Directory);
  // A listing after cd sets the directory of the terminal back
  CHECK(DoList(T, Home).Ok);
  CHECK(T.CurrentDirectory() == Home);

  // Long output
  C = DoCommand(T, QStringLiteral("seq 1 20000"));
  CHECK_MSG(C.Result.Ok && (C.Output.size() == 20000) && (C.Output.value(19999) == QStringLiteral("20000")), QString::number(C.Output.size()));

  // Cancel a long running command; the session stays usable
  QTimer::singleShot(500, [&T]() { T.Progress().Cancel(); });
  C = DoCommand(T, QStringLiteral("sleep 5; echo late"));
  CHECK_MSG(!C.Result.Ok && C.Result.Aborted && !C.Result.Fatal, Describe(C.Result));
  C = DoCommand(T, QStringLiteral("echo after"));
  CHECK_MSG(C.Result.Ok && (C.Output == QStringList{QStringLiteral("after")}), Describe(C.Result) + C.Output.join(QLatin1Char('|')));
  CHECK(DoList(T, Home).Ok);
}

// SSH settings applied by SecureShell (SessionData Connection/Proxy/Tunnel/SSH pages)
static void TestConnectionSettings(const SessionData & Data, TestUserInterface & UI, const QString & Fingerprint)
{
  auto TryOpen = [&UI](const SessionData & D, OperationResult * Result = nullptr) -> QString
  {
    UI.Reset();
    Terminal T(D, &UI);
    OperationResult R = DoOpen(T);
    if (Result != nullptr) *Result = R;
    QString Info = R.Ok ? T.ServerInfo() : QString();
    if (R.Ok)
    {
      // the session must work
      CHECK(DoList(T, T.CurrentDirectory()).Ok);
    }
    else
    {
      Log(QStringLiteral("open failed: %1").arg(Describe(R)));
    }
    return Info;
  };

  std::printf("[ssh: configured host key]\n");
  {
    SessionData D = Data;
    D.KnownHostsFile = QDir::temp().filePath(QStringLiteral("nixscp-none-%1").arg(QCoreApplication::applicationPid()));
    D.HostKey = QStringLiteral("ssh-ed25519 255 ") + Fingerprint;
    OperationResult R;
    TryOpen(D, &R);
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(UI.HostKeyCalls == 0);
    D.HostKey = QStringLiteral("ssh-ed25519 255 SHA256:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
    TryOpen(D, &R);
    CHECK_MSG(!R.Ok && R.Fatal && R.Message.startsWith(QStringLiteral("Host key does not match configured key fingerprint")), Describe(R));
    CHECK(UI.HostKeyCalls == 0);
    CHECK(!QFile::exists(D.KnownHostsFile));
  }

  std::printf("[ssh: cipher preference]\n");
  {
    SessionData D = Data;
    D.CipherList = QStringLiteral("aesgcm,aes,chacha20,3des,WARN,des,blowfish,arcfour");
    QString Info = TryOpen(D);
    CHECK_MSG(Info.contains(QStringLiteral("aes256-gcm@openssh.com")), Info);
    D.CipherList = QStringLiteral("aes,chacha20,aesgcm,3des,WARN,des,blowfish,arcfour");
    Info = TryOpen(D);
    CHECK_MSG(Info.contains(QStringLiteral("aes256-ctr")), Info);
    // WinSCP default prefers NTRU Prime and ML-KEM hybrids, libssh 0.11
    // implements neither, so curve25519 (from "ecdh") is used
    D = Data;
    Info = TryOpen(D);
    CHECK_MSG(Info.contains(QStringLiteral("Key exchange algorithm: curve25519-sha256")), Info);
  }

  std::printf("[ssh: address family]\n");
  {
    SessionData D = Data;
    D.AddressFamily = afIPv4;
    OperationResult R;
    TryOpen(D, &R);
    CHECK_MSG(R.Ok, Describe(R));
    if (D.HostName == QStringLiteral("127.0.0.1"))
    {
      D.AddressFamily = afIPv6;
      TryOpen(D, &R);
      CHECK_MSG(!R.Ok && R.Fatal, Describe(R));
    }
  }

  std::printf("[ssh: local proxy command]\n");
  if (qEnvironmentVariableIsEmpty("NIXSCP_TEST_PROXY"))
  {
    // libssh terminates the proxy process with kill() on disconnect and waits
    // for it; sandboxes that forbid signalling other programs make this hang
    Log(QStringLiteral("SKIPPED (set NIXSCP_TEST_PROXY=1 to run)"));
  }
  else
  {
    SessionData D = Data;
    D.ProxyMethod = pmCmd;
    D.ProxyLocalCommand = QStringLiteral("nc %host %port");
    OperationResult R;
    TryOpen(D, &R);
    CHECK_MSG(R.Ok, Describe(R));
  }

  std::printf("[ssh: tunnel]\n");
  {
    SessionData D = Data;
    D.Tunnel = true;
    D.TunnelHostName = Data.HostName;
    D.TunnelPortNumber = Data.PortNumber;
    D.TunnelUserName = Data.UserName;
    D.TunnelPublicKeyFile = Data.PublicKeyFile;
    OperationResult R;
    TryOpen(D, &R);
    CHECK_MSG(R.Ok, Describe(R));
    D.TunnelPublicKeyFile = QStringLiteral("/nonexistent-key");
    TryOpen(D, &R);
    CHECK_MSG(!R.Ok && R.Fatal, Describe(R));
  }
}

int main(int argc, char ** argv)
{
  QCoreApplication App(argc, argv);

  TestListingParser();
  TestShellQuote();

  QByteArray PortStr = qgetenv("NIXSCP_TEST_SSH_PORT");
  QString KeyFile = qEnvironmentVariable("NIXSCP_TEST_SSH_KEY");
  if (PortStr.isEmpty() || KeyFile.isEmpty())
  {
    std::printf("test_scp: server tests SKIPPED (set NIXSCP_TEST_SSH_PORT, NIXSCP_TEST_SSH_KEY)\n");
    std::printf((Failures == 0) ? "test_scp: all tests passed\n" : "test_scp: %d failures\n", Failures);
    return (Failures == 0) ? 0 : 1;
  }
  QString Host = qEnvironmentVariable("NIXSCP_TEST_SSH_HOST", QStringLiteral("127.0.0.1"));
  QString User = qEnvironmentVariable("NIXSCP_TEST_SSH_USER", qEnvironmentVariable("USER"));

  QTemporaryDir Scratch;
  CHECK(Scratch.isValid());
  QString KnownHosts = Scratch.filePath(QStringLiteral("dotssh/known_hosts"));

  SessionData Data;
  Data.HostName = Host;
  Data.PortNumber = PortStr.toInt();
  Data.UserName = User;
  Data.PublicKeyFile = KeyFile;
  Data.TryAgent = false;
  Data.AuthKI = false;
  Data.KnownHostsFile = KnownHosts;
  Data.FSProtocol = fsSCPonly;

  TestUserInterface UI;
  CopyParam Param;

  std::printf("[scp session]\n");
  Terminal T(Data, &UI);
  OperationResult R = DoOpen(T);
  CHECK_MSG(R.Ok, Describe(R));
  if (!R.Ok)
  {
    std::printf("test_scp: cannot connect, giving up\n");
    return 1;
  }
  CHECK(UI.HostKeyCalls == 1);
  CHECK(T.ProtocolName() == QStringLiteral("SCP"));
  Log(QStringLiteral("Server info: %1").arg(T.ServerInfo().replace(QLatin1Char('\n'), QStringLiteral("; "))));
  QString Home = T.CurrentDirectory();
  Log(QStringLiteral("Home: %1").arg(Home));
  CHECK(Home.startsWith(QLatin1Char('/')));
  QString Fingerprint = UI.LastFingerprint;
  TestConnectionSettings(Data, UI, Fingerprint);

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
  if (const RemoteFile * Bin = Find(Files, QStringLiteral("bin")))
  {
    Log(QStringLiteral("/bin: symlink=%1 dir=%2 -> %3").arg(Bin->IsSymLink).arg(Bin->IsDirectory).arg(Bin->LinkTo));
    if (Bin->IsSymLink)
    {
      CHECK(Bin->IsDirectory);
    }
  }
  R = DoList(T, QStringLiteral("."), &Files, &Dir);
  CHECK_MSG(R.Ok && (Dir == Home), Describe(R) + Dir);
  R = DoList(T, QStringLiteral("/nonexistent-nixscp"), &Files, &Dir);
  CHECK_MSG(!R.Ok && !R.Fatal, Describe(R));
  Log(QStringLiteral("list nonexistent: ") + Describe(R));

  QString Base = UnixPath::Combine(Home, QStringLiteral("nixscp-scp-test-%1").arg(QCoreApplication::applicationPid()));
  R = DoSimple([&](Terminal::Callback Cb) { T.CreateDirectory(Base, 0755, Cb); });
  CHECK_MSG(R.Ok, Describe(R));
  R = DoSimple([&](Terminal::Callback Cb) { T.CreateDirectory(Base, 0755, Cb); });
  CHECK_MSG(!R.Ok && !R.Fatal, Describe(R));
  Log(QStringLiteral("mkdir existing: ") + Describe(R));
  {
    // the error was offered as Retry / Skip / Abort (TRetryOperationLoop)
    QMutexLocker Lock(&UI.Mutex);
    CHECK((UI.Queries.size() == 1) && UI.Queries[0].startsWith(QStringLiteral("Error creating folder")));
    UI.Queries.clear();
  }
  bool LocalFs = QFileInfo(Base).isDir();
  Log(QStringLiteral("Server filesystem is local: %1").arg(LocalFs));

  // ---------------------------------------------------------------- local tree
  QTemporaryDir Work;
  QString Src = Work.filePath(QStringLiteral("src"));
  QString UnicodeName = QStringLiteral("file with spaces ünïcødé €$`\"'.txt");
  const qint64 BigSize = 8 * 1024 * 1024 + 123;
  QDir().mkpath(Src + QStringLiteral("/sub/deeper"));
  QDir().mkpath(Src + QStringLiteral("/emptydir"));
  const qint64 T0 = 1577934245; // 2020-01-02 03:04:05 UTC
  WriteFile(Src + QStringLiteral("/a.txt"), "hello\n", T0);
  WriteFile(Src + QStringLiteral("/empty.bin"), QByteArray(), T0 + 1);
  WriteFile(Src + QStringLiteral("/") + UnicodeName, "unicode\n", T0 + 2);
  WriteFile(Src + QStringLiteral("/.hidden"), "hidden\n", T0 + 3);
  WriteFile(Src + QStringLiteral("/sub/nested.txt"), RandomData(100000), T0 + 4);
  WriteFile(Src + QStringLiteral("/sub/deeper/x.dat"), RandomData(32768 * 3 + 1), T0 + 5);
  QString BigDir = Work.filePath(QStringLiteral("bigsrc"));
  QDir().mkpath(BigDir);
  QString BigLocal = BigDir + QStringLiteral("/big.bin");
  WriteFile(BigLocal, RandomData(BigSize), T0 + 6);

  std::printf("[scp upload tree]\n");
  R = DoSimple([&](Terminal::Callback Cb) { T.CopyToRemote(QStringList{Src}, Base, Param, false, Cb); });
  CHECK_MSG(R.Ok, Describe(R));
  CHECK(UI.Queries.isEmpty());
  {
    FileOperationProgress::Snapshot S = T.Progress().Get();
    CHECK(S.TotalTransferred == S.TotalSize);
    CHECK(S.TotalSize == 6 + 0 + 8 + 7 + 100000 + 32768 * 3 + 1);
  }
  QString RSrc = UnixPath::Combine(Base, QStringLiteral("src"));
  R = DoList(T, RSrc, &Files);
  CHECK_MSG(R.Ok, Describe(R));
  {
    CHECK(Files.size() == 6);
    const RemoteFile * F = Find(Files, QStringLiteral("a.txt"));
    CHECK(F && (F->Size == 6) && (F->Modification.toSecsSinceEpoch() == T0) && !F->IsDirectory && (F->Permissions == 0644));
    F = Find(Files, UnicodeName);
    CHECK(F && (F->Size == 8) && (F->Modification.toSecsSinceEpoch() == T0 + 2));
    F = Find(Files, QStringLiteral(".hidden"));
    CHECK(F && F->IsHidden);
    F = Find(Files, QStringLiteral("sub"));
    CHECK(F && F->IsDirectory);
    F = Find(Files, QStringLiteral("emptydir"));
    CHECK(F && F->IsDirectory);
  }
  if (LocalFs)
  {
    CompareTrees(Src, RSrc, true);
  }

  std::printf("[scp upload/download big file]\n");
  {
    QElapsedTimer Timer;
    Timer.start();
    R = DoSimple([&](Terminal::Callback Cb) { T.CopyToRemote(QStringList{BigLocal}, Base, Param, false, Cb); });
    CHECK_MSG(R.Ok, Describe(R));
    Log(QStringLiteral("upload %1 MB/s").arg((BigSize / 1048576.0) / (std::max<qint64>(1, Timer.elapsed()) / 1000.0), 0, 'f', 1));
  }
  QString RBig = UnixPath::Combine(Base, QStringLiteral("big.bin"));
  if (LocalFs)
  {
    CHECK(Sha256(RBig) == Sha256(BigLocal));
  }
  QString Dl = Work.filePath(QStringLiteral("dl"));
  QDir().mkpath(Dl);
  {
    QElapsedTimer Timer;
    Timer.start();
    R = DoSimple([&](Terminal::Callback Cb) { T.CopyToLocal(QStringList{RBig}, Dl, Param, false, Cb); });
    CHECK_MSG(R.Ok, Describe(R));
    Log(QStringLiteral("download %1 MB/s").arg((BigSize / 1048576.0) / (std::max<qint64>(1, Timer.elapsed()) / 1000.0), 0, 'f', 1));
    CHECK(Sha256(Dl + QStringLiteral("/big.bin")) == Sha256(BigLocal));
    CHECK(QFileInfo(Dl + QStringLiteral("/big.bin")).lastModified().toSecsSinceEpoch() == T0 + 6);
  }

  std::printf("[scp download tree]\n");
  R = DoSimple([&](Terminal::Callback Cb) { T.CopyToLocal(QStringList{RSrc}, Dl, Param, false, Cb); });
  CHECK_MSG(R.Ok, Describe(R));
  CompareTrees(Src, Dl + QStringLiteral("/src"), true);

  std::printf("[scp many small files]\n");
  {
    QString Many = Work.filePath(QStringLiteral("many"));
    QDir().mkpath(Many);
    const int Count = 50;
    for (int I = 0; I < Count; I++)
    {
      WriteFile(Many + QStringLiteral("/f%1.txt").arg(I), QByteArray::number(I).repeated(10), T0);
    }
    QElapsedTimer Timer;
    Timer.start();
    R = DoSimple([&](Terminal::Callback Cb) { T.CopyToRemote(QStringList{Many}, Base, Param, false, Cb); });
    CHECK_MSG(R.Ok, Describe(R));
    double Up = Count * 1000.0 / std::max<qint64>(1, Timer.elapsed());
    QString DlMany = Work.filePath(QStringLiteral("dlmany"));
    QDir().mkpath(DlMany);
    Timer.restart();
    R = DoSimple([&](Terminal::Callback Cb) { T.CopyToLocal(QStringList{UnixPath::Combine(Base, QStringLiteral("many"))}, DlMany, Param, false, Cb); });
    CHECK_MSG(R.Ok, Describe(R));
    double Down = Count * 1000.0 / std::max<qint64>(1, Timer.elapsed());
    CompareTrees(Many, DlMany + QStringLiteral("/many"), true);
    Log(QStringLiteral("small files: upload %1/s, download %2/s").arg(Up, 0, 'f', 0).arg(Down, 0, 'f', 0));
  }

  std::printf("[scp overwrite confirmation]\n");
  {
    QFile F(Src + QStringLiteral("/a.txt"));
    CHECK(F.open(QIODevice::Append));
    F.write("v2");
    F.close();
    UI.Reset();
    UI.QueryAnswers = {qaNo};
    R = DoSimple([&](Terminal::Callback Cb) { T.CopyToRemote(QStringList{Src + QStringLiteral("/a.txt")}, RSrc, Param, false, Cb); });
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(UI.Queries.size() == 1);
    DoList(T, RSrc, &Files);
    CHECK(Find(Files, QStringLiteral("a.txt")) && (Find(Files, QStringLiteral("a.txt"))->Size == 6));
    UI.Reset();
    UI.QueryAnswers = {qaYes};
    R = DoSimple([&](Terminal::Callback Cb) { T.CopyToRemote(QStringList{Src + QStringLiteral("/a.txt")}, RSrc, Param, false, Cb); });
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(UI.Queries.size() == 1);
    DoList(T, RSrc, &Files);
    CHECK(Find(Files, QStringLiteral("a.txt")) && (Find(Files, QStringLiteral("a.txt"))->Size == 8));
    UI.Reset();
    UI.QueryAnswers = {qaCancel};
    R = DoSimple([&](Terminal::Callback Cb) { T.CopyToLocal(QStringList{RSrc}, Dl, Param, false, Cb); });
    CHECK_MSG(!R.Ok && R.Aborted && !R.Fatal, Describe(R));
    UI.Reset();
    UI.QueryAnswers = {qaYesToAll};
    R = DoSimple([&](Terminal::Callback Cb) { T.CopyToLocal(QStringList{RSrc}, Dl, Param, false, Cb); });
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(UI.Queries.size() == 1);
    CompareTrees(Src, Dl + QStringLiteral("/src"), true);
  }

  std::printf("[scp rename/chmod/symlink/delete]\n");
  {
    R = DoSimple([&](Terminal::Callback Cb) { T.RenameFile(UnixPath::Combine(RSrc, QStringLiteral("a.txt")), UnixPath::Combine(RSrc, QStringLiteral("renamed ä $x.txt")), Cb); });
    CHECK_MSG(R.Ok, Describe(R));
    DoList(T, RSrc, &Files);
    CHECK(!Find(Files, QStringLiteral("a.txt")) && Find(Files, QStringLiteral("renamed ä $x.txt")));
    R = DoSimple([&](Terminal::Callback Cb) { T.RenameFile(UnixPath::Combine(RSrc, QStringLiteral("nonexistent")), UnixPath::Combine(RSrc, QStringLiteral("x")), Cb); });
    CHECK_MSG(!R.Ok && !R.Fatal, Describe(R));
    Log(QStringLiteral("rename nonexistent: ") + Describe(R));

    QString File = UnixPath::Combine(RSrc, QStringLiteral("renamed ä $x.txt"));
    R = DoSimple([&](Terminal::Callback Cb) { T.ChangeFileProperties(QStringList{File}, 0600, false, Cb); });
    CHECK_MSG(R.Ok, Describe(R));
    DoList(T, RSrc, &Files);
    CHECK(Find(Files, QStringLiteral("renamed ä $x.txt")) && (Find(Files, QStringLiteral("renamed ä $x.txt"))->Permissions == 0600));
    QString Sub = UnixPath::Combine(RSrc, QStringLiteral("sub"));
    // "Add X to directories" checked
    R = DoSimple([&](Terminal::Callback Cb) { T.ChangeFileProperties(QStringList{Sub}, 0640, true, Cb, true); });
    CHECK_MSG(R.Ok, Describe(R));
    if (LocalFs)
    {
      CHECK(LocalMode(Sub) == 0750);
      CHECK(LocalMode(Sub + QStringLiteral("/nested.txt")) == 0640);
    }

    QString Link = UnixPath::Combine(Base, QStringLiteral("link to sub"));
    R = DoSimple([&](Terminal::Callback Cb) { T.CreateLink(Link, Sub, Cb); });
    CHECK_MSG(R.Ok, Describe(R));
    QString FileLink = UnixPath::Combine(Base, QStringLiteral("filelink"));
    R = DoSimple([&](Terminal::Callback Cb) { T.CreateLink(FileLink, QStringLiteral("big.bin"), Cb); });
    CHECK_MSG(R.Ok, Describe(R));
    QString Broken = UnixPath::Combine(Base, QStringLiteral("broken"));
    R = DoSimple([&](Terminal::Callback Cb) { T.CreateLink(Broken, QStringLiteral("/nonexistent-nixscp"), Cb); });
    CHECK_MSG(R.Ok, Describe(R));
    R = DoList(T, Base, &Files);
    CHECK_MSG(R.Ok, Describe(R));
    const RemoteFile * L = Find(Files, QStringLiteral("link to sub"));
    CHECK(L && L->IsSymLink && L->IsDirectory && (L->LinkTo == Sub));
    L = Find(Files, QStringLiteral("filelink"));
    CHECK(L && L->IsSymLink && !L->IsDirectory && (L->LinkTo == QStringLiteral("big.bin")) && (L->Size == BigSize));
    L = Find(Files, QStringLiteral("broken"));
    CHECK(L && L->IsSymLink && !L->IsDirectory);
    // Recursive delete of a symlink to a directory removes the link only
    R = DoSimple([&](Terminal::Callback Cb) { T.DeleteFiles(QStringList{Link, Broken}, Cb); });
    CHECK_MSG(R.Ok, Describe(R));
    R = DoList(T, Sub, &Files);
    CHECK_MSG(R.Ok && (Files.size() == 2), Describe(R));
  }

  std::printf("[scp errors]\n");
  {
    R = DoSimple([&](Terminal::Callback Cb) { T.CopyToLocal(QStringList{UnixPath::Combine(Base, QStringLiteral("nonexistent"))}, Dl, Param, false, Cb); });
    CHECK_MSG(!R.Ok && !R.Fatal, Describe(R));
    Log(QStringLiteral("download nonexistent: ") + Describe(R));
    R = DoSimple([&](Terminal::Callback Cb) { T.CopyToRemote(QStringList{Src + QStringLiteral("/empty.bin")}, UnixPath::Combine(Base, QStringLiteral("nonexistent")), Param, false, Cb); });
    CHECK_MSG(!R.Ok && !R.Fatal, Describe(R));
    Log(QStringLiteral("upload to nonexistent dir: ") + Describe(R));
    R = DoList(T, Base, &Files);
    CHECK_MSG(R.Ok, Describe(R));
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
  CopyParam NoResume = Param;
  NoResume.ResumeSupport = CopyParam::rsOff;
  std::printf("[scp cancel download]\n");
  {
    QString Dl3 = Work.filePath(QStringLiteral("dl3"));
    QDir().mkpath(Dl3);
    qint64 At = -1;
    QPointer<QTimer> Timer = CancelWhenStarted(At);
    R = DoSimple([&](Terminal::Callback Cb) { T.CopyToLocal(QStringList{RBig}, Dl3, NoResume, false, Cb); });
    delete Timer.data();
    Log(QStringLiteral("cancelled at %1 of %2 bytes: %3").arg(At).arg(BigSize).arg(Describe(R)));
    if ((At >= 0) && (At < BigSize))
    {
      CHECK_MSG(!R.Ok && R.Aborted && !R.Fatal, Describe(R));
      CHECK(!QFile::exists(Dl3 + QStringLiteral("/big.bin")));
    }
    CHECK(T.Active());
    R = DoList(T, Base, &Files);
    CHECK_MSG(R.Ok, Describe(R));
    R = DoSimple([&](Terminal::Callback Cb) { T.CopyToLocal(QStringList{RBig}, Dl3, Param, false, Cb); });
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(Sha256(Dl3 + QStringLiteral("/big.bin")) == Sha256(BigLocal));
  }
  std::printf("[scp cancel upload]\n");
  {
    qint64 At = -1;
    QPointer<QTimer> Timer = CancelWhenStarted(At);
    UI.Reset();
    UI.QueryAnswers = {qaYes};
    R = DoSimple([&](Terminal::Callback Cb) { T.CopyToRemote(QStringList{BigLocal}, Base, NoResume, false, Cb); });
    delete Timer.data();
    Log(QStringLiteral("cancelled at %1 of %2 bytes: %3").arg(At).arg(BigSize).arg(Describe(R)));
    if ((At >= 0) && (At < BigSize))
    {
      CHECK_MSG(!R.Ok && R.Aborted && !R.Fatal, Describe(R));
    }
    R = DoList(T, Base, &Files);
    CHECK_MSG(R.Ok, Describe(R));
    if ((At >= 0) && (At < BigSize))
    {
      CHECK(!Find(Files, QStringLiteral("big.bin")));
    }
  }

  std::printf("[scp resume (full retransfer) and speed limit]\n");
  {
    CopyParam Resume = Param;
    Resume.ResumeSupport = CopyParam::rsOn;
    Resume.ConfirmResume = false;
    QString Dl5 = Work.filePath(QStringLiteral("dl5"));
    QDir().mkpath(Dl5);
    UI.Reset();
    R = DoSimple([&](Terminal::Callback Cb) { T.CopyToRemote(QStringList{BigLocal}, Base, Resume, false, Cb); });
    CHECK_MSG(R.Ok, Describe(R));
    // Partial local file from an earlier attempt
    QByteArray Big = RandomData(BigSize);
    WriteFile(Dl5 + QStringLiteral("/big.bin.filepart"), Big.left(1000000), T0);
    R = DoSimple([&](Terminal::Callback Cb) { T.CopyToLocal(QStringList{RBig}, Dl5, Resume, false, Cb); });
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(Sha256(Dl5 + QStringLiteral("/big.bin")) == Sha256(BigLocal));
    // SCP cannot resume (no fcResumeSupport, as WinSCP): no .filepart is used,
    // the one from the earlier attempt is left alone
    CHECK(QFileInfo(Dl5 + QStringLiteral("/big.bin.filepart")).size() == 1000000);
    FileOperationProgress::Snapshot S = T.Progress().Get();
    CHECK_MSG(S.TotalTransferred == BigSize, QString::number(S.TotalTransferred));

    QString Small = Work.filePath(QStringLiteral("limited.bin"));
    WriteFile(Small, RandomData(500 * 1024), T0);
    CopyParam Limited = Param;
    Limited.CPSLimit = 200 * 1024;
    QElapsedTimer Timer;
    Timer.start();
    R = DoSimple([&](Terminal::Callback Cb) { T.CopyToRemote(QStringList{Small}, Base, Limited, false, Cb); });
    qint64 Ms = Timer.elapsed();
    CHECK_MSG(R.Ok, Describe(R));
    CHECK_MSG(Ms >= 1000, QString::number(Ms));
    Log(QStringLiteral("500 KB at 200 KB/s: %1 ms").arg(Ms));
  }

  std::printf("[scp batch: file name case, per file errors, overwrite, move]\n");
  {
    auto ReadAll = [](const QString & FileName)
    {
      QFile F(FileName);
      return F.open(QIODevice::ReadOnly) ? F.readAll() : QByteArray("<missing>");
    };
    QString BSrc = Work.filePath(QStringLiteral("Batch"));
    QDir().mkpath(BSrc + QStringLiteral("/Sub"));
    WriteFile(BSrc + QStringLiteral("/One.TXT"), "one\n", T0);
    WriteFile(BSrc + QStringLiteral("/Sub/Two.txt"), "two\n", T0 + 1);
    WriteFile(BSrc + QStringLiteral("/Sub/Locked.txt"), "locked\n", T0 + 2);
    WriteFile(BSrc + QStringLiteral("/Three.txt"), "three\n", T0 + 3);
    QString Locked = BSrc + QStringLiteral("/Sub/Locked.txt");
    QFile::setPermissions(Locked, QFileDevice::Permissions());
    bool CanLock = !QFileInfo(Locked).isReadable();
    CopyParam Lower = Param;
    Lower.FileNameCase = CopyParam::ncLowerCase;
    UI.Reset();
    // the unreadable file: Retry (one by one after the batch), then Skip
    UI.QueryAnswers = {qaRetry, qaSkip};
    R = DoSimple([&](Terminal::Callback Cb) { T.CopyToRemote(QStringList{BSrc}, Base, Lower, false, Cb); });
    CHECK_MSG(R.Ok, Describe(R));
    QString RB = UnixPath::Combine(Base, QStringLiteral("batch"));
    if (CanLock)
    {
      QMutexLocker Lock(&UI.Mutex);
      CHECK_MSG((UI.Queries.size() == 2) && UI.Queries[0].startsWith(QStringLiteral("Can't open file")) &&
        UI.Queries[1].startsWith(QStringLiteral("Can't open file")), UI.Queries.join(QLatin1Char('|')));
      FileOperationProgress::Snapshot S = T.Progress().Get();
      CHECK_MSG(S.TotalTransferred == S.TotalSize, QStringLiteral("%1/%2").arg(S.TotalTransferred).arg(S.TotalSize));
    }
    R = DoList(T, RB, &Files);
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(Find(Files, QStringLiteral("one.txt")) && Find(Files, QStringLiteral("three.txt")) && Find(Files, QStringLiteral("sub")));
    R = DoList(T, UnixPath::Combine(RB, QStringLiteral("sub")), &Files);
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(Find(Files, QStringLiteral("two.txt")) && (Find(Files, QStringLiteral("two.txt"))->Modification.toSecsSinceEpoch() == T0 + 1));
    CHECK(!CanLock || !Find(Files, QStringLiteral("locked.txt")));

    // Overwrite: No for one.txt, Yes to all for the rest (two.txt, three.txt); locked.txt is new
    QFile::setPermissions(Locked, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    WriteFile(BSrc + QStringLiteral("/One.TXT"), "one v2\n", T0 + 10);
    WriteFile(BSrc + QStringLiteral("/Three.txt"), "three v2\n", T0 + 13);
    UI.Reset();
    UI.QueryAnswers = {qaNo, qaYesToAll};
    R = DoSimple([&](Terminal::Callback Cb) { T.CopyToRemote(QStringList{BSrc}, Base, Lower, false, Cb); });
    CHECK_MSG(R.Ok, Describe(R));
    {
      QMutexLocker Lock(&UI.Mutex);
      CHECK_MSG(UI.Queries.size() == 2, UI.Queries.join(QLatin1Char(0x7c)));
    }
    if (LocalFs)
    {
      CHECK(ReadAll(UnixPath::Combine(RB, QStringLiteral("one.txt"))) == "one\n");
      CHECK(ReadAll(UnixPath::Combine(RB, QStringLiteral("three.txt"))) == "three v2\n");
      CHECK(ReadAll(UnixPath::Combine(RB, QStringLiteral("sub/locked.txt"))) == "locked\n");
    }

    // Download skipping all existing files: the batch excludes them (no error)
    QString DlSkip = Work.filePath(QStringLiteral("dlskip"));
    QDir().mkpath(DlSkip);
    UI.Reset();
    R = DoSimple([&](Terminal::Callback Cb) { T.CopyToLocal(QStringList{RB}, DlSkip, Param, false, Cb); });
    CHECK_MSG(R.Ok, Describe(R));
    WriteFile(DlSkip + QStringLiteral("/batch/sub/two.txt"), "local two\n", T0);
    UI.QueryAnswers = {qaNoToAll};
    R = DoSimple([&](Terminal::Callback Cb) { T.CopyToLocal(QStringList{RB}, DlSkip, Param, false, Cb); });
    CHECK_MSG(R.Ok, Describe(R));
    {
      QMutexLocker Lock(&UI.Mutex);
      CHECK_MSG(UI.Queries.size() == 1, UI.Queries.join(QLatin1Char(0x7c)));
      FileOperationProgress::Snapshot S = T.Progress().Get();
      CHECK_MSG(S.TotalTransferred == S.TotalSize, QStringLiteral("%1/%2").arg(S.TotalTransferred).arg(S.TotalSize));
    }
    CHECK(ReadAll(DlSkip + QStringLiteral("/batch/sub/two.txt")) == "local two\n");

    // Move download: the transferred remote sources are deleted
    QString DlB = Work.filePath(QStringLiteral("dlbatch"));
    QDir().mkpath(DlB);
    UI.Reset();
    R = DoSimple([&](Terminal::Callback Cb) { T.CopyToLocal(QStringList{RB}, DlB, Param, true, Cb); });
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(UI.Queries.isEmpty());
    CHECK(ReadAll(DlB + QStringLiteral("/batch/sub/two.txt")) == "two\n");
    CHECK(ReadAll(DlB + QStringLiteral("/batch/three.txt")) == "three v2\n");
    CHECK(QFileInfo(DlB + QStringLiteral("/batch/sub/two.txt")).lastModified().toSecsSinceEpoch() == T0 + 1);
    R = DoList(T, Base, &Files);
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(!Find(Files, QStringLiteral("batch")));

    // Move upload: the local sources are deleted
    UI.Reset();
    R = DoSimple([&](Terminal::Callback Cb) { T.CopyToRemote(QStringList{DlB + QStringLiteral("/batch")}, Base, Param, true, Cb); });
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(!QFileInfo::exists(DlB + QStringLiteral("/batch")));
    R = DoList(T, UnixPath::Combine(RB, QStringLiteral("sub")), &Files);
    CHECK_MSG(R.Ok && Find(Files, QStringLiteral("two.txt")), Describe(R));
  }

  std::printf("[text mode EOL conversion]\n");
  {
    QString TSrc = Work.filePath(QStringLiteral("text"));
    QDir().mkpath(TSrc);
    const QByteArray Content("\xEF\xBB\xBFline1\nline2\r\nline3\n");
    WriteFile(TSrc + QStringLiteral("/t.txt"), Content, T0);
    WriteFile(TSrc + QStringLiteral("/b.bin"), "bin\n", T0);
    CopyParam Text = Param;
    Text.Mode = CopyParam::tmAutomatic;
    Text.AsciiFileMask = FileMasks(QStringLiteral("*.txt"));
    Text.RemoveBOM = true;
    for (TFSProtocol Protocol : {fsSCPonly, fsSFTPonly})
    {
      SessionData CrLf = Data;
      CrLf.EOLType = eolCRLF;
      CrLf.FSProtocol = Protocol;
      Terminal T3(CrLf, &UI);
      R = DoOpen(T3);
      CHECK_MSG(R.Ok, Describe(R));
      if (!R.Ok)
      {
        continue;
      }
      QString RText = UnixPath::Combine(Base, QStringLiteral("text"));
      UI.Reset();
      UI.QueryAnswers = {qaYesToAll};
      R = DoSimple([&](Terminal::Callback Cb) { T3.CopyToRemote(QStringList{TSrc}, Base, Text, false, Cb); });
      CHECK_MSG(R.Ok, Describe(R));
      FileOperationProgress::Snapshot S = T3.Progress().Get();
      CHECK_MSG(S.TotalTransferred == S.TotalSize, QStringLiteral("%1/%2").arg(S.TotalTransferred).arg(S.TotalSize));
      if (LocalFs)
      {
        QFile F(UnixPath::Combine(RText, QStringLiteral("t.txt")));
        CHECK(F.open(QIODevice::ReadOnly) && (F.readAll() == "line1\r\nline2\r\nline3\r\n"));
        QFile B(UnixPath::Combine(RText, QStringLiteral("b.bin")));
        CHECK(B.open(QIODevice::ReadOnly) && (B.readAll() == "bin\n"));
      }
      QString DlT = Work.filePath(QStringLiteral("dltext-%1").arg(static_cast<int>(Protocol)));
      QDir().mkpath(DlT);
      R = DoSimple([&](Terminal::Callback Cb) { T3.CopyToLocal(QStringList{RText}, DlT, Text, false, Cb); });
      CHECK_MSG(R.Ok, Describe(R));
      QFile F(DlT + QStringLiteral("/text/t.txt"));
      CHECK(F.open(QIODevice::ReadOnly) && (F.readAll() == "line1\nline2\nline3\n"));
      QFile B(DlT + QStringLiteral("/text/b.bin"));
      CHECK(B.open(QIODevice::ReadOnly) && (B.readAll() == "bin\n"));
      T3.Close();
    }
  }

  TestConsole(T, Home, Base, "scp");

  // Console on an SFTP session
  {
    SessionData SftpData = Data;
    SftpData.FSProtocol = fsSFTP;
    Terminal T2(SftpData, &UI);
    R = DoOpen(T2);
    CHECK_MSG(R.Ok, Describe(R));
    if (R.Ok)
    {
      TestConsole(T2, T2.CurrentDirectory(), Base, "sftp");
      // The SFTP session still works after the commands
      CHECK(DoList(T2, Base).Ok);
    }
  }

  std::printf("[scp delete recursive]\n");
  {
    R = DoSimple([&](Terminal::Callback Cb) { T.DeleteFiles(QStringList{Base}, Cb); });
    CHECK_MSG(R.Ok, Describe(R));
    DoList(T, Home, &Files);
    CHECK(!Find(Files, UnixPath::ExtractFileName(Base)));
  }

  std::printf("[scp close]\n");
  T.Close();
  CHECK(!T.Active());

  if (Failures == 0)
  {
    std::printf("test_scp: all tests passed\n");
    return 0;
  }
  std::printf("test_scp: %d failures\n", Failures);
  return 1;
}
