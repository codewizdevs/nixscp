// "Check connection" of a site: is the server there? Resolves the name,
// connects to the port (timed) and reads the server's greeting (SSH and FTP
// send one), so "server down", "wrong address" and "wrong password" are told
// apart before connecting.
#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QTcpSocket>

class SiteChecker : public QObject
{
  Q_OBJECT
  Q_PROPERTY(bool checking READ checking NOTIFY checkingChanged)

public:
  explicit SiteChecker(QObject * Parent = nullptr) : QObject(Parent) {}
  bool checking() const { return FSocket != nullptr; }

  Q_INVOKABLE void check(const QString & Site);
  Q_INVOKABLE void stop();

signals:
  void checkingChanged();
  // ok: the port answered; report: the steps, one per line
  void finished(const QString & site, bool ok, const QString & report);

private:
  QPointer<QTcpSocket> FSocket;
  QElapsedTimer FTimer;
  QString FSite;
  QStringList FReport;
  bool FConnected = false;

  void Finish(bool Ok);
};
