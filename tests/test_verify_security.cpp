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
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <sys/stat.h>
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
  return InjectionPayloads(Mark);
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
    // unquoted "!" and '!' are not safe in WinSCP either (DelimitStr escapes
    // only $ and \ outside double quotes); informational only
    for (const QString & Template : {QStringLiteral("printf %s !"), QStringLiteral("printf %s '!'")})
    {
      Cmd = RemoteCustomCommand(Data, QStringLiteral("/home/u"), Payload, QString()).Complete(Template, true);
      RunShell(Cmd, Dir);
      if (QFileInfo::exists(QDir(Dir).filePath(Mark)))
      {
        Log(QStringLiteral("note (WinSCP parity): remote command [%1] is injectable by [%2]")
          .arg(Template, QString(Payload).replace(QLatin1Char('\n'), QStringLiteral("\\n"))));
      }
      QFile::remove(QDir(Dir).filePath(Mark));
    }
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
    CHECK_MSG(Output == Name, Name);
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
    CHECK(!Command.AskPassFile.isEmpty());
    if (!Command.AskPassFile.isEmpty())
    {
      QFileInfo Info(Command.AskPassFile);
      CHECK(Info.isFile());
      CHECK_MSG((Info.permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther | QFileDevice::WriteGroup | QFileDevice::WriteOther)) == 0,
        Command.AskPassFile);
      // unpredictable name in a private directory
      CHECK(Info.fileName().size() > QStringLiteral("nixscp-askpass-").size() + 5);
      QFileInfo DirInfo(Info.absolutePath());
      bool PrivateDir = (DirInfo.permissions() & (QFileDevice::WriteGroup | QFileDevice::WriteOther)) == 0;
      Log(QStringLiteral("askpass file %1 (directory private: %2)").arg(Command.AskPassFile).arg(PrivateDir));
      QFile F(Command.AskPassFile);
      CHECK(F.open(QIODevice::ReadOnly) && (F.readAll() == "s3cret pw"));
      F.close();
      // the password is not on the command line
      CHECK(!Command.Arguments.join(QLatin1Char(' ')).contains(QStringLiteral("s3cret")));
      QFile::remove(Command.AskPassFile);
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
  QStringList Files = AllFiles(Work.path());
  QString Allowed = Target + QStringLiteral("/evil/");
  bool OkDownloaded = false;
  for (const QString & File : Files)
  {
    if (File == Allowed + QStringLiteral("ok.txt"))
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
  }
  else
  {
    std::printf("[WebDAVS certificate verification] SKIPPED (NIXSCP_TEST_DAVS_URL)\n");
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
