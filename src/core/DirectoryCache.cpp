// Port of WinSCP source/core/RemoteFiles.cpp (TRemoteDirectoryCache,
// TRemoteDirectoryChangesCache), the directory cache parts of
// source/core/Terminal.cpp and source/core/Configuration.cpp
// (LoadDirectoryChangesCache / SaveDirectoryChangesCache)
#include "core/DirectoryCache.h"

#include "core/Configuration.h"
#include "core/FileSystem.h"
#include "core/IniStorage.h"
#include "core/SessionData.h"

#include <QCoreApplication>
#include <QThread>

#include <atomic>
#include <chrono>
#include <future>

namespace
{

const QString CDCacheSection = QStringLiteral("Configuration\\CDCache");
// "//" value of a directory known to be canonical
const QString CanonicalMarker = QStringLiteral("//");
// TConfiguration::CacheDirectoryChangesMaxSize default
const int DefaultCacheDirectoryChangesMaxSize = 100;
// Modifications remembered to reject listings that raced with them
const int MaxModificationRecords = 512;
// SessionDirectoryCache::SetPersistence: -1 = GUI application only
std::atomic<int> PersistenceMode{-1};

bool PersistenceEnabled()
{
  int Mode = PersistenceMode;
  if (Mode >= 0)
  {
    return (Mode != 0);
  }
  // Only the GUI application touches the configuration by default, so
  // tests and tools linking the core never write the user's nixscp.ini
  QCoreApplication * App = QCoreApplication::instance();
  return (App != nullptr) && App->inherits("QGuiApplication");
}

QString ExcludeTrailingSlash(const QString & Path)
{
  QString Result = Path;
  while ((Result.size() > 1) && Result.endsWith(QLatin1Char('/')))
  {
    Result.chop(1);
  }
  return Result;
}

bool IsChildOrSelf(const QString & Parent, const QString & Path)
{
  if (Path == Parent)
  {
    return true;
  }
  if (Parent == QStringLiteral("/"))
  {
    return Path.startsWith(QLatin1Char('/'));
  }
  return Path.startsWith(Parent) && (Path.size() > Parent.size()) && (Path[Parent.size()] == QLatin1Char('/'));
}

// The configuration lives on the application thread; worker threads ask it
// with a bounded wait, so a GUI thread waiting for the worker (closing a
// session that is still connecting) cannot deadlock with it
template<typename TResult>
TResult RunOnApplicationThread(const std::function<TResult()> & Func, TResult Fallback)
{
  QCoreApplication * App = QCoreApplication::instance();
  if (App == nullptr)
  {
    return Fallback;
  }
  if (QThread::currentThread() == App->thread())
  {
    return Func();
  }
  auto Promise = std::make_shared<std::promise<TResult>>();
  std::future<TResult> Future = Promise->get_future();
  QMetaObject::invokeMethod(App, [Func, Promise]() { Promise->set_value(Func()); }, Qt::QueuedConnection);
  if (Future.wait_for(std::chrono::seconds(2)) != std::future_status::ready)
  {
    return Fallback;
  }
  return Future.get();
}

// TConfiguration::LoadDirectoryChangesCache (the data is hex encoded
// "binary data", as THierarchicalStorage::ReadBinaryData in WinSCP.ini)
QString LoadDirectoryChangesCache(const QString & SessionKey)
{
  if (!PersistenceEnabled())
  {
    return QString();
  }
  return RunOnApplicationThread<QString>([SessionKey]() -> QString
  {
    Configuration & Config = Configuration::Instance();
    if (Config.IsNulStorage())
    {
      return QString();
    }
    QString Hex = Config.Ini().ReadRaw(CDCacheSection, MungeIniName(SessionKey));
    return QString::fromUtf8(QByteArray::fromHex(Hex.toLatin1()));
  }, QString());
}

int CacheDirectoryChangesMaxSize()
{
  if (!PersistenceEnabled())
  {
    return DefaultCacheDirectoryChangesMaxSize;
  }
  return RunOnApplicationThread<int>([]() -> int
  {
    bool Ok = false;
    int Value = Configuration::Instance().Ini().ReadRaw(QStringLiteral("Configuration\\Interface"),
      QStringLiteral("CacheDirectoryChangesMaxSize")).toInt(&Ok);
    return (Ok && (Value > 0)) ? Value : DefaultCacheDirectoryChangesMaxSize;
  }, DefaultCacheDirectoryChangesMaxSize);
}

// TConfiguration::SaveDirectoryChangesCache; posted, never waits
void SaveDirectoryChangesCache(const QString & SessionKey, const QString & Data)
{
  QCoreApplication * App = QCoreApplication::instance();
  if ((App == nullptr) || !PersistenceEnabled())
  {
    return;
  }
  auto Write = [SessionKey, Data]()
  {
    Configuration & Config = Configuration::Instance();
    if (Config.IsNulStorage())
    {
      return;
    }
    QString Hex = QString::fromLatin1(Data.toUtf8().toHex().toUpper());
    if (Config.Ini().ReadRaw(CDCacheSection, MungeIniName(SessionKey)) != Hex)
    {
      Config.Ini().WriteRaw(CDCacheSection, MungeIniName(SessionKey), Hex);
      Config.Save();
    }
  };
  if (QThread::currentThread() == App->thread())
  {
    Write();
  }
  else
  {
    QMetaObject::invokeMethod(App, Write, Qt::QueuedConnection);
  }
}

}

//---------------------------------------------------------------------------
// RemoteDirectoryChangesCache

void RemoteDirectoryChangesCache::SetValue(const QString & Name, const QString & Value)
{
  FNames.removeOne(Name);
  FNames.append(Name);
  FValues.insert(Name, Value);
}

QString RemoteDirectoryChangesCache::GetValue(const QString & Name)
{
  auto It = FValues.constFind(Name);
  if (It == FValues.constEnd())
  {
    return QString();
  }
  QString Value = It.value();
  // move to the end: most recently used
  SetValue(Name, Value);
  return Value;
}

void RemoteDirectoryChangesCache::Delete(const QString & Name)
{
  FNames.removeOne(Name);
  FValues.remove(Name);
}

bool RemoteDirectoryChangesCache::DirectoryChangeKey(const QString & SourceDir, const QString & Change, QString & Key)
{
  bool Result = !Change.isEmpty();
  if (Result)
  {
    bool Absolute = Change.startsWith(QLatin1Char('/'));
    Result = !SourceDir.isEmpty() || Absolute;
    if (Result)
    {
      Key = Absolute ? SessionDirectoryCache::NormalizePath(Change) : (SourceDir + QLatin1Char(',') + Change);
    }
  }
  return Result;
}

void RemoteDirectoryChangesCache::AddDirectoryChange(const QString & SourceDir, const QString & Change, const QString & TargetDir)
{
  if (TargetDir.isEmpty())
  {
    return;
  }
  SetValue(TargetDir, CanonicalMarker);
  if (SessionDirectoryCache::NormalizePath(Change, SourceDir) != TargetDir)
  {
    QString Key;
    if (DirectoryChangeKey(SourceDir, Change, Key))
    {
      SetValue(Key, TargetDir);
    }
  }
}

void RemoteDirectoryChangesCache::ClearDirectoryChange(const QString & SourceDir)
{
  const QStringList Names = FNames;
  for (const QString & Name : Names)
  {
    if (Name.startsWith(SourceDir))
    {
      Delete(Name);
    }
  }
}

void RemoteDirectoryChangesCache::ClearDirectoryChangeTarget(const QString & TargetDir)
{
  QString Key;
  // hack to clear at least local sym-link change in case symlink is deleted
  DirectoryChangeKey(ExcludeTrailingSlash(UnixPath::ExtractFileDir(TargetDir)), UnixPath::ExtractFileName(TargetDir), Key);
  const QStringList Names = FNames;
  for (const QString & Name : Names)
  {
    if (Name.startsWith(TargetDir) || FValues.value(Name).startsWith(TargetDir) ||
        (!Key.isEmpty() && (Name == Key)))
    {
      Delete(Name);
    }
  }
}

bool RemoteDirectoryChangesCache::GetDirectoryChange(const QString & SourceDir, const QString & Change, QString & TargetDir)
{
  QString Key = SessionDirectoryCache::NormalizePath(Change, SourceDir);
  if (!Key.isEmpty() && FValues.contains(Key))
  {
    TargetDir = GetValue(Key);
    // TargetDir is not "//" here only when Change is full path to symbolic link
    if (TargetDir == CanonicalMarker)
    {
      TargetDir = Key;
    }
    return true;
  }
  if (DirectoryChangeKey(SourceDir, Change, Key))
  {
    QString Directory = GetValue(Key);
    if (!Directory.isEmpty() && (Directory != CanonicalMarker))
    {
      TargetDir = Directory;
      return true;
    }
  }
  return false;
}

bool RemoteDirectoryChangesCache::IsKnownTarget(const QString & Directory) const
{
  return FValues.value(Directory) == CanonicalMarker;
}

bool RemoteDirectoryChangesCache::HasTargetBelow(const QString & Directory) const
{
  for (auto It = FValues.constBegin(); It != FValues.constEnd(); ++It)
  {
    if ((It.value() == CanonicalMarker) && IsChildOrSelf(Directory, It.key()))
    {
      return true;
    }
  }
  return false;
}

QString RemoteDirectoryChangesCache::Serialize() const
{
  QString Data = QStringLiteral("A");
  qsizetype Start = std::max<qsizetype>(0, FNames.size() - FMaxSize);
  for (qsizetype Index = Start; Index < FNames.size(); Index++)
  {
    Data += FNames[Index] + QLatin1Char('=') + FValues.value(FNames[Index]) + QStringLiteral("\r\n");
  }
  return Data;
}

void RemoteDirectoryChangesCache::Deserialize(const QString & Data)
{
  Clear();
  if (Data.isEmpty())
  {
    return;
  }
  const QStringList Lines = Data.mid(1).split(QLatin1Char('\n'));
  for (QString Line : Lines)
  {
    if (Line.endsWith(QLatin1Char('\r')))
    {
      Line.chop(1);
    }
    qsizetype P = Line.indexOf(QLatin1Char('='));
    if (P > 0)
    {
      SetValue(Line.left(P), Line.mid(P + 1));
    }
  }
}

//---------------------------------------------------------------------------
// SessionDirectoryCache

namespace
{
QMutex RegistryMutex;
QHash<QString, std::weak_ptr<SessionDirectoryCache>> & Registry()
{
  static QHash<QString, std::weak_ptr<SessionDirectoryCache>> Result;
  return Result;
}
}

QString SessionDirectoryCache::SessionKey(const SessionData & Data)
{
  QString Result = QStringLiteral("%1@%2").arg(Data.UserNameExpanded(), Data.HostNameExpanded());
  if (Data.PortNumber != Data.DefaultPort())
  {
    Result += QStringLiteral(":%1").arg(Data.PortNumber);
  }
  return Result;
}

QString SessionDirectoryCache::ServerKey(const SessionData & Data)
{
  return QStringLiteral("%1|%2|%3|%4").arg(static_cast<int>(Data.FSProtocol)).arg(SessionKey(Data), Data.SftpServer, Data.Shell);
}

QString SessionDirectoryCache::NormalizePath(const QString & Path, const QString & Base)
{
  QString Full = Path;
  if (!Full.startsWith(QLatin1Char('/')))
  {
    if (!Base.startsWith(QLatin1Char('/')) || Full.startsWith(QLatin1Char('~')))
    {
      // not resolvable locally (home relative)
      return QString();
    }
    Full = UnixPath::Combine(Base, Full);
  }
  QStringList Parts;
  const QStringList Segments = Full.split(QLatin1Char('/'), Qt::SkipEmptyParts);
  for (const QString & Segment : Segments)
  {
    if (Segment == QStringLiteral("."))
    {
      continue;
    }
    if (Segment == QStringLiteral(".."))
    {
      if (!Parts.isEmpty())
      {
        Parts.removeLast();
      }
      continue;
    }
    Parts.append(Segment);
  }
  return QLatin1Char('/') + Parts.join(QLatin1Char('/'));
}

void SessionDirectoryCache::SetPersistence(bool Enabled)
{
  PersistenceMode = Enabled ? 1 : 0;
}

std::shared_ptr<SessionDirectoryCache> SessionDirectoryCache::Acquire(const SessionData & Data)
{
  QString Key = ServerKey(Data);
  bool Preserve = Data.CacheDirectoryChanges && Data.PreserveDirectoryChanges;
  std::shared_ptr<SessionDirectoryCache> Result;
  bool Created = false;
  {
    QMutexLocker Lock(&RegistryMutex);
    Result = Registry().value(Key).lock();
    if (!Result)
    {
      Result.reset(new SessionDirectoryCache());
      Registry().insert(Key, Result);
      Created = true;
    }
  }
  if (Preserve)
  {
    QString SessionKeyStr = SessionKey(Data);
    bool Load = false;
    {
      QMutexLocker Lock(&Result->FMutex);
      if (!Result->FPreserve)
      {
        Result->FPreserve = true;
        Result->FSessionKey = SessionKeyStr;
        Load = Created;
      }
    }
    if (Load)
    {
      // outside of the locks: may wait for the application thread
      int MaxSize = CacheDirectoryChangesMaxSize();
      QString Stored = LoadDirectoryChangesCache(SessionKeyStr);
      QMutexLocker Lock(&Result->FMutex);
      Result->FChanges = RemoteDirectoryChangesCache(MaxSize);
      Result->FChanges.Deserialize(Stored);
    }
  }
  return Result;
}

SessionDirectoryCache::~SessionDirectoryCache()
{
  {
    QMutexLocker Lock(&RegistryMutex);
    for (auto It = Registry().begin(); It != Registry().end(); )
    {
      if (It.value().expired())
      {
        It = Registry().erase(It);
      }
      else
      {
        ++It;
      }
    }
  }
  if (FPreserve)
  {
    SaveDirectoryChangesCache(FSessionKey, FChanges.Serialize());
  }
}

bool SessionDirectoryCache::HasFileList(const QString & Directory) const
{
  QMutexLocker Lock(&FMutex);
  return FLists.contains(ExcludeTrailingSlash(Directory));
}

bool SessionDirectoryCache::GetFileList(const QString & Directory, RemoteFileList & Files) const
{
  QMutexLocker Lock(&FMutex);
  auto It = FLists.constFind(ExcludeTrailingSlash(Directory));
  if (It == FLists.constEnd())
  {
    return false;
  }
  Files = It.value().Files;
  return true;
}

quint64 SessionDirectoryCache::Generation() const
{
  QMutexLocker Lock(&FMutex);
  return FGeneration;
}

void SessionDirectoryCache::AddFileList(const QString & Directory, const RemoteFileList & Files, quint64 StartGeneration)
{
  QString Dir = ExcludeTrailingSlash(Directory);
  QMutexLocker Lock(&FMutex);
  if (ModifiedSince(StartGeneration, Dir))
  {
    // modified (by another session) while being listed
    return;
  }
  FLists.insert(Dir, Entry{Files});
}

void SessionDirectoryCache::RecordModification(quint64 Generation, const QString & Path, bool SubDirs)
{
  FModifications.append(ModificationRecord{Generation, Path, SubDirs});
  if (FModifications.size() > MaxModificationRecords)
  {
    FModifications.remove(0, FModifications.size() - MaxModificationRecords);
  }
}

bool SessionDirectoryCache::ModifiedSince(quint64 Generation, const QString & Directory) const
{
  quint64 Changes = FGeneration - Generation;
  if (Changes == 0)
  {
    return false;
  }
  if (Changes > static_cast<quint64>(FModifications.size()))
  {
    // too many to tell
    return true;
  }
  for (const ModificationRecord & Record : FModifications)
  {
    if (Record.Generation <= Generation)
    {
      continue;
    }
    if (Record.Path.isEmpty() ||
        (Directory == Record.Path) ||
        (Directory == UnixPath::Parent(Record.Path)) ||
        (Record.SubDirs && IsChildOrSelf(Record.Path, Directory)))
    {
      return true;
    }
  }
  return false;
}

void SessionDirectoryCache::DoClearFileList(const QString & Directory, bool SubDirs)
{
  QString Dir = ExcludeTrailingSlash(Directory);
  FLists.remove(Dir);
  if (SubDirs)
  {
    for (auto It = FLists.begin(); It != FLists.end(); )
    {
      if (IsChildOrSelf(Dir, It.key()))
      {
        It = FLists.erase(It);
      }
      else
      {
        ++It;
      }
    }
  }
}

void SessionDirectoryCache::ClearFileList(const QString & Directory, bool SubDirs)
{
  QMutexLocker Lock(&FMutex);
  FGeneration++;
  RecordModification(FGeneration, ExcludeTrailingSlash(Directory), SubDirs);
  DoClearFileList(Directory, SubDirs);
}

bool SessionDirectoryCache::GetDirectoryChange(const QString & SourceDir, const QString & Change, QString & TargetDir)
{
  QMutexLocker Lock(&FMutex);
  return FChanges.GetDirectoryChange(SourceDir, Change, TargetDir);
}

void SessionDirectoryCache::AddDirectoryChange(const QString & SourceDir, const QString & Change, const QString & TargetDir)
{
  QMutexLocker Lock(&FMutex);
  FChanges.AddDirectoryChange(SourceDir, Change, TargetDir);
}

void SessionDirectoryCache::ClearDirectoryChange(const QString & SourceDir)
{
  QMutexLocker Lock(&FMutex);
  FChanges.ClearDirectoryChange(SourceDir);
}

bool SessionDirectoryCache::IsKnownCanonical(const QString & Directory) const
{
  if (Directory == QStringLiteral("/"))
  {
    return true;
  }
  if (!Directory.startsWith(QLatin1Char('/')) || (NormalizePath(Directory) != Directory))
  {
    return false;
  }
  QMutexLocker Lock(&FMutex);
  if (FChanges.IsKnownTarget(Directory) || FChanges.HasTargetBelow(Directory))
  {
    return true;
  }
  QString Parent = UnixPath::Parent(Directory);
  if ((Parent == Directory) ||
      !((Parent == QStringLiteral("/")) || FChanges.IsKnownTarget(Parent) || FChanges.HasTargetBelow(Parent)))
  {
    return false;
  }
  auto It = FLists.constFind(Parent);
  if (It == FLists.constEnd())
  {
    return false;
  }
  QString Name = UnixPath::ExtractFileName(Directory);
  for (const RemoteFile & File : It.value().Files)
  {
    if (File.FileName == Name)
    {
      return (File.FileType == RemoteFile::Directory) && !File.IsSymLink;
    }
  }
  return false;
}

void SessionDirectoryCache::FileModified(const QString & APath, bool SubDirs, bool ClearChanges)
{
  QString Path = ExcludeTrailingSlash(APath);
  QMutexLocker Lock(&FMutex);
  FGeneration++;
  if (!Path.startsWith(QLatin1Char('/')))
  {
    // cannot tell which directory it is
    RecordModification(FGeneration, QString(), true);
    FLists.clear();
    if (ClearChanges)
    {
      FChanges.Clear();
    }
    return;
  }
  RecordModification(FGeneration, Path, SubDirs);
  DoClearFileList(Path, SubDirs);
  DoClearFileList(UnixPath::Parent(Path), false);
  if (ClearChanges)
  {
    FChanges.ClearDirectoryChange(Path);
    FChanges.ClearDirectoryChangeTarget(Path);
  }
}

void SessionDirectoryCache::Clear()
{
  QMutexLocker Lock(&FMutex);
  FGeneration++;
  RecordModification(FGeneration, QString(), true);
  FLists.clear();
  FChanges.Clear();
}

bool SessionDirectoryCache::IsEmpty() const
{
  QMutexLocker Lock(&FMutex);
  return FLists.isEmpty() && FChanges.IsEmpty();
}

QString SessionDirectoryCache::SerializeChanges() const
{
  QMutexLocker Lock(&FMutex);
  return FChanges.Serialize();
}

void SessionDirectoryCache::DeserializeChanges(const QString & Data)
{
  QMutexLocker Lock(&FMutex);
  FChanges.Deserialize(Data);
}

//---------------------------------------------------------------------------
// DirectoryCaching

namespace DirectoryCaching
{

void ReadDirectory(FileSystem & Fs, const QString & Path, const QString & CurrentDirectory,
  QString & Directory, RemoteFileList & Files)
{
  CachingFileSystem * Caching = dynamic_cast<CachingFileSystem *>(&Fs);
  SessionDirectoryCache * Cache = (Caching != nullptr) ? Caching->DirectoryCache() : nullptr;
  if (Cache == nullptr)
  {
    Directory = Fs.CanonicalizePath(Path);
    Files = Fs.ReadDirectory(Directory);
    return;
  }

  QString Current = CurrentDirectory.startsWith(QLatin1Char('/')) ? SessionDirectoryCache::NormalizePath(CurrentDirectory) : QString();
  QString Requested = SessionDirectoryCache::NormalizePath(Path, Current);
  // TTerminal::ReloadDirectory: the current directory requested again
  bool Reload = !Current.isEmpty() && (Requested == Current);
  bool UseChanges = Caching->CacheDirectoryChanges();
  bool UseListings = Caching->CacheDirectories();

  QString Target;
  bool CachedTarget = false;
  // never use directory change cache during startup (no current directory
  // yet), this ensures, we never end-up initially in non-existing directory
  if (UseChanges && !Reload && !Current.isEmpty() && !Requested.isEmpty())
  {
    if (Cache->GetDirectoryChange(Current, Path, Target))
    {
      CachedTarget = true;
    }
    else if (Cache->IsKnownCanonical(Requested))
    {
      Target = Requested;
      CachedTarget = true;
    }
  }
  if (Reload)
  {
    if (UseListings)
    {
      Cache->ClearFileList(Current, false);
    }
    if (UseChanges)
    {
      Cache->ClearDirectoryChange(Current);
    }
  }

  auto Canonicalize = [&]()
  {
    Target = Fs.CanonicalizePath(Path);
    if (UseChanges)
    {
      Cache->AddDirectoryChange(Current, Path, Target);
    }
  };
  auto List = [&]()
  {
    if (UseListings && !Reload && Cache->GetFileList(Target, Files))
    {
      return;
    }
    quint64 Generation = Cache->Generation();
    Files = Fs.ReadDirectory(Target);
    if (UseListings)
    {
      Cache->AddFileList(Target, Files, Generation);
    }
  };

  if (!CachedTarget)
  {
    Canonicalize();
    List();
  }
  else
  {
    try
    {
      List();
    }
    catch (const FatalException &)
    {
      throw;
    }
    catch (const AbortException &)
    {
      throw;
    }
    catch (const ExtException &)
    {
      // The cached directory change is stale (directory removed or replaced
      // outside of this application): forget it and change directory properly
      Cache->FileModified(Target, true, true);
      Canonicalize();
      List();
    }
  }
  Directory = Target;
}

void ClearCaches(FileSystem & Fs)
{
  CachingFileSystem * Caching = dynamic_cast<CachingFileSystem *>(&Fs);
  if ((Caching != nullptr) && (Caching->DirectoryCache() != nullptr))
  {
    Caching->DirectoryCache()->Clear();
  }
}

bool AreCachesEmpty(FileSystem & Fs)
{
  CachingFileSystem * Caching = dynamic_cast<CachingFileSystem *>(&Fs);
  return (Caching == nullptr) || (Caching->DirectoryCache() == nullptr) || Caching->DirectoryCache()->IsEmpty();
}

}
