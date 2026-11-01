// End-to-end tests of the SFTP/SCP site settings and performance paths of
// WP06 against a real (local) OpenSSH server: pipelined READDIR, pipelined
// symlink resolution and ResolveSymlinks, SFTPUploadQueue / SFTPDownloadQueue
// / SFTPMaxPacketSize, one SETSTAT for mode + time, UsePosixRename,
// SFTPSymlinkBug, Utf (legacy file names), SFTPSignedTSBug, the remote
// directory cache through Terminal, SCP batch transfers with T records,
// chmod -R, groups, and the SCP fallback of SFTP sessions.
//
// Skipped (exit 0) unless NIXSCP_TEST_SSH_PORT and NIXSCP_TEST_SSH_KEY are
// set (see test_sftp.cpp). The server must serve the local file system
// (results are checked with stat()). Optional:
//   NIXSCP_TEST_SSH_NOSFTP_PORT  an sshd of the same machine without SFTP
//                                subsystem (SCP fallback tests)
#include "core/DirectoryCache.h"
#include "core/ScpFileSystem.h"
#include "core/SftpFileSystem.h"
#include "core/Terminal.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
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

class TestUserInterface : public UserInterface
{
public:
  HostKeyAnswer VerifyHostKey(const QString &, int, const QString &, const QString &, const QString &, HostKeyState) override
  {
    return HostKeyAccept;
  }
  bool PromptUser(PromptKind, const QString &, const QString &, QList<Prompt> &) override { return false; }
  void DisplayBanner(const QString &, const QString &) override {}
  unsigned int QueryUser(const QString &, const QString &, unsigned int) override { return qaCancel; }
};

struct Waiter
{
  QEventLoop Loop;
  bool Called = false;
  OperationResult Result;
  QString Directory;
  RemoteFileList Files;
  Terminal::Callback Cb()
  {
    return [this](const OperationResult & R) { Result = R; Called = true; Loop.quit(); };
  }
  Terminal::ListCallback ListCb()
  {
    return [this](const OperationResult & R, const QString & Dir, const RemoteFileList & F)
    {
      Result = R; Directory = Dir; Files = F; Called = true; Loop.quit();
    };
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
  return QStringLiteral("ok=%1 msg=\"%2\" more=\"%3\"").arg(R.Ok).arg(R.Message, R.MoreMessages);
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

static void WriteFile(const QString & FileName, const QByteArray & Data, qint64 MTime = -1)
{
  QFile F(FileName);
  CHECK(F.open(QIODevice::WriteOnly | QIODevice::Truncate));
  F.write(Data);
  F.close();
  if (MTime >= 0)
  {
    struct timespec Times[2] = {{static_cast<time_t>(MTime), 0}, {static_cast<time_t>(MTime), 0}};
    utimensat(AT_FDCWD, QFile::encodeName(FileName).constData(), Times, AT_SYMLINK_NOFOLLOW);
  }
}

static QByteArray ReadAll(const QString & FileName)
{
  QFile F(FileName);
  return F.open(QIODevice::ReadOnly) ? F.readAll() : QByteArray();
}

static struct stat Stat(const QString & FileName)
{
  struct stat St = {};
  ::lstat(QFile::encodeName(FileName).constData(), &St);
  return St;
}

template<typename TFunc>
static bool Throws(TFunc Func, QString * Message = nullptr)
{
  try
  {
    Func();
  }
  catch (const ExtException & E)
  {
    if (Message != nullptr)
    {
      *Message = E.Message() + QLatin1Char('\n') + E.MoreMessages();
    }
    return true;
  }
  return false;
}

static void TestSftpListing(const SessionData & BaseData, TestUserInterface & UI, const QString & Root)
{
  std::printf("[sftp pipelined listing, symlinks]\n");
  QString Dir = Root + QStringLiteral("/big");
  QDir().mkpath(Dir);
  const int Count = 2500;
  for (int I = 0; I < Count; I++)
  {
    WriteFile(QStringLiteral("%1/file%2.txt").arg(Dir).arg(I, 5, 10, QLatin1Char('0')), QByteArray::number(I));
  }
  QDir().mkpath(Dir + QStringLiteral("/realdir"));
  const int LinkCount = 150;
  for (int I = 0; I < LinkCount; I++)
  {
    QString Target = (I % 2) ? QStringLiteral("realdir") : QStringLiteral("file00007.txt");
    CHECK(QFile::link(Target, QStringLiteral("%1/link%2").arg(Dir).arg(I)));
  }
  CHECK(QFile::link(QStringLiteral("nowhere"), Dir + QStringLiteral("/broken")));

  SftpFileSystem Fs(BaseData, &UI);
  Fs.Open();
  QElapsedTimer Timer;
  Timer.start();
  RemoteFileList Files = Fs.ReadDirectory(Dir);
  std::printf("  %lld entries listed in %lld ms\n", static_cast<long long>(Files.size()), static_cast<long long>(Timer.elapsed()));
  CHECK(Files.size() == Count + 1 + LinkCount + 1);
  const RemoteFile * F = Find(Files, QStringLiteral("file01234.txt"));
  CHECK((F != nullptr) && (F->Size == 4) && !F->IsDirectory && (F->FileType == RemoteFile::File));
  CHECK((F != nullptr) && (F->Modification.toSecsSinceEpoch() == QFileInfo(Dir + QStringLiteral("/file01234.txt")).lastModified().toSecsSinceEpoch()));
  CHECK((F != nullptr) && !F->Owner.isEmpty() && !F->Group.isEmpty());
  CHECK((F != nullptr) && (F->Permissions == (Stat(Dir + QStringLiteral("/file01234.txt")).st_mode & 07777)));
  CHECK(Find(Files, QStringLiteral(".")) == nullptr);
  CHECK(Find(Files, QStringLiteral("..")) == nullptr);
  const RemoteFile * DirLink = Find(Files, QStringLiteral("link1"));
  CHECK((DirLink != nullptr) && DirLink->IsSymLink && DirLink->IsDirectory && (DirLink->LinkTo == QStringLiteral("realdir")));
  const RemoteFile * FileLink = Find(Files, QStringLiteral("link0"));
  CHECK((FileLink != nullptr) && FileLink->IsSymLink && !FileLink->IsDirectory && (FileLink->LinkTo == QStringLiteral("file00007.txt")) &&
    (FileLink->Size == 1));
  const RemoteFile * Broken = Find(Files, QStringLiteral("broken"));
  CHECK((Broken != nullptr) && Broken->IsSymLink && !Broken->IsDirectory && (Broken->LinkTo == QStringLiteral("nowhere")));
  int Dirs = 0;
  for (const RemoteFile & File : Files)
  {
    Dirs += File.IsDirectory ? 1 : 0;
  }
  CHECK(Dirs == 1 + LinkCount / 2);
  // single file: lstat + readlink/stat
  RemoteFile Single = Fs.ReadFile(Dir + QStringLiteral("/link1"));
  CHECK(Single.IsSymLink && Single.IsDirectory && (Single.LinkTo == QStringLiteral("realdir")));
  RemoteFile Exists;
  CHECK(Fs.FileExists(Dir + QStringLiteral("/link0"), &Exists) && Exists.IsSymLink && !Exists.IsDirectory);
  // errors are reported and the session stays usable
  QString Message;
  CHECK(Throws([&]() { Fs.ReadDirectory(Dir + QStringLiteral("/missing")); }, &Message));
  CHECK_MSG(Message.startsWith(QStringLiteral("Error listing directory")) && Message.contains(QStringLiteral("No such file")), Message);
  CHECK(Throws([&]() { Fs.ReadDirectory(Dir + QStringLiteral("/file00001.txt")); }));
  CHECK(Fs.ReadDirectory(Dir + QStringLiteral("/realdir")).isEmpty());
  CHECK(Fs.CanonicalizePath(Dir + QStringLiteral("/link1")) == Dir + QStringLiteral("/realdir"));
  Fs.Close();

  std::printf("[sftp ResolveSymlinks off, SFTPListingQueue 1]\n");
  SessionData Data = BaseData;
  Data.ResolveSymlinks = false;
  Data.SFTPListingQueue = 1;
  SftpFileSystem Fs2(Data, &UI);
  Fs2.Open();
  Files = Fs2.ReadDirectory(Dir);
  CHECK(Files.size() == Count + 1 + LinkCount + 1);
  DirLink = Find(Files, QStringLiteral("link1"));
  CHECK((DirLink != nullptr) && DirLink->IsSymLink && !DirLink->IsDirectory && DirLink->LinkTo.isEmpty());
  Fs2.Close();
}

static void TestSftpSettings(const SessionData & BaseData, TestUserInterface & UI, const QString & Root)
{
  std::printf("[sftp transfer queues and packet size]\n");
  QString Dir = Root + QStringLiteral("/settings");
  QDir().mkpath(Dir);
  QByteArray Data(3 * 1024 * 1024 + 123, Qt::Uninitialized);
  for (qsizetype I = 0; I < Data.size(); I++)
  {
    Data[I] = static_cast<char>((I * 7919) >> 3);
  }
  WriteFile(Dir + QStringLiteral("/source.bin"), Data);
  for (int Variant = 0; Variant < 3; Variant++)
  {
    SessionData S = BaseData;
    S.SFTPUploadQueue = (Variant == 0) ? 1 : 64;
    S.SFTPDownloadQueue = (Variant == 0) ? 1 : 32;
    S.SFTPMaxPacketSize = (Variant == 1) ? 4096 : 0;
    if (Variant == 2)
    {
      S.SFTPUploadQueue = 7;
      S.SFTPDownloadQueue = 3;
      S.SFTPMaxPacketSize = 33000;
    }
    SftpFileSystem Fs(S, &UI);
    Fs.Open();
    FileOperationProgress Progress;
    Progress.Start(FileOperationProgress::Copy, FileOperationProgress::ToRemote, 1);
    QString Remote = Dir + QStringLiteral("/up%1.bin").arg(Variant);
    Fs.Upload(Dir + QStringLiteral("/source.bin"), Remote, 0644, Progress);
    CHECK(ReadAll(Remote) == Data);
    Progress.Start(FileOperationProgress::Copy, FileOperationProgress::ToLocal, 1);
    QString Local = Dir + QStringLiteral("/down%1.bin").arg(Variant);
    Fs.Download(Remote, Local, Progress);
    CHECK(ReadAll(Local) == Data);
    CHECK(Progress.Get().TotalTransferred == Data.size());
    // empty file (the EOF probe goes with the first burst)
    WriteFile(Dir + QStringLiteral("/empty"), QByteArray());
    Fs.Download(Dir + QStringLiteral("/empty"), Local, Progress);
    CHECK(QFileInfo(Local).exists() && (QFileInfo(Local).size() == 0));
    Fs.Close();
  }

  SftpFileSystem Fs(BaseData, &UI);
  Fs.Open();

  std::printf("[sftp one SETSTAT for mode and time]\n");
  QString Target = Dir + QStringLiteral("/props");
  WriteFile(Target, "x");
  QDateTime Time = QDateTime::fromSecsSinceEpoch(1234567890);
  Fs.SetFileProperties(Target, 0600, Time);
  CHECK((Stat(Target).st_mode & 07777) == 0600);
  CHECK(Stat(Target).st_mtime == 1234567890);
  Fs.SetFileProperties(Target, 0640, QDateTime());
  CHECK((Stat(Target).st_mode & 07777) == 0640);
  CHECK(Stat(Target).st_mtime == 1234567890);
  Fs.SetModificationTime(Target, QDateTime::fromSecsSinceEpoch(1300000000));
  CHECK((Stat(Target).st_mode & 07777) == 0640);
  CHECK(Stat(Target).st_mtime == 1300000000);
  QString Message;
  CHECK(Throws([&]() { Fs.SetFileProperties(Dir + QStringLiteral("/nonexistent"), 0600, Time); }, &Message));
  CHECK_MSG(Message.contains(QStringLiteral("error occurred while setting the permissions and/or timestamp")), Message);

  std::printf("[sftp UsePosixRename]\n");
  WriteFile(Dir + QStringLiteral("/ren-a"), "a");
  WriteFile(Dir + QStringLiteral("/ren-b"), "b");
  // plain SSH_FXP_RENAME does not replace an existing file
  CHECK(Throws([&]() { Fs.RenameFile(Dir + QStringLiteral("/ren-a"), Dir + QStringLiteral("/ren-b")); }, &Message));
  CHECK_MSG(Message.contains(QStringLiteral("Error renaming file")), Message);
  CHECK(ReadAll(Dir + QStringLiteral("/ren-b")) == "b");
  Fs.RenameFile(Dir + QStringLiteral("/ren-a"), Dir + QStringLiteral("/ren-c"));
  CHECK(ReadAll(Dir + QStringLiteral("/ren-c")) == "a");
  Fs.Close();
  {
    SessionData S = BaseData;
    S.UsePosixRename = true;
    SftpFileSystem Posix(S, &UI);
    Posix.Open();
    Posix.RenameFile(Dir + QStringLiteral("/ren-c"), Dir + QStringLiteral("/ren-b"));
    CHECK(ReadAll(Dir + QStringLiteral("/ren-b")) == "a");
    CHECK(!QFile::exists(Dir + QStringLiteral("/ren-c")));
    Posix.Close();
  }

  std::printf("[sftp SFTPSymlinkBug]\n");
  for (TAutoSwitch Bug : {asAuto, asOn})
  {
    SessionData S = BaseData;
    S.SFTPBug[sbSymlink] = Bug;
    SftpFileSystem LinkFs(S, &UI);
    LinkFs.Open();
    QString Link = Dir + QStringLiteral("/symlink%1").arg(static_cast<int>(Bug));
    LinkFs.CreateLink(Link, QStringLiteral("ren-b"));
    CHECK(QFileInfo(Link).isSymLink() && (QFile::symLinkTarget(Link) == Dir + QStringLiteral("/ren-b")));
    LinkFs.Close();
  }
  {
    // the workaround off against OpenSSH (which has the bug): arguments swapped
    SessionData S = BaseData;
    S.SFTPBug[sbSymlink] = asOff;
    SftpFileSystem LinkFs(S, &UI);
    LinkFs.Open();
    QString Link = Dir + QStringLiteral("/symlink-off");
    Throws([&]() { LinkFs.CreateLink(Link, Dir + QStringLiteral("/swapped-target")); });
    CHECK(QFileInfo(Dir + QStringLiteral("/swapped-target")).isSymLink());
    LinkFs.Close();
  }

  std::printf("[sftp Utf: legacy names]\n");
  {
    // "café" in Windows-1252 / Latin-1 bytes
    QByteArray LegacyName = QByteArray("caf\xE9.txt");
    QByteArray LocalPath = QFile::encodeName(Dir) + "/" + LegacyName;
    int Fd = ::open(LocalPath.constData(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    CHECK(Fd >= 0);
    CHECK(::write(Fd, "legacy", 6) == 6);
    ::close(Fd);
    SessionData S = BaseData;
    S.NotUtf = asOn;
    SftpFileSystem LegacyFs(S, &UI);
    LegacyFs.Open();
    RemoteFileList Files = LegacyFs.ReadDirectory(Dir);
    const RemoteFile * F = Find(Files, QStringLiteral("café.txt"));
    CHECK(F != nullptr);
    FileOperationProgress Progress;
    Progress.Start(FileOperationProgress::Copy, FileOperationProgress::ToLocal, 1);
    QString Local = Dir + QStringLiteral("/legacy-download");
    LegacyFs.Download(Dir + QStringLiteral("/café.txt"), Local, Progress);
    CHECK(ReadAll(Local) == "legacy");
    LegacyFs.Close();
    // auto: switches to legacy names once the server sends invalid UTF-8
    S.NotUtf = asAuto;
    SftpFileSystem AutoFs(S, &UI);
    AutoFs.Open();
    Files = AutoFs.ReadDirectory(Dir);
    CHECK(Find(Files, QStringLiteral("café.txt")) != nullptr);
    AutoFs.Close();
    ::unlink(LocalPath.constData());
  }

  std::printf("[sftp SFTPSignedTSBug]\n");
  {
    QString Old = Dir + QStringLiteral("/before1970");
    WriteFile(Old, "old", -86400 * 365);
    if (Stat(Old).st_mtime < 0)
    {
      SftpFileSystem TsFs(BaseData, &UI);
      TsFs.Open();
      RemoteFile F = TsFs.ReadFile(Old);
      CHECK(F.Modification.toSecsSinceEpoch() == -86400 * 365);
      TsFs.Close();
      SessionData S = BaseData;
      S.SFTPBug[sbSignedTS] = asOff;
      SftpFileSystem UnsignedFs(S, &UI);
      UnsignedFs.Open();
      F = UnsignedFs.ReadFile(Old);
      CHECK(F.Modification.toSecsSinceEpoch() == (static_cast<qint64>(1) << 32) - 86400 * 365);
      UnsignedFs.Close();
    }
    else
    {
      std::printf("  (file system without pre-1970 times, skipped)\n");
    }
  }
}

static void TestTerminalCache(const SessionData & BaseData, TestUserInterface & UI, const QString & Root, TFSProtocol Protocol)
{
  std::printf("[%s directory cache through Terminal]\n", (Protocol == fsSCPonly) ? "scp" : "sftp");
  QString Dir = Root + ((Protocol == fsSCPonly) ? QStringLiteral("/cache-scp") : QStringLiteral("/cache-sftp"));
  QDir().mkpath(Dir + QStringLiteral("/a"));
  QDir().mkpath(Dir + QStringLiteral("/b"));
  WriteFile(Dir + QStringLiteral("/a/one"), "1");
  SessionData Data = BaseData;
  Data.FSProtocol = Protocol;
  Data.PreserveDirectoryChanges = false;
  Data.RemoteDirectory = Dir + QStringLiteral("/a");
  Terminal T(Data, &UI);
  {
    Waiter W;
    T.Open(W.Cb());
    CHECK_MSG(W.Wait().Ok, Describe(W.Result));
  }
  auto List = [&](const QString & Path) -> RemoteFileList
  {
    Waiter W;
    T.ReadDirectory(Path, W.ListCb());
    CHECK_MSG(W.Wait().Ok, Describe(W.Result));
    return W.Files;
  };
  CHECK(Find(List(Dir + QStringLiteral("/a")), QStringLiteral("one")) != nullptr);
  CHECK(T.CurrentDirectory() == Dir + QStringLiteral("/a"));
  List(Dir + QStringLiteral("/b"));
  CHECK(T.CurrentDirectory() == Dir + QStringLiteral("/b"));
  // changed outside of the application: Back shows the cached listing ...
  WriteFile(Dir + QStringLiteral("/a/two"), "2");
  RemoteFileList Files = List(Dir + QStringLiteral("/a"));
  CHECK(Find(Files, QStringLiteral("two")) == nullptr);
  // ... until it is reloaded (the current directory requested again)
  Files = List(Dir + QStringLiteral("/a"));
  CHECK(Find(Files, QStringLiteral("two")) != nullptr);
  // changes made through any session of the server invalidate the cache
  List(Dir + QStringLiteral("/b"));
  {
    Terminal Other(Data, &UI);
    Waiter W;
    Other.Open(W.Cb());
    CHECK_MSG(W.Wait().Ok, Describe(W.Result));
    Waiter D;
    Other.DeleteFiles({Dir + QStringLiteral("/a/one")}, D.Cb());
    CHECK_MSG(D.Wait().Ok, Describe(D.Result));
  }
  Files = List(Dir + QStringLiteral("/a"));
  CHECK(Find(Files, QStringLiteral("one")) == nullptr);
  CHECK(Find(Files, QStringLiteral("two")) != nullptr);
  // Clear Caches (Commands menu) through the worker hook
  WriteFile(Dir + QStringLiteral("/a/three"), "3");
  List(Dir + QStringLiteral("/b"));
  {
    auto Empty = std::make_shared<bool>(true);
    Waiter W;
    T.RunOnWorker([Empty](FileSystem & Fs, FileOperationProgress &, UserInterface &)
    {
      *Empty = DirectoryCaching::AreCachesEmpty(Fs);
      DirectoryCaching::ClearCaches(Fs);
    }, W.Cb());
    CHECK(W.Wait().Ok);
    CHECK(!*Empty);
  }
  Files = List(Dir + QStringLiteral("/a"));
  CHECK(Find(Files, QStringLiteral("three")) != nullptr);
  T.Close();
}

static void TestScpBatch(const SessionData & BaseData, TestUserInterface & UI, const QString & Root)
{
  std::printf("[scp batch upload with T records]\n");
  QString Source = Root + QStringLiteral("/scp-src");
  QDir().mkpath(Source + QStringLiteral("/tree/sub/deeper"));
  QDir().mkpath(Source + QStringLiteral("/tree/empty"));
  const int Files = 40;
  for (int I = 0; I < Files; I++)
  {
    WriteFile(QStringLiteral("%1/tree/sub/f%2").arg(Source).arg(I), QByteArray(I * 37, 'a' + (I % 26)), 1500000000 + I);
  }
  WriteFile(Source + QStringLiteral("/tree/sub/deeper/name with spaces"), "spaces", 1400000000);
  WriteFile(Source + QStringLiteral("/single.txt"), "single", 1300000000);
  WriteFile(Source + QStringLiteral("/skipped.tmp"), "skip", 1300000000);
  ::chmod(QFile::encodeName(Source + QStringLiteral("/single.txt")).constData(), 0604);
  struct timespec DirTimes[2] = {{1200000000, 0}, {1200000000, 0}};
  utimensat(AT_FDCWD, QFile::encodeName(Source + QStringLiteral("/tree/sub")).constData(), DirTimes, 0);

  QString Target = Root + QStringLiteral("/scp-dst");
  QDir().mkpath(Target);
  SessionData Data = BaseData;
  Data.FSProtocol = fsSCPonly;
  ScpFileSystem Fs(Data, &UI);
  Fs.Open();
  ScpTransferOptions Options;
  Options.PreserveTime = true;
  Options.PreserveRights = true;
  Options.Include = [](const QString & Path, bool) { return !Path.endsWith(QStringLiteral(".tmp")); };
  QStringList Finished;
  Options.Finished = [&Finished](const QString & SourcePath, const QString &, bool) { Finished.append(SourcePath); };
  FileOperationProgress Progress;
  Progress.Start(FileOperationProgress::Copy, FileOperationProgress::ToRemote, 3);
  QElapsedTimer Timer;
  Timer.start();
  Fs.UploadBatch({Source + QStringLiteral("/tree"), Source + QStringLiteral("/single.txt"), Source + QStringLiteral("/skipped.tmp")},
    Target, Options, Progress);
  std::printf("  %d files uploaded in %lld ms\n", Files + 2, static_cast<long long>(Timer.elapsed()));
  for (int I = 0; I < Files; I++)
  {
    QString Remote = QStringLiteral("%1/tree/sub/f%2").arg(Target).arg(I);
    CHECK(ReadAll(Remote) == QByteArray(I * 37, 'a' + (I % 26)));
    CHECK(Stat(Remote).st_mtime == 1500000000 + I);
  }
  CHECK(ReadAll(Target + QStringLiteral("/tree/sub/deeper/name with spaces")) == "spaces");
  CHECK(QFileInfo(Target + QStringLiteral("/tree/empty")).isDir());
  CHECK(ReadAll(Target + QStringLiteral("/single.txt")) == "single");
  CHECK(Stat(Target + QStringLiteral("/single.txt")).st_mtime == 1300000000);
  CHECK((Stat(Target + QStringLiteral("/single.txt")).st_mode & 07777) == 0604);
  CHECK(Stat(Target + QStringLiteral("/tree/sub")).st_mtime == 1200000000);
  CHECK(!QFile::exists(Target + QStringLiteral("/skipped.tmp")));
  CHECK(Finished.contains(Source + QStringLiteral("/single.txt")));
  CHECK(Finished.contains(Source + QStringLiteral("/tree")));
  CHECK(Finished.size() == Files + 2 + 4);
  // the session is still usable
  CHECK(Find(Fs.ReadDirectory(Target), QStringLiteral("single.txt")) != nullptr);

  std::printf("[scp batch download with exact times]\n");
  QString Local = Root + QStringLiteral("/scp-local");
  QDir().mkpath(Local);
  ScpTransferOptions Down;
  Down.PreserveTime = true;
  Progress.Start(FileOperationProgress::Copy, FileOperationProgress::ToLocal, 2);
  Timer.restart();
  Fs.DownloadBatch({Target + QStringLiteral("/tree"), Target + QStringLiteral("/single.txt")}, Local, Down, Progress);
  std::printf("  %d files downloaded in %lld ms\n", Files + 2, static_cast<long long>(Timer.elapsed()));
  for (int I = 0; I < Files; I++)
  {
    QString File = QStringLiteral("%1/tree/sub/f%2").arg(Local).arg(I);
    CHECK(ReadAll(File) == QByteArray(I * 37, 'a' + (I % 26)));
    CHECK(Stat(File).st_mtime == 1500000000 + I);
  }
  CHECK(ReadAll(Local + QStringLiteral("/tree/sub/deeper/name with spaces")) == "spaces");
  CHECK(Stat(Local + QStringLiteral("/single.txt")).st_mtime == 1300000000);
  CHECK(Stat(Local + QStringLiteral("/tree/sub")).st_mtime == 1200000000);
  CHECK(QFileInfo(Local + QStringLiteral("/tree/empty")).isDir());

  std::printf("[scp batch errors]\n");
  QStringList ErrorPaths;
  Down.Error = [&ErrorPaths](const QString & Path, const ExtException &) { ErrorPaths.append(Path); return true; };
  Fs.DownloadBatch({Target + QStringLiteral("/missing"), Target + QStringLiteral("/single.txt")}, Local + QStringLiteral("/tree"), Down, Progress);
  CHECK(ErrorPaths.size() == 1);
  CHECK(ReadAll(Local + QStringLiteral("/tree/single.txt")) == "single");
  Down.Error = nullptr;
  CHECK(Throws([&]() { Fs.DownloadBatch({Target + QStringLiteral("/missing")}, Local, Down, Progress); }));
  CHECK(Throws([&]() { Fs.UploadBatch({Source + QStringLiteral("/single.txt")}, Target + QStringLiteral("/no/such/dir"), Options, Progress); }));
  CHECK(Find(Fs.ReadDirectory(Target), QStringLiteral("tree")) != nullptr);

  std::printf("[scp chmod -R, rm -f -r, groups]\n");
  Fs.ChangeModeRecursive(Target + QStringLiteral("/tree"), 0700);
  CHECK((Stat(Target + QStringLiteral("/tree/sub/f3")).st_mode & 07777) == 0700);
  CHECK((Stat(Target + QStringLiteral("/tree/sub/deeper")).st_mode & 07777) == 0700);
  CHECK(Fs.DeletesRecursively());
  Fs.RemoveDirectory(Target + QStringLiteral("/tree"));
  CHECK(!QFile::exists(Target + QStringLiteral("/tree")));
  QStringList Groups = Fs.LookupUsersGroups();
  CHECK(!Groups.isEmpty());
  Fs.Close();
  {
    SessionData S = Data;
    S.LookupUserGroups = asOff;
    ScpFileSystem NoGroups(S, &UI);
    NoGroups.Open();
    CHECK(NoGroups.LookupUsersGroups().isEmpty());
    NoGroups.Close();
  }

  std::printf("[scp Scp1Compatibility]\n");
  {
    SessionData S = Data;
    S.Scp1Compatibility = true;
    ScpFileSystem Scp1(S, &UI);
    Scp1.Open();
    // "-1" is passed to the remote scp (modern OpenSSH rejects it)
    QString Message;
    bool Failed = Throws([&]()
    {
      FileOperationProgress P;
      P.Start(FileOperationProgress::Copy, FileOperationProgress::ToRemote, 1);
      Scp1.Upload(Source + QStringLiteral("/single.txt"), Target + QStringLiteral("/scp1.txt"), 0644, P);
    }, &Message);
    std::printf("  scp -1: %s\n", Failed ? qPrintable(Message.section(QLatin1Char('\n'), 0, 1)) : "accepted");
    CHECK(Scp1.IsOpened());
    Scp1.Close();
  }
}

static void TestScpFallback(const SessionData & BaseData, TestUserInterface & UI, const QString & Root, int NoSftpPort)
{
  std::printf("[sftp with scp fallback]\n");
  SessionData Data = BaseData;
  Data.PortNumber = NoSftpPort;
  Data.FSProtocol = fsSFTPonly;
  {
    SftpFileSystem Fs(Data, &UI);
    QString Message;
    CHECK(Throws([&]() { Fs.Open(); }, &Message));
    CHECK_MSG(Message.startsWith(QStringLiteral("Cannot initialize SFTP protocol")), Message);
  }
  Data.FSProtocol = fsSFTP;
  QString Dir = Root + QStringLiteral("/fallback");
  QDir().mkpath(Dir);
  WriteFile(Dir + QStringLiteral("/f"), "fallback");
  {
    SftpFileSystem Fs(Data, &UI);
    Fs.Open();
    CHECK(Fs.UsingScpFallback());
    CHECK(Fs.ProtocolName() == QStringLiteral("SCP"));
    CHECK(Fs.IsOpened());
    CHECK(Fs.IsCapable(fcShellAnyCommand, false));
    CHECK(!Fs.IsCapable(fcResumeSupport, true));
    CHECK(ScpFileSystem::Of(Fs) == Fs.ScpFallback());
    RemoteFileList Files = Fs.ReadDirectory(Dir);
    CHECK((Find(Files, QStringLiteral("f")) != nullptr) && (Find(Files, QStringLiteral("f"))->Size == 8));
    FileOperationProgress Progress;
    Progress.Start(FileOperationProgress::Copy, FileOperationProgress::ToLocal, 1);
    Fs.Download(Dir + QStringLiteral("/f"), Dir + QStringLiteral("/local"), Progress);
    CHECK(ReadAll(Dir + QStringLiteral("/local")) == "fallback");
    Fs.Close();
  }
  // through Terminal
  Terminal T(Data, &UI);
  Waiter W;
  T.Open(W.Cb());
  CHECK_MSG(W.Wait().Ok, Describe(W.Result));
  CHECK(T.ProtocolName() == QStringLiteral("SCP"));
  CHECK(T.CanAnyCommand());
  T.Close();
}

int main(int argc, char ** argv)
{
  QCoreApplication App(argc, argv);
  QByteArray PortStr = qgetenv("NIXSCP_TEST_SSH_PORT");
  QString KeyFile = qEnvironmentVariable("NIXSCP_TEST_SSH_KEY");
  if (PortStr.isEmpty() || KeyFile.isEmpty())
  {
    std::printf("test_protocolsettings: SKIPPED (set NIXSCP_TEST_SSH_PORT, NIXSCP_TEST_SSH_KEY)\n");
    return 0;
  }
  QTemporaryDir Scratch;
  CHECK(Scratch.isValid());
  // canonical, as the server reports it
  QString Root = QFileInfo(Scratch.path()).canonicalFilePath();

  SessionData Data;
  Data.HostName = qEnvironmentVariable("NIXSCP_TEST_SSH_HOST", QStringLiteral("127.0.0.1"));
  Data.PortNumber = PortStr.toInt();
  Data.UserName = qEnvironmentVariable("NIXSCP_TEST_SSH_USER", qEnvironmentVariable("USER"));
  Data.PublicKeyFile = KeyFile;
  Data.TryAgent = false;
  Data.AuthKI = false;
  Data.KnownHostsFile = Scratch.filePath(QStringLiteral("dotssh/known_hosts"));
  Data.FSProtocol = fsSFTPonly;
  Data.PreserveDirectoryChanges = false;
  TestUserInterface UI;

  try
  {
    TestSftpListing(Data, UI, Root);
    TestSftpSettings(Data, UI, Root);
    TestTerminalCache(Data, UI, Root, fsSFTPonly);
    TestTerminalCache(Data, UI, Root, fsSCPonly);
    TestScpBatch(Data, UI, Root);
    int NoSftpPort = qEnvironmentVariableIntValue("NIXSCP_TEST_SSH_NOSFTP_PORT");
    if (NoSftpPort > 0)
    {
      TestScpFallback(Data, UI, Root, NoSftpPort);
    }
    else
    {
      std::printf("[sftp with scp fallback] SKIPPED (set NIXSCP_TEST_SSH_NOSFTP_PORT)\n");
    }
  }
  catch (const ExtException & E)
  {
    std::printf("FAIL: unexpected exception: %s\n%s\n", qPrintable(E.Message()), qPrintable(E.MoreMessages()));
    Failures++;
  }

  std::printf((Failures == 0) ? "test_protocolsettings: all tests passed\n" : "test_protocolsettings: %d failures\n", Failures);
  return (Failures == 0) ? 0 : 1;
}
