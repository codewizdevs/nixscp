// Port of WinSCP source/core/SessionInfo.{h,cpp} TSessionLog: the text
// session log ("Preferences > Logging", /log, /loglevel, /logsize).
//
// Line format as in WinSCP: "<mark> yyyy-mm-dd hh:nn:ss.zzz <text>", where
// the mark is '<' (output from the server), '>' (input sent to the server),
// '!' (raw error output), '.' (message) or '*' (exception).
//
// A SessionLog belongs to a session (Terminal). Its worker thread has it as
// the "current" log (SetCurrent), so protocol code anywhere below the
// Terminal logs with one-line static calls (LogEvent, LogLine...) without
// carrying a log pointer around; on threads without a log they do nothing.
// libssh's own logging (ssh_set_log_callback / ssh_set_log_level, which are
// thread local in libssh) is routed into the thread's log as well:
// Reduced = SSH_LOG_WARNING, Normal = SSH_LOG_PROTOCOL, Debug 1 =
// SSH_LOG_PACKET, Debug 2 = SSH_LOG_FUNCTIONS.
//
// Several logs writing to the same (expanded) file name share one writer
// (mutex protected), so sessions and background transfers do not overwrite
// or corrupt each other's lines; rotation (LogMaxSize / LogMaxCount) is done
// by that writer.
#pragma once

#include <QDateTime>
#include <QMutex>
#include <QString>
#include <QStringList>
#include <atomic>
#include <memory>

class Configuration;
struct ConfigurationData;
class SessionData;
class QObject;
struct ssh_session_struct;

// TLogLineType (core/SessionInfo.h); marks "<>!.*"
enum LogLineType { llOutput = 0, llInput = 1, llStdError = 2, llMessage = 3, llException = 4 };

// Logging part of TConfiguration (Configuration\Logging)
struct SessionLogOptions
{
  bool Logging = false;        // Logging
  QString FileName;            // LogFileName (with ! patterns); empty = no log file
  bool Append = true;          // LogFileAppend
  int Protocol = 0;            // LogProtocol: -1 Reduced, 0 Normal, 1 Debug 1, 2 Debug 2
  bool Sensitive = false;      // LogSensitive
  qint64 MaxSize = 0;          // LogMaxSize (0 = no rotation)
  int MaxCount = 0;            // LogMaxCount (0 = keep all)

  static SessionLogOptions FromConfiguration(const Configuration & Config);
};

class SessionLog
{
public:
  explicit SessionLog(const SessionLogOptions & Options, const SessionData * Data = nullptr,
    const QDateTime & Started = QDateTime::currentDateTime());
  ~SessionLog();

  SessionLog(const SessionLog &) = delete;
  SessionLog & operator=(const SessionLog &) = delete;

  const SessionLogOptions & Options() const { return FOptions; }
  // Logging is on and the file could be opened (or was not tried yet)
  bool Logging() const { return FLogging; }
  // LogProtocol (ActualLogProtocol)
  int Level() const { return FOptions.Protocol; }
  // Opens the log file now (otherwise it is opened by the first line).
  // False (with the error) when it cannot be created; logging is then off.
  bool Open(QString * Error = nullptr);
  QString CurrentFileName() const;
  QString LastError() const { return FLastError; }
  // "[Name] " prefix of every line (TSessionLog::SetParent name, e.g. "Background 1")
  void SetName(const QString & Name) { FName = Name; }

  // TSessionLog::Add: each line of Line (split at '\n') with the mark of Type
  void Add(LogLineType Type, const QString & Line);
  // TSessionLog::AddException ("(ExtException) message")
  void AddException(const QString & Message, const QString & MoreMessages = QString());
  void AddSeparator();
  // TSessionLog::AddSystemInfo: the environment block (versions, configuration,
  // log level, account, process, command line, time zone, login time) between
  // separators. Logged once per log object.
  void AddSystemInfo(const QString & CommandLine = QString());
  // TSessionLog::DoAddStartupInfo(Data): the session settings dump
  void AddStartupInfo(const SessionData & Data);
  // TSessionLog::LogSensitive: the value itself with LogSensitive, otherwise "Yes"/"No"
  QString Sensitive(const QString & Str) const;

  // GetExpandedLogFileName (core/SessionData.cpp): environment variables and
  // !Y !M !D !T (time of Started), !P (process ID), !@ (host), !S (session), !!
  static QString ExpandFileName(const QString & FileName, const QDateTime & Started, const SessionData * Data);
  // TSessionLog::LogPartFileName: "<base>.<index>" (index >= 1)
  static QString PartFileName(const QString & BaseName, int Index);
  static QString Separator();
  // Default LogFileName / ActionsLogFileName: $XDG_STATE_HOME/nixscp/!S.log / !S.xml
  // (WinSCP: %TEMP%\!S.log / %TEMP%\!S.xml)
  static QString DefaultLogFileName();
  static QString DefaultActionsLogFileName();
  // "Reduced" / "Normal" / "Debug 1" / "Debug 2"
  static QString LevelName(int Protocol);
  // Command line with passwords of session URLs and -password= switches masked
  static QString MaskCommandLine(const QStringList & Arguments);
  // libssh verbosity for a LogProtocol level
  static int SshLogLevel(int Protocol);

  // ---- The current thread's log ----
  // Makes Log the log of the calling thread and routes libssh's logging of
  // the thread into it (nullptr detaches; the log stays alive while current)
  static void SetCurrent(const std::shared_ptr<SessionLog> & Log);
  static SessionLog * Current();
  // Process wide log used by the sessions instead of the configured one
  // (nixscp-cli /log: the script and its sessions share one log)
  static void SetProcessLog(const std::shared_ptr<SessionLog> & Log);
  static std::shared_ptr<SessionLog> ProcessLog();
  // The log a new session uses: the process log, or one by Config (nullptr when logging is off)
  static std::shared_ptr<SessionLog> CreateForSession(const SessionData & Data, const Configuration & Config);
  // Terminal hook: creates the session's text and XML logs (Configuration::Instance()
  // or the process logs) and makes them current on Worker's thread (queued)
  // Nothing is taken from Configuration::Instance() before it was loaded
  // (core users without a configuration, e.g. tests, do not create it)
  static void AttachToWorker(QObject * Worker, const SessionData & Data);
  // Configuration::Load hook
  static void SetConfigurationLoaded();
  // Terminal hook (worker thread, when the session ends): releases the
  // thread's text and XML logs (the XML log is closed when no session uses it)
  static void DetachThread();

  // ---- Command-line logging (TConfiguration::TemporaryLogging & co.) ----
  // /log, /xmllog, /loglevel and /logsize change the configuration for this
  // run only. Begin remembers the permanent values (before the first switch
  // is applied), End the values set by the switches; Configuration::Save
  // writes the permanent ones while the configuration still has the
  // temporary ones (PermanentLoggingScope). Saving Preferences makes the
  // current values permanent (ClearTemporaryLogging).
  static void BeginTemporaryLogging(const ConfigurationData & Config);
  static void EndTemporaryLogging(const ConfigurationData & Config);
  static void ClearTemporaryLogging();
  class PermanentLoggingScope
  {
  public:
    explicit PermanentLoggingScope(ConfigurationData & Config);
    ~PermanentLoggingScope();
    PermanentLoggingScope(const PermanentLoggingScope &) = delete;
    PermanentLoggingScope & operator=(const PermanentLoggingScope &) = delete;
  private:
    ConfigurationData & FConfig;
    bool FSwapped = false;
  };

  // ---- One-line hooks; they do nothing when the thread has no log ----
  // The thread logs at least Level (0 Normal, 1 Debug 1, 2 Debug 2, -1 always)
  static bool IsLogging(int Level = 0);
  // TTerminal::LogEvent: '.' line
  static void LogEvent(const QString & Str);
  // TTerminal::LogEvent(Level, Str): only when LogProtocol >= Level
  static void LogEvent(int Level, const QString & Str);
  // Line of the given type (Level as above)
  static void LogLine(LogLineType Type, const QString & Str, int Level = 0);
  // A protocol trace line: "> command" is logged as input, "< reply" as
  // output (without the "> " / "< "), anything else as a message
  static void LogProtocolLine(const QString & Line, int Level = -1);
  // TTerminal::LogEvent of an exception ('*' line)
  static void LogException(const QString & Message, const QString & MoreMessages = QString());
  // TTerminal::Open: system info (once per log) and the session settings
  static void LogStartup(const SessionData & Data);
  // Value of a sensitive setting for the thread's log ("Yes"/"No" unless LogSensitive)
  static QString LogSensitive(const QString & Str);
  // Masks a password in a protocol command ("PASS secret" -> "PASS ******")
  // unless the thread's log has LogSensitive
  static QString MaskCommand(const QString & Command, const QString & Secret);
  // TTerminal::QueryUser logging: "Asking user:" before, "Answer: Retry" after
  static void LogQuery(const QString & Query, const QString & MoreMessages);
  static void LogAnswer(unsigned int Answer);
  // SecureShell hook: sets the session's libssh verbosity by the thread's log
  static void SetupSsh(ssh_session_struct * Session);

private:
  struct Writer;
  SessionLogOptions FOptions;
  std::unique_ptr<SessionData> FData;          // for the ! patterns (nullptr = no session)
  QDateTime FStarted;
  QString FName;
  std::atomic<bool> FLogging{false};
  bool FSystemInfoAdded = false;
  QString FLastError;
  mutable QMutex FOpenMutex;      // FWriter
  std::shared_ptr<Writer> FWriter;

  bool DoOpen(QString * Error);
  std::shared_ptr<Writer> OpenedWriter();
  void DoAdd(Writer & W, LogLineType Type, const QString & Line);
};
