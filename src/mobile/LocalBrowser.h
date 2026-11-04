// The phone pane: a folder of the phone's shared storage (Download, DCIM,
// Documents...), for copying to and from the server, opening, sharing,
// renaming and deleting. On Android it needs "All files access" (Android
// 11+) or the storage permission (Android 9/10); without it the pane asks.
#pragma once

#include "mobile/FileListModel.h"

#include <QObject>

class LocalBrowser : public QObject
{
  Q_OBJECT
  Q_PROPERTY(QString directory READ directory NOTIFY directoryChanged)
  // the folder under the storage root, as shown ("Download/NixSCP"; "" = the root)
  Q_PROPERTY(QString displayDirectory READ displayDirectory NOTIFY directoryChanged)
  Q_PROPERTY(QString root READ root CONSTANT)
  Q_PROPERTY(bool atRoot READ atRoot NOTIFY directoryChanged)
  Q_PROPERTY(bool accessible READ accessible NOTIFY accessChanged)
  Q_PROPERTY(FileListModel * files READ files CONSTANT)

public:
  explicit LocalBrowser(QObject * Parent = nullptr);

  QString directory() const { return FDirectory; }
  QString displayDirectory() const;
  QString root() const { return FRoot; }
  bool atRoot() const;
  bool accessible() const { return FAccessible; }
  FileListModel * files() { return &FFiles; }

  // Path: absolute, or relative to the root
  Q_INVOKABLE void cd(const QString & Path);
  Q_INVOKABLE void enter(const QString & Name);
  Q_INVOKABLE void up();
  Q_INVOKABLE void refresh();
  Q_INVOKABLE QString path(const QString & Name) const;
  Q_INVOKABLE QStringList paths(const QStringList & Names) const;
  // false with the reason in lastError
  Q_INVOKABLE bool remove(const QStringList & Names);
  Q_INVOKABLE bool rename(const QString & From, const QString & To);
  Q_INVOKABLE bool createFolder(const QString & Name);
  // an empty file (not one that exists)
  Q_INVOKABLE bool createFile(const QString & Name);
  Q_INVOKABLE QString lastError() const { return FLastError; }
  // re-checks the permission (after coming back from the system settings)
  Q_INVOKABLE void checkAccess();
  Q_INVOKABLE void requestAccess();

signals:
  void directoryChanged();
  void accessChanged();

private:
  FileListModel FFiles;
  QString FRoot;
  QString FDirectory;
  QString FLastError;
  bool FAccessible = false;

  bool Load(const QString & Directory);
};
