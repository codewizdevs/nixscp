// Local file system primitives of LocalFileOperations, one implementation per
// operating system (LocalFileSystemPosix.cpp, LocalFileSystemWin.cpp). The
// copy/move engine itself (prompts, progress, temporary files) is shared.
// Errors are reported as an error code (errno / Win32), see ErrorText().
#pragma once

#include <QString>
#include <QStringList>

#include <functional>

namespace LocalFs
{
  struct Stat
  {
    enum TType { Regular, Directory, Symlink, Special };
    TType Type = Regular;
    qint64 Size = 0;
    qint64 ModifiedSec = 0;
    qint64 ModifiedNsec = 0;
    qint64 AccessedSec = 0;
    qint64 AccessedNsec = 0;
    // File identity (device + inode / volume serial + file index); 0 = unknown
    quint64 Device = 0;
    quint64 Inode = 0;
    // Unix permission bits (07777); Windows: 0444 for read-only, else 0666 (0777 directories)
    unsigned int Mode = 0;
    unsigned int Uid = 0;
    unsigned int Gid = 0;
    // POSIX special files (FIFO, devices): the file type bits and device number
    unsigned int RawType = 0;
    quint64 RawDevice = 0;

    bool IsDirectory() const { return Type == Directory; }
    bool IsRegular() const { return Type == Regular; }
    bool IsSymlink() const { return Type == Symlink; }
    bool SameFile(const Stat & Other) const { return (Inode != 0) && (Device == Other.Device) && (Inode == Other.Inode); }
  };

  int LastError();
  QString ErrorText(int Error);
  bool IsExistsError(int Error);
  bool IsCrossDeviceError(int Error);

  // Without following a final symbolic link / junction
  bool LStat(const QString & Path, Stat & Result);
  // Following links
  bool StatFollow(const QString & Path, Stat & Result);
  // Names of the entries (no "." and ".."); throws ExtException
  QStringList ListDirectory(const QString & Path);

  bool MakeDirectory(const QString & Path);
  bool RemoveDirectory(const QString & Path);
  bool RemoveFile(const QString & Path);
  // Replaces an existing Target file (as rename(2)); false with
  // IsCrossDeviceError when Target is on another file system
  bool Rename(const QString & Source, const QString & Target);

  // Copies the data of the regular file Source into the new file Target
  // (created exclusively; IsExistsError when it exists). Progress gets the
  // bytes copied since the last call and returns false to cancel (then
  // Cancelled is set). Reflink: copy-on-write clone where supported.
  enum TCopyStep { csOpenSource, csCreateTarget, csCopy };
  bool CopyFileData(const QString & Source, const QString & Target, qint64 Size, bool Reflink,
    const std::function<bool(qint64 Delta)> & Progress, bool & Cancelled, TCopyStep & Step);
  // Link at Path pointing to Target (symbolic: Target as given; hard: an existing file)
  bool CreateLink(const QString & Path, const QString & Target, bool Symbolic);
  // Recreates a symbolic link / special file Source (described by St) as Target
  bool CopySpecial(const QString & Source, const Stat & St, const QString & Target);

  // Metadata; NoFollow: of a symbolic link itself
  bool SetMode(const QString & Path, unsigned int Mode);
  bool SetTimes(const QString & Path, const Stat & From, bool NoFollow);
  // Best effort (only root may give files away; own groups work); never fails
  void CopyOwner(const QString & Path, const Stat & From, bool NoFollow);
  bool ChangeOwner(const QString & Path, unsigned int Uid, unsigned int Gid, bool ChangeUid, bool ChangeGid);
  // Permission bits of a newly created file or directory (0666/0777 minus umask)
  unsigned int DefaultMode(bool Directory);
  // Whether the platform has Unix owners/permissions (Properties dialog)
  bool HasUnixPermissions();

  // Users and groups (Properties dialog); numeric names are accepted
  bool ResolveUser(const QString & Name, unsigned int & Uid);
  bool ResolveGroup(const QString & Name, unsigned int & Gid);
  QStringList UserNames();
  QStringList GroupNames();
  QString OwnerName(unsigned int Uid);
  QString GroupName(unsigned int Gid);

  // Trash fallbacks after QFile::moveToTrash failed (Linux: "gio trash"); Output: tool messages
  bool MoveToTrashFallback(const QString & Path, QString & Output);
}
