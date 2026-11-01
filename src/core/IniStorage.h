// Port of the WinSCP.ini side of WinSCP source/core/HierarchicalStorage.cpp
// (TIniFileStorage) and PuTTY's escape_registry_key/unescape_registry_key.
// The file format is identical to WinSCP.ini, so the files are interchangeable.
#pragma once

#include <QString>
#include <QStringList>
#include <QList>
#include <QPair>
#include <QHash>

#include <functional>

// MungeStr/UnMungeStr: PuTTY %-escaping, UTF-8 with BOM marker for non-ASCII
QString MungeStr(const QString & Str, bool Value);
QString UnMungeStr(const QString & Str);
QString MungeIniName(const QString & Str);
QString UnMungeIniName(const QString & Str);

class IniFile
{
public:
  // Also accepts a .reg export of WinSCP's registry settings.
  // Values before the first section header belong to the root section ""
  // (machine-wide policies are kept there, like in the root of the
  // HKLM\Software\Martin Prikryl\WinSCP 2 key).
  bool Load(const QString & FileName);
  bool LoadFromData(const QByteArray & Data);
  // Atomic (QSaveFile); a newly created file is readable by the owner only,
  // as it contains (obfuscated or encrypted) passwords
  bool Save(const QString & FileName) const;
  QByteArray SaveToData() const;
  void Clear();
  bool IsEmpty() const { return FSections.isEmpty(); }

  QStringList Sections() const;
  bool SectionExists(const QString & Section) const;
  void EraseSection(const QString & Section);
  // Erases the section and all its subsections ("Section\...")
  void EraseSectionTree(const QString & Section);
  // A section with the name exists or it has subsections
  bool KeyExists(const QString & Section) const;
  // Creates an empty section, when it does not exist yet
  void AddSection(const QString & Section);
  QStringList Keys(const QString & Section) const;

  // Raw (no munging) access
  QString ReadRaw(const QString & Section, const QString & Key, const QString & Default = QString(), bool * Found = nullptr) const;
  void WriteRaw(const QString & Section, const QString & Key, const QString & Value);
  void DeleteKey(const QString & Section, const QString & Key);

  // Copies all sections and values of Other over this file
  void Overlay(const IniFile & Other);

  // Change set between two states of a file, to be applied to a third one.
  // Used by Configuration::Save to merge the changes made by this instance
  // into the file as it is on disk now (other instances may have saved meanwhile).
  struct Change
  {
    enum TKind { ckWrite, ckDelete, ckAddSection, ckEraseSection };
    TKind Kind;
    QString Section;
    QString Key;
    QString Value;
  };
  // Equivalent (optional) can declare two different raw values the same
  // (e.g. the same password encrypted twice with a random padding)
  using EquivalentFunc = std::function<bool(const QString & Section, const QString & Key, const QString & From, const QString & To)>;
  static QList<Change> Diff(const IniFile & From, const IniFile & To, const EquivalentFunc & Equivalent = EquivalentFunc());
  void Apply(const QList<Change> & Changes);

private:
  struct Section
  {
    QString Name;
    QList<QPair<QString, QString>> Values;
  };
  QList<Section> FSections;
  // lower-cased section name -> index to FSections (sections are case insensitive)
  mutable QHash<QString, int> FIndex;
  mutable bool FIndexValid = false;
  // last looked up section (IniStorage accesses the same section repeatedly)
  mutable QString FLastName;
  mutable int FLastIndex = -1;

  void LoadRegistryExport(const QString & Text);
  void ParseText(const QString & Text);

  const Section * Find(const QString & Name) const;
  Section * Find(const QString & Name);
  Section * FindOrAdd(const QString & Name);
};

// Hierarchical view over an IniFile, like THierarchicalStorage:
// sections are named "Key\SubKey", values are munged as in WinSCP.
class IniStorage
{
public:
  explicit IniStorage(IniFile * File);

  // Opens one key level; a backslash in SubKey is munged (%5C), as it may
  // be part of a site name (TSessionData::MakeValidName)
  bool OpenSubKey(const QString & SubKey, bool CanCreate);
  // THierarchicalStorage::OpenSubKeyPath: opens all levels of a
  // backslash-separated path ("Configuration\Interface"); all or nothing
  bool OpenSubKeyPath(const QString & KeyPath, bool CanCreate);
  // Closes the last open: one level after OpenSubKey, all levels of the
  // path after OpenSubKeyPath
  void CloseSubKey();
  void CloseSubKeyPath() { CloseSubKey(); }
  QStringList GetSubKeyNames() const;   // unmunged names of direct children
  QStringList GetValueNames() const;
  bool HasSubKey(const QString & SubKey) const;
  void RecursiveDeleteSubKey(const QString & SubKey);
  bool ValueExists(const QString & Name) const;
  void DeleteValue(const QString & Name);

  QString ReadString(const QString & Name, const QString & Default) const;
  QString ReadStringRaw(const QString & Name, const QString & Default) const;
  int ReadInteger(const QString & Name, int Default) const;
  bool ReadBool(const QString & Name, bool Default) const;

  void WriteString(const QString & Name, const QString & Value);
  void WriteStringRaw(const QString & Name, const QString & Value);
  void WriteInteger(const QString & Name, int Value);
  void WriteBool(const QString & Name, bool Value);

  QString CurrentSection() const;

private:
  IniFile * FFile;
  QStringList FKeyHistory; // munged, one entry per level
  QList<int> FOpenLevels;  // levels opened by each OpenSubKey/OpenSubKeyPath call
  QString FCurrentSection; // FKeyHistory joined (hot path: every value access)

  void UpdateCurrentSection();
};
