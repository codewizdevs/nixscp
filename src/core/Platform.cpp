#include "core/Platform.h"

#include <QFile>
#include <QProcess>

#ifdef Q_OS_WIN
#include <lmcons.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Platform
{

#ifdef Q_OS_WIN

static FILETIME ToFileTime(qint64 Seconds)
{
  // FILETIME: 100 ns intervals since 1601-01-01
  ULARGE_INTEGER Value;
  Value.QuadPart = static_cast<ULONGLONG>(Seconds + 11644473600LL) * 10000000ULL;
  FILETIME Result;
  Result.dwLowDateTime = Value.LowPart;
  Result.dwHighDateTime = Value.HighPart;
  return Result;
}

static QString Win32ErrorText(DWORD Error)
{
  wchar_t * Buffer = nullptr;
  DWORD Length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
    nullptr, Error, 0, reinterpret_cast<wchar_t *>(&Buffer), 0, nullptr);
  QString Result = (Length > 0) ? QString::fromWCharArray(Buffer, static_cast<int>(Length)).trimmed() : QStringLiteral("Error %1").arg(Error);
  LocalFree(Buffer);
  return Result;
}

bool SetFileTimes(const QString & Path, qint64 Modified, qint64 Accessed, QString * Error)
{
  // FILE_FLAG_BACKUP_SEMANTICS: directories can be opened too
  HANDLE File = CreateFileW(reinterpret_cast<const wchar_t *>(Path.utf16()), FILE_WRITE_ATTRIBUTES,
    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (File == INVALID_HANDLE_VALUE)
  {
    if (Error != nullptr) *Error = Win32ErrorText(GetLastError());
    return false;
  }
  FILETIME Write = ToFileTime(Modified);
  FILETIME Access = ToFileTime((Accessed < 0) ? Modified : Accessed);
  bool Ok = SetFileTime(File, nullptr, &Access, &Write);
  if (!Ok && (Error != nullptr)) *Error = Win32ErrorText(GetLastError());
  CloseHandle(File);
  return Ok;
}

QString CurrentUserName()
{
  wchar_t Name[UNLEN + 1];
  DWORD Size = UNLEN + 1;
  if (GetUserNameW(Name, &Size) && (Size > 1))
  {
    return QString::fromWCharArray(Name, static_cast<int>(Size - 1));
  }
  return qEnvironmentVariable("USERNAME");
}

QString UserName(uint)
{
  return QString();
}

QString GroupName(uint)
{
  return QString();
}

bool HasUnixOwnership()
{
  return false;
}

QString UserKey()
{
  return CurrentUserName();
}

bool OwnedByCurrentUser(uint)
{
  return true;
}

QString LastErrorText()
{
  return Win32ErrorText(GetLastError());
}

#else

bool SetFileTimes(const QString & Path, qint64 Modified, qint64 Accessed, QString * Error)
{
  struct timespec Times[2];
  Times[0].tv_sec = static_cast<time_t>((Accessed < 0) ? Modified : Accessed);
  Times[1].tv_sec = static_cast<time_t>(Modified);
  Times[0].tv_nsec = Times[1].tv_nsec = 0;
  if (utimensat(AT_FDCWD, QFile::encodeName(Path).constData(), Times, 0) != 0)
  {
    if (Error != nullptr) *Error = LastErrorText();
    return false;
  }
  return true;
}

QString CurrentUserName()
{
  struct passwd * Pw = getpwuid(getuid());
  if (Pw != nullptr)
  {
    return QString::fromLocal8Bit(Pw->pw_name);
  }
  return qEnvironmentVariable("USER");
}

QString UserName(uint Uid)
{
  struct passwd * Entry = getpwuid(static_cast<uid_t>(Uid));
  return (Entry != nullptr) ? QString::fromLocal8Bit(Entry->pw_name) : QString::number(Uid);
}

QString GroupName(uint Gid)
{
  struct group * Entry = getgrgid(static_cast<gid_t>(Gid));
  return (Entry != nullptr) ? QString::fromLocal8Bit(Entry->gr_name) : QString::number(Gid);
}

bool HasUnixOwnership()
{
  return true;
}

QString UserKey()
{
  return QString::number(getuid());
}

bool OwnedByCurrentUser(uint OwnerId)
{
  return OwnerId == static_cast<uint>(getuid());
}

QString LastErrorText()
{
  return QString::fromLocal8Bit(strerror(errno));
}

#endif

void SetShellCommand(QProcess & Process, const QString & Command)
{
#ifdef Q_OS_WIN
  // cmd.exe parses its own command line: no quoting by QProcess. /s: only the
  // outer quotes are removed; /d: no AutoRun
  QString Shell = qEnvironmentVariable("ComSpec");
  Process.setProgram(Shell.isEmpty() ? QStringLiteral("cmd.exe") : Shell);
  Process.setArguments({});
  Process.setNativeArguments(QStringLiteral("/d /s /c \"%1\"").arg(Command));
#else
  Process.setProgram(QStringLiteral("/bin/sh"));
  Process.setArguments({QStringLiteral("-c"), Command});
#endif
}

QString ShellQuote(const QString & Value)
{
#ifdef Q_OS_WIN
  // AddQuotes; '"' cannot be part of a Windows file name
  return QLatin1Char('"') + Value + QLatin1Char('"');
#else
  QString Result = Value;
  Result.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
  return QLatin1Char('\'') + Result + QLatin1Char('\'');
#endif
}

}
