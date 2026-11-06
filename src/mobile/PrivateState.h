// What the phone keeps about its sites beside the configuration:
// favourites, recent sites, collapsed folders, recent folders and the key of
// each site. All of it names sites, folders or keys, so it is one file
// encrypted with the configuration's key while encryption is on (mobile.ini
// stays plain: the theme and the lock are needed before the unlock). It is
// read after the unlock, as the configuration.
//
// MobileCache: the app's cache folder. Each session has its own folders
// there (tabs never share files); they go with the session, and what a
// killed app left is removed at every start.
#pragma once

#include <QString>
#include <QStringList>
#include <QVariantMap>

class PrivateState
{
public:
  static QString DefaultFile();
  explicit PrivateState(const QString & FileName = DefaultFile()) : FFileName(FileName) {}

  void Load();
  // After encryption was turned on or off: the file as it should be now
  void Reseal();
  QStringList Strings(const QString & Key) const;
  // false: it could not be written
  bool SetStrings(const QString & Key, const QStringList & Value);

private:
  QString FFileName;
  QVariantMap FData;
  bool FLoaded = false;

  bool Save();
};

namespace MobileCache
{
  QString Root();
  // A session's folder of this kind ("edit", "open", "upload", "archive")
  QString Folder(const QString & Kind, int Session);
  // The thumbnails of a session: shared by all and kept between runs, but
  // with encryption on the session's own, which go with it
  QString Thumbnails(int Session);
  // Removes the session's folders
  void RemoveSession(int Session);
  // At start, before anything is read: what an earlier run left
  void Clean(bool Encrypted);
  // Encryption was turned on: the thumbnails kept between runs go
  void RemoveSharedThumbnails();
}
