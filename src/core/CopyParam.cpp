// Port of WinSCP source/core/CopyParam.cpp (TCopyParamType), plus
// TTerminal::UsableCopyParamAttrs (core/Terminal.cpp), TFileBuffer::Convert
// (core/FileBuffer.cpp) and FormatMultiFilesToOneConfirmation (core/RemoteFiles.cpp)
#include "core/CopyParam.h"
#include "core/FileSystem.h"
#include "core/IniStorage.h"
#include "core/RemoteFile.h"
#include "core/Terminal.h"

const QString CopyParam::DefaultAsciiFileMask = QStringLiteral(
  "*.*html; *.htm; *.txt; *.php; *.php3; *.cgi; *.c; *.cpp; *.h; *.pas; "
  "*.bas; *.tex; *.pl; *.js; .htaccess; *.xtml; *.css; *.cfg; *.ini; *.sh; *.xml");

const unsigned long MinSpeed = 8 * 1024;
const unsigned long MaxSpeed = 8 * 1024 * 1024;

static void AddToList(QString & List, const QString & Value, const QString & Delimiter)
{
  if (!Value.isEmpty())
  {
    if (!List.isEmpty() && !List.endsWith(Delimiter))
    {
      List += Delimiter;
    }
    List += Value;
  }
}

static QString StripHotkey(QString Text)
{
  return Text.remove(QLatin1Char('&'));
}

QString RightsToText(unsigned int Mode)
{
  return Rights::ToText(Mode);
}

bool RightsFromText(const QString & Text, unsigned int & Mode)
{
  if (Text.size() != 9)
  {
    return false;
  }
  const char * Letters = "rwxrwxrwx";
  unsigned int Result = 0;
  for (int I = 0; I < 9; I++)
  {
    QChar C = Text[I];
    unsigned int Bit = 1u << (8 - I);
    bool Exec = ((I % 3) == 2);
    if (C == QLatin1Char(Letters[I]))
    {
      Result |= Bit;
    }
    else if (Exec && ((C == QLatin1Char('s')) || (C == QLatin1Char('t'))))
    {
      Result |= Bit | ((I == 2) ? 04000 : (I == 5) ? 02000 : 01000);
    }
    else if (Exec && ((C == QLatin1Char('S')) || (C == QLatin1Char('T'))))
    {
      Result |= ((I == 2) ? 04000 : (I == 5) ? 02000 : 01000);
    }
    else if ((C != QLatin1Char('-')) && (C != QLatin1Char('?')))  // '?' undefined (keep)
    {
      return false;
    }
  }
  Mode = Result;
  return true;
}

unsigned int AddExecute(unsigned int Mode)
{
  return Mode | ((Mode & 0400) ? 0100 : 0) | ((Mode & 0040) ? 0010 : 0) | ((Mode & 0004) ? 0001 : 0);
}

bool TryGetSpeedLimit(const QString & Text, unsigned long & Speed)
{
  // SPEED_UNLIMITED
  if (Text.trimmed().compare(QStringLiteral("Unlimited"), Qt::CaseInsensitive) == 0)
  {
    Speed = 0;
    return true;
  }
  bool Ok = false;
  int SSpeed = Text.trimmed().toInt(&Ok);
  if (Ok && (SSpeed >= 0))
  {
    Speed = static_cast<unsigned long>(SSpeed) * 1024;
    return true;
  }
  return false;
}

unsigned long GetSpeedLimit(const QString & Text)
{
  unsigned long Speed = 0;
  if (!TryGetSpeedLimit(Text, Speed))
  {
    throw ExtException(QStringLiteral("'%1' is not a valid speed limit.").arg(Text));
  }
  return Speed;
}

QString SetSpeedLimit(unsigned long Limit)
{
  return (Limit == 0) ? QStringLiteral("Unlimited") : QString::number(Limit / 1024);
}

int CopyParam::UsableAttrs(bool ToRemote)
{
  // TTerminal::UsableCopyParamAttrs for SFTP: everything is capable except
  // file encryption. Text mode stays available even though local and remote
  // EOL are both LF (BOM/EOF removal still makes sense).
  int General = cpaNoEncryptNewFiles;
  if (ToRemote)
  {
    return General | cpaNoPreserveReadOnly;
  }
  return General | cpaNoClearArchive | cpaNoIgnorePermErrors | cpaNoRights | cpaNoRemoveCtrlZ | cpaNoRemoveBOM;
}

int CopyParam::UsableAttrs(bool ToRemote, const std::function<bool(int Capability)> & IsCapable, bool Move)
{
  // TTerminal::UsableCopyParamAttrs
  auto Not = [&](FSCapability Capability, int Flag) { return IsCapable(Capability) ? 0 : Flag; };
  int General =
    Not(fcTextMode, cpaNoTransferMode) |
    Not(fcModeChanging, cpaNoRights) |
    Not(fcModeChanging, cpaNoPreserveReadOnly) |
    (Move ? cpaNoClearArchive : 0) |
    Not(fcIgnorePermErrors, cpaNoIgnorePermErrors) |
    // the following three are never supported for download, so when they are
    // not supported for upload too, set them in General flags, so that they do
    // not get enabled on Synchronize dialog
    Not(fcModeChangingUpload, cpaNoRights) |
    Not(fcRemoveCtrlZUpload, cpaNoRemoveCtrlZ) |
    Not(fcRemoveBOMUpload, cpaNoRemoveBOM) |
    Not(fcPreservingTimestampDirs, cpaNoPreserveTimeDirs) |
    Not(fcResumeSupport, cpaNoResumeSupport) |
    // IsEncryptingFiles: NixSCP sites have no file encryption key
    cpaNoEncryptNewFiles;
  if (ToRemote)
  {
    return General | cpaNoPreserveReadOnly | Not(fcPreservingTimestampUpload, cpaNoPreserveTime);
    // (fcNewerOnlyUpload is not required: Terminal compares the timestamps itself)
  }
  return General | cpaNoClearArchive | cpaNoIgnorePermErrors | cpaNoRights | cpaNoRemoveCtrlZ | cpaNoRemoveBOM |
    cpaNoEncryptNewFiles;
}

int CopyParam::UsableAttrs(bool ToRemote, const Terminal * ATerminal, bool Move)
{
  if (ATerminal == nullptr)
  {
    return UsableAttrs(ToRemote, [](int Capability)
      { return Terminal::ProtocolCapability(fsSFTP, static_cast<FSCapability>(Capability)); }, Move);
  }
  return UsableAttrs(ToRemote, [ATerminal](int Capability)
    { return ATerminal->IsCapable(static_cast<FSCapability>(Capability)); }, Move);
}

QString CopyParam::GetInfoStr(const QString & Separator, int Attrs) const
{
  QString Result;
  bool SomeAttrIncluded;
  DoGetInfoStr(Separator, Attrs, Result, SomeAttrIncluded);
  return Result;
}

bool CopyParam::AnyUsableCopyParam(int Attrs) const
{
  QString Result;
  bool SomeAttrIncluded;
  DoGetInfoStr(QStringLiteral(";"), Attrs, Result, SomeAttrIncluded);
  return SomeAttrIncluded;
}

void CopyParam::DoGetInfoStr(const QString & Separator, int Options, QString & Result, bool & SomeAttrIncluded) const
{
  CopyParam Defaults;
  bool SomeAttrExcluded = false;
  SomeAttrIncluded = false;
  auto Add = [&](const QString & Str, int Except) -> bool
  {
    if ((Options & Except) == 0)
    {
      AddToList(Result, Str, Separator);
      SomeAttrIncluded = true;
      return true;
    }
    SomeAttrExcluded = true;
    return false;
  };

  bool AsciiFileMaskDiffers = (Mode == tmAutomatic) && !(AsciiFileMask == Defaults.AsciiFileMask);
  bool TransferModeDiffers = ((Mode != Defaults.Mode) || AsciiFileMaskDiffers);

  if ((Options & (cpaIncludeMaskOnly | cpaNoTransferMode)) == 0)
  {
    // Adding Transfer type unconditionally
    // COPY_INFO_TRANSFER_TYPE2 "Transfer type: %s|Binary|Text|Automatic (%s)|Automatic"
    QString ModeStr;
    switch (Mode)
    {
      case tmBinary: ModeStr = QStringLiteral("Binary"); break;
      case tmAscii: ModeStr = QStringLiteral("Text"); break;
      case tmAutomatic:
      default:
        ModeStr = !(AsciiFileMask == Defaults.AsciiFileMask) ?
          QStringLiteral("Automatic (%1)").arg(AsciiFileMask.Masks()) : QStringLiteral("Automatic");
        break;
    }
    AddToList(Result, QStringLiteral("Transfer type: %1").arg(ModeStr), Separator);
    if (TransferModeDiffers)
    {
      Add(QString(), cpaIncludeMaskOnly | cpaNoTransferMode);
    }
  }
  else if (TransferModeDiffers)
  {
    SomeAttrExcluded = true;
  }

  if (FileNameCase != Defaults.FileNameCase)
  {
    // COPY_INFO_FILENAME
    static const char * const CaseNames[] = { "No change", "Upper case", "Lower case", "First upper case", "Lower case 8.3" };
    Add(QStringLiteral("Filename modification: %1").arg(QLatin1String(CaseNames[FileNameCase])), cpaIncludeMaskOnly);
  }

  if ((PreserveRights != Defaults.PreserveRights) ||
      (PreserveRights && ((Rights != Defaults.Rights) || (AddXToDirectories != Defaults.AddXToDirectories))))
  {
    if (PreserveRights)
    {
      QString RightsStr = RightsToText(Rights);
      if (AddXToDirectories)
      {
        RightsStr += QStringLiteral(", Add X to directories");
      }
      Add(QStringLiteral("Set permissions: %1").arg(RightsStr), cpaIncludeMaskOnly | cpaNoRights);
    }
  }

  bool APreserveTimeDirs = PreserveTime && PreserveTimeDirs;
  if ((PreserveTime != Defaults.PreserveTime) || (APreserveTimeDirs != Defaults.PreserveTimeDirs))
  {
    bool AddPreserveTime = false;
    QString Str = PreserveTime ? QStringLiteral("Preserve timestamp") : QStringLiteral("Do not preserve timestamp");

    const int ExceptDirs = cpaNoPreserveTimeDirs;
    if (APreserveTimeDirs != Defaults.PreserveTimeDirs)
    {
      if ((Options & ExceptDirs) == 0)
      {
        Str = QStringLiteral("%1 (including directories)").arg(Str);
        AddPreserveTime = true;
      }
      Add(QString(), ExceptDirs);
    }

    const int Except = cpaIncludeMaskOnly | cpaNoPreserveTime;
    if (PreserveTime != Defaults.PreserveTime)
    {
      if ((Options & Except) == 0)
      {
        AddPreserveTime = true;
      }
      Add(QString(), Except);
    }

    if (AddPreserveTime)
    {
      AddToList(Result, Str, Separator);
    }
  }

  if ((PreserveRights || PreserveTime) && (IgnorePermErrors != Defaults.IgnorePermErrors) && IgnorePermErrors)
  {
    Add(QStringLiteral("Ignore permission errors"), cpaIncludeMaskOnly | cpaNoIgnorePermErrors);
  }

  if ((PreserveReadOnly != Defaults.PreserveReadOnly) && PreserveReadOnly)
  {
    Add(QStringLiteral("Preserve read-only"), cpaIncludeMaskOnly | cpaNoPreserveReadOnly);
  }

  if ((CalculateSize != Defaults.CalculateSize) && !CalculateSize)
  {
    Add(QStringLiteral("Do not calculate transfer size"), cpaIncludeMaskOnly | cpaNoCalculateSize);
  }

  if ((Mode == tmAscii) || (Mode == tmAutomatic))
  {
    if ((RemoveBOM != Defaults.RemoveBOM) && RemoveBOM)
    {
      Add(QStringLiteral("Remove BOM"), cpaIncludeMaskOnly | cpaNoRemoveBOM | cpaNoTransferMode);
    }
    if ((RemoveCtrlZ != Defaults.RemoveCtrlZ) && RemoveCtrlZ)
    {
      Add(QStringLiteral("Remove EOF mark"), cpaIncludeMaskOnly | cpaNoRemoveCtrlZ | cpaNoTransferMode);
    }
  }

  if (!(IncludeFileMask == Defaults.IncludeFileMask))
  {
    Add(QStringLiteral("File mask: %1").arg(IncludeFileMask.Masks()), cpaNoIncludeMask);
  }

  if (CPSLimit > 0)
  {
    Add(QStringLiteral("Transfer speed limit: %1 KB/s").arg(CPSLimit / 1024), cpaIncludeMaskOnly);
  }

  if ((NewerOnly != Defaults.NewerOnly) && NewerOnly)
  {
    Add(StripHotkey(QStringLiteral("&New and updated files only")), cpaIncludeMaskOnly | cpaNoNewerOnly);
  }

  if ((ExcludeHiddenFiles != Defaults.ExcludeHiddenFiles) && ExcludeHiddenFiles)
  {
    Add(QStringLiteral("Exclude hidden files"), cpaNoIncludeMask);
  }

  if ((ExcludeEmptyDirectories != Defaults.ExcludeEmptyDirectories) && ExcludeEmptyDirectories)
  {
    Add(QStringLiteral("Exclude empty directories"), 0);
  }

  // Resume support changes appear in script arguments only (not in the info string)

  if (SomeAttrExcluded)
  {
    // COPY_INFO_NOT_USABLE
    Result += (Result.isEmpty() ? QString() : Separator) +
      QStringLiteral("%1 configured transfer settings cannot be used in current context")
        .arg(SomeAttrIncluded ? QStringLiteral("Some") : QStringLiteral("All"));
  }
  else if (Result.isEmpty())
  {
    Result = QStringLiteral("Default transfer settings");
  }
}

bool CopyParam::UseAsciiTransfer(const QString & FileName, bool Local, const FileMasks::Params & Params) const
{
  switch (Mode)
  {
    case tmBinary: return false;
    case tmAscii: return true;
    case tmAutomatic: return AsciiFileMask.Matches(FileName, Local, false, &Params);
  }
  return false;
}

bool CopyParam::AllowResume(qint64 Size, const QString & FileName) const
{
  if (FileName.size() + PartialExt.size() > 255) // it's a different limit than MAX_PATH
  {
    return false;
  }
  switch (ResumeSupport)
  {
    case rsOn: return true;
    case rsOff: return false;
    case rsSmart: return (Size >= ResumeThreshold);
  }
  return false;
}

bool CopyParam::AllowAnyTransfer() const
{
  return IncludeFileMask.Masks().isEmpty() && !ExcludeHiddenFiles && !ExcludeEmptyDirectories;
}

bool CopyParam::AllowTransfer(const QString & FileName, bool Local, bool Directory, const FileMasks::Params & Params, bool Hidden) const
{
  if (Hidden && ExcludeHiddenFiles)
  {
    return false;
  }
  if (!IncludeFileMask.Masks().isEmpty())
  {
    return IncludeFileMask.Matches(FileName, Local, Directory, &Params);
  }
  return true;
}

QString CopyParam::ChangeFileName(const QString & FileName, bool FirstLevel) const
{
  if (FirstLevel)
  {
    return ChangeFileName(MaskFileName(FileName, FileMask));
  }
  return ChangeFileName(FileName);
}

QString CopyParam::ChangeFileName(const QString & AFileName) const
{
  QString FileName = AFileName;
  if (TrimVMSVersions)
  {
    // TTerminal::GetBaseFileName
    qsizetype P = FileName.lastIndexOf(QLatin1Char(';'));
    if (P >= 0)
    {
      FileName.truncate(P);
    }
  }
  switch (FileNameCase)
  {
    case ncUpperCase: return FileName.toUpper();
    case ncLowerCase: return FileName.toLower();
    case ncFirstUpperCase: return FileName.left(1).toUpper() + FileName.mid(1).toLower();
    case ncLowerCaseShort:
      if ((FileName.size() <= 12) && (FileName.indexOf(QLatin1Char('.')) < 8 + 1) && (FileName == FileName.toUpper()))
      {
        return FileName.toLower();
      }
      return FileName;
    case ncNoChange:
    default:
      return FileName;
  }
}

// The rules of Windows, compiled on every platform: tested on Linux too.
static constexpr QChar TokenPrefix = QLatin1Char('%');

// LocalInvalidChars ("/\\:*?\"<>|", plus the control characters) + TokenPrefix
static bool IsTokenizible(QChar Char)
{
  return (Char.unicode() < 0x20) || QStringView(u"/\\:*?\"<>|%").contains(Char);
}

// IsReservedName: a device, also with an extension, and with spaces after the
// name, which Windows drops before it looks for a device ("NUL .txt" is NUL).
// CONIN$ and CONOUT$ are consoles; the superscript digits count as digits.
static bool IsReservedName(const QString & FileName)
{
  qsizetype Dot = FileName.indexOf(QLatin1Char('.'));
  QString Base = ((Dot >= 0) ? FileName.left(Dot) : FileName).toUpper();
  while (Base.endsWith(QLatin1Char(' ')))
  {
    Base.chop(1);
  }
  static const QStringList Reserved = {
    QStringLiteral("CON"), QStringLiteral("PRN"), QStringLiteral("AUX"), QStringLiteral("NUL"),
    QStringLiteral("CONIN$"), QStringLiteral("CONOUT$")};
  if (Reserved.contains(Base))
  {
    return true;
  }
  static const QString Digits = QStringLiteral("0123456789\u00B9\u00B2\u00B3");
  return (Base.size() == 4) && (Base.startsWith(QStringLiteral("COM")) || Base.startsWith(QStringLiteral("LPT"))) &&
    Digits.contains(Base.at(3));
}

static QString CharToken(QChar Char)
{
  return TokenPrefix + QStringLiteral("%1").arg(static_cast<uint>(Char.unicode()), 2, 16, QLatin1Char('0')).toUpper();
}

// The character of a %XX token at Index (TokenPrefix position); null if none
static QChar TokenChar(const QString & FileName, qsizetype Index)
{
  if (Index + 2 >= FileName.size())
  {
    return QChar();
  }
  bool Ok = false;
  uint Code = FileName.mid(Index + 1, 2).toUInt(&Ok, 16);
  return Ok ? QChar(static_cast<char16_t>(Code)) : QChar();
}

QString CopyParam::WindowsLocalFileName(const QString & FileName)
{
  QString Result;
  Result.reserve(FileName.size());
  for (qsizetype Index = 0; Index < FileName.size(); Index++)
  {
    QChar Char = FileName.at(Index);
    // a "%" stays, unless it would read as a token
    QChar Token = (Char == TokenPrefix) ? TokenChar(FileName, Index) : QChar();
    if ((Char == TokenPrefix) && (Token.isNull() || !IsTokenizible(Token)))
    {
      Result += Char;
    }
    else if (IsTokenizible(Char))
    {
      Result += CharToken(Char);
    }
    else
    {
      Result += Char;
    }
  }
  // Windows trims a trailing space or dot
  if (Result.endsWith(QLatin1Char(' ')) || Result.endsWith(QLatin1Char('.')))
  {
    Result = Result.left(Result.size() - 1) + CharToken(Result.back());
  }
  if (IsReservedName(Result))
  {
    qsizetype Dot = Result.indexOf(QLatin1Char('.'));
    Result.insert((Dot >= 0) ? Dot : Result.size(), QStringLiteral("%00"));
  }
  return Result;
}

QString CopyParam::WindowsRestoreChars(const QString & FileName)
{
  QString Result = FileName;
  qsizetype Index = 0;
  while ((Index = Result.indexOf(TokenPrefix, Index)) >= 0)
  {
    QChar Char = TokenChar(Result, Index);
    bool Last = (Index == Result.size() - 3);
    if (!Char.isNull() && (Char != QChar(u'\0')) &&
        (IsTokenizible(Char) || (((Char == QLatin1Char(' ')) || (Char == QLatin1Char('.'))) && Last)))
    {
      Result.replace(Index, 3, Char);
      Index++;
    }
    else if ((Result.mid(Index + 1, 2) == QStringLiteral("00")) &&
      (Last || ((Index + 3 < Result.size()) && (Result.at(Index + 3) == QLatin1Char('.')))) &&
      IsReservedName(Result.left(Index) + Result.mid(Index + 3)))
    {
      Result.remove(Index, 3);
    }
    else
    {
      Index++;
    }
  }
  return Result;
}

QString CopyParam::ValidLocalFileName(const QString & FileName)
{
#ifdef Q_OS_WIN
  return WindowsLocalFileName(FileName);
#else
  return FileName;
#endif
}

QString CopyParam::RestoreChars(const QString & FileName)
{
#ifdef Q_OS_WIN
  return WindowsRestoreChars(FileName);
#else
  return FileName;
#endif
}

unsigned int CopyParam::RemoteFileRights(bool Directory) const
{
  return (Directory && AddXToDirectories) ? AddExecute(Rights) : Rights;
}

CopyParam CopyParam::TemporaryFileCopyParam(bool ForceText) const
{
  CopyParam Result = *this;
  Result.FileNameCase = ncNoChange;
  Result.PreserveRights = false;
  Result.IncludeFileMask = FileMasks();
  Result.NewerOnly = false;
  if (ForceText)
  {
    Result.Mode = tmAscii;
  }
  return Result;
}

void CopyParam::Load(const IniStorage & Storage)
{
  AddXToDirectories = Storage.ReadBool(QStringLiteral("AddXToDirectories"), AddXToDirectories);
  try
  {
    AsciiFileMask = Storage.ReadString(QStringLiteral("Masks"), AsciiFileMask.Masks());
  }
  catch (const FileMasksException &)
  {
    // keep previous (valid) masks
  }
  int Case = Storage.ReadInteger(QStringLiteral("FileNameCase"), FileNameCase);
  if ((Case >= ncNoChange) && (Case <= ncLowerCaseShort))
  {
    FileNameCase = static_cast<FileNameCaseType>(Case);
  }
  PreserveReadOnly = Storage.ReadBool(QStringLiteral("PreserveReadOnly"), PreserveReadOnly);
  PreserveTime = Storage.ReadBool(QStringLiteral("PreserveTime"), PreserveTime);
  PreserveTimeDirs = Storage.ReadBool(QStringLiteral("PreserveTimeDirs"), PreserveTimeDirs);
  PreserveRights = Storage.ReadBool(QStringLiteral("PreserveRights"), PreserveRights);
  IgnorePermErrors = Storage.ReadBool(QStringLiteral("IgnorePermErrors"), IgnorePermErrors);
  RightsFromText(Storage.ReadString(QStringLiteral("Text"), RightsToText(Rights)), Rights);
  int AMode = Storage.ReadInteger(QStringLiteral("TransferMode"), Mode);
  if ((AMode >= tmBinary) && (AMode <= tmAutomatic))
  {
    Mode = static_cast<TransferMode>(AMode);
  }
  int Resume = Storage.ReadInteger(QStringLiteral("ResumeSupport"), ResumeSupport);
  if ((Resume >= rsOn) && (Resume <= rsOff))
  {
    ResumeSupport = static_cast<ResumeSupportType>(Resume);
  }
  bool Ok = false;
  qint64 Threshold = Storage.ReadStringRaw(QStringLiteral("ResumeThreshold"), QString()).toLongLong(&Ok);
  if (Ok)
  {
    ResumeThreshold = Threshold;
  }
  CalculateSize = Storage.ReadBool(QStringLiteral("CalculateSize"), CalculateSize);
  try
  {
    if (Storage.ValueExists(QStringLiteral("IncludeFileMask")))
    {
      IncludeFileMask = Storage.ReadString(QStringLiteral("IncludeFileMask"), IncludeFileMask.Masks());
    }
    else if (Storage.ValueExists(QStringLiteral("ExcludeFileMask")))
    {
      QString ExcludeFileMask = Storage.ReadString(QStringLiteral("ExcludeFileMask"), QString());
      if (!ExcludeFileMask.isEmpty())
      {
        bool NegativeExclude = Storage.ReadBool(QStringLiteral("NegativeExclude"), false);
        if (NegativeExclude)
        {
          IncludeFileMask = ExcludeFileMask;
        }
        // convert at least simple cases to new format
        else if (!ExcludeFileMask.contains(FileMasks::IncludeExcludeFileMasksDelimiter))
        {
          IncludeFileMask = QString(FileMasks::IncludeExcludeFileMasksDelimiter) + ExcludeFileMask;
        }
      }
    }
  }
  catch (const FileMasksException &)
  {
  }
  RemoveCtrlZ = Storage.ReadBool(QStringLiteral("RemoveCtrlZ"), RemoveCtrlZ);
  RemoveBOM = Storage.ReadBool(QStringLiteral("RemoveBOM"), RemoveBOM);
  int Cps = Storage.ReadInteger(QStringLiteral("CPSLimit"), static_cast<int>(CPSLimit));
  CPSLimit = (Cps > 0) ? static_cast<unsigned long>(Cps) : 0;
  NewerOnly = Storage.ReadBool(QStringLiteral("NewerOnly"), NewerOnly);
  ExcludeHiddenFiles = Storage.ReadBool(QStringLiteral("ExcludeHiddenFiles"), ExcludeHiddenFiles);
  ExcludeEmptyDirectories = Storage.ReadBool(QStringLiteral("ExcludeEmptyDirectories"), ExcludeEmptyDirectories);
  // TGUICopyParamType
  Queue = Storage.ReadBool(QStringLiteral("Queue"), Queue);
  QueueNoConfirmation = Storage.ReadBool(QStringLiteral("QueueNoConfirmation"), QueueNoConfirmation);
  QueueParallel = Storage.ReadBool(QStringLiteral("QueueParallel"), QueueParallel);
}

void CopyParam::Save(IniStorage & Storage) const
{
  Storage.WriteBool(QStringLiteral("AddXToDirectories"), AddXToDirectories);
  Storage.WriteString(QStringLiteral("Masks"), AsciiFileMask.Masks());
  Storage.WriteInteger(QStringLiteral("FileNameCase"), FileNameCase);
  Storage.WriteBool(QStringLiteral("PreserveReadOnly"), PreserveReadOnly);
  Storage.WriteBool(QStringLiteral("PreserveTime"), PreserveTime);
  Storage.WriteBool(QStringLiteral("PreserveTimeDirs"), PreserveTimeDirs);
  Storage.WriteBool(QStringLiteral("PreserveRights"), PreserveRights);
  Storage.WriteBool(QStringLiteral("IgnorePermErrors"), IgnorePermErrors);
  Storage.WriteString(QStringLiteral("Text"), RightsToText(Rights));
  Storage.WriteInteger(QStringLiteral("TransferMode"), Mode);
  Storage.WriteInteger(QStringLiteral("ResumeSupport"), ResumeSupport);
  Storage.WriteStringRaw(QStringLiteral("ResumeThreshold"), QString::number(ResumeThreshold));
  Storage.WriteBool(QStringLiteral("CalculateSize"), CalculateSize);
  Storage.WriteString(QStringLiteral("IncludeFileMask"), IncludeFileMask.Masks());
  Storage.DeleteValue(QStringLiteral("ExcludeFileMask")); // obsolete
  Storage.DeleteValue(QStringLiteral("NegativeExclude")); // obsolete
  Storage.WriteBool(QStringLiteral("RemoveCtrlZ"), RemoveCtrlZ);
  Storage.WriteBool(QStringLiteral("RemoveBOM"), RemoveBOM);
  Storage.WriteInteger(QStringLiteral("CPSLimit"), static_cast<int>(CPSLimit));
  Storage.WriteBool(QStringLiteral("NewerOnly"), NewerOnly);
  Storage.WriteBool(QStringLiteral("ExcludeHiddenFiles"), ExcludeHiddenFiles);
  Storage.WriteBool(QStringLiteral("ExcludeEmptyDirectories"), ExcludeEmptyDirectories);
  Storage.WriteBool(QStringLiteral("Queue"), Queue);
  Storage.WriteBool(QStringLiteral("QueueNoConfirmation"), QueueNoConfirmation);
  Storage.WriteBool(QStringLiteral("QueueParallel"), QueueParallel);
}

bool CopyParam::operator==(const CopyParam & O) const
{
  return
    (Mode == O.Mode) && (AsciiFileMask == O.AsciiFileMask) && (FileNameCase == O.FileNameCase) &&
    (PreserveReadOnly == O.PreserveReadOnly) &&
    (PreserveTime == O.PreserveTime) && (PreserveTimeDirs == O.PreserveTimeDirs) &&
    (PreserveRights == O.PreserveRights) && (Rights == O.Rights) && (AddXToDirectories == O.AddXToDirectories) &&
    (IgnorePermErrors == O.IgnorePermErrors) && (ResumeSupport == O.ResumeSupport) &&
    (ResumeThreshold == O.ResumeThreshold) && (CalculateSize == O.CalculateSize) &&
    (IncludeFileMask == O.IncludeFileMask) && (RemoveCtrlZ == O.RemoveCtrlZ) && (RemoveBOM == O.RemoveBOM) &&
    (CPSLimit == O.CPSLimit) && (NewerOnly == O.NewerOnly) && (ExcludeHiddenFiles == O.ExcludeHiddenFiles) &&
    (ExcludeEmptyDirectories == O.ExcludeEmptyDirectories);
}

QString FormatMultiFilesToOneConfirmation(const QString & Target)
{
  // MULTI_FILES_TO_ONE
  QString Dir = UnixPath::ExtractFileDir(Target);
  QString Name = UnixPath::ExtractFileName(Target);
  QString Path = Target.endsWith(QLatin1Char('/')) ? Target : (Target + QLatin1Char('/'));
  return QStringLiteral(
    "Are you sure you want to transfer multiple files to a single file '%1' in a directory '%2'?\n\n"
    "The files will overwrite one another.\n\n"
    "If you actually want to transfer all files to a directory '%3', keeping their name, "
    "make sure you terminate the path with a slash.").arg(Name, Dir, Path);
}

unsigned int PreserveReadOnlyMode(unsigned int LocalMode, unsigned int RemoteMode, bool PreserveReadOnly)
{
  // TTerminal::SinkFile: faReadOnly when the remote file is not writable by its owner
  if (PreserveReadOnly && ((RemoteMode & 0200) == 0))
  {
    return LocalMode & ~0222u;
  }
  return LocalMode;
}

QByteArray EOLToStr(int EOLType)
{
  switch (EOLType)
  {
    case 1: return QByteArrayLiteral("\r\n");
    case 2: return QByteArrayLiteral("\r");
    case 0:
    default: return QByteArrayLiteral("\n");
  }
}

TextEOLConverter::TextEOLConverter(const QByteArray & Source, const QByteArray & Dest) :
  FSource(Source.isEmpty() ? LocalEOL : Source.left(2)),
  FDest(Dest.isEmpty() ? LocalEOL : Dest.left(2))
{
}

void TextEOLConverter::Emit(QByteArray & Out, char C)
{
  // standalone byte (not part of a recognized sequence)
  if ((FSource.size() == 1) && (C == FSource[0]))
  {
    Out += FDest;
  }
  else
  {
    Out += C;
  }
}

QByteArray TextEOLConverter::Convert(const QByteArray & Chunk)
{
  if (Identity())
  {
    return Chunk;
  }
  QByteArray Out;
  Out.reserve(Chunk.size() + Chunk.size() / 8 + 2);
  const char * P = Chunk.constData();
  const qsizetype Size = Chunk.size();
  qsizetype Index = 0;
  if (FPending && (Size > 0))
  {
    FPending = false;
    if (FSource.size() == 2)
    {
      // last chunk ended with the first byte of the source EOL
      if (P[0] == FSource[1])
      {
        Out += FDest;
        Index = 1;
      }
      else
      {
        Out += FPendingChar;
      }
    }
    else
    {
      // last chunk ended with the first byte of the two byte destination EOL
      if (P[0] == FDest[1])
      {
        Out += FDest;   // already in destination format
        Index = 1;
      }
      else
      {
        Emit(Out, FPendingChar);
      }
    }
  }
  if (FSource.size() == 2)
  {
    const char S0 = FSource[0];
    const char S1 = FSource[1];
    for (; Index < Size; Index++)
    {
      char C = P[Index];
      if (C == S0)
      {
        if (Index + 1 < Size)
        {
          if (P[Index + 1] == S1)
          {
            Out += FDest;
            Index++;
            continue;
          }
        }
        else
        {
          FPending = true;
          FPendingChar = C;
          break;
        }
      }
      Out += C;
    }
  }
  else
  {
    const char S0 = FSource[0];
    const bool TwoByteDest = (FDest.size() == 2);
    const char D0 = FDest[0];
    const char D1 = TwoByteDest ? FDest[1] : 0;
    for (; Index < Size; Index++)
    {
      char C = P[Index];
      if (TwoByteDest && (C == D0))
      {
        if (Index + 1 < Size)
        {
          if (P[Index + 1] == D1)
          {
            // EOL already in destination format, pass it unmodified
            Out += FDest;
            Index++;
            continue;
          }
        }
        else
        {
          FPending = true;
          FPendingChar = C;
          break;
        }
      }
      if (C == S0)
      {
        Out += FDest;
      }
      else
      {
        Out += C;
      }
    }
  }
  return Out;
}

QByteArray TextEOLConverter::Finish()
{
  QByteArray Out;
  if (FPending)
  {
    FPending = false;
    if (FSource.size() == 2)
    {
      Out += FPendingChar;
    }
    else
    {
      Emit(Out, FPendingChar);
    }
  }
  return Out;
}

QDateTime ReduceDateTimePrecision(const QDateTime & DateTime, TModificationFmt Precision)
{
  if (!DateTime.isValid())
  {
    return DateTime;
  }
  QDateTime Result = DateTime;
  switch (Precision)
  {
    case mfNone:
      Result = QDateTime();
      break;
    case mfMDY:
      Result.setTime(QTime(0, 0));
      break;
    case mfMDHM:
    case mfYMDHM:
    {
      QTime T = Result.time();
      Result.setTime(QTime(T.hour(), T.minute()));
      break;
    }
    case mfFull:
    default:
    {
      // whole seconds
      QTime T = Result.time();
      Result.setTime(QTime(T.hour(), T.minute(), T.second()));
      break;
    }
  }
  return Result;
}

TModificationFmt LessDateTimePrecision(TModificationFmt Precision1, TModificationFmt Precision2)
{
  // mfMDHM and mfYMDHM are the same precision for comparison
  auto Rank = [](TModificationFmt P)
  {
    switch (P)
    {
      case mfNone: return 0;
      case mfMDY: return 1;
      case mfMDHM:
      case mfYMDHM: return 2;
      case mfFull:
      default: return 3;
    }
  };
  return (Rank(Precision1) < Rank(Precision2)) ? Precision1 : Precision2;
}

bool IsNewerModification(const QDateTime & Source, TModificationFmt SourcePrecision,
  const QDateTime & Dest, TModificationFmt DestPrecision)
{
  TModificationFmt Precision = LessDateTimePrecision(SourcePrecision, DestPrecision);
  if (Precision == mfNone)
  {
    // nothing to compare: transfer
    return true;
  }
  QDateTime S = ReduceDateTimePrecision(Source, Precision);
  QDateTime D = ReduceDateTimePrecision(Dest, Precision);
  if (!S.isValid() || !D.isValid())
  {
    return true;
  }
  return S > D;
}
