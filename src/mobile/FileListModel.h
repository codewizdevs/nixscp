// The file list of one pane (the phone's folder or the server's): sorting,
// the filter typed in the pane, hidden files and the selection. The panes
// fill it with entries; QML shows only the visible ones.
#pragma once

#include <QAbstractListModel>
#include <QDateTime>
#include <QSet>
#include <QVariantMap>

class FileListModel : public QAbstractListModel
{
  Q_OBJECT
  Q_PROPERTY(int sortBy READ sortBy WRITE setSortBy NOTIFY viewChanged)
  Q_PROPERTY(bool descending READ descending WRITE setDescending NOTIFY viewChanged)
  Q_PROPERTY(bool showHidden READ showHidden WRITE setShowHidden NOTIFY viewChanged)
  Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY viewChanged)
  // shown / in the folder
  Q_PROPERTY(int count READ count NOTIFY countChanged)
  Q_PROPERTY(int total READ total NOTIFY countChanged)
  Q_PROPERTY(int selectedCount READ selectedCount NOTIFY selectionChanged)
  Q_PROPERTY(bool selectedHasFolder READ selectedHasFolder NOTIFY selectionChanged)

public:
  struct Entry
  {
    QString Name;
    bool IsDir = false;
    bool IsLink = false;
    qint64 Size = 0;
    QDateTime Modified;
    QString Rights;
    unsigned int Mode = 0;   // permission bits (lower 12)
    QString Owner;
    QString Group;
    QString Thumbnail;       // an image's thumbnail (file URL), when there is one
  };
  enum Roles { NameRole = Qt::UserRole + 1, IsDirRole, IsLinkRole, SizeRole, DateRole, RightsRole, SelectedRole, ModeRole, OwnerRole, GroupRole,
    ThumbnailRole, PreviewRole };
  enum Sort { SortName, SortDate, SortSize };
  Q_ENUM(Sort)

  explicit FileListModel(QObject * Parent = nullptr) : QAbstractListModel(Parent) {}
  // A new listing; the selection is cleared
  void SetEntries(const QList<Entry> & Entries);

  int sortBy() const { return FSortBy; }
  void setSortBy(int Value);
  bool descending() const { return FDescending; }
  void setDescending(bool Value);
  bool showHidden() const { return FShowHidden; }
  void setShowHidden(bool Value);
  QString filter() const { return FFilter; }
  void setFilter(const QString & Value);
  int count() const { return static_cast<int>(FVisible.size()); }
  int total() const { return static_cast<int>(FAll.size()); }
  int selectedCount() const { return static_cast<int>(FSelected.size()); }
  bool selectedHasFolder() const;

  Q_INVOKABLE void toggle(const QString & Name);
  // the shown files
  Q_INVOKABLE void selectAll();
  Q_INVOKABLE void clearSelection();
  Q_INVOKABLE QStringList selectedNames() const;
  Q_INVOKABLE bool isDir(const QString & Name) const;
  // mode (permission bits), owner and group of a file
  Q_INVOKABLE QVariantMap entry(const QString & Name) const;
  // "image", "text" or "" (by the name): what Preview shows
  Q_INVOKABLE static QString previewKind(const QString & Name);
  void SetThumbnail(const QString & Name, const QString & Url);

  int rowCount(const QModelIndex & Parent = QModelIndex()) const override;
  QVariant data(const QModelIndex & Index, int Role) const override;
  QHash<int, QByteArray> roleNames() const override;

signals:
  void viewChanged();
  void countChanged();
  void selectionChanged();

private:
  QList<Entry> FAll;
  QList<int> FVisible;   // indexes of FAll, in the shown order
  QSet<QString> FSelected;
  int FSortBy = SortName;
  bool FDescending = false;
  bool FShowHidden = false;
  QString FFilter;

  void Rebuild();
};
