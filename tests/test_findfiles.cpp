// Tests of core/FileFind (local + remote Find Files) and core/Bookmarks.
//
// The remote part is skipped unless NIXSCP_TEST_SSH_PORT and NIXSCP_TEST_SSH_KEY
// are set (see tests/test_sftp.cpp); it assumes the server runs on this machine
// (searches the same temporary tree remotely). A temporary known_hosts file is
// used, ~/.ssh is never touched.
#include "core/Bookmarks.h"
#include "core/FileFind.h"
#include "core/IniStorage.h"
#include "core/SessionData.h"
#include "core/Terminal.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QHostInfo>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>
#include <sys/time.h>
#include <unistd.h>

static int Failures = 0;

#define CHECK(Cond) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); std::fflush(stdout); Failures++; } } while (0)
#define CHECK_EQ(A, B) \
  do { auto VA = (A); auto VB = (B); if (!(VA == VB)) { std::printf("FAIL %s:%d: %s == %s (\"%s\" vs \"%s\")\n", __FILE__, __LINE__, #A, #B, \
    qPrintable(QVariant::fromValue(VA).toString()), qPrintable(QVariant::fromValue(VB).toString())); std::fflush(stdout); Failures++; } } while (0)

static void WriteFile(const QString & Path, qint64 Size, int AgeDays = 0)
{
  QDir().mkpath(QFileInfo(Path).absolutePath());
  QFile File(Path);
  CHECK(File.open(QIODevice::WriteOnly));
  File.write(QByteArray(Size, 'x'));
  File.close();
  if (AgeDays > 0)
  {
    struct timeval Times[2];
    time_t T = time(nullptr) - static_cast<time_t>(AgeDays) * 86400;
    Times[0].tv_sec = T;
    Times[0].tv_usec = 0;
    Times[1] = Times[0];
    utimes(QFile::encodeName(Path).constData(), Times);
  }
}

// Relative paths of the results, sorted ("sub/b.txt", "sub/" for directories)
static QStringList Names(const QList<FoundFile> & Files, const QString & Root)
{
  QStringList Result;
  for (const FoundFile & F : Files)
  {
    QString Rel = F.FullPath.mid(Root.size() + 1);
    if (F.File.IsDirectory)
    {
      Rel += QLatin1Char('/');
    }
    Result.append(Rel);
  }
  Result.sort();
  return Result;
}

class NullUserInterface : public UserInterface
{
public:
  HostKeyAnswer VerifyHostKey(const QString &, int, const QString &, const QString &, const QString &, HostKeyState) override
  {
    return HostKeyOnce;
  }
  bool PromptUser(PromptKind, const QString &, const QString &, QList<Prompt> &) override { return false; }
  void DisplayBanner(const QString &, const QString &) override {}
  unsigned int QueryUser(const QString &, const QString &, unsigned int) override { return qaCancel; }
};

struct AsyncResult
{
  QList<FoundFile> Files;
  QStringList Directories;
  bool Finished = false;
  bool Aborted = false;
  QString Error;
  int Batches = 0;
};

static AsyncResult RunAsync(FileFind & Find, Terminal * Term, const QString & Dir, const QString & Mask,
  int StopAfterFiles = -1)
{
  AsyncResult R;
  QEventLoop Loop;
  QObject Context;
  QObject::connect(&Find, &FileFind::FilesFound, &Context, [&](const QList<FoundFile> & Files)
  {
    CHECK(QThread::currentThread() == QCoreApplication::instance()->thread());
    R.Files.append(Files);
    R.Batches++;
    if ((StopAfterFiles >= 0) && (R.Files.size() >= StopAfterFiles))
    {
      Find.Stop();
    }
  });
  QObject::connect(&Find, &FileFind::SearchingDirectory, &Context, [&](const QString & D) { R.Directories.append(D); });
  QObject::connect(&Find, &FileFind::Finished, &Context, [&](bool Aborted, const QString & Error)
  {
    R.Finished = true;
    R.Aborted = Aborted;
    R.Error = Error;
    Loop.quit();
  });
  QString StartError;
  bool Started = Find.Start(Term, Dir, Mask, &StartError);
  CHECK(Started);
  if (Started && (StopAfterFiles == 0))
  {
    Find.Stop();
  }
  if (Started)
  {
    QTimer::singleShot(120000, &Loop, [&]() { std::printf("FAIL: search timed out\n"); Failures++; Loop.quit(); });
    Loop.exec();
  }
  return R;
}

static void BuildTree(const QString & Root)
{
  WriteFile(Root + QStringLiteral("/a.txt"), 10);
  WriteFile(Root + QStringLiteral("/big.txt"), 20000);
  WriteFile(Root + QStringLiteral("/old.log"), 5, 400);
  WriteFile(Root + QStringLiteral("/README"), 1);
  WriteFile(Root + QStringLiteral("/sub/b.TXT"), 100);
  WriteFile(Root + QStringLiteral("/sub/c.dat"), 100);
  WriteFile(Root + QStringLiteral("/sub/deep/d.txt"), 3000);
  WriteFile(Root + QStringLiteral("/.git/config.txt"), 1);
  WriteFile(Root + QStringLiteral("/node_modules/x/y.txt"), 1);
  QDir().mkpath(Root + QStringLiteral("/empty"));
  // symlink loop: sub/deep/loop -> ../.. (= Root/sub's parent)
  CHECK(::symlink("../..", QFile::encodeName(Root + QStringLiteral("/sub/deep/loop")).constData()) == 0);
}

static void TestLocal(const QString & Root)
{
  std::printf("[local FindLocal]\n");
  QStringList Errors;
  QStringList All = Names(FileFind::FindLocal(Root, QStringLiteral("*.txt"), &Errors), Root);
  CHECK_EQ(All.join(QLatin1Char(',')),
    QStringLiteral(".git/config.txt,a.txt,big.txt,node_modules/x/y.txt,sub/b.TXT,sub/deep/d.txt"));
  CHECK(Errors.isEmpty());

  // size condition
  CHECK_EQ(Names(FileFind::FindLocal(Root, QStringLiteral("*.txt>1K"), nullptr), Root).join(QLatin1Char(',')),
    QStringLiteral("big.txt,sub/deep/d.txt"));
  // time condition: older than 100 days
  CHECK_EQ(Names(FileFind::FindLocal(Root, QStringLiteral("<100D"), nullptr), Root).join(QLatin1Char(',')),
    QStringLiteral("old.log"));
  // files without extension
  CHECK_EQ(Names(FileFind::FindLocal(Root, QStringLiteral("*."), nullptr), Root).join(QLatin1Char(',')),
    QStringLiteral("README"));
  // exclude directories: not descended into
  CHECK_EQ(Names(FileFind::FindLocal(Root, QStringLiteral("*.txt | .git/; node_modules/"), nullptr), Root).join(QLatin1Char(',')),
    QStringLiteral("a.txt,big.txt,sub/b.TXT,sub/deep/d.txt"));
  // directory masks find directories (loop directory found once, not recursed)
  CHECK_EQ(Names(FileFind::FindLocal(Root, QStringLiteral("*/"), nullptr), Root).join(QLatin1Char(',')),
    QStringLiteral(".git/,empty/,node_modules/,node_modules/x/,sub/,sub/deep/,sub/deep/loop/"));
  // path mask
  CHECK_EQ(Names(FileFind::FindLocal(Root, QStringLiteral("*/sub/*"), nullptr), Root).join(QLatin1Char(',')),
    QStringLiteral("sub/b.TXT,sub/c.dat"));
  // exclude files
  CHECK_EQ(Names(FileFind::FindLocal(Root, QStringLiteral("* | *.txt; *.TXT; */"), nullptr), Root).join(QLatin1Char(',')),
    QStringLiteral("README,old.log"));

  std::printf("[local async]\n");
  {
    FileFind Find;
    AsyncResult R = RunAsync(Find, nullptr, Root, QStringLiteral("*.txt"));
    CHECK(R.Finished);
    CHECK(!R.Aborted);
    CHECK(R.Error.isEmpty());
    CHECK_EQ(Names(R.Files, Root).join(QLatin1Char(',')), All.join(QLatin1Char(',')));
    CHECK(!R.Directories.isEmpty());
    CHECK(!Find.Finding());
    // directory of each result
    for (const FoundFile & F : R.Files)
    {
      CHECK_EQ(F.Directory + QLatin1Char('/') + F.File.FileName, F.FullPath);
    }
  }
  std::printf("[local errors]\n");
  {
    FileFind Find;
    QString Error;
    CHECK(!Find.Start(nullptr, Root, QStringLiteral("[a-"), &Error));
    CHECK(!Error.isEmpty());
    AsyncResult R = RunAsync(Find, nullptr, Root + QStringLiteral("/nonexistent"), QStringLiteral("*"));
    CHECK(R.Finished);
    CHECK(!R.Aborted);
    CHECK(R.Error.startsWith(QStringLiteral("Error retrieving file list for")));
  }
  std::printf("[local stop]\n");
  {
    QString Big = Root + QStringLiteral("/../bigtree");
    for (int I = 0; I < 200; I++)
    {
      WriteFile(Big + QStringLiteral("/d%1/f.txt").arg(I % 40) + QString::number(I), 1);
    }
    FileFind Find;
    AsyncResult R = RunAsync(Find, nullptr, Big, QStringLiteral("*"), 1);
    CHECK(R.Finished);
    // may have finished before the stop got through on a fast machine
    std::printf("  aborted=%d files=%d\n", R.Aborted, static_cast<int>(R.Files.size()));
    R = RunAsync(Find, nullptr, Big, QStringLiteral("*"), 0);
    CHECK(R.Finished);
    CHECK(R.Aborted);
    // A stopped object can be started again
    AsyncResult R2 = RunAsync(Find, nullptr, Big, QStringLiteral("*.txt*"));
    CHECK(!R2.Aborted);
    CHECK_EQ(static_cast<int>(R2.Files.size()), 200);
  }
}

static void TestBookmarks()
{
  std::printf("[bookmarks]\n");
  IniFile File;
  {
    Bookmarks B;
    B.Load(File);
    CHECK(B.Keys().isEmpty());
    BookmarkList List;
    Bookmark A;
    A.Name = QStringLiteral("www");
    A.Local = QStringLiteral("/home/me/www");
    A.Remote = QStringLiteral("/var/www");
    A.ShortCut = 0x4000 | 'W';   // Ctrl+W
    List.Add(A);
    Bookmark C;
    C.Name = QStringLiteral("Logs dir");
    C.Remote = QStringLiteral("/var/log");
    C.Node = QStringLiteral("Server stuff");
    List.Add(C);
    Bookmark D = C;
    D.Name = QStringLiteral("logs DIR");   // duplicate (case insensitive)
    bool Thrown = false;
    try
    {
      List.Add(D);
    }
    catch (const ExtException & E)
    {
      Thrown = true;
      CHECK(E.Message().contains(QStringLiteral("already exists")));
    }
    CHECK(Thrown);
    List.SetNodeOpened(QStringLiteral("Server stuff"), true);
    B.SetList(QStringLiteral("me@example.com"), List);
    BookmarkList Shared;
    Bookmark S;
    S.Name = QStringLiteral("/tmp");
    S.Local = QStringLiteral("/tmp");
    S.Remote = QStringLiteral("/tmp");
    Shared.Add(S);
    B.SetSharedList(Shared);
    B.Save(File);
  }
  // WinSCP.ini compatible layout
  CHECK_EQ(File.ReadRaw(QStringLiteral("Configuration\\Bookmarks\\Remote\\me@example.com"), QStringLiteral("www")),
    QStringLiteral("/var/www"));
  CHECK_EQ(File.ReadRaw(QStringLiteral("Configuration\\Bookmarks\\Remote\\me@example.com\\Server%20stuff"), QStringLiteral("Logs dir")),
    QStringLiteral("/var/log"));
  CHECK_EQ(File.ReadRaw(QStringLiteral("Configuration\\Bookmarks\\ShortCuts\\me@example.com"), QStringLiteral("www")),
    QString::number(0x4000 | 'W'));
  CHECK(File.SectionExists(QStringLiteral("Configuration\\Bookmarks\\Local\\_!_shared")));
  CHECK_EQ(File.ReadRaw(QStringLiteral("Configuration\\Bookmarks\\Options\\me@example.com"), QStringLiteral("OpenedNodes")),
    QStringLiteral("\"Server%20stuff\""));
  {
    Bookmarks B;
    B.Load(File);
    BookmarkList List = B.List(QStringLiteral("me@example.com"));
    CHECK_EQ(List.Count(), 2);
    if (List.Count() == 2)
    {
      CHECK_EQ(List.At(0).Name, QStringLiteral("www"));
      CHECK_EQ(List.At(0).Local, QStringLiteral("/home/me/www"));
      CHECK_EQ(List.At(0).Remote, QStringLiteral("/var/www"));
      CHECK_EQ(List.At(0).ShortCut, 0x4000 | 'W');
      CHECK_EQ(List.At(1).Node, QStringLiteral("Server stuff"));
      CHECK(List.At(1).Local.isEmpty());
    }
    CHECK(List.NodeOpened(QStringLiteral("server stuff")));
    CHECK_EQ(List.Folders().join(QLatin1Char('|')), QStringLiteral("Server stuff"));
    CHECK(List.FindByShortCut(0x4000 | 'W') != nullptr);
    CHECK_EQ(B.SharedList().Count(), 1);
    // removing a bookmark rewrites only that list
    List.Delete(0);
    B.SetList(QStringLiteral("me@example.com"), List);
    B.Save(File);
    CHECK(!File.SectionExists(QStringLiteral("Configuration\\Bookmarks\\ShortCuts\\me@example.com")) ||
      File.Keys(QStringLiteral("Configuration\\Bookmarks\\ShortCuts\\me@example.com")).isEmpty());
    CHECK(File.ReadRaw(QStringLiteral("Configuration\\Bookmarks\\Remote\\me@example.com"), QStringLiteral("www")).isEmpty());
    CHECK_EQ(File.ReadRaw(QStringLiteral("Configuration\\Bookmarks\\Remote\\_!_shared"), QStringLiteral("/tmp")),
      QStringLiteral("/tmp"));
  }
  // legacy numbered entries
  {
    IniFile Legacy;
    Legacy.WriteRaw(QStringLiteral("Configuration\\Bookmarks\\Remote\\u@h"), QStringLiteral("0"), QStringLiteral("%2Fsrv"));
    Bookmarks B;
    B.Load(Legacy);
    BookmarkList List = B.List(QStringLiteral("u@h"));
    CHECK_EQ(List.Count(), 1);
    if (List.Count() == 1)
    {
      CHECK_EQ(List.At(0).Name, QStringLiteral("/srv"));
      CHECK_EQ(List.At(0).Remote, QStringLiteral("/srv"));
    }
  }
  // session key and UseSharedBookmarks
  {
    SessionData Data;
    Data.UserName = QStringLiteral("martin");
    Data.HostName = QStringLiteral("example.com");
    Data.FSProtocol = fsSFTP;
    Data.PortNumber = 22;
    CHECK_EQ(Bookmarks::SessionKey(Data), QStringLiteral("martin@example.com"));
    Data.PortNumber = 2222;
    CHECK_EQ(Bookmarks::SessionKey(Data), QStringLiteral("martin@example.com:2222"));
    CHECK(!Bookmarks::UseSharedBookmarks(File));
    Bookmarks::SetUseSharedBookmarks(File, true);
    CHECK(Bookmarks::UseSharedBookmarks(File));
    CHECK_EQ(File.ReadRaw(QStringLiteral("Configuration\\Interface"), QStringLiteral("UseSharedBookmarks")), QStringLiteral("1"));
  }
  // Move
  {
    BookmarkList List;
    for (const char * N : { "a", "b", "c" })
    {
      Bookmark X;
      X.Name = QString::fromLatin1(N);
      List.Add(X);
    }
    List.Move(0, 2);
    CHECK_EQ(List.At(0).Name + List.At(1).Name + List.At(2).Name, QStringLiteral("bca"));
  }
}

static void TestRemote(const QString & Root)
{
  QByteArray PortStr = qgetenv("NIXSCP_TEST_SSH_PORT");
  QString KeyFile = qEnvironmentVariable("NIXSCP_TEST_SSH_KEY");
  if (PortStr.isEmpty() || KeyFile.isEmpty())
  {
    std::printf("[remote] SKIPPED (set NIXSCP_TEST_SSH_PORT, NIXSCP_TEST_SSH_KEY)\n");
    return;
  }
  std::printf("[remote]\n");
  QTemporaryDir Scratch;
  SessionData Data;
  Data.HostName = qEnvironmentVariable("NIXSCP_TEST_SSH_HOST", QStringLiteral("127.0.0.1"));
  Data.PortNumber = PortStr.toInt();
  Data.UserName = qEnvironmentVariable("NIXSCP_TEST_SSH_USER", qEnvironmentVariable("USER"));
  Data.PublicKeyFile = KeyFile;
  Data.TryAgent = false;
  Data.AuthKI = false;
  Data.KnownHostsFile = Scratch.filePath(QStringLiteral("known_hosts"));
  NullUserInterface UI;
  Terminal Term(Data, &UI);
  {
    QEventLoop Loop;
    OperationResult Result;
    Term.Open([&](const OperationResult & R) { Result = R; Loop.quit(); });
    Loop.exec();
    CHECK(Result.Ok);
    if (!Result.Ok)
    {
      std::printf("  open failed: %s %s\n", qPrintable(Result.Message), qPrintable(Result.MoreMessages));
      return;
    }
  }
  FileFind Find;
  AsyncResult R = RunAsync(Find, &Term, Root, QStringLiteral("*.txt"));
  CHECK(R.Finished);
  CHECK(!R.Aborted);
  CHECK(R.Error.isEmpty());
  CHECK_EQ(Names(R.Files, Root).join(QLatin1Char(',')),
    QStringLiteral(".git/config.txt,a.txt,big.txt,node_modules/x/y.txt,sub/b.TXT,sub/deep/d.txt"));
  R = RunAsync(Find, &Term, Root + QStringLiteral("/"), QStringLiteral("*.txt>1K | node_modules/"));
  CHECK_EQ(Names(R.Files, Root).join(QLatin1Char(',')), QStringLiteral("big.txt,sub/deep/d.txt"));
  // symlink loop detected via link target
  R = RunAsync(Find, &Term, Root, QStringLiteral("*/"));
  CHECK_EQ(Names(R.Files, Root).join(QLatin1Char(',')),
    QStringLiteral(".git/,empty/,node_modules/,node_modules/x/,sub/,sub/deep/,sub/deep/loop/"));
  R = RunAsync(Find, &Term, Root, QStringLiteral("<100D"));
  CHECK_EQ(Names(R.Files, Root).join(QLatin1Char(',')), QStringLiteral("old.log"));
  // error on start directory
  R = RunAsync(Find, &Term, Root + QStringLiteral("/nonexistent"), QStringLiteral("*"));
  CHECK(!R.Aborted);
  CHECK(R.Error.startsWith(QStringLiteral("Error retrieving file list for")));
  // stop
  QString Big = Root + QStringLiteral("/../bigtree");
  R = RunAsync(Find, &Term, Big, QStringLiteral("*"), 1);
  CHECK(R.Finished);
  std::printf("  remote stop: aborted=%d files=%d\n", R.Aborted, static_cast<int>(R.Files.size()));
  R = RunAsync(Find, &Term, Big, QStringLiteral("*"), 0);
  CHECK(R.Finished);
  CHECK(R.Aborted);
  // terminal still usable after stopping
  R = RunAsync(Find, &Term, Big, QStringLiteral("*.txt*"));
  CHECK(!R.Aborted);
  CHECK_EQ(static_cast<int>(R.Files.size()), 200);
  Term.Close();
}

int main(int argc, char ** argv)
{
  QCoreApplication App(argc, argv);
  QTemporaryDir Dir;
  CHECK(Dir.isValid());
  QString Root = QDir(Dir.path()).filePath(QStringLiteral("tree"));
  BuildTree(Root);

  TestLocal(Root);
  TestBookmarks();
  TestRemote(Root);

  if (Failures > 0)
  {
    std::printf("test_findfiles: %d FAILURE(S)\n", Failures);
    return 1;
  }
  std::printf("test_findfiles: OK\n");
  return 0;
}
