// Port of WinSCP source/core/HierarchicalStorage.cpp (INI part)
#include "core/IniStorage.h"
#include "core/ConfigVault.h"

#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

static const QByteArray Bom("\xEF\xBB\xBF");

// PuTTY windows/utils/escape_registry_key.c
static QString PuttyEscape(const QByteArray & In, bool Value)
{
  static const char Hex[] = "0123456789ABCDEF";
  QString Out;
  bool CanDot = false;
  for (char C : In)
  {
    if ((C == ' ') || (C == '\\') || (C == '*') || (C == '?') ||
        (C == '%') || (C < ' ') || (C > '~') || ((C == '.') && !CanDot))
    {
      Out += QLatin1Char('%');
      Out += QLatin1Char(Hex[static_cast<unsigned char>(C) >> 4]);
      Out += QLatin1Char(Hex[static_cast<unsigned char>(C) & 15]);
    }
    else
    {
      Out += QLatin1Char(C);
    }
    CanDot = true;
  }
  if (Value)
  {
    // We do not want to munge * in PasswordMask
    Out.replace(QStringLiteral("%2A"), QStringLiteral("*"));
  }
  return Out;
}

static QByteArray PuttyUnescape(const QString & Str)
{
  QByteArray In = Str.toLatin1();
  QByteArray Out;
  for (int I = 0; I < In.size(); )
  {
    if ((In[I] == '%') && (I + 2 < In.size()))
    {
      int A = In[I + 1] - '0';
      A -= (A > 9 ? 7 : 0);
      int B = In[I + 2] - '0';
      B -= (B > 9 ? 7 : 0);
      Out += static_cast<char>((A << 4) + B);
      I += 3;
    }
    else
    {
      Out += In[I++];
    }
  }
  return Out;
}

static QByteArray ToUnicode(const QString & Str)
{
  QByteArray Utf = Str.toUtf8();
  if (Utf.size() > Str.size())
  {
    Utf.prepend(Bom);
  }
  return Utf;
}

QString MungeStr(const QString & Str, bool Value)
{
  return PuttyEscape(ToUnicode(Str), Value);
}

QString UnMungeStr(const QString & Str)
{
  QByteArray Dest = PuttyUnescape(Str);
  if (Dest.startsWith(Bom))
  {
    return QString::fromUtf8(Dest.mid(Bom.size()));
  }
  else
  {
    return QString::fromLatin1(Dest);
  }
}

QString MungeIniName(const QString & Str)
{
  bool NeedEscaping = false;
  for (QChar Ch : Str)
  {
    if ((Ch.unicode() > 127) || (Ch == QLatin1Char('=')))
    {
      NeedEscaping = true;
      break;
    }
  }
  QString Result = NeedEscaping ? PuttyEscape(ToUnicode(Str), false) : Str;
  Result.replace(QStringLiteral("="), QStringLiteral("%3D"));
  return Result;
}

QString UnMungeIniName(const QString & Str)
{
  static const QString EscapedBom = PuttyEscape(Bom, false);
  if (Str.startsWith(EscapedBom))
  {
    return UnMungeStr(Str);
  }
  QString Result = Str;
  Result.replace(QStringLiteral("%3D"), QStringLiteral("="));
  return Result;
}

//---------------------------------------------------------------------------

bool IniFile::Load(const QString & FileName)
{
  Clear();
  QFile F(FileName);
  if (!F.open(QIODevice::ReadOnly))
  {
    return false;
  }
  // an encrypted configuration (NixSCP Pro) is readable once unlocked
  QByteArray Plaintext;
  if (!ConfigVault::Open(F.readAll(), Plaintext))
  {
    return false;
  }
  return LoadFromData(Plaintext);
}

bool IniFile::LoadFromData(const QByteArray & AData)
{
  Clear();
  QByteArray Data = AData;
  QString Text;
  // WinSCP writes UTF-8 (optionally with BOM); old versions wrote ANSI or UTF-16
  if (Data.startsWith("\xFF\xFE"))
  {
    Text = QString::fromUtf16(reinterpret_cast<const char16_t *>(Data.constData() + 2), (Data.size() - 2) / 2);
  }
  else
  {
    if (Data.startsWith(Bom))
    {
      Data.remove(0, Bom.size());
    }
    Text = QString::fromUtf8(Data);
  }
  if (Text.startsWith(QStringLiteral("Windows Registry Editor")) || Text.startsWith(QStringLiteral("REGEDIT4")))
  {
    LoadRegistryExport(Text);
  }
  else
  {
    ParseText(Text);
  }
  return true;
}

void IniFile::ParseText(const QString & Text)
{
  Section * Current = nullptr;
  const QStringList Lines = Text.split(QLatin1Char('\n'));
  for (QString Line : Lines)
  {
    Line = Line.trimmed();
    if (Line.isEmpty() || Line.startsWith(QLatin1Char(';')))
    {
      continue;
    }
    if (Line.startsWith(QLatin1Char('[')) && Line.endsWith(QLatin1Char(']')))
    {
      Current = FindOrAdd(Line.mid(1, Line.size() - 2));
    }
    else
    {
      int P = Line.indexOf(QLatin1Char('='));
      if (P > 0)
      {
        if (Current == nullptr)
        {
          // values before the first section: root key
          Current = FindOrAdd(QString());
        }
        Current->Values.append({Line.left(P), Line.mid(P + 1)});
      }
    }
  }
}

// Registry export of WinSCP settings ("reg export HKCU\\Software\\Martin Prikryl\\WinSCP 2 x.reg"
// on Windows). WinSCP keeps the same tree and the same (munged) values in the registry
// as in WinSCP.ini (THierarchicalStorage), so keys below "WinSCP 2" map 1:1 to INI sections.
static QString RegUnquote(const QString & Str, int & Pos)
{
  // Str[Pos] is the opening quote; returns content, Pos after closing quote
  QString Result;
  for (Pos++; Pos < Str.size(); Pos++)
  {
    QChar Ch = Str[Pos];
    if ((Ch == QLatin1Char('\\')) && (Pos + 1 < Str.size()))
    {
      Result += Str[++Pos];
    }
    else if (Ch == QLatin1Char('"'))
    {
      Pos++;
      break;
    }
    else
    {
      Result += Ch;
    }
  }
  return Result;
}

// REG_BINARY value ("hex:aa,bb,..."). TRegistryStorage keeps Int64 values
// (LogMaxSize, CopyParam ResumeThreshold, ...) as 8 byte binary data, while
// TCustomIniFileStorage keeps them as decimal strings; DateTime, Float and other
// binary data are kept as BytesToHex in WinSCP.ini.
static bool RegBinaryToIniValue(const QString & Name, const QString & HexList, QString & Result)
{
  static const QStringList Int64Names = {
    QStringLiteral("LogMaxSize"), QStringLiteral("ResumeThreshold"), QStringLiteral("DownloadSize") };
  QByteArray Bytes;
  const QStringList Parts = HexList.split(QLatin1Char(','), Qt::SkipEmptyParts);
  for (const QString & Part : Parts)
  {
    bool Ok = false;
    uint Byte = Part.trimmed().toUInt(&Ok, 16);
    if (!Ok || (Byte > 0xFF))
    {
      return false;
    }
    Bytes.append(static_cast<char>(Byte));
  }
  if (Int64Names.contains(Name, Qt::CaseInsensitive) && (Bytes.size() == static_cast<int>(sizeof(qint64))))
  {
    qint64 Value = 0;
    for (int Index = static_cast<int>(sizeof(qint64)) - 1; Index >= 0; Index--)
    {
      Value = (Value << 8) | static_cast<unsigned char>(Bytes[Index]);
    }
    Result = QString::number(Value);
  }
  else
  {
    Result = QString::fromLatin1(Bytes.toHex().toUpper());
  }
  return true;
}

void IniFile::LoadRegistryExport(const QString & Text)
{
  static const QString RootKey = QStringLiteral("\\WinSCP 2\\");
  Section * Current = nullptr;
  // "reg export" wraps long hex: values, continued lines end with a backslash
  QStringList Lines;
  {
    const QStringList RawLines = Text.split(QLatin1Char('\n'));
    bool Continued = false;
    for (const QString & RawLine : RawLines)
    {
      QString Line = RawLine.trimmed();
      if (Continued && !Lines.isEmpty())
      {
        Lines.last() += Line;
      }
      else
      {
        Lines.append(Line);
      }
      Continued = Lines.last().endsWith(QLatin1Char('\\')) && !Lines.last().startsWith(QLatin1Char('['));
      if (Continued)
      {
        Lines.last().chop(1);
      }
    }
  }
  for (QString Line : Lines)
  {
    Line = Line.trimmed();
    if (Line.startsWith(QLatin1Char('[')) && Line.endsWith(QLatin1Char(']')))
    {
      QString Key = Line.mid(1, Line.size() - 2);
      int P = Key.indexOf(RootKey, 0, Qt::CaseInsensitive);
      Current = nullptr;
      if (P >= 0)
      {
        Current = FindOrAdd(Key.mid(P + RootKey.size()));
      }
    }
    else if ((Current != nullptr) && Line.startsWith(QLatin1Char('"')))
    {
      int Pos = 0;
      QString Name = RegUnquote(Line, Pos);
      if ((Pos >= Line.size()) || (Line[Pos] != QLatin1Char('=')))
      {
        continue;
      }
      QString Value = Line.mid(Pos + 1);
      if (Value.startsWith(QLatin1Char('"')))
      {
        int VPos = 0;
        Current->Values.append({Name, RegUnquote(Value, VPos)});
      }
      else if (Value.startsWith(QStringLiteral("dword:"), Qt::CaseInsensitive))
      {
        // Integers and booleans (REG_DWORD, signed as WinSCP reads them)
        bool Ok = false;
        qint32 Number = static_cast<qint32>(Value.mid(6).toUInt(&Ok, 16));
        if (Ok)
        {
          Current->Values.append({Name, QString::number(Number)});
        }
      }
      else if (Value.startsWith(QStringLiteral("hex:"), Qt::CaseInsensitive))
      {
        QString IniValue;
        if (RegBinaryToIniValue(Name, Value.mid(4), IniValue))
        {
          Current->Values.append({Name, IniValue});
        }
      }
    }
  }
}

QByteArray IniFile::SaveToData() const
{
  QByteArray Out;
  // root values (no section header) have to go first
  for (const Section & S : FSections)
  {
    if (S.Name.isEmpty())
    {
      for (const auto & V : S.Values)
      {
        Out += V.first.toUtf8() + '=' + V.second.toUtf8() + "\r\n";
      }
      if (!S.Values.isEmpty())
      {
        Out += "\r\n";
      }
    }
  }
  for (const Section & S : FSections)
  {
    if (S.Name.isEmpty())
    {
      continue;
    }
    Out += '[' + S.Name.toUtf8() + "]\r\n";
    for (const auto & V : S.Values)
    {
      Out += V.first.toUtf8() + '=' + V.second.toUtf8() + "\r\n";
    }
    Out += "\r\n";
  }
  return Out;
}

bool IniFile::Save(const QString & FileName) const
{
  bool Existed = QFileInfo::exists(FileName);
  QSaveFile F(FileName);
  if (!F.open(QIODevice::WriteOnly | QIODevice::Truncate))
  {
    return false;
  }
  if (!Existed)
  {
    // QSaveFile keeps permissions of an existing file only
    F.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  }
  else
  {
    // the file holds (encrypted) passwords: an imported 0644 WinSCP.ini
    // is tightened to owner-only on the first save
    QFileDevice::Permissions Perms = QFileInfo(FileName).permissions() &
      (QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadUser | QFileDevice::WriteUser);
    F.setPermissions(Perms | QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  }
  F.write(ConfigVault::ForFile(FileName, SaveToData()));
  return F.commit();
}

void IniFile::Clear()
{
  FLastIndex = -1;
  FSections.clear();
  FIndex.clear();
  FIndexValid = true;
}

const IniFile::Section * IniFile::Find(const QString & Name) const
{
  if (FIndexValid && (FLastIndex >= 0) && (FLastIndex < FSections.size()) && (Name == FLastName))
  {
    return &FSections[FLastIndex];
  }
  if (!FIndexValid)
  {
    FIndex.clear();
    for (int I = 0; I < FSections.size(); I++)
    {
      FIndex.insert(FSections[I].Name.toLower(), I);
    }
    FIndexValid = true;
  }
  auto It = FIndex.constFind(Name.toLower());
  if (It == FIndex.constEnd())
  {
    return nullptr;
  }
  FLastName = Name;
  FLastIndex = It.value();
  return &FSections[It.value()];
}

IniFile::Section * IniFile::Find(const QString & Name)
{
  const Section * S = std::as_const(*this).Find(Name);
  if (S == nullptr)
  {
    return nullptr;
  }
  // non-const access detaches the (implicitly shared) list
  return &FSections[static_cast<int>(S - FSections.constData())];
}

IniFile::Section * IniFile::FindOrAdd(const QString & Name)
{
  Section * S = Find(Name);
  if (S == nullptr)
  {
    FSections.append(Section{Name, {}});
    FIndex.insert(Name.toLower(), FSections.size() - 1);
    S = &FSections.last();
  }
  return S;
}

QStringList IniFile::Sections() const
{
  QStringList Result;
  for (const Section & S : FSections)
  {
    Result.append(S.Name);
  }
  return Result;
}

bool IniFile::SectionExists(const QString & Section) const
{
  return Find(Section) != nullptr;
}

void IniFile::AddSection(const QString & Section)
{
  FindOrAdd(Section);
}

void IniFile::EraseSection(const QString & Name)
{
  if (const Section * S = Find(Name))
  {
    int Index = static_cast<int>(S - FSections.constData());
    FLastIndex = -1;
    FIndex.remove(S->Name.toLower());
    FSections.removeAt(Index);
    // keep the index valid (cheaper than rebuilding it, sites are erased one by one on save)
    for (auto It = FIndex.begin(); It != FIndex.end(); ++It)
    {
      if (It.value() > Index)
      {
        --It.value();
      }
    }
  }
}

void IniFile::EraseSectionTree(const QString & Name)
{
  QString Prefix = Name + QLatin1Char('\\');
  QStringList SubSections;
  for (const Section & S : FSections)
  {
    if ((S.Name.size() > Prefix.size()) && S.Name.startsWith(Prefix, Qt::CaseInsensitive))
    {
      SubSections.append(S.Name);
    }
  }
  EraseSection(Name);
  for (const QString & SubSection : SubSections)
  {
    EraseSection(SubSection);
  }
}

bool IniFile::KeyExists(const QString & Name) const
{
  if (Find(Name) != nullptr)
  {
    return true;
  }
  QString Prefix = Name + QLatin1Char('\\');
  for (const Section & S : FSections)
  {
    if (S.Name.startsWith(Prefix, Qt::CaseInsensitive))
    {
      return true;
    }
  }
  return false;
}

QStringList IniFile::Keys(const QString & Name) const
{
  QStringList Result;
  if (const Section * S = Find(Name))
  {
    for (const auto & V : S->Values)
    {
      Result.append(V.first);
    }
  }
  return Result;
}

QString IniFile::ReadRaw(const QString & Name, const QString & Key, const QString & Default, bool * Found) const
{
  if (const Section * S = Find(Name))
  {
    for (const auto & V : S->Values)
    {
      if (V.first.compare(Key, Qt::CaseInsensitive) == 0)
      {
        if (Found) *Found = true;
        return V.second;
      }
    }
  }
  if (Found) *Found = false;
  return Default;
}

void IniFile::WriteRaw(const QString & Name, const QString & Key, const QString & Value)
{
  Section * S = FindOrAdd(Name);
  for (auto & V : S->Values)
  {
    if (V.first.compare(Key, Qt::CaseInsensitive) == 0)
    {
      V.second = Value;
      return;
    }
  }
  S->Values.append({Key, Value});
}

void IniFile::DeleteKey(const QString & Name, const QString & Key)
{
  if (Section * S = Find(Name))
  {
    for (int I = 0; I < S->Values.size(); I++)
    {
      if (S->Values[I].first.compare(Key, Qt::CaseInsensitive) == 0)
      {
        S->Values.removeAt(I);
        return;
      }
    }
  }
}

void IniFile::Overlay(const IniFile & Other)
{
  for (const Section & S : Other.FSections)
  {
    FindOrAdd(S.Name);
    for (const auto & V : S.Values)
    {
      WriteRaw(S.Name, V.first, V.second);
    }
  }
}

QList<IniFile::Change> IniFile::Diff(const IniFile & From, const IniFile & To, const EquivalentFunc & Equivalent)
{
  QList<Change> Result;
  for (const Section & S : From.FSections)
  {
    const Section * T = To.Find(S.Name);
    if (T == nullptr)
    {
      Result.append({Change::ckEraseSection, S.Name, QString(), QString()});
      continue;
    }
    for (const auto & V : S.Values)
    {
      bool Found = false;
      QString NewValue = To.ReadRaw(S.Name, V.first, QString(), &Found);
      if (!Found)
      {
        Result.append({Change::ckDelete, S.Name, V.first, QString()});
      }
      else if ((NewValue != V.second) &&
               (!Equivalent || !Equivalent(S.Name, V.first, V.second, NewValue)))
      {
        Result.append({Change::ckWrite, S.Name, V.first, NewValue});
      }
    }
  }
  for (const Section & T : To.FSections)
  {
    const Section * S = From.Find(T.Name);
    if (S == nullptr)
    {
      Result.append({Change::ckAddSection, T.Name, QString(), QString()});
    }
    for (const auto & V : T.Values)
    {
      bool Found = false;
      if (S != nullptr)
      {
        From.ReadRaw(T.Name, V.first, QString(), &Found);
      }
      if (!Found)
      {
        Result.append({Change::ckWrite, T.Name, V.first, V.second});
      }
    }
  }
  return Result;
}

void IniFile::Apply(const QList<Change> & Changes)
{
  for (const Change & C : Changes)
  {
    switch (C.Kind)
    {
      case Change::ckWrite:
        WriteRaw(C.Section, C.Key, C.Value);
        break;
      case Change::ckDelete:
        DeleteKey(C.Section, C.Key);
        break;
      case Change::ckAddSection:
        AddSection(C.Section);
        break;
      case Change::ckEraseSection:
        EraseSection(C.Section);
        break;
    }
  }
}

//---------------------------------------------------------------------------

IniStorage::IniStorage(IniFile * File) :
  FFile(File)
{
}

QString IniStorage::CurrentSection() const
{
  return FCurrentSection;
}

void IniStorage::UpdateCurrentSection()
{
  FCurrentSection = FKeyHistory.join(QLatin1Char('\\'));
}

bool IniStorage::OpenSubKey(const QString & SubKey, bool CanCreate)
{
  QString Munged = MungeStr(SubKey, false);
  QStringList NewHistory = FKeyHistory;
  NewHistory.append(Munged);
  // A key "exists" also when it only has subkeys (no need to know when it can be created)
  if (CanCreate || FFile->KeyExists(NewHistory.join(QLatin1Char('\\'))))
  {
    FKeyHistory = NewHistory;
    FOpenLevels.append(1);
    UpdateCurrentSection();
    return true;
  }
  return false;
}

bool IniStorage::OpenSubKeyPath(const QString & KeyPath, bool CanCreate)
{
  const QStringList Parts = KeyPath.split(QLatin1Char('\\'), Qt::SkipEmptyParts);
  int Opens = 0;
  bool Result = !Parts.isEmpty();
  for (const QString & Part : Parts)
  {
    if (!OpenSubKey(Part, CanCreate))
    {
      Result = false;
      break;
    }
    Opens++;
  }
  if (Result)
  {
    // collapse the individual opens into one entry
    FOpenLevels.resize(FOpenLevels.size() - Opens);
    FOpenLevels.append(Opens);
  }
  else
  {
    while (Opens-- > 0)
    {
      CloseSubKey();
    }
  }
  return Result;
}

void IniStorage::CloseSubKey()
{
  int Levels = FOpenLevels.isEmpty() ? 1 : FOpenLevels.takeLast();
  while ((Levels-- > 0) && !FKeyHistory.isEmpty())
  {
    FKeyHistory.removeLast();
  }
  UpdateCurrentSection();
}

QStringList IniStorage::GetSubKeyNames() const
{
  QString Prefix = CurrentSection();
  if (!Prefix.isEmpty())
  {
    Prefix += QLatin1Char('\\');
  }
  QStringList Result;
  for (const QString & S : FFile->Sections())
  {
    if (S.startsWith(Prefix, Qt::CaseInsensitive))
    {
      QString Rest = S.mid(Prefix.size());
      int P = Rest.indexOf(QLatin1Char('\\'));
      QString Name = UnMungeStr((P >= 0) ? Rest.left(P) : Rest);
      if (!Name.isEmpty() && !Result.contains(Name))
      {
        Result.append(Name);
      }
    }
  }
  return Result;
}

QStringList IniStorage::GetValueNames() const
{
  QStringList Result;
  for (const QString & Key : FFile->Keys(CurrentSection()))
  {
    Result.append(UnMungeIniName(Key));
  }
  return Result;
}

bool IniStorage::HasSubKey(const QString & SubKey) const
{
  return GetSubKeyNames().contains(SubKey);
}

void IniStorage::RecursiveDeleteSubKey(const QString & SubKey)
{
  QString Section = CurrentSection();
  if (!Section.isEmpty())
  {
    Section += QLatin1Char('\\');
  }
  Section += MungeStr(SubKey, false);
  FFile->EraseSectionTree(Section);
}

bool IniStorage::ValueExists(const QString & Name) const
{
  bool Found = false;
  FFile->ReadRaw(CurrentSection(), MungeIniName(Name), QString(), &Found);
  return Found;
}

void IniStorage::DeleteValue(const QString & Name)
{
  FFile->DeleteKey(CurrentSection(), MungeIniName(Name));
}

QString IniStorage::ReadStringRaw(const QString & Name, const QString & Default) const
{
  return FFile->ReadRaw(CurrentSection(), MungeIniName(Name), Default);
}

QString IniStorage::ReadString(const QString & Name, const QString & Default) const
{
  bool Found = false;
  QString Value = FFile->ReadRaw(CurrentSection(), MungeIniName(Name), QString(), &Found);
  return Found ? UnMungeStr(Value) : Default;
}

int IniStorage::ReadInteger(const QString & Name, int Default) const
{
  bool Ok = false;
  int Result = ReadStringRaw(Name, QString()).toInt(&Ok);
  return Ok ? Result : Default;
}

bool IniStorage::ReadBool(const QString & Name, bool Default) const
{
  QString S = ReadStringRaw(Name, QString());
  if (S.isEmpty())
  {
    return Default;
  }
  if (S.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0) return true;
  if (S.compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0) return false;
  bool Ok = false;
  int I = S.toInt(&Ok);
  return Ok ? (I != 0) : Default;
}

void IniStorage::WriteStringRaw(const QString & Name, const QString & Value)
{
  FFile->WriteRaw(CurrentSection(), MungeIniName(Name), Value);
}

void IniStorage::WriteString(const QString & Name, const QString & Value)
{
  WriteStringRaw(Name, MungeStr(Value, true));
}

void IniStorage::WriteInteger(const QString & Name, int Value)
{
  WriteStringRaw(Name, QString::number(Value));
}

void IniStorage::WriteBool(const QString & Name, bool Value)
{
  // TIniFile::WriteBool writes 1/0
  WriteStringRaw(Name, Value ? QStringLiteral("1") : QStringLiteral("0"));
}
