// Port of WinSCP source/core/Script.cpp (TScript, TManagementScript)
#include "cli/Script.h"
#include "core/Platform.h"
#include "cli/ConsoleUserInterface.h"
#include "cli/ScriptHelp.h"
#include "core/FtpFileSystem.h"
#include "core/IniStorage.h"
#include "core/S3FileSystem.h"
#include "core/ScpFileSystem.h"
#include "core/SftpFileSystem.h"
#include "core/SiteImport.h"
#include "core/SecureShell.h"
#include "core/Configuration.h"
#include "core/SessionLog.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QLocale>
#include <QMutexLocker>
#include <QTemporaryFile>
#include <QTimer>
#include <QUrl>

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#ifdef Q_OS_WIN
#include <io.h>
#endif
#include <algorithm>
#include <cstdio>

#include "core/Synchronize.h"

namespace
{
const QStringList ToggleNames{QStringLiteral("off"), QStringLiteral("on")};
const QStringList TransferModeNames{QStringLiteral("binary"), QStringLiteral("ascii"), QStringLiteral("automatic")};
// keep in sync with Session constructor in .NET
const int BatchSessionReopenTimeout = 2 * 60 * 1000;
const int BatchInputTimeout = 10000;
const int ScriptProgressFileNameLimit = 25;
const char * const EngShortMonthNames[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

QString UnixExtractFilePath(const QString & Path)
{
  qsizetype P = Path.lastIndexOf(QLatin1Char('/'));
  return (P >= 0) ? Path.left(P + 1) : QString();
}

QString UnixExtractFileName(const QString & Path)
{
  return Path.mid(Path.lastIndexOf(QLatin1Char('/')) + 1);
}

QString UnixExcludeTrailingBackslash(const QString & Path)
{
  QString Result = Path;
  while ((Result.size() > 1) && Result.endsWith(QLatin1Char('/')))
  {
    Result.chop(1);
  }
  return Result;
}

QString UnixIncludeTrailingBackslash(const QString & Path)
{
  return Path.endsWith(QLatin1Char('/')) ? Path : (Path + QLatin1Char('/'));
}

QString UnixParentPath(const QString & Path)
{
  QString P = UnixExcludeTrailingBackslash(Path);
  qsizetype Slash = P.lastIndexOf(QLatin1Char('/'));
  return (Slash <= 0) ? QStringLiteral("/") : P.left(Slash);
}

// Local paths (ExtractFilePath/ExtractFileName for '/' separators)
QString LocalExtractFilePath(const QString & Path)
{
  return UnixExtractFilePath(Path);
}

QString LocalExtractFileName(const QString & Path)
{
  return UnixExtractFileName(Path);
}

// FindFirstFile semantics of local paths, as WinSCP: '*' and '?' are the only
// wildcards, "a[1].txt" is a file name (and '[' a character in a mask)
bool IsLocalMask(const QString & Mask)
{
  return Mask.contains(QLatin1Char('*')) || Mask.contains(QLatin1Char('?'));
}

QString LocalMask(QString Mask)
{
  return Mask.replace(QLatin1Char('['), QStringLiteral("[[]"));
}

// ExtractCommonPath / UnixExtractCommonPath: the directory of all the files
QString CommonPath(const QStringList & Files)
{
  QString Result = Files.isEmpty() ? QString() : UnixExtractFilePath(Files.first());
  for (const QString & File : Files)
  {
    while (!Result.isEmpty() && !File.startsWith(Result))
    {
      Result = UnixExtractFilePath(UnixExcludeTrailingBackslash(Result));
    }
  }
  return Result;
}

QString LocalAbsolutePath(const QString & Path)
{
  QString Result = Path;
  if (Result.startsWith(QStringLiteral("~/")) || (Result == QStringLiteral("~")))
  {
    Result = QDir::homePath() + Result.mid(1);
  }
  return QDir::cleanPath(QDir::current().absoluteFilePath(Result));
}

QString ModificationStr(const QDateTime & DateTime)
{
  // ModificationStr(mfFull); mfNone (TRemoteDirectoryFile, e.g. the
  // parent directory entry of an FTP listing) gives an empty string
  if (!DateTime.isValid())
  {
    return QString();
  }
  QDateTime T = DateTime.toLocalTime();
  QDate D = T.date();
  QTime Tm = T.time();
  return QString::asprintf("%3s %2d %2d:%02d:%02d %4d", EngShortMonthNames[std::clamp(D.month(), 1, 12) - 1], D.day(),
    Tm.hour(), Tm.minute(), Tm.second(), D.year());
}

void SetLocalModificationTime(const QString & Path, const QDateTime & Time)
{
  Platform::SetFileTimes(Path, Time.toSecsSinceEpoch());
}

bool IsRealFile(const QString & FileName)
{
  return (FileName != QStringLiteral(".")) && (FileName != QStringLiteral(".."));
}

QString RemoveEmptyLines(const QString & Str)
{
  QStringList Lines = Str.split(QLatin1Char('\n'));
  QStringList Result;
  for (const QString & Line : Lines)
  {
    if (!Line.trimmed().isEmpty())
    {
      Result.append(Line);
    }
  }
  return Result.join(QLatin1Char('\n'));
}

// The shell checksum commands of TTerminal::GetShellChecksumAlgDefs
// (Configuration ChecksumCommands default)
QString ShellChecksumCommand(const QString & Alg)
{
  static const char * const Defs[][2] = {
    {"sha-512", "sha512sum"}, {"sha-384", "sha384sum"}, {"sha-256", "sha256sum"},
    {"sha-224", "sha224sum"}, {"sha-1", "sha1sum"}, {"md5", "md5sum"}};
  QString Normalized = Alg;
  Normalized.remove(QLatin1Char('-'));
  for (const auto & Def : Defs)
  {
    QString Name = QString::fromLatin1(Def[0]);
    if ((Name.compare(Alg, Qt::CaseInsensitive) == 0) ||
        (QString(Name).remove(QLatin1Char('-')).compare(Normalized, Qt::CaseInsensitive) == 0))
    {
      return QString::fromLatin1(Def[1]);
    }
  }
  return QString();
}

}

ScriptProcParams::ScriptProcParams(const QString & AFullCommand, const QString & AParamsStr) :
  FullCommand(AFullCommand),
  ParamsStr(AParamsStr)
{
  SetSwitchMarks(QStringLiteral("-"));
  Parse(AParamsStr);
}

// TScript::SetSynchronizeParams
static const int AcceptedSynchronizeParams = spExistingOnly | spTimestamp | spNotByTime | spBySize | spByChecksum | spCaseSensitive;

Script::Script(StoredSessionList * StoredSessions) :
  FStoredSessions(StoredSessions),
  // Not 0: comparing case sensitively is the right thing on a file system that is
  FSynchronizeParams(spDefault & AcceptedSynchronizeParams)
{
  FSessionReopenTimeout = BatchSessionReopenTimeout;
  FInteractiveSessionReopenTimeout = 0;

  // TScript::Init
  Register(QStringLiteral("help"), QStringLiteral("Displays help"), SCRIPT_HELP_HELP, &Script::HelpProc, 0, -1, false);
  Register(QStringLiteral("man"), QString(), SCRIPT_HELP_HELP, &Script::HelpProc, 0, -1, false);
  Register(QStringLiteral("call"), QStringLiteral("Executes arbitrary remote command"), SCRIPT_CALL_HELP2, &Script::CallProc, 1, -1, true);
  Register(QStringLiteral("!"), QString(), SCRIPT_CALL_HELP2, &Script::CallProc, 1, -1, true);
  Register(QStringLiteral("pwd"), QStringLiteral("Prints remote working directory"), SCRIPT_PWD_HELP, &Script::PwdProc, 0, 0, false);
  Register(QStringLiteral("cd"), QStringLiteral("Changes remote working directory"), SCRIPT_CD_HELP, &Script::CdProc, 0, 1, false);
  Register(QStringLiteral("ls"), QStringLiteral("Lists the contents of remote directory"), SCRIPT_LS_HELP2, &Script::LsProc, 0, 1, false);
  Register(QStringLiteral("dir"), QString(), SCRIPT_LS_HELP2, &Script::LsProc, 0, 1, false);
  Register(QStringLiteral("rm"), QStringLiteral("Removes remote file"), SCRIPT_RM_HELP2, &Script::RmProc, 1, -1, true);
  Register(QStringLiteral("rmdir"), QStringLiteral("Removes remote directory"), SCRIPT_RMDIR_HELP, &Script::RmDirProc, 1, -1, false);
  Register(QStringLiteral("mv"), QStringLiteral("Moves or renames remote file"), SCRIPT_MV_HELP2, &Script::MvProc, 2, -1, false);
  Register(QStringLiteral("rename"), QString(), SCRIPT_MV_HELP2, &Script::MvProc, 2, -1, false);
  Register(QStringLiteral("cp"), QStringLiteral("Duplicates remote file"), SCRIPT_CP_HELP, &Script::CpProc, 2, -1, false);
  Register(QStringLiteral("chmod"), QStringLiteral("Changes permissions of remote file"), SCRIPT_CHMOD_HELP2, &Script::ChModProc, 2, -1, false);
  Register(QStringLiteral("ln"), QStringLiteral("Creates remote symbolic link"), SCRIPT_LN_HELP, &Script::LnProc, 2, 2, false);
  Register(QStringLiteral("symlink"), QString(), SCRIPT_LN_HELP, &Script::LnProc, 2, 2, false);
  Register(QStringLiteral("mkdir"), QStringLiteral("Creates remote directory"), SCRIPT_MKDIR_HELP, &Script::MkDirProc, 1, 1, false);
  Register(QStringLiteral("get"), QStringLiteral("Downloads file from remote directory to local directory"), SCRIPT_GET_HELP8, &Script::GetProc, 0, -1, true);
  Register(QStringLiteral("recv"), QString(), SCRIPT_GET_HELP8, &Script::GetProc, 0, -1, true);
  Register(QStringLiteral("mget"), QString(), SCRIPT_GET_HELP8, &Script::GetProc, 0, -1, true);
  Register(QStringLiteral("put"), QStringLiteral("Uploads file from local directory to remote directory"), SCRIPT_PUT_HELP8, &Script::PutProc, 0, -1, true);
  Register(QStringLiteral("send"), QString(), SCRIPT_PUT_HELP8, &Script::PutProc, 0, -1, true);
  Register(QStringLiteral("mput"), QString(), SCRIPT_PUT_HELP8, &Script::PutProc, 0, -1, true);
  Register(QStringLiteral("option"), QStringLiteral("Sets or shows value of script options"), SCRIPT_OPTION_HELP7, &Script::OptionProc, -1, 2, false);
  Register(QStringLiteral("ascii"), QString(), SCRIPT_OPTION_HELP7, &Script::AsciiProc, 0, 0, false);
  Register(QStringLiteral("binary"), QString(), SCRIPT_OPTION_HELP7, &Script::BinaryProc, 0, 0, false);
  Register(QStringLiteral("synchronize"), QStringLiteral("Synchronizes remote directory with local one"), SCRIPT_SYNCHRONIZE_HELP7, &Script::SynchronizeProc, 0, -1, true);
  Register(QStringLiteral("keepuptodate"), QStringLiteral("Continuously reflects changes in local directory on remote one"), SCRIPT_KEEPUPTODATE_HELP5, &Script::KeepUpToDateProc, 0, 2, true);
  Register(QStringLiteral("echo"), QStringLiteral("Displays its arguments as message"), SCRIPT_ECHO_HELP, &Script::EchoProc, -1, -1, true);
  Register(QStringLiteral("stat"), QStringLiteral("Retrieves attributes of remote file"), SCRIPT_STAT_HELP, &Script::StatProc, 1, 1, false);
  Register(QStringLiteral("checksum"), QStringLiteral("Calculates checksum of remote file"), SCRIPT_CHECKSUM_HELP, &Script::ChecksumProc, 2, 2, false);
  Register(QStringLiteral("copyid"), QString(), "", &Script::CopyIdProc, 1, 1, false);
  // TManagementScript
  Register(QStringLiteral("exit"), QStringLiteral("Closes all sessions and terminates the program"), SCRIPT_EXIT_HELP, &Script::ExitProc, 0, 0, false);
  Register(QStringLiteral("bye"), QString(), SCRIPT_EXIT_HELP, &Script::ExitProc, 0, 0, false);
  Register(QStringLiteral("open"), QStringLiteral("Connects to server"), SCRIPT_OPEN_HELP11, &Script::OpenProc, 0, -1, true);
  Register(QStringLiteral("close"), QStringLiteral("Closes session"), SCRIPT_CLOSE_HELP, &Script::CloseProc, 0, 1, false);
  Register(QStringLiteral("session"), QStringLiteral("Lists connected sessions or selects active session"), SCRIPT_SESSION_HELP, &Script::SessionProc, 0, 1, false);
  Register(QStringLiteral("lpwd"), QStringLiteral("Prints local working directory"), SCRIPT_LPWD_HELP, &Script::LPwdProc, 0, 0, false);
  Register(QStringLiteral("lcd"), QStringLiteral("Changes local working directory"), SCRIPT_LCD_HELP, &Script::LCdProc, 1, 1, false);
  Register(QStringLiteral("lls"), QStringLiteral("Lists the contents of local directory"), SCRIPT_LLS_HELP2, &Script::LLsProc, 0, 1, false);

  // TScriptCommands is a sorted case-insensitive list
  std::sort(FCommands.begin(), FCommands.end(),
    [](const CommandDef & A, const CommandDef & B) { return A.Name.compare(B.Name, Qt::CaseInsensitive) < 0; });
}

Script::~Script()
{
  // TManagementScript::~TManagementScript: terminals are freed without messages
  FSession = nullptr;
  FSessions.clear();
  Console::Instance().FinishProgress();
}

void Script::Register(const QString & Name, const QString & Description, const char * Help, CommandProc Proc,
  int MinParams, int MaxParams, bool Switches)
{
  FCommands.push_back(CommandDef{Name, Description, Help, Proc, MinParams, MaxParams, Switches});
}

void Script::SetSynchronizeParams(int Value)
{
  FSynchronizeParams = (Value & AcceptedSynchronizeParams);
  FWarnNonDefaultSynchronizeParams = (FSynchronizeParams != (spDefault & AcceptedSynchronizeParams));
}

void Script::CheckDefaultSynchronizeParams()
{
  if (FWarnNonDefaultSynchronizeParams)
  {
    // SCRIPT_NON_DEFAULT_SYNC_PARAM
    PrintLine(QStringLiteral("Using configured synchronization options different from factory defaults."));
    FWarnNonDefaultSynchronizeParams = false;
  }
}

void Script::SetCopyParam(const CopyParam & Value)
{
  FCopyParam = Value;
  FWarnNonDefaultCopyParam = (FCopyParam != CopyParam());
}

void Script::SetConfigurationReopenTimeout(int Value)
{
  // TScript::Init
  FSessionReopenTimeout = Value;
  FInteractiveSessionReopenTimeout = FSessionReopenTimeout;
  if (FSessionReopenTimeout == 0)
  {
    FSessionReopenTimeout = BatchSessionReopenTimeout;
  }
}

QString Script::ExpandLogFileName(const QString & FileName, const SessionData * Data)
{
  // GetExpandedLogFileName (core/SessionInfo.cpp)
  QString Result;
  QDateTime N = QDateTime::currentDateTime();
  for (qsizetype I = 0; I < FileName.size(); I++)
  {
    QChar C = FileName[I];
    if ((C == QLatin1Char('!')) && (I + 1 < FileName.size()))
    {
      QChar Pattern = FileName[I + 1].toUpper();
      QString Replacement;
      bool Known = true;
      switch (Pattern.toLatin1())
      {
        case 'Y': Replacement = N.toString(QStringLiteral("yyyy")); break;
        case 'M': Replacement = N.toString(QStringLiteral("MM")); break;
        case 'D': Replacement = N.toString(QStringLiteral("dd")); break;
        case 'T': Replacement = N.toString(QStringLiteral("HHmmss")); break;
        case 'P': Replacement = QString::number(QCoreApplication::applicationPid()); break;
        case '@':
          Replacement = (Data != nullptr) ? Data->HostName : QStringLiteral("nohost");
          break;
        case 'S':
          Replacement = (Data != nullptr) ? Data->SessionName() : QStringLiteral("nosession");
          break;
        case '!': Replacement = QStringLiteral("!"); break;
        case '-': Replacement = QString(); break;
        default: Known = false; break;
      }
      if (Known)
      {
        // file name safe (no path separators from session names)
        if ((Pattern == QLatin1Char('@')) || (Pattern == QLatin1Char('S')))
        {
          Replacement.replace(QLatin1Char('/'), QLatin1Char('_'));
        }
        Result += Replacement;
        I++;
        continue;
      }
    }
    Result += C;
  }
  return Result;
}

QString Script::MinimizeName(const QString & FileName, int MaxLen, bool Unix)
{
  // MinimizeName + CutFirstDirectory (core/RemoteFiles.cpp); local paths are
  // POSIX paths on Linux too, so only the separator logic of Unix applies
  Q_UNUSED(Unix);
  const QString Sep = QStringLiteral("/");
  const QString Ellipsis = QStringLiteral("...");
  QString Dir;
  QString Name;
  qsizetype P = FileName.lastIndexOf(QLatin1Char('/'));
  if (P >= 0)
  {
    Dir = FileName.left(P + 1);
    Name = FileName.mid(P + 1);
  }
  else
  {
    Name = FileName;
  }
  QString Result = FileName;
  while (!Dir.isEmpty() && (Result.size() > MaxLen))
  {
    if (Dir == Sep + Ellipsis + Sep)
    {
      Dir = Ellipsis + Sep;
    }
    else if (Dir == Sep)
    {
      Dir.clear();
    }
    else
    {
      // CutFirstDirectory
      bool Root = Dir.startsWith(Sep);
      if (Root)
      {
        Dir.remove(0, 1);
      }
      if (Dir.startsWith(QLatin1Char('.')))
      {
        Dir.remove(0, 4);
      }
      qsizetype Slash = Dir.indexOf(Sep);
      if (Slash >= 0)
      {
        Dir = Ellipsis + Sep + Dir.mid(Slash + 1);
      }
      else
      {
        Dir.clear();
      }
      if (Root)
      {
        Dir = Sep + Dir;
      }
    }
    Result = Dir + Name;
  }
  if (Result.size() > MaxLen)
  {
    Result.truncate(MaxLen);
  }
  return Result;
}

bool Script::OpenLog(const QString & FileName, QString & Error, qint64 MaxSize, int MaxCount)
{
  // TConfiguration::TemporaryLogging + TemporaryLogProtocol/LogSensitive/LogMaxSize/LogMaxCount
  SessionLogOptions Options;
  Options.Logging = true;
  Options.FileName = FileName;
  Options.Append = Configuration::Instance().LogFileAppend;
  Options.Protocol = FLogLevel;
  Options.Sensitive = FLogPasswords;
  Options.MaxSize = MaxSize;
  Options.MaxCount = MaxCount;
  auto Log = std::make_shared<SessionLog>(Options);
  if (!Log->Open(&Error))
  {
    return false;
  }
  FLog = Log;
  // the sessions log into the script's log
  SessionLog::SetProcessLog(FLog);
  return true;
}

void Script::SetLogLevel(int Level, bool LogPasswords)
{
  FLogLevel = Level;
  FLogPasswords = LogPasswords;
}

void Script::Log(QChar Type, const QString & Str)
{
  if (!FLog)
  {
    return;
  }
  // Reduced logging (-1) leaves out the script output
  if ((FLogLevel < 0) && (Type == QLatin1Char('<')))
  {
    return;
  }
  LogLineType LineType = llMessage;
  switch (Type.unicode())
  {
    case '<': LineType = llOutput; break;
    case '>': LineType = llInput; break;
    case '!': LineType = llStdError; break;
    case '*': LineType = llException; break;
    default: break;
  }
  FLog->Add(LineType, Str);
}

// --- XML log (TActionLog) ---

bool Script::OpenXmlLog(const QString & FileName, bool Groups, bool Required, QString & Error)
{
  auto Log = std::make_shared<ActionsLog>(FileName, nullptr);
  if (!Log->Open(&Error, true))
  {
    return false;
  }
  FXmlLog = Log;
  FXmlGroups = Groups;
  FXmlRequired = Required;
  ActionsLog::SetProcessLog(Log);
  // the actions of the script commands, on this thread
  ActionsLog::SetCurrent(Log);
  return true;
}

void Script::CloseXmlLog()
{
  if (FXmlLog)
  {
    ActionsLog::SetProcessLog(nullptr);
    ActionsLog::SetCurrent(nullptr);
    FXmlLog->Close();
    FXmlLog.reset();
  }
}

void Script::CheckXmlLog()
{
  if (FXmlRequired && FXmlLog && FXmlLog->Failed())
  {
    // LOG_FATAL_ERROR; once, the script stops here
    FXmlRequired = false;
    NotifyAbort();
    throw ExtException(QStringLiteral("Error occurred during logging. Cannot continue."), FXmlLog->LastError());
  }
}

unsigned int Script::InputTimeout() const
{
  return (FBatch != BatchOff) ? BatchInputTimeout : 0;
}

void Script::NotifyAbort()
{
  // TConsoleRunner::NotifyAbort
  if (FBatchScript)
  {
    FAborted = true;
  }
}

void Script::Print(const QString & Str)
{
  Console::Instance().Print(Str);
}

void Script::PrintLine(const QString & Str, bool /*Error*/)
{
  Log(QLatin1Char('<'), QStringLiteral("Script: ") + Str);
  Console::Instance().PrintLine(Str);
}

void Script::PrintMessage(const QString & Str, bool Error)
{
  PrintLine(RemoveEmptyLines(Str), Error);
}

void Script::ShowException(const ExtException & E)
{
  // TConsoleRunner::DoShowException / ExceptionFullMessage
  QString Message = E.Message();
  if (!E.MoreMessages().isEmpty())
  {
    Message += QLatin1Char('\n') + E.MoreMessages();
  }
  Log(QLatin1Char('*'), QStringLiteral("(%1) %2").arg(QStringLiteral("ExtException"), Message));
  if (XmlLogging())
  {
    FXmlLog->AddFailure(E);
  }
  if (!Message.trimmed().isEmpty())
  {
    FCommandError = true;
    PrintMessage(Message, true);
  }
}

void Script::StartInteractive()
{
  FBatch = FInteractiveBatch;
  FConfirm = FInteractiveConfirm;
  FSessionReopenTimeout = FInteractiveSessionReopenTimeout;
}

QString Script::MaskPasswordInCommandLine(const QString & CommandLine)
{
  // TManagementScript::MaskPasswordInCommandLine (simplified): masks URL
  // password and -password/-passphrase/-newpassword switch values
  QString Result;
  QString Rest = CommandLine;
  QString Token;
  QString Raw;
  QString Separator;
  bool First = true;
  bool UrlDone = false;
  bool RawSettings = false;
  static const QStringList SensitiveRawSettings{QStringLiteral("Password"), QStringLiteral("PasswordPlain"),
    QStringLiteral("Passphrase"), QStringLiteral("PassphrasePlain"), QStringLiteral("ProxyPassword"),
    QStringLiteral("ProxyPasswordEnc"), QStringLiteral("TunnelPassword"), QStringLiteral("TunnelPasswordPlain"),
    QStringLiteral("TunnelPassphrase"), QStringLiteral("TunnelPassphrasePlain"), QStringLiteral("EncryptKey"),
    QStringLiteral("EncryptKeyPlain")};
  while (CutToken(Rest, Token, &Raw, &Separator))
  {
    QString Out = Raw;
    if (!First)
    {
      bool IsSwitch = Token.startsWith(QLatin1Char('-')) || Token.startsWith(QLatin1Char('/'));
      if (!IsSwitch && RawSettings)
      {
        // -rawsettings Key=Value ...: values of password settings
        QString Key = Token.section(QLatin1Char('='), 0, 0);
        if (Token.contains(QLatin1Char('=')) && SensitiveRawSettings.contains(Key, Qt::CaseInsensitive))
        {
          Out = Key + QStringLiteral("=***");
        }
      }
      else if (IsSwitch)
      {
        QString Name = Token.mid(1).section(QLatin1Char('='), 0, 0);
        RawSettings = (Name.compare(QStringLiteral("rawsettings"), Qt::CaseInsensitive) == 0);
        if ((Name.compare(QStringLiteral("password"), Qt::CaseInsensitive) == 0) ||
            (Name.compare(QStringLiteral("passphrase"), Qt::CaseInsensitive) == 0) ||
            (Name.compare(QStringLiteral("newpassword"), Qt::CaseInsensitive) == 0))
        {
          Out = QStringLiteral("%1%2=***").arg(Token.at(0)).arg(Name);
        }
      }
      else if (!UrlDone)
      {
        UrlDone = true;
        SessionData Dummy;
        SessionExtras DummyExtras;
        int ParsedInfo = 0;
        QString Masked;
        ParseSessionUrl(Token, nullptr, nullptr, Dummy, DummyExtras, ParsedInfo, &Masked);
        if (Masked != Token)
        {
          Out = QuoteParam(Masked);
        }
      }
    }
    First = false;
    Result += Out + Separator;
  }
  return Result.trimmed();
}

QString Script::MaskPasswordInCommand(const QString & FullCommand, const QString & Command)
{
  int Index = FindCommand(Command, nullptr);
  if ((Index >= 0) && (FCommands[Index].Name == QStringLiteral("open")))
  {
    return MaskPasswordInCommandLine(FullCommand);
  }
  return FullCommand;
}

int Script::FindCommand(const QString & Command, QString * Matches) const
{
  // TScriptCommands::FindCommand: exact match, else unique prefix
  for (int I = 0; I < static_cast<int>(FCommands.size()); I++)
  {
    if (FCommands[I].Name.compare(Command, Qt::CaseInsensitive) == 0)
    {
      return I;
    }
  }
  int Result = -1;
  int MatchesCount = 0;
  for (int I = 0; I < static_cast<int>(FCommands.size()); I++)
  {
    if ((Command.size() <= FCommands[I].Name.size()) && FCommands[I].Name.startsWith(Command, Qt::CaseInsensitive))
    {
      if (Matches != nullptr)
      {
        if (!Matches->isEmpty())
        {
          *Matches += QStringLiteral(", ");
        }
        *Matches += FCommands[I].Name;
      }
      MatchesCount++;
      Result = I;
    }
  }
  if (MatchesCount == 0)
  {
    return -1;
  }
  return (MatchesCount > 1) ? -2 : Result;
}

int Script::FindToggle(const QStringList & Names, const QString & Value)
{
  // TScriptCommands::FindCommand over a fixed list
  int Result = -1;
  int Count = 0;
  for (int I = 0; I < Names.size(); I++)
  {
    if (Names[I].compare(Value, Qt::CaseInsensitive) == 0)
    {
      return I;
    }
    if (!Value.isEmpty() && Names[I].startsWith(Value, Qt::CaseInsensitive))
    {
      Result = I;
      Count++;
    }
  }
  return (Count == 1) ? Result : -1;
}

void Script::Execute(const QString & Command, const QString & Params)
{
  QString Matches;
  int Index = FindCommand(Command, &Matches);
  if (Index == -2)
  {
    throw ExtException(QStringLiteral("Ambiguous command '%1'. Possible matches are: %2").arg(Command, Matches));
  }
  else if (Index < 0)
  {
    throw ExtException(QStringLiteral("Unknown command '%1'.").arg(Command));
  }
  const CommandDef & Def = FCommands[Index];
  ScriptProcParams Parameters(Def.Name, Params);
  if (Parameters.ParamCount() < Def.MinParams)
  {
    throw ExtException(QStringLiteral("Missing parameter for command '%1'.").arg(Def.Name));
  }
  else if ((Def.MaxParams >= 0) && (Parameters.ParamCount() > Def.MaxParams))
  {
    throw ExtException(QStringLiteral("Too many parameters for command '%1'.").arg(Def.Name));
  }
  if (!Def.Switches)
  {
    CheckParams(Parameters);
  }
  (this->*Def.Proc)(Parameters);
}

void Script::HandleException(const ExtException & E, bool Fatal)
{
  ShowException(E);
  // TManagementScript::HandleExtendedException: fatal error closes the session
  if (Fatal && (FSession != nullptr))
  {
    DoClose(FSession);
  }
}

void Script::Command(const QString & ACmd)
{
  bool Group = false;
  // TScript::Command; with /xmlgroups the elements of each command are
  // wrapped in <group> (the failure too)
  auto Handle = [this](const std::function<void()> & Proc)
  {
    try
    {
      Proc();
    }
    catch (const AbortException &)
    {
      // EAbort is silent; failed prompts/queries flag the error themselves
      if (FInterrupted)
      {
        PrintMessage(QStringLiteral("Terminated by user."), true);
        FCommandError = true;
        NotifyAbort();
      }
    }
    catch (const FatalException & E)
    {
      HandleException(E, true);
    }
    catch (const ExtException & E)
    {
      HandleException(E, false);
    }
  };
  Handle([&]()
  {
    QString Cmd = ACmd;
    QString Trimmed = Cmd.trimmed();
    if (!Trimmed.isEmpty() && (Trimmed[0] != QLatin1Char(';')) && (Trimmed[0] != QLatin1Char('#')))
    {
      QString FullCmd = Cmd;
      QString CommandName;
      if (CutToken(Cmd, CommandName))
      {
        QString LogCmd = FLogPasswords ? FullCmd.trimmed() : MaskPasswordInCommand(FullCmd.trimmed(), CommandName);
        Log(QLatin1Char('>'), QStringLiteral("Script: ") + LogCmd);
        if (FEcho)
        {
          PrintLine(LogCmd);
        }
        // TScript::Command: only commands run with a session open are grouped
        if (FXmlGroups && XmlLogging() && (FSession != nullptr))
        {
          FXmlLog->BeginGroup(FLogPasswords ? LogCmd : MaskPasswordInCommand(FullCmd.trimmed(), CommandName));
          Group = true;
        }
        Handle([&]()
        {
          Execute(CommandName, Cmd);
          CheckXmlLog();
        });
      }
    }
  });
  if (Group && FXmlLog)
  {
    FXmlLog->EndGroup();
  }
  FInterrupted = false;
  Console::Instance().ClearPendingAbort();
}

void Script::ApplySessionSettings(Session & S)
{
  // TScript::Command sets Configuration->SessionReopenTimeout for the command
  Terminal::Settings Settings = S.Term->GetSettings();
  if (Settings.ReopenTimeout != FSessionReopenTimeout)
  {
    Settings.ReopenTimeout = FSessionReopenTimeout;
    S.Term->SetSettings(Settings);
  }
}

void Script::UpdateTitle()
{
  // TConsoleRunner::UpdateTitle
  QString AppName = QStringLiteral("NixSCP");
  Console::Instance().SetTitle((FSession != nullptr) ? QStringLiteral("%1 - %2").arg(FSession->Data.SessionName(), AppName) : AppName);
}

OperationResult Script::Wait(Session & S, const std::function<void(Terminal::Callback)> & Start, bool Transfer,
  const std::function<void()> & OnInterrupt)
{
  QEventLoop Loop;
  bool Finished = false;
  OperationResult Result;
  if (Transfer)
  {
    ResetTransfer();
  }
  ApplySessionSettings(S);
  Start([&](const OperationResult & R)
  {
    Result = R;
    Finished = true;
    Loop.quit();
  });
  QTimer Timer;
  Timer.setInterval(200);
  QObject::connect(&Timer, &QTimer::timeout, [&]()
  {
    if (Console::Instance().PendingAbort() && !FInterrupted)
    {
      FInterrupted = true;
      S.Term->Progress().Cancel();
      if (OnInterrupt)
      {
        OnInterrupt();
      }
    }
    if (Transfer && FWantsProgress)
    {
      PrintTransferProgress(S, false);
    }
  });
  Timer.start();
  if (!Finished)
  {
    Loop.exec();
  }
  Timer.stop();
  if (Transfer && FWantsProgress && Result.Ok)
  {
    PrintTransferProgress(S, true);
  }
  return Result;
}

void Script::Check(const OperationResult & Result)
{
  if (Result.Ok)
  {
    return;
  }
  if (Result.Aborted)
  {
    throw AbortException();
  }
  if (Result.Fatal)
  {
    throw FatalException(Result.Message, Result.MoreMessages);
  }
  throw ExtException(Result.Message, Result.MoreMessages);
}

void Script::RunWorker(Session & S, Terminal::WorkerJob Job, bool Transfer)
{
  Check(Wait(S, [&](Terminal::Callback Done) { S.Term->RunOnWorker(std::move(Job), Done); }, Transfer));
}

void Script::ResetTransfer()
{
  FLastProgressFile.clear();
  FLastProgressMessage.clear();
}

void Script::PrintTransferProgress(Session & S, bool Final)
{
  // TManagementScript::TerminalOperationProgress
  FileOperationProgress::Snapshot Data = S.Term->Progress().Get();
  if ((Data.Op != FileOperationProgress::Copy) && (Data.Op != FileOperationProgress::Move))
  {
    return;
  }
  if (Data.FileName.isEmpty() || (!Data.InProgress && !Final))
  {
    return;
  }
  bool First = (Data.FileName != FLastProgressFile);
  bool Local = (Data.Dir == FileOperationProgress::ToRemote);
  QString FileName = MinimizeName(UnixExcludeTrailingBackslash(Data.FileName), ScriptProgressFileNameLimit, !Local);
  QString TransferredSizeStr = (Data.FileTransferred < 1024) ?
    QStringLiteral("%1 B").arg(Data.FileTransferred) : QStringLiteral("%1 KB").arg(Data.FileTransferred / 1024);
  double CPS = (Data.ElapsedMs > 0) ? (static_cast<double>(Data.TotalTransferred) * 1000.0 / static_cast<double>(Data.ElapsedMs)) : 0.0;
  int Percent = (Data.FileSize > 0) ? static_cast<int>(std::min<qint64>(100, Data.FileTransferred * 100 / Data.FileSize)) : 100;
  bool Ascii = FCopyParam.UseAsciiTransfer(Data.FileName, Local, FileMasks::Params());
  // "%-*s | %14s | %6.1f KB/s | %-6.6s | %3d%%" (the name padded in characters, not bytes)
  QString Message = FileName.leftJustified(ScriptProgressFileNameLimit) +
    QString::asprintf(" | %14s | %6.1f KB/s | %-6.6s | %3d%%",
      qPrintable(TransferredSizeStr), CPS / 1024.0, Ascii ? "ascii" : "binary", Percent);
  if (Message != FLastProgressMessage)
  {
    Console::Instance().PrintProgress(First, Message);
    FLastProgressMessage = Message;
    FLastProgressFile = Data.FileName;
  }
}

void Script::CheckSession()
{
  if (FSession == nullptr)
  {
    throw ExtException(QStringLiteral("No session."));
  }
}

void Script::CheckParams(ScriptProcParams & Params)
{
  QString Switch;
  if (Params.UnusedSwitch(Switch))
  {
    throw ExtException(QStringLiteral("Unknown switch '%1'.").arg(Switch));
  }
}

void Script::RequireParams(ScriptProcParams & Params, int MinParams)
{
  if (Params.ParamCount() < MinParams)
  {
    throw ExtException(QStringLiteral("Missing parameter for command '%1'.").arg(Params.FullCommand));
  }
}

void Script::NotSupported()
{
  throw ExtException(QStringLiteral("Operation not supported."));
}

void Script::NoMatch(const QString & Message)
{
  if (FFailOnNoMatch)
  {
    throw ExtException(Message);
  }
  PrintLine(Message);
}

void Script::NoMatch(const QString & Mask, const QString & Error)
{
  QString Message = QStringLiteral("No file matching '%1' found.").arg(Mask);
  if (!Error.isEmpty())
  {
    Message += QStringLiteral(" (%1)").arg(Error);
  }
  NoMatch(Message);
}

void Script::CheckDefaultCopyParam()
{
  if (FWarnNonDefaultCopyParam)
  {
    if (FCopyParam != CopyParam())
    {
      PrintLine(QStringLiteral("Using configured transfer settings different from factory defaults."));
    }
    FWarnNonDefaultCopyParam = false;
  }
}

void Script::CheckMultiFilesToOne(int Count, const QString & Target, bool /*Unix*/)
{
  QString Name = UnixExtractFileName(Target);
  if (!IsFileNameMask(Name) && (Count > 1))
  {
    // MULTI_FILES_TO_ONE (unformatted)
    QString Dir = UnixExtractFilePath(Target);
    PrintLine(QStringLiteral("Are you sure you want to transfer multiple files to a single file '%1' in a directory '%2'?\n"
      "The files will overwrite one another.\n"
      "If you actually want to transfer all files to a directory '%3', keeping their name, make sure you terminate the path with a slash.")
      .arg(Name, Dir, Target));
  }
}

QString Script::AbsolutePath(const QString & Path) const
{
  if (Path.startsWith(QLatin1Char('/')))
  {
    return Path;
  }
  QString Cwd = (FSession != nullptr) ? FSession->CurrentDirectory : QStringLiteral("/");
  if (Path.isEmpty() || (Path == QStringLiteral(".")))
  {
    return Cwd;
  }
  if ((Path == QStringLiteral("~")) && (FSession != nullptr))
  {
    return FSession->HomeDirectory;
  }
  if (Path.startsWith(QStringLiteral("~/")) && (FSession != nullptr))
  {
    return UnixPath::Combine(FSession->HomeDirectory, Path.mid(2));
  }
  return UnixPath::Combine(Cwd, Path);
}

QString Script::ListingStr(const RemoteFile & File)
{
  // TRemoteFile::GetListingStr
  QString LinkPart;
  if (File.IsSymLink)
  {
    LinkPart = QStringLiteral(" -> ") + File.LinkTo;
  }
  return QString::asprintf("%s%s %3s %-8s %-8s %9s %-12s %s%s",
    qPrintable(File.TypeChar()), qPrintable(File.RightsText()), "0",
    qPrintable(File.Owner), qPrintable(File.Group), qPrintable(QString::number(File.Size)),
    qPrintable(ModificationStr(File.Modification)), qPrintable(File.FileName), qPrintable(LinkPart));
}

std::vector<Script::RemoteItem> Script::CreateFileList(ScriptProcParams & Params, int Start, int End, int ListType)
{
  std::vector<RemoteItem> Result;
  QMap<QString, RemoteFileList> FileLists;
  for (int I = Start; I <= End; I++)
  {
    QString FileName = Params.Param(I);
    if (UnixExcludeTrailingBackslash(FileName) != FileName)
    {
      PrintLine(QStringLiteral("Selecting files using a path ending with slash is ambiguous. Remove the slash to select the folder. "
        "Append * mask to select all files in the folder."));
    }
    if (ListType & fltDirectories)
    {
      RemoteItem Item;
      Item.Name = FileName;
      Item.Path = UnixExcludeTrailingBackslash(AbsolutePath(FileName));
      Item.File.FileName = UnixExtractFileName(Item.Path);
      Item.File.IsDirectory = true;
      Item.File.FileType = RemoteFile::Directory;
      Result.push_back(Item);
    }
    else if ((ListType & fltMask) && FileMasks::IsMask(UnixExtractFileName(FileName)))
    {
      QString FileDirectory = UnixExtractFilePath(FileName);
      QString Directory = AbsolutePath(FileDirectory.isEmpty() ? QString() : FileDirectory);
      if (!FileLists.contains(Directory))
      {
        auto List = std::make_shared<RemoteFileList>();
        RunWorker(*FSession, [List, Directory](FileSystem & Fs, FileOperationProgress &, UserInterface &)
        {
          *List = Fs.ReadDirectory(Directory);
        });
        FileLists.insert(Directory, *List);
      }
      FileMasks Mask;
      Mask.SetMask(UnixExtractFileName(FileName));
      bool AnyFound = false;
      for (const RemoteFile & File : FileLists.value(Directory))
      {
        FileMasks::Params MaskParams;
        MaskParams.Size = File.Size;
        MaskParams.Modification = File.Modification;
        if (IsRealFile(File.FileName) && Mask.MatchesFileName(File.FileName, false, &MaskParams))
        {
          RemoteItem Item;
          Item.Name = FileDirectory + File.FileName;
          Item.Path = UnixPath::Combine(Directory, File.FileName);
          Item.File = File;
          Item.HaveFile = true;
          Result.push_back(Item);
          AnyFound = true;
        }
      }
      if (!AnyFound)
      {
        NoMatch(Mask.Masks(), QString());
      }
    }
    else
    {
      RemoteItem Item;
      Item.Name = FileName;
      Item.Path = UnixExcludeTrailingBackslash(AbsolutePath(FileName));
      if (ListType & fltQueryServer)
      {
        auto File = std::make_shared<RemoteFile>();
        QString Path = Item.Path;
        RunWorker(*FSession, [File, Path](FileSystem & Fs, FileOperationProgress &, UserInterface &)
        {
          *File = Fs.ReadFile(Path);
          if (File->IsSymLink)
          {
            // follow the link to know whether it is a directory
            RemoteFile Target;
            try
            {
              QString Dummy = Fs.CanonicalizePath(Path);
              RemoteFileList Probe;
              Target = Fs.ReadFile(Dummy);
              File->IsDirectory = Target.IsDirectory;
            }
            catch (const FatalException &)
            {
              throw;
            }
            catch (const ExtException &)
            {
              // broken link
            }
          }
        });
        Item.File = *File;
        Item.HaveFile = true;
      }
      Result.push_back(Item);
    }
  }

  if ((ListType & fltLatest) && (Result.size() > 1))
  {
    size_t LatestIndex = 0;
    for (size_t Index = 1; Index < Result.size(); Index++)
    {
      if (Result[LatestIndex].File.Modification < Result[Index].File.Modification)
      {
        LatestIndex = Index;
      }
    }
    RemoteItem Latest = Result[LatestIndex];
    Result.clear();
    Result.push_back(Latest);
  }

  if (ListType & fltOnlyFile)
  {
    for (const RemoteItem & Item : Result)
    {
      if (Item.File.IsDirectory)
      {
        throw ExtException(QStringLiteral("'%1' is not file!").arg(Item.File.FileName));
      }
    }
  }
  return Result;
}

std::vector<Script::LocalItem> Script::CreateLocalFileList(ScriptProcParams & Params, int Start, int End, int ListType)
{
  std::vector<LocalItem> Result;
  auto MakeItem = [](const QFileInfo & Info)
  {
    LocalItem Item;
    Item.Path = QDir::cleanPath(Info.absoluteFilePath());
    Item.Exists = Info.exists();
    Item.Directory = Info.isDir();
    Item.Size = Info.isDir() ? 0 : Info.size();
    Item.Modification = Info.lastModified();
    return Item;
  };
  for (int I = Start; I <= End; I++)
  {
    QString Param = Params.Param(I);
    QString FileName = Param;
    while ((FileName.size() > 1) && FileName.endsWith(QLatin1Char('/')))
    {
      FileName.chop(1);
    }
    if (FileName != Param)
    {
      PrintLine(QStringLiteral("Selecting files using a path ending with slash is ambiguous. Remove the slash to select the folder. "
        "Append * mask to select all files in the folder."));
    }
    QString AbsFileName = LocalAbsolutePath(FileName);
    QString MaskStr = LocalExtractFileName(FileName);
    if (IsLocalMask(MaskStr))
    {
      QString Directory = LocalAbsolutePath(LocalExtractFilePath(FileName).isEmpty() ? QStringLiteral(".") : LocalExtractFilePath(FileName));
      QDir Dir(Directory);
      FileMasks Mask;
      Mask.SetMask(LocalMask(MaskStr));
      bool AnyFound = false;
      QString Error;
      if (!Dir.exists())
      {
        Error = QStringLiteral("No such file or directory");
      }
      const QFileInfoList Entries = Dir.entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
      for (const QFileInfo & Info : Entries)
      {
        FileMasks::Params MaskParams;
        MaskParams.Size = Info.isDir() ? 0 : Info.size();
        MaskParams.Modification = Info.lastModified();
        if (Mask.MatchesFileName(Info.fileName(), false, &MaskParams))
        {
          Result.push_back(MakeItem(Info));
          AnyFound = true;
        }
      }
      if (!AnyFound)
      {
        NoMatch(MaskStr, Error);
      }
    }
    else
    {
      QFileInfo Info(AbsFileName);
      if (!Info.exists() && (ListType & fltLatest))
      {
        throw ExtException(QStringLiteral("File or folder '%1' does not exist.").arg(FileName));
      }
      // not existing file is left to fail later
      Result.push_back(MakeItem(Info));
    }
  }
  if ((ListType & fltLatest) && (Result.size() > 1))
  {
    size_t LatestIndex = 0;
    for (size_t Index = 1; Index < Result.size(); Index++)
    {
      if (Result[LatestIndex].Modification < Result[Index].Modification)
      {
        LatestIndex = Index;
      }
    }
    LocalItem Latest = Result[LatestIndex];
    Result.clear();
    Result.push_back(Latest);
  }
  return Result;
}

CopyParam::TransferMode Script::ParseTransferModeName(const QString & Name)
{
  int Value = FindToggle(TransferModeNames, Name);
  if (Value < 0)
  {
    throw ExtException(QStringLiteral("Unknown value '%1' of option '%2'.").arg(QStringLiteral("transfer"), Name));
  }
  return static_cast<CopyParam::TransferMode>(Value);
}

void Script::TransferParamParams(ScriptProcParams & Params, CopyParam & Param, bool & Move, bool & Resume)
{
  Move = Params.FindSwitch(QStringLiteral("delete"));
  Resume = Params.FindSwitch(QStringLiteral("resume"));
  // cpAppend
  Param.Append = !Resume && Params.FindSwitch(QStringLiteral("append"));
}

void Script::CopyParamParams(CopyParam & Param, ScriptProcParams & Params)
{
  QString Value;
  if (Params.FindSwitch(QStringLiteral("nopreservetime")))
  {
    Param.PreserveTime = false;
    Param.PreserveTimeDirs = false;
  }
  if (Params.FindSwitch(QStringLiteral("preservetime"), Value))
  {
    Param.PreserveTime = true;
    if (Value.compare(QStringLiteral("all"), Qt::CaseInsensitive) == 0)
    {
      Param.PreserveTimeDirs = true;
    }
  }
  if (Params.FindSwitch(QStringLiteral("nopermissions")))
  {
    Param.PreserveRights = false;
  }
  if (Params.FindSwitch(QStringLiteral("permissions"), Value))
  {
    unsigned int Mode = 0;
    if (!Rights::FromOctal(Value, Mode))
    {
      throw ExtException(QStringLiteral("'%1' is not a valid permission in octal format.").arg(Value));
    }
    Param.PreserveRights = true;
    Param.Rights = Mode;
  }
  if (Params.FindSwitch(QStringLiteral("speed"), Value))
  {
    long long CPSLimit = 0;
    if (!Value.isEmpty())
    {
      bool Ok = false;
      CPSLimit = Value.toLongLong(&Ok) * 1024;
      if (!Ok)
      {
        throw ExtException(QStringLiteral("'%1' is not a valid integer value.").arg(Value));
      }
      CPSLimit = std::max(0LL, CPSLimit);
    }
    Param.CPSLimit = static_cast<unsigned long>(CPSLimit);
  }
  if (Params.FindSwitch(QStringLiteral("transfer"), Value))
  {
    Param.Mode = ParseTransferModeName(Value);
  }
  if (Params.FindSwitch(QStringLiteral("filemask"), Value))
  {
    Param.IncludeFileMask = Value;
    if (FIncludeFileMaskOptionUsed)
    {
      PrintLine(QStringLiteral("Switch -filemask overrides obsolete options include/exclude."));
    }
  }
  if (Params.FindSwitch(QStringLiteral("resumesupport"), Value))
  {
    int Toggle = FindToggle(ToggleNames, Value);
    if (Toggle == 0)
    {
      Param.ResumeSupport = CopyParam::rsOff;
    }
    else if (Toggle == 1)
    {
      Param.ResumeSupport = CopyParam::rsOn;
    }
    else
    {
      bool Ok = false;
      int Threshold = Value.toInt(&Ok);
      if (!Ok)
      {
        throw ExtException(QStringLiteral("Unknown value '%1' of option '%2'.").arg(Value, QStringLiteral("resumesupport")));
      }
      Param.ResumeSupport = CopyParam::rsSmart;
      Param.ResumeThreshold = static_cast<qint64>(Threshold) * 1024;
    }
  }
  if (Params.FindSwitch(QStringLiteral("noneweronly")))
  {
    Param.NewerOnly = false;
  }
  if (Params.FindSwitch(QStringLiteral("neweronly")))
  {
    Param.NewerOnly = true;
  }
  QStringList RawSettings;
  if (Params.FindSwitch(QStringLiteral("rawtransfersettings"), RawSettings))
  {
    IniFile File;
    IniStorage Storage(&File);
    Storage.OpenSubKey(QStringLiteral("CopyParam"), true);
    for (const QString & Setting : RawSettings)
    {
      qsizetype P = Setting.indexOf(QLatin1Char('='));
      Storage.WriteString((P >= 0) ? Setting.left(P) : Setting, (P >= 0) ? Setting.mid(P + 1) : QString());
    }
    Param.Load(Storage);
  }
}

bool Script::RemoteIsDirectory(const QString & Path)
{
  auto IsDir = std::make_shared<bool>(false);
  Terminal::WorkerJob Job = [IsDir, Path](FileSystem & Fs, FileOperationProgress &, UserInterface &)
  {
    RemoteFile File;
    if (Fs.FileExists(Path, &File))
    {
      *IsDir = File.IsDirectory;
      if (File.IsSymLink)
      {
        try
        {
          *IsDir = Fs.ReadFile(Fs.CanonicalizePath(Path)).IsDirectory;
        }
        catch (const FatalException &)
        {
          throw;
        }
        catch (const ExtException &)
        {
        }
      }
    }
  };
  Session & S = *FSession;
  OperationResult Result = Wait(S, [&](Terminal::Callback Done) { S.Term->RunOnWorker(Job, Done); });
  if (Result.Fatal)
  {
    // This lookup has no counterpart in WinSCP (whose transfer would reopen
    // a connection lost meanwhile, TRobustOperationLoop): reopen and look again
    Log(QLatin1Char('.'), QStringLiteral("Connection was lost, reopening: %1").arg(Result.Message));
    OperationResult Reopened = Wait(S, [&](Terminal::Callback Done) { S.Term->Reopen(Done); });
    if (Reopened.Ok)
    {
      Result = Wait(S, [&](Terminal::Callback Done) { S.Term->RunOnWorker(Job, Done); });
    }
  }
  Check(Result);
  return *IsDir;
}

void Script::Download(const std::vector<RemoteItem> & Files, const QString & TargetDir, const QString & FileMask,
  CopyParam Param, bool Move, bool Resume)
{
  if (Files.empty())
  {
    return;
  }
  Param.ConfirmOverwriting = FConfirm;
  Param.ConfirmResume = FConfirm;
  Session & S = *FSession;
  bool Masked = IsEffectiveFileNameMask(FileMask);
  QStringList Plain;
  for (const RemoteItem & Item : Files)
  {
    QString Name = UnixExtractFileName(Item.Path);
    if (Masked && Item.File.IsDirectory)
    {
      // CopyParam.FileMask applies to the first level directories too
      // (TTerminal::ChangeFileName): "get dir1 dir2" downloads into ./dir2
      QString NewName = MaskFileName(Param.ChangeFileName(Name), FileMask);
      if (NewName != Name)
      {
        DownloadDirectoryAs(Item, QDir(TargetDir).filePath(CopyParam::ValidLocalFileName(NewName)), Param, Move);
        continue;
      }
    }
    // a file stored under a new name: CopyParam.FileMask, applied by the
    // core to the first level (TCopyParamType::ChangeFileName)
    if (Resume && !Item.File.IsDirectory)
    {
      // -resume: continue an existing smaller target via the core's resumable transfer
      QString Target = QDir(TargetDir).filePath(CopyParam::ValidLocalFileName(MaskFileName(Param.ChangeFileName(Name), FileMask)));
      QFileInfo Existing(Target);
      if (Existing.exists() && !Existing.isDir() && (Existing.size() < Item.File.Size) && !QFileInfo::exists(Target + PartialExt))
      {
        QFile::rename(Target, Target + PartialExt);
      }
    }
    Plain.append(Item.Path);
  }
  if (!Plain.isEmpty())
  {
    if (Resume)
    {
      Param.ResumeSupport = CopyParam::rsOn;
      Param.ConfirmResume = false;
    }
    if (Masked)
    {
      Param.FileMask = FileMask;
    }
    if (!TargetDir.isEmpty() && !QFileInfo(TargetDir).isDir())
    {
      // WinSCP's console fails creating the file (the core's "Create?"
      // query is the GUI Copy dialog's)
      QString FirstTarget = QDir(TargetDir).filePath(CopyParam::ValidLocalFileName(MaskFileName(Param.ChangeFileName(UnixPath::ExtractFileName(Plain.first())), FileMask)));
      throw ExtException(QStringLiteral("Can't create file '%1'.").arg(FirstTarget), QStringLiteral("No such file or directory"));
    }
    Check(Wait(S, [&](Terminal::Callback Done) { S.Term->CopyToLocal(Plain, TargetDir, Param, Move, Done); }, true));
  }
}

void Script::Upload(const std::vector<LocalItem> & Files, const QString & TargetDir, const QString & FileMask,
  CopyParam Param, bool Move, bool Resume)
{
  if (Files.empty())
  {
    return;
  }
  Param.ConfirmOverwriting = FConfirm;
  Param.ConfirmResume = FConfirm;
  Session & S = *FSession;
  bool Masked = IsEffectiveFileNameMask(FileMask);
  QStringList Plain;
  QStringList ResumeTargets;
  for (const LocalItem & Item : Files)
  {
    QString Name = QFileInfo(Item.Path).fileName();
    if (Masked && Item.Exists && Item.Directory)
    {
      // "put dir1 dir2" uploads into dir2 (TTerminal::ChangeFileName on the first level)
      QString NewName = MaskFileName(Param.ChangeFileName(Name), FileMask);
      if (NewName != Name)
      {
        UploadDirectoryAs(Item, UnixPath::Combine(TargetDir, CopyParam::RestoreChars(NewName)), Param, Move);
        continue;
      }
    }
    // a file stored under a new name: CopyParam.FileMask, applied by the
    // core to the first level (TCopyParamType::ChangeFileName)
    if (Resume && Item.Exists && !Item.Directory)
    {
      ResumeTargets.append(UnixPath::Combine(TargetDir, CopyParam::RestoreChars(MaskFileName(Param.ChangeFileName(Name), FileMask))));
      ResumeTargets.append(QString::number(Item.Size));
    }
    Plain.append(Item.Path);
  }
  if (!ResumeTargets.isEmpty())
  {
    RunWorker(S, [ResumeTargets](FileSystem & Fs, FileOperationProgress &, UserInterface &)
    {
      for (qsizetype I = 0; I + 1 < ResumeTargets.size(); I += 2)
      {
        RemoteFile Existing;
        RemoteFile Partial;
        const QString & Target = ResumeTargets[I];
        if (Fs.FileExists(Target, &Existing) && !Existing.IsDirectory && !Existing.IsSymLink &&
            (Existing.Size < ResumeTargets[I + 1].toLongLong()) && !Fs.FileExists(Target + PartialExt, &Partial))
        {
          Fs.RenameFile(Target, Target + PartialExt);
        }
      }
    });
  }
  if (!Plain.isEmpty())
  {
    if (Resume)
    {
      Param.ResumeSupport = CopyParam::rsOn;
      Param.ConfirmResume = false;
    }
    if (Masked)
    {
      Param.FileMask = FileMask;
    }
    Check(Wait(S, [&](Terminal::Callback Done) { S.Term->CopyToRemote(Plain, TargetDir, Param, Move, Done); }, true));
  }
}

void Script::DownloadDirectoryAs(const RemoteItem & Item, const QString & Target, const CopyParam & Param, bool Move)
{
  Session & S = *FSession;
  if (!QDir().mkpath(Target))
  {
    // CREATE_DIR_ERROR
    throw ExtException(QStringLiteral("Can't create directory '%1'.").arg(Target));
  }
  auto Children = std::make_shared<QStringList>();
  QString Source = Item.Path;
  RunWorker(S, [Children, Source](FileSystem & Fs, FileOperationProgress &, UserInterface &)
  {
    const RemoteFileList List = Fs.ReadDirectory(Source);
    for (const RemoteFile & File : List)
    {
      if (IsValidListingName(File.FileName) && !File.IsParentDirectory)
      {
        Children->append(UnixPath::Combine(Source, File.FileName));
      }
    }
  });
  if (!Children->isEmpty())
  {
    CopyParam ChildParam = Param;
    Check(Wait(S, [&](Terminal::Callback Done) { S.Term->CopyToLocal(*Children, Target, ChildParam, Move, Done); }, true));
  }
  if (Param.PreserveTime && Param.PreserveTimeDirs && Item.HaveFile && Item.File.Modification.isValid())
  {
    SetLocalModificationTime(Target, Item.File.Modification);
  }
  if (Move)
  {
    RunWorker(S, [Source](FileSystem & Fs, FileOperationProgress &, UserInterface &) { Fs.RemoveDirectory(Source); });
  }
}

void Script::UploadDirectoryAs(const LocalItem & Item, const QString & Target, const CopyParam & Param, bool Move)
{
  Session & S = *FSession;
  RunWorker(S, [Target](FileSystem & Fs, FileOperationProgress &, UserInterface &)
  {
    RemoteFile Existing;
    if (!Fs.FileExists(Target, &Existing))
    {
      Fs.CreateDirectory(Target, ServerDefaultMode);
    }
    else if (!Existing.IsDirectory)
    {
      throw ExtException(QStringLiteral("Can't create directory '%1'.").arg(Target));
    }
  });
  QStringList Children;
  const QFileInfoList Entries = QDir(Item.Path).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
  for (const QFileInfo & Info : Entries)
  {
    Children.append(QDir::cleanPath(Info.absoluteFilePath()));
  }
  if (!Children.isEmpty())
  {
    CopyParam ChildParam = Param;
    Check(Wait(S, [&](Terminal::Callback Done) { S.Term->CopyToRemote(Children, Target, ChildParam, Move, Done); }, true));
  }
  if (Param.PreserveTime && Param.PreserveTimeDirs && Item.Modification.isValid())
  {
    QDateTime Modification = Item.Modification;
    RunWorker(S, [Target, Modification](FileSystem & Fs, FileOperationProgress &, UserInterface &)
    {
      try
      {
        Fs.SetModificationTime(Target, Modification);
      }
      catch (const FatalException &)
      {
        throw;
      }
      catch (const ExtException &)
      {
      }
    });
  }
  if (Move)
  {
    QDir().rmdir(Item.Path);
  }
}

void Script::StreamDownload(const RemoteItem & Item, const CopyParam & Param)
{
  // CopyParam.OnTransferOut (TConsoleRunner::ScriptTransferOut): the file is
  // downloaded to a temporary file, which is then written to stdout, as is
  // (binary) or as HTTP chunks (chunked), ending with an empty chunk
  Session & S = *FSession;
  QTemporaryFile Temp(QDir(QDir::tempPath()).filePath(QStringLiteral("nixscp-stdout-XXXXXX")));
  if (!Temp.open())
  {
    throw ExtException(QStringLiteral("Cannot create temporary file: %1").arg(Temp.errorString()));
  }
  Platform::MakePrivate(Temp);
  QString TempName = Temp.fileName();
  Temp.close();
  QString Source = Item.Path;
  qint64 Size = Item.File.Size;
  unsigned long CPSLimit = Param.CPSLimit;
  RunWorker(S, [Source, TempName, Size, CPSLimit](FileSystem & Fs, FileOperationProgress & Progress, UserInterface &)
  {
    Progress.Start(FileOperationProgress::Copy, FileOperationProgress::ToLocal, 1);
    Progress.SetCPSLimit(CPSLimit);
    Progress.AddTotalSize(Size);
    Progress.SetFile(Source, Size);
    Fs.Download(Source, TempName, TransferParams(), Progress);
    Progress.FileFinished();
  }, true);
  QFile In(TempName);
  if (!In.open(QIODevice::ReadOnly))
  {
    throw ExtException(QStringLiteral("Cannot open file '%1'.").arg(TempName));
  }
  Console & C = Console::Instance();
  C.FinishProgress();
#ifdef Q_OS_WIN
  // the bytes of the file, not text with its line ends changed
  fflush(stdout);
  int StdOutMode = _setmode(_fileno(stdout), _O_BINARY);
#endif
  for (;;)
  {
    QByteArray Chunk = In.read(64 * 1024);
    if (Chunk.isEmpty())
    {
      break;
    }
    if (FStdOut == StdInOutChunked)
    {
      C.WriteStdOut(QByteArray::number(Chunk.size(), 16) + "\r\n" + Chunk + "\r\n");
    }
    else
    {
      C.WriteStdOut(Chunk);
    }
  }
  if (FStdOut == StdInOutChunked)
  {
    C.WriteStdOut("0\r\n\r\n");
  }
#ifdef Q_OS_WIN
  fflush(stdout);
  _setmode(_fileno(stdout), StdOutMode);
#endif
  SessionAction(laDownload, Source, QStringLiteral("-")).Commit(true, In.size());
}

void Script::StreamUpload(const QString & Target, const CopyParam & Param)
{
  // CopyParam.OnTransferIn (TConsoleRunner::ScriptTransferIn): stdin is read
  // to its end into a temporary file, which is then uploaded
  Session & S = *FSession;
  QTemporaryFile Temp(QDir(QDir::tempPath()).filePath(QStringLiteral("nixscp-stdin-XXXXXX")));
  if (!Temp.open())
  {
    throw ExtException(QStringLiteral("Cannot create temporary file: %1").arg(Temp.errorString()));
  }
  Platform::MakePrivate(Temp);
#ifdef Q_OS_WIN
  // the bytes as they come: in text mode line ends change and Ctrl+Z ends the input
  _setmode(STDIN_FILENO, _O_BINARY);
#endif
  QByteArray Buffer(64 * 1024, Qt::Uninitialized);
  for (;;)
  {
    ssize_t Read = read(STDIN_FILENO, Buffer.data(), static_cast<size_t>(Buffer.size()));
    if (Read < 0)
    {
      if (errno == EINTR)
      {
        continue;
      }
      throw ExtException(QStringLiteral("Error reading standard input."));
    }
    if (Read == 0)
    {
      break;
    }
    Temp.write(Buffer.constData(), Read);
  }
  Temp.flush();
  QString TempName = Temp.fileName();
  qint64 Size = Temp.size();
  int Mode = Param.PreserveRights ? static_cast<int>(Param.RemoteFileRights(false)) : ServerDefaultMode;
  unsigned long CPSLimit = Param.CPSLimit;
  RunWorker(S, [Target, TempName, Size, Mode, CPSLimit](FileSystem & Fs, FileOperationProgress & Progress, UserInterface &)
  {
    Progress.Start(FileOperationProgress::Copy, FileOperationProgress::ToRemote, 1);
    Progress.SetCPSLimit(CPSLimit);
    Progress.AddTotalSize(Size);
    Progress.SetFile(Target, Size);
    Fs.Upload(TempName, Target, Mode, TransferParams(), Progress);
    Progress.FileFinished();
  }, true);
  SessionAction(laUpload, QStringLiteral("-"), Target).Commit(true, Size);
}

// --- commands ---

void Script::HelpProc(ScriptProcParams & Params)
{
  QString Output;
  if (Params.ParamCount() == 0)
  {
    for (const CommandDef & Def : FCommands)
    {
      if (!Def.Description.isEmpty())
      {
        Output += QString::asprintf("%-8s %s\n", qPrintable(Def.Name), qPrintable(Def.Description));
      }
    }
  }
  else
  {
    for (int I = 1; I <= Params.ParamCount(); I++)
    {
      int Index = FindCommand(Params.Param(I), nullptr);
      if (Index < 0)
      {
        throw ExtException(QStringLiteral("Unknown command '%1'.").arg(Params.Param(I)));
      }
      Output += QString::fromUtf8(FCommands[Index].Help);
    }
  }
  Print(Output);
}

void Script::CallProc(ScriptProcParams & Params)
{
  // TScript::CallProc -> TTerminal::AnyCommand: runs in the current remote
  // directory, prints the output (TerminalCaptureLog) and follows "cd"
  CheckSession();
  Session & S = *FSession;
  if (!S.Term->CanAnyCommand())
  {
    NotSupported();
  }
  bool Shell = (S.Data.FSProtocol != fsFTP);
  QString Directory = S.CurrentDirectory;
  QString Command = Params.ParamsStr;
  if (Shell && !Directory.isEmpty())
  {
    QString Quoted = Directory;
    Quoted.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    Command = QStringLiteral("cd '%1' && %2").arg(Quoted, Params.ParamsStr);
  }
  int ExitCode = -1;
  QString NewDirectory;
  QStringList Output;
  QStringList ErrorOutput;
  OperationResult Result = Wait(S, [&](Terminal::Callback Done)
  {
    S.Term->AnyCommand(Command,
      [&](const QString & Line, bool StdError)
      {
        (StdError ? ErrorOutput : Output).append(Line);
        PrintLine(Line, StdError);
      },
      [&ExitCode, &NewDirectory, Done](const OperationResult & R, int Code, const QString & Dir)
      {
        ExitCode = Code;
        NewDirectory = Dir;
        Done(R);
      }, false);
  });
  if (Result.Ok && !NewDirectory.isEmpty())
  {
    S.CurrentDirectory = NewDirectory;
  }
  // TSCPFileSystem::ReadCommandOutput with coRaiseExcept: return code 1 is a
  // warning unless the site has ExitCode1IsError; an error output with no
  // regular output fails too
  QString FailMessage;
  QString FailMore;
  if (Result.Ok && Shell)
  {
    bool WrongReturnCode = (ExitCode > 1) || ((ExitCode == 1) && S.Data.ExitCode1IsError);
    QString Message = ErrorOutput.join(QLatin1Char('\n')).trimmed();
    bool WrongOutput = !Message.isEmpty() && Output.isEmpty();
    if (WrongReturnCode || WrongOutput)
    {
      if (Message.isEmpty())
      {
        // COMMAND_FAILED_CODEONLY
        FailMessage = QStringLiteral("Command failed with return code %1.").arg(ExitCode);
      }
      else
      {
        // COMMAND_FAILED2
        FailMessage = QStringLiteral("Command '%1'\nfailed with return code %2 and the following error message.").arg(Params.ParamsStr).arg(ExitCode);
        FailMore = Message;
      }
    }
  }
  {
    SessionAction Action(laCall);
    Action.Command(Params.ParamsStr);
    Action.Destination(Directory);
    if (!Output.isEmpty())
    {
      Action.AddOutput(Output.join(QStringLiteral("\r\n")), false);
    }
    if (!ErrorOutput.isEmpty())
    {
      Action.AddOutput(ErrorOutput.join(QStringLiteral("\r\n")), true);
    }
    if (ExitCode >= 0)
    {
      Action.ExitCode(ExitCode);
    }
    if (!Result.Ok)
    {
      Action.Rollback(QStringList{Result.Message});
    }
    else if (!FailMessage.isEmpty())
    {
      Action.Rollback(QStringList{FailMessage});
    }
    else
    {
      Action.Commit();
    }
  }
  Check(Result);
  if (!FailMessage.isEmpty())
  {
    throw ExtException(FailMessage, FailMore);
  }
}

void Script::EchoProc(ScriptProcParams & Params)
{
  PrintLine(Params.ParamsStr);
}

void Script::StatProc(ScriptProcParams & Params)
{
  CheckSession();
  QString Path = UnixExcludeTrailingBackslash(AbsolutePath(Params.Param(1)));
  auto File = std::make_shared<RemoteFile>();
  OperationResult Result = Wait(*FSession, [&](Terminal::Callback Done)
  {
    FSession->Term->RunOnWorker([File, Path](FileSystem & Fs, FileOperationProgress &, UserInterface &)
    {
      *File = Fs.ReadFile(Path);
    }, Done);
  });
  {
    SessionAction Action(laStat, Path);
    if (Result.Ok)
    {
      Action.File(*File);
      Action.Commit();
    }
    else
    {
      Action.Rollback(QStringList{Result.Message});
    }
  }
  Check(Result);
  PrintLine(ListingStr(*File));
}

void Script::ChecksumProc(ScriptProcParams & Params)
{
  // TScript::ChecksumProc -> TTerminal::CalculateFilesChecksum: FTP checksum
  // commands (HASH, XSHA256, ...), otherwise the shell's sha*sum/md5sum
  // (TSCPFileSystem::CalculateFilesChecksum, the secondary shell of SFTP)
  CheckSession();
  Session & S = *FSession;
  // SFTP "check-file" too: the server may have no shell
  bool ServerChecksum = S.Term->IsCapable(fcCalculatingChecksum);
  if (!ServerChecksum && (!S.Term->CanAnyCommand() || (S.Data.FSProtocol == fsFTP)))
  {
    NotSupported();
  }
  QString Alg = Params.Param(1);
  std::vector<RemoteItem> Files = CreateFileList(Params, 2, 2, fltQueryServer);
  if ((Files.size() != 1) || Files[0].File.IsDirectory)
  {
    // NOT_FILE_ERROR
    throw ExtException(QStringLiteral("'%1' is not file!").arg(Files.empty() ? Params.Param(2) : Files[0].Name));
  }
  QString Path = Files[0].Path;
  auto Checksum = std::make_shared<QString>();
  auto UsedAlg = std::make_shared<QString>(Alg);
  OperationResult Result = Wait(S, [&](Terminal::Callback Done)
  {
    S.Term->RunOnWorker([Checksum, UsedAlg, Path, Alg](FileSystem & Fs, FileOperationProgress & Progress, UserInterface &)
    {
      if (Fs.IsCapable(fcCalculatingChecksum, false))
      {
        *Checksum = Fs.CalculateFileChecksum(Path, Alg);
        return;
      }
      AnyCommandFileSystem * Shell = dynamic_cast<AnyCommandFileSystem *>(&Fs);
      if (Shell == nullptr)
      {
        throw ExtException(QStringLiteral("Operation not supported."));
      }
      QString AlgCommand = ShellChecksumCommand(Alg);
      if (AlgCommand.isEmpty())
      {
        // UNKNOWN_CHECKSUM
        throw ExtException(QStringLiteral("Checksum algorithm '%1' is not supported.").arg(Alg));
      }
      QString Command = QStringLiteral("%1 %2").arg(AlgCommand, ShellQuoteStr(Path));
      QStringList Output;
      QStringList ErrorOutput;
      auto Run = [&]()
      {
        Output.clear();
        ErrorOutput.clear();
        return Shell->AnyCommand(Command, QString(),
          [&](const QString & Line, bool StdError) { (StdError ? ErrorOutput : Output).append(Line); }, Progress, nullptr);
      };
      int ExitCode = Run();
      // 126/127: the command does not exist or cannot be executed
      QString Fallback = Terminal::ShellChecksumFallback(AlgCommand);
      if (((ExitCode == 126) || (ExitCode == 127)) && !Fallback.isEmpty())
      {
        Command = QStringLiteral("%1 %2").arg(Fallback, ShellQuoteStr(Path));
        ExitCode = Run();
      }
      if ((ExitCode != 0) || (Output.size() != 1))
      {
        if (!ErrorOutput.isEmpty())
        {
          throw ExtException(QStringLiteral("Command '%1'\nfailed with return code %2 and the following error message.").arg(Command).arg(ExitCode),
            ErrorOutput.join(QLatin1Char('\n')));
        }
        // INVALID_OUTPUT_ERROR
        throw ExtException(QStringLiteral("Command '%1' failed with invalid output '%2'.").arg(Command, Output.join(QLatin1Char('\n'))));
      }
      // TSCPFileSystem::ParseFileChecksum
      QHash<QString, QString> Parsed = ParseChecksumOutput(Output);
      if (Parsed.size() != 1)
      {
        throw ExtException(QStringLiteral("Command '%1' failed with invalid output '%2'.").arg(Command, Output.first()));
      }
      *Checksum = Parsed.begin().value();
      *UsedAlg = Alg.toLower();
    }, Done);
  });
  {
    SessionAction Action(laChecksum, Path);
    if (Result.Ok)
    {
      Action.Checksum(*UsedAlg, *Checksum);
      Action.Commit();
    }
    else
    {
      Action.Rollback(QStringList{Result.Message});
    }
  }
  Check(Result);
  // TScript::DoCalculatedChecksum
  PrintLine(QStringLiteral("%1 %2").arg(*Checksum, UnixExtractFileName(Path)));
}

void Script::PwdProc(ScriptProcParams & /*Params*/)
{
  CheckSession();
  PrintLine(FSession->CurrentDirectory);
  SessionAction Action(laCwd);
  Action.Cwd(FSession->CurrentDirectory);
  Action.Commit();
}

void Script::CopyIdProc(ScriptProcParams & Params)
{
  // TScript::CopyIdProc -> TTerminal::UploadPublicKey
  CheckSession();
  Session & S = *FSession;
  if (!S.Data.IsSsh())
  {
    NotSupported();
  }
  QString KeyFile = Params.Param(1);
  auto Messages = std::make_shared<QStringList>();
  RunWorker(S, [KeyFile, Messages](FileSystem & Fs, FileOperationProgress &, UserInterface &)
  {
    SftpFileSystem * Sftp = dynamic_cast<SftpFileSystem *>(&Fs);
    ScpFileSystem * Scp = ScpFileSystem::Of(Fs);
    SecureShell & Shell = (Sftp != nullptr) ? Sftp->Shell() : Scp->Shell();
    QString Result = Shell.InstallPublicKey(KeyFile, [Messages](const QString & Message) { Messages->append(Message); });
    Messages->append(Result);
  });
  for (const QString & Message : *Messages)
  {
    QString Text = Message;
    PrintMessage(Text.remove(QStringLiteral("**")));
  }
}

void Script::CdProc(ScriptProcParams & Params)
{
  CheckSession();
  QString Directory = (Params.ParamCount() == 0) ? FSession->HomeDirectory : AbsolutePath(Params.Param(1));
  auto Result = std::make_shared<QString>();
  QString Requested = (Params.ParamCount() == 0) ? FSession->HomeDirectory : Params.Param(1);
  RunWorker(*FSession, [Result, Directory, Requested](FileSystem & Fs, FileOperationProgress &, UserInterface &)
  {
    try
    {
      QString Path = Fs.CanonicalizePath(Directory);
      RemoteFile File = Fs.ReadFile(Path);
      if (!File.IsDirectory)
      {
        // canonicalized path is not a link anymore; probe for a directory anyway
        Fs.ReadDirectory(Path);
      }
      *Result = Path;
    }
    catch (const FatalException &)
    {
      throw;
    }
    catch (const ExtException & E)
    {
      // CHANGE_DIR_ERROR
      QString More = E.Message();
      if (!E.MoreMessages().isEmpty())
      {
        More += QLatin1Char('\n') + E.MoreMessages();
      }
      throw ExtException(QStringLiteral("Error changing directory to '%1'.").arg(Requested), More);
    }
  });
  FSession->CurrentDirectory = *Result;
  PrintLine(FSession->CurrentDirectory);
  SessionAction Action(laCwd);
  Action.Cwd(FSession->CurrentDirectory);
  Action.Commit();
}

void Script::LsProc(ScriptProcParams & Params)
{
  CheckSession();
  QString Directory;
  FileMasks Mask;
  bool HaveMask = false;
  if (Params.ParamCount() > 0)
  {
    Directory = Params.Param(1);
    QString MaskStr = UnixExtractFileName(Directory);
    HaveMask = FileMasks::IsMask(MaskStr);
    if (HaveMask)
    {
      Mask.SetMask(MaskStr);
      Directory = UnixExtractFilePath(Directory);
    }
  }
  QString AbsDirectory = UnixExcludeTrailingBackslash(AbsolutePath(Directory));
  auto Files = std::make_shared<RemoteFileList>();
  // TTerminal::ReadDirectoryListing: a lost connection is reopened (CustomReadDirectory)
  RemoteFileList List;
  Session & S = *FSession;
  Check(Wait(S, [&](Terminal::Callback Done)
  {
    S.Term->ReadDirectoryListing(AbsDirectory, [&List, Done](const OperationResult & R, const QString &, const RemoteFileList & Listed)
    {
      List = Listed;
      Done(R);
    });
  }));
  RunWorker(*FSession, [Files, AbsDirectory](FileSystem & Fs, FileOperationProgress &, UserInterface &)
  {
    // WinSCP lists the parent directory entry too
    try
    {
      RemoteFile Parent = Fs.ReadFile(UnixPath::Combine(AbsDirectory, QStringLiteral("..")));
      Parent.FileName = QStringLiteral("..");
      Parent.IsParentDirectory = true;
      Files->append(Parent);
    }
    catch (const FatalException &)
    {
      throw;
    }
    catch (const ExtException &)
    {
    }
  });
  Files->append(List);
  {
    SessionAction Action(laLs);
    Action.Destination(AbsDirectory);
    Action.FileList(*Files);
    Action.Commit();
  }
  int Count = 0;
  for (const RemoteFile & File : *Files)
  {
    FileMasks::Params MaskParams;
    MaskParams.Size = File.Size;
    MaskParams.Modification = File.Modification;
    if (!HaveMask || Mask.MatchesFileName(File.FileName, false, &MaskParams))
    {
      PrintLine(ListingStr(File));
      Count++;
    }
  }
  if ((Count == 0) && HaveMask)
  {
    NoMatch(Mask.Masks(), QString());
  }
}

void Script::RmProc(ScriptProcParams & Params)
{
  CheckSession();
  bool OnlyFile = Params.FindSwitch(QStringLiteral("onlyfile"));
  std::vector<RemoteItem> Files = CreateFileList(Params, 1, Params.ParamCount(),
    fltQueryServer | fltMask | (OnlyFile ? fltOnlyFile : 0));
  CheckParams(Params);
  for (const RemoteItem & Item : Files)
  {
    // Terminal::DeleteFiles moves the files to the session's remote recycle
    // bin when "Preserve deleted remote files to recycle bin" is on
    Session & S = *FSession;
    Check(Wait(S, [&](Terminal::Callback Done) { S.Term->DeleteFiles(QStringList{Item.Path}, Done); }));
    // TManagementScript::TerminalOperationFinished
    PrintLine(Item.Path);
  }
}

void Script::RmDirProc(ScriptProcParams & Params)
{
  CheckSession();
  std::vector<RemoteItem> Files = CreateFileList(Params, 1, Params.ParamCount(), fltDirectories);
  for (const RemoteItem & Item : Files)
  {
    Session & S = *FSession;
    Check(Wait(S, [&](Terminal::Callback Done) { S.Term->DeleteFiles(QStringList{Item.Path}, Done); }));
    PrintLine(Item.Path);
  }
}

void Script::MvProc(ScriptProcParams & Params)
{
  CheckSession();
  std::vector<RemoteItem> Files = CreateFileList(Params, 1, Params.ParamCount() - 1, fltMask | fltQueryServer);
  QString Target = Params.Param(Params.ParamCount());
  QString TargetDirectory = UnixExtractFilePath(Target);
  QString FileMask = UnixExtractFileName(Target);
  if (!FileMask.isEmpty() && !IsFileNameMask(FileMask) && RemoteIsDirectory(AbsolutePath(Target)))
  {
    // NixSCP: an existing directory is a target directory even without the trailing slash
    TargetDirectory = UnixIncludeTrailingBackslash(Target);
    FileMask.clear();
  }
  Target = UnixIncludeTrailingBackslash(TargetDirectory) + FileMask;
  CheckMultiFilesToOne(static_cast<int>(Files.size()), Target, true);
  for (const RemoteItem & Item : Files)
  {
    QString Name = UnixExtractFileName(Item.Path);
    QString NewName = FileMask.isEmpty() ? Name : MaskFileName(Name, FileMask);
    QString Directory = TargetDirectory.isEmpty() ? UnixParentPath(Item.Path) : UnixExcludeTrailingBackslash(AbsolutePath(TargetDirectory));
    QString NewPath = UnixPath::Combine(Directory, NewName);
    if (NewPath == Item.Path)
    {
      continue;
    }
    Session & S = *FSession;
    Check(Wait(S, [&](Terminal::Callback Done) { S.Term->RenameFile(Item.Path, NewPath, Done); }));
    PrintLine(Item.Path);
  }
}

void Script::CpProc(ScriptProcParams & Params)
{
  // TScript::DoMvOrCp(fcRemoteCopy) -> TTerminal::CopyFiles: FTP SITE CPFR/CPTO,
  // S3 server side copy, otherwise "cp -p -r -f" in the shell
  // (TSCPFileSystem::CopyFile, the secondary shell of SFTP)
  CheckSession();
  Session & S = *FSession;
  TFSProtocol Protocol = S.Data.FSProtocol;
  bool Capable =
    ((Protocol == fsFTP) && S.Term->IsCapable(fcRemoteCopy)) ||
    (Protocol == fsS3) ||
    ((Protocol != fsFTP) && (Protocol != fsWebDAV) && S.Term->CanAnyCommand());
  if (!Capable)
  {
    NotSupported();
  }
  std::vector<RemoteItem> Files = CreateFileList(Params, 1, Params.ParamCount() - 1, fltMask | fltQueryServer);
  QString Target = Params.Param(Params.ParamCount());
  QString TargetDirectory = UnixExtractFilePath(Target);
  QString FileMask = UnixExtractFileName(Target);
  if (!FileMask.isEmpty() && !IsFileNameMask(FileMask) && RemoteIsDirectory(AbsolutePath(Target)))
  {
    // NixSCP (as mv): an existing directory is a target directory even without the trailing slash
    TargetDirectory = UnixIncludeTrailingBackslash(Target);
    FileMask.clear();
  }
  Target = UnixIncludeTrailingBackslash(TargetDirectory) + FileMask;
  CheckMultiFilesToOne(static_cast<int>(Files.size()), Target, true);
  for (const RemoteItem & Item : Files)
  {
    QString Name = UnixExtractFileName(Item.Path);
    QString NewName = FileMask.isEmpty() ? Name : MaskFileName(Name, FileMask);
    QString Directory = TargetDirectory.isEmpty() ? UnixParentPath(Item.Path) : UnixExcludeTrailingBackslash(AbsolutePath(TargetDirectory));
    QString NewPath = UnixPath::Combine(Directory, NewName);
    QString Source = Item.Path;
    bool Directory_ = Item.File.IsDirectory;
    OperationResult Result = Wait(S, [&](Terminal::Callback Done)
    {
      S.Term->RunOnWorker([Source, NewPath](FileSystem & Fs, FileOperationProgress & Progress, UserInterface &)
      {
        if (FtpFileSystem * Ftp = dynamic_cast<FtpFileSystem *>(&Fs))
        {
          Ftp->CopyFile(Source, NewPath);
          return;
        }
        if (S3FileSystem * S3 = dynamic_cast<S3FileSystem *>(&Fs))
        {
          S3->CopyFile(Source, NewPath);
          return;
        }
        if (Fs.IsCapable(fcRemoteCopy, false))
        {
          // SFTP "copy-data": no shell needed
          Fs.CopyFile(Source, NewPath);
          return;
        }
        AnyCommandFileSystem * Shell = dynamic_cast<AnyCommandFileSystem *>(&Fs);
        if (Shell == nullptr)
        {
          throw ExtException(QStringLiteral("Operation not supported."));
        }
        auto Run = [&](const QString & AdditionalSwitches, QStringList & ErrorOutput)
        {
          // fsCopyFile: cp -p -r -f %s "%s" "%s" (names delimited and quoted)
          QString Command = QStringLiteral("cp -p -r -f ") +
            (AdditionalSwitches.isEmpty() ? QString() : (AdditionalSwitches + QLatin1Char(' '))) +
            ShellQuoteStr(Source) + QLatin1Char(' ') + ShellQuoteStr(NewPath);
          return Shell->AnyCommand(Command, QString(),
            [&](const QString & Line, bool StdError) { if (StdError) ErrorOutput.append(Line); }, Progress, nullptr);
        };
        QStringList ErrorOutput;
        // -T (no target directory) is a GNU extension; retry without it
        int ExitCode = Run(QStringLiteral("-T"), ErrorOutput);
        if (ExitCode != 0)
        {
          QStringList ErrorOutput2;
          ExitCode = Run(QString(), ErrorOutput2);
          if (ExitCode != 0)
          {
            // COPY_FILE_ERROR
            throw ExtException(QStringLiteral("Error copying file '%1' to '%2'.").arg(Source, NewPath),
              (ErrorOutput2.isEmpty() ? ErrorOutput : ErrorOutput2).join(QLatin1Char('\n')));
          }
        }
      }, Done);
    });
    {
      SessionAction Action(laCp, Source, NewPath);
      if (Directory_)
      {
        Action.Recursive();
      }
      if (Result.Ok)
      {
        Action.Commit();
      }
      else
      {
        Action.Rollback(QStringList{Result.Message});
      }
    }
    Check(Result);
    PrintLine(Item.Path);
  }
}

void Script::ChModProc(ScriptProcParams & Params)
{
  CheckSession();
  unsigned int Mode = 0;
  QString Octal = Params.Param(1);
  if (!Rights::FromOctal(Octal, Mode))
  {
    throw ExtException(QStringLiteral("'%1' is not a valid permission in octal format.").arg(Octal));
  }
  std::vector<RemoteItem> Files = CreateFileList(Params, 2, Params.ParamCount(), fltMask);
  for (const RemoteItem & Item : Files)
  {
    QString Path = Item.Path;
    OperationResult Result = Wait(*FSession, [&](Terminal::Callback Done)
    {
      FSession->Term->RunOnWorker([Path, Mode](FileSystem & Fs, FileOperationProgress &, UserInterface &)
      {
        Fs.ChangeMode(Path, Mode);
      }, Done);
    });
    {
      SessionAction Action(laChmod, Path);
      Action.Rights(Mode);
      if (Result.Ok)
      {
        Action.Commit();
      }
      else
      {
        Action.Rollback(QStringList{Result.Message});
      }
    }
    Check(Result);
    PrintLine(Path);
  }
}

void Script::LnProc(ScriptProcParams & Params)
{
  CheckSession();
  if (!FSession->Term->IsCapable(fcSymbolicLink))
  {
    NotSupported();
  }
  QString Link = AbsolutePath(Params.Param(2));
  QString Target = Params.Param(1);
  Session & S = *FSession;
  Check(Wait(S, [&](Terminal::Callback Done) { S.Term->CreateLink(Link, Target, Done); }));
}

void Script::MkDirProc(ScriptProcParams & Params)
{
  CheckSession();
  QString Path = UnixExcludeTrailingBackslash(AbsolutePath(Params.Param(1)));
  Session & S = *FSession;
  Check(Wait(S, [&](Terminal::Callback Done) { S.Term->CreateDirectory(Path, ServerDefaultMode, Done); }));
}

void Script::GetProc(ScriptProcParams & Params)
{
  CheckSession();
  ResetTransfer();
  bool Latest = Params.FindSwitch(QStringLiteral("latest"));
  bool OnlyFile = Params.FindSwitch(QStringLiteral("onlyfile"));
  CheckDefaultCopyParam();
  CopyParam Param = FCopyParam;
  CopyParamParams(Param, Params);
  bool Move = false;
  bool Resume = false;
  TransferParamParams(Params, Param, Move, Resume);
  RequireParams(Params, 1);
  int LastFileParam = (Params.ParamCount() == 1) ? 1 : (Params.ParamCount() - 1);
  // "get file -" streams the file to stdout (needs /stdout)
  bool Stream = (FStdOut != StdInOutOff) && (Params.ParamCount() > 1) && (Params.Param(Params.ParamCount()) == QStringLiteral("-"));
  if (Stream)
  {
    OnlyFile = true;
  }
  std::vector<RemoteItem> Files = CreateFileList(Params, 1, LastFileParam,
    fltQueryServer | fltMask | (Latest ? fltLatest : 0) | (OnlyFile ? fltOnlyFile : 0));
  if (Stream)
  {
    CheckParams(Params);
    for (const RemoteItem & Item : Files)
    {
      StreamDownload(Item, Param);
      if (Move)
      {
        Session & S = *FSession;
        Check(Wait(S, [&](Terminal::Callback Done) { S.Term->DeleteFiles(QStringList{Item.Path}, Done, dfForceDelete); }));
      }
    }
    return;
  }
  QString TargetDirectory;
  QString FileMask;
  if (Params.ParamCount() == 1)
  {
    TargetDirectory = QDir::currentPath();
  }
  else
  {
    QString Target = Params.Param(Params.ParamCount());
    TargetDirectory = LocalExtractFilePath(Target);
    if (TargetDirectory.isEmpty())
    {
      TargetDirectory = QDir::currentPath();
    }
    FileMask = LocalExtractFileName(Target);
    if (!FileMask.isEmpty() && !IsFileNameMask(FileMask) && QFileInfo(LocalAbsolutePath(Target)).isDir())
    {
      // NixSCP: an existing directory is a target directory even without the trailing slash
      TargetDirectory = Target;
      FileMask.clear();
    }
    CheckMultiFilesToOne(static_cast<int>(Files.size()), UnixIncludeTrailingBackslash(TargetDirectory) + FileMask, false);
  }
  CheckParams(Params);
  {
    QStringList Paths;
    for (const RemoteItem & Item : Files)
    {
      Paths.append(Item.Path);
    }
    Param.IncludeFileMask.SetRoots(LocalAbsolutePath(TargetDirectory), CommonPath(Paths));
  }
  Download(Files, LocalAbsolutePath(TargetDirectory), FileMask, Param, Move, Resume);
}

void Script::PutProc(ScriptProcParams & Params)
{
  CheckSession();
  ResetTransfer();
  bool Latest = Params.FindSwitch(QStringLiteral("latest"));
  CheckDefaultCopyParam();
  CopyParam Param = FCopyParam;
  CopyParamParams(Param, Params);
  bool Move = false;
  bool Resume = false;
  TransferParamParams(Params, Param, Move, Resume);
  RequireParams(Params, 1);
  int LastFileParam = (Params.ParamCount() == 1) ? 1 : (Params.ParamCount() - 1);
  // We use stdin only if - is the very first parameter (needs /stdin)
  if (FStdIn && (Params.Param(1) == QStringLiteral("-")))
  {
    // STREAM_IN_SCRIPT_ERROR
    const QString StreamInError =
      QStringLiteral("When uploading streamed data, only one source can be specified and the target must specify a filename.");
    if (Params.ParamCount() > 2)
    {
      throw ExtException(StreamInError);
    }
    QString Target = (Params.ParamCount() == 2) ? Params.Param(2) : QString();
    QString TargetDirectory = UnixExtractFilePath(Target);
    if (TargetDirectory.isEmpty())
    {
      TargetDirectory = FSession->CurrentDirectory;
    }
    QString FileName = UnixExtractFileName(Target);
    if (FileName.isEmpty() || IsFileNameMask(FileName))
    {
      throw ExtException(StreamInError);
    }
    CheckParams(Params);
    StreamUpload(UnixPath::Combine(UnixExcludeTrailingBackslash(AbsolutePath(TargetDirectory)), FileName), Param);
    return;
  }
  if (Param.Append && !FSession->Term->IsCapable(fcAppendUpload))
  {
    NotSupported();
  }
  std::vector<LocalItem> Files = CreateLocalFileList(Params, 1, LastFileParam, fltMask | (Latest ? fltLatest : 0));
  QString TargetDirectory;
  QString FileMask;
  if (Params.ParamCount() == 1)
  {
    TargetDirectory = FSession->CurrentDirectory;
  }
  else
  {
    QString Target = Params.Param(Params.ParamCount());
    TargetDirectory = UnixExtractFilePath(Target);
    if (TargetDirectory.isEmpty())
    {
      TargetDirectory = FSession->CurrentDirectory;
    }
    FileMask = UnixExtractFileName(Target);
    if (!FileMask.isEmpty() && !IsFileNameMask(FileMask) && RemoteIsDirectory(AbsolutePath(Target)))
    {
      // NixSCP: an existing directory is a target directory even without the trailing slash
      TargetDirectory = Target;
      FileMask.clear();
    }
    CheckMultiFilesToOne(static_cast<int>(Files.size()), UnixIncludeTrailingBackslash(TargetDirectory) + FileMask, true);
  }
  CheckParams(Params);
  {
    QStringList Paths;
    for (const LocalItem & Item : Files)
    {
      Paths.append(Item.Path);
    }
    Param.IncludeFileMask.SetRoots(CommonPath(Paths), UnixExcludeTrailingBackslash(AbsolutePath(TargetDirectory)));
  }
  Upload(Files, UnixExcludeTrailingBackslash(AbsolutePath(TargetDirectory)), FileMask, Param, Move, Resume);
}

void Script::OptionImpl(QString OptionName, QString ValueName)
{
  enum { Echo, Batch, Confirm, Transfer, SynchDelete, Exclude, Include, ReconnectTime, FailOnNoMatch };
  static const QStringList Names{QStringLiteral("echo"), QStringLiteral("batch"), QStringLiteral("confirm"),
    QStringLiteral("transfer"), QStringLiteral("synchdelete"), QStringLiteral("exclude"), QStringLiteral("include"),
    QStringLiteral("reconnecttime"), QStringLiteral("failonnomatch")};
  static const QStringList BatchModeNames{QStringLiteral("off"), QStringLiteral("on"), QStringLiteral("abort"), QStringLiteral("continue")};

  int Option = -1;
  if (!OptionName.isEmpty())
  {
    Option = FindToggle(Names, OptionName);
    if (Option < 0)
    {
      throw ExtException(QStringLiteral("Unknown option '%1'.").arg(OptionName));
    }
    OptionName = Names[Option];
  }
  auto Opt = [&](int O) { return (Option < 0) || (Option == O); };
  auto Line = [](const QString & Name, const QString & Value) { return QString::asprintf("%-15s %-10s", qPrintable(Name), qPrintable(Value)); };
  auto ToggleValue = [&]()
  {
    int Value = FindToggle(ToggleNames, ValueName);
    if (Value < 0)
    {
      throw ExtException(QStringLiteral("Unknown value '%1' of option '%2'.").arg(ValueName, OptionName));
    }
    return (Value == 1);
  };
  bool SetValue = !ValueName.isEmpty();
  bool PrintReconnectTime = false;

  if (Opt(Echo))
  {
    if (SetValue)
    {
      FEcho = ToggleValue();
    }
    PrintLine(Line(Names[Echo], ToggleNames[FEcho ? 1 : 0]));
  }
  if (Opt(Batch))
  {
    if (SetValue)
    {
      int Value = FindToggle(BatchModeNames, ValueName);
      if (Value < 0)
      {
        throw ExtException(QStringLiteral("Unknown value '%1' of option '%2'.").arg(ValueName, OptionName));
      }
      FBatch = static_cast<BatchMode>(Value);
      FInteractiveBatch = FBatch;
      if ((FBatch != BatchOff) && (FSessionReopenTimeout == 0))
      {
        FSessionReopenTimeout = BatchSessionReopenTimeout;
        FInteractiveSessionReopenTimeout = FSessionReopenTimeout;
        PrintReconnectTime = true;
      }
    }
    PrintLine(Line(Names[Batch], BatchModeNames[FBatch.load()]));
  }
  if (Opt(Confirm))
  {
    if (SetValue)
    {
      FConfirm = ToggleValue();
      FInteractiveConfirm = FConfirm;
    }
    PrintLine(Line(Names[Confirm], ToggleNames[FConfirm ? 1 : 0]));
  }
  // omitted in listing
  if (Option == Transfer)
  {
    if (SetValue)
    {
      FCopyParam.Mode = ParseTransferModeName(ValueName);
    }
    PrintLine(Line(Names[Transfer], TransferModeNames[FCopyParam.Mode]));
  }
  if (Option == SynchDelete)
  {
    if (SetValue)
    {
      FSynchronizeParams = (FSynchronizeParams & ~spDelete) | (ToggleValue() ? spDelete : 0);
    }
    PrintLine(Line(Names[SynchDelete], ToggleNames[(FSynchronizeParams & spDelete) ? 1 : 0]));
  }
  if (Option == Include)
  {
    if (SetValue)
    {
      FCopyParam.IncludeFileMask = (ValueName == QStringLiteral("clear")) ? QString() : ValueName;
      FIncludeFileMaskOptionUsed = (ValueName != QStringLiteral("clear"));
    }
    PrintLine(Line(Names[Include], FCopyParam.IncludeFileMask.Masks()));
  }
  if (Option == Exclude)
  {
    if (SetValue)
    {
      FCopyParam.IncludeFileMask = (ValueName == QStringLiteral("clear")) ?
        QString() : (QString(FileMasks::IncludeExcludeFileMasksDelimiter) + ValueName);
      FIncludeFileMaskOptionUsed = (ValueName != QStringLiteral("clear"));
    }
    PrintLine(Line(Names[Include], FCopyParam.IncludeFileMask.Masks()));
  }
  if (Opt(ReconnectTime) || PrintReconnectTime)
  {
    if (SetValue && !PrintReconnectTime)
    {
      int Value = 0;
      if (ValueName.compare(ToggleNames[0], Qt::CaseInsensitive) != 0)
      {
        bool Ok = false;
        Value = ValueName.toInt(&Ok);
        if (!Ok)
        {
          throw ExtException(QStringLiteral("Unknown value '%1' of option '%2'.").arg(ValueName, OptionName));
        }
        Value *= 1000;
      }
      FSessionReopenTimeout = Value;
      FInteractiveSessionReopenTimeout = FSessionReopenTimeout;
    }
    PrintLine(Line(Names[ReconnectTime],
      (FSessionReopenTimeout == 0) ? ToggleNames[0] : QString::number(FSessionReopenTimeout / 1000)));
  }
  if (Opt(FailOnNoMatch))
  {
    if (SetValue)
    {
      FFailOnNoMatch = ToggleValue();
    }
    PrintLine(Line(Names[FailOnNoMatch], ToggleNames[FFailOnNoMatch ? 1 : 0]));
  }
}

void Script::OptionProc(ScriptProcParams & Params)
{
  QString OptionName = (Params.ParamCount() >= 1) ? Params.Param(1) : QString();
  QString ValueName = (Params.ParamCount() >= 2) ? Params.Param(2) : QString();
  OptionImpl(OptionName, ValueName);
}

void Script::AsciiProc(ScriptProcParams & /*Params*/)
{
  OptionImpl(QStringLiteral("transfer"), QStringLiteral("ascii"));
}

void Script::BinaryProc(ScriptProcParams & /*Params*/)
{
  OptionImpl(QStringLiteral("transfer"), QStringLiteral("binary"));
}

void Script::SynchronizeDirectories(ScriptProcParams & Params, QString & LocalDirectory, QString & RemoteDirectory, int FirstParam)
{
  LocalDirectory = (Params.ParamCount() >= FirstParam) ? Params.Param(FirstParam) : QDir::currentPath();
  RemoteDirectory = (Params.ParamCount() >= FirstParam + 1) ? Params.Param(FirstParam + 1) : FSession->CurrentDirectory;
  LocalDirectory = LocalAbsolutePath(LocalDirectory);
  RemoteDirectory = UnixExcludeTrailingBackslash(AbsolutePath(RemoteDirectory));
}

namespace
{
// TScript::SynchronizeFileRecord
QString SynchronizeFileRecord(const QString & RootDirectory, const SynchronizeChecklist::Item * Item, bool Local)
{
  const SynchronizeChecklist::FileInfo & Info = Local ? Item->Info1 : Item->Info2;
  QString Path = UnixIncludeTrailingBackslash(Info.Directory) + Info.FileName;
  if (Path.startsWith(RootDirectory, Qt::CaseInsensitive))
  {
    Path = QStringLiteral("./") + Path.mid(RootDirectory.size());
  }
  if (Item->IsDirectory)
  {
    return UnixIncludeTrailingBackslash(Path);
  }
  return QStringLiteral("%1 [%2, %3]").arg(Path).arg(Info.Size).arg(ModificationStr(Info.Modification));
}

// TSessionActionRecord::SynchronizeChecklistItem
QString SynchronizeActionName(SynchronizeChecklist::ActionType Action)
{
  switch (Action)
  {
    case SynchronizeChecklist::saUploadNew: return QStringLiteral("uploadnew");
    case SynchronizeChecklist::saDownloadNew: return QStringLiteral("downloadnew");
    case SynchronizeChecklist::saUploadUpdate: return QStringLiteral("uploadupdate");
    case SynchronizeChecklist::saDownloadUpdate: return QStringLiteral("downloadupdate");
    case SynchronizeChecklist::saDeleteRemote: return QStringLiteral("deleteremote");
    case SynchronizeChecklist::saDeleteLocal: return QStringLiteral("deletelocal");
    default: return QString();
  }
}

QString SynchronizeIntro(SynchronizeMode Mode, const QString & LocalDirectory, const QString & RemoteDirectory)
{
  // SCRIPT_SYNCHRONIZE
  QString Arrow = (Mode == smRemote) ? QStringLiteral("=>") : ((Mode == smLocal) ? QStringLiteral("<=") : QStringLiteral("<=>"));
  return QStringLiteral("Local '%1' %2 Remote '%3'").arg(LocalDirectory, Arrow, UnixExcludeTrailingBackslash(RemoteDirectory));
}
}

void Script::SynchronizeProc(ScriptProcParams & Params)
{
  CheckSession();
  ResetTransfer();
  static const QStringList ModeNames{QStringLiteral("remote"), QStringLiteral("local"), QStringLiteral("both")};
  CheckDefaultCopyParam();
  CheckDefaultSynchronizeParams();
  CopyParam Param = FCopyParam;
  CopyParamParams(Param, Params);
  RequireParams(Params, 1);
  if (Params.ParamCount() > 3)
  {
    throw ExtException(QStringLiteral("Too many parameters for command '%1'.").arg(QStringLiteral("synchronize")));
  }
  QString ModeName = Params.Param(1);
  int ModeIndex = FindToggle(ModeNames, ModeName);
  if (ModeIndex < 0)
  {
    throw ExtException(QStringLiteral("Unknown option '%1'.").arg(ModeName));
  }
  SynchronizeMode Mode = static_cast<SynchronizeMode>(ModeIndex);
  QString LocalDirectory;
  QString RemoteDirectory;
  SynchronizeDirectories(Params, LocalDirectory, RemoteDirectory, 2);
  Param.IncludeFileMask.SetRoots(LocalDirectory, RemoteDirectory);

  int SynchronizeParams = FSynchronizeParams | spNoConfirmation;
  QString Value;
  if (Params.FindSwitch(QStringLiteral("delete")))
  {
    SynchronizeParams |= spDelete;
  }
  if (Params.FindSwitch(QStringLiteral("mirror")) && (Mode != smBoth))
  {
    SynchronizeParams |= spMirror;
  }
  if (Params.FindSwitch(QStringLiteral("criteria"), Value))
  {
    static const QStringList CriteriaNames{QStringLiteral("none"), QStringLiteral("either"), QStringLiteral("both")};
    int Criteria = FindToggle(CriteriaNames, Value);
    if (Criteria == 0)
    {
      SynchronizeParams |= spNotByTime;
      SynchronizeParams &= ~spBySize;
    }
    else if (Criteria > 0)
    {
      SynchronizeParams &= ~spNotByTime;
      SynchronizeParams |= spBySize;
    }
    else
    {
      int CriteriaParams = spNotByTime;
      for (const QString & Token : Value.split(QLatin1Char(','), Qt::SkipEmptyParts))
      {
        static const QStringList TokenNames{QStringLiteral("time"), QStringLiteral("size"), QStringLiteral("checksum")};
        int T = FindToggle(TokenNames, Token.trimmed());
        if (T == 0) CriteriaParams &= ~spNotByTime;
        else if (T == 1) CriteriaParams |= spBySize;
        else if (T == 2) CriteriaParams |= spByChecksum;
        else
        {
          CriteriaParams = -1;
          break;
        }
      }
      if (CriteriaParams >= 0)
      {
        SynchronizeParams &= ~(spNotByTime | spBySize | spByChecksum);
        SynchronizeParams |= CriteriaParams;
      }
    }
  }
  bool Preview = Params.FindSwitch(QStringLiteral("preview"));
  // enforce rules
  if (Mode == smBoth)
  {
    SynchronizeParams &= ~(spNotByTime | spBySize | spByChecksum);
  }
  CheckParams(Params);

  PrintLine(QStringLiteral("Comparing..."));
  Session & S = *FSession;
  auto Status = std::make_shared<SynchronizeStatus>();
  SynchronizeChecklistPtr Checklist;
  OperationResult Result = Wait(S, [&](Terminal::Callback Done)
  {
    SynchronizeCollect(S.Term.get(), LocalDirectory, RemoteDirectory, Mode, Param, SynchronizeParams, SynchronizeOptions(), Status,
      [&Checklist, Done](const OperationResult & R, SynchronizeChecklistPtr List)
      {
        Checklist = List;
        Done(R);
      });
  }, false, [Status]() { Status->Cancel(); });
  Check(Result);

  int Index = 0;
  const SynchronizeChecklist::Item * Item = nullptr;
  if (!Checklist || !Checklist->GetNextChecked(Index, Item))
  {
    NoMatch(QStringLiteral("Nothing to synchronize."));
    return;
  }
  if (Preview)
  {
    PrintLine(QStringLiteral("Differences found:"));
    QString LocalRoot = UnixIncludeTrailingBackslash(LocalDirectory);
    QString RemoteRoot = UnixIncludeTrailingBackslash(RemoteDirectory);
    Index = 0;
    while (Checklist->GetNextChecked(Index, Item))
    {
      QString LocalRecord = SynchronizeFileRecord(LocalRoot, Item, true);
      QString RemoteRecord = SynchronizeFileRecord(RemoteRoot, Item, false);
      QString Message;
      switch (Item->Action)
      {
        case SynchronizeChecklist::saUploadNew: Message = QStringLiteral("New local file %1").arg(LocalRecord); break;
        case SynchronizeChecklist::saDownloadNew: Message = QStringLiteral("New remote file %1").arg(RemoteRecord); break;
        case SynchronizeChecklist::saUploadUpdate: Message = QStringLiteral("Local file %1 newer than remote file %2").arg(LocalRecord, RemoteRecord); break;
        case SynchronizeChecklist::saDownloadUpdate: Message = QStringLiteral("Remote file %1 newer than local file %2").arg(RemoteRecord, LocalRecord); break;
        case SynchronizeChecklist::saDeleteRemote: Message = QStringLiteral("Orphan remote file %1").arg(RemoteRecord); break;
        case SynchronizeChecklist::saDeleteLocal: Message = QStringLiteral("Orphan local file %1").arg(LocalRecord); break;
        default: break;
      }
      PrintLine(Message);
      // TDifferenceSessionAction
      SessionAction Action(laDifference);
      Action.Difference(SynchronizeActionName(Item->Action), Item->IsDirectory,
        UnixIncludeTrailingBackslash(Item->Info1.Directory) + Item->Info1.FileName, Item->Info1.Size, Item->Info1.Modification,
        UnixIncludeTrailingBackslash(Item->Info2.Directory) + Item->Info2.FileName, Item->Info2.Size, Item->Info2.Modification);
      Action.Commit();
    }
    return;
  }

  PrintLine(QStringLiteral("Synchronizing..."));
  QString LastIntro;
  Result = Wait(S, [&](Terminal::Callback Done)
  {
    SynchronizeApply(S.Term.get(), Checklist, Param, SynchronizeParams, Status,
      [&](const SynchronizeChecklist::Item * Processed)
      {
        // TManagementScript::ShowPendingProgress / TerminalOperationFinished
        QString Intro = SynchronizeIntro(Mode, Processed->Info1.Directory, Processed->Info2.Directory);
        if (Intro != LastIntro)
        {
          PrintLine(Intro);
          LastIntro = Intro;
        }
        if ((Processed->Action == SynchronizeChecklist::saDeleteRemote) || (Processed->Action == SynchronizeChecklist::saDeleteLocal))
        {
          PrintLine(QStringLiteral("'%1' deleted").arg(Processed->GetFilePath()));
        }
      },
      [Done](const OperationResult & R, const SynchronizeStatistics &) { Done(R); });
  }, true, [Status]() { Status->Cancel(); });
  Check(Result);
}

void Script::KeepUpToDateProc(ScriptProcParams & Params)
{
  CheckSession();
  ResetTransfer();
  CheckDefaultCopyParam();
  CheckDefaultSynchronizeParams();
  CopyParam Param = FCopyParam;
  CopyParamParams(Param, Params);
  QString LocalDirectory;
  QString RemoteDirectory;
  SynchronizeDirectories(Params, LocalDirectory, RemoteDirectory, 1);
  int SynchronizeParams = FSynchronizeParams | spNoConfirmation | spNoRecurse | spUseCache | spDelayProgress | spSubDirs;
  if (Params.FindSwitch(QStringLiteral("delete")))
  {
    SynchronizeParams |= spDelete;
  }
  CheckParams(Params);
  PrintLine(QStringLiteral("Watching for changes, press Ctrl-C to abort..."));

  // TConsoleRunner::ScriptSynchronizeStartStop
  Session & S = *FSession;
  SynchronizeController Controller(S.Term.get());
  QEventLoop Loop;
  bool Aborted = false;
  QString AbortMessage;
  QString AbortMore;
  bool AbortClose = false;
  QObject::connect(&Controller, &SynchronizeController::Log,
    [this](SynchronizeLogEntry, const QString & Message, const QString & MoreMessages)
    {
      PrintMessage(MoreMessages.isEmpty() ? Message : (Message + QLatin1Char('\n') + MoreMessages));
    });
  // TConsoleRunner::SynchronizeControllerTooManyDirectories: no question in
  // scripting, the limit starts at MaxWatchDirectories and then doubles
  QObject::connect(&Controller, &SynchronizeController::TooManyDirectories, &Controller,
    [this, &Controller](int & MaxDirectories, bool & AbortWatching)
    {
      if (this->Aborted() || Console::Instance().PendingAbort())
      {
        AbortWatching = true;
        return;
      }
      MaxDirectories = (MaxDirectories < Controller.MaxWatchDirectories()) ? Controller.MaxWatchDirectories() : (MaxDirectories * 2);
    }, Qt::DirectConnection);
  QObject::connect(&Controller, &SynchronizeController::Aborted,
    [&](bool Close, const QString & Message, const QString & MoreMessages)
    {
      Aborted = true;
      AbortClose = Close;
      AbortMessage = Message;
      AbortMore = MoreMessages;
      Loop.quit();
    });
  QTimer Timer;
  Timer.setInterval(200);
  QObject::connect(&Timer, &QTimer::timeout, [&]()
  {
    if (Console::Instance().PendingAbort())
    {
      Loop.quit();
    }
  });
  SynchronizeParamType SyncParams;
  SyncParams.LocalDirectory = LocalDirectory;
  SyncParams.RemoteDirectory = RemoteDirectory;
  SyncParams.Params = SynchronizeParams;
  SyncParams.Options = soRecurse;
  Timer.start();
  Controller.Start(SyncParams, Param, SynchronizeOptions());
  if (!Aborted)
  {
    Loop.exec();
  }
  Timer.stop();
  Controller.Stop();
  bool Interrupted = Console::Instance().PendingAbort();
  if (Aborted || Interrupted)
  {
    // SynchronizeControllerAbort / Aborted(): the script cannot continue
    NotifyAbort();
  }
  if (Aborted && !AbortMessage.isEmpty())
  {
    if (AbortClose)
    {
      throw FatalException(AbortMessage, AbortMore);
    }
    throw ExtException(AbortMessage, AbortMore);
  }
  if (Interrupted)
  {
    FInterrupted = true;
    throw AbortException();
  }
}

void Script::ExitProc(ScriptProcParams & /*Params*/)
{
  FContinue = false;
}

void Script::OpenProc(ScriptProcParams & Params)
{
  QString SessionName = (Params.ParamCount() > 0) ? Params.Param(1) : QString();
  Connect(SessionName, Params, true);
}

void Script::Input(const QString & Prompt, QString & Str, bool AllowEmpty)
{
  // TManagementScript::Input
  do
  {
    Str.clear();
    Print(Prompt);
    bool Ok;
    if ((FBatch != BatchOff) && !Console::Instance().InputIsTty())
    {
      Ok = false;
    }
    else
    {
      Ok = Console::Instance().Input(Str, true, InputTimeout());
    }
    if (!Ok)
    {
      NotifyAbort();
      throw AbortException();
    }
  }
  while (Str.trimmed().isEmpty() && !AllowEmpty);
}

void Script::Connect(const QString & SessionName, Options & Opts, bool CheckParams)
{
  // TManagementScript::Connect
  auto NewSession = std::make_unique<Session>();
  int ParsedInfo = 0;
  bool DefaultsOnly = false;
  if (Opts.FindSwitch(QStringLiteral("filezilla")))
  {
    // -filezilla: the site of FileZilla's Site Manager
    // (Configuration->SelectFilezillaSessionsForImport)
    ImportSessionList FilezillaSessions = SiteImport::SelectFilezillaSessionsForImport(*FStoredSessions);
    if (!FilezillaSessions.Error.isEmpty())
    {
      throw ExtException(FilezillaSessions.Error);
    }
    std::shared_ptr<SessionData> Site = FilezillaSessions.FindByName(SessionName);
    if (!Site)
    {
      // FILEZILLA_SITE_NOT_EXIST
      throw ExtException(QStringLiteral("FileZilla site \"%1\" was not found.").arg(SessionName));
    }
    NewSession->Data = *Site;
    ParsedInfo = piProtocolDefined;
  }
  else
  {
  for (const QString & Folder : FStoredSessions->Folders())
  {
    if (!SessionName.isEmpty() && (Folder.compare(SessionName, Qt::CaseInsensitive) == 0))
    {
      throw ExtException(QStringLiteral("Cannot open site folder or workspace."));
    }
  }
  for (const auto & Site : FStoredSessions->Sessions())
  {
    // TStoredSessionList::IsFolder / IsWorkspace
    if (!SessionName.isEmpty() &&
        (Site->Name.startsWith(SessionName + QLatin1Char('/'), Qt::CaseInsensitive) ||
         (Site->IsWorkspace && (Site->Name.compare(SessionName, Qt::CaseInsensitive) == 0))))
    {
      throw ExtException(QStringLiteral("Cannot open site folder or workspace."));
    }
  }

  ParseSessionUrl(SessionName, &Opts, FStoredSessions, NewSession->Data, NewSession->Extras, ParsedInfo);
  DefaultsOnly = (ParsedInfo & piDefaultsOnly) != 0;
  }
  SessionData & Data = NewSession->Data;

  if (CheckParams)
  {
    if (Opts.ParamCount() > 1)
    {
      throw ExtException(QStringLiteral("Too many parameters for command '%1'.").arg(QStringLiteral("open")));
    }
    QString Switch;
    if (Opts.UnusedSwitch(Switch))
    {
      throw ExtException(QStringLiteral("Unknown switch '%1'.").arg(Switch));
    }
  }

  if (!SessionName.isEmpty() && !Data.Name.isEmpty() && !(ParsedInfo & piProtocolDefined) && (FBatch != BatchOff) && FUsageWarnings)
  {
    // SCRIPT_SITE_WARNING + GenerateOpenCommandArgs
    PrintLine(QStringLiteral("In scripting you should not rely on saved sites, use this command instead:"));
    SessionData Masked = Data;
    QString Url = QStringLiteral("%1://%2%3@%4%5/").arg(
      (Data.FSProtocol == fsFTP) ? ((Data.Ftps == ftpsImplicit) ? QStringLiteral("ftps") : ((Data.Ftps == ftpsNone) ? QStringLiteral("ftp") : QStringLiteral("ftpes"))) :
        ((Data.FSProtocol == fsSCPonly) ? QStringLiteral("scp") : QStringLiteral("sftp")),
      QString::fromLatin1(QUrl::toPercentEncoding(Data.UserName)),
      Data.Password.isEmpty() ? QString() : QStringLiteral(":***"),
      Data.HostName,
      (Data.PortNumber == SessionData::DefaultPort(Data.FSProtocol, Data.Ftps)) ? QString() : QStringLiteral(":%1").arg(Data.PortNumber));
    QString Command = QStringLiteral("open ") + QuoteParam(Url);
    // GenerateOpenCommandArgs: the switches of the settings in use
    if (!Data.HostKey.isEmpty())
    {
      bool UsesSsh = (Data.FSProtocol != fsFTP) && (Data.FSProtocol != fsWebDAV) && (Data.FSProtocol != fsS3);
      Command += QStringLiteral(" -%1=").arg(UsesSsh ? QStringLiteral("hostkey") : QStringLiteral("certificate")) + QuoteParam(Data.HostKey);
    }
    if (!Data.PublicKeyFile.isEmpty())
    {
      Command += QStringLiteral(" -privatekey=") + QuoteParam(Data.PublicKeyFile);
    }
    if (!Data.TlsCertificateFile.isEmpty())
    {
      Command += QStringLiteral(" -clientcert=") + QuoteParam(Data.TlsCertificateFile);
    }
    PrintLine(Command);
  }

  if (Data.NoteOnly)
  {
    throw ExtException(QStringLiteral("\"%1\" is a note in NixSCP, not a site: it cannot be opened.").arg(Data.Name));
  }
  if (!Data.CanLogin() || DefaultsOnly)
  {
    if (Data.HostName.isEmpty())
    {
      QString Value;
      Input(QStringLiteral("Host: "), Value, false);
      Data.HostName = Value.trimmed();
    }
  }

  // NixSCP: hidden switch for tests and sandboxes, known_hosts file to use
  QByteArray KnownHosts = qgetenv("NIXSCP_KNOWN_HOSTS");
  if (Data.KnownHostsFile.isEmpty() && !KnownHosts.isEmpty())
  {
    Data.KnownHostsFile = QFile::decodeName(KnownHosts);
  }

  if (FXmlLog)
  {
    FXmlLog->SetSession(Data);
  }
  Session & S = *NewSession;
  S.UI = std::make_unique<ConsoleUserInterface>(this, Data, S.Extras);
  S.Term = std::make_unique<Terminal>(Data, S.UI.get());
  // the session settings and the connection are logged by the session (Terminal::Open)
  PrintLine(QStringLiteral("Searching for host..."));
  PrintLine(QStringLiteral("Connecting to host..."));
  S.UI->SetOpening(true);
  OperationResult Result = Wait(S, [&](Terminal::Callback Done) { S.Term->Open(Done); });
  if (Result.Ok)
  {
    S.UI->PrintAuthenticating();
  }
  S.UI->SetOpening(false);
  if (!Result.Ok)
  {
    QString HostKeyError = S.UI->HostKeyError();
    QString HostKeyDetail = S.UI->HostKeyErrorDetail();
    bool AuthenticationCancelled = S.UI->AuthenticationCancelled();
    bool StoredCredentialsTried = S.UI->StoredCredentialsTried();
    NewSession.reset();
    if (Result.Aborted)
    {
      // TSecureShell: a cancelled authentication prompt (credentials given
      // on the command line already refused, or no input in batch mode)
      // closes the connection with an error, not silently
      if (AuthenticationCancelled && !FInterrupted && !Console::Instance().PendingAbort())
      {
        throw ExtException(StoredCredentialsTried ? QStringLiteral("Authentication failed.") :
          QStringLiteral("Credentials were not specified."));
      }
      throw AbortException();
    }
    if (!HostKeyError.isEmpty())
    {
      throw ExtException(HostKeyError, HostKeyDetail);
    }
    throw ExtException(Result.Message, Result.MoreMessages);
  }
  PrintLine(QStringLiteral("Authenticated."));
  PrintLine(QStringLiteral("Starting the session..."));
  S.CurrentDirectory = S.Term->CurrentDirectory();
  auto Home = std::make_shared<QString>();
  RunWorker(S, [Home](FileSystem & Fs, FileOperationProgress &, UserInterface &) { *Home = Fs.HomeDirectory(); });
  S.HomeDirectory = Home->isEmpty() ? S.CurrentDirectory : *Home;
  PrintLine(QStringLiteral("Session started."));
  Log(QLatin1Char('.'), QStringLiteral("Session started (%1, %2)").arg(S.Term->ProtocolName(), S.Term->ServerInfo()));

  FSessions.push_back(std::move(NewSession));
  FSession = &S;
  UpdateTitle();

  if (!Data.LocalDirectory.isEmpty())
  {
    try
    {
      DoChangeLocalDirectory(Data.LocalDirectory);
    }
    catch (const ExtException & E)
    {
      ShowException(E);
    }
  }
  PrintActiveSession();
}

int Script::SessionIndex(const Session * S) const
{
  for (size_t I = 0; I < FSessions.size(); I++)
  {
    if (FSessions[I].get() == S)
    {
      return static_cast<int>(I);
    }
  }
  return -1;
}

Script::Session * Script::FindSession(const QString & Index)
{
  bool Ok = false;
  int I = Index.toInt(&Ok);
  if (!Ok || (I <= 0) || (I > static_cast<int>(FSessions.size())))
  {
    throw ExtException(QStringLiteral("Invalid session number '%1'.").arg(Index));
  }
  return FSessions[I - 1].get();
}

void Script::PrintActiveSession()
{
  PrintLine(QStringLiteral("Active session: [%1] %2").arg(SessionIndex(FSession) + 1).arg(FSession->Data.SessionName()));
}

void Script::DoClose(Session * S)
{
  int Index = SessionIndex(S);
  if (Index < 0)
  {
    return;
  }
  bool WasActive = (FSession == S);
  QString SessionName = S->Data.SessionName();
  FSessions.erase(FSessions.begin() + Index);
  if (WasActive)
  {
    FSession = nullptr;
  }
  PrintLine(QStringLiteral("Session '%1' closed.").arg(SessionName));
  if (WasActive)
  {
    if (!FSessions.empty())
    {
      FSession = (Index < static_cast<int>(FSessions.size())) ? FSessions[Index].get() : FSessions[0].get();
      PrintActiveSession();
    }
    else
    {
      PrintLine(QStringLiteral("No session."));
    }
  }
  UpdateTitle();
}

void Script::CloseProc(ScriptProcParams & Params)
{
  CheckSession();
  Session * S = (Params.ParamCount() == 0) ? FSession : FindSession(Params.Param(1));
  DoClose(S);
}

void Script::SessionProc(ScriptProcParams & Params)
{
  CheckSession();
  if (Params.ParamCount() == 0)
  {
    for (size_t I = 0; I < FSessions.size(); I++)
    {
      PrintLine(QString::asprintf("%3d  %s", static_cast<int>(I + 1), qPrintable(FSessions[I]->Data.SessionName())));
    }
  }
  else
  {
    FSession = FindSession(Params.Param(1));
    UpdateTitle();
  }
  PrintActiveSession();
}

void Script::DoChangeLocalDirectory(const QString & Directory)
{
  if (!QDir::setCurrent(LocalAbsolutePath(Directory)))
  {
    throw ExtException(QStringLiteral("Error changing directory to '%1'.").arg(Directory));
  }
}

void Script::LPwdProc(ScriptProcParams & /*Params*/)
{
  PrintLine(QDir::currentPath());
}

void Script::LCdProc(ScriptProcParams & Params)
{
  DoChangeLocalDirectory(Params.Param(1));
  PrintLine(QDir::currentPath());
}

void Script::LLsProc(ScriptProcParams & Params)
{
  QString Directory;
  QString Mask;
  if (Params.ParamCount() > 0)
  {
    Directory = Params.Param(1);
    Mask = LocalExtractFileName(Directory);
    if (IsLocalMask(Mask))
    {
      Directory = LocalExtractFilePath(Directory);
    }
    else
    {
      Mask.clear();
    }
  }
  if (Directory.isEmpty())
  {
    Directory = QDir::currentPath();
  }
  if (Mask.isEmpty())
  {
    Mask = QStringLiteral("*.*");
  }
  QDir Dir(LocalAbsolutePath(Directory));
  if (!Dir.exists())
  {
    NoMatch(Mask, QStringLiteral("No such file or directory"));
    return;
  }
  FileMasks Masks;
  Masks.SetMask(LocalMask(Mask));
  QFileInfoList Entries = Dir.entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDot, QDir::Name);
  QLocale Locale;
  int Count = 0;
  for (const QFileInfo & Info : Entries)
  {
    FileMasks::Params MaskParams;
    MaskParams.Size = Info.isDir() ? 0 : Info.size();
    MaskParams.Modification = Info.lastModified();
    // FindFirstFile matches ".." with "*.*" (and "*") only
    bool Match = (Info.fileName() == QStringLiteral("..")) ?
      ((Mask == QStringLiteral("*.*")) || (Mask == QStringLiteral("*"))) : Masks.MatchesFileName(Info.fileName(), false, &MaskParams);
    if (!Match)
    {
      continue;
    }
    QDateTime DateTime = Info.lastModified();
    QString DateStr = DateTime.toString(QStringLiteral("yyyy-MM-dd"));
    QString TimeStr = DateTime.toString(QStringLiteral("HH:mm:ss"));
    QString SizeStr = Info.isDir() ? QStringLiteral("<DIR>") : QStringLiteral("%1").arg(Locale.toString(Info.size()), 14);
    PrintLine(QString::asprintf("%s  %s    %-14s %s", qPrintable(DateStr), qPrintable(TimeStr), qPrintable(SizeStr),
      qPrintable(Info.fileName())));
    Count++;
  }
  if (Count == 0)
  {
    NoMatch(Mask, QString());
  }
}
