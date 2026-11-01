// Port of WinSCP source/core/RemoteFiles.cpp (TRemoteDirectoryCache,
// TRemoteDirectoryChangesCache) and the directory cache parts of
// source/core/Terminal.cpp (TTerminal::ReadDirectory, ChangeDirectory,
// DirectoryModified, ClearCaches, AreCachesEmpty) and
// source/core/Configuration.cpp (Load/SaveDirectoryChangesCache).
//
// Differences to WinSCP:
//  - The cache is shared by all sessions of this process connected to the
//    same server (same protocol, user, host, port, shell / SFTP server), not
//    only by a terminal and its secondary terminals. So a background (queue)
//    transfer invalidates the listing the panel session has cached.
//  - The file system itself reports its modifications (DirectoryModified),
//    so all operations (transfers, synchronization, scripts, custom commands)
//    keep the cache consistent without help from Terminal.
//  - A path is known to be canonical (no CanonicalizePath round trip) when it
//    was the result of a canonicalization, is an ancestor of such a path, or
//    is a plain (non-symlink) subdirectory in the cached listing of a known
//    canonical parent.
#pragma once

#include "core/RemoteFile.h"

#include <QHash>
#include <QMutex>
#include <QString>
#include <QStringList>

#include <memory>

class FileSystem;
class SessionData;

// TRemoteDirectoryChangesCache: "SourceDir,Change" -> TargetDir, and
// "TargetDir" -> "//" for every directory known to be canonical. Ordered by
// last use (most recent last); Serialize keeps the MaxSize most recent ones.
class RemoteDirectoryChangesCache
{
public:
  explicit RemoteDirectoryChangesCache(int MaxSize = 100) : FMaxSize(MaxSize) {}

  void Clear() { FNames.clear(); FValues.clear(); }
  bool IsEmpty() const { return FNames.isEmpty(); }
  int Count() const { return static_cast<int>(FNames.size()); }

  void AddDirectoryChange(const QString & SourceDir, const QString & Change, const QString & TargetDir);
  void ClearDirectoryChange(const QString & SourceDir);
  void ClearDirectoryChangeTarget(const QString & TargetDir);
  bool GetDirectoryChange(const QString & SourceDir, const QString & Change, QString & TargetDir);
  // A canonicalization result (stored as "TargetDir=//")
  bool IsKnownTarget(const QString & Directory) const;
  // Some known target is Directory or below it
  bool HasTargetBelow(const QString & Directory) const;

  // "A" + Name=Value lines (TStringList::Text)
  QString Serialize() const;
  void Deserialize(const QString & Data);

  static bool DirectoryChangeKey(const QString & SourceDir, const QString & Change, QString & Key);

private:
  int FMaxSize;
  QStringList FNames;               // in order of last use
  QHash<QString, QString> FValues;

  void SetValue(const QString & Name, const QString & Value);
  QString GetValue(const QString & Name);
  void Delete(const QString & Name);
};

// Listing and directory change caches of one server, shared by all sessions
// connected to it; thread safe (file systems of several Terminals use it
// from their worker threads).
class SessionDirectoryCache
{
public:
  // The cache for the server of Data (created on first use). With
  // Data.CacheDirectoryChanges && Data.PreserveDirectoryChanges the directory
  // changes are loaded from the configuration ([Configuration\CDCache],
  // value SessionKey) on creation and saved there when the last user
  // releases the cache.
  static std::shared_ptr<SessionDirectoryCache> Acquire(const SessionData & Data);
  // Whether PreserveDirectoryChanges reads and writes the configuration
  // (Configuration::Instance()). Default: only in a GUI application
  // (QGuiApplication), so tests and tools never touch the user's file.
  static void SetPersistence(bool Enabled);
  // Registry key of Data (protocol, user, host, port, shell / SFTP server)
  static QString ServerKey(const SessionData & Data);
  // TSessionData::SessionKey: "user@host[:port]" (port only when not the default)
  static QString SessionKey(const SessionData & Data);
  // "/a/./b/../c/" -> "/a/c" (TTerminal::ExpandFileName / AbsolutePath for
  // absolute paths; relative paths are combined with Base first)
  static QString NormalizePath(const QString & Path, const QString & Base = QString());

  ~SessionDirectoryCache();

  // ---- listing cache (TRemoteDirectoryCache)
  bool HasFileList(const QString & Directory) const;
  bool GetFileList(const QString & Directory, RemoteFileList & Files) const;
  // Value of the modification counter; take it before listing a directory
  // and pass it to AddFileList, so a listing that raced with a modification
  // (of another session) is not cached
  quint64 Generation() const;
  void AddFileList(const QString & Directory, const RemoteFileList & Files, quint64 StartGeneration);
  void ClearFileList(const QString & Directory, bool SubDirs);

  // ---- directory changes cache (TRemoteDirectoryChangesCache)
  bool GetDirectoryChange(const QString & SourceDir, const QString & Change, QString & TargetDir);
  void AddDirectoryChange(const QString & SourceDir, const QString & Change, const QString & TargetDir);
  void ClearDirectoryChange(const QString & SourceDir);
  // Directory needs no CanonicalizePath (see the header comment)
  bool IsKnownCanonical(const QString & Directory) const;

  // A file or directory was modified (TTerminal::DirectoryModified +
  // ReactOnCommand): Path itself (with its subdirectories when SubDirs) and
  // its parent directory are removed from the listing cache. With
  // ClearChanges (directory deleted or renamed) the directory changes from
  // and to Path are forgotten too.
  void FileModified(const QString & Path, bool SubDirs, bool ClearChanges);

  // TTerminal::ClearCaches / AreCachesEmpty
  void Clear();
  bool IsEmpty() const;

  // Persistence (PreserveDirectoryChanges); public for tests
  QString SerializeChanges() const;
  void DeserializeChanges(const QString & Data);

private:
  SessionDirectoryCache() = default;

  struct Entry
  {
    RemoteFileList Files;
  };
  struct ModificationRecord
  {
    quint64 Generation;
    QString Path;        // empty = everything
    bool SubDirs;
  };

  mutable QMutex FMutex;
  QHash<QString, Entry> FLists;
  RemoteDirectoryChangesCache FChanges;
  quint64 FGeneration = 0;
  bool FPreserve = false;
  QString FSessionKey;
  // recent modifications, to reject listings that raced with them
  QList<ModificationRecord> FModifications;

  void DoClearFileList(const QString & Directory, bool SubDirs);
  void RecordModification(quint64 Generation, const QString & Path, bool SubDirs);
  bool ModifiedSince(quint64 Generation, const QString & Directory) const;
};

// Implemented by file systems that keep a SessionDirectoryCache
// (SFTP and SCP; WinSCP keeps the cache in TTerminal for all protocols)
class CachingFileSystem
{
public:
  virtual ~CachingFileSystem() = default;
  // Null when the session caches neither listings nor directory changes
  virtual SessionDirectoryCache * DirectoryCache() const = 0;
  virtual bool CacheDirectories() const = 0;
  virtual bool CacheDirectoryChanges() const = 0;
};

namespace DirectoryCaching
{
  // The cached part of TTerminal::ChangeDirectory + ReadDirectory, called by
  // Terminal::ReadDirectory on the worker thread. Path is the requested
  // directory, CurrentDirectory the current one when the request was made.
  // Requesting the current directory again is a reload (ReloadDirectory,
  // Ctrl+R, refresh after an operation): its listing and directory change
  // are not taken from the cache, but stored to it. Other directories
  // (Back, Forward, Parent, subdirectories) are taken from the cache when
  // the session has "Cache visited remote directories"; "Cache directory
  // changes" skips the canonicalization of directories already seen.
  // Without a caching file system: Directory = Fs.CanonicalizePath(Path),
  // Files = Fs.ReadDirectory(Directory).
  void ReadDirectory(FileSystem & Fs, const QString & Path, const QString & CurrentDirectory,
    QString & Directory, RemoteFileList & Files);
  // Commands > Clear Caches (TTerminal::ClearCaches); worker thread
  void ClearCaches(FileSystem & Fs);
  // TTerminal::AreCachesEmpty (the command is disabled then); worker thread
  bool AreCachesEmpty(FileSystem & Fs);
}
