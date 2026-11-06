// Port of WinSCP source/core/SessionInfo.{h,cpp} TActionLog,
// TSessionAction(Record) and its descendants: the XML log
// ("Preferences > Logging > XML log", LogActions / ActionsLogFileName, /xmllog).
//
// The schema is WinSCP's (http://winscp.net/schema/session/1.0), so tools
// parsing WinSCP's XML log (the .NET assembly, CI wrappers) read it as is:
//
//   <?xml version="1.0" encoding="UTF-8"?>
//   <session xmlns="http://winscp.net/schema/session/1.0" name="user@host" start="2026-11-01T12:00:00.123Z">
//     <upload>
//       <filename value="/home/user/a.txt" />
//       <destination value="/srv/a.txt" />
//       <size value="1234" />
//       <result success="true" />
//     </upload>
//     <rm>
//       <filename value="/srv/b.txt" />
//       <result success="false">
//         <message>Permission denied.</message>
//       </result>
//     </rm>
//   </session>
//
// Actions are recorded in the order they were started (a directory's <rm>
// before the <rm> of its contents), each once it is finished (TActionLog
// pending actions). Like SessionLog, the log is bound to the session's worker
// thread (SetCurrent), and protocol / Terminal code records an action with a
// SessionAction on the stack:
//
//   SessionAction Action(laMkdir, Path);
//   FFileSystem->CreateDirectory(Path, Mode);
//   Action.Commit();
//
// A SessionAction that is neither committed nor cancelled when it goes out of
// scope is rolled back (<result success="false">) when an error was noted in it
// (SessionAction::NoteError, called by the retry loops) or an exception is
// propagating; otherwise (skipped) it is not recorded at all.
#pragma once

#include "core/RemoteFile.h"

#include <QDateTime>
#include <QFile>
#include <QMutex>
#include <QString>
#include <QStringList>
#include <deque>
#include <memory>

class Configuration;
class ExtException;
class SessionData;

// TLogAction
enum LogAction { laUpload, laDownload, laTouch, laChmod, laMkdir, laRm, laMv, laCp, laCall, laLs, laStat,
  laChecksum, laCwd, laDifference };

class ActionsLog
{
public:
  // FileName with the ! patterns of the session log (expanded with Data, Started)
  ActionsLog(const QString & FileName, const SessionData * Data, const QDateTime & Started = QDateTime::currentDateTime());
  ~ActionsLog();

  ActionsLog(const ActionsLog &) = delete;
  ActionsLog & operator=(const ActionsLog &) = delete;

  // Creates the file and writes the XML declaration and <session>
  // (TActionLog::ReflectSettings); false with the error when it cannot be created.
  // DeferHeader: those are written with the first record, for the session
  // named by SetSession until then.
  bool Open(QString * Error = nullptr, bool DeferHeader = false);
  void SetSession(const SessionData & Data);
  // Ends a group, writes </session> and closes the file
  void Close();
  bool Logging() const;
  // Writing has failed, and logging was turned off (LOG_GEN_ERROR)
  bool Failed() const;
  QString CurrentFileName() const;
  QString LastError() const;

  // TActionLog::Add / AddIndented (one line each)
  void Add(const QString & Line);
  void AddIndented(const QString & Line);
  // TActionLog::AddFailure: <failure><message>...</message></failure>
  void AddFailure(const QStringList & Messages);
  void AddFailure(const ExtException & E);
  // /xmlgroups: <group name="command" start="..."> ... </group>
  void BeginGroup(const QString & Name);
  void EndGroup();
  bool InGroup() const;

  // DoXmlEscape: XmlEscape (element content) / XmlAttributeEscape (also new lines)
  static QString XmlEscape(const QString & Str);
  static QString XmlAttributeEscape(const QString & Str);
  // StandardTimestamp (core/Common.cpp): "2026-11-01T11:00:00.123Z"
  static QString StandardTimestamp(const QDateTime & DateTime = QDateTime::currentDateTime());
  // ExceptionToMoreMessages
  static QStringList ExceptionMessages(const QString & Message, const QString & MoreMessages);

  // ---- The current thread's log (see SessionLog) ----
  static void SetCurrent(const std::shared_ptr<ActionsLog> & Log);
  static ActionsLog * Current();
  // Whether sessions create their own XML log from the configuration
  // (LogActions). nixscp-cli turns it off: its script writes the XML log.
  static void SetSessionLoggingEnabled(bool Enabled);
  // One log of the process, used by all its sessions (/xmllog of nixscp-cli)
  static void SetProcessLog(const std::shared_ptr<ActionsLog> & Log);
  static std::shared_ptr<ActionsLog> ProcessLog();
  // The XML log a new session uses (nullptr when off)
  static std::shared_ptr<ActionsLog> CreateForSession(const SessionData & Data, const Configuration & Config);

private:
  friend class SessionAction;
  struct Record;

  mutable QMutex FMutex;
  QFile FFile;
  QString FFileNamePattern;
  std::unique_ptr<SessionData> FData;
  QDateTime FStarted;
  QString FCurrentFileName;
  QString FLastError;
  QString FIndent = QStringLiteral("  ");
  bool FLogging = false;
  bool FHeaderPending = false;
  bool FFailed = false;
  bool FInGroup = false;
  std::deque<std::shared_ptr<Record>> FPending;

  void AddHeader();
  void DoAdd(const QString & Line);
  void AddMessages(const QString & Indent, const QStringList & Messages);
  void AddPending(const std::shared_ptr<Record> & Action);
  void RecordPending();
  void WriteRecord(const Record & Action);
  void WriteFile(const QString & Indent, const RemoteFile & File, bool IncludeFileName);
};

// TSessionAction and descendants in one class: the parameters an action
// records are set with the methods below
class SessionAction
{
public:
  // Records into Log (default: the current thread's log; nothing when it has none)
  explicit SessionAction(LogAction Action, ActionsLog * Log = ActionsLog::Current());
  // TFileSessionAction / TFileLocationSessionAction: filename and destination
  SessionAction(LogAction Action, const QString & FileName, const QString & Destination = QString());
  ~SessionAction();

  SessionAction(const SessionAction &) = delete;
  SessionAction & operator=(const SessionAction &) = delete;

  bool Active() const { return (FRecord != nullptr); }

  void FileName(const QString & FileName);
  void Destination(const QString & Destination);
  void Size(qint64 Size);
  void Rights(unsigned int Mode);
  void Modification(const QDateTime & DateTime);
  void Recursive();
  void Command(const QString & Command);
  void AddOutput(const QString & Output, bool StdError);
  void ExitCode(int ExitCode);
  void Checksum(const QString & Alg, const QString & Checksum);
  void Cwd(const QString & Path);
  void FileList(const RemoteFileList & Files);
  void File(const RemoteFile & File);
  // TDifferenceSessionAction (synchronize checklist item): Action is
  // uploadnew/downloadnew/uploadupdate/downloadupdate/deleteremote/deletelocal
  void Difference(const QString & Action, bool IsDirectory, const QString & LocalPath, qint64 LocalSize,
    const QDateTime & LocalModification, const QString & RemotePath, qint64 RemoteSize, const QDateTime & RemoteModification);

  // Succeeded (recorded with <result success="true" />)
  void Commit();
  // Commit when Done, otherwise Cancel; returns Done
  bool Commit(bool Done);
  // Commit(Done) recording the transferred Size on success (TTerminal::LogFileDone)
  bool Commit(bool Done, qint64 Size);
  // Failed with the messages (<result success="false">)
  void Rollback(const QStringList & Messages = QStringList());
  void Rollback(const ExtException & E);
  // Not recorded (e.g. skipped by the user)
  void Cancel();
  // TSessionAction::Restart: forgets the parameters and the noted error (retry)
  void Restart();

  // Notes the error of the innermost open action of the current thread
  // (one-line hook for the retry loops: the action is rolled back with the
  // error unless it is committed after a retry)
  static void NoteError(const ExtException & E);
  static void NoteError(const QString & Message, const QString & MoreMessages = QString());

private:
  ActionsLog * FLog = nullptr;
  std::shared_ptr<ActionsLog::Record> FRecord;
  QStringList FNotedError;
  bool FErrorNoted = false;
  int FUncaught = 0;

  void Close(int State);
};
