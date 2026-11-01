// Port of WinSCP source/windows/GUIConfiguration.cpp (TCopyParamRule,
// TCopyParamList, TGUIConfiguration preset properties) and
// forms/CustomScpExplorer.cpp (TransferPresetAutoSelect)
#include "core/CopyParamList.h"
#include "core/Configuration.h"
#include "core/FileSystem.h"
#include "core/IniStorage.h"

#include <QSet>

const QString CopyParamList::InvalidChars = QStringLiteral("/\\[]");

static const QString CopyParamKeyPath = QStringLiteral("Configuration\\Interface\\CopyParam");

static QString StripHotkey(QString Text)
{
  return Text.remove(QLatin1Char('&'));
}

//---------------------------------------------------------------------------
// CopyParamRule
//---------------------------------------------------------------------------

// TFileMasks with ForceDirectoryMasks = 1: every mask is a directory mask
static QString ForceDirectoryMasks(const QString & Masks)
{
  QString Result;
  QString Element;
  int Brackets = 0;
  auto Flush = [&]()
  {
    QString Trimmed = Element.trimmed();
    if (!Trimmed.isEmpty() && !Trimmed.endsWith(QLatin1Char('/')))
    {
      // keep the original spacing, just append the slash to the mask itself
      int End = Element.lastIndexOf(Trimmed) + Trimmed.size();
      Element.insert(End, QLatin1Char('/'));
    }
    Result += Element;
    Element.clear();
  };
  for (QChar C : Masks)
  {
    if (C == QLatin1Char('['))
    {
      Brackets++;
    }
    else if ((C == QLatin1Char(']')) && (Brackets > 0))
    {
      Brackets--;
    }
    if ((Brackets == 0) &&
        ((C == QLatin1Char(';')) || (C == QLatin1Char(',')) || (C == FileMasks::IncludeExcludeFileMasksDelimiter)))
    {
      Flush();
      Result += C;
    }
    else
    {
      Element += C;
    }
  }
  Flush();
  return Result;
}

static bool MatchRuleMask(const QString & Mask, const QString & Value, bool Path, bool Local)
{
  if (Mask.isEmpty())
  {
    return true;
  }
  try
  {
    if (Path)
    {
      FileMasks M(ForceDirectoryMasks(Mask));
      QString AValue = Value;
      while ((AValue.size() > 1) && AValue.endsWith(QLatin1Char('/')))
      {
        AValue.chop(1);
      }
      return M.Matches(AValue, Local, true);
    }
    FileMasks M(Mask);
    return M.MatchesFileName(Value);
  }
  catch (const FileMasksException &)
  {
    return false;
  }
}

bool CopyParamRule::Matches(const CopyParamRuleData & Value) const
{
  return
    MatchRuleMask(Data.HostName, Value.HostName, false, true) &&
    MatchRuleMask(Data.UserName, Value.UserName, false, true) &&
    MatchRuleMask(Data.RemoteDirectory, Value.RemoteDirectory, true, false) &&
    MatchRuleMask(Data.LocalDirectory, Value.LocalDirectory, true, true);
}

void CopyParamRule::Load(const IniStorage & Storage)
{
  Data.HostName = Storage.ReadString(QStringLiteral("HostName"), Data.HostName);
  Data.UserName = Storage.ReadString(QStringLiteral("UserName"), Data.UserName);
  Data.RemoteDirectory = Storage.ReadString(QStringLiteral("RemoteDirectory"), Data.RemoteDirectory);
  Data.LocalDirectory = Storage.ReadString(QStringLiteral("LocalDirectory"), Data.LocalDirectory);
}

void CopyParamRule::Save(IniStorage & Storage) const
{
  Storage.WriteString(QStringLiteral("HostName"), Data.HostName);
  Storage.WriteString(QStringLiteral("UserName"), Data.UserName);
  Storage.WriteString(QStringLiteral("RemoteDirectory"), Data.RemoteDirectory);
  Storage.WriteString(QStringLiteral("LocalDirectory"), Data.LocalDirectory);
}

bool CopyParamRule::IsEmpty() const
{
  return Data.HostName.isEmpty() && Data.UserName.isEmpty() &&
    Data.RemoteDirectory.isEmpty() && Data.LocalDirectory.isEmpty();
}

QString CopyParamRule::GetInfoStr(const QString & Separator) const
{
  QString Result;
  auto Add = [&](const QString & Format, const QString & Value)
  {
    if (!Value.isEmpty())
    {
      Result += (Result.isEmpty() ? QString() : Separator) + Format.arg(Value);
    }
  };
  // COPY_RULE_HOSTNAME, COPY_RULE_USERNAME, COPY_RULE_REMOTE_DIR, COPY_RULE_LOCAL_DIR
  Add(QStringLiteral("Hostname: %1"), Data.HostName);
  Add(QStringLiteral("Username: %1"), Data.UserName);
  Add(QStringLiteral("Remote directory: %1"), Data.RemoteDirectory);
  Add(QStringLiteral("Local directory: %1"), Data.LocalDirectory);
  return Result;
}

//---------------------------------------------------------------------------
// CopyParamList
//---------------------------------------------------------------------------

const CopyParamRule * CopyParamList::Rule(int Index) const
{
  const Item & I = FItems.at(Index);
  return I.Rule.has_value() ? &*I.Rule : nullptr;
}

QStringList CopyParamList::NameList() const
{
  QStringList Result;
  for (const Item & I : FItems)
  {
    Result.append(I.Name);
  }
  return Result;
}

bool CopyParamList::AnyRule() const
{
  for (const Item & I : FItems)
  {
    if (I.Rule.has_value())
    {
      return true;
    }
  }
  return false;
}

int CopyParamList::IndexOfName(const QString & Name) const
{
  // TStringList::IndexOf: case insensitive
  for (int Index = 0; Index < Count(); Index++)
  {
    if (FItems.at(Index).Name.compare(Name, Qt::CaseInsensitive) == 0)
    {
      return Index;
    }
  }
  return -1;
}

int CopyParamList::Find(const CopyParamRuleData & Value) const
{
  for (int Index = 0; Index < Count(); Index++)
  {
    const Item & I = FItems.at(Index);
    if (I.Rule.has_value() && I.Rule->Matches(Value))
    {
      return Index;
    }
  }
  return -1;
}

void CopyParamList::Clear()
{
  if (!FItems.isEmpty())
  {
    FItems.clear();
    FModified = true;
  }
}

void CopyParamList::Add(const QString & Name, const CopyParam & Param, const std::optional<CopyParamRule> & Rule)
{
  Insert(Count(), Name, Param, Rule);
}

void CopyParamList::Insert(int Index, const QString & Name, const CopyParam & Param,
  const std::optional<CopyParamRule> & Rule)
{
  Item I;
  I.Name = Name;
  I.Param = Param;
  I.Rule = Rule;
  FItems.insert(qBound(0, Index, Count()), I);
  FModified = true;
}

void CopyParamList::Change(int Index, const QString & Name, const CopyParam & Param,
  const std::optional<CopyParamRule> & Rule)
{
  Item & I = FItems[Index];
  if ((I.Name != Name) || (I.Param != Param) || (I.Rule != Rule))
  {
    I.Name = Name;
    I.Param = Param;
    I.Rule = Rule;
    FModified = true;
  }
}

void CopyParamList::Move(int CurIndex, int NewIndex)
{
  if ((CurIndex != NewIndex) && (CurIndex >= 0) && (CurIndex < Count()) && (NewIndex >= 0) && (NewIndex < Count()))
  {
    FItems.move(CurIndex, NewIndex);
    FModified = true;
  }
}

void CopyParamList::Delete(int Index)
{
  if ((Index >= 0) && (Index < Count()))
  {
    FItems.removeAt(Index);
    FModified = true;
  }
}

void CopyParamList::ValidateName(const QString & Name)
{
  for (QChar C : InvalidChars)
  {
    if (Name.contains(C))
    {
      // ITEM_NAME_INVALID
      throw ExtException(QStringLiteral("Invalid name '%1'. Name cannot contain '%2'.").arg(Name, InvalidChars));
    }
  }
}

// Values written by CopyParam::Save and the preset/rule values
static const QSet<QString> & KnownPresetValues()
{
  static const QSet<QString> Result = []()
  {
    QSet<QString> Names;
    IniFile File;
    IniStorage Storage(&File);
    Storage.OpenSubKey(QStringLiteral("X"), true);
    CopyParam().Save(Storage);
    CopyParamRule().Save(Storage);
    for (const QString & Name : Storage.GetValueNames())
    {
      Names.insert(Name.toLower());
    }
    for (const char * Name : { "Name", "HasRule", "ExcludeFileMask", "NegativeExclude" })
    {
      Names.insert(QString::fromLatin1(Name).toLower());
    }
    return Names;
  }();
  return Result;
}

void CopyParamList::Load(const IniStorage & AStorage, int ACount)
{
  IniStorage & Storage = const_cast<IniStorage &>(AStorage);   // OpenSubKey/CloseSubKey only
  FItems.clear();
  for (int Index = 0; Index < ACount; Index++)
  {
    Item I;
    I.Name = QString::number(Index);
    if (Storage.OpenSubKey(QString::number(Index), false))
    {
      I.Name = Storage.ReadString(QStringLiteral("Name"), I.Name);
      I.Param.Load(Storage);
      if (Storage.ReadBool(QStringLiteral("HasRule"), false))
      {
        CopyParamRule Rule;
        Rule.Load(Storage);
        I.Rule = Rule;
      }
      const QSet<QString> & Known = KnownPresetValues();
      for (const QString & Name : Storage.GetValueNames())
      {
        if (!Known.contains(Name.toLower()))
        {
          I.Extra.append(qMakePair(Name, Storage.ReadStringRaw(Name, QString())));
        }
      }
      Storage.CloseSubKey();
    }
    FItems.append(I);
  }
  FModified = false;
}

void CopyParamList::Save(IniStorage & Storage) const
{
  // Storage->ClearSubKeys()
  for (const QString & SubKey : Storage.GetSubKeyNames())
  {
    Storage.RecursiveDeleteSubKey(SubKey);
  }
  for (int Index = 0; Index < Count(); Index++)
  {
    if (Storage.OpenSubKey(QString::number(Index), true))
    {
      const Item & I = FItems.at(Index);
      Storage.WriteString(QStringLiteral("Name"), I.Name);
      I.Param.Save(Storage);
      // TGUICopyParamType is not saved for presets
      for (const char * Name : { "Queue", "QueueNoConfirmation", "QueueParallel" })
      {
        Storage.DeleteValue(QString::fromLatin1(Name));
      }
      Storage.WriteBool(QStringLiteral("HasRule"), I.Rule.has_value());
      if (I.Rule.has_value())
      {
        I.Rule->Save(Storage);
      }
      for (const auto & Extra : I.Extra)
      {
        Storage.WriteStringRaw(Extra.first, Extra.second);
      }
      Storage.CloseSubKey();
    }
  }
}

bool CopyParamList::operator==(const CopyParamList & Other) const
{
  if (Count() != Other.Count())
  {
    return false;
  }
  for (int Index = 0; Index < Count(); Index++)
  {
    const Item & A = FItems.at(Index);
    const Item & B = Other.FItems.at(Index);
    if ((A.Name != B.Name) || (A.Param != B.Param) || (A.Rule != B.Rule))
    {
      return false;
    }
  }
  return true;
}

CopyParamList CopyParamList::Defaults(const CopyParam & DefaultCopyParam)
{
  CopyParamList Result;
  CopyParam Param = DefaultCopyParam;
  Param.Mode = CopyParam::tmAscii;
  Result.Add(QStringLiteral("&Text"), Param);                          // COPY_PARAM_PRESET_ASCII

  Param = DefaultCopyParam;
  Param.Mode = CopyParam::tmBinary;
  Result.Add(QStringLiteral("&Binary"), Param);                        // COPY_PARAM_PRESET_BINARY

  Param = DefaultCopyParam;
  Param.NewerOnly = true;
  Result.Add(QStringLiteral("&New and updated files only"), Param);   // COPY_PARAM_NEWER_ONLY

  Param = DefaultCopyParam;
  Param.IncludeFileMask = QStringLiteral("%1 */").arg(FileMasks::IncludeExcludeFileMasksDelimiter);
  Result.Add(QStringLiteral("Exclude &directories"), Param);          // COPY_PARAM_PRESET_EXCLUDE_ALL_DIR

  Result.Reset();
  return Result;
}

//---------------------------------------------------------------------------
// CopyParamPresets
//---------------------------------------------------------------------------

CopyParamPresets & CopyParamPresets::Instance()
{
  static CopyParamPresets * Result = new CopyParamPresets();
  return *Result;
}

CopyParamPresets::CopyParamPresets()
{
  Configuration & Config = Configuration::Instance();
  // Load, Reload (another instance saved), Import
  connect(&Config, &Configuration::Changed, this, &CopyParamPresets::Reload);
  connect(&Config, &Configuration::StoredSessionsChanged, this, &CopyParamPresets::Reload);
}

void CopyParamPresets::EnsureLoaded()
{
  const CopyParam & DefaultCopyParam = Configuration::Instance().DefaultCopyParam();
  if (FLoaded)
  {
    // the built-in presets follow the default transfer settings
    if (FDefaults && (FDefaultsBase != DefaultCopyParam))
    {
      FDefaultsBase = DefaultCopyParam;
      FList = CopyParamList::Defaults(FDefaultsBase);
    }
    return;
  }
  FLoaded = true;
  int Count = -1;
  IniStorage Storage(&Configuration::Instance().Ini());
  if (Storage.OpenSubKeyPath(CopyParamKeyPath, false))
  {
    Count = Storage.ReadInteger(QStringLiteral("CopyParamList"), -1);
    if (Count >= 0)
    {
      FList.Load(Storage, Count);
    }
    Storage.CloseSubKeyPath();
  }
  FDefaults = (Count < 0);
  if (FDefaults)
  {
    FDefaultsBase = DefaultCopyParam;
    FList = CopyParamList::Defaults(FDefaultsBase);
  }
  FList.Reset();
}

void CopyParamPresets::Reload()
{
  CopyParamList Previous = FList;
  FLoaded = false;
  EnsureLoaded();
  bool Changed = (Previous != FList);
  if (!HasPreset(FCurrent))
  {
    FCurrent.clear();
    emit CurrentChanged();
  }
  if (Changed)
  {
    emit ListChanged();
  }
}

const CopyParamList & CopyParamPresets::List()
{
  EnsureLoaded();
  return FList;
}

bool CopyParamPresets::IsDefaultList()
{
  EnsureLoaded();
  return FDefaults;
}

void CopyParamPresets::SetList(const CopyParamList & List)
{
  EnsureLoaded();
  if (FList != List)
  {
    FList = List;
    FList.Reset();
    FDefaults = false;
    IniStorage Storage(&Configuration::Instance().Ini());
    if (Storage.OpenSubKeyPath(CopyParamKeyPath, true))
    {
      Storage.WriteInteger(QStringLiteral("CopyParamList"), FList.Count());
      FList.Save(Storage);
      Storage.CloseSubKeyPath();
    }
    if (!HasPreset(FCurrent))
    {
      FCurrent.clear();
      emit CurrentChanged();
    }
    emit ListChanged();
  }
}

void CopyParamPresets::SetCurrent(const QString & Name)
{
  if (FCurrent != Name)
  {
    FCurrent = Name;
    emit CurrentChanged();
  }
}

int CopyParamPresets::CurrentIndex()
{
  return FCurrent.isEmpty() ? -1 : List().IndexOfName(FCurrent);
}

void CopyParamPresets::SetCurrentIndex(int Index)
{
  SetCurrent(((Index < 0) || (Index >= List().Count())) ? QString() : List().Name(Index));
}

void CopyParamPresets::SelectByUser(const QString & Name)
{
  // TCustomScpExplorerForm::TransferListChange
  if (FAutoSelected.isEmpty())
  {
    // if previous preset was not autoselected, make new preset the "default"
    FDefaultPreset = Name;
  }
  SetCurrent(Name);
}

CopyParam CopyParamPresets::Preset(const QString & Name)
{
  const CopyParam & DefaultCopyParam = Configuration::Instance().DefaultCopyParam();
  CopyParam Result = DefaultCopyParam;
  if (!Name.isEmpty())
  {
    int Index = List().IndexOfName(Name);
    if (Index >= 0)
    {
      // overwrite all but GUI options
      Result = List().Param(Index);
      Result.Queue = DefaultCopyParam.Queue;
      Result.QueueNoConfirmation = DefaultCopyParam.QueueNoConfirmation;
      Result.QueueParallel = DefaultCopyParam.QueueParallel;
      Result.ConfirmOverwriting = DefaultCopyParam.ConfirmOverwriting;
      Result.ConfirmResume = DefaultCopyParam.ConfirmResume;
      // reset all options known not to be configurable per-preset
      Result.ResumeSupport = DefaultCopyParam.ResumeSupport;
      Result.ResumeThreshold = DefaultCopyParam.ResumeThreshold;
    }
  }
  return Result;
}

bool CopyParamPresets::HasPreset(const QString & Name)
{
  return Name.isEmpty() || (List().IndexOfName(Name) >= 0);
}

bool CopyParamPresets::AutoSelect(const CopyParamRuleData & Data, int CopyParamAttrs, QString * Message, QString * Info)
{
  int CopyParamIndex = List().Find(Data);
  QString PreviousCurrent = FCurrent;

  if (CopyParamIndex < 0)
  {
    // there is no preset that matches autoselection
    // set preset that we consider "default"
    FAutoSelected.clear();   // forget last autoselected preset
    SetCurrent(HasPreset(FDefaultPreset) ? FDefaultPreset : QString());
  }
  else
  {
    QString CopyParamName = List().Name(CopyParamIndex);
    if (CopyParamName != FAutoSelected)
    {
      // autoselected the different preset then the last time (or there was
      // default preset set); else make no change (i.e. preserve custom user preset)
      FAutoSelected = CopyParamName;
      SetCurrent(CopyParamName);
    }
  }

  if (FCurrent == PreviousCurrent)
  {
    return false;
  }

  if (Message != nullptr)
  {
    QString Format =
      (CopyParamIndex < 0) ?
        ((CurrentIndex() < 0) ? QStringLiteral("Returned back to default transfer settings.") :
          QStringLiteral("Returned back to transfer settings preset '%1'.")) :
        QStringLiteral("Transfer settings preset '%1' was automatically selected.");
    *Message = Format.contains(QStringLiteral("%1")) ? Format.arg(StripHotkey(FCurrent)) : Format;
  }
  if (Info != nullptr)
  {
    *Info = CurrentCopyParam().GetInfoStr(QStringLiteral("\n"), CopyParamAttrs);
    if (CopyParamIndex >= 0)
    {
      const CopyParamRule * Rule = List().Rule(CopyParamIndex);
      if (Rule != nullptr)
      {
        // COPY_PARAM_RULE
        *Info += QStringLiteral("\n \nAutoselection rule:\n%1").arg(Rule->GetInfoStr(QStringLiteral("\n")));
      }
    }
  }
  return true;
}
