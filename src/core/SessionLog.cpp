// Port of WinSCP source/core/SessionInfo.cpp (TSessionLog) and
// GetExpandedLogFileName (core/SessionData.cpp)
#include "core/SessionLog.h"
#include "core/ActionsLog.h"
#include "core/Configuration.h"
#include "core/FileSystem.h"
#include "core/SessionData.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLocale>
#include <QMetaObject>
#include <QMutex>
#include <QObject>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTimeZone>
#include <QtGlobal>

#include <libssh/libssh.h>
#include <libssh/callbacks.h>
#include <openssl/crypto.h>

#include <atomic>
#include <optional>
#include <pwd.h>
#include <unistd.h>

//---------------------------------------------------------------------------
// The file shared by all logs with the same expanded name

struct SessionLog::Writer
{
  QMutex Mutex;
  QFile File;
  QString FileName;
  qint64 Size = 0;
  bool Failed = false;

  // TSessionLog::CheckSize
  void CheckSize(qint64 MaxSize, int MaxCount, qint64 Addition)
  {
    if ((MaxSize > 0) && (Size > 0) && (Size + Addition >= MaxSize))
    {
      File.close();
      Size = 0;
      int Index = 0;
      while (QFileInfo::exists(SessionLog::PartFileName(FileName, Index + 1)))
      {
        Index++;
      }
      do
      {
        QString LogPart = SessionLog::PartFileName(FileName, Index);
        if ((MaxCount > 0) && (Index >= MaxCount))
        {
          QFile::remove(LogPart);
        }
        else
        {
          QString Next = SessionLog::PartFileName(FileName, Index + 1);
          QFile::remove(Next);
          QFile::rename(LogPart, Next);
        }
        Index--;
      }
      while (Index >= 0);
      File.setFileName(FileName);
      if (!File.open(QIODevice::WriteOnly | QIODevice::Truncate))
      {
        Failed = true;
        return;
      }
      File.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }
  }
};

namespace
{
QMutex RegistryMutex;
QHash<QString, std::weak_ptr<void>> & Registry()
{
  static QHash<QString, std::weak_ptr<void>> Result;
  return Result;
}

thread_local std::shared_ptr<SessionLog> CurrentLog;
std::atomic<bool> ConfigurationLoaded{false};
thread_local bool SshHooked = false;

QMutex ProcessLogMutex;
std::shared_ptr<SessionLog> & ProcessLogRef()
{
  static std::shared_ptr<SessionLog> Result;
  return Result;
}

const char LogLineMarks[] = "<>!.*";

void SshLogCallback(int /*Priority*/, const char * /*Function*/, const char * Buffer, void * /*UserData*/)
{
  SessionLog * Log = CurrentLog.get();
  if ((Log != nullptr) && (Buffer != nullptr))
  {
    Log->Add(llMessage, QString::fromUtf8(Buffer));
  }
}

QString BooleanToEngStr(bool B)
{
  return B ? QStringLiteral("Yes") : QStringLiteral("No");
}

// EnumName (core/SessionInfo.cpp)
QString EnumName(int Value, const QString & Names)
{
  QStringList List = Names.split(QLatin1Char(';'));
  if ((Value >= 0) && (Value < List.size()))
  {
    return List[Value];
  }
  return QStringLiteral("(unknown %1)").arg(Value);
}

const QString AutoSwitchNames = QStringLiteral("On;Off;Auto");
const QString NotAutoSwitchNames = QStringLiteral("Off;On;Auto");

// SizeToStr (core/Common.cpp)
QString SizeToStr(qint64 Size)
{
  if ((Size <= 0) || ((Size % 1024) != 0))
  {
    return QString::number(Size);
  }
  Size /= 1024;
  if ((Size % 1024) != 0)
  {
    return QString::number(Size) + QLatin1Char('K');
  }
  Size /= 1024;
  if ((Size % 1024) != 0)
  {
    return QString::number(Size) + QLatin1Char('M');
  }
  return QString::number(Size / 1024) + QLatin1Char('G');
}

// MakeValidFileName (core/Common.cpp)
QString MakeValidFileName(QString Str)
{
  static const QString IllegalChars = QStringLiteral(":;,=+<>|\"[] \\/?*");
  for (QChar & C : Str)
  {
    if (IllegalChars.contains(C) || (C.unicode() < 32))
    {
      C = QLatin1Char('_');
    }
  }
  return Str;
}

// FormatTimeZone (core/Common.cpp)
QString FormatTimeZone(int Sec)
{
  QString Sign = (Sec < 0) ? QStringLiteral("-") : QStringLiteral("+");
  int Abs = qAbs(Sec);
  int Hours = Abs / 3600;
  int Minutes = (Abs % 3600) / 60;
  QString Result = Sign + QString::number(Hours);
  if (Minutes != 0)
  {
    Result += QStringLiteral(":%1").arg(Minutes, 2, 10, QLatin1Char('0'));
  }
  return Result;
}

// GetTimeZoneLogString (core/Common.cpp)
QString TimeZoneLogString()
{
  QDateTime Now = QDateTime::currentDateTime();
  QTimeZone Zone = QTimeZone::systemTimeZone();
  QString Result = QStringLiteral("Current: GMT%1").arg(FormatTimeZone(Zone.offsetFromUtc(Now)));
  if (!Zone.hasDaylightTime())
  {
    Result += QStringLiteral(" (%1), No DST").arg(Zone.abbreviation(Now));
  }
  else
  {
    QDate Today = Now.date();
    QDateTime YearStart(QDate(Today.year(), 1, 1), QTime(0, 0));
    QDateTime YearEnd(QDate(Today.year(), 12, 31), QTime(23, 59, 59));
    QString StandardName, DaylightName, DstStart, DstEnd;
    int StandardOffset = Zone.standardTimeOffset(Now);
    int DaylightOffset = StandardOffset;
    const QTimeZone::OffsetDataList Transitions = Zone.transitions(YearStart, YearEnd);
    for (const QTimeZone::OffsetData & Data : Transitions)
    {
      if (Data.daylightTimeOffset != 0)
      {
        DaylightName = Data.abbreviation;
        DaylightOffset = Data.offsetFromUtc;
        DstStart = QLocale::c().toString(Data.atUtc.toLocalTime().date(), QStringLiteral("M/d/yyyy"));
      }
      else
      {
        StandardName = Data.abbreviation;
        DstEnd = QLocale::c().toString(Data.atUtc.toLocalTime().date(), QStringLiteral("M/d/yyyy"));
      }
    }
    Result += QStringLiteral(", Standard: GMT%1 (%2), DST: GMT%3 (%4), DST Start: %5, DST End: %6")
      .arg(FormatTimeZone(StandardOffset), StandardName, FormatTimeZone(DaylightOffset), DaylightName, DstStart, DstEnd);
  }
  return Result;
}

QString ProcessName(qint64 Pid, qint64 * ParentPid)
{
  QFile Stat(QStringLiteral("/proc/%1/stat").arg(Pid));
  if (!Stat.open(QIODevice::ReadOnly))
  {
    return QString();
  }
  QByteArray Data = Stat.readAll();
  qsizetype Open = Data.indexOf('(');
  qsizetype Close = Data.lastIndexOf(')');
  if ((Open < 0) || (Close < Open))
  {
    return QString();
  }
  QString Name = QString::fromLocal8Bit(Data.mid(Open + 1, Close - Open - 1));
  QList<QByteArray> Fields = Data.mid(Close + 2).split(' ');
  if ((ParentPid != nullptr) && (Fields.size() > 1))
  {
    *ParentPid = Fields[1].toLongLong();
  }
  return Name;
}

// GetAncestorProcessNames (core/Common.cpp), from /proc
QString AncestorProcessNames()
{
  QStringList Names;
  qint64 Pid = getppid();
  for (int Depth = 0; (Depth < 20) && (Pid > 1); Depth++)
  {
    qint64 Parent = 0;
    QString Name = ProcessName(Pid, &Parent);
    if (Name.isEmpty())
    {
      break;
    }
    Names.append(Name);
    Pid = Parent;
  }
  return Names.isEmpty() ? QStringLiteral("unknown") : Names.join(QStringLiteral(", "));
}

// GetOSInfo
QString OSInfo()
{
  return QStringLiteral("%1 %2 - %3").arg(QSysInfo::kernelType(), QSysInfo::kernelVersion(), QSysInfo::prettyProductName());
}

QString DesktopInfo()
{
  QString Desktop = qEnvironmentVariable("XDG_CURRENT_DESKTOP");
  QString SessionType = qEnvironmentVariable("XDG_SESSION_TYPE");
  if (SessionType.isEmpty())
  {
    SessionType = qEnvironmentVariableIsSet("WAYLAND_DISPLAY") ? QStringLiteral("wayland") :
      (qEnvironmentVariableIsSet("DISPLAY") ? QStringLiteral("x11") : QStringLiteral("none"));
  }
  return QStringLiteral("%1 (%2)").arg(Desktop.isEmpty() ? QStringLiteral("unknown") : Desktop, SessionType);
}

QString LocalAccount()
{
  struct passwd * Pw = getpwuid(getuid());
  if (Pw != nullptr)
  {
    return QString::fromLocal8Bit(Pw->pw_name);
  }
  QString User = qEnvironmentVariable("USER");
  return User.isEmpty() ? QStringLiteral("<Failed to retrieve username>") : User;
}

// TSessionData::GetFSProtocolStr (FSProtocolNames)
QString FSProtocolStr(TFSProtocol Protocol)
{
  switch (Protocol)
  {
    case fsSCPonly: return QStringLiteral("SCP");
    case fsSFTP: return QStringLiteral("SFTP (SCP)");
    case fsSFTPonly: return QStringLiteral("SFTP");
    case fsFTP: return QStringLiteral("FTP");
    case fsWebDAV: return QStringLiteral("WebDAV");
    case fsS3: return QStringLiteral("S3");
  }
  return QString();
}

QString TlsVersionName(TTlsVersion Version)
{
  switch (Version)
  {
    case tls10: return QStringLiteral("TLSv1.0");
    case tls11: return QStringLiteral("TLSv1.1");
    case tls12: return QStringLiteral("TLSv1.2");
    case tls13: return QStringLiteral("TLSv1.3");
  }
  return QStringLiteral("TLSv?");
}
}

//---------------------------------------------------------------------------
// Temporary (command-line) logging settings

namespace
{
struct LoggingSettings
{
  bool Logging = false;
  QString LogFileName;
  bool LogFileAppend = true;
  int LogProtocol = 0;
  bool LogSensitive = false;
  qint64 LogMaxSize = 0;
  int LogMaxCount = 0;
  bool LogActions = false;
  QString ActionsLogFileName;

  bool operator==(const LoggingSettings & Other) const = default;

  static LoggingSettings From(const ConfigurationData & C)
  {
    return LoggingSettings{C.Logging, C.LogFileName, C.LogFileAppend, C.LogProtocol, C.LogSensitive,
      C.LogMaxSize, C.LogMaxCount, C.LogActions, C.ActionsLogFileName};
  }

  void To(ConfigurationData & C) const
  {
    C.Logging = Logging;
    C.LogFileName = LogFileName;
    C.LogFileAppend = LogFileAppend;
    C.LogProtocol = LogProtocol;
    C.LogSensitive = LogSensitive;
    C.LogMaxSize = LogMaxSize;
    C.LogMaxCount = LogMaxCount;
    C.LogActions = LogActions;
    C.ActionsLogFileName = ActionsLogFileName;
  }
};

std::optional<LoggingSettings> PermanentLogging;
std::optional<LoggingSettings> TemporaryLogging;
}

void SessionLog::BeginTemporaryLogging(const ConfigurationData & Config)
{
  if (!PermanentLogging)
  {
    PermanentLogging = LoggingSettings::From(Config);
  }
}

void SessionLog::EndTemporaryLogging(const ConfigurationData & Config)
{
  if (PermanentLogging)
  {
    TemporaryLogging = LoggingSettings::From(Config);
  }
}

void SessionLog::ClearTemporaryLogging()
{
  PermanentLogging.reset();
  TemporaryLogging.reset();
}

SessionLog::PermanentLoggingScope::PermanentLoggingScope(ConfigurationData & Config) :
  FConfig(Config)
{
  // only while the configuration has the values set by the switches
  // (not e.g. when the defaults are being written)
  if (PermanentLogging && TemporaryLogging && (LoggingSettings::From(Config) == *TemporaryLogging))
  {
    PermanentLogging->To(Config);
    FSwapped = true;
  }
}

SessionLog::PermanentLoggingScope::~PermanentLoggingScope()
{
  if (FSwapped)
  {
    TemporaryLogging->To(FConfig);
  }
}

//---------------------------------------------------------------------------

SessionLogOptions SessionLogOptions::FromConfiguration(const Configuration & Config)
{
  SessionLogOptions Result;
  Result.Logging = Config.Logging;
  Result.FileName = Config.LogFileName;
  Result.Append = Config.LogFileAppend;
  Result.Protocol = Config.LogProtocol;
  Result.Sensitive = Config.LogSensitive;
  Result.MaxSize = Config.LogMaxSize;
  Result.MaxCount = Config.LogMaxCount;
  return Result;
}

//---------------------------------------------------------------------------

SessionLog::SessionLog(const SessionLogOptions & Options, const SessionData * Data, const QDateTime & Started) :
  FOptions(Options),
  FStarted(Started)
{
  if (Data != nullptr)
  {
    FData = std::make_unique<SessionData>(*Data);
  }
  // TSessionLog::ReflectSettings / LogToFile
  FLogging = FOptions.Logging && !FOptions.FileName.isEmpty();
}

SessionLog::~SessionLog()
{
  if (FWriter)
  {
    QMutexLocker Lock(&FWriter->Mutex);
    FWriter->File.flush();
  }
}

QString SessionLog::Separator()
{
  // TSessionLog::GetSeparator
  return QStringLiteral("--------------------------------------------------------------------------");
}

QString SessionLog::DefaultLogFileName()
{
  return QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + QStringLiteral("/nixscp/!S.log");
}

QString SessionLog::DefaultActionsLogFileName()
{
  return QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + QStringLiteral("/nixscp/!S.xml");
}

QString SessionLog::LevelName(int Protocol)
{
  if (Protocol <= -1)
  {
    return QStringLiteral("Reduced");
  }
  if (Protocol == 0)
  {
    return QStringLiteral("Normal");
  }
  if (Protocol == 1)
  {
    return QStringLiteral("Debug 1");
  }
  return QStringLiteral("Debug 2");
}

int SessionLog::SshLogLevel(int Protocol)
{
  if (Protocol <= -1)
  {
    return SSH_LOG_WARNING;
  }
  if (Protocol == 0)
  {
    return SSH_LOG_PROTOCOL;
  }
  if (Protocol == 1)
  {
    return SSH_LOG_PACKET;
  }
  return SSH_LOG_FUNCTIONS;
}

QString SessionLog::PartFileName(const QString & BaseName, int Index)
{
  return (Index >= 1) ? QStringLiteral("%1.%2").arg(BaseName).arg(Index) : BaseName;
}

QString SessionLog::ExpandFileName(const QString & FileName, const QDateTime & Started, const SessionData * Data)
{
  // GetExpandedLogFileName
  QString Name = SessionData::ExpandEnvironmentVariables(FileName);
  QString Result;
  Result.reserve(Name.size());
  for (qsizetype Index = 0; Index < Name.size(); Index++)
  {
    QChar C = Name[Index];
    if ((C == QLatin1Char('!')) && (Index + 1 < Name.size()))
    {
      QChar P = Name[Index + 1].toLower();
      QString Replacement;
      switch (P.unicode())
      {
        case 'y': Replacement = Started.toString(QStringLiteral("yyyy")); break;
        case 'm': Replacement = Started.toString(QStringLiteral("MM")); break;
        case 'd': Replacement = Started.toString(QStringLiteral("dd")); break;
        case 't': Replacement = Started.toString(QStringLiteral("HHmmss")); break;
        case 'p': Replacement = QString::number(QCoreApplication::applicationPid()); break;
        case '@':
          Replacement = (Data != nullptr) ? MakeValidFileName(Data->HostNameExpanded()) : QStringLiteral("nohost");
          break;
        case 's':
          Replacement = (Data != nullptr) ? MakeValidFileName(Data->SessionName()) : QStringLiteral("nosession");
          break;
        case '!': Replacement = QStringLiteral("!"); break;
        default: Replacement = QStringLiteral("!") + Name[Index + 1]; break;
      }
      Result += Replacement;
      Index++;
    }
    else
    {
      Result += C;
    }
  }
  return Result;
}

QString SessionLog::MaskCommandLine(const QStringList & Arguments)
{
  // TSessionLog::GetCmdLineLog (MaskPasswordInCommandLine, simplified):
  // passwords in session URLs and in password switches
  static const QRegularExpression UrlPassword(QStringLiteral("^([A-Za-z][A-Za-z0-9+.-]*://[^:@/;]*):([^@]*)@"));
  static const QRegularExpression SwitchPassword(
    QStringLiteral("^([-/](?:password|passphrase|tunnelpassword|proxypassword|passwordsfromfiles)=)(.+)$"),
    QRegularExpression::CaseInsensitiveOption);
  QStringList Result;
  for (QString Arg : Arguments)
  {
    QRegularExpressionMatch Match = UrlPassword.match(Arg);
    if (Match.hasMatch())
    {
      Arg = Match.captured(1) + QStringLiteral(":***@") + Arg.mid(Match.capturedEnd(0));
    }
    Match = SwitchPassword.match(Arg);
    if (Match.hasMatch())
    {
      Arg = Match.captured(1) + QStringLiteral("***");
    }
    if (Arg.contains(QLatin1Char(' ')))
    {
      Arg = QLatin1Char('"') + Arg + QLatin1Char('"');
    }
    Result.append(Arg);
  }
  return Result.join(QLatin1Char(' '));
}

bool SessionLog::Open(QString * Error)
{
  QMutexLocker Lock(&FOpenMutex);
  return DoOpen(Error);
}

std::shared_ptr<SessionLog::Writer> SessionLog::OpenedWriter()
{
  QMutexLocker Lock(&FOpenMutex);
  if (!FWriter)
  {
    DoOpen(nullptr);
  }
  return FWriter;
}

bool SessionLog::DoOpen(QString * Error)
{
  // FOpenMutex is locked
  if (FWriter)
  {
    return true;
  }
  if (!FOptions.Logging || FOptions.FileName.isEmpty())
  {
    FLogging = false;
    return false;
  }
  QString Name = QFileInfo(ExpandFileName(FOptions.FileName, FStarted, FData.get())).absoluteFilePath();
  std::shared_ptr<Writer> W;
  {
    QMutexLocker Lock(&RegistryMutex);
    auto It = Registry().find(Name);
    if (It != Registry().end())
    {
      W = std::static_pointer_cast<Writer>(It.value().lock());
    }
    if (!W || W->Failed)
    {
      W = std::make_shared<Writer>();
      W->FileName = Name;
      QDir().mkpath(QFileInfo(Name).absolutePath());
      bool Existed = QFileInfo::exists(Name);
      W->File.setFileName(Name);
      if (!W->File.open(QIODevice::WriteOnly | (FOptions.Append ? QIODevice::Append : QIODevice::Truncate)))
      {
        // LOG_OPENERROR
        FLastError = QStringLiteral("Can't open log file '%1'.\n%2").arg(Name, W->File.errorString());
        FLogging = false;
        if (Error != nullptr)
        {
          *Error = FLastError;
        }
        return false;
      }
      if (!Existed)
      {
        // the log can contain sensitive data
        W->File.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
      }
      W->Size = W->File.size();
      Registry().insert(Name, std::static_pointer_cast<void>(W));
    }
  }
  FWriter = W;
  {
    // in case we are appending and the existing log file is already too large
    QMutexLocker Lock(&FWriter->Mutex);
    FWriter->CheckSize(FOptions.MaxSize, FOptions.MaxCount, 0);
  }
  return true;
}

QString SessionLog::CurrentFileName() const
{
  QMutexLocker Lock(&FOpenMutex);
  if (FWriter)
  {
    return FWriter->FileName;
  }
  if (FOptions.FileName.isEmpty())
  {
    return QString();
  }
  return QFileInfo(ExpandFileName(FOptions.FileName, FStarted, FData.get())).absoluteFilePath();
}

void SessionLog::DoAdd(Writer & W, LogLineType Type, const QString & Line)
{
  // TSessionLog::DoAddToSelf; called with the writer locked
  QString Timestamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
  QString Prefix = FName.isEmpty() ? QString() : (QLatin1Char('[') + FName + QStringLiteral("] "));
  QByteArray Data = (QString(QLatin1Char(LogLineMarks[Type])) + QLatin1Char(' ') + Timestamp + QLatin1Char(' ') + Prefix + Line + QLatin1Char('\n')).toUtf8();
  W.CheckSize(FOptions.MaxSize, FOptions.MaxCount, Data.size());
  if (W.Failed)
  {
    FLogging = false;
    return;
  }
  qint64 Written = W.File.write(Data);
  if (Written != Data.size())
  {
    // LOG_GEN_ERROR: logging is turned off
    W.Failed = true;
    FLogging = false;
    FLastError = W.File.errorString();
    return;
  }
  W.Size += Written;
}

void SessionLog::Add(LogLineType Type, const QString & Line)
{
  if (!FLogging)
  {
    return;
  }
  std::shared_ptr<Writer> W = OpenedWriter();
  if (!W)
  {
    return;
  }
  QMutexLocker Lock(&W->Mutex);
  // TSessionLog::DoAdd: line by line (CutToChar)
  QString Rest = Line;
  Rest.remove(QLatin1Char('\r'));
  while (!Rest.isEmpty() && FLogging)
  {
    qsizetype P = Rest.indexOf(QLatin1Char('\n'));
    if (P < 0)
    {
      DoAdd(*W, Type, Rest);
      Rest.clear();
    }
    else
    {
      DoAdd(*W, Type, Rest.left(P));
      Rest.remove(0, P + 1);
    }
  }
  W->File.flush();
}

void SessionLog::AddException(const QString & Message, const QString & MoreMessages)
{
  // ExceptionLogString
  QString Str = QStringLiteral("(ExtException) ") + Message;
  if (!MoreMessages.isEmpty())
  {
    Str += QLatin1Char('\n') + MoreMessages;
  }
  Add(llException, Str);
}

void SessionLog::AddSeparator()
{
  Add(llMessage, Separator());
}

QString SessionLog::Sensitive(const QString & Str) const
{
  if (FOptions.Sensitive && !Str.isEmpty())
  {
    return Str;
  }
  return BooleanToEngStr(!Str.isEmpty());
}

void SessionLog::AddSystemInfo(const QString & CommandLine)
{
  // TSessionLog::DoAddStartupInfo(NULL)
  if (!FLogging || FSystemInfoAdded)
  {
    return;
  }
  FSystemInfoAdded = true;
  QString Version = QCoreApplication::applicationVersion();
  QStringList Lines;
  Lines << Separator();
  Lines << QStringLiteral("NixSCP Version %1 (OS %2)").arg(Version.isEmpty() ? QStringLiteral("unknown") : Version, OSInfo());
  Lines << QStringLiteral("Libraries: libssh %1, %2, Qt %3")
    .arg(QString::fromLatin1(ssh_version(0)), QString::fromLatin1(OpenSSL_version(OPENSSL_VERSION)), QString::fromLatin1(qVersion()));
  Lines << QStringLiteral("Desktop: %1").arg(DesktopInfo());
  // (the configuration is not created just for the log)
  QString ConfigurationSource = QStringLiteral("nul");
  if (ConfigurationLoaded && !Configuration::Instance().IsNulStorage())
  {
    ConfigurationSource = Configuration::Instance().IniFileName();
  }
  Lines << QStringLiteral("Configuration: %1").arg(ConfigurationSource);
  QString LogStr = LevelName(FOptions.Protocol);
  if (FOptions.Sensitive)
  {
    LogStr += QStringLiteral(", Logging passwords");
  }
  if (FOptions.MaxSize > 0)
  {
    LogStr += QStringLiteral(", Rotating after: %1").arg(SizeToStr(FOptions.MaxSize));
    if (FOptions.MaxCount > 0)
    {
      LogStr += QStringLiteral(", Keeping at most %1 logs").arg(FOptions.MaxCount);
    }
  }
  Lines << QStringLiteral("Log level: %1").arg(LogStr);
  Lines << QStringLiteral("Local account: %1").arg(LocalAccount());
  Lines << QStringLiteral("Working directory: %1").arg(QDir::currentPath());
  Lines << QStringLiteral("Process ID: %1").arg(QCoreApplication::applicationPid());
  Lines << QStringLiteral("Ancestor processes: %1").arg(AncestorProcessNames());
  QString ACommandLine = CommandLine;
  if (ACommandLine.isEmpty())
  {
    QStringList Arguments = QCoreApplication::arguments();
    ACommandLine = FOptions.Sensitive ? Arguments.join(QLatin1Char(' ')) : MaskCommandLine(Arguments);
  }
  Lines << QStringLiteral("Command-line: %1").arg(ACommandLine);
  Lines << QStringLiteral("Time zone: %1").arg(TimeZoneLogString());
  Lines << QStringLiteral("Login time: %1").arg(
    QLocale(QLocale::English, QLocale::UnitedStates).toString(QDateTime::currentDateTime(), QStringLiteral("dddd, MMMM d, yyyy h:mm:ss AP")));
  Lines << Separator();
  for (const QString & Line : Lines)
  {
    Add(llMessage, Line);
  }
}

void SessionLog::AddStartupInfo(const SessionData & Data)
{
  // TSessionLog::DoAddStartupInfo(Data)
  if (!FLogging)
  {
    return;
  }
  QStringList L;
  auto ADF = [&L](const QString & S) { L << S; };
  bool UsesSsh = Data.IsSsh();
  ADF(QStringLiteral("Session name: %1 (%2)").arg(Data.SessionName(),
    Data.Name.isEmpty() ? QStringLiteral("Ad-Hoc site") : QStringLiteral("Site")));
  QString AddressFamily;
  if (Data.AddressFamily != afAuto)
  {
    AddressFamily = QStringLiteral("%1, ").arg((Data.AddressFamily == afIPv4) ? QStringLiteral("IPv4") : QStringLiteral("IPv6"));
  }
  ADF(QStringLiteral("Hostname: %1 (%2Port: %3)").arg(Data.HostNameExpanded(), AddressFamily).arg(Data.PortNumber));
  ADF(QStringLiteral("Username: %1 (Password: %2, Key file: %3, Passphrase: %4)")
    .arg(Data.UserNameExpanded(), Sensitive(Data.Password), Sensitive(Data.PublicKeyFile), Sensitive(QString())));
  if (UsesSsh)
  {
    ADF(QStringLiteral("Tunnel: %1").arg(BooleanToEngStr(Data.Tunnel)));
    if (Data.Tunnel)
    {
      ADF(QStringLiteral("Tunnel: Hostname: %1 (Port: %2)").arg(Data.TunnelHostName).arg(Data.TunnelPortNumber));
      ADF(QStringLiteral("Tunnel: Username: %1 (Password: %2, Key file: %3)")
        .arg(Data.TunnelUserName, Sensitive(Data.TunnelPassword), Sensitive(Data.TunnelPublicKeyFile)));
      ADF(QStringLiteral("Tunnel: Local port number: %1").arg(Data.TunnelLocalPortNumber));
    }
  }
  ADF(QStringLiteral("Transfer Protocol: %1").arg(FSProtocolStr(Data.FSProtocol)));
  if (UsesSsh || (Data.FSProtocol == fsFTP))
  {
    QString PingType;
    int PingInterval;
    if (Data.FSProtocol == fsFTP)
    {
      PingType = EnumName(Data.FtpPingType, QStringLiteral("Off;Dummy;Dummy;List"));
      PingInterval = Data.FtpPingInterval;
    }
    else
    {
      PingType = EnumName(Data.PingType, QStringLiteral("Off;Null;Dummy"));
      PingInterval = Data.PingInterval;
    }
    ADF(QStringLiteral("Ping type: %1, Ping interval: %2 sec; Timeout: %3 sec").arg(PingType).arg(PingInterval).arg(Data.Timeout));
    ADF(QStringLiteral("Disable Nagle: %1").arg(BooleanToEngStr(Data.TcpNoDelay)));
  }
  ADF(QStringLiteral("Proxy: %1").arg((Data.FtpProxyLogonType != 0) ?
    QStringLiteral("FTP proxy %1").arg(Data.FtpProxyLogonType) :
    EnumName(Data.ProxyMethod, QStringLiteral("None;SOCKS4;SOCKS5;HTTP;Telnet;Cmd"))));
  if ((Data.FtpProxyLogonType != 0) || (Data.ProxyMethod != pmNone))
  {
    ADF(QStringLiteral("HostName: %1 (Port: %2); Username: %3; Passwd: %4")
      .arg(Data.ProxyHost).arg(Data.ProxyPort).arg(Data.ProxyUsername, Sensitive(Data.ProxyPassword)));
    if (Data.ProxyMethod == pmTelnet)
    {
      ADF(QStringLiteral("Telnet command: %1").arg(Data.ProxyTelnetCommand));
    }
    if (Data.ProxyMethod == pmCmd)
    {
      ADF(QStringLiteral("Local command: %1").arg(Data.ProxyLocalCommand));
    }
  }
  if (UsesSsh || (Data.FSProtocol == fsFTP) || (Data.FSProtocol == fsS3))
  {
    ADF(QStringLiteral("Send buffer: %1").arg(Data.SendBuf));
  }
  if (UsesSsh && !Data.SourceAddress.isEmpty())
  {
    ADF(QStringLiteral("Source address: %1").arg(Data.SourceAddress));
  }
  if (UsesSsh)
  {
    ADF(QStringLiteral("Compression: %1").arg(BooleanToEngStr(Data.Compression)));
    ADF(QStringLiteral("Bypass authentication: %1").arg(BooleanToEngStr(Data.SshNoUserAuth)));
    ADF(QStringLiteral("Try agent: %1; Agent forwarding: %2; KI: %3; GSSAPI: %4")
      .arg(BooleanToEngStr(Data.TryAgent), BooleanToEngStr(Data.AgentFwd), BooleanToEngStr(Data.AuthKI), BooleanToEngStr(Data.AuthGSSAPI)));
    if (Data.AuthGSSAPI)
    {
      ADF(QStringLiteral("GSSAPI: KEX: %1; Forwarding: %2")
        .arg(BooleanToEngStr(Data.AuthGSSAPIKEX), BooleanToEngStr(Data.GSSAPIFwdTGT)));
    }
    ADF(QStringLiteral("Ciphers: %1; Ssh2DES: %2").arg(Data.Ciphers().join(QLatin1Char(',')), BooleanToEngStr(Data.Ssh2DES)));
    ADF(QStringLiteral("KEX: %1").arg(Data.Kexes().join(QLatin1Char(','))));
    QStringList Bugs;
    for (int Index = 0; Index < sbCount; Index++)
    {
      Bugs.append(EnumName(Data.Bug[Index], AutoSwitchNames));
    }
    ADF(QStringLiteral("SSH Bugs: %1").arg(Bugs.join(QLatin1Char(','))));
    ADF(QStringLiteral("Simple channel: %1").arg(BooleanToEngStr(Data.SshSimple)));
    ADF(QStringLiteral("Return code variable: %1; Lookup user groups: %2")
      .arg(Data.ReturnVar.isEmpty() ? QStringLiteral("Autodetect") : Data.ReturnVar, EnumName(Data.LookupUserGroups, AutoSwitchNames)));
    ADF(QStringLiteral("Shell: %1").arg(Data.Shell.isEmpty() ? QStringLiteral("default") : Data.Shell));
    ADF(QStringLiteral("EOL: %1, UTF: %2").arg(EnumName(Data.EOLType, QStringLiteral("LF;CRLF;CR")), EnumName(Data.NotUtf, NotAutoSwitchNames)));
    ADF(QStringLiteral("Clear aliases: %1, Unset nat.vars: %2, Resolve symlinks: %3; Follow directory symlinks: %4")
      .arg(BooleanToEngStr(Data.ClearAliases), BooleanToEngStr(Data.UnsetNationalVars),
        BooleanToEngStr(Data.ResolveSymlinks), BooleanToEngStr(Data.FollowDirectorySymlinks)));
    ADF(QStringLiteral("LS: %1, Ign LS warn: %2, Scp1 Comp: %3; Exit code 1 is error: %4")
      .arg(Data.ListingCommand, BooleanToEngStr(Data.IgnoreLsWarnings), BooleanToEngStr(Data.Scp1Compatibility),
        BooleanToEngStr(Data.ExitCode1IsError)));
  }
  if ((Data.FSProtocol == fsSFTP) || (Data.FSProtocol == fsSFTPonly))
  {
    QStringList Bugs;
    for (int Index = 0; Index < sftpbCount; Index++)
    {
      Bugs.append(EnumName(Data.SFTPBug[Index], AutoSwitchNames));
    }
    ADF(QStringLiteral("SFTP Bugs: %1").arg(Bugs.join(QLatin1Char(','))));
    ADF(QStringLiteral("SFTP Server: %1").arg(Data.SftpServer.isEmpty() ? QStringLiteral("default") : Data.SftpServer));
    if (Data.SFTPRealPath != asAuto)
    {
      ADF(QStringLiteral("SFTP Real path: %1").arg(EnumName(Data.SFTPRealPath, AutoSwitchNames)));
    }
    if (Data.UsePosixRename)
    {
      ADF(QStringLiteral("Use POSIX rename: %1").arg(BooleanToEngStr(Data.UsePosixRename)));
    }
  }
  bool FtpsOn = false;
  if (Data.FSProtocol == fsFTP)
  {
    ADF(QStringLiteral("UTF: %1").arg(EnumName(Data.NotUtf, NotAutoSwitchNames)));
    QString Ftps;
    switch (Data.Ftps)
    {
      case ftpsImplicit: Ftps = QStringLiteral("Implicit TLS/SSL"); FtpsOn = true; break;
      case ftpsExplicitSsl: Ftps = QStringLiteral("Explicit SSL/TLS"); FtpsOn = true; break;
      case ftpsExplicitTls: Ftps = QStringLiteral("Explicit TLS/SSL"); FtpsOn = true; break;
      default: Ftps = QStringLiteral("None"); break;
    }
    if (Data.FtpTransferActiveImmediately != asAuto)
    {
      ADF(QStringLiteral("Transfer active immediately: %1").arg(EnumName(Data.FtpTransferActiveImmediately, AutoSwitchNames)));
    }
    ADF(QStringLiteral("FTPS: %1 [Client certificate: %2]").arg(Ftps, Sensitive(Data.TlsCertificateFile)));
    ADF(QStringLiteral("FTP: Passive: %1 [Force IP: %2]; MLSD: %3 [List all: %4]; HOST: %5")
      .arg(BooleanToEngStr(Data.FtpPasvMode), EnumName(Data.FtpForcePasvIp, AutoSwitchNames),
        EnumName(Data.FtpUseMlsd, AutoSwitchNames), EnumName(Data.FtpListAll, AutoSwitchNames),
        EnumName(Data.FtpHost, AutoSwitchNames)));
    if (Data.FtpWorkFromCwd != asAuto)
    {
      ADF(QStringLiteral("FTP: Relative paths: %1").arg(EnumName(Data.FtpWorkFromCwd, AutoSwitchNames)));
    }
  }
  if (Data.FSProtocol == fsWebDAV)
  {
    FtpsOn = (Data.Ftps != ftpsNone);
    ADF(QStringLiteral("HTTPS: %1 [Client certificate: %2]").arg(BooleanToEngStr(FtpsOn), Sensitive(Data.TlsCertificateFile)));
    if (Data.WebDavLiberalEscaping)
    {
      ADF(QStringLiteral("WebDAV: Tolerate non-encoded: %1").arg(BooleanToEngStr(Data.WebDavLiberalEscaping)));
    }
  }
  if (Data.FSProtocol == fsS3)
  {
    FtpsOn = (Data.Ftps != ftpsNone);
    ADF(QStringLiteral("HTTPS: %1").arg(BooleanToEngStr(FtpsOn)));
    ADF(QStringLiteral("S3: URL Style: %1").arg(EnumName(Data.S3UrlStyle, QStringLiteral("Virtual Host;Path"))));
    if (!Data.S3DefaultRegion.isEmpty())
    {
      ADF(QStringLiteral("S3: Default region: %1").arg(Data.S3DefaultRegion));
    }
    if (!Data.S3SessionToken.isEmpty())
    {
      ADF(QStringLiteral("S3: Session token: %1").arg(Sensitive(Data.S3SessionToken)));
    }
    if (!Data.S3RoleArn.isEmpty())
    {
      ADF(QStringLiteral("S3: Role ARN: %1 (session name: %2)")
        .arg(Data.S3RoleArn, Data.S3RoleSessionName.isEmpty() ? QStringLiteral("default") : Data.S3RoleSessionName));
    }
    if (Data.S3CredentialsEnv)
    {
      ADF(QStringLiteral("S3: Credentials from AWS environment: %1")
        .arg(Data.S3Profile.isEmpty() ? QStringLiteral("General") : Data.S3Profile));
    }
  }
  if (FtpsOn)
  {
    if (Data.FSProtocol == fsFTP)
    {
      ADF(QStringLiteral("Session reuse: %1").arg(BooleanToEngStr(Data.SslSessionReuse)));
    }
    ADF(QStringLiteral("TLS/SSL versions: %1-%2").arg(TlsVersionName(Data.MinTlsVersion), TlsVersionName(Data.MaxTlsVersion)));
  }
  ADF(QStringLiteral("Local directory: %1, Remote directory: %2, Update: %3, Cache: %4")
    .arg(Data.LocalDirectory.isEmpty() ? QStringLiteral("default") : Data.LocalDirectory,
      Data.RemoteDirectory.isEmpty() ? QStringLiteral("home") : Data.RemoteDirectory,
      BooleanToEngStr(Data.UpdateDirectories), BooleanToEngStr(Data.CacheDirectories)));
  ADF(QStringLiteral("Cache directory changes: %1, Permanent: %2")
    .arg(BooleanToEngStr(Data.CacheDirectoryChanges), BooleanToEngStr(Data.PreserveDirectoryChanges)));
  ADF(QStringLiteral("Recycle bin: Delete to: %1, Overwritten to: %2, Bin path: %3")
    .arg(BooleanToEngStr(Data.DeleteToRecycleBin), BooleanToEngStr(Data.OverwrittenToRecycleBin), Data.RecycleBinPath));
  if (Data.TrimVMSVersions || Data.VMSAllRevisions)
  {
    ADF(QStringLiteral("Trim VMS versions: %1; VMS all revisions: %2")
      .arg(BooleanToEngStr(Data.TrimVMSVersions), BooleanToEngStr(Data.VMSAllRevisions)));
  }
  QStringList TimeInfo;
  if ((Data.FSProtocol == fsSFTP) || (Data.FSProtocol == fsSFTPonly) || (Data.FSProtocol == fsSCPonly) || (Data.FSProtocol == fsWebDAV))
  {
    TimeInfo.append(QStringLiteral("DST mode: %1").arg(EnumName(Data.DSTMode, QStringLiteral("Win;Unix;Keep"))));
  }
  if ((Data.FSProtocol == fsSCPonly) || (Data.FSProtocol == fsFTP))
  {
    int TimeDifferenceMin = qRound(Data.TimeDifference * 24 * 60);
    TimeInfo.append(QStringLiteral("Timezone offset: %1h %2m").arg(TimeDifferenceMin / 60).arg(TimeDifferenceMin % 60));
  }
  if (!TimeInfo.isEmpty())
  {
    ADF(TimeInfo.join(QLatin1Char(';')));
  }
  if (Data.FSProtocol == fsWebDAV)
  {
    ADF(QStringLiteral("Compression: %1").arg(BooleanToEngStr(Data.Compression)));
  }
  L << Separator();
  for (const QString & Line : L)
  {
    Add(llMessage, Line);
  }
}

//---------------------------------------------------------------------------
// Thread binding

void SessionLog::SetCurrent(const std::shared_ptr<SessionLog> & Log)
{
  CurrentLog = Log;
  if (Log && Log->Logging())
  {
    ssh_set_log_callback(&SshLogCallback);
    ssh_set_log_level(SshLogLevel(Log->Level()));
    SshHooked = true;
  }
  else if (SshHooked)
  {
    ssh_set_log_level(SSH_LOG_NOLOG);
    SshHooked = false;
  }
}

SessionLog * SessionLog::Current()
{
  return CurrentLog.get();
}

void SessionLog::SetProcessLog(const std::shared_ptr<SessionLog> & Log)
{
  QMutexLocker Lock(&ProcessLogMutex);
  ProcessLogRef() = Log;
}

std::shared_ptr<SessionLog> SessionLog::ProcessLog()
{
  QMutexLocker Lock(&ProcessLogMutex);
  return ProcessLogRef();
}

std::shared_ptr<SessionLog> SessionLog::CreateForSession(const SessionData & Data, const Configuration & Config)
{
  std::shared_ptr<SessionLog> Result = ProcessLog();
  if (!Result && Config.Logging && !Config.LogFileName.isEmpty())
  {
    Result = std::make_shared<SessionLog>(SessionLogOptions::FromConfiguration(Config), &Data);
  }
  return Result;
}

void SessionLog::SetConfigurationLoaded()
{
  ConfigurationLoaded = true;
}

void SessionLog::AttachToWorker(QObject * Worker, const SessionData & Data)
{
  std::shared_ptr<SessionLog> Log = ProcessLog();
  std::shared_ptr<ActionsLog> Actions;
  if (ConfigurationLoaded)
  {
    Configuration & Config = Configuration::Instance();
    Log = CreateForSession(Data, Config);
    Actions = ActionsLog::CreateForSession(Data, Config);
  }
  if (!Log && !Actions)
  {
    return;
  }
  QMetaObject::invokeMethod(Worker, [Log, Actions]()
  {
    SessionLog::SetCurrent(Log);
    ActionsLog::SetCurrent(Actions);
  }, Qt::QueuedConnection);
}

void SessionLog::DetachThread()
{
  SetCurrent(nullptr);
  ActionsLog::SetCurrent(nullptr);
}

//---------------------------------------------------------------------------
// Hooks

bool SessionLog::IsLogging(int Level)
{
  SessionLog * Log = CurrentLog.get();
  return (Log != nullptr) && Log->Logging() && ((Level < 0) || (Log->Level() >= Level));
}

void SessionLog::LogEvent(const QString & Str)
{
  if (SessionLog * Log = CurrentLog.get())
  {
    Log->Add(llMessage, Str);
  }
}

void SessionLog::LogEvent(int Level, const QString & Str)
{
  if (IsLogging(Level))
  {
    CurrentLog->Add(llMessage, Str);
  }
}

void SessionLog::LogLine(LogLineType Type, const QString & Str, int Level)
{
  if (IsLogging(Level))
  {
    CurrentLog->Add(Type, Str);
  }
}

void SessionLog::LogProtocolLine(const QString & Line, int Level)
{
  if (!IsLogging(Level))
  {
    return;
  }
  if (Line.startsWith(QLatin1String("> ")))
  {
    CurrentLog->Add(llInput, Line.mid(2));
  }
  else if (Line.startsWith(QLatin1String("< ")))
  {
    CurrentLog->Add(llOutput, Line.mid(2));
  }
  else
  {
    CurrentLog->Add(llMessage, Line);
  }
}

void SessionLog::LogException(const QString & Message, const QString & MoreMessages)
{
  if (SessionLog * Log = CurrentLog.get())
  {
    Log->AddException(Message, MoreMessages);
  }
}

void SessionLog::LogStartup(const SessionData & Data)
{
  if (SessionLog * Log = CurrentLog.get())
  {
    Log->AddSystemInfo();
    Log->AddStartupInfo(Data);
  }
}

QString SessionLog::LogSensitive(const QString & Str)
{
  if (SessionLog * Log = CurrentLog.get())
  {
    return Log->Sensitive(Str);
  }
  return BooleanToEngStr(!Str.isEmpty());
}

QString SessionLog::MaskCommand(const QString & Command, const QString & Secret)
{
  SessionLog * Log = CurrentLog.get();
  if (Secret.isEmpty() || ((Log != nullptr) && Log->Options().Sensitive))
  {
    return Command;
  }
  QString Result = Command;
  Result.replace(Secret, QString(Secret.size(), QLatin1Char('*')));
  return Result;
}

void SessionLog::LogQuery(const QString & Query, const QString & MoreMessages)
{
  // TTerminal::QueryUser
  if (IsLogging(-1))
  {
    QString More = MoreMessages;
    More.replace(QLatin1Char('\n'), QLatin1Char(','));
    CurrentLog->Add(llMessage, QStringLiteral("Asking user:\n%1 (%2)").arg(Query, More));
  }
}

void SessionLog::LogAnswer(unsigned int Answer)
{
  if (!IsLogging(-1))
  {
    return;
  }
  static const struct { unsigned int Answer; const char * Name; } Names[] = {
    {qaYes, "Yes"}, {qaNo, "No"}, {qaOK, "OK"}, {qaCancel, "Cancel"}, {qaYesToAll, "Yes to All"},
    {qaNoToAll, "No to All"}, {qaAbort, "Abort"}, {qaRetry, "Retry"}, {qaSkip, "Skip"},
    {qaNeverAskAgain, "Never ask again"}, {qaIgnore, "Ignore"}, {qaAll, "All"}, {qaHelp, "Help"}};
  QString Name = QString::number(Answer);
  for (const auto & Item : Names)
  {
    if (Item.Answer == Answer)
    {
      Name = QString::fromLatin1(Item.Name);
      break;
    }
  }
  CurrentLog->Add(llMessage, QStringLiteral("Answer: %1").arg(Name));
}

void SessionLog::SetupSsh(ssh_session_struct * Session)
{
  SessionLog * Log = CurrentLog.get();
  if ((Session != nullptr) && (Log != nullptr) && Log->Logging())
  {
    int Verbosity = SshLogLevel(Log->Level());
    ssh_options_set(Session, SSH_OPTIONS_LOG_VERBOSITY, &Verbosity);
  }
}
