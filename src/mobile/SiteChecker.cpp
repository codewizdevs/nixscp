#include "mobile/SiteChecker.h"

#include "core/Configuration.h"
#include "core/SessionData.h"

#include <QTimer>

void SiteChecker::check(const QString & Site)
{
  stop();
  std::shared_ptr<SessionData> Data = Configuration::Instance().StoredSessions().FindByName(Site);
  if (Data == nullptr)
  {
    emit finished(Site, false, QStringLiteral("The site no longer exists."));
    return;
  }
  FSite = Site;
  FReport.clear();
  FConnected = false;
  QString Host = Data->HostNameExpanded();
  FPort = static_cast<quint16>(Data->PortNumber);
  FReport.append(QStringLiteral("%1, port %2 (%3)").arg(Host).arg(FPort).arg(Data->ProtocolStr()));
  FTimer.start();
  // in the background: a slow name server must not freeze the app
  FLookup = QHostInfo::lookupHost(Host, this, [this](const QHostInfo & Info) { Connect(Info); });
  emit checkingChanged();
}

void SiteChecker::Connect(const QHostInfo & Info)
{
  FLookup = -1;
  if (Info.error() != QHostInfo::NoError)
  {
    FReport.append(QStringLiteral("✗ The name does not resolve: %1").arg(Info.errorString()));
    emit checkingChanged();
    emit finished(FSite, false, FReport.join(QLatin1Char('\n')));
    return;
  }
  QString Address = Info.addresses().value(0).toString();
  FReport.append(QStringLiteral("✓ Address %1 (%2 ms)").arg(Address).arg(FTimer.elapsed()));

  auto * Socket = new QTcpSocket(this);
  FSocket = Socket;
  emit checkingChanged();
  FTimer.restart();
  connect(Socket, &QTcpSocket::connected, this, [this, Socket]()
  {
    FConnected = true;
    FReport.append(QStringLiteral("✓ Port open, connected in %1 ms").arg(FTimer.elapsed()));
    // the greeting: SSH and FTP servers speak first
    QTimer::singleShot(2000, Socket, [this]() { Finish(true); });
  });
  connect(Socket, &QTcpSocket::readyRead, this, [this, Socket]()
  {
    QString Line = QString::fromUtf8(Socket->readLine(256)).trimmed();
    if (!Line.isEmpty())
    {
      FReport.append(QStringLiteral("✓ Server says: %1").arg(Line));
    }
    Finish(true);
  });
  connect(Socket, &QTcpSocket::errorOccurred, this, [this, Socket](QAbstractSocket::SocketError)
  {
    if (FConnected)
    {
      Finish(true);
      return;
    }
    FReport.append(QStringLiteral("✗ %1 (after %2 ms)").arg(Socket->errorString()).arg(FTimer.elapsed()));
    Finish(false);
  });
  // no answer at all (a firewall dropping packets)
  QTimer::singleShot(10000, Socket, [this]()
  {
    if (!FConnected)
    {
      FReport.append(QStringLiteral("✗ No answer in 10 seconds (a firewall, or the server is down)"));
      Finish(false);
    }
  });
  Socket->connectToHost(Info.addresses().value(0), FPort);
}

void SiteChecker::Finish(bool Ok)
{
  if (FSocket == nullptr)
  {
    return;
  }
  QTcpSocket * Socket = FSocket;
  FSocket = nullptr;
  Socket->disconnect(this);
  Socket->abort();
  Socket->deleteLater();
  emit checkingChanged();
  emit finished(FSite, Ok, FReport.join(QLatin1Char('\n')));
}

void SiteChecker::stop()
{
  if (FLookup >= 0)
  {
    QHostInfo::abortHostLookup(FLookup);
    FLookup = -1;
    emit checkingChanged();
  }
  if (FSocket != nullptr)
  {
    QTcpSocket * Socket = FSocket;
    FSocket = nullptr;
    Socket->disconnect(this);
    Socket->abort();
    Socket->deleteLater();
    emit checkingChanged();
  }
}
