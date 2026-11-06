// core/Platform: what differs between the operating systems, and the rules of
// Windows that can be checked anywhere. cmd.exe quoting is proved against a
// model of how cmd.exe and the C runtime read a command line, on every platform;
// built for Windows the same is run through the real cmd.exe, and the ACLs and
// pipes are read back from the system.
#include "core/AgentAccess.h"
#include "core/IniStorage.h"
#include "core/Platform.h"
#include "core/Security.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryDir>

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <aclapi.h>
#include <sddl.h>
#else
#include <sys/stat.h>
#endif

static int Failures = 0;
static int Checks = 0;

#define CHECK(Cond) \
  do { Checks++; if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); std::fflush(stdout); Failures++; } } while (0)
#define CHECK_MSG(Cond, Text) \
  do { Checks++; if (!(Cond)) { std::printf("FAIL %s:%d: %s [%s]\n", __FILE__, __LINE__, #Cond, qPrintable(Text)); std::fflush(stdout); Failures++; } } while (0)
#define CHECK_EQ(A, B) \
  do { Checks++; auto VA = (A); auto VB = (B); if (!(VA == VB)) { std::printf("FAIL %s:%d: %s == %s [%s]\n", __FILE__, __LINE__, #A, #B, qPrintable(QString(VA))); \
    std::fflush(stdout); Failures++; } } while (0)

//---------------------------------------------------------------------------
// A model of cmd.exe /c

// Percent expansion comes first, also inside quotes, and what it gives is not
// looked at again. %cd:~,% is empty; "%%" keeps a "%" and goes on at the second.
// False when a variable would be expanded.
static bool ExpandPercents(const QString & Command, QString & Result)
{
  Result.clear();
  qsizetype Index = 0;
  while (Index < Command.size())
  {
    if (Command.at(Index) != QLatin1Char('%'))
    {
      Result += Command.at(Index++);
      continue;
    }
    qsizetype End = Command.indexOf(QLatin1Char('%'), Index + 1);
    if (End < 0)
    {
      Result += Command.mid(Index);
      break;
    }
    QString Name = Command.mid(Index + 1, End - Index - 1);
    if (Name == QStringLiteral("cd:~,"))
    {
      Index = End + 1;
    }
    else if (Name.isEmpty())
    {
      Result += QLatin1Char('%');
      Index++;
    }
    else
    {
      return false;
    }
  }
  return true;
}

// Then the line is parsed: quotes switch the special characters off, outside
// quotes "^" takes the next character literally and is removed, and the rest
// of the syntax is what must not appear. Result: what the program gets.
static bool CmdParse(const QString & Command, QString & Result)
{
  bool Quoted = false;
  Result.clear();
  for (qsizetype Index = 0; Index < Command.size(); Index++)
  {
    QChar C = Command.at(Index);
    if (C == QLatin1Char('"'))
    {
      Quoted = !Quoted;
      Result += C;
    }
    else if (Quoted)
    {
      Result += C;
    }
    else if (C == QLatin1Char('^'))
    {
      Index++;
      if (Index < Command.size())
      {
        Result += Command.at(Index);
      }
    }
    else if (QStringLiteral("&|<>()").contains(C) || (C.unicode() < 0x20))
    {
      return false;
    }
    else
    {
      Result += C;
    }
  }
  return !Quoted;
}

// How the C runtime reads a single argument
static QString ProgramArgument(const QString & Line)
{
  QString Result;
  bool Quoted = false;
  qsizetype Backslashes = 0;
  for (QChar C : Line)
  {
    if (C == QLatin1Char('\\'))
    {
      Backslashes++;
      continue;
    }
    if (C == QLatin1Char('"'))
    {
      Result += QString(Backslashes / 2, QLatin1Char('\\'));
      if ((Backslashes % 2) != 0)
      {
        Result += C;
      }
      else
      {
        Quoted = !Quoted;
      }
    }
    else
    {
      Result += QString(Backslashes, QLatin1Char('\\')) + C;
    }
    Backslashes = 0;
  }
  return Result + QString(Backslashes, QLatin1Char('\\'));
}

static void CheckQuoted(const QString & Value)
{
  QString Expected = Value;
  Expected.remove(QRegularExpression(QStringLiteral("[\"\\x00-\\x1f]")));
  // alone, and in the double quotes of a command
  for (const QString & Command : {Platform::CmdQuote(Value), QLatin1Char('"') + Platform::CmdInsideQuotes(Value) + QLatin1Char('"')})
  {
    QString Expanded;
    CHECK_MSG(ExpandPercents(Command, Expanded), Command);
    QString Line;
    CHECK_MSG(CmdParse(Expanded, Line), Command);
    CHECK_MSG(ProgramArgument(Line) == Expected, Command);
  }
}

static void TestCmdQuote()
{
  CHECK_EQ(Platform::CmdQuote(QStringLiteral("plain")), QStringLiteral("\"plain\""));
  CHECK_EQ(Platform::CmdQuote(QStringLiteral("a b&c|d<e>f^g(h)")), QStringLiteral("\"a b&c|d<e>f^g(h)\""));
  CHECK_EQ(Platform::CmdQuote(QStringLiteral("%PATH%")), QStringLiteral("\"%%cd:~,%PATH%%cd:~,%\""));
  CHECK_EQ(Platform::CmdQuote(QStringLiteral("a\"&calc&\"b")), QStringLiteral("\"a&calc&b\""));
  CHECK_EQ(Platform::CmdQuote(QStringLiteral("a\\\"b")), QStringLiteral("\"a\\b\""));
  CHECK_EQ(Platform::CmdQuote(QStringLiteral("C:\\dir\\")), QStringLiteral("\"C:\\dir\\\\\""));
  CHECK_EQ(Platform::CmdInsideQuotes(QStringLiteral("a\r\nb\tc")), QStringLiteral("abc"));
  // the model does find what it has to
  QString Line;
  CHECK(!ExpandPercents(QStringLiteral("\"%PATH%\""), Line));
  CHECK(!CmdParse(QStringLiteral("\"a\"&calc&\"b\""), Line));
  for (const char * Value : {"", "a b", "a&calc", "a\"&calc&\"b", "\"", "\"\"", "%", "%%", "100%", "%PATH%", "%CMDCMDLINE%",
         "%PATH:~0,1%", "a%b%c%d", "^", "^^", "^\"", "x^", "(a)", "a|b", "a<b>c", "\\", "\\\\", "\\\"", "a\\\\\"b", "C:\\x\\", "!x!", "%\"%",
         "\"%\"", "%^%", "a\nb", "a\rb"})
  {
    CheckQuoted(QString::fromUtf8(Value));
  }
  const QString Alphabet = QStringLiteral("ab \"\\%^&|<>()!\n=,;");
  for (int Round = 0; Round < 20000; Round++)
  {
    QString Value;
    int Length = QRandomGenerator::global()->bounded(12);
    for (int Index = 0; Index < Length; Index++)
    {
      Value += Alphabet.at(QRandomGenerator::global()->bounded(static_cast<int>(Alphabet.size())));
    }
    CheckQuoted(Value);
  }
}

//---------------------------------------------------------------------------
// Private files

#ifdef _WIN32
// Wine keeps the ACEs of a file, but neither the "protected" flag nor inheritance
static bool RunningOnWine()
{
  return GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
}

// Who the ACL of Path lets in, besides this user and the system (always the two)
static QString AclProblem(const QString & Path, bool Inherited = false)
{
  PACL Acl = nullptr;
  PSECURITY_DESCRIPTOR Descriptor = nullptr;
  std::wstring Name = QDir::toNativeSeparators(Path).toStdWString();
  if (GetNamedSecurityInfoW(Name.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &Acl, nullptr, &Descriptor) != ERROR_SUCCESS)
  {
    return QStringLiteral("no ACL");
  }
  QString Result;
  SECURITY_DESCRIPTOR_CONTROL Control = 0;
  DWORD Revision = 0;
  GetSecurityDescriptorControl(Descriptor, &Control, &Revision);
  if (!Inherited && ((Control & SE_DACL_PROTECTED) == 0) && !RunningOnWine())
  {
    Result += QStringLiteral("not protected; ");
  }
  QStringList Sids;
  for (DWORD Index = 0; (Acl != nullptr) && (Index < Acl->AceCount); Index++)
  {
    ACCESS_ALLOWED_ACE * Ace = nullptr;
    if (GetAce(Acl, Index, reinterpret_cast<void **>(&Ace)) && (Ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE))
    {
      wchar_t * Text = nullptr;
      if (ConvertSidToStringSidW(reinterpret_cast<PSID>(&Ace->SidStart), &Text))
      {
        Sids.append(QString::fromWCharArray(Text));
        LocalFree(Text);
      }
    }
  }
  LocalFree(Descriptor);
  Sids.removeAll(Platform::UserKey());
  Sids.removeAll(QStringLiteral("S-1-5-18"));
  return Result + Sids.join(QLatin1Char(' '));
}
#endif

static void CheckPrivate(const QString & Path, bool Directory)
{
#ifdef _WIN32
  Q_UNUSED(Directory);
  QString Problem = AclProblem(Path);
  CHECK_MSG(Problem.isEmpty(), Path + QStringLiteral(": ") + Problem);
#else
  struct stat Info = {};
  CHECK(stat(QFile::encodeName(Path).constData(), &Info) == 0);
  CHECK((Info.st_mode & 0777) == (Directory ? 0700u : 0600u));
#endif
}

static void TestPrivateFiles(const QString & Folder)
{
  QString Name = Folder + QStringLiteral("/plain.txt");
  QFile Plain(Name);
  CHECK(Plain.open(QIODevice::WriteOnly));
  CHECK(Platform::MakePrivate(Plain));
  Plain.write("x");
  Plain.close();
  CheckPrivate(Name, false);

  // by name, later
  QString Later = Folder + QStringLiteral("/later.txt");
  QFile File(Later);
  CHECK(File.open(QIODevice::WriteOnly));
  File.close();
  CHECK(Platform::MakePrivate(Later));
  CheckPrivate(Later, false);

  // through the temporary file of a QSaveFile, new and over an existing file:
  // nothing is written before, and the ACL of the old file is gone with it
  QString Saved = Folder + QStringLiteral("/saved.ini");
  for (int Round = 0; Round < 2; Round++)
  {
    QSaveFile Save(Saved);
    CHECK(Save.open(QIODevice::WriteOnly));
    CHECK(Platform::MakePrivate(Save));
    Save.write("secret");
    CHECK(Save.commit());
    CheckPrivate(Saved, false);
  }
  QFile Check(Saved);
  CHECK(Check.open(QIODevice::ReadOnly) && (Check.readAll() == "secret"));

  // the configuration, as with /ini= in a folder shared with others
  QString Ini = Folder + QStringLiteral("/nixscp.ini");
  IniFile Config;
  for (int Round = 0; Round < 2; Round++)
  {
    CHECK(Config.Save(Ini));
    CheckPrivate(Ini, false);
  }

  // a folder, and what comes into it
  QString Directory = Folder + QStringLiteral("/private");
  CHECK(QDir().mkdir(Directory));
  CHECK(Platform::MakeDirectoryPrivate(Directory));
  CheckPrivate(Directory, true);
#ifdef _WIN32
  QString Child = Directory + QStringLiteral("/child.txt");
  QFile ChildFile(Child);
  CHECK(ChildFile.open(QIODevice::WriteOnly));
  ChildFile.close();
  if (RunningOnWine())
  {
    std::printf("inherited ACL: SKIPPED (Wine has no inheritance)\n");
  }
  else
  {
    QString Problem = AclProblem(Child, true);
    CHECK_MSG(Problem.isEmpty(), Problem);
  }
  // the temporary folder of Qt, as EditorManager uses it
  QTemporaryDir Temp(Folder + QStringLiteral("/nixscp-XXXXXX"));
  CHECK(Temp.isValid() && Platform::MakeDirectoryPrivate(Temp.path()));
  CheckPrivate(Temp.path(), true);
#endif
}

//---------------------------------------------------------------------------
// Local sockets and pipes

static void TestPeers(const QString & Folder)
{
#ifdef _WIN32
  QString Name = QStringLiteral("nixscp-test-") + QString::number(GetCurrentProcessId());
#else
  QString Name = Folder + QStringLiteral("/s");
#endif
  Q_UNUSED(Folder);
  QLocalServer Server;
  Server.setSocketOptions(QLocalServer::UserAccessOption);
  CHECK(Server.listen(Name));
  QLocalSocket Client;
  Client.connectToServer(Name);
  CHECK(Client.waitForConnected(3000));
  CHECK(AgentAccess::PeerIsSameUser(Client.socketDescriptor()));
  CHECK(Server.waitForNewConnection(3000));
  QLocalSocket * Accepted = Server.nextPendingConnection();
  CHECK(Accepted != nullptr);
  if (Accepted != nullptr)
  {
    CHECK(AgentAccess::PeerIsSameUser(Accepted->socketDescriptor()));
  }
#ifdef _WIN32
  CHECK(Platform::UserKey().startsWith(QStringLiteral("S-1-")));
  // A pipe somebody else created first. The owner of the test's pipe is a group
  // here, as no other account can be used: it is not this user
  BYTE Administrators[SECURITY_MAX_SID_SIZE];
  DWORD Size = sizeof(Administrators);
  SECURITY_DESCRIPTOR Descriptor;
  if (CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, Administrators, &Size) &&
      InitializeSecurityDescriptor(&Descriptor, SECURITY_DESCRIPTOR_REVISION) &&
      SetSecurityDescriptorDacl(&Descriptor, TRUE, nullptr, FALSE) &&
      SetSecurityDescriptorOwner(&Descriptor, Administrators, FALSE))
  {
    SECURITY_ATTRIBUTES Attributes = {sizeof(Attributes), &Descriptor, FALSE};
    std::wstring Squatted = (QStringLiteral("\\\\.\\pipe\\") + Name + QStringLiteral("-squat")).toStdWString();
    HANDLE Pipe = CreateNamedPipeW(Squatted.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE, PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0, &Attributes);
    if (Pipe != INVALID_HANDLE_VALUE)
    {
      HANDLE Other = CreateFileW(Squatted.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
      CHECK(Other != INVALID_HANDLE_VALUE);
      CHECK(!AgentAccess::PeerIsSameUser(reinterpret_cast<qintptr>(Other)));
      CHECK(!AgentAccess::PeerIsSameUser(reinterpret_cast<qintptr>(Pipe)));
      CloseHandle(Other);
      CloseHandle(Pipe);
    }
    else
    {
      std::printf("squatted pipe: SKIPPED (not an administrator)\n");
    }
  }
#endif
}

//---------------------------------------------------------------------------
// Secrets

static void TestSecureBytes()
{
  Security::ProtectProcessMemory();
  SecureBytes Key(QByteArrayLiteral("0123456789abcdef"));
  CHECK(Key.Size() == 16);
  CHECK(Key.Equals(QByteArrayLiteral("0123456789abcdef")));
  CHECK(!Key.Equals(QByteArrayLiteral("0123456789abcdeX")));
  CHECK(Key.Copy() == QByteArrayLiteral("0123456789abcdef"));
  SecureBytes Other(Key);
  Key.Clear();
  CHECK(Key.IsEmpty() && (Other.Size() == 16));
  // more than a page, and many: locking is allowed to fail, the rest is not
  QList<SecureBytes> Many;
  for (int Index = 0; Index < 200; Index++)
  {
    Many.append(SecureBytes(QByteArray(9000, 'k')));
  }
  CHECK(Many.last().Equals(QByteArray(9000, 'k')));
}

//---------------------------------------------------------------------------
// The real cmd.exe

#ifdef _WIN32
static const char * const PrintArguments = "--print-arguments";

// A program that prints the arguments it got, one per line in hex
static int RunPrintArguments()
{
  const QStringList Arguments = QCoreApplication::arguments().mid(2);
  for (const QString & Argument : Arguments)
  {
    std::printf("%s\n", Argument.toUtf8().toHex().constData());
  }
  return 0;
}

static QStringList Printed(QProcess & Process)
{
  Process.start();
  CHECK(Process.waitForFinished(30000));
  QStringList Result;
  const QList<QByteArray> Lines = Process.readAllStandardOutput().split('\n');
  for (const QByteArray & Line : Lines)
  {
    if (!Line.trimmed().isEmpty())
    {
      Result.append(QString::fromUtf8(QByteArray::fromHex(Line.trimmed())));
    }
  }
  return Result;
}

static void TestRealCmd(const QString & Folder)
{
  QString Self = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
  QString Marker = Folder + QStringLiteral("/marker.txt");
  QString NativeMarker = QDir::toNativeSeparators(Marker);
  qputenv("NIXSCP_TEST_VARIABLE", "expanded");
  const QStringList Names = {
    QStringLiteral("a b"), QStringLiteral("a&echo x>") + NativeMarker, QStringLiteral("a\"&echo x>") + NativeMarker + QStringLiteral("&\"b"),
    QStringLiteral("%NIXSCP_TEST_VARIABLE%"), QStringLiteral("x%NIXSCP_TEST_VARIABLE%y%z"), QStringLiteral("^a^"), QStringLiteral("(a)"),
    QStringLiteral("a|b"), QStringLiteral("a<b>c"), QStringLiteral("C:\\dir\\"), QStringLiteral("!PATH!"), QStringLiteral("100%")};
  // the shell, as custom commands use it
  for (const QString & Name : Names)
  {
    QProcess Process;
    Platform::SetShellCommand(Process, Platform::CmdQuote(Self) + QLatin1Char(' ') + QString::fromLatin1(PrintArguments) + QLatin1Char(' ') + Platform::CmdQuote(Name));
    QStringList Result = Printed(Process);
    QString Expected = Name;
    Expected.remove(QLatin1Char('"'));
    CHECK_MSG((Result.size() == 1) && (Result.value(0) == Expected), Name + QStringLiteral(" -> ") + Result.join(QStringLiteral(" | ")));
    CHECK_MSG(!QFile::exists(Marker), Name);
    QFile::remove(Marker);
  }
  // a batch file as an external editor: its %* goes through cmd.exe again
  QString Batch = Folder + QStringLiteral("/editor.cmd");
  QFile BatchFile(Batch);
  CHECK(BatchFile.open(QIODevice::WriteOnly));
  BatchFile.write(QStringLiteral("@\"%1\" %2 %*\r\n").arg(Self, QString::fromLatin1(PrintArguments)).toLocal8Bit());
  BatchFile.close();
  for (const QString & Name : {QStringLiteral("a&echo x>") + NativeMarker, QStringLiteral("a b.txt"), QStringLiteral("x%NIXSCP_TEST_VARIABLE%y")})
  {
    QProcess Process;
    Platform::SetProgram(Process, QDir::toNativeSeparators(Batch), {Name});
    QStringList Result = Printed(Process);
    CHECK_MSG((Result.size() == 1) && (Result.value(0) == Name), Name + QStringLiteral(" -> ") + Result.join(QStringLiteral(" | ")));
    CHECK_MSG(!QFile::exists(Marker), Name);
    QFile::remove(Marker);
  }
  // a program of the same name in the current folder is not what runs
  QString Current = QDir::currentPath();
  QDir::setCurrent(Folder);
  QFile::copy(QCoreApplication::applicationFilePath(), Folder + QStringLiteral("/nixscp-planted.exe"));
  QProcess Planted;
  Platform::SetProgram(Planted, QStringLiteral("nixscp-planted"), {QString::fromLatin1(PrintArguments), QStringLiteral("x")});
  CHECK(!Planted.program().contains(QLatin1Char('\\')));
  QDir::setCurrent(Current);
}
#endif

int main(int argc, char ** argv)
{
  Platform::HardenProcess();
  QCoreApplication App(argc, argv);
#ifdef _WIN32
  if ((argc > 1) && (std::strcmp(argv[1], PrintArguments) == 0))
  {
    return RunPrintArguments();
  }
#endif
  QTemporaryDir Folder;
  CHECK(Folder.isValid());
  TestCmdQuote();
  TestPrivateFiles(Folder.path());
  TestPeers(Folder.path());
  TestSecureBytes();
#ifdef _WIN32
  TestRealCmd(Folder.path());
#endif
  std::printf("%d checks, %d failures\n", Checks, Failures);
  return (Failures == 0) ? 0 : 1;
}
