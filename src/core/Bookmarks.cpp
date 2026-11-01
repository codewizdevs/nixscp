// Port of WinSCP source/core/Bookmarks.cpp and the bookmark part of
// windows/WinConfiguration.cpp
#include "core/Bookmarks.h"
#include "core/Configuration.h"
#include "core/FileSystem.h"
#include "core/IniStorage.h"
#include "core/SessionData.h"

#include <algorithm>

const QString Bookmarks::SharedKey = QStringLiteral("_!_shared");

static const QString ConfigurationKey = QStringLiteral("Configuration");
static const QString BookmarksKey = QStringLiteral("Bookmarks");
static const QString InterfaceKey = QStringLiteral("Interface");
// TBookmarks::Keys
static const QString BookmarkKeys[] = { QStringLiteral("Local"), QStringLiteral("Remote"),
  QStringLiteral("ShortCuts"), QStringLiteral("Options") };

static bool IsNumber(const QString & Str)
{
  bool Ok = false;
  Str.toLongLong(&Ok);
  return Ok;
}

// TStrings::CommaText
static QString ToCommaText(const QStringList & List)
{
  QStringList Parts;
  for (const QString & Item : List)
  {
    bool Quote = Item.isEmpty();
    for (QChar C : Item)
    {
      if ((C.unicode() <= ' ') || (C == QLatin1Char(',')) || (C == QLatin1Char('"')))
      {
        Quote = true;
        break;
      }
    }
    if (Quote)
    {
      QString Escaped = Item;
      Escaped.replace(QStringLiteral("\""), QStringLiteral("\"\""));
      Parts.append(QLatin1Char('"') + Escaped + QLatin1Char('"'));
    }
    else
    {
      Parts.append(Item);
    }
  }
  return Parts.join(QLatin1Char(','));
}

static QStringList FromCommaText(const QString & Text)
{
  QStringList Result;
  int I = 0;
  int Len = Text.size();
  auto SkipBlanks = [&]()
  {
    while ((I < Len) && (Text[I].unicode() <= ' ') && (Text[I].unicode() > 0))
    {
      I++;
    }
  };
  SkipBlanks();
  while (I < Len)
  {
    QString Item;
    if (Text[I] == QLatin1Char('"'))
    {
      I++;
      while (I < Len)
      {
        if (Text[I] == QLatin1Char('"'))
        {
          if ((I + 1 < Len) && (Text[I + 1] == QLatin1Char('"')))
          {
            Item += QLatin1Char('"');
            I += 2;
            continue;
          }
          I++;
          break;
        }
        Item += Text[I++];
      }
    }
    else
    {
      while ((I < Len) && (Text[I].unicode() > ' ') && (Text[I] != QLatin1Char(',')))
      {
        Item += Text[I++];
      }
    }
    Result.append(Item);
    SkipBlanks();
    if ((I < Len) && (Text[I] == QLatin1Char(',')))
    {
      I++;
      SkipBlanks();
    }
  }
  return Result;
}

//---------------------------------------------------------------------------
// BookmarkList
//---------------------------------------------------------------------------

void BookmarkList::Clear()
{
  FBookmarks.clear();
  FOpenedNodes.clear();
}

void BookmarkList::CheckDuplicate(const Bookmark & ABookmark, int Except) const
{
  int Index = IndexOf(ABookmark.Node, ABookmark.Name);
  if ((Index >= 0) && (Index != Except))
  {
    // DUPLICATE_BOOKMARK
    throw ExtException(QStringLiteral("Location Profile with name '%1' already exists.").arg(ABookmark.Name));
  }
}

void BookmarkList::Add(const Bookmark & ABookmark)
{
  Insert(FBookmarks.size(), ABookmark);
}

void BookmarkList::Insert(int Index, const Bookmark & ABookmark)
{
  CheckDuplicate(ABookmark, -1);
  FModified = true;
  FBookmarks.insert(std::clamp(Index, 0, static_cast<int>(FBookmarks.size())), ABookmark);
}

void BookmarkList::Delete(int Index)
{
  FModified = true;
  FBookmarks.removeAt(Index);
}

void BookmarkList::Update(int Index, const Bookmark & ABookmark)
{
  if (FBookmarks.at(Index) == ABookmark)
  {
    return;
  }
  CheckDuplicate(ABookmark, Index);
  FModified = true;
  FBookmarks[Index] = ABookmark;
}

void BookmarkList::Move(int OldIndex, int NewIndex)
{
  if (OldIndex != NewIndex)
  {
    FModified = true;
    FBookmarks.move(OldIndex, NewIndex);
  }
}

int BookmarkList::IndexOf(const QString & Node, const QString & Name) const
{
  for (int Index = 0; Index < FBookmarks.size(); Index++)
  {
    const Bookmark & B = FBookmarks.at(Index);
    if ((B.Node.compare(Node, Qt::CaseInsensitive) == 0) && (B.Name.compare(Name, Qt::CaseInsensitive) == 0))
    {
      return Index;
    }
  }
  return -1;
}

const Bookmark * BookmarkList::FindByName(const QString & Node, const QString & Name) const
{
  int Index = IndexOf(Node, Name);
  return (Index >= 0) ? &FBookmarks.at(Index) : nullptr;
}

const Bookmark * BookmarkList::FindByShortCut(int ShortCut) const
{
  for (const Bookmark & B : FBookmarks)
  {
    if ((ShortCut != 0) && (B.ShortCut == ShortCut))
    {
      return &B;
    }
  }
  return nullptr;
}

QStringList BookmarkList::Folders() const
{
  QStringList Result;
  for (const Bookmark & B : FBookmarks)
  {
    if (!B.Node.isEmpty() && !Result.contains(B.Node, Qt::CaseInsensitive))
    {
      Result.append(B.Node);
    }
  }
  std::sort(Result.begin(), Result.end(), [](const QString & A, const QString & B)
  {
    return A.compare(B, Qt::CaseInsensitive) < 0;
  });
  return Result;
}

QList<int> BookmarkList::ShortCuts() const
{
  QList<int> Result;
  for (const Bookmark & B : FBookmarks)
  {
    if (B.ShortCut != 0)
    {
      Result.append(B.ShortCut);
    }
  }
  return Result;
}

bool BookmarkList::NodeOpened(const QString & Node) const
{
  return FOpenedNodes.contains(Node, Qt::CaseInsensitive);
}

void BookmarkList::SetNodeOpened(const QString & Node, bool Opened)
{
  if (NodeOpened(Node) != Opened)
  {
    if (Opened)
    {
      FOpenedNodes.append(Node);
      std::sort(FOpenedNodes.begin(), FOpenedNodes.end(), [](const QString & A, const QString & B)
      {
        return A.compare(B, Qt::CaseInsensitive) < 0;
      });
    }
    else
    {
      for (int Index = 0; Index < FOpenedNodes.size(); Index++)
      {
        if (FOpenedNodes.at(Index).compare(Node, Qt::CaseInsensitive) == 0)
        {
          FOpenedNodes.removeAt(Index);
          break;
        }
      }
    }
    FModified = true;
  }
}

void BookmarkList::LoadOptions(IniStorage & Storage)
{
  FOpenedNodes = FromCommaText(Storage.ReadString(QStringLiteral("OpenedNodes"), QString()));
}

void BookmarkList::SaveOptions(IniStorage & Storage) const
{
  Storage.WriteString(QStringLiteral("OpenedNodes"), ToCommaText(FOpenedNodes));
}

//---------------------------------------------------------------------------
// Bookmarks
//---------------------------------------------------------------------------

void Bookmarks::Load(IniFile & File)
{
  IniStorage Storage(&File);
  if (Storage.OpenSubKey(ConfigurationKey, false))
  {
    if (Storage.OpenSubKey(BookmarksKey, false))
    {
      for (int I = 0; I <= 3; I++)
      {
        if (Storage.OpenSubKey(BookmarkKeys[I], false))
        {
          const QStringList Keys = Storage.GetSubKeyNames();
          for (const QString & Key : Keys)
          {
            if (Storage.OpenSubKey(Key, false))
            {
              BookmarkList & List = FLists[Key];
              if (I < 3)
              {
                LoadLevel(Storage, QString(), I, List);
              }
              else
              {
                List.LoadOptions(Storage);
              }
              Storage.CloseSubKey();
            }
          }
          Storage.CloseSubKey();
        }
      }
      Storage.CloseSubKey();
    }
    Storage.CloseSubKey();
  }
  ModifyAll(false);
}

void Bookmarks::LoadLevel(IniStorage & Storage, const QString & Key, int Index, BookmarkList & List)
{
  const QStringList Names = Storage.GetValueNames();
  for (QString Name : Names)
  {
    bool IsDirectory = (Index == 0) || (Index == 1);
    QString Directory;
    int ShortCut = 0;
    if (IsDirectory)
    {
      Directory = Storage.ReadString(Name, QString());
    }
    else
    {
      ShortCut = Storage.ReadInteger(Name, 0);
    }
    if (IsNumber(Name))
    {
      // legacy format: bookmarks without names
      Name = Directory;
    }
    if (Name.isEmpty())
    {
      continue;
    }
    int Existing = List.IndexOf(Key, Name);
    Bookmark B;
    if (Existing >= 0)
    {
      B = List.At(Existing);
    }
    else
    {
      B.Node = Key;
      B.Name = Name;
    }
    switch (Index)
    {
      case 0: B.Local = Directory; break;
      case 1: B.Remote = Directory; break;
      case 2: B.ShortCut = ShortCut; break;
    }
    if (Existing >= 0)
    {
      List.Update(Existing, B);
    }
    else
    {
      List.Add(B);
    }
  }

  const QStringList SubKeys = Storage.GetSubKeyNames();
  for (const QString & SubKey : SubKeys)
  {
    if (Storage.OpenSubKey(SubKey, false))
    {
      LoadLevel(Storage, Key + (Key.isEmpty() ? QString() : QStringLiteral("/")) + SubKey, Index, List);
      Storage.CloseSubKey();
    }
  }
}

void Bookmarks::Save(IniFile & File, bool All)
{
  IniStorage Storage(&File);
  if (Storage.OpenSubKey(ConfigurationKey, true))
  {
    if (Storage.OpenSubKey(BookmarksKey, true))
    {
      for (int I = 0; I <= 3; I++)
      {
        if (Storage.OpenSubKey(BookmarkKeys[I], true))
        {
          for (auto It = FLists.cbegin(); It != FLists.cend(); ++It)
          {
            const BookmarkList & List = It.value();
            if (!All && !List.Modified())
            {
              continue;
            }
            Storage.RecursiveDeleteSubKey(It.key());
            if (Storage.OpenSubKey(It.key(), true))
            {
              if (I < 3)
              {
                for (const Bookmark & B : List.Items())
                {
                  // avoid creating empty subfolder if there's no shortcut
                  if ((I == 2) && (B.ShortCut == 0))
                  {
                    continue;
                  }
                  bool HasNode = !B.Node.isEmpty();
                  if (!HasNode || Storage.OpenSubKey(B.Node, true))
                  {
                    switch (I)
                    {
                      case 0: Storage.WriteString(B.Name, B.Local); break;
                      case 1: Storage.WriteString(B.Name, B.Remote); break;
                      case 2: Storage.WriteInteger(B.Name, B.ShortCut); break;
                    }
                    if (HasNode)
                    {
                      Storage.CloseSubKey();
                    }
                  }
                }
              }
              else
              {
                List.SaveOptions(Storage);
              }
              Storage.CloseSubKey();
            }
          }
          Storage.CloseSubKey();
        }
      }
      Storage.CloseSubKey();
    }
    Storage.CloseSubKey();
  }
  if (!All)
  {
    ModifyAll(false);
  }
}

void Bookmarks::ModifyAll(bool Modify)
{
  for (auto It = FLists.begin(); It != FLists.end(); ++It)
  {
    It.value().SetModified(Modify);
  }
}

BookmarkList Bookmarks::List(const QString & Key) const
{
  return FLists.value(Key);
}

void Bookmarks::SetList(const QString & Key, const BookmarkList & AList)
{
  BookmarkList & List = FLists[Key];
  List = AList;
  List.SetModified(true);
}

QString Bookmarks::SessionKey(const SessionData & Data)
{
  QString Result = QStringLiteral("%1@%2").arg(Data.UserName, Data.HostName);
  if (Data.PortNumber != SessionData::DefaultPort(Data.FSProtocol, Data.Ftps))
  {
    Result += QStringLiteral(":%1").arg(Data.PortNumber);
  }
  return Result;
}

bool Bookmarks::UseSharedBookmarks(IniFile & File)
{
  IniStorage Storage(&File);
  bool Result = false;
  if (Storage.OpenSubKey(ConfigurationKey, false) && Storage.OpenSubKey(InterfaceKey, false))
  {
    Result = Storage.ReadBool(QStringLiteral("UseSharedBookmarks"), false);
  }
  return Result;
}

void Bookmarks::SetUseSharedBookmarks(IniFile & File, bool Value)
{
  IniStorage Storage(&File);
  if (Storage.OpenSubKey(ConfigurationKey, true) && Storage.OpenSubKey(InterfaceKey, true))
  {
    Storage.WriteBool(QStringLiteral("UseSharedBookmarks"), Value);
  }
}

BookmarkList Bookmarks::ConfiguredList(const QString & Key)
{
  Bookmarks All;
  All.Load(Configuration::Instance().Ini());
  return All.List(Key);
}

bool Bookmarks::OpenBookmarkTarget(const Bookmark & ABookmark, bool UseLocationProfiles, bool Connected, bool Remote,
  QString & LocalDirectory, QString & RemoteDirectory)
{
  LocalDirectory.clear();
  RemoteDirectory.clear();
  if (UseLocationProfiles && Connected)
  {
    // DoOpenBookmark(Bookmark->Local, Bookmark->Remote)
    LocalDirectory = ABookmark.Local;
    RemoteDirectory = ABookmark.Remote;
    return !LocalDirectory.isEmpty() || !RemoteDirectory.isEmpty();
  }
  // BookmarkSide = IsLocalBrowserMode() ? osLocal : Side
  bool Side = Remote && Connected;
  QString Path = ABookmark.SideDirectory(Side);
  if (Path.isEmpty())
  {
    return false;
  }
  (Side ? RemoteDirectory : LocalDirectory) = Path;
  return true;
}
