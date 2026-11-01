// Port of the inter-instance part of WinSCP source/windows/WinMain.cpp
// (FindOtherInstances, SendToAnotherInstance, Refresh, TCopyDataMessage) and
// forms/CustomScpExplorer.cpp (WMCopyData, CanCommandLineFromAnotherInstance).
//
// Linux adaptation of WM_COPYDATA: a QLocalServer (Unix domain socket) in
// $XDG_RUNTIME_DIR, named per user and per configuration file, so that only
// instances sharing the same nixscp.ini talk to each other (as WinSCP only
// finds instances of the same executable). The first instance listens; a
// later one sends its command line (Preferences > Window > "Open externally
// initiated sessions in existing window", ExternalSessionInExistingInstance)
// or a "refresh" request (/refresh) and exits.
//
// Protocol (QDataStream, Qt 6.0 format): request = magic, version, command,
// arguments, working directory, activation token; reply = one qint32
// (1 = accepted, 0 = refused, e.g. a modal dialog is open in the running
// instance - TCopyDataMessage::CommandCanCommandLine).
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <functional>

class QLocalServer;
class QLocalSocket;

class SingleInstance : public QObject
{
  Q_OBJECT

public:
  // TCopyDataMessage commands
  enum Command
  {
    CommandCommandLine = 1,
    CommandRefreshPanel = 2,
  };

  explicit SingleInstance(const QString & ServerName, QObject * Parent = nullptr);
  ~SingleInstance() override;

  // Socket path for the configuration file (IniFileName; "nul" for none):
  // $XDG_RUNTIME_DIR/nixscp-<uid>-<hash of the configuration path>
  // (in the temporary directory when that path exceeds the 107 bytes of a
  // Unix socket address)
  static QString DefaultServerName(const QString & IniFileName);

  // Starts listening. A stale socket of a crashed instance is replaced.
  // False when another instance already listens (or the socket cannot be created).
  bool Listen();
  bool Listening() const;
  QString ServerName() const { return FServerName; }

  // CanCommandLineFromAnotherInstance: asked before a command line is
  // accepted (default: always)
  void SetCanAcceptCommandLine(std::function<bool()> Func) { FCanAccept = std::move(Func); }

  // Client side: sends the command line to the instance listening on
  // ServerName. True when it was accepted (the caller then exits).
  static bool SendCommandLine(const QString & ServerName, const QStringList & Arguments,
    const QString & WorkingDirectory, int TimeoutMs = 5000);
  // /refresh [session] [path]: asks the running instance to refresh panels.
  // True when an instance received it.
  static bool SendRefresh(const QString & ServerName, const QString & Session, const QString & Path,
    int TimeoutMs = 5000);

signals:
  // Emitted (queued, after the reply was sent) for an accepted command line.
  // ActivationToken: XDG_ACTIVATION_TOKEN / DESKTOP_STARTUP_ID of the sender,
  // to let the window be raised under Wayland / X11 startup notification.
  void CommandLineReceived(const QStringList & Arguments, const QString & WorkingDirectory,
    const QString & ActivationToken);
  void RefreshRequested(const QString & Session, const QString & Path);

private:
  QString FServerName;
  QLocalServer * FServer = nullptr;
  std::function<bool()> FCanAccept;

  void NewConnection();
  void ReadRequest(QLocalSocket * Socket);
  static bool Send(const QString & ServerName, int Command, const QStringList & Arguments,
    const QString & WorkingDirectory, int TimeoutMs);
};
