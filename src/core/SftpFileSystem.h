// Port of WinSCP source/core/SftpFileSystem.cpp, implemented on top of libssh
// instead of WinSCP's PuTTY fork; the SSH connection is core/SecureShell.
// Metadata requests that libssh 0.11 can only do one round trip at a time
// (READDIR, READLINK + STAT of symlinks, RENAME, SYMLINK) are sent as raw
// SFTP v3 packets on the libssh channel (WinSCP's TSFTPPacket equivalent),
// so they can be pipelined. With "Allow SCP fallback" (fsSFTP), a server
// without SFTP is served by an ScpFileSystem over the same connection.
#pragma once

#include "core/DirectoryCache.h"
#include "core/FileSystem.h"
#include "core/SessionData.h"
#include "core/SecureShell.h"

#include <libssh/libssh.h>
#include <libssh/sftp.h>

#include <QHash>
#include <memory>

class ScpFileSystem;

class SftpFileSystem : public FileSystem, public AnyCommandFileSystem, public CachingFileSystem
{
public:
  SftpFileSystem(const SessionData & Data, UserInterface * UI);
  ~SftpFileSystem() override;

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

  void Download(const QString & RemotePath, const QString & LocalPath, FileOperationProgress & Progress) override;
  void Upload(const QString & LocalPath, const QString & RemotePath, int Mode, FileOperationProgress & Progress) override;
  void Download(const QString & RemotePath, const QString & LocalPath, const TransferParams & Params, FileOperationProgress & Progress) override;
  void Upload(const QString & LocalPath, const QString & RemotePath, int Mode, const TransferParams & Params, FileOperationProgress & Progress) override;

  // AnyCommandFileSystem: runs the command in a separate exec channel
  int AnyCommand(const QString & Command, const QString & Directory, OutputEvent Output,
    FileOperationProgress & Progress, QString * NewDirectory) override;

  SecureShell & Shell() { return *FShell; }

  // Capabilities of the SCP fallback file system, when it is used
  bool IsCapable(FSCapability Capability, bool Default) const override;

  // CachingFileSystem (Cache visited remote directories / Cache directory changes)
  SessionDirectoryCache * DirectoryCache() const override;
  bool CacheDirectories() const override { return FData.CacheDirectories; }
  bool CacheDirectoryChanges() const override { return FData.CacheDirectoryChanges; }

  // Permissions and/or modification time in one SSH_FXP_SETSTAT
  // (TSFTPFileSystem::SFTPSource sends one SETSTAT after the upload).
  // Mode < 0 = keep permissions, invalid Time = keep time.
  void SetFileProperties(const QString & Path, int Mode, const QDateTime & Time);
  // The server is not SFTP: the session runs on the SCP fallback (fsSFTP)
  bool UsingScpFallback() const { return FFallback != nullptr; }
  ScpFileSystem * ScpFallback() const { return FFallback.get(); }

  // Server side operations (TSFTPFileSystem): SETSTAT with uid/gid (SFTP v3
  // changes ownership by ID; names are resolved from the listings),
  // copy-data / copy-file (directories: "cp" in a separate exec channel),
  // hardlink@openssh.com, statvfs@openssh.com, check-file(-name)
  void ChangeFileProperties(const QString & Path, const RemoteFile & File, const RemoteProperties & Properties) override;
  bool ChangesPropertiesRecursively() const override;
  void ChangeOwnerGroup(const QString & Path, const QString & Owner, const QString & Group) override;
  void CopyFile(const QString & From, const QString & To) override;
  void CreateHardLink(const QString & Path, const QString & Target) override;
  SpaceAvailableInfo SpaceAvailable(const QString & Path) override;
  QStringList ChecksumAlgorithms() const override;
  QString CalculateFileChecksum(const QString & Path, const QString & Alg) override;
  void LookupUsersGroups(QStringList & Users, QStringList & Groups) override;
  // The server announces the extension (any version data)
  bool SupportsExtension(const char * Name) const;

private:
  SessionData FData;
  UserInterface * FUI;
  std::unique_ptr<SecureShell> FShell;
  std::unique_ptr<SshChannel> FCommandChannel;
  sftp_session FSftp = nullptr;
  QString FHomeDirectory;
  std::shared_ptr<SessionDirectoryCache> FCache;
  std::unique_ptr<ScpFileSystem> FFallback;
  // FUtfStrings: asOn = UTF-8, asOff = legacy (Windows-1252) names,
  // asAuto = UTF-8 until the server sends an invalid UTF-8 string
  TAutoSwitch FUtfStrings = asOn;
  bool FSignedTS = false;
  int FListingQueue = 2;
  // raw packet layer
  QByteArray FRawOut;
  struct RawResponse
  {
    quint8 Type;
    QByteArray Payload;
  };
  QHash<quint32, RawResponse> FRawStash;

  RemoteFile FileFromAttributes(sftp_attributes Attrs, const QString & Directory);
  [[noreturn]] void SftpError(const QString & Message);
  [[noreturn]] void SshError(const QString & Message);
  void CheckConnection();
  QByteArray Path(const QString & Path) const;
  QString DecodeString(const char * Data, qsizetype Len);
  QDateTime ModificationFromUnix(quint32 Time, quint64 Time64) const;
  void DirectoryModified(const QString & Path, bool SubDirs, bool ClearChanges = false);
  bool OpenScpFallback(const QString & Reason);

  bool RawCapable() const;
  quint32 RawSend(quint8 Type, const QByteArray & Body);
  void RawFlush();
  void RawRead(char * Buffer, qsizetype Len);
  RawResponse RawReceive(quint32 Id);
  [[noreturn]] void RawStatusError(const RawResponse & Response, const QString & Message);
  void RawExpectStatus(quint32 Id, const QString & Message);
  RemoteFile RawFileFromName(const QByteArray & Payload, int & Pos, const QString & Directory, bool & Valid);
  RemoteFileList RawReadDirectory(const QString & Directory);
  // READLINK + STAT of the symlinks Links of Files (ResolveSymlinks), pipelined
  void ResolveSymlinks(const QString & Directory, RemoteFileList & Files, const QList<qsizetype> & Links);
  // User and group names seen in listings -> uid / gid (TRemoteTokenList)
  QHash<QString, quint32> FUserIds;
  QHash<QString, quint32> FGroupIds;
  void RememberTokens(const QString & Owner, const QString & Group, bool IdsValid, quint32 Uid, quint32 Gid);
  bool ResolveToken(const QString & Text, const QHash<QString, quint32> & Ids, quint32 & Id) const;
  QByteArray RawOpenHandle(const QString & APath, quint32 Flags, const QString & Message);
  void RawCloseHandle(const QByteArray & Handle);
  void ShellCopy(const QString & From, const QString & To);
};
