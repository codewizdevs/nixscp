// Test of core/Synchronize (synchronization checklist, apply, compare
// directories, keep remote directory up to date) against a real SSH server.
//
// Skipped (exit 0) unless NIXSCP_TEST_SSH_PORT is set; uses the same
// environment as test_sftp (NIXSCP_TEST_SSH_HOST/USER/KEY). The server must
// run on this machine as the same user: the "remote" trees are created and
// verified directly on the local filesystem.
#include "core/Synchronize.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTimer>

#include <fcntl.h>
#include <sys/stat.h>
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
  HostKeyAnswer VerifyHostKey(const QString &, int, const QString &, const QString &, const QString &, HostKeyState) override
  {
    return HostKeyOnce;
  }
  bool PromptUser(PromptKind, const QString &, const QString &, QList<Prompt> &) override { return false; }
  void DisplayBanner(const QString &, const QString &) override {}
  unsigned int QueryUser(const QString & Query, const QString &, unsigned int) override
  {
    Log(QStringLiteral("unexpected QueryUser \"%1\"").arg(Query));
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
  if (!F.open(QIODevice::WriteOnly | QIODevice::Truncate) || (F.write(Data) != Data.size()))
  {
    std::printf("FAIL cannot write %s\n", qPrintable(FileName));
    Failures++;
  }
  F.close();
  struct timespec Times[2] = {{static_cast<time_t>(MTime), 0}, {static_cast<time_t>(MTime), 0}};
  utimensat(AT_FDCWD, QFile::encodeName(FileName).constData(), Times, 0);
}

static QByteArray ReadAll(const QString & FileName)
{
  QFile F(FileName);
  return F.open(QIODevice::ReadOnly) ? F.readAll() : QByteArray("<missing>");
}

static qint64 MTime(const QString & FileName)
{
  return QFileInfo(FileName).lastModified().toSecsSinceEpoch();
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

// Both trees identical (names, types, contents; optionally file times)
static bool SameTrees(const QString & A, const QString & B, bool CheckTimes, QStringList Ignore = QStringList())
{
  QMap<QString, QFileInfo> TA = Tree(A);
  QMap<QString, QFileInfo> TB = Tree(B);
  for (const QString & I : Ignore)
  {
    TA.remove(I);
    TB.remove(I);
  }
  bool Result = true;
  if (TA.keys() != TB.keys())
  {
    Log(QStringLiteral("trees differ: %1 vs %2").arg(TA.keys().join(QLatin1Char(',')), TB.keys().join(QLatin1Char(','))));
    return false;
  }
  for (auto It = TA.cbegin(); It != TA.cend(); ++It)
  {
    const QFileInfo & Other = TB[It.key()];
    if (It->isDir() != Other.isDir())
    {
      Log(QStringLiteral("type differs: %1").arg(It.key()));
      Result = false;
      continue;
    }
    if (It->isDir())
    {
      continue;
    }
    if (Sha256(It->filePath()) != Sha256(Other.filePath()))
    {
      Log(QStringLiteral("content differs: %1").arg(It.key()));
      Result = false;
    }
    if (CheckTimes && (It->lastModified().toSecsSinceEpoch() != Other.lastModified().toSecsSinceEpoch()))
    {
      Log(QStringLiteral("time differs: %1 %2 vs %3").arg(It.key()).arg(It->lastModified().toSecsSinceEpoch()).arg(Other.lastModified().toSecsSinceEpoch()));
      Result = false;
    }
  }
  return Result;
}

static const char * ActionName(SynchronizeChecklist::ActionType Action)
{
  switch (Action)
  {
    case SynchronizeChecklist::saUploadNew: return "UploadNew";
    case SynchronizeChecklist::saDownloadNew: return "DownloadNew";
    case SynchronizeChecklist::saUploadUpdate: return "UploadUpdate";
    case SynchronizeChecklist::saDownloadUpdate: return "DownloadUpdate";
    case SynchronizeChecklist::saDeleteRemote: return "DeleteRemote";
    case SynchronizeChecklist::saDeleteLocal: return "DeleteLocal";
    default: return "None";
  }
}

struct Env
{
  Terminal * Term;
  QString Local;
  QString Remote;
};

// Checklist as sorted "Action relative/path[/] [x]" lines
static QStringList Describe(const Env & E, const SynchronizeChecklist & Checklist)
{
  QStringList Result;
  for (int Index = 0; Index < Checklist.Count(); Index++)
  {
    const SynchronizeChecklist::Item * Item = Checklist.GetItem(Index);
    QString Path = Item->IsLocalOnly() || (Item->Action == SynchronizeChecklist::saUploadUpdate) ?
      QDir(E.Local).relativeFilePath(Item->GetLocalPath()) :
      QDir(E.Remote).relativeFilePath(Item->GetRemotePath());
    if (Item->IsDirectory)
    {
      Path += QLatin1Char('/');
    }
    Result.append(QStringLiteral("%1 %2%3").arg(QString::fromLatin1(ActionName(Item->Action)), Path, Item->Checked ? QStringLiteral(" [x]") : QString()));
  }
  Result.sort();
  return Result;
}

static SynchronizeChecklistPtr Collect(const Env & E, SynchronizeMode Mode, int Params,
  const CopyParam & Param = CopyParam(), const SynchronizeOptions & Options = SynchronizeOptions())
{
  bool Called = false;
  SynchronizeChecklistPtr Result;
  auto Status = std::make_shared<SynchronizeStatus>();
  SynchronizeCollect(E.Term, E.Local, E.Remote, Mode, Param, Params | spNoConfirmation, Options, Status,
    [&](const OperationResult & R, SynchronizeChecklistPtr Checklist)
    {
      CHECK_MSG(R.Ok, Describe(R));
      Result = Checklist;
      Called = true;
    });
  CHECK(WaitUntil([&]() { return Called; }, 60000));
  if (!Result)
  {
    Result = std::make_shared<SynchronizeChecklist>();
  }
  return Result;
}

static SynchronizeStatistics Apply(const Env & E, SynchronizeChecklistPtr Checklist, int Params, const CopyParam & Param = CopyParam())
{
  bool Called = false;
  int Processed = 0;
  SynchronizeStatistics Result;
  SynchronizeApply(E.Term, Checklist, Param, Params | spNoConfirmation, nullptr,
    [&](const SynchronizeChecklist::Item *) { Processed++; },
    [&](const OperationResult & R, const SynchronizeStatistics & Statistics)
    {
      CHECK_MSG(R.Ok, Describe(R));
      Result = Statistics;
      Called = true;
    });
  CHECK(WaitUntil([&]() { return Called; }, 60000));
  CHECK_MSG(Processed == Checklist->CheckedCount(), QStringLiteral("%1 of %2").arg(Processed).arg(Checklist->CheckedCount()));
  return Result;
}

static const qint64 T0 = QDateTime::currentSecsSinceEpoch() - 100000;

// Two trees with all kinds of differences
static void BuildTrees(const Env & E)
{
  QDir(E.Local).removeRecursively();
  QDir(E.Remote).removeRecursively();
  QDir().mkpath(E.Local);
  QDir().mkpath(E.Remote);
  auto L = [&](const QString & N, const QByteArray & D, qint64 T) { WriteFile(QDir(E.Local).filePath(N), D, T); };
  auto R = [&](const QString & N, const QByteArray & D, qint64 T) { WriteFile(QDir(E.Remote).filePath(N), D, T); };
  L(QStringLiteral("same.txt"), "same", T0);                 R(QStringLiteral("same.txt"), "same", T0);
  L(QStringLiteral("newer_local.txt"), "LOCAL1", T0 + 100);  R(QStringLiteral("newer_local.txt"), "REMOT1", T0);
  L(QStringLiteral("newer_remote.txt"), "AAA", T0);          R(QStringLiteral("newer_remote.txt"), "BBBBBB", T0 + 100);
  L(QStringLiteral("size_diff.txt"), "aaaa", T0);            R(QStringLiteral("size_diff.txt"), "aaaaaaaa", T0);
  L(QStringLiteral("only_local.txt"), "ol", T0);
  R(QStringLiteral("only_remote.txt"), "or", T0);
  L(QStringLiteral("dir_local_only/f1.txt"), "f1", T0);
  L(QStringLiteral("dir_local_only/sub/f2.txt"), "f2", T0);
  R(QStringLiteral("dir_remote_only/g1.txt"), "g1", T0);
  L(QStringLiteral("common/inner_same.txt"), "is", T0);      R(QStringLiteral("common/inner_same.txt"), "is", T0);
  L(QStringLiteral("common/inner_newer_local.txt"), "inl-local", T0 + 50); R(QStringLiteral("common/inner_newer_local.txt"), "inl", T0);
  L(QStringLiteral("common/inner_only_local.txt"), "iol", T0);
  R(QStringLiteral("common/inner_only_remote.txt"), "ior", T0);
  // one second difference is within the 2 s tolerance (CompareFileTime)
  L(QStringLiteral("tolerance.txt"), "tol", T0 + 1);         R(QStringLiteral("tolerance.txt"), "tol", T0);
  // type mismatch: file on one side, directory on the other
  L(QStringLiteral("mismatch"), "file", T0);                 R(QStringLiteral("mismatch/x.txt"), "x", T0);
}

static void CheckList(const Env & E, const SynchronizeChecklist & Checklist, QStringList Expected, const char * Name)
{
  Expected.sort();
  QStringList Actual = Describe(E, Checklist);
  CHECK_MSG(Actual == Expected, QStringLiteral("%1:\n    actual:   %2\n    expected: %3").arg(QString::fromLatin1(Name),
    Actual.join(QStringLiteral("\n              ")), Expected.join(QStringLiteral("\n              "))));
}

int main(int argc, char ** argv)
{
  QCoreApplication App(argc, argv);

  QByteArray PortStr = qgetenv("NIXSCP_TEST_SSH_PORT");
  if (PortStr.isEmpty())
  {
    std::printf("test_sync: SKIPPED (set NIXSCP_TEST_SSH_PORT, NIXSCP_TEST_SSH_KEY [, NIXSCP_TEST_SSH_HOST, NIXSCP_TEST_SSH_USER])\n");
    return 0;
  }

  // --------------------------------------------------------------- compare directories (no server needed)
  std::printf("[compare directories]\n");
  {
    QDateTime Base = QDateTime::fromSecsSinceEpoch(T0);
    QList<CompareEntry> Local, Remote;
    auto Add = [](QList<CompareEntry> & List, const char * Name, qint64 Size, const QDateTime & Time, bool Dir, bool Seconds)
    {
      CompareEntry Entry;
      Entry.FileName = QString::fromLatin1(Name);
      Entry.Size = Size;
      Entry.Modification = Time;
      Entry.Directory = Dir;
      Entry.SecondPrecision = Seconds;
      List.append(Entry);
    };
    Add(Local, "same", 1, Base.addMSecs(500), false, false);  Add(Remote, "same", 1, Base, false, true);
    Add(Local, "newer", 1, Base.addSecs(10), false, false);   Add(Remote, "newer", 1, Base, false, true);
    Add(Local, "older", 1, Base, false, false);               Add(Remote, "older", 1, Base.addSecs(10), false, true);
    Add(Local, "size", 1, Base, false, false);                Add(Remote, "size", 2, Base, false, true);
    Add(Local, "lonly", 1, Base, false, false);
    Add(Remote, "ronly", 1, Base, false, true);
    Add(Local, "dir", 0, Base, true, false);
    QStringList L, R;
    CompareDirectories(Local, Remote, true, false, false, L, R);
    L.sort(); R.sort();
    CHECK_MSG(L == (QStringList{QStringLiteral("lonly"), QStringLiteral("newer")}), L.join(QLatin1Char(',')));
    CHECK_MSG(R == (QStringList{QStringLiteral("older"), QStringLiteral("ronly")}), R.join(QLatin1Char(',')));
    CompareDirectories(Local, Remote, true, true, false, L, R);
    L.sort(); R.sort();
    CHECK_MSG(L == (QStringList{QStringLiteral("lonly"), QStringLiteral("newer"), QStringLiteral("size")}), L.join(QLatin1Char(',')));
    CHECK_MSG(R == (QStringList{QStringLiteral("older"), QStringLiteral("ronly"), QStringLiteral("size")}), R.join(QLatin1Char(',')));
    CompareDirectories(Local, Remote, false, true, true, L, R);
    CHECK_MSG(L == QStringList{QStringLiteral("size")}, L.join(QLatin1Char(',')));
  }

  QString Host = qEnvironmentVariable("NIXSCP_TEST_SSH_HOST", QStringLiteral("127.0.0.1"));
  QString User = qEnvironmentVariable("NIXSCP_TEST_SSH_USER", qEnvironmentVariable("USER"));
  QString KeyFile = qEnvironmentVariable("NIXSCP_TEST_SSH_KEY");

  QTemporaryDir Scratch;
  CHECK(Scratch.isValid());
  SessionData Data;
  Data.HostName = Host;
  Data.PortNumber = PortStr.toInt();
  Data.UserName = User;
  Data.PublicKeyFile = KeyFile;
  Data.TryAgent = false;
  Data.AuthKI = false;
  Data.KnownHostsFile = Scratch.filePath(QStringLiteral("dotssh/known_hosts"));

  TestUserInterface UI;
  Terminal T(Data, &UI);
  {
    bool Called = false;
    T.Open([&](const OperationResult & R)
    {
      CHECK_MSG(R.Ok, Describe(R));
      Called = true;
    });
    CHECK(WaitUntil([&]() { return Called; }, 30000));
    if (!T.Active())
    {
      std::printf("test_sync: cannot connect\n");
      return 1;
    }
  }

  Env E{&T, Scratch.filePath(QStringLiteral("local")), Scratch.filePath(QStringLiteral("remote"))};
  {
    // The server must see our filesystem
    QDir().mkpath(E.Remote);
    WriteFile(QDir(E.Remote).filePath(QStringLiteral("probe")), "x", T0);
    bool Called = false;
    bool Found = false;
    T.ReadDirectory(E.Remote, [&](const OperationResult & R, const QString &, const RemoteFileList & Files)
    {
      Called = true;
      Found = R.Ok && (Files.size() >= 1);
    });
    CHECK(WaitUntil([&]() { return Called; }, 30000));
    if (!Found)
    {
      std::printf("test_sync: SKIPPED (server does not run on this machine as this user)\n");
      return Failures > 0 ? 1 : 0;
    }
  }

  // ---------------------------------------------------------------- collect
  std::printf("[collect: remote]\n");
  BuildTrees(E);
  {
    auto C = Collect(E, smRemote, 0);
    CheckList(E, *C, {
      QStringLiteral("UploadUpdate newer_local.txt [x]"),
      QStringLiteral("UploadNew only_local.txt [x]"),
      QStringLiteral("DeleteRemote only_remote.txt"),
      QStringLiteral("UploadNew dir_local_only/ [x]"),
      QStringLiteral("DeleteRemote dir_remote_only/"),
      QStringLiteral("UploadUpdate common/inner_newer_local.txt [x]"),
      QStringLiteral("UploadNew common/inner_only_local.txt [x]"),
      QStringLiteral("DeleteRemote common/inner_only_remote.txt"),
    }, "remote");
    // sizes/times
    for (int I = 0; I < C->Count(); I++)
    {
      const SynchronizeChecklist::Item * Item = C->GetItem(I);
      if (Item->GetFileName() == QStringLiteral("newer_local.txt"))
      {
        CHECK(Item->Info1.Size == 6);
        CHECK(Item->Info2.Size == 6);
        CHECK(Item->Info1.Modification.toSecsSinceEpoch() == T0 + 100);
        CHECK(Item->Info2.Modification.toSecsSinceEpoch() == T0);
        CHECK(Item->HasRemoteFile);
      }
    }
  }

  std::printf("[collect: remote, delete]\n");
  {
    auto C = Collect(E, smRemote, spDelete);
    CheckList(E, *C, {
      QStringLiteral("UploadUpdate newer_local.txt [x]"),
      QStringLiteral("UploadNew only_local.txt [x]"),
      QStringLiteral("DeleteRemote only_remote.txt [x]"),
      QStringLiteral("UploadNew dir_local_only/ [x]"),
      QStringLiteral("DeleteRemote dir_remote_only/ [x]"),
      QStringLiteral("UploadUpdate common/inner_newer_local.txt [x]"),
      QStringLiteral("UploadNew common/inner_only_local.txt [x]"),
      QStringLiteral("DeleteRemote common/inner_only_remote.txt [x]"),
    }, "remote delete");
  }

  std::printf("[collect: remote, existing only]\n");
  {
    auto C = Collect(E, smRemote, spExistingOnly);
    CheckList(E, *C, {
      QStringLiteral("UploadUpdate newer_local.txt [x]"),
      QStringLiteral("UploadNew only_local.txt"),
      QStringLiteral("DeleteRemote only_remote.txt"),
      QStringLiteral("UploadNew dir_local_only/"),
      QStringLiteral("DeleteRemote dir_remote_only/"),
      QStringLiteral("UploadUpdate common/inner_newer_local.txt [x]"),
      QStringLiteral("UploadNew common/inner_only_local.txt"),
      QStringLiteral("DeleteRemote common/inner_only_remote.txt"),
    }, "remote existing only");
  }

  std::printf("[collect: remote, mirror, by size]\n");
  {
    auto C = Collect(E, smRemote, spMirror | spBySize);
    CheckList(E, *C, {
      QStringLiteral("UploadUpdate newer_local.txt [x]"),
      QStringLiteral("UploadUpdate newer_remote.txt [x]"),
      QStringLiteral("UploadUpdate size_diff.txt [x]"),
      QStringLiteral("UploadNew only_local.txt [x]"),
      QStringLiteral("DeleteRemote only_remote.txt"),
      QStringLiteral("UploadNew dir_local_only/ [x]"),
      QStringLiteral("DeleteRemote dir_remote_only/"),
      QStringLiteral("UploadUpdate common/inner_newer_local.txt [x]"),
      QStringLiteral("UploadNew common/inner_only_local.txt [x]"),
      QStringLiteral("DeleteRemote common/inner_only_remote.txt"),
    }, "remote mirror by size");
  }

  std::printf("[collect: local]\n");
  {
    auto C = Collect(E, smLocal, spDelete);
    CheckList(E, *C, {
      QStringLiteral("DownloadUpdate newer_remote.txt [x]"),
      QStringLiteral("DownloadNew only_remote.txt [x]"),
      QStringLiteral("DeleteLocal only_local.txt [x]"),
      QStringLiteral("DownloadNew dir_remote_only/ [x]"),
      QStringLiteral("DeleteLocal dir_local_only/ [x]"),
      QStringLiteral("DownloadNew common/inner_only_remote.txt [x]"),
      QStringLiteral("DeleteLocal common/inner_only_local.txt [x]"),
    }, "local delete");
  }

  std::printf("[collect: both]\n");
  {
    auto C = Collect(E, smBoth, 0);
    CheckList(E, *C, {
      QStringLiteral("UploadUpdate newer_local.txt [x]"),
      QStringLiteral("DownloadUpdate newer_remote.txt [x]"),
      QStringLiteral("UploadNew only_local.txt [x]"),
      QStringLiteral("DownloadNew only_remote.txt [x]"),
      QStringLiteral("UploadNew dir_local_only/ [x]"),
      QStringLiteral("DownloadNew dir_remote_only/ [x]"),
      QStringLiteral("UploadUpdate common/inner_newer_local.txt [x]"),
      QStringLiteral("UploadNew common/inner_only_local.txt [x]"),
      QStringLiteral("DownloadNew common/inner_only_remote.txt [x]"),
    }, "both");
  }

  std::printf("[collect: timestamps]\n");
  {
    auto C = Collect(E, smRemote, spTimestamp);
    CheckList(E, *C, {
      QStringLiteral("UploadUpdate newer_local.txt [x]"),
      QStringLiteral("UploadUpdate newer_remote.txt [x]"),
      QStringLiteral("UploadUpdate common/inner_newer_local.txt [x]"),
    }, "timestamps");
    // "Same size only"
    C = Collect(E, smRemote, spTimestamp | spBySize);
    CheckList(E, *C, {
      QStringLiteral("UploadUpdate newer_local.txt [x]"),
    }, "timestamps same size");
    C = Collect(E, smLocal, spTimestamp);
    CheckList(E, *C, {
      QStringLiteral("DownloadUpdate newer_local.txt [x]"),
      QStringLiteral("DownloadUpdate newer_remote.txt [x]"),
      QStringLiteral("DownloadUpdate common/inner_newer_local.txt [x]"),
    }, "timestamps local");
  }

  std::printf("[collect: size only, checksum]\n");
  {
    auto C = Collect(E, smRemote, spNotByTime | spBySize);
    CheckList(E, *C, {
      QStringLiteral("UploadUpdate newer_remote.txt [x]"),
      QStringLiteral("UploadUpdate size_diff.txt [x]"),
      QStringLiteral("UploadUpdate common/inner_newer_local.txt [x]"),
      QStringLiteral("UploadNew only_local.txt [x]"),
      QStringLiteral("DeleteRemote only_remote.txt"),
      QStringLiteral("UploadNew dir_local_only/ [x]"),
      QStringLiteral("DeleteRemote dir_remote_only/"),
      QStringLiteral("UploadNew common/inner_only_local.txt [x]"),
      QStringLiteral("DeleteRemote common/inner_only_remote.txt"),
    }, "size only");
    C = Collect(E, smRemote, spNotByTime | spByChecksum | spExistingOnly);
    CheckList(E, *C, {
      QStringLiteral("UploadUpdate newer_local.txt [x]"),
      QStringLiteral("UploadUpdate newer_remote.txt [x]"),
      QStringLiteral("UploadUpdate size_diff.txt [x]"),
      QStringLiteral("UploadUpdate common/inner_newer_local.txt [x]"),
      QStringLiteral("UploadNew only_local.txt"),
      QStringLiteral("DeleteRemote only_remote.txt"),
      QStringLiteral("UploadNew dir_local_only/"),
      QStringLiteral("DeleteRemote dir_remote_only/"),
      QStringLiteral("UploadNew common/inner_only_local.txt"),
      QStringLiteral("DeleteRemote common/inner_only_remote.txt"),
    }, "checksum");
  }

  std::printf("[collect: no recurse, subdirs]\n");
  {
    auto C = Collect(E, smRemote, spDelete | spNoRecurse);
    CheckList(E, *C, {
      QStringLiteral("UploadUpdate newer_local.txt [x]"),
      QStringLiteral("UploadNew only_local.txt [x]"),
      QStringLiteral("DeleteRemote only_remote.txt [x]"),
      QStringLiteral("UploadNew dir_local_only/"),
      QStringLiteral("DeleteRemote dir_remote_only/"),
    }, "no recurse");
    C = Collect(E, smRemote, spDelete | spNoRecurse | spSubDirs);
    CheckList(E, *C, {
      QStringLiteral("UploadUpdate newer_local.txt [x]"),
      QStringLiteral("UploadNew only_local.txt [x]"),
      QStringLiteral("DeleteRemote only_remote.txt [x]"),
      QStringLiteral("UploadNew dir_local_only/ [x]"),
      QStringLiteral("DeleteRemote dir_remote_only/ [x]"),
    }, "no recurse subdirs");
  }

  std::printf("[collect: selected only filter, masks]\n");
  {
    SynchronizeOptions Options;
    Options.UseFilter = true;
    Options.Filter = QStringList{QStringLiteral("ONLY_LOCAL.TXT"), QStringLiteral("common")};
    auto C = Collect(E, smRemote, spDelete, CopyParam(), Options);
    CheckList(E, *C, {
      QStringLiteral("UploadNew only_local.txt [x]"),
      QStringLiteral("UploadUpdate common/inner_newer_local.txt [x]"),
      QStringLiteral("UploadNew common/inner_only_local.txt [x]"),
      QStringLiteral("DeleteRemote common/inner_only_remote.txt [x]"),
    }, "filter");
    CopyParam Masked;
    Masked.IncludeFileMask = FileMasks(QStringLiteral("| *only*; inner_*"));
    C = Collect(E, smRemote, spDelete, Masked);
    // file masks do not apply to directories
    CheckList(E, *C, {
      QStringLiteral("UploadUpdate newer_local.txt [x]"),
      QStringLiteral("UploadNew dir_local_only/ [x]"),
      QStringLiteral("DeleteRemote dir_remote_only/ [x]"),
    }, "masks");
  }

  // ---------------------------------------------------------------- apply
  std::printf("[apply: remote mirror -> identical]\n");
  {
    auto C = Collect(E, smRemote, spDelete | spMirror | spBySize);
    SynchronizeStatistics S = Apply(E, C, spDelete | spMirror | spBySize);
    Log(QStringLiteral("uploaded %1 (%2 B), deleted remote %3").arg(S.FilesUploaded).arg(S.TotalUploaded).arg(S.FilesDeletedRemote));
    CHECK(S.FilesUploaded == 8);
    CHECK(S.FilesDeletedRemote >= 3);
    // the type mismatch is never synchronized
    CHECK(SameTrees(E.Local, E.Remote, true, {QStringLiteral("mismatch"), QStringLiteral("mismatch/x.txt"), QStringLiteral("tolerance.txt")}));
    CHECK(Collect(E, smRemote, spDelete | spMirror | spBySize)->Count() == 0);
  }

  std::printf("[apply: local mirror -> identical]\n");
  BuildTrees(E);
  {
    auto C = Collect(E, smLocal, spDelete | spMirror | spBySize);
    SynchronizeStatistics S = Apply(E, C, spDelete | spMirror | spBySize);
    Log(QStringLiteral("downloaded %1 (%2 B), deleted local %3").arg(S.FilesDownloaded).arg(S.TotalDownloaded).arg(S.FilesDeletedLocal));
    CHECK(S.FilesDownloaded == 7);
    CHECK(S.FilesDeletedLocal == 3);
    CHECK(SameTrees(E.Local, E.Remote, true, {QStringLiteral("mismatch"), QStringLiteral("mismatch/x.txt"), QStringLiteral("tolerance.txt")}));
  }

  std::printf("[apply: both]\n");
  BuildTrees(E);
  {
    auto C = Collect(E, smBoth, 0);
    Apply(E, C, 0);
    CHECK(SameTrees(E.Local, E.Remote, true, {QStringLiteral("mismatch"), QStringLiteral("mismatch/x.txt"),
      QStringLiteral("tolerance.txt"), QStringLiteral("size_diff.txt")}));
    CHECK(ReadAll(QDir(E.Local).filePath(QStringLiteral("newer_remote.txt"))) == "BBBBBB");
    CHECK(ReadAll(QDir(E.Remote).filePath(QStringLiteral("newer_local.txt"))) == "LOCAL1");
    CHECK(ReadAll(QDir(E.Local).filePath(QStringLiteral("size_diff.txt"))) == "aaaa");
    CHECK(Collect(E, smBoth, 0)->Count() == 0);
  }

  std::printf("[apply: unchecked and reversed items]\n");
  BuildTrees(E);
  {
    auto C = Collect(E, smRemote, spDelete);
    for (int I = 0; I < C->Count(); I++)
    {
      const SynchronizeChecklist::Item * Item = C->GetItem(I);
      if (Item->GetFileName() == QStringLiteral("only_remote.txt"))
      {
        // reverse: download instead of deleting
        C->Update(Item, true, SynchronizeChecklist::Reverse(Item->Action));
      }
      else if (Item->GetFileName() == QStringLiteral("dir_local_only"))
      {
        C->Update(Item, false, Item->Action);
      }
    }
    Apply(E, C, spDelete);
    CHECK(QFile::exists(QDir(E.Local).filePath(QStringLiteral("only_remote.txt"))));
    CHECK(QFile::exists(QDir(E.Remote).filePath(QStringLiteral("only_remote.txt"))));
    CHECK(!QFile::exists(QDir(E.Remote).filePath(QStringLiteral("dir_local_only"))));
    CHECK(!QFile::exists(QDir(E.Remote).filePath(QStringLiteral("dir_remote_only"))));
    CHECK(ReadAll(QDir(E.Remote).filePath(QStringLiteral("newer_remote.txt"))) == "BBBBBB");
  }

  std::printf("[apply: timestamps]\n");
  BuildTrees(E);
  {
    auto C = Collect(E, smRemote, spTimestamp);
    Apply(E, C, spTimestamp);
    CHECK(MTime(QDir(E.Remote).filePath(QStringLiteral("newer_local.txt"))) == T0 + 100);
    CHECK(MTime(QDir(E.Remote).filePath(QStringLiteral("newer_remote.txt"))) == T0);
    CHECK(ReadAll(QDir(E.Remote).filePath(QStringLiteral("newer_local.txt"))) == "REMOT1");
    CHECK(!QFile::exists(QDir(E.Remote).filePath(QStringLiteral("only_local.txt"))));
    C = Collect(E, smLocal, spTimestamp);
    CHECK(C->Count() == 0);
  }

  std::printf("[apply: masks and resume threshold honoured]\n");
  BuildTrees(E);
  {
    QByteArray Big(300 * 1024, 'b');
    WriteFile(QDir(E.Local).filePath(QStringLiteral("big.bin")), Big, T0);
    CopyParam Param;
    Param.IncludeFileMask = FileMasks(QStringLiteral("| *.txt"));
    auto C = Collect(E, smRemote, 0, Param);
    CheckList(E, *C, {
      QStringLiteral("UploadNew big.bin [x]"),
      QStringLiteral("UploadNew dir_local_only/ [x]"),
      QStringLiteral("DeleteRemote dir_remote_only/"),
    }, "mask *.txt excluded");
    Apply(E, C, 0, Param);
    CHECK(Sha256(QDir(E.Remote).filePath(QStringLiteral("big.bin"))) == Sha256(QDir(E.Local).filePath(QStringLiteral("big.bin"))));
    CHECK(QFileInfo(QDir(E.Remote).filePath(QStringLiteral("dir_local_only/sub"))).isDir());
    // the mask applies inside new directories as well
    CHECK(!QFile::exists(QDir(E.Remote).filePath(QStringLiteral("dir_local_only/f1.txt"))));
    CHECK(!QFile::exists(QDir(E.Remote).filePath(QStringLiteral("big.bin") + PartialExt)));
  }

  std::printf("[cancel]\n");
  BuildTrees(E);
  {
    auto Status = std::make_shared<SynchronizeStatus>();
    Status->Cancel();
    bool Called = false;
    SynchronizeCollect(&T, E.Local, E.Remote, smRemote, CopyParam(), 0, SynchronizeOptions(), Status,
      [&](const OperationResult & R, SynchronizeChecklistPtr Checklist)
      {
        CHECK(!R.Ok && R.Aborted);
        CHECK(!Checklist);
        Called = true;
      });
    CHECK(WaitUntil([&]() { return Called; }, 30000));
  }

  std::printf("[errors]\n");
  {
    bool Called = false;
    SynchronizeCollect(&T, E.Local, E.Remote + QStringLiteral("/nonexistent"), smRemote, CopyParam(), 0, SynchronizeOptions(), nullptr,
      [&](const OperationResult & R, SynchronizeChecklistPtr)
      {
        CHECK_MSG(!R.Ok && !R.Fatal && !R.Aborted, Describe(R));
        Log(QStringLiteral("error: %1").arg(R.Message));
        Called = true;
      });
    CHECK(WaitUntil([&]() { return Called; }, 30000));
    CHECK(T.Active());
  }

  // ---------------------------------------------------------------- keep up to date
  std::printf("[keep remote directory up to date]\n");
  BuildTrees(E);
  {
    SynchronizeController Controller(&T);
    QStringList LogLines;
    bool Aborted = false;
    QObject::connect(&Controller, &SynchronizeController::Log, [&](SynchronizeLogEntry Entry, const QString & Message, const QString & More)
    {
      LogLines.append(QStringLiteral("%1 %2").arg(Entry).arg(Message));
      Log(QStringLiteral("log %1: %2 %3").arg(Entry).arg(Message, More));
    });
    QObject::connect(&Controller, &SynchronizeController::Aborted, [&](bool Close, const QString & Message, const QString &)
    {
      Log(QStringLiteral("aborted close=%1 %2").arg(Close).arg(Message));
      Aborted = true;
    });
    SynchronizeParamType Params;
    Params.LocalDirectory = E.Local;
    Params.RemoteDirectory = E.Remote;
    Params.Params = spDelete;
    Params.Options = soRecurse | soSynchronize;
    Controller.SetChangeDelay(200);
    Controller.Start(Params, CopyParam(), SynchronizeOptions());
    auto Logged = [&](const QString & Prefix) { return LogLines.filter(Prefix).size(); };
    CHECK(WaitUntil([&]() { return Logged(QStringLiteral("Watching for changes")) > 0; }, 30000));
    CHECK(Controller.Active());
    CHECK(!Aborted);
    // initial synchronization: remote now matches local, except newer remote files
    // (keep up to date does not mirror) and the type mismatch
    CHECK(SameTrees(E.Local, E.Remote, false, {QStringLiteral("mismatch"), QStringLiteral("mismatch/x.txt"),
      QStringLiteral("tolerance.txt"), QStringLiteral("newer_remote.txt"), QStringLiteral("size_diff.txt")}));
    Log(QStringLiteral("watching %1 directories").arg(Controller.Directories()));
    CHECK(Controller.Directories() == 4); // local, common, dir_local_only, dir_local_only/sub

    QString L = E.Local;
    QString R = E.Remote;
    auto RemoteHas = [&](const QString & Name, const QByteArray & Content)
    {
      return ReadAll(QDir(R).filePath(Name)) == Content;
    };
    // modify
    QElapsedTimer Timer;
    Timer.start();
    WriteFile(QDir(L).filePath(QStringLiteral("same.txt")), "modified", QDateTime::currentSecsSinceEpoch());
    CHECK(WaitUntil([&]() { return RemoteHas(QStringLiteral("same.txt"), "modified"); }, 5000));
    Log(QStringLiteral("modification reflected in %1 ms").arg(Timer.elapsed()));
    // new file in subdirectory
    WriteFile(QDir(L).filePath(QStringLiteral("common/created.txt")), "created", QDateTime::currentSecsSinceEpoch());
    CHECK(WaitUntil([&]() { return RemoteHas(QStringLiteral("common/created.txt"), "created"); }, 5000));
    // new directory tree, then a file in it once watched
    QDir().mkpath(QDir(L).filePath(QStringLiteral("newdir/deep")));
    WriteFile(QDir(L).filePath(QStringLiteral("newdir/deep/n.txt")), "n", QDateTime::currentSecsSinceEpoch());
    CHECK(WaitUntil([&]() { return RemoteHas(QStringLiteral("newdir/deep/n.txt"), "n"); }, 5000));
    CHECK(WaitUntil([&]() { return !Controller.Busy(); }, 5000));
    WriteFile(QDir(L).filePath(QStringLiteral("newdir/deep/m.txt")), "m", QDateTime::currentSecsSinceEpoch());
    CHECK(WaitUntil([&]() { return RemoteHas(QStringLiteral("newdir/deep/m.txt"), "m"); }, 5000));
    // delete
    QFile::remove(QDir(L).filePath(QStringLiteral("only_local.txt")));
    CHECK(WaitUntil([&]() { return !QFile::exists(QDir(R).filePath(QStringLiteral("only_local.txt"))); }, 5000));
    QDir(QDir(L).filePath(QStringLiteral("dir_local_only"))).removeRecursively();
    CHECK(WaitUntil([&]() { return !QFile::exists(QDir(R).filePath(QStringLiteral("dir_local_only"))); }, 5000));
    // rename
    QFile::rename(QDir(L).filePath(QStringLiteral("common/created.txt")), QDir(L).filePath(QStringLiteral("common/renamed.txt")));
    CHECK(WaitUntil([&]() { return RemoteHas(QStringLiteral("common/renamed.txt"), "created") &&
      !QFile::exists(QDir(R).filePath(QStringLiteral("common/created.txt"))); }, 5000));
    CHECK(Logged(QStringLiteral("uploaded.")) >= 4);
    CHECK(Logged(QStringLiteral("deleted.")) >= 2);
    CHECK(!Aborted);

    // stop: changes are no longer reflected
    Controller.Stop();
    CHECK(!Controller.Active());
    WriteFile(QDir(L).filePath(QStringLiteral("after_stop.txt")), "x", QDateTime::currentSecsSinceEpoch());
    WaitUntil([]() { return false; }, 1000);
    CHECK(!QFile::exists(QDir(R).filePath(QStringLiteral("after_stop.txt"))));

    // existing files only, no delete, no recursion, no initial synchronization
    Params.Params = spExistingOnly;
    Params.Options = 0;
    LogLines.clear();
    Controller.Start(Params, CopyParam(), SynchronizeOptions());
    CHECK(WaitUntil([&]() { return Logged(QStringLiteral("Watching for changes in 1 directories")) > 0; }, 10000));
    WriteFile(QDir(L).filePath(QStringLiteral("brand_new.txt")), "bn", QDateTime::currentSecsSinceEpoch());
    WriteFile(QDir(L).filePath(QStringLiteral("newer_local.txt")), "updated", QDateTime::currentSecsSinceEpoch());
    QFile::remove(QDir(L).filePath(QStringLiteral("same.txt")));
    CHECK(WaitUntil([&]() { return RemoteHas(QStringLiteral("newer_local.txt"), "updated"); }, 5000));
    CHECK(WaitUntil([&]() { return !Controller.Busy(); }, 5000));
    CHECK(!QFile::exists(QDir(R).filePath(QStringLiteral("brand_new.txt"))));
    CHECK(QFile::exists(QDir(R).filePath(QStringLiteral("same.txt"))));
    WriteFile(QDir(L).filePath(QStringLiteral("common/inner_same.txt")), "not watched", QDateTime::currentSecsSinceEpoch());
    WaitUntil([]() { return false; }, 1000);
    CHECK(RemoteHas(QStringLiteral("common/inner_same.txt"), "is"));

    // the watched directory disappears
    Aborted = false;
    QDir(L).removeRecursively();
    CHECK(WaitUntil([&]() { return Aborted; }, 5000));
    CHECK(!Controller.Active());
  }
  CHECK(T.Active());

  if (Failures > 0)
  {
    std::printf("test_sync: %d FAILURE(S)\n", Failures);
    return 1;
  }
  std::printf("test_sync: OK\n");
  return 0;
}
