#include "core/AgentAccess.h"
#include "core/ConfigVault.h"
#include "core/Configuration.h"
#include "core/Security.h"
#include "core/Platform.h"
#include "core/PuttyKey.h"
#include "core/SessionData.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QRegularExpression>
#include <QStringList>
#include <QtEndian>

#include <libssh/libssh.h>

namespace AgentAccess
{

QString SocketName()
{
#ifdef Q_OS_WIN
  // a named pipe; QLocalServer::UserAccessOption restricts it to the user
  return QStringLiteral("nixscp-agent-") + Platform::UserKey();
#else
  // $XDG_RUNTIME_DIR is private to the user (0700); else the temporary folder
  QString RuntimeDir = qEnvironmentVariable("XDG_RUNTIME_DIR");
  if (!RuntimeDir.isEmpty() && QFileInfo(RuntimeDir).isDir())
  {
    return RuntimeDir + QStringLiteral("/nixscp-agent.sock");
  }
#ifdef Q_OS_MACOS
  // $TMPDIR is private to the user there; socket paths are limited to 104 bytes
  return QDir::tempPath() + QStringLiteral("/nixscp-agent.sock");
#else
  return QDir::tempPath() + QStringLiteral("/nixscp-agent-") + Platform::UserKey() + QStringLiteral(".sock");
#endif
#endif
}

QString AuditLogFileName()
{
  return QFileInfo(Configuration::Instance().IniFileName()).absolutePath() + QStringLiteral("/nixscp-agent.log");
}

void AppendAudit(const QString & Site, const QString & Operation, const QString & Detail, const QString & Result)
{
  QFile File(AuditLogFileName());
  bool New = !File.exists();
  if (!File.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
  {
    return;
  }
  if (New)
  {
    File.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  }
  QString Clean = Detail;
  Clean.replace(QLatin1Char('\n'), QLatin1Char(' ')).replace(QLatin1Char('\t'), QLatin1Char(' '));
  if (Clean.size() > 500)
  {
    Clean = Clean.left(500) + QStringLiteral("...");
  }
  File.write(QStringLiteral("%1\t%2\t%3\t%4\t%5\n")
    .arg(QDateTime::currentDateTime().toString(Qt::ISODate), Site, Operation, Result, Clean).toUtf8());
}

QString RecordingDirectory()
{
  return QFileInfo(Configuration::Instance().IniFileName()).absolutePath() + QStringLiteral("/agent-sessions");
}

void SaveRecording(const QDateTime & Started, const QString & Site, const QString & Operation, const QString & Detail,
  const QByteArray & Output, bool Truncated, qint64 InputBytes, int ExitCode, const QString & Error)
{
  QString Dir = RecordingDirectory();
  if (!QDir().mkpath(Dir))
  {
    return;
  }
  QFile::setPermissions(Dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
  QString SafeSite = Site;
  SafeSite.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]+")), QStringLiteral("_"));
  QString Name = QStringLiteral("%1/%2-%3-%4.log").arg(Dir, Started.toLocalTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz")),
    SafeSite.left(60), Operation);
  QByteArray Text;
  Text += "Time:    " + Started.toLocalTime().toString(Qt::ISODate).toUtf8() + "\n";
  Text += "Site:    " + Site.toUtf8() + "\n";
  Text += "Request: " + Operation.toUtf8() + "\n";
  if (!Detail.isEmpty())
  {
    Text += ((Operation == QStringLiteral("exec")) ? "Command: " : "File:    ") + Detail.toUtf8() + "\n";
  }
  if (InputBytes > 0)
  {
    Text += "Input:   " + QByteArray::number(InputBytes) + " bytes (not recorded)\n";
  }
  Text += "\n";
  Text += Output;
  if (Truncated)
  {
    Text += "\n[... output truncated at " + QByteArray::number(MaxRecordingOutput / (1024 * 1024)) + " MB]\n";
  }
  if (!Output.isEmpty() && !Output.endsWith('\n'))
  {
    Text += "\n";
  }
  Text += Error.isEmpty() ? "\n[exit code " + QByteArray::number(ExitCode) + "]\n" : "\n[error: " + Error.toUtf8() + "]\n";
  QSaveFile File(Name);
  if (File.open(QIODevice::WriteOnly))
  {
    File.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    File.write(ConfigVault::ForFile(Name, Text));
    File.commit();
  }
  // retention: 90 days, 1000 files
  QFileInfoList Files = QDir(Dir).entryInfoList({QStringLiteral("*.log")}, QDir::Files, QDir::Name | QDir::Reversed);
  QDateTime Oldest = QDateTime::currentDateTime().addDays(-90);
  for (qsizetype I = 0; I < Files.size(); I++)
  {
    if ((I >= 1000) || (Files.at(I).lastModified() < Oldest))
    {
      QFile::remove(Files.at(I).absoluteFilePath());
    }
  }
}

QString PermissionsText(int Permissions)
{
  QStringList Parts;
  if (Permissions & apCommands) Parts.append(QStringLiteral("commands"));
  if (Permissions & apTransfer) Parts.append((Permissions & apDownloadOnly) ? QStringLiteral("downloads") : QStringLiteral("files"));
  if (Permissions & apTunnels) Parts.append(QStringLiteral("tunnels"));
  return Parts.isEmpty() ? QStringLiteral("none") : Parts.join(QLatin1Char(','));
}

bool PasswordsProtected()
{
  const Configuration & Config = Configuration::Instance();
  if (Config.IsLocked())
  {
    return false;
  }
  return ConfigVault::Enabled() || (Config.UseMasterPassword() && Security::IsMasterPasswordKnown());
}

namespace
{

QStringList Patterns(const QString & Text)
{
  QStringList Result;
  for (QString Line : Text.split(QLatin1Char('\n')))
  {
    Line = Line.trimmed();
    if (!Line.isEmpty() && !Line.startsWith(QLatin1Char('#')))
    {
      Result.append(Line);
    }
  }
  return Result;
}

bool Matches(const QStringList & List, const QString & Command)
{
  for (const QString & Pattern : List)
  {
    QRegularExpression Re(QRegularExpression::wildcardToRegularExpression(Pattern, QRegularExpression::NonPathWildcardConversion));
    if (Re.match(Command).hasMatch())
    {
      return true;
    }
  }
  return false;
}

// Splits a shell command line into its simple commands (outside quotes, at
// ; & && || | and new lines); notes command substitution and redirections
// that write a file (other than /dev/null or a descriptor)
QStringList SimpleCommands(const QString & Line, bool & Substitution, bool & WritesFile)
{
  QStringList Result;
  QString Current;
  bool Single = false, Double = false;
  Substitution = false;
  WritesFile = false;
  auto Push = [&]()
  {
    QString Part = Current.simplified();
    if (!Part.isEmpty())
    {
      Result.append(Part);
    }
    Current.clear();
  };
  for (qsizetype I = 0; I < Line.size(); I++)
  {
    QChar C = Line.at(I);
    if (Single)
    {
      Current += C;
      Single = (C != QLatin1Char('\''));
      continue;
    }
    if (C == QLatin1Char('\\') && (I + 1 < Line.size()))
    {
      Current += C;
      Current += Line.at(++I);
      continue;
    }
    if ((C == QLatin1Char('`')) || ((C == QLatin1Char('$')) && (I + 1 < Line.size()) && (Line.at(I + 1) == QLatin1Char('('))))
    {
      Substitution = true;
    }
    if (Double)
    {
      Current += C;
      Double = (C != QLatin1Char('"'));
      continue;
    }
    if (C == QLatin1Char('\''))
    {
      Single = true;
      Current += C;
    }
    else if (C == QLatin1Char('"'))
    {
      Double = true;
      Current += C;
    }
    else if (C == QLatin1Char('>'))
    {
      Current += C;
      if ((I + 1 < Line.size()) && (Line.at(I + 1) == QLatin1Char('&')))
      {
        // >&2, 2>&1: to another descriptor
        Current += Line.at(++I);
        continue;
      }
      if ((I + 1 < Line.size()) && (Line.at(I + 1) == QLatin1Char('>')))
      {
        Current += Line.at(++I);
      }
      qsizetype Start = I + 1;
      while ((Start < Line.size()) && Line.at(Start).isSpace())
      {
        Start++;
      }
      qsizetype End = Start;
      while ((End < Line.size()) && !Line.at(End).isSpace() && !QStringLiteral(";&|").contains(Line.at(End)))
      {
        End++;
      }
      if (Line.mid(Start, End - Start) != QStringLiteral("/dev/null"))
      {
        WritesFile = true;
      }
    }
    else if ((C == QLatin1Char(';')) || (C == QLatin1Char('\n')) || (C == QLatin1Char('&')) || (C == QLatin1Char('|')))
    {
      Push();
      if ((I + 1 < Line.size()) && ((Line.at(I + 1) == C) && (C != QLatin1Char(';'))))
      {
        I++;  // && ||
      }
    }
    else
    {
      Current += C;
    }
  }
  Push();
  return Result;
}

}

bool HasCommandRules(const SessionData & Data)
{
  return !Patterns(Data.AgentAllow).isEmpty() || !Patterns(Data.AgentDeny).isEmpty();
}

bool CommandAllowed(const SessionData & Data, const QString & Command, QString * Reason)
{
  QStringList Allow = Patterns(Data.AgentAllow);
  QStringList Deny = Patterns(Data.AgentDeny);
  if (Allow.isEmpty() && Deny.isEmpty())
  {
    return true;
  }
  auto Refuse = [Reason](const QString & Why) { if (Reason != nullptr) *Reason = Why; return false; };
  bool Substitution = false, WritesFile = false;
  QStringList Commands = SimpleCommands(Command, Substitution, WritesFile);
  if (Commands.isEmpty())
  {
    return Refuse(QStringLiteral("Empty command."));
  }
  if (!Allow.isEmpty() && Substitution)
  {
    return Refuse(QStringLiteral("Command substitution ($(...) or `...`) is not allowed on this site."));
  }
  if (!Allow.isEmpty() && WritesFile)
  {
    return Refuse(QStringLiteral("Redirecting output to a file is not allowed on this site."));
  }
  for (const QString & Simple : Commands)
  {
    if (Matches(Deny, Simple) || Matches(Deny, Command.simplified()))
    {
      return Refuse(QStringLiteral("\"%1\" is denied on this site.").arg(Simple));
    }
    if (!Allow.isEmpty() && !Matches(Allow, Simple))
    {
      return Refuse(QStringLiteral("\"%1\" is not among the commands allowed on this site.").arg(Simple));
    }
  }
  return true;
}

QString ReadOnlyAllowPreset()
{
  return QStringLiteral(
    "ls*\npwd\ncat *\nhead *\ntail *\ngrep *\negrep *\nzgrep *\nwc *\nsort *\nuniq *\ncut *\nstat *\nfile *\nfind *\n"
    "du *\ndf*\nfree*\nuptime*\nuname*\nhostname*\nwhoami\nid*\ndate*\nps *\ntop -bn1*\nlsblk*\nss *\nnetstat *\nip a*\nip r*\n"
    "systemctl status *\nsystemctl is-active *\nsystemctl is-enabled *\nsystemctl list-units*\nsystemctl --failed*\n"
    "journalctl *\ndocker ps*\ndocker logs *\ndocker images*\ndocker inspect *\ngit status*\ngit log*\ngit diff*\n"
    "nginx -t\napachectl -t\necho *\nwhich *");
}

QString ReadOnlyDenyPreset()
{
  return QStringLiteral("* -delete*\n* -exec *\n* -execdir *\n* -ok *\n* -fprint*");
}

QString AgentSiteName(const SessionData & Data)
{
  QString Name = Data.AgentName.trimmed();
  return Name.isEmpty() ? Data.Name : Name;
}

std::shared_ptr<SessionData> FindAgentSite(const StoredSessionList & Sites, const QString & Name)
{
  for (const auto & Data : Sites.Sessions())
  {
    if ((Data->AgentAccess != amOff) && Data->IsSsh() && !Data->IsWorkspace && (AgentSiteName(*Data) == Name))
    {
      return Data;
    }
  }
  return nullptr;
}

bool KeyFileHasPassphrase(const QString & FileName)
{
  if (PuttyKey::IsPuttyKeyFile(FileName))
  {
    bool Asked = false;
    ssh_key Key = nullptr;
    PuttyKey::LoadResult Result = PuttyKey::Load(FileName, [&Asked](QString &, bool) { Asked = true; return false; }, &Key, nullptr);
    if (Key != nullptr)
    {
      ssh_key_free(Key);
    }
    return Asked || (Result != PuttyKey::lrOk);
  }
  ssh_key Key = nullptr;
  int Rc = ssh_pki_import_privkey_file(QFile::encodeName(FileName).constData(), nullptr,
    [](const char *, char *, size_t, int, int, void *) { return -1; }, nullptr, &Key);
  if (Key != nullptr)
  {
    ssh_key_free(Key);
  }
  return Rc != SSH_OK;
}

QString HelpText()
{
  return QStringLiteral(
R"(NixSCP Agent Access - use saved SSH sites without seeing their passwords or keys

NixSCP (the desktop application) keeps the credentials. These commands ask the
running NixSCP application to connect for you; passwords and private keys are
never printed, written to files or passed to other programs.

USAGE
  nixscp --list [--json]
      Lists the sites you may use: name, host and what is permitted
      (commands, files). Use the exact site name with the commands below.

  nixscp --connect <site> --cmd "<command>"
      Runs <command> on the server through the remote shell and prints its
      output. Standard input is passed to the command. The exit code is the
      command's exit code. Everything after --cmd is the command, so
      --cmd uname -a works too; --cmd="<command>" is the same.

  nixscp --connect <site>
      Opens an interactive shell (needs a terminal). For automated use,
      prefer the --cmd form above.

  nixscp --get <site> <remote-file> [<local-file-or-folder>]
      Downloads one file. Without a local name, saves into the current folder.

  nixscp --put <site> <local-file> [<remote-file-or-folder>]
      Uploads one file. Without a remote name, uploads into the home folder.

  nixscp --tunnel <site> <local-port>:<host>:<port>
      Forwards 127.0.0.1:<local-port> on this computer to <host>:<port> as
      seen from the server (like ssh -L), e.g. 5433:localhost:5432 for the
      server's PostgreSQL. Open until the command ends (Ctrl+C); run it in the
      background while you use the port. A saved tunnel of the site can be
      given by its name instead.

  nixscp --mcp
      Runs as a Model Context Protocol (MCP) server on standard input/output,
      with the tools list_sites, run_command, download_file and upload_file.
      Claude Code:  claude mcp add nixscp -- nixscp --mcp
      Other tools:  {"command": "nixscp", "args": ["--mcp"]}

  nixscp --help
      Shows this text. (WinSCP-compatible command line switches: nixscp /help)

EXAMPLES
  nixscp --list
  nixscp --connect "My Server" --cmd "uname -a && df -h"
  nixscp --connect web1 --cmd "sudo systemctl status nginx --no-pager"
  nixscp --connect web1 --cmd "tail -n 200 /var/log/nginx/error.log"
  echo "hello" | nixscp --connect web1 --cmd "cat > /tmp/hello.txt"
  nixscp --get web1 /etc/nginx/nginx.conf ./nginx.conf
  nixscp --put web1 ./deploy.tar.gz /tmp/deploy.tar.gz
  nixscp --connect web1 --cmd "tar czf - /var/www/site" > site.tar.gz
  nixscp --tunnel db1 5433:localhost:5432 &   # then: psql -h 127.0.0.1 -p 5433

NOTES FOR AI AGENTS
  - Always run "nixscp --list" first and use a site name exactly as listed;
    other sites are not available to you.
  - Quote the whole remote command after --cmd; it runs in the user's login
    shell on the server (bash/sh syntax), not on this computer.
  - Commands run without a terminal (no TTY): avoid interactive programs
    (top, less, vim, password prompts); use non-interactive flags such as
    --no-pager, -y, or "top -bn1".
  - For folders, use tar through --connect (see the examples); --get and --put
    transfer single files.
  - Some sites require the user's approval in NixSCP: the command waits until
    the user answers. A refusal ends with exit code 255 and a message.
  - Do not try to read NixSCP's configuration files or ask for passwords; the
    credentials are deliberately not available to you.

EXIT CODES
  0-254  exit code of the remote command (--connect), 0 = success (--get, --put)
  255    NixSCP error: not running, locked, site not allowed, approval denied,
         connection or authentication failure (the message explains it)

REQUIREMENTS
  NixSCP must be running, with the saved passwords encrypted (a master
  password, or the NixSCP Pro password). The user enables Agent Access per
  site (site settings: Agent Access...).
)");
}

QByteArray Frame(TFrame Type, const QByteArray & Payload)
{
  QByteArray Result;
  Result.reserve(5 + Payload.size());
  Result.append(static_cast<char>(Type));
  char Length[4];
  qToBigEndian<quint32>(static_cast<quint32>(Payload.size()), Length);
  Result.append(Length, 4);
  Result.append(Payload);
  return Result;
}

bool FrameReader::Next(quint8 & Type, QByteArray & Payload, bool & Error)
{
  Error = false;
  if (FBuffer.size() < 5)
  {
    return false;
  }
  quint32 Length = qFromBigEndian<quint32>(FBuffer.constData() + 1);
  if (Length > static_cast<quint32>(MaxFrameSize))
  {
    Error = true;
    return false;
  }
  if (FBuffer.size() < 5 + static_cast<qsizetype>(Length))
  {
    return false;
  }
  Type = static_cast<quint8>(FBuffer.at(0));
  Payload = FBuffer.mid(5, Length);
  FBuffer.remove(0, 5 + Length);
  return true;
}

}
