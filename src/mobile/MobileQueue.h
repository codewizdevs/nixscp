// The transfer queue of all tabs: downloads and uploads waiting, running
// and finished. Each session runs its own items, one at a time, on a second
// connection (RemoteSession's background terminal), so browsing goes on
// meanwhile; the transfer service keeps them running with the app in the
// background, its notification showing summary and progress.
#pragma once

#include <QAbstractListModel>
#include <QPointer>
#include <QStringList>

class RemoteSession;

class MobileQueue : public QAbstractListModel
{
  Q_OBJECT
  // waiting and running items
  Q_PROPERTY(int active READ active NOTIFY changed)
  Q_PROPERTY(int finished READ finished NOTIFY changed)
  // the running item's progress 0..1 (-1 unknown or none)
  Q_PROPERTY(double progress READ progress NOTIFY changed)
  // "report.pdf · 45% · 2 waiting"
  Q_PROPERTY(QString summary READ summary NOTIFY changed)

public:
  enum Direction { Download, Upload };
  enum State { Waiting, Running, Done, Failed, Cancelled };
  struct Item
  {
    int Id = 0;
    QPointer<RemoteSession> Owner;
    QString Site;
    Direction Dir = Download;
    QStringList Sources;   // remote paths (download) or local paths (upload)
    QString Target;        // local folder (download) or remote folder (upload)
    bool Publish = false;  // downloaded into the app's folder: to Download/NixSCP through Android
    QString CacheFolder;   // the copies made for an upload, kept for Retry
    QString Title;
    State St = Waiting;
    double Progress = -1;
    QString Detail;
    QString Error;
  };
  enum Roles { IdRole = Qt::UserRole + 1, SiteRole, TitleRole, DirectionRole, StateRole, ProgressRole, DetailRole,
    ErrorRole, TargetRole, CanRetryRole };

  explicit MobileQueue(QObject * Parent = nullptr) : QAbstractListModel(Parent) {}

  int active() const;
  int finished() const;
  double progress() const;
  QString summary() const;

  int Add(RemoteSession * Owner, Direction Dir, const QStringList & Sources, const QString & Target, bool Publish,
    const QString & CacheFolder = QString());
  // an item of the session waits or runs
  bool HasActive(const RemoteSession * Owner) const;
  // the session's first waiting item (null: none)
  const Item * Next(RemoteSession * Owner) const;
  const Item * Find(int Id) const;
  void SetState(int Id, State St, const QString & Error = QString());
  void SetProgress(int Id, double Progress, const QString & Detail);
  // a closed session: its waiting items are cancelled
  void Abandon(RemoteSession * Owner);

  Q_INVOKABLE void cancel(int Id);
  Q_INVOKABLE void retry(int Id);
  Q_INVOKABLE void clearFinished();

  int rowCount(const QModelIndex & Parent = QModelIndex()) const override;
  QVariant data(const QModelIndex & Index, int Role) const override;
  QHash<int, QByteArray> roleNames() const override;

signals:
  void changed();
  // an item for the session was added (or retried): it may start
  void added(RemoteSession * owner);
  // cancel of a running item (its session stops the transfer)
  void cancelRequested(int id);
  // an upload into this folder ended, in any way: its listing changed
  void uploadEnded(const QString & directory);
  // results, as RemoteSession's
  void downloaded(const QStringList & paths);
  void copied(const QString & targetDir, int count);
  void uploaded(const QString & directory, int count);
  void failed(const QString & site, const QString & message, const QString & details);

private:
  QList<Item> FItems;
  int FNextId = 1;

  int RowOf(int Id) const;
  void RemoveCopies(Item & I);
};
