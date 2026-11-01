// Port of WinSCP source/core/FileMasks.cpp (TFileMasks, MaskFileName & co.),
// the size/time parsing helpers of source/core/Common.cpp and the matching of
// Delphi's Masks unit (Masks::TMask).
#include "core/FileMasks.h"

#include <QRegularExpression>
#include <stdexcept>

const QChar FileMasks::IncludeExcludeFileMasksDelimiter = QLatin1Char('|');
const QString FileMasks::FileMasksDelimiters = QStringLiteral(";,");
static const QString AllFileMasksDelimiters = QStringLiteral(";,|");
static const QString DirectoryMaskDelimiters = QStringLiteral("/\\");
static const QString MaskSymbols = QStringLiteral("?*[");
static const QString AnyMask = QStringLiteral("*.*");

// ---------------------------------------------------------------------------
// Common.cpp helpers (0-based ports)

static bool IsDelimiterAt(const QString & Delimiters, const QString & Str, int Index)
{
  return (Index >= 0) && (Index < Str.size()) && Delimiters.contains(Str[Index]);
}

static int LastDelimiter(const QString & Delimiters, const QString & Str)
{
  for (int Index = Str.size() - 1; Index >= 0; Index--)
  {
    if (Delimiters.contains(Str[Index]))
    {
      return Index;
    }
  }
  return -1;
}

// CopyToChars: From is advanced past the delimiter (as if there were one at the end)
static QString CopyToChars(const QString & Str, int & From, const QString & Chs, QChar * Delimiter, bool DoubleDelimiterEscapes)
{
  QString Result;
  int P;
  for (P = From; P < Str.size(); P++)
  {
    if (IsDelimiterAt(Chs, Str, P))
    {
      if (DoubleDelimiterEscapes && (P < Str.size() - 1) && (Str[P + 1] == Str[P]))
      {
        Result += Str[P];
        P++;
      }
      else
      {
        break;
      }
    }
    else
    {
      Result += Str[P];
    }
  }
  if (Delimiter != nullptr)
  {
    *Delimiter = (P < Str.size()) ? Str[P] : QChar();
  }
  From = P + 1;
  return Result;
}

// Delphi Trim* trim all characters <= ' '
static QString TrimLeft(const QString & S)
{
  int I = 0;
  while ((I < S.size()) && (S[I] <= QLatin1Char(' ')))
  {
    I++;
  }
  return S.mid(I);
}

static QString TrimRight(const QString & S)
{
  int I = S.size();
  while ((I > 0) && (S[I - 1] <= QLatin1Char(' ')))
  {
    I--;
  }
  return S.left(I);
}

static void TrimEx(QString & Str, int & Start, int & End)
{
  QString Buf = TrimLeft(Str);
  Start += Str.size() - Buf.size();
  Str = TrimRight(Buf);
  End -= Buf.size() - Str.size();
}

static bool TryStrToInt64(const QString & S, qint64 & Value)
{
  bool Ok = false;
  Value = S.trimmed().toLongLong(&Ok);
  return Ok;
}

bool TryStrToSize(QString SizeStr, qint64 & Size)
{
  int Index = 0;
  while ((Index < SizeStr.size()) && SizeStr[Index].isDigit())
  {
    Index++;
  }
  bool Result = (Index > 0) && TryStrToInt64(SizeStr.left(Index), Size);
  if (Result)
  {
    SizeStr = SizeStr.mid(Index).trimmed();
    if (!SizeStr.isEmpty())
    {
      Result = (SizeStr.size() == 1);
      if (Result)
      {
        switch (SizeStr[0].toUpper().unicode())
        {
          case 'G':
            Size *= 1024;
            [[fallthrough]];
          case 'M':
            Size *= 1024;
            [[fallthrough]];
          case 'K':
            Size *= 1024;
            break;
          default:
            Result = false;
        }
      }
    }
  }
  return Result;
}

bool TryRelativeStrToDateTime(QString S, QDateTime & DateTime, bool Add)
{
  S = S.trimmed();
  if (S.compare(QStringLiteral("today"), Qt::CaseInsensitive) == 0)
  {
    S = QStringLiteral("0DS");
  }
  else if (S.compare(QStringLiteral("yesterday"), Qt::CaseInsensitive) == 0)
  {
    S = QStringLiteral("1DS");
  }

  int Index = 0;
  while ((Index < S.size()) && S[Index].isDigit())
  {
    Index++;
  }
  bool Ok = false;
  int Number = S.left(Index).toInt(&Ok);
  if (!Ok)
  {
    return false;
  }
  if (!Add)
  {
    Number = -Number;
  }
  S = S.mid(Index).trimmed().toUpper();
  bool Start = (S.size() == 2) && (S[1] == QLatin1Char('S'));
  if (Start)
  {
    S.chop(1);
  }
  QDateTime Now = QDateTime::currentDateTime();
  // These may not overlap with ParseSize (K, M and G)
  if (S == QStringLiteral("S"))
  {
    DateTime = Now.addSecs(Number);
    if (Start)
    {
      DateTime = DateTime.addMSecs(-DateTime.time().msec());
    }
  }
  else if (S == QStringLiteral("N"))
  {
    DateTime = Now.addSecs(60LL * Number);
    if (Start)
    {
      QTime T = DateTime.time();
      DateTime.setTime(QTime(T.hour(), T.minute()));
    }
  }
  else if (S == QStringLiteral("H"))
  {
    DateTime = Now.addSecs(3600LL * Number);
    if (Start)
    {
      DateTime.setTime(QTime(DateTime.time().hour(), 0));
    }
  }
  else if (S == QStringLiteral("D"))
  {
    DateTime = Now.addDays(Number);
    if (Start)
    {
      DateTime.setTime(QTime(0, 0));
    }
  }
  else if (S == QStringLiteral("Y"))
  {
    DateTime = Now.addYears(Number);
    if (Start)
    {
      DateTime = QDateTime(QDate(DateTime.date().year(), 1, 1), QTime(0, 0));
    }
  }
  else
  {
    return false;
  }
  return true;
}

bool TryStrToDateTimeStandard(const QString & S, QDateTime & Value)
{
  // FormatSettings: DateSeparator '-', TimeSeparator ':', yyyy/mm/dd hh:nn:ss
  static const QRegularExpression Re(
    QStringLiteral("^\\s*(\\d{4})-(\\d{1,2})-(\\d{1,2})(?:\\s+(\\d{1,2}):(\\d{1,2})(?::(\\d{1,2}))?)?\\s*$"));
  QRegularExpressionMatch Match = Re.match(S);
  if (!Match.hasMatch())
  {
    return false;
  }
  QDate Date(Match.captured(1).toInt(), Match.captured(2).toInt(), Match.captured(3).toInt());
  QTime Time(0, 0);
  if (!Match.captured(4).isEmpty())
  {
    Time = QTime(Match.captured(4).toInt(), Match.captured(5).toInt(),
      Match.captured(6).isEmpty() ? 0 : Match.captured(6).toInt());
  }
  if (!Date.isValid() || !Time.isValid())
  {
    return false;
  }
  Value = QDateTime(Date, Time);
  return true;
}

static QString SimpleUnixExcludeTrailingBackslash(const QString & Path)
{
  // "/" stays "/"
  if ((Path.size() > 1) && Path.endsWith(QLatin1Char('/')))
  {
    return Path.left(Path.size() - 1);
  }
  return Path;
}

static bool IsUnixRootPath(const QString & Path)
{
  return Path.isEmpty() || (Path == QStringLiteral("/"));
}

static QString UnixExtractFileName(const QString & Path)
{
  return Path.mid(Path.lastIndexOf(QLatin1Char('/')) + 1);
}

static QString UnixExtractFileDir(const QString & Path)
{
  // UnixExtractFilePath without the trailing slash, root stays "/"
  int P = Path.lastIndexOf(QLatin1Char('/'));
  if (P < 0)
  {
    return QString();
  }
  return SimpleUnixExcludeTrailingBackslash(Path.left(P + 1));
}

// ---------------------------------------------------------------------------
// Masks::TMask

WildcardMask::WildcardMask(const QString & Mask)
{
  int I = 0;
  while (I < Mask.size())
  {
    Element E;
    QChar C = Mask[I];
    if (C == QLatin1Char('*'))
    {
      E.ElementKind = Element::AnyText;
      // collapse "**"
      if (!FElements.empty() && (FElements.back().ElementKind == Element::AnyText))
      {
        I++;
        continue;
      }
    }
    else if (C == QLatin1Char('?'))
    {
      E.ElementKind = Element::AnyChar;
    }
    else if (C == QLatin1Char('['))
    {
      E.ElementKind = Element::Set;
      I++;
      if ((I < Mask.size()) && (Mask[I] == QLatin1Char('!')))
      {
        E.Negate = true;
        I++;
      }
      bool Closed = false;
      while (I < Mask.size())
      {
        QChar From = Mask[I];
        if (From == QLatin1Char(']'))
        {
          Closed = true;
          break;
        }
        QChar To = From;
        if ((I + 2 < Mask.size()) && (Mask[I + 1] == QLatin1Char('-')) && (Mask[I + 2] != QLatin1Char(']')))
        {
          To = Mask[I + 2];
          I += 2;
        }
        E.Ranges.emplace_back(From.toUpper(), To.toUpper());
        I++;
      }
      if (!Closed)
      {
        throw std::invalid_argument("Invalid mask");
      }
    }
    else
    {
      E.Char = C.toUpper();
    }
    FElements.push_back(E);
    I++;
  }
}

bool WildcardMask::MatchesElement(const Element & E, QChar C) const
{
  switch (E.ElementKind)
  {
    case Element::Literal:
      return (C.toUpper() == E.Char);
    case Element::AnyChar:
      return true;
    case Element::Set:
    {
      QChar U = C.toUpper();
      bool In = false;
      for (const auto & [From, To] : E.Ranges)
      {
        if ((U >= From) && (U <= To))
        {
          In = true;
          break;
        }
      }
      return (In != E.Negate);
    }
    default:
      return false;
  }
}

bool WildcardMask::Matches(const QString & Str) const
{
  // Classic wildcard matching with backtracking to the last '*'
  size_t E = 0;
  int S = 0;
  size_t StarE = SIZE_MAX;
  int StarS = 0;
  while (S < Str.size())
  {
    if ((E < FElements.size()) && (FElements[E].ElementKind == Element::AnyText))
    {
      StarE = E++;
      StarS = S;
    }
    else if ((E < FElements.size()) && MatchesElement(FElements[E], Str[S]))
    {
      E++;
      S++;
    }
    else if (StarE != SIZE_MAX)
    {
      E = StarE + 1;
      S = ++StarS;
    }
    else
    {
      return false;
    }
  }
  while ((E < FElements.size()) && (FElements[E].ElementKind == Element::AnyText))
  {
    E++;
  }
  return (E == FElements.size());
}

// ---------------------------------------------------------------------------
// MaskFileName & co.

static QString MaskFilePart(const QString & Part, const QString & Mask, bool & Masked)
{
  QString Result;
  int RestStart = 0;
  bool Delim = false;
  for (int Index = 0; Index < Mask.size(); Index++)
  {
    QChar C = Mask[Index];
    if ((C == QLatin1Char('\\')) && !Delim)
    {
      Delim = true;
      Masked = false;
    }
    else if ((C == QLatin1Char('*')) && !Delim)
    {
      Result += Part.mid(RestStart);
      RestStart = Part.size();
      Masked = true;
    }
    else if ((C == QLatin1Char('?')) && !Delim)
    {
      if (RestStart < Part.size())
      {
        Result += Part[RestStart];
        RestStart++;
      }
      Masked = true;
    }
    else
    {
      Result += C;
      RestStart++;
      Delim = false;
    }
  }
  return Result;
}

QString MaskFileName(QString FileName, const QString & Mask)
{
  if (IsEffectiveFileNameMask(Mask))
  {
    bool Masked = false;
    int P = Mask.lastIndexOf(QLatin1Char('.'));
    if (P >= 0)
    {
      int P2 = FileName.lastIndexOf(QLatin1Char('.'));
      // only dot at beginning of file name is not considered as name/ext separator
      QString FileExt = (P2 > 0) ? FileName.mid(P2 + 1) : QString();
      FileExt = MaskFilePart(FileExt, Mask.mid(P + 1), Masked);
      if (P2 > 0)
      {
        FileName.truncate(P2);
      }
      FileName = MaskFilePart(FileName, Mask.left(P), Masked);
      if (!FileExt.isEmpty())
      {
        FileName += QLatin1Char('.') + FileExt;
      }
    }
    else
    {
      FileName = MaskFilePart(FileName, Mask, Masked);
    }
  }
  return FileName;
}

bool IsFileNameMask(const QString & Mask)
{
  bool Result = Mask.isEmpty(); // empty mask is the same as *
  if (!Result)
  {
    MaskFilePart(QString(), Mask, Result);
  }
  return Result;
}

bool IsEffectiveFileNameMask(const QString & Mask)
{
  return !Mask.isEmpty() && (Mask != QStringLiteral("*")) && (Mask != AnyMask);
}

QString DelimitFileNameMask(QString Mask)
{
  for (int I = 0; I < Mask.size(); I++)
  {
    if (QStringLiteral("\\*?").contains(Mask[I]))
    {
      Mask.insert(I, QLatin1Char('\\'));
      I++;
    }
  }
  return Mask;
}

// ---------------------------------------------------------------------------
// TFileMasks

bool FileMasks::IsMask(const QString & Mask)
{
  return (LastDelimiter(MaskSymbols, Mask) >= 0);
}

QString FileMasks::EscapeMask(const QString & S)
{
  QString Result;
  for (QChar C : S)
  {
    if (MaskSymbols.contains(C))
    {
      Result += QLatin1Char('[') + C + QLatin1Char(']');
    }
    else
    {
      Result += C;
    }
  }
  return Result;
}

QString FileMasks::NormalizeMask(const QString & Mask, const QString & AAnyMask)
{
  return IsEffectiveFileNameMask(Mask) ? Mask : AAnyMask;
}

FileMasks::FileMasks(const QString & AMasks)
{
  SetStr(AMasks, false);
}

void FileMasks::SetMasks(const QString & Value)
{
  if (FStr != Value)
  {
    SetStr(Value, false);
  }
}

void FileMasks::SetMask(const QString & AMask)
{
  SetStr(AMask, true);
}

QStringList FileMasks::MasksStr(bool Directory, bool Include) const
{
  QStringList Result;
  for (const Mask & M : FMasks[MaskIndex(Directory, Include)])
  {
    Result.append(M.UserStr);
  }
  return Result;
}

void FileMasks::ThrowError(int Start, int End) const
{
  // MASK_ERROR
  throw FileMasksException(QStringLiteral("Mask is invalid near '%1'.").arg(FStr.mid(Start, End - Start + 1)),
    Start, End - Start + 1);
}

void FileMasks::SetStr(const QString & Str, bool SingleMask)
{
  QString Backup = FStr;
  MaskList BackupMasks[4];
  for (int I = 0; I < 4; I++)
  {
    BackupMasks[I] = std::move(FMasks[I]);
    FMasks[I].clear();
  }
  try
  {
    FStr = Str;
    int NextMaskFrom = 0;
    bool Include = true;
    while (NextMaskFrom < Str.size())
    {
      int MaskStart = NextMaskFrom;
      QChar NextMaskDelimiter;
      QString MaskStr;
      if (SingleMask)
      {
        MaskStr = Str;
        NextMaskFrom = Str.size() + 1;
      }
      else
      {
        MaskStr = CopyToChars(Str, NextMaskFrom, AllFileMasksDelimiters, &NextMaskDelimiter, true);
      }
      int MaskEnd = NextMaskFrom - 2;

      TrimEx(MaskStr, MaskStart, MaskEnd);

      if (!MaskStr.isEmpty())
      {
        CreateMask(MaskStr, MaskStart, Include);
      }

      if (NextMaskDelimiter == IncludeExcludeFileMasksDelimiter)
      {
        if (Include)
        {
          Include = false;
        }
        else
        {
          ThrowError(NextMaskFrom - 1, Str.size() - 1);
        }
      }
    }
  }
  catch (...)
  {
    FStr = Backup;
    for (int I = 0; I < 4; I++)
    {
      FMasks[I] = std::move(BackupMasks[I]);
    }
    throw;
  }
}

void FileMasks::CreateMaskMask(const QString & AMask, int Start, int End, bool Ex,
  Mask::Kind & MaskKind, std::shared_ptr<WildcardMask> & MaskMask)
{
  try
  {
    if (Ex && !IsEffectiveFileNameMask(AMask))
    {
      MaskKind = Mask::Any;
      MaskMask.reset();
    }
    else
    {
      MaskKind = (Ex && (AMask == QStringLiteral("*."))) ? Mask::NoExt : Mask::Regular;
      MaskMask = std::make_shared<WildcardMask>(AMask);
    }
  }
  catch (const std::invalid_argument &)
  {
    ThrowError(Start, End);
  }
}

void FileMasks::CreateMask(const QString & MaskStr, int MaskStart, bool Include)
{
  bool Directory = false;
  Mask M;
  M.MaskStr = MaskStr;
  M.UserStr = MaskStr;

  QChar NextPartDelimiter;
  int NextPartFrom = 0;
  while (NextPartFrom < MaskStr.size())
  {
    QChar PartDelimiter = NextPartDelimiter;
    int PartFrom = NextPartFrom;
    QString PartStr = CopyToChars(MaskStr, NextPartFrom, QStringLiteral("<>"), &NextPartDelimiter, true);

    int PartStart = MaskStart + PartFrom;
    int PartEnd = MaskStart + NextPartFrom - 2;

    TrimEx(PartStr, PartStart, PartEnd);

    if (!PartDelimiter.isNull())
    {
      bool Low = (PartDelimiter == QLatin1Char('>'));

      Mask::Boundary Boundary;
      if (PartStr.startsWith(QLatin1Char('=')))
      {
        Boundary = Mask::Close;
        PartStr.remove(0, 1);
      }
      else
      {
        Boundary = Mask::Open;
      }

      QDateTime Modification;
      qint64 DummySize;
      if ((!TryStrToInt64(PartStr, DummySize) && TryStrToDateTimeStandard(PartStr, Modification)) ||
          TryRelativeStrToDateTime(PartStr, Modification, false))
      {
        Mask::Boundary & ModificationMask = (Low ? M.LowModificationMask : M.HighModificationMask);
        if ((ModificationMask != Mask::None) || Directory)
        {
          // include delimiter into size part
          ThrowError(PartStart - 1, PartEnd);
        }
        ModificationMask = Boundary;
        (Low ? M.LowModification : M.HighModification) = Modification;
      }
      else
      {
        Mask::Boundary & SizeMask = (Low ? M.LowSizeMask : M.HighSizeMask);
        qint64 & Size = (Low ? M.LowSize : M.HighSize);
        if ((SizeMask != Mask::None) || Directory)
        {
          ThrowError(PartStart - 1, PartEnd);
        }
        SizeMask = Boundary;
        if (!TryStrToSize(PartStr, Size))
        {
          ThrowError(PartStart, PartEnd);
        }
      }
    }
    else if (!PartStr.isEmpty())
    {
      int D = LastDelimiter(DirectoryMaskDelimiters, PartStr);
      Directory = (D >= 0) && (D == PartStr.size() - 1);
      if (Directory)
      {
        do
        {
          PartStr.chop(1);
          M.UserStr.remove(PartStart - MaskStart + D, 1);
          D--;
        }
        while (IsDelimiterAt(DirectoryMaskDelimiters, PartStr, PartStr.size() - 1));
        D = LastDelimiter(DirectoryMaskDelimiters, PartStr);
      }

      if (D >= 0)
      {
        QString DirectoryMaskStr = PartStr.left(D + 1);
        DirectoryMaskStr.replace(QLatin1Char('\\'), QLatin1Char('/'));
        // make sure sole "/" (root dir) is preserved as is
        DirectoryMaskStr = SimpleUnixExcludeTrailingBackslash(DirectoryMaskStr);
        CreateMaskMask(DirectoryMaskStr, PartStart, PartStart + D, false, M.DirectoryMaskKind, M.DirectoryMask);
        CreateMaskMask(PartStr.mid(D + 1), PartStart + D + 1, PartEnd, true, M.FileNameMaskKind, M.FileNameMask);
      }
      else
      {
        CreateMaskMask(PartStr, PartStart, PartEnd, true, M.FileNameMaskKind, M.FileNameMask);
      }
    }
  }

  FMasks[MaskIndex(Directory, Include)].push_back(M);
}

bool FileMasks::MatchesMaskMask(Mask::Kind MaskKind, const WildcardMask * MaskMask, const QString & Str)
{
  if (MaskKind == Mask::Any)
  {
    return true;
  }
  if ((MaskKind == Mask::NoExt) && !Str.contains(QLatin1Char('.')))
  {
    return true;
  }
  return MaskMask->Matches(Str);
}

bool FileMasks::MatchesMasks(const QString & FileName, bool Directory, const QString & Path,
  const Params * AParams, const MaskList & Masks, bool Recurse)
{
  bool Result = false;
  for (auto I = Masks.begin(); !Result && (I != Masks.end()); ++I)
  {
    const Mask & M = *I;
    Result =
      MatchesMaskMask(M.DirectoryMaskKind, M.DirectoryMask.get(), Path) &&
      MatchesMaskMask(M.FileNameMaskKind, M.FileNameMask.get(), FileName);

    if (Result)
    {
      bool HasSize = (AParams != nullptr);
      switch (M.HighSizeMask)
      {
        case Mask::None: Result = true; break;
        case Mask::Open: Result = HasSize && (AParams->Size < M.HighSize); break;
        case Mask::Close: Result = HasSize && (AParams->Size <= M.HighSize); break;
      }
      if (Result)
      {
        switch (M.LowSizeMask)
        {
          case Mask::None: Result = true; break;
          case Mask::Open: Result = HasSize && (AParams->Size > M.LowSize); break;
          case Mask::Close: Result = HasSize && (AParams->Size >= M.LowSize); break;
        }
      }
      bool HasModification = (AParams != nullptr) && AParams->Modification.isValid();
      if (Result)
      {
        switch (M.HighModificationMask)
        {
          case Mask::None: Result = true; break;
          case Mask::Open: Result = HasModification && (AParams->Modification < M.HighModification); break;
          case Mask::Close: Result = HasModification && (AParams->Modification <= M.HighModification); break;
        }
      }
      if (Result)
      {
        switch (M.LowModificationMask)
        {
          case Mask::None: Result = true; break;
          case Mask::Open: Result = HasModification && (AParams->Modification > M.LowModification); break;
          case Mask::Close: Result = HasModification && (AParams->Modification >= M.LowModification); break;
        }
      }
    }
  }

  if (!Result && Directory && !IsUnixRootPath(Path) && Recurse)
  {
    // directory is included, when any of its parents is
    QString ParentFileName = UnixExtractFileName(Path);
    QString ParentPath = UnixExtractFileDir(Path);
    Result = MatchesMasks(ParentFileName, true, ParentPath, AParams, Masks, Recurse);
  }
  return Result;
}

bool FileMasks::DoMatches(const QString & FileName, bool Directory, const QString & Path, const Params * AParams,
  bool RecurseInclude, bool & ImplicitMatch) const
{
  const MaskList & IncludeMasks = FMasks[MaskIndex(Directory, true)];
  const MaskList & ExcludeMasks = FMasks[MaskIndex(Directory, false)];
  bool ImplicitIncludeMatch = (AllDirsAreImplicitlyIncluded && Directory) || IncludeMasks.empty();
  bool ExplicitIncludeMatch = MatchesMasks(FileName, Directory, Path, AParams, IncludeMasks, RecurseInclude);
  bool Result =
    (ImplicitIncludeMatch || ExplicitIncludeMatch) &&
    !MatchesMasks(FileName, Directory, Path, AParams, ExcludeMasks, false);
  ImplicitMatch =
    Result && ImplicitIncludeMatch && !ExplicitIncludeMatch &&
    ((Directory && NoImplicitMatchWithDirExcludeMask) || ExcludeMasks.empty());
  return Result;
}

bool FileMasks::Matches(const QString & FileName, bool Local, bool Directory, const Params * AParams) const
{
  bool ImplicitMatch;
  return Matches(FileName, Local, Directory, AParams, true, ImplicitMatch);
}

bool FileMasks::Matches(const QString & FileName, bool /*Local*/, bool Directory, const Params * AParams,
  bool RecurseInclude, bool & ImplicitMatch) const
{
  return DoMatches(UnixExtractFileName(FileName), Directory, UnixExtractFileDir(FileName), AParams,
    RecurseInclude, ImplicitMatch);
}

bool FileMasks::Matches(const QString & FileName, bool Directory, const QString & Path, const Params * AParams) const
{
  bool ImplicitMatch;
  return DoMatches(FileName, Directory, SimpleUnixExcludeTrailingBackslash(Path), AParams, true, ImplicitMatch);
}

bool FileMasks::MatchesFileName(const QString & FileName, bool Directory, const Params * AParams) const
{
  bool ImplicitMatch;
  return DoMatches(FileName, Directory, QString(), AParams, true, ImplicitMatch);
}
