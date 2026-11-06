#include "mobile/MobileQueue.h"
#include "mobile/RemoteSession.h"

#include <QDir>
#include <QFileInfo>

#include <algorithm>

int MobileQueue::active() const
{
  int Count = 0;
  for (const Item & I : FItems)
  {
    if ((I.St == Waiting) || (I.St == Running))
    {
      Count++;
    }
  }
  return Count;
}

int MobileQueue::finished() const
{
  return static_cast<int>(FItems.size()) - active();
}

double MobileQueue::progress() const
{
  for (const Item & I : FItems)
  {
    if (I.St == Running)
    {
      return I.Progress;
    }
  }
  return -1;
}

QString MobileQueue::summary() const
{
  const Item * Running = nullptr;
  int Waiting = 0;
  for (const Item & I : FItems)
  {
    if ((I.St == MobileQueue::Running) && (Running == nullptr))
    {
      Running = &I;
    }
    else if (I.St == MobileQueue::Waiting)
    {
      Waiting++;
    }
  }
  QStringList Parts;
  if (Running != nullptr)
  {
    Parts.append((Running->Dir == Download ? QStringLiteral("↓ ") : QStringLiteral("↑ ")) + Running->Title);
    if (Running->Progress >= 0)
    {
      Parts.append(QStringLiteral("%1%").arg(qRound(Running->Progress * 100)));
    }
  }
  if (Waiting > 0)
  {
    Parts.append(QStringLiteral("%1 waiting").arg(Waiting));
  }
  return Parts.join(QStringLiteral(" · "));
}

int MobileQueue::RowOf(int Id) const
{
  for (int Row = 0; Row < FItems.size(); Row++)
  {
    if (FItems.at(Row).Id == Id)
    {
      return Row;
    }
  }
  return -1;
}

const MobileQueue::Item * MobileQueue::Find(int Id) const
{
  int Row = RowOf(Id);
  return (Row >= 0) ? &FItems.at(Row) : nullptr;
}

int MobileQueue::Add(RemoteSession * Owner, Direction Dir, const QStringList & Sources, const QString & Target, bool Publish,
  const QString & CacheFolder)
{
  Item I;
  I.Id = FNextId++;
  I.Owner = Owner;
  I.Site = Owner->site();
  I.Dir = Dir;
  I.Sources = Sources;
  I.Target = Target;
  I.Publish = Publish;
  I.CacheFolder = CacheFolder;
  QString First = QFileInfo(Sources.value(0)).fileName();
  I.Title = (Sources.size() == 1) ? First : QStringLiteral("%1 and %2 more").arg(First).arg(Sources.size() - 1);
  int Row = static_cast<int>(FItems.size());
  beginInsertRows(QModelIndex(), Row, Row);
  FItems.append(I);
  endInsertRows();
  emit changed();
  emit added(Owner);
  return I.Id;
}

bool MobileQueue::HasActive(const RemoteSession * Owner) const
{
  return std::any_of(FItems.cbegin(), FItems.cend(), [Owner](const Item & I)
  {
    return (I.Owner == Owner) && ((I.St == Waiting) || (I.St == Running));
  });
}

void MobileQueue::RemoveCopies(Item & I)
{
  if (!I.CacheFolder.isEmpty())
  {
    QDir(I.CacheFolder).removeRecursively();
    I.CacheFolder.clear();
  }
}

const MobileQueue::Item * MobileQueue::Next(RemoteSession * Owner) const
{
  for (const Item & I : FItems)
  {
    if ((I.Owner == Owner) && (I.St == Waiting))
    {
      return &I;
    }
  }
  return nullptr;
}

void MobileQueue::SetState(int Id, State St, const QString & Error)
{
  int Row = RowOf(Id);
  if (Row < 0)
  {
    return;
  }
  Item & I = FItems[Row];
  I.St = St;
  I.Error = Error;
  if (St == Done)
  {
    I.Progress = 1;
    RemoveCopies(I);
    int Count = static_cast<int>(I.Sources.size());
    if (I.Dir == Upload)
    {
      emit uploaded(I.Target, Count);
    }
    else if (I.Publish)
    {
      QStringList Local;
      for (const QString & Source : I.Sources)
      {
        Local.append(QDir(I.Target).filePath(QFileInfo(Source).fileName()));
      }
      emit downloaded(Local);
    }
    else
    {
      emit copied(I.Target, Count);
    }
  }
  else if ((St == Failed) && !Error.isEmpty())
  {
    emit failed(I.Site, Error, QString());
  }
  emit dataChanged(index(Row), index(Row));
  emit changed();
}

void MobileQueue::SetProgress(int Id, double Progress, const QString & Detail)
{
  int Row = RowOf(Id);
  if (Row < 0)
  {
    return;
  }
  FItems[Row].Progress = Progress;
  FItems[Row].Detail = Detail;
  emit dataChanged(index(Row), index(Row), {ProgressRole, DetailRole});
  emit changed();
}

void MobileQueue::Abandon(RemoteSession * Owner)
{
  for (const Item & I : std::as_const(FItems))
  {
    if ((I.Owner == Owner) && ((I.St == Waiting) || (I.St == Running)))
    {
      SetState(I.Id, Cancelled);
    }
  }
  // no Retry without the session: the rows change what they offer
  for (int Row = 0; Row < FItems.size(); Row++)
  {
    if (FItems.at(Row).Owner == Owner)
    {
      RemoveCopies(FItems[Row]);
      emit dataChanged(index(Row), index(Row), {CanRetryRole});
    }
  }
}

void MobileQueue::cancel(int Id)
{
  const Item * I = Find(Id);
  if (I == nullptr)
  {
    return;
  }
  if (I->St == Waiting)
  {
    SetState(Id, Cancelled);
  }
  else if (I->St == Running)
  {
    emit cancelRequested(Id);
  }
}

void MobileQueue::retry(int Id)
{
  int Row = RowOf(Id);
  if ((Row < 0) || (FItems.at(Row).Owner == nullptr) || !FItems.at(Row).Owner->connected() ||
      (FItems.at(Row).St == Waiting) || (FItems.at(Row).St == Running))
  {
    return;
  }
  FItems[Row].St = Waiting;
  FItems[Row].Progress = -1;
  FItems[Row].Error.clear();
  FItems[Row].Detail.clear();
  emit dataChanged(index(Row), index(Row));
  emit changed();
  emit added(FItems.at(Row).Owner);
}

void MobileQueue::clearFinished()
{
  for (int Row = static_cast<int>(FItems.size()) - 1; Row >= 0; Row--)
  {
    State St = FItems.at(Row).St;
    if ((St != Waiting) && (St != Running))
    {
      RemoveCopies(FItems[Row]);
      beginRemoveRows(QModelIndex(), Row, Row);
      FItems.removeAt(Row);
      endRemoveRows();
    }
  }
  emit changed();
}

int MobileQueue::rowCount(const QModelIndex & Parent) const
{
  return Parent.isValid() ? 0 : static_cast<int>(FItems.size());
}

QVariant MobileQueue::data(const QModelIndex & Index, int Role) const
{
  if (!Index.isValid() || (Index.row() >= FItems.size()))
  {
    return QVariant();
  }
  const Item & I = FItems.at(Index.row());
  static const char * States[] = {"waiting", "running", "done", "failed", "cancelled"};
  switch (Role)
  {
    case IdRole: return I.Id;
    case SiteRole: return I.Site;
    case TitleRole: return I.Title;
    case DirectionRole: return (I.Dir == Download) ? QStringLiteral("down") : QStringLiteral("up");
    case StateRole: return QString::fromLatin1(States[I.St]);
    case ProgressRole: return I.Progress;
    case DetailRole: return I.Detail;
    case ErrorRole: return I.Error;
    case TargetRole: return I.Target;
    // the connection of a closed tab is gone
    case CanRetryRole: return (I.Owner != nullptr) && I.Owner->connected() && ((I.St == Failed) || (I.St == Cancelled));
    default: return QVariant();
  }
}

QHash<int, QByteArray> MobileQueue::roleNames() const
{
  return {{IdRole, "itemId"}, {SiteRole, "site"}, {TitleRole, "title"}, {DirectionRole, "direction"},
    {StateRole, "status"}, {ProgressRole, "progress"}, {DetailRole, "detail"}, {ErrorRole, "error"}, {TargetRole, "target"},
    {CanRetryRole, "canRetry"}};
}
