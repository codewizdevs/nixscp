// Tests of the WP15 additions to core/Synchronize (WinSCP source/core/Terminal.cpp
// SameFileChecksum on the server, SynchronizeToQueue, SynchronizeChecklistCalculateSize,
// TCustomScpExplorerForm::DoSynchronizeMove, local deletes to the trash, spUseCache,
// MaxWatchDirectories of TSynchronizeController, inotify error hints).
//
// The parts without a server always run. The server parts are skipped unless
// NIXSCP_TEST_SSH_PORT is set (NIXSCP_TEST_SSH_HOST/USER/KEY as test_sftp);
// the server must run on this machine as the same user.
#include "core/Queue.h"
#include "core/Synchronize.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

static int Failures = 0;

#define CHECK(Cond) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); std::fflush(stdout); Failures++; } } while (0)
#define CHECK_MSG(Cond, Msg) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, #Cond, qPrintable(Msg)); std::fflush(stdout); Failures++; } } while (0)

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
    std::printf("  unexpected QueryUser \"%s\"\n", qPrintable(Query));
    Failures++;
    return qaCancel;
  }
};

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
    QTimer::singleShot(20, &Loop, &QEventLoop::quit);
    Loop.exec();
  }
  return true;
}

static QString Describe(const OperationResult & R)
{
  return QStringLiteral("ok=%1 aborted=%2 fatal=%3 msg=\"%4\" more=\"%5\"")
    .arg(R.Ok).arg(R.Aborted).arg(R.Fatal).arg(R.Message, R.MoreMessages);
}

static void WriteFile(const QString & FileName, const QByteArray & Data, qint64 MTime)
{
  QDir().mkpath(QFileInfo(FileName).path());
  QFile F(FileName);
  CHECK(F.open(QIODevice::WriteOnly | QIODevice::Truncate));
  F.write(Data);
  F.close();
  struct timespec Times[2] = {{static_cast<time_t>(MTime), 0}, {static_cast<time_t>(MTime), 0}};
  utimensat(AT_FDCWD, QFile::encodeName(FileName).constData(), Times, 0);
}

static QByteArray ReadAll(const QString & FileName)
{
  QFile F(FileName);
  return F.open(QIODevice::ReadOnly) ? F.readAll() : QByteArray("<missing>");
}

static const qint64 T0 = QDateTime::currentSecsSinceEpoch() - 100000;

static SynchronizeChecklistPtr Collect(Terminal * Term, const QString & Local, const QString & Remote, SynchronizeMode Mode,
  int Params, const SynchronizeOptions & Options = SynchronizeOptions())
{
  bool Called = false;
  SynchronizeChecklistPtr Result;
  SynchronizeCollect(Term, Local, Remote, Mode, CopyParam(), Params | spNoConfirmation, Options, std::make_shared<SynchronizeStatus>(),
    [&](const OperationResult & R, SynchronizeChecklistPtr Checklist)
    {
      CHECK_MSG(R.Ok, Describe(R));
      Result = Checklist;
      Called = true;
    });
  CHECK(WaitUntil([&]() { return Called; }, 60000));
  return Result ? Result : std::make_shared<SynchronizeChecklist>();
}

static QStringList Names(const SynchronizeChecklist & Checklist)
{
  QStringList Result;
  for (int Index = 0; Index < Checklist.Count(); Index++)
  {
    Result.append(Checklist.GetItem(Index)->GetFileName());
  }
  Result.sort();
  return Result;
}

static const SynchronizeChecklist::Item * Find(const SynchronizeChecklist & Checklist, const QString & Name)
{
  for (int Index = 0; Index < Checklist.Count(); Index++)
  {
    if (Checklist.GetItem(Index)->GetFileName() == Name)
    {
      return Checklist.GetItem(Index);
    }
  }
  return nullptr;
}

static void TestWithoutServer(const QString & Scratch)
{
  std::printf("[defaults]\n");
  // Linux: case sensitive by default; not on macOS, where the disk ignores case
#ifdef Q_OS_MACOS
  CHECK((spDefault & spCaseSensitive) == 0);
#else
  CHECK((spDefault & spCaseSensitive) != 0);
#endif
  CHECK((spDefault & spPreviewChanges) != 0);
  CHECK(SynchronizeChecklist::OppositeMoveAction(SynchronizeChecklist::saUploadNew) == SynchronizeChecklist::saDeleteRemote);
  CHECK(SynchronizeChecklist::OppositeMoveAction(SynchronizeChecklist::saDeleteLocal) == SynchronizeChecklist::saDownloadNew);
  CHECK(SynchronizeChecklist::OppositeMoveAction(SynchronizeChecklist::saUploadUpdate) == SynchronizeChecklist::saNone);
  CHECK(SynchronizeChecklist::IsTransferNewAction(SynchronizeChecklist::saDownloadNew));
  CHECK(!SynchronizeChecklist::IsTransferNewAction(SynchronizeChecklist::saDeleteRemote));

  std::printf("[checksum output]\n");
  {
    QHash<QString, QString> H = ParseChecksumOutput({
      QStringLiteral("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855  /tmp/a b.txt"),
      QStringLiteral("ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef0123456789 */bin/x"),
      QStringLiteral("\\0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef  /tmp/new\\nline\\\\back"),
      QStringLiteral("sha256sum: /tmp/missing: No such file or directory"),
      QStringLiteral("d41d8cd98f00b204e9800998ecf8427e /tmp/md5 -r.txt"),
      QString()});
    CHECK(H.size() == 4);
    CHECK(H.value(QStringLiteral("/tmp/md5 -r.txt")) == QStringLiteral("d41d8cd98f00b204e9800998ecf8427e"));
    CHECK(H.value(QStringLiteral("/tmp/a b.txt")) == QStringLiteral("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK(H.value(QStringLiteral("/bin/x")) == QStringLiteral("abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789"));
    CHECK(H.contains(QStringLiteral("/tmp/new\nline\\back")));
  }
  {
    QString F = QDir(Scratch).filePath(QStringLiteral("hash.txt"));
    WriteFile(F, QByteArray(), T0);
    CHECK(LocalFileChecksum(F, QStringLiteral("sha-256")) == QStringLiteral("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK(LocalFileChecksum(F, QStringLiteral("sha256")) == QStringLiteral("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK(LocalFileChecksum(F, QStringLiteral("sha-1")) == QStringLiteral("da39a3ee5e6b4b0d3255bfef95601890afd80709"));
    CHECK(LocalFileChecksum(F, QStringLiteral("md5")) == QStringLiteral("d41d8cd98f00b204e9800998ecf8427e"));
    bool Thrown = false;
    try
    {
      LocalFileChecksum(F, QStringLiteral("crc32"));
    }
    catch (const ExtException &)
    {
      Thrown = true;
    }
    CHECK(Thrown);
  }

  std::printf("[listing cache]\n");
  {
    SynchronizeListingCache Cache;
    RemoteFileList L;
    RemoteFile F;
    F.FileName = QStringLiteral("a");
    L.append(F);
    Cache.Put(QStringLiteral("/r"), L);
    Cache.Put(QStringLiteral("/r/sub"), L);
    Cache.Put(QStringLiteral("/rx"), L);
    RemoteFileList Out;
    CHECK(Cache.Get(QStringLiteral("/r/sub"), Out) && (Out.size() == 1));
    Cache.Invalidate(QStringLiteral("/r"));
    CHECK(!Cache.Get(QStringLiteral("/r"), Out));
    CHECK(!Cache.Get(QStringLiteral("/r/sub"), Out));
    CHECK(Cache.Get(QStringLiteral("/rx"), Out));
    CHECK(Cache.Count() == 1);
    Cache.Clear();
    CHECK(Cache.Count() == 0);
  }

  std::printf("[inotify hints]\n");
  {
    QString M = InotifyErrorMessage(ENOSPC);
    CHECK_MSG(M.contains(QStringLiteral("fs.inotify.max_user_watches")), M);
    M = InotifyErrorMessage(EMFILE);
    CHECK_MSG(M.contains(QStringLiteral("fs.inotify.max_user_instances")), M);
    CHECK(!InotifyErrorMessage(EACCES).contains(QStringLiteral("inotify")));
  }

  std::printf("[MaxWatchDirectories]\n");
  {
    // 12 directories; the limit 5 is asked for once (and doubled), then the
    // second time the watching is aborted
    QString Root = QDir(Scratch).filePath(QStringLiteral("watch"));
    for (int Index = 0; Index < 11; Index++)
    {
      QDir().mkpath(QDir(Root).filePath(QStringLiteral("d%1").arg(Index)));
    }
    SessionData Data;
    Data.HostName = QStringLiteral("127.0.0.1");
    TestUserInterface UI;
    Terminal T(Data, &UI);
    SynchronizeController Controller(&T);
    Controller.SetMaxWatchDirectories(5);
    QList<int> Asked;
    QObject::connect(&Controller, &SynchronizeController::TooManyDirectories, [&](int & MaxDirectories, bool & Abort)
    {
      Asked.append(MaxDirectories);
      if (Asked.size() == 1)
      {
        MaxDirectories *= 2;  // "Yes"
      }
      else
      {
        Abort = true;         // "No"
      }
    });
    bool Aborted = false;
    QString AbortMessage = QStringLiteral("<none>");
    QObject::connect(&Controller, &SynchronizeController::Aborted, [&](bool, const QString & Message, const QString &)
    {
      Aborted = true;
      AbortMessage = Message;
    });
    SynchronizeParamType Params;
    Params.LocalDirectory = Root;
    Params.RemoteDirectory = QStringLiteral("/nonexistent");
    Params.Options = soRecurse;
    Controller.Start(Params, CopyParam(), SynchronizeOptions());
    CHECK(WaitUntil([&]() { return Aborted; }, 5000));
    CHECK_MSG(Asked == (QList<int>{5, 10}), QString::number(Asked.size()));
    CHECK_MSG(AbortMessage.isEmpty(), AbortMessage);
    CHECK(!Controller.Active());

    // a sufficient limit: watching starts
    Asked.clear();
    Aborted = false;
    Controller.SetMaxWatchDirectories(100);
    QObject::disconnect(&Controller, &SynchronizeController::TooManyDirectories, nullptr, nullptr);
    QObject::connect(&Controller, &SynchronizeController::TooManyDirectories, [&](int & MaxDirectories, bool &)
    {
      Asked.append(MaxDirectories);
      MaxDirectories = 100;
    });
    QStringList Logs;
    QObject::connect(&Controller, &SynchronizeController::Log, [&](SynchronizeLogEntry, const QString & Message, const QString &)
    {
      Logs.append(Message);
    });
    Controller.Start(Params, CopyParam(), SynchronizeOptions());
    CHECK(WaitUntil([&]() { return Controller.Directories() == 12; }, 5000));
    CHECK(!Aborted);
    CHECK_MSG(Logs.contains(QStringLiteral("Watching for changes in 12 directories...")), Logs.join(QLatin1Char('|')));
    CHECK(Asked.isEmpty());
    Controller.Stop();
  }
}

// Synchronization by checksum with a remote file of the same size and time
static void TestChecksum(Terminal * Term, const QString & Local, const QString & Remote, const char * Name)
{
  std::printf("[checksum: %s]\n", Name);
  QDir(Local).removeRecursively();
  QDir(Remote).removeRecursively();
  WriteFile(QDir(Local).filePath(QStringLiteral("same.txt")), "identical", T0);
  WriteFile(QDir(Remote).filePath(QStringLiteral("same.txt")), "identical", T0);
  WriteFile(QDir(Local).filePath(QStringLiteral("differs.txt")), "content-A", T0);
  WriteFile(QDir(Remote).filePath(QStringLiteral("differs.txt")), "content-B", T0);
  WriteFile(QDir(Local).filePath(QStringLiteral("sub/q u'o\"te $x.txt")), "same-in-sub", T0);
  WriteFile(QDir(Remote).filePath(QStringLiteral("sub/q u'o\"te $x.txt")), "diff-in-sub", T0);
  WriteFile(QDir(Local).filePath(QStringLiteral("sub/ok.txt")), "ok", T0);
  WriteFile(QDir(Remote).filePath(QStringLiteral("sub/ok.txt")), "ok", T0);
  // many files in one directory: more than one batch of the checksum command
  for (int Index = 0; Index < 70; Index++)
  {
    QString FileName = QStringLiteral("many/f%1.txt").arg(Index);
    WriteFile(QDir(Local).filePath(FileName), QByteArray::number(Index), T0);
    WriteFile(QDir(Remote).filePath(FileName), (Index == 67) ? QByteArray("X") + QByteArray::number(Index).mid(1) : QByteArray::number(Index), T0);
  }
  auto C = Collect(Term, Local, Remote, smRemote, spNotByTime | spByChecksum);
  // calculated on the server (sha256sum), nothing was downloaded
  CHECK_MSG(Term->Progress().Get().TotalTransferred == 0, QString::number(Term->Progress().Get().TotalTransferred));
  CHECK_MSG(Names(*C) == (QStringList{QStringLiteral("differs.txt"), QStringLiteral("f67.txt"), QStringLiteral("q u'o\"te $x.txt")}),
    Names(*C).join(QLatin1Char(',')));
  for (int Index = 0; Index < C->Count(); Index++)
  {
    CHECK(C->GetItem(Index)->Action == SynchronizeChecklist::saUploadUpdate);
  }
  // mode Local: downloads instead
  C = Collect(Term, Local, Remote, smLocal, spNotByTime | spByChecksum);
  CHECK(Names(*C).size() == 3);
  const SynchronizeChecklist::Item * Item = Find(*C, QStringLiteral("differs.txt"));
  CHECK((Item != nullptr) && (Item->Action == SynchronizeChecklist::saDownloadUpdate));
}

int main(int argc, char ** argv)
{
  QCoreApplication App(argc, argv);
  QTemporaryDir Scratch;
  CHECK(Scratch.isValid());
  // QFile::moveToTrash: keep the trash in the scratch directory
  qputenv("XDG_DATA_HOME", QFile::encodeName(Scratch.filePath(QStringLiteral("xdg-data"))));
  QDir().mkpath(Scratch.filePath(QStringLiteral("xdg-data")));

  TestWithoutServer(Scratch.path());

  QByteArray PortStr = qgetenv("NIXSCP_TEST_SSH_PORT");
  if (PortStr.isEmpty())
  {
    std::printf("test_syncex: server tests SKIPPED (set NIXSCP_TEST_SSH_PORT, NIXSCP_TEST_SSH_KEY)\n");
    std::printf(Failures ? "test_syncex: %d FAILURES\n" : "test_syncex: OK\n", Failures);
    return Failures ? 1 : 0;
  }

  SessionData Data;
  Data.HostName = qEnvironmentVariable("NIXSCP_TEST_SSH_HOST", QStringLiteral("127.0.0.1"));
  Data.PortNumber = PortStr.toInt();
  Data.UserName = qEnvironmentVariable("NIXSCP_TEST_SSH_USER", qEnvironmentVariable("USER"));
  Data.PublicKeyFile = qEnvironmentVariable("NIXSCP_TEST_SSH_KEY");
  Data.TryAgent = false;
  Data.AuthKI = false;
  Data.KnownHostsFile = Scratch.filePath(QStringLiteral("dotssh/known_hosts"));

  TestUserInterface UI;
  QString Local = Scratch.filePath(QStringLiteral("local"));
  QString Remote = Scratch.filePath(QStringLiteral("remote"));

  for (TFSProtocol Protocol : {fsSFTP, fsSCPonly})
  {
    SessionData D = Data;
    D.FSProtocol = Protocol;
    Terminal T(D, &UI);
    bool Called = false;
    T.Open([&](const OperationResult & R) { CHECK_MSG(R.Ok, Describe(R)); Called = true; });
    CHECK(WaitUntil([&]() { return Called; }, 30000));
    if (!T.Active())
    {
      std::printf("test_syncex: cannot connect\n");
      return 1;
    }
    TestChecksum(&T, Local, Remote, (Protocol == fsSFTP) ? "sftp" : "scp");
  }

  Terminal T(Data, &UI);
  {
    bool Called = false;
    T.Open([&](const OperationResult & R) { CHECK_MSG(R.Ok, Describe(R)); Called = true; });
    CHECK(WaitUntil([&]() { return Called; }, 30000));
  }

  std::printf("[calculate size]\n");
  {
    QDir(Local).removeRecursively();
    QDir(Remote).removeRecursively();
    QDir().mkpath(Local);
    QDir().mkpath(Remote);
    WriteFile(QDir(Local).filePath(QStringLiteral("ldir/a.bin")), QByteArray(1000, 'a'), T0);
    WriteFile(QDir(Local).filePath(QStringLiteral("ldir/sub/b.bin")), QByteArray(234, 'b'), T0);
    WriteFile(QDir(Remote).filePath(QStringLiteral("rdir/c.bin")), QByteArray(4096, 'c'), T0);
    WriteFile(QDir(Remote).filePath(QStringLiteral("rdir/d/e.bin")), QByteArray(4, 'e'), T0);
    auto C = Collect(&T, Local, Remote, smBoth, 0);
    const SynchronizeChecklist::Item * LDir = Find(*C, QStringLiteral("ldir"));
    const SynchronizeChecklist::Item * RDir = Find(*C, QStringLiteral("rdir"));
    CHECK((LDir != nullptr) && (RDir != nullptr));
    if ((LDir != nullptr) && (RDir != nullptr))
    {
      CHECK(!LDir->HasSize() && !RDir->HasSize());
      bool Called = false;
      SynchronizeChecklistCalculateSize(&T, C, {LDir, RDir}, CopyParam(), 0, [&](const OperationResult & R)
      {
        CHECK_MSG(R.Ok, Describe(R));
        Called = true;
      });
      CHECK(WaitUntil([&]() { return Called; }, 30000));
      CHECK(LDir->HasSize() && RDir->HasSize());
      CHECK_MSG(LDir->Info1.Size == 1234, QString::number(LDir->Info1.Size));
      CHECK_MSG(RDir->Info2.Size == 4100, QString::number(RDir->Info2.Size));
      CHECK(LDir->GetSize() == 1234);
      // with a mask: only *.bin directly or below "sub" excluded
      CopyParam Masked;
      Masked.IncludeFileMask.SetMasks(QStringLiteral("| sub/"));
      Called = false;
      SynchronizeChecklistCalculateSize(&T, C, {LDir}, Masked, 0, [&](const OperationResult &) { Called = true; });
      CHECK(WaitUntil([&]() { return Called; }, 30000));
      CHECK_MSG(LDir->Info1.Size == 1000, QString::number(LDir->Info1.Size));
    }
  }

  std::printf("[move]\n");
  {
    QDir(Local).removeRecursively();
    QDir(Remote).removeRecursively();
    // remote: renamed file (same size, different name in the same directory)
    WriteFile(QDir(Local).filePath(QStringLiteral("renamed.txt")), "payload", T0);
    WriteFile(QDir(Remote).filePath(QStringLiteral("original.txt")), "payload", T0);
    // local: a directory to move files into
    WriteFile(QDir(Remote).filePath(QStringLiteral("target/x.txt")), "x", T0);
    WriteFile(QDir(Local).filePath(QStringLiteral("x.txt")), "x", T0);
    bool Called = false;
    bool Ok = false;
    SynchronizeMove(&T, true, {QDir(Remote).filePath(QStringLiteral("original.txt"))}, QDir(Remote).filePath(QStringLiteral("renamed.txt")),
      false, [&](const OperationResult & R) { Ok = R.Ok; Called = true; });
    CHECK(WaitUntil([&]() { return Called; }, 30000));
    CHECK(Ok);
    CHECK(ReadAll(QDir(Remote).filePath(QStringLiteral("renamed.txt"))) == "payload");
    CHECK(!QFileInfo::exists(QDir(Remote).filePath(QStringLiteral("original.txt"))));
    // remote into a (new) directory
    WriteFile(QDir(Remote).filePath(QStringLiteral("loose.txt")), "l", T0);
    Called = false;
    SynchronizeMove(&T, true, {QDir(Remote).filePath(QStringLiteral("loose.txt"))}, QDir(Remote).filePath(QStringLiteral("newdir")),
      true, [&](const OperationResult & R) { Ok = R.Ok; Called = true; });
    CHECK(WaitUntil([&]() { return Called; }, 30000));
    CHECK(Ok && (ReadAll(QDir(Remote).filePath(QStringLiteral("newdir/loose.txt"))) == "l"));
    // local into a directory
    Called = false;
    SynchronizeMove(&T, false, {QDir(Local).filePath(QStringLiteral("x.txt"))}, QDir(Local).filePath(QStringLiteral("target")),
      true, [&](const OperationResult & R) { Ok = R.Ok; Called = true; });
    CHECK(WaitUntil([&]() { return Called; }, 30000));
    CHECK(Ok && (ReadAll(QDir(Local).filePath(QStringLiteral("target/x.txt"))) == "x"));
    CHECK(!QFileInfo::exists(QDir(Local).filePath(QStringLiteral("x.txt"))));
    // failure is reported
    Called = false;
    SynchronizeMove(&T, false, {QDir(Local).filePath(QStringLiteral("nonexistent"))}, QDir(Local).filePath(QStringLiteral("y")),
      false, [&](const OperationResult & R) { Ok = R.Ok; Called = true; });
    CHECK(WaitUntil([&]() { return Called; }, 30000));
    CHECK(!Ok);
  }

#ifdef Q_OS_MACOS
  std::printf("[both Unicode forms of a name]\n");
  {
    // one file to the disk: not a new one on each side, and a transfer of
    // the "new" file would overwrite the other one unasked
    QDir(Local).removeRecursively();
    QDir(Remote).removeRecursively();
    WriteFile(QDir(Local).filePath(QString::fromUtf8("cafe\xCC\x81.txt")), "same", T0);
    WriteFile(QDir(Remote).filePath(QString::fromUtf8("caf\xC3\xA9.txt")), "same", T0);
    auto C = Collect(&T, Local, Remote, smBoth, 0);
    CHECK_MSG(C->Count() == 0, Names(*C).join(QLatin1Char(',')));
  }
#endif

  std::printf("[local deletes to trash]\n");
  {
    QDir(Local).removeRecursively();
    QDir(Remote).removeRecursively();
    QDir().mkpath(Remote);
    WriteFile(QDir(Local).filePath(QStringLiteral("obsolete.txt")), "obsolete", T0);
    WriteFile(QDir(Local).filePath(QStringLiteral("obsdir/f.txt")), "f", T0);
    auto C = Collect(&T, Local, Remote, smLocal, spDelete);
    CHECK(C->CheckedCount() == 2);
    bool Called = false;
    SynchronizeStatistics Stats;
    SynchronizeApply(&T, C, CopyParam(), spDelete | spNoConfirmation, nullptr, nullptr,
      [&](const OperationResult & R, const SynchronizeStatistics & S) { CHECK_MSG(R.Ok, Describe(R)); Stats = S; Called = true; }, true);
    CHECK(WaitUntil([&]() { return Called; }, 30000));
    CHECK(Stats.FilesDeletedLocal == 2);
    CHECK(!QFileInfo::exists(QDir(Local).filePath(QStringLiteral("obsolete.txt"))));
#ifdef Q_OS_MACOS
    // the system's trash, ~/.Trash of the real user whatever $HOME says, and the name may get a suffix
    QString Trash = QString::fromLocal8Bit(getpwuid(getuid())->pw_dir) + QStringLiteral("/.Trash");
    bool InTrash = !QDir(Trash).entryList({QStringLiteral("obsolete*.txt")}, QDir::Files).isEmpty();
#else
    QString Trash = Scratch.filePath(QStringLiteral("xdg-data/Trash/files"));
    bool InTrash = QFileInfo::exists(QDir(Trash).filePath(QStringLiteral("obsolete.txt")));
#endif
    // /tmp may be another file system than XDG_DATA_HOME: Qt then uses $topdir/.Trash-$uid
    if (!InTrash)
    {
      QDirIterator It(QStringLiteral("/tmp"), {QStringLiteral(".Trash-*")}, QDir::Dirs | QDir::Hidden);
      while (It.hasNext() && !InTrash)
      {
        InTrash = QFileInfo::exists(QDir(It.next()).filePath(QStringLiteral("files/obsolete.txt")));
      }
    }
    CHECK_MSG(InTrash, Trash);
  }

  std::printf("[start in background]\n");
  {
    QDir(Local).removeRecursively();
    QDir(Remote).removeRecursively();
    WriteFile(QDir(Local).filePath(QStringLiteral("up.txt")), "up", T0);
    WriteFile(QDir(Local).filePath(QStringLiteral("updir/in.txt")), "in", T0);
    WriteFile(QDir(Remote).filePath(QStringLiteral("down.txt")), "down", T0);
    WriteFile(QDir(Remote).filePath(QStringLiteral("gone.txt")), "gone", T0);
    auto C = Collect(&T, Local, Remote, smBoth, 0);
    // smBoth: upload up.txt, updir; download down.txt, gone.txt
    CHECK_MSG(C->CheckedCount() == 4, QString::number(C->CheckedCount()));
    TransferQueue Queue(Data, &UI);
    Queue.SetKeepDoneItemsFor(-1);
    // the host key prompt of the background connection is answered by UI
    QObject::connect(&Queue, &TransferQueue::UserActionPending, &Queue, [&Queue](int Id) { Queue.ProcessUserAction(Id); }, Qt::QueuedConnection);
    int Queued = SynchronizeToQueue(&Queue, *C, CopyParam(), spNoConfirmation, false);
    CHECK(Queued == 4);
    CHECK(WaitUntil([&]() { return Queue.ActiveCount() == 0; }, 60000));
    CHECK(ReadAll(QDir(Remote).filePath(QStringLiteral("up.txt"))) == "up");
    CHECK(ReadAll(QDir(Remote).filePath(QStringLiteral("updir/in.txt"))) == "in");
    CHECK(ReadAll(QDir(Local).filePath(QStringLiteral("down.txt"))) == "down");
    for (const QueueItemInfo & Info : Queue.Items())
    {
      CHECK_MSG(Info.State == QueueItemInfo::qsDone, QStringLiteral("state %1: %2 %3").arg(Info.State).arg(Info.Error, Info.MoreMessages));
    }
    // timestamps cannot be queued
    CHECK(SynchronizeToQueue(&Queue, *C, CopyParam(), spTimestamp, false) == 0);
  }

  std::printf("[spUseCache]\n");
  {
    QDir(Local).removeRecursively();
    QDir(Remote).removeRecursively();
    QDir().mkpath(Local);
    WriteFile(QDir(Remote).filePath(QStringLiteral("r1.txt")), "r1", T0);
    SynchronizeOptions Options;
    Options.Cache = std::make_shared<SynchronizeListingCache>();
    auto C = Collect(&T, Local, Remote, smLocal, spUseCache, Options);
    CHECK(Names(*C) == QStringList{QStringLiteral("r1.txt")});
    CHECK(Options.Cache->Count() == 1);
    // the remote change is not seen while the listing is cached ...
    WriteFile(QDir(Remote).filePath(QStringLiteral("r2.txt")), "r2", T0);
    C = Collect(&T, Local, Remote, smLocal, spUseCache, Options);
    CHECK(Names(*C) == QStringList{QStringLiteral("r1.txt")});
    // ... until it is invalidated, without spUseCache it is always read
    C = Collect(&T, Local, Remote, smLocal, 0, Options);
    CHECK(Names(*C).size() == 2);
    Options.Cache->Invalidate(QDir::cleanPath(Remote));
    C = Collect(&T, Local, Remote, smLocal, spUseCache, Options);
    CHECK(Names(*C).size() == 2);
  }

  std::printf(Failures ? "test_syncex: %d FAILURES\n" : "test_syncex: OK\n", Failures);
  return Failures ? 1 : 0;
}
