#include "core/AgentAccess.h"
#include "core/ConfigVault.h"
#include "core/Configuration.h"
#include "core/IniStorage.h"
#include "core/Security.h"
#include "core/Platform.h"
#include "core/PuttyKey.h"
#include "core/SessionData.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMap>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QRegularExpression>
#include <QStringList>
#include <QtEndian>

#include <libssh/libssh.h>

#include <algorithm>

#ifndef Q_OS_WIN
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace AgentAccess
{

#ifndef Q_OS_WIN
// Only this user can create or replace anything in the folder: it is ours and
// neither the group nor others may write to it
static bool PrivateDirectory(const QString & Path)
{
  struct stat Info = {};
  return (stat(QFile::encodeName(Path).constData(), &Info) == 0) && S_ISDIR(Info.st_mode) &&
    (Info.st_uid == getuid()) && ((Info.st_mode & (S_IWGRP | S_IWOTH)) == 0);
}
#endif

QString SocketName()
{
#ifdef Q_OS_WIN
  // a named pipe; QLocalServer::UserAccessOption restricts it to the user, but
  // names of pipes are global: PeerIsSameUser finds out who created this one
  return QStringLiteral("nixscp-agent-") + Platform::UserKey();
#else
  // Never in a folder others can write to: whoever creates the socket first
  // there gets the commands and uploads of every client. $XDG_RUNTIME_DIR is
  // private to the user (0700), when it is what it should be
  QString RuntimeDir = qEnvironmentVariable("XDG_RUNTIME_DIR");
  if (!RuntimeDir.isEmpty() && PrivateDirectory(RuntimeDir))
  {
    return RuntimeDir + QStringLiteral("/nixscp-agent.sock");
  }
#ifdef Q_OS_MACOS
  // $TMPDIR is private to the user there; socket paths are limited to 104 bytes
  if (PrivateDirectory(QDir::tempPath()))
  {
    return QDir::tempPath() + QStringLiteral("/nixscp-agent.sock");
  }
#endif
  return QDir::homePath() + QStringLiteral("/.nixscp-agent.sock");
#endif
}

bool PeerIsSameUser(qintptr Socket)
{
#if defined(Q_OS_LINUX)
  struct ucred Credentials = {};
  socklen_t Length = sizeof(Credentials);
  if (getsockopt(static_cast<int>(Socket), SOL_SOCKET, SO_PEERCRED, &Credentials, &Length) != 0)
  {
    return false;
  }
  return Credentials.uid == getuid();
#elif defined(Q_OS_MACOS)
  uid_t Uid = 0;
  gid_t Gid = 0;
  if (getpeereid(static_cast<int>(Socket), &Uid, &Gid) != 0)
  {
    return false;
  }
  return Uid == getuid();
#elif defined(Q_OS_WIN)
  // The ACL of a pipe of ours (UserAccessOption) admits this user only, on
  // both ends, so the question is who created it: anyone can be first with a name
  return Platform::OwnsPipe(Socket);
#else
  Q_UNUSED(Socket);
  return false;
#endif
}

QString AuditLogFileName()
{
  return QFileInfo(Configuration::Instance().IniFileName()).absolutePath() + QStringLiteral("/nixscp-agent.log");
}

QString VisibleText(const QString & Text, bool KeepNewLines)
{
  QString Result;
  Result.reserve(Text.size());
  for (qsizetype I = 0; I < Text.size(); I++)
  {
    QChar C = Text.at(I);
    char16_t U = C.unicode();
    if (C.isHighSurrogate() && (I + 1 < Text.size()) && Text.at(I + 1).isLowSurrogate())
    {
      char32_t Code = QChar::surrogateToUcs4(C, Text.at(++I));
      Result += (QChar::category(Code) == QChar::Other_Format) ? QStringLiteral("\\U%1").arg(static_cast<uint>(Code), 8, 16, QLatin1Char('0')) :
        QString(QChar::fromUcs4(Code));
    }
    else if ((U == u'\n') && KeepNewLines)
    {
      Result += C;
    }
    else if (U == u'\n')
    {
      Result += QStringLiteral("\\n");
    }
    else if (U == u'\t')
    {
      Result += KeepNewLines ? QString(C) : QStringLiteral("\\t");
    }
    else if (U == u'\r')
    {
      Result += QStringLiteral("\\r");
    }
    else if ((U < 0x20) || ((U >= 0x7F) && (U < 0xA0)) || (U == 0x2028) || (U == 0x2029) ||
             // text direction overrides show a command in another order than it
             // runs, others are not shown at all ...
             (C.category() == QChar::Other_Format) ||
             // ... and this one is gone from what is sent
             C.isSurrogate())
    {
      Result += (U < 0x100) ? QStringLiteral("\\x%1").arg(static_cast<uint>(U), 2, 16, QLatin1Char('0')) :
        QStringLiteral("\\u%1").arg(static_cast<uint>(U), 4, 16, QLatin1Char('0'));
    }
    else
    {
      Result += C;
    }
  }
  return Result;
}

bool ExactUtf8(const QString & Text)
{
  return Text.isValidUtf16() && (QString::fromUtf8(Text.toUtf8()) == Text);
}

bool AppendAudit(const QString & Site, const QString & Operation, const QString & Detail, const QString & Result)
{
  QFile File(AuditLogFileName());
  if (!File.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
  {
    return false;
  }
  Platform::MakePrivate(File);
  // one line per request: nothing an agent or a site name brings in may start
  // another line or field, as if a request had been recorded that never was
  QString Clean = VisibleText(Detail, false);
  if (Clean.size() > 500)
  {
    Clean = Clean.left(500) + QStringLiteral("...");
  }
  QByteArray Line = QStringLiteral("%1\t%2\t%3\t%4\t%5\n").arg(QDateTime::currentDateTime().toString(Qt::ISODate),
    VisibleText(Site, false), VisibleText(Operation, false), VisibleText(Result, false), Clean).toUtf8();
  return (File.write(Line) == Line.size()) && File.flush();
}

QString RecordingDirectory()
{
  return QFileInfo(Configuration::Instance().IniFileName()).absolutePath() + QStringLiteral("/agent-sessions");
}

bool CanRecord()
{
  return ConfigVault::Enabled();
}

bool SaveRecording(const QDateTime & Started, const QString & Site, const QString & Operation, const QString & Detail,
  const QByteArray & Output, bool Truncated, qint64 InputBytes, int ExitCode, const QString & Error)
{
  // output can hold anything a command printed: never as plain text on disk,
  // and a master password alone gives no key for it (CanRecord)
  if (!CanRecord())
  {
    return false;
  }
  QString Dir = RecordingDirectory();
  if (!QDir().mkpath(Dir))
  {
    return false;
  }
  Platform::MakeDirectoryPrivate(Dir);
  QString SafeSite = Site;
  SafeSite.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]+")), QStringLiteral("_"));
  QString Time = Started.toLocalTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"));
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
  // the role of this folder, or ReadFile refuses it; never plain text if there is no role
  QByteArray Sealed = ConfigVault::ForFile(Dir + QStringLiteral("/recording.log"), Text);
  if (!ConfigVault::IsSealed(Sealed))
  {
    return false;
  }
  // requests of the same millisecond get a file each ("site~2")
  QFile File;
  for (int Number = 1; ; Number++)
  {
    // Not "(Number == 1) ? QString() : ...": Apple clang 16 drops all that follows
    // from the second round of the loop for that temporary, and the program ends on a trap
    QString Suffix;
    if (Number > 1)
    {
      Suffix = QStringLiteral("~%1").arg(Number);
    }
    File.setFileName(QStringLiteral("%1/%2-%3%4-%5.log").arg(Dir, Time, SafeSite.left(60), Suffix, Operation));
    if (File.open(QIODevice::WriteOnly | QIODevice::NewOnly))
    {
      break;
    }
    if (!File.exists() || (Number >= 1000))
    {
      return false;
    }
  }
  Platform::MakePrivate(File);
  bool Written = (File.write(Sealed) == Sealed.size()) && File.flush();
  File.close();
  if (!Written)
  {
    File.remove();
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
  return Written;
}

QString PermissionsText(int Permissions)
{
  QStringList Parts;
  if (Permissions & apCommands) Parts.append(QStringLiteral("commands"));
  if (Permissions & apTransfer) Parts.append((Permissions & apDownloadOnly) ? QStringLiteral("downloads") : QStringLiteral("files"));
  if (Permissions & apTunnels) Parts.append(QStringLiteral("tunnels"));
  if (Permissions & apNoteWrite) Parts.append(QStringLiteral("note-edit"));
  else if (Permissions & apNoteRead) Parts.append(QStringLiteral("note"));
  return Parts.isEmpty() ? QStringLiteral("none") : Parts.join(QLatin1Char(','));
}

bool NoteAcceptable(const QString & Text, QString * Reason)
{
  auto Refuse = [Reason](const QString & Why) { if (Reason != nullptr) *Reason = Why; return false; };
  if (!ExactUtf8(Text))
  {
    return Refuse(QStringLiteral("The note must be valid Unicode text."));
  }
  if (Text.toUtf8().size() > MaxNoteBytes)
  {
    return Refuse(QStringLiteral("The note is too long: at most %1 KB.").arg(MaxNoteBytes / 1024));
  }
  for (QChar C : Text)
  {
    if (((C.unicode() < 0x20) && (C != QLatin1Char('\n')) && (C != QLatin1Char('\r')) && (C != QLatin1Char('\t'))) || (C.unicode() == 0x7F))
    {
      return Refuse(QStringLiteral("The note cannot contain control characters other than line breaks and tabs."));
    }
  }
  return true;
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

QStringList Patterns(const QString & Text);

QString SealFileName()
{
  return QFileInfo(Configuration::Instance().IniFileName()).absolutePath() + QStringLiteral("/nixscp-agent-seals");
}

// Every setting of a site as it is saved, values of keys this version does not
// know included: a field added later counts without anyone thinking of it. A
// changed host, proxy command, host key or cipher list would hand the stored
// password to someone else. Not the passwords, which a program cannot forge and
// which come out different each time, and not what only the panels use, which
// NixSCP itself changes (the last used folders).
QMap<QString, QString> SavedSettings(const SessionData & Data)
{
  SessionData Copy = Data;
  Copy.Password.clear();
  Copy.ProxyPassword.clear();
  Copy.TunnelPassword.clear();
  Copy.TunnelPassphrase.clear();
  IniFile File;
  IniStorage Storage(&File);
  Storage.OpenSubKey(QStringLiteral("Site"), true);
  Copy.Save(Storage);
  static const QSet<QString> PanelsOnly{QStringLiteral("Color"), QStringLiteral("Note"), QStringLiteral("LocalDirectory"),
    QStringLiteral("OtherLocalDirectory"), QStringLiteral("RemoteDirectory")};
  QMap<QString, QString> Result;
  for (const QString & Key : Storage.GetValueNames())
  {
    if (!PanelsOnly.contains(Key))
    {
      Result.insert(Key, Storage.ReadStringRaw(Key, QString()));
    }
  }
  return Result;
}

QByteArray SettingsDigest(const SessionData & Data)
{
  QStringList Fields{Data.Name};
  const QMap<QString, QString> Settings = SavedSettings(Data);
  for (auto It = Settings.cbegin(); It != Settings.cend(); ++It)
  {
    Fields << It.key() << It.value();
  }
  QByteArray Canonical;
  for (const QString & Field : Fields)
  {
    QByteArray Bytes = Field.toUtf8();
    Canonical += QByteArray::number(Bytes.size()) + ':' + Bytes;
  }
  return QCryptographicHash::hash(Canonical, QCryptographicHash::Sha256).toHex();
}

QByteArray SealKey(const SessionData & Data)
{
  return QCryptographicHash::hash(Data.Name.toUtf8(), QCryptographicHash::Sha256).toHex();
}

// The seal file: a line "<key> <seal>" per site
QHash<QByteArray, QByteArray> ReadSeals()
{
  QHash<QByteArray, QByteArray> Result;
  QFile File(SealFileName());
  if (File.open(QIODevice::ReadOnly))
  {
    for (const QByteArray & Line : File.readAll().split('\n'))
    {
      qsizetype Space = Line.indexOf(' ');
      if (Space > 0)
      {
        Result.insert(Line.left(Space), Line.mid(Space + 1));
      }
    }
  }
  return Result;
}

}

bool SettingsConfirmed(const SessionData & Data)
{
  // the encrypted file cannot be edited without the key
  if (ConfigVault::Enabled())
  {
    return true;
  }
  QByteArray Seal = ReadSeals().value(SealKey(Data));
  // only the master password makes such a value; plain obfuscation does not count
  if (Seal.isEmpty() || !Security::IsStronglyEncrypted(Seal))
  {
    return false;
  }
  Security::NoMasterPasswordPrompt NoPrompt;
  return Security::DecryptPassword(Seal, QString::fromLatin1(SealKey(Data))).toLatin1() == SettingsDigest(Data);
}

bool ConfirmSettings(const SessionData & Data)
{
  if (ConfigVault::Enabled())
  {
    return true;
  }
  QByteArray Seal = Security::EncryptPassword(QString::fromLatin1(SettingsDigest(Data)), QString::fromLatin1(SealKey(Data)));
  if (!Security::IsStronglyEncrypted(Seal))
  {
    return false;
  }
  QHash<QByteArray, QByteArray> Seals = ReadSeals();
  Seals.insert(SealKey(Data), Seal);
  QByteArray Text;
  for (auto It = Seals.cbegin(); It != Seals.cend(); ++It)
  {
    Text += It.key() + ' ' + It.value() + '\n';
  }
  QSaveFile File(SealFileName());
  if (!File.open(QIODevice::WriteOnly))
  {
    return false;
  }
  Platform::MakePrivate(File);
  return (File.write(Text) == Text.size()) && File.commit();
}

QString SettingsText(const SessionData & Data)
{
  QStringList Lines;
  Lines.append(QStringLiteral("Server: %1@%2:%3").arg(Data.UserName, Data.HostName, QString::number(Data.PortNumber)));
  Lines.append(QStringLiteral("Allowed: %1").arg(PermissionsText(Data.AgentPermissions)));
  Lines.append(QStringLiteral("Approval: %1").arg((Data.AgentAccess == amAsk) ? QStringLiteral("asks each time") : QStringLiteral("none")));
  if (!AgentSiteName(Data).isEmpty() && (AgentSiteName(Data) != Data.Name))
  {
    Lines.append(QStringLiteral("Name for agents: %1").arg(AgentSiteName(Data)));
  }
  Lines.append(QStringLiteral("Allowed commands: %1").arg(Patterns(Data.AgentAllow).isEmpty() ? QStringLiteral("all") : Patterns(Data.AgentAllow).join(QStringLiteral(", "))));
  if (!Patterns(Data.AgentDeny).isEmpty())
  {
    Lines.append(QStringLiteral("Denied commands: %1").arg(Patterns(Data.AgentDeny).join(QStringLiteral(", "))));
  }
  if (Data.Tunnel)
  {
    Lines.append(QStringLiteral("Through: %1@%2:%3").arg(Data.TunnelUserName, Data.TunnelHostName, QString::number(Data.TunnelPortNumber)));
  }
  if (Data.ProxyMethod != pmNone)
  {
    Lines.append(QStringLiteral("Proxy: %1:%2 %3").arg(Data.ProxyHost, QString::number(Data.ProxyPort), Data.ProxyCommand()));
  }
  // the seal covers them, so the user has to see them: a program that edited a
  // cipher list or a host key would otherwise get it accepted unseen
  static const QSet<QString> Shown{QStringLiteral("HostName"), QStringLiteral("PortNumber"), QStringLiteral("UserName"), QStringLiteral("AgentAccess"),
    QStringLiteral("AgentPermissions"), QStringLiteral("AgentName"), QStringLiteral("AgentAllow"), QStringLiteral("AgentDeny"), QStringLiteral("Tunnel"),
    QStringLiteral("TunnelHostName"), QStringLiteral("TunnelPortNumber"), QStringLiteral("TunnelUserName"), QStringLiteral("ProxyMethod"),
    QStringLiteral("ProxyHost"), QStringLiteral("ProxyPort"), QStringLiteral("ProxyTelnetCommand")};
  const QMap<QString, QString> Defaults = SavedSettings(SessionData());
  const QMap<QString, QString> Settings = SavedSettings(Data);
  for (auto It = Settings.cbegin(); It != Settings.cend(); ++It)
  {
    if (!Shown.contains(It.key()) && (Defaults.value(It.key()) != It.value()))
    {
      // (this text goes into the audit log too)
      bool Secret = It.key().contains(QStringLiteral("Token")) || It.key().contains(QStringLiteral("Secret")) ||
        It.key().contains(QStringLiteral("Encrypt"));
      Lines.append(QStringLiteral("%1: %2").arg(It.key(), Secret ? QStringLiteral("(set)") : It.value()));
    }
  }
  return Lines.join(QLatin1Char('\n'));
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

using PatternList = QList<QRegularExpression>;

PatternList Compile(const QStringList & Patterns, QRegularExpression::PatternOptions Options)
{
  PatternList Result;
  for (const QString & Pattern : Patterns)
  {
    Result.append(QRegularExpression(QRegularExpression::wildcardToRegularExpression(Pattern, QRegularExpression::NonPathWildcardConversion), Options));
  }
  return Result;
}

// PCRE can give up, and does on a few thousand characters under a pattern with
// three wildcards. hasMatch() is false then too, so a denied pattern would let
// through just what it could not read. With Unknown, that counts as a match, and
// *Unknown tells it was one of those
bool Matches(const PatternList & List, const QString & Text, bool * Unknown = nullptr)
{
  for (const QRegularExpression & Re : List)
  {
    if (!Re.isValid())
    {
      continue;
    }
    QRegularExpressionMatch Match = Re.match(Text);
    if (Match.hasMatch())
    {
      return true;
    }
    if (!Match.isValid() && (Unknown != nullptr))
    {
      *Unknown = true;
      return true;
    }
  }
  return false;
}

// More than anyone types; the check runs on the main thread
constexpr qsizetype MaxCheckedCommand = 64 * 1024;
// A line that takes longer is not one a person wrote, and the window waits for it
constexpr int MaxCheckMs = 1000;

// One simple command of a command line
struct SimpleCommand
{
  QStringList Words;      // with the quoting removed, without the redirections
  QStringList Targets;    // what the redirections read from and write to
  QString Raw;            // as written
};

// Reads a command line as a POSIX shell would, as far as the rules need it,
// and gives up at the first thing it cannot be sure about (Problem): a rule can
// only be checked against text that is known to be what the shell will run.
// That excludes everything whose meaning is only known on the server
// ($variables, wildcards as a program name, $(...)) or that re-reads text
// (here-documents, comments, line continuations), and anything else it does
// not model. New shell syntax must land in one of the refusals, never in a word.
class CommandParser
{
public:
  QList<SimpleCommand> Commands;
  bool WritesFile = false;    // a redirection writes to a file (other than /dev/null)
  QString Problem;            // not empty: the line was not understood, nothing else here counts

  explicit CommandParser(const QString & Line) : FLine(Line)
  {
    Parse();
  }

private:
  enum TRedirect { rdNone, rdRead, rdWrite, rdDuplicateIn, rdDuplicateOut };

  const QString & FLine;
  qsizetype FPos = 0;
  qsizetype FStart = 0;       // of the current simple command
  SimpleCommand FCommand;
  QString FWord;
  bool FInWord = false;
  qsizetype FQuotedFrom = -1; // where in FWord the first quoted text is
  bool FWildcard = false;     // FWord has an unquoted * ? or [
  bool FBothOutputs = false;  // the simple command has a &> or &>>
  TRedirect FRedirect = rdNone;

  QChar At(qsizetype Index) const
  {
    return (Index < FLine.size()) ? FLine.at(Index) : QChar();
  }

  void Refuse(const QString & Why)
  {
    if (Problem.isEmpty())
    {
      Problem = Why;
    }
  }

  void MarkQuoted()
  {
    FInWord = true;
    if (FQuotedFrom < 0)
    {
      FQuotedFrom = FWord.size();
    }
  }

  static bool AllDigits(const QString & Text)
  {
    return !Text.isEmpty() &&
      std::all_of(Text.begin(), Text.end(), [](QChar C) { return (C >= QLatin1Char('0')) && (C <= QLatin1Char('9')); });
  }

  static bool IsWordEnd(QChar C)
  {
    return C.isNull() || QStringLiteral(" \t\n;&|<>)").contains(C);
  }

  void CheckProgramWord()
  {
    static const QSet<QString> Keywords{QStringLiteral("if"), QStringLiteral("then"), QStringLiteral("elif"), QStringLiteral("else"),
      QStringLiteral("fi"), QStringLiteral("case"), QStringLiteral("esac"), QStringLiteral("for"), QStringLiteral("select"),
      QStringLiteral("while"), QStringLiteral("until"), QStringLiteral("do"), QStringLiteral("done"), QStringLiteral("in"),
      QStringLiteral("function"), QStringLiteral("time"), QStringLiteral("coproc"), QStringLiteral("!"), QStringLiteral("[["),
      QStringLiteral("]]"), QStringLiteral("repeat"), QStringLiteral("foreach")};
    if ((FQuotedFrom < 0) && Keywords.contains(FWord))
    {
      Refuse(QStringLiteral("Shell keywords such as \"%1\" are not allowed on this site. Run one simple command at a time.").arg(FWord));
    }
    if (FWildcard)
    {
      Refuse(QStringLiteral("A wildcard in the name of a program is not allowed on this site."));
    }
    // zsh expands =rm to the path of rm
    if (FWord.startsWith(QLatin1Char('=')) && (FQuotedFrom != 0))
    {
      Refuse(QStringLiteral("A program name that starts with = is not allowed on this site."));
    }
    // VAR=value before a program changes what the program does (LD_PRELOAD,
    // PATH, GIT_PAGER...); on its own it prepares a later $VAR
    qsizetype Equal = FWord.indexOf(QLatin1Char('='));
    if ((Equal > 0) && ((FQuotedFrom < 0) || (FQuotedFrom > Equal)))
    {
      static const QRegularExpression Name(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*\\+?$"));
      if (Name.match(FWord.left(Equal)).hasMatch())
      {
        Refuse(QStringLiteral("Setting variables is not allowed on this site."));
      }
    }
  }

  void EndRedirect()
  {
    FCommand.Targets.append(FWord);
    bool Descriptor = (FWord == QStringLiteral("-")) || AllDigits(FWord) ||
      (FWord.endsWith(QLatin1Char('-')) && AllDigits(FWord.chopped(1)));
    bool Null = (FWord == QStringLiteral("/dev/null"));
    if ((FRedirect == rdWrite) && !Null)
    {
      WritesFile = true;
    }
    else if ((FRedirect == rdDuplicateOut) && !Descriptor && !Null)
    {
      // ">&file" is the same as "> file 2>&1"
      WritesFile = true;
    }
    else if ((FRedirect == rdDuplicateIn) && !Descriptor)
    {
      Refuse(QStringLiteral("The redirection \"<&%1\" is not allowed on this site.").arg(FWord));
    }
    FRedirect = rdNone;
  }

  void EndWord()
  {
    if (!FInWord)
    {
      return;
    }
    if (FRedirect != rdNone)
    {
      EndRedirect();
    }
    else
    {
      if (FCommand.Words.isEmpty())
      {
        CheckProgramWord();
      }
      else if (FWildcard && FWord.startsWith(QLatin1Char('-')))
      {
        // the option that runs is the name of whatever file matches
        Refuse(QStringLiteral("A wildcard in an option (\"%1\") is not allowed on this site.").arg(FWord));
      }
      if (FBothOutputs)
      {
        // bash: more arguments. dash: & ended the command, and this one is a
        // new program: "ls &>/dev/null rm x"
        Refuse(QStringLiteral("Words after &> mean different things to different shells, which is not allowed on this site."));
      }
      FCommand.Words.append(FWord);
    }
    FWord.clear();
    FInWord = false;
    FQuotedFrom = -1;
    FWildcard = false;
  }

  void EndCommand()
  {
    EndWord();
    if (FRedirect != rdNone)
    {
      Refuse(QStringLiteral("A redirection without a target is not allowed on this site."));
    }
    if (!FCommand.Words.isEmpty() || !FCommand.Targets.isEmpty())
    {
      FCommand.Raw = FLine.mid(FStart, FPos - FStart).simplified();
      Commands.append(FCommand);
    }
    FCommand = SimpleCommand();
    FBothOutputs = false;
  }

  void Separator(int Extra)
  {
    EndCommand();
    FPos += Extra;
    FStart = FPos + 1;
  }

  void BeginRedirect(TRedirect Kind)
  {
    if (FRedirect != rdNone)
    {
      Refuse(QStringLiteral("A redirection without a target is not allowed on this site."));
    }
    FRedirect = Kind;
  }

  void Redirection(QChar C, QChar Next)
  {
    // "2>": a digit right before it is the descriptor, not a word. One digit
    // only: to dash more are an argument, and to bash when too large for a
    // descriptor, so "date 010100002000>/dev/null" sets the clock
    if (FInWord && (FQuotedFrom < 0) && (FWord.size() == 1) && AllDigits(FWord))
    {
      FWord.clear();
      FInWord = false;
    }
    else
    {
      EndWord();
    }
    if (Next == QLatin1Char('('))
    {
      Refuse(QStringLiteral("Process substitution is not allowed on this site."));
    }
    else if (C == QLatin1Char('<'))
    {
      if (Next == QLatin1Char('<'))
      {
        // the body of a here-document is lines the shell reads differently from
        // the commands around them
        Refuse(QStringLiteral("Here-documents (<<) are not allowed on this site."));
      }
      else if (Next == QLatin1Char('>'))
      {
        // "<>" creates the file, and opens it for writing
        FPos++;
        BeginRedirect(rdWrite);
      }
      else if (Next == QLatin1Char('&'))
      {
        FPos++;
        BeginRedirect(rdDuplicateIn);
      }
      else
      {
        BeginRedirect(rdRead);
      }
    }
    else if (Next == QLatin1Char('&'))
    {
      FPos++;
      BeginRedirect(rdDuplicateOut);
    }
    else
    {
      if ((Next == QLatin1Char('>')) || (Next == QLatin1Char('|')))
      {
        FPos++;
      }
      BeginRedirect(rdWrite);
    }
  }

  void Dollar(QChar Next, bool InQuotes)
  {
    if (Next == QLatin1Char('('))
    {
      Refuse(QStringLiteral("Command substitution ($(...) or `...`) is not allowed on this site."));
    }
    else if (!InQuotes && ((Next == QLatin1Char('\'')) || (Next == QLatin1Char('"'))))
    {
      // $'...' has quoting rules of its own: \' does not end it
      Refuse(QStringLiteral("$'...' and $\"...\" quoting is not allowed on this site."));
    }
    else if (Next.isLetterOrNumber() || QStringLiteral("_{[@*#?$!-").contains(Next))
    {
      // the value is only known on the server, and without quotes it splits
      // into words: find /srv $IFS-delete
      Refuse(QStringLiteral("Variables ($NAME) are not allowed on this site."));
    }
  }

  void DoubleQuoted()
  {
    MarkQuoted();
    for (FPos++; FPos < FLine.size(); FPos++)
    {
      QChar C = FLine.at(FPos);
      QChar Next = At(FPos + 1);
      if (C == QLatin1Char('"'))
      {
        return;
      }
      if (C == QLatin1Char('`'))
      {
        Refuse(QStringLiteral("Command substitution ($(...) or `...`) is not allowed on this site."));
      }
      else if (C == QLatin1Char('$'))
      {
        Dollar(Next, true);
      }
      else if ((C == QLatin1Char('\\')) && (Next == QLatin1Char('\n')))
      {
        Refuse(QStringLiteral("Line continuations are not allowed on this site."));
      }
      else if ((C == QLatin1Char('\\')) && !Next.isNull() && QStringLiteral("$`\"\\").contains(Next))
      {
        C = Next;
        FPos++;
      }
      FWord += C;
    }
    Refuse(QStringLiteral("The command has an unterminated quote."));
  }

  void Parse()
  {
    for (QChar C : FLine)
    {
      if (((C.unicode() < 0x20) && (C != QLatin1Char('\n')) && (C != QLatin1Char('\t'))) || (C.unicode() == 0x7F))
      {
        Refuse(QStringLiteral("Control characters in a command are not allowed on this site."));
        return;
      }
    }
    for (FPos = 0; (FPos < FLine.size()) && Problem.isEmpty(); FPos++)
    {
      QChar C = FLine.at(FPos);
      QChar Next = At(FPos + 1);
      switch (C.unicode())
      {
        case u' ':
        case u'\t':
          EndWord();
          break;
        case u'\n':
          Separator(0);
          break;
        case u';':
          if ((Next == QLatin1Char(';')) || (Next == QLatin1Char('&')))
          {
            Refuse(QStringLiteral("Shell keywords such as \"case\" are not allowed on this site. Run one simple command at a time."));
          }
          Separator(0);
          break;
        case u'&':
          if (Next == QLatin1Char('>'))
          {
            // &> and &>>: output and errors to a file
            EndWord();
            FPos += (At(FPos + 2) == QLatin1Char('>')) ? 2 : 1;
            FBothOutputs = true;
            BeginRedirect(rdWrite);
          }
          else if (Next == QLatin1Char('!'))
          {
            // zsh: one token, so "ls &!rm x" runs rm
            Refuse(QStringLiteral("&! means different things to different shells, which is not allowed on this site."));
          }
          else
          {
            Separator((Next == QLatin1Char('&')) ? 1 : 0);
          }
          break;
        case u'|':
          Separator(((Next == QLatin1Char('|')) || (Next == QLatin1Char('&'))) ? 1 : 0);
          break;
        case u'<':
        case u'>':
          Redirection(C, Next);
          break;
        case u'(':
        case u')':
          Refuse(QStringLiteral("Subshells and functions are not allowed on this site. Run one simple command at a time."));
          break;
        case u'{':
        case u'}':
          // {} is plain text (the placeholder of find and xargs); any other
          // brace can expand into several words or group commands
          if ((C == QLatin1Char('{')) && (Next == QLatin1Char('}')) && !FInWord && IsWordEnd(At(FPos + 2)))
          {
            FInWord = true;
            FWord += QStringLiteral("{}");
            FPos++;
          }
          else
          {
            Refuse(QStringLiteral("Braces are not allowed on this site unless quoted."));
          }
          break;
        case u'#':
          if (!FInWord)
          {
            // quotes in a comment mean nothing to the shell, and everything
            // after it depends on where a shell thinks it ends
            Refuse(QStringLiteral("Comments are not allowed on this site."));
          }
          FInWord = true;
          FWord += C;
          break;
        case u'`':
          Refuse(QStringLiteral("Command substitution ($(...) or `...`) is not allowed on this site."));
          break;
        case u'$':
          Dollar(Next, false);
          FInWord = true;
          FWord += C;
          break;
        case u'\'':
        {
          qsizetype End = FLine.indexOf(QLatin1Char('\''), FPos + 1);
          if (End < 0)
          {
            Refuse(QStringLiteral("The command has an unterminated quote."));
            break;
          }
          MarkQuoted();
          FWord += FLine.mid(FPos + 1, End - FPos - 1);
          FPos = End;
          break;
        }
        case u'"':
          DoubleQuoted();
          break;
        case u'\\':
          if (Next == QLatin1Char('\n'))
          {
            Refuse(QStringLiteral("Line continuations are not allowed on this site."));
          }
          else if (Next.isNull())
          {
            Refuse(QStringLiteral("The command ends with a backslash."));
          }
          else
          {
            MarkQuoted();
            FWord += Next;
            FPos++;
          }
          break;
        case u'*':
        case u'?':
        case u'[':
          FWildcard = true;
          Q_FALLTHROUGH();
        default:
          FInWord = true;
          FWord += C;
          break;
      }
    }
    if (Problem.isEmpty())
    {
      FPos = FLine.size();
      EndCommand();
    }
  }
};

QString JoinWords(const QStringList & Words)
{
  return Words.join(QLatin1Char(' ')).simplified();
}

QString ProgramName(const QString & Word)
{
  QString Name = Word.section(QLatin1Char('/'), -1);
  // zsh: =rm is the path of rm (CommandParser refuses it as the first word)
  return Name.startsWith(QLatin1Char('=')) ? Name.mid(1) : Name;
}

// The positions in Words that may name a program: the first and, as a denied
// command can hide behind what starts the program in its arguments ("sudo rm",
// "env rm", "xargs rm", "find -exec rm"...), the words after that
QList<qsizetype> ProgramWords(const QStringList & Words)
{
  static const QSet<QString> Names{QStringLiteral("sudo"), QStringLiteral("doas"), QStringLiteral("su"), QStringLiteral("env"),
    QStringLiteral("exec"), QStringLiteral("command"), QStringLiteral("builtin"), QStringLiteral("nohup"), QStringLiteral("nice"),
    QStringLiteral("ionice"), QStringLiteral("setsid"), QStringLiteral("stdbuf"), QStringLiteral("timeout"), QStringLiteral("busybox"),
    QStringLiteral("xargs"), QStringLiteral("watch"), QStringLiteral("flock"), QStringLiteral("chroot"), QStringLiteral("unshare"),
    QStringLiteral("nsenter"), QStringLiteral("strace"), QStringLiteral("ltrace"), QStringLiteral("noglob"), QStringLiteral("nocorrect"),
    QStringLiteral("time"), QStringLiteral("runuser"), QStringLiteral("pkexec"), QStringLiteral("setpriv"), QStringLiteral("taskset"),
    QStringLiteral("chrt"), QStringLiteral("toybox"), QStringLiteral("systemd-run"), QStringLiteral("sg"), QStringLiteral("script"),
    // zsh: "- rm x" starts rm
    QStringLiteral("-")};
  QList<qsizetype> Result;
  if (Words.isEmpty())
  {
    return Result;
  }
  Result.append(0);
  // (lower case: some servers have case-insensitive file systems)
  QString First = ProgramName(Words.first()).toLower();
  bool Running = Names.contains(First);
  for (qsizetype I = 1; I < Words.size(); I++)
  {
    // before that the words of find are paths and tests: "find . -name sh"
    Running = Running || ((First == QStringLiteral("find")) &&
      (Words.at(I - 1).startsWith(QStringLiteral("-exec")) || Words.at(I - 1).startsWith(QStringLiteral("-ok"))));
    if (Running)
    {
      Result.append(I);
    }
  }
  return Result;
}

// They read their commands from text this checker never sees: a shell, the
// builtins that make the shell itself do it, and what hands all it gets to a
// shell (su, watch...) or starts one that reads its standard input
bool RunsShellText(const QString & Program)
{
  static const QSet<QString> Names{QStringLiteral("sh"), QStringLiteral("bash"), QStringLiteral("dash"), QStringLiteral("ash"),
    QStringLiteral("ksh"), QStringLiteral("mksh"), QStringLiteral("zsh"), QStringLiteral("fish"), QStringLiteral("csh"),
    QStringLiteral("tcsh"), QStringLiteral("eval"), QStringLiteral("source"), QStringLiteral("."), QStringLiteral("alias"),
    QStringLiteral("trap"), QStringLiteral("su"), QStringLiteral("runuser"), QStringLiteral("sg"), QStringLiteral("watch"),
    QStringLiteral("script"), QStringLiteral("compgen"), QStringLiteral("complete"), QStringLiteral("mapfile"),
    QStringLiteral("readarray"), QStringLiteral("bind"), QStringLiteral("fc"), QStringLiteral("enable")};
  return Names.contains(Program.toLower());
}

// bash evaluates the subscript of an array name these get, $(...) included, in
// text that was quoted: let 'x[$(rm y)]=1', printf -v 'a[$(rm y)]' z. No plain
// string search around regular expressions: they can give up (see Matches)
bool EvaluatesSubscript(const QStringList & Words)
{
  static const QSet<QString> Names{QStringLiteral("let"), QStringLiteral("declare"), QStringLiteral("typeset"), QStringLiteral("local"),
    QStringLiteral("readonly"), QStringLiteral("export"), QStringLiteral("read"), QStringLiteral("printf"), QStringLiteral("test"),
    QStringLiteral("unset"), QStringLiteral("wait"), QStringLiteral("getopts")};
  if (!Names.contains(ProgramName(Words.first()).toLower()))
  {
    return false;
  }
  return std::any_of(Words.begin(), Words.end(), [](const QString & Word)
  {
    qsizetype Open = Word.indexOf(QLatin1Char('['));
    return (Open >= 0) && ((Word.indexOf(QLatin1Char('$'), Open) >= 0) || (Word.indexOf(QLatin1Char('`'), Open) >= 0));
  });
}

// Options that make a program run shell text, as those of Words: the program and
// what follows. A value can be attached, which to the checker is a single word
// nothing else looks into: su -c'rm x' is "-crm x", and env -S'rm x' likewise.
struct ShellOptions
{
  QString Letters;        // short options
  QString ValueLetters;   // other short options with a value, which is not an option
  QStringList Names;      // long options; any unique beginning works too
  int Operands;           // words the options may still come after (flock <file> -c)
};

bool HasShellOption(const QStringList & Words)
{
  static const QHash<QString, ShellOptions> Programs{
    {QStringLiteral("sudo"), {QStringLiteral("si"), QStringLiteral("ugCDhpRrtTU"), {QStringLiteral("--shell"), QStringLiteral("--login")}, 0}},
    {QStringLiteral("doas"), {QStringLiteral("s"), QStringLiteral("uC"), {}, 0}},
    {QStringLiteral("env"), {QStringLiteral("S"), QStringLiteral("uCa"), {QStringLiteral("--split-string")}, 0}},
    {QStringLiteral("flock"), {QStringLiteral("c"), QStringLiteral("wE"), {QStringLiteral("--command")}, 1}},
    // -o '|command'
    {QStringLiteral("strace"), {QStringLiteral("o"), QStringLiteral("abeEIPpSsUuX"), {QStringLiteral("--output")}, 0}}};
  auto Found = Programs.constFind(ProgramName(Words.first()).toLower());
  if (Found == Programs.constEnd())
  {
    return false;
  }
  int Operands = Found->Operands;
  for (qsizetype I = 1; I < Words.size(); I++)
  {
    const QString & Word = Words.at(I);
    if (Word == QStringLiteral("--"))
    {
      return false;
    }
    if (Word.startsWith(QStringLiteral("--")))
    {
      QString Name = Word.section(QLatin1Char('='), 0, 0);
      if (std::any_of(Found->Names.begin(), Found->Names.end(), [&Name](const QString & Long) { return Long.startsWith(Name); }))
      {
        return true;
      }
      // which long options have a value is not known here: the word after it is
      // not taken for the program, whose options must not count ("sudo du -sh")
      I += Word.contains(QLatin1Char('=')) ? 0 : 1;
    }
    else if (Word.startsWith(QLatin1Char('-')) && (Word.size() > 1))
    {
      for (qsizetype Letter = 1; Letter < Word.size(); Letter++)
      {
        if (Found->Letters.contains(Word.at(Letter)))
        {
          return true;
        }
        if (Found->ValueLetters.contains(Word.at(Letter)))
        {
          I += (Letter == Word.size() - 1) ? 1 : 0;
          break;
        }
      }
    }
    else if (Operands-- <= 0)
    {
      return false;
    }
  }
  return false;
}

// Not QString::trimmed(), which takes control characters and Unicode spaces too:
// to the shell those are parts of a word
QString ShellTrimmed(const QString & Line)
{
  auto Blank = [](QChar C) { return (C == QLatin1Char(' ')) || (C == QLatin1Char('\t')) || (C == QLatin1Char('\n')); };
  qsizetype Start = 0;
  qsizetype End = Line.size();
  while ((Start < End) && Blank(Line.at(Start)))
  {
    Start++;
  }
  while ((End > Start) && Blank(Line.at(End - 1)))
  {
    End--;
  }
  return Line.mid(Start, End - Start);
}

}

bool HasCommandRules(const SessionData & Data)
{
  return !Patterns(Data.AgentAllow).isEmpty() || !Patterns(Data.AgentDeny).isEmpty();
}

QString InvalidPattern(const QString & Rules)
{
  for (const QString & Pattern : Patterns(Rules))
  {
    if (!Compile({Pattern}, QRegularExpression::NoPatternOption).first().isValid())
    {
      return Pattern;
    }
  }
  return QString();
}

bool CommandAllowed(const SessionData & Data, const QString & Command, QString * Reason)
{
  auto Refuse = [Reason](const QString & Why) { if (Reason != nullptr) *Reason = Why; return false; };
  // refused with or without rules: the server would get the text up to the
  // NUL only, not what was checked, shown and logged
  if (Command.contains(QChar(0)))
  {
    return Refuse(QStringLiteral("A command cannot contain a NUL character."));
  }
  // the same for anything else the bytes for the server lose: everything below
  // reads this text, so it has to be those bytes
  if (!ExactUtf8(Command))
  {
    return Refuse(QStringLiteral("A command must be valid Unicode text: no lone surrogates, no byte order mark at the start."));
  }
  QStringList AllowList = Patterns(Data.AgentAllow);
  QStringList DenyList = Patterns(Data.AgentDeny);
  if (AllowList.isEmpty() && DenyList.isEmpty())
  {
    return true;
  }
  QString BadDeny = InvalidPattern(Data.AgentDeny);
  if (!BadDeny.isEmpty())
  {
    // a pattern that cannot match must not read as "nothing is denied"
    return Refuse(QStringLiteral("The denied pattern \"%1\" of this site is not valid, so no command can be checked. "
      "The user has to correct it in NixSCP.").arg(BadDeny));
  }
  if (Command.size() > MaxCheckedCommand)
  {
    return Refuse(QStringLiteral("The command is too long to be checked against the rules of this site."));
  }
  // Deny is case-insensitive: some servers have case-insensitive file systems
  PatternList Deny = Compile(DenyList, QRegularExpression::CaseInsensitiveOption);
  PatternList Allow = Compile(AllowList, QRegularExpression::NoPatternOption);
  QDeadlineTimer Time(MaxCheckMs);
  // a denied pattern that gave no answer counts as a match
  bool Unknown = false;
  auto IsDenied = [&Deny, &Unknown](const QString & Text) { return Matches(Deny, Text, &Unknown); };
  auto Denied = [&Unknown](const QString & What)
  {
    return Unknown ? QStringLiteral("The command is too complex to be checked against the rules of this site.") :
      QStringLiteral("\"%1\" is denied on this site.").arg(What);
  };
  QString Whole = Command.simplified();
  if (Whole.isEmpty())
  {
    return Refuse(QStringLiteral("Empty command."));
  }
  if (IsDenied(Whole))
  {
    return Refuse(Denied(Whole));
  }
  // the user spelled out this very line: the one way past the parser
  QString Exact = ShellTrimmed(Command);
  for (const QString & Pattern : AllowList)
  {
    if ((Pattern == Exact) && !Pattern.contains(QRegularExpression(QStringLiteral("[*?\\[]"))))
    {
      return true;
    }
  }
  CommandParser Parser(Command);
  if (!Parser.Problem.isEmpty())
  {
    return Refuse(Parser.Problem);
  }
  if (Parser.Commands.isEmpty())
  {
    return Refuse(QStringLiteral("Empty command."));
  }
  if (!Allow.isEmpty() && Parser.WritesFile)
  {
    return Refuse(QStringLiteral("Redirecting output to a file is not allowed on this site."));
  }
  // an allowed pattern that names the program, unlike a bare "*"
  QStringList NamingList = AllowList.filter(QRegularExpression(QStringLiteral("[^*]")));
  PatternList Naming = Compile(NamingList, QRegularExpression::NoPatternOption);
  QString TooComplex = QStringLiteral("The command is too complex to be checked against the rules of this site.");
  for (const SimpleCommand & Simple : Parser.Commands)
  {
    if (Time.hasExpired())
    {
      return Refuse(TooComplex);
    }
    // for denied patterns the more alike the better; an allowed pattern gets the
    // words as they are, as "git status" is not git\u00a0status or 'a  b' two words
    QString Text = JoinWords(Simple.Words);
    QString Words = Simple.Words.join(QLatin1Char(' '));
    if (IsDenied(Text) || IsDenied(Simple.Raw) || IsDenied(JoinWords(Simple.Words + Simple.Targets)))
    {
      return Refuse(Denied(Simple.Raw));
    }
    bool Named = Matches(Naming, Words);
    for (qsizetype I : ProgramWords(Simple.Words))
    {
      if (Time.hasExpired())
      {
        return Refuse(TooComplex);
      }
      QStringList Tail = Simple.Words.mid(I);
      QString Program = ProgramName(Tail.first());
      QString Tailed = JoinWords(Tail);
      bool ShellText = RunsShellText(Program) || HasShellOption(Tail);
      bool Subscript = EvaluatesSubscript(Tail);
      Tail.first() = Program;
      QString Bare = JoinWords(Tail);
      // xargs and find add arguments, so "rm *" also denies a bare "rm" there
      if (IsDenied(Tailed) || IsDenied(Bare) || ((I > 0) && IsDenied(Bare + QLatin1Char(' '))))
      {
        return Refuse(Denied(Simple.Raw));
      }
      if (ShellText && !Named)
      {
        return Refuse(QStringLiteral("\"%1\" runs commands NixSCP cannot check, which is not allowed on this site "
          "unless an allowed pattern names it.").arg(Simple.Raw));
      }
      // whoever allows printf * does not mean this
      if (Subscript)
      {
        return Refuse(QStringLiteral("\"%1\" has an array subscript the shell evaluates, which is not allowed on this site.").arg(Simple.Raw));
      }
    }
    if (!Allow.isEmpty() && !Matches(Allow, Words))
    {
      return Refuse(QStringLiteral("\"%1\" is not among the commands allowed on this site.").arg(Simple.Raw));
    }
    // a pattern sees one line of text: 'cat /x' reads like cat with an argument,
    // and is a program with a space in its path
    if (!Allow.isEmpty() && !Simple.Words.isEmpty() &&
        std::any_of(Simple.Words.first().begin(), Simple.Words.first().end(), [](QChar C) { return C.isSpace(); }))
    {
      return Refuse(QStringLiteral("A program name with spaces is only allowed on this site by a pattern equal to the whole command."));
    }
  }
  return true;
}

QString ReadOnlyAllowPreset()
{
  // Each line was read for options that write or run something. Programs that
  // have such options (sort -o, uniq in out, date -s, hostname name, ip addr
  // add, systemctl --failed restart ...) get their harmless forms spelled out
  // instead of a trailing *, or are covered by ReadOnlyDenyPreset
  return QStringLiteral(
    "ls\nls *\npwd\ncat *\nhead *\ntail *\ngrep *\negrep *\nzgrep *\nwc\nwc *\ncut *\nstat *\nfile *\nfind *\n"
    "sort\nsort -n\nsort -r\nsort -u\nsort -h\nsort -V\nsort -f\nsort -rn\nsort -nr\nsort -rh\nsort -hr\nsort -nu\nsort -ru\n"
    "sort -k[1-9]\nsort -nk[1-9]\nsort -rk[1-9]\nsort -rnk[1-9]\nsort -nrk[1-9]\n"
    "uniq\nuniq -c\nuniq -d\nuniq -u\nuniq -i\nuniq -ci\nuniq -ic\n"
    "du\ndu *\ndf\ndf *\nfree\nfree *\nuptime\nuptime *\nuname\nuname *\n"
    "hostname\nhostname -f\nhostname -s\nhostname -d\nhostname -i\nhostname -I\nhostname -A\nwhoami\nid\nid *\n"
    "date\ndate -u\ndate -R\ndate -I\ndate -Iseconds\ndate +*\ndate -u +*\n"
    "ps\nps *\ntop -bn1\ntop -bn1 *\nlsblk\nlsblk *\nss -*\nnetstat\nnetstat *\n"
    "ip a\nip addr\nip a show*\nip addr show*\nip -br a\nip -br addr\nip -4 a\nip -6 a\nip -4 addr\nip -6 addr\n"
    "ip r\nip route\nip r show*\nip route show*\nip route get *\nip link\nip link show*\nip -br link\n"
    "systemctl status *\nsystemctl is-active *\nsystemctl is-enabled *\nsystemctl list-units\nsystemctl list-units *\n"
    "systemctl --failed\nsystemctl --failed --no-pager\n"
    "journalctl\njournalctl *\ndocker ps\ndocker ps *\ndocker logs *\ndocker images\ndocker images *\ndocker inspect *\n"
    "git status\ngit status *\ngit log\ngit log *\ngit diff\ngit diff *\n"
    "nginx -t\napachectl -t\necho\necho *\nwhich *");
}

QString ReadOnlyDenyPreset()
{
  // Options of the allowed programs that write files, delete data or run other
  // programs. GNU long options may be shortened, so the shortest unique form counts
  return QStringLiteral(
    "* -delete*\n* -exec*\n* -fprint*\nfind* -ok*\nfind* -fls*\n"
    "date * -*\n"
    "file *-*C*\nfile *--c*\n"
    "ss *-*D*\nss *-*K*\nss *--d*\nss *--k*\n"
    "journalctl *--ro*\njournalctl *--va*\njournalctl *--fl*\njournalctl *--rel*\njournalctl *--sm*\n"
    "journalctl *--se*\njournalctl *--syn*\njournalctl *--up*\njournalctl *--cu*\n"
    "git *--ou*\ngit *--ext*");
}

QString AgentSiteName(const SessionData & Data)
{
  QString Name = Data.AgentName.trimmed();
  return Name.isEmpty() ? Data.Name : Name;
}

bool UsableByAgents(const SessionData & Data)
{
  // not "!= amOff": a mode this version does not know must not run unasked
  // a note item has no server, but its note can be for agents
  return ((Data.AgentAccess == amAllow) || (Data.AgentAccess == amAsk)) && (Data.IsSsh() || Data.NoteOnly) && !Data.IsWorkspace;
}

std::shared_ptr<SessionData> FindAgentSite(const StoredSessionList & Sites, const QString & Name, bool * Ambiguous)
{
  std::shared_ptr<SessionData> Result;
  if (Ambiguous != nullptr)
  {
    *Ambiguous = false;
  }
  for (const auto & Data : Sites.Sessions())
  {
    if (UsableByAgents(*Data) && (AgentSiteName(*Data) == Name))
    {
      if (Result != nullptr)
      {
        // commands must not land on whichever of two servers happens to come first
        if (Ambiguous != nullptr)
        {
          *Ambiguous = true;
        }
        return nullptr;
      }
      Result = Data;
    }
  }
  return Result;
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
      (commands, files, tunnels, note). Use the exact site name with the
      commands below.

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

  nixscp --note <site>
      Prints the site's note: what the user wrote down about the server, for
      example where its logs are or what not to restart. Only for sites whose
      note the user shares with agents (permission "note" or "note-edit").

  nixscp --note-set <site> [<file>]
      Replaces the site's note with the text of <file>, or with standard input
      (at most 64 KB). Only with permission "note-edit". Read the note first
      and keep what is still true: this replaces the whole note.

  nixscp --mcp
      Runs as a Model Context Protocol (MCP) server on standard input/output,
      with the tools list_sites, run_command, download_file, upload_file,
      read_note and write_note.
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
  - Commands are written to the agent access log as you send them, and on
    some sites recorded with their output: do not put secrets in a command.
  - Sites with command rules refuse what they cannot check: $variables, $(...),
    comments, here-documents, line continuations, VAR=value prefixes, keywords
    (if, for...), subshells and braces. Run plain commands joined with ; && ||
    or |, and quote text containing such characters in single quotes.

EXIT CODES
  0-254  exit code of the remote command (--connect), 0 = success (--get, --put)
  255    NixSCP error: not running, locked, site not allowed, approval denied,
         connection or authentication failure, or the server did not report
         an exit status (the message explains it)

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

DownloadFile::DownloadFile(const QString & FileName) : FFileName(FileName)
{
}

DownloadFile::~DownloadFile()
{
  Cancel();
}

bool DownloadFile::Open()
{
  QFileInfo Info(FFileName);
  if (Info.exists() && !Info.isFile() && !Info.isDir())
  {
    FFile = std::make_unique<QFile>(FFileName);
  }
  else
  {
    FFile = std::make_unique<QSaveFile>(FFileName);
  }
  return FFile->open(QIODevice::WriteOnly);
}

bool DownloadFile::Write(const QByteArray & Data)
{
  return (FFile != nullptr) && (FFile->write(Data) == Data.size());
}

bool DownloadFile::Commit()
{
  if (FFile == nullptr)
  {
    return false;
  }
  auto * Save = qobject_cast<QSaveFile *>(FFile.get());
  // also reports what could only be written now: the disk filled up
  bool Ok = (Save != nullptr) ? Save->commit() : FFile->flush();
  if (Ok)
  {
    FFile.reset();
  }
  return Ok;
}

void DownloadFile::Cancel()
{
  if (auto * Save = qobject_cast<QSaveFile *>(FFile.get()))
  {
    Save->cancelWriting();
  }
  FFile.reset();
}

QString DownloadFile::ErrorString() const
{
  return (FFile != nullptr) ? FFile->errorString() : QString();
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
