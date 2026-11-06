#include "mobile/SessionManager.h"

#include <QTimer>

SessionManager::SessionManager(MobileUserInterface * UI, MobileSettings * Settings, MobileQueue * Queue, QObject * Parent) :
  QObject(Parent), FUI(UI), FSettings(Settings), FQueue(Queue), FIdle(new RemoteSession(UI, Settings, nullptr, this))
{
  connect(this, &SessionManager::currentChanged, this, &SessionManager::clipboardChanged);
}

void SessionManager::copyFiles(const QStringList & Names, bool Move)
{
  if (Names.isEmpty() || (current() == FIdle))
  {
    return;
  }
  FClipboard = current()->remotePaths(Names);
  FClipboardMove = Move;
  FClipboardOwner = current();
  emit clipboardChanged();
}

void SessionManager::pasteFiles()
{
  if (!canPaste())
  {
    return;
  }
  current()->paste(FClipboard, FClipboardMove);
  // moved files are no longer where they were; copies may be pasted again
  if (FClipboardMove)
  {
    clearClipboard();
  }
}

void SessionManager::clearClipboard()
{
  FClipboard.clear();
  FClipboardOwner = nullptr;
  emit clipboardChanged();
}

SessionManager::~SessionManager()
{
  for (RemoteSession * Session : FSessions)
  {
    disconnect(Session, nullptr, this, nullptr);
  }
}

RemoteSession * SessionManager::current() const
{
  return ((FCurrent >= 0) && (FCurrent < FSessions.size())) ? FSessions.at(FCurrent) : FIdle;
}

void SessionManager::setCurrentIndex(int Index)
{
  if ((Index != FCurrent) && (Index >= -1) && (Index < FSessions.size()))
  {
    FCurrent = Index;
    emit currentChanged();
  }
}

QList<QObject *> SessionManager::sessions() const
{
  QList<QObject *> Result;
  for (RemoteSession * Session : FSessions)
  {
    Result.append(Session);
  }
  return Result;
}

bool SessionManager::transferring() const
{
  for (RemoteSession * Session : FSessions)
  {
    if (Session->transferring())
    {
      return true;
    }
  }
  return false;
}

QString SessionManager::transferStatus() const
{
  for (RemoteSession * Session : FSessions)
  {
    if (Session->transferring())
    {
      return Session->status();
    }
  }
  return QString();
}

void SessionManager::Watch(RemoteSession * Session)
{
  connect(Session, &RemoteSession::established, this, [this, Session]() { emit established(Session->site()); });
  connect(Session, &RemoteSession::closed, this, [this, Session]()
  {
    // not inside the session's own call
    QTimer::singleShot(0, this, [this, Session]() { Remove(Session); });
  });
  // a lost connection ends the tab, shown or not: its queue must not wait for ever
  connect(Session, &RemoteSession::stateChanged, this, [Session]()
  {
    if (!Session->connected() && !Session->busy() && !Session->site().isEmpty())
    {
      QTimer::singleShot(0, Session, [Session]()
      {
        if (!Session->connected() && !Session->busy() && !Session->site().isEmpty())
        {
          Session->close();
        }
      });
    }
  });
  connect(Session, &RemoteSession::error, this, [this, Session](const QString & Message, const QString & Details)
  {
    emit error(Session->site(), Message, Details);
  });
  connect(Session, &RemoteSession::downloaded, this, &SessionManager::downloaded);
  connect(Session, &RemoteSession::copied, this, &SessionManager::copied);
  connect(Session, &RemoteSession::uploaded, this, [this, Session](int Count) { emit uploaded(Session->directory(), Count); });
  connect(Session, &RemoteSession::opened, this, &SessionManager::opened);
  connect(Session, &RemoteSession::shared, this, &SessionManager::shared);
  connect(Session, &RemoteSession::editReady, this, [this, Session](const QString & Local, const QString & Remote)
  {
    emit editReady(Session, Local, Remote);
  });
  connect(Session, &RemoteSession::editUnsent, this, [this, Session](const QString & Local)
  {
    emit editUnsent(FLastSite.value(Session), Local);
  });
  connect(Session, &RemoteSession::archived, this, &SessionManager::archived);
  connect(Session, &RemoteSession::previewReady, this, [this, Session](const QString & Local, const QString & Name)
  {
    emit previewReady(Session, Local, Name);
  });
  connect(Session, &RemoteSession::notice, this, &SessionManager::notice);
  connect(Session, &RemoteSession::progressChanged, this, &SessionManager::transferChanged);
  connect(Session, &RemoteSession::statusChanged, this, &SessionManager::transferChanged);
}

void SessionManager::open(const QString & Site)
{
  auto * Session = new RemoteSession(FUI, FSettings, FQueue, this);
  Watch(Session);
  FSessions.append(Session);
  FLastSite.insert(Session, Site);
  FCurrent = static_cast<int>(FSessions.size()) - 1;
  emit sessionsChanged();
  emit currentChanged();
  Session->open(Site);
}

void SessionManager::closeCurrent()
{
  closeAt(FCurrent);
}

void SessionManager::closeAt(int Index)
{
  if ((Index >= 0) && (Index < FSessions.size()))
  {
    // closed() removes it
    FSessions.at(Index)->close();
  }
}

void SessionManager::Remove(RemoteSession * Session)
{
  int Index = static_cast<int>(FSessions.indexOf(Session));
  if (Index < 0)
  {
    return;
  }
  emit sessionEnded(Session);
  disconnect(Session, nullptr, this, nullptr);
  FSessions.removeAt(Index);
  FLastSite.remove(Session);
  // the neighbour becomes current (the idle session when none is left)
  bool WasCurrent = (Index == FCurrent);
  if (Index < FCurrent)
  {
    FCurrent--;
  }
  else if (WasCurrent)
  {
    FCurrent = std::min(Index, static_cast<int>(FSessions.size()) - 1);
  }
  emit sessionsChanged();
  emit currentChanged();
  emit transferChanged();
  // its connections may still end, with callbacks into it
  Session->DeleteWhenClosed();
}

void SessionManager::CloseAll()
{
  for (RemoteSession * Session : std::as_const(FSessions))
  {
    Session->close();
  }
}

void SessionManager::checkEdits()
{
  for (RemoteSession * Session : FSessions)
  {
    Session->checkEdits();
  }
}
