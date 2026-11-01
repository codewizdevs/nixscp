// Port of the list part of WinSCP source/components/DirView.cpp and UnixDirView.cpp,
// packages/filemng/DirViewInt.pas (CompareFile), packages/filemng/BaseUtils.pas
// (FormatBytes/FormatPanelBytes), core/Common.cpp (CompareLogicalText) and
// windows/WinConfiguration.cpp (TFileColorData).
#include "gui/FileListModel.h"
#include "gui/Translator.h"

#include <QCollator>
#include <QCollatorSortKey>
#include <QDir>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLocale>
#include <QMimeDatabase>
#include <QPalette>

#include <algorithm>
#include <cmath>
#include <functional>

//---------------------------------------------------------------------------
// TFileColorData
//---------------------------------------------------------------------------

// TStringList::CommaText parsing/formatting (Delphi rules: items separated by
// commas or spaces, quoted with " when they contain a delimiter or quote)
static QStringList FromCommaText(const QString & Text)
{
  QStringList Result;
  int Index = 0;
  int Length = Text.length();
  while (Index < Length)
  {
    while ((Index < Length) && (Text.at(Index) <= QLatin1Char(' ')))
    {
      Index++;
    }
    if (Index >= Length)
    {
      break;
    }
    QString Item;
    if (Text.at(Index) == QLatin1Char('"'))
    {
      Index++;
      while (Index < Length)
      {
        if (Text.at(Index) == QLatin1Char('"'))
        {
          if ((Index + 1 < Length) && (Text.at(Index + 1) == QLatin1Char('"')))
          {
            Item += QLatin1Char('"');
            Index += 2;
            continue;
          }
          Index++;
          break;
        }
        Item += Text.at(Index);
        Index++;
      }
    }
    else
    {
      while ((Index < Length) && (Text.at(Index) > QLatin1Char(' ')) && (Text.at(Index) != QLatin1Char(',')))
      {
        Item += Text.at(Index);
        Index++;
      }
    }
    Result.append(Item);
    while ((Index < Length) && (Text.at(Index) <= QLatin1Char(' ')))
    {
      Index++;
    }
    if ((Index < Length) && (Text.at(Index) == QLatin1Char(',')))
    {
      Index++;
    }
  }
  return Result;
}

static QString ToCommaText(const QStringList & List)
{
  QStringList Items;
  for (const QString & Item : List)
  {
    bool Quote = Item.isEmpty();
    for (QChar C : Item)
    {
      if ((C <= QLatin1Char(' ')) || (C == QLatin1Char(',')) || (C == QLatin1Char('"')))
      {
        Quote = true;
        break;
      }
    }
    if (Quote)
    {
      QString Escaped = Item;
      Escaped.replace(QLatin1Char('"'), QStringLiteral("\"\""));
      Items.append(QStringLiteral("\"%1\"").arg(Escaped));
    }
    else
    {
      Items.append(Item);
    }
  }
  return Items.join(QLatin1Char(','));
}

QList<FileColorRule> FileColorRule::LoadList(const QString & Str)
{
  QList<FileColorRule> Result;
  for (const QString & Item : FromCommaText(Str))
  {
    int Colon = Item.indexOf(QLatin1Char(':'));
    if (Colon <= 0)
    {
      continue;
    }
    bool Ok = false;
    // TColor is $00BBGGRR
    uint Value = Item.left(Colon).toUInt(&Ok, 16);
    if (!Ok)
    {
      continue;
    }
    FileColorRule Rule;
    Rule.Color = QColor(Value & 0xFF, (Value >> 8) & 0xFF, (Value >> 16) & 0xFF);
    Rule.Mask = Item.mid(Colon + 1);
    try
    {
      Rule.Masks = FileMasks(Rule.Mask);
    }
    catch (const FileMasksException &)
    {
      continue;
    }
    Result.append(Rule);
  }
  return Result;
}

QString FileColorRule::SaveList(const QList<FileColorRule> & List)
{
  QStringList Items;
  for (const FileColorRule & Rule : List)
  {
    uint Value = static_cast<uint>(Rule.Color.red()) | (static_cast<uint>(Rule.Color.green()) << 8) |
      (static_cast<uint>(Rule.Color.blue()) << 16);
    Items.append(QString::number(Value, 16).toUpper().rightJustified(6, QLatin1Char('0')) + QLatin1Char(':') + Rule.Mask);
  }
  return ToCommaText(Items);
}

//---------------------------------------------------------------------------
// Icons and type names by extension (shared by all panels)
//---------------------------------------------------------------------------

namespace
{

struct MimeInfo
{
  QString IconName;
  QString GenericIconName;
  QString Comment;
  bool Default = true;
};

// Key covering the last two extensions (".tar.gz", ".txt"); names without an
// extension are matched by their whole name ("Makefile")
QString MimeKey(const QString & FileName)
{
  int Dot = FileName.lastIndexOf(QLatin1Char('.'));
  if (Dot <= 0)
  {
    return QLatin1Char('\x01') + FileName.toLower();
  }
  int Dot2 = FileName.lastIndexOf(QLatin1Char('.'), Dot - 1);
  return FileName.mid((Dot2 > 0) ? Dot2 : Dot).toLower();
}

const MimeInfo & MimeInfoFor(const QString & FileName)
{
  static QHash<QString, MimeInfo> Cache;
  QString Key = MimeKey(FileName);
  auto It = Cache.constFind(Key);
  if (It != Cache.constEnd())
  {
    return It.value();
  }
  if (Cache.size() > 50000)
  {
    Cache.clear();
  }
  static QMimeDatabase Db;
  QMimeType Mime = Db.mimeTypeForFile(FileName, QMimeDatabase::MatchExtension);
  MimeInfo Info;
  Info.Default = Mime.isDefault();
  if (!Info.Default)
  {
    Info.IconName = Mime.iconName();
    Info.GenericIconName = Mime.genericIconName();
    Info.Comment = Mime.comment();
  }
  return Cache.insert(Key, Info).value();
}

QIcon CachedIcon(const QString & Key, const std::function<QIcon()> & Create)
{
  static QHash<QString, QIcon> Cache;
  auto It = Cache.constFind(Key);
  if (It != Cache.constEnd())
  {
    return It.value();
  }
  QIcon Icon = Create();
  Cache.insert(Key, Icon);
  return Icon;
}

QFileIconProvider & IconProvider()
{
  static QFileIconProvider Provider;
  return Provider;
}

} // namespace

QIcon FileListModel::IconFor(const RemoteFile & File)
{
  if (File.IsParentDirectory)
  {
    return CachedIcon(QStringLiteral("\x01parent"), []()
    {
      return QIcon::fromTheme(QStringLiteral("go-up"), IconProvider().icon(QFileIconProvider::Folder));
    });
  }
  if (File.IsDirectory)
  {
    return CachedIcon(QStringLiteral("\x01folder"), []()
    {
      return QIcon::fromTheme(QStringLiteral("folder"), IconProvider().icon(QFileIconProvider::Folder));
    });
  }
  const MimeInfo & Info = MimeInfoFor(File.FileName);
  if (Info.Default)
  {
    if ((File.Permissions & 0111) && !File.FileName.contains(QLatin1Char('.')))
    {
      return CachedIcon(QStringLiteral("\x01exec"), []()
      {
        return QIcon::fromTheme(QStringLiteral("application-x-executable"), IconProvider().icon(QFileIconProvider::File));
      });
    }
    return CachedIcon(QStringLiteral("\x01file"), []()
    {
      return QIcon::fromTheme(QStringLiteral("text-x-generic"), IconProvider().icon(QFileIconProvider::File));
    });
  }
  return CachedIcon(Info.IconName, [&Info]()
  {
    QIcon Icon = QIcon::fromTheme(Info.IconName);
    if (Icon.isNull())
    {
      Icon = QIcon::fromTheme(Info.GenericIconName);
    }
    if (Icon.isNull())
    {
      Icon = IconProvider().icon(QFileIconProvider::File);
    }
    return Icon;
  });
}

//---------------------------------------------------------------------------
// FileListModel
//---------------------------------------------------------------------------

FileListModel::FileListModel(bool Remote, QObject * Parent) :
  QAbstractTableModel(Parent),
  FRemote(Remote)
{
  FParentEntry.FileName = QStringLiteral("..");
  FParentEntry.FileType = RemoteFile::Directory;
  FParentEntry.IsDirectory = true;
  FParentEntry.IsParentDirectory = true;
  // column captions and type names follow the UI language
  connect(Translator::Instance(), &Translator::LanguageChanged, this, [this]()
  {
    emit headerDataChanged(Qt::Horizontal, 0, ColCount - 1);
    if (rowCount() > 0)
    {
      emit dataChanged(index(0, 0), index(rowCount() - 1, ColCount - 1));
    }
  });
}

QString FileListModel::ColumnCaption(int Column)
{
  // Captions of TDirView / TUnixDirView columns (DirViewColProperties.pas,
  // UnixDirViewColProperties.pas). Local "Attr" shows POSIX rights on Linux.
  switch (Column)
  {
    case ColName: return Translator::Tr(QStringLiteral("Name"));
    case ColSize: return Translator::Tr(QStringLiteral("Size"));
    case ColType: return Translator::Tr(QStringLiteral("Type"));
    case ColChanged: return Translator::Tr(QStringLiteral("Date modified"));
    case ColRights: return Translator::Tr(QStringLiteral("Rights"));
    case ColOwner: return Translator::Tr(QStringLiteral("Owner"));
    case ColGroup: return Translator::Tr(QStringLiteral("Group"));
    case ColExt: return Translator::Tr(QStringLiteral("Ext"));
    case ColLinkTarget: return Translator::Tr(QStringLiteral("Link target"));
  }
  return QString();
}

bool FileListModel::DefaultColumnVisible(int Column, bool Remote)
{
  // ScpCommanderRemotePanelDirViewParamsDefault / ScpCommanderLocalPanelDirViewParamsDefault
  if (Remote)
  {
    return (Column == ColName) || (Column == ColSize) || (Column == ColChanged) ||
      (Column == ColRights) || (Column == ColOwner);
  }
  else
  {
    return (Column == ColName) || (Column == ColSize) || (Column == ColType) || (Column == ColChanged);
  }
}

int FileListModel::DefaultColumnWidth(int Column, bool Remote)
{
  // Widths of the WinSCP defaults (96 DPI)
  switch (Column)
  {
    case ColName: return 150;
    case ColSize: return 70;
    case ColType: return Remote ? 125 : 120;
    case ColChanged: return 150;
    case ColRights: return Remote ? 79 : 55;
    case ColOwner: return 62;
    case ColGroup: return 55;
    case ColExt: return Remote ? 20 : 55;
    case ColLinkTarget: return 150;
  }
  return 80;
}

QString FileListModel::FormatBytes(qint64 Bytes)
{
  return FormatPanelBytes(Bytes, fbShort);
}

QString FileListModel::FormatPanelBytes(qint64 Bytes, TFormatBytesStyle Style)
{
  // BaseUtils.pas FormatBytes(Bytes, Style, UseUnitsForBytes = Style <> fbNone)
  QLocale Locale;
  if ((Style == fbNone) || ((Style == fbShort) && (Bytes < 1024)))
  {
    QString Result = Locale.toString(Bytes);
    if (Style != fbNone)
    {
      Result += QStringLiteral(" B");
    }
    return Result;
  }
  if (Style == fbKilobytes)
  {
    qint64 Value = Bytes / 1024;
    if ((Bytes % 1024) > 0)
    {
      Value++;
    }
    return QStringLiteral("%1 KB").arg(Locale.toString(Value));
  }
  qint64 Order;
  const char * Unit;
  if (Bytes < qint64(1000) * 1024)
  {
    Order = 1024;
    Unit = "KB";
  }
  else if (Bytes < qint64(1000) * 1024 * 1024)
  {
    Order = 1024 * 1024;
    Unit = "MB";
  }
  else
  {
    Order = qint64(1024) * 1024 * 1024;
    Unit = "GB";
  }
  double Value = static_cast<double>(Bytes) / static_cast<double>(Order);
  QString Number;
  if (Value >= 100)
  {
    Number = Locale.toString(std::floor(Value), 'f', 0);
  }
  else if (Value >= 10)
  {
    Number = Locale.toString(std::floor(Value * 10) / 10, 'f', 1);
  }
  else
  {
    Number = Locale.toString(std::floor(Value * 100) / 100, 'f', 2);
  }
  return QStringLiteral("%1 %2").arg(Number, QLatin1String(Unit));
}

QString FileListModel::FormatSizeKB(qint64 Bytes)
{
  return FormatPanelBytes(Bytes, fbKilobytes);
}

QString FileListModel::FormatModification(const QDateTime & Modification)
{
  if (!Modification.isValid())
  {
    return QString();
  }
  // FormatDateTime("ddddd tt"): short date and long time (with seconds)
  static QString Format;
  static QString FormatLocale;
  QLocale Locale;
  if (Format.isEmpty() || (FormatLocale != Locale.name()))
  {
    QString TimeFormat = Locale.timeFormat(QLocale::ShortFormat);
    if (!TimeFormat.contains(QLatin1Char('s')))
    {
      int Minutes = TimeFormat.indexOf(QStringLiteral("mm"));
      if (Minutes >= 0)
      {
        TimeFormat.insert(Minutes + 2, QStringLiteral(":ss"));
      }
    }
    Format = Locale.dateFormat(QLocale::ShortFormat) + QLatin1Char(' ') + TimeFormat;
    FormatLocale = Locale.name();
  }
  return Locale.toString(Modification.toLocalTime(), Format);
}

bool FileListModel::IsAtRoot() const
{
  return FDirectory.isEmpty() || (FDirectory == QStringLiteral("/"));
}

int FileListModel::FileIndex(int Row) const
{
  if ((Row < 0) || (Row >= static_cast<int>(FRows.size())))
  {
    return -1;
  }
  return FRows[Row];
}

const RemoteFile & FileListModel::File(int Row) const
{
  static const RemoteFile Empty;
  if ((Row < 0) || (Row >= static_cast<int>(FRows.size())))
  {
    return Empty;
  }
  int Index = FRows[Row];
  return (Index < 0) ? FParentEntry : FAllFiles.at(Index);
}

void FileListModel::SetFiles(const QString & Directory, const RemoteFileList & Files)
{
  QSet<QString> KeepMarked;
  if ((Directory == FDirectory) && (FMarkedCount > 0))
  {
    const QStringList Names = MarkedNames();
    KeepMarked = QSet<QString>(Names.begin(), Names.end());
  }
  // Calculated directory sizes survive a reload of the same directory as long
  // as the directory entry is unchanged (TDirViewInt reload updates changed items only)
  QHash<QString, QPair<QDateTime, qint64>> KeepCalculated;
  if (Directory == FDirectory)
  {
    for (int Index = 0; Index < static_cast<int>(FCalculatedSizes.size()); Index++)
    {
      if (FCalculatedSizes[Index] >= 0)
      {
        const RemoteFile & File = FAllFiles.at(Index);
        KeepCalculated.insert(File.FileName, qMakePair(File.Modification, FCalculatedSizes[Index]));
      }
    }
  }
  beginResetModel();
  FDirectory = Directory;
  FAllFiles = Files;
  // "." and ".." of the listing are never shown (the panel adds its own "..")
  for (int Index = FAllFiles.size() - 1; Index >= 0; Index--)
  {
    const QString & Name = FAllFiles.at(Index).FileName;
    if ((Name == QStringLiteral(".")) || (Name == QStringLiteral("..")) || Name.isEmpty())
    {
      FAllFiles.removeAt(Index);
    }
  }
  int Count = FAllFiles.size();
  FNameIndex.clear();
  FNameIndex.reserve(Count);
  for (int Index = 0; Index < Count; Index++)
  {
    FNameIndex.insert(FAllFiles.at(Index).FileName, Index);
  }
  FMarked.assign(Count, 0);
  FMarkedCount = 0;
  FMarkedSize = 0;
  FCalculatedSizes.assign(Count, -1);
  for (auto It = KeepCalculated.cbegin(); It != KeepCalculated.cend(); ++It)
  {
    auto Found = FNameIndex.constFind(It.key());
    if ((Found != FNameIndex.constEnd()) && FAllFiles.at(Found.value()).IsDirectory &&
        (FAllFiles.at(Found.value()).Modification == It.value().first))
    {
      FCalculatedSizes[Found.value()] = It.value().second;
    }
  }
  FColorCache.assign(Count, 0);
  FBrokenLinkCache.clear();
  ComputeNameRanks();
  for (const QString & Name : KeepMarked)
  {
    auto It = FNameIndex.constFind(Name);
    if (It != FNameIndex.constEnd())
    {
      MarkIndex(It.value(), true);
    }
  }
  Rebuild();
  endResetModel();
  emit MarksChanged();
}

void FileListModel::Clear()
{
  beginResetModel();
  FDirectory.clear();
  FAllFiles.clear();
  FRows.clear();
  FRowOfFile.clear();
  FMarked.clear();
  FCalculatedSizes.clear();
  FMarkedCount = 0;
  FMarkedSize = 0;
  FNameIndex.clear();
  FNameRank.clear();
  FColorCache.clear();
  FBrokenLinkCache.clear();
  FHiddenCount = 0;
  FFilteredCount = 0;
  FTotalSize = 0;
  endResetModel();
  emit MarksChanged();
}

void FileListModel::SetShowHiddenFiles(bool Show)
{
  if (FShowHiddenFiles != Show)
  {
    beginResetModel();
    FShowHiddenFiles = Show;
    Rebuild();
    endResetModel();
    emit MarksChanged();
  }
}

void FileListModel::SetHideInaccessibleDirectories(bool Hide, const QString & UserName)
{
  if ((FHideInaccessible != Hide) || (FUserName != UserName))
  {
    beginResetModel();
    FHideInaccessible = Hide;
    FUserName = UserName;
    Rebuild();
    endResetModel();
    emit MarksChanged();
  }
}

bool FileListModel::SetFilterMask(const QString & Mask)
{
  bool Result = true;
  QString NewMask = Mask.trimmed().isEmpty() ? QString() : Mask;
  FileMasks NewMasks;
  try
  {
    NewMasks = FileMasks(NewMask);
  }
  catch (const FileMasksException &)
  {
    NewMask.clear();
    Result = false;
  }
  if (NewMask != FFilterMask)
  {
    beginResetModel();
    FFilterMask = NewMask;
    FFilterMasks = NewMasks;
    Rebuild();
    endResetModel();
    emit MarksChanged();
  }
  return Result;
}

bool FileListModel::IsInaccessibleDirectory(const RemoteFile & File) const
{
  // TRemoteFile::GetIsInaccessibleDirectory; group membership is not known,
  // so group execute permission counts as accessible
  if (!File.IsDirectory || File.IsParentDirectory || (File.Permissions == 0) ||
      (FUserName == QStringLiteral("root")))
  {
    return false;
  }
  unsigned int Mode = File.Permissions;
  if ((Mode & 0001) || (Mode & 0010))
  {
    return false;
  }
  if ((Mode & 0100) && (File.Owner == FUserName))
  {
    return false;
  }
  return true;
}

void FileListModel::Rebuild()
{
  FRows.clear();
  FHiddenCount = 0;
  FFilteredCount = 0;
  FTotalSize = 0;
  int Count = FAllFiles.size();
  if (FDirectory.isEmpty())
  {
    FRowOfFile.assign(Count, -1);
    return;
  }
  FRows.reserve(Count + 1);
  if (!IsAtRoot())
  {
    FRows.push_back(-1);
  }
  for (int Index = 0; Index < Count; Index++)
  {
    const RemoteFile & File = FAllFiles.at(Index);
    bool Visible = true;
    bool Hidden = File.IsHidden || File.FileName.startsWith(QLatin1Char('.'));
    if (Hidden && !FShowHiddenFiles)
    {
      FHiddenCount++;
      Visible = false;
    }
    else if (FRemote && FHideInaccessible && IsInaccessibleDirectory(File))
    {
      Visible = false;
    }
    else if (!FFilterMask.isEmpty())
    {
      // TCustomScpExplorerForm::DirViewMatchMask with AllowImplicitMatches
      // (TDirViewInt::FileMatches)
      FileMasks::Params Params;
      Params.Size = File.IsDirectory ? 0 : File.Size;
      Params.Modification = File.Modification;
      bool ImplicitMatch = false;
      if (!FFilterMasks.Matches(File.FileName, false, File.IsDirectory, &Params, true, ImplicitMatch))
      {
        FFilteredCount++;
        Visible = false;
      }
    }
    if (Visible)
    {
      FRows.push_back(Index);
      FTotalSize += EffectiveSize(Index);
    }
    else if (FMarked[Index])
    {
      // Files that are not shown cannot stay selected
      MarkIndex(Index, false);
    }
  }
  SortRows();
  UpdateRowIndex();
}

void FileListModel::UpdateRowIndex()
{
  FRowOfFile.assign(FAllFiles.size(), -1);
  for (int Row = 0; Row < static_cast<int>(FRows.size()); Row++)
  {
    if (FRows[Row] >= 0)
    {
      FRowOfFile[FRows[Row]] = Row;
    }
  }
}

static QCollator MakeCollator(bool Natural)
{
  // CompareLogicalText: StrCmpLogicalW (natural) or lstrcmpi
  QCollator Collator;
  Collator.setCaseSensitivity(Qt::CaseInsensitive);
  Collator.setNumericMode(Natural);
  Collator.setIgnorePunctuation(false);
  return Collator;
}

void FileListModel::ComputeNameRanks()
{
  int Count = FAllFiles.size();
  FNameRank.assign(Count, 0);
  if (Count == 0)
  {
    return;
  }
  // Sort keys are computed once per name (QCollator::compare per comparison is
  // several times slower on big directories)
  QCollator Collator = MakeCollator(FNaturalOrderNumericalSorting);
  std::vector<QCollatorSortKey> Keys;
  Keys.reserve(Count);
  for (const RemoteFile & File : FAllFiles)
  {
    Keys.push_back(Collator.sortKey(File.FileName));
  }
  std::vector<int> Order(Count);
  for (int Index = 0; Index < Count; Index++)
  {
    Order[Index] = Index;
  }
  std::sort(Order.begin(), Order.end(), [this, &Keys](int A, int B)
  {
    int Result = Keys[A].compare(Keys[B]);
    if (Result == 0)
    {
      // For deterministic results (lstrcmp)
      Result = FAllFiles.at(A).FileName.compare(FAllFiles.at(B).FileName);
    }
    return Result < 0;
  });
  for (int Rank = 0; Rank < Count; Rank++)
  {
    FNameRank[Order[Rank]] = Rank;
  }
}

QString FileListModel::Extension(const RemoteFile & File) const
{
  if (File.IsParentDirectory)
  {
    return QString();
  }
  int Dot = File.FileName.lastIndexOf(QLatin1Char('.'));
  if (Dot < 0)
  {
    return QString();
  }
  if (FRemote)
  {
    // UnixExtractFileExt (with the dot)
    return File.FileName.mid(Dot);
  }
  // TDirViewInt: upper case, without the dot
  return File.FileName.mid(Dot + 1).toUpper();
}

QString FileListModel::TypeName(const RemoteFile & File) const
{
  if (File.IsParentDirectory)
  {
    return Translator::Tr(QStringLiteral("Parent directory"));
  }
  if (File.IsDirectory)
  {
    return QStringLiteral("File folder");
  }
  const MimeInfo & Info = MimeInfoFor(File.FileName);
  if (!Info.Default && !Info.Comment.isEmpty())
  {
    return Info.Comment;
  }
  QString Suffix = QFileInfo(File.FileName).suffix();
  return Suffix.isEmpty() ? QStringLiteral("File") : QStringLiteral("%1 File").arg(Suffix.toUpper());
}

std::vector<int> FileListModel::ColumnRanks(int Column) const
{
  // Ranks of distinct column values in CompareLogicalText order, so sorting
  // compares integers only
  int Count = FAllFiles.size();
  std::vector<int> Ranks(Count, 0);
  QHash<QString, int> Distinct;
  std::vector<QString> Values(Count);
  for (int Row = 0; Row < static_cast<int>(FRows.size()); Row++)
  {
    int Index = FRows[Row];
    if (Index < 0)
    {
      continue;
    }
    const RemoteFile & File = FAllFiles.at(Index);
    QString Value;
    switch (Column)
    {
      case ColType: Value = TypeName(File); break;
      case ColExt: Value = Extension(File); break;
      case ColRights: Value = Rights::ToText(File.Permissions); break;
      case ColOwner: Value = File.Owner; break;
      case ColGroup: Value = File.Group; break;
      case ColLinkTarget: Value = File.LinkTo; break;
      default: break;
    }
    Distinct.insert(Value, 0);
    Values[Index] = Value;
  }
  QStringList Sorted = Distinct.keys();
  QCollator Collator = MakeCollator((Column != ColRights) && FNaturalOrderNumericalSorting);
  std::sort(Sorted.begin(), Sorted.end(), [&Collator](const QString & A, const QString & B)
  {
    int Result = Collator.compare(A, B);
    return (Result != 0) ? (Result < 0) : (A.compare(B) < 0);
  });
  for (int Rank = 0; Rank < Sorted.size(); Rank++)
  {
    Distinct[Sorted.at(Rank)] = Rank;
  }
  for (int Row = 0; Row < static_cast<int>(FRows.size()); Row++)
  {
    int Index = FRows[Row];
    if (Index >= 0)
    {
      Ranks[Index] = Distinct.value(Values[Index]);
    }
  }
  return Ranks;
}

void FileListModel::SortRows()
{
  // TUnixDirView CompareFile / TDirViewInt CompareFile
  const int Column = FSortColumn;
  const bool Ascending = (FSortOrder == Qt::AscendingOrder);
  std::vector<int> Ranks;
  std::vector<int> ExtRanks;
  bool StringColumn = (Column == ColType) || (Column == ColExt) || (Column == ColOwner) ||
    (Column == ColGroup) || (Column == ColLinkTarget) || ((Column == ColRights) && FRemote);
  if (StringColumn)
  {
    Ranks = ColumnRanks(Column);
  }
  if (Column == ColType)
  {
    ExtRanks = ColumnRanks(ColExt);
  }
  auto Compare = [&](int A, int B) -> bool
  {
    if ((A < 0) || (B < 0))
    {
      // ".." always first, regardless of the order
      return (A < 0) && (B >= 0);
    }
    const RemoteFile & FA = FAllFiles.at(A);
    const RemoteFile & FB = FAllFiles.at(B);
    // Directories are always grouped before files
    if (FA.IsDirectory != FB.IsDirectory)
    {
      return FA.IsDirectory;
    }
    int Result = 0;
    if (!(FA.IsDirectory && FAlwaysSortDirectoriesByName))
    {
      switch (Column)
      {
        case ColSize:
          if (!FA.IsDirectory)
          {
            Result = (FA.Size < FB.Size) ? -1 : ((FA.Size > FB.Size) ? 1 : 0);
          }
          else
          {
            // GetItemFileSize: calculated directory sizes (not calculated = -1)
            qint64 SA = FCalculatedSizes[A];
            qint64 SB = FCalculatedSizes[B];
            Result = (SA < SB) ? -1 : ((SA > SB) ? 1 : 0);
          }
          break;
        case ColChanged:
          Result = (FA.Modification < FB.Modification) ? -1 : ((FB.Modification < FA.Modification) ? 1 : 0);
          break;
        case ColRights:
          if (!FRemote)
          {
            Result = (FA.Permissions < FB.Permissions) ? -1 : ((FA.Permissions > FB.Permissions) ? 1 : 0);
          }
          else
          {
            Result = Ranks[A] - Ranks[B];
          }
          break;
        case ColExt:
          if (!FA.IsDirectory)
          {
            Result = Ranks[A] - Ranks[B];
          }
          break;
        case ColType:
          Result = Ranks[A] - Ranks[B];
          if ((Result == 0) && !FA.IsDirectory)
          {
            Result = ExtRanks[A] - ExtRanks[B];
          }
          break;
        case ColOwner:
        case ColGroup:
        case ColLinkTarget:
          Result = Ranks[A] - Ranks[B];
          break;
        default:
          break;
      }
    }
    if (Result == 0)
    {
      Result = FNameRank[A] - FNameRank[B];
    }
    return Ascending ? (Result < 0) : (Result > 0);
  };
  std::sort(FRows.begin(), FRows.end(), Compare);
}

void FileListModel::sort(int Column, Qt::SortOrder Order)
{
  if ((Column < 0) || (Column >= ColCount))
  {
    return;
  }
  emit layoutAboutToBeChanged({}, QAbstractItemModel::VerticalSortHint);
  FSortColumn = Column;
  FSortOrder = Order;

  // Remember persistent indexes (current item, editor) by file
  const QModelIndexList Persistent = persistentIndexList();
  std::vector<int> Files;
  Files.reserve(Persistent.size());
  for (const QModelIndex & Index : Persistent)
  {
    Files.push_back(FileIndex(Index.row()));
  }

  SortRows();
  UpdateRowIndex();

  QModelIndexList NewIndexes;
  NewIndexes.reserve(Persistent.size());
  for (int I = 0; I < Persistent.size(); I++)
  {
    int File = Files[I];
    int Row = (File < 0) ? (((!FRows.empty()) && (FRows[0] < 0)) ? 0 : -1) : FRowOfFile[File];
    NewIndexes.append((Row >= 0) ? index(Row, Persistent.at(I).column()) : QModelIndex());
  }
  changePersistentIndexList(Persistent, NewIndexes);
  emit layoutChanged({}, QAbstractItemModel::VerticalSortHint);
}

bool FileListModel::IsParentRow(int Row) const
{
  return (Row >= 0) && (Row < static_cast<int>(FRows.size())) && (FRows[Row] < 0);
}

int FileListModel::RowOfName(const QString & FileName) const
{
  if (FileName == QStringLiteral(".."))
  {
    return ((!FRows.empty()) && (FRows[0] < 0)) ? 0 : -1;
  }
  auto It = FNameIndex.constFind(FileName);
  if (It == FNameIndex.constEnd())
  {
    return -1;
  }
  return FRowOfFile[It.value()];
}

QString FileListModel::FullPath(const QString & FileName) const
{
  if (FRemote)
  {
    return UnixPath::Combine(FDirectory, FileName);
  }
  return QDir::cleanPath(QDir(FDirectory).filePath(FileName));
}

int FileListModel::FileCount() const
{
  int Result = static_cast<int>(FRows.size());
  if ((Result > 0) && (FRows[0] < 0))
  {
    Result--;
  }
  return Result;
}

qint64 FileListModel::TotalSize() const
{
  return FTotalSize;
}

qint64 FileListModel::EffectiveSize(int Index) const
{
  const RemoteFile & File = FAllFiles.at(Index);
  if (!File.IsDirectory)
  {
    return File.Size;
  }
  return ((Index < static_cast<int>(FCalculatedSizes.size())) && (FCalculatedSizes[Index] >= 0)) ? FCalculatedSizes[Index] : 0;
}

bool FileListModel::SetCalculatedSize(const QString & FileName, qint64 Size)
{
  // TUnixDirView/TDirViewInt::SetItemCalculatedSize + ItemCalculatedSizeUpdated
  auto It = FNameIndex.constFind(FileName);
  if ((It == FNameIndex.constEnd()) || !FAllFiles.at(It.value()).IsDirectory)
  {
    return false;
  }
  int Index = It.value();
  qint64 NewSize = (Size >= 0) ? Size : -1;
  qint64 OldEffective = EffectiveSize(Index);
  FCalculatedSizes[Index] = NewSize;
  qint64 Delta = EffectiveSize(Index) - OldEffective;
  int Row = FRowOfFile[Index];
  if (Row >= 0)
  {
    FTotalSize += Delta;
    emit dataChanged(index(Row, ColSize), index(Row, ColSize), {Qt::DisplayRole});
  }
  if (FMarked[Index])
  {
    FMarkedSize += Delta;
    emit MarksChanged();
  }
  return true;
}

qint64 FileListModel::CalculatedSize(const QString & FileName) const
{
  auto It = FNameIndex.constFind(FileName);
  if ((It == FNameIndex.constEnd()) || (It.value() >= static_cast<int>(FCalculatedSizes.size())))
  {
    return -1;
  }
  return FCalculatedSizes[It.value()];
}

//---------------------------------------------------------------------------
// Marks

void FileListModel::MarkIndex(int Index, bool Marked)
{
  if ((Index < 0) || (Index >= static_cast<int>(FMarked.size())) || ((FMarked[Index] != 0) == Marked))
  {
    return;
  }
  FMarked[Index] = Marked ? 1 : 0;
  qint64 Size = EffectiveSize(Index);
  if (Marked)
  {
    FMarkedCount++;
    FMarkedSize += Size;
  }
  else
  {
    FMarkedCount--;
    FMarkedSize -= Size;
  }
}

void FileListModel::EmitRowsChanged(int First, int Last)
{
  if ((First <= Last) && (First >= 0))
  {
    emit dataChanged(index(First, 0), index(Last, ColCount - 1), {MarkedRole});
  }
  emit MarksChanged();
}

bool FileListModel::IsMarked(int Row) const
{
  int Index = FileIndex(Row);
  return (Index >= 0) && FMarked[Index];
}

void FileListModel::SetMarked(int Row, bool Marked)
{
  int Index = FileIndex(Row);
  if ((Index >= 0) && ((FMarked[Index] != 0) != Marked))
  {
    MarkIndex(Index, Marked);
    EmitRowsChanged(Row, Row);
  }
}

void FileListModel::ToggleMarked(int Row)
{
  SetMarked(Row, !IsMarked(Row));
}

void FileListModel::SetMarkedRange(int First, int Last, bool Marked)
{
  if (First > Last)
  {
    std::swap(First, Last);
  }
  First = std::max(First, 0);
  Last = std::min(Last, static_cast<int>(FRows.size()) - 1);
  for (int Row = First; Row <= Last; Row++)
  {
    MarkIndex(FRows[Row], Marked);
  }
  EmitRowsChanged(First, Last);
}

void FileListModel::MarkAll(bool Marked)
{
  if (!Marked && (FMarkedCount == 0))
  {
    return;
  }
  if (Marked)
  {
    for (int Index : FRows)
    {
      MarkIndex(Index, true);
    }
  }
  else
  {
    std::fill(FMarked.begin(), FMarked.end(), 0);
    FMarkedCount = 0;
    FMarkedSize = 0;
  }
  EmitRowsChanged(0, static_cast<int>(FRows.size()) - 1);
}

void FileListModel::InvertMarks()
{
  for (int Index : FRows)
  {
    if (Index >= 0)
    {
      MarkIndex(Index, !FMarked[Index]);
    }
  }
  EmitRowsChanged(0, static_cast<int>(FRows.size()) - 1);
}

void FileListModel::SetMarkedRows(const QList<int> & Rows, bool ClearOthers)
{
  if (ClearOthers)
  {
    std::fill(FMarked.begin(), FMarked.end(), 0);
    FMarkedCount = 0;
    FMarkedSize = 0;
  }
  for (int Row : Rows)
  {
    MarkIndex(FileIndex(Row), true);
  }
  EmitRowsChanged(0, static_cast<int>(FRows.size()) - 1);
}

void FileListModel::SetMarkedNames(const QSet<QString> & Names, bool Marked, bool ClearOthers)
{
  if (ClearOthers)
  {
    std::fill(FMarked.begin(), FMarked.end(), 0);
    FMarkedCount = 0;
    FMarkedSize = 0;
  }
  for (const QString & Name : Names)
  {
    auto It = FNameIndex.constFind(Name);
    if ((It != FNameIndex.constEnd()) && (FRowOfFile[It.value()] >= 0))
    {
      MarkIndex(It.value(), Marked);
    }
  }
  EmitRowsChanged(0, static_cast<int>(FRows.size()) - 1);
}

bool FileListModel::UnmarkFile(const QString & FileName)
{
  auto It = FNameIndex.constFind(FileName);
  if ((It == FNameIndex.constEnd()) || !FMarked[It.value()])
  {
    return false;
  }
  MarkIndex(It.value(), false);
  int Row = FRowOfFile[It.value()];
  EmitRowsChanged(Row, Row);
  return true;
}

QList<int> FileListModel::MarkedRows() const
{
  QList<int> Result;
  if (FMarkedCount == 0)
  {
    return Result;
  }
  Result.reserve(FMarkedCount);
  for (int Row = 0; Row < static_cast<int>(FRows.size()); Row++)
  {
    int Index = FRows[Row];
    if ((Index >= 0) && FMarked[Index])
    {
      Result.append(Row);
    }
  }
  return Result;
}

QStringList FileListModel::MarkedNames() const
{
  QStringList Result;
  for (int Row : MarkedRows())
  {
    Result.append(File(Row).FileName);
  }
  return Result;
}

//---------------------------------------------------------------------------
// Preferences

void FileListModel::SetFormatSizeBytes(TFormatBytesStyle Style)
{
  if (FFormatSizeBytes != Style)
  {
    FFormatSizeBytes = Style;
    if (!FRows.empty())
    {
      emit dataChanged(index(0, ColSize), index(static_cast<int>(FRows.size()) - 1, ColSize), {Qt::DisplayRole});
    }
  }
}

void FileListModel::SetNaturalOrderNumericalSorting(bool Value)
{
  if (FNaturalOrderNumericalSorting != Value)
  {
    FNaturalOrderNumericalSorting = Value;
    ComputeNameRanks();
    sort(FSortColumn, FSortOrder);
  }
}

void FileListModel::SetAlwaysSortDirectoriesByName(bool Value)
{
  if (FAlwaysSortDirectoriesByName != Value)
  {
    FAlwaysSortDirectoriesByName = Value;
    sort(FSortColumn, FSortOrder);
  }
}

void FileListModel::SetDimHiddenFiles(bool Value)
{
  if (FDimHiddenFiles != Value)
  {
    FDimHiddenFiles = Value;
    if (!FRows.empty())
    {
      emit dataChanged(index(0, 0), index(static_cast<int>(FRows.size()) - 1, ColCount - 1), {Qt::ForegroundRole});
    }
  }
}

void FileListModel::SetFileColors(const QList<FileColorRule> & Rules)
{
  FFileColors = Rules;
  std::fill(FColorCache.begin(), FColorCache.end(), 0);
  if (!FRows.empty())
  {
    emit dataChanged(index(0, 0), index(static_cast<int>(FRows.size()) - 1, ColCount - 1), {Qt::ForegroundRole});
  }
}

QColor FileListModel::FileColor(int Index) const
{
  // TCustomScpExplorerForm::DirViewGetItemColor: first matching rule wins
  if (FFileColors.isEmpty() || (Index < 0))
  {
    return QColor();
  }
  QRgb & Cached = FColorCache[Index];
  if (Cached == 0)
  {
    Cached = 1;
    const RemoteFile & File = FAllFiles.at(Index);
    FileMasks::Params Params;
    Params.Size = File.Size;
    Params.Modification = File.Modification;
    QString Path = FullPath(File.FileName);
    for (const FileColorRule & Rule : FFileColors)
    {
      bool ImplicitMatch = false;
      if (Rule.Masks.Matches(Path, !FRemote, File.IsDirectory, &Params, false, ImplicitMatch) && !ImplicitMatch)
      {
        // alpha keeps the value distinct from the 0/1 markers
        Cached = Rule.Color.rgb() | 0xFF000000;
        break;
      }
    }
  }
  return (Cached == 1) ? QColor() : QColor::fromRgb(Cached);
}

int FileListModel::Overlays(int Row) const
{
  int Index = FileIndex(Row);
  if (Index < 0)
  {
    return ovNone;
  }
  const RemoteFile & File = FAllFiles.at(Index);
  int Result = ovNone;
  if (File.IsSymLink)
  {
    bool Broken = false;
    if (!FRemote)
    {
      auto It = FBrokenLinkCache.constFind(Index);
      if (It == FBrokenLinkCache.constEnd())
      {
        Broken = !QFileInfo::exists(FullPath(File.FileName));
        FBrokenLinkCache.insert(Index, Broken);
      }
      else
      {
        Broken = It.value();
      }
    }
    Result |= Broken ? ovBrokenLink : ovLink;
  }
  if (!File.IsDirectory && File.FileName.endsWith(QStringLiteral(".filepart"), Qt::CaseInsensitive))
  {
    Result |= ovPartial;
  }
  return Result;
}

//---------------------------------------------------------------------------
// QAbstractItemModel

int FileListModel::rowCount(const QModelIndex & Parent) const
{
  return Parent.isValid() ? 0 : static_cast<int>(FRows.size());
}

int FileListModel::columnCount(const QModelIndex & Parent) const
{
  return Parent.isValid() ? 0 : ColCount;
}

QVariant FileListModel::data(const QModelIndex & Index, int Role) const
{
  if (!Index.isValid() || (Index.row() >= static_cast<int>(FRows.size())))
  {
    return QVariant();
  }
  int FileIdx = FRows[Index.row()];
  const RemoteFile & File = (FileIdx < 0) ? FParentEntry : FAllFiles.at(FileIdx);
  int Column = Index.column();

  switch (Role)
  {
    case Qt::DisplayRole:
      switch (Column)
      {
        case ColName:
          return File.FileName;
        case ColSize:
          if (File.IsDirectory)
          {
            // uvSize: File->CalculatedSize of a directory, when calculated
            qint64 Calculated = ((FileIdx >= 0) && (FileIdx < static_cast<int>(FCalculatedSizes.size()))) ? FCalculatedSizes[FileIdx] : -1;
            return (Calculated >= 0) ? FormatPanelBytes(Calculated, FFormatSizeBytes) : QString();
          }
          return FormatPanelBytes(File.Size, FFormatSizeBytes);
        case ColType:
          return TypeName(File);
        case ColChanged:
          return File.IsParentDirectory ? QString() : FormatModification(File.Modification);
        case ColRights:
          return File.IsParentDirectory ? QString() : Rights::ToText(File.Permissions);
        case ColOwner:
          return File.Owner;
        case ColGroup:
          return File.Group;
        case ColExt:
          return File.IsDirectory ? QString() : Extension(File);
        case ColLinkTarget:
          return File.LinkTo;
      }
      break;

    case Qt::EditRole:
      if (Column == ColName)
      {
        return File.FileName;
      }
      break;

    case Qt::DecorationRole:
      if (Column == ColName)
      {
        return IconFor(File);
      }
      break;

    case Qt::TextAlignmentRole:
      if (Column == ColSize)
      {
        return QVariant::fromValue(Qt::AlignRight | Qt::AlignVCenter);
      }
      break;

    case Qt::ForegroundRole:
      {
        QColor Color = FileColor(FileIdx);
        if (Color.isValid())
        {
          return Color;
        }
        if (FDimHiddenFiles && (File.IsHidden || File.FileName.startsWith(QLatin1Char('.'))) && !File.IsParentDirectory)
        {
          return QGuiApplication::palette().brush(QPalette::Disabled, QPalette::Text);
        }
      }
      break;

    case Qt::ToolTipRole:
      if ((Column == ColName) && File.IsSymLink && !File.LinkTo.isEmpty())
      {
        return QStringLiteral("%1 -> %2").arg(File.FileName, File.LinkTo);
      }
      break;

    case MarkedRole:
      return (FileIdx >= 0) && FMarked[FileIdx];

    case OverlayRole:
      return (Column == ColName) ? Overlays(Index.row()) : 0;

    case ParentRole:
      return File.IsParentDirectory;
  }
  return QVariant();
}

QVariant FileListModel::headerData(int Section, Qt::Orientation Orientation, int Role) const
{
  if (Orientation != Qt::Horizontal)
  {
    return QVariant();
  }
  if (Role == Qt::DisplayRole)
  {
    return ColumnCaption(Section);
  }
  if ((Role == Qt::TextAlignmentRole) && (Section == ColSize))
  {
    return QVariant::fromValue(Qt::AlignRight | Qt::AlignVCenter);
  }
  if (Role == Qt::InitialSortOrderRole)
  {
    // SortAscendingByDefault: size and time sort descending first
    return ((Section == ColSize) || (Section == ColChanged)) ? Qt::DescendingOrder : Qt::AscendingOrder;
  }
  return QVariant();
}

Qt::ItemFlags FileListModel::flags(const QModelIndex & Index) const
{
  if (!Index.isValid())
  {
    return Qt::ItemIsDropEnabled;
  }
  Qt::ItemFlags Result = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
  const RemoteFile & File = this->File(Index.row());
  if (!File.IsParentDirectory)
  {
    Result |= Qt::ItemIsDragEnabled;
    if (Index.column() == ColName)
    {
      Result |= Qt::ItemIsEditable;
    }
  }
  if (File.IsDirectory)
  {
    Result |= Qt::ItemIsDropEnabled;
  }
  return Result;
}

bool FileListModel::setData(const QModelIndex & Index, const QVariant & Value, int Role)
{
  if (!Index.isValid() || (Role != Qt::EditRole) || (Index.column() != ColName))
  {
    return false;
  }
  const RemoteFile & File = this->File(Index.row());
  QString NewName = Value.toString();
  if (File.IsParentDirectory || NewName.isEmpty() || (NewName == File.FileName))
  {
    return false;
  }
  emit RenameRequested(File.FileName, NewName);
  // The listing is refreshed by whoever performs the rename
  return false;
}
