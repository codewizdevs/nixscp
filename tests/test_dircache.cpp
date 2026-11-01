// Unit tests of core/DirectoryCache (WinSCP TRemoteDirectoryCache,
// TRemoteDirectoryChangesCache, TTerminal::ReadDirectory caching): the
// cached ReadDirectory path is driven against an in-memory file system that
// counts its CanonicalizePath and ReadDirectory calls (round trips).
// No server needed.
#include "core/Configuration.h"
#include "core/DirectoryCache.h"
#include "core/FileSystem.h"
#include "core/IniStorage.h"
#include "core/SessionData.h"

#include <QCoreApplication>
#include <QFile>
#include <QHash>
#include <QTemporaryDir>
#include <QThread>

#include <cstdio>

static int Failures = 0;

#define CHECK(Cond) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); std::fflush(stdout); Failures++; } } while (0)

// In-memory remote: directories with their listings, symlinks to directories
class FakeFileSystem : public FileSystem, public CachingFileSystem
{
public:
  explicit FakeFileSystem(const SessionData & Data) :
    FData(Data)
  {
    FCache = SessionDirectoryCache::Acquire(Data);
  }

  QHash<QString, RemoteFileList> Dirs;
  QHash<QString, QString> Links;   // "/home/u/link" -> "/data/real"
  int Canonicalizations = 0;
  int Listings = 0;

  void AddDir(const QString & Path, const RemoteFileList & Files) { Dirs.insert(Path, Files); }

  static RemoteFile Dir(const QString & Name)
  {
    RemoteFile F;
    F.FileName = Name;
    F.FileType = RemoteFile::Directory;
    F.IsDirectory = true;
    return F;
  }
  static RemoteFile Link(const QString & Name)
  {
    RemoteFile F;
    F.FileName = Name;
    F.FileType = RemoteFile::Symlink;
    F.IsSymLink = true;
    F.IsDirectory = true;
    return F;
  }
  static RemoteFile File(const QString & Name)
  {
    RemoteFile F;
    F.FileName = Name;
    return F;
  }

  // CachingFileSystem
  SessionDirectoryCache * DirectoryCache() const override { return FCache.get(); }
  bool CacheDirectories() const override { return FData.CacheDirectories; }
  bool CacheDirectoryChanges() const override { return FData.CacheDirectoryChanges; }

  void Open() override {}
  void Close() override {}
  bool IsOpened() const override { return true; }
  QString ProtocolName() const override { return QStringLiteral("FAKE"); }
  QString ServerInfo() const override { return QString(); }
  QString CanonicalizePath(const QString & Path) override
  {
    Canonicalizations++;
    QString Result = SessionDirectoryCache::NormalizePath(Path, QStringLiteral("/home/u"));
    // resolve symlinked components
    for (auto It = Links.constBegin(); It != Links.constEnd(); ++It)
    {
      if ((Result == It.key()) || Result.startsWith(It.key() + QLatin1Char('/')))
      {
        Result = It.value() + Result.mid(It.key().size());
      }
    }
    if (!Dirs.contains(Result))
    {
      throw ExtException(QStringLiteral("Cannot get real path for '%1'.").arg(Path));
    }
    return Result;
  }
  QString HomeDirectory() override { return QStringLiteral("/home/u"); }
  RemoteFileList ReadDirectory(const QString & Path) override
  {
    Listings++;
    if (!Dirs.contains(Path))
    {
      throw ExtException(QStringLiteral("Error listing directory '%1'.").arg(Path));
    }
    return Dirs.value(Path);
  }
  RemoteFile ReadFile(const QString &) override { return RemoteFile(); }
  bool FileExists(const QString &, RemoteFile *) override { return false; }
  void CreateDirectory(const QString & Path, int) override { FCache->FileModified(Path, false, false); }
  void DeleteFile(const QString & Path) override { FCache->FileModified(Path, false, true); }
  void RemoveDirectory(const QString & Path) override { FCache->FileModified(Path, true, true); }
  void RenameFile(const QString &, const QString &) override {}
  void ChangeMode(const QString &, unsigned int) override {}
  void SetModificationTime(const QString &, const QDateTime &) override {}
  void CreateLink(const QString &, const QString &) override {}
  void Download(const QString &, const QString &, FileOperationProgress &) override {}
  void Upload(const QString &, const QString &, int, FileOperationProgress &) override {}

private:
  SessionData FData;
  std::shared_ptr<SessionDirectoryCache> FCache;
};

static SessionData FakeData(const QString & Host)
{
  SessionData Data;
  Data.HostName = Host;
  Data.UserName = QStringLiteral("u");
  Data.FSProtocol = fsSFTP;
  Data.CacheDirectories = true;
  Data.CacheDirectoryChanges = true;
  Data.PreserveDirectoryChanges = false;
  return Data;
}

static void Read(FakeFileSystem & Fs, const QString & Path, const QString & Current, QString & Dir, RemoteFileList & Files)
{
  DirectoryCaching::ReadDirectory(Fs, Path, Current, Dir, Files);
}

static void TestNormalize()
{
  std::printf("[normalize]\n");
  CHECK(SessionDirectoryCache::NormalizePath(QStringLiteral("/a/./b/../c/")) == QStringLiteral("/a/c"));
  CHECK(SessionDirectoryCache::NormalizePath(QStringLiteral("/")) == QStringLiteral("/"));
  CHECK(SessionDirectoryCache::NormalizePath(QStringLiteral("/..")) == QStringLiteral("/"));
  CHECK(SessionDirectoryCache::NormalizePath(QStringLiteral("//a//b")) == QStringLiteral("/a/b"));
  CHECK(SessionDirectoryCache::NormalizePath(QStringLiteral("sub"), QStringLiteral("/home/u")) == QStringLiteral("/home/u/sub"));
  CHECK(SessionDirectoryCache::NormalizePath(QStringLiteral(".."), QStringLiteral("/home/u")) == QStringLiteral("/home"));
  CHECK(SessionDirectoryCache::NormalizePath(QStringLiteral("~/x"), QStringLiteral("/home/u")).isEmpty());
  CHECK(SessionDirectoryCache::NormalizePath(QStringLiteral("sub")).isEmpty());
}

static void TestChangesCache()
{
  std::printf("[directory changes cache]\n");
  RemoteDirectoryChangesCache Cache(3);
  QString Target;
  CHECK(!Cache.GetDirectoryChange(QStringLiteral("/home/u"), QStringLiteral("x"), Target));
  // symlink: "/home/u" + "link" -> "/data/real"
  Cache.AddDirectoryChange(QStringLiteral("/home/u"), QStringLiteral("link"), QStringLiteral("/data/real"));
  CHECK(Cache.IsKnownTarget(QStringLiteral("/data/real")));
  CHECK(Cache.GetDirectoryChange(QStringLiteral("/home/u"), QStringLiteral("link"), Target) && (Target == QStringLiteral("/data/real")));
  // absolute change to a canonical target: stored as "//"
  Cache.AddDirectoryChange(QStringLiteral("/data/real"), QStringLiteral("/home/u"), QStringLiteral("/home/u"));
  CHECK(Cache.GetDirectoryChange(QStringLiteral("/x"), QStringLiteral("/home/u"), Target) && (Target == QStringLiteral("/home/u")));
  CHECK(Cache.HasTargetBelow(QStringLiteral("/home")));
  CHECK(!Cache.HasTargetBelow(QStringLiteral("/hom")));
  // serialization: "A" + Name=Value lines, only the MaxSize most recent
  QString Data = Cache.Serialize();
  CHECK(Data.startsWith(QLatin1Char('A')));
  CHECK(Data.contains(QStringLiteral("/home/u,link=/data/real\r\n")));
  CHECK(Data.contains(QStringLiteral("/data/real=//\r\n")));
  RemoteDirectoryChangesCache Copy(3);
  Copy.Deserialize(Data);
  CHECK(Copy.Count() == 3);
  CHECK(Copy.GetDirectoryChange(QStringLiteral("/home/u"), QStringLiteral("link"), Target) && (Target == QStringLiteral("/data/real")));
  Cache.AddDirectoryChange(QStringLiteral("/"), QStringLiteral("/a"), QStringLiteral("/a"));
  Cache.AddDirectoryChange(QStringLiteral("/"), QStringLiteral("/b"), QStringLiteral("/b"));
  RemoteDirectoryChangesCache Limited(3);
  Limited.Deserialize(Cache.Serialize());
  CHECK(Limited.Count() == 3);
  CHECK(Limited.IsKnownTarget(QStringLiteral("/b")));
  // deleting a directory forgets changes from and to it
  Cache.ClearDirectoryChangeTarget(QStringLiteral("/data/real"));
  CHECK(!Cache.GetDirectoryChange(QStringLiteral("/home/u"), QStringLiteral("link"), Target));
  Cache.ClearDirectoryChange(QStringLiteral("/home/u"));
  CHECK(!Cache.IsKnownTarget(QStringLiteral("/home/u")));
  Copy.Deserialize(QString());
  CHECK(Copy.IsEmpty());
}

static void TestCachedNavigation()
{
  std::printf("[cached navigation]\n");
  FakeFileSystem Fs(FakeData(QStringLiteral("nav.example")));
  Fs.AddDir(QStringLiteral("/"), {FakeFileSystem::Dir(QStringLiteral("home")), FakeFileSystem::Dir(QStringLiteral("data"))});
  Fs.AddDir(QStringLiteral("/home"), {FakeFileSystem::Dir(QStringLiteral("u"))});
  Fs.AddDir(QStringLiteral("/home/u"), {FakeFileSystem::Dir(QStringLiteral("sub")), FakeFileSystem::Link(QStringLiteral("link")),
    FakeFileSystem::File(QStringLiteral("f.txt"))});
  Fs.AddDir(QStringLiteral("/home/u/sub"), {FakeFileSystem::File(QStringLiteral("a"))});
  Fs.AddDir(QStringLiteral("/data"), {FakeFileSystem::Dir(QStringLiteral("real"))});
  Fs.AddDir(QStringLiteral("/data/real"), {FakeFileSystem::File(QStringLiteral("r"))});
  Fs.Links.insert(QStringLiteral("/home/u/link"), QStringLiteral("/data/real"));

  QString Dir;
  RemoteFileList Files;
  // startup: no current directory, always canonicalized
  Read(Fs, QStringLiteral("."), QString(), Dir, Files);
  CHECK(Dir == QStringLiteral("/home/u"));
  CHECK((Fs.Canonicalizations == 1) && (Fs.Listings == 1));
  CHECK(Files.size() == 3);
  // plain subdirectory of a canonical directory: no realpath round trip
  Read(Fs, QStringLiteral("/home/u/sub"), QStringLiteral("/home/u"), Dir, Files);
  CHECK(Dir == QStringLiteral("/home/u/sub"));
  CHECK((Fs.Canonicalizations == 1) && (Fs.Listings == 2));
  // Back: listing and directory from the caches, no round trip at all
  Read(Fs, QStringLiteral("/home/u"), QStringLiteral("/home/u/sub"), Dir, Files);
  CHECK(Dir == QStringLiteral("/home/u"));
  CHECK((Fs.Canonicalizations == 1) && (Fs.Listings == 2));
  CHECK(Files.size() == 3);
  // Parent of a canonical directory is canonical
  Read(Fs, QStringLiteral(".."), QStringLiteral("/home/u"), Dir, Files);
  CHECK(Dir == QStringLiteral("/home"));
  CHECK((Fs.Canonicalizations == 1) && (Fs.Listings == 3));
  // a symlink needs the realpath; the change is remembered
  Read(Fs, QStringLiteral("/home/u/link"), QStringLiteral("/home"), Dir, Files);
  CHECK(Dir == QStringLiteral("/data/real"));
  CHECK((Fs.Canonicalizations == 2) && (Fs.Listings == 4));
  Read(Fs, QStringLiteral("/home"), QStringLiteral("/data/real"), Dir, Files);
  CHECK((Fs.Canonicalizations == 2) && (Fs.Listings == 4));
  Read(Fs, QStringLiteral("u/link"), QStringLiteral("/home"), Dir, Files);
  CHECK(Dir == QStringLiteral("/data/real"));
  CHECK((Fs.Canonicalizations == 2) && (Fs.Listings == 4));
  // Reload (the current directory again): always from the server
  Fs.Dirs[QStringLiteral("/data/real")].append(FakeFileSystem::File(QStringLiteral("external")));
  Read(Fs, QStringLiteral("/data/real"), QStringLiteral("/data/real"), Dir, Files);
  CHECK((Fs.Canonicalizations == 3) && (Fs.Listings == 5));
  CHECK(Files.size() == 2);
  // a modification through the file system invalidates the parent listing
  Read(Fs, QStringLiteral("/home/u/sub"), QStringLiteral("/data/real"), Dir, Files);
  int Listings = Fs.Listings;
  CHECK(Listings == 5);
  Fs.Dirs[QStringLiteral("/home/u/sub")].append(FakeFileSystem::File(QStringLiteral("new")));
  Fs.CreateDirectory(QStringLiteral("/home/u/sub/new"), 0755);
  Read(Fs, QStringLiteral("/home/u/sub"), QStringLiteral("/home"), Dir, Files);
  CHECK(Fs.Listings == Listings + 1);
  CHECK(Files.size() == 2);
  // removing a directory forgets it and everything below it
  Fs.RemoveDirectory(QStringLiteral("/home/u"));
  CHECK(!Fs.DirectoryCache()->HasFileList(QStringLiteral("/home/u/sub")));
  CHECK(!Fs.DirectoryCache()->HasFileList(QStringLiteral("/home/u")));
  CHECK(!Fs.DirectoryCache()->HasFileList(QStringLiteral("/home")));
  // stale cached directory change: falls back to the realpath, which reports the error
  Read(Fs, QStringLiteral("/data/real"), QStringLiteral("/"), Dir, Files);
  int Canonicalizations = Fs.Canonicalizations;
  Fs.Dirs.remove(QStringLiteral("/data/real"));
  Fs.DirectoryCache()->ClearFileList(QStringLiteral("/data/real"), false);
  bool Failed = false;
  try
  {
    Read(Fs, QStringLiteral("/data/real"), QStringLiteral("/"), Dir, Files);
  }
  catch (const ExtException & E)
  {
    Failed = E.Message().startsWith(QStringLiteral("Cannot get real path"));
  }
  CHECK(Failed);
  CHECK(Fs.Canonicalizations == Canonicalizations + 1);
  // Clear Caches
  Read(Fs, QStringLiteral("/home"), QStringLiteral("/"), Dir, Files);
  CHECK(!DirectoryCaching::AreCachesEmpty(Fs));
  DirectoryCaching::ClearCaches(Fs);
  CHECK(DirectoryCaching::AreCachesEmpty(Fs));
}

static void TestCachingOff()
{
  std::printf("[caching off]\n");
  SessionData Data = FakeData(QStringLiteral("off.example"));
  Data.CacheDirectories = false;
  Data.CacheDirectoryChanges = false;
  FakeFileSystem Fs(Data);
  Fs.AddDir(QStringLiteral("/home/u"), {FakeFileSystem::Dir(QStringLiteral("sub"))});
  Fs.AddDir(QStringLiteral("/home/u/sub"), {});
  QString Dir;
  RemoteFileList Files;
  Read(Fs, QStringLiteral("/home/u"), QString(), Dir, Files);
  Read(Fs, QStringLiteral("/home/u/sub"), QStringLiteral("/home/u"), Dir, Files);
  Read(Fs, QStringLiteral("/home/u"), QStringLiteral("/home/u/sub"), Dir, Files);
  CHECK((Fs.Canonicalizations == 3) && (Fs.Listings == 3));
  CHECK(DirectoryCaching::AreCachesEmpty(Fs));

  // only "Cache directory changes": listings always loaded, realpath skipped
  Data.CacheDirectoryChanges = true;
  Data.HostName = QStringLiteral("changes.example");
  FakeFileSystem Fs2(Data);
  Fs2.Dirs = Fs.Dirs;
  Read(Fs2, QStringLiteral("/home/u"), QString(), Dir, Files);
  Read(Fs2, QStringLiteral("/home/u/sub"), QStringLiteral("/home/u"), Dir, Files);
  Read(Fs2, QStringLiteral("/home/u"), QStringLiteral("/home/u/sub"), Dir, Files);
  CHECK(Fs2.Listings == 3);
  // the subdirectory needs the realpath without the parent's cached listing
  CHECK(Fs2.Canonicalizations == 2);
}

static void TestSharingAndRaces()
{
  std::printf("[shared cache, racing listings]\n");
  SessionData Data = FakeData(QStringLiteral("shared.example"));
  std::shared_ptr<SessionDirectoryCache> A = SessionDirectoryCache::Acquire(Data);
  std::shared_ptr<SessionDirectoryCache> B = SessionDirectoryCache::Acquire(Data);
  CHECK(A == B);
  SessionData Other = Data;
  Other.PortNumber = 2222;
  CHECK(SessionDirectoryCache::Acquire(Other) != A);
  CHECK(SessionDirectoryCache::SessionKey(Other) == QStringLiteral("u@shared.example:2222"));
  CHECK(SessionDirectoryCache::SessionKey(Data) == QStringLiteral("u@shared.example"));

  RemoteFileList Files{FakeFileSystem::File(QStringLiteral("x"))};
  quint64 Generation = A->Generation();
  // another session modifies the directory while it is being listed
  B->FileModified(QStringLiteral("/d/x"), false, false);
  A->AddFileList(QStringLiteral("/d"), Files, Generation);
  CHECK(!A->HasFileList(QStringLiteral("/d")));
  // unrelated modifications do not prevent caching
  Generation = A->Generation();
  B->FileModified(QStringLiteral("/elsewhere/y"), false, false);
  A->AddFileList(QStringLiteral("/d/"), Files, Generation);
  CHECK(B->HasFileList(QStringLiteral("/d")));
  RemoteFileList Got;
  CHECK(B->GetFileList(QStringLiteral("/d"), Got) && (Got.size() == 1));
  // a recursive modification above it does
  Generation = A->Generation();
  B->FileModified(QStringLiteral("/"), true, false);
  A->AddFileList(QStringLiteral("/d"), Files, Generation);
  CHECK(!A->HasFileList(QStringLiteral("/d")));
  // from another thread
  Generation = A->Generation();
  QThread * Worker = QThread::create([B]() { B->FileModified(QStringLiteral("/d"), true, true); });
  Worker->start();
  Worker->wait();
  delete Worker;
  A->AddFileList(QStringLiteral("/d/sub"), Files, Generation);
  CHECK(!A->HasFileList(QStringLiteral("/d/sub")));
}

static void TestPersistence(const QString & IniPath)
{
  std::printf("[PreserveDirectoryChanges]\n");
  SessionData Data = FakeData(QStringLiteral("persist.example"));
  Data.PreserveDirectoryChanges = true;
  {
    // not a GUI application: the configuration is not touched by default
    std::shared_ptr<SessionDirectoryCache> Cache = SessionDirectoryCache::Acquire(Data);
    Cache->AddDirectoryChange(QStringLiteral("/a"), QStringLiteral("b"), QStringLiteral("/c"));
  }
  CHECK(!QFile::exists(IniPath));
  SessionDirectoryCache::SetPersistence(true);
  Configuration & Config = Configuration::Instance();
  Config.SetIniFileName(IniPath);
  Config.Load();
  {
    std::shared_ptr<SessionDirectoryCache> Cache = SessionDirectoryCache::Acquire(Data);
    Cache->AddDirectoryChange(QStringLiteral("/home/u"), QStringLiteral("link"), QStringLiteral("/data/real"));
  }
  // saved when the last session released it ([Configuration\CDCache], hex "binary data")
  IniFile File;
  CHECK(File.Load(IniPath));
  QString Hex = File.ReadRaw(QStringLiteral("Configuration\\CDCache"), MungeIniName(QStringLiteral("u@persist.example")));
  CHECK(!Hex.isEmpty());
  QString Decoded = QString::fromUtf8(QByteArray::fromHex(Hex.toLatin1()));
  CHECK(Decoded.startsWith(QLatin1Char('A')) && Decoded.contains(QStringLiteral("/home/u,link=/data/real")));
  CHECK(Hex == Hex.toUpper());
  // and loaded by the next session
  {
    std::shared_ptr<SessionDirectoryCache> Cache = SessionDirectoryCache::Acquire(Data);
    QString Target;
    CHECK(Cache->GetDirectoryChange(QStringLiteral("/home/u"), QStringLiteral("link"), Target) && (Target == QStringLiteral("/data/real")));
  }
  // without PreserveDirectoryChanges nothing is loaded
  Data.PreserveDirectoryChanges = false;
  {
    std::shared_ptr<SessionDirectoryCache> Cache = SessionDirectoryCache::Acquire(Data);
    QString Target;
    CHECK(!Cache->GetDirectoryChange(QStringLiteral("/home/u"), QStringLiteral("link"), Target));
  }
  // a WinSCP.ini entry (Windows line breaks) is understood
  Data.HostName = QStringLiteral("winscp.example");
  Data.PreserveDirectoryChanges = true;
  QByteArray WinData = QByteArray("A/srv,www=/var/www\r\n/var/www=//\r\n");
  Config.Ini().WriteRaw(QStringLiteral("Configuration\\CDCache"), MungeIniName(QStringLiteral("u@winscp.example")),
    QString::fromLatin1(WinData.toHex().toUpper()));
  {
    std::shared_ptr<SessionDirectoryCache> Cache = SessionDirectoryCache::Acquire(Data);
    QString Target;
    CHECK(Cache->GetDirectoryChange(QStringLiteral("/srv"), QStringLiteral("www"), Target) && (Target == QStringLiteral("/var/www")));
    CHECK(Cache->IsKnownCanonical(QStringLiteral("/var")));
  }
}

int main(int argc, char ** argv)
{
  QCoreApplication App(argc, argv);
  QTemporaryDir Scratch;
  CHECK(Scratch.isValid());

  TestNormalize();
  TestChangesCache();
  TestCachedNavigation();
  TestCachingOff();
  TestSharingAndRaces();
  TestPersistence(Scratch.filePath(QStringLiteral("nixscp.ini")));

  std::printf((Failures == 0) ? "test_dircache: all tests passed\n" : "test_dircache: %d failures\n", Failures);
  return (Failures == 0) ? 0 : 1;
}
