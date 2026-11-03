// Operating system services that differ between Linux, Windows and macOS.
// Everything else in NixSCP uses Qt or these functions, so the same code
// builds for all of them.
#pragma once

#include <QString>
#include <QtGlobal>

// Time fields of struct stat: st_mtim (POSIX 2008) is st_mtimespec on macOS
#ifdef Q_OS_MACOS
#define NIXSCP_ST_MTIM st_mtimespec
#define NIXSCP_ST_ATIM st_atimespec
#else
#define NIXSCP_ST_MTIM st_mtim
#define NIXSCP_ST_ATIM st_atim
#endif

class QProcess;

namespace Platform
{
  // Last-modified (and last-access) time of a local file or directory, in
  // seconds since the epoch; Accessed < 0: the same as Modified
  bool SetFileTimes(const QString & Path, qint64 Modified, qint64 Accessed = -1, QString * Error = nullptr);

  // Name of the account running NixSCP
  QString CurrentUserName();
  // Account / group of a numeric owner (Unix); empty where not applicable (Windows)
  QString UserName(uint Uid);
  QString GroupName(uint Gid);
  bool HasUnixOwnership();
  // Identifies the user for per-user names (numeric uid on Linux, account name on Windows)
  QString UserKey();
  // Whether the current user owns the file (always true on Windows: per-user folders)
  bool OwnedByCurrentUser(uint OwnerId);

  // Sets up Process to run a command line through the system shell
  // (/bin/sh -c; Windows: cmd.exe /c, as WinSCP); then start() or startDetached()
  void SetShellCommand(QProcess & Process, const QString & Command);
  // Quotes a local file name for that shell
  QString ShellQuote(const QString & Value);

  // Text of the last error of the C library (errno) or, on Windows, of Win32
  QString LastErrorText();
}
