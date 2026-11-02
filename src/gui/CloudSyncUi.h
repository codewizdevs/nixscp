// NixSCP Pro cloud sync user interface (core/CloudSync):
//  - Options > Cloud Sync... dialog: browser sign-in, sync password, options,
//  - status bar indicator of the main window and the conflict prompt,
//  - open tabs uploaded to the "session" slot and reopened at startup.
#pragma once

#include "core/SessionData.h"

#include <QByteArray>
#include <QList>

#include <functional>

class CommanderWindow;
class QMenu;
class QWidget;

namespace CloudSyncUi
{
  void ShowDialog(QWidget * Parent);
  // Top menu after Help: "Cloud Sync" when signed out, "Pro" when signed in;
  // filled every time it opens
  void SetupMenu(QMenu * Menu, QWidget * Window);

  // Tools > Export/Backup Configuration: the configuration, with the cloud
  // sync key when the user chooses so (WinSCP ignores the extra section)
  bool ExportConfiguration(QWidget * Parent, const QString & FileName, QString * Error);
  // Tools > Import/Restore Configuration: a cloud sync key in the backup is
  // taken out of the configuration and used for syncing
  bool ImportConfiguration(QWidget * Parent, const QString & FileName, QString * Error);

  // Status bar indicator, conflict prompt and session uploads for the window;
  // call SessionsChanged when tabs open, close or change directory
  void Attach(CommanderWindow * Window);
  void SessionsChanged(CommanderWindow * Window);
  // On exit: uploads pending settings and the open tabs, waiting a few seconds at most
  void FlushOnExit(CommanderWindow * Window, const QList<SessionData> & Tabs);

  // The tabs as a workspace in WinSCP.ini format (tabs of stored sites are
  // links to them), and back, resolved against this computer's sites
  QByteArray SessionPayload(const QList<SessionData> & Tabs);
  QList<SessionData> SessionsFromPayload(const QByteArray & Payload);

  // Startup: opens the tabs saved last on any computer; Done(false) when
  // there is nothing to reopen (the Login dialog is shown then)
  void RestoreSessions(CommanderWindow * Window, const std::function<void(bool Restored)> & Done);
}
