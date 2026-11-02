// Local file system primitives for Windows (see LocalFileSystem.h). Wide
// (UTF-16) Win32 functions only, so any file name works, with the \\?\ prefix
// for long paths. Unix owners and permissions do not exist here: the
// read-only attribute stands in for the write permission.
#include "core/LocalFileSystem.h"
#include "core/FileSystem.h"

#ifdef Q_OS_WIN

#include <QDir>
#include <QFileInfo>

#include <string>

namespace LocalFs
{

static std::wstring Wide(const QString & Path)
{
  QString Native = QDir::toNativeSeparators(QDir::cleanPath(Path));
  // \\?\ lifts MAX_PATH (absolute paths only; UNC paths become \\?\UNC\...)
  if (Native.size() >= MAX_PATH - 12)
  {
    if (Native.startsWith(QStringLiteral("\\\\")))
    {
      Native = QStringLiteral("\\\\?\\UNC\\") + Native.mid(2);
    }
    else if ((Native.size() > 2) && (Native[1] == QLatin1Char(':')))
    {
      Native = QStringLiteral("\\\\?\\") + Native;
    }
  }
  return Native.toStdWString();
}

static void FromFileTime(const FILETIME & Time, qint64 & Seconds, qint64 & Nanoseconds)
{
  ULARGE_INTEGER Value;
  Value.LowPart = Time.dwLowDateTime;
  Value.HighPart = Time.dwHighDateTime;
  // 100 ns intervals since 1601-01-01
  qint64 Ticks = static_cast<qint64>(Value.QuadPart) - 116444736000000000LL;
  Seconds = Ticks / 10000000LL;
  Nanoseconds = (Ticks % 10000000LL) * 100;
  if (Nanoseconds < 0)
  {
    Seconds--;
    Nanoseconds += 1000000000LL;
  }
}

static FILETIME ToFileTime(qint64 Seconds, qint64 Nanoseconds)
{
  ULARGE_INTEGER Value;
  Value.QuadPart = static_cast<ULONGLONG>((Seconds * 10000000LL) + (Nanoseconds / 100) + 116444736000000000LL);
  FILETIME Result;
  Result.dwLowDateTime = Value.LowPart;
  Result.dwHighDateTime = Value.HighPart;
  return Result;
}

int LastError()
{
  return static_cast<int>(GetLastError());
}

QString ErrorText(int Error)
{
  wchar_t * Buffer = nullptr;
  DWORD Length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
    nullptr, static_cast<DWORD>(Error), 0, reinterpret_cast<wchar_t *>(&Buffer), 0, nullptr);
  QString Result = (Length > 0) ? QString::fromWCharArray(Buffer, static_cast<int>(Length)).trimmed() : QStringLiteral("Error %1").arg(Error);
  LocalFree(Buffer);
  return Result;
}

bool IsExistsError(int Error)
{
  return (Error == ERROR_FILE_EXISTS) || (Error == ERROR_ALREADY_EXISTS);
}

bool IsCrossDeviceError(int Error)
{
  return Error == ERROR_NOT_SAME_DEVICE;
}

static bool DoStat(const QString & Path, Stat & Result, bool Follow)
{
  std::wstring Name = Wide(Path);
  Result = Stat();
  DWORD Flags = FILE_FLAG_BACKUP_SEMANTICS | (Follow ? 0 : FILE_FLAG_OPEN_REPARSE_POINT);
  HANDLE File = CreateFileW(Name.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
    nullptr, OPEN_EXISTING, Flags, nullptr);
  DWORD Attributes;
  FILETIME Write, Access;
  if (File != INVALID_HANDLE_VALUE)
  {
    BY_HANDLE_FILE_INFORMATION Info;
    bool Ok = GetFileInformationByHandle(File, &Info);
    DWORD Error = GetLastError();
    CloseHandle(File);
    if (!Ok)
    {
      SetLastError(Error);
      return false;
    }
    Attributes = Info.dwFileAttributes;
    Write = Info.ftLastWriteTime;
    Access = Info.ftLastAccessTime;
    Result.Size = (static_cast<qint64>(Info.nFileSizeHigh) << 32) | Info.nFileSizeLow;
    Result.Device = Info.dwVolumeSerialNumber;
    Result.Inode = (static_cast<quint64>(Info.nFileIndexHigh) << 32) | Info.nFileIndexLow;
  }
  else
  {
    // Files that cannot be opened at all (e.g. in use exclusively): no identity
    WIN32_FILE_ATTRIBUTE_DATA Data;
    if (!GetFileAttributesExW(Name.c_str(), GetFileExInfoStandard, &Data))
    {
      return false;
    }
    Attributes = Data.dwFileAttributes;
    Write = Data.ftLastWriteTime;
    Access = Data.ftLastAccessTime;
    Result.Size = (static_cast<qint64>(Data.nFileSizeHigh) << 32) | Data.nFileSizeLow;
  }
  if (!Follow && (Attributes & FILE_ATTRIBUTE_REPARSE_POINT))
  {
    // Symbolic links and junctions are not followed (they may form cycles)
    Result.Type = Stat::Symlink;
  }
  else if (Attributes & FILE_ATTRIBUTE_DIRECTORY)
  {
    Result.Type = Stat::Directory;
    Result.Size = 0;
  }
  else
  {
    Result.Type = Stat::Regular;
  }
  Result.Mode = Result.IsDirectory() ? 0777 : ((Attributes & FILE_ATTRIBUTE_READONLY) ? 0444 : 0666);
  FromFileTime(Write, Result.ModifiedSec, Result.ModifiedNsec);
  FromFileTime(Access, Result.AccessedSec, Result.AccessedNsec);
  return true;
}

bool LStat(const QString & Path, Stat & Result)
{
  return DoStat(Path, Result, false);
}

bool StatFollow(const QString & Path, Stat & Result)
{
  return DoStat(Path, Result, true);
}

QStringList ListDirectory(const QString & Path)
{
  QStringList Result;
  std::wstring Pattern = Wide(Path + QStringLiteral("/*"));
  WIN32_FIND_DATAW Data;
  HANDLE Find = FindFirstFileExW(Pattern.c_str(), FindExInfoBasic, &Data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
  if (Find == INVALID_HANDLE_VALUE)
  {
    DWORD Error = GetLastError();
    if (Error == ERROR_FILE_NOT_FOUND)
    {
      return Result;   // empty directory (no "." even, on some file systems)
    }
    throw ExtException(QStringLiteral("Error listing directory '%1'.").arg(Path), ErrorText(static_cast<int>(Error)));
  }
  do
  {
    QString Name = QString::fromWCharArray(Data.cFileName);
    if ((Name != QStringLiteral(".")) && (Name != QStringLiteral("..")))
    {
      Result.append(Name);
    }
  }
  while (FindNextFileW(Find, &Data));
  FindClose(Find);
  return Result;
}

bool MakeDirectory(const QString & Path)
{
  return CreateDirectoryW(Wide(Path).c_str(), nullptr);
}

bool RemoveDirectory(const QString & Path)
{
  return RemoveDirectoryW(Wide(Path).c_str());
}

// A read-only file is not deleted / replaced by Windows: clear the attribute
static bool ClearReadOnly(const std::wstring & Name)
{
  DWORD Attributes = GetFileAttributesW(Name.c_str());
  return (Attributes != INVALID_FILE_ATTRIBUTES) && (Attributes & FILE_ATTRIBUTE_READONLY) &&
    SetFileAttributesW(Name.c_str(), Attributes & ~FILE_ATTRIBUTE_READONLY);
}

bool RemoveFile(const QString & Path)
{
  std::wstring Name = Wide(Path);
  if (DeleteFileW(Name.c_str()))
  {
    return true;
  }
  DWORD Error = GetLastError();
  if ((Error == ERROR_ACCESS_DENIED) && ClearReadOnly(Name) && DeleteFileW(Name.c_str()))
  {
    return true;
  }
  SetLastError(Error);
  return false;
}

bool Rename(const QString & Source, const QString & Target)
{
  std::wstring From = Wide(Source);
  std::wstring To = Wide(Target);
  if (MoveFileExW(From.c_str(), To.c_str(), MOVEFILE_REPLACE_EXISTING))
  {
    return true;
  }
  DWORD Error = GetLastError();
  if ((Error == ERROR_ACCESS_DENIED) && ClearReadOnly(To) && MoveFileExW(From.c_str(), To.c_str(), MOVEFILE_REPLACE_EXISTING))
  {
    return true;
  }
  SetLastError(Error);
  return false;
}

struct CopyContext
{
  const std::function<bool(qint64)> * Progress;
  qint64 Reported = 0;
};

static DWORD CALLBACK CopyProgress(LARGE_INTEGER, LARGE_INTEGER Transferred, LARGE_INTEGER, LARGE_INTEGER, DWORD, DWORD,
  HANDLE, HANDLE, LPVOID Data)
{
  CopyContext * Context = static_cast<CopyContext *>(Data);
  qint64 Delta = static_cast<qint64>(Transferred.QuadPart) - Context->Reported;
  Context->Reported = static_cast<qint64>(Transferred.QuadPart);
  return (*Context->Progress)(Delta) ? PROGRESS_CONTINUE : PROGRESS_CANCEL;
}

bool CopyFileData(const QString & Source, const QString & Target, qint64, bool,
  const std::function<bool(qint64)> & Progress, bool & Cancelled, TCopyStep & Step)
{
  Cancelled = false;
  std::wstring From = Wide(Source);
  std::wstring To = Wide(Target);
  // Which side fails is reported separately (Cannot open / Cannot create)
  Step = csOpenSource;
  HANDLE In = CreateFileW(From.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
    OPEN_EXISTING, 0, nullptr);
  if (In == INVALID_HANDLE_VALUE)
  {
    return false;
  }
  CloseHandle(In);
  Step = csCopy;
  CopyContext Context{&Progress};
  // Copies data, attributes and the modification time; block cloning on ReFS
  if (CopyFileExW(From.c_str(), To.c_str(), &CopyProgress, &Context, nullptr, COPY_FILE_FAIL_IF_EXISTS))
  {
    return true;
  }
  DWORD Error = GetLastError();
  if (Error == ERROR_REQUEST_ABORTED)
  {
    Cancelled = true;
  }
  else if ((Error == ERROR_FILE_EXISTS) || (Error == ERROR_ALREADY_EXISTS) || (Error == ERROR_PATH_NOT_FOUND))
  {
    Step = csCreateTarget;
  }
  SetLastError(Error);
  return false;
}

bool CreateLink(const QString & Path, const QString & Target, bool Symbolic)
{
  std::wstring Name = Wide(Path);
  if (!Symbolic)
  {
    return CreateHardLinkW(Name.c_str(), Wide(Target).c_str(), nullptr);
  }
  // Relative targets stay relative; unprivileged creation needs Developer Mode
  std::wstring To = QDir::toNativeSeparators(Target).toStdWString();
  QString Absolute = QDir(QFileInfo(Path).absolutePath()).absoluteFilePath(Target);
  DWORD Flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE | (QFileInfo(Absolute).isDir() ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0);
  if (CreateSymbolicLinkW(Name.c_str(), To.c_str(), Flags))
  {
    return true;
  }
  // Older Windows 10 rejects the unprivileged flag itself
  return CreateSymbolicLinkW(Name.c_str(), To.c_str(), Flags & ~SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE);
}

bool CopySpecial(const QString &, const Stat &, const QString &)
{
  // Symbolic links and junctions: creating them needs privileges on Windows
  SetLastError(ERROR_NOT_SUPPORTED);
  return false;
}

bool SetMode(const QString & Path, unsigned int Mode)
{
  std::wstring Name = Wide(Path);
  DWORD Attributes = GetFileAttributesW(Name.c_str());
  if (Attributes == INVALID_FILE_ATTRIBUTES)
  {
    return false;
  }
  if (Attributes & FILE_ATTRIBUTE_DIRECTORY)
  {
    return true;   // read-only means something else for folders
  }
  bool ReadOnly = ((Mode & 0222) == 0);
  DWORD NewAttributes = ReadOnly ? (Attributes | FILE_ATTRIBUTE_READONLY) : (Attributes & ~FILE_ATTRIBUTE_READONLY);
  return (NewAttributes == Attributes) || SetFileAttributesW(Name.c_str(), NewAttributes);
}

bool SetTimes(const QString & Path, const Stat & From, bool NoFollow)
{
  DWORD Flags = FILE_FLAG_BACKUP_SEMANTICS | (NoFollow ? FILE_FLAG_OPEN_REPARSE_POINT : 0);
  HANDLE File = CreateFileW(Wide(Path).c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
    nullptr, OPEN_EXISTING, Flags, nullptr);
  if (File == INVALID_HANDLE_VALUE)
  {
    return false;
  }
  FILETIME Access = ToFileTime(From.AccessedSec, From.AccessedNsec);
  FILETIME Write = ToFileTime(From.ModifiedSec, From.ModifiedNsec);
  bool Ok = SetFileTime(File, nullptr, &Access, &Write);
  DWORD Error = GetLastError();
  CloseHandle(File);
  SetLastError(Error);
  return Ok;
}

void CopyOwner(const QString &, const Stat &, bool)
{
}

bool ChangeOwner(const QString &, unsigned int, unsigned int, bool, bool)
{
  SetLastError(ERROR_NOT_SUPPORTED);
  return false;
}

unsigned int DefaultMode(bool Directory)
{
  return Directory ? 0777 : 0666;
}

bool HasUnixPermissions()
{
  return false;
}

bool ResolveUser(const QString &, unsigned int &)
{
  return false;
}

bool ResolveGroup(const QString &, unsigned int &)
{
  return false;
}

QStringList UserNames()
{
  return QStringList();
}

QStringList GroupNames()
{
  return QStringList();
}

QString OwnerName(unsigned int)
{
  return QString();
}

QString GroupName(unsigned int)
{
  return QString();
}

bool MoveToTrashFallback(const QString &, QString &)
{
  return false;
}

}

#endif
