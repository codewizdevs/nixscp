// Local file system primitives for Linux (see LocalFileSystem.h)
#include "core/LocalFileSystem.h"
#include "core/FileSystem.h"

#ifndef Q_OS_WIN

#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <linux/fs.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace LocalFs
{

static QByteArray Native(const QString & Path)
{
  return QFile::encodeName(Path);
}

class FdGuard
{
public:
  explicit FdGuard(int Fd = -1) : FFd(Fd) {}
  FdGuard(const FdGuard &) = delete;
  FdGuard & operator=(const FdGuard &) = delete;
  ~FdGuard() { if (FFd >= 0) ::close(FFd); }
  int Get() const { return FFd; }
  int Release() { int Fd = FFd; FFd = -1; return Fd; }
private:
  int FFd;
};

int LastError()
{
  return errno;
}

QString ErrorText(int Error)
{
  return QString::fromLocal8Bit(std::strerror(Error));
}

bool IsExistsError(int Error)
{
  return Error == EEXIST;
}

bool IsCrossDeviceError(int Error)
{
  return Error == EXDEV;
}

static void Convert(const struct stat & St, Stat & Result)
{
  Result = Stat();
  if (S_ISDIR(St.st_mode)) Result.Type = Stat::Directory;
  else if (S_ISREG(St.st_mode)) Result.Type = Stat::Regular;
  else if (S_ISLNK(St.st_mode)) Result.Type = Stat::Symlink;
  else Result.Type = Stat::Special;
  Result.Size = St.st_size;
  Result.ModifiedSec = St.st_mtim.tv_sec;
  Result.ModifiedNsec = St.st_mtim.tv_nsec;
  Result.AccessedSec = St.st_atim.tv_sec;
  Result.AccessedNsec = St.st_atim.tv_nsec;
  Result.Device = static_cast<quint64>(St.st_dev);
  Result.Inode = static_cast<quint64>(St.st_ino);
  Result.Mode = St.st_mode & 07777;
  Result.Uid = St.st_uid;
  Result.Gid = St.st_gid;
  Result.RawType = St.st_mode & S_IFMT;
  Result.RawDevice = static_cast<quint64>(St.st_rdev);
}

bool LStat(const QString & Path, Stat & Result)
{
  struct stat St;
  if (lstat(Native(Path).constData(), &St) != 0)
  {
    return false;
  }
  Convert(St, Result);
  return true;
}

bool StatFollow(const QString & Path, Stat & Result)
{
  struct stat St;
  if (::stat(Native(Path).constData(), &St) != 0)
  {
    return false;
  }
  Convert(St, Result);
  return true;
}

QStringList ListDirectory(const QString & Path)
{
  QStringList Result;
  DIR * Dir = opendir(Native(Path).constData());
  if (Dir == nullptr)
  {
    int Error = errno;
    throw ExtException(QStringLiteral("Error listing directory '%1'.").arg(Path), ErrorText(Error));
  }
  while (dirent * Entry = readdir(Dir))
  {
    if ((std::strcmp(Entry->d_name, ".") == 0) || (std::strcmp(Entry->d_name, "..") == 0))
    {
      continue;
    }
    Result.append(QFile::decodeName(QByteArray(Entry->d_name)));
  }
  closedir(Dir);
  return Result;
}

bool MakeDirectory(const QString & Path)
{
  return mkdir(Native(Path).constData(), 0700) == 0;
}

bool RemoveDirectory(const QString & Path)
{
  return rmdir(Native(Path).constData()) == 0;
}

bool RemoveFile(const QString & Path)
{
  return unlink(Native(Path).constData()) == 0;
}

bool Rename(const QString & Source, const QString & Target)
{
  return rename(Native(Source).constData(), Native(Target).constData()) == 0;
}

bool CopyFileData(const QString & Source, const QString & Target, qint64 Size, bool Reflink,
  const std::function<bool(qint64)> & Progress, bool & Cancelled, TCopyStep & Step)
{
  Cancelled = false;
  Step = csOpenSource;
  FdGuard In(open(Native(Source).constData(), O_RDONLY | O_CLOEXEC | O_NOCTTY));
  if (In.Get() < 0)
  {
    return false;
  }
  Step = csCreateTarget;
  FdGuard Out(open(Native(Target).constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOCTTY, 0600));
  if (Out.Get() < 0)
  {
    return false;
  }
  Step = csCopy;
  // Copy-on-write clone (btrfs, xfs, bcachefs; same file system only)
  if (Reflink && (Size > 0) && (ioctl(Out.Get(), FICLONE, In.Get()) == 0))
  {
    Progress(Size);
    return (::close(Out.Release()) == 0);
  }
  bool UseCopyFileRange = true;
  qint64 Copied = 0;
  std::vector<char> Buffer;
  for (;;)
  {
    if (!Progress(0))
    {
      Cancelled = true;
      return false;
    }
    ssize_t Count;
    if (UseCopyFileRange)
    {
      Count = copy_file_range(In.Get(), nullptr, Out.Get(), nullptr, 8 * 1024 * 1024, 0);
      if ((Count < 0) && (Copied == 0) &&
          ((errno == EXDEV) || (errno == EINVAL) || (errno == ENOSYS) || (errno == EOPNOTSUPP) ||
           (errno == EBADF) || (errno == EPERM) || (errno == ETXTBSY)))
      {
        UseCopyFileRange = false;
        continue;
      }
    }
    else
    {
      if (Buffer.empty())
      {
        Buffer.resize(1024 * 1024);
      }
      Count = read(In.Get(), Buffer.data(), Buffer.size());
      if (Count > 0)
      {
        ssize_t Written = 0;
        while (Written < Count)
        {
          ssize_t W = write(Out.Get(), Buffer.data() + Written, static_cast<size_t>(Count - Written));
          if (W < 0)
          {
            if (errno == EINTR)
            {
              continue;
            }
            return false;
          }
          Written += W;
        }
      }
    }
    if (Count < 0)
    {
      if (errno == EINTR)
      {
        continue;
      }
      return false;
    }
    if (Count == 0)
    {
      break;
    }
    Copied += Count;
    if (!Progress(Count))
    {
      Cancelled = true;
      return false;
    }
  }
  return (::close(Out.Release()) == 0);
}

bool CreateLink(const QString & Path, const QString & Target, bool Symbolic)
{
  return Symbolic ? (symlink(Native(Target).constData(), Native(Path).constData()) == 0) :
    (link(Native(Target).constData(), Native(Path).constData()) == 0);
}

bool CopySpecial(const QString & Source, const Stat & St, const QString & Target)
{
  if (St.IsSymlink())
  {
    std::vector<char> Buffer(static_cast<size_t>(std::max<qint64>(St.Size, 255)) + 1);
    QByteArray LinkTarget;
    for (;;)
    {
      ssize_t Length = readlink(Native(Source).constData(), Buffer.data(), Buffer.size());
      if (Length < 0)
      {
        return false;
      }
      if (static_cast<size_t>(Length) < Buffer.size())
      {
        LinkTarget = QByteArray(Buffer.data(), static_cast<int>(Length));
        break;
      }
      Buffer.resize(Buffer.size() * 2);
    }
    return symlink(LinkTarget.constData(), Native(Target).constData()) == 0;
  }
  if (St.RawType == S_IFIFO)
  {
    return mkfifo(Native(Target).constData(), 0600) == 0;
  }
  // character/block devices and sockets: only root can create devices
  return mknod(Native(Target).constData(), static_cast<mode_t>(St.RawType) | 0600, static_cast<dev_t>(St.RawDevice)) == 0;
}

bool SetMode(const QString & Path, unsigned int Mode)
{
  return chmod(Native(Path).constData(), static_cast<mode_t>(Mode & 07777)) == 0;
}

bool SetTimes(const QString & Path, const Stat & From, bool NoFollow)
{
  struct timespec Times[2];
  Times[0].tv_sec = static_cast<time_t>(From.AccessedSec);
  Times[0].tv_nsec = static_cast<long>(From.AccessedNsec);
  Times[1].tv_sec = static_cast<time_t>(From.ModifiedSec);
  Times[1].tv_nsec = static_cast<long>(From.ModifiedNsec);
  return utimensat(AT_FDCWD, Native(Path).constData(), Times, NoFollow ? AT_SYMLINK_NOFOLLOW : 0) == 0;
}

void CopyOwner(const QString & Path, const Stat & From, bool NoFollow)
{
  if ((From.Uid == geteuid()) && (From.Gid == getegid()))
  {
    return;
  }
  int Flags = NoFollow ? AT_SYMLINK_NOFOLLOW : 0;
  if (fchownat(AT_FDCWD, Native(Path).constData(), From.Uid, From.Gid, Flags) != 0)
  {
    // a group change works for own groups
    if (fchownat(AT_FDCWD, Native(Path).constData(), static_cast<uid_t>(-1), From.Gid, Flags) != 0) {}
  }
}

bool ChangeOwner(const QString & Path, unsigned int Uid, unsigned int Gid, bool ChangeUid, bool ChangeGid)
{
  return fchownat(AT_FDCWD, Native(Path).constData(), ChangeUid ? static_cast<uid_t>(Uid) : static_cast<uid_t>(-1),
    ChangeGid ? static_cast<gid_t>(Gid) : static_cast<gid_t>(-1), AT_SYMLINK_NOFOLLOW) == 0;
}

// The process umask without changing it (umask(2) cannot be read atomically)
static mode_t CurrentUmask()
{
  static mode_t Result = []()
  {
    QFile Status(QStringLiteral("/proc/self/status"));
    if (Status.open(QIODevice::ReadOnly))
    {
      for (const QByteArray & Line : Status.readAll().split('\n'))
      {
        if (Line.startsWith("Umask:"))
        {
          bool Ok = false;
          unsigned int Value = Line.mid(6).trimmed().toUInt(&Ok, 8);
          if (Ok)
          {
            return static_cast<mode_t>(Value);
          }
        }
      }
    }
    return static_cast<mode_t>(022);
  }();
  return Result;
}

unsigned int DefaultMode(bool Directory)
{
  return (Directory ? 0777 : 0666) & ~CurrentUmask();
}

bool HasUnixPermissions()
{
  return true;
}

bool ResolveUser(const QString & Name, unsigned int & Uid)
{
  bool Numeric = false;
  unsigned int Value = Name.toUInt(&Numeric);
  if (Numeric)
  {
    Uid = Value;
    return true;
  }
  struct passwd * Entry = getpwnam(Name.toLocal8Bit().constData());
  if (Entry == nullptr)
  {
    return false;
  }
  Uid = Entry->pw_uid;
  return true;
}

bool ResolveGroup(const QString & Name, unsigned int & Gid)
{
  bool Numeric = false;
  unsigned int Value = Name.toUInt(&Numeric);
  if (Numeric)
  {
    Gid = Value;
    return true;
  }
  struct group * Entry = getgrnam(Name.toLocal8Bit().constData());
  if (Entry == nullptr)
  {
    return false;
  }
  Gid = Entry->gr_gid;
  return true;
}

QStringList UserNames()
{
  QStringList Result;
  setpwent();
  while (struct passwd * Entry = getpwent())
  {
    Result.append(QString::fromLocal8Bit(Entry->pw_name));
  }
  endpwent();
  Result.removeDuplicates();
  Result.sort(Qt::CaseInsensitive);
  return Result;
}

QStringList GroupNames()
{
  QStringList Result;
  setgrent();
  while (struct group * Entry = getgrent())
  {
    Result.append(QString::fromLocal8Bit(Entry->gr_name));
  }
  endgrent();
  Result.removeDuplicates();
  Result.sort(Qt::CaseInsensitive);
  return Result;
}

QString OwnerName(unsigned int Uid)
{
  struct passwd * Entry = getpwuid(static_cast<uid_t>(Uid));
  return (Entry != nullptr) ? QString::fromLocal8Bit(Entry->pw_name) : QString::number(Uid);
}

QString GroupName(unsigned int Gid)
{
  struct group * Entry = getgrgid(static_cast<gid_t>(Gid));
  return (Entry != nullptr) ? QString::fromLocal8Bit(Entry->gr_name) : QString::number(Gid);
}

bool MoveToTrashFallback(const QString & Path, QString & Output)
{
  // GVfs knows more (network and FUSE mounts under /run/user/<uid>/gvfs)
  QString Gio = QStandardPaths::findExecutable(QStringLiteral("gio"));
  if (Gio.isEmpty())
  {
    return false;
  }
  QFileInfo Info(Path);
  QProcess Process;
  Process.setProcessChannelMode(QProcess::MergedChannels);
  Process.start(Gio, {QStringLiteral("trash"), QStringLiteral("--"), Info.absoluteFilePath()});
  if (Process.waitForFinished(60000) && (Process.exitStatus() == QProcess::NormalExit) && (Process.exitCode() == 0))
  {
    QFileInfo After(Path);
    if (!After.exists() && !After.isSymLink())
    {
      return true;
    }
  }
  else if (Process.state() != QProcess::NotRunning)
  {
    Process.kill();
    Process.waitForFinished(1000);
  }
  Output = QString::fromLocal8Bit(Process.readAll()).trimmed();
  return false;
}

}

#endif
