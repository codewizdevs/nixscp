// Tests of the server side operations of the Properties dialog and the
// Files menu (port of WinSCP core/Terminal.cpp ChangeFilesProperties,
// CalculateFilesSize, CalculateFilesChecksum, CopyFiles, MoveFiles,
// CreateLink, SpaceAvailable, LookupUsersGroups and the protocol parts in
// core/SftpFileSystem.cpp / core/ScpFileSystem.cpp): tri-state permissions
// (only the changed bits on each file), chown/chgrp, recursive changes with
// "Add X to directories", directory sizes, checksums (SFTP secondary shell,
// SCP shell), Duplicate (copy-data and "cp"), Move To with masks, symbolic
// and hard links, space available, Create Directory with permissions.
//
// The unit parts always run. The server parts are skipped unless
// NIXSCP_TEST_SSH_PORT and NIXSCP_TEST_SSH_KEY are set; the server must run
// on this machine (remote paths are checked locally).
#include "core/Terminal.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTimer>

#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>

static int Failures = 0;

#define CHECK(Cond) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); std::fflush(stdout); Failures++; } } while (0)
#define CHECK_MSG(Cond, Msg) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, #Cond, qPrintable(Msg)); std::fflush(stdout); Failures++; } } while (0)

class TestUserInterface : public UserInterface
{
public:
  QList<unsigned int> QueryAnswers;
  QStringList Queries;

  HostKeyAnswer VerifyHostKey(const QString &, int, const QString &, const QString &, const QString &, HostKeyState) override
  {
    return HostKeyOnce;
  }
  bool PromptUser(PromptKind, const QString &, const QString &, QList<Prompt> &) override { return false; }
  void DisplayBanner(const QString &, const QString &) override {}
  unsigned int QueryUser(const QString & Query, const QString & More, unsigned int) override
  {
    Queries.append(Query);
    std::printf("  query: %s | %s\n", qPrintable(Query.left(100)), qPrintable(More.left(200)));
    return QueryAnswers.isEmpty() ? static_cast<unsigned int>(qaAbort) : QueryAnswers.takeFirst();
  }
};

struct Waiter
{
  QEventLoop Loop;
  OperationResult Result;
  bool Called = false;
  Terminal::Callback Cb()
  {
    return [this](const OperationResult & R) { Result = R; Called = true; Loop.quit(); };
  }
  OperationResult Wait()
  {
    if (!Called)
    {
      QTimer::singleShot(120000, &Loop, [this]() { std::printf("FAIL: operation timed out\n"); Failures++; Loop.quit(); });
      Loop.exec();
    }
    return Result;
  }
};

static QString Describe(const OperationResult & R)
{
  return QStringLiteral("ok=%1 aborted=%2 msg=\"%3\" more=\"%4\"").arg(R.Ok).arg(R.Aborted).arg(R.Message, R.MoreMessages);
}

static void WriteFile(const QString & FileName, const QByteArray & Data, unsigned int Mode)
{
  QFile F(FileName);
  CHECK(F.open(QIODevice::WriteOnly | QIODevice::Truncate));
  F.write(Data);
  F.close();
  chmod(QFile::encodeName(FileName).constData(), Mode);
}

static unsigned int ModeOf(const QString & FileName)
{
  struct stat St = {};
  if (lstat(QFile::encodeName(FileName).constData(), &St) != 0)
  {
    return 0xFFFFFFFF;
  }
  return St.st_mode & 07777;
}

static QByteArray Contents(const QString & FileName)
{
  QFile F(FileName);
  return F.open(QIODevice::ReadOnly) ? F.readAll() : QByteArray();
}

static void UnitTests()
{
  std::printf("[RemoteProperties]\n");
  RemoteProperties P;
  P.ValidRights = true;
  P.RightsSet = 0002;
  // Others W ticked on a mixed selection: other bits stay
  CHECK(P.CombineRights(0644, false) == 0646);
  CHECK(P.CombineRights(0755, false) == 0757);
  CHECK(!P.RightsDefined());
  CHECK(P.ChmodStr(false) == QStringLiteral("o+w"));
  P.RightsSet = 0644;
  P.RightsUnset = 07133;
  CHECK(P.RightsDefined());
  CHECK(P.CombineRights(0777, false) == 0644);
  CHECK(P.ChmodStr(false) == QStringLiteral("u+rw-xs,g+r-wxs,o+r-wxt"));
  P.AddXToDirectories = true;
  CHECK(P.CombineRights(0, true) == 0755);
  CHECK(P.CombineRights(0, false) == 0644);
  CHECK(P.ChmodStr(true) == QStringLiteral("u+rwx-s,g+rx-ws,o+rx-wt"));
  RemoteProperties Empty;
  CHECK(Empty.Empty());
  CHECK(Empty.ChmodStr(false).isEmpty());

  std::printf("[checksum commands]\n");
  CHECK(Terminal::ShellChecksumCommand(QStringLiteral("sha-256")) == QStringLiteral("sha256sum"));
  CHECK(Terminal::ShellChecksumCommand(QStringLiteral("SHA1")) == QStringLiteral("sha1sum"));
  CHECK(Terminal::ShellChecksumCommand(QStringLiteral("md5")) == QStringLiteral("md5sum"));
  CHECK(Terminal::ShellChecksumFallback(QStringLiteral("sha256sum")) == QStringLiteral("shasum -a 256"));
  CHECK(Terminal::ShellChecksumFallback(QStringLiteral("sha1sum")) == QStringLiteral("shasum -a 1"));
  CHECK(Terminal::ShellChecksumFallback(QStringLiteral("md5sum")) == QStringLiteral("md5 -r"));
  CHECK(Terminal::ShellChecksumFallback(QStringLiteral("xxhsum")).isEmpty());
  CHECK(Terminal::ShellChecksumCommand(QStringLiteral("whirlpool")).isEmpty());
  CHECK(Terminal::ShellChecksumAlgs().first() == QStringLiteral("sha-512"));
}

static void ServerTests(const SessionData & AData, TFSProtocol Protocol, const QString & Base)
{
  bool Sftp = (Protocol != fsSCPonly);
  std::printf("[%s]\n", Sftp ? "SFTP" : "SCP");
  SessionData Data = AData;
  Data.FSProtocol = Protocol;
  QDir().mkpath(Base);
  TestUserInterface UI;
  Terminal T(Data, &UI);
  {
    Waiter W;
    T.Open(W.Cb());
    OperationResult R = W.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    if (!R.Ok)
    {
      return;
    }
  }

  // capabilities
  CHECK(T.IsCapable(fcOwnerChanging));
  CHECK(T.IsCapable(fcGroupChanging));
  CHECK(T.IsCapable(fcHardLink));
  CHECK(T.IsCapable(fcRemoteCopy));
  CHECK(T.IsCapable(fcCheckingSpaceAvailable) == Sftp);
  CHECK(T.SupportedChecksumAlgs().contains(QStringLiteral("sha-256")));
  CHECK(T.AllowDirectRemoteCopy(false) == Terminal::drcAllow);
  CHECK(T.AllowDirectRemoteCopy(true) == (Sftp ? Terminal::drcConfirmCommandSessionDirs : Terminal::drcAllow));

  const QString A = Base + QStringLiteral("/a.txt");
  const QString Run = Base + QStringLiteral("/run.sh");
  const QString D = Base + QStringLiteral("/d");
  const QString Sub = D + QStringLiteral("/sub.txt");
  WriteFile(A, QByteArrayLiteral("hello\n"), 0644);
  WriteFile(Run, QByteArrayLiteral("#!/bin/sh\necho run\n"), 0755);
  QDir().mkpath(D);
  chmod(QFile::encodeName(D).constData(), 0700);
  WriteFile(Sub, QByteArray(1000, 'x'), 0600);

  // ---- tri-state permissions: Others W on a.txt 0644 + run.sh 0755
  {
    RemoteProperties P;
    P.ValidRights = true;
    P.RightsSet = 0002;
    Waiter W;
    T.ChangeFilesProperties({A, Run}, P, W.Cb());
    OperationResult R = W.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    CHECK_MSG(ModeOf(A) == 0646, Rights::ToOctal(ModeOf(A)));
    CHECK_MSG(ModeOf(Run) == 0757, Rights::ToOctal(ModeOf(Run)));
  }
  {
    // clear Others R and W, keep the rest
    RemoteProperties P;
    P.ValidRights = true;
    P.RightsUnset = 0006;
    Waiter W;
    T.ChangeFilesProperties({A, Run}, P, W.Cb());
    CHECK(W.Wait().Ok);
    CHECK_MSG(ModeOf(A) == 0640, Rights::ToOctal(ModeOf(A)));
    CHECK_MSG(ModeOf(Run) == 0751, Rights::ToOctal(ModeOf(Run)));
  }
  // ---- recursive, defined rights with Add X to directories
  {
    RemoteProperties P;
    P.ValidRights = true;
    P.RightsSet = 0644;
    P.RightsUnset = 07133;
    P.AddXToDirectories = true;
    P.Recursive = true;
    Waiter W;
    T.ChangeFilesProperties({D}, P, W.Cb());
    OperationResult R = W.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    CHECK_MSG(ModeOf(D) == 0755, Rights::ToOctal(ModeOf(D)));
    CHECK_MSG(ModeOf(Sub) == 0644, Rights::ToOctal(ModeOf(Sub)));
  }
  // ---- owner and group (the user's own; chown to others needs root)
  {
    struct passwd * Pw = getpwuid(getuid());
    struct group * Gr = getgrgid(getgid());
    CHECK((Pw != nullptr) && (Gr != nullptr));
    if ((Pw != nullptr) && (Gr != nullptr))
    {
      // BSD: a new file gets the group of its folder, on macOS one of /tmp that
      // the user does not need to be in. The own group is the one tested
      CHECK(chown(QFile::encodeName(A).constData(), static_cast<uid_t>(-1), getgid()) == 0);
      // the listing makes the names known (SFTP v3 changes ownership by ID)
      QEventLoop Loop;
      T.ReadDirectory(Base, [&](const OperationResult &, const QString &, const RemoteFileList &) { Loop.quit(); });
      Loop.exec();
      RemoteProperties P;
      P.Owner = QString::fromLocal8Bit(Pw->pw_name);
      P.Group = QString::fromLocal8Bit(Gr->gr_name);
      Waiter W;
      T.ChangeFilesProperties({A}, P, W.Cb());
      OperationResult R = W.Wait();
      CHECK_MSG(R.Ok, Describe(R));
      // by ID
      RemoteProperties ById;
      ById.Group = QStringLiteral("%1 [%2]").arg(P.Group).arg(getgid());
      Waiter W2;
      T.ChangeFilesProperties({A}, ById, W2.Cb());
      R = W2.Wait();
      CHECK_MSG(R.Ok, Describe(R));
      // unknown user: error, answered with Abort
      RemoteProperties Bad;
      Bad.Owner = QStringLiteral("no-such-user-nixscp");
      Waiter W3;
      T.ChangeFilesProperties({A}, Bad, W3.Cb());
      R = W3.Wait();
      CHECK(!R.Ok);
      CHECK(!UI.Queries.isEmpty() && UI.Queries.last().contains(QStringLiteral("Cannot change properties of file")));
      // the mode is untouched by owner/group changes
      CHECK_MSG(ModeOf(A) == 0640, Rights::ToOctal(ModeOf(A)));

      Waiter W4;
      QStringList Users, Groups;
      T.LookupUsersGroups([&](const OperationResult & Result, const QStringList & U, const QStringList & G)
      {
        Users = U;
        Groups = G;
        W4.Result = Result;
        W4.Called = true;
        W4.Loop.quit();
      });
      CHECK(W4.Wait().Ok);
      CHECK_MSG(Users.contains(P.Owner), Users.join(QLatin1Char(',')));
      CHECK_MSG(Groups.contains(P.Group), Groups.join(QLatin1Char(',')));
    }
  }
  // ---- calculate size
  {
    Waiter W;
    qint64 Size = -1;
    Terminal::CalculateSizeStats Stats;
    QList<qint64> Sizes;
    T.CalculateFilesSize({D, A}, [&](const OperationResult & R, qint64 ASize, const Terminal::CalculateSizeStats & AStats,
      const QList<qint64> & ASizes)
    {
      W.Result = R;
      Size = ASize;
      Stats = AStats;
      Sizes = ASizes;
      W.Called = true;
      W.Loop.quit();
    });
    CHECK(W.Wait().Ok);
    CHECK(Size == 1006);
    CHECK(Stats.Files == 2);
    CHECK(Stats.Directories == 1);
    CHECK((Sizes.size() == 2) && (Sizes[0] == 1000) && (Sizes[1] == 6));
  }
  // ---- checksums (shell sha256sum)
  {
    QMap<QString, QString> Checksums;
    QString UsedAlg;
    Waiter W;
    T.CalculateFilesChecksum(QStringLiteral("SHA256"), {A, D},
      [&](const QString & Name, const QString & Alg, const QString & Hash) { Checksums.insert(Name, Hash); UsedAlg = Alg; },
      W.Cb());
    OperationResult R = W.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(UsedAlg == QStringLiteral("sha-256"));
    CHECK(Checksums.value(QStringLiteral("a.txt")) ==
      QString::fromLatin1(QCryptographicHash::hash(Contents(A), QCryptographicHash::Sha256).toHex()));
    CHECK_MSG(Checksums.value(QStringLiteral("d/sub.txt")) ==
      QString::fromLatin1(QCryptographicHash::hash(Contents(Sub), QCryptographicHash::Sha256).toHex()),
      QStringList(Checksums.keys()).join(QLatin1Char(',')));
  }
  // ---- Duplicate
  const QString CopyDir = Base + QStringLiteral("/copy");
  QDir().mkpath(CopyDir);
  {
    Waiter W;
    T.CopyFiles({A, D, Run}, CopyDir, QStringLiteral("*.*"), W.Cb());
    OperationResult R = W.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(Contents(CopyDir + QStringLiteral("/a.txt")) == Contents(A));
    CHECK(Contents(CopyDir + QStringLiteral("/d/sub.txt")) == Contents(Sub));
    CHECK_MSG(ModeOf(CopyDir + QStringLiteral("/run.sh")) == 0751, Rights::ToOctal(ModeOf(CopyDir + QStringLiteral("/run.sh"))));
    CHECK(QFileInfo(CopyDir + QStringLiteral("/a.txt")).lastModified().toSecsSinceEpoch() ==
      QFileInfo(A).lastModified().toSecsSinceEpoch());
  }
  {
    // mask, and overwrite confirmation (Yes)
    UI.QueryAnswers = {qaYes};
    UI.Queries.clear();
    WriteFile(CopyDir + QStringLiteral("/a.bak"), QByteArrayLiteral("old"), 0644);
    Waiter W;
    T.CopyFiles({A}, CopyDir, QStringLiteral("*.bak"), W.Cb());
    OperationResult R = W.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(UI.Queries.size() == 1);
    CHECK(Contents(CopyDir + QStringLiteral("/a.bak")) == Contents(A));
  }
  // ---- Move To with mask
  {
    const QString MoveDir = Base + QStringLiteral("/moved");
    QDir().mkpath(MoveDir);
    Waiter W;
    T.MoveFiles({CopyDir + QStringLiteral("/a.txt"), CopyDir + QStringLiteral("/d")}, MoveDir, QStringLiteral("*.*"), W.Cb());
    OperationResult R = W.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    CHECK(QFileInfo::exists(MoveDir + QStringLiteral("/a.txt")));
    CHECK(QFileInfo::exists(MoveDir + QStringLiteral("/d/sub.txt")));
    CHECK(!QFileInfo::exists(CopyDir + QStringLiteral("/a.txt")));
  }
  // ---- links
  {
    Waiter W;
    T.CreateLink(Base + QStringLiteral("/hard"), QStringLiteral("a.txt"), false, W.Cb());
    OperationResult R = W.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    struct stat S1 = {}, S2 = {};
    stat(QFile::encodeName(A).constData(), &S1);
    CHECK(lstat(QFile::encodeName(Base + QStringLiteral("/hard")).constData(), &S2) == 0);
    CHECK(S1.st_ino == S2.st_ino);
    Waiter W2;
    T.CreateLink(Base + QStringLiteral("/soft"), QStringLiteral("a.txt"), true, W2.Cb());
    CHECK(W2.Wait().Ok);
    CHECK(QFileInfo(Base + QStringLiteral("/soft")).isSymLink());
    CHECK(QFileInfo(Base + QStringLiteral("/soft")).symLinkTarget() == QFileInfo(A).absoluteFilePath());
  }
  // ---- space available
  {
    Waiter W;
    SpaceAvailableInfo Space;
    T.SpaceAvailable(Base, [&](const OperationResult & R, const SpaceAvailableInfo & S)
    {
      W.Result = R;
      Space = S;
      W.Called = true;
      W.Loop.quit();
    });
    OperationResult R = W.Wait();
    CHECK(R.Ok == Sftp);
    if (Sftp)
    {
      CHECK(Space.BytesOnDevice > 0);
      CHECK(Space.UnusedBytesAvailableToUser > 0);
      CHECK(Space.BytesPerAllocationUnit > 0);
    }
  }
  // ---- create directory with permissions
  {
    RemoteProperties P;
    P.ValidRights = true;
    P.RightsSet = 0710;
    P.RightsUnset = 07067;
    Waiter W;
    T.CreateDirectory(Base + QStringLiteral("/newdir"), P, W.Cb());
    OperationResult R = W.Wait();
    CHECK_MSG(R.Ok, Describe(R));
    CHECK_MSG(ModeOf(Base + QStringLiteral("/newdir")) == 0710, Rights::ToOctal(ModeOf(Base + QStringLiteral("/newdir"))));
  }
  T.Close();
}

int main(int argc, char ** argv)
{
  QCoreApplication App(argc, argv);
  UnitTests();

  QString PortStr = qEnvironmentVariable("NIXSCP_TEST_SSH_PORT");
  QString KeyFile = qEnvironmentVariable("NIXSCP_TEST_SSH_KEY");
  if (PortStr.isEmpty() || KeyFile.isEmpty())
  {
    std::printf("test_serverops: server parts SKIPPED (set NIXSCP_TEST_SSH_PORT, NIXSCP_TEST_SSH_KEY)\n");
  }
  else
  {
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
    ServerTests(Data, fsSFTPonly, Scratch.filePath(QStringLiteral("sftp")));
    ServerTests(Data, fsSCPonly, Scratch.filePath(QStringLiteral("scp")));
  }
  std::printf(Failures ? "test_serverops: %d FAILURES\n" : "test_serverops: OK\n", Failures);
  return Failures ? 1 : 0;
}
