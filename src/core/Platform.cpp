#include "core/Platform.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

#include <openssl/crypto.h>

#ifdef Q_OS_WIN
#include <aclapi.h>
#include <io.h>
#include <lmcons.h>
#include <sddl.h>
#include <string>
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

// The SID of the account running NixSCP, in a buffer of its own
static QByteArray CurrentUserSid()
{
  QByteArray Result;
  HANDLE Token = nullptr;
  if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Token))
  {
    DWORD Size = 0;
    GetTokenInformation(Token, TokenUser, nullptr, 0, &Size);
    QByteArray Buffer(static_cast<qsizetype>(Size), Qt::Uninitialized);
    if ((Size > 0) && GetTokenInformation(Token, TokenUser, Buffer.data(), Size, &Size))
    {
      PSID Sid = reinterpret_cast<TOKEN_USER *>(Buffer.data())->User.Sid;
      Result = QByteArray(static_cast<const char *>(Sid), static_cast<qsizetype>(GetLengthSid(Sid)));
    }
    CloseHandle(Token);
  }
  return Result;
}

QString UserKey()
{
  static const QString Result = []()
  {
    QByteArray Sid = CurrentUserSid();
    wchar_t * Text = nullptr;
    if (Sid.isEmpty() || !ConvertSidToStringSidW(Sid.data(), &Text))
    {
      return CurrentUserName();
    }
    QString Key = QString::fromWCharArray(Text);
    LocalFree(Text);
    return Key;
  }();
  return Result;
}

bool OwnedByCurrentUser(uint)
{
  return true;
}

bool OwnsPipe(qintptr Handle)
{
  QByteArray User = CurrentUserSid();
  PSID Owner = nullptr;
  PSECURITY_DESCRIPTOR Descriptor = nullptr;
  if (User.isEmpty() || (GetSecurityInfo(reinterpret_cast<HANDLE>(Handle), SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION,
        &Owner, nullptr, nullptr, nullptr, &Descriptor) != ERROR_SUCCESS))
  {
    return false;
  }
  bool Result = (Owner != nullptr) && EqualSid(Owner, User.data());
  LocalFree(Descriptor);
  return Result;
}

// Replaces the ACL of a file or folder: this user and the system, and nothing
// handed down from the folder above. Inherited by what a folder comes to hold.
static bool SetPrivateAcl(const std::wstring & Path, bool Directory)
{
  QByteArray User = CurrentUserSid();
  BYTE System[SECURITY_MAX_SID_SIZE];
  DWORD SystemSize = sizeof(System);
  if (User.isEmpty() || !CreateWellKnownSid(WinLocalSystemSid, nullptr, System, &SystemSize))
  {
    return false;
  }
  EXPLICIT_ACCESSW Access[2] = {};
  PSID Sids[2] = {User.data(), System};
  for (int Index = 0; Index < 2; Index++)
  {
    Access[Index].grfAccessPermissions = FILE_ALL_ACCESS;
    Access[Index].grfAccessMode = SET_ACCESS;
    Access[Index].grfInheritance = Directory ? SUB_CONTAINERS_AND_OBJECTS_INHERIT : NO_INHERITANCE;
    Access[Index].Trustee.TrusteeForm = TRUSTEE_IS_SID;
    Access[Index].Trustee.TrusteeType = TRUSTEE_IS_USER;
    Access[Index].Trustee.ptstrName = static_cast<LPWSTR>(Sids[Index]);
  }
  PACL Acl = nullptr;
  if (SetEntriesInAclW(2, Access, nullptr, &Acl) != ERROR_SUCCESS)
  {
    return false;
  }
  DWORD Error = SetNamedSecurityInfoW(const_cast<LPWSTR>(Path.c_str()), SE_FILE_OBJECT,
    DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, Acl, nullptr);
  LocalFree(Acl);
  return Error == ERROR_SUCCESS;
}

static std::wstring NativePath(const QString & Path)
{
  return QDir::toNativeSeparators(QFileInfo(Path).absoluteFilePath()).toStdWString();
}

// What is open: for a QSaveFile that is the temporary file, not fileName()
static std::wstring OpenFilePath(QFileDevice & File)
{
  int Descriptor = File.handle();
  HANDLE Handle = (Descriptor >= 0) ? reinterpret_cast<HANDLE>(_get_osfhandle(Descriptor)) : INVALID_HANDLE_VALUE;
  if (Handle != INVALID_HANDLE_VALUE)
  {
    std::wstring Buffer(32768, L'\0');
    DWORD Length = GetFinalPathNameByHandleW(Handle, Buffer.data(), static_cast<DWORD>(Buffer.size()),
      FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if ((Length > 0) && (Length < Buffer.size()))
    {
      Buffer.resize(Length);
      return Buffer;
    }
  }
  return NativePath(File.fileName());
}

bool MakePrivate(QFileDevice & File)
{
  return SetPrivateAcl(OpenFilePath(File), false);
}

bool MakePrivate(const QString & FileName)
{
  return SetPrivateAcl(NativePath(FileName), false);
}

bool MakeDirectoryPrivate(const QString & Path)
{
  return SetPrivateAcl(NativePath(Path), true);
}

// The folder of this program. (QCoreApplication has none yet: this runs before it)
static QString ProgramFolder()
{
  std::wstring Buffer(32768, L'\0');
  DWORD Length = GetModuleFileNameW(nullptr, Buffer.data(), static_cast<DWORD>(Buffer.size()));
  if ((Length == 0) || (Length >= Buffer.size()))
  {
    return QString();
  }
  QString Path = QDir::fromNativeSeparators(QString::fromWCharArray(Buffer.c_str(), static_cast<qsizetype>(Length)));
  return Path.left(Path.lastIndexOf(QLatin1Char('/')));
}

// The system folder, where cmd.exe is
static QString SystemFolder()
{
  wchar_t Buffer[MAX_PATH];
  UINT Length = GetSystemDirectoryW(Buffer, MAX_PATH);
  return ((Length > 0) && (Length < MAX_PATH)) ? QDir::fromNativeSeparators(QString::fromWCharArray(Buffer, static_cast<qsizetype>(Length))) : QString();
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

bool OwnsPipe(qintptr)
{
  return false;
}

bool MakePrivate(QFileDevice & File)
{
  return File.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}

bool MakePrivate(const QString & FileName)
{
  return QFile::setPermissions(FileName, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}

bool MakeDirectoryPrivate(const QString & Path)
{
  return QFile::setPermissions(Path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
}

QString LastErrorText()
{
  return QString::fromLocal8Bit(strerror(errno));
}

#endif

void HardenProcess()
{
#ifdef Q_OS_WIN
  // A LoadLibrary without a path: the program's folder and System32, not the
  // current folder (the folder of a file opened in Explorer, a share) nor PATH
  SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS);
  // Programs started without a path. Inherited, also for what a custom command runs
  qputenv("NoDefaultCurrentDirectoryInExePath", "1");
  // No DLL over the network, unless the program itself runs from there
  // (an option on the command line could otherwise name a share), and System32
  // first for a name that is also next to the program
  QString Program = ProgramFolder();
  PROCESS_MITIGATION_IMAGE_LOAD_POLICY Policy = {};
  Policy.NoRemoteImages = !Program.startsWith(QStringLiteral("//")) &&
    (GetDriveTypeW(reinterpret_cast<const wchar_t *>((Program.left(2) + QLatin1Char('\\')).utf16())) != DRIVE_REMOTE);
  Policy.NoLowMandatoryLabelImages = 1;
  Policy.PreferSystem32Images = 1;
  SetProcessMitigationPolicy(ProcessImageLoadPolicy, &Policy, sizeof(Policy));
  // Plugins only from the program's folder: not from the folder Qt was built
  // in or next to it ("C:\plugins" for a program in "C:\NixSCP"), which any user
  // of the computer may be able to create
  if (!Program.isEmpty())
  {
    qunsetenv("QT_PLUGIN_PATH");
    qunsetenv("QT_QPA_PLATFORM_PLUGIN_PATH");
    QCoreApplication::setLibraryPaths({Program});
  }
#elif defined(Q_OS_MACOS)
  qunsetenv("QT_PLUGIN_PATH");
  qunsetenv("QT_QPA_PLATFORM_PLUGIN_PATH");
#endif
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
  // OpenSSL's built-in folder is where it was built, on Windows a path on the
  // current drive ("\nixscp-deps\ssl"): whoever can create that folder would
  // decide what the configuration loads into this process. The choice is the
  // first call's. Nothing here needs a configuration file
  OPENSSL_init_crypto(OPENSSL_INIT_NO_LOAD_CONFIG, nullptr);
#endif
}

void SetShellCommand(QProcess & Process, const QString & Command)
{
#ifdef Q_OS_WIN
  // cmd.exe parses its own command line: no quoting by QProcess. /s: only the
  // outer quotes are removed; /d: no AutoRun; /v:off: "!" stays a plain
  // character; /e:on: CmdInsideQuotes depends on it
  QString Shell = qEnvironmentVariable("ComSpec");
  if (!QDir::isAbsolutePath(Shell))
  {
    Shell = SystemFolder() + QStringLiteral("/cmd.exe");
  }
  Process.setProgram(QDir::toNativeSeparators(Shell));
  Process.setArguments({});
  Process.setNativeArguments(QStringLiteral("/d /s /e:on /v:off /c \"%1\"").arg(Command));
#else
  Process.setProgram(QStringLiteral("/bin/sh"));
  Process.setArguments({QStringLiteral("-c"), Command});
#endif
}

#ifdef Q_OS_WIN
// A bare name, as CreateProcess would find it but for the current folder
static QString SafeProgramPath(const QString & Program)
{
  if (Program.contains(QLatin1Char('/')) || Program.contains(QLatin1Char('\\')))
  {
    return Program;
  }
  QStringList Folders;
  const QStringList Path = qEnvironmentVariable("PATH").split(QDir::listSeparator(), Qt::SkipEmptyParts);
  for (const QString & Folder : Path)
  {
    if (QDir::isAbsolutePath(Folder))
    {
      Folders.append(Folder);
    }
  }
  QString Result = QStandardPaths::findExecutable(Program, Folders);
  return Result.isEmpty() ? Program : QDir::toNativeSeparators(Result);
}

static bool IsBatchFile(const QString & Program)
{
  // CreateProcess drops dots and spaces at the end as any other file name
  QString Name = Program;
  while (Name.endsWith(QLatin1Char('.')) || Name.endsWith(QLatin1Char(' ')))
  {
    Name.chop(1);
  }
  return Name.endsWith(QStringLiteral(".bat"), Qt::CaseInsensitive) || Name.endsWith(QStringLiteral(".cmd"), Qt::CaseInsensitive);
}
#endif

void SetProgram(QProcess & Process, const QString & Program, const QStringList & Arguments)
{
#ifdef Q_OS_WIN
  QString Path = SafeProgramPath(Program);
  if (IsBatchFile(Path))
  {
    QString Command = CmdQuote(Path);
    for (const QString & Argument : Arguments)
    {
      Command += QLatin1Char(' ') + CmdQuote(Argument);
    }
    SetShellCommand(Process, Command);
    return;
  }
  Process.setProgram(Path);
#else
  Process.setProgram(Program);
#endif
  Process.setArguments(Arguments);
}

QString CmdInsideQuotes(const QString & Value)
{
  QString Result;
  Result.reserve(Value.size());
  qsizetype Backslashes = 0;
  for (QChar C : Value)
  {
    if ((C == QLatin1Char('"')) || (C.unicode() < 0x20))
    {
      // A quote would end the quotes and a line end the command, and no form of
      // them holds in cmd.exe and in the program alike. No Windows file name has them
      continue;
    }
    if (C == QLatin1Char('%'))
    {
      // %cd:~,% is an empty part of a variable that always exists: the first "%"
      // stays, and what follows cannot complete the name of a variable.
      // Variables are expanded once, so nothing else in the name is
      Result += QStringLiteral("%%cd:~,%");
    }
    else
    {
      Result += C;
    }
    Backslashes = (C == QLatin1Char('\\')) ? (Backslashes + 1) : 0;
  }
  // as the closing quote follows
  return Result + QString(Backslashes, QLatin1Char('\\'));
}

QString CmdQuote(const QString & Value)
{
  return QLatin1Char('"') + CmdInsideQuotes(Value) + QLatin1Char('"');
}

QString ShellQuote(const QString & Value)
{
#ifdef Q_OS_WIN
  return CmdQuote(Value);
#else
  QString Result = Value;
  Result.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
  return QLatin1Char('\'') + Result + QLatin1Char('\'');
#endif
}

}
