// Local file operations engine (WP18): src/core/LocalFileOperations - the
// local copy/move used by drag & drop within/between local panels and local
// paste, and local Properties (chmod/chown). Checks: preserved timestamps,
// permissions and symbolic links, overwrite prompts (No keeps the target,
// Cancel never destroys it), atomic replace, rename(2) moves within a file
// system (same inode), copy+delete moves across file systems, copy into itself,
// "name (copy)" duplicates, Retry/Skip of errors and recursive chmod.
#include "core/FileSystem.h"
#include "core/LocalFileOperations.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <cstdio>
#include <functional>
#include <thread>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static int Failures = 0;
static int Checks = 0;

#define CHECK(Cond) \
  do { Checks++; if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); std::fflush(stdout); Failures++; } } while (0)

class FakeUI : public UserInterface
{
public:
  QList<unsigned int> Answers;
  QStringList Queries;
  HostKeyAnswer VerifyHostKey(const QString &, int, const QString &, const QString &, const QString &, HostKeyState) override
  {
    return HostKeyCancel;
  }
  bool PromptUser(PromptKind, const QString &, const QString &, QList<Prompt> &) override { return false; }
  void DisplayBanner(const QString &, const QString &) override {}
  unsigned int QueryUser(const QString & Query, const QString &, unsigned int) override
  {
    Queries.append(Query);
    return Answers.isEmpty() ? qaCancel : Answers.takeFirst();
  }
  unsigned int QueryUserEx(const QString & Query, const QString & More, unsigned int Answers_, const QueryUserParams &) override
  {
    return QueryUser(Query, More, Answers_);
  }
};

static void WriteFile(const QString & Path, const QByteArray & Data)
{
  QFile F(Path);
  if (!F.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
  F.write(Data);
}

static QByteArray ReadFile(const QString & Path)
{
  QFile F(Path);
  if (!F.open(QIODevice::ReadOnly)) return QByteArray();
  return F.readAll();
}

static void SetTime(const QString & Path, time_t Sec)
{
  struct timespec Times[2] = {{Sec, 0}, {Sec, 0}};
  utimensat(AT_FDCWD, QFile::encodeName(Path).constData(), Times, AT_SYMLINK_NOFOLLOW);
}

static struct stat LStat(const QString & Path)
{
  struct stat St = {};
  lstat(QFile::encodeName(Path).constData(), &St);
  return St;
}

static bool Exists(const QString & Path)
{
  struct stat St;
  return lstat(QFile::encodeName(Path).constData(), &St) == 0;
}

static int TempFilesIn(const QString & Dir)
{
  int Count = 0;
  for (const QString & Name : QDir(Dir).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot))
  {
    if (Name.contains(QStringLiteral(".nixscp-")))
    {
      Count++;
    }
  }
  return Count;
}

int main(int argc, char ** argv)
{
  QCoreApplication App(argc, argv);
  QTemporaryDir Root;
  CHECK(Root.isValid());
  QString Src = Root.filePath(QStringLiteral("src"));
  QString Dst = Root.filePath(QStringLiteral("dst"));
  QDir().mkpath(Src + QStringLiteral("/sub/deep"));
  QDir().mkpath(Dst);

  // ---- Copy a tree: content, mtime, mode, symlinks
  QByteArray Big(3 * 1024 * 1024 + 17, 'x');
  for (int I = 0; I < Big.size(); I += 4096) Big[I] = static_cast<char>(I % 251);
  WriteFile(Src + QStringLiteral("/big.bin"), Big);
  WriteFile(Src + QStringLiteral("/sub/a.txt"), "hello");
  WriteFile(Src + QStringLiteral("/sub/deep/run.sh"), "#!/bin/sh\n");
  chmod(QFile::encodeName(Src + QStringLiteral("/sub/deep/run.sh")).constData(), 0750);
  CHECK(symlink("a.txt", QFile::encodeName(Src + QStringLiteral("/sub/link")).constData()) == 0);
  CHECK(symlink("/nonexistent/target", QFile::encodeName(Src + QStringLiteral("/sub/dangling")).constData()) == 0);
  SetTime(Src + QStringLiteral("/big.bin"), 1000000000);
  SetTime(Src + QStringLiteral("/sub/a.txt"), 1100000000);
  SetTime(Src + QStringLiteral("/sub/link"), 1200000000);
  SetTime(Src + QStringLiteral("/sub/deep"), 1300000000);
  SetTime(Src + QStringLiteral("/sub"), 1400000000);

  {
    FileOperationProgress Progress;
    LocalFileOperations::Params P;
    P.Op = LocalFileOperations::opCopy;
    P.Sources = {Src + QStringLiteral("/big.bin"), Src + QStringLiteral("/sub")};
    P.TargetDirectory = Dst;
    FakeUI UI;
    LocalOperationResult R = LocalFileOperations::Execute(P, &Progress, &UI);
    CHECK(R.Ok);
    CHECK(R.Processed == 2);
    CHECK(UI.Queries.isEmpty());
    CHECK(ReadFile(Dst + QStringLiteral("/big.bin")) == Big);
    CHECK(LStat(Dst + QStringLiteral("/big.bin")).st_mtim.tv_sec == 1000000000);
    CHECK(ReadFile(Dst + QStringLiteral("/sub/a.txt")) == "hello");
    CHECK(LStat(Dst + QStringLiteral("/sub/a.txt")).st_mtim.tv_sec == 1100000000);
    CHECK((LStat(Dst + QStringLiteral("/sub/deep/run.sh")).st_mode & 07777) == 0750);
    CHECK(S_ISLNK(LStat(Dst + QStringLiteral("/sub/link")).st_mode));
    CHECK(QFileInfo(Dst + QStringLiteral("/sub/link")).symLinkTarget().endsWith(QStringLiteral("/sub/a.txt")));
    CHECK(LStat(Dst + QStringLiteral("/sub/link")).st_mtim.tv_sec == 1200000000);
    CHECK(S_ISLNK(LStat(Dst + QStringLiteral("/sub/dangling")).st_mode));
    CHECK(LStat(Dst + QStringLiteral("/sub/deep")).st_mtim.tv_sec == 1300000000);
    CHECK(LStat(Dst + QStringLiteral("/sub")).st_mtim.tv_sec == 1400000000);
    FileOperationProgress::Snapshot S = Progress.Get();
    CHECK(S.TotalSize == Big.size() + 5 + 10);
    CHECK(S.TotalTransferred == S.TotalSize);
    CHECK(S.FilesFinished == 5);
    CHECK(!S.InProgress);
    CHECK(TempFilesIn(Dst) == 0);
    CHECK(TempFilesIn(Dst + QStringLiteral("/sub")) == 0);
  }

  // ---- Overwrite: No keeps the target, Cancel aborts without touching it, Yes replaces
  {
    WriteFile(Dst + QStringLiteral("/big.bin"), "old content");
    LocalFileOperations::Params P;
    P.Sources = {Src + QStringLiteral("/big.bin")};
    P.TargetDirectory = Dst;
    FakeUI UI;
    UI.Answers = {qaNo};
    LocalOperationResult R = LocalFileOperations::Execute(P, nullptr, &UI);
    CHECK(R.Ok);
    CHECK(R.Skipped == 1);
    CHECK(UI.Queries.size() == 1);
    CHECK(UI.Queries.value(0).contains(QStringLiteral("File 'big.bin' already exists. Overwrite?")));
    CHECK(ReadFile(Dst + QStringLiteral("/big.bin")) == "old content");

    UI.Answers = {qaCancel};
    R = LocalFileOperations::Execute(P, nullptr, &UI);
    CHECK(!R.Ok && R.Aborted);
    CHECK(ReadFile(Dst + QStringLiteral("/big.bin")) == "old content");

    ino_t OldInode = LStat(Dst + QStringLiteral("/big.bin")).st_ino;
    UI.Answers = {qaYes};
    R = LocalFileOperations::Execute(P, nullptr, &UI);
    CHECK(R.Ok);
    CHECK(ReadFile(Dst + QStringLiteral("/big.bin")) == Big);
    // replaced by rename of the completed temporary file
    CHECK(LStat(Dst + QStringLiteral("/big.bin")).st_ino != OldInode);
    CHECK(TempFilesIn(Dst) == 0);

    // Yes to all for a directory merge (directory + files inside)
    WriteFile(Dst + QStringLiteral("/sub/a.txt"), "changed");
    UI.Answers = {qaYesToAll};
    UI.Queries.clear();
    P.Sources = {Src + QStringLiteral("/sub")};
    R = LocalFileOperations::Execute(P, nullptr, &UI);
    CHECK(R.Ok);
    CHECK(UI.Queries.size() == 1);
    CHECK(ReadFile(Dst + QStringLiteral("/sub/a.txt")) == "hello");

    // no confirmation
    WriteFile(Dst + QStringLiteral("/sub/a.txt"), "changed");
    P.ConfirmOverwrite = false;
    UI.Queries.clear();
    R = LocalFileOperations::Execute(P, nullptr, &UI);
    CHECK(R.Ok && UI.Queries.isEmpty());
    CHECK(ReadFile(Dst + QStringLiteral("/sub/a.txt")) == "hello");
  }

  // ---- Cancel during copy: the existing target is intact, no temporary left
  {
    WriteFile(Dst + QStringLiteral("/big.bin"), "keep me");
    LocalFileOperations::Params P;
    P.TargetDirectory = Dst;
    P.ConfirmOverwrite = false;
    P.UseReflink = false;
    LocalOperationResult R;
    FileOperationProgress Progress2;
    std::thread Canceller([&Progress2]()
    {
      while (Progress2.Get().FileName.isEmpty()) usleep(100);
      Progress2.Cancel();
    });
    // a larger file so the copy is still running when cancelled
    QByteArray Huge(64 * 1024 * 1024, 'h');
    WriteFile(Src + QStringLiteral("/huge.bin"), Huge);
    WriteFile(Dst + QStringLiteral("/huge.bin"), "keep me");
    P.Sources = {Src + QStringLiteral("/huge.bin")};
    R = LocalFileOperations::Execute(P, &Progress2, nullptr);
    Canceller.join();
    if (R.Aborted)
    {
      CHECK(ReadFile(Dst + QStringLiteral("/huge.bin")) == "keep me");
    }
    else
    {
      // the kernel was faster than the canceller: the copy is complete
      CHECK(ReadFile(Dst + QStringLiteral("/huge.bin")) == Huge);
    }
    CHECK(TempFilesIn(Dst) == 0);
    QFile::remove(Src + QStringLiteral("/huge.bin"));
    QFile::remove(Dst + QStringLiteral("/huge.bin"));
  }

  // ---- Copy into itself, duplicate in the same directory
  {
    LocalFileOperations::Params P;
    P.Sources = {Src + QStringLiteral("/sub")};
    P.TargetDirectory = Src + QStringLiteral("/sub/deep");
    LocalOperationResult R = LocalFileOperations::Execute(P, nullptr, nullptr);
    CHECK(!R.Ok);
    CHECK(R.Message.contains(QStringLiteral("into itself")));
    CHECK(!Exists(Src + QStringLiteral("/sub/deep/sub")));

    CHECK(LocalFileOperations::IsSubPath(QStringLiteral("/a/b"), QStringLiteral("/a/b/c")));
    CHECK(LocalFileOperations::IsSubPath(QStringLiteral("/a/b"), QStringLiteral("/a/b")));
    CHECK(!LocalFileOperations::IsSubPath(QStringLiteral("/a/b"), QStringLiteral("/a/bc")));
    CHECK(LocalFileOperations::IsSubPath(QStringLiteral("/"), QStringLiteral("/x")));

    P.Sources = {Src + QStringLiteral("/sub/a.txt")};
    P.TargetDirectory = Src + QStringLiteral("/sub");
    R = LocalFileOperations::Execute(P, nullptr, nullptr);
    CHECK(R.Ok && (R.Processed == 0));   // drop onto itself: nothing to do
    P.DuplicateIfSame = true;
    R = LocalFileOperations::Execute(P, nullptr, nullptr);
    CHECK(R.Ok && (R.Processed == 1));
    CHECK(ReadFile(Src + QStringLiteral("/sub/a (copy).txt")) == "hello");
    R = LocalFileOperations::Execute(P, nullptr, nullptr);
    CHECK(ReadFile(Src + QStringLiteral("/sub/a (copy 2).txt")) == "hello");
    CHECK(LocalFileOperations::DuplicateName(Src, QStringLiteral("x.tar.gz")) == QStringLiteral("x (copy).tar.gz"));
    CHECK(LocalFileOperations::DuplicateName(Src, QStringLiteral(".bashrc")) == QStringLiteral(".bashrc (copy)"));
    QFile::remove(Src + QStringLiteral("/sub/a (copy).txt"));
    QFile::remove(Src + QStringLiteral("/sub/a (copy 2).txt"));
  }

  // ---- Move within the file system: rename (same inode), merge into existing directory
  {
    QString M = Root.filePath(QStringLiteral("move"));
    QDir().mkpath(M + QStringLiteral("/from/dir"));
    QDir().mkpath(M + QStringLiteral("/to/dir"));
    WriteFile(M + QStringLiteral("/from/f.txt"), "f");
    WriteFile(M + QStringLiteral("/from/dir/inner.txt"), "inner");
    WriteFile(M + QStringLiteral("/to/dir/existing.txt"), "existing");
    ino_t Inode = LStat(M + QStringLiteral("/from/f.txt")).st_ino;
    LocalFileOperations::Params P;
    P.Op = LocalFileOperations::opMove;
    P.Sources = {M + QStringLiteral("/from/f.txt"), M + QStringLiteral("/from/dir")};
    P.TargetDirectory = M + QStringLiteral("/to");
    FakeUI UI;
    UI.Answers = {qaYes};
    LocalOperationResult R = LocalFileOperations::Execute(P, nullptr, &UI);
    CHECK(R.Ok && (R.Processed == 2));
    CHECK(LStat(M + QStringLiteral("/to/f.txt")).st_ino == Inode);
    CHECK(!Exists(M + QStringLiteral("/from/f.txt")));
    CHECK(ReadFile(M + QStringLiteral("/to/dir/inner.txt")) == "inner");
    CHECK(ReadFile(M + QStringLiteral("/to/dir/existing.txt")) == "existing");
    CHECK(!Exists(M + QStringLiteral("/from/dir")));
  }

  // ---- Move across file systems (/dev/shm is tmpfs): copy + delete source
  {
    QTemporaryDir Shm(QStringLiteral("/dev/shm/nixscp-test-XXXXXX"));
    struct stat A, B;
    if (Shm.isValid() && (stat(QFile::encodeName(Shm.path()).constData(), &A) == 0) &&
        (stat(QFile::encodeName(Root.path()).constData(), &B) == 0) && (A.st_dev != B.st_dev))
    {
      QDir().mkpath(Shm.path() + QStringLiteral("/tree/x"));
      WriteFile(Shm.path() + QStringLiteral("/tree/x/y.txt"), "yyy");
      SetTime(Shm.path() + QStringLiteral("/tree/x/y.txt"), 1500000000);
      CHECK(symlink("x/y.txt", QFile::encodeName(Shm.path() + QStringLiteral("/tree/l")).constData()) == 0);
      LocalFileOperations::Params P;
      P.Op = LocalFileOperations::opMove;
      P.Sources = {Shm.path() + QStringLiteral("/tree")};
      P.TargetDirectory = Dst;
      LocalOperationResult R = LocalFileOperations::Execute(P, nullptr, nullptr);
      CHECK(R.Ok);
      CHECK(ReadFile(Dst + QStringLiteral("/tree/x/y.txt")) == "yyy");
      CHECK(LStat(Dst + QStringLiteral("/tree/x/y.txt")).st_mtim.tv_sec == 1500000000);
      CHECK(S_ISLNK(LStat(Dst + QStringLiteral("/tree/l")).st_mode));
      CHECK(!Exists(Shm.path() + QStringLiteral("/tree")));
    }
    else
    {
      std::printf("SKIP: cross file system move (no separate tmpfs)\n");
    }
  }

  // ---- Errors: unreadable file, Skip continues, no UI aborts
  if (geteuid() != 0)
  {
    QString E = Root.filePath(QStringLiteral("err"));
    QDir().mkpath(E + QStringLiteral("/in"));
    QDir().mkpath(E + QStringLiteral("/out"));
    WriteFile(E + QStringLiteral("/in/secret"), "s");
    WriteFile(E + QStringLiteral("/in/public"), "p");
    chmod(QFile::encodeName(E + QStringLiteral("/in/secret")).constData(), 0);
    LocalFileOperations::Params P;
    P.Sources = {E + QStringLiteral("/in")};
    P.TargetDirectory = E + QStringLiteral("/out");
    FakeUI UI;
    UI.Answers = {qaRetry, qaSkip};
    LocalOperationResult R = LocalFileOperations::Execute(P, nullptr, &UI);
    CHECK(R.Ok);
    CHECK(UI.Queries.size() == 2);
    CHECK(UI.Queries.value(0).contains(QStringLiteral("secret")));
    CHECK(R.Skipped == 1);
    CHECK(ReadFile(E + QStringLiteral("/out/in/public")) == "p");
    CHECK(!Exists(E + QStringLiteral("/out/in/secret")));
    QDir(E + QStringLiteral("/out")).removeRecursively();
    QDir().mkpath(E + QStringLiteral("/out"));
    R = LocalFileOperations::Execute(P, nullptr, nullptr);
    CHECK(!R.Ok && !R.Aborted);
    CHECK(R.Message.contains(QStringLiteral("secret")));
    // a move whose copy failed keeps the source
    chmod(QFile::encodeName(E + QStringLiteral("/in/secret")).constData(), 0600);
  }

  // ---- Properties: recursive chmod with Add X to directories, tri-state masks, owner
  {
    QString D = Root.filePath(QStringLiteral("props"));
    QDir().mkpath(D + QStringLiteral("/d/e"));
    WriteFile(D + QStringLiteral("/d/f"), "f");
    WriteFile(D + QStringLiteral("/d/e/g"), "g");
    chmod(QFile::encodeName(D + QStringLiteral("/d/e/g")).constData(), 0755);
    LocalFileOperations::PropertiesParams P;
    P.Paths = {D + QStringLiteral("/d")};
    P.ChangeMode = true;
    P.Mode = 0644;
    P.AddXToDirectories = true;
    P.Recursive = true;
    LocalOperationResult R = LocalFileOperations::ChangeProperties(P, nullptr, nullptr);
    CHECK(R.Ok);
    CHECK((LStat(D + QStringLiteral("/d")).st_mode & 07777) == 0755);
    CHECK((LStat(D + QStringLiteral("/d/e")).st_mode & 07777) == 0755);
    CHECK((LStat(D + QStringLiteral("/d/f")).st_mode & 07777) == 0644);
    CHECK((LStat(D + QStringLiteral("/d/e/g")).st_mode & 07777) == 0644);

    chmod(QFile::encodeName(D + QStringLiteral("/d/e/g")).constData(), 0755);
    P.Paths = {D + QStringLiteral("/d/f"), D + QStringLiteral("/d/e/g")};
    P.Recursive = false;
    P.ModeMasks = true;
    P.SetMask = 0002;
    P.ClearMask = 0040;
    R = LocalFileOperations::ChangeProperties(P, nullptr, nullptr);
    CHECK(R.Ok);
    CHECK((LStat(D + QStringLiteral("/d/f")).st_mode & 07777) == 0606);
    CHECK((LStat(D + QStringLiteral("/d/e/g")).st_mode & 07777) == 0717);

    LocalFileOperations::PropertiesParams O;
    O.Paths = {D + QStringLiteral("/d/f")};
    O.Group = LocalFileOperations::GroupName(getegid());
    R = LocalFileOperations::ChangeProperties(O, nullptr, nullptr);
    CHECK(R.Ok);
    O.Owner = QStringLiteral("no-such-user-nixscp");
    R = LocalFileOperations::ChangeProperties(O, nullptr, nullptr);
    CHECK(!R.Ok && R.Message.contains(QStringLiteral("does not exist")));
    CHECK(LocalFileOperations::UserNames().contains(LocalFileOperations::OwnerName(geteuid())));
    CHECK(LocalFileOperations::GroupNames().contains(LocalFileOperations::GroupName(getegid())));
  }

  std::printf("%s: %d checks, %d failures\n", Failures ? "FAILED" : "OK", Checks, Failures);
  return Failures ? 1 : 0;
}
