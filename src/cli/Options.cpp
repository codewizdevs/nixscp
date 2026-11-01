// Port of WinSCP source/core/Option.cpp (TOptions) and CutToken from source/core/Common.cpp
#include "cli/Options.h"
#include "core/FileSystem.h"

static const QChar ArrayValueDelimiter = QLatin1Char('[');
static const QChar ArrayValueEnd = QLatin1Char(']');

bool CutToken(QString & Str, QString & Token, QString * RawToken, QString * Separator)
{
  // DoCutToken (inspired by PuTTY's sftp_getcmd() from PSFTP.C)
  Token.clear();
  qsizetype Index = 0;
  while ((Index < Str.size()) && ((Str[Index] == QLatin1Char(' ')) || (Str[Index] == QLatin1Char('\t'))))
  {
    Index++;
  }
  if (Index >= Str.size())
  {
    Str.clear();
    return false;
  }
  qsizetype Start = Index;
  bool Quoting = false;
  while (Index < Str.size())
  {
    QChar C = Str[Index];
    if (!Quoting && ((C == QLatin1Char(' ')) || (C == QLatin1Char('\t'))))
    {
      break;
    }
    else if ((C == QLatin1Char('"')) && (Index + 1 < Str.size()) && (Str[Index + 1] == QLatin1Char('"')))
    {
      Index += 2;
      Token += QLatin1Char('"');
    }
    else if (C == QLatin1Char('"'))
    {
      Index++;
      Quoting = !Quoting;
    }
    else
    {
      Token += C;
      Index++;
    }
  }
  if (RawToken != nullptr)
  {
    *RawToken = Str.mid(Start, Index - Start);
  }
  if (Index < Str.size())
  {
    if (Separator != nullptr)
    {
      *Separator = Str.mid(Index, 1);
    }
    Index++;
  }
  else if (Separator != nullptr)
  {
    Separator->clear();
  }
  Str = Str.mid(Index);
  return true;
}

QString QuoteParam(const QString & Param)
{
  QString Result = Param;
  Result.replace(QLatin1Char('"'), QStringLiteral("\"\""));
  if (Result.isEmpty() || Result.contains(QLatin1Char(' ')) || Result.contains(QLatin1Char('\t')) || (Result != Param))
  {
    Result = QLatin1Char('"') + Result + QLatin1Char('"');
  }
  return Result;
}

Options::Options() :
  FSwitchMarks(QStringLiteral("/-")),
  FSwitchValueDelimiters(QStringLiteral("=:["))
{
}

void Options::Parse(const QString & CmdLine)
{
  QString ACmdLine = CmdLine;
  QString Param;
  while (CutToken(ACmdLine, Param))
  {
    Add(Param);
  }
}

void Options::Add(const QString & Value)
{
  if (!FNoMoreSwitches && (Value.size() == 2) && (Value[0] == Value[1]) && FSwitchMarks.contains(Value[0]))
  {
    FNoMoreSwitches = true;
    return;
  }

  bool Switch = false;
  qsizetype Index = 0;
  QChar SwitchMark;
  QChar ValueDelimiter;
  if (!FNoMoreSwitches && (Value.size() >= 2) && FSwitchMarks.contains(Value[0]))
  {
    Index = 1;
    Switch = true;
    SwitchMark = Value[0];
    while (Switch && (Index < Value.size()))
    {
      QChar C = Value[Index];
      if (FSwitchValueDelimiters.contains(C))
      {
        ValueDelimiter = C;
        break;
      }
      // This is to treat /home/martin as parameter, not as switch.
      else if ((C == QLatin1Char('?')) || C.isLetter() ||
               ((C == QLatin1Char('-')) && (SwitchMark == QLatin1Char('-')) && (Value[1] == QLatin1Char('-'))))
      {
        // noop
      }
      else
      {
        Switch = false;
        break;
      }
      ++Index;
    }
  }

  Option O;
  if (Switch)
  {
    O.Type = otSwitch;
    O.Name = Value.mid(1, Index - 1);
    O.Value = Value.mid(Index + 1);
    if ((ValueDelimiter == ArrayValueDelimiter) && O.Value.endsWith(ArrayValueEnd))
    {
      O.Value.chop(1);
    }
    O.ValueSet = (Index < Value.size());
  }
  else
  {
    O.Type = otParam;
    O.Value = Value;
    ++FParamCount;
  }
  O.SwitchMark = SwitchMark;
  FOptions.push_back(O);
}

QString Options::Param(int Index)
{
  QString Result;
  for (Option & O : FOptions)
  {
    if (O.Type == otParam)
    {
      --Index;
      if (Index == 0)
      {
        Result = O.Value;
        O.Used = true;
        break;
      }
    }
  }
  return Result;
}

bool Options::DoFindSwitch(const QString & Switch, QString & Value, int & ParamsStart, int & ParamsCount, bool & ValueSet)
{
  ParamsStart = 0;
  ValueSet = false;
  size_t Index = 0;
  bool Found = false;
  while ((Index < FOptions.size()) && !Found)
  {
    Option & O = FOptions[Index];
    if (O.Type == otParam)
    {
      ParamsStart++;
    }
    else if (O.Name.compare(Switch, Qt::CaseInsensitive) == 0)
    {
      Found = true;
      Value = O.Value;
      ValueSet = O.ValueSet;
      O.Used = true;
    }
    Index++;
  }
  ParamsCount = 0;
  if (Found)
  {
    ParamsStart++;
    while ((Index + ParamsCount < FOptions.size()) && (FOptions[Index + ParamsCount].Type == otParam))
    {
      ParamsCount++;
    }
  }
  else
  {
    ParamsStart = 0;
  }
  return Found;
}

bool Options::FindSwitch(const QString & Switch)
{
  QString Value;
  return FindSwitch(Switch, Value);
}

bool Options::FindSwitch(const QString & Switch, QString & Value)
{
  bool ValueSet;
  return FindSwitch(Switch, Value, ValueSet);
}

bool Options::FindSwitch(const QString & Switch, QString & Value, bool & ValueSet)
{
  int ParamsStart;
  int ParamsCount;
  return DoFindSwitch(Switch, Value, ParamsStart, ParamsCount, ValueSet);
}

bool Options::FindSwitch(const QString & Switch, QStringList & Params, int ParamsMax)
{
  QString Value;
  int ParamsStart;
  int ParamsCount;
  bool ValueSet;
  bool Result = DoFindSwitch(Switch, Value, ParamsStart, ParamsCount, ValueSet);
  if (Result)
  {
    bool Ok = false;
    int AParamsCount = Value.toInt(&Ok);
    if (Ok && (AParamsCount < ParamsCount))
    {
      ParamsCount = AParamsCount;
    }
    if ((ParamsMax >= 0) && (ParamsCount > ParamsMax))
    {
      ParamsCount = ParamsMax;
    }
    for (int Index = 0; Index < ParamsCount; Index++)
    {
      Params.append(Param(ParamsStart + Index));
    }
    ParamsProcessed(ParamsStart, ParamsCount);
  }
  return Result;
}

QString Options::SwitchValue(const QString & Switch, const QString & Default)
{
  QString Value;
  FindSwitch(Switch, Value);
  return Value.isEmpty() ? Default : Value;
}

bool Options::SwitchValue(const QString & Switch, bool Default, bool DefaultOnNonExistence)
{
  QString Value;
  if (!FindSwitch(Switch, Value))
  {
    return DefaultOnNonExistence;
  }
  if (Value.isEmpty())
  {
    return Default;
  }
  if (Value.compare(QStringLiteral("on"), Qt::CaseInsensitive) == 0)
  {
    return true;
  }
  if (Value.compare(QStringLiteral("off"), Qt::CaseInsensitive) == 0)
  {
    return false;
  }
  bool Ok = false;
  int IntValue = Value.toInt(&Ok);
  if (Ok)
  {
    return (IntValue != 0);
  }
  // URL_OPTION_BOOL_VALUE_ERROR
  throw ExtException(QStringLiteral("Invalid switch value '%1'. Valid values are 'on' and 'off'.").arg(Value));
}

bool Options::UnusedSwitch(QString & Switch) const
{
  for (const Option & O : FOptions)
  {
    if ((O.Type == otSwitch) && !O.Used)
    {
      Switch = O.Name;
      return true;
    }
  }
  return false;
}

bool Options::WasSwitchAdded(QString & Switch, QString & Value, QChar & SwitchMark) const
{
  bool Result = !FOptions.empty() && (FOptions.back().Type == otSwitch);
  if (Result)
  {
    Switch = FOptions.back().Name;
    Value = FOptions.back().Value;
    SwitchMark = FOptions.back().SwitchMark;
  }
  return Result;
}

void Options::ParamsProcessed(int ParamsStart, int ParamsCount)
{
  if (ParamsCount <= 0)
  {
    return;
  }
  size_t Index = 0;
  while ((Index < FOptions.size()) && (ParamsStart > 0))
  {
    if (FOptions[Index].Type == otParam)
    {
      --ParamsStart;
      if (ParamsStart == 0)
      {
        while ((ParamsCount > 0) && (Index < FOptions.size()))
        {
          FOptions.erase(FOptions.begin() + static_cast<std::ptrdiff_t>(Index));
          --FParamCount;
          --ParamsCount;
        }
        break;
      }
    }
    Index++;
  }
}
