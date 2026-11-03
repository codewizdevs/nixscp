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
  // While a transfer runs: a foreground service (with a notification) keeps
  // Android from stopping the app and cutting its connection in the background
  Q_INVOKABLE void keepAlive(bool Busy, const QString & Text);
  // Copies downloaded files into the public Downloads/NixSCP folder (shown by
  // the Files app); the places they went, empty where it failed
  Q_INVOKABLE QStringList publish(const QStringList & Paths);

private:
  bool FKeptAlive = false;
  bool FAskedNotifications = false;
};
