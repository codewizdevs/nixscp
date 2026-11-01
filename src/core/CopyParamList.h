// Port of WinSCP source/windows/GUIConfiguration.{h,cpp}: TCopyParamRuleData,
// TCopyParamRule, TCopyParamList (transfer settings presets) and the preset
// part of TGUIConfiguration (CopyParamList, CopyParamCurrent, CopyParamPreset,
// DefaultLocalized defaults), plus the preset autoselection of
// forms/CustomScpExplorer.cpp (TransferPresetAutoSelect).
//
// WinSCP.ini layout (identical to WinSCP):
//   [Configuration\Interface\CopyParam]  CopyParamList=<count>, -1 = built-in defaults
//   [Configuration\Interface\CopyParam\<index>]  Name=..., <TCopyParamType keys>,
//     HasRule=0/1, HostName, UserName, RemoteDirectory, LocalDirectory
// Values of a preset that NixSCP does not know (ClearArchive,
// ReplaceInvalidChars, LocalInvalidChars, EncryptNewFiles, ...) are kept and
// written back verbatim.
#pragma once

#include "core/CopyParam.h"

#include <QList>
#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>

#include <optional>

class IniStorage;

struct CopyParamRuleData
{
  QString HostName;
  QString UserName;
  QString RemoteDirectory;
  QString LocalDirectory;

  bool operator==(const CopyParamRuleData & Other) const = default;
};

class CopyParamRule
{
public:
  CopyParamRule() = default;
  explicit CopyParamRule(const CopyParamRuleData & AData) : Data(AData) {}

  CopyParamRuleData Data;

  // Empty masks match anything; host/user masks match the name, directory
  // masks match the whole path (as directory masks)
  bool Matches(const CopyParamRuleData & Value) const;
  void Load(const IniStorage & Storage);
  void Save(IniStorage & Storage) const;
  bool IsEmpty() const;
  // COPY_RULE_HOSTNAME etc. joined by Separator
  QString GetInfoStr(const QString & Separator) const;

  bool operator==(const CopyParamRule & Other) const { return Data == Other.Data; }
};

class CopyParamList
{
public:
  int Count() const { return static_cast<int>(FItems.size()); }
  QString Name(int Index) const { return FItems.at(Index).Name; }
  const CopyParam & Param(int Index) const { return FItems.at(Index).Param; }
  // nullptr when the preset has no autoselection rule
  const CopyParamRule * Rule(int Index) const;
  QStringList NameList() const;
  bool AnyRule() const;
  bool Modified() const { return FModified; }

  int IndexOfName(const QString & Name) const;
  // First preset whose rule matches (TCopyParamList::Find), -1 when none
  int Find(const CopyParamRuleData & Value) const;

  void Clear();
  void Add(const QString & Name, const CopyParam & Param, const std::optional<CopyParamRule> & Rule = std::nullopt);
  void Insert(int Index, const QString & Name, const CopyParam & Param,
    const std::optional<CopyParamRule> & Rule = std::nullopt);
  void Change(int Index, const QString & Name, const CopyParam & Param,
    const std::optional<CopyParamRule> & Rule = std::nullopt);
  void Move(int CurIndex, int NewIndex);
  void Delete(int Index);
  void Reset() { FModified = false; }

  // TCopyParamList::ValidateName: throws ExtException (ITEM_NAME_INVALID)
  static void ValidateName(const QString & Name);
  static const QString InvalidChars;   // "/\[]"

  // Storage opened at Configuration\Interface\CopyParam
  void Load(const IniStorage & Storage, int Count);
  void Save(IniStorage & Storage) const;

  bool operator==(const CopyParamList & Other) const;
  bool operator!=(const CopyParamList & Other) const { return !(*this == Other); }

  // TGUIConfiguration::DefaultLocalized: Text, Binary, New and updated files
  // only, Exclude directories (based on the default transfer settings)
  static CopyParamList Defaults(const CopyParam & DefaultCopyParam);

private:
  struct Item
  {
    QString Name;
    CopyParam Param;
    std::optional<CopyParamRule> Rule;
    // values not known to NixSCP, written back verbatim (raw)
    QList<QPair<QString, QString>> Extra;
  };
  QList<Item> FItems;
  bool FModified = false;
};

// Transfer settings presets of the application (TGUIConfiguration preset
// properties), stored in Configuration::Instance().Ini(). The model of the
// Copy dialog presets menu, Preferences > Transfer and the Transfer Settings
// toolbar.
class CopyParamPresets : public QObject
{
  Q_OBJECT

public:
  static CopyParamPresets & Instance();

  // The presets (built-in defaults when WinSCP.ini has CopyParamList=-1 or none)
  const CopyParamList & List();
  bool IsDefaultList();
  // TGUIConfiguration::SetCopyParamList: writes to the configuration
  // (Configuration::Save() persists it); emits ListChanged
  void SetList(const CopyParamList & List);

  // CopyParamCurrent: name of the selected preset, empty = Default (not saved,
  // as in WinSCP)
  QString Current() const { return FCurrent; }
  void SetCurrent(const QString & Name);
  int CurrentIndex();
  void SetCurrentIndex(int Index);
  // Selection by the user (Transfer Settings toolbar): also becomes the
  // preset to return to when no autoselection rule matches
  void SelectByUser(const QString & Name);

  // TGUIConfiguration::GetCopyParamPreset: the default transfer settings
  // overwritten by the preset (except resume settings and the queue options)
  CopyParam Preset(const QString & Name);
  CopyParam CurrentCopyParam() { return Preset(FCurrent); }
  bool HasPreset(const QString & Name);

  // TCustomScpExplorerForm::TransferPresetAutoSelect: picks the preset whose
  // rule matches (or returns to the default one). Returns true when the
  // current preset changed; Message is COPY_PARAM_AUTOSELECTED /
  // COPY_PARAM_DEFAULT_CUSTOM / COPY_PARAM_DEFAULT_NORM, Info the settings
  // summary (and the rule). Show it as a message box when
  // Configuration::CopyParamAutoSelectNotice is on, as a note otherwise.
  bool AutoSelect(const CopyParamRuleData & Data, int CopyParamAttrs, QString * Message = nullptr, QString * Info = nullptr);

  // Reads the presets again from the configuration (after Load/Import)
  void Reload();

signals:
  void ListChanged();
  void CurrentChanged();

private:
  CopyParamPresets();
  void EnsureLoaded();

  CopyParamList FList;
  bool FLoaded = false;
  bool FDefaults = true;
  CopyParam FDefaultsBase;
  QString FCurrent;
  QString FAutoSelected;
  QString FDefaultPreset;
};
