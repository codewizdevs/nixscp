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
#include <QTemporaryDir>

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
  R = RunCli({QStringLiteral("/ini=nul"), QStringLiteral("/loglevel=7"), QStringLiteral("/command"), QStringLiteral("exit")});
  CHECK_MSG((R.ExitCode == 1) && R.Output.contains(QStringLiteral("Unknown value '7' of option 'loglevel'.")), R.Output);

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

// ------------------------------------------------------------------ server

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
  R = RunCli({QStringLiteral("/ini=nul"), QStringLiteral("/command"), Open,
    QStringLiteral("call ls /nonexistent-nixscp-path"), QStringLiteral("echo after"), QStringLiteral("exit")});
  CHECK_MSG((R.ExitCode == 1) && R.Output.contains(QStringLiteral("Command 'ls /nonexistent-nixscp-path'\nfailed with return code 2 and the following error message.")) &&
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
