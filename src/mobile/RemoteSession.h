// One connection to a saved site for the touch interface (a core Terminal):
// browse, download, upload, delete, rename, new folder. Downloads go to the
// app's download folder; uploads come from local paths (the document picker
// copies content:// files there first).
#pragma once

#include "core/RemoteFile.h"
#include "core/Terminal.h"

#include <QAbstractListModel>
#include <QPointer>

class MobileUserInterface;

class FilesModel : public QAbstractListModel
{
  Q_OBJECT

public:
  enum Roles { NameRole = Qt::UserRole + 1, IsDirRole, SizeRole, DateRole, RightsRole, IsLinkRole };
  explicit FilesModel(QObject * Parent = nullptr) : QAbstractListModel(Parent) {}
  void SetFiles(const RemoteFileList & Files);
  const RemoteFileList & Files() const { return FFiles; }
  int rowCount(const QModelIndex & Parent = QModelIndex()) const override;
  QVariant data(const QModelIndex & Index, int Role) const override;
  QHash<int, QByteArray> roleNames() const override;

private:
  RemoteFileList FFiles;
};

class RemoteSession : public QObject
{
  Q_OBJECT
  Q_PROPERTY(bool connected READ connected NOTIFY stateChanged)
  Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
  Q_PROPERTY(QString site READ site NOTIFY stateChanged)
  Q_PROPERTY(QString directory READ directory NOTIFY directoryChanged)
  Q_PROPERTY(QString status READ status NOTIFY statusChanged)
  Q_PROPERTY(QString downloadFolder READ downloadFolder CONSTANT)
  Q_PROPERTY(FilesModel * files READ files CONSTANT)

public:
  RemoteSession(MobileUserInterface * UI, QObject * Parent = nullptr);
  ~RemoteSession() override;

  bool connected() const;
  bool busy() const { return FBusy; }
  QString site() const { return FSite; }
  QString directory() const { return FDirectory; }
  QString status() const { return FStatus; }
  QString downloadFolder() const;
  FilesModel * files() { return &FFiles; }

  Q_INVOKABLE void open(const QString & SiteName);
  Q_INVOKABLE void close();
  Q_INVOKABLE void cd(const QString & Path);
  Q_INVOKABLE void enter(const QString & Name);   // a folder of the listing, ".." = up
  Q_INVOKABLE void refresh();
  Q_INVOKABLE void download(const QStringList & Names);
  // downloads one file into the cache and emits opened() (QML hands it to
  // another app: Open, or Share when Share is set)
  Q_INVOKABLE void openFile(const QString & Name, bool Share = false);
  // local paths, file:// URLs or content:// documents
  Q_INVOKABLE void upload(const QStringList & Sources);
  Q_INVOKABLE void remove(const QStringList & Names);
  Q_INVOKABLE void rename(const QString & From, const QString & To);
  Q_INVOKABLE void createFolder(const QString & Name);

signals:
  void stateChanged();
  void directoryChanged();
  void statusChanged();
  // an operation failed (message and details for the user)
  void error(const QString & message, const QString & details);
  // a download finished: the local files
  void downloaded(const QStringList & paths);
  // a file of openFile() is ready
  void opened(const QString & path, bool share);

private:
  MobileUserInterface * FUI;
  QPointer<Terminal> FTerminal;
  FilesModel FFiles;
  QString FSite;
  QString FDirectory;
  QString FStatus;
  bool FBusy = false;

  void SetBusy(bool Busy, const QString & Status = QString());
  void Load(const QString & Path);
  QString RemotePath(const QString & Name) const;
  bool Failed(const OperationResult & Result);
};
