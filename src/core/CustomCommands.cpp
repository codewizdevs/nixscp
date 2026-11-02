// Port of WinSCP custom commands: windows/WinConfiguration.cpp (TCustomCommandType,
// TCustomCommandList), core/FileMasks.cpp (TCustomCommand, TInteractiveCustomCommand,
// TCustomCommandData, TFileCustomCommand), windows/WinInterface.cpp
// (TWinInteractiveCustomCommand), windows/GUITools.cpp (TLocalCustomCommand),
// forms/CustomScpExplorer.cpp (CustomCommandState)
#include "core/CustomCommands.h"
#include "core/Configuration.h"
#include "core/IniStorage.h"
#include "core/Platform.h"

#include <QRegularExpression>
#include "core/SessionData.h"

#include <QProcess>

static const QString ConfigurationKey = QStringLiteral("Configuration");
static const QString CustomCommandsKey = QStringLiteral("CustomCommands");
static const QString CustomCommandsParamsKey = QStringLiteral("CustomCommandsParams");
static const QString CustomCommandsShortCutsKey = QStringLiteral("CustomCommandsShortCuts");
static const QString CustomCommandsNoneValue = QStringLiteral("CustomCommandsNone");

// TEXT_TOKEN (WinSCP uses L'\255'; a noncharacter cannot clash with real text)
static const QChar TextToken(0xFFFF);
static const QString Quotes = QStringLiteral("\"'");

//---------------------------------------------------------------------------
// TCustomCommandType / TCustomCommandList
//---------------------------------------------------------------------------

QString CustomCommandType::DisplayName() const
{
  // Menus::StripHotkey
  QString Result;
  for (int Index = 0; Index < Name.size(); Index++)
  {
    if (Name[Index] == QLatin1Char('&'))
    {
      if ((Index + 1 < Name.size()) && (Name[Index + 1] == QLatin1Char('&')))
      {
        Result += QLatin1Char('&');
        Index++;
      }
    }
    else
    {
      Result += Name[Index];
    }
  }
  return Result;
}

QString CustomCommandType::PlainName() const
{
  // StripEllipsis
  QString Result = DisplayName();
  if (Result.endsWith(QStringLiteral("...")))
  {
    Result.chop(3);
    while (!Result.isEmpty() && Result.back().isSpace())
    {
      Result.chop(1);
    }
  }
  return Result;
}

void CustomCommandList::Clear()
{
  FCommands.clear();
}

void CustomCommandList::Add(const QString & Name, const QString & Command, int Params)
{
  Add(CustomCommandType(Name, Command, Params));
}

void CustomCommandList::Add(const CustomCommandType & Command)
{
  Insert(FCommands.size(), Command);
}

void CustomCommandList::Insert(int Index, const CustomCommandType & Command)
{
  FCommands.insert(Index, Command);
  FModified = true;
}

void CustomCommandList::Change(int Index, const CustomCommandType & Command)
{
  if (!(FCommands[Index] == Command))
  {
    FCommands[Index] = Command;
    FModified = true;
  }
}

void CustomCommandList::Move(int CurIndex, int NewIndex)
{
  if (CurIndex != NewIndex)
  {
    FCommands.move(CurIndex, NewIndex);
    FModified = true;
  }
}

void CustomCommandList::Delete(int Index)
{
  FCommands.removeAt(Index);
  FModified = true;
}

const CustomCommandType * CustomCommandList::Find(const QString & Name) const
{
  for (const CustomCommandType & Command : FCommands)
  {
    if (Command.Name == Name)
    {
      return &Command;
    }
  }
  return nullptr;
}

const CustomCommandType * CustomCommandList::FindByShortCut(int ShortCut) const
{
  for (const CustomCommandType & Command : FCommands)
  {
    if (Command.ShortCut == ShortCut)
    {
      return &Command;
    }
  }
  return nullptr;
}

QList<int> CustomCommandList::ShortCuts() const
{
  QList<int> Result;
  for (const CustomCommandType & Command : FCommands)
  {
    if (Command.ShortCut != 0)
    {
      Result.append(Command.ShortCut);
    }
  }
  return Result;
}

CustomCommandList CustomCommandList::Defaults()
{
  // TWinConfiguration::DefaultLocalized
  CustomCommandList Result;
  Result.Add(QStringLiteral("&Execute"), QStringLiteral("\"./!\""), 0);
  Result.Add(QStringLiteral("&Touch"), QStringLiteral("touch \"!\""), ccApplyToDirectories | ccRecursive);
  Result.Add(QStringLiteral("Ta&r/GZip..."),
    QStringLiteral("tar -cz  -f \"!?%1?archive.tgz!\" !&").arg(QStringLiteral("&Archive Name:")), ccApplyToDirectories);
  Result.Add(QStringLiteral("&UnTar/GZip..."),
    QStringLiteral("tar -xz --directory=\"!?%1?.!\" -f \"!\"").arg(QStringLiteral("&Extract to directory:")), 0);
  Result.Add(QStringLiteral("&Grep..."),
    QStringLiteral("grep \"!?%1?!\" !&").arg(QStringLiteral("&Search for pattern:")), ccShowResults);
  // WinSCP: notepad.exe /p "!"
  Result.Add(QStringLiteral("&Print"), QStringLiteral("lp \"!\""), ccLocal);
  Result.Reset();
  return Result;
}

void CustomCommandList::Load(IniFile & File, bool * IsDefault)
{
  // TWinConfiguration::LoadData + TCustomCommandList::Load
  IniStorage Storage(&File);
  bool Stored = false;
  if (Storage.OpenSubKey(ConfigurationKey, false))
  {
    Stored = Storage.HasSubKey(CustomCommandsKey) || Storage.ReadBool(CustomCommandsNoneValue, false);
    if (Stored)
    {
      Clear();
      if (Storage.OpenSubKey(CustomCommandsKey, false))
      {
        for (const QString & Name : Storage.GetValueNames())
        {
          Add(Name, Storage.ReadString(Name, QString()), 0);
        }
        Storage.CloseSubKey();
      }
      if (Storage.OpenSubKey(CustomCommandsParamsKey, false))
      {
        for (CustomCommandType & Command : FCommands)
        {
          Command.Params = Storage.ReadInteger(Command.Name, Command.Params);
        }
        Storage.CloseSubKey();
      }
      if (Storage.OpenSubKey(CustomCommandsShortCutsKey, false))
      {
        for (CustomCommandType & Command : FCommands)
        {
          Command.ShortCut = static_cast<quint16>(Storage.ReadInteger(Command.Name, Command.ShortCut));
        }
        Storage.CloseSubKey();
      }
    }
    Storage.CloseSubKey();
  }
  if (!Stored)
  {
    *this = Defaults();
  }
  Reset();
  if (IsDefault != nullptr)
  {
    *IsDefault = !Stored;
  }
}

// THierarchicalStorage::ClearValues
static void ClearValues(IniFile & File, IniStorage & Storage)
{
  File.EraseSection(Storage.CurrentSection());
}

void CustomCommandList::Save(IniFile & File) const
{
  // TCustomCommandList::Save + TWinConfiguration::SaveData
  IniStorage Storage(&File);
  if (Storage.OpenSubKey(ConfigurationKey, true))
  {
    if (Storage.OpenSubKey(CustomCommandsKey, true))
    {
      ClearValues(File, Storage);
      for (const CustomCommandType & Command : FCommands)
      {
        Storage.WriteString(Command.Name, Command.Command);
      }
      Storage.CloseSubKey();
    }
    if (Storage.OpenSubKey(CustomCommandsParamsKey, true))
    {
      ClearValues(File, Storage);
      for (const CustomCommandType & Command : FCommands)
      {
        Storage.WriteInteger(Command.Name, Command.Params);
      }
      Storage.CloseSubKey();
    }
    if (Storage.OpenSubKey(CustomCommandsShortCutsKey, true))
    {
      ClearValues(File, Storage);
      for (const CustomCommandType & Command : FCommands)
      {
        if (Command.ShortCut != 0)
        {
          Storage.WriteInteger(Command.Name, Command.ShortCut);
        }
      }
      Storage.CloseSubKey();
    }
    if (FCommands.isEmpty())
    {
      Storage.WriteBool(CustomCommandsNoneValue, true);
    }
    else
    {
      Storage.DeleteValue(CustomCommandsNoneValue);
    }
    Storage.CloseSubKey();
  }
}

CustomCommandList CustomCommandList::Configured()
{
  CustomCommandList Result;
  Result.Load(Configuration::Instance().Ini());
  return Result;
}

void CustomCommandList::SetConfigured(const CustomCommandList & List)
{
  // TWinConfiguration::SetCustomCommandList: the defaults stay unsaved
  // until the list is changed
  IniFile & File = Configuration::Instance().Ini();
  CustomCommandList Current;
  Current.Load(File);
  if (!(Current == List))
  {
    List.Save(File);
  }
}

//---------------------------------------------------------------------------
// TCustomCommandData
//---------------------------------------------------------------------------

// EncodeUrlString (core/Common.cpp)
static QString EncodeUrlString(const QString & S)
{
  QString Result;
  for (QChar C : S)
  {
    if (C.isLetter() || ((C >= QLatin1Char('0')) && (C <= QLatin1Char('9'))) ||
        (C == QLatin1Char('_')) || (C == QLatin1Char('-')) || (C == QLatin1Char('.')) || (C == QLatin1Char('*')))
    {
      Result += C;
    }
    else
    {
      for (unsigned char B : QString(C).toUtf8())
      {
        Result += QStringLiteral("%%1").arg(B, 2, 16, QLatin1Char('0')).toUpper();
      }
    }
  }
  return Result;
}

QString CustomCommandData::GenerateSessionUrl(const SessionData & Data, const QString & Password)
{
  // TSessionData::GetProtocolUrl
  QString Url;
  switch (Data.FSProtocol)
  {
    case fsSCPonly:
      Url = QStringLiteral("scp");
      break;
    case fsFTP:
      if (Data.Ftps == ftpsImplicit)
      {
        Url = QStringLiteral("ftps");
      }
      else if ((Data.Ftps == ftpsExplicitTls) || (Data.Ftps == ftpsExplicitSsl))
      {
        Url = QStringLiteral("ftpes");
      }
      else
      {
        Url = QStringLiteral("ftp");
      }
      break;
    case fsWebDAV:
      Url = (Data.Ftps == ftpsImplicit) ? QStringLiteral("davs") : QStringLiteral("dav");
      break;
    case fsS3:
      Url = (Data.Ftps == ftpsImplicit) ? QStringLiteral("s3") : QStringLiteral("s3plain");
      break;
    case fsSFTP:
    case fsSFTPonly:
    default:
      Url = QStringLiteral("sftp");
      break;
  }
  Url += QStringLiteral("://");

  // sufSession = sufUserName | sufPassword | sufHostKey (host key not included)
  if (!Data.UserName.isEmpty())
  {
    Url += EncodeUrlString(Data.UserName);
    if (!Password.isEmpty())
    {
      Url += QLatin1Char(':') + EncodeUrlString(Password);
    }
    Url += QLatin1Char('@');
  }

  if (Data.HostName.contains(QLatin1Char(':')))
  {
    // EscapeIPv6Literal
    Url += QLatin1Char('[') + Data.HostName + QLatin1Char(']');
  }
  else
  {
    Url += EncodeUrlString(Data.HostName);
  }
  if (Data.PortNumber != SessionData::DefaultPort(Data.FSProtocol, Data.Ftps))
  {
    Url += QLatin1Char(':') + QString::number(Data.PortNumber);
  }
  Url += QLatin1Char('/');
  return Url;
}

CustomCommandData CustomCommandData::FromSession(const SessionData & Data, const QString & Password)
{
  CustomCommandData Result;
  Result.HasSession = true;
  Result.HostName = Data.HostName;
  Result.UserName = Data.UserName;
  Result.Password = Password.isEmpty() ? Data.Password : Password;
  Result.PortNumber = Data.PortNumber;
  Result.SessionUrl = GenerateSessionUrl(Data, Result.Password);
  Result.SessionName = Data.SessionName();
  Result.PublicKeyFile = Data.PublicKeyFile;
  return Result;
}

//---------------------------------------------------------------------------
// TCustomCommand
//---------------------------------------------------------------------------

QString CustomCommand::Escape(const QString & S)
{
  return QString(S).replace(QLatin1Char('!'), QStringLiteral("!!"));
}

QString CustomCommand::DelimitStr(const QString & Str, QChar Quote)
{
  QString SpecialChars;
  if (Quote != QLatin1Char('\''))
  {
    SpecialChars = QStringLiteral("$\\");
    if (Quote == QLatin1Char('"'))
    {
      SpecialChars += QStringLiteral("`\"");
    }
  }
  QString Result;
  Result.reserve(Str.size());
  for (QChar C : Str)
  {
    if (SpecialChars.contains(C))
    {
      Result += QLatin1Char('\\');
    }
    // NixSCP hardening (WinSCP leaves it): a ' would end '!' quoting
    if ((Quote == QLatin1Char('\'')) && (C == QLatin1Char('\'')))
    {
      Result += QStringLiteral("'\\''");
      continue;
    }
    Result += C;
  }
  if (Result.startsWith(QLatin1Char('-')))
  {
    Result.prepend(QStringLiteral("./"));
  }
  return Result;
}

QChar CustomCommand::CharAt(const QString & Command, int Index)
{
  return ((Index >= 1) && (Index <= Command.size())) ? Command[Index - 1] : QChar();
}

void CustomCommand::GetToken(const QString & Command, int Index, int & Len, QChar & PatternCmd)
{
  if (CharAt(Command, Index) == QLatin1Char('!'))
  {
    PatternCmd = CharAt(Command, Index + 1);
    if (PatternCmd.isNull())
    {
      Len = 1;
    }
    else if (PatternCmd == QLatin1Char('!'))
    {
      Len = 2;
    }
    else
    {
      Len = PatternLen(Command, Index);
    }

    if (Len <= 0)
    {
      throw ExtException(QStringLiteral("Unknown pattern '%1' starting at %2.").arg(PatternCmd).arg(Index));
    }
    else if ((Command.size() - Index + 1) < Len)
    {
      throw ExtException(QStringLiteral("Unterminated pattern '%1' starting at %2.").arg(PatternCmd).arg(Index));
    }
  }
  else
  {
    PatternCmd = TextToken;
    int NextPattern = Command.indexOf(QLatin1Char('!'), Index - 1);
    if (NextPattern < 0)
    {
      Len = Command.size() - Index + 1;
    }
    else
    {
      Len = NextPattern - (Index - 1);
    }
  }
}

void CustomCommand::PatternHint(int, const QString &)
{
  // noop
}

QString CustomCommand::Complete(const QString & Command, bool LastPass)
{
  int Index = 1;
  int PatternIndex = 0;
  while (Index <= Command.size())
  {
    int Len;
    QChar PatternCmd;
    GetToken(Command, Index, Len, PatternCmd);

    if ((PatternCmd != TextToken) && (PatternCmd != QLatin1Char('!')))
    {
      PatternHint(PatternIndex, Command.mid(Index - 1, Len));
      PatternIndex++;
    }

    Index += Len;
  }

  QString Result;
  Index = 1;
  PatternIndex = 0;
  while (Index <= Command.size())
  {
    int Len;
    QChar PatternCmd;
    GetToken(Command, Index, Len, PatternCmd);

    if (PatternCmd == TextToken)
    {
      Result += Command.mid(Index - 1, Len);
    }
    else if (PatternCmd == QLatin1Char('!'))
    {
      if (LastPass)
      {
        Result += QLatin1Char('!');
      }
      else
      {
        Result += Command.mid(Index - 1, Len);
      }
    }
    else
    {
      QChar Quote;
      QChar Before = CharAt(Command, Index - 1);
      QChar After = CharAt(Command, Index + Len);
      if ((Index > 1) && (Index + Len - 1 < Command.size()) &&
          Quotes.contains(Before) && Quotes.contains(After) && (Before == After))
      {
        Quote = Before;
      }
      QString Pattern = Command.mid(Index - 1, Len);
      QString Replacement;
      bool Delimit = true;
      if (PatternReplacement(PatternIndex, Pattern, Replacement, Delimit))
      {
        if (!LastPass)
        {
          Replacement = Escape(Replacement);
        }
        if (Delimit)
        {
          DelimitReplacement(Replacement, Quote);
        }
        Result += Replacement;
      }
      else
      {
        Result += Pattern;
      }

      PatternIndex++;
    }

    Index += Len;
  }

  return Result;
}

void CustomCommand::DelimitReplacement(QString & Replacement, QChar Quote)
{
  Replacement = DelimitStr(Replacement, Quote);
}

void CustomCommand::Validate(const QString & Command)
{
  CustomValidate(Command, nullptr);
}

void CustomCommand::CustomValidate(const QString & Command, int * Found)
{
  int Index = 1;
  while (Index <= Command.size())
  {
    int Len;
    QChar PatternCmd;
    GetToken(Command, Index, Len, PatternCmd);
    ValidatePattern(Command, Index, Len, PatternCmd, Found);
    Index += Len;
  }
}

bool CustomCommand::FindPattern(const QString & Command, QChar PatternCmd)
{
  bool Result = false;
  int Index = 1;
  while (!Result && (Index <= Command.size()))
  {
    int Len;
    QChar APatternCmd;
    GetToken(Command, Index, Len, APatternCmd);
    if (((PatternCmd != QLatin1Char('!')) && !PatternCmd.isNull() && (PatternCmd.toLower() == APatternCmd.toLower())) ||
        ((PatternCmd == QLatin1Char('!')) && (Len == 1) && (APatternCmd != TextToken)) ||
        (PatternCmd.isNull() && (APatternCmd != TextToken)))
    {
      Result = true;
    }
    Index += Len;
  }
  return Result;
}

bool CustomCommand::HasAnyPatterns(const QString & Command)
{
  return FindPattern(Command, QChar());
}

void CustomCommand::ValidatePattern(const QString &, int, int, QChar, int *)
{
}

//---------------------------------------------------------------------------
// TInteractiveCustomCommand / TWinInteractiveCustomCommand
//---------------------------------------------------------------------------

InteractiveCustomCommand::InteractiveCustomCommand(CustomCommand * ChildCustomCommand,
    PromptsCallback OnPrompts, ExecuteCallback OnExecute) :
  FChildCustomCommand(ChildCustomCommand),
  FOnPrompts(std::move(OnPrompts)),
  FOnExecute(std::move(OnExecute))
{
}

QString InteractiveCustomCommand::ExecuteLocalCommand(const QString & Command)
{
  // ExecuteProcessAndReadOutput(Command, Value, ExitCode, false) + TrimRight
  QProcess Process;
  Process.setProcessChannelMode(QProcess::SeparateChannels);
  Platform::SetShellCommand(Process, Command);
  Process.start();
  if (!Process.waitForStarted())
  {
    throw ExtException(QStringLiteral("Cannot execute '%1'.").arg(Command));
  }
  Process.closeWriteChannel();
  Process.waitForFinished(-1);
  QString Result = QString::fromLocal8Bit(Process.readAllStandardOutput());
  while (!Result.isEmpty() && (Result.back() <= QLatin1Char(' ')))
  {
    Result.chop(1);
  }
  return Result;
}

int InteractiveCustomCommand::PatternLen(const QString & Command, int Index)
{
  int Len;
  QChar PatternCmd = (Index < Command.size()) ? CharAt(Command, Index + 1) : QChar();
  if (PatternCmd == QLatin1Char('?'))
  {
    // the first "!" after "!?"
    int PatternEnd = Command.indexOf(QLatin1Char('!'), Index);
    if (PatternEnd < 0)
    {
      throw ExtException(QStringLiteral("Unterminated pattern '%1' starting at %2.").arg(PatternCmd).arg(Index));
    }
    Len = PatternEnd - (Index - 1) + 1;
  }
  else if (PatternCmd == QLatin1Char('`'))
  {
    int PatternEnd = Command.indexOf(QLatin1Char('`'), Index + 1);
    if (PatternEnd < 0)
    {
      throw ExtException(QStringLiteral("Unterminated pattern '%1' starting at %2.").arg(PatternCmd).arg(Index));
    }
    Len = PatternEnd - (Index - 1) + 1;
  }
  else
  {
    Len = FChildCustomCommand->PatternLen(Command, Index);
  }
  return Len;
}

bool InteractiveCustomCommand::IsPromptPattern(const QString & Pattern)
{
  return (Pattern.size() >= 3) && (Pattern[1] == QLatin1Char('?'));
}

void InteractiveCustomCommand::ParsePromptPattern(const QString & Pattern, QString & Prompt, QString & Default, bool & Delimit)
{
  // Pattern is "!?prompt?default!" or "!?prompt\?default!" or "!?prompt!"
  int Pos = Pattern.mid(2).indexOf(QLatin1Char('?')) + 1;
  if (Pos > 0)
  {
    Default = Pattern.mid(2 + Pos, Pattern.size() - 3 - Pos);
    if ((Pos > 1) && (Pattern[Pos] == QLatin1Char('\\')))
    {
      Delimit = false;
      Pos--;
    }
    Prompt = Pattern.mid(2, Pos - 1);
  }
  else
  {
    Prompt = Pattern.mid(2, Pattern.size() - 3);
  }
}

void InteractiveCustomCommand::PatternHint(int Index, const QString & Pattern)
{
  if (IsPromptPattern(Pattern))
  {
    QString Prompt;
    QString Default;
    bool Delimit = false;
    ParsePromptPattern(Pattern, Prompt, Default, Delimit);
    FIndexes[Index] = FPrompts.size();
    FPrompts.append(Prompt);
    FDefaults.append(Default);
  }
}

bool InteractiveCustomCommand::PatternReplacement(int Index, const QString & Pattern, QString & Replacement, bool & Delimit)
{
  bool Result;
  if (IsPromptPattern(Pattern))
  {
    QString PromptStr;
    ParsePromptPattern(Pattern, PromptStr, Replacement, Delimit);
    // TWinInteractiveCustomCommand::Prompt: all prompts in one go
    auto It = FIndexes.find(Index);
    if (It != FIndexes.end())
    {
      if (!FPrompted)
      {
        FValues = FDefaults;
        if (FOnPrompts && !FOnPrompts(FPrompts, FDefaults, FValues))
        {
          throw AbortException();
        }
        FPrompted = true;
      }
      if (It->second < FValues.size())
      {
        Replacement = FValues[It->second];
      }
    }
    Result = true;
  }
  else if ((Pattern.size() >= 3) && (Pattern[1] == QLatin1Char('`')))
  {
    QString Command = Pattern.mid(2, Pattern.size() - 3);
    Command = FChildCustomCommand->Complete(Command, true);
    Replacement = FOnExecute ? FOnExecute(Command) : ExecuteLocalCommand(Command);
    Delimit = false;
    Result = true;
  }
  else
  {
    Result = false;
  }
  return Result;
}

//---------------------------------------------------------------------------
// TFileCustomCommand
//---------------------------------------------------------------------------

FileCustomCommand::FileCustomCommand(const CustomCommandData & Data, const QString & Path) :
  FData(Data), FPath(Path)
{
}

FileCustomCommand::FileCustomCommand(const CustomCommandData & Data, const QString & Path,
    const QString & FileName, const QString & FileList) :
  FData(Data), FPath(Path), FFileName(FileName), FFileList(FileList)
{
}

int FileCustomCommand::PatternLen(const QString & Command, int Index)
{
  QChar PatternCmd = (Index < Command.size()) ? CharAt(Command, Index + 1).toLower() : QChar();
  static const QString TwoCharPatterns = QStringLiteral("se@upk#/&n");
  return (!PatternCmd.isNull() && TwoCharPatterns.contains(PatternCmd)) ? 2 : 1;
}

bool FileCustomCommand::PatternReplacement(int, const QString & Pattern, QString & Replacement, bool & Delimit)
{
  // keep consistent with TSessionLog::OpenLogFile
  if ((Pattern.compare(QStringLiteral("!s"), Qt::CaseInsensitive) == 0) ||
      (Pattern.compare(QStringLiteral("!e"), Qt::CaseInsensitive) == 0))
  {
    Replacement = FData.HasSession ? FData.SessionUrl : QString();
  }
  else if (Pattern == QStringLiteral("!@"))
  {
    Replacement = FData.HostName;
  }
  else if (Pattern.compare(QStringLiteral("!u"), Qt::CaseInsensitive) == 0)
  {
    Replacement = FData.UserName;
  }
  else if (Pattern.compare(QStringLiteral("!p"), Qt::CaseInsensitive) == 0)
  {
    Replacement = FData.Password;
  }
  else if (Pattern == QStringLiteral("!#"))
  {
    Replacement = FData.HasSession ? QString::number(FData.PortNumber) : QString();
  }
  else if (Pattern.compare(QStringLiteral("!k"), Qt::CaseInsensitive) == 0)
  {
    Replacement = FData.PublicKeyFile;
  }
  else if (Pattern == QStringLiteral("!/"))
  {
    // UnixIncludeTrailingBackslash
    Replacement = FPath;
    if (!Replacement.isEmpty() && !Replacement.endsWith(QLatin1Char('/')))
    {
      Replacement += QLatin1Char('/');
    }
  }
  else if (Pattern == QStringLiteral("!&"))
  {
    Replacement = FFileList;
    // already delimited
    Delimit = false;
  }
  else if (Pattern.compare(QStringLiteral("!n"), Qt::CaseInsensitive) == 0)
  {
    Replacement = FData.SessionName;
  }
  else
  {
    Replacement = FFileName;
  }
  return true;
}

void FileCustomCommand::Validate(const QString & Command)
{
  int Found[2] = { 0, 0 };
  CustomValidate(Command, Found);
  if ((Found[0] > 0) && (Found[1] > 0))
  {
    throw ExtException(QStringLiteral("Cannot combine file name pattern (starting at %1) with file list pattern (starting at %2).")
      .arg(Found[1]).arg(Found[0]));
  }
}

void FileCustomCommand::ValidatePattern(const QString & Command, int Index, int, QChar PatternCmd, int * Found)
{
  if (Found == nullptr)
  {
    return;
  }
  if (PatternCmd == QLatin1Char('&'))
  {
    Found[0] = Index;
  }
  // Unlike WinSCP, "!!" (escaped exclamation mark) is not taken for a file name pattern
  else if ((PatternCmd != TextToken) && (PatternCmd != QLatin1Char('!')) && (PatternLen(Command, Index) == 1))
  {
    Found[1] = Index;
  }
}

bool FileCustomCommand::IsFileListCommand(const QString & Command)
{
  return FindPattern(Command, QLatin1Char('&'));
}

bool FileCustomCommand::IsRemoteFileCommand(const QString & Command)
{
  return FindPattern(Command, QLatin1Char('!')) || FindPattern(Command, QLatin1Char('&'));
}

bool FileCustomCommand::IsFileCommand(const QString & Command)
{
  return IsRemoteFileCommand(Command);
}

bool FileCustomCommand::IsSiteCommand(const QString & Command)
{
  return FindPattern(Command, QLatin1Char('@')) || FindPattern(Command, QLatin1Char('S')) || FindPattern(Command, QLatin1Char('E'));
}

bool FileCustomCommand::IsSessionCommand(const QString & Command)
{
  return
    IsSiteCommand(Command) || IsPasswordCommand(Command) ||
    FindPattern(Command, QLatin1Char('U')) || FindPattern(Command, QLatin1Char('#')) || FindPattern(Command, QLatin1Char('N')) ||
    FindPattern(Command, QLatin1Char('/'));
}

bool FileCustomCommand::IsPasswordCommand(const QString & Command)
{
  return FindPattern(Command, QLatin1Char('p'));
}

//---------------------------------------------------------------------------
// TLocalCustomCommand
//---------------------------------------------------------------------------

LocalCustomCommand::LocalCustomCommand(const CustomCommandData & Data, const QString & RemotePath, const QString & LocalPath) :
  FileCustomCommand(Data, RemotePath), FLocalPath(LocalPath)
{
}

LocalCustomCommand::LocalCustomCommand(const CustomCommandData & Data, const QString & RemotePath, const QString & LocalPath,
    const QString & FileName, const QString & LocalFileName, const QString & FileList) :
  FileCustomCommand(Data, RemotePath, FileName, FileList), FLocalPath(LocalPath), FLocalFileName(LocalFileName)
{
}

int LocalCustomCommand::PatternLen(const QString & Command, int Index)
{
  int Len;
  if ((Index < Command.size()) && (CharAt(Command, Index + 1) == QLatin1Char('\\')))
  {
    Len = 2;
  }
  else if ((Index < Command.size()) && (CharAt(Command, Index + 1) == QLatin1Char('^')))
  {
    Len = 3;
  }
  else
  {
    Len = FileCustomCommand::PatternLen(Command, Index);
  }
  return Len;
}

bool LocalCustomCommand::PatternReplacement(int Index, const QString & Pattern, QString & Replacement, bool & Delimit)
{
  bool Result;
  if (Pattern == QStringLiteral("!\\"))
  {
    // ExcludeTrailingBackslash
    Replacement = FLocalPath;
    while ((Replacement.size() > 1) && Replacement.endsWith(QLatin1Char('/')))
    {
      Replacement.chop(1);
    }
    Result = true;
  }
  else if (Pattern == QStringLiteral("!^!"))
  {
    Replacement = FLocalFileName;
    Result = true;
  }
  else
  {
    Result = FileCustomCommand::PatternReplacement(Index, Pattern, Replacement, Delimit);
  }
  return Result;
}

// Shell-quotes a value for a local command run through /bin/sh -c.
// WinSCP never delimits local commands (cmd.exe), but on Linux file names
// chosen by the server must not be able to inject shell syntax.
static QString ShellSingleQuoteLocal(const QString & Value)
{
  return Platform::ShellQuote(Value);
}

void LocalCustomCommand::DelimitReplacement(QString & Replacement, QChar Quote)
{
#ifdef Q_OS_WIN
  // cmd.exe: WinSCP does not delimit. Downloaded names cannot contain '"',
  // '|', '<' or '>' (ValidLocalFileName), but "&", "^", "(", ")" and "%" are
  // valid in Windows names and syntax for cmd.exe: unquoted, such a name
  // (chosen by the server) is put in double quotes. Inside quotes it is inert.
  static const QRegularExpression CmdSyntax(QStringLiteral("[\\s&^()%;,=!]"));
  if (FShellQuoting && (Quote != QLatin1Char('"')) && Replacement.contains(CmdSyntax))
  {
    Replacement = QLatin1Char('"') + Replacement + QLatin1Char('"');
  }
  return;
#endif
  if (!FShellQuoting || Replacement.isEmpty())
  {
    return;
  }
  if (Quote == QLatin1Char('\''))
  {
    Replacement.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
  }
  else if (Quote == QLatin1Char('"'))
  {
    QString Result;
    Result.reserve(Replacement.size());
    for (QChar C : std::as_const(Replacement))
    {
      if ((C == QLatin1Char('$')) || (C == QLatin1Char('`')) || (C == QLatin1Char('"')) || (C == QLatin1Char('\\')))
      {
        Result += QLatin1Char('\\');
      }
      Result += C;
    }
    Replacement = Result;
  }
  else
  {
    Replacement = ShellSingleQuoteLocal(Replacement);
  }
}

bool LocalCustomCommand::HasLocalFileName(const QString & Command)
{
  return FindPattern(Command, QLatin1Char('^'));
}

bool LocalCustomCommand::IsFileCommand(const QString & Command)
{
  return FileCustomCommand::IsFileCommand(Command) || HasLocalFileName(Command);
}

//---------------------------------------------------------------------------
// Helpers
//---------------------------------------------------------------------------

QString MakeShellFileList(const QStringList & FileNames)
{
  QStringList Args;
  for (const QString & FileName : FileNames)
  {
    Args.append(QLatin1Char('"') + CustomCommand::DelimitStr(FileName, QLatin1Char('"')) + QLatin1Char('"'));
  }
  return Args.join(QLatin1Char(' '));
}

QString MakeLocalFileList(const QStringList & FileNames)
{
  QStringList Args;
  for (const QString & FileName : FileNames)
  {
    // AddQuotes; single quotes when the list is run by /bin/sh
    Args.append(ShellSingleQuoteLocal(FileName));
  }
  return Args.join(QLatin1Char(' '));
}

// No dialogs, no local commands executed: for analysis only
static QString NoExecute(const QString &)
{
  return QString();
}

QString ValidateCustomCommand(const QString & Command, bool Local)
{
  // TCustomCommandDialog::FormCloseQuery
  try
  {
    RemoteCustomCommand Remote;
    LocalCustomCommand LocalCommand;
    FileCustomCommand * FileCommand = Local ? static_cast<FileCustomCommand *>(&LocalCommand) : &Remote;
    InteractiveCustomCommand Interactive(FileCommand, InteractiveCustomCommand::PromptsCallback(), NoExecute);
    Interactive.Validate(Command);
    QString Cmd = Interactive.Complete(Command, false);
    FileCommand->Validate(Cmd);
  }
  catch (const ExtException & E)
  {
    return E.Message();
  }
  return QString();
}

CustomCommandInfo AnalyzeCustomCommand(const QString & Command, bool Local)
{
  CustomCommandInfo Result;
  try
  {
    RemoteCustomCommand Remote;
    LocalCustomCommand LocalCommand;
    FileCustomCommand * FileCommand = Local ? static_cast<FileCustomCommand *>(&LocalCommand) : &Remote;
    InteractiveCustomCommand Interactive(FileCommand, InteractiveCustomCommand::PromptsCallback(), NoExecute);
    QString Cmd = Interactive.Complete(Command, false);
    Result.FileCommand = FileCommand->IsFileCommand(Cmd);
    Result.FileListCommand = FileCommand->IsFileListCommand(Cmd);
    Result.RemoteFileCommand = FileCommand->IsRemoteFileCommand(Cmd);
    Result.LocalFileName = Local && LocalCommand.HasLocalFileName(Cmd);
    Result.SessionCommand = FileCommand->IsSessionCommand(Cmd);
    Result.PasswordCommand = FileCommand->IsPasswordCommand(Cmd);
  }
  catch (const ExtException &)
  {
    Result = CustomCommandInfo();
    Result.Valid = false;
  }
  return Result;
}

int CustomCommandState(const CustomCommandType & Command, const CustomCommandAvailability & A, CustomCommandListType ListType)
{
  // -1 = hidden, 0 = disabled, 1 = enabled
  int Result;
  bool Local = Command.IsLocal();
  CustomCommandInfo Info = AnalyzeCustomCommand(Command.Command, Local);
  if (!Info.Valid)
  {
    return 0;
  }
  // TScpCommanderForm::IsSideLocalBrowser / IsLocalBrowserMode
  bool LocalBrowserMode = !A.RemotePanel;
  bool CurrentSideLocalBrowser = A.LocalSideActive || LocalBrowserMode;

  if (!Local)
  {
    int AllowedState = A.RemoteAllowed ? 1 : 0;
    // custom command that does not operate with files can be executed anytime ...
    if (!Info.FileCommand)
    {
      // ... but do not show such command in remote file menu
      Result = ((ListType == ccltAll) || (ListType == ccltNonFile)) ? AllowedState : -1;
    }
    else if ((ListType == ccltAll) || ((ListType == ccltFile) && !CurrentSideLocalBrowser))
    {
      Result = A.AnySelectedCurrent ? AllowedState : 0;
    }
    else
    {
      Result = -1;
    }
  }
  else
  {
    // custom command that does not operate with files can be executed anytime
    if (!Info.FileCommand)
    {
      Result = ((ListType == ccltAll) || (ListType == ccltNonFile)) ? 1 : -1;
    }
    else if (Info.LocalFileName)
    {
      if ((ListType == ccltAll) || (ListType == ccltFile))
      {
        // special case is "diff"-style command that can be executed over any side,
        // if we have both sides
        Result = (!LocalBrowserMode && A.AnySelectedLocal && A.AnySelectedRemote) ? 1 : 0;
      }
      else if (ListType == ccltBoth)
      {
        Result = 1;
      }
      else
      {
        Result = -1;
      }
    }
    else
    {
      if ((ListType == ccltAll) ||
          ((ListType == ccltFile) && (!CurrentSideLocalBrowser || ((Command.Params & ccRemoteFiles) == 0))))
      {
        Result = A.AnySelectedCurrent ? 1 : 0;
      }
      else
      {
        Result = -1;
      }
    }

    if ((Result > 0) && Info.SessionCommand && !A.SessionAvailable)
    {
      Result = 0;
    }
  }
  return Result;
}
