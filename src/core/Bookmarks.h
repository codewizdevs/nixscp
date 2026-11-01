// Port of WinSCP source/core/Bookmarks.{h,cpp} (TBookmarks, TBookmarkList, TBookmark)
// and the bookmark part of windows/WinConfiguration.cpp (Bookmarks key,
// UseSharedBookmarks).
//
// Storage is identical to WinSCP.ini:
//   [Configuration\Bookmarks\Local\<session key>[\<folder>]]   <name>=<local directory>
//   [Configuration\Bookmarks\Remote\<session key>[\<folder>]]  <name>=<remote directory>
//   [Configuration\Bookmarks\ShortCuts\<session key>[\<folder>]] <name>=<VCL TShortCut>
//   [Configuration\Bookmarks\Options\<session key>]             OpenedNodes=<comma text>
// Shared bookmarks use the session key "_!_shared".
// Shortcuts are kept in VCL TShortCut format (virtual key | scShift 0x2000 |
// scCtrl 0x4000 | scAlt 0x8000) so the files stay interchangeable; the GUI
// converts them (LocationProfilesDialog::ShortCutToKeySequence).
#pragma once

#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

class IniFile;
class IniStorage;
class SessionData;

class Bookmark
{
public:
  QString Name;
  QString Local;
  QString Remote;
  QString Node;           // folder ("" = top level)
  int ShortCut = 0;       // VCL TShortCut, 0 = none

  // TBookmark::GetSideDirectory
  QString SideDirectory(bool Remote) const { return Remote ? this->Remote : Local; }
  bool operator==(const Bookmark & Other) const = default;
};

// TBookmarkList: ordered list, (Node, Name) unique (case insensitive)
class BookmarkList
{
public:
  int Count() const { return FBookmarks.size(); }
  const Bookmark & At(int Index) const { return FBookmarks.at(Index); }
  const QList<Bookmark> & Items() const { return FBookmarks; }

  void Clear();
  // Throw ExtException ("Location Profile with name '%s' already exists.") on duplicates
  void Add(const Bookmark & ABookmark);
  void Insert(int Index, const Bookmark & ABookmark);
  void Delete(int Index);
  // Replaces bookmark at Index; throws on duplicate (Node, Name)
  void Update(int Index, const Bookmark & ABookmark);
  // TStringList::Move semantics
  void Move(int OldIndex, int NewIndex);
  int IndexOf(const QString & Node, const QString & Name) const;   // -1 = not found
  const Bookmark * FindByName(const QString & Node, const QString & Name) const;
  const Bookmark * FindByShortCut(int ShortCut) const;
  QStringList Folders() const;      // distinct non-empty nodes, sorted (case insensitive)
  QList<int> ShortCuts() const;     // all assigned shortcuts

  bool NodeOpened(const QString & Node) const;
  void SetNodeOpened(const QString & Node, bool Opened);
  QStringList OpenedNodes() const { return FOpenedNodes; }

  bool Modified() const { return FModified; }
  void SetModified(bool Value) { FModified = Value; }

  void LoadOptions(IniStorage & Storage);
  void SaveOptions(IniStorage & Storage) const;

private:
  QList<Bookmark> FBookmarks;
  QStringList FOpenedNodes;
  bool FModified = false;

  void CheckDuplicate(const Bookmark & ABookmark, int Except) const;
};

// TBookmarks: all bookmark lists, keyed by session key
class Bookmarks
{
public:
  static const QString SharedKey;    // "_!_shared" (TNamedObjectList::HiddenPrefix + "shared")

  // Configuration\Bookmarks of the file
  void Load(IniFile & File);
  // Writes the modified lists only (All = every list), as TBookmarks::Save
  void Save(IniFile & File, bool All = false);
  void Clear() { FLists.clear(); }
  void ModifyAll(bool Modify);

  // Copy of the list (empty when there are no bookmarks for the key)
  BookmarkList List(const QString & Key) const;
  void SetList(const QString & Key, const BookmarkList & List);   // marks it modified
  QStringList Keys() const { return FLists.keys(); }
  BookmarkList SharedList() const { return List(SharedKey); }
  void SetSharedList(const BookmarkList & AList) { SetList(SharedKey, AList); }

  // TSessionData::GetSessionKey: "user@host" + ":port" for non-default port
  static QString SessionKey(const SessionData & Data);

  // TWinConfiguration::UseSharedBookmarks (Configuration\Interface\UseSharedBookmarks)
  static bool UseSharedBookmarks(IniFile & File);
  static void SetUseSharedBookmarks(IniFile & File, bool Value);

  // Convenience for the GUI: bookmarks of the session (and shared ones) from
  // Configuration::Instance(), e.g. for a "bookmarks" menu or shortcut lookup
  static BookmarkList ConfiguredList(const QString & Key);
  // Shared bookmarks of Configuration::Instance() (WinConfiguration->SharedBookmarks)
  static BookmarkList ConfiguredSharedList() { return ConfiguredList(SharedKey); }

  // TScpCommanderForm::OpenBookmark / TCustomScpExplorerForm::OpenBookmark:
  // directories to open for a bookmark chosen by its shortcut. With location
  // profiles and a session (UseLocationProfiles && Connected) both panels
  // change (empty = keep); otherwise only the panel of the active side
  // (Remote) changes to the bookmark's directory of that side, and false is
  // returned when the bookmark has no directory for the side.
  static bool OpenBookmarkTarget(const Bookmark & ABookmark, bool UseLocationProfiles, bool Connected, bool Remote,
    QString & LocalDirectory, QString & RemoteDirectory);

private:
  QMap<QString, BookmarkList> FLists;

  void LoadLevel(IniStorage & Storage, const QString & Key, int Index, BookmarkList & List);
};
