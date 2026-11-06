// Port of WinSCP source/windows/ConsoleRunner.cpp (TConsoleRunner::Run,
// Console(), Usage(), ExpandCommand, LoadScriptFromFile) and the scripting
// part of source/windows/WinMain.cpp: the winscp.com equivalent.
//
// Also the command-line only modes of WinSCP: FingerprintScan, Info, KeyGen,
// CopyId and BatchSettings (windows/ConsoleRunner.cpp) and the master password prompt.
//
// nixscp-cli [session|url] [/script=file] [/command "cmd1" "cmd2"...]
//            [/parameter // param1...] [/stdout[=binary|chunked]] [/stdin]
//            [/log=file [/loglevel=n]] [/logsize=[n*]size] [/xmllog=file [/xmlgroups]]
//            [/nointeractiveinput] [/ini=file|nul] [/rawconfig ...]
//            [/rawtransfersettings setting1=value1 ...] [/batchsettings ...]
//            [/fingerprintscan url] [/keygen keyfile ...] [/copyid /identity=file session]
//            [/info] [/help]
#include "cli/ConsoleUserInterface.h"
#include "core/Platform.h"
#include "cli/Options.h"
#include "cli/Script.h"
#include "cli/SessionUrl.h"
#include "core/CloudCrypto.h"
#include "core/ConfigVault.h"
#include "core/Configuration.h"
#include "core/FileMasks.h"
#include "core/IniStorage.h"
#include "core/PuttyKey.h"
#include "core/SecureShell.h"
#include "core/Security.h"
#include "core/ActionsLog.h"
#include "core/SessionLog.h"
#include "core/Synchronize.h"
#include "core/Terminal.h"

#include <QCryptographicHash>
#include <QEventLoop>
#include <QSslCertificate>
#include <QSslCipher>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <libssh/libssh.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QTimeZone>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QRegularExpression>
#include <QStringDecoder>

#include <csignal>

#ifndef PROJECT_VERSION
#define PROJECT_VERSION "0.0.0"
#endif

static const int RESULT_SUCCESS = 0;
static const int RESULT_ANY_ERROR = 1;
static const QString TimestampVarName = QStringLiteral("TIMESTAMP");

// ExpandFileName (with ~ and environment variables, as WinSCP expands
// %VARIABLES% in /ini and /log)
static QString LocalExpandFileName(const QString & FileName)
{
  return SessionData::ExpandEnvironmentVariables(FileName);
}

static void PrintUsageSyntax(Console & C, const QString & Str)
{
  C.PrintLine(QStringLiteral("nixscp-cli ") + Str);
}

static void Usage(Console & C)
{
  // Usage() of winscp.com (cfCommandLineOnly)
  C.PrintLine(QStringLiteral("NixSCP, Version %1").arg(QStringLiteral(PROJECT_VERSION)));
  C.PrintLine(QStringLiteral("Copyright (c) 2026 Dragan Plavšić, CodeWiz - https://codewizdev.com/"));
  C.PrintLine(QStringLiteral("Based on WinSCP, Copyright (c) 2000-2026 Martin Prikryl"));
  C.PrintLine();
  C.PrintLine(QStringLiteral("Usage:"));
  PrintUsageSyntax(C, QStringLiteral("site|workspace|folder"));
  PrintUsageSyntax(C, QStringLiteral("(sftp|scp|ftp[es]|dav[s]|s3)://[user[:password]@]host[:port][/path/[file]]"));
  PrintUsageSyntax(C, QStringLiteral("[mysession] /sessionname=<name>"));
  PrintUsageSyntax(C, QStringLiteral("[mysession] [/privatekey=<file> [/passphrase=<passphrase>]]"));
  PrintUsageSyntax(C, QStringLiteral("[mysession] [/hostkey=<fingerprint>]"));
  PrintUsageSyntax(C, QStringLiteral("[mysession] [/username=<user> [/password=<password>]]"));
  PrintUsageSyntax(C, QStringLiteral("[mysession] [/passwordsfromfiles]"));
  PrintUsageSyntax(C, QStringLiteral("[mysession] [/clientcert=<file> [/passphrase=<passphrase>]]"));
  PrintUsageSyntax(C, QStringLiteral("[mysession] [/certificate=<fingerprint>]"));
  PrintUsageSyntax(C, QStringLiteral("[mysession] [/passive[=on|off]] [/implicit|explicit]"));
  PrintUsageSyntax(C, QStringLiteral("[mysession] [/timeout=<sec>]"));
  PrintUsageSyntax(C, QStringLiteral("[mysession] [/rawsettings setting1=value1 setting2=value2 ...]"));
  PrintUsageSyntax(C, QStringLiteral("[/script=<file>] [/command cmd1...] [/parameter // param1...]"));
  PrintUsageSyntax(C, QStringLiteral("[/stdout[=binary|chunked]] [/stdin]"));
  PrintUsageSyntax(C, QStringLiteral("[/log=<logfile> [/loglevel=<level>]] [/logsize=[<count>*]<size>]"));
  PrintUsageSyntax(C, QStringLiteral("[/xmllog=<logfile> [/xmlgroups]] [/nointeractiveinput]"));
  PrintUsageSyntax(C, QStringLiteral("[/ini=<inifile>]"));
  PrintUsageSyntax(C, QStringLiteral("[/rawconfig config1=value1 config2=value2 ...]"));
  PrintUsageSyntax(C, QStringLiteral("[/rawtransfersettings setting1=value1 setting2=value2 ...]"));
  PrintUsageSyntax(C, QStringLiteral("/batchsettings <site_mask> setting1=value1 setting2=value2 ..."));
  PrintUsageSyntax(C, QStringLiteral("/keygen keyfile [/output=<file>] [/changepassphrase]"));
  PrintUsageSyntax(C, QStringLiteral("/keygen keyfile [/comment=<text>]"));
  PrintUsageSyntax(C, QStringLiteral("/copyid /identity=<file> mysession"));
  PrintUsageSyntax(C, QStringLiteral("/fingerprintscan <session>"));
  PrintUsageSyntax(C, QStringLiteral("/info"));
  PrintUsageSyntax(C, QStringLiteral("/help"));
  C.PrintLine();

  const std::vector<std::pair<QString, QString>> Switches =
  {
    { QStringLiteral("session"), QStringLiteral("Name of site or direct session specification.") },
    { QStringLiteral("/sessionname="), QStringLiteral("Gives a name to the session") },
    { QStringLiteral("/privatekey="), QStringLiteral("SSH private key file.") },
    { QStringLiteral("/hostkey="), QStringLiteral("Fingerprint of server SSH host key.") },
    { QStringLiteral("/username="), QStringLiteral("An alternative way to provide a username") },
    { QStringLiteral("/password="), QStringLiteral("An alternative way to provide a password") },
    { QStringLiteral("/passwordsfromfiles"), QStringLiteral("Read all passwords from files") },
    { QStringLiteral("/clientcert="), QStringLiteral("TLS/SSL client certificate file.") },
    { QStringLiteral("/certificate="), QStringLiteral("Fingerprint of server TLS/SSL certificate (FTPS protocol only).") },
    { QStringLiteral("/passphrase="), QStringLiteral("A passphrase for an encrypted private key or a client certificate") },
    { QStringLiteral("/passive="), QStringLiteral("Passive mode (FTP protocol only).") },
    { QStringLiteral("/implicit"), QStringLiteral("Implicit TLS/SSL (FTP protocol only).") },
    { QStringLiteral("/explicit"), QStringLiteral("Explicit TLS/SSL (FTP protocol only).") },
    { QStringLiteral("/timeout="), QStringLiteral("Server response timeout.") },
    { QStringLiteral("/rawsettings"), QStringLiteral("Configures any session settings using a raw format as in an INI file.") },
    { QStringLiteral("/script="), QStringLiteral("Executes batch script file. If the script does not end with 'exit' command, normal interactive mode follows.") },
    { QStringLiteral("/command"), QStringLiteral("Executes list of script commands.") },
    { QStringLiteral("/parameter"), QStringLiteral("Passes list of parameters to script.") },
    { QStringLiteral("/stdout"), QStringLiteral("Allows streaming files to stdout (and redirects status output to stderr)") },
    { QStringLiteral("/stdin"), QStringLiteral("Allows streaming files from stdin") },
    { QStringLiteral("/log="), QStringLiteral("Turns on session logging to file.") },
    { QStringLiteral("/loglevel="), QStringLiteral("Logging level (0..2), add * to enable password logging.") },
    { QStringLiteral("/logsize="), QStringLiteral("Enables log rotation and optionally deleting of old logs.") },
    { QStringLiteral("/xmllog="), QStringLiteral("Turns on XML logging to file.") },
    { QStringLiteral("/xmlgroups"), QStringLiteral("Group all XML log elements belonging to the same command.") },
    { QStringLiteral("/nointeractiveinput"), QStringLiteral("All prompts are automatically cancelled") },
    { QStringLiteral("/ini="), QStringLiteral("Path to configuration INI-file.") },
    { QStringLiteral("/rawconfig"), QStringLiteral("Configures any settings using a raw format as in an INI file.") },
    { QStringLiteral("/rawtransfersettings"), QStringLiteral("Configures any transfer settings using a raw format as in an INI file.") },
    { QStringLiteral("/batchsettings"), QStringLiteral("Updates settings of sites matching a mask using a raw format as in an INI file.") },
    { QStringLiteral("/keygen"), QStringLiteral("Converts private key to .ppk format or edits key. Use /output= to specify output file. "
      "Use /changepassphrase to change or set passphrase. Use /comment= to change or set comment.") },
    { QStringLiteral("/copyid"), QStringLiteral("Installs your public key in a server's authorized_keys.") },
    { QStringLiteral("/fingerprintscan"), QStringLiteral("Prints fingerprints of server SSH host key or TLS/SSL certificate.") },
    { QStringLiteral("/info"), QStringLiteral("Prints list of supported ciphers and algorithms.") },
    { QStringLiteral("/help"), QStringLiteral("Prints this usage.") },
  };
  int MaxSwitchLen = 0;
  for (const auto & S : Switches)
  {
    MaxSwitchLen = std::max(MaxSwitchLen, static_cast<int>(S.first.size()));
  }
  const int ConsoleWidth = 80;
  for (const auto & S : Switches)
  {
    QString Label = QLatin1Char(' ') + S.first.leftJustified(MaxSwitchLen) + QLatin1Char(' ');
    int DescWidth = ConsoleWidth - static_cast<int>(Label.size()) - 1;
    // WrapText
    QStringList Lines;
    QString Line;
    for (const QString & Word : S.second.split(QLatin1Char(' ')))
    {
      if (!Line.isEmpty() && (Line.size() + 1 + Word.size() > DescWidth))
      {
        Lines.append(Line);
        Line.clear();
      }
      Line += (Line.isEmpty() ? QString() : QStringLiteral(" ")) + Word;
    }
    Lines.append(Line);
    for (int I = 0; I < Lines.size(); I++)
    {
      C.PrintLine(((I == 0) ? Label : QString(Label.size(), QLatin1Char(' '))) + Lines[I]);
    }
  }
}

// Delphi FormatDateTime subset used by %TIMESTAMP#format%
static QString ScriptFormatDateTime(const QString & Format, const QDateTime & T)
{
  QString Result;
  qsizetype I = 0;
  bool LastWasHour = false;
  while (I < Format.size())
  {
    QChar C = Format[I].toLower();
    qsizetype N = 1;
    while ((I + N < Format.size()) && (Format[I + N].toLower() == C))
    {
      N++;
    }
    QString Token;
    bool IsHour = false;
    if ((C == QLatin1Char('"')) || (C == QLatin1Char('\'')))
    {
      qsizetype End = Format.indexOf(Format[I], I + 1);
      if (End < 0)
      {
        End = Format.size();
      }
      Result += Format.mid(I + 1, End - I - 1);
      I = End + 1;
      continue;
    }
    if (C == QLatin1Char('y'))
    {
      Token = (N <= 2) ? QStringLiteral("%1").arg(T.date().year() % 100, 2, 10, QLatin1Char('0')) : QString::number(T.date().year());
    }
    else if ((C == QLatin1Char('m')) && (N >= 3) && !LastWasHour)
    {
      static const char * const ShortMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
      static const char * const LongMonths[] = {"January", "February", "March", "April", "May", "June", "July", "August",
        "September", "October", "November", "December"};
      int Month = T.date().month() - 1;
      Token = QString::fromLatin1((N == 3) ? ShortMonths[Month] : LongMonths[Month]);
    }
    else if (C == QLatin1Char('m'))
    {
      // "mm" right after an hour means minutes (Delphi)
      int Value = LastWasHour ? T.time().minute() : T.date().month();
      Token = (N >= 2) ? QStringLiteral("%1").arg(Value, 2, 10, QLatin1Char('0')) : QString::number(Value);
    }
    else if ((C == QLatin1Char('d')) && (N >= 3))
    {
      static const char * const ShortDays[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
      static const char * const LongDays[] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
      int Day = T.date().dayOfWeek() - 1;
      if (N == 3) Token = QString::fromLatin1(ShortDays[Day]);
      else if (N == 4) Token = QString::fromLatin1(LongDays[Day]);
      else if (N == 5) Token = QLocale().toString(T.date(), QLocale::ShortFormat);   // ddddd: short date
      else Token = QLocale().toString(T.date(), QLocale::LongFormat);                 // dddddd: long date
    }
    else if (C == QLatin1Char('d'))
    {
      Token = (N >= 2) ? QStringLiteral("%1").arg(T.date().day(), 2, 10, QLatin1Char('0')) : QString::number(T.date().day());
    }
    else if (C == QLatin1Char('c'))
    {
      // c: short date + long time
      Token = QLocale().toString(T.date(), QLocale::ShortFormat) + QLatin1Char(' ') + QLocale().toString(T.time(), QLocale::LongFormat);
    }
    else if (C == QLatin1Char('t'))
    {
      // t: short time, tt: long time
      Token = QLocale().toString(T.time(), (N >= 2) ? QLocale::LongFormat : QLocale::ShortFormat);
    }
    else if ((C == QLatin1Char('a')) && Format.mid(I, 5).compare(QStringLiteral("am/pm"), Qt::CaseInsensitive) == 0)
    {
      bool Pm = (T.time().hour() >= 12);
      QString Src = Format.mid(I, 5);
      Token = Pm ? Src.mid(3, 2) : Src.left(2);
      Result += Token;
      I += 5;
      continue;
    }
    else if ((C == QLatin1Char('a')) && Format.mid(I, 3).compare(QStringLiteral("a/p"), Qt::CaseInsensitive) == 0)
    {
      bool Pm = (T.time().hour() >= 12);
      QString Src = Format.mid(I, 3);
      Token = Pm ? Src.mid(2, 1) : Src.left(1);
      Result += Token;
      I += 3;
      continue;
    }
    else if (C == QLatin1Char('h'))
    {
      Token = (N >= 2) ? QStringLiteral("%1").arg(T.time().hour(), 2, 10, QLatin1Char('0')) : QString::number(T.time().hour());
      IsHour = true;
    }
    else if (C == QLatin1Char('n'))
    {
      Token = (N >= 2) ? QStringLiteral("%1").arg(T.time().minute(), 2, 10, QLatin1Char('0')) : QString::number(T.time().minute());
    }
    else if (C == QLatin1Char('s'))
    {
      Token = (N >= 2) ? QStringLiteral("%1").arg(T.time().second(), 2, 10, QLatin1Char('0')) : QString::number(T.time().second());
    }
    else if (C == QLatin1Char('z'))
    {
      Token = (N >= 3) ? QStringLiteral("%1").arg(T.time().msec(), 3, 10, QLatin1Char('0')) : QString::number(T.time().msec());
    }
    else
    {
      Token = Format.mid(I, N);
    }
    if (C != QLatin1Char(':') && C != QLatin1Char(' '))
    {
      LastWasHour = IsHour;
    }
    Result += Token;
    I += N;
  }
  return Result;
}

// RelativeStrToDateTime: "<number><unit>" with units Y, M (months),
// D (default), H, N (minutes), S
static bool RelativeStrToDateTime(QString S, QDateTime & T, bool Add)
{
  S = S.trimmed();
  if (S.isEmpty())
  {
    return false;
  }
  QChar Unit = QLatin1Char('D');
  if (S.back().isLetter())
  {
    Unit = S.back().toUpper();
    S.chop(1);
  }
  bool Ok = false;
  int Value = S.toInt(&Ok);
  if (!Ok)
  {
    return false;
  }
  if (!Add)
  {
    Value = -Value;
  }
  switch (Unit.toLatin1())
  {
    case 'Y': T = T.addYears(Value); break;
    case 'M': T = T.addMonths(Value); break;
    case 'D': T = T.addDays(Value); break;
    case 'H': T = T.addSecs(static_cast<qint64>(Value) * 3600); break;
    case 'N': T = T.addSecs(static_cast<qint64>(Value) * 60); break;
    case 'S': T = T.addSecs(Value); break;
    default: return false;
  }
  return true;
}

// TConsoleRunner::ExpandCommand
static QString ExpandCommand(QString Command, const QStringList & ScriptParameters)
{
  for (int Index = 0; Index < ScriptParameters.size(); Index++)
  {
    Command.replace(QStringLiteral("%%1%").arg(Index + 1), ScriptParameters[Index]);
  }
  QDateTime N = QDateTime::currentDateTime();
  if (!qEnvironmentVariableIsSet("TIMESTAMP"))
  {
    Command.replace(QStringLiteral("%TIMESTAMP%"), ScriptFormatDateTime(QStringLiteral("yyyymmddhhnnss"), N), Qt::CaseInsensitive);
  }
  qsizetype Offset = 0;
  for (;;)
  {
    qsizetype P = Command.indexOf(QLatin1Char('%') + TimestampVarName, Offset, Qt::CaseInsensitive);
    if (P < 0)
    {
      break;
    }
    Offset = P + 1 + TimestampVarName.size();
    qsizetype P2 = Command.indexOf(QLatin1Char('%'), Offset);
    qsizetype P3 = Command.indexOf(QLatin1Char('#'), Offset);
    if ((P2 >= 0) && (P3 >= 0) && (P3 < P2) &&
        ((P3 == Offset) || (Command[Offset] == QLatin1Char('+')) || (Command[Offset] == QLatin1Char('-'))))
    {
      bool Valid = true;
      QDateTime T = N;
      if (P3 > Offset)
      {
        bool Add = (Command[Offset] == QLatin1Char('+'));
        Valid = RelativeStrToDateTime(Command.mid(Offset + 1, P3 - Offset - 1), T, Add);
      }
      if (Valid)
      {
        QString Value = ScriptFormatDateTime(Command.mid(P3 + 1, P2 - P3 - 1), T);
        Command = Command.left(P) + Value + Command.mid(P2 + 1);
        Offset = P + Value.size();
      }
    }
  }
  // ExpandEnvironmentVariables: %NAME% of defined variables
  static const QRegularExpression EnvVar(QStringLiteral("%([A-Za-z_][A-Za-z0-9_]*)%"));
  QString Result;
  qsizetype Last = 0;
  QRegularExpressionMatchIterator It = EnvVar.globalMatch(Command);
  while (It.hasNext())
  {
    QRegularExpressionMatch M = It.next();
    if (M.capturedStart() < Last)
    {
      continue;
    }
    QByteArray Name = M.captured(1).toLocal8Bit();
    if (qEnvironmentVariableIsSet(Name.constData()))
    {
      Result += Command.mid(Last, M.capturedStart() - Last) + qEnvironmentVariable(Name.constData());
      Last = M.capturedEnd();
    }
  }
  Result += Command.mid(Last);
  return Result;
}

// LoadScriptFromFile (UTF-8 with or without BOM, otherwise the locale encoding)
static bool LoadScriptFromFile(const QString & FileName, QStringList & Lines, QString & Error)
{
  QFile File(FileName);
  if (!File.open(QIODevice::ReadOnly))
  {
    Error = QStringLiteral("File '%1' not found.").arg(FileName);
    return false;
  }
  QByteArray Data = File.readAll();
  QString Text;
  if (Data.startsWith("\xEF\xBB\xBF"))
  {
    Text = QString::fromUtf8(Data.mid(3));
  }
  else
  {
    QStringDecoder Decoder(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    Text = Decoder.decode(Data);
    if (Decoder.hasError())
    {
      Text = QString::fromLocal8Bit(Data);
    }
  }
  Text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
  Lines = Text.split(QLatin1Char('\n'));
  if (!Lines.isEmpty() && Lines.last().isEmpty())
  {
    Lines.removeLast();
  }
  return true;
}

// Command line for the log with passwords masked
// (TManagementScript::MaskPasswordInCommandLine with Recurse)
static QString MaskedCommandLine(const QStringList & Args)
{
  QStringList Result{QStringLiteral("nixscp-cli")};
  bool SubCommands = false;
  for (const QString & Arg : Args)
  {
    Options Probe;
    Probe.Add(Arg);
    QString Switch;
    QString Value;
    QChar Mark;
    QString Out = Arg;
    if (Probe.WasSwitchAdded(Switch, Value, Mark))
    {
      SubCommands = (Switch.compare(QStringLiteral("command"), Qt::CaseInsensitive) == 0);
      if ((Switch.compare(QStringLiteral("password"), Qt::CaseInsensitive) == 0) ||
          (Switch.compare(QStringLiteral("passphrase"), Qt::CaseInsensitive) == 0) ||
          (Switch.compare(QStringLiteral("newpassword"), Qt::CaseInsensitive) == 0))
      {
        Out = QStringLiteral("%1%2=***").arg(Mark).arg(Switch);
      }
    }
    else if (SubCommands)
    {
      QString Rest = Arg;
      QString Command;
      if (CutToken(Rest, Command) && (Command.compare(QStringLiteral("open"), Qt::CaseInsensitive) == 0))
      {
        Out = Script::MaskPasswordInCommandLine(Arg);
      }
    }
    else if (Arg.contains(QStringLiteral("://")))
    {
      Out = Script::MaskPasswordInCommandLine(QStringLiteral("open ") + QuoteParam(Arg)).mid(5);
    }
    Result.append(QuoteParam(Out));
  }
  return Result.join(QLatin1Char(' '));
}

// TConsoleRunner::Run
static int Run(Script & AScript, const QString & Session, Options & Params, const QStringList & ScriptCommands,
  const QStringList & ScriptParameters, bool UsageWarnings)
{
  Console & C = Console::Instance();
  bool AnyError = false;
  auto Failed = [&]()
  {
    AScript.Log(QLatin1Char('.'), QStringLiteral("Script: Failed"));
    AnyError = true;
  };
  // everything until the first manually entered command is "batch"
  // (including opening session from command line and script file)
  AScript.SetBatchScript(true);
  if (!Session.isEmpty())
  {
    if (UsageWarnings)
    {
      AScript.PrintMessage(QStringLiteral("Opening session using command-line parameter in scripting is deprecated. Use 'open' command instead."));
    }
    AScript.ResetCommandError();
    try
    {
      AScript.Connect(Session, Params, false);
    }
    catch (const AbortException &)
    {
    }
    catch (const ExtException & E)
    {
      AScript.ShowException(E);
    }
    if (AScript.CommandError())
    {
      Failed();
    }
  }

  int ScriptPos = 0;
  bool Result = true;
  do
  {
    QString Command;
    if (ScriptPos < ScriptCommands.size())
    {
      Result = true;
      Command = ScriptCommands[ScriptPos];
      ScriptPos++;
    }
    else
    {
      if (AScript.BatchScript())
      {
        // no longer batch
        AScript.SetBatchScript(false);
        AScript.StartInteractive();
      }
      Result = C.ReadCommandLine(QStringLiteral("winscp> "), Command);
      if (!Result && !(C.InputIsTty() && C.OutputIsTty()))
      {
        // end of input: break the prompt line
        C.Print(QStringLiteral("\n"));
      }
    }
    if (Result)
    {
      AScript.ResetCommandError();
      AScript.Command(ExpandCommand(Command, ScriptParameters));
      if (AScript.CommandError())
      {
        Failed();
        if (AScript.Batch() == Script::BatchAbort)
        {
          Result = false;
        }
      }
    }
  }
  while (Result && AScript.Continue() && !AScript.Aborted());

  C.FinishProgress();
  int ExitCode = (AnyError || AScript.Aborted()) ? RESULT_ANY_ERROR : RESULT_SUCCESS;
  QString ExitCodeMessage = QStringLiteral("Exit code: %1").arg(ExitCode);
  AScript.Log(QLatin1Char('.'), QStringLiteral("Script: ") + ExitCodeMessage);
  if (AScript.LogLevel() >= 1)
  {
    C.PrintLine(ExitCodeMessage);
  }
  return ExitCode;
}

// HandleException (windows/ConsoleRunner.cpp) for the command-line only modes
static int HandleException(Console & C, const ExtException & E)
{
  QString Message = E.Message();
  if (!E.MoreMessages().isEmpty())
  {
    Message += QLatin1Char('\n') + E.MoreMessages();
  }
  C.PrintLine(Message);
  return RESULT_ANY_ERROR;
}

namespace
{
// Captures the host key of an SSH server without accepting it
class FingerprintScanUI : public UserInterface
{
public:
  QString KeyType;
  QString SHA256;
  QString MD5;

  HostKeyAnswer VerifyHostKey(const QString &, int, const QString & AKeyType, const QString & FingerprintSHA256,
    const QString & FingerprintMD5, HostKeyState) override
  {
    KeyType = AKeyType;
    SHA256 = FingerprintSHA256;
    MD5 = FingerprintMD5;
    return HostKeyCancel;
  }
  bool PromptUser(PromptKind, const QString &, const QString &, QList<Prompt> &) override { return false; }
  void DisplayBanner(const QString &, const QString &) override {}
  unsigned int QueryUser(const QString &, const QString &, unsigned int Answers) override { return CancelAnswer(Answers); }
};

// "<type> <bits>" prefix of WinSCP's host key fingerprints (TSecureShell)
QString SignKeyType(const QString & KeyType)
{
  QString Bits;
  if (KeyType == QStringLiteral("ssh-ed25519")) Bits = QStringLiteral("255");
  else if (KeyType == QStringLiteral("ssh-ed448")) Bits = QStringLiteral("448");
  else if (KeyType.endsWith(QStringLiteral("nistp256"))) Bits = QStringLiteral("256");
  else if (KeyType.endsWith(QStringLiteral("nistp384"))) Bits = QStringLiteral("384");
  else if (KeyType.endsWith(QStringLiteral("nistp521"))) Bits = QStringLiteral("521");
  return Bits.isEmpty() ? KeyType : (KeyType + QLatin1Char(' ') + Bits);
}

QString HexFingerprint(const QByteArray & Digest)
{
  return QString::fromLatin1(Digest.toHex(':')).toLower();
}
}

// FingerprintScan (windows/ConsoleRunner.cpp) + TTerminal::FingerprintScan
static int FingerprintScan(Console & C, Options & Params, StoredSessionList * StoredSessions)
{
  int Result = RESULT_SUCCESS;
  try
  {
    SessionData Data;
    SessionExtras Extras;
    bool Valid = false;
    if (Params.ParamCount() > 0)
    {
      int ParsedInfo = 0;
      ParseSessionUrl(Params.Param(1), &Params, StoredSessions, Data, Extras, ParsedInfo);
      bool UsesSsh = (Data.FSProtocol != fsFTP) && (Data.FSProtocol != fsWebDAV) && (Data.FSProtocol != fsS3);
      Valid = !(ParsedInfo & piDefaultsOnly) && !Data.HostName.isEmpty() && (UsesSsh || (Data.Ftps != ftpsNone));
    }
    if (!Valid)
    {
      // FINGERPRINTSCAN_NEED_SECURE_SESSION
      throw ExtException(QStringLiteral("Secure session (SSH or TLS/SSL) not specified."));
    }
    bool UsesSsh = (Data.FSProtocol != fsFTP) && (Data.FSProtocol != fsWebDAV) && (Data.FSProtocol != fsS3);
    if (UsesSsh)
    {
      // Connect with an empty known_hosts and no configured fingerprint,
      // so that the host key always reaches the UI, which refuses it
      QTemporaryDir Temp;
      Data.HostKey.clear();
      Data.KnownHostsFile = Temp.isValid() ? Temp.filePath(QStringLiteral("known_hosts")) : QStringLiteral("/dev/null");
      FingerprintScanUI UI;
      {
        Terminal Term(Data, &UI);
        QEventLoop Loop;
        OperationResult OpenResult;
        Term.Open([&](const OperationResult & R) { OpenResult = R; Loop.quit(); });
        Loop.exec();
        Term.Close();
        if (UI.SHA256.isEmpty())
        {
          throw ExtException(OpenResult.Message, OpenResult.MoreMessages);
        }
      }
      QString Prefix = SignKeyType(UI.KeyType) + QLatin1Char(' ');
      QString SHA256 = UI.SHA256;
      if (SHA256.startsWith(QStringLiteral("SHA256:")))
      {
        SHA256 = SHA256.mid(7);
      }
      QString MD5 = UI.MD5;
      if (MD5.startsWith(QStringLiteral("MD5:")))
      {
        MD5 = MD5.mid(4);
      }
      C.PrintLine(QStringLiteral("SHA-256: %1").arg(Prefix + SHA256));
      C.PrintLine(QStringLiteral("MD5:     %1").arg(Prefix + MD5));
    }
    else
    {
      // TLS: implicit (ftps://, davs://) or explicit FTP (AUTH TLS)
      QSslSocket Socket;
      QSslConfiguration Config = Socket.sslConfiguration();
      Config.setPeerVerifyMode(QSslSocket::VerifyNone);
      Socket.setSslConfiguration(Config);
      Socket.setPeerVerifyName(Data.HostName);
      int TimeoutMs = std::max(Data.Timeout, 1) * 1000;
      auto Fail = [&](const QString & Message)
      {
        throw ExtException(Message.isEmpty() ? QStringLiteral("Connection failed.") : Message);
      };
      auto ReadReply = [&]()
      {
        // FTP reply (multi-line "nnn-" ... "nnn ")
        QString Line;
        do
        {
          while (!Socket.canReadLine())
          {
            if (!Socket.waitForReadyRead(TimeoutMs))
            {
              Fail(Socket.errorString());
            }
          }
          Line = QString::fromUtf8(Socket.readLine()).trimmed();
        }
        while ((Line.size() < 4) || (Line[3] != QLatin1Char(' ')));
        return Line;
      };
      bool Explicit = (Data.FSProtocol == fsFTP) && (Data.Ftps != ftpsImplicit);
      if (Explicit)
      {
        Socket.connectToHost(Data.HostName, static_cast<quint16>(Data.PortNumber));
        if (!Socket.waitForConnected(TimeoutMs))
        {
          Fail(Socket.errorString());
        }
        ReadReply();
        Socket.write((Data.Ftps == ftpsExplicitSsl) ? "AUTH SSL\r\n" : "AUTH TLS\r\n");
        QString Reply = ReadReply();
        if (!Reply.startsWith(QLatin1Char('2')))
        {
          Fail(Reply);
        }
        Socket.startClientEncryption();
      }
      else
      {
        Socket.connectToHostEncrypted(Data.HostName, static_cast<quint16>(Data.PortNumber));
      }
      if (!Socket.waitForEncrypted(TimeoutMs))
      {
        Fail(Socket.errorString());
      }
      QSslCertificate Certificate = Socket.peerCertificate();
      Socket.abort();
      C.PrintLine(QStringLiteral("SHA-256: %1").arg(HexFingerprint(Certificate.digest(QCryptographicHash::Sha256))));
      C.PrintLine(QStringLiteral("SHA-1: %1").arg(HexFingerprint(Certificate.digest(QCryptographicHash::Sha1))));
    }
  }
  catch (const ExtException & E)
  {
    Result = HandleException(C, E);
  }
  return Result;
}

// Info (windows/ConsoleRunner.cpp): supported algorithms of this build
static int Info(Console & C)
{
  auto PrintList = [&](const QString & Caption, const QStringList & List)
  {
    C.PrintLine(Caption);
    for (const QString & Item : List)
    {
      C.PrintLine(QStringLiteral("  ") + Item);
    }
  };
  // Algorithms libssh accepts (ssh_options_set fails for unsupported ones)
  auto Supported = [](enum ssh_options_e Option, const QStringList & Candidates)
  {
    QStringList Result;
    ssh_session Session = ssh_new();
    for (const QString & Name : Candidates)
    {
      if (ssh_options_set(Session, Option, Name.toLatin1().constData()) == SSH_OK)
      {
        Result.append(Name);
      }
    }
    ssh_free(Session);
    return Result;
  };
  C.PrintLine(QStringLiteral("SSH implementation: libssh %1").arg(QString::fromLatin1(ssh_version(0))));
  C.PrintLine();
  PrintList(QStringLiteral("SSH encryption ciphers:"), Supported(SSH_OPTIONS_CIPHERS_C_S, {
    QStringLiteral("chacha20-poly1305@openssh.com"), QStringLiteral("aes256-gcm@openssh.com"), QStringLiteral("aes128-gcm@openssh.com"),
    QStringLiteral("aes256-ctr"), QStringLiteral("aes192-ctr"), QStringLiteral("aes128-ctr"),
    QStringLiteral("aes256-cbc"), QStringLiteral("aes192-cbc"), QStringLiteral("aes128-cbc"), QStringLiteral("3des-cbc")}));
  C.PrintLine();
  PrintList(QStringLiteral("SSH key exchange algorithms:"), Supported(SSH_OPTIONS_KEY_EXCHANGE, {
    QStringLiteral("mlkem768x25519-sha256"), QStringLiteral("sntrup761x25519-sha512"), QStringLiteral("sntrup761x25519-sha512@openssh.com"),
    QStringLiteral("curve25519-sha256"), QStringLiteral("curve25519-sha256@libssh.org"),
    QStringLiteral("ecdh-sha2-nistp256"), QStringLiteral("ecdh-sha2-nistp384"), QStringLiteral("ecdh-sha2-nistp521"),
    QStringLiteral("diffie-hellman-group18-sha512"), QStringLiteral("diffie-hellman-group16-sha512"),
    QStringLiteral("diffie-hellman-group-exchange-sha256"), QStringLiteral("diffie-hellman-group14-sha256"),
    QStringLiteral("diffie-hellman-group14-sha1"), QStringLiteral("diffie-hellman-group-exchange-sha1"),
    QStringLiteral("diffie-hellman-group1-sha1")}));
  C.PrintLine();
  PrintList(QStringLiteral("SSH host key algorithms:"), Supported(SSH_OPTIONS_HOSTKEYS, {
    QStringLiteral("ssh-ed25519"), QStringLiteral("ecdsa-sha2-nistp521"), QStringLiteral("ecdsa-sha2-nistp384"),
    QStringLiteral("ecdsa-sha2-nistp256"), QStringLiteral("rsa-sha2-512"), QStringLiteral("rsa-sha2-256"), QStringLiteral("ssh-rsa")}));
  C.PrintLine();
  PrintList(QStringLiteral("SSH MAC algorithms:"), Supported(SSH_OPTIONS_HMAC_C_S, {
    QStringLiteral("hmac-sha2-256-etm@openssh.com"), QStringLiteral("hmac-sha2-512-etm@openssh.com"), QStringLiteral("hmac-sha1-etm@openssh.com"),
    QStringLiteral("hmac-sha2-256"), QStringLiteral("hmac-sha2-512"), QStringLiteral("hmac-sha1")}));
  C.PrintLine();
  C.PrintLine(QStringLiteral("TLS/SSL implementation: %1").arg(QSslSocket::sslLibraryVersionString()));
  C.PrintLine();
  QStringList Ciphers;
  for (const QSslCipher & Cipher : QSslConfiguration::supportedCiphers())
  {
    Ciphers.append(Cipher.name());
  }
  PrintList(QStringLiteral("TLS/SSL cipher suites:"), Ciphers);
  return RESULT_SUCCESS;
}

// FindNixCompatibleSwitch (windows/ConsoleRunner.cpp): /name=value or the puttygen form "-n value"
static bool FindNixCompatibleSwitch(Options & Params, const QString & Name, const QString & NixName, QString & Value, bool & Set)
{
  bool Result = !Name.isEmpty() && Params.FindSwitch(Name, Value, Set);
  if (!Result)
  {
    QStringList Args;
    Result = Params.FindSwitchCaseSensitive(NixName, Args, 1);
    if (Result && !Args.isEmpty())
    {
      Value = Args.first();
      Set = true;
    }
  }
  return Result;
}

// KeyType (core/PuttyIntf.cpp): what a key file holds, by its first bytes
enum KeyFileType { kftUnopenable, kftUnknown, kftPutty, kftOpenSsh, kftSsh1, kftPublic, kftSshCom };
static KeyFileType GetKeyFileType(const QString & FileName)
{
  QFile File(FileName);
  if (!File.open(QIODevice::ReadOnly))
  {
    return kftUnopenable;
  }
  QByteArray Head = File.read(64);
  if (Head.startsWith("PuTTY-User-Key-File-"))
  {
    return kftPutty;
  }
  if (Head.startsWith("SSH PRIVATE KEY FILE FORMAT 1.1"))
  {
    return kftSsh1;
  }
  if (Head.startsWith("---- BEGIN SSH2 ENCRYPTED PRIVATE KEY"))
  {
    return kftSshCom;
  }
  if (Head.startsWith("-----BEGIN OPENSSH PRIVATE KEY") || (Head.startsWith("-----BEGIN ") && Head.contains("PRIVATE KEY")))
  {
    return kftOpenSsh;
  }
  if (Head.startsWith("---- BEGIN SSH2 PUBLIC KEY") || Head.startsWith("ssh-") || Head.startsWith("ecdsa-") || Head.startsWith("sk-"))
  {
    return kftPublic;
  }
  return kftUnknown;
}

// KeyGen (windows/ConsoleRunner.cpp). The output is a .ppk file; adding a
// certificate is not done: "<key>-cert.pub" next to the key is picked up anyway.
static int KeyGen(Console & C, Options & Params)
{
  int Result = RESULT_SUCCESS;
  ssh_key Key = nullptr;
  QString Passphrase;
  QString NewPassphrase;
  try
  {
    QStringList Args;
    if (!Params.FindSwitch(QStringLiteral("keygen"), Args, 1) || Args.isEmpty() || Args.first().isEmpty())
    {
      // KEYGEN_NO_INPUT
      throw ExtException(QStringLiteral("You have to specify input key file."));
    }
    QString InputFileName = LocalExpandFileName(Args.first());
    bool ValueSet = false;
    QString OutputFileName;
    FindNixCompatibleSwitch(Params, QStringLiteral("output"), QStringLiteral("o"), OutputFileName, ValueSet);
    QString NewComment;
    FindNixCompatibleSwitch(Params, QStringLiteral("comment"), QStringLiteral("C"), NewComment, ValueSet);
    bool ChangePassphrase = false;
    bool NewPassphraseSet = false;
    if (Params.FindSwitchCaseSensitive(QStringLiteral("P")))
    {
      ChangePassphrase = true;
      FindNixCompatibleSwitch(Params, QString(), QStringLiteral("-new-passphrase"), NewPassphrase, NewPassphraseSet);
    }
    else
    {
      ChangePassphrase = Params.FindSwitch(QStringLiteral("changepassphrase"), NewPassphrase, NewPassphraseSet);
    }
    QString Certificate;
    FindNixCompatibleSwitch(Params, QStringLiteral("certificate"), QStringLiteral("-certificate"), Certificate, ValueSet);
    FindNixCompatibleSwitch(Params, QStringLiteral("passphrase"), QStringLiteral("-old-passphrase"), Passphrase, ValueSet);
    if (Params.ParamCount() > 0)
    {
      // TOO_MANY_PARAMS_ERROR
      throw ExtException(QStringLiteral("Too many parameters."));
    }
    if (!Certificate.isEmpty())
    {
      throw ExtException(QStringLiteral("Adding a certificate to a key file is not supported. "
        "Keep it next to the key as \"<key>-cert.pub\", or use the Certificate setting of the site."));
    }

    KeyFileType Type = GetKeyFileType(InputFileName);
    switch (Type)
    {
      case kftSsh1:
        // KEYGEN_SSH1
        throw ExtException(QStringLiteral("Editing SSH-1 keys is not supported."));
      case kftPutty:
        if (NewComment.isEmpty() && !ChangePassphrase && OutputFileName.isEmpty())
        {
          // KEYGEN_NO_ACTION
          throw ExtException(QStringLiteral("No action specified."));
        }
        break;
      case kftOpenSsh:
        if (OutputFileName.isEmpty())
        {
          // KEYGEN_NO_OUTPUT
          throw ExtException(QStringLiteral("You have to specify output key file."));
        }
        break;
      case kftSshCom:
        throw ExtException(QStringLiteral("Keys in ssh.com format are not supported."));
      case kftPublic:
        // KEYGEN_PUBLIC
        throw ExtException(QStringLiteral("Converting public keys is not supported."));
      case kftUnopenable:
        // KEY_TYPE_UNOPENABLE
        throw ExtException(QStringLiteral("Private key file '%1' does not exist or cannot be opened.").arg(InputFileName));
      case kftUnknown:
      default:
        // KEY_TYPE_UNKNOWN2
        throw ExtException(QStringLiteral("File '%1' does not contain private key in known format.").arg(InputFileName));
    }

    QString Comment;
    bool PassphrasePrompted = false;
    auto PromptPassphrase = [&](QString & Value) -> bool
    {
      if (!Passphrase.isEmpty() && !PassphrasePrompted)
      {
        PassphrasePrompted = true;
        Value = Passphrase;
        return true;
      }
      PassphrasePrompted = true;
      // PROMPT_KEY_PASSPHRASE
      C.Print(QStringLiteral("Passphrase for key '%1': ").arg(QFileInfo(InputFileName).fileName()));
      Passphrase.clear();
      if (!C.Input(Passphrase, false, 0) || Passphrase.isEmpty())
      {
        return false;
      }
      Value = Passphrase;
      return true;
    };
    if (Type == kftPutty)
    {
      QString Error;
      Comment = PuttyKey::ReadComment(InputFileName);
      PuttyKey::LoadResult Loaded = PuttyKey::Load(InputFileName,
        [&](QString & Value, bool) { return PromptPassphrase(Value); }, &Key, &Error);
      if (Loaded == PuttyKey::lrCancelled)
      {
        throw AbortException();
      }
      if (Loaded != PuttyKey::lrOk)
      {
        throw ExtException(QStringLiteral("Unable to load private key from '%1'.").arg(InputFileName), Error);
      }
    }
    else
    {
      QByteArray Path = QFile::encodeName(InputFileName);
      int Rc = ssh_pki_import_privkey_file(Path.constData(), Passphrase.toUtf8().constData(), nullptr, nullptr, &Key);
      for (int Attempt = 0; (Rc != SSH_OK) && (Attempt < 3); Attempt++)
      {
        QString Value;
        if (!PromptPassphrase(Value))
        {
          throw AbortException();
        }
        Rc = ssh_pki_import_privkey_file(Path.constData(), Value.toUtf8().constData(), nullptr, nullptr, &Key);
      }
      if (Rc != SSH_OK)
      {
        throw ExtException(QStringLiteral("Unable to load private key from '%1'.").arg(InputFileName));
      }
      // The comment of an OpenSSH key is kept in the .pub file
      try
      {
        SecureShell::PublicKeyLine(InputFileName, &Comment);
      }
      catch (const ExtException &)
      {
      }
      if (Comment.isEmpty())
      {
        Comment = QStringLiteral("imported-openssh-key");
      }
    }

    if (!NewComment.isEmpty())
    {
      Comment = NewComment;
    }
    if (ChangePassphrase)
    {
      if (!NewPassphraseSet)
      {
        // KEYGEN_PASSPHRASE
        C.Print(QStringLiteral("Enter passphrase to save key: "));
        if (!C.Input(NewPassphrase, false, 0))
        {
          throw AbortException();
        }
        // KEYGEN_PASSPHRASE2
        C.Print(QStringLiteral("Re-enter passphrase to verify: "));
        QString NewPassphrase2;
        if (!C.Input(NewPassphrase2, false, 0))
        {
          throw AbortException();
        }
        bool Same = (NewPassphrase == NewPassphrase2);
        NewPassphrase2.fill(QChar(0));
        if (!Same)
        {
          // KEYGEN_PASSPHRASES_MISMATCH
          throw ExtException(QStringLiteral("Passphrases do not match."));
        }
      }
    }
    else
    {
      NewPassphrase = Passphrase;
    }
    if (OutputFileName.isEmpty())
    {
      OutputFileName = InputFileName;
    }
    OutputFileName = LocalExpandFileName(OutputFileName);
    QString Error;
    if (!PuttyKey::Save(OutputFileName, Key, Comment, NewPassphrase, &Error))
    {
      throw ExtException(Error);
    }
    // KEYGEN_SAVED
    C.PrintLine(QStringLiteral("Key saved to \"%1\".").arg(OutputFileName));
  }
  catch (const AbortException &)
  {
    Result = RESULT_ANY_ERROR;
  }
  catch (const ExtException & E)
  {
    Result = HandleException(C, E);
  }
  ssh_key_free(Key);
  Passphrase.fill(QChar(0));
  NewPassphrase.fill(QChar(0));
  return Result;
}

// BatchSettings (windows/ConsoleRunner.cpp)
static int BatchSettings(Console & C, Options & Params, Configuration * Config)
{
  int Result = RESULT_SUCCESS;
  try
  {
    QStringList Arguments;
    Params.FindSwitch(QStringLiteral("batchsettings"), Arguments);
    if (Arguments.size() < 1)
    {
      throw ExtException(QStringLiteral("No site mask specified."));
    }
    if (Arguments.size() < 2)
    {
      throw ExtException(QStringLiteral("No site settings specified."));
    }
    if (Config == nullptr)
    {
      throw ExtException(QStringLiteral("No configuration storage."));
    }
    FileMasks Mask;
    Mask.SetMask(Arguments.takeFirst());
    int Matches = 0;
    int Changes = 0;
    auto Snapshot = [](const SessionData & Data)
    {
      IniFile File;
      IniStorage Storage(&File);
      Storage.OpenSubKey(QStringLiteral("Site"), true);
      Data.Save(Storage);
      QStringList Result;
      for (const QString & Section : File.Sections())
      {
        for (const QString & Key : File.Keys(Section))
        {
          Result.append(Section + QLatin1Char('\\') + Key + QLatin1Char('=') + File.ReadRaw(Section, Key));
        }
      }
      return Result;
    };
    for (const auto & Data : Config->StoredSessions().Sessions())
    {
      if (!Data->IsWorkspace && Mask.MatchesFileName(Data->Name, false, nullptr))
      {
        Matches++;
        QStringList Before = Snapshot(*Data);
        SessionExtras Extras;
        ApplySessionRawSettings(*Data, Extras, Arguments);
        bool Changed = (Snapshot(*Data) != Before);
        if (Changed)
        {
          Changes++;
        }
        // TitleSeparator
        C.PrintLine(Data->Name + QStringLiteral(" – ") + (Changed ? QStringLiteral("Changed") : QStringLiteral("Not changed")));
      }
    }
    if ((Changes > 0) && !Config->Save())
    {
      throw ExtException(Config->LastSaveError());
    }
    // BATCH_SET_SUMMARY
    C.PrintLine(QStringLiteral("Found %1 sites, Changed %2 sites").arg(Matches).arg(Changes));
  }
  catch (const ExtException & E)
  {
    Result = HandleException(C, E);
  }
  return Result;
}

// TOptionsStorage over /rawconfig "Path\Key=Value" lines, relative to the
// "Configuration" key: applied to a copy of the configuration file
static void ApplyRawConfig(Configuration & Config, const QStringList & RawConfig, QTemporaryDir & Temp)
{
  if (RawConfig.isEmpty() || !Temp.isValid())
  {
    return;
  }
  Platform::MakeDirectoryPrivate(Temp.path());
  QString FileName = Temp.filePath(QStringLiteral("nixscp.ini"));
  if (!Config.Export(FileName))
  {
    return;
  }
  IniFile File;
  File.Load(FileName);
  for (const QString & Setting : RawConfig)
  {
    qsizetype P = Setting.indexOf(QLatin1Char('='));
    QString Path = (P >= 0) ? Setting.left(P) : Setting;
    QString Value = (P >= 0) ? Setting.mid(P + 1) : QString();
    qsizetype Slash = Path.lastIndexOf(QLatin1Char('\\'));
    QString KeyPath = QStringLiteral("Configuration") + ((Slash >= 0) ? (QLatin1Char('\\') + Path.left(Slash)) : QString());
    QString Name = (Slash >= 0) ? Path.mid(Slash + 1) : Path;
    IniStorage Storage(&File);
    if (Storage.OpenSubKeyPath(KeyPath, true))
    {
      Storage.WriteString(Name, Value);
      Storage.CloseSubKeyPath();
    }
  }
  File.Save(FileName);
  Config.SetIniFileName(FileName);
  Config.Load();
}

int main(int argc, char * argv[])
{
  Platform::HardenProcess();
#ifdef Q_OS_MACOS
  // a closed connection is an error result, not a signal (no MSG_NOSIGNAL there)
  signal(SIGPIPE, SIG_IGN);
#endif
  QCoreApplication App(argc, argv);
  QCoreApplication::setApplicationName(QStringLiteral("nixscp"));
  QCoreApplication::setApplicationVersion(QStringLiteral(PROJECT_VERSION));

  Console & C = Console::Instance();
  Options Params;
  // Unicode on Windows too (argv there is in the ANSI code page)
  const QStringList Args = QCoreApplication::arguments().mid(1);
  for (const QString & Arg : Args)
  {
    Params.Add(Arg);
  }

  // GNU conventions besides WinSCP's /help, /h, /?
  if (Args.contains(QStringLiteral("--version")))
  {
    C.PrintLine(QStringLiteral("nixscp-cli %1").arg(QStringLiteral(PROJECT_VERSION)));
    return RESULT_SUCCESS;
  }
  if (Args.contains(QStringLiteral("--help")) ||
      Params.FindSwitch(QStringLiteral("help")) || Params.FindSwitch(QStringLiteral("h")) || Params.FindSwitch(QStringLiteral("?")))
  {
    Usage(C);
    return RESULT_SUCCESS;
  }

  C.InstallSignalHandler();

  // /stdout, /stdin and /nointeractiveinput (TExternalConsole)
  Script::StdInOutMode StdOut = Script::StdInOutOff;
  bool StdIn = false;
  {
    QString Value;
    if (Params.FindSwitch(QStringLiteral("stdout"), Value))
    {
      if (Value.isEmpty() || (Value.compare(QStringLiteral("binary"), Qt::CaseInsensitive) == 0))
      {
        StdOut = Script::StdInOutBinary;
      }
      else if (Value.compare(QStringLiteral("chunked"), Qt::CaseInsensitive) == 0)
      {
        StdOut = Script::StdInOutChunked;
      }
      else
      {
        C.PrintLine(QStringLiteral("Unknown value '%1' of option '%2'.").arg(Value, QStringLiteral("stdout")));
        return RESULT_ANY_ERROR;
      }
    }
    if (Params.FindSwitch(QStringLiteral("stdin"), Value))
    {
      if (!Value.isEmpty() && (Value.compare(QStringLiteral("binary"), Qt::CaseInsensitive) != 0))
      {
        C.PrintLine(QStringLiteral("Unknown value '%1' of option '%2'.").arg(Value, QStringLiteral("stdin")));
        return RESULT_ANY_ERROR;
      }
      StdIn = true;
    }
  }
  C.SetUseStdErr(StdOut != Script::StdInOutOff);
  C.SetNoInteractiveInput(Params.FindSwitch(QStringLiteral("nointeractiveinput")) || StdIn);

  // Master password of the stored passwords (CONSOLE_MASTER_PASSWORD_PROMPT)
  Security::SetMasterPasswordPrompt([&C]()
  {
    C.Print(QStringLiteral("Master password: "));
    QString Password;
    if (!C.Input(Password, false, 0))
    {
      return false;
    }
    if (!Security::ValidateMasterPassword(Password))
    {
      // MASTER_PASSWORD_INCORRECT
      C.PrintLine(QStringLiteral("You did not enter the correct current master password."));
      return false;
    }
    Security::SetMasterPassword(Password);
    return true;
  });

  // Configuration: /ini=nul (none), /ini=<file>, or the automatic nixscp.ini
  // (transfer defaults, endurance and stored sites come from it)
  Configuration & Config = Configuration::Instance();
  QString IniFileName;
  if (Params.FindSwitch(QStringLiteral("ini"), IniFileName) && !IniFileName.isEmpty())
  {
    Config.SetIniFileName((IniFileName.compare(QStringLiteral("nul"), Qt::CaseInsensitive) == 0) ?
      QStringLiteral("nul") : LocalExpandFileName(IniFileName));
  }
  QString ConfigProblem = Config.IsNulStorage() ? QString() : ConfigVault::ConfigProblemText(Config.IniFileName());
  if (!ConfigProblem.isEmpty())
  {
    C.PrintLine(ConfigProblem);
    return 1;
  }
  if (!Config.IsNulStorage() && ConfigVault::IsSealedFile(Config.IniFileName()))
  {
    // NixSCP Pro: the configuration is encrypted with the NixSCP password
    bool Unlocked = false;
    for (int Attempt = 0; !Unlocked && (Attempt < 3); Attempt++)
    {
      C.Print(QStringLiteral("NixSCP password (or recovery key): "));
      QString Secret;
      if (!C.Input(Secret, false, 0) || Secret.isEmpty())
      {
        break;
      }
      QString Problem;
      Unlocked = ConfigVault::Unlock(Config.IniFileName(), Secret, false, &Problem) ||
        (!CloudCrypto::ParseRecoveryKey(Secret).isEmpty() && ConfigVault::Unlock(Config.IniFileName(), Secret, true, &Problem));
      if (!Unlocked)
      {
        C.PrintLine(Problem);
      }
    }
    if (!Unlocked)
    {
      C.PrintLine(QStringLiteral("The configuration \"%1\" is encrypted (NixSCP Pro) and needs your NixSCP password. "
        "Run interactively, or use /ini=nul for scripts that do not need stored sites.").arg(Config.IniFileName()));
      return 1;
    }
  }
  Config.Load();
  QStringList RawConfig;
  QTemporaryDir RawConfigDir;
  if (Params.FindSwitch(QStringLiteral("rawconfig"), RawConfig))
  {
    ApplyRawConfig(Config, RawConfig, RawConfigDir);
  }
  StoredSessionList * StoredSessions = &Config.StoredSessions();
  CopyParam DefaultCopyParam = Config.DefaultCopyParam();

  if (Params.FindSwitch(QStringLiteral("batchsettings")))
  {
    return BatchSettings(C, Params, &Config);
  }
  if (Params.FindSwitch(QStringLiteral("keygen")))
  {
    return KeyGen(C, Params);
  }
  if (Params.FindSwitch(QStringLiteral("fingerprintscan")))
  {
    return FingerprintScan(C, Params, StoredSessions);
  }
  if (Params.FindSwitch(QStringLiteral("info")))
  {
    return Info(C);
  }

  QStringList RawTransferSettings;
  if (Params.FindSwitch(QStringLiteral("rawtransfersettings"), RawTransferSettings))
  {
    IniFile File;
    IniStorage Storage(&File);
    Storage.OpenSubKey(QStringLiteral("CopyParam"), true);
    for (const QString & Setting : RawTransferSettings)
    {
      qsizetype P = Setting.indexOf(QLatin1Char('='));
      Storage.WriteString((P >= 0) ? Setting.left(P) : Setting, (P >= 0) ? Setting.mid(P + 1) : QString());
    }
    DefaultCopyParam.Load(Storage);
  }

  int Result = RESULT_SUCCESS;
  Script AScript(StoredSessions);
  AScript.SetConfigurationReopenTimeout(Config.SessionReopenTimeout);
  AScript.SetStdOut(StdOut);
  AScript.SetStdIn(StdIn);
  try
  {
    QStringList ScriptCommands;
    QStringList ScriptParameters;
    QString Value;
    if (Params.FindSwitch(QStringLiteral("copyid")))
    {
      // TConsoleRunner: the "copyid" command on the session of the command line
      bool ValueSet = false;
      QString IdentityFileName;
      FindNixCompatibleSwitch(Params, QStringLiteral("identity"), QStringLiteral("i"), IdentityFileName, ValueSet);
      if (IdentityFileName.isEmpty())
      {
        // COPYID_IDETITY_MISSING
        throw ExtException(QStringLiteral("Identity/key file was not specified."));
      }
      ScriptCommands.append(QStringLiteral("copyid %1").arg(QuoteParam(IdentityFileName)));
      ScriptCommands.append(QStringLiteral("exit"));
    }
    else if (Params.FindSwitch(QStringLiteral("script"), Value) && !Value.isEmpty())
    {
      QString Error;
      if (!LoadScriptFromFile(Value, ScriptCommands, Error))
      {
        throw ExtException(Error);
      }
    }
    Params.FindSwitch(QStringLiteral("command"), ScriptCommands);
    Params.FindSwitch(QStringLiteral("parameter"), ScriptParameters);

    // /loglevel=<-1..2>[*]
    QString LogLevelStr;
    if (Params.FindSwitch(QStringLiteral("loglevel"), LogLevelStr))
    {
      // windows/WinMain.cpp: "*" logs passwords, "*-" does not; an invalid level is ignored
      bool LogPasswords = false;
      qsizetype StarPos = LogLevelStr.indexOf(QLatin1Char('*'));
      if (StarPos >= 0)
      {
        LogPasswords = true;
        LogLevelStr.remove(StarPos, 1);
        if ((StarPos < LogLevelStr.size()) && (LogLevelStr[StarPos] == QLatin1Char('-')))
        {
          LogPasswords = false;
          LogLevelStr.remove(StarPos, 1);
        }
      }
      bool Ok = false;
      int Level = LogLevelStr.trimmed().toInt(&Ok);
      AScript.SetLogLevel((Ok && (Level >= -1)) ? Level : AScript.LogLevel(), LogPasswords);
    }
    // /logsize=[<count>*]<size>[K|M|G]
    qint64 LogMaxSize = 0;
    int LogMaxCount = 0;
    QString LogSizeStr;
    if (Params.FindSwitch(QStringLiteral("logsize"), LogSizeStr) && !LogSizeStr.isEmpty())
    {
      QString SizeStr = LogSizeStr;
      qsizetype Star = SizeStr.indexOf(QLatin1Char('*'));
      bool Ok = true;
      if (Star >= 0)
      {
        LogMaxCount = SizeStr.left(Star).toInt(&Ok);
        SizeStr = SizeStr.mid(Star + 1);
      }
      qint64 Multiplier = 1;
      if (!SizeStr.isEmpty())
      {
        QChar Unit = SizeStr.back().toUpper();
        if (Unit == QLatin1Char('K')) Multiplier = 1024;
        else if (Unit == QLatin1Char('M')) Multiplier = 1024 * 1024;
        else if (Unit == QLatin1Char('G')) Multiplier = 1024LL * 1024 * 1024;
        if (Multiplier > 1)
        {
          SizeStr.chop(1);
        }
      }
      bool SizeOk = false;
      LogMaxSize = SizeStr.toLongLong(&SizeOk) * Multiplier;
      if (!Ok || !SizeOk || (LogMaxSize <= 0))
      {
        throw ExtException(QStringLiteral("Unknown value '%1' of option '%2'.").arg(LogSizeStr, QStringLiteral("logsize")));
      }
    }

    QString LogFileName;
    if (Params.FindSwitch(QStringLiteral("log"), LogFileName) && !LogFileName.isEmpty())
    {
      QString Error;
      if (!AScript.OpenLog(LocalExpandFileName(LogFileName), Error, LogMaxSize, LogMaxCount))
      {
        // LOG_GEN_ERROR is a warning only
        C.PrintLine(QStringLiteral("Error occurred during logging. It's been turned off.\nCan't open log file '%1'.\n%2").arg(LogFileName, Error));
      }
      else if (std::shared_ptr<SessionLog> Log = SessionLog::ProcessLog())
      {
        // TSessionLog::AddSystemInfo (the sessions add their settings when opened)
        Log->AddSystemInfo(MaskedCommandLine(Args));
      }
    }
    // The script writes the XML log (/xmllog); the sessions do not write
    // another one from the configuration (LogActions)
    ActionsLog::SetSessionLoggingEnabled(false);

    // /xmllog=<file> [/xmlgroups] [/xmllogrequired]
    QString XmlLogFileName;
    if (Params.FindSwitch(QStringLiteral("xmllog"), XmlLogFileName) && !XmlLogFileName.isEmpty())
    {
      bool Required = Params.FindSwitch(QStringLiteral("xmllogrequired"));
      QString Error;
      if (!AScript.OpenXmlLog(LocalExpandFileName(XmlLogFileName), Params.FindSwitch(QStringLiteral("xmlgroups")), Required, Error))
      {
        if (Required)
        {
          // LOG_FATAL_ERROR
          throw ExtException(QStringLiteral("Error occurred during logging. Cannot continue."),
            QStringLiteral("Can't open log file '%1'.\n%2").arg(XmlLogFileName, Error));
        }
        C.PrintLine(QStringLiteral("Error occurred during logging. It's been turned off.\nCan't open log file '%1'.\n%2").arg(XmlLogFileName, Error));
      }
    }
    else
    {
      Params.FindSwitch(QStringLiteral("xmlgroups"));
      Params.FindSwitch(QStringLiteral("xmllogrequired"));
    }

    QString Session;
    if (Params.ParamCount() >= 1)
    {
      Session = Params.Param(1);
      if (Params.ParamCount() > 1)
      {
        // Check if the pending parameters will be consumed by ParseUrl (/rawsettings)
        Options OptionsCopy(Params);
        SessionData Data;
        SessionExtras Extras;
        int ParsedInfo = 0;
        try
        {
          ParseSessionUrl(Session, &OptionsCopy, StoredSessions, Data, Extras, ParsedInfo);
        }
        catch (const ExtException &)
        {
        }
        if (OptionsCopy.ParamCount() > 1)
        {
          AScript.PrintMessage(QStringLiteral("Scripting does not use standalone parameters. The parameters you have specified on command-line will not be used. Your command-line syntax is probably wrong."));
        }
      }
    }

    AScript.SetCopyParam(DefaultCopyParam);
    {
      // Interface\\SynchronizeParams, the setting of the GUI too (/rawconfig sets it for a script)
      IniStorage Storage(&Config.Ini());
      if (Storage.OpenSubKeyPath(QStringLiteral("Configuration\\Interface"), false) && Storage.ValueExists(QStringLiteral("SynchronizeParams")))
      {
        AScript.SetSynchronizeParams(Storage.ReadInteger(QStringLiteral("SynchronizeParams"), spDefault));
      }
    }
    AScript.SetWantsProgress(C.OutputIsTty());
    bool UsageWarnings = !ScriptCommands.isEmpty() || !C.InputIsTty();
    AScript.SetUsageWarnings(UsageWarnings);
    Result = Run(AScript, Session, Params, ScriptCommands, ScriptParameters, UsageWarnings);
  }
  catch (const ExtException & E)
  {
    AScript.ShowException(E);
    Result = RESULT_ANY_ERROR;
  }
  AScript.CloseXmlLog();
  C.RestoreTitle();
  return Result;
}
