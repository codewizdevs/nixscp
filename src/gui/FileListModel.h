// Item model of the file panel.
// Port of the list part of WinSCP source/components/DirView.cpp (local),
// UnixDirView.cpp (remote), packages/filemng/CustomDirView.pas, DirViewInt.pas
// (CompareFile), packages/filemng/BaseUtils.pas (FormatBytes) and the selection
// bookkeeping of packages/my/NortonLikeListView.pas: columns, ".." entry,
// sorting, hidden/filtered files, marked (selected) files, file colors.
#pragma once

#include "core/Configuration.h"
#include "core/FileMasks.h"
#include "core/RemoteFile.h"

#include <QAbstractTableModel>
#include <QColor>
#include <QHash>
#include <QIcon>
#include <QList>
#include <QSet>
#include <QStringList>

#include <vector>

// TFileColorData (windows/WinConfiguration.cpp): one "Preferences > Panels >
// File colors" rule. Stored as WinSCP does in Configuration\Interface\FileColors:
// TStringList::CommaText of "BBGGRR:mask" items (StoreColor = IntToHex(TColor, 6)).
struct FileColorRule
{
  QColor Color;
  QString Mask;
  FileMasks Masks;

  static QList<FileColorRule> LoadList(const QString & Str);    // invalid masks are skipped
  static QString SaveList(const QList<FileColorRule> & List);
};

class FileListModel : public QAbstractTableModel
{
  Q_OBJECT

public:
  // Model columns. Local and remote panels share them; the WinSCP column
  // indexes of DirViewParams are mapped in FilePanel (TDirViewCol / TUnixDirViewCol).
  enum Column { ColName, ColSize, ColType, ColChanged, ColRights, ColOwner, ColGroup, ColExt, ColLinkTarget, ColCount };
  enum Role
  {
    MarkedRole = Qt::UserRole + 1,   // bool
    OverlayRole,                     // int, Overlay flags (name column)
    ParentRole,                      // bool, the ".." row
  };
  // Overlay icons (TUnixDirView::ItemOverlayIndexes: oiLink, oiBrokenLink, oiPartial)
  enum Overlay { ovNone = 0, ovLink = 1, ovBrokenLink = 2, ovPartial = 4 };

  explicit FileListModel(bool Remote, QObject * Parent = nullptr);

  bool IsRemote() const { return FRemote; }
  // Replaces the listing. Directory is the (canonical) directory the files belong to.
  // Marks are kept for files of the same name when Directory did not change.
  void SetFiles(const QString & Directory, const RemoteFileList & Files);
  void Clear();
  QString Directory() const { return FDirectory; }
  const RemoteFileList & AllFiles() const { return FAllFiles; }

  void SetShowHiddenFiles(bool Show);
  bool ShowHiddenFiles() const { return FShowHiddenFiles; }
  // Remote "Show inaccessible directories" off (TRemoteFile::GetIsInaccessibleDirectory);
  // UserName is the session user (root sees everything)
  void SetHideInaccessibleDirectories(bool Hide, const QString & UserName);

  // Panel filter (TCustomDirView::Mask): WinSCP file mask; files not matching
  // are hidden, directories are shown unless excluded by a directory mask.
  // An invalid mask clears the filter (returns false).
  bool SetFilterMask(const QString & Mask);
  QString FilterMask() const { return FFilterMask; }
  int HiddenCount() const { return FHiddenCount; }       // hidden files not shown
  int FilteredCount() const { return FFilteredCount; }   // files hidden by the filter

  const RemoteFile & File(int Row) const;
  bool IsParentRow(int Row) const;
  int RowOfName(const QString & FileName) const;   // -1 if not shown; O(1)
  QString FullPath(const QString & FileName) const;
  int FileCount() const;                           // visible rows except ".."
  qint64 TotalSize() const;                        // visible files (not directories) and calculated directory sizes

  // ---- Calculated directory sizes (TCustomDirView::SetItemCalculatedSize) ----
  // Size of a directory as computed by Calculate Directory Sizes: shown in the
  // Size column, used for sorting by size and counted in the total/selected
  // sizes of the status bar. -1 clears it. Kept over reloads of the same
  // directory while the directory entry does not change. Returns false when
  // FileName is not a directory of the listing.
  bool SetCalculatedSize(const QString & FileName, qint64 Size);
  qint64 CalculatedSize(const QString & FileName) const;   // -1 = not calculated

  // ---- Marked files (TCustomNortonLikeListView selection) ----
  // ".." can never be marked. Marks survive sorting and refreshes (by name).
  bool IsMarked(int Row) const;
  void SetMarked(int Row, bool Marked);
  void ToggleMarked(int Row);
  void SetMarkedRange(int First, int Last, bool Marked);   // inclusive, any order
  void MarkAll(bool Marked);
  void InvertMarks();
  // Marks exactly Rows (or Names), unmarking everything else when ClearOthers
  void SetMarkedRows(const QList<int> & Rows, bool ClearOthers);
  void SetMarkedNames(const QSet<QString> & Names, bool Marked, bool ClearOthers);
  bool UnmarkFile(const QString & FileName);       // false when not marked/listed
  int MarkedCount() const { return FMarkedCount; }
  qint64 MarkedSize() const { return FMarkedSize; } // marked files, not directories
  QList<int> MarkedRows() const;                   // ascending visible rows
  QStringList MarkedNames() const;

  // ---- Sorting (TCustomDirView SortColumn/SortAscending) ----
  int SortColumn() const { return FSortColumn; }
  Qt::SortOrder SortOrder() const { return FSortOrder; }
  bool SortAscending() const { return FSortOrder == Qt::AscendingOrder; }

  // ---- Display preferences ----
  void SetFormatSizeBytes(TFormatBytesStyle Style);
  TFormatBytesStyle FormatSizeBytesStyle() const { return FFormatSizeBytes; }
  void SetNaturalOrderNumericalSorting(bool Value);
  void SetAlwaysSortDirectoriesByName(bool Value);
  void SetDimHiddenFiles(bool Value);
  void SetFileColors(const QList<FileColorRule> & Rules);

  static QString ColumnCaption(int Column);
  static bool DefaultColumnVisible(int Column, bool Remote);
  static int DefaultColumnWidth(int Column, bool Remote);
  // FormatBytes(Bytes, fbShort) of WinSCP: status bar, queue, synchronization
  // ("0 B", "15.2 KB", "1.23 MB", "123 MB")
  static QString FormatBytes(qint64 Bytes);
  // FormatPanelBytes (size column): Bytes ("1,234"), Kilobytes ("2 KB"), Short ("1.21 KB")
  static QString FormatPanelBytes(qint64 Bytes, TFormatBytesStyle Style);
  static QString FormatSizeKB(qint64 Bytes);       // FormatPanelBytes(Bytes, fbKilobytes)
  // Changed column: short date + long time ("ddddd tt"), in local time
  static QString FormatModification(const QDateTime & Modification);
  // Ext column: local "TXT" (TDirViewInt), remote ".txt" (UnixExtractFileExt)
  QString Extension(const RemoteFile & File) const;
  QString TypeName(const RemoteFile & File) const;
  int Overlays(int Row) const;

  // Icon by extension / MIME type, shared by all panels (no file contents are read)
  static QIcon IconFor(const RemoteFile & File);

  int rowCount(const QModelIndex & Parent = QModelIndex()) const override;
  int columnCount(const QModelIndex & Parent = QModelIndex()) const override;
  QVariant data(const QModelIndex & Index, int Role = Qt::DisplayRole) const override;
  QVariant headerData(int Section, Qt::Orientation Orientation, int Role = Qt::DisplayRole) const override;
  Qt::ItemFlags flags(const QModelIndex & Index) const override;
  bool setData(const QModelIndex & Index, const QVariant & Value, int Role = Qt::EditRole) override;
  void sort(int Column, Qt::SortOrder Order = Qt::AscendingOrder) override;

signals:
  // Inline edit committed. The model never renames anything itself.
  void RenameRequested(const QString & OldFileName, const QString & NewName);
  void MarksChanged();

private:
  bool FRemote;
  bool FShowHiddenFiles = false;
  bool FHideInaccessible = false;
  QString FUserName;
  bool FDimHiddenFiles = true;
  QString FFilterMask;
  FileMasks FFilterMasks;
  int FHiddenCount = 0;
  int FFilteredCount = 0;
  qint64 FTotalSize = 0;
  QString FDirectory;
  RemoteFileList FAllFiles;
  RemoteFile FParentEntry;            // ".."
  std::vector<int> FRows;             // visible row -> FAllFiles index, -1 = ".."
  std::vector<int> FRowOfFile;        // FAllFiles index -> visible row, -1 = not shown
  std::vector<char> FMarked;          // per FAllFiles index
  std::vector<qint64> FCalculatedSizes;   // per FAllFiles index, -1 = not calculated
  int FMarkedCount = 0;
  qint64 FMarkedSize = 0;
  QHash<QString, int> FNameIndex;     // file name -> FAllFiles index
  std::vector<int> FNameRank;         // CompareLogicalText order of names
  int FSortColumn = ColName;
  Qt::SortOrder FSortOrder = Qt::AscendingOrder;
  TFormatBytesStyle FFormatSizeBytes = fbKilobytes;
  bool FNaturalOrderNumericalSorting = true;
  bool FAlwaysSortDirectoriesByName = false;
  QList<FileColorRule> FFileColors;
  mutable std::vector<QRgb> FColorCache;   // per FAllFiles index; 0 = not computed, 1 = none
  mutable QHash<int, bool> FBrokenLinkCache;
  mutable QHash<QString, QString> FTypeCache;

  void Rebuild();
  void SortRows();
  void UpdateRowIndex();
  void ComputeNameRanks();
  std::vector<int> ColumnRanks(int Column) const;
  bool IsInaccessibleDirectory(const RemoteFile & File) const;
  bool IsAtRoot() const;
  int FileIndex(int Row) const;
  // File size for totals and sorting: the calculated size of a directory (0 when not calculated)
  qint64 EffectiveSize(int Index) const;
  void MarkIndex(int Index, bool Marked);
  QColor FileColor(int Index) const;
  void EmitRowsChanged(int First, int Last);
};
