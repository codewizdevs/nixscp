// Handing local files to other apps: Open (ACTION_VIEW) and Share
// (ACTION_SEND), through the app's FileProvider as content:// URIs with a
// read grant (Android forbids file:// URIs between apps). Elsewhere Open
// uses QDesktopServices and Share is unavailable.
#pragma once

#include <QObject>
#include <QStringList>
#include <QVariantList>

class AndroidFiles : public QObject
{
  Q_OBJECT
  // files shared to NixSCP from another app (Share -> NixSCP), to upload:
  // content:// URIs and their names
  Q_PROPERTY(QStringList sharedFiles READ sharedFiles NOTIFY sharedFilesChanged)
  Q_PROPERTY(QStringList sharedNames READ sharedNames NOTIFY sharedFilesChanged)

public:
  explicit AndroidFiles(QObject * Parent = nullptr);
  ~AndroidFiles() override;

  // Reads the intent NixSCP was started with and listens for new ones
  // (shared files, a site's home-screen shortcut)
  void Start();
  QStringList sharedFiles() const { return FShared; }
  QStringList sharedNames() const { return FSharedNames; }
  Q_INVOKABLE void clearShared();
  // A home-screen shortcut opening the site (the launcher asks first)
  Q_INVOKABLE bool pinShortcut(const QString & Site);
  // The name of a content:// document (its display name), or the path's
  static QString DisplayName(const QString & Uri);
  // false: no app can open it
  Q_INVOKABLE bool open(const QString & Path);
  Q_INVOKABLE bool share(const QStringList & Paths);
  // Shares a text (a public key...) through Android's share sheet
  Q_INVOKABLE bool shareText(const QString & Text);
  // Hands the file to an editor with write access (the changes come back
  // into the file): Package = an app's package, or "ask" for Android's chooser
  Q_INVOKABLE bool edit(const QString & Path, const QString & Package);
  // The apps that edit or view text: [{name, package}]
  Q_INVOKABLE QVariantList editors() const;
  // While a transfer runs: a foreground service (with a notification) keeps
  // Android from stopping the app and cutting its connection in the background
  // Percent: of the notification's progress bar, -1 = unknown
  Q_INVOKABLE void keepAlive(bool Busy, const QString & Text, int Percent = -1);
  // Copies downloaded files into the public Downloads/NixSCP folder (shown by
  // the Files app); the places they went, empty where it failed
  Q_INVOKABLE QStringList publish(const QStringList & Paths);

  // The phone's shared storage (/storage/emulated/0), and whether the app
  // may use all of it: "All files access" on Android 11+, the storage
  // permission on Android 9 and 10. The request opens the system setting
  // (or asks); the answer is known when the app is active again.
  static QString StorageRoot();
  static bool HasStorageAccess();
  static void RequestStorageAccess();

signals:
  void sharedFilesChanged();
  // a site's shortcut was used
  void openSiteRequested(const QString & site);

private:
  class IntentListener;
  IntentListener * FListener = nullptr;
  QStringList FShared;
  QStringList FSharedNames;
  QString FLastText;
  int FLastPercent = -2;
  qint64 FLastUpdate = 0;
  bool FKeptAlive = false;
  void HandleIntent(const QString & Action, const QStringList & Uris, const QString & Site);
  bool FAskedNotifications = false;
};
