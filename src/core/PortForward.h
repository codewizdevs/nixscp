// Local port forwarding over SSH ("tunnels"): listens on 127.0.0.1:<local
// port> and forwards each connection through an SSH channel of the site to
// <host>:<port> as seen from the server (ssh -L). One SSH connection per
// tunnel, run on its own thread; the credentials stay in this process.
// Saved forwards of a site: SessionData::PortForwards, one per line
// "name|local port|host|port".
#pragma once

#include "core/SessionData.h"

#include <QList>
#include <QObject>
#include <QString>
#include <QThread>

#include <atomic>

class UserInterface;

struct PortForwardSpec
{
  QString Name;
  int LocalPort = 0;
  QString Host;
  int Port = 0;

  bool IsValid() const;
  QString Text() const;   // "5433 -> localhost:5432"
  // "name|5433|localhost|5432"
  QString Save() const;
  static PortForwardSpec Load(const QString & Line);
  // "5433:localhost:5432" (as ssh -L)
  static bool Parse(const QString & Text, PortForwardSpec & Spec, QString * Error = nullptr);
  static QList<PortForwardSpec> LoadAll(const QString & Lines);
  static QString SaveAll(const QList<PortForwardSpec> & Specs);
};

class PortForward : public QObject
{
  Q_OBJECT

public:
  // UI answers host key and passphrase prompts (on the tunnel's thread)
  PortForward(const SessionData & Data, const PortForwardSpec & Spec, UserInterface * UI, QObject * Parent = nullptr);
  ~PortForward() override;

  void Start();
  void Stop();
  bool Running() const { return FRunning; }
  const PortForwardSpec & Spec() const { return FSpec; }
  const SessionData & Data() const { return FData; }
  int Connections() const { return FConnections; }
  qint64 BytesTransferred() const { return FBytes; }

signals:
  // listening (Ok) or failed to start (Error)
  void Started(bool Ok, const QString & Error);
  // ended: stopped (Error empty) or the connection broke
  void Stopped(const QString & Error);
  void ConnectionsChanged(int Count);
  // a local connection could not be forwarded (e.g. the server does not
  // allow forwarding, or nothing listens on the target)
  void ConnectionFailed(const QString & Error);

private:
  SessionData FData;
  PortForwardSpec FSpec;
  UserInterface * FUI;
  QThread * FThread = nullptr;
  std::atomic<bool> FStop{false};
  std::atomic<bool> FRunning{false};
  std::atomic<int> FConnections{0};
  std::atomic<qint64> FBytes{0};

  void Run();
};
