// Operating system services that differ between Linux, Windows and macOS.
// Everything else in NixSCP uses Qt or these functions, so the same code
// builds for all of them.
#pragma once

#include <QString>
#include <QStringList>
#include <QtGlobal>

// Time fields of struct stat: st_mtim (POSIX 2008) is st_mtimespec on macOS
#ifdef Q_OS_MACOS
#define NIXSCP_ST_MTIM st_mtimespec
#define NIXSCP_ST_ATIM st_atimespec
#else
#define NIXSCP_ST_MTIM st_mtim
#define NIXSCP_ST_ATIM st_atim
#endif

class QFileDevice;
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
  // Identifies the user for per-user names (numeric uid; the SID on Windows, as
  // account names repeat between a computer and its domain)
  QString UserKey();
  // Whether the current user owns the file (always true on Windows: per-user folders)
  bool OwnedByCurrentUser(uint OwnerId);

  // First thing in main(), before any library is used. Windows: no DLL from the
  // current folder or PATH, no Qt plugin from outside the program's folder, no
  // program from the current folder; Windows and macOS: no OpenSSL configuration,
  // as its built-in path is not a safe one there. Nothing on Linux.
  void HardenProcess();

  // Only this user (and the operating system) can reach the file or folder:
  // owner-only permissions; on Windows, where QFile::setPermissions sets the
  // read-only attribute and nothing else, an ACL of its own instead of what the
  // folder hands down. False where that is not possible (FAT, some shares).
  // An open file is changed before anything is written to it: a QSaveFile
  // through its temporary file, which replaces the ACL of the file it replaces.
  bool MakePrivate(QFileDevice & File);
  bool MakePrivate(const QString & FileName);
  bool MakeDirectoryPrivate(const QString & Path);

  // Windows: the owner of a named pipe (the handle of either end) is this user.
  // A pipe has one owner and one ACL, those of its first instance: when another
  // user created the name first, also the instances made here carry that ACL
  bool OwnsPipe(qintptr Handle);

  // Sets up Process to run a command line through the system shell
  // (/bin/sh -c; Windows: cmd.exe /c, as WinSCP); then start() or startDetached()
  void SetShellCommand(QProcess & Process, const QString & Command);
  // Sets up Process to run Program directly. Windows: a bare name is looked up
  // in PATH only, never in the current folder, and a batch file gets its
  // arguments quoted for cmd.exe, which CreateProcess hands it to as they are
  void SetProgram(QProcess & Process, const QString & Program, const QStringList & Arguments);
  // Quotes a local file name for that shell
  QString ShellQuote(const QString & Value);
  // cmd.exe, on any platform (for the tests): a value in double quotes, and the
  // part that goes between double quotes a command already has. Inside quotes
  // cmd.exe still expands %VAR%, and a " ends them: a variable cannot form, and
  // " and control characters are left out, which only a name from a server has
  QString CmdQuote(const QString & Value);
  QString CmdInsideQuotes(const QString & Value);

  // Text of the last error of the C library (errno) or, on Windows, of Win32
  QString LastErrorText();
}
