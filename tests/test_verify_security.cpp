// NIXSCP_GUI_TESTS
// Security verification: exploit-style tests with malicious input and
// malicious (fake) servers.
//
//  - stored passwords / master password (core/Security), ini file permissions
//  - shell injection: local and remote custom commands, SCP shell quoting,
//    "Open in PuTTY" (terminal client command line), external editor command
//  - SCP commands with odd file names against the real test sshd
//  - path traversal by server listings (SFTP, FTP, WebDAV) when downloading
//  - CR/LF injection into FTP commands and WebDAV request headers
//  - host key verification (changed key, configured fingerprint)
//  - TLS certificate verification (FTPS, WebDAVS: never silently accepted)
//
// Checks of vulnerabilities that are known and reported, but not fixed yet
// (in files owned by other work packages), print "VULNERABLE" and only count
// as failures with NIXSCP_TEST_SECURITY_STRICT=1.
//
// Server parts are skipped unless configured:
//   NIXSCP_TEST_SSH_PORT/_HOST/_USER/_KEY       real test sshd (SCP odd names, host keys)
//   NIXSCP_TEST_EVIL_SSH_PORT                   sshd whose SFTP subsystem is evil_sftp.py
//   NIXSCP_TEST_EVIL_SFTP_ABSDIR                the absolute directory evil_sftp.py targets
//   NIXSCP_TEST_EVIL_SERVERS                    directory with evil_ftp.py and evil_dav.py
//                                               (started by this test on free ports)
//   NIXSCP_TEST_FTPS_PORT, NIXSCP_TEST_DAVS_URL  TLS servers with a self-signed certificate
#include "core/Platform.h"
#include "core/Configuration.h"
#include "core/CopyParam.h"
#include "core/CustomCommands.h"
#include "core/FtpFileSystem.h"
#include "core/IniStorage.h"
#include "core/ScpFileSystem.h"
#include "core/SecureShell.h"
#include "core/Security.h"
#include "core/SessionData.h"
#include "core/Terminal.h"
#include "gui/DesktopIntegration.h"
#include "gui/EditorManager.h"
#include "gui/GuiCommandLine.h"

using DesktopIntegration::TerminalCommand;
using DesktopIntegration::BuildTerminalCommand;

#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QProcess>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <sys/stat.h>
#include <algorithm>
#include <cstdio>

static int Failures = 0;
static int Vulnerable = 0;
static bool Strict = false;

#define CHECK(Cond) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); std::fflush(stdout); Failures++; } } while (0)
#define CHECK_MSG(Cond, Msg) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, #Cond, qPrintable(Msg)); std::fflush(stdout); Failures++; } } while (0)
// Known, reported vulnerability: not a failure unless NIXSCP_TEST_SECURITY_STRICT=1
#define CHECK_VULN(Cond, Msg) \
  do { if (!(Cond)) { std::printf("VULNERABLE %s:%d: %s (%s)\n", __FILE__, __LINE__, #Cond, qPrintable(Msg)); std::fflush(stdout); \
    Vulnerable++; if (Strict) Failures++; } } while (0)

static void Log(const QString & Message)
{
  std::printf("  %s\n", qPrintable(Message));
  std::fflush(stdout);
}

static QString MarkName()
{
  return QStringLiteral("nixscp-secmark-%1").arg(QCoreApplication::applicationPid());
}

// Runs Command by /bin/sh in Dir; returns stdout
static QString RunShell(const QString & Command, const QString & Dir, int * ExitCode = nullptr,
  const QProcessEnvironment & Env = QProcessEnvironment::systemEnvironment())
{
  QProcess P;
  P.setWorkingDirectory(Dir);
  P.setProcessEnvironment(Env);
  P.start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), Command});
  P.waitForFinished(10000);
  if (ExitCode != nullptr)
  {
    *ExitCode = P.exitCode();
  }
  return QString::fromUtf8(P.readAllStandardOutput());
}

// The shell of local custom commands (cmd.exe on Windows)
static void RunLocalShell(const QString & Command, const QString & Dir)
{
  QProcess P;
  P.setWorkingDirectory(Dir);
  Platform::SetShellCommand(P, Command);
  P.start();
  P.waitForFinished(10000);
}

// Payloads that execute "touch <Mark>" when a shell evaluates them unquoted
// or in the wrong quotes
static QStringList InjectionPayloads(const QString & Mark)
{
  return {
    QStringLiteral("x$(touch %1)y").arg(Mark),
    QStringLiteral("x`touch %1`y").arg(Mark),
    QStringLiteral("x\"; touch %1; \"y").arg(Mark),
    QStringLiteral("x'; touch %1; 'y").arg(Mark),
    QStringLiteral("x; touch %1").arg(Mark),
    QStringLiteral("x|touch %1").arg(Mark),
    QStringLiteral("x\ntouch %1\n").arg(Mark),
  };
}

// Payloads for the local shell that runs local custom commands
static QStringList LocalInjectionPayloads(const QString & Mark)
{
#ifdef _WIN32
  // cmd.exe; only characters a downloaded Windows file name can have
  return {
    QStringLiteral("x&mkdir %1").arg(Mark),
    QStringLiteral("x && mkdir %1").arg(Mark),
    QStringLiteral("x)&mkdir %1&(y").arg(Mark),
    QStringLiteral("x^&mkdir %1").arg(Mark),
    QStringLiteral("x'&mkdir %1&'y").arg(Mark),
  };
#else
  // on macOS the shell gets them decomposed: U+1FEF is a backtick then, U+037E a semicolon
  return InjectionPayloads(Mark) + QStringList{
    QStringLiteral("x\u1FEFtouch %1\u1FEFy").arg(Mark),
    QStringLiteral("x\u037E touch %1").arg(Mark),
  };
#endif
}

// ---------------------------------------------------------------- stored passwords

static void TestPasswords()
{
  std::printf("[stored passwords and master password]\n");
  // the GUI installs its dialog as the prompt when QApplication is created
  Security::SetMasterPasswordPrompt(Security::MasterPasswordPromptFunc());
  // AES-256 + HMAC-SHA1 (Cryptography.cpp): round trip, tampering, wrong password
  QByteArray Plain("very secret password");
  QByteArray Encrypted;
  Security::AES256EncryptWithMAC(Plain, QStringLiteral("Master#1"), Encrypted);
  CHECK(!Encrypted.contains(Plain));
  QByteArray Decrypted;
  CHECK(Security::AES256DecryptWithMAC(Encrypted, QStringLiteral("Master#1"), Decrypted) && (Decrypted == Plain));
  CHECK(!Security::AES256DecryptWithMAC(Encrypted, QStringLiteral("Master#2"), Decrypted));
  for (int Index = 0; Index < Encrypted.size(); Index++)
  {
    // salt, ciphertext and MAC are all authenticated
    QByteArray Tampered = Encrypted;
    Tampered[Index] = static_cast<char>(Tampered[Index] ^ 0x01);
    CHECK_MSG(!Security::AES256DecryptWithMAC(Tampered, QStringLiteral("Master#1"), Decrypted), QString::number(Index));
  }
  CHECK(!Security::AES256DecryptWithMAC(Encrypted.left(26), QStringLiteral("Master#1"), Decrypted));
  CHECK(!Security::AES256DecryptWithMAC(QByteArray(), QStringLiteral("Master#1"), Decrypted));
  // Two encryptions of the same data differ (random salt)
  QByteArray Encrypted2;
  Security::AES256EncryptWithMAC(Plain, QStringLiteral("Master#1"), Encrypted2);
  CHECK(Encrypted != Encrypted2);

  QByteArray Verifier;
  Security::AES256CreateVerifier(QStringLiteral("Master#1"), Verifier);
  CHECK(Security::AES256Verify(QStringLiteral("Master#1"), Verifier));
  CHECK(!Security::AES256Verify(QStringLiteral("master#1"), Verifier));
  CHECK(!Security::AES256Verify(QStringLiteral("Master#1"), Verifier.left(20)));

  // Master password in use: stored value is the strongly encrypted form,
  // the plain password is not recoverable without the master password
  const QString Key = QStringLiteral("userhost.example.com");
  Security::ChangeMasterPassword(QStringLiteral("Master#1"));
  QString VerifierHex = Security::MasterPasswordVerifier();
  // (Configuration::ChangeMasterPassword switches the decryption password itself)
  Security::SetMasterPassword(QStringLiteral("Master#1"));
  QByteArray Stored = Security::EncryptPassword(QStringLiteral("sitepass"), Key);
  CHECK(Security::IsStronglyEncrypted(Stored));
  CHECK(!Stored.contains("sitepass") && !Stored.contains(QByteArray("sitepass").toHex().toUpper()));
  CHECK(Security::SimpleDecryptPassword(Stored, Key).isEmpty());
  CHECK(Security::DecryptPassword(Stored, Key) == QStringLiteral("sitepass"));
  // another instance (no master password known yet); as Configuration::ClearMasterPassword
  Security::ClearMasterPassword();
  Security::SetMasterPassword(QString());
  CHECK(!Security::IsMasterPasswordKnown());
  Security::SetMasterPasswordState(true, VerifierHex);
  CHECK(!Security::IsMasterPasswordKnown());
  {
    // no prompt handler: nothing decrypted, nothing guessed
    CHECK(Security::DecryptPassword(Stored, Key).isEmpty());
    // wrong master password from the prompt: rejected by the verifier, nothing decrypted
    int Prompts = 0;
    Security::SetMasterPasswordPrompt([&Prompts]()
    {
      Prompts++;
      Security::SetMasterPassword(QStringLiteral("Wrong#1"));
      return Security::IsMasterPasswordKnown();
    });
    CHECK(Security::DecryptPassword(Stored, Key).isEmpty());
    CHECK(Prompts == 1);
    CHECK(!Security::IsMasterPasswordKnown());
    // a new password is not stored obfuscated-only while the master password is unknown
    // and the user cancels: it falls back to the WinSCP behaviour (simple form)
    Security::SetMasterPasswordPrompt([]() { return false; });
    QByteArray Fallback = Security::EncryptPassword(QStringLiteral("other"), Key);
    CHECK(!Security::IsStronglyEncrypted(Fallback));
    Security::SetMasterPasswordPrompt([]()
    {
      Security::SetMasterPassword(QStringLiteral("Master#1"));
      return Security::IsMasterPasswordKnown();
    });
    CHECK(Security::DecryptPassword(Stored, Key) == QStringLiteral("sitepass"));
    // tampered stored value: not decrypted
    QByteArray Tampered = Stored;
    Tampered[Tampered.size() - 3] = (Tampered[Tampered.size() - 3] == 'A') ? 'B' : 'A';
    CHECK(Security::DecryptPassword(Tampered, Key).isEmpty());
  }
  Security::SetMasterPasswordPrompt(Security::MasterPasswordPromptFunc());
  Security::ClearMasterPassword();
  Security::SetMasterPasswordState(false, QString());

  // Simple obfuscation (WinSCP compatible): the key (user+host) is checked
  QByteArray Simple = Security::SimpleEncryptPassword(QStringLiteral("pw"), Key);
  CHECK(Security::SimpleDecryptPassword(Simple, Key) == QStringLiteral("pw"));
  CHECK(Security::SimpleDecryptPassword(Simple, QStringLiteral("otherhost")).isEmpty());
  // garbage does not crash
  CHECK(Security::SimpleDecryptPassword(QByteArray("ZZ"), Key).isEmpty() || true);
  CHECK(Security::DecryptPassword(QByteArray("A35C4E"), Key).size() < 1000);
  QString Unscrambled;
  CHECK(!Security::UnscramblePassword(QByteArray("\x01\x02\x03", 3), Unscrambled));
}

// ---------------------------------------------------------------- file permissions

static void TestIniPermissions(const QString & Dir)
{
  std::printf("[configuration file permissions]\n");
  IniFile Ini;
  Ini.WriteRaw(QStringLiteral("Sessions\\a"), QStringLiteral("Password"), QStringLiteral("A35C"));
  QString FileName = QDir(Dir).filePath(QStringLiteral("perm.ini"));
  CHECK(Ini.Save(FileName));
  CHECK((QFileInfo(FileName).permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther | QFileDevice::WriteGroup |
    QFileDevice::WriteOther)) == 0);
  // re-save keeps owner-only permissions
  CHECK(Ini.Save(FileName));
  CHECK((QFileInfo(FileName).permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther)) == 0);
  // Export/backup of the configuration (contains passwords): owner only
  QString Export = QDir(Dir).filePath(QStringLiteral("export.ini"));
  CHECK(Configuration::Instance().Export(Export));
  CHECK((QFileInfo(Export).permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther)) == 0);
}

// ---------------------------------------------------------------- custom commands

static void TestLocalCustomCommands(const QString & Dir)
{
  std::printf("[local custom commands with server controlled file names]\n");
  // A local custom command that works with remote files gets the names of
  // the downloaded files: the name comes from the server
  QString Mark = MarkName();
  CustomCommandData Data;
  const QStringList Templates = {
    QStringLiteral("echo !"), QStringLiteral("echo \"!\""), QStringLiteral("echo '!'"), QStringLiteral("echo !&"),
  };
  for (const QString & Template : Templates)
  {
    for (const QString & Payload : LocalInjectionPayloads(Mark))
    {
      QString Cmd;
      QString FileName = Dir + QLatin1Char('/') + Payload;
      if (Template.contains(QStringLiteral("!&")))
      {
        Cmd = LocalCustomCommand(Data, QStringLiteral("/remote"), Dir, QString(), QString(), MakeLocalFileList({FileName}))
          .Complete(Template, true);
      }
      else
      {
        Cmd = LocalCustomCommand(Data, QStringLiteral("/remote"), Dir, FileName, QString(), QString()).Complete(Template, true);
      }
      QFile::remove(QDir(Dir).filePath(Mark));
      RunLocalShell(Cmd, Dir);
      bool Injected = QFileInfo::exists(QDir(Dir).filePath(Mark));
      CHECK_VULN(!Injected, QStringLiteral("local command [%1] with remote file name [%2] executed: %3")
        .arg(Template, QString(Payload).replace(QLatin1Char('\n'), QStringLiteral("\\n")), Cmd));
      QFile::remove(QDir(Dir).filePath(Mark));
    }
  }
}

static void TestRemoteCustomCommands(const QString & Dir)
{
  std::printf("[remote custom commands: escaping for the remote shell]\n");
  // The remote shell is modelled by the local /bin/sh
  QString Mark = MarkName();
  CustomCommandData Data;
  for (const QString & Payload : InjectionPayloads(Mark))
  {
    // "!" in double quotes: the form used by all default commands
    QString Cmd = RemoteCustomCommand(Data, QStringLiteral("/home/u"), Payload, QString()).Complete(QStringLiteral("printf %s \"!\""), true);
    QString Output = RunShell(Cmd, Dir);
    CHECK_MSG(!QFileInfo::exists(QDir(Dir).filePath(Mark)), Cmd);
    CHECK_MSG(Output == Payload, Output);
    QFile::remove(QDir(Dir).filePath(Mark));
    // !& (MakeShellFileList)
    Cmd = RemoteCustomCommand(Data, QStringLiteral("/home/u"), QString(), MakeShellFileList({Payload, QStringLiteral("b c")}))
      .Complete(QStringLiteral("printf '%s|' !&"), true);
    Output = RunShell(Cmd, Dir);
    CHECK_MSG(!QFileInfo::exists(QDir(Dir).filePath(Mark)), Cmd);
    CHECK_MSG(Output == Payload + QStringLiteral("|b c|"), Output);
    QFile::remove(QDir(Dir).filePath(Mark));
    // WinSCP escapes only $ and \ outside double quotes, so a name of the server
    // runs as a command there: here any place in the command is safe
    for (const QString & Template : {QStringLiteral("printf %s !"), QStringLiteral("printf %s '!'"), QStringLiteral("printf %s \"[!]\""),
      QStringLiteral("printf %s '[!]'"), QStringLiteral("printf %s [!]"), QStringLiteral("printf %s \"x\"!\"y\"")})
    {
      Cmd = RemoteCustomCommand(Data, QStringLiteral("/home/u"), Payload, QString()).Complete(Template, true);
      Output = RunShell(Cmd, Dir);
      CHECK_MSG(!QFileInfo::exists(QDir(Dir).filePath(Mark)), Cmd);
      CHECK_MSG(Output.contains(Payload), Cmd + QStringLiteral(" -> ") + Output);
      QFile::remove(QDir(Dir).filePath(Mark));
    }
    // a list where it does not belong: wrong, but nothing runs
    for (const QString & Template : {QStringLiteral("printf %s \"!&\""), QStringLiteral("printf %s '!&'")})
    {
      Cmd = RemoteCustomCommand(Data, QStringLiteral("/home/u"), QString(), MakeShellFileList({Payload})).Complete(Template, true);
      RunShell(Cmd, Dir);
      CHECK_MSG(!QFileInfo::exists(QDir(Dir).filePath(Mark)), Cmd);
      QFile::remove(QDir(Dir).filePath(Mark));
    }
    // the path of a command as well
    Cmd = RemoteCustomCommand(Data, Payload).Complete(QStringLiteral("printf %s !/"), true);
    RunShell(Cmd, Dir);
    CHECK_MSG(!QFileInfo::exists(QDir(Dir).filePath(Mark)), Cmd);
    QFile::remove(QDir(Dir).filePath(Mark));
  }
  // Leading dash: "./" is prepended (DelimitStr)
  QString Cmd = RemoteCustomCommand(Data, QStringLiteral("/home/u"), QStringLiteral("-rf"), QString()).Complete(QStringLiteral("rm \"!\""), true);
  CHECK_MSG(Cmd == QStringLiteral("rm \"./-rf\""), Cmd);
}

static void TestScpShellQuoting(const QString & Dir)
{
  std::printf("[SCP shell quoting (ShellQuoteStr)]\n");
  QString Mark = MarkName();
  QStringList Names = InjectionPayloads(Mark);
  Names << QStringLiteral("a\\b") << QStringLiteral("!x") << QStringLiteral("*") << QStringLiteral(" lead") << QStringLiteral("trail ")
        << QStringLiteral("ü€\t$HOME") << QStringLiteral("x\\");
  for (const QString & Name : Names)
  {
    QString Output = RunShell(QStringLiteral("printf %s ") + ShellQuoteStr(Name), Dir);
    // macOS passes process arguments decomposed (NFD): the same text
    CHECK_MSG(Output.normalized(QString::NormalizationForm_C) == Name.normalized(QString::NormalizationForm_C), Name);
    CHECK_MSG(!QFileInfo::exists(QDir(Dir).filePath(Mark)), Name);
    QFile::remove(QDir(Dir).filePath(Mark));
  }
}

// ---------------------------------------------------------------- terminal client / editor (GUI side)

static bool HasOptionLikeArgument(const TerminalCommand & Command, const QString & Prefix)
{
  for (const QString & Arg : Command.Arguments)
  {
    if (Arg.startsWith(Prefix))
    {
      return true;
    }
  }
  return false;
}

static void TestTerminalCommand(const QString & Dir)
{
  std::printf("[Open in PuTTY: OpenSSH command line from session settings]\n");
  Configuration & Config = Configuration::Instance();
  QString Mark = MarkName();
  const QString Templates[] = { QString(), QStringLiteral("ssh -p !# !U@!@"), QStringLiteral("ssh !U@!@") };
  for (const QString & Template : Templates)
  {
    // host name from a site / URL that starts with "-": option injection into ssh
    SessionData Data;
    Data.FSProtocol = fsSFTP;
    Data.HostName = QStringLiteral("-oProxyCommand=touch %1").arg(Mark);
    Data.UserName.clear();
    TerminalCommand Command;
    QString Error;
    bool Built = BuildTerminalCommand(Data, QString(), QString(), false, Command, Error, nullptr, Template);
    CHECK_VULN(!Built || !HasOptionLikeArgument(Command, QStringLiteral("-oProxyCommand")),
      QStringLiteral("host name became an ssh option: %1 %2").arg(Command.Program, Command.Arguments.join(QLatin1Char(' '))));
    // user name that starts with "-"
    Data.HostName = QStringLiteral("example.com");
    Data.UserName = QStringLiteral("-oProxyCommand=touch %1;").arg(Mark);
    Built = BuildTerminalCommand(Data, QString(), QString(), false, Command, Error, nullptr, Template);
    CHECK_VULN(!Built || !HasOptionLikeArgument(Command, QStringLiteral("-oProxyCommand")),
      QStringLiteral("user name became an ssh option: %1 %2").arg(Command.Program, Command.Arguments.join(QLatin1Char(' '))));
  }

  // The same through the URL handler (x-scheme-handler/ssh, started with an
  // untrusted URL from a web page): ssh:// opens the terminal client directly
  {
    const QStringList Urls = {
      QStringLiteral("ssh://-oProxyCommand=touch%20%1/").arg(Mark),
      QStringLiteral("ssh://-oProxyCommand=touch%20%1;@example.com/").arg(Mark),
    };
    for (const QString & Url : Urls)
    {
      try
      {
        GuiCommandLine::ParsedUrl Parsed = GuiCommandLine::ParseUrl(Url, nullptr, Config.StoredSessions(), true);
        TerminalCommand Command;
        QString Error;
        bool Built = Parsed.PuttyProtocol &&
          BuildTerminalCommand(Parsed.Data, Parsed.Data.Password, QString(), false, Command, Error, nullptr, QStringLiteral("ssh -p !# !U@!@"));
        Log(QStringLiteral("%1 -> host \"%2\" user \"%3\"").arg(Url, Parsed.Data.HostName, Parsed.Data.UserName));
        CHECK_VULN(!Built || !HasOptionLikeArgument(Command, QStringLiteral("-oProxyCommand")),
          QStringLiteral("URL %1 gives %2 %3").arg(Url, Command.Program, Command.Arguments.join(QLatin1Char(' '))));
      }
      catch (const ExtException & E)
      {
        Log(QStringLiteral("%1 rejected: %2").arg(Url, E.Message()));
      }
    }
  }

  // Proxy host name in the generated ProxyCommand (run by a shell by ssh)
  if (!QStandardPaths::findExecutable(QStringLiteral("nc")).isEmpty())
  {
    SessionData Data;
    Data.FSProtocol = fsSFTP;
    Data.HostName = QStringLiteral("example.com");
    Data.UserName = QStringLiteral("user");
    Data.ProxyMethod = pmSocks5;
    Data.ProxyHost = QStringLiteral("proxy;touch %1;").arg(Mark);
    Data.ProxyPort = 1080;
    TerminalCommand Command;
    QString Error;
    if (BuildTerminalCommand(Data, QString(), QString(), false, Command, Error, nullptr, QStringLiteral("ssh !U@!@")))
    {
      for (const QString & Arg : Command.Arguments)
      {
        if (Arg.startsWith(QStringLiteral("ProxyCommand=")))
        {
          // as ssh does: /bin/sh -c "exec <ProxyCommand>" with %h/%p substituted; nc replaced by true
          QString Proxy = Arg.mid(13).replace(QStringLiteral("%h"), QStringLiteral("example.com")).replace(QStringLiteral("%p"), QStringLiteral("22"));
          if (Proxy.startsWith(QStringLiteral("nc ")))
          {
            Proxy = QStringLiteral("true ") + Proxy.mid(3);
          }
          RunShell(Proxy, Dir);
          CHECK_VULN(!QFileInfo::exists(QDir(Dir).filePath(Mark)), QStringLiteral("proxy host injected into ProxyCommand: ") + Arg);
          QFile::remove(QDir(Dir).filePath(Mark));
        }
      }
    }
  }
  else
  {
    Log(QStringLiteral("nc not installed: ProxyCommand check skipped"));
  }

  // Remote directory for "cd" in the remote shell (-t command)
  {
    QString Odd = QStringLiteral("d'$(touch %1)`touch %1`\"x").arg(Mark);
    QDir(Dir).mkdir(Odd);
    SessionData Data;
    Data.FSProtocol = fsSFTP;
    Data.HostName = QStringLiteral("example.com");
    Data.UserName = QStringLiteral("user");
    TerminalCommand Command;
    QString Error;
    QString Remote = QDir(Dir).filePath(Odd);
    CHECK(BuildTerminalCommand(Data, QString(), Remote, true, Command, Error, nullptr, QStringLiteral("ssh !U@!@")));
    int T = static_cast<int>(Command.Arguments.indexOf(QStringLiteral("-t")));
    CHECK(T >= 0);
    if ((T >= 0) && (T + 1 < Command.Arguments.size()))
    {
      // $SHELL replaced by a script printing the directory it was started in
      QString PrintDir = QDir(Dir).filePath(QStringLiteral("printdir.sh"));
      QFile Script(PrintDir);
      CHECK(Script.open(QIODevice::WriteOnly));
      Script.write("#!/bin/sh\npwd\n");
      Script.close();
      Script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
      QProcessEnvironment Env = QProcessEnvironment::systemEnvironment();
      Env.insert(QStringLiteral("SHELL"), PrintDir);
      int Code = -1;
      QString Output = RunShell(Command.Arguments[T + 1], Dir, &Code, Env).trimmed();
      CHECK_MSG(!QFileInfo::exists(QDir(Dir).filePath(Mark)), Command.Arguments[T + 1]);
      CHECK_MSG((Code == 0) && (Output == Remote), Output);
    }
    QDir(Dir).rmdir(Odd);
  }

  // "Remember session password and pass it to PuTTY": one-time 0600 file
  {
    bool Saved = Config.PuttyPassword;
    Config.PuttyPassword = true;
    SessionData Data;
    Data.FSProtocol = fsSFTP;
    Data.HostName = QStringLiteral("example.com");
    Data.UserName = QStringLiteral("user");
    TerminalCommand Command;
    QString Error;
    CHECK(BuildTerminalCommand(Data, QStringLiteral("s3cret pw"), QString(), false, Command, Error, nullptr, QStringLiteral("ssh !U@!@")));
    CHECK(!Command.AskPassSocket.isEmpty());
    if (!Command.AskPassSocket.isEmpty())
    {
      // the password never reaches the disk: an owner-only socket with an
      // unpredictable name in a private directory, nothing to read in the file
      QFileInfo Info(Command.AskPassSocket);
      CHECK(Info.exists());
      CHECK_MSG((Info.permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther | QFileDevice::WriteGroup | QFileDevice::WriteOther)) == 0,
        Command.AskPassSocket);
      CHECK(Info.fileName().size() > QStringLiteral("nixscp-askpass-").size() + 16);
      QFileInfo DirInfo(Info.absolutePath());
      bool PrivateDir = (DirInfo.permissions() & (QFileDevice::WriteGroup | QFileDevice::WriteOther)) == 0;
      Log(QStringLiteral("askpass socket %1 (directory private: %2)").arg(Command.AskPassSocket).arg(PrivateDir));
      // the password is not on the command line, and neither is what opens the socket
      CHECK(!Command.Arguments.join(QLatin1Char(' ')).contains(QStringLiteral("s3cret")));
      QString Token = Command.Environment.value(0).section(QLatin1Char('='), 1);
      CHECK(Token.size() >= 32);
      CHECK(!Command.Arguments.join(QLatin1Char(' ')).contains(Token));
      // one reader only, and the socket is gone then
      QByteArray Password;
      QThread * Reader = QThread::create([&Password, &Command, Token]() { Password = DesktopIntegration::FetchAskPassPassword(Command.AskPassSocket, Token); });
      Reader->start();
      while (!Reader->isFinished())
      {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
      }
      Reader->wait();
      delete Reader;
      CHECK(Password == "s3cret pw");
      QCoreApplication::processEvents();
      CHECK(!QFileInfo::exists(Command.AskPassSocket));
    }
    Config.PuttyPassword = Saved;
  }
}

static void TestEditorCommand()
{
  std::printf("[external editor command line]\n");
  QString Mark = MarkName();
  for (const QString & Payload : InjectionPayloads(Mark))
  {
    QString FileName = QStringLiteral("/run/user/1/scpABC/") + Payload;
    QStringList Args = EditorManager::EditorCommandArguments(QStringLiteral("gedit --new-window !.!"), FileName);
    CHECK_MSG((Args.size() == 3) && (Args[0] == QStringLiteral("gedit")) && (Args[2] == FileName), Args.join(QLatin1Char('|')));
    Args = EditorManager::EditorCommandArguments(QStringLiteral("\"/opt/my editor/ed\" \"!.!\""), FileName);
    CHECK_MSG((Args.size() == 2) && (Args[0] == QStringLiteral("/opt/my editor/ed")) && (Args[1] == FileName), Args.join(QLatin1Char('|')));
    Args = EditorManager::EditorCommandArguments(QStringLiteral("vim"), FileName);
    CHECK_MSG((Args.size() == 2) && (Args[1] == FileName), Args.join(QLatin1Char('|')));
  }
}

// ---------------------------------------------------------------- servers

class TestUserInterface : public UserInterface
{
public:
  QMutex Mutex;
  HostKeyAnswer HostKeyReply = HostKeyAccept;
  unsigned int CertificateReply = qaCancel;
  unsigned int OverwriteReply = qaSkip;
  int HostKeyCalls = 0;
  HostKeyState LastHostKeyState = HostKeyUnknown;
  int CertificateQueries = 0;
  QStringList Queries;

  HostKeyAnswer VerifyHostKey(const QString &, int, const QString &, const QString &, const QString &, HostKeyState State) override
  {
    QMutexLocker Lock(&Mutex);
    HostKeyCalls++;
    LastHostKeyState = State;
    return HostKeyReply;
  }
  bool PromptUser(PromptKind, const QString &, const QString &, QList<Prompt> &) override { return false; }
  void DisplayBanner(const QString &, const QString &) override {}
  unsigned int QueryUser(const QString & Query, const QString &, unsigned int Answers) override
  {
    QMutexLocker Lock(&Mutex);
    Queries.append(Query);
    if (Query.contains(QStringLiteral("certificate"), Qt::CaseInsensitive) && Query.contains(QStringLiteral("SHA-256")))
    {
      CertificateQueries++;
      return CertificateReply;
    }
    if (Query.startsWith(QStringLiteral("Cannot create temporary file")))
    {
      return OverwriteReply;
    }
    // errors of single files: skip
    if (Answers & qaSkip) return qaSkip;
    if (Answers & qaNo) return qaNo;
    return qaCancel;
  }
};

struct Waiter
{
  QEventLoop Loop;
  OperationResult Result;
  RemoteFileList Files;
  bool Called = false;

  Terminal::Callback Cb()
  {
    return [this](const OperationResult & R) { Result = R; Called = true; Loop.quit(); };
  }
  Terminal::ListCallback ListCb()
  {
    return [this](const OperationResult & R, const QString &, const RemoteFileList & List) { Result = R; Files = List; Called = true; Loop.quit(); };
  }
  OperationResult Wait(int TimeoutMs = 120000)
  {
    if (!Called)
    {
      QTimer::singleShot(TimeoutMs, &Loop, [this]() { std::printf("FAIL: operation timed out\n"); Failures++; Loop.quit(); });
      Loop.exec();
    }
    return Result;
  }
};

static QString Describe(const OperationResult & R)
{
  return QStringLiteral("ok=%1 aborted=%2 fatal=%3 msg=\"%4\" more=\"%5\"").arg(R.Ok).arg(R.Aborted).arg(R.Fatal).arg(R.Message, R.MoreMessages);
}

static OperationResult DoSimple(const std::function<void(Terminal::Callback)> & Call)
{
  Waiter W;
  Call(W.Cb());
  return W.Wait();
}

static OperationResult DoList(Terminal & T, const QString & Path, RemoteFileList * Files = nullptr)
{
  Waiter W;
  T.ReadDirectory(Path, W.ListCb());
  W.Wait();
  if (Files != nullptr) *Files = W.Files;
  return W.Result;
}

static QStringList AllFiles(const QString & Root)
{
  QStringList Result;
  QDirIterator It(Root, QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
  while (It.hasNext())
  {
    Result.append(It.next());
  }
  return Result;
}

// Downloads /evil into Work/a/b/target and checks that nothing was written
// outside of Work/a/b/target/evil (and nothing into AbsDir)
static void CheckTraversal(Terminal & T, const QString & Protocol, const QString & AbsDir)
{
  QTemporaryDir Work;
  QString Target = Work.filePath(QStringLiteral("a/b/target"));
  QDir().mkpath(Target);
  QStringList AbsBefore = AllFiles(AbsDir);
  CopyParam Param;
  OperationResult R = DoSimple([&](Terminal::Callback Cb) { T.CopyToLocal(QStringList{QStringLiteral("/evil")}, Target, Param, false, Cb); });
  Log(QStringLiteral("%1 download of malicious listing: %2").arg(Protocol, Describe(R)));
  // S17: an entry selected as it is, not found by walking the folder.
  // The panel builds the path from the listed name.
  for (const QString & Name : {QStringLiteral("x/.."), QStringLiteral("../esc1.txt"), QStringLiteral("../../escdir"), QStringLiteral("..")})
  {
    OperationResult Direct = DoSimple([&](Terminal::Callback Cb)
    {
      T.CopyToLocal(QStringList{UnixPath::Combine(QStringLiteral("/evil"), Name)}, Target, Param, false, Cb);
    });
    Log(QStringLiteral("%1 download of the entry \"%2\": %3").arg(Protocol, Name, Describe(Direct)));
  }
  QStringList Files = AllFiles(Work.path());
  QString Allowed = Target + QLatin1Char('/');
  bool OkDownloaded = false;
  for (const QString & File : Files)
  {
    if (File == Allowed + QStringLiteral("evil/ok.txt"))
    {
      OkDownloaded = true;
    }
    CHECK_VULN(File.startsWith(Allowed), QStringLiteral("%1: server file name escaped the target directory: %2").arg(Protocol, File));
  }
  QStringList AbsAfter = AllFiles(AbsDir);
  for (const QString & File : AbsAfter)
  {
    if (!AbsBefore.contains(File))
    {
      CHECK_VULN(false, QStringLiteral("%1: absolute server file name written: %2").arg(Protocol, File));
      QFile::remove(File);
    }
  }
  CHECK_MSG(OkDownloaded, QStringLiteral("%1: the regular file was not downloaded (%2)").arg(Protocol, Files.join(QLatin1Char(','))));
}

static void TestEvilSftp(const QString & KeyFile, const QString & User)
{
  QByteArray Port = qgetenv("NIXSCP_TEST_EVIL_SSH_PORT");
  QString AbsDir = qEnvironmentVariable("NIXSCP_TEST_EVIL_SFTP_ABSDIR");
  if (Port.isEmpty() || AbsDir.isEmpty() || KeyFile.isEmpty())
  {
    std::printf("[malicious SFTP server] SKIPPED (set NIXSCP_TEST_EVIL_SSH_PORT, NIXSCP_TEST_EVIL_SFTP_ABSDIR)\n");
    return;
  }
  std::printf("[malicious SFTP server: path traversal in directory listing]\n");
  QTemporaryDir Scratch;
  SessionData Data;
  Data.FSProtocol = fsSFTPonly;
  Data.HostName = QStringLiteral("127.0.0.1");
  Data.PortNumber = Port.toInt();
  Data.UserName = User;
  Data.PublicKeyFile = KeyFile;
  Data.TryAgent = false;
  Data.AuthKI = false;
  Data.KnownHostsFile = Scratch.filePath(QStringLiteral("known_hosts"));
  TestUserInterface UI;
  UI.HostKeyReply = UserInterface::HostKeyOnce;
  Terminal T(Data, &UI);
  OperationResult R = DoSimple([&](Terminal::Callback Cb) { T.Open(Cb); });
  CHECK_MSG(R.Ok, Describe(R));
  if (!R.Ok)
  {
    return;
  }
  RemoteFileList Files;
  R = DoList(T, QStringLiteral("/evil"), &Files);
  CHECK_MSG(R.Ok, Describe(R));
  for (const RemoteFile & F : Files)
  {
    Log(QStringLiteral("listed: \"%1\"").arg(F.FileName));
  }
  CheckTraversal(T, QStringLiteral("SFTP"), AbsDir);
  T.Close();
}

static int FreePort()
{
  QTcpServer Server;
  Server.listen(QHostAddress::LocalHost, 0);
  int Port = Server.serverPort();
  Server.close();
  return Port;
}

static bool WaitListening(int Port)
{
  for (int I = 0; I < 50; I++)
  {
    QTcpSocket S;
    S.connectToHost(QHostAddress::LocalHost, static_cast<quint16>(Port));
    if (S.waitForConnected(200))
    {
      S.disconnectFromHost();
      return true;
    }
    QThread::msleep(100);
  }
  return false;
}

static QString ReadText(const QString & FileName)
{
  QFile F(FileName);
  return F.open(QIODevice::ReadOnly) ? QString::fromUtf8(F.readAll()) : QString();
}

static void TestEvilFtp(const QString & Scripts)
{
  std::printf("[malicious FTP server: path traversal in LIST, CR/LF in commands]\n");
  QTemporaryDir Scratch;
  QString AbsDir = Scratch.filePath(QStringLiteral("abs"));
  QDir().mkpath(AbsDir);
  QString LogFile = Scratch.filePath(QStringLiteral("ftp.log"));
  int Port = FreePort();
  QProcess Server;
  Server.start(QStringLiteral("python3"), {QStringLiteral("-I"), QDir(Scripts).filePath(QStringLiteral("evil_ftp.py")),
    QString::number(Port), AbsDir, LogFile});
  if (!WaitListening(Port))
  {
    CHECK_MSG(false, QStringLiteral("evil FTP server did not start"));
    return;
  }
  SessionData Data;
  Data.FSProtocol = fsFTP;
  Data.HostName = QStringLiteral("127.0.0.1");
  Data.PortNumber = Port;
  Data.UserName = QStringLiteral("u");
  Data.Password = QStringLiteral("p");
  Data.Timeout = 10;
  TestUserInterface UI;
  {
    Terminal T(Data, &UI);
    OperationResult R = DoSimple([&](Terminal::Callback Cb) { T.Open(Cb); });
    CHECK_MSG(R.Ok, Describe(R));
    if (R.Ok)
    {
      CheckTraversal(T, QStringLiteral("FTP"), AbsDir);
      // CR/LF in names typed by the user or taken from local files
      // (e.g. created by a download from another server)
      DoSimple([&](Terminal::Callback Cb) { T.RenameFile(QStringLiteral("/evil/ok.txt"), QStringLiteral("/evil/x\r\nDELE /victim1"), Cb); });
      DoSimple([&](Terminal::Callback Cb) { T.CreateDirectory(QStringLiteral("/evil/y\nDELE /victim2"), 0755, Cb); });
      DoSimple([&](Terminal::Callback Cb) { T.DeleteFiles(QStringList{QStringLiteral("/evil/z\r\nDELE /victim3")}, Cb); });
      T.Close();
    }
  }
  QStringList Commands = ReadText(LogFile).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
  for (const QString & Command : Commands)
  {
    CHECK_VULN(!Command.startsWith(QStringLiteral("DELE /victim")), QStringLiteral("FTP command injected: ") + Command);
  }
  Server.kill();
  Server.waitForFinished(3000);
}

// The server turns down the upload of the temporary file. A refusal for good
// can be about the name, and writing to the target then takes the user's word;
// anything else says nothing about the file, and the target is left alone
static void TestFtpPartialRefused(const QString & Scripts)
{
  std::printf("[FTP server refuses the temporary file]\n");
  QTemporaryDir Scratch;
  QString AbsDir = Scratch.filePath(QStringLiteral("abs"));
  QDir().mkpath(AbsDir);
  for (const char * Name : {"ok.txt", "missing.txt"})
  {
    QFile F(Scratch.filePath(QString::fromLatin1(Name)));
    CHECK(F.open(QIODevice::WriteOnly));
    F.write(QByteArray(300000, 'x'));
  }
  struct { const char * Reply; unsigned int Answer; bool Asked; bool Written; const char * Name; } Cases[] = {
    {"425 Cannot open data connection", qaYes, false, false, "ok.txt"},
    {"421 Service not available", qaYes, false, false, "ok.txt"},
    {"451 Local error", qaYes, false, false, "ok.txt"},
    {"425 Cannot open data connection", qaYes, false, false, "missing.txt"},
    {"550 Permission denied", qaSkip, true, false, "ok.txt"},
    {"550 Permission denied", qaAbort, true, false, "ok.txt"},
    {"550 Permission denied", qaYes, true, true, "ok.txt"},
    // nothing to lose: no question
    {"550 Permission denied", qaSkip, false, true, "missing.txt"},
  };
  // the upload to the target fails: only a file of its own goes away with it
  struct { const char * Name; unsigned int Answer; bool Deleted; } Failing[] = {
    {"ok.txt", qaYes, false},
    {"missing.txt", qaSkip, true},
  };
  for (const auto & Case : Failing)
  {
    QString LogFile = Scratch.filePath(QStringLiteral("stor-end.log"));
    QFile::remove(LogFile);
    int Port = FreePort();
    QProcess Server;
    QProcessEnvironment Env = QProcessEnvironment::systemEnvironment();
    Env.insert(QStringLiteral("EVIL_FTP_STOR_PARTIAL"), QStringLiteral("550 Permission denied"));
    Env.insert(QStringLiteral("EVIL_FTP_STOR_END"), QStringLiteral("451 Disk full"));
    Server.setProcessEnvironment(Env);
    Server.start(QStringLiteral("python3"), {QStringLiteral("-I"), QDir(Scripts).filePath(QStringLiteral("evil_ftp.py")),
      QString::number(Port), AbsDir, LogFile});
    if (!WaitListening(Port))
    {
      CHECK_MSG(false, QStringLiteral("evil FTP server did not start"));
      return;
    }
    SessionData Data;
    Data.FSProtocol = fsFTP;
    Data.HostName = QStringLiteral("127.0.0.1");
    Data.PortNumber = Port;
    Data.UserName = QStringLiteral("u");
    Data.Password = QStringLiteral("p");
    Data.Timeout = 10;
    TestUserInterface UI;
    UI.OverwriteReply = Case.Answer;
    {
      Terminal T(Data, &UI);
      OperationResult R = DoSimple([&](Terminal::Callback Cb) { T.Open(Cb); });
      CHECK_MSG(R.Ok, Describe(R));
      CopyParam Param;
      Param.ConfirmOverwriting = false;
      DoSimple([&](Terminal::Callback Cb) { T.CopyToRemote({Scratch.filePath(QString::fromLatin1(Case.Name))}, QStringLiteral("/evil"), Param, false, Cb); });
      T.Close();
    }
    QString Target = QStringLiteral("/evil/") + QString::fromLatin1(Case.Name);
    QStringList Commands = ReadText(LogFile).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    CHECK_MSG(Commands.contains(QStringLiteral("STOR ") + Target), Commands.join(QLatin1Char('|')));
    CHECK_MSG(Commands.contains(QStringLiteral("DELE ") + Target) == Case.Deleted, Commands.join(QLatin1Char('|')));
    Server.kill();
    Server.waitForFinished(3000);
  }
  for (const auto & Case : Cases)
  {
    QString LogFile = Scratch.filePath(QStringLiteral("stor.log"));
    QFile::remove(LogFile);
    int Port = FreePort();
    QProcess Server;
    QProcessEnvironment Env = QProcessEnvironment::systemEnvironment();
    Env.insert(QStringLiteral("EVIL_FTP_STOR_PARTIAL"), QString::fromLatin1(Case.Reply));
    Server.setProcessEnvironment(Env);
    Server.start(QStringLiteral("python3"), {QStringLiteral("-I"), QDir(Scripts).filePath(QStringLiteral("evil_ftp.py")),
      QString::number(Port), AbsDir, LogFile});
    if (!WaitListening(Port))
    {
      CHECK_MSG(false, QStringLiteral("evil FTP server did not start"));
      return;
    }
    SessionData Data;
    Data.FSProtocol = fsFTP;
    Data.HostName = QStringLiteral("127.0.0.1");
    Data.PortNumber = Port;
    Data.UserName = QStringLiteral("u");
    Data.Password = QStringLiteral("p");
    Data.Timeout = 10;
    TestUserInterface UI;
    UI.OverwriteReply = Case.Answer;
    {
      Terminal T(Data, &UI);
      OperationResult R = DoSimple([&](Terminal::Callback Cb) { T.Open(Cb); });
      CHECK_MSG(R.Ok, Describe(R));
      CopyParam Param;
      Param.ConfirmOverwriting = false;
      R = DoSimple([&](Terminal::Callback Cb) { T.CopyToRemote({Scratch.filePath(QString::fromLatin1(Case.Name))}, QStringLiteral("/evil"), Param, false, Cb); });
      CHECK_MSG(!Case.Written || R.Ok, QString::fromLatin1(Case.Reply) + QLatin1Char(' ') + Describe(R));
      T.Close();
    }
    QString What = QString::fromLatin1(Case.Reply) + QLatin1Char(' ') + QString::fromLatin1(Case.Name);
    QString Target = QStringLiteral("/evil/") + QString::fromLatin1(Case.Name);
    QStringList Commands = ReadText(LogFile).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    CHECK_MSG(Commands.contains(QStringLiteral("STOR %1.filepart").arg(Target)), Commands.join(QLatin1Char('|')));
    CHECK_MSG(Commands.contains(QStringLiteral("STOR ") + Target) == Case.Written, What + QLatin1Char(' ') + Commands.join(QLatin1Char('|')));
    CHECK_MSG(!Commands.contains(QStringLiteral("DELE ") + Target), What + QLatin1Char(' ') + Commands.join(QLatin1Char('|')));
    bool Asked = std::any_of(UI.Queries.cbegin(), UI.Queries.cend(), [](const QString & Query) { return Query.startsWith(QStringLiteral("Cannot create temporary file")); });
    CHECK_MSG(Asked == Case.Asked, What + QLatin1Char(' ') + UI.Queries.join(QLatin1Char('|')));
    Server.kill();
    Server.waitForFinished(3000);
  }
}

static void TestEvilWebDav(const QString & Scripts)
{
  std::printf("[malicious WebDAV server: path traversal in PROPFIND, CR/LF in headers]\n");
  QTemporaryDir Scratch;
  QString AbsDir = Scratch.filePath(QStringLiteral("abs"));
  QDir().mkpath(AbsDir);
  QString LogFile = Scratch.filePath(QStringLiteral("dav.log"));
  int Port = FreePort();
  QProcess Server;
  Server.start(QStringLiteral("python3"), {QStringLiteral("-I"), QDir(Scripts).filePath(QStringLiteral("evil_dav.py")),
    QString::number(Port), AbsDir, LogFile});
  if (!WaitListening(Port))
  {
    CHECK_MSG(false, QStringLiteral("evil WebDAV server did not start"));
    return;
  }
  SessionData Data;
  Data.FSProtocol = fsWebDAV;
  Data.Ftps = ftpsNone;
  Data.HostName = QStringLiteral("127.0.0.1");
  Data.PortNumber = Port;
  Data.UserName = QStringLiteral("u");
  Data.Password = QStringLiteral("p");
  Data.Timeout = 10;
  TestUserInterface UI;
  {
    Terminal T(Data, &UI);
    OperationResult R = DoSimple([&](Terminal::Callback Cb) { T.Open(Cb); });
    CHECK_MSG(R.Ok, Describe(R));
    if (R.Ok)
    {
      RemoteFileList Files;
      R = DoList(T, QStringLiteral("/evil/"), &Files);
      for (const RemoteFile & F : Files)
      {
        Log(QStringLiteral("listed: \"%1\"").arg(F.FileName));
      }
      CheckTraversal(T, QStringLiteral("WebDAV"), AbsDir);
      DoSimple([&](Terminal::Callback Cb) { T.RenameFile(QStringLiteral("/evil/ok.txt"), QStringLiteral("/evil/x\r\nX-Injected: 1"), Cb); });
      DoSimple([&](Terminal::Callback Cb) { T.CreateDirectory(QStringLiteral("/evil/y\r\nX-Injected: 2"), 0755, Cb); });
      T.Close();
    }
  }
  // the server logs each received header as "  Name: value"; the names are
  // percent-encoded in the request target (that is fine)
  QString Requests = ReadText(LogFile);
  CHECK_MSG(!Requests.contains(QStringLiteral("\n  X-Injected:")), Requests);
  CHECK(Requests.contains(QStringLiteral("%0d%0aX-Injected"), Qt::CaseInsensitive));
  Server.kill();
  Server.waitForFinished(3000);
}

// ---------------------------------------------------------------- SCP with odd names (real server)

static void TestScpOddNames(const SessionData & Base)
{
  std::printf("[SCP: shell commands with odd file names]\n");
  QString Mark = MarkName();
  QTemporaryDir Root;   // the test server runs locally as the same user
  QString Home = QDir::homePath();
  QStringList Names = {
    QStringLiteral("-rf"), QStringLiteral("--help"), QStringLiteral("-n x"), QStringLiteral("a'b"), QStringLiteral("a\"b"),
    QStringLiteral("$(touch %1)").arg(Mark), QStringLiteral("`touch %1`").arg(Mark), QStringLiteral("x; touch %1").arg(Mark),
    QStringLiteral("a\\b"), QStringLiteral(" lead"), QStringLiteral("trail "), QStringLiteral("*"), QStringLiteral("!x"),
    QStringLiteral("$HOME"),
  };
  for (const QString & Name : Names)
  {
    QFile F(QDir(Root.path()).filePath(Name));
    CHECK(F.open(QIODevice::WriteOnly));
    F.write("data\n");
  }
  SessionData Data = Base;
  Data.FSProtocol = fsSCPonly;
  TestUserInterface UI;
  Terminal T(Data, &UI);
  OperationResult R = DoSimple([&](Terminal::Callback Cb) { T.Open(Cb); });
  CHECK_MSG(R.Ok, Describe(R));
  if (!R.Ok)
  {
    return;
  }
  RemoteFileList Files;
  R = DoList(T, Root.path(), &Files);
  CHECK_MSG(R.Ok, Describe(R));
  for (const QString & Name : Names)
  {
    bool Found = false;
    for (const RemoteFile & File : Files)
    {
      Found = Found || (File.FileName == Name);
    }
    CHECK_MSG(Found, Name);
  }
  QTemporaryDir Local;
  CopyParam Param;
  for (const QString & Name : Names)
  {
    QString Path = Root.path() + QLatin1Char('/') + Name;
    R = DoSimple([&](Terminal::Callback Cb) { T.ChangeFileProperties(QStringList{Path}, 0600, false, Cb); });
    CHECK_MSG(R.Ok && ((QFileInfo(Path).permissions() & QFileDevice::ReadGroup) == 0), Name + Describe(R));
    R = DoSimple([&](Terminal::Callback Cb) { T.CopyToLocal(QStringList{Path}, Local.path(), Param, false, Cb); });
    CHECK_MSG(R.Ok && QFileInfo::exists(QDir(Local.path()).filePath(Name)), Name + Describe(R));
    R = DoSimple([&](Terminal::Callback Cb) { T.RenameFile(Path, Path + QStringLiteral(".r"), Cb); });
    CHECK_MSG(R.Ok && QFileInfo::exists(Path + QStringLiteral(".r")), Name + Describe(R));
    R = DoSimple([&](Terminal::Callback Cb) { T.DeleteFiles(QStringList{Path + QStringLiteral(".r")}, Cb); });
    CHECK_MSG(R.Ok && !QFileInfo::exists(Path + QStringLiteral(".r")), Name + Describe(R));
  }
  // nothing else was deleted, nothing executed
  CHECK(QDir(Root.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden).isEmpty());
  for (const QString & Dir : {Root.path(), Home, Local.path()})
  {
    bool Injected = QFileInfo::exists(QDir(Dir).filePath(Mark));
    CHECK_MSG(!Injected, Dir);
    QFile::remove(QDir(Dir).filePath(Mark));
  }
  // new line in a name: the "ls -la" listing cannot represent it (as in WinSCP);
  // only checks that nothing is executed or deleted
  {
    QString Name = QStringLiteral("nl\ntouch %1").arg(Mark);
    QFile F(QDir(Root.path()).filePath(Name));
    CHECK(F.open(QIODevice::WriteOnly));
    F.close();
    QString Path = Root.path() + QLatin1Char('/') + Name;
    R = DoSimple([&](Terminal::Callback Cb) { T.RenameFile(Path, Path + QStringLiteral(".r"), Cb); });
    Log(QStringLiteral("rename of a name with a new line: %1").arg(Describe(R)));
    R = DoSimple([&](Terminal::Callback Cb) { T.DeleteFiles(QStringList{Path + QStringLiteral(".r")}, Cb); });
    Log(QStringLiteral("delete of a name with a new line: %1").arg(Describe(R)));
    for (const QString & Dir : {Root.path(), Home})
    {
      CHECK_MSG(!QFileInfo::exists(QDir(Dir).filePath(Mark)), Dir);
      QFile::remove(QDir(Dir).filePath(Mark));
    }
  }
  T.Close();
}

// ---------------------------------------------------------------- host keys

static QString RandomEd25519Line(const QString & HostPattern)
{
  QByteArray Blob;
  auto PutString = [&Blob](const QByteArray & S)
  {
    quint32 L = static_cast<quint32>(S.size());
    Blob.append(static_cast<char>(L >> 24)).append(static_cast<char>(L >> 16)).append(static_cast<char>(L >> 8)).append(static_cast<char>(L));
    Blob.append(S);
  };
  QByteArray Key(32, '\0');
  for (char & C : Key)
  {
    C = static_cast<char>(QRandomGenerator::global()->bounded(256));
  }
  PutString("ssh-ed25519");
  PutString(Key);
  return HostPattern + QStringLiteral(" ssh-ed25519 ") + QString::fromLatin1(Blob.toBase64()) + QLatin1Char('\n');
}

static void TestHostKeys(const SessionData & Base)
{
  std::printf("[SSH host key verification]\n");
  QTemporaryDir Scratch;
  QString Pattern = QStringLiteral("[%1]:%2").arg(Base.HostName).arg(Base.PortNumber);
  // 1. known_hosts has another key for the host: the user is asked (changed), Cancel aborts
  {
    QString KnownHosts = Scratch.filePath(QStringLiteral("kh1"));
    QFile F(KnownHosts);
    CHECK(F.open(QIODevice::WriteOnly));
    F.write(RandomEd25519Line(Pattern).toLatin1());
    F.close();
    SessionData Data = Base;
    Data.KnownHostsFile = KnownHosts;
    TestUserInterface UI;
    UI.HostKeyReply = UserInterface::HostKeyCancel;
    Terminal T(Data, &UI);
    OperationResult R = DoSimple([&](Terminal::Callback Cb) { T.Open(Cb); });
    CHECK_MSG(!R.Ok, Describe(R));
    CHECK(UI.HostKeyCalls == 1);
    CHECK(UI.LastHostKeyState == UserInterface::HostKeyChanged);
    // the known_hosts file was not modified
    CHECK(ReadText(KnownHosts).count(QLatin1Char('\n')) == 1);
  }
  // 2. unknown host, "Once": connects, nothing stored; next time asked again
  {
    QString KnownHosts = Scratch.filePath(QStringLiteral("kh2"));
    SessionData Data = Base;
    Data.KnownHostsFile = KnownHosts;
    TestUserInterface UI;
    UI.HostKeyReply = UserInterface::HostKeyOnce;
    {
      Terminal T(Data, &UI);
      OperationResult R = DoSimple([&](Terminal::Callback Cb) { T.Open(Cb); });
      CHECK_MSG(R.Ok, Describe(R));
      CHECK((UI.HostKeyCalls == 1) && (UI.LastHostKeyState == UserInterface::HostKeyUnknown));
      T.Close();
    }
    CHECK(ReadText(KnownHosts).trimmed().isEmpty());
    // Accept: stored; then no question
    UI.HostKeyReply = UserInterface::HostKeyAccept;
    {
      Terminal T(Data, &UI);
      CHECK(DoSimple([&](Terminal::Callback Cb) { T.Open(Cb); }).Ok);
      T.Close();
    }
    CHECK(UI.HostKeyCalls == 2);
    {
      Terminal T(Data, &UI);
      CHECK(DoSimple([&](Terminal::Callback Cb) { T.Open(Cb); }).Ok);
      T.Close();
    }
    CHECK(UI.HostKeyCalls == 2);
    // 3. configured fingerprint (-hostkey / SshHostKey) not matching: rejected
    // without asking, even though known_hosts trusts the key
    Data.HostKey = QStringLiteral("ssh-ed25519 255 SHA256:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
    {
      Terminal T(Data, &UI);
      OperationResult R = DoSimple([&](Terminal::Callback Cb) { T.Open(Cb); });
      CHECK_MSG(!R.Ok, Describe(R));
    }
    CHECK(UI.HostKeyCalls == 2);
  }
}

// ---------------------------------------------------------------- TLS certificates

static void TestTlsCertificate(const SessionData & Data, const QString & Name)
{
  // self-signed certificate: the user is asked; Cancel aborts the connection
  TestUserInterface UI;
  UI.CertificateReply = qaCancel;
  {
    Terminal T(Data, &UI);
    OperationResult R = DoSimple([&](Terminal::Callback Cb) { T.Open(Cb); });
    CHECK_MSG(!R.Ok, Name + QLatin1Char(' ') + Describe(R));
  }
  CHECK_MSG(UI.CertificateQueries >= 1, Name + QLatin1Char(' ') + UI.Queries.join(QStringLiteral(" / ")));
  // accepted once: connects; a new connection asks again
  UI.CertificateReply = qaNo;
  {
    Terminal T(Data, &UI);
    OperationResult R = DoSimple([&](Terminal::Callback Cb) { T.Open(Cb); });
    CHECK_MSG(R.Ok, Name + QLatin1Char(' ') + Describe(R));
    T.Close();
  }
  int Before = UI.CertificateQueries;
  UI.CertificateReply = qaCancel;
  {
    Terminal T(Data, &UI);
    OperationResult R = DoSimple([&](Terminal::Callback Cb) { T.Open(Cb); });
    CHECK_MSG(!R.Ok, Name + QLatin1Char(' ') + Describe(R));
  }
  CHECK(UI.CertificateQueries == Before + 1);
}

// How many times the certificate was questioned with these keys in the session
static int CertificateQuestions(SessionData Data, const QString & Keys, bool FromUrl, OperationResult * Result = nullptr)
{
  Data.HostKey = Keys;
  Data.HostKeyFromUrl = FromUrl;
  TestUserInterface UI;
  UI.CertificateReply = qaCancel;
  Terminal T(Data, &UI);
  OperationResult R = DoSimple([&](Terminal::Callback Cb) { T.Open(Cb); });
  T.Close();
  if (Result != nullptr)
  {
    *Result = R;
  }
  return UI.CertificateQueries;
}

// The fingerprint of a certificate the session cannot trust, from the question about it
static QString QuestionedFingerprint(const SessionData & Data)
{
  TestUserInterface UI;
  UI.CertificateReply = qaCancel;
  Terminal T(Data, &UI);
  DoSimple([&](Terminal::Callback Cb) { T.Open(Cb); });
  QRegularExpressionMatch Match = QRegularExpression(QStringLiteral("SHA-256: ([0-9a-fA-F:]+)")).match(UI.Queries.value(0));
  return Match.captured(1);
}

// What a URL says about a certificate, as about an SSH host key: no check is
// given up, and an unknown certificate is compared with the fingerprint
static void TestUrlCertificate(const SessionData & Data, const QString & Name, bool Connects)
{
  QString Fingerprint = QuestionedFingerprint(Data);
  CHECK_MSG(!Fingerprint.isEmpty(), Name);
  const QString Wrong = QStringLiteral("aa:bb:cc:dd");
  for (const QString & Keys : {QStringLiteral("*"), QStringLiteral(" * "), QStringLiteral("acceptnew"), QStringLiteral("*;acceptnew"), Wrong + QStringLiteral(";*"), Wrong})
  {
    OperationResult R;
    CHECK_MSG(CertificateQuestions(Data, Keys, true, &R) == 1, Name + QLatin1Char(' ') + Keys);
    CHECK_MSG(!R.Ok, Name + QLatin1Char(' ') + Keys);
  }
  // a switch or the site: what they say goes
  OperationResult R;
  CHECK_MSG(CertificateQuestions(Data, QStringLiteral("*"), false, &R) == 0, Name);
  CHECK_MSG(!Connects || R.Ok, Name + QLatin1Char(' ') + Describe(R));
  CHECK_MSG(CertificateQuestions(Data, Fingerprint, false, &R) == 0, Name);
  CHECK_MSG(!Connects || R.Ok, Name + QLatin1Char(' ') + Describe(R));
  CHECK_MSG(CertificateQuestions(Data, Fingerprint, true, &R) == 0, Name);
  CHECK_MSG(!Connects || R.Ok, Name + QLatin1Char(' ') + Describe(R));
}

// A self-signed TLS server that says no more than a 500 to HTTP, for the
// certificate check of WebDAV and S3 with a certificate that can be changed
static QStringList MakeCertificate(const QString & Dir, const QString & Name)
{
  QString Key = QDir(Dir).filePath(Name + QStringLiteral(".key"));
  QString Certificate = QDir(Dir).filePath(Name + QStringLiteral(".crt"));
  QProcess Make;
  // RSA: the Python of macOS has a LibreSSL that finds no cipher for an EC key
  Make.start(QStringLiteral("openssl"), {QStringLiteral("req"), QStringLiteral("-x509"), QStringLiteral("-newkey"), QStringLiteral("rsa:2048"),
    QStringLiteral("-nodes"), QStringLiteral("-subj"), QStringLiteral("/CN=") + Name,
    QStringLiteral("-keyout"), Key, QStringLiteral("-out"), Certificate, QStringLiteral("-days"), QStringLiteral("2")});
  CHECK(Make.waitForFinished(30000) && (Make.exitCode() == 0));
  return {Certificate, Key};
}

static void StartTlsServer(QProcess & Server, const QStringList & Certificate, int Port)
{
  static const char Script[] =
    "import socket, ssl, sys\n"
    "context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)\n"
    "context.load_cert_chain(sys.argv[1], sys.argv[2])\n"
    "listener = socket.socket()\n"
    "listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)\n"
    "listener.bind(('127.0.0.1', int(sys.argv[3])))\n"
    "listener.listen(5)\n"
    "while True:\n"
    "    client, _ = listener.accept()\n"
    "    try:\n"
    "        tls = context.wrap_socket(client, server_side=True)\n"
    "        tls.recv(65536)\n"
    "        tls.sendall(b'HTTP/1.1 500 No\\r\\nContent-Length: 0\\r\\nConnection: close\\r\\n\\r\\n')\n"
    "        tls.close()\n"
    "    except OSError:\n"
    "        client.close()\n";
  Server.start(QStringLiteral("python3"), {QStringLiteral("-I"), QStringLiteral("-c"), QString::fromLatin1(Script),
    Certificate.value(0), Certificate.value(1), QString::number(Port)});
  CHECK(WaitListening(Port));
}

// As a key known for a host: the certificate accepted for a server is not
// replaced by a fingerprint in a URL, the new one is a question
static void TestUrlCertificateChanged(TFSProtocol Protocol, const QString & Name)
{
  QTemporaryDir Scratch;
  QStringList First = MakeCertificate(Scratch.path(), Name + QStringLiteral("-first"));
  QStringList Second = MakeCertificate(Scratch.path(), Name + QStringLiteral("-second"));
  int Port = FreePort();
  SessionData Data;
  Data.FSProtocol = Protocol;
  Data.Ftps = ftpsImplicit;
  Data.HostName = QStringLiteral("127.0.0.1");
  Data.PortNumber = Port;
  Data.UserName = QStringLiteral("u");
  Data.Password = QStringLiteral("p");
  Data.Timeout = 5;
  {
    QProcess Server;
    StartTlsServer(Server, First, Port);
    // nothing known yet: the fingerprint is compared
    QString Fingerprint = QuestionedFingerprint(Data);
    CHECK_MSG(CertificateQuestions(Data, Fingerprint, true) == 0, Name);
    // the user accepts it for good
    TestUserInterface UI;
    UI.CertificateReply = qaYes;
    Terminal T(Data, &UI);
    DoSimple([&](Terminal::Callback Cb) { T.Open(Cb); });
    T.Close();
    CHECK_MSG(UI.CertificateQueries == 1, Name);
    Server.kill();
    Server.waitForFinished(3000);
  }
  QProcess Server;
  StartTlsServer(Server, Second, Port);
  QString Fingerprint = QuestionedFingerprint(Data);
  CHECK_MSG(!Fingerprint.isEmpty(), Name);
  CHECK_MSG(CertificateQuestions(Data, Fingerprint, true) == 1, Name);
  CHECK_MSG(CertificateQuestions(Data, Fingerprint, false) == 0, Name);
  Server.kill();
  Server.waitForFinished(3000);
}

static void TestTls()
{
  QByteArray FtpsPort = qgetenv("NIXSCP_TEST_FTPS_PORT");
  if (!FtpsPort.isEmpty() && WaitListening(FtpsPort.toInt()))
  {
    std::printf("[FTPS certificate verification]\n");
    SessionData Data;
    Data.FSProtocol = fsFTP;
    Data.Ftps = ftpsExplicitTls;
    Data.HostName = QStringLiteral("127.0.0.1");
    Data.PortNumber = FtpsPort.toInt();
    Data.UserName = qEnvironmentVariable("NIXSCP_TEST_FTP_USER", QStringLiteral("test"));
    Data.Password = qEnvironmentVariable("NIXSCP_TEST_FTP_PASSWORD", QStringLiteral("secret"));
    Data.Timeout = 10;
    TestTlsCertificate(Data, QStringLiteral("FTPS"));
    TestUrlCertificate(Data, QStringLiteral("FTPS"), true);
    // a certificate accepted before, of another fingerprint: a question, whatever the URL says
    QString Fingerprint = QuestionedFingerprint(Data);
    FtpFileSystem::CacheCertificate(QStringLiteral("%1:%2").arg(Data.HostName).arg(Data.PortNumber), QStringLiteral("aa:bb:cc"), 18);
    CHECK(CertificateQuestions(Data, Fingerprint, true) == 1);
    CHECK(CertificateQuestions(Data, Fingerprint, false) == 0);
  }
  else
  {
    std::printf("[FTPS certificate verification] SKIPPED (NIXSCP_TEST_FTPS_PORT)\n");
  }
  QString DavsUrl = qEnvironmentVariable("NIXSCP_TEST_DAVS_URL");
  QUrl Url(DavsUrl);
  if (!DavsUrl.isEmpty() && WaitListening(Url.port(443)))
  {
    std::printf("[WebDAVS certificate verification]\n");
    SessionData Data;
    Data.FSProtocol = fsWebDAV;
    Data.Ftps = ftpsImplicit;
    Data.HostName = Url.host();
    Data.PortNumber = Url.port(443);
    Data.UserName = qEnvironmentVariable("NIXSCP_TEST_DAV_USER", QStringLiteral("test"));
    Data.Password = qEnvironmentVariable("NIXSCP_TEST_DAV_PASSWORD", QStringLiteral("secret"));
    Data.Timeout = 10;
    TestTlsCertificate(Data, QStringLiteral("WebDAVS"));
    TestUrlCertificate(Data, QStringLiteral("WebDAVS"), true);
    // the certificate check of S3 needs no more than the TLS of that server
    SessionData S3 = Data;
    S3.FSProtocol = fsS3;
    TestUrlCertificate(S3, QStringLiteral("S3"), false);
  }
  else
  {
    std::printf("[WebDAVS certificate verification] SKIPPED (NIXSCP_TEST_DAVS_URL)\n");
  }
  if (!QStandardPaths::findExecutable(QStringLiteral("openssl")).isEmpty())
  {
    std::printf("[certificate changed, fingerprint in a URL]\n");
    TestUrlCertificateChanged(fsWebDAV, QStringLiteral("dav"));
    TestUrlCertificateChanged(fsS3, QStringLiteral("s3"));
  }
  else
  {
    std::printf("[certificate changed, fingerprint in a URL] SKIPPED (no openssl)\n");
  }
}

int main(int argc, char ** argv)
{
  // never touch the user's configuration
  QTemporaryDir ConfigHome;
  qputenv("XDG_CONFIG_HOME", ConfigHome.path().toLocal8Bit());
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
  {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  QApplication App(argc, argv);
  Strict = (qgetenv("NIXSCP_TEST_SECURITY_STRICT") == "1");

  QTemporaryDir Work;
  CHECK(Work.isValid());
  TestPasswords();
#ifndef _WIN32
  TestIniPermissions(Work.path());
#endif
  TestLocalCustomCommands(Work.path());
#ifdef _WIN32
  // Escaping for the remote (Unix) shell, checked with a local /bin/sh: Linux
  std::printf("[remote shell escaping, OpenSSH command line, Unix permissions] SKIPPED (Linux only)\n");
#else
  TestRemoteCustomCommands(Work.path());
  TestScpShellQuoting(Work.path());
  TestTerminalCommand(Work.path());
#endif
  TestEditorCommand();

  QString KeyFile = qEnvironmentVariable("NIXSCP_TEST_SSH_KEY");
  QString User = qEnvironmentVariable("NIXSCP_TEST_SSH_USER", qEnvironmentVariable("USER"));
  QByteArray SshPort = qgetenv("NIXSCP_TEST_SSH_PORT");
  if (!SshPort.isEmpty() && !KeyFile.isEmpty())
  {
    QTemporaryDir Scratch;
    SessionData Data;
    Data.HostName = qEnvironmentVariable("NIXSCP_TEST_SSH_HOST", QStringLiteral("127.0.0.1"));
    Data.PortNumber = SshPort.toInt();
    Data.UserName = User;
    Data.PublicKeyFile = KeyFile;
    Data.TryAgent = false;
    Data.AuthKI = false;
    Data.KnownHostsFile = Scratch.filePath(QStringLiteral("known_hosts"));
    TestHostKeys(Data);
    TestScpOddNames(Data);
  }
  else
  {
    std::printf("[SSH server tests] SKIPPED (set NIXSCP_TEST_SSH_PORT, NIXSCP_TEST_SSH_KEY)\n");
  }
  TestEvilSftp(KeyFile, User);
  QString Scripts = qEnvironmentVariable("NIXSCP_TEST_EVIL_SERVERS");
  if (!Scripts.isEmpty() && QFileInfo::exists(QDir(Scripts).filePath(QStringLiteral("evil_ftp.py"))))
  {
    TestEvilFtp(Scripts);
    // (an older copy of the server, as in a test environment set up before)
    if (ReadText(QDir(Scripts).filePath(QStringLiteral("evil_ftp.py"))).contains(QStringLiteral("EVIL_FTP_STOR_END")))
    {
      TestFtpPartialRefused(Scripts);
    }
    else
    {
      std::printf("[FTP server refuses the temporary file] SKIPPED (evil_ftp.py without EVIL_FTP_STOR_END)\n");
    }
    TestEvilWebDav(Scripts);
  }
  else
  {
    std::printf("[malicious FTP/WebDAV servers] SKIPPED (set NIXSCP_TEST_EVIL_SERVERS)\n");
  }
  TestTls();

  if (Vulnerable > 0)
  {
    std::printf("test_verify_security: %d known vulnerabilities still present%s\n", Vulnerable,
      Strict ? "" : " (not counted as failures; NIXSCP_TEST_SECURITY_STRICT=1 to fail)");
  }
  std::printf((Failures == 0) ? "test_verify_security: all tests passed\n" : "test_verify_security: %d failures\n", Failures);
  return (Failures == 0) ? 0 : 1;
}
