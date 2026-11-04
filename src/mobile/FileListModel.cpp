#include "mobile/FileListModel.h"

#include "core/SessionData.h"

#include <QFileInfo>
#include <QLocale>
#include <QMimeDatabase>
#include <QUrl>

#include <algorithm>

void FileListModel::SetEntries(const QList<Entry> & Entries)
{
  FAll = Entries;
  bool HadSelection = !FSelected.isEmpty();
  FSelected.clear();
  Rebuild();
  if (HadSelection)
  {
    emit selectionChanged();
  }
}

void FileListModel::setSortBy(int Value)
{
  if (Value != FSortBy)
  {
    FSortBy = Value;
    Rebuild();
    emit viewChanged();
  }
}

void FileListModel::setDescending(bool Value)
{
  if (Value != FDescending)
  {
    FDescending = Value;
    Rebuild();
    emit viewChanged();
  }
}

void FileListModel::setShowHidden(bool Value)
{
  if (Value != FShowHidden)
  {
    FShowHidden = Value;
    Rebuild();
    emit viewChanged();
  }
}

void FileListModel::setFilter(const QString & Value)
{
  if (Value != FFilter)
  {
    FFilter = Value;
    Rebuild();
    emit viewChanged();
  }
}

void FileListModel::Rebuild()
{
  beginResetModel();
  FVisible.clear();
  QString Filter = FFilter.trimmed();
  for (int I = 0; I < FAll.size(); I++)
  {
    const Entry & E = FAll.at(I);
    if (!FShowHidden && E.Name.startsWith(QLatin1Char('.')))
    {
      continue;
    }
    if (!Filter.isEmpty() && !E.Name.contains(Filter, Qt::CaseInsensitive))
    {
      continue;
    }
    FVisible.append(I);
  }
  // folders always first (as the desktop panels), then by the chosen column
  std::sort(FVisible.begin(), FVisible.end(), [this](int AI, int BI)
  {
    const Entry & A = FAll.at(AI);
    const Entry & B = FAll.at(BI);
    if (A.IsDir != B.IsDir)
    {
      return A.IsDir;
    }
    int Order = 0;
    if ((FSortBy == SortDate) && (A.Modified != B.Modified))
    {
      Order = (A.Modified < B.Modified) ? -1 : 1;
    }
    else if ((FSortBy == SortSize) && !A.IsDir && (A.Size != B.Size))
    {
      Order = (A.Size < B.Size) ? -1 : 1;
    }
    if (Order == 0)
    {
      Order = StoredSessionList::CompareLogicalText(A.Name, B.Name);
    }
    return FDescending ? (Order > 0) : (Order < 0);
  });
  endResetModel();
  emit countChanged();
  // hidden by the filter: no longer selected
  int Before = static_cast<int>(FSelected.size());
  QSet<QString> Shown;
  for (int I : FVisible)
  {
    Shown.insert(FAll.at(I).Name);
  }
  FSelected.intersect(Shown);
  if (FSelected.size() != Before)
  {
    emit selectionChanged();
  }
}

bool FileListModel::selectedHasFolder() const
{
  for (const Entry & E : FAll)
  {
    if (E.IsDir && FSelected.contains(E.Name))
    {
      return true;
    }
  }
  return false;
}

void FileListModel::toggle(const QString & Name)
{
  if (!FSelected.remove(Name))
  {
    FSelected.insert(Name);
  }
  for (int Row = 0; Row < FVisible.size(); Row++)
  {
    if (FAll.at(FVisible.at(Row)).Name == Name)
    {
      emit dataChanged(index(Row), index(Row), {SelectedRole});
      break;
    }
  }
  emit selectionChanged();
}

void FileListModel::selectAll()
{
  for (int I : FVisible)
  {
    FSelected.insert(FAll.at(I).Name);
  }
  if (!FVisible.isEmpty())
  {
    emit dataChanged(index(0), index(static_cast<int>(FVisible.size()) - 1), {SelectedRole});
  }
  emit selectionChanged();
}

void FileListModel::clearSelection()
{
  if (FSelected.isEmpty())
  {
    return;
  }
  FSelected.clear();
  if (!FVisible.isEmpty())
  {
    emit dataChanged(index(0), index(static_cast<int>(FVisible.size()) - 1), {SelectedRole});
  }
  emit selectionChanged();
}

QStringList FileListModel::selectedNames() const
{
  // in the shown order
  QStringList Result;
  for (int I : FVisible)
  {
    if (FSelected.contains(FAll.at(I).Name))
    {
      Result.append(FAll.at(I).Name);
    }
  }
  return Result;
}

bool FileListModel::isDir(const QString & Name) const
{
  for (const Entry & E : FAll)
  {
    if (E.Name == Name)
    {
      return E.IsDir;
    }
  }
  return false;
}

QVariantMap FileListModel::entry(const QString & Name) const
{
  for (const Entry & E : FAll)
  {
    if (E.Name == Name)
    {
      return {{QStringLiteral("name"), E.Name}, {QStringLiteral("isDir"), E.IsDir},
        {QStringLiteral("mode"), E.Mode}, {QStringLiteral("owner"), E.Owner}, {QStringLiteral("group"), E.Group},
        {QStringLiteral("size"), E.Size}};
    }
  }
  return {};
}

QString FileListModel::previewKind(const QString & Name)
{
  static const QStringList Images = {QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("png"),
    QStringLiteral("gif"), QStringLiteral("webp"), QStringLiteral("bmp")};
  static const QStringList Texts = {QStringLiteral("conf"), QStringLiteral("cfg"), QStringLiteral("ini"),
    QStringLiteral("log"), QStringLiteral("env"), QStringLiteral("yml"), QStringLiteral("yaml"), QStringLiteral("toml"),
    QStringLiteral("htaccess"), QStringLiteral("service"), QStringLiteral("md"), QStringLiteral("csv")};
  QString Suffix = QFileInfo(Name).suffix().toLower();
  if (Images.contains(Suffix))
  {
    return QStringLiteral("image");
  }
  if (Texts.contains(Suffix) || QMimeDatabase().mimeTypeForFile(Name, QMimeDatabase::MatchExtension).inherits(QStringLiteral("text/plain")))
  {
    return QStringLiteral("text");
  }
  return QString();
}

void FileListModel::SetThumbnail(const QString & Name, const QString & Url)
{
  for (Entry & E : FAll)
  {
    if (E.Name == Name)
    {
      E.Thumbnail = Url;
      break;
    }
  }
  for (int Row = 0; Row < FVisible.size(); Row++)
  {
    if (FAll.at(FVisible.at(Row)).Name == Name)
    {
      emit dataChanged(index(Row), index(Row), {ThumbnailRole});
      break;
    }
  }
}

int FileListModel::rowCount(const QModelIndex & Parent) const
{
  return Parent.isValid() ? 0 : static_cast<int>(FVisible.size());
}

QVariant FileListModel::data(const QModelIndex & Index, int Role) const
{
  if (!Index.isValid() || (Index.row() >= FVisible.size()))
  {
    return QVariant();
  }
  const Entry & E = FAll.at(FVisible.at(Index.row()));
  switch (Role)
  {
    case NameRole: return E.Name;
    case IsDirRole: return E.IsDir;
    case IsLinkRole: return E.IsLink;
    case SizeRole: return E.IsDir ? QString() : QLocale().formattedDataSize(E.Size, 1, QLocale::DataSizeTraditionalFormat);
    case DateRole: return QLocale().toString(E.Modified, QLocale::ShortFormat);
    case RightsRole: return E.Rights;
    case SelectedRole: return FSelected.contains(E.Name);
    case ModeRole: return E.Mode;
    case OwnerRole: return E.Owner;
    case GroupRole: return E.Group;
    case ThumbnailRole: return E.Thumbnail;
    case PreviewRole: return E.IsDir ? QString() : previewKind(E.Name);
    default: return QVariant();
  }
}

QHash<int, QByteArray> FileListModel::roleNames() const
{
  return {{NameRole, "name"}, {IsDirRole, "isDir"}, {IsLinkRole, "isLink"}, {SizeRole, "size"},
    {DateRole, "date"}, {RightsRole, "rights"}, {SelectedRole, "selected"}, {ModeRole, "mode"},
    {OwnerRole, "owner"}, {GroupRole, "group"}, {ThumbnailRole, "thumbnail"}, {PreviewRole, "preview"}};
}
