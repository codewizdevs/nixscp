// Port of WinSCP source/core/ScpFileSystem.cpp (TSCPFileSystem) and
// TRemoteFile::SetListingStr from source/core/RemoteFiles.cpp
#include "core/ScpFileSystem.h"
#include "core/CopyParam.h"
#include "core/SessionLog.h"
#include "core/SftpFileSystem.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QTimeZone>

#include <fcntl.h>
#include <sys/stat.h>

namespace
{

const int coRaiseExcept = 1;
const int coExpectNoOutput = 2;
const int coWaitForLastLine = 4;
const int coOnlyReturnCode = 8;
const int coIgnoreWarnings = 16;
const int coIgnoreStdErr = 64;

const int ecRaiseExcept = 0x01;
const int ecIgnoreWarnings = 0x02;
const int ecIgnoreStdErr = 0x08;
const int ecDefault = ecRaiseExcept;

// Only one character! See TSCPFileSystem::ReadCommandOutput()
const QString LastLineSeparator = QStringLiteral(":");
const QString LastLine = QStringLiteral("WinSCP: this is end-of-file");
const QString FirstLine = QStringLiteral("WinSCP: this is begin-of-file");

const char * const NationalVars[] =
  {"LANG", "LANGUAGE", "LC_CTYPE", "LC_COLLATE", "LC_MONETARY", "LC_NUMERIC",
   "LC_TIME", "LC_MESSAGES", "LC_ALL", "HUMAN_BLOCKS", "BLOCK_SIZE", "LS_BLOCK_SIZE"};
const QString FullTimeOption = QStringLiteral("--full-time");

// Commands of DefaultCommandSet, for ClearAliases
const char * const ShellCommands[] =
  {"echo", "pwd", "cd", "groups", "scp", "rm", "mv", "mkdir", "chmod", "chgrp", "chown", "unset", "unalias", "ln", "cp", "printenv", "touch"};

const qint64 TransferBlockSize = 32768;
// Larger blocks for sending: fewer blocking writes into the channel window
const qint64 UploadBlockSize = 262144;

// Remote side reported an error over the scp protocol
// (EScpFileSkipped for 1, EScp for 2)
class ScpRemoteError : public ExtException
{
public:
  ScpRemoteError(const QString & Message, bool AFatal) : ExtException(Message), Fatal(AFatal) {}
  bool Fatal;
};

// Progress of a text mode upload: the converted data is sent, the progress
// counts the bytes of the local file (the size the operation expects)
class ConvertedProgress
{
public:
  ConvertedProgress(qint64 LocalSize, qint64 ConvertedSize) : FLocal(LocalSize), FConverted(ConvertedSize) {}
  // Bytes to count once Sent bytes of the converted data were sent in total
  qint64 Advance(qint64 Sent)
  {
    qint64 Total = (FConverted > 0) ?
      static_cast<qint64>((static_cast<long double>(Sent) * FLocal) / FConverted) : FLocal;
    qint64 Result = Total - FCounted;
    FCounted = Total;
    return Result;
  }

private:
  qint64 FLocal;
  qint64 FConverted;
  qint64 FCounted = 0;
};

QString UnixExcludeTrailingBackslash(const QString & Path)
{
  QString Result = Path;
  while ((Result.size() > 1) && Result.endsWith(QLatin1Char('/')))
  {
    Result.chop(1);
  }
  return Result;
}

// TRights::SetText with AllowUndef
unsigned int RightsFromText(const QString & Text)
{
  unsigned int Mode = 0;
  static const unsigned int Bits[9] = {0400, 0200, 0100, 0040, 0020, 0010, 0004, 0002, 0001};
  for (int I = 0; (I < 9) && (I < Text.size()); I++)
  {
    QChar C = Text[I];
    switch (C.unicode())
    {
      case 'r': case 'w': case 'x':
        Mode |= Bits[I];
        break;
      case 's':
        Mode |= Bits[I] | ((I == 2) ? 04000 : (I == 5) ? 02000 : 0);
        break;
      case 'S':
        Mode |= (I == 2) ? 04000 : (I == 5) ? 02000 : 0;
        break;
      case 't':
        Mode |= Bits[I] | ((I == 8) ? 01000 : 0);
        break;
      case 'T':
        Mode |= (I == 8) ? 01000 : 0;
        break;
      default:
        break;
    }
  }
  return Mode;
}

QString JoinMessages(const ExtException & E)
{
  return E.MoreMessages().isEmpty() ? E.Message() : (E.Message() + QLatin1Char('\n') + E.MoreMessages());
}

}

ScpFileSystem::ScpFileSystem(const SessionData & Data, UserInterface * UI) :
  FData(Data),
  FUI(UI),
  FOwnShell(std::make_unique<SecureShell>(Data, UI)),
  FShell(FOwnShell.get()),
  FLsFullTime(Data.SCPLsFullTime)
{
}

ScpFileSystem::ScpFileSystem(const SessionData & Data, UserInterface * UI, SecureShell * ExistingShell) :
  FData(Data),
  FUI(UI),
  FShell(ExistingShell),
  FLsFullTime(Data.SCPLsFullTime)
{
}

ScpFileSystem * ScpFileSystem::Of(FileSystem & Fs)
{
  if (ScpFileSystem * Scp = dynamic_cast<ScpFileSystem *>(&Fs))
  {
    return Scp;
  }
  if (SftpFileSystem * Sftp = dynamic_cast<SftpFileSystem *>(&Fs))
  {
    return Sftp->ScpFallback();
  }
  return nullptr;
}

void ScpFileSystem::DirectoryModified(const QString & Path, bool SubDirs, bool ClearChanges)
{
  if (FCache)
  {
    FCache->FileModified(AbsolutePath(Path), SubDirs, ClearChanges);
  }
}

ScpFileSystem::~ScpFileSystem()
{
  Close();
}

// ---------------------------------------------------------------- session

void ScpFileSystem::Open()
{
  if (FOwnShell)
  {
    // The shell protocol is a sequence of small request/response exchanges
    FShell->SetTcpNoDelay(true);
    FShell->Open();
  }
  else if (!FShell->Active())
  {
    throw FatalException(QStringLiteral("Connection has been unexpectedly closed."));
  }
  FData.UserName = FShell->UserName();
  if (!FCache && (FData.CacheDirectories || FData.CacheDirectoryChanges))
  {
    FCache = SessionDirectoryCache::Acquire(FData);
  }
  FChannel = std::make_unique<SshChannel>(*FShell);
  SessionLog::LogEvent(QStringLiteral("Using SCP protocol."));
  StartShell();
  // TSCPFileSystem::ReadCurrentDirectory; the shell starts in the home directory
  ExecCommand(QStringLiteral("pwd"), ecDefault, 1);
  FHomeDirectory = UnixExcludeTrailingBackslash(FOutput.value(0));
  FShellDirectory = FHomeDirectory;
  if (FLsFullTime == asAuto)
  {
    // WinSCP detects --full-time support with the first directory listing
    // and uses it for single files only once known; detecting it upfront
    // gives exact timestamps to all operations
    try
    {
      ExecCommand(QStringLiteral("%1 -d%2 %3").arg(FData.ListingCommand, ListingOptions(true), ShellQuoteStr(QStringLiteral("."))), ecDefault);
      bool Parsed = false;
      for (const QString & Line : std::as_const(FOutput))
      {
        if (!Line.isEmpty() && !IsTotalListingLine(Line))
        {
          ParseListingLine(Line);
          Parsed = true;
        }
      }
      FLsFullTime = Parsed ? asOn : asOff;
    }
    catch (const FatalException &)
    {
      throw;
    }
    catch (const ExtException &)
    {
      FLsFullTime = asOff;
    }
  }
  // PingType ptDummyCommand: TSCPFileSystem::Idle executes fsNull
  FShell->SetDummyCommandCallback([this]()
  {
    if (!FProcessingCommand && IsOpened())
    {
      ExecCommand(QString(), 0);
    }
  });
}

void ScpFileSystem::Close()
{
  FChannel.reset();
  if (FOwnShell)
  {
    FShell->Close();
  }
  else
  {
    FShell->SetDummyCommandCallback(nullptr);
  }
}

bool ScpFileSystem::IsOpened() const
{
  return FShell->Active() && FChannel && FChannel->IsOpen();
}

void ScpFileSystem::CheckConnection()
{
  if (!IsOpened())
  {
    throw FatalException(QStringLiteral("Connection has been unexpectedly closed."));
  }
}

QString ScpFileSystem::ProtocolName() const
{
  return QStringLiteral("SCP");
}

QString ScpFileSystem::ServerInfo() const
{
  return FShell->SessionInfo().join(QLatin1Char('\n'));
}

void ScpFileSystem::StartShell()
{
  FChannel->OpenShell(FData.Shell);
  // "Use UTF-8 encoding" (Utf / NotUtf)
  FChannel->SetUtf(FData.NotUtf != asOn);
  FShellDirectory.clear();
  if (!FData.PostLoginCommands.isEmpty())
  {
    // TSecureShell::Open: post login commands are sent before the startup
    for (const QString & Command : FData.PostLoginCommands.split(QLatin1Char('\n'), Qt::SkipEmptyParts))
    {
      FChannel->SendLine(Command);
    }
  }
  // TSCPFileSystem::DoStartup: SkipStartupMessage and DetectReturnVar must
  // succeed, otherwise session is to be closed
  try
  {
    SkipStartupMessage();
    if (FData.ReturnVar.isEmpty())
    {
      if (FReturnVar.isEmpty())
      {
        DetectReturnVar();
      }
    }
    else
    {
      FReturnVar = FData.ReturnVar;
    }
  }
  catch (const FatalException &)
  {
    throw;
  }
  catch (const ExtException & E)
  {
    throw FatalException(E.Message(), E.MoreMessages());
  }
  if (FData.ClearAliases)
  {
    ClearAliases();
  }
  if (FData.UnsetNationalVars)
  {
    UnsetNationalVars();
  }
}

void ScpFileSystem::ResetShell()
{
  // Used after an interrupted transfer or command: the remote side of the
  // shell is in an unknown state, so the shell channel is replaced
  // (WinSCP would reconnect the whole session)
  FChannel->Close();
  FOutput.clear();
  if (FShell->Active())
  {
    StartShell();
  }
}

void ScpFileSystem::SkipStartupMessage()
{
  try
  {
    ExecCommand(QString(), 0);
  }
  catch (const FatalException & E)
  {
    throw FatalException(QStringLiteral("Error skipping startup message. Your shell is probably incompatible with the application (BASH is recommended)."),
      JoinMessages(E));
  }
}

void ScpFileSystem::DetectReturnVar()
{
  // #60 17.10.01: "status" and "?" switched
  static const QString ReturnVars[2] = {QStringLiteral("status"), QStringLiteral("?")};
  for (const QString & Var : ReturnVars)
  {
    try
    {
      ExecCommand(QStringLiteral("echo \"$%1\"").arg(Var), ecDefault);
      bool Ok = false;
      int Value = (FOutput.size() == 1) ? FOutput[0].toInt(&Ok) : 256;
      if (Ok && (Value >= 0) && (Value <= 255))
      {
        FReturnVar = Var;
        return;
      }
    }
    catch (const FatalException &)
    {
      throw;
    }
    catch (const ExtException &)
    {
      // try next variable
    }
  }
  throw ExtException(QStringLiteral("Error detecting variable containing return code of last command."));
}

void ScpFileSystem::ClearAliases()
{
  // This command usually fails, because there will never be aliases on all commands
  QString ListingCommand = FData.ListingCommand.section(QLatin1Char(' '), 0, 0);
  QStringList Commands;
  if (!ListingCommand.isEmpty())
  {
    Commands << ListingCommand;
  }
  for (const char * Command : ShellCommands)
  {
    QString Name = QString::fromLatin1(Command);
    if (!Commands.contains(Name))
    {
      Commands << Name;
    }
  }
  // One round trip instead of one per alias
  QStringList Parts;
  for (const QString & Command : Commands)
  {
    Parts << QStringLiteral("unalias \"%1\" 2>/dev/null").arg(Command);
  }
  ExecCommand(Parts.join(QStringLiteral(" ; ")), 0);
}

void ScpFileSystem::UnsetNationalVars()
{
  QStringList Parts;
  for (const char * Var : NationalVars)
  {
    Parts << QStringLiteral("unset \"%1\"").arg(QString::fromLatin1(Var));
  }
  ExecCommand(Parts.join(QStringLiteral(" ; ")), 0);
}

// ---------------------------------------------------------------- command execution

QString ScpFileSystem::FullCommand(const QString & Command, bool Interactive, bool NewLine) const
{
  // TCommandSet::FullCommand ("echo last line" on the same line, #56)
  QString Separator = NewLine ? QStringLiteral("\n") : QStringLiteral(" ; ");
  QString ReturnVar = FReturnVar.isEmpty() ? QStringLiteral("0") : (QLatin1Char('$') + FReturnVar);
  QString LastLineCmd = QStringLiteral("echo \"%1%2%3\"").arg(LastLine, LastLineSeparator, ReturnVar);
  QString FirstLineCmd;
  if (Interactive)
  {
    FirstLineCmd = QStringLiteral("echo \"%1\"").arg(FirstLine) + Separator;
  }
  if (!Command.isEmpty())
  {
    return FirstLineCmd + Command + Separator + LastLineCmd;
  }
  return FirstLineCmd + LastLineCmd;
}

void ScpFileSystem::SendCommand(const QString & Command)
{
  CheckConnection();
  FChannel->ClearStdError();
  FReturnCode = 0;
  FOutput.clear();
  // We suppose, that 'Cmd' already contains command that ensures,
  // that 'LastLine' will be printed
  SessionLog::LogLine(llInput, Command);
  FChannel->SendLine(Command);
  FProcessingCommand = true;
}

bool ScpFileSystem::IsTotalListingLine(const QString & Line)
{
  // On some hosts there is not "total" but "totalt"
  return Line.left(5).compare(QStringLiteral("total"), Qt::CaseInsensitive) == 0;
}

bool ScpFileSystem::RemoveLastLine(QString & Line, int & ReturnCode) const
{
  // #55: even when last line of command output does not contain CR/LF,
  // we can recognize last line
  qsizetype Pos = Line.indexOf(LastLine);
  if (Pos >= 0)
  {
    // There must be nothing after return code number to consider string as last line
    QString ReturnCodeStr = Line.mid(Pos + LastLine.size() + 1);
    bool Ok = false;
    int Code = ReturnCodeStr.toInt(&Ok);
    if (Ok)
    {
      ReturnCode = Code;
      Line.truncate(Pos);
      return true;
    }
  }
  return false;
}

bool ScpFileSystem::TryRemoveLastLine(QString & Line)
{
  return RemoveLastLine(Line, FReturnCode);
}

void ScpFileSystem::SkipFirstLine()
{
  QString Line = FChannel->ReceiveLine();
  SessionLog::LogLine(llOutput, Line, 0);
  if (Line != FirstLine)
  {
    throw FatalException(QStringLiteral("Unexpected input from server: %1").arg(Line));
  }
}

void ScpFileSystem::ReadCommandOutput(int Params, const QString * Command, const OutputEvent * Output)
{
  struct ProcessingGuard
  {
    bool & Flag;
    ~ProcessingGuard() { Flag = false; }
  } Guard{FProcessingCommand};

  if (Params & coWaitForLastLine)
  {
    bool IsLast;
    do
    {
      QString Line = FChannel->ReceiveLine();
      SessionLog::LogLine(llOutput, Line, 0);
      IsLast = TryRemoveLastLine(Line);
      if (!IsLast || !Line.isEmpty())
      {
        FOutput.append(Line);
        if ((Output != nullptr) && *Output)
        {
          (*Output)(Line, false);
        }
      }
    }
    while (!IsLast);
  }

  if (((FReturnCode != 0) && (Params & coRaiseExcept)) || ((Output != nullptr) && *Output))
  {
    // stderr travels separately from stdout; give it a moment to arrive,
    // so it is not lost or attributed to the next command (always for the
    // console, which shows it)
    FChannel->WaitForStdError(50);
  }
  FChannel->FlushStdErrorLine();
  if (!FChannel->StdError().isEmpty()) SessionLog::LogLine(llStdError, FChannel->StdError());

  if (Params & coRaiseExcept)
  {
    QString Message = FChannel->StdError();
    if ((Params & coExpectNoOutput) && !FOutput.isEmpty())
    {
      if (!Message.isEmpty())
      {
        Message += QLatin1Char('\n');
      }
      Message += FOutput.join(QLatin1Char('\n'));
    }
    while (Message.endsWith(QLatin1Char('\n')) || Message.endsWith(QLatin1Char('\r')))
    {
      Message.chop(1);
    }

    bool WrongReturnCode =
      (FReturnCode > 1) ||
      ((FReturnCode == 1) && !(Params & coIgnoreWarnings));

    if (Params & coOnlyReturnCode)
    {
      if (WrongReturnCode)
      {
        throw ExtException(QStringLiteral("Command failed with return code %1.").arg(FReturnCode), Message);
      }
    }
    else
    {
      bool IsStdErrOnlyError = !(Params & coIgnoreWarnings) && !(Params & coIgnoreStdErr);
      bool WrongOutput = !Message.isEmpty() && (FOutput.isEmpty() || IsStdErrOnlyError);
      if (WrongOutput || WrongReturnCode)
      {
        if (Message.isEmpty())
        {
          throw ExtException(QStringLiteral("Command failed with return code %1.").arg(FReturnCode));
        }
        throw ExtException(QStringLiteral("Command '%1'\nfailed with return code %2 and the following error message.")
          .arg((Command != nullptr) ? *Command : QString()).arg(FReturnCode), Message);
      }
    }
  }
}

[[noreturn]] void ScpFileSystem::InvalidOutputError(const QString & Command)
{
  throw ExtException(QStringLiteral("Command '%1' failed with invalid output '%2'.").arg(Command, FOutput.join(QLatin1Char('\n'))));
}

void ScpFileSystem::ExecCommand(const QString & Command, int Params, int MinLines)
{
  if (Params < 0)
  {
    Params = ecDefault;
  }
  SendCommand(FullCommand(Command, false));
  int COParams =
    coWaitForLastLine |
    ((Params & ecRaiseExcept) ? coRaiseExcept : 0) |
    ((Params & ecIgnoreWarnings) ? coIgnoreWarnings : 0) |
    ((Params & ecIgnoreStdErr) ? coIgnoreStdErr : 0);
  ReadCommandOutput(COParams, &Command);
  if ((Params & ecRaiseExcept) && (MinLines >= 0) && (FOutput.size() < MinLines))
  {
    InvalidOutputError(Command);
  }
}

void ScpFileSystem::ExecCommandOrError(const QString & Command, const QString & Message, int Params)
{
  try
  {
    ExecCommand(Command, Params);
  }
  catch (const FatalException &)
  {
    throw;
  }
  catch (const AbortException &)
  {
    throw;
  }
  catch (const ExtException & E)
  {
    throw ExtException(Message, JoinMessages(E));
  }
}

// ---------------------------------------------------------------- directories and files

QString ScpFileSystem::AbsolutePath(const QString & Path) const
{
  // Relative paths are relative to the home directory, as with SFTP
  if (Path.isEmpty() || (Path == QStringLiteral(".")))
  {
    return FHomeDirectory;
  }
  if (Path.startsWith(QLatin1Char('/')) || Path.startsWith(QLatin1Char('~')))
  {
    return Path;
  }
  return UnixPath::Combine(FHomeDirectory, Path);
}

QString ScpFileSystem::CanonicalizePath(const QString & APath)
{
  // TSCPFileSystem::ChangeDirectory + ReadCurrentDirectory
  QString Directory = AbsolutePath(APath);
  QString ToDir;
  // This effectively disallows entering subdirectories starting with ~ and containing space
  if (Directory.startsWith(QLatin1Char('~')) && (Directory.left(2) != QStringLiteral("~ ")))
  {
    int Slash = Directory.indexOf(QLatin1Char('/'));
    ToDir = (Slash < 0) ? Directory : (Directory.left(Slash + 1) + ShellQuoteStr(Directory.mid(Slash + 1)));
  }
  else
  {
    ToDir = ShellQuoteStr(Directory);
  }
  ExecCommandOrError(QStringLiteral("cd %1 && pwd").arg(ToDir),
    QStringLiteral("Error changing directory to '%1'.").arg(APath));
  if (FOutput.size() < 1)
  {
    InvalidOutputError(QStringLiteral("pwd"));
  }
  FShellDirectory = UnixExcludeTrailingBackslash(FOutput.last());
  return FShellDirectory;
}

QString ScpFileSystem::HomeDirectory()
{
  return FHomeDirectory;
}

QString ScpFileSystem::ListingOptions(bool Auto) const
{
  bool FullTime = (FLsFullTime == asOn) || (Auto && (FLsFullTime == asAuto));
  return FullTime ? (QLatin1Char(' ') + FullTimeOption) : QString();
}

RemoteFile ScpFileSystem::ParseFile(const QString & Line, const QDate & Today) const
{
  RemoteFile File = ParseListingLine(Line, Today);
  // TRemoteFile::ShiftTimeInSeconds(TimeDifference)
  if ((FData.TimeDifference != 0) && File.Modification.isValid())
  {
    File.Modification = File.Modification.addSecs(static_cast<qint64>(FData.TimeDifference * 24 * 60 * 60));
  }
  return File;
}

void ScpFileSystem::ResolveSymlinks(const QString & Directory, RemoteFileList & Files)
{
  // TRemoteFile::FindLinkedFile: the linked files of all symlinks of the
  // listing are looked up by one "ls -d -L" command (per 50 links)
  if (!FData.ResolveSymlinks)
  {
    return;
  }
  QList<qsizetype> Links;
  for (qsizetype I = 0; I < Files.size(); I++)
  {
    if (Files[I].IsSymLink)
    {
      Links.append(I);
    }
  }
  // batches limited by the command line length (well below ARG_MAX)
  const qsizetype MaxCommandLength = 65536;
  QDate Today = QDate::currentDate();
  for (qsizetype Start = 0; Start < Links.size(); )
  {
    QStringList Args;
    QMap<QString, qsizetype> ByName;
    qsizetype Length = 0;
    qsizetype I = Start;
    for (; (I < Links.size()) && ((I == Start) || (Length < MaxCommandLength)); I++)
    {
      const RemoteFile & Link = Files[Links[I]];
      Args << ShellQuoteStr(UnixPath::Combine(Directory, Link.FileName));
      Length += Args.last().size() + 1;
      ByName.insert(Link.FileName, Links[I]);
    }
    Start = I;
    // Broken links make ls fail; whatever was listed is used
    ExecCommand(QStringLiteral("%1 -d -L%2 %3").arg(FData.ListingCommand, ListingOptions(false), Args.join(QLatin1Char(' '))), 0);
    for (const QString & Line : std::as_const(FOutput))
    {
      if (Line.isEmpty() || IsTotalListingLine(Line))
      {
        continue;
      }
      try
      {
        RemoteFile Linked = ParseFile(Line, Today);
        auto It = ByName.find(Linked.FileName);
        if (It != ByName.end())
        {
          RemoteFile & Link = Files[It.value()];
          Link.IsDirectory = (Linked.FileType == RemoteFile::Directory);
          if (!Link.IsDirectory)
          {
            Link.Size = Linked.Size;
          }
        }
      }
      catch (const ExtException &)
      {
      }
    }
  }
}

RemoteFileList ScpFileSystem::ReadDirectory(const QString & Directory)
{
  RemoteFileList Result;
  bool Again;
  do
  {
    Again = false;
    Result.clear();
    try
    {
      int Params = ecDefault | (FData.IgnoreLsWarnings ? ecIgnoreWarnings : 0);
      QString Command = QStringLiteral("%1%2 %3").arg(FData.ListingCommand, ListingOptions(true), ShellQuoteStr(Directory));
      ExecCommand(Command, Params);
      // If output is not empty, we have successfully got file listing,
      // otherwise there was an error
      if (FOutput.isEmpty())
      {
        throw ExtException(QStringLiteral("Server returned empty listing for directory '%1'.").arg(Directory));
      }
      QStringList Output = FOutput;
      // delete leading "total xxx" line
      if (IsTotalListingLine(Output[0]))
      {
        Output.removeFirst();
      }
      QDate Today = QDate::currentDate();
      for (const QString & Line : std::as_const(Output))
      {
        if (Line.isEmpty())
        {
          continue;
        }
        RemoteFile File = ParseFile(Line, Today);
        if ((File.FileName != QStringLiteral(".")) && (File.FileName != QStringLiteral("..")))
        {
          Result.append(File);
        }
      }
      if (FLsFullTime == asAuto)
      {
        // Directory listing with --full-time succeeded, next time all errors
        // during directory listing will be displayed immediately
        FLsFullTime = asOn;
      }
    }
    catch (const FatalException &)
    {
      throw;
    }
    catch (const ExtException & E)
    {
      if (FLsFullTime == asAuto)
      {
        // Directory listing with --full-time failed, try again regular listing
        FLsFullTime = asOff;
        Again = true;
      }
      else
      {
        throw ExtException(QStringLiteral("Error listing directory '%1'.").arg(Directory), JoinMessages(E));
      }
    }
  }
  while (Again);
  ResolveSymlinks(Directory, Result);
  return Result;
}

RemoteFile ScpFileSystem::ReadFile(const QString & Path)
{
  // TSCPFileSystem::CustomReadFile; --full-time only when known to be supported
  int Params = ecDefault | (FData.IgnoreLsWarnings ? ecIgnoreWarnings : 0);
  ExecCommandOrError(QStringLiteral("%1 -d%2 %3").arg(FData.ListingCommand, ListingOptions(false), ShellQuoteStr(Path)),
    QStringLiteral("Cannot get attributes of file '%1'.").arg(Path), Params);
  if (FOutput.isEmpty())
  {
    InvalidOutputError(FData.ListingCommand);
  }
  int LineIndex = 0;
  if (IsTotalListingLine(FOutput[LineIndex]) && (FOutput.size() > 1))
  {
    LineIndex++;
  }
  RemoteFileList Files{ParseFile(FOutput[LineIndex])};
  ResolveSymlinks(UnixPath::ExtractFileDir(Path), Files);
  return Files[0];
}

bool ScpFileSystem::FileExists(const QString & Path, RemoteFile * File)
{
  ExecCommand(QStringLiteral("%1 -d%2 %3").arg(FData.ListingCommand, ListingOptions(false), ShellQuoteStr(Path)), 0);
  QStringList Output = FOutput;
  while (!Output.isEmpty() && (Output[0].isEmpty() || (IsTotalListingLine(Output[0]) && (Output.size() > 1))))
  {
    Output.removeFirst();
  }
  if ((FReturnCode > 1) || Output.isEmpty())
  {
    return false;
  }
  if (File != nullptr)
  {
    RemoteFileList Files{ParseFile(Output[0])};
    ResolveSymlinks(UnixPath::ExtractFileDir(Path), Files);
    *File = Files[0];
  }
  return true;
}

void ScpFileSystem::CreateDirectory(const QString & Path, int /*Mode*/)
{
  // As TSCPFileSystem::CreateDirectory, the mode is not applied (umask)
  auto Modified = qScopeGuard([&]() { DirectoryModified(Path, false); });
  ExecCommandOrError(QStringLiteral("mkdir %1").arg(ShellQuoteStr(Path)),
    QStringLiteral("Error creating folder '%1'.").arg(Path));
}

void ScpFileSystem::DeleteFile(const QString & Path)
{
  // "rm -f -r" deletes directories with their contents
  auto Modified = qScopeGuard([&]() { DirectoryModified(Path, true, true); });
  ExecCommandOrError(QStringLiteral("rm -f -r %1").arg(ShellQuoteStr(Path)),
    QStringLiteral("Error deleting file '%1'.").arg(Path));
}

void ScpFileSystem::RemoveDirectory(const QString & Path)
{
  DeleteFile(Path);
}

void ScpFileSystem::RenameFile(const QString & From, const QString & To)
{
  auto Modified = qScopeGuard([&]()
  {
    DirectoryModified(From, true, true);
    DirectoryModified(To, true, true);
  });
  ExecCommandOrError(QStringLiteral("mv -f %1 %2").arg(ShellQuoteStr(From), ShellQuoteStr(To)),
    QStringLiteral("Error renaming file '%1' to '%2'.").arg(UnixPath::ExtractFileName(From), UnixPath::ExtractFileName(To)));
}

void ScpFileSystem::ChangeMode(const QString & Path, unsigned int Mode)
{
  auto Modified = qScopeGuard([&]() { DirectoryModified(Path, false); });
  ExecCommandOrError(QStringLiteral("chmod %1 %2").arg(Rights::ToOctal(Mode), ShellQuoteStr(Path)),
    QStringLiteral("Can't change properties of file '%1'.").arg(Path));
}

void ScpFileSystem::SetModificationTime(const QString & Path, const QDateTime & Time)
{
  // Not in WinSCP's SCP (it uses "scp -p"); POSIX touch with an UTC timestamp
  QString Stamp = Time.toUTC().toString(QStringLiteral("yyyyMMddHHmm.ss"));
  auto Modified = qScopeGuard([&]() { DirectoryModified(Path, false); });
  ExecCommandOrError(QStringLiteral("TZ=UTC touch -m -t %1 %2").arg(Stamp, ShellQuoteStr(Path)),
    QStringLiteral("Upload of file '%1' was successful, but error occurred while setting the permissions and/or timestamp.").arg(UnixPath::ExtractFileName(Path)));
}

void ScpFileSystem::CreateLink(const QString & Path, const QString & Target)
{
  auto Modified = qScopeGuard([&]() { DirectoryModified(Path, false); });
  ExecCommandOrError(QStringLiteral("ln -s %1 %2").arg(ShellQuoteStr(Target), ShellQuoteStr(Path)),
    QStringLiteral("Cannot create link '%1'.").arg(Path));
}

// ---------------------------------------------------------------- console

int ScpFileSystem::AnyCommand(const QString & Command, const QString & Directory, OutputEvent Output,
  FileOperationProgress & Progress, QString * NewDirectory)
{
  // TSCPFileSystem::AnyCommand; the directory of the console is entered
  // first (EnsureLocation), commands that change it are tracked.
  // A command may change anything (TTerminal::AnyCommand: DirectoryModified)
  auto Modified = qScopeGuard([this]() { DirectoryModified(QStringLiteral("/"), true); });
  if (!Directory.isEmpty() && (Directory != FShellDirectory))
  {
    CanonicalizePath(Directory);
  }
  FChannel->SetCancelProgress(&Progress);
  FChannel->SetStdErrorCallback([&Output](const QString & Line) { if (Output) Output(Line, true); });
  int ExitCode = -1;
  try
  {
    // On its own line, so a trailing comment does not swallow the end marker
    SendCommand(FullCommand(Command, false, true));
    ReadCommandOutput(coWaitForLastLine, &Command, &Output);
    ExitCode = FReturnCode;
    FChannel->SetStdErrorCallback(nullptr);
    FChannel->SetCancelProgress(nullptr);
  }
  catch (const AbortException &)
  {
    FChannel->SetStdErrorCallback(nullptr);
    FChannel->SetCancelProgress(nullptr);
    ResetShell();
    throw;
  }
  catch (...)
  {
    FChannel->SetStdErrorCallback(nullptr);
    FChannel->SetCancelProgress(nullptr);
    throw;
  }
  try
  {
    ExecCommand(QStringLiteral("pwd"), ecDefault, 1);
    FShellDirectory = UnixExcludeTrailingBackslash(FOutput.last());
  }
  catch (const FatalException &)
  {
    throw;
  }
  catch (const ExtException &)
  {
    FShellDirectory.clear();
  }
  if (NewDirectory != nullptr)
  {
    *NewDirectory = FShellDirectory;
  }
  return ExitCode;
}

// ---------------------------------------------------------------- transfer protocol

void ScpFileSystem::SCPResponse(bool * GotLastLine)
{
  // Taken from scp.c response() and modified
  char Resp = 0;
  FChannel->Receive(&Resp, 1);
  if (Resp == 0)
  {
    return;
  }
  QString Msg = FChannel->ReceiveLine();
  QString Line = QChar(QLatin1Char(Resp)) + Msg;
  if (TryRemoveLastLine(Line))
  {
    // Remote scp terminated (or was not started at all)
    if (GotLastLine != nullptr)
    {
      *GotLastLine = true;
    }
    FChannel->ClearStdError();
    ReadCommandOutput(coExpectNoOutput | coRaiseExcept | coOnlyReturnCode);
    throw ScpRemoteError(Line.isEmpty() ? QStringLiteral("Server unexpectedly closed network connection.") : Line, true);
  }
  bool Fatal = (Resp != 1);
  if (!Fatal)
  {
    // The OpenSSH scp server always sends 1 even for fatal errors; using the
    // error message to tell which errors are fatal (OpenSSH 5.3p1 and 7.2p2)
    if ((Msg.compare(QStringLiteral("scp: ambiguous target"), Qt::CaseInsensitive) == 0) ||
        Msg.startsWith(QStringLiteral("scp: error: unexpected filename: "), Qt::CaseInsensitive) ||
        Msg.startsWith(QStringLiteral("scp: protocol error: "), Qt::CaseInsensitive))
    {
      Fatal = true;
    }
  }
  if (Fatal)
  {
    FScpFatalError = true;
  }
  throw ScpRemoteError(Msg, Fatal);
}

void ScpFileSystem::SCPSendError(const QString & /*Message*/, bool Fatal)
{
  char ErrorLevel = Fatal ? 2 : 1;
  FChannel->Send(&ErrorLevel, 1);
  // We don't send exact error message, because some unspecified
  // characters can terminate remote scp
  FChannel->SendLine(QStringLiteral("scp: error"));
}

void ScpFileSystem::Upload(const QString & LocalPath, const QString & RemotePath, int Mode, FileOperationProgress & Progress)
{
  TransferParams Params;
  Upload(LocalPath, RemotePath, Mode, Params, Progress);
}

void ScpFileSystem::Upload(const QString & LocalPath, const QString & RemotePath, int Mode, const TransferParams & Params, FileOperationProgress & Progress)
{
  // TSCPFileSystem::CopyToRemote + SCPSource for a single file: the target is
  // the full remote path, so "scp -t" writes the file under that name
  CheckConnection();
  auto Modified = qScopeGuard([&]() { DirectoryModified(RemotePath, false); });
  QFile Local(LocalPath);
  if (!Local.open(QIODevice::ReadOnly))
  {
    throw ExtException(QStringLiteral("Can't open file '%1'.").arg(LocalPath), Local.errorString());
  }
  qint64 LocalPos = 0;
  qint64 LocalEnd = Local.size();
  const qint64 LocalSize = LocalEnd;
  // Text mode: the whole file converted at once, its size is needed for the
  // C record (TSCPFileSystem::SCPSource AsciiBuf)
  QByteArray Text;
  if (Params.AsciiTransfer)
  {
    Text = LoadTextFile(Local, LocalPath, Params.RemoveBOM, Params.RemoveCtrlZ);
  }
  else
  {
    // Conversions without text mode (TFileBuffer::Convert with cpRemoveBOM / cpRemoveCtrlZ)
    if (Params.RemoveBOM && (Local.peek(3) == QByteArray("\xEF\xBB\xBF")))
    {
      LocalPos = 3;
    }
    if (Params.RemoveCtrlZ && (LocalEnd > LocalPos))
    {
      char Last = 0;
      if (Local.seek(LocalEnd - 1) && (Local.read(&Last, 1) == 1) && (Last == '\x1A'))
      {
        LocalEnd--;
      }
    }
    if (!Local.seek(LocalPos))
    {
      throw ExtException(QStringLiteral("Can't read file '%1'.").arg(LocalPath), Local.errorString());
    }
  }
  qint64 Size = Params.AsciiTransfer ? Text.size() : (LocalEnd - LocalPos);
  // The part already counted by the caller as resumed
  qint64 Uncounted = std::max<qint64>(Params.ResumeOffset, 0);

  FScpFatalError = false;
  bool GotLastLine = false;
  bool BatchStarted = false;
  bool Transferring = false;
  QString DestFileName = UnixPath::ExtractFileName(RemotePath);

  auto Finish = [&](bool Failed)
  {
    // Tell remote side, that we're done
    if (!GotLastLine && IsOpened())
    {
      if (BatchStarted && !FScpFatalError)
      {
        FChannel->SendLine(QStringLiteral("E"));
        try
        {
          SCPResponse();
        }
        catch (const ScpRemoteError &)
        {
        }
      }
      FChannel->ClearStdError();
      ReadCommandOutput(coExpectNoOutput | coWaitForLastLine | coOnlyReturnCode | (Failed ? 0 : coRaiseExcept));
    }
  };

  FChannel->SetCancelProgress(&Progress);
  try
  {
    SendCommand(FullCommand(QStringLiteral("scp%1 -t %2").arg(ScpOptions(false, false), ShellQuoteStr(RemotePath)), true));
    SkipFirstLine();
    try
    {
      SCPResponse(&GotLastLine);
    }
    catch (const ScpRemoteError & E)
    {
      if (GotLastLine)
      {
        throw ExtException(QStringLiteral("Cannot execute SCP to start transfer. Please make sure that SCP is installed on the server and path to it is included in PATH. You may also try SFTP instead of SCP."),
          E.Message());
      }
      throw;
    }
    catch (const ExtException & E)
    {
      if (GotLastLine && IsOpened())
      {
        throw ExtException(QStringLiteral("Cannot execute SCP to start transfer. Please make sure that SCP is installed on the server and path to it is included in PATH. You may also try SFTP instead of SCP."),
          JoinMessages(E));
      }
      throw;
    }
    BatchStarted = true;

    // Send file modes (rights), filesize and file name
    FChannel->SendLine(QStringLiteral("C%1 %2 %3").arg(Rights::ToOctal(static_cast<unsigned int>(Mode))).arg(Size).arg(DestFileName));
    SCPResponse(&GotLastLine);
    // From now on the file has to be finished, otherwise the shell is reset
    Transferring = true;

    QByteArray Buffer(static_cast<qsizetype>(UploadBlockSize), Qt::Uninitialized);
    qint64 Sent = 0;
    if (Params.AsciiTransfer)
    {
      ConvertedProgress Counter(LocalSize, Size);
      while (Sent < Size)
      {
        Progress.CheckCancel();
        qint64 Want = Progress.AdjustToCPSLimit(static_cast<unsigned long>(std::min<qint64>(UploadBlockSize, Size - Sent)));
        FChannel->Send(Text.constData() + Sent, Want);
        Sent += Want;
        Progress.AddTransferred(Counter.Advance(Sent));
      }
      Progress.AddTransferred(Counter.Advance(Size));
    }
    while (Sent < Size)
    {
      Progress.CheckCancel();
      qint64 Want = std::min<qint64>(UploadBlockSize, Size - Sent);
      if (Sent >= Uncounted)
      {
        Want = Progress.AdjustToCPSLimit(static_cast<unsigned long>(Want));
      }
      qint64 Read = Local.read(Buffer.data(), Want);
      if (Read <= 0)
      {
        throw ExtException(QStringLiteral("Can't read file '%1'.").arg(LocalPath), Local.errorString());
      }
      FChannel->Send(Buffer.constData(), Read);
      qint64 Counted = std::max<qint64>(0, Sent + Read - std::max(Sent, Uncounted));
      Sent += Read;
      if (Counted > 0)
      {
        Progress.AddTransferred(Counted);
      }
    }
    FChannel->SendNull();
    Transferring = false;
    SCPResponse(&GotLastLine);
    Finish(false);
  }
  catch (const FatalException &)
  {
    FChannel->SetCancelProgress(nullptr);
    throw;
  }
  catch (const ScpRemoteError & E)
  {
    // The remote side knows the transfer failed
    FChannel->SetCancelProgress(nullptr);
    if (!E.Fatal || !GotLastLine)
    {
      try
      {
        Finish(true);
      }
      catch (const FatalException &)
      {
        throw;
      }
      catch (const ExtException &)
      {
      }
    }
    throw ExtException(QStringLiteral("Copying file '%1' failed.").arg(LocalPath), E.Message());
  }
  catch (...)
  {
    // Cancelled or failed in the middle of the protocol: the remote scp is
    // in an unknown state, so the shell is replaced
    FChannel->SetCancelProgress(nullptr);
    bool WasTransferring = Transferring;
    ResetShell();
    if (WasTransferring && !Params.KeepPartial)
    {
      try
      {
        ExecCommand(QStringLiteral("rm -f %1").arg(ShellQuoteStr(RemotePath)), 0);
      }
      catch (const FatalException &)
      {
        throw;
      }
      catch (const ExtException &)
      {
      }
    }
    throw;
  }
  FChannel->SetCancelProgress(nullptr);
}

void ScpFileSystem::Download(const QString & RemotePath, const QString & LocalPath, FileOperationProgress & Progress)
{
  // Plain download; partial file cleanup is up to the caller
  TransferParams Params;
  Params.KeepPartial = true;
  Download(RemotePath, LocalPath, Params, Progress);
}

void ScpFileSystem::Download(const QString & RemotePath, const QString & LocalPath, const TransferParams & Params, FileOperationProgress & Progress)
{
  // TSCPFileSystem::CopyToLocal + SCPSink for a single file
  CheckConnection();
  qint64 Uncounted = std::max<qint64>(Params.ResumeOffset, 0);
  QFile Local(LocalPath);
  bool LocalCreated = false;
  bool Received = false;
  QString Error;
  QString ErrorMore;
  QString FileName = UnixPath::ExtractFileName(RemotePath);

  auto RemovePartial = [&]()
  {
    if (LocalCreated)
    {
      Local.close();
      // As TSFTPFileSystem::Sink: delete the incomplete file, unless the transfer
      // is resumable and something was transferred
      if (!Params.KeepPartial || (Local.size() == 0))
      {
        Local.remove();
      }
    }
  };

  FChannel->SetCancelProgress(&Progress);
  try
  {
    SendCommand(FullCommand(QStringLiteral("scp%1 -f %2").arg(ScpOptions(false, false), ShellQuoteStr(RemotePath)), true));
    SkipFirstLine();
    FChannel->SendNull();

    for (;;)
    {
      // Receive control record
      QString Line = FChannel->ReceiveLine();
      SessionLog::LogLine(llOutput, Line, 0);
      if (Line.isEmpty())
      {
        throw FatalException(QStringLiteral("SCP protocol error: Unexpected newline"));
      }
      if (TryRemoveLastLine(Line))
      {
        // Remote side finished copying, so remote SCP was closed
        FChannel->ClearStdError();
        try
        {
          ReadCommandOutput(coExpectNoOutput | coRaiseExcept | coOnlyReturnCode | coIgnoreWarnings);
        }
        catch (const ExtException & E)
        {
          if (Error.isEmpty())
          {
            Error = QStringLiteral("Cannot execute SCP to start transfer. Please make sure that SCP is installed on the server and path to it is included in PATH. You may also try SFTP instead of SCP.");
            ErrorMore = JoinMessages(E);
          }
        }
        if (!Received && Error.isEmpty())
        {
          Error = QStringLiteral("Cannot execute SCP to start transfer. Please make sure that SCP is installed on the server and path to it is included in PATH. You may also try SFTP instead of SCP.");
        }
        break;
      }

      // First character distinguish type of control record
      QChar Ctrl = Line[0];
      Line.remove(0, 1);
      switch (Ctrl.unicode())
      {
        case 1:
          // Error; the remote scp continues with the next file (there is none)
          if (Error.isEmpty())
          {
            Error = QStringLiteral("Copying file '%1' failed.").arg(RemotePath);
            ErrorMore = QStringLiteral("Received error message from remote side: '%1'").arg(Line);
          }
          continue;

        case 2:
          // Fatal error, terminate copying
          throw ExtException(Line);

        case 'E':
          FChannel->SendNull();
          continue;

        case 'T':
          // Times are applied by the caller from the listing
          FChannel->SendNull();
          continue;

        case 'C':
        case 'D':
          break;

        default:
          throw FatalException(QStringLiteral("SCP protocol error: Invalid control record (%1; %2)").arg(Ctrl).arg(Line));
      }

      qint64 TSize = -1;
      QString OnlyFileName;
      {
        // "mode size name"; do not trim leading spaces of the filename
        int P1 = Line.indexOf(QLatin1Char(' '));
        int P2 = (P1 >= 0) ? Line.indexOf(QLatin1Char(' '), P1 + 1) : -1;
        bool Ok = false;
        if (P2 > 0)
        {
          TSize = Line.mid(P1 + 1, P2 - P1 - 1).trimmed().toLongLong(&Ok);
          OnlyFileName = UnixPath::ExtractFileName(Line.mid(P2 + 1));
        }
        if (!Ok)
        {
          SCPSendError(QString(), false);
          Error = QStringLiteral("Copying file '%1' failed.").arg(RemotePath);
          ErrorMore = QStringLiteral("SCP protocol error: Illegal file descriptor format");
          continue;
        }
      }
      // Security fix: ensure the file ends up where we asked for it
      if ((Ctrl == QLatin1Char('D')) || (OnlyFileName != FileName) || Received)
      {
        SCPSendError(QString(), false);
        Error = QStringLiteral("Copying file '%1' failed.").arg(RemotePath);
        ErrorMore = QStringLiteral("Server sent a file that was not requested.");
        continue;
      }

      // The whole file is received again even when resuming
      if (!Local.open(QIODevice::WriteOnly | QIODevice::Truncate))
      {
        // In this step we can still cancel transfer, so we do it
        SCPSendError(QString(), false);
        Error = QStringLiteral("Can't create file '%1'.").arg(LocalPath);
        ErrorMore = Local.errorString();
        continue;
      }
      LocalCreated = true;

      // We succeeded, so we confirm transfer to remote side
      FChannel->SendNull();
      // From now we need to finish file transfer, if not, the shell is reset
      QByteArray Buffer(static_cast<qsizetype>(TransferBlockSize), Qt::Uninitialized);
      // Text mode: EOL of the server to the local one (TSCPFileSystem::SCPSink)
      TextEOLConverter Converter(EOLToStr(FData.EOLType), LocalEOL);
      bool Convert = Params.AsciiTransfer && !Converter.Identity();
      auto Write = [&](const QByteArray & Data)
      {
        if (Local.write(Data) != Data.size())
        {
          throw ExtException(QStringLiteral("Can't write to file '%1'.").arg(LocalPath), Local.errorString());
        }
      };
      qint64 Done = 0;
      while (Done < TSize)
      {
        Progress.CheckCancel();
        qint64 Want = std::min<qint64>(TransferBlockSize, TSize - Done);
        if (Done >= Uncounted)
        {
          Want = Progress.AdjustToCPSLimit(static_cast<unsigned long>(Want));
        }
        FChannel->Receive(Buffer.data(), Want);
        if (Convert)
        {
          Write(Converter.Convert(QByteArray::fromRawData(Buffer.constData(), Want)));
        }
        else if (Local.write(Buffer.constData(), Want) != Want)
        {
          throw ExtException(QStringLiteral("Can't write to file '%1'.").arg(LocalPath), Local.errorString());
        }
        qint64 Counted = std::max<qint64>(0, Done + Want - std::max(Done, Uncounted));
        Done += Want;
        if (Counted > 0)
        {
          Progress.AddTransferred(Counted);
        }
      }
      if (Convert)
      {
        Write(Converter.Finish());
      }
      try
      {
        SCPResponse();
      }
      catch (const ScpRemoteError & E)
      {
        // We still need to send confirmation to other side
        FChannel->SendNull();
        Error = QStringLiteral("Copying file '%1' failed.").arg(RemotePath);
        ErrorMore = E.Message();
        continue;
      }
      FChannel->SendNull();
      if (!Local.flush())
      {
        throw ExtException(QStringLiteral("Can't write to file '%1'.").arg(LocalPath), Local.errorString());
      }
      Received = true;
    }
  }
  catch (const FatalException &)
  {
    FChannel->SetCancelProgress(nullptr);
    RemovePartial();
    throw;
  }
  catch (...)
  {
    // Cancelled or failed in the middle of the protocol: make sure the remote
    // scp is gone by replacing the shell
    FChannel->SetCancelProgress(nullptr);
    RemovePartial();
    ResetShell();
    throw;
  }
  FChannel->SetCancelProgress(nullptr);
  if (!Error.isEmpty())
  {
    RemovePartial();
    throw ExtException(Error, ErrorMore);
  }
  Local.close();
}

// ---------------------------------------------------------------- batch transfers and recursive operations

QString ScpFileSystem::ScpOptions(bool PreserveRights, bool PreserveTime) const
{
  // TSCPFileSystem::CopyToRemote / CopyToLocal options
  QString Result;
  if (PreserveRights || PreserveTime)
  {
    Result += QStringLiteral(" -p");
  }
  if (FData.Scp1Compatibility)
  {
    Result += QStringLiteral(" -1");
  }
  return Result;
}

void ScpFileSystem::ChangeModeRecursive(const QString & Path, unsigned int Mode)
{
  auto Modified = qScopeGuard([&]() { DirectoryModified(Path, true); });
  ExecCommandOrError(QStringLiteral("chmod -R %1 %2").arg(Rights::ToOctal(Mode), ShellQuoteStr(Path)),
    QStringLiteral("Can't change properties of file '%1'.").arg(Path));
}

QStringList ScpFileSystem::LookupUsersGroups()
{
  // TTerminal::LookupUsersGroups: LookupUserGroups != asOff
  if ((FData.LookupUserGroups == asOff) || FGroupsLookedUp)
  {
    return FGroups;
  }
  FGroupsLookedUp = true;
  try
  {
    ExecCommand(QStringLiteral("groups"), ecDefault);
    if (!FOutput.isEmpty())
    {
      FGroups = FOutput[0].split(QLatin1Char(' '), Qt::SkipEmptyParts);
    }
  }
  catch (const FatalException &)
  {
    throw;
  }
  catch (const ExtException &)
  {
    // "Error looking up user groups." is only logged by WinSCP
    FGroups.clear();
  }
  return FGroups;
}

bool ScpFileSystem::BatchError(const ScpTransferOptions & Options, const QString & Path, const ExtException & E,
  QList<std::pair<QString, ExtException>> & Errors)
{
  Errors.append({Path, E});
  return !Options.Error || Options.Error(Path, E);
}

namespace
{

unsigned int LocalMode(const QString & Path, bool Directory, const ScpTransferOptions & Options)
{
  if (Options.PreserveRights)
  {
    struct stat St;
    if (::stat(QFile::encodeName(Path).constData(), &St) == 0)
    {
      return St.st_mode & 07777;
    }
  }
  return Directory ? Options.DirMode : Options.FileMode;
}

QString TimeRecord(const QString & Path)
{
  // "T<mtime> 0 <atime> 0" (TSCPFileSystem::SCPSource)
  struct stat St;
  if (::stat(QFile::encodeName(Path).constData(), &St) != 0)
  {
    return QString();
  }
  return QStringLiteral("T%1 0 %2 0").arg(static_cast<qint64>(St.st_mtime)).arg(static_cast<qint64>(St.st_atime));
}

void SetLocalTimes(const QString & Path, qint64 MTime, qint64 ATime)
{
  struct timespec Times[2];
  Times[0].tv_sec = static_cast<time_t>(ATime);
  Times[0].tv_nsec = 0;
  Times[1].tv_sec = static_cast<time_t>(MTime);
  Times[1].tv_nsec = 0;
  utimensat(AT_FDCWD, QFile::encodeName(Path).constData(), Times, 0);
}

bool ValidScpFileName(const QString & Name)
{
  // Security: the server must not escape the target directory
  return !Name.isEmpty() && (Name != QStringLiteral(".")) && (Name != QStringLiteral("..")) &&
    !Name.contains(QLatin1Char('/'));
}

}

QByteArray ScpFileSystem::LoadTextFile(QFile & Local, const QString & LocalPath, bool RemoveBOM, bool RemoveCtrlZ) const
{
  // TFileBuffer::Convert(LocalEOLType, SessionData->EOLType, cpRemoveCtrlZ | cpRemoveBOM)
  QByteArray Data = Local.readAll();
  if (Local.error() != QFileDevice::NoError)
  {
    throw ExtException(QStringLiteral("Can't read file '%1'.").arg(LocalPath), Local.errorString());
  }
  if (RemoveBOM && Data.startsWith(QByteArray("\xEF\xBB\xBF")))
  {
    Data.remove(0, 3);
  }
  if (RemoveCtrlZ && Data.endsWith('\x1A'))
  {
    Data.chop(1);
  }
  TextEOLConverter Converter(LocalEOL, EOLToStr(FData.EOLType));
  if (!Converter.Identity())
  {
    Data = Converter.Convert(Data) + Converter.Finish();
  }
  return Data;
}

void ScpFileSystem::BatchSource(const QString & LocalPath, const QString & TargetDir, const ScpTransferOptions & Options,
  FileOperationProgress & Progress, QSet<QString> & Visited, QString & Transferring,
  QList<std::pair<QString, ExtException>> & Errors)
{
  // TSCPFileSystem::SCPSource / SCPDirectorySource
  Progress.CheckCancel();
  QFileInfo Info(LocalPath);
  bool Directory = Info.isDir();
  if (Options.Include && !Options.Include(LocalPath, Directory))
  {
    return;
  }
  QString Name = Options.TargetName ? Options.TargetName(LocalPath, Directory) : QString();
  if (Name.isEmpty())
  {
    Name = Info.fileName();
  }
  QString TargetPath = UnixPath::Combine(TargetDir, Name);
  auto ModeOf = [&](bool IsDirectory)
  {
    return Options.Mode ? Options.Mode(LocalPath, IsDirectory) : LocalMode(LocalPath, IsDirectory, Options);
  };
  if (Directory)
  {
    QString Canonical = Info.canonicalFilePath();
    if (Visited.contains(Canonical))
    {
      // symlink loop
      return;
    }
    Visited.insert(Canonical);
    try
    {
      if (Options.PreserveTime && Options.PreserveTimeDirs)
      {
        QString Times = TimeRecord(LocalPath);
        if (!Times.isEmpty())
        {
          FChannel->SendLine(Times);
          SCPResponse();
        }
      }
      FChannel->SendLine(QStringLiteral("D%1 0 %2").arg(Rights::ToOctal(ModeOf(true)), Name));
      SCPResponse();
    }
    catch (const ScpRemoteError & E)
    {
      if (E.Fatal)
      {
        throw;
      }
      // the remote side skips the directory
      if (!BatchError(Options, LocalPath, ExtException(QStringLiteral("Copying file '%1' failed.").arg(LocalPath), E.Message()), Errors))
      {
        throw AbortException();
      }
      return;
    }
    const QFileInfoList Entries = QDir(LocalPath).entryInfoList(
      QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name);
    for (const QFileInfo & Entry : Entries)
    {
      BatchSource(Entry.filePath(), TargetPath, Options, Progress, Visited, Transferring, Errors);
    }
    // Leaving directory
    FChannel->SendLine(QStringLiteral("E"));
    SCPResponse();
    if (Options.Finished)
    {
      Options.Finished(LocalPath, TargetPath, true);
    }
    return;
  }

  QFile Local(LocalPath);
  bool Ascii = Options.AsciiTransfer && Options.AsciiTransfer(LocalPath);
  QByteArray Text;
  qint64 LocalSize = 0;
  try
  {
    if (!Local.open(QIODevice::ReadOnly))
    {
      throw ExtException(QStringLiteral("Can't open file '%1'.").arg(LocalPath), Local.errorString());
    }
    LocalSize = Local.size();
    if (Ascii)
    {
      Text = LoadTextFile(Local, LocalPath, Options.RemoveBOM, Options.RemoveCtrlZ);
    }
  }
  catch (const ExtException & E)
  {
    // nothing was sent for this file yet
    if (!BatchError(Options, LocalPath, E, Errors))
    {
      throw AbortException();
    }
    return;
  }
  qint64 Size = Ascii ? Text.size() : LocalSize;
  Progress.SetFile(LocalPath, LocalSize);
  try
  {
    if (Options.PreserveTime)
    {
      QString Times = TimeRecord(LocalPath);
      if (!Times.isEmpty())
      {
        FChannel->SendLine(Times);
        SCPResponse();
      }
    }
    // Send file modes (rights), filesize and file name
    FChannel->SendLine(QStringLiteral("C%1 %2 %3").arg(Rights::ToOctal(ModeOf(false))).arg(Size).arg(Name));
    SCPResponse();
  }
  catch (const ScpRemoteError & E)
  {
    if (E.Fatal)
    {
      throw;
    }
    if (!BatchError(Options, LocalPath, ExtException(QStringLiteral("Copying file '%1' failed.").arg(LocalPath), E.Message()), Errors))
    {
      throw AbortException();
    }
    return;
  }
  // From now on the file has to be finished, otherwise the shell is reset
  // (and the incomplete target is deleted)
  Transferring = TargetPath;
  qint64 Sent = 0;
  if (Ascii)
  {
    ConvertedProgress Counter(LocalSize, Size);
    while (Sent < Size)
    {
      Progress.CheckCancel();
      qint64 Want = Progress.AdjustToCPSLimit(static_cast<unsigned long>(std::min<qint64>(UploadBlockSize, Size - Sent)));
      FChannel->Send(Text.constData() + Sent, Want);
      Sent += Want;
      Progress.AddTransferred(Counter.Advance(Sent));
    }
    Progress.AddTransferred(Counter.Advance(Size));
  }
  else
  {
    QByteArray Buffer(static_cast<qsizetype>(std::min<qint64>(UploadBlockSize, std::max<qint64>(Size, 1))), Qt::Uninitialized);
    while (Sent < Size)
    {
      Progress.CheckCancel();
      qint64 Want = Progress.AdjustToCPSLimit(static_cast<unsigned long>(std::min<qint64>(Buffer.size(), Size - Sent)));
      qint64 Read = Local.read(Buffer.data(), Want);
      if (Read <= 0)
      {
        // This is crucial, if it fails during file transfer, it's fatal error
        // (the shell is reset by UploadBatch)
        throw ExtException(QStringLiteral("Can't read file '%1'.").arg(LocalPath), Local.errorString());
      }
      FChannel->Send(Buffer.constData(), Read);
      Sent += Read;
      Progress.AddTransferred(Read);
    }
  }
  FChannel->SendNull();
  Transferring.clear();
  try
  {
    SCPResponse();
  }
  catch (const ScpRemoteError & E)
  {
    if (E.Fatal)
    {
      throw;
    }
    if (!BatchError(Options, LocalPath, ExtException(QStringLiteral("Copying file '%1' failed.").arg(LocalPath), E.Message()), Errors))
    {
      throw AbortException();
    }
    return;
  }
  if (Options.Finished)
  {
    Options.Finished(LocalPath, TargetPath, false);
  }
}

void ScpFileSystem::UploadBatch(const QStringList & LocalPaths, const QString & TargetDir, const ScpTransferOptions & Options,
  FileOperationProgress & Progress)
{
  // TSCPFileSystem::CopyToRemote
  CheckConnection();
  auto Modified = qScopeGuard([&]() { DirectoryModified(TargetDir, true); });
  FScpFatalError = false;
  bool GotLastLine = false;
  bool BatchStarted = false;
  QString Transferring;
  QList<std::pair<QString, ExtException>> Errors;
  QSet<QString> Visited;

  auto Finish = [&](bool Failed)
  {
    // Tell remote side, that we're done
    if (!GotLastLine && IsOpened())
    {
      if (BatchStarted && !FScpFatalError)
      {
        FChannel->SendLine(QStringLiteral("E"));
        try
        {
          SCPResponse();
        }
        catch (const ScpRemoteError &)
        {
        }
      }
      FChannel->ClearStdError();
      ReadCommandOutput(coExpectNoOutput | coWaitForLastLine | coOnlyReturnCode | coIgnoreWarnings | (Failed ? 0 : coRaiseExcept));
    }
  };

  FChannel->SetCancelProgress(&Progress);
  try
  {
    QString Target = UnixExcludeTrailingBackslash(TargetDir);
    SendCommand(FullCommand(QStringLiteral("scp -r%1 -d -t %2").arg(ScpOptions(Options.PreserveRights, false), ShellQuoteStr(Target)), true));
    SkipFirstLine();
    try
    {
      SCPResponse(&GotLastLine);
    }
    catch (const ExtException & E)
    {
      if (GotLastLine)
      {
        throw ExtException(QStringLiteral("Cannot execute SCP to start transfer. Please make sure that SCP is installed on the server and path to it is included in PATH. You may also try SFTP instead of SCP."),
          JoinMessages(E));
      }
      throw;
    }
    BatchStarted = true;
    for (const QString & LocalPath : LocalPaths)
    {
      if (Progress.Cancelled())
      {
        break;
      }
      BatchSource(LocalPath, Target, Options, Progress, Visited, Transferring, Errors);
    }
    Finish(false);
  }
  catch (const FatalException &)
  {
    FChannel->SetCancelProgress(nullptr);
    throw;
  }
  catch (const ScpRemoteError & E)
  {
    FChannel->SetCancelProgress(nullptr);
    if (!E.Fatal || !GotLastLine)
    {
      try
      {
        Finish(true);
      }
      catch (const FatalException &)
      {
        throw;
      }
      catch (const ExtException &)
      {
      }
    }
    throw ExtException(QStringLiteral("Copying files to remote side failed."), E.Message());
  }
  catch (...)
  {
    // Cancelled or failed in the middle of the protocol: the remote scp is
    // in an unknown state, so the shell is replaced
    FChannel->SetCancelProgress(nullptr);
    if (BatchStarted)
    {
      ResetShell();
    }
    if (!Transferring.isEmpty())
    {
      // SCP cannot resume: do not leave the incomplete file behind
      try
      {
        ExecCommand(QStringLiteral("rm -f %1").arg(ShellQuoteStr(Transferring)), 0);
      }
      catch (const FatalException &)
      {
        throw;
      }
      catch (const ExtException &)
      {
      }
    }
    throw;
  }
  FChannel->SetCancelProgress(nullptr);
  if (!Errors.isEmpty() && !Options.Error)
  {
    throw ExtException(Errors.first().second.Message(), Errors.first().second.MoreMessages());
  }
}

void ScpFileSystem::BatchSink(const QString & RemotePath, const QString & TargetDir, const ScpTransferOptions & Options,
  FileOperationProgress & Progress, QList<std::pair<QString, ExtException>> & Errors)
{
  // TSCPFileSystem::SCPSink
  struct Level
  {
    QString LocalDir;      // the local directory being filled
    QString RemoteDir;
    qint64 MTime = -1;     // times of the directory (T record before D)
    qint64 ATime = -1;
  };
  QList<Level> Stack{Level{TargetDir, UnixPath::ExtractFileDir(RemotePath)}};
  QString TopName = UnixPath::ExtractFileName(UnixExcludeTrailingBackslash(RemotePath));
  bool TopReceived = false;
  qint64 MTime = -1;
  qint64 ATime = -1;
  bool Received = false;
  // Something was refused (excluded): the remote scp then exits with 1
  bool Excluded = false;

  FChannel->SendNull();
  for (;;)
  {
    QString Line = FChannel->ReceiveLine();
    SessionLog::LogLine(llOutput, Line, 0);
    if (Line.isEmpty())
    {
      throw FatalException(QStringLiteral("SCP protocol error: Unexpected newline"));
    }
    if (TryRemoveLastLine(Line))
    {
      // Remote side finished copying, so remote SCP was closed
      FChannel->ClearStdError();
      try
      {
        ReadCommandOutput(coExpectNoOutput | coRaiseExcept | coOnlyReturnCode | coIgnoreWarnings);
      }
      catch (const ExtException & E)
      {
        if (!Received && Errors.isEmpty() && !Excluded)
        {
          if (!BatchError(Options, RemotePath, ExtException(QStringLiteral("Cannot execute SCP to start transfer. Please make sure that SCP is installed on the server and path to it is included in PATH. You may also try SFTP instead of SCP."),
                JoinMessages(E)), Errors))
          {
            throw AbortException();
          }
        }
      }
      return;
    }

    QChar Ctrl = Line[0];
    Line.remove(0, 1);
    switch (Ctrl.unicode())
    {
      case 1:
      {
        // Error; the remote scp continues with the next file.
        // OpenSSH: "scp: <path>: <reason>" (run_err), the path of the file when known
        QString ErrorPath = RemotePath;
        if (Line.startsWith(QStringLiteral("scp: /")))
        {
          qsizetype Colon = Line.lastIndexOf(QStringLiteral(": "));
          if (Colon > 5)
          {
            ErrorPath = Line.mid(5, Colon - 5);
          }
        }
        if (!BatchError(Options, ErrorPath, ExtException(QStringLiteral("Copying file '%1' failed.").arg(ErrorPath),
              QStringLiteral("Received error message from remote side: '%1'").arg(Line)), Errors))
        {
          throw AbortException();
        }
        continue;
      }

      case 2:
        // Fatal error, terminate copying
        throw ExtException(Line);

      case 'E':
        // Exit from directory
        FChannel->SendNull();
        if (Stack.size() <= 1)
        {
          throw FatalException(QStringLiteral("SCP protocol error: Unexpected end of directory"));
        }
        {
          Level Done = Stack.takeLast();
          if (Options.PreserveTime && Options.PreserveTimeDirs && (Done.MTime >= 0))
          {
            SetLocalTimes(Done.LocalDir, Done.MTime, Done.ATime);
          }
          if (Options.Finished)
          {
            Options.Finished(Done.RemoteDir, Done.LocalDir, true);
          }
        }
        continue;

      case 'T':
      {
        // "mtime 0 atime 0": exact times of the next file or directory
        QStringList Parts = Line.split(QLatin1Char(' '));
        bool Ok1 = false, Ok2 = false;
        MTime = (Parts.size() >= 3) ? Parts[0].toLongLong(&Ok1) : -1;
        ATime = (Parts.size() >= 3) ? Parts[2].toLongLong(&Ok2) : -1;
        if (!Ok1 || !Ok2)
        {
          MTime = ATime = -1;
        }
        FChannel->SendNull();
        continue;
      }

      case 'C':
      case 'D':
        break;

      default:
        throw FatalException(QStringLiteral("SCP protocol error: Invalid control record (%1; %2)").arg(Ctrl).arg(Line));
    }

    qint64 TMTime = MTime;
    qint64 TATime = ATime;
    MTime = ATime = -1;
    unsigned int Mode = 0;
    qint64 TSize = -1;
    QString FileName;
    {
      // "mode size name"; do not trim leading spaces of the filename
      int P1 = Line.indexOf(QLatin1Char(' '));
      int P2 = (P1 >= 0) ? Line.indexOf(QLatin1Char(' '), P1 + 1) : -1;
      bool Ok = (P2 > 0) && Rights::FromOctal(Line.left(P1), Mode);
      if (Ok)
      {
        TSize = Line.mid(P1 + 1, P2 - P1 - 1).trimmed().toLongLong(&Ok);
        FileName = Line.mid(P2 + 1);
      }
      if (!Ok)
      {
        SCPSendError(QString(), false);
        if (!BatchError(Options, RemotePath, ExtException(QStringLiteral("Copying file '%1' failed.").arg(RemotePath),
              QStringLiteral("SCP protocol error: Illegal file descriptor format")), Errors))
        {
          throw AbortException();
        }
        continue;
      }
    }
    const Level & Current = Stack.last();
    QString RemoteFull = UnixPath::Combine(Current.RemoteDir, FileName);
    // Security fix: ensure the files end up where we asked for them
    bool Valid = ValidScpFileName(FileName);
    if (Valid && (Stack.size() == 1))
    {
      Valid = !TopReceived && (FileName == TopName);
      TopReceived = true;
    }
    if (!Valid)
    {
      SCPSendError(QString(), false);
      if (!BatchError(Options, RemotePath, ExtException(QStringLiteral("Copying file '%1' failed.").arg(RemotePath),
            QStringLiteral("Server sent a file that was not requested.")), Errors))
      {
        throw AbortException();
      }
      continue;
    }
    bool IsDirectory = (Ctrl == QLatin1Char('D'));
    if (Options.Include && !Options.Include(RemoteFull, IsDirectory))
    {
      // skipped: the remote side skips the file or directory on an error response
      SCPSendError(QString(), false);
      Excluded = true;
      continue;
    }
    QString LocalName = Options.TargetName ? Options.TargetName(RemoteFull, IsDirectory) : QString();
    if (LocalName.isEmpty() || LocalName.contains(QLatin1Char('/')))
    {
      LocalName = FileName;
    }
    QString LocalFull = QDir(Current.LocalDir).filePath(LocalName);

    if (IsDirectory)
    {
      QFileInfo Info(LocalFull);
      if ((Info.exists() && !Info.isDir()) || (!Info.exists() && !QDir().mkdir(LocalFull)))
      {
        SCPSendError(QString(), false);
        if (!BatchError(Options, RemoteFull, ExtException(QStringLiteral("Can't create directory '%1'.").arg(LocalFull)), Errors))
        {
          throw AbortException();
        }
        continue;
      }
      if (Options.PreserveRights)
      {
        ::chmod(QFile::encodeName(LocalFull).constData(), Mode);
      }
      Stack.append(Level{LocalFull, RemoteFull, TMTime, TATime});
      FChannel->SendNull();
      continue;
    }

    Progress.SetFile(RemoteFull, TSize);
    QFile Local(LocalFull);
    if (!Local.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
      // In this step we can still cancel transfer, so we do it
      SCPSendError(QString(), false);
      Progress.AddTransferred(TSize);
      if (!BatchError(Options, RemoteFull, ExtException(QStringLiteral("Can't create file '%1'.").arg(LocalFull), Local.errorString()), Errors))
      {
        throw AbortException();
      }
      continue;
    }
    // We succeeded, so we confirm transfer to remote side
    FChannel->SendNull();
    try
    {
      QByteArray Buffer(static_cast<qsizetype>(std::min<qint64>(TransferBlockSize, std::max<qint64>(TSize, 1))), Qt::Uninitialized);
      // Text mode: EOL of the server to the local one (TSCPFileSystem::SCPSink)
      TextEOLConverter Converter(EOLToStr(FData.EOLType), LocalEOL);
      bool Convert = Options.AsciiTransfer && !Converter.Identity() && Options.AsciiTransfer(RemoteFull);
      auto Write = [&](const char * Data, qint64 Len)
      {
        if (Local.write(Data, Len) != Len)
        {
          throw ExtException(QStringLiteral("Can't write to file '%1'.").arg(LocalFull), Local.errorString());
        }
      };
      qint64 Done = 0;
      while (Done < TSize)
      {
        Progress.CheckCancel();
        qint64 Want = Progress.AdjustToCPSLimit(static_cast<unsigned long>(std::min<qint64>(Buffer.size(), TSize - Done)));
        FChannel->Receive(Buffer.data(), Want);
        if (Convert)
        {
          QByteArray Converted = Converter.Convert(QByteArray::fromRawData(Buffer.constData(), Want));
          Write(Converted.constData(), Converted.size());
        }
        else
        {
          Write(Buffer.constData(), Want);
        }
        Done += Want;
        Progress.AddTransferred(Want);
      }
      if (Convert)
      {
        QByteArray Rest = Converter.Finish();
        Write(Rest.constData(), Rest.size());
      }
    }
    catch (...)
    {
      Local.close();
      Local.remove();
      throw;
    }
    try
    {
      SCPResponse();
    }
    catch (const ScpRemoteError & E)
    {
      // We still need to send confirmation to other side
      FChannel->SendNull();
      Local.close();
      Local.remove();
      if (!BatchError(Options, RemoteFull, ExtException(QStringLiteral("Copying file '%1' failed.").arg(RemoteFull), E.Message()), Errors))
      {
        throw AbortException();
      }
      continue;
    }
    FChannel->SendNull();
    if (!Local.flush())
    {
      throw ExtException(QStringLiteral("Can't write to file '%1'.").arg(LocalFull), Local.errorString());
    }
    Local.close();
    if (Options.PreserveRights)
    {
      ::chmod(QFile::encodeName(LocalFull).constData(), Mode);
    }
    if (Options.PreserveTime && (TMTime >= 0))
    {
      SetLocalTimes(LocalFull, TMTime, TATime);
    }
    Received = true;
    if (Options.Finished)
    {
      Options.Finished(RemoteFull, LocalFull, false);
    }
  }
}

void ScpFileSystem::DownloadBatch(const QStringList & RemotePaths, const QString & TargetDir, const ScpTransferOptions & Options,
  FileOperationProgress & Progress)
{
  // TSCPFileSystem::CopyToLocal: one scp per selected file or directory
  CheckConnection();
  QList<std::pair<QString, ExtException>> Errors;
  FChannel->SetCancelProgress(&Progress);
  try
  {
    for (const QString & RemotePath : RemotePaths)
    {
      if (Progress.Cancelled())
      {
        break;
      }
      QString Source = UnixExcludeTrailingBackslash(RemotePath);
      SendCommand(FullCommand(QStringLiteral("scp -r%1 -d -f %2")
        .arg(ScpOptions(Options.PreserveRights, Options.PreserveTime), ShellQuoteStr(Source)), true));
      SkipFirstLine();
      BatchSink(Source, TargetDir, Options, Progress, Errors);
    }
  }
  catch (const FatalException &)
  {
    FChannel->SetCancelProgress(nullptr);
    throw;
  }
  catch (...)
  {
    // Cancelled or failed in the middle of the protocol: make sure the remote
    // scp is gone by replacing the shell
    FChannel->SetCancelProgress(nullptr);
    ResetShell();
    throw;
  }
  FChannel->SetCancelProgress(nullptr);
  if (!Errors.isEmpty() && !Options.Error)
  {
    throw ExtException(Errors.first().second.Message(), Errors.first().second.MoreMessages());
  }
}

// ---------------------------------------------------------------- listing parser

RemoteFile ScpFileSystem::ParseListingLine(const QString & Value, const QDate & Today)
{
  // TRemoteFile::SetListingStr; value stored in 'Value' can be used for error message
  struct ListingError {};
  QString Line = Value;
  RemoteFile File;
  try
  {
    QString Col;
    // Do we need to do this (is ever TAB is LS output)?
    Line.replace(QLatin1Char('\t'), QLatin1Char(' '));

    auto GetNCol = [&]()
    {
      if (Line.isEmpty()) throw ListingError();
      int P = Line.indexOf(QLatin1Char(' '));
      if (P >= 0)
      {
        Col = Line.left(P);
        Line.remove(0, P + 1);
      }
      else
      {
        Col = Line;
        Line.clear();
      }
    };
    auto TrimLeft = [&]()
    {
      qsizetype I = 0;
      while ((I < Line.size()) && (Line[I] == QLatin1Char(' '))) I++;
      Line.remove(0, I);
    };
    auto GetCol = [&]() { GetNCol(); TrimLeft(); };
    auto ToWord = [](const QString & S) -> int
    {
      bool Ok = false;
      int V = S.toInt(&Ok);
      if (!Ok) throw ListingError();
      return V;
    };
    static const char * const EngShortMonthNames[12] =
      {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    auto ColToMonth = [&]() -> int
    {
      for (int IMonth = 0; IMonth < 12; IMonth++)
      {
        if (Col.compare(QString::fromLatin1(EngShortMonthNames[IMonth]), Qt::CaseInsensitive) == 0)
        {
          return IMonth + 1;
        }
      }
      return 0;
    };

    if (Line.isEmpty()) throw ListingError();
    QChar Type = Line[0];
    Line.remove(0, 1);

    // On some system there is no space between permissions and node blocks count columns
    // so we get only first 9 characters and trim all following spaces (if any)
    File.Permissions = RightsFromText(Line.left(9));
    Line.remove(0, 9);
    // Rights column maybe followed by '+', '@' or '.' signs, we ignore them
    // (On MacOS, there may be a space in between)
    auto IsAttrSign = [](QChar C) { return (C == QLatin1Char('+')) || (C == QLatin1Char('@')) || (C == QLatin1Char('.')); };
    if (!Line.isEmpty() && IsAttrSign(Line[0]))
    {
      Line.remove(0, 1);
    }
    else if ((Line.size() >= 2) && (Line[0] == QLatin1Char(' ')) && IsAttrSign(Line[1]))
    {
      Line.remove(0, 2);
    }
    TrimLeft();

    GetCol();
    bool IsNumber = false;
    Col.toInt(&IsNumber);
    if (IsNumber)
    {
      GetCol();
    }
    // else: if the column is not an integer, suppose it's owner (Android BusyBox)

    File.Owner = Col;

    // #60 17.10.01: group name can contain space
    File.Group.clear();
    GetCol();
    qint64 ASize;
    do
    {
      File.Group += Col;
      GetCol();
      // SSH FS link like "d????????? ? ? ? ? ? name"
      if ((File.Group == QStringLiteral("?")) && (Col == QStringLiteral("?")))
      {
        ASize = 0;
      }
      else
      {
        if (Col.isEmpty()) throw ListingError();
        // for devices etc.. there is additional column ending by comma, we ignore it
        if (Col.endsWith(QLatin1Char(','))) GetCol();
        bool Ok = false;
        ASize = Col.toLongLong(&Ok);
        if (!Ok || (ASize < 0)) ASize = -1;
        // if it's not a number (file size) we take it as part of group name
        // (at least on CygWin, there can be group with space in its name)
        if (ASize < 0) Col = QLatin1Char(' ') + Col;
      }
    }
    while (ASize < 0);

    bool FullTime = false;
    bool DayMonthFormat = false;
    int Day = 0, Month = 0, Year = 0, Hour = 0, Min = 0, Sec = 0;
    int TzOffset = 0;
    bool HasTz = false;
    bool HasTime = true;

    GetCol();
    // SSH FS link, see above
    if (Col == QStringLiteral("?"))
    {
      GetCol();
      HasTime = false;
    }
    else
    {
      // format dd mmm or mmm dd ?
      Day = Col.toInt();
      if (Day > 0)
      {
        DayMonthFormat = true;
        GetCol();
      }
      Month = ColToMonth();
      // if the column is not known month name, it may have been "yyyy-mm-dd"
      // for --full-time format
      if ((Month == 0) && (Col.size() == 10) && (Col[4] == QLatin1Char('-')) && (Col[7] == QLatin1Char('-')))
      {
        Year = ToWord(Col.mid(0, 4));
        Month = ToWord(Col.mid(5, 2));
        Day = ToWord(Col.mid(8, 2));
        GetCol();
        Hour = ToWord(Col.mid(0, 2));
        Min = ToWord(Col.mid(3, 2));
        Sec = (Col.size() >= 8) ? ToWord(Col.mid(6, 2)) : 0;
        // TZ (WinSCP skips it; used here to get the exact time)
        // do not trim leading space of filename
        GetNCol();
        if ((Col.size() == 5) && ((Col[0] == QLatin1Char('+')) || (Col[0] == QLatin1Char('-'))))
        {
          bool Ok = false;
          int Tz = Col.mid(1).toInt(&Ok);
          if (Ok)
          {
            TzOffset = ((Tz / 100) * 3600 + (Tz % 100) * 60) * ((Col[0] == QLatin1Char('-')) ? -1 : 1);
            HasTz = true;
          }
        }
      }
      else
      {
        // or it may have been day name for another format of --full-time
        if (Month == 0)
        {
          GetCol();
          Month = ColToMonth();
          // neither standard, not --full-time format
          if (Month == 0)
          {
            throw ListingError();
          }
          FullTime = true;
        }

        if (Day == 0)
        {
          GetNCol();
          Day = ToWord(Col);
        }
        if ((Day < 1) || (Day > 31)) throw ListingError();

        // second full-time format
        // ddd mmm dd hh:nn:ss yyyy
        if (FullTime)
        {
          GetCol();
          if (Col.size() != 8)
          {
            throw ListingError();
          }
          Hour = ToWord(Col.mid(0, 2));
          Min = ToWord(Col.mid(3, 2));
          Sec = ToWord(Col.mid(6, 2));
          // do not trim leading space of filename
          GetNCol();
          Year = ToWord(Col);
        }
        else
        {
          // for format dd mmm the below description seems not to be true,
          // the year is not aligned to 5 characters
          if (DayMonthFormat)
          {
            GetCol();
          }
          else
          {
            // Time/Year indicator is always 5 characters long (???), on most
            // systems year is aligned to right (_YYYY), but on some to left (YYYY_),
            // we must ensure that trailing space is also deleted, so real
            // separator space is not treated as part of file name
            Col = Line.left(6).trimmed();
            Line.remove(0, 6);
          }
          // Check if we got time (contains :) or year
          int P = Col.indexOf(QLatin1Char(':'));
          if (P > 0)
          {
            Hour = ToWord(Col.left(P));
            Min = ToWord(Col.mid(P + 1));
            if ((Hour > 23) || (Min > 59)) throw ListingError();
            // When we don't got year, we assume current year
            // with exception that the date would be in future
            // in this case we assume last year.
            Year = Today.year();
            if ((Month > Today.month()) ||
                ((Month == Today.month()) && (Day > Today.day()))) Year--;
            Sec = 0;
          }
          else
          {
            Year = ToWord(Col);
            if (Year > 10000) throw ListingError();
            // When we didn't get time we assume midnight
            Hour = 0; Min = 0; Sec = 0;
          }
        }
      }
    }

    if (HasTime)
    {
      QDate Date(Year, Month, Day);
      QTime Time(Hour, Min, Sec);
      if (!Date.isValid() || !Time.isValid())
      {
        throw ListingError();
      }
      File.Modification = HasTz ?
        QDateTime(Date, Time, QTimeZone::fromSecondsAheadOfUtc(TzOffset)).toLocalTime() :
        QDateTime(Date, Time);
    }

    // separating space is already deleted, other spaces are treated as part of name
    File.Size = ASize;
    File.IsSymLink = (Type.toUpper() == QLatin1Char('L'));
    if (File.IsSymLink)
    {
      const QString SymLinkStr = QStringLiteral(" -> ");
      int P = Line.indexOf(SymLinkStr);
      if (P < 0)
      {
        throw ListingError();
      }
      File.LinkTo = Line.mid(P + SymLinkStr.size());
      Line.truncate(P);
    }
    // "ls -d dir/" or "ls -d /"
    QString Path = Line;
    while ((Path.size() > 1) && Path.endsWith(QLatin1Char('/')))
    {
      Path.chop(1);
    }
    File.FileName = (Path == QStringLiteral("/")) ? Path : UnixPath::ExtractFileName(Path);
    if (File.FileName.isEmpty())
    {
      throw ListingError();
    }
    if (File.IsSymLink)
    {
      File.FileType = RemoteFile::Symlink;
    }
    else if (Type.toUpper() == QLatin1Char('D'))
    {
      File.FileType = RemoteFile::Directory;
    }
    else if (Type == QLatin1Char('-'))
    {
      File.FileType = RemoteFile::File;
    }
    else
    {
      File.FileType = RemoteFile::Other;
    }
    File.IsDirectory = (File.FileType == RemoteFile::Directory);
    File.IsHidden = File.FileName.startsWith(QLatin1Char('.'));
  }
  catch (const ListingError &)
  {
    throw ExtException(QStringLiteral("Unexpected directory listing line '%1'.").arg(Value));
  }
  return File;
}

// ---------------------------------------------------------------- server side operations

// TSCPFileSystem::ChangeFileToken: the ID of "name [id]" / "[id]", otherwise the name
static QString ScpToken(const QString & AText)
{
  QString Text = AText.trimmed();
  qsizetype IDStart = Text.lastIndexOf(QLatin1Char('['));
  if ((IDStart >= 0) && Text.endsWith(QLatin1Char(']')))
  {
    bool Ok = false;
    uint Id = Text.mid(IDStart + 1, Text.size() - IDStart - 2).toUInt(&Ok);
    if (Ok)
    {
      return QString::number(Id);
    }
  }
  return Text;
}

void ScpFileSystem::ChangeFileProperties(const QString & Path, const RemoteFile & File, const RemoteProperties & Properties)
{
  // TSCPFileSystem::ChangeFileProperties
  bool IsDirectory = File.IsDirectory;
  bool Recursive = Properties.Recursive && IsDirectory;
  QString RecursiveStr = Recursive ? QStringLiteral("-R ") : QString();
  auto Modified = qScopeGuard([&]() { DirectoryModified(Path, Recursive); });
  const QString Message = QStringLiteral("Can't change properties of file '%1'.").arg(Path);
  // change group before permissions as chgrp change permissions
  if (!Properties.Group.isEmpty())
  {
    ExecCommandOrError(QStringLiteral("chgrp %1%2 %3").arg(RecursiveStr, ShellQuoteStr(ScpToken(Properties.Group)), ShellQuoteStr(Path)),
      Message);
  }
  if (!Properties.Owner.isEmpty())
  {
    ExecCommandOrError(QStringLiteral("chown %1%2 %3").arg(RecursiveStr, ShellQuoteStr(ScpToken(Properties.Owner)), ShellQuoteStr(Path)),
      Message);
  }
  if (Properties.ValidRights)
  {
    QString Mode;
    if (Recursive && Properties.AddXToDirectories)
    {
      // WinSCP adds X to the top directory after the recursive command,
      // which fails when the new mode makes the directories inaccessible
      // while chmod descends; symbolic "X" adds it to all directories
      // at once (x where r, as TRights::AddExecute)
      RemoteProperties Adjusted = Properties;
      Adjusted.AddXToDirectories = false;
      Mode = Adjusted.ChmodStr(false);
      static const char Who[3] = {'u', 'g', 'o'};
      for (int Index = 0; Index < 3; Index++)
      {
        if (Properties.RightsSet & (0400u >> (Index * 3)))
        {
          Mode += (Mode.isEmpty() ? QString() : QStringLiteral(",")) + QLatin1Char(Who[Index]) + QStringLiteral("+X");
        }
      }
    }
    else
    {
      // if we don't set modes recursively, we may add X at once with other options
      bool AddX = IsDirectory && Properties.AddXToDirectories;
      Mode = Properties.RightsDefined() ? Rights::ToOctal(Properties.CombineRights(0, AddX)) : Properties.ChmodStr(AddX);
    }
    if (!Mode.isEmpty())
    {
      ExecCommandOrError(QStringLiteral("chmod %1%2 %3").arg(RecursiveStr, Mode, ShellQuoteStr(Path)), Message);
    }
  }
}

void ScpFileSystem::ChangeOwnerGroup(const QString & Path, const QString & Owner, const QString & Group)
{
  RemoteProperties Properties;
  Properties.Owner = Owner;
  Properties.Group = Group;
  ChangeFileProperties(Path, RemoteFile(), Properties);
}

void ScpFileSystem::CopyFile(const QString & From, const QString & To)
{
  // TSCPFileSystem::CopyFile: the -T is GNU switch and may not be available on all platforms
  auto Modified = qScopeGuard([&]() { DirectoryModified(To, true); });
  const QString Message = QStringLiteral("Error copying file '%1' to '%2'.").arg(From, To);
  QString Args = ShellQuoteStr(From) + QLatin1Char(' ') + ShellQuoteStr(To);
  try
  {
    ExecCommand(QStringLiteral("cp -p -r -f -T ") + Args, ecDefault);
  }
  catch (const FatalException &)
  {
    throw;
  }
  catch (const AbortException &)
  {
    throw;
  }
  catch (const ExtException &)
  {
    // Attempt with -T failed, trying without
    ExecCommandOrError(QStringLiteral("cp -p -r -f ") + Args, Message);
  }
}

void ScpFileSystem::CreateHardLink(const QString & Path, const QString & Target)
{
  // "ln" without -s; a relative target is relative to the link's directory
  QString AbsoluteTarget = Target.startsWith(QLatin1Char('/')) ? Target :
    UnixPath::Combine(UnixPath::ExtractFileDir(AbsolutePath(Path)), Target);
  auto Modified = qScopeGuard([&]() { DirectoryModified(Path, false); });
  ExecCommandOrError(QStringLiteral("ln %1 %2").arg(ShellQuoteStr(AbsoluteTarget), ShellQuoteStr(Path)),
    QStringLiteral("Cannot create link '%1'.").arg(Path));
}

void ScpFileSystem::LookupUsersGroups(QStringList & Users, QStringList & Groups)
{
  // TSCPFileSystem::LookupUsersGroups: the groups of the user ("groups")
  Groups = LookupUsersGroups();
  if (!FData.UserName.isEmpty())
  {
    Users = QStringList{FData.UserName};
  }
}
