// Tunnels > Manage Tunnels...: the saved port forwards of all SSH sites
// (SessionData::PortForwards, core/PortForward.h), started and stopped here;
// running tunnels outlive the dialog (TunnelManager) until NixSCP exits.
// The main window has a Tunnels menu (start/stop each saved tunnel) and a
// status bar indicator of the running ones (TunnelsUi).
#pragma once

#include "core/PortForward.h"

#include <QDialog>
#include <QHash>
#include <QPointer>

class QTableWidget;
class QPushButton;
class GuiUserInterface;

// Tunnels started by the user (the GUI answers host key and passphrase prompts)
class TunnelManager : public QObject
{
  Q_OBJECT

public:
  static TunnelManager & Instance();
  // Key: "<site name>|<tunnel name>"
  static QString Key(const QString & Site, const QString & Name);
  void Start(const SessionData & Data, const PortForwardSpec & Spec);
  void Stop(const QString & Key);
  void StopAll();
  PortForward * Find(const QString & Key) const;
  // the tunnels started (connecting or running)
  QList<PortForward *> Tunnels() const;
  QString LastError(const QString & Key) const { return FErrors.value(Key); }

signals:
  void Changed();

private:
  TunnelManager();
  GuiUserInterface * FUI = nullptr;
  QHash<QString, QPointer<PortForward>> FTunnels;
  QHash<QString, QString> FErrors;
};

class QMenu;
class QToolButton;

namespace TunnelsUi
{
  // Fills Menu with the saved tunnels (checked when running; a click starts or
  // stops it), Manage Tunnels... (Manage) and Stop All Tunnels; rebuilt each
  // time it opens
  void SetupMenu(QMenu * Menu, QAction * Manage);
  // A status bar button, shown while tunnels run: "<n> tunnel(s) running",
  // their list as the tooltip; a click opens the Tunnels dialog
  QToolButton * CreateIndicator(QWidget * Window);
  // "db1: postgres 127.0.0.1:5433 -> localhost:5432 (2 connections)"
  QString Describe(const PortForward * Tunnel, const QString & Site);
}

class TunnelsDialog : public QDialog
{
  Q_OBJECT

public:
  explicit TunnelsDialog(QWidget * Parent = nullptr);
  static void Execute(QWidget * Parent);

private:
  QTableWidget * FTable;
  QPushButton * FStart;
  QPushButton * FStop;
  QPushButton * FEdit;
  QPushButton * FRemove;

  void Reload();
  void UpdateButtons();
  // the selected row: site name and tunnel name
  bool Selected(QString & Site, QString & Name) const;
  void Edit(bool New);
  void Remove();
  void StartSelected();
  void StopSelected();
};
