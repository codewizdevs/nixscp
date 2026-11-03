// Handing local files to other apps: Open (ACTION_VIEW) and Share
// (ACTION_SEND), through the app's FileProvider as content:// URIs with a
// read grant (Android forbids file:// URIs between apps). Elsewhere Open
// uses QDesktopServices and Share is unavailable.
#pragma once

#include <QObject>
#include <QStringList>

class AndroidFiles : public QObject
{
  Q_OBJECT

public:
  explicit AndroidFiles(QObject * Parent = nullptr) : QObject(Parent) {}
  // false: no app can open it
  Q_INVOKABLE bool open(const QString & Path);
  Q_INVOKABLE bool share(const QStringList & Paths);
};
