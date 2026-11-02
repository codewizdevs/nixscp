// Port of WinSCP source/core/ScpFileSystem.{h,cpp} (TSCPFileSystem): file
// operations by shell commands over an SSH session channel, transfers with
// the scp protocol ("scp -t" / "scp -f"); listing parser from
// source/core/RemoteFiles.cpp (TRemoteFile::SetListingStr).
// Batch transfers (one "scp -r -d -t/-f" per selection, T records for
// times) are TSCPFileSystem::CopyToRemote/SCPSource/SCPDirectorySource and
// CopyToLocal/SCPSink.
#pragma once

#include "core/DirectoryCache.h"
#include "core/FileSystem.h"
#include "core/SessionData.h"
#include "core/SecureShell.h"

#include <QDate>
#include <QFile>
#include <QSet>
#include <QStringList>
#include <functional>
#include <memory>

// Options of ScpFileSystem::UploadBatch / DownloadBatch (the parts of
// TCopyParamType the scp protocol carries)
struct ScpTransferOptions
{
  bool PreserveTime = false;      // T records (upload) / applied from T records (download, "-p")
  bool PreserveTimeDirs = true;   // with PreserveTime: times of directories too (CopyParam::PreserveTimeDirs)
  bool PreserveRights = false;    // modes of the sources ("-p"), otherwise FileMode / DirMode (upload)
  unsigned int FileMode = 0644;
  unsigned int DirMode = 0755;
  // Upload: mode sent in the C / D record (TCopyParamType::RemoteFileRights);
  // overrides PreserveRights' local modes and FileMode / DirMode
  std::function<unsigned int(const QString & SourcePath, bool Directory)> Mode;
  // Called before each file or directory (at any depth) is transferred;
  // false skips it (file masks, exclusions, overwrite answered No). Skipped
  // files are not counted to the progress (the caller accounts for them).
  std::function<bool(const QString & SourcePath, bool Directory)> Include;
  // Name of the target of a file or directory (TTerminal::ChangeFileName,
  // file mask); empty = the source name. Must not contain '/'.
  std::function<QString(const QString & SourcePath, bool Directory)> TargetName;
  // Text mode of a file (CopyParam::UseAsciiTransfer): line endings are
  // converted between LocalEOL and SessionData::EOLType; uploads also remove
  // the BOM / the trailing ^Z with RemoveBOM / RemoveCtrlZ
  std::function<bool(const QString & SourcePath)> AsciiTransfer;
  bool RemoveBOM = false;
  bool RemoveCtrlZ = false;
  // Called after each file or directory is completely transferred
  // (TargetPath: full path of the target)
  std::function<void(const QString & SourcePath, const QString & TargetPath, bool Directory)> Finished;
  // Called for each file or directory that failed (EScpFileSkipped /
  // ESkipFile); return false to abort the batch (the handler may also throw).
  // Without a handler the batch continues and the first error is thrown when
  // it is done. Download errors reported by the remote scp carry the path
  // from its message when there is one, otherwise the requested path.
  std::function<bool(const QString & SourcePath, const ExtException & Error)> Error;
};

class ScpFileSystem : public FileSystem, public AnyCommandFileSystem, public CachingFileSystem
{
public:
  ScpFileSystem(const SessionData & Data, UserInterface * UI);
  // Runs on an already opened SSH connection owned by the caller (the SCP
  // fallback of an SFTP session, "Allow SCP fallback"); Close leaves the
  // connection open
  ScpFileSystem(const SessionData & Data, UserInterface * UI, SecureShell * ExistingShell);
  ~ScpFileSystem() override;

  void Open() override;
  void Close() override;
  bool IsOpened() const override;
  QString ProtocolName() const override;
  QString ServerInfo() const override;

  QString CanonicalizePath(const QString & Path) override;
  QString HomeDirectory() override;
  RemoteFileList ReadDirectory(const QString & Path) override;
  RemoteFile ReadFile(const QString & Path) override;
  bool FileExists(const QString & Path, RemoteFile * File) override;
  void CreateDirectory(const QString & Path, int Mode) override;
  void DeleteFile(const QString & Path) override;
  void RemoveDirectory(const QString & Path) override;
  void RenameFile(const QString & From, const QString & To) override;
  void ChangeMode(const QString & Path, unsigned int Mode) override;
  void SetModificationTime(const QString & Path, const QDateTime & Time) override;
  void CreateLink(const QString & Path, const QString & Target) override;

  // SCP cannot resume (fcResumeSupport is off in WinSCP): with
  // Params.ResumeOffset > 0 the whole file is transferred again, only the part
  // beyond the offset is counted to the progress (the offset was added by the
  // caller with AddResumed).
  void Download(const QString & RemotePath, const QString & LocalPath, FileOperationProgress & Progress) override;
  void Upload(const QString & LocalPath, const QString & RemotePath, int Mode, FileOperationProgress & Progress) override;
  void Download(const QString & RemotePath, const QString & LocalPath, const TransferParams & Params, FileOperationProgress & Progress) override;
  void Upload(const QString & LocalPath, const QString & RemotePath, int Mode, const TransferParams & Params, FileOperationProgress & Progress) override;

  // AnyCommandFileSystem: runs the command in the session shell
  int AnyCommand(const QString & Command, const QString & Directory, OutputEvent Output,
    FileOperationProgress & Progress, QString * NewDirectory) override;

  SecureShell & Shell() { return *FShell; }

  // CachingFileSystem (Cache visited remote directories / Cache directory changes)
  SessionDirectoryCache * DirectoryCache() const override { return FCache.get(); }
  bool CacheDirectories() const override { return FData.CacheDirectories; }
  bool CacheDirectoryChanges() const override { return FData.CacheDirectoryChanges; }

  // TSCPFileSystem::CopyToRemote: all LocalPaths (files and directories,
  // recursively) in one "scp -r [-p] -d -t TargetDir" session; times as T
  // records with Options.PreserveTime. Files keep their local names unless
  // Options.TargetName renames them. No overwrite confirmation (the caller
  // checks against the target listing). A file interrupted by an exception
  // (cancel, skip) is deleted on the server (SCP cannot resume).
  void UploadBatch(const QStringList & LocalPaths, const QString & TargetDir, const ScpTransferOptions & Options,
    FileOperationProgress & Progress);
  // TSCPFileSystem::CopyToLocal: one "scp -r [-p] -d -f Path" per RemotePaths
  // item into TargetDir; exact times from the T records
  void DownloadBatch(const QStringList & RemotePaths, const QString & TargetDir, const ScpTransferOptions & Options,
    FileOperationProgress & Progress);
  // "chmod -R" (TSCPFileSystem::ChangeFileProperties with Recursive)
  void ChangeModeRecursive(const QString & Path, unsigned int Mode);
  // DeleteFile and RemoveDirectory delete recursively ("rm -f -r"), so the
  // caller does not need to delete the contents of a directory first
  bool DeletesRecursively() const { return true; }
  // TSCPFileSystem::LookupUsersGroups ("groups"), with "Lookup user groups"
  // on or auto; empty when off or when the command fails. Cached.
  QStringList LookupUsersGroups();

  // Server side operations by shell commands (TSCPFileSystem):
  // "chgrp/chown/chmod [-R]" (tri-state rights as symbolic mode), "cp -p -r -f",
  // "ln" (hard link)
  void ChangeFileProperties(const QString & Path, const RemoteFile & File, const RemoteProperties & Properties) override;
  bool ChangesPropertiesRecursively() const override { return true; }
  void ChangeOwnerGroup(const QString & Path, const QString & Owner, const QString & Group) override;
  void CopyFile(const QString & From, const QString & To) override;
  void CreateHardLink(const QString & Path, const QString & Target) override;
  void LookupUsersGroups(QStringList & Users, QStringList & Groups) override;

  // TRemoteFile::SetListingStr: parses one "ls -la" line. Today is used for
  // listings without year ("Oct 31 22:15"). Throws ExtException
  // ("Unexpected directory listing line '%s'.") for unparsable lines.
  static RemoteFile ParseListingLine(const QString & Line, const QDate & Today = QDate::currentDate());
  // The SCP file system of Fs: Fs itself, or the SCP fallback of an SFTP session
  static ScpFileSystem * Of(FileSystem & Fs);
  // TSCPFileSystem::IsTotalListingLine
  static bool IsTotalListingLine(const QString & Line);

private:
  SessionData FData;
  std::unique_ptr<SecureShell> FOwnShell;
  SecureShell * FShell;
  std::unique_ptr<SshChannel> FChannel;
  QString FReturnVar;            // "status" or "?"; empty while detecting
  TAutoSwitch FLsFullTime;
  QStringList FOutput;
  int FReturnCode = 0;
  bool FScpFatalError = false;
  QString FHomeDirectory;
  QString FShellDirectory;       // working directory of the shell, empty = unknown
  bool FProcessingCommand = false;
  std::shared_ptr<SessionDirectoryCache> FCache;
  bool FGroupsLookedUp = false;
  QStringList FGroups;

  void StartShell();
  void ResetShell();
  void CheckConnection();
  void SkipStartupMessage();
  void DetectReturnVar();
  void ClearAliases();
  void UnsetNationalVars();

  QString FullCommand(const QString & Command, bool Interactive, bool NewLine = false) const;
  void SendCommand(const QString & Command);
  bool RemoveLastLine(QString & Line, int & ReturnCode) const;
  bool TryRemoveLastLine(QString & Line);
  void SkipFirstLine();
  void ReadCommandOutput(int Params, const QString * Command = nullptr, const OutputEvent * Output = nullptr);
  void ExecCommand(const QString & Command, int Params, int MinLines = -1);
  [[noreturn]] void InvalidOutputError(const QString & Command);
  // Runs Command, rethrowing a non-fatal failure as Message with the details
  void ExecCommandOrError(const QString & Command, const QString & Message, int Params = -1);

  QString ListingOptions(bool Auto) const;
  RemoteFile ParseFile(const QString & Line, const QDate & Today = QDate::currentDate()) const;
  void DirectoryModified(const QString & Path, bool SubDirs, bool ClearChanges = false);
  QString ScpOptions(bool PreserveRights, bool PreserveTime) const;
  void BatchSource(const QString & LocalPath, const QString & TargetDir, const ScpTransferOptions & Options,
    FileOperationProgress & Progress, QSet<QString> & Visited, QString & Transferring,
    QList<std::pair<QString, ExtException>> & Errors);
  // Text mode upload: the whole converted file (TSCPFileSystem::SCPSource AsciiBuf)
  QByteArray LoadTextFile(QFile & Local, const QString & LocalPath, bool RemoveBOM, bool RemoveCtrlZ) const;
  bool BatchError(const ScpTransferOptions & Options, const QString & Path, const ExtException & E,
    QList<std::pair<QString, ExtException>> & Errors);
  void BatchSink(const QString & RemotePath, const QString & TargetDir, const ScpTransferOptions & Options,
    FileOperationProgress & Progress, QList<std::pair<QString, ExtException>> & Errors);
  void ResolveSymlinks(const QString & Directory, RemoteFileList & Files);
  QString AbsolutePath(const QString & Path) const;

  void SCPResponse(bool * GotLastLine = nullptr);
  void SCPSendError(const QString & Message, bool Fatal);
};
