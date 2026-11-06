// The open sessions (tabs): each a RemoteSession with its own connection,
// files and transfers. QML's "session" is the current one (main.cpp sets
// it on currentChanged); the results of every session (downloads, edits,
// errors...) are forwarded from here, so a background tab is heard too.
// A session that ends (closed, failed, lost) is removed.
#pragma once

#include "mobile/RemoteSession.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>

class MobileSettings;
class MobileUserInterface;
class MobileQueue;

class SessionManager : public QObject
{
  Q_OBJECT
  // never null: an idle session when none is open
  Q_PROPERTY(RemoteSession * current READ current NOTIFY currentChanged)
  Q_PROPERTY(int currentIndex READ currentIndex WRITE setCurrentIndex NOTIFY currentChanged)
  Q_PROPERTY(QList<QObject *> sessions READ sessions NOTIFY sessionsChanged)
  Q_PROPERTY(int count READ count NOTIFY sessionsChanged)
  // some session transfers (the background service runs meanwhile)
  Q_PROPERTY(bool transferring READ transferring NOTIFY transferChanged)
  Q_PROPERTY(QString transferStatus READ transferStatus NOTIFY transferChanged)
  // Copy / Cut on the server: what is waiting to be pasted (the current tab
  // pastes it when it is the same session)
  Q_PROPERTY(int clipboardCount READ clipboardCount NOTIFY clipboardChanged)
  Q_PROPERTY(bool clipboardMove READ clipboardMove NOTIFY clipboardChanged)
  Q_PROPERTY(bool canPaste READ canPaste NOTIFY clipboardChanged)

public:
  SessionManager(MobileUserInterface * UI, MobileSettings * Settings, MobileQueue * Queue, QObject * Parent = nullptr);
  ~SessionManager() override;

  RemoteSession * current() const;
  int currentIndex() const { return FCurrent; }
  void setCurrentIndex(int Index);
  QList<QObject *> sessions() const;
  int count() const { return static_cast<int>(FSessions.size()); }
  bool transferring() const;
  QString transferStatus() const;
  int clipboardCount() const { return static_cast<int>(FClipboard.size()); }
  bool clipboardMove() const { return FClipboardMove; }
  bool canPaste() const { return !FClipboard.isEmpty() && (FClipboardOwner != nullptr) && (FClipboardOwner == current()); }

  // names of the current tab's folder
  Q_INVOKABLE void copyFiles(const QStringList & Names, bool Move);
  Q_INVOKABLE void pasteFiles();
  Q_INVOKABLE void clearClipboard();

  // a new tab connecting to the site (the current one)
  Q_INVOKABLE void open(const QString & Site);
  Q_INVOKABLE void closeCurrent();
  Q_INVOKABLE void closeAt(int Index);
  // edited files of every session go to their servers
  Q_INVOKABLE void checkEdits();
  // at the end of the app: every connection, before libssh is finalized
  void CloseAll();

signals:
  void currentChanged();
  void clipboardChanged();
  void previewReady(QObject * session, const QString & localPath, const QString & name);
  void sessionsChanged();
  void transferChanged();
  void established(const QString & site);
  void error(const QString & site, const QString & message, const QString & details);
  void downloaded(const QStringList & paths);
  void copied(const QString & targetDir, int count);
  void uploaded(const QString & directory, int count);
  void opened(const QString & path, bool share);
  void shared(const QStringList & paths);
  void editReady(QObject * session, const QString & localPath, const QString & remotePath);
  void editUnsent(const QString & site, const QString & localPath);
  // a session ended (before it is removed): what belongs to it moves on
  void sessionEnded(QObject * session);
  void archived(const QString & path);
  void notice(const QString & message);

private:
  MobileUserInterface * FUI;
  MobileSettings * FSettings;
  MobileQueue * FQueue;
  QStringList FClipboard;
  bool FClipboardMove = false;
  QPointer<RemoteSession> FClipboardOwner;
  RemoteSession * FIdle;
  QList<RemoteSession *> FSessions;
  // the site of each session, also when it has just closed
  QHash<RemoteSession *, QString> FLastSite;
  int FCurrent = -1;

  void Watch(RemoteSession * Session);
  void Remove(RemoteSession * Session);
};
