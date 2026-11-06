// The phone's own settings (Settings page), kept in mobile.ini next to the
// configuration: connection timeout and keepalive for every site opened on
// the phone, the editor for remote files, the theme, confirmations, the
// file panes' view and the archive format.
#pragma once

#include "mobile/PrivateState.h"

#include <QObject>
#include <QSettings>

class SessionData;

class MobileSettings : public QObject
{
  Q_OBJECT
  // seconds
  Q_PROPERTY(int timeout READ timeout WRITE setTimeout NOTIFY changed)
  // seconds between keepalives, 0 = off
  Q_PROPERTY(int keepalive READ keepalive WRITE setKeepalive NOTIFY changed)
  // "builtin", "ask" (Android's chooser) or the package of an app
  Q_PROPERTY(QString editor READ editor WRITE setEditor NOTIFY changed)
  Q_PROPERTY(QString editorName READ editorName WRITE setEditorName NOTIFY changed)
  // 0 = as the system, 1 = light, 2 = dark
  Q_PROPERTY(int theme READ theme WRITE setTheme NOTIFY changed)
  Q_PROPERTY(bool confirmDelete READ confirmDelete WRITE setConfirmDelete NOTIFY changed)
  Q_PROPERTY(bool confirmOverwrite READ confirmOverwrite WRITE setConfirmOverwrite NOTIFY changed)
  // file panes: 0 name, 1 date, 2 size
  Q_PROPERTY(int sortBy READ sortBy WRITE setSortBy NOTIFY changed)
  Q_PROPERTY(bool descending READ descending WRITE setDescending NOTIFY changed)
  Q_PROPERTY(bool showHidden READ showHidden WRITE setShowHidden NOTIFY changed)
  // "tar.gz" or "zip"
  Q_PROPERTY(QString archiveFormat READ archiveFormat WRITE setArchiveFormat NOTIFY changed)
  // thumbnails of server images (fetched on the background connection)
  Q_PROPERTY(bool thumbnails READ thumbnails WRITE setThumbnails NOTIFY changed)

public:
  explicit MobileSettings(QObject * Parent = nullptr);

  int timeout() const { return Int(QStringLiteral("Timeout"), 15); }
  void setTimeout(int Value) { Set(QStringLiteral("Timeout"), qBound(5, Value, 600)); }
  int keepalive() const { return Int(QStringLiteral("Keepalive"), 30); }
  void setKeepalive(int Value) { Set(QStringLiteral("Keepalive"), qBound(0, Value, 3600)); }
  QString editor() const { return String(QStringLiteral("Editor"), QStringLiteral("builtin")); }
  void setEditor(const QString & Value) { Set(QStringLiteral("Editor"), Value); }
  QString editorName() const { return String(QStringLiteral("EditorName"), QString()); }
  void setEditorName(const QString & Value) { Set(QStringLiteral("EditorName"), Value); }
  int theme() const { return Int(QStringLiteral("Theme"), 0); }
  void setTheme(int Value) { Set(QStringLiteral("Theme"), Value); }
  bool confirmDelete() const { return Bool(QStringLiteral("ConfirmDelete"), true); }
  void setConfirmDelete(bool Value) { Set(QStringLiteral("ConfirmDelete"), Value); }
  bool confirmOverwrite() const { return Bool(QStringLiteral("ConfirmOverwrite"), true); }
  void setConfirmOverwrite(bool Value);
  int sortBy() const { return Int(QStringLiteral("SortBy"), 0); }
  void setSortBy(int Value) { Set(QStringLiteral("SortBy"), Value); }
  bool descending() const { return Bool(QStringLiteral("Descending"), false); }
  void setDescending(bool Value) { Set(QStringLiteral("Descending"), Value); }
  bool showHidden() const { return Bool(QStringLiteral("ShowHidden"), false); }
  void setShowHidden(bool Value) { Set(QStringLiteral("ShowHidden"), Value); }
  QString archiveFormat() const { return String(QStringLiteral("ArchiveFormat"), QStringLiteral("tar.gz")); }
  void setArchiveFormat(const QString & Value) { Set(QStringLiteral("ArchiveFormat"), Value); }
  bool thumbnails() const { return Bool(QStringLiteral("Thumbnails"), true); }
  void setThumbnails(bool Value) { Set(QStringLiteral("Thumbnails"), Value); }
  // the site's recently visited folders, newest first (at most 10)
  QStringList recentFolders(const QString & Site) const;
  void addRecentFolder(const QString & Site, const QString & Folder);
  // The SSH key a site uses on this phone (a key of KeyStore, by name): the
  // site's own key file (PublicKeyFile, synced) is another device's path
  QString siteKey(const QString & Site) const;
  void setSiteKey(const QString & Site, const QString & KeyName);

  // what names sites, folders and keys (encrypted with the configuration)
  PrivateState & Private() { return FPrivate; }

  // the phone's settings kept in the configuration (after it is loaded, and
  // after every change of it: a cloud pull brings another device's settings)
  void ApplyToConfiguration();
  // The site as opened on the phone: these settings over its own
  void Apply(SessionData & Data) const;

signals:
  void changed();

private:
  mutable QSettings FSettings;
  PrivateState FPrivate;

  int Int(const QString & Key, int Default) const;
  bool Bool(const QString & Key, bool Default) const;
  QString String(const QString & Key, const QString & Default) const;
  void Set(const QString & Key, const QVariant & Value);
};
