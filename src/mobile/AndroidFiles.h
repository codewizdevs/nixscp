// Handing local files to other apps: Open (ACTION_VIEW) and Share
// (ACTION_SEND), through the app's FileProvider as content:// URIs with a
// read grant (Android forbids file:// URIs between apps). Elsewhere Open
// uses QDesktopServices and Share is unavailable.
#pragma once

#include <QMap>
#include <QObject>
#include <QStringList>
#include <QUrl>
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
  // Percent: of the notification's progress bar, -1 = unknown. Who: what
  // needs it; the service runs while anything does, so the end of a sign-in
  // does not stop the transfers, or the other way round
  Q_INVOKABLE void keepAlive(bool Busy, const QString & Text, int Percent = -1, const QString & Who = QStringLiteral("transfers"));
  bool KeptAlive() const { return !FKeepers.isEmpty(); }
  // Copies downloaded files and folders into the public Downloads/NixSCP
  // folder (shown by the Files app); the places they went, empty where it failed
  Q_INVOKABLE QStringList publish(const QStringList & Paths);
  // The text as a file in Downloads/NixSCP, in Encoding when it can be
  // written in it (UTF-8 otherwise); the place, empty on failure
  Q_INVOKABLE QString saveCopy(const QString & Name, const QString & Text, const QString & Encoding);
  // For an Image: any file name is a valid URL
  Q_INVOKABLE QUrl fileUrl(const QString & Path) const { return QUrl::fromLocalFile(Path); }
  // The files below Path (itself, for a file) with their folders relative to its parent
  static QList<QPair<QString, QString>> FilesToPublish(const QString & Path);

  // Screen capture and the recents thumbnail show nothing of the app
  static void SecureWindow();

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
  struct Keeper
  {
    QString Text;
    int Percent = -1;
  };
  QMap<QString, Keeper> FKeepers;
  QString FLastText;
  int FLastPercent = -2;
  qint64 FLastUpdate = 0;
  bool FKeptAlive = false;
  void HandleIntent(const QString & Action, const QStringList & Uris, const QString & Site);
  bool FAskedNotifications = false;
};
