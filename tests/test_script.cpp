// Tests of the console scripting client (src/cli, winscp.com equivalent).
//
// Part 1 (always): command line/script tokenizer, TOptions port, session URL
// parsing and host key fingerprint matching; the cli sources are compiled in.
// Part 2: runs the nixscp-cli binary (env NIXSCP_CLI, default: next to this
// test binary); skipped when the binary is not found.
// Part 3: commands against a real SSH server; skipped unless
// NIXSCP_TEST_SSH_PORT and NIXSCP_TEST_SSH_KEY are set (see test_sftp.cpp;
// optional NIXSCP_TEST_SSH_HOST, NIXSCP_TEST_SSH_USER, NIXSCP_TEST_SSH_PASSKEY,
// NIXSCP_TEST_SSH_PASSPHRASE). The server must run on this machine, as
// remote paths are verified on the local file system.
// The user's ~/.ssh and ~/.config/nixscp are never touched: the client runs
// with XDG_CONFIG_HOME and NIXSCP_KNOWN_HOSTS pointing to a scratch directory.
#include "cli/Options.cpp"
#include "cli/SessionUrl.cpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>

#include "core/Platform.h"

#include <sys/stat.h>

#ifndef _WIN32
#include <signal.h>
#endif
#include <cstdio>

static int Failures = 0;

#define CHECK(Cond) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); std::fflush(stdout); Failures++; } } while (0)
#define CHECK_MSG(Cond, Msg) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n----\n%s\n----\n", __FILE__, __LINE__, #Cond, qPrintable(Msg)); std::fflush(stdout); Failures++; } } while (0)
static QString ToStr(const QString & S) { return S; }
static QString ToStr(const QStringList & L) { return L.join(QStringLiteral("|")); }
static QString ToStr(const QByteArray & B) { return QString::fromLatin1(B); }
template <typename T> static QString ToStr(const T & V) { return QString::number(static_cast<long long>(V)); }
#define CHECK_EQ(A, B) \
  do { auto VA = (A); auto VB = (B); if (!(VA == VB)) { std::printf("FAIL %s:%d: %s == %s (\"%s\" vs \"%s\")\n", __FILE__, __LINE__, #A, #B, \
    qPrintable(ToStr(VA)), qPrintable(ToStr(VB))); std::fflush(stdout); Failures++; } } while (0)

static void Section(const char * Name)
{
  std::printf("== %s\n", Name);
  std::fflush(stdout);
}

// ------------------------------------------------------------------ part 1

static QStringList Tokens(const QString & Line)
{
  QStringList Result;
  QString Rest = Line;
  QString Token;
  while (CutToken(Rest, Token))
  {
    Result.append(Token);
  }
  return Result;
}

static void TestTokenizer()
{
  Section("tokenizer");
  CHECK_EQ(Tokens(QStringLiteral("  put  a.txt   /tmp/  ")), (QStringList{"put", "a.txt", "/tmp/"}));
  CHECK_EQ(Tokens(QStringLiteral("get \"my file.txt\" \"C:\\dir with space\\\"")), (QStringList{"get", "my file.txt", "C:\\dir with space\\"}));
  CHECK_EQ(Tokens(QStringLiteral("echo \"say \"\"hi\"\"\"")), (QStringList{"echo", "say \"hi\""}));
  CHECK_EQ(Tokens(QStringLiteral("a\tb")), (QStringList{"a", "b"}));
  CHECK_EQ(Tokens(QStringLiteral("x\"\"y")), (QStringList{"x\"y"}));
  CHECK(Tokens(QStringLiteral("   ")).isEmpty());
  CHECK_EQ(QuoteParam(QStringLiteral("a b")), QStringLiteral("\"a b\""));
  CHECK_EQ(QuoteParam(QStringLiteral("ab")), QStringLiteral("ab"));
}

static void TestOptions()
{
  Section("options");
  {
    // Program command line
    Options O;
    for (const char * Arg : {"sftp://u@h/", "/command", "open x", "ls", "/log=a.log", "-ini:nul", "/script=s.txt", "/home/martin", "-?"})
    {
      O.Add(QString::fromLatin1(Arg));
    }
    QStringList Commands;
    CHECK(O.FindSwitch(QStringLiteral("COMMAND"), Commands));
    CHECK_EQ(Commands, (QStringList{"open x", "ls"}));
    QString Value;
    CHECK(O.FindSwitch(QStringLiteral("log"), Value) && (Value == QStringLiteral("a.log")));
    CHECK(O.FindSwitch(QStringLiteral("ini"), Value) && (Value == QStringLiteral("nul")));
    CHECK(O.FindSwitch(QStringLiteral("script"), Value) && (Value == QStringLiteral("s.txt")));
    CHECK(O.FindSwitch(QStringLiteral("?")));
    // "/home/martin" is a parameter, not a switch; /command parameters were consumed
    CHECK_EQ(O.ParamCount(), 2);
    CHECK_EQ(O.Param(1), QStringLiteral("sftp://u@h/"));
    CHECK_EQ(O.Param(2), QStringLiteral("/home/martin"));
    QString Unused;
    CHECK(!O.UnusedSwitch(Unused));
  }
  {
    // "//" ends switches (/parameter // -a /b)
    Options O;
    for (const char * Arg : {"/parameter", "//", "-a", "/b"})
    {
      O.Add(QString::fromLatin1(Arg));
    }
    QStringList Parameters;
    CHECK(O.FindSwitch(QStringLiteral("parameter"), Parameters));
    CHECK_EQ(Parameters, (QStringList{"-a", "/b"}));
  }
  {
    // Script command parameters: only '-' marks switches
    Options O;
    O.SetSwitchMarks(QStringLiteral("-"));
    O.Parse(QStringLiteral("-delete -speed=10 /remote/dir/ -transfer:ascii \"-not switch\" local -rawsettings A=1 B=2"));
    CHECK(O.FindSwitch(QStringLiteral("delete")));
    QString Value;
    bool ValueSet = false;
    CHECK(O.FindSwitch(QStringLiteral("speed"), Value, ValueSet) && ValueSet && (Value == QStringLiteral("10")));
    CHECK(O.FindSwitch(QStringLiteral("transfer"), Value) && (Value == QStringLiteral("ascii")));
    QStringList Raw;
    CHECK(O.FindSwitch(QStringLiteral("rawsettings"), Raw));
    CHECK_EQ(Raw, (QStringList{"A=1", "B=2"}));
    CHECK_EQ(O.ParamCount(), 3);
    CHECK_EQ(O.Param(1), QStringLiteral("/remote/dir/"));
    CHECK_EQ(O.Param(2), QStringLiteral("-not switch"));
    CHECK_EQ(O.Param(3), QStringLiteral("local"));
    CHECK(O.SwitchValue(QStringLiteral("delete"), true));
    O.Add(QStringLiteral("-unknown"));
    QString Unused;
    CHECK(O.UnusedSwitch(Unused) && (Unused == QStringLiteral("unknown")));
  }
  {
    Options O;
    O.Parse(QStringLiteral("-passive=off -implicit=bad"));
    CHECK(!O.SwitchValue(QStringLiteral("passive"), true));
    bool Thrown = false;
    try
    {
      O.SwitchValue(QStringLiteral("implicit"), true);
    }
    catch (const ExtException & E)
    {
      Thrown = E.Message().contains(QStringLiteral("Valid values are 'on' and 'off'"));
    }
    CHECK(Thrown);
  }
}

static void TestUrls()
{
  Section("session urls");
  StoredSessionList Stored;
  {
    SessionData D;
    SessionExtras X;
    int Info = 0;
    QString Masked;
    ParseSessionUrl(QStringLiteral("sftp://martin:pa%40ss;fingerprint=ssh-ed25519-255-abc@example.com:2222/home/martin/"),
      nullptr, &Stored, D, X, Info, &Masked);
    CHECK(Info & piProtocolDefined);
    CHECK(!(Info & piDefaultsOnly));
    CHECK_EQ(D.FSProtocol, fsSFTPonly);
    CHECK_EQ(D.HostName, QStringLiteral("example.com"));
    CHECK_EQ(D.PortNumber, 2222);
    CHECK_EQ(D.UserName, QStringLiteral("martin"));
    CHECK_EQ(D.Password, QStringLiteral("pa@ss"));
    CHECK_EQ(D.RemoteDirectory, QStringLiteral("/home/martin/"));
    CHECK_EQ(D.HostKey, QStringLiteral("ssh-ed25519-255-abc"));
    CHECK_EQ(Masked, QStringLiteral("sftp://martin:***@example.com:2222/home/martin/"));
    CHECK_EQ(D.SessionName(), QStringLiteral("martin@example.com"));
  }
  struct { const char * Url; TFSProtocol Protocol; TFtps Ftps; int Port; } Protocols[] =
  {
    { "scp://h/", fsSCPonly, ftpsNone, 22 },
    { "sftp://h", fsSFTPonly, ftpsNone, 22 },
    { "ftp://h/", fsFTP, ftpsNone, 21 },
    { "ftps://h/", fsFTP, ftpsImplicit, 990 },
    { "ftpes://h:2121/", fsFTP, ftpsExplicitTls, 2121 },
    { "davs://h/", fsWebDAV, ftpsImplicit, 443 },
  };
  for (const auto & P : Protocols)
  {
    SessionData D;
    SessionExtras X;
    int Info = 0;
    ParseSessionUrl(QString::fromLatin1(P.Url), nullptr, &Stored, D, X, Info);
    CHECK_EQ(D.FSProtocol, P.Protocol);
    CHECK_EQ(D.Ftps, P.Ftps);
    CHECK_EQ(D.PortNumber, P.Port);
    CHECK_EQ(D.HostName, QStringLiteral("h"));
  }
  {
    // no protocol: host only, user@host, IPv6
    SessionData D;
    SessionExtras X;
    int Info = 0;
    ParseSessionUrl(QStringLiteral("user@[::1]:23"), nullptr, &Stored, D, X, Info);
    CHECK(!(Info & piProtocolDefined));
    CHECK_EQ(D.FSProtocol, fsSFTP);
    CHECK_EQ(D.HostName, QStringLiteral("::1"));
    CHECK_EQ(D.PortNumber, 23);
    CHECK_EQ(D.UserName, QStringLiteral("user"));
    ParseSessionUrl(QString(), nullptr, &Stored, D, X, Info);
    CHECK(Info & piDefaultsOnly);
  }
  {
    // stored site, with folder and path
    auto Site = std::make_shared<SessionData>();
    Site->Name = QStringLiteral("Work/My Server");
    Site->HostName = QStringLiteral("server.example");
    Site->UserName = QStringLiteral("admin");
    Site->PortNumber = 2200;
    Stored.Add(Site);
    SessionData D;
    SessionExtras X;
    int Info = 0;
    ParseSessionUrl(QStringLiteral("work/my%20server/var/www"), nullptr, &Stored, D, X, Info);
    CHECK_EQ(D.HostName, QStringLiteral("server.example"));
    CHECK_EQ(D.UserName, QStringLiteral("admin"));
    CHECK_EQ(D.PortNumber, 2200);
    CHECK_EQ(D.Name, QStringLiteral("Work/My Server"));
    CHECK_EQ(D.RemoteDirectory, QStringLiteral("/var/www"));
  }
  {
    // switches
    Options O;
    O.SetSwitchMarks(QStringLiteral("-"));
    O.Parse(QStringLiteral("sftp://h/ -hostkey=\"ssh-rsa 2048 xx:yy\" -privatekey=/k.ppk -passphrase=secret -timeout=30 "
      "-username=bob -password=pw -implicit -rawsettings PortNumber=4022 Compression=1 KnownHostsFile=/tmp/kh"));
    SessionData D;
    SessionExtras X;
    int Info = 0;
    ParseSessionUrl(O.Param(1), &O, &Stored, D, X, Info);
    CHECK_EQ(D.HostKey, QStringLiteral("ssh-rsa 2048 xx:yy"));
    CHECK_EQ(X.Passphrase, QStringLiteral("secret"));
    CHECK_EQ(D.PublicKeyFile, QStringLiteral("/k.ppk"));
    CHECK_EQ(D.Timeout, 30);
    CHECK_EQ(D.UserName, QStringLiteral("bob"));
    CHECK_EQ(D.Password, QStringLiteral("pw"));
    CHECK_EQ(D.Ftps, ftpsImplicit);
    CHECK_EQ(D.PortNumber, 4022);
    CHECK(D.Compression);
    CHECK_EQ(D.KnownHostsFile, QStringLiteral("/tmp/kh"));
    QString Unused;
    CHECK(!O.UnusedSwitch(Unused));
  }
  CHECK_EQ(DecodeUrlChars(QStringLiteral("a+b%C3%A9%2F")), QString::fromUtf8("a b\xC3\xA9/"));
}

static void TestFingerprints()
{
  Section("fingerprints");
  QString SHA = QStringLiteral("SHA256:BmBEshZP/98IzuhyeJScnasKJpGTQS8VxhfaqpVwPm8");
  QString MD5 = QStringLiteral("MD5:12:34:56:78:9a:bc:de:f0:12:34:56:78:9a:bc:de:f0");
  CHECK(VerifyFingerprint(QStringLiteral("ssh-ed25519 255 BmBEshZP/98IzuhyeJScnasKJpGTQS8VxhfaqpVwPm8"), SHA, MD5));
  CHECK(VerifyFingerprint(QStringLiteral("ssh-ed25519 255 BmBEshZP/98IzuhyeJScnasKJpGTQS8VxhfaqpVwPm8="), SHA, MD5));
  CHECK(VerifyFingerprint(QStringLiteral("SHA256:BmBEshZP/98IzuhyeJScnasKJpGTQS8VxhfaqpVwPm8"), SHA, MD5));
  CHECK(VerifyFingerprint(QStringLiteral("BmBEshZP/98IzuhyeJScnasKJpGTQS8VxhfaqpVwPm8"), SHA, MD5));
  CHECK(VerifyFingerprint(QStringLiteral("ssh-ed25519 255 12:34:56:78:9A:BC:DE:F0:12:34:56:78:9a:bc:de:f0"), SHA, MD5));
  CHECK(VerifyFingerprint(QStringLiteral("12-34-56-78-9a-bc-de-f0-12-34-56-78-9a-bc-de-f0"), SHA, MD5));
  CHECK(!VerifyFingerprint(QStringLiteral("ssh-ed25519 255 AmBEshZP/98IzuhyeJScnasKJpGTQS8VxhfaqpVwPm8"), SHA, MD5));
  CHECK(!VerifyFingerprint(QString(), SHA, MD5));
}

// ------------------------------------------------------------------ part 2

static QString CliPath;
static QString ScratchDir;

struct RunResult
{
  int ExitCode = -1;
  QString Output;
};

static RunResult RunCli(const QStringList & Args, const QByteArray & Input = QByteArray(), const QString & WorkDir = QString(),
  const QString & KnownHosts = QString())
{
  QProcess P;
  P.setProgram(CliPath);
  P.setArguments(Args);
  P.setProcessChannelMode(QProcess::MergedChannels);
  QProcessEnvironment Env = QProcessEnvironment::systemEnvironment();
  Env.insert(QStringLiteral("XDG_CONFIG_HOME"), ScratchDir + QStringLiteral("/xdg"));
  Env.insert(QStringLiteral("NIXSCP_KNOWN_HOSTS"), KnownHosts.isEmpty() ? (ScratchDir + QStringLiteral("/known_hosts")) : KnownHosts);
  Env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C.UTF-8"));
  Env.remove(QStringLiteral("SSH_AUTH_SOCK"));
  P.setProcessEnvironment(Env);
  P.setWorkingDirectory(WorkDir.isEmpty() ? ScratchDir : WorkDir);
  P.start();
  if (!P.waitForStarted(10000))
  {
    std::printf("FAIL: cannot start %s\n", qPrintable(CliPath));
    Failures++;
    return RunResult();
  }
  P.write(Input);
  P.closeWriteChannel();
  if (!P.waitForFinished(180000))
  {
    std::printf("FAIL: %s timed out\n", qPrintable(Args.join(QLatin1Char(' '))));
    Failures++;
    P.kill();
    P.waitForFinished();
  }
  RunResult Result;
  Result.ExitCode = (P.exitStatus() == QProcess::NormalExit) ? P.exitCode() : -1;
  Result.Output = QString::fromLocal8Bit(P.readAll());
  return Result;
}

static RunResult RunCommands(const QStringList & Commands, const QString & WorkDir = QString(), const QByteArray & Input = QByteArray())
{
  QStringList Args{QStringLiteral("/ini=nul"), QStringLiteral("/command")};
  Args.append(Commands);
  return RunCli(Args, Input, WorkDir);
}

static void TestCli()
{
  Section("cli basics");
  RunResult R = RunCli({QStringLiteral("/help")});
  CHECK_MSG((R.ExitCode == 0) && R.Output.contains(QStringLiteral("Usage:")) && R.Output.contains(QStringLiteral("/script=")), R.Output);
  R = RunCli({QStringLiteral("-help")});
  CHECK_MSG((R.ExitCode == 0) && R.Output.contains(QStringLiteral("Usage:")), R.Output);

  R = RunCommands({QStringLiteral("echo Hello  world"), QStringLiteral("exit")});
  CHECK_MSG((R.ExitCode == 0) && (R.Output == QStringLiteral("Hello  world\n")), R.Output);

  // option listing and setting
  R = RunCommands({QStringLiteral("option"), QStringLiteral("option confirm on"), QStringLiteral("option tr ascii"), QStringLiteral("exit")});
  CHECK_MSG(R.ExitCode == 0, R.Output);
  CHECK_MSG(R.Output.contains(QStringLiteral("batch           abort")), R.Output);
  CHECK_MSG(R.Output.contains(QStringLiteral("confirm         on")), R.Output);
  CHECK_MSG(R.Output.contains(QStringLiteral("transfer        ascii")), R.Output);
  R = RunCommands({QStringLiteral("option batch maybe"), QStringLiteral("exit")});
  CHECK_MSG((R.ExitCode == 1) && R.Output.contains(QStringLiteral("Unknown value 'maybe' of option 'batch'.")), R.Output);

  // help
  R = RunCommands({QStringLiteral("help"), QStringLiteral("help put"), QStringLiteral("exit")});
  CHECK_MSG((R.ExitCode == 0) && R.Output.contains(QStringLiteral("put      Uploads file from local directory to remote directory")) &&
    R.Output.contains(QStringLiteral("-neweronly")), R.Output);

  // errors: batch abort (default for scripts) stops at the first error, exit code 1
  R = RunCommands({QStringLiteral("frobnicate"), QStringLiteral("echo after"), QStringLiteral("exit")});
  CHECK_MSG((R.ExitCode == 1) && R.Output.contains(QStringLiteral("Unknown command 'frobnicate'.")) &&
    !R.Output.contains(QStringLiteral("after")), R.Output);
  R = RunCommands({QStringLiteral("e"), QStringLiteral("exit")});
  CHECK_MSG((R.ExitCode == 1) && R.Output.contains(QStringLiteral("Ambiguous command 'e'. Possible matches are: echo, exit")), R.Output);
  // ... batch continue/on go on, but the exit code still reports the failure
  R = RunCommands({QStringLiteral("option batch continue"), QStringLiteral("pwd"), QStringLiteral("echo after"), QStringLiteral("exit")});
  CHECK_MSG((R.ExitCode == 1) && R.Output.contains(QStringLiteral("No session.")) && R.Output.contains(QStringLiteral("after")), R.Output);
  R = RunCommands({QStringLiteral("option batch on"), QStringLiteral("ls"), QStringLiteral("echo after"), QStringLiteral("exit")});
  CHECK_MSG((R.ExitCode == 1) && R.Output.contains(QStringLiteral("after")), R.Output);
  // parameter checks
  R = RunCommands({QStringLiteral("lcd"), QStringLiteral("exit")});
  CHECK_MSG((R.ExitCode == 1) && R.Output.contains(QStringLiteral("Missing parameter for command 'lcd'.")), R.Output);
  R = RunCommands({QStringLiteral("pwd x"), QStringLiteral("exit")});
  CHECK_MSG((R.ExitCode == 1) && R.Output.contains(QStringLiteral("Too many parameters for command 'pwd'.")), R.Output);
  R = RunCommands({QStringLiteral("lls -foo"), QStringLiteral("exit")});
  CHECK_MSG((R.ExitCode == 1) && R.Output.contains(QStringLiteral("Unknown switch 'foo'.")), R.Output);
  // comments and empty lines
  R = RunCommands({QStringLiteral("# comment"), QStringLiteral("; comment"), QString(), QStringLiteral("exit")});
  CHECK_MSG((R.ExitCode == 0) && R.Output.isEmpty(), R.Output);

  // no exit: interactive mode follows, ends at EOF of stdin
  R = RunCommands({QStringLiteral("echo one")}, QString(), QByteArray("echo two\n"));
  CHECK_MSG((R.ExitCode == 0) && R.Output.contains(QStringLiteral("one\nwinscp> two\nwinscp> ")), R.Output);
  // interactive mode: errors do not stop it
  R = RunCli({QStringLiteral("/ini=nul")}, QByteArray("bogus\necho still here\nexit\n"));
  CHECK_MSG((R.ExitCode == 1) && R.Output.contains(QStringLiteral("still here")), R.Output);

  // parameters, environment and timestamps
  qputenv("NIXSCP_TEST_VAR", "envvalue");
  R = RunCli({QStringLiteral("/ini=nul"), QStringLiteral("/command"), QStringLiteral("echo [%1%] [%2%] [%NIXSCP_TEST_VAR%] [%UNDEFINED_NIXSCP_VAR%]"),
    QStringLiteral("echo %TIMESTAMP#yyyy%"), QStringLiteral("exit"), QStringLiteral("/parameter"), QStringLiteral("//"), QStringLiteral("a b"), QStringLiteral("-c")});
  CHECK_MSG(R.Output.contains(QStringLiteral("[a b] [-c] [envvalue] [%UNDEFINED_NIXSCP_VAR%]")), R.Output);
  CHECK_MSG(R.Output.contains(QString::number(QDate::currentDate().year())), R.Output);

  // script file, local commands
  QDir(ScratchDir).mkpath(QStringLiteral("local/sub"));
  QFile Script(ScratchDir + QStringLiteral("/script.txt"));
  CHECK(Script.open(QIODevice::WriteOnly));
  Script.write("\xEF\xBB\xBF" "lcd local\r\nlpwd\r\nlls\r\nlls *.none\r\noption failonnomatch on\r\nlls *.none\r\nexit\r\n");
  Script.close();
  R = RunCli({QStringLiteral("/ini=nul"), QStringLiteral("/script=") + Script.fileName()});
  QString LocalDir = QDir(ScratchDir + QStringLiteral("/local")).canonicalPath();
  CHECK_MSG(R.Output.startsWith(LocalDir + QLatin1Char('\n') + LocalDir + QLatin1Char('\n')), R.Output);
  CHECK_MSG(R.Output.contains(QRegularExpression(QStringLiteral("<DIR> +sub\n"))), R.Output);
  CHECK_MSG(R.Output.contains(QStringLiteral("No file matching '*.none' found.")), R.Output);
  CHECK_MSG(R.ExitCode == 1, R.Output);
  R = RunCli({QStringLiteral("/ini=nul"), QStringLiteral("/script=") + ScratchDir + QStringLiteral("/missing.txt")});
  CHECK_MSG((R.ExitCode == 1) && R.Output.contains(QStringLiteral("not found")), R.Output);

  // stored sites from the configuration (XDG_CONFIG_HOME) and from /ini=
  QDir(ScratchDir).mkpath(QStringLiteral("xdg/nixscp"));
  QFile Ini(ScratchDir + QStringLiteral("/site.ini"));
  CHECK(Ini.open(QIODevice::WriteOnly));
  Ini.write("[Sessions\\Folder/Site]\nHostName=127.0.0.1\nPortNumber=1\nUserName=nobody\n");
  Ini.close();
  R = RunCli({QStringLiteral("/ini=") + Ini.fileName(), QStringLiteral("/command"), QStringLiteral("open Folder"), QStringLiteral("exit")});
  CHECK_MSG((R.ExitCode == 1) && R.Output.contains(QStringLiteral("Cannot open site folder or workspace.")), R.Output);
  R = RunCli({QStringLiteral("/ini=") + Ini.fileName(), QStringLiteral("/command"), QStringLiteral("open Folder/Site"), QStringLiteral("exit")});
  CHECK_MSG((R.ExitCode == 1) && R.Output.contains(QStringLiteral("In scripting you should not rely on saved sites")) &&
    R.Output.contains(QStringLiteral("open sftp://nobody@127.0.0.1:1/")) && R.Output.contains(QStringLiteral("Connecting to host...")) &&
    !R.Output.contains(QStringLiteral("Session started.")), R.Output);
  // command line session (deprecated in scripting); connection refused
  R = RunCli({QStringLiteral("/ini=nul"), QStringLiteral("sftp://nobody@127.0.0.1:1/"), QStringLiteral("/command"), QStringLiteral("echo next")});
  CHECK_MSG((R.ExitCode == 1) && R.Output.contains(QStringLiteral("Opening session using command-line parameter in scripting is deprecated.")) &&
    R.Output.contains(QStringLiteral("Network error")) && R.Output.contains(QStringLiteral("next")), R.Output);
  // WebDAV is supported now: the session is routed to a real connection attempt
  R = RunCommands({QStringLiteral("open dav://127.0.0.1:1/"), QStringLiteral("exit")});
  CHECK_MSG((R.ExitCode == 1) && !R.Output.contains(QStringLiteral("not supported")) &&
    R.Output.contains(QStringLiteral("Connecting to host...")), R.Output);
}

// ------------------------------------------------------------------ part 3

static QByteArray Sha256(const QString & FileName)
{
  QFile F(FileName);
  if (!F.open(QIODevice::ReadOnly))
  {
    return QByteArray();
  }
  QCryptographicHash Hash(QCryptographicHash::Sha256);
  Hash.addData(&F);
  return Hash.result().toHex();
}

static void WriteFile(const QString & FileName, const QByteArray & Data, qint64 MTime = 0)
{
  QFile F(FileName);
  if (!F.open(QIODevice::WriteOnly | QIODevice::Truncate) || (F.write(Data) != Data.size()))
  {
    std::printf("FAIL: cannot write %s\n", qPrintable(FileName));
    Failures++;
    return;
  }
  F.close();
  if (MTime > 0)
  {
    Platform::SetFileTimes(FileName, MTime);
  }
}

static qint64 MTime(const QString & FileName)
{
  return QFileInfo(FileName).lastModified().toSecsSinceEpoch();
}

static void TestServer(const QString & Host, int Port, const QString & User, const QString & Key,
  const QString & PassKey, const QString & Passphrase)
{
  Section("server: host keys");
  QString Url = QStringLiteral("sftp://%1@%2:%3/").arg(User, Host).arg(Port);
  QString OpenBase = QStringLiteral("open %1 -privatekey=\"%2\"").arg(Url, Key);
  QFile::remove(ScratchDir + QStringLiteral("/known_hosts"));

  // unknown host key, batch mode: aborted without waiting, fingerprint reported
  RunResult R = RunCommands({OpenBase, QStringLiteral("echo after"), QStringLiteral("exit")});
  CHECK_MSG((R.ExitCode == 1) && R.Output.contains(QStringLiteral("Continue connecting and add host key to the cache?")) &&
    R.Output.contains(QStringLiteral("(Y)es, (N)o, (C)opy key, (A)bort")) &&
    R.Output.contains(QStringLiteral("Host key wasn't verified!")) && !R.Output.contains(QStringLiteral("after")), R.Output);
  QRegularExpressionMatch M = QRegularExpression(QStringLiteral("Host key fingerprint is (.+)\\.\n")).match(R.Output);
  CHECK_MSG(M.hasMatch(), R.Output);
  QString Fingerprint = M.captured(1);
  CHECK(!QFileInfo::exists(ScratchDir + QStringLiteral("/known_hosts")) || (QFileInfo(ScratchDir + QStringLiteral("/known_hosts")).size() == 0));

  // wrong configured key
  R = RunCommands({OpenBase + QStringLiteral(" -hostkey=\"ssh-ed25519 255 AAAAshZP/98IzuhyeJScnasKJpGTQS8VxhfaqpVwPm8\""), QStringLiteral("exit")});
  CHECK_MSG((R.ExitCode == 1) && R.Output.contains(QStringLiteral("Host key does not match configured key fingerprint")) &&
    !R.Output.contains(QStringLiteral("Continue connecting")), R.Output);
  // any key
  R = RunCommands({OpenBase + QStringLiteral(" -hostkey=*"), QStringLiteral("exit")});
  CHECK_MSG((R.ExitCode == 0) && R.Output.contains(QStringLiteral("WARNING! Giving up security and accepting any host key as configured!")) &&
    R.Output.contains(QStringLiteral("Session started.")), R.Output);
  // interactive answers from stdin: copy key, then accept (stores into known_hosts)
  R = RunCommands({QStringLiteral("option batch off"), OpenBase, QStringLiteral("exit")}, QString(), QByteArray("c\ny\n"));
  CHECK_MSG((R.ExitCode == 0) && R.Output.contains(QStringLiteral("(A)bort: Copy key\n") + Fingerprint) &&
    R.Output.contains(QStringLiteral("(A)bort: Yes\n")) && R.Output.contains(QStringLiteral("Active session: [1] ") + User + QStringLiteral("@") + Host), R.Output);
  CHECK(QFileInfo(ScratchDir + QStringLiteral("/known_hosts")).size() > 0);
  // known now: no prompt
  R = RunCommands({OpenBase, QStringLiteral("exit")});
  CHECK_MSG((R.ExitCode == 0) && !R.Output.contains(QStringLiteral("Continue connecting")), R.Output);
  QFile::remove(ScratchDir + QStringLiteral("/known_hosts"));

  QString Open = OpenBase + QStringLiteral(" -hostkey=\"%1\"").arg(Fingerprint);
  R = RunCommands({Open, QStringLiteral("pwd"), QStringLiteral("exit")});
  CHECK_MSG((R.ExitCode == 0) && R.Output.contains(QStringLiteral("Searching for host...\nConnecting to host...\nAuthenticating...\n")) &&
    R.Output.contains(QStringLiteral("Authenticated.\nStarting the session...\nSession started.\nActive session: [1] ")), R.Output);
  // fingerprint without type/size, as SHA256:...
  QString Bare = Fingerprint.section(QLatin1Char(' '), -1);
  R = RunCommands({OpenBase + QStringLiteral(" -hostkey=SHA256:") + Bare, QStringLiteral("exit")});
  CHECK_MSG(R.ExitCode == 0, R.Output);
  // the command line session with switches
  R = RunCli({QStringLiteral("/ini=nul"), Url, QStringLiteral("-privatekey=") + Key, QStringLiteral("/hostkey=") + Fingerprint,
    QStringLiteral("/command"), QStringLiteral("pwd"), QStringLiteral("exit")});
  CHECK_MSG((R.ExitCode == 0) && R.Output.contains(QStringLiteral("Session started.")), R.Output);
  // wrong key file
  R = RunCommands({QStringLiteral("open %1 -privatekey=\"%2/none\" -hostkey=\"%3\"").arg(Url, ScratchDir, Fingerprint), QStringLiteral("exit")});
  CHECK_MSG((R.ExitCode == 1) && !R.Output.contains(QStringLiteral("Session started.")), R.Output);
  if (!PassKey.isEmpty())
  {
    R = RunCommands({QStringLiteral("open %1 -privatekey=\"%2\" -passphrase=\"%3\" -hostkey=\"%4\"").arg(Url, PassKey, Passphrase, Fingerprint),
      QStringLiteral("exit")});
    CHECK_MSG((R.ExitCode == 0) && R.Output.contains(QStringLiteral("Session started.")), R.Output);
    // batch mode: no passphrase, no prompting
    R = RunCommands({QStringLiteral("open %1 -privatekey=\"%2\" -hostkey=\"%3\"").arg(Url, PassKey, Fingerprint), QStringLiteral("exit")});
    CHECK_MSG((R.ExitCode == 1) && !R.Output.contains(QStringLiteral("Session started.")), R.Output);
  }

  Section("server: commands");
  QString Remote = QDir(ScratchDir).canonicalPath() + QStringLiteral("/remote");
  QString Local = QDir(ScratchDir).canonicalPath() + QStringLiteral("/up");
  QString Down = QDir(ScratchDir).canonicalPath() + QStringLiteral("/down");
  QDir().mkpath(Local + QStringLiteral("/tree/deep"));
  QDir().mkpath(Down);
  QByteArray Big(3 * 1024 * 1024 + 123, Qt::Uninitialized);
  for (qsizetype I = 0; I < Big.size(); I++)
  {
    Big[I] = static_cast<char>((I * 7919) % 251);
  }
  WriteFile(Local + QStringLiteral("/big.bin"), Big, 1500000000);
  WriteFile(Local + QStringLiteral("/a.txt"), "alpha\n", 1600000000);
  WriteFile(Local + QStringLiteral("/b.txt"), "beta\n", 1600000000);
  WriteFile(Local + QStringLiteral("/c.log"), "log\n");
  WriteFile(Local + QStringLiteral("/tree/deep/file.dat"), "deep\n");

  R = RunCommands({Open,
    QStringLiteral("mkdir \"%1\"").arg(Remote),
    QStringLiteral("cd \"%1\"").arg(Remote),
    QStringLiteral("pwd"),
    QStringLiteral("lcd \"%1\"").arg(Local),
    QStringLiteral("put big.bin"),
    QStringLiteral("put *.txt"),
    QStringLiteral("put -nopreservetime c.log"),
    QStringLiteral("put tree"),
    QStringLiteral("ls"),
    QStringLiteral("ls *.txt"),
    QStringLiteral("ls *.none"),
    QStringLiteral("stat a.txt"),
    QStringLiteral("exit")});
  CHECK_MSG(R.ExitCode == 0, R.Output);
  CHECK_MSG(R.Output.contains(Remote + QStringLiteral("\n") + Remote + QStringLiteral("\n")), R.Output);
  CHECK_EQ(Sha256(Remote + QStringLiteral("/big.bin")), Sha256(Local + QStringLiteral("/big.bin")));
  CHECK_EQ(MTime(Remote + QStringLiteral("/big.bin")), 1500000000LL);
  CHECK_EQ(MTime(Remote + QStringLiteral("/a.txt")), 1600000000LL);
  CHECK(QFileInfo::exists(Remote + QStringLiteral("/b.txt")));
  CHECK(MTime(Remote + QStringLiteral("/c.log")) > 1600000000LL);
  CHECK(QFileInfo::exists(Remote + QStringLiteral("/tree/deep/file.dat")));
  CHECK_MSG(R.Output.contains(QRegularExpression(QStringLiteral("\n-rw-[-r][-w]-[-r][-w]-   0 \\S+ +\\S+ +6 Sep 13 \\d\\d:\\d\\d:\\d\\d 2020 a.txt\n"))), R.Output);
  CHECK_MSG(R.Output.contains(QRegularExpression(QStringLiteral("\ndrwx\\S+ +0 \\S+ +\\S+ +\\d+ .{20} \\.\\.\n"))), R.Output);
  CHECK_MSG(R.Output.contains(QRegularExpression(QStringLiteral("\nd\\S+ +0 .* tree\n"))), R.Output);
  CHECK_MSG(R.Output.contains(QStringLiteral("No file matching '*.none' found.")), R.Output);
  // "ls *.txt" lists a.txt and b.txt only; stat a.txt once more
  CHECK_EQ(static_cast<int>(R.Output.count(QStringLiteral(" a.txt\n"))), 3);
  CHECK_EQ(static_cast<int>(R.Output.count(QStringLiteral(" big.bin\n"))), 1);

  // get round trip, wildcards, rename by mask, -delete, local target directory
  R = RunCommands({Open,
    QStringLiteral("cd \"%1\"").arg(Remote),
    QStringLiteral("lcd \"%1\"").arg(Down),
    QStringLiteral("get big.bin"),
    QStringLiteral("get *.txt sub/"),
    QStringLiteral("get a.txt renamed.txt"),
    QStringLiteral("get *.txt *.bak"),
    QStringLiteral("get tree \"%1\"").arg(Down),
    QStringLiteral("get -delete c.log ./"),
    QStringLiteral("get -preservetime=all -speed=100000 -transfer=binary b.txt b2.txt"),
    QStringLiteral("exit")});
  CHECK_MSG(R.ExitCode == 1, R.Output);  // "sub/" does not exist locally
  R = RunCommands({Open,
    QStringLiteral("cd \"%1\"").arg(Remote),
    QStringLiteral("lcd \"%1\"").arg(Down),
    QStringLiteral("get big.bin"),
    QStringLiteral("get a.txt renamed.txt"),
    QStringLiteral("get *.txt *.bak"),
    QStringLiteral("get tree \"%1\"").arg(Down),
    QStringLiteral("get -delete c.log ./"),
    QStringLiteral("get -preservetime -speed=100000 -transfer=binary b.txt b2.txt"),
    QStringLiteral("exit")});
  CHECK_MSG(R.ExitCode == 0, R.Output);
  CHECK_EQ(Sha256(Down + QStringLiteral("/big.bin")), Sha256(Local + QStringLiteral("/big.bin")));
  CHECK_EQ(MTime(Down + QStringLiteral("/big.bin")), 1500000000LL);
  CHECK_EQ(Sha256(Down + QStringLiteral("/renamed.txt")), Sha256(Local + QStringLiteral("/a.txt")));
  CHECK_EQ(Sha256(Down + QStringLiteral("/a.bak")), Sha256(Local + QStringLiteral("/a.txt")));
  CHECK_EQ(Sha256(Down + QStringLiteral("/b.bak")), Sha256(Local + QStringLiteral("/b.txt")));
  CHECK_EQ(Sha256(Down + QStringLiteral("/b2.txt")), Sha256(Local + QStringLiteral("/b.txt")));
  CHECK_EQ(MTime(Down + QStringLiteral("/b2.txt")), 1600000000LL);
  CHECK(QFileInfo::exists(Down + QStringLiteral("/tree/deep/file.dat")));
  CHECK(QFileInfo::exists(Down + QStringLiteral("/c.log")));
  CHECK(!QFileInfo::exists(Remote + QStringLiteral("/c.log")));

  // -neweronly: only the changed file is transferred
  WriteFile(Down + QStringLiteral("/a.txt"), "local old\n", 1500000000);
  WriteFile(Down + QStringLiteral("/b.txt"), "local new\n", 1700000000);
  R = RunCommands({Open,
    QStringLiteral("cd \"%1\"").arg(Remote),
    QStringLiteral("lcd \"%1\"").arg(Down),
    QStringLiteral("get -neweronly *.txt"),
    QStringLiteral("exit")});
  CHECK_MSG(R.ExitCode == 0, R.Output);
  CHECK_EQ(Sha256(Down + QStringLiteral("/a.txt")), Sha256(Local + QStringLiteral("/a.txt")));
  CHECK_EQ(QFileInfo(Down + QStringLiteral("/b.txt")).size(), 10LL);
  // ... and for uploads (b.txt is newer locally now)
  R = RunCommands({Open,
    QStringLiteral("cd \"%1\"").arg(Remote),
    QStringLiteral("lcd \"%1\"").arg(Down),
    QStringLiteral("put -neweronly a.txt b.txt ./"),
    QStringLiteral("put -neweronly a.txt b.txt ./*.new"),
    QStringLiteral("exit")});
  CHECK_MSG(R.ExitCode == 0, R.Output);
  CHECK_EQ(QFileInfo(Remote + QStringLiteral("/b.txt")).size(), 10LL);
  CHECK_EQ(QFileInfo(Remote + QStringLiteral("/a.txt")).size(), 6LL);
  CHECK(QFileInfo::exists(Remote + QStringLiteral("/a.new")) && QFileInfo::exists(Remote + QStringLiteral("/b.new")));

  // overwrite confirmation in batch mode: aborted, script stops
  R = RunCommands({Open,
    QStringLiteral("cd \"%1\"").arg(Remote),
    QStringLiteral("lcd \"%1\"").arg(Down),
    QStringLiteral("option confirm on"),
    QStringLiteral("get a.txt"),
    QStringLiteral("echo after"),
    QStringLiteral("exit")});
  // LOCAL_FILE_OVERWRITE2 (TTerminal::ConfirmFileOverwrite)
  CHECK_MSG((R.ExitCode == 1) && R.Output.contains(QStringLiteral("Overwrite local file 'a.txt'?")) &&
    R.Output.contains(QStringLiteral("(Y)es, (N)o, (C)ancel, A(p)pend, New na(m)e, Ne(w)er only, N(o) to All, Yes to A(l)l: Cancel")) &&
    !R.Output.contains(QStringLiteral("after")), R.Output);
  // ... interactive answer from stdin
  WriteFile(Down + QStringLiteral("/a.txt"), "changed\n", 1500000000);
  R = RunCommands({Open,
    QStringLiteral("cd \"%1\"").arg(Remote),
    QStringLiteral("lcd \"%1\"").arg(Down),
    QStringLiteral("option batch off"),
    QStringLiteral("option confirm on"),
    QStringLiteral("get a.txt b.txt ./"),
    QStringLiteral("exit")}, QString(), QByteArray("n\ny\n"));
  CHECK_MSG(R.ExitCode == 0, R.Output);
  CHECK_EQ(QFileInfo(Down + QStringLiteral("/a.txt")).size(), 8LL);
  CHECK_EQ(QFileInfo(Down + QStringLiteral("/b.txt")).size(), 10LL);

  // put -delete, mv, chmod, ln, rm with mask, rmdir, cd errors
  WriteFile(Local + QStringLiteral("/moveme.txt"), "move\n");
  R = RunCommands({Open,
    QStringLiteral("cd \"%1\"").arg(Remote),
    QStringLiteral("lcd \"%1\"").arg(Local),
    QStringLiteral("put -delete moveme.txt"),
    QStringLiteral("mv moveme.txt moved.txt"),
    QStringLiteral("mkdir dir"),
    QStringLiteral("mv moved.txt dir"),
    QStringLiteral("mv a.new b.new dir/*.old"),
    QStringLiteral("chmod 600 a.txt"),
    QStringLiteral("chmod 640 *.bin"),
    QStringLiteral("ln a.txt link.txt"),
    QStringLiteral("stat link.txt"),
    QStringLiteral("cd dir"),
    QStringLiteral("cd .."),
    QStringLiteral("cd"),
    QStringLiteral("cd \"%1\"").arg(Remote),
    QStringLiteral("rm *.new"),
    QStringLiteral("exit")});
  CHECK_MSG(R.ExitCode == 0, R.Output);
  CHECK(!QFileInfo::exists(Local + QStringLiteral("/moveme.txt")));
  CHECK(QFileInfo::exists(Remote + QStringLiteral("/dir/moved.txt")));
  CHECK(QFileInfo::exists(Remote + QStringLiteral("/dir/a.old")) && QFileInfo::exists(Remote + QStringLiteral("/dir/b.old")));
  CHECK_EQ(static_cast<int>(QFileInfo(Remote + QStringLiteral("/a.txt")).permissions() & 0x7777), static_cast<int>(QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser | QFile::WriteUser));
  struct stat St;
  CHECK((stat(QFile::encodeName(Remote + QStringLiteral("/big.bin")).constData(), &St) == 0) && ((St.st_mode & 0777) == 0640));
  CHECK(QFileInfo(Remote + QStringLiteral("/link.txt")).isSymLink());
  CHECK_MSG(R.Output.contains(QStringLiteral("link.txt")) && R.Output.contains(QRegularExpression(QStringLiteral("\nl\\S+ .* link.txt"))), R.Output);
  CHECK_MSG(R.Output.contains(Remote + QStringLiteral("/dir\n") + Remote + QStringLiteral("\n")), R.Output);
  CHECK_MSG(R.Output.contains(Remote + QStringLiteral("/moveme.txt\n")), R.Output);
  CHECK_MSG(R.Output.contains(Remote + QStringLiteral("/a.txt\n")), R.Output);

  R = RunCommands({Open,
    QStringLiteral("cd \"%1\"").arg(Remote),
    QStringLiteral("cd nonexistent"),
    QStringLiteral("exit")});
  CHECK_MSG((R.ExitCode == 1) && R.Output.contains(QStringLiteral("Error changing directory to 'nonexistent'.")), R.Output);
  R = RunCommands({Open,
    QStringLiteral("cd \"%1\"").arg(Remote),
    QStringLiteral("rm nonexistent.txt"),
    QStringLiteral("exit")});
  CHECK_MSG(R.ExitCode == 1, R.Output);
  R = RunCommands({Open,
    QStringLiteral("cd \"%1\"").arg(Remote),
    QStringLiteral("rm *.nomatch"),
    QStringLiteral("option failonnomatch on"),
    QStringLiteral("rm *.nomatch"),
    QStringLiteral("echo after"),
    QStringLiteral("exit")});
  CHECK_MSG((R.ExitCode == 1) && (R.Output.count(QStringLiteral("No file matching '*.nomatch' found.")) == 2) &&
    !R.Output.contains(QStringLiteral("after")), R.Output);
  // put of a missing file fails
  R = RunCommands({Open,
    QStringLiteral("cd \"%1\"").arg(Remote),
    QStringLiteral("put \"%1/missing.file\"").arg(Local),
    QStringLiteral("exit")});
  CHECK_MSG((R.ExitCode == 1) && R.Output.contains(QStringLiteral("missing.file")), R.Output);
  // filemask include/exclude
  R = RunCommands({Open,
    QStringLiteral("cd \"%1\"").arg(Remote),
    QStringLiteral("mkdir masked"),
    QStringLiteral("put -filemask=*.txt \"%1/*\" masked/").arg(Local),
    QStringLiteral("exit")});
  CHECK_MSG(R.ExitCode == 0, R.Output);
  CHECK(QFileInfo::exists(Remote + QStringLiteral("/masked/a.txt")));
  CHECK(!QFileInfo::exists(Remote + QStringLiteral("/masked/big.bin")));

  // sessions: two sessions, switching, closing
  R = RunCommands({Open, Open,
    QStringLiteral("session"),
    QStringLiteral("session 1"),
    QStringLiteral("close 2"),
    QStringLiteral("close"),
    QStringLiteral("pwd"),
    QStringLiteral("exit")});
  QString Name = User + QStringLiteral("@") + Host;
  CHECK_MSG((R.ExitCode == 1) && R.Output.contains(QStringLiteral("Active session: [2] ") + Name) &&
    R.Output.contains(QStringLiteral("  1  ") + Name + QStringLiteral("\n  2  ") + Name + QStringLiteral("\nActive session: [2] ")) &&
    R.Output.contains(QStringLiteral("Active session: [1] ") + Name + QStringLiteral("\nSession '") + Name +
      QStringLiteral("' closed.\nSession '") + Name + QStringLiteral("' closed.\nNo session.\nNo session.\n")), R.Output);

  Section("server: synchronize");
  QString SyncLocal = QDir(ScratchDir).canonicalPath() + QStringLiteral("/synclocal");
  QString SyncRemote = Remote + QStringLiteral("/sync");
  QDir().mkpath(SyncLocal + QStringLiteral("/sub"));
  QDir().mkpath(SyncRemote);
  WriteFile(SyncLocal + QStringLiteral("/one.txt"), "one\n", 1600000000);
  WriteFile(SyncLocal + QStringLiteral("/sub/two.txt"), "two\n", 1600000000);
  WriteFile(SyncRemote + QStringLiteral("/orphan.txt"), "orphan\n", 1600000000);
  R = RunCommands({Open,
    QStringLiteral("synchronize remote -preview -delete \"%1\" \"%2\"").arg(SyncLocal, SyncRemote),
    QStringLiteral("synchronize remote -delete \"%1\" \"%2\"").arg(SyncLocal, SyncRemote),
    QStringLiteral("synchronize remote \"%1\" \"%2\"").arg(SyncLocal, SyncRemote),
    QStringLiteral("exit")});
  CHECK_MSG((R.ExitCode == 0) && R.Output.contains(QStringLiteral("Comparing...\nDifferences found:\n")) &&
    R.Output.contains(QStringLiteral("New local file ./one.txt [4, Sep 13 ")) &&
    R.Output.contains(QStringLiteral("Orphan remote file ./orphan.txt [7, ")) &&
    R.Output.contains(QStringLiteral("New local file ./sub/\n")) &&
    R.Output.contains(QStringLiteral("Synchronizing...\nLocal '") + SyncLocal + QStringLiteral("' => Remote '") + SyncRemote + QStringLiteral("'\n")) &&
    R.Output.contains(QStringLiteral("'") + SyncRemote + QStringLiteral("/orphan.txt' deleted\n")) &&
    R.Output.contains(QStringLiteral("Comparing...\nNothing to synchronize.\n")), R.Output);
  CHECK(QFileInfo::exists(SyncRemote + QStringLiteral("/sub/two.txt")));
  CHECK(!QFileInfo::exists(SyncRemote + QStringLiteral("/orphan.txt")));
  CHECK_EQ(MTime(SyncRemote + QStringLiteral("/one.txt")), 1600000000LL);
  // local direction and errors
  WriteFile(SyncRemote + QStringLiteral("/fromremote.txt"), "remote\n");
  R = RunCommands({Open,
    QStringLiteral("cd \"%1\"").arg(SyncRemote),
    QStringLiteral("lcd \"%1\"").arg(SyncLocal),
    QStringLiteral("synchronize local"),
    QStringLiteral("synchronize sideways"),
    QStringLiteral("exit")});
  CHECK_MSG((R.ExitCode == 1) && R.Output.contains(QStringLiteral("Unknown option 'sideways'.")), R.Output);
  CHECK(QFileInfo::exists(SyncLocal + QStringLiteral("/fromremote.txt")));

  // keepuptodate: runs until Ctrl+C (needs SIGINT delivery to the child)
#ifndef _WIN32
  {
    QProcess P;
    P.setProgram(CliPath);
    P.setArguments({QStringLiteral("/ini=nul"), QStringLiteral("/command"), Open,
      QStringLiteral("keepuptodate -delete \"%1\" \"%2\"").arg(SyncLocal, SyncRemote), QStringLiteral("echo after"), QStringLiteral("exit")});
    P.setProcessChannelMode(QProcess::MergedChannels);
    QProcessEnvironment Env = QProcessEnvironment::systemEnvironment();
    Env.insert(QStringLiteral("XDG_CONFIG_HOME"), ScratchDir + QStringLiteral("/xdg"));
    Env.insert(QStringLiteral("NIXSCP_KNOWN_HOSTS"), ScratchDir + QStringLiteral("/known_hosts"));
    P.setProcessEnvironment(Env);
    P.start();
    CHECK(P.waitForStarted(10000));
    QString Output;
    auto WaitFor = [&](const QString & Text)
    {
      QElapsedTimer Timer;
      Timer.start();
      while (!Output.contains(Text) && (Timer.elapsed() < 30000) && (P.state() == QProcess::Running))
      {
        P.waitForReadyRead(200);
        Output += QString::fromLocal8Bit(P.readAll());
      }
      return Output.contains(Text);
    };
    CHECK_MSG(WaitFor(QStringLiteral("Watching for changes in ")), Output);
    WriteFile(SyncLocal + QStringLiteral("/live.txt"), "live\n");
    QFile::remove(SyncLocal + QStringLiteral("/one.txt"));
    CHECK_MSG(WaitFor(QStringLiteral("live.txt' uploaded.")), Output);
    CHECK_MSG(WaitFor(QStringLiteral("one.txt' deleted.")), Output);
    ::kill(static_cast<pid_t>(P.processId()), SIGINT);
    P.waitForFinished(30000);
    Output += QString::fromLocal8Bit(P.readAll());
    CHECK_MSG((P.exitCode() == 1) && Output.contains(QStringLiteral("Watching for changes, press Ctrl-C to abort...")) &&
      Output.contains(QStringLiteral("Terminated by user.")) && !Output.contains(QStringLiteral("after")), Output);
    CHECK(QFileInfo::exists(SyncRemote + QStringLiteral("/live.txt")));
    CHECK(!QFileInfo::exists(SyncRemote + QStringLiteral("/one.txt")));
  }
#endif

  // rmdir, log file
  QString LogFile = ScratchDir + QStringLiteral("/session.log");
  R = RunCli({QStringLiteral("/ini=nul"), QStringLiteral("/log=") + LogFile, QStringLiteral("/command"),
    QStringLiteral("open sftp://%1:secretpw@%2:%3/ -privatekey=\"%4\" -hostkey=\"%5\"").arg(User, Host).arg(Port).arg(Key, Fingerprint),
    QStringLiteral("rmdir \"%1\"").arg(Remote),
    QStringLiteral("exit")});
  CHECK_MSG((R.ExitCode == 0) && R.Output.contains(Remote + QLatin1Char('\n')), R.Output);
  CHECK(!QFileInfo::exists(Remote));
  QFile Log(LogFile);
  CHECK(Log.open(QIODevice::ReadOnly));
  QString LogText = QString::fromUtf8(Log.readAll());
  CHECK_MSG(LogText.contains(QStringLiteral("Script: open sftp://") + User + QStringLiteral(":***@")) && !LogText.contains(QStringLiteral("secretpw")) &&
    LogText.contains(QStringLiteral("Command-line: nixscp-cli /ini=nul")) &&
    LogText.contains(QStringLiteral("< ")) && LogText.contains(QStringLiteral("Script: Session started.")), LogText);
}

int main(int argc, char ** argv)
{
  QCoreApplication App(argc, argv);

  TestTokenizer();
  TestOptions();
  TestUrls();
  TestFingerprints();

  CliPath = qEnvironmentVariable("NIXSCP_CLI");
  if (CliPath.isEmpty())
  {
    CliPath = QCoreApplication::applicationDirPath() + QStringLiteral("/nixscp-cli");
  }
  QTemporaryDir Scratch;
  CHECK(Scratch.isValid());
  ScratchDir = QDir(Scratch.path()).canonicalPath();
  if (!QFileInfo(CliPath).isExecutable())
  {
    std::printf("test_script: nixscp-cli not found at %s, cli tests SKIPPED (set NIXSCP_CLI)\n", qPrintable(CliPath));
  }
  else
  {
    TestCli();
    QByteArray PortStr = qgetenv("NIXSCP_TEST_SSH_PORT");
    QString Key = qEnvironmentVariable("NIXSCP_TEST_SSH_KEY");
    QString Host = qEnvironmentVariable("NIXSCP_TEST_SSH_HOST", QStringLiteral("127.0.0.1"));
    if (PortStr.isEmpty() || Key.isEmpty())
    {
      std::printf("test_script: server tests SKIPPED (set NIXSCP_TEST_SSH_PORT, NIXSCP_TEST_SSH_KEY)\n");
    }
    else if ((Host != QStringLiteral("127.0.0.1")) && (Host != QStringLiteral("localhost")))
    {
      std::printf("test_script: server tests SKIPPED (server must run on this machine)\n");
    }
    else
    {
      TestServer(Host, PortStr.toInt(), qEnvironmentVariable("NIXSCP_TEST_SSH_USER", qEnvironmentVariable("USER")), Key,
        qEnvironmentVariable("NIXSCP_TEST_SSH_PASSKEY"), qEnvironmentVariable("NIXSCP_TEST_SSH_PASSPHRASE"));
    }
  }

  if (Failures > 0)
  {
    std::printf("test_script: %d FAILURES\n", Failures);
    return 1;
  }
  std::printf("test_script: all tests passed\n");
  return 0;
}
