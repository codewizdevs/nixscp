// Port of WinSCP source/core/SessionInfo.cpp: TActionLog, TSessionActionRecord, TSessionAction
#include "core/ActionsLog.h"
#include "core/Platform.h"
#include "core/Configuration.h"
#include "core/FileSystem.h"
#include "core/SessionData.h"
#include "core/SessionLog.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QPair>
#include <QtGlobal>
#include <atomic>
#include <exception>
#include <vector>

struct ActionsLog::Record
{
  enum State { Opened, Committed, RolledBack, Cancelled };
  LogAction Action = laUpload;
  State St = Opened;
  bool Recursive = false;
  bool HasErrors = false;
  QStringList Errors;
  QList<QPair<QString, QString>> Parameters;
  bool HasFileList = false;
  RemoteFileList Files;
  bool HasFile = false;
  RemoteFile File;

  void Parameter(const QString & Name, const QString & Value = QString())
  {
    Parameters.append(qMakePair(Name, Value));
  }

  // TSessionActionRecord::ActionName
  const char * ActionName() const
  {
    switch (Action)
    {
      case laUpload: return "upload";
      case laDownload: return "download";
      case laTouch: return "touch";
      case laChmod: return "chmod";
      case laMkdir: return "mkdir";
      case laRm: return "rm";
      case laMv: return "mv";
      case laCp: return "cp";
      case laCall: return "call";
      case laLs: return "ls";
      case laStat: return "stat";
      case laChecksum: return "checksum";
      case laCwd: return "cwd";
      case laDifference: return "difference";
    }
    return "";
  }
};

namespace
{
thread_local std::shared_ptr<ActionsLog> CurrentActionsLog;
thread_local std::vector<SessionAction *> OpenActions;
std::atomic<bool> SessionLoggingEnabled{true};
std::shared_ptr<ActionsLog> ProcessActionsLog;

QMutex SharedLogsMutex;
QHash<QString, std::weak_ptr<ActionsLog>> & SharedLogs()
{
  static QHash<QString, std::weak_ptr<ActionsLog>> Result;
  return Result;
}

const QString XmlDeclaration = QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>");

QString DoXmlEscape(const QString & Str, bool NewLine)
{
  // DoXmlEscape (core/SessionInfo.cpp)
  QString Result;
  Result.reserve(Str.size());
  for (QChar C : Str)
  {
    ushort U = C.unicode();
    if (U == '\r')
    {
      continue;
    }
    if ((U < 0x20) && (U != '\n') && (U != '\t'))
    {
      Result += QStringLiteral("&#x%1;").arg(QString::number(U, 16).toUpper().rightJustified(2, QLatin1Char('0')));
      continue;
    }
    switch (U)
    {
      case 0xFFFE:
      case 0xFFFF:
        Result += QStringLiteral("&#x%1;").arg(QString::number(U, 16).toUpper());
        break;
      case '&': Result += QStringLiteral("&amp;"); break;
      case '>': Result += QStringLiteral("&gt;"); break;
      case '<': Result += QStringLiteral("&lt;"); break;
      case '"': Result += QStringLiteral("&quot;"); break;
      case '\n':
        Result += NewLine ? QStringLiteral("&#10;") : QStringLiteral("\n");
        break;
      default:
        Result += C;
        break;
    }
  }
  return Result;
}
}

//---------------------------------------------------------------------------

ActionsLog::ActionsLog(const QString & FileName, const SessionData * Data, const QDateTime & Started) :
  FFileNamePattern(FileName),
  FStarted(Started)
{
  if (Data != nullptr)
  {
    FData = std::make_unique<SessionData>(*Data);
  }
}

ActionsLog::~ActionsLog()
{
  Close();
}

QString ActionsLog::XmlEscape(const QString & Str)
{
  return DoXmlEscape(Str, false);
}

QString ActionsLog::XmlAttributeEscape(const QString & Str)
{
  return DoXmlEscape(Str, true);
}

QString ActionsLog::StandardTimestamp(const QDateTime & DateTime)
{
  return DateTime.toUTC().toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss.zzz'Z'"));
}

QStringList ActionsLog::ExceptionMessages(const QString & Message, const QString & MoreMessages)
{
  // ExceptionToMoreMessages
  QStringList Result;
  if (!Message.isEmpty())
  {
    Result.append(Message);
  }
  if (!MoreMessages.isEmpty())
  {
    Result.append(MoreMessages.split(QLatin1Char('\n'), Qt::SkipEmptyParts));
  }
  return Result;
}

bool ActionsLog::Open(QString * Error, bool DeferHeader)
{
  QMutexLocker Lock(&FMutex);
  if (FLogging)
  {
    return true;
  }
  if (FFailed || FFileNamePattern.isEmpty())
  {
    return false;
  }
  // TActionLog::OpenLogFile (OpenFile, never appending)
  FCurrentFileName = QFileInfo(SessionLog::ExpandFileName(FFileNamePattern, FStarted, FData.get())).absoluteFilePath();
  QDir().mkpath(QFileInfo(FCurrentFileName).absolutePath());
  FFile.setFileName(FCurrentFileName);
  if (!FFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
  {
    // LOG_OPENERROR
    FLastError = QStringLiteral("Can't open log file '%1'.\n%2").arg(FCurrentFileName, FFile.errorString());
    FFailed = true;
    if (Error != nullptr)
    {
      *Error = FLastError;
    }
    return false;
  }
  Platform::MakePrivate(FFile);
  FLogging = true;
  FHeaderPending = true;
  if (!DeferHeader)
  {
    AddHeader();
  }
  return FLogging;
}

void ActionsLog::AddHeader()
{
  // TActionLog::ReflectSettings; FMutex is locked
  FHeaderPending = false;
  DoAdd(XmlDeclaration);
  QString SessionName = (FData != nullptr) ? XmlAttributeEscape(FData->SessionName()) : QStringLiteral("nosession");
  DoAdd(QStringLiteral("<session xmlns=\"http://winscp.net/schema/session/1.0\" name=\"%1\" start=\"%2\">")
    .arg(SessionName, StandardTimestamp()));
}

void ActionsLog::SetSession(const SessionData & Data)
{
  QMutexLocker Lock(&FMutex);
  if (FHeaderPending)
  {
    FData = std::make_unique<SessionData>(Data);
  }
}

void ActionsLog::Close()
{
  QMutexLocker Lock(&FMutex);
  if (!FLogging)
  {
    return;
  }
  // finished actions still waiting for an unfinished one before them
  for (const std::shared_ptr<Record> & Action : FPending)
  {
    if ((Action->St == Record::Committed) || (Action->St == Record::RolledBack))
    {
      WriteRecord(*Action);
    }
  }
  FPending.clear();
  if (FInGroup)
  {
    FInGroup = false;
    FIndent = QStringLiteral("  ");
    DoAdd(FIndent + QStringLiteral("</group>"));
  }
  DoAdd(QStringLiteral("</session>"));
  FFile.close();
  FLogging = false;
}

bool ActionsLog::Logging() const
{
  QMutexLocker Lock(&FMutex);
  return FLogging;
}

bool ActionsLog::Failed() const
{
  QMutexLocker Lock(&FMutex);
  return FFailed;
}

QString ActionsLog::CurrentFileName() const
{
  QMutexLocker Lock(&FMutex);
  return FCurrentFileName;
}

QString ActionsLog::LastError() const
{
  QMutexLocker Lock(&FMutex);
  return FLastError;
}

void ActionsLog::DoAdd(const QString & Line)
{
  // TActionLog::Add; FMutex is locked
  if (!FLogging)
  {
    return;
  }
  if (FHeaderPending)
  {
    AddHeader();
  }
  QByteArray Data = (Line + QLatin1Char('\n')).toUtf8();
  if ((FFile.write(Data) != Data.size()) || !FFile.flush())
  {
    // LOG_GEN_ERROR: logging is turned off
    FLastError = FFile.errorString();
    FFailed = true;
    FLogging = false;
    FFile.close();
  }
}

void ActionsLog::Add(const QString & Line)
{
  QMutexLocker Lock(&FMutex);
  DoAdd(Line);
}

void ActionsLog::AddIndented(const QString & Line)
{
  QMutexLocker Lock(&FMutex);
  DoAdd(FIndent + Line);
}

void ActionsLog::AddMessages(const QString & Indent, const QStringList & Messages)
{
  for (const QString & Message : Messages)
  {
    DoAdd(FIndent + Indent + QStringLiteral("<message>%1</message>").arg(XmlEscape(Message)));
  }
}

void ActionsLog::AddFailure(const QStringList & Messages)
{
  QMutexLocker Lock(&FMutex);
  DoAdd(FIndent + QStringLiteral("<failure>"));
  AddMessages(QStringLiteral("  "), Messages);
  DoAdd(FIndent + QStringLiteral("</failure>"));
}

void ActionsLog::AddFailure(const ExtException & E)
{
  QStringList Messages = ExceptionMessages(E.Message(), E.MoreMessages());
  if (!Messages.isEmpty())
  {
    AddFailure(Messages);
  }
}

void ActionsLog::BeginGroup(const QString & Name)
{
  QMutexLocker Lock(&FMutex);
  if (FInGroup)
  {
    return;
  }
  FInGroup = true;
  DoAdd(FIndent + QStringLiteral("<group name=\"%1\" start=\"%2\">").arg(XmlAttributeEscape(Name), StandardTimestamp()));
  FIndent = QStringLiteral("    ");
}

void ActionsLog::EndGroup()
{
  QMutexLocker Lock(&FMutex);
  if (!FInGroup)
  {
    return;
  }
  FInGroup = false;
  FIndent = QStringLiteral("  ");
  DoAdd(FIndent + QStringLiteral("</group>"));
}

bool ActionsLog::InGroup() const
{
  QMutexLocker Lock(&FMutex);
  return FInGroup;
}

void ActionsLog::AddPending(const std::shared_ptr<Record> & Action)
{
  QMutexLocker Lock(&FMutex);
  FPending.push_back(Action);
}

void ActionsLog::RecordPending()
{
  // TActionLog::RecordPendingActions; FMutex is locked
  while (!FPending.empty() && (FPending.front()->St != Record::Opened))
  {
    std::shared_ptr<Record> Action = FPending.front();
    FPending.pop_front();
    if (Action->St != Record::Cancelled)
    {
      WriteRecord(*Action);
    }
  }
}

void ActionsLog::WriteFile(const QString & Indent, const RemoteFile & File, bool IncludeFileName)
{
  // TSessionActionRecord::RecordFile
  DoAdd(FIndent + Indent + QStringLiteral("<file>"));
  if (IncludeFileName)
  {
    DoAdd(FIndent + Indent + QStringLiteral("  <filename value=\"%1\" />").arg(XmlAttributeEscape(File.FileName)));
  }
  DoAdd(FIndent + Indent + QStringLiteral("  <type value=\"%1\" />").arg(XmlAttributeEscape(File.TypeChar().toUpper())));
  if (!File.IsDirectory)
  {
    DoAdd(FIndent + Indent + QStringLiteral("  <size value=\"%1\" />").arg(File.Size));
  }
  if (File.Modification.isValid())
  {
    DoAdd(FIndent + Indent + QStringLiteral("  <modification value=\"%1\" />").arg(StandardTimestamp(File.Modification)));
  }
  QString RightsText = File.RightsText();
  if (!RightsText.isEmpty())
  {
    DoAdd(FIndent + Indent + QStringLiteral("  <permissions value=\"%1\" />").arg(XmlAttributeEscape(RightsText)));
  }
  if (!File.Owner.isEmpty())
  {
    DoAdd(FIndent + Indent + QStringLiteral("  <owner value=\"%1\" />").arg(XmlAttributeEscape(File.Owner)));
  }
  if (!File.Group.isEmpty())
  {
    DoAdd(FIndent + Indent + QStringLiteral("  <group value=\"%1\" />").arg(XmlAttributeEscape(File.Group)));
  }
  DoAdd(FIndent + Indent + QStringLiteral("</file>"));
}

void ActionsLog::WriteRecord(const Record & Action)
{
  // TSessionActionRecord::Record
  QString Name = QString::fromLatin1(Action.ActionName());
  DoAdd(FIndent + QStringLiteral("<%1%2>").arg(Name, Action.Recursive ? QStringLiteral(" recursive=\"true\"") : QString()));
  for (const auto & Param : Action.Parameters)
  {
    if (Param.second.isEmpty())
    {
      DoAdd(FIndent + QStringLiteral("  <%1 />").arg(Param.first));
    }
    else
    {
      DoAdd(FIndent + QStringLiteral("  <%1 value=\"%2\" />").arg(Param.first, XmlAttributeEscape(Param.second)));
    }
  }
  if (Action.HasFileList)
  {
    DoAdd(FIndent + QStringLiteral("  <files>"));
    for (const RemoteFile & File : Action.Files)
    {
      WriteFile(QStringLiteral("    "), File, true);
    }
    DoAdd(FIndent + QStringLiteral("  </files>"));
  }
  if (Action.HasFile)
  {
    WriteFile(QStringLiteral("  "), Action.File, false);
  }
  if (Action.St == Record::RolledBack)
  {
    if (Action.HasErrors && !Action.Errors.isEmpty())
    {
      DoAdd(FIndent + QStringLiteral("  <result success=\"false\">"));
      AddMessages(QStringLiteral("    "), Action.Errors);
      DoAdd(FIndent + QStringLiteral("  </result>"));
    }
    else
    {
      DoAdd(FIndent + QStringLiteral("  <result success=\"false\" />"));
    }
  }
  else
  {
    DoAdd(FIndent + QStringLiteral("  <result success=\"true\" />"));
  }
  DoAdd(FIndent + QStringLiteral("</%1>").arg(Name));
}

//---------------------------------------------------------------------------

void ActionsLog::SetCurrent(const std::shared_ptr<ActionsLog> & Log)
{
  CurrentActionsLog = Log;
}

ActionsLog * ActionsLog::Current()
{
  return CurrentActionsLog.get();
}

void ActionsLog::SetSessionLoggingEnabled(bool Enabled)
{
  SessionLoggingEnabled = Enabled;
}

void ActionsLog::SetProcessLog(const std::shared_ptr<ActionsLog> & Log)
{
  QMutexLocker Lock(&SharedLogsMutex);
  ProcessActionsLog = Log;
}

std::shared_ptr<ActionsLog> ActionsLog::ProcessLog()
{
  QMutexLocker Lock(&SharedLogsMutex);
  return ProcessActionsLog;
}

std::shared_ptr<ActionsLog> ActionsLog::CreateForSession(const SessionData & Data, const Configuration & Config)
{
  if (std::shared_ptr<ActionsLog> Log = ProcessLog())
  {
    return Log;
  }
  if (!SessionLoggingEnabled || !Config.LogActions || Config.ActionsLogFileName.isEmpty())
  {
    return nullptr;
  }
  QDateTime Started = QDateTime::currentDateTime();
  QString Name = QFileInfo(SessionLog::ExpandFileName(Config.ActionsLogFileName, Started, &Data)).absoluteFilePath();
  QMutexLocker Lock(&SharedLogsMutex);
  // Sessions logging into the same file (e.g. background transfers of a
  // session) share one log, so they do not overwrite each other
  std::shared_ptr<ActionsLog> Result = SharedLogs().value(Name).lock();
  if (!Result || !Result->Logging())
  {
    Result = std::make_shared<ActionsLog>(Config.ActionsLogFileName, &Data, Started);
    QString Error;
    if (!Result->Open(&Error))
    {
      // LOG_GEN_ERROR: XML logging is turned off for the session
      qWarning("%s", qPrintable(Error));
      return nullptr;
    }
    SharedLogs().insert(Name, Result);
  }
  return Result;
}

//---------------------------------------------------------------------------

SessionAction::SessionAction(LogAction Action, ActionsLog * Log) :
  FUncaught(std::uncaught_exceptions())
{
  if ((Log != nullptr) && Log->Logging())
  {
    FLog = Log;
    FRecord = std::make_shared<ActionsLog::Record>();
    FRecord->Action = Action;
    FLog->AddPending(FRecord);
    OpenActions.push_back(this);
  }
}

SessionAction::SessionAction(LogAction Action, const QString & AFileName, const QString & ADestination) :
  SessionAction(Action)
{
  if (FRecord)
  {
    FileName(AFileName);
    if (!ADestination.isEmpty())
    {
      Destination(ADestination);
    }
  }
}

SessionAction::~SessionAction()
{
  if (FRecord)
  {
    if (FErrorNoted || (std::uncaught_exceptions() > FUncaught))
    {
      Rollback(FNotedError);
    }
    else
    {
      Cancel();
    }
  }
}

void SessionAction::Close(int State)
{
  if (!FRecord)
  {
    return;
  }
  for (auto It = OpenActions.begin(); It != OpenActions.end(); ++It)
  {
    if (*It == this)
    {
      OpenActions.erase(It);
      break;
    }
  }
  std::shared_ptr<ActionsLog::Record> Record = FRecord;
  FRecord.reset();
  QMutexLocker Lock(&FLog->FMutex);
  Record->St = static_cast<ActionsLog::Record::State>(State);
  FLog->RecordPending();
}

void SessionAction::Commit()
{
  Close(ActionsLog::Record::Committed);
}

bool SessionAction::Commit(bool Done)
{
  if (Done)
  {
    Commit();
  }
  else if (FErrorNoted)
  {
    Rollback(FNotedError);
  }
  else
  {
    Cancel();
  }
  return Done;
}

bool SessionAction::Commit(bool Done, qint64 ASize)
{
  if (Done)
  {
    Size(ASize);
  }
  return Commit(Done);
}

void SessionAction::Rollback(const QStringList & Messages)
{
  if (FRecord)
  {
    FRecord->HasErrors = !Messages.isEmpty();
    FRecord->Errors = Messages;
    Close(ActionsLog::Record::RolledBack);
  }
}

void SessionAction::Rollback(const ExtException & E)
{
  Rollback(ActionsLog::ExceptionMessages(E.Message(), E.MoreMessages()));
}

void SessionAction::Cancel()
{
  Close(ActionsLog::Record::Cancelled);
}

void SessionAction::Restart()
{
  if (FRecord)
  {
    FRecord->Recursive = false;
    FRecord->HasFileList = false;
    FRecord->Files.clear();
    FRecord->HasFile = false;
    FRecord->Parameters.clear();
  }
  FErrorNoted = false;
  FNotedError.clear();
}

void SessionAction::NoteError(const ExtException & E)
{
  NoteError(E.Message(), E.MoreMessages());
}

void SessionAction::NoteError(const QString & Message, const QString & MoreMessages)
{
  if (!OpenActions.empty())
  {
    SessionAction * Action = OpenActions.back();
    Action->FErrorNoted = true;
    Action->FNotedError = ActionsLog::ExceptionMessages(Message, MoreMessages);
  }
}

void SessionAction::FileName(const QString & FileName)
{
  if (FRecord)
  {
    FRecord->Parameter(QStringLiteral("filename"), FileName);
  }
}

void SessionAction::Destination(const QString & Destination)
{
  if (FRecord)
  {
    FRecord->Parameter(QStringLiteral("destination"), Destination);
  }
}

void SessionAction::Size(qint64 Size)
{
  if (FRecord)
  {
    FRecord->Parameter(QStringLiteral("size"), QString::number(Size));
  }
}

void SessionAction::Rights(unsigned int Mode)
{
  if (FRecord)
  {
    FRecord->Parameter(QStringLiteral("permissions"), ::Rights::ToText(Mode));
  }
}

void SessionAction::Modification(const QDateTime & DateTime)
{
  if (FRecord)
  {
    FRecord->Parameter(QStringLiteral("modification"), ActionsLog::StandardTimestamp(DateTime));
  }
}

void SessionAction::Recursive()
{
  if (FRecord)
  {
    FRecord->Recursive = true;
  }
}

void SessionAction::Command(const QString & Command)
{
  if (FRecord)
  {
    FRecord->Parameter(QStringLiteral("command"), Command);
  }
}

void SessionAction::AddOutput(const QString & Output, bool StdError)
{
  if (!FRecord)
  {
    return;
  }
  QString Name = StdError ? QStringLiteral("erroroutput") : QStringLiteral("output");
  for (auto & Param : FRecord->Parameters)
  {
    if (Param.first == Name)
    {
      Param.second += QStringLiteral("\r\n") + Output;
      return;
    }
  }
  FRecord->Parameter(Name, Output);
}

void SessionAction::ExitCode(int ExitCode)
{
  if (FRecord)
  {
    FRecord->Parameter(QStringLiteral("exitcode"), QString::number(ExitCode));
  }
}

void SessionAction::Checksum(const QString & Alg, const QString & Checksum)
{
  if (FRecord)
  {
    FRecord->Parameter(QStringLiteral("algorithm"), Alg);
    FRecord->Parameter(QStringLiteral("checksum"), Checksum);
  }
}

void SessionAction::Cwd(const QString & Path)
{
  if (FRecord)
  {
    FRecord->Parameter(QStringLiteral("cwd"), Path);
  }
}

void SessionAction::FileList(const RemoteFileList & Files)
{
  if (FRecord)
  {
    FRecord->HasFileList = true;
    FRecord->Files = Files;
  }
}

void SessionAction::File(const RemoteFile & File)
{
  if (FRecord)
  {
    FRecord->HasFile = true;
    FRecord->File = File;
  }
}

void SessionAction::Difference(const QString & Action, bool IsDirectory, const QString & LocalPath, qint64 LocalSize,
  const QDateTime & LocalModification, const QString & RemotePath, qint64 RemoteSize, const QDateTime & RemoteModification)
{
  // TSessionActionRecord::SynchronizeChecklistItem
  if (!FRecord)
  {
    return;
  }
  FRecord->Parameter(QStringLiteral("action"), Action);
  auto FileInfo = [this, IsDirectory](const QString & Path, qint64 Size, const QDateTime & Modification)
  {
    FRecord->Parameter(QStringLiteral("type"), IsDirectory ? QStringLiteral("D") : QStringLiteral("-"));
    FRecord->Parameter(QStringLiteral("filename"), Path);
    if (!IsDirectory)
    {
      FRecord->Parameter(QStringLiteral("size"), QString::number(Size));
    }
    if (Modification.isValid())
    {
      FRecord->Parameter(QStringLiteral("modification"), ActionsLog::StandardTimestamp(Modification));
    }
  };
  bool RecordLocal = (Action == QLatin1String("uploadnew")) || (Action == QLatin1String("uploadupdate")) ||
    (Action == QLatin1String("downloadupdate")) || (Action == QLatin1String("deletelocal"));
  bool RecordRemote = (Action == QLatin1String("downloadnew")) || (Action == QLatin1String("uploadupdate")) ||
    (Action == QLatin1String("downloadupdate")) || (Action == QLatin1String("deleteremote"));
  if (RecordLocal)
  {
    FileInfo(LocalPath, LocalSize, LocalModification);
  }
  if (RecordRemote)
  {
    FileInfo(RemotePath, RemoteSize, RemoteModification);
  }
}
