// Tests of the winscp.com scripting parity work in nixscp-cli (WP16):
// session URL/switch parsing (raw "HostKey" = algorithm list, ;fingerprint=,
// explicitly empty password, -explicitssl, -clientcert, -newpassword,
// -passwordsfromfiles), MinimizeName and log file name patterns, the new
// command-line modes (/info, /fingerprintscan, /batchsettings, --help,
// --version, /stdout, /xmllog) and, against a real SSH server, the commands
// fixed or added: get/put of a directory under a new name, call exit codes,
// cp, checksum, "get ... -" with /stdout[=chunked], "put - ..." with /stdin,
// the XML log with /xmlgroups and rm into the remote recycle bin.
//
// The cli sources are compiled in. The binary is NIXSCP_CLI or nixscp-cli
// next to this test. Server tests need NIXSCP_TEST_SSH_PORT and
// NIXSCP_TEST_SSH_KEY (optional NIXSCP_TEST_SSH_HOST, NIXSCP_TEST_SSH_USER);
// the server must run on this machine (remote paths are checked locally).
// ~/.ssh and ~/.config/nixscp are never touched (XDG_CONFIG_HOME and
// NIXSCP_KNOWN_HOSTS point to a scratch directory).
#include "cli/Options.cpp"
#include "cli/SessionUrl.cpp"
#include "cli/ConsoleUserInterface.cpp"
#include "cli/Script.cpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>

#include <cstdio>

static int Failures = 0;

#define CHECK(Cond) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); std::fflush(stdout); Failures++; } } while (0)
#define CHECK_MSG(Cond, Msg) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n----\n%s\n----\n", __FILE__, __LINE__, #Cond, qPrintable(Msg)); std::fflush(stdout); Failures++; } } while (0)
#define CHECK_EQ(A, B) \
  do { auto VA = (A); auto VB = (B); if (!(VA == VB)) { std::printf("FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #A, #B); \
    std::fflush(stdout); Failures++; } } while (0)

static void Section(const char * Name)
{
  std::printf("== %s\n", Name);
  std::fflush(stdout);
}

static QString ScratchDir;
static QString CliPath;

static void WriteText(const QString & FileName, const QByteArray & Data)
{
  QFile F(FileName);
  if (!F.open(QIODevice::WriteOnly | QIODevice::Truncate) || (F.write(Data) != Data.size()))
  {
    std::printf("FAIL: cannot write %s\n", qPrintable(FileName));
    Failures++;
  }
}

static QByteArray ReadAll(const QString & FileName)
{
  QFile F(FileName);
  return F.open(QIODevice::ReadOnly) ? F.readAll() : QByteArray();
}

// ------------------------------------------------------------------ units

static void TestSessionSwitches()
{
  Section("session switches");
  StoredSessionList Stored;
  {
    // the raw setting HostKey is the host key algorithm list (WinSCP meaning),
    // SshHostKey the expected fingerprint
    Options O;
    O.SetSwitchMarks(QStringLiteral("-"));
    O.Parse(QStringLiteral("sftp://u@h/ -rawsettings HostKey=ed25519,ecdsa,rsa,WARN"));
    SessionData D;
    SessionExtras X;
    int Info = 0;
    ParseSessionUrl(O.Param(1), &O, &Stored, D, X, Info);
    CHECK_EQ(D.HostKeyList, QStringLiteral("ed25519,ecdsa,rsa,WARN"));
    CHECK(D.HostKey.isEmpty());
  }
  {
    Options O;
    O.SetSwitchMarks(QStringLiteral("-"));
    O.Parse(QStringLiteral("sftp://u@h/ -rawsettings SshHostKey=ssh-ed25519-255-abc"));
    SessionData D;
    SessionExtras X;
    int Info = 0;
    ParseSessionUrl(O.Param(1), &O, &Stored, D, X, Info);
    CHECK_EQ(D.HostKey, QStringLiteral("ssh-ed25519-255-abc"));
  }
  {
    // several fingerprints, -certificate is the same setting
    Options O;
    O.SetSwitchMarks(QStringLiteral("-"));
    O.Parse(QStringLiteral("ftpes://h/ -certificate=\"aa:bb;cc:dd\" -clientcert=/c.pem -newpassword=np"));
    SessionData D;
    SessionExtras X;
    int Info = 0;
    ParseSessionUrl(O.Param(1), &O, &Stored, D, X, Info);
    CHECK_EQ(D.HostKey, QStringLiteral("aa:bb;cc:dd"));
    CHECK_EQ(D.TlsCertificateFile, QStringLiteral("/c.pem"));
    CHECK_EQ(X.NewPassword, QStringLiteral("np"));
    QString Unused;
    CHECK(!O.UnusedSwitch(Unused));
  }
  {
    // ;fingerprint= goes to the core-verified setting
    SessionData D;
    SessionExtras X;
    int Info = 0;
    ParseSessionUrl(QStringLiteral("sftp://u;fingerprint=SHA256:abc@h/"), nullptr, &Stored, D, X, Info);
    CHECK_EQ(D.HostKey, QStringLiteral("SHA256:abc"));
    CHECK_EQ(D.UserName, QStringLiteral("u"));
  }
  {
    // DenormalizeString: "user:@host" is an explicitly empty password
    SessionData D;
    SessionExtras X;
    int Info = 0;
    ParseSessionUrl(QStringLiteral("ftp://u:@h/"), nullptr, &Stored, D, X, Info);
    CHECK(D.Password.isEmpty());
    CHECK(X.EmptyPassword);
    SessionData D2;
    SessionExtras X2;
    ParseSessionUrl(QStringLiteral("ftp://u@h/"), nullptr, &Stored, D2, X2, Info);
    CHECK(!X2.EmptyPassword);
  }
  {
    // -explicitssl (5.5.x compatibility)
    Options O;
    O.SetSwitchMarks(QStringLiteral("-"));
    O.Parse(QStringLiteral("ftp://h/ -explicitssl"));
    SessionData D;
    SessionExtras X;
    int Info = 0;
    ParseSessionUrl(O.Param(1), &O, &Stored, D, X, Info);
    CHECK_EQ(D.Ftps, ftpsExplicitSsl);
    CHECK_EQ(D.PortNumber, 21);
  }
  {
    // -passwordsfromfiles
    QString PwFile = ScratchDir + QStringLiteral("/pw.txt");
    QString PassFile = ScratchDir + QStringLiteral("/pass.txt");
    WriteText(PwFile, "s3cret\r\nignored\n");
    WriteText(PassFile, "\xEF\xBB\xBFphrase\n");
    Options O;
    O.SetSwitchMarks(QStringLiteral("-"));
    O.Parse(QStringLiteral("sftp://u@h/ -password=\"%1\" -passphrase=\"%2\" -passwordsfromfiles").arg(PwFile, PassFile));
    SessionData D;
    SessionExtras X;
    int Info = 0;
    ParseSessionUrl(O.Param(1), &O, &Stored, D, X, Info);
    CHECK_EQ(D.Password, QStringLiteral("s3cret"));
    CHECK_EQ(X.Passphrase, QStringLiteral("phrase"));
  }
}

static void TestHelpers()
{
  Section("helpers");
  // MinimizeName (ScriptProgressFileNameLimit = 25)
  CHECK_EQ(Script::MinimizeName(QStringLiteral("/home/user/file.txt"), 25, true), QStringLiteral("/home/user/file.txt"));
  CHECK_EQ(Script::MinimizeName(QStringLiteral("/home/user/projects/nixscp/file.txt"), 25, true), QStringLiteral("/.../nixscp/file.txt"));
  CHECK_EQ(Script::MinimizeName(QStringLiteral("/a/b/averyveryverylongfilename.txt"), 25, true), QStringLiteral("averyveryverylongfilename"));
  CHECK_EQ(Script::MinimizeName(QStringLiteral("rel/dir/x.txt"), 10, true), QStringLiteral(".../x.txt"));
  // log file name patterns
  SessionData D;
  D.HostName = QStringLiteral("example.com");
  D.UserName = QStringLiteral("me");
  QString Expanded = Script::ExpandLogFileName(QStringLiteral("/tmp/log-!Y!M!D-!@-!S-!!.log"), &D);
  QDate Today = QDate::currentDate();
  CHECK_MSG(Expanded == QStringLiteral("/tmp/log-%1-example.com-me@example.com-!.log").arg(Today.toString(QStringLiteral("yyyyMMdd"))), Expanded);
  CHECK_EQ(Script::ExpandLogFileName(QStringLiteral("x!Py"), nullptr), QStringLiteral("x%1y").arg(QCoreApplication::applicationPid()));
  CHECK_EQ(Script::ExpandLogFileName(QStringLiteral("!S"), nullptr), QStringLiteral("nosession"));
  // passwords masked in logged/echoed open commands, including raw settings
  QString Masked = Script::MaskPasswordInCommandLine(
    QStringLiteral("open sftp://u:pw@h/ -password=x -rawsettings ProxyPassword=secret TunnelPassword=s2 Compression=1"));
  CHECK_MSG(Masked == QStringLiteral("open sftp://u:***@h/ -password=*** -rawsettings ProxyPassword=*** TunnelPassword=*** Compression=1"), Masked);
}

// ------------------------------------------------------------------ cli

struct RunResult
{
  int ExitCode = -1;
  QByteArray StdOut;
  QString Output;   // stderr (+ stdout when merged)
};

static RunResult RunCli(const QStringList & Args, const QByteArray & Input = QByteArray(), bool Merge = true,
  const QString & WorkDir = QString())
{
  QProcess P;
  P.setProgram(CliPath);
  P.setArguments(Args);
  P.setProcessChannelMode(Merge ? QProcess::MergedChannels : QProcess::SeparateChannels);
  QProcessEnvironment Env = QProcessEnvironment::systemEnvironment();
  Env.insert(QStringLiteral("XDG_CONFIG_HOME"), ScratchDir + QStringLiteral("/xdg"));
  Env.insert(QStringLiteral("NIXSCP_KNOWN_HOSTS"), ScratchDir + QStringLiteral("/known_hosts"));
#ifdef Q_OS_MACOS
  // no C.UTF-8 there, and Qt says so on stderr
  Env.insert(QStringLiteral("LC_ALL"), QStringLiteral("en_US.UTF-8"));
#else
  Env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C.UTF-8"));
#endif
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
  if (Merge)
  {
    Result.Output = QString::fromUtf8(P.readAllStandardOutput());
  }
  else
  {
    Result.StdOut = P.readAllStandardOutput();
    Result.Output = QString::fromUtf8(P.readAllStandardError());
  }
  return Result;
}

static void TestCliModes()
{
  Section("cli modes");
  RunResult R = RunCli({QStringLiteral("--version")});
  CHECK_MSG((R.ExitCode == 0) && R.Output.startsWith(QStringLiteral("nixscp-cli ")), R.Output);
  for (const QString & Switch : {QStringLiteral("--help"), QStringLiteral("/h"), QStringLiteral("/?")})
  {
    R = RunCli({Switch});
    CHECK_MSG((R.ExitCode == 0) && R.Output.contains(QStringLiteral("Usage:")) && R.Output.contains(QStringLiteral("/xmllog=")) &&
      R.Output.contains(QStringLiteral("/stdout[=binary|chunked]")) && R.Output.contains(QStringLiteral("/loglevel=")) &&
      R.Output.contains(QStringLiteral("/nointeractiveinput")), R.Output);
  }
  R = RunCli({QStringLiteral("/ini=nul"), QStringLiteral("/info")});
  CHECK_MSG((R.ExitCode == 0) && R.Output.contains(QStringLiteral("SSH encryption ciphers:")) &&
    R.Output.contains(QStringLiteral("aes256-ctr")) && R.Output.contains(QStringLiteral("TLS/SSL cipher suites:")), R.Output);
  R = RunCli({QStringLiteral("/ini=nul"), QStringLiteral("/stdout=foo"), QStringLiteral("/command"), QStringLiteral("exit")});
  CHECK_MSG((R.ExitCode == 1) && R.Output.contains(QStringLiteral("Unknown value 'foo' of option 'stdout'.")), R.Output);
  R = RunCli({QStringLiteral("/ini=nul"), QStringLiteral("/fingerprintscan"), QStringLiteral("ftp://h/")});
  CHECK_MSG((R.ExitCode == 1) && R.Output.contains(QStringLiteral("Secure session (SSH or TLS/SSL) not specified.")), R.Output);
  // L7: an invalid level is ignored, "*-" is the opposite of "*"
  for (const QString & Level : {QStringLiteral("7"), QStringLiteral("x"), QStringLiteral("1*-"), QStringLiteral("*"), QStringLiteral("*-")})
  {
    R = RunCli({QStringLiteral("/ini=nul"), QStringLiteral("/loglevel=") + Level, QStringLiteral("/command"), QStringLiteral("exit")});
    CHECK_MSG(R.ExitCode == 0, Level + QLatin1Char(' ') + R.Output);
  }
  // L17: the exit code is echoed from log level 1 up
  R = RunCli({QStringLiteral("/ini=nul"), QStringLiteral("/loglevel=1"), QStringLiteral("/command"), QStringLiteral("exit")});
  CHECK_MSG(R.Output.contains(QStringLiteral("Exit code: 0\n")), R.Output);
  R = RunCli({QStringLiteral("/ini=nul"), QStringLiteral("/command"), QStringLiteral("exit")});
  CHECK_MSG(!R.Output.contains(QStringLiteral("Exit code:")), R.Output);
  // L8: variables in /log (and /ini)
  qputenv("NIXSCP_TEST_DIR", ScratchDir.toUtf8());
  R = RunCli({QStringLiteral("/ini=nul"), QStringLiteral("/log=%NIXSCP_TEST_DIR%/expanded.log"), QStringLiteral("/command"), QStringLiteral("exit")});
  CHECK_MSG(QFileInfo::exists(ScratchDir + QStringLiteral("/expanded.log")), R.Output);
  WriteText(ScratchDir + QStringLiteral("/expanded.ini"), "[Sessions\\inisite]\nHostName=ini.example\n");
  R = RunCli({QStringLiteral("/ini=$NIXSCP_TEST_DIR/expanded.ini"), QStringLiteral("/batchsettings"), QStringLiteral("ini*"), QStringLiteral("PortNumber=2201")});
  CHECK_MSG(R.Output.contains(QStringLiteral("Found 1 sites")), R.Output);
  // /xmllogrequired: a log that cannot be written stops the script, not only one that cannot be opened
  if (QFileInfo::exists(QStringLiteral("/dev/full")))
  {
    R = RunCli({QStringLiteral("/ini=nul"), QStringLiteral("/xmllog=/dev/full"), QStringLiteral("/xmllogrequired"), QStringLiteral("/command"),
      QStringLiteral("pwd"), QStringLiteral("echo after"), QStringLiteral("exit")});
    CHECK_MSG((R.ExitCode == 1) && !R.Output.contains(QStringLiteral("after")), R.Output);
  }

  // XML log of a failure without a session
  QString Xml = ScratchDir + QStringLiteral("/nosession.xml");
  R = RunCli({QStringLiteral("/ini=nul"), QStringLiteral("/xmllog=") + Xml, QStringLiteral("/command"), QStringLiteral("pwd"), QStringLiteral("exit")});
  QString XmlText = QString::fromUtf8(ReadAll(Xml));
  CHECK_MSG((R.ExitCode == 1) && XmlText.startsWith(QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<session xmlns=\"http://winscp.net/schema/session/1.0\" name=\"nosession\"")) &&
    XmlText.contains(QStringLiteral("<failure>\n    <message>No session.</message>\n  </failure>")) &&
    XmlText.endsWith(QStringLiteral("</session>\n")), XmlText);

  // /batchsettings on a copy of a configuration
  QString Ini = ScratchDir + QStringLiteral("/batch.ini");
  WriteText(Ini, "[Sessions\\one]\nHostName=a.example\n[Sessions\\two]\nHostName=b.example\n[Sessions\\other]\nHostName=c.example\n");
  R = RunCli({QStringLiteral("/ini=") + Ini, QStringLiteral("/batchsettings"), QStringLiteral("o*"), QStringLiteral("PortNumber=2200")});
  CHECK_MSG((R.ExitCode == 0) && R.Output.contains(QStringLiteral("Found 2 sites, Changed 2 sites")) &&
    R.Output.contains(QString::fromUtf8("one \xE2\x80\x93 Changed")), R.Output);
  QString IniText = QString::fromUtf8(ReadAll(Ini));
  CHECK_MSG(IniText.count(QStringLiteral("PortNumber=2200")) == 2, IniText);
  R = RunCli({QStringLiteral("/ini=") + Ini, QStringLiteral("/batchsettings"), QStringLiteral("one")});
  CHECK_MSG((R.ExitCode == 1) && R.Output.contains(QStringLiteral("No site settings specified.")), R.Output);
}

static void TestKeyGen()
{
  Section("keygen");
  auto KeyGen = [](const QStringList & Args, const QByteArray & Input = QByteArray())
  {
    return RunCli(QStringList{QStringLiteral("/ini=nul"), QStringLiteral("/keygen")} + Args, Input);
  };
  RunResult R = KeyGen({});
  CHECK_MSG((R.ExitCode == 1) && R.Output.contains(QStringLiteral("You have to specify input key file.")), R.Output);
  R = KeyGen({ScratchDir + QStringLiteral("/no-such-key")});
  CHECK_MSG((R.ExitCode == 1) && R.Output.contains(QStringLiteral("does not exist or cannot be opened")), R.Output);
  // it is no session to connect to
  CHECK_MSG(!R.Output.contains(QStringLiteral("Host:")) && !R.Output.contains(QStringLiteral("Connecting")), R.Output);

  if (QStandardPaths::findExecutable(QStringLiteral("ssh-keygen")).isEmpty())
  {
    std::printf("keygen conversions SKIPPED (no ssh-keygen)\n");
    return;
  }
  QString OpenSsh = ScratchDir + QStringLiteral("/id_keygen");
  QProcess Gen;
  Gen.start(QStringLiteral("ssh-keygen"), {QStringLiteral("-q"), QStringLiteral("-t"), QStringLiteral("ed25519"), QStringLiteral("-N"), QStringLiteral("old"),
    QStringLiteral("-C"), QStringLiteral("keygen comment"), QStringLiteral("-f"), OpenSsh});
  CHECK(Gen.waitForFinished(30000) && (Gen.exitCode() == 0));

  QString Ppk = ScratchDir + QStringLiteral("/converted.ppk");
  // OpenSSH key: an output file is needed, the passphrase carries over
  R = KeyGen({OpenSsh, QStringLiteral("/passphrase=old")});
  CHECK_MSG((R.ExitCode == 1) && R.Output.contains(QStringLiteral("You have to specify output key file.")), R.Output);
  R = KeyGen({OpenSsh, QStringLiteral("/output=") + Ppk, QStringLiteral("/passphrase=old")});
  CHECK_MSG((R.ExitCode == 0) && R.Output.contains(QStringLiteral("Key saved to \"%1\".").arg(Ppk)), R.Output);
  QByteArray Text = ReadAll(Ppk);
  CHECK_MSG(Text.startsWith("PuTTY-User-Key-File-3: ssh-ed25519\n") && Text.contains("Encryption: aes256-cbc\n") &&
    Text.contains("Comment: keygen comment\n"), QString::fromUtf8(Text));
  // a .ppk: nothing to do
  R = KeyGen({Ppk, QStringLiteral("/passphrase=old")});
  CHECK_MSG((R.ExitCode == 1) && R.Output.contains(QStringLiteral("No action specified.")), R.Output);
  // comment and passphrase in place, the old one wrong first
  R = KeyGen({Ppk, QStringLiteral("/comment=new comment"), QStringLiteral("/changepassphrase=new"), QStringLiteral("/passphrase=wrong")}, "old\n");
  CHECK_MSG(R.ExitCode == 0, R.Output);
  Text = ReadAll(Ppk);
  CHECK_MSG(Text.contains("Comment: new comment\n"), QString::fromUtf8(Text));
  // removing the passphrase; puttygen spelling of the switches
  R = KeyGen({Ppk, QStringLiteral("-o"), Ppk + QStringLiteral(".plain"), QStringLiteral("-P"), QStringLiteral("--new-passphrase"), QString(),
    QStringLiteral("--old-passphrase"), QStringLiteral("new")});
  CHECK_MSG(R.ExitCode == 0, R.Output);
  CHECK_MSG(ReadAll(Ppk + QStringLiteral(".plain")).contains("Encryption: none\n"), R.Output);
  // what is refused, said plainly
  R = KeyGen({OpenSsh + QStringLiteral(".pub"), QStringLiteral("/output=") + Ppk});
  CHECK_MSG((R.ExitCode == 1) && R.Output.contains(QStringLiteral("Converting public keys is not supported.")), R.Output);
  R = KeyGen({Ppk, QStringLiteral("/certificate=/some/cert"), QStringLiteral("/passphrase=new")});
  CHECK_MSG((R.ExitCode == 1) && R.Output.contains(QStringLiteral("not supported")), R.Output);
  WriteText(ScratchDir + QStringLiteral("/ssh1"), "SSH PRIVATE KEY FILE FORMAT 1.1\n");
  R = KeyGen({ScratchDir + QStringLiteral("/ssh1")});
  CHECK_MSG((R.ExitCode == 1) && R.Output.contains(QStringLiteral("Editing SSH-1 keys is not supported.")), R.Output);
  WriteText(ScratchDir + QStringLiteral("/nokey"), "this is no key\n");
  R = KeyGen({ScratchDir + QStringLiteral("/nokey")});
  CHECK_MSG((R.ExitCode == 1) && R.Output.contains(QStringLiteral("does not contain private key in known format")), R.Output);
}

// ------------------------------------------------------------------ server

static void TestScriptFixes(const QString & Url, const QString & Key, const QString & Fingerprint)
{
  QString Open = QStringLiteral("open %1 -privatekey=\"%2\" -hostkey=\"%3\"").arg(Url, Key, Fingerprint);
  QString Remote = ScratchDir + QStringLiteral("/fixes-remote");
  QString Local = ScratchDir + QStringLiteral("/fixes-local");
  QDir().mkpath(Remote);
  QDir().mkpath(Local);
  auto Script = [&](const QStringList & Commands, const QStringList & Extra = QStringList())
  {
    return RunCli(QStringList{QStringLiteral("/ini=nul")} + Extra + QStringList{QStringLiteral("/command")} + QStringList{Open,
      QStringLiteral("cd \"%1\"").arg(Remote), QStringLiteral("lcd \"%1\"").arg(Local)} + Commands + QStringList{QStringLiteral("exit")});
  };

  Section("server: an initial directory that does not exist (B4)");
  RunResult R = RunCli({QStringLiteral("/ini=nul"), QStringLiteral("/command"),
    QStringLiteral("open %1no/such/dir/ -privatekey=\"%2\" -hostkey=\"%3\"").arg(Url, Key, Fingerprint),
    QStringLiteral("echo after"), QStringLiteral("exit")});
  CHECK_MSG((R.ExitCode == 1) && R.Output.contains(QStringLiteral("Error changing directory")) && !R.Output.contains(QStringLiteral("after")), R.Output);

  Section("server: names that differ by case (B3)");
  QDir().mkpath(Local + QStringLiteral("/case"));
  QDir().mkpath(Remote + QStringLiteral("/case"));
  if ((spDefault & spCaseSensitive) != 0)
  {
    WriteText(Local + QStringLiteral("/case/README"), "upper\n");
    WriteText(Local + QStringLiteral("/case/readme"), "lower\n");
    R = Script({QStringLiteral("synchronize remote case case")});
    CHECK_MSG(R.ExitCode == 0, R.Output);
    CHECK_EQ(ReadAll(Remote + QStringLiteral("/case/README")), QByteArray("upper\n"));
    CHECK_EQ(ReadAll(Remote + QStringLiteral("/case/readme")), QByteArray("lower\n"));
    CHECK_MSG(!R.Output.contains(QStringLiteral("different from factory defaults")), R.Output);
    // a script can still ask for the other, with the warning of WinSCP
    QFile::remove(Remote + QStringLiteral("/case/README"));
    QFile::remove(Remote + QStringLiteral("/case/readme"));
    R = Script({QStringLiteral("synchronize remote case case")}, {QStringLiteral("/rawconfig"), QStringLiteral("Interface\\SynchronizeParams=66")});
    CHECK_MSG((R.ExitCode == 0) && R.Output.contains(QStringLiteral("Using configured synchronization options different from factory defaults.")), R.Output);
    CHECK_EQ(QDir(Remote + QStringLiteral("/case")).entryList(QDir::Files).size(), 1);
  }
  else
  {
    // macOS: the disk has no two such names, and by default they are one file
    QDateTime Time = QDateTime::currentDateTimeUtc().addSecs(-3600);
    for (const QString & Name : {Local + QStringLiteral("/case/README"), Remote + QStringLiteral("/case/readme")})
    {
      WriteText(Name, "same\n");
      QFile F(Name);
      CHECK(F.open(QIODevice::ReadWrite) && F.setFileTime(Time, QFileDevice::FileModificationTime));
    }
    R = Script({QStringLiteral("synchronize remote case case")});
    CHECK_MSG((R.ExitCode == 0) && R.Output.contains(QStringLiteral("Nothing to synchronize.")), R.Output);
    CHECK_MSG(!R.Output.contains(QStringLiteral("different from factory defaults")), R.Output);
    // a script can still ask for the other, with the warning of WinSCP
    R = Script({QStringLiteral("synchronize remote case case")},
      {QStringLiteral("/rawconfig"), QStringLiteral("Interface\\SynchronizeParams=%1").arg(spDefault | spCaseSensitive)});
    CHECK_MSG((R.ExitCode == 0) && !R.Output.contains(QStringLiteral("Nothing to synchronize.")) &&
      R.Output.contains(QStringLiteral("Using configured synchronization options different from factory defaults.")), R.Output);
  }

  Section("server: '[' in local paths (B11)");
  WriteText(Local + QStringLiteral("/a[1].txt"), "bracket\n");
  WriteText(Local + QStringLiteral("/[2024] report.txt"), "report\n");
  WriteText(Local + QStringLiteral("/a1.txt"), "not this\n");
  R = Script({QStringLiteral("put \"a[1].txt\""), QStringLiteral("put \"[2024] report.txt\""), QStringLiteral("lls \"a[1]*\"")});
  CHECK_MSG(R.ExitCode == 0, R.Output);
  CHECK_EQ(ReadAll(Remote + QStringLiteral("/a[1].txt")), QByteArray("bracket\n"));
  CHECK_EQ(ReadAll(Remote + QStringLiteral("/[2024] report.txt")), QByteArray("report\n"));
  CHECK(!QFileInfo::exists(Remote + QStringLiteral("/a1.txt")));
  // a wildcard next to it
  R = Script({QStringLiteral("rm \"a[[]1].txt\""), QStringLiteral("put \"a[1]*\"")});
  CHECK_MSG(R.ExitCode == 0, R.Output);
  CHECK_EQ(ReadAll(Remote + QStringLiteral("/a[1].txt")), QByteArray("bracket\n"));
  CHECK(!QFileInfo::exists(Remote + QStringLiteral("/a1.txt")));

  Section("server: put -append, get -append (B12)");
  WriteText(Remote + QStringLiteral("/app.txt"), "one\n");
  WriteText(Local + QStringLiteral("/app.txt"), "two\n");
  R = Script({QStringLiteral("put -append app.txt")});
  CHECK_MSG(R.ExitCode == 0, R.Output);
  CHECK_EQ(ReadAll(Remote + QStringLiteral("/app.txt")), QByteArray("one\ntwo\n"));
  WriteText(Local + QStringLiteral("/app.txt"), "local\n");
  R = Script({QStringLiteral("get -append app.txt")});
  CHECK_MSG(R.ExitCode == 0, R.Output);
  CHECK_EQ(ReadAll(Local + QStringLiteral("/app.txt")), QByteArray("local\none\ntwo\n"));
  // no target: an ordinary transfer
  WriteText(Local + QStringLiteral("/newapp.txt"), "new\n");
  R = Script({QStringLiteral("put -append newapp.txt")});
  CHECK_MSG(R.ExitCode == 0, R.Output);
  CHECK_EQ(ReadAll(Remote + QStringLiteral("/newapp.txt")), QByteArray("new\n"));
  // SCP has no way to append
  R = RunCli({QStringLiteral("/ini=nul"), QStringLiteral("/command"),
    QStringLiteral("open %1 -privatekey=\"%2\" -hostkey=\"%3\"").arg(QString(Url).replace(QStringLiteral("sftp://"), QStringLiteral("scp://")), Key, Fingerprint),
    QStringLiteral("lcd \"%1\"").arg(Local), QStringLiteral("put -append app.txt \"%1/scp.txt\"").arg(Remote), QStringLiteral("exit")});
  CHECK_MSG((R.ExitCode == 1) && R.Output.contains(QStringLiteral("not supported")), R.Output);

  Section("server: -hostkey=acceptnew (B12), ;fingerprint= (M13)");
  QString KnownHosts = ScratchDir + QStringLiteral("/known_hosts");
  QFile::remove(KnownHosts);
  QString AcceptNew = QStringLiteral("open %1 -privatekey=\"%2\" -hostkey=acceptnew").arg(Url, Key);
  R = RunCli({QStringLiteral("/ini=nul"), QStringLiteral("/command"), AcceptNew, QStringLiteral("exit")});
  CHECK_MSG((R.ExitCode == 0) && R.Output.contains(QStringLiteral("Session started.")), R.Output);
  CHECK(!ReadAll(KnownHosts).isEmpty());
  // once known, the key is compared
  QByteArray Hosts = ReadAll(KnownHosts);
  QByteArray Wrong = Hosts.left(Hosts.lastIndexOf(' ')) + " AAAAC3NzaC1lZDI1NTE5AAAAIJ7JYDOxW+0ViCHOQNLEaFcv4UUxbM+AYD+kUYfMdHAh\n";
  WriteText(KnownHosts, Wrong);
  R = RunCli({QStringLiteral("/ini=nul"), QStringLiteral("/command"), AcceptNew, QStringLiteral("exit")});
  CHECK_MSG((R.ExitCode == 1) && !R.Output.contains(QStringLiteral("Session started.")), R.Output);
  CHECK_EQ(ReadAll(KnownHosts), Wrong);
  // a fingerprint in a URL does not outrank a known key, as -hostkey does
  QString FingerprintUrl = QString(Url).replace(QStringLiteral("@"), QStringLiteral(";fingerprint=%1@").arg(QString(Fingerprint).replace(QLatin1Char('+'), QLatin1Char('-')).replace(QLatin1Char('/'), QLatin1Char('_')).replace(QLatin1Char(' '), QLatin1Char('-'))));
  R = RunCli({QStringLiteral("/ini=nul"), QStringLiteral("/command"), QStringLiteral("open %1 -privatekey=\"%2\"").arg(FingerprintUrl, Key), QStringLiteral("exit")});
  CHECK_MSG((R.ExitCode == 1) && !R.Output.contains(QStringLiteral("Session started.")), R.Output);
  R = RunCli({QStringLiteral("/ini=nul"), QStringLiteral("/command"), QStringLiteral("open %1 -privatekey=\"%2\" -hostkey=\"%3\"").arg(Url, Key, Fingerprint), QStringLiteral("exit")});
  CHECK_MSG(R.ExitCode == 0, R.Output);
  QFile::remove(KnownHosts);
  R = RunCli({QStringLiteral("/ini=nul"), QStringLiteral("/command"), QStringLiteral("open %1 -privatekey=\"%2\"").arg(FingerprintUrl, Key), QStringLiteral("exit")});
  CHECK_MSG(R.ExitCode == 0, R.Output);
  Section("server: \"*\" and acceptnew of a URL");
  // what a script writes as a switch counts, what it holds in a URL does not:
  // nothing stored, and in batch mode nobody to ask
  for (const QString & Param : {QStringLiteral(";fingerprint=acceptnew"), QStringLiteral(";fingerprint=*"), QStringLiteral(";fingerprint=%2A"),
    QStringLiteral(";x-SshHostKey=*"), QStringLiteral(";x-SshHostKey=acceptnew")})
  {
    QFile::remove(KnownHosts);
    QString KeyUrl = QString(Url).replace(QStringLiteral("@"), Param + QLatin1Char('@'));
    R = RunCli({QStringLiteral("/ini=nul"), QStringLiteral("/command"), QStringLiteral("open %1 -privatekey=\"%2\"").arg(KeyUrl, Key), QStringLiteral("exit")});
    CHECK_MSG((R.ExitCode == 1) && !R.Output.contains(QStringLiteral("Session started.")), Param + QLatin1Char(' ') + R.Output);
    CHECK_MSG(ReadAll(KnownHosts).isEmpty(), Param);
    // a known key that changed stays a refusal, and stays what it was
    WriteText(KnownHosts, Wrong);
    R = RunCli({QStringLiteral("/ini=nul"), QStringLiteral("/command"), QStringLiteral("open %1 -privatekey=\"%2\"").arg(KeyUrl, Key), QStringLiteral("exit")});
    CHECK_MSG((R.ExitCode == 1) && !R.Output.contains(QStringLiteral("Session started.")), Param + QLatin1Char(' ') + R.Output);
    CHECK_EQ(ReadAll(KnownHosts), Wrong);
  }
  {
    // the fingerprint of the server itself, as a raw setting of a URL: the same as ;fingerprint=
    WriteText(KnownHosts, Wrong);
    QString RawKeyUrl = QString(Url).replace(QStringLiteral("@"), QStringLiteral(";x-SshHostKey=%1@").arg(QString::fromLatin1(QUrl::toPercentEncoding(Fingerprint))));
    R = RunCli({QStringLiteral("/ini=nul"), QStringLiteral("/command"), QStringLiteral("open %1 -privatekey=\"%2\"").arg(RawKeyUrl, Key), QStringLiteral("exit")});
    CHECK_MSG((R.ExitCode == 1) && !R.Output.contains(QStringLiteral("Session started.")), R.Output);
    CHECK_EQ(ReadAll(KnownHosts), Wrong);
    QFile::remove(KnownHosts);
  }
  R = RunCli({QStringLiteral("/ini=nul"), QStringLiteral("/command"), QStringLiteral("open %1 -privatekey=\"%2\" -hostkey=*").arg(Url, Key), QStringLiteral("exit")});
  CHECK_MSG((R.ExitCode == 0) && R.Output.contains(QStringLiteral("Session started.")), R.Output);
  R = RunCli({QStringLiteral("/ini=nul"), QStringLiteral("/command"), QStringLiteral("open %1 -privatekey=\"%2\" -rawsettings SshHostKey=*").arg(Url, Key), QStringLiteral("exit")});
  CHECK_MSG((R.ExitCode == 0) && R.Output.contains(QStringLiteral("Session started.")), R.Output);
  CHECK(ReadAll(KnownHosts).isEmpty());
  Section("server: DisableAcceptingHostKeys (M13)");
  QString System = ScratchDir + QStringLiteral("/sysconf");
  QDir().mkpath(System + QStringLiteral("/nixscp"));
  WriteText(System + QStringLiteral("/nixscp/nixscp.ini"), "DisableAcceptingHostKeys=1\n");
  qputenv("XDG_CONFIG_DIRS", System.toUtf8());
  QFile::remove(KnownHosts);
  QString Interactive = QStringLiteral("open %1 -privatekey=\"%2\"").arg(Url, Key);
  R = RunCli({QStringLiteral("/ini=nul"), QStringLiteral("/command"), QStringLiteral("option batch off"), Interactive, QStringLiteral("exit")}, "y\n");
  CHECK_MSG((R.ExitCode == 1) && !R.Output.contains(QStringLiteral("Session started.")) && !R.Output.contains(QStringLiteral("Continue connecting")), R.Output);
  CHECK(ReadAll(KnownHosts).isEmpty());
  R = RunCli({QStringLiteral("/ini=nul"), QStringLiteral("/command"), AcceptNew, QStringLiteral("exit")});
  CHECK_MSG((R.ExitCode == 1) && !R.Output.contains(QStringLiteral("Session started.")), R.Output);
  CHECK(ReadAll(KnownHosts).isEmpty());
  // a URL is not what the administrator configured
  R = RunCli({QStringLiteral("/ini=nul"), QStringLiteral("/command"), QStringLiteral("open %1 -privatekey=\"%2\"").arg(FingerprintUrl, Key), QStringLiteral("exit")});
  CHECK_MSG((R.ExitCode == 1) && !R.Output.contains(QStringLiteral("Session started.")), R.Output);
  CHECK(ReadAll(KnownHosts).isEmpty());
  QString AnyKeyUrl = QString(Url).replace(QStringLiteral("@"), QStringLiteral(";fingerprint=*@"));
  R = RunCli({QStringLiteral("/ini=nul"), QStringLiteral("/command"), QStringLiteral("open %1 -privatekey=\"%2\"").arg(AnyKeyUrl, Key), QStringLiteral("exit")});
  CHECK_MSG((R.ExitCode == 1) && !R.Output.contains(QStringLiteral("Session started.")), R.Output);
  // a configured key still connects
  R = RunCli({QStringLiteral("/ini=nul"), QStringLiteral("/command"), QStringLiteral("open %1 -privatekey=\"%2\" -hostkey=\"%3\"").arg(Url, Key, Fingerprint), QStringLiteral("exit")});
  CHECK_MSG(R.ExitCode == 0, R.Output);
  qunsetenv("XDG_CONFIG_DIRS");

  Section("server: partial file bigger than the source (L3)");
  WriteText(Local + QStringLiteral("/big.bin"), QByteArray(200000, 'b'));
  WriteText(Remote + QStringLiteral("/big.bin.filepart"), QByteArray(300000, 'p'));
  R = Script({QStringLiteral("put big.bin")});
  CHECK_MSG(R.ExitCode == 0, R.Output);
  CHECK_EQ(ReadAll(Remote + QStringLiteral("/big.bin")), QByteArray(200000, 'b'));
  CHECK(!QFileInfo::exists(Remote + QStringLiteral("/big.bin.filepart")));

  Section("server: relative masks (L4)");
  QDir().mkpath(Local + QStringLiteral("/masks/sub"));
  QDir().mkpath(Local + QStringLiteral("/masks/other"));
  WriteText(Local + QStringLiteral("/masks/sub/a.txt"), "a\n");
  WriteText(Local + QStringLiteral("/masks/other/a.txt"), "b\n");
  QDir().mkpath(Remote + QStringLiteral("/masks"));
  R = Script({QStringLiteral("synchronize remote -filemask=\"./sub/*.txt\" masks masks")});
  CHECK_MSG(R.ExitCode == 0, R.Output);
  CHECK(QFileInfo::exists(Remote + QStringLiteral("/masks/sub/a.txt")));
  CHECK(!QFileInfo::exists(Remote + QStringLiteral("/masks/other/a.txt")));

  Section("server: XML log, one record per file (B15)");
  QString Xml = ScratchDir + QStringLiteral("/fixes.xml");
  QDir().mkpath(Local + QStringLiteral("/tree/deep"));
  WriteText(Local + QStringLiteral("/tree/one.txt"), "12345");
  WriteText(Local + QStringLiteral("/tree/deep/two.txt"), "1234567");
  R = Script({QStringLiteral("pwd"), QStringLiteral("put tree"), QStringLiteral("chmod 640 tree/one.txt"), QStringLiteral("mkdir made"),
    QStringLiteral("get tree treeback")}, {QStringLiteral("/xmllog=") + Xml});
  CHECK_MSG(R.ExitCode == 0, R.Output);
  QString Log = QString::fromUtf8(ReadAll(Xml));
  CHECK_MSG(Log.contains(QStringLiteral("<cwd value=\"%1\" />").arg(Remote)), Log);
  CHECK_MSG(Log.contains(QStringLiteral("<upload>\n    <filename value=\"%1/tree/one.txt\" />\n    <destination value=\"%2/tree/one.txt\" />\n    <size value=\"5\" />").arg(Local, Remote)), Log);
  CHECK_MSG(Log.contains(QStringLiteral("<filename value=\"%1/tree/deep/two.txt\" />").arg(Local)) && Log.contains(QStringLiteral("<size value=\"7\" />")), Log);
  CHECK_MSG(Log.count(QStringLiteral("<upload>")) == 2, Log);
  CHECK_MSG(Log.count(QStringLiteral("<download>")) == 2, Log);
  CHECK_MSG(Log.contains(QStringLiteral("<touch>")) && Log.contains(QStringLiteral("<modification value=")), Log);
  CHECK_MSG(Log.contains(QStringLiteral("<mkdir>\n    <filename value=\"%1/tree\" />").arg(Remote)), Log);
  CHECK_MSG(Log.contains(QStringLiteral("<mkdir>\n    <filename value=\"%1/made\" />").arg(Remote)), Log);
  CHECK_MSG(Log.contains(QStringLiteral("<chmod>")) && Log.contains(QStringLiteral("<permissions value=\"rw-r-----\" />")), Log);
  // synchronize: what it does, and what it would do
  QDir().mkpath(Local + QStringLiteral("/syncxml"));
  QDir().mkpath(Remote + QStringLiteral("/syncxml"));
  WriteText(Local + QStringLiteral("/syncxml/s.txt"), "sync");
  QString Xml2 = ScratchDir + QStringLiteral("/preview.xml");
  R = Script({QStringLiteral("synchronize remote -preview syncxml syncxml")}, {QStringLiteral("/xmllog=") + Xml2});
  CHECK_MSG((R.ExitCode == 0) && !QFileInfo::exists(Remote + QStringLiteral("/syncxml/s.txt")), R.Output);
  Log = QString::fromUtf8(ReadAll(Xml2));
  CHECK_MSG(Log.contains(QStringLiteral("<difference>")) && Log.contains(QStringLiteral("<action value=\"uploadnew\" />")) &&
    Log.contains(QStringLiteral("s.txt")), Log);
  QString Xml3 = ScratchDir + QStringLiteral("/sync.xml");
  R = Script({QStringLiteral("synchronize remote syncxml syncxml")}, {QStringLiteral("/xmllog=") + Xml3});
  CHECK_MSG(R.ExitCode == 0, R.Output);
  Log = QString::fromUtf8(ReadAll(Xml3));
  CHECK_MSG(Log.contains(QStringLiteral("<upload>")) && Log.contains(QStringLiteral("syncxml/s.txt")), Log);
}

// /copyid and the copyid command, on a server with a scratch home directory
// (never the ~/.ssh of the real one)
static void TestCopyId(const QString & Host, const QString & User, const QString & Key)
{
  QByteArray Port = qgetenv("NIXSCP_TEST_SSH_CA_PORT");
  QString Home = qEnvironmentVariable("NIXSCP_TEST_SSH_CA_HOME");
  if (Port.isEmpty() || Home.isEmpty() || QStandardPaths::findExecutable(QStringLiteral("ssh-keygen")).isEmpty())
  {
    std::printf("copyid SKIPPED (set NIXSCP_TEST_SSH_CA_PORT, NIXSCP_TEST_SSH_CA_HOME)\n");
    return;
  }
  Section("server: /copyid");
  QString Identity = ScratchDir + QStringLiteral("/id_copy");
  QProcess Gen;
  Gen.start(QStringLiteral("ssh-keygen"), {QStringLiteral("-q"), QStringLiteral("-t"), QStringLiteral("ed25519"), QStringLiteral("-N"), QString(),
    QStringLiteral("-C"), QStringLiteral("copyid comment"), QStringLiteral("-f"), Identity});
  CHECK(Gen.waitForFinished(30000) && (Gen.exitCode() == 0));
  QByteArray PublicKey = ReadAll(Identity + QStringLiteral(".pub")).trimmed();
  QString AuthorizedKeys = Home + QStringLiteral("/.ssh/authorized_keys");
  QDir(Home + QStringLiteral("/.ssh")).removeRecursively();
  QString Url = QStringLiteral("sftp://%1@%2:%3/").arg(User, Host, QString::fromLatin1(Port));
  RunResult R = RunCli({QStringLiteral("/ini=nul"), QStringLiteral("/copyid"), Url, QStringLiteral("/privatekey=") + Key, QStringLiteral("/hostkey=*")});
  CHECK_MSG((R.ExitCode == 1) && R.Output.contains(QStringLiteral("Identity/key file was not specified.")), R.Output);
  R = RunCli({QStringLiteral("/ini=nul"), QStringLiteral("/copyid"), QStringLiteral("/identity=") + Identity, Url,
    QStringLiteral("/privatekey=") + Key, QStringLiteral("/hostkey=*")});
  CHECK_MSG(R.ExitCode == 0, R.Output);
  CHECK_MSG(ReadAll(AuthorizedKeys).contains(PublicKey), R.Output);
  // the command, and the key of a second run is not added again
  R = RunCli({QStringLiteral("/ini=nul"), QStringLiteral("/command"),
    QStringLiteral("open %1 -privatekey=\"%2\" -hostkey=*").arg(Url, Key),
    QStringLiteral("copyid \"%1\"").arg(Identity), QStringLiteral("exit")});
  CHECK_MSG(R.ExitCode == 0, R.Output);
  CHECK_EQ(ReadAll(AuthorizedKeys).count(PublicKey), 1);
  QDir(Home + QStringLiteral("/.ssh")).removeRecursively();
}

static void TestServer(const QString & Host, int Port, const QString & User, const QString & Key)
{
  Section("server: fingerprint scan");
  QString Url = QStringLiteral("sftp://%1@%2:%3/").arg(User, Host).arg(Port);
  RunResult R = RunCli({QStringLiteral("/ini=nul"), QStringLiteral("/fingerprintscan"), Url});
  QRegularExpressionMatch M = QRegularExpression(QStringLiteral("SHA-256: (\\S+ \\d+ \\S+)\n")).match(R.Output);
  CHECK_MSG((R.ExitCode == 0) && M.hasMatch() && R.Output.contains(QStringLiteral("MD5:     ")), R.Output);
  QString Fingerprint = M.captured(1);
  QFile::remove(ScratchDir + QStringLiteral("/known_hosts"));
  CHECK(!QFileInfo::exists(ScratchDir + QStringLiteral("/known_hosts")) || (QFileInfo(ScratchDir + QStringLiteral("/known_hosts")).size() == 0));

  // several fingerprints, one matching; raw HostKey only orders algorithms
  QString Open = QStringLiteral("open %1 -privatekey=\"%2\" -hostkey=\"ssh-ed25519 255 AAAAshZP/98IzuhyeJScnasKJpGTQS8VxhfaqpVwPm8;%3\" "
    "-rawsettings HostKey=ed25519,ecdsa,rsa,WARN").arg(Url, Key, Fingerprint);

  TestScriptFixes(Url, Key, Fingerprint);
  TestCopyId(Host, User, Key);

  QString Remote = ScratchDir + QStringLiteral("/remote");
  QString Local = ScratchDir + QStringLiteral("/local");
  QDir().mkpath(Remote + QStringLiteral("/dir1/sub"));
  QDir().mkpath(Local + QStringLiteral("/up1/deep"));
  WriteText(Remote + QStringLiteral("/dir1/a.txt"), "alpha\n");
  WriteText(Remote + QStringLiteral("/dir1/sub/b.txt"), "beta\n");
  WriteText(Remote + QStringLiteral("/f.txt"), "data\n");
  WriteText(Local + QStringLiteral("/up1/c.txt"), "gamma\n");
  WriteText(Local + QStringLiteral("/up1/deep/d.txt"), "delta\n");
  QString Xml = ScratchDir + QStringLiteral("/session.xml");

  Section("server: directory under a new name, cp, checksum, call");
  R = RunCli({QStringLiteral("/ini=nul"), QStringLiteral("/xmllog=") + Xml, QStringLiteral("/xmlgroups"), QStringLiteral("/command"), Open,
    QStringLiteral("cd \"%1\"").arg(Remote),
    QStringLiteral("lcd \"%1\"").arg(Local),
    QStringLiteral("get dir1 dir2"),
    QStringLiteral("put up1 up2"),
    QStringLiteral("cp f.txt g.txt"),
    QStringLiteral("checksum sha-256 f.txt"),
    QStringLiteral("checksum md5 f.txt"),
    QStringLiteral("call sh -c 'exit 1'"),
    QStringLiteral("call echo out"),
    QStringLiteral("exit")});
  CHECK_MSG(R.ExitCode == 0, R.Output);
  CHECK_EQ(ReadAll(Local + QStringLiteral("/dir2/a.txt")), QByteArray("alpha\n"));
  CHECK_EQ(ReadAll(Local + QStringLiteral("/dir2/sub/b.txt")), QByteArray("beta\n"));
  CHECK(!QFileInfo::exists(Local + QStringLiteral("/dir1")));
  CHECK_EQ(ReadAll(Remote + QStringLiteral("/up2/c.txt")), QByteArray("gamma\n"));
  CHECK_EQ(ReadAll(Remote + QStringLiteral("/up2/deep/d.txt")), QByteArray("delta\n"));
  CHECK(!QFileInfo::exists(Remote + QStringLiteral("/up1")));
  CHECK_EQ(ReadAll(Remote + QStringLiteral("/g.txt")), QByteArray("data\n"));
  CHECK_MSG(R.Output.contains(QStringLiteral("6667b2d1aab6a00caa5aee5af8ad9f1465e567abf1c209d15727d57b3e8f6e5f f.txt\n")) &&
    R.Output.contains(QStringLiteral("6137cde4893c59f76f005a8123d8e8e6 f.txt\n")) && R.Output.contains(QStringLiteral("out\n")) &&
    R.Output.contains(Remote + QStringLiteral("/f.txt\n")), R.Output);
  QString XmlText = QString::fromUtf8(ReadAll(Xml));
  CHECK_MSG(XmlText.contains(QStringLiteral("<group name=\"get dir1 dir2\"")) && XmlText.contains(QStringLiteral("<download>")) &&
    XmlText.contains(QStringLiteral("<upload>")) && XmlText.contains(QStringLiteral("<cp>")) &&
    XmlText.contains(QStringLiteral("<checksum value=\"6137cde4893c59f76f005a8123d8e8e6\" />")) &&
    XmlText.contains(QStringLiteral("<exitcode value=\"1\" />")) && XmlText.contains(QStringLiteral("<output value=\"out\" />")) &&
    XmlText.contains(QStringLiteral("<result success=\"true\" />")) && XmlText.endsWith(QStringLiteral("</session>\n")) &&
    !XmlText.contains(QStringLiteral("name=\"nosession\"")), XmlText);

  // cp and checksum quote names with spaces and shell metacharacters
  // (fsCopyFile "cp -p -r -f %s \"%s\" \"%s\"")
  WriteText(Remote + QStringLiteral("/s p$HOME.txt"), "data\n");
  R = RunCli({QStringLiteral("/ini=nul"), QStringLiteral("/command"), Open,
    QStringLiteral("cd \"%1\"").arg(Remote),
    QStringLiteral("cp \"s p$HOME.txt\" \"c  p.txt\""),
    QStringLiteral("checksum md5 \"s p$HOME.txt\""),
    QStringLiteral("exit")});
  CHECK_MSG(R.ExitCode == 0, R.Output);
  CHECK_EQ(ReadAll(Remote + QStringLiteral("/c  p.txt")), QByteArray("data\n"));
  CHECK_MSG(R.Output.contains(QStringLiteral("6137cde4893c59f76f005a8123d8e8e6 s p$HOME.txt\n")), R.Output);

  // call: exit code 2 with error output fails (COMMAND_FAILED2); batch abort stops
  // (the code of ls itself is 1 in BSD)
  R = RunCli({QStringLiteral("/ini=nul"), QStringLiteral("/command"), Open,
    QStringLiteral("call ls /nonexistent-nixscp-path; exit 2"), QStringLiteral("echo after"), QStringLiteral("exit")});
  CHECK_MSG((R.ExitCode == 1) && R.Output.contains(QStringLiteral("Command 'ls /nonexistent-nixscp-path; exit 2'\nfailed with return code 2 and the following error message.")) &&
    !R.Output.contains(QStringLiteral("after")), R.Output);
  // exit code 1 is an error with ExitCode1IsError
  R = RunCli({QStringLiteral("/ini=nul"), QStringLiteral("/command"), Open + QStringLiteral(" ExitCode1IsError=1"),
    QStringLiteral("call sh -c 'exit 1'"), QStringLiteral("exit")});
  CHECK_MSG((R.ExitCode == 1) && R.Output.contains(QStringLiteral("Command failed with return code 1.")), R.Output);

  Section("server: streaming");
  R = RunCli({QStringLiteral("/ini=nul"), QStringLiteral("/stdout"), QStringLiteral("/command"), Open,
    QStringLiteral("get \"%1/f.txt\" -").arg(Remote), QStringLiteral("exit")}, QByteArray(), false);
  CHECK_MSG((R.ExitCode == 0) && (R.StdOut == QByteArray("data\n")) && R.Output.contains(QStringLiteral("Session started.")), R.Output + QString::fromLatin1(R.StdOut));
  R = RunCli({QStringLiteral("/ini=nul"), QStringLiteral("/stdout=chunked"), QStringLiteral("/command"), Open,
    QStringLiteral("get \"%1/f.txt\" -").arg(Remote), QStringLiteral("exit")}, QByteArray(), false);
  CHECK_MSG((R.ExitCode == 0) && (R.StdOut == QByteArray("5\r\ndata\n\r\n0\r\n\r\n")), QString::fromLatin1(R.StdOut));
  // "put - target" reads stdin with /stdin
  R = RunCli({QStringLiteral("/ini=nul"), QStringLiteral("/stdin"), QStringLiteral("/command"), Open,
    QStringLiteral("put - \"%1/streamed.txt\"").arg(Remote), QStringLiteral("exit")}, QByteArray("from stdin\n"));
  CHECK_MSG(R.ExitCode == 0, R.Output);
  CHECK_EQ(ReadAll(Remote + QStringLiteral("/streamed.txt")), QByteArray("from stdin\n"));
  R = RunCli({QStringLiteral("/ini=nul"), QStringLiteral("/stdin"), QStringLiteral("/command"), Open,
    QStringLiteral("put - \"%1/\"").arg(Remote), QStringLiteral("exit")}, QByteArray("x"));
  CHECK_MSG((R.ExitCode == 1) && R.Output.contains(QStringLiteral("When uploading streamed data, only one source can be specified")), R.Output);

  Section("server: rm into the remote recycle bin, option reconnecttime");
  QDir().mkpath(Remote + QStringLiteral("/bin"));
  WriteText(Remote + QStringLiteral("/trashme.txt"), "x\n");
  R = RunCli({QStringLiteral("/ini=nul"), QStringLiteral("/command"),
    Open + QStringLiteral(" DeleteToRecycleBin=1 RecycleBinPath=\"%1/bin\"").arg(Remote),
    QStringLiteral("rm \"%1/trashme.txt\"").arg(Remote),
    QStringLiteral("option reconnecttime 30"),
    QStringLiteral("option reconnecttime off"),
    QStringLiteral("exit")});
  CHECK_MSG((R.ExitCode == 0) && R.Output.contains(Remote + QStringLiteral("/trashme.txt\n")) &&
    R.Output.contains(QStringLiteral("reconnecttime   30")) && R.Output.contains(QStringLiteral("reconnecttime   off")), R.Output);
  CHECK(!QFileInfo::exists(Remote + QStringLiteral("/trashme.txt")));
  CHECK_MSG(!QDir(Remote + QStringLiteral("/bin")).entryList(QDir::Files).isEmpty(), QDir(Remote + QStringLiteral("/bin")).entryList().join(QLatin1Char(' ')));
}

int main(int argc, char ** argv)
{
  QCoreApplication App(argc, argv);
  QTemporaryDir Scratch;
  CHECK(Scratch.isValid());
  ScratchDir = QDir(Scratch.path()).canonicalPath();
  QDir(ScratchDir).mkpath(QStringLiteral("xdg"));

  TestSessionSwitches();
  TestHelpers();

  CliPath = qEnvironmentVariable("NIXSCP_CLI");
  if (CliPath.isEmpty())
  {
    CliPath = QCoreApplication::applicationDirPath() + QStringLiteral("/nixscp-cli");
  }
  if (!QFileInfo(CliPath).isExecutable())
  {
    std::printf("test_cliscripting: nixscp-cli not found at %s, cli tests SKIPPED (set NIXSCP_CLI)\n", qPrintable(CliPath));
  }
  else
  {
    TestCliModes();
    TestKeyGen();
    QByteArray PortStr = qgetenv("NIXSCP_TEST_SSH_PORT");
    QString Key = qEnvironmentVariable("NIXSCP_TEST_SSH_KEY");
    QString Host = qEnvironmentVariable("NIXSCP_TEST_SSH_HOST", QStringLiteral("127.0.0.1"));
    if (PortStr.isEmpty() || Key.isEmpty())
    {
      std::printf("test_cliscripting: server tests SKIPPED (set NIXSCP_TEST_SSH_PORT, NIXSCP_TEST_SSH_KEY)\n");
    }
    else if ((Host != QStringLiteral("127.0.0.1")) && (Host != QStringLiteral("localhost")))
    {
      std::printf("test_cliscripting: server tests SKIPPED (server must run on this machine)\n");
    }
    else
    {
      TestServer(Host, PortStr.toInt(), qEnvironmentVariable("NIXSCP_TEST_SSH_USER", qEnvironmentVariable("USER")), Key);
    }
  }

  if (Failures > 0)
  {
    std::printf("test_cliscripting: %d FAILURES\n", Failures);
    return 1;
  }
  std::printf("test_cliscripting: all tests passed\n");
  return 0;
}
