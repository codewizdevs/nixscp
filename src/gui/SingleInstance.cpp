// Port of the inter-instance part of WinSCP source/windows/WinMain.cpp and
// forms/CustomScpExplorer.cpp (WMCopyData); see SingleInstance.h
#include "gui/SingleInstance.h"
#include "core/Platform.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QPointer>
#include <QStandardPaths>
#include <QTimer>

#include <unistd.h>

static const QString Magic = QStringLiteral("NixSCP-Instance");
static const qint32 ProtocolVersion = 1;

SingleInstance::SingleInstance(const QString & ServerName, QObject * Parent) :
  QObject(Parent),
  FServerName(ServerName)
{
}

SingleInstance::~SingleInstance()
{
  if (FServer != nullptr)
  {
    FServer->close();
  }
}

QString SingleInstance::DefaultServerName(const QString & IniFileName)
{
  QString Path = IniFileName;
  if (Path.compare(QStringLiteral("nul"), Qt::CaseInsensitive) != 0)
  {
    QFileInfo Info(IniFileName);
    QString Canonical = Info.canonicalFilePath();
    Path = Canonical.isEmpty() ? Info.absoluteFilePath() : Canonical;
  }
  QByteArray Hash = QCryptographicHash::hash(Path.toUtf8(), QCryptographicHash::Sha256).toHex().left(16);
  QString Name = QStringLiteral("nixscp-%1-%2").arg(Platform::UserKey(), QString::fromLatin1(Hash));
  // Qt creates the runtime directory (0700) when $XDG_RUNTIME_DIR is not set
  QString Dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
  QString Result = Dir.isEmpty() ? Name : QDir(Dir).filePath(Name);
  // sockaddr_un holds at most 107 bytes: with a long runtime directory
  // QLocalServer/QLocalSocket fail silently (every instance would run on
  // its own); the name alone goes to the temporary directory (0600 socket)
  if (QFile::encodeName(Result).size() > 100)
  {
    Result = QDir(QDir::tempPath()).filePath(Name);
    if (QFile::encodeName(Result).size() > 100)
    {
      Result = QStringLiteral("/tmp/") + Name;
    }
  }
  return Result;
}

bool SingleInstance::Listening() const
{
  return (FServer != nullptr) && FServer->isListening();
}

bool SingleInstance::Listen()
{
  if (Listening())
  {
    return true;
  }
  if (FServer == nullptr)
  {
    FServer = new QLocalServer(this);
    FServer->setSocketOptions(QLocalServer::UserAccessOption);
    connect(FServer, &QLocalServer::newConnection, this, &SingleInstance::NewConnection);
  }
  // Two instances started at once (e.g. several sftp:// links opened
  // together) must not both find the name free and replace each other's
  // socket: the check and the listen() are serialized by a lock file
  // (WinSCP: the instance mutex)
  QLockFile Lock(FServerName + QStringLiteral(".lock"));
  Lock.setStaleLockTime(10000);
  bool Locked = Lock.tryLock(5000);
  Q_UNUSED(Locked); // without the lock (unwritable directory) proceed as before
  // Is somebody listening already? (With UserAccessOption Qt binds in a
  // private directory and renames the socket into place, replacing any file,
  // so the check cannot be left to listen().) A socket left by a crashed
  // instance does not answer and is replaced.
  {
    QLocalSocket Probe;
    Probe.connectToServer(FServerName);
    if (Probe.waitForConnected(1000))
    {
      Probe.abort();
      return false;
    }
  }
  QLocalServer::removeServer(FServerName);
  return FServer->listen(FServerName);
}

void SingleInstance::NewConnection()
{
  while (QLocalSocket * Socket = FServer->nextPendingConnection())
  {
    connect(Socket, &QLocalSocket::disconnected, Socket, &QObject::deleteLater);
    connect(Socket, &QLocalSocket::readyRead, this, [this, Socket]() { ReadRequest(Socket); });
    // A client that never completes its request is dropped
    QPointer<QLocalSocket> Guard(Socket);
    QTimer::singleShot(10000, this, [Guard]()
    {
      if (Guard)
      {
        Guard->abort();
        Guard->deleteLater();
      }
    });
    if (Socket->bytesAvailable() > 0)
    {
      ReadRequest(Socket);
    }
  }
}

void SingleInstance::ReadRequest(QLocalSocket * Socket)
{
  QDataStream Stream(Socket);
  Stream.setVersion(QDataStream::Qt_6_0);
  Stream.startTransaction();
  QString AMagic;
  qint32 Version = 0;
  qint32 ACommand = 0;
  QStringList Arguments;
  QString WorkingDirectory;
  QString ActivationToken;
  Stream >> AMagic >> Version >> ACommand >> Arguments >> WorkingDirectory >> ActivationToken;
  if (!Stream.commitTransaction())
  {
    // incomplete; wait for more data (or a corrupt request, dropped by the timeout)
    if ((Stream.status() == QDataStream::ReadCorruptData) || (Socket->bytesAvailable() > 1024 * 1024))
    {
      Socket->abort();
    }
    return;
  }
  disconnect(Socket, &QLocalSocket::readyRead, this, nullptr);

  bool Result = (AMagic == Magic) && (Version == ProtocolVersion);
  if (Result)
  {
    switch (ACommand)
    {
      case CommandCommandLine:
        Result = !Arguments.isEmpty() && (!FCanAccept || FCanAccept());
        break;

      case CommandRefreshPanel:
        Result = true;
        break;

      default:
        Result = false;
        break;
    }
  }

  QByteArray Reply;
  {
    QDataStream ReplyStream(&Reply, QIODevice::WriteOnly);
    ReplyStream.setVersion(QDataStream::Qt_6_0);
    ReplyStream << static_cast<qint32>(Result ? 1 : 0);
  }
  Socket->write(Reply);
  Socket->flush();
  Socket->disconnectFromServer();

  if (Result)
  {
    // Processed only after replying: the command line may show dialogs
    // (Login dialog...), the sender must not wait for them
    if (ACommand == CommandCommandLine)
    {
      QTimer::singleShot(0, this, [this, Arguments, WorkingDirectory, ActivationToken]()
      {
        emit CommandLineReceived(Arguments, WorkingDirectory, ActivationToken);
      });
    }
    else
    {
      QString Session = Arguments.value(0);
      QString Path = Arguments.value(1);
      QTimer::singleShot(0, this, [this, Session, Path]() { emit RefreshRequested(Session, Path); });
    }
  }
}

bool SingleInstance::Send(const QString & ServerName, int Command, const QStringList & Arguments,
  const QString & WorkingDirectory, int TimeoutMs)
{
  QLocalSocket Socket;
  Socket.connectToServer(ServerName);
  if (!Socket.waitForConnected(TimeoutMs))
  {
    return false;
  }
  QString ActivationToken = qEnvironmentVariable("XDG_ACTIVATION_TOKEN");
  if (ActivationToken.isEmpty())
  {
    ActivationToken = qEnvironmentVariable("DESKTOP_STARTUP_ID");
  }
  QByteArray Request;
  {
    QDataStream Stream(&Request, QIODevice::WriteOnly);
    Stream.setVersion(QDataStream::Qt_6_0);
    Stream << Magic << ProtocolVersion << static_cast<qint32>(Command) << Arguments << WorkingDirectory << ActivationToken;
  }
  Socket.write(Request);
  if (!Socket.waitForBytesWritten(TimeoutMs))
  {
    return false;
  }
  QDataStream Stream(&Socket);
  Stream.setVersion(QDataStream::Qt_6_0);
  qint32 Result = 0;
  while (true)
  {
    Stream.startTransaction();
    Stream >> Result;
    if (Stream.commitTransaction())
    {
      break;
    }
    if (!Socket.waitForReadyRead(TimeoutMs))
    {
      return false;
    }
  }
  return (Result == 1);
}

bool SingleInstance::SendCommandLine(const QString & ServerName, const QStringList & Arguments,
  const QString & WorkingDirectory, int TimeoutMs)
{
  return Send(ServerName, CommandCommandLine, Arguments, WorkingDirectory, TimeoutMs);
}

bool SingleInstance::SendRefresh(const QString & ServerName, const QString & Session, const QString & Path, int TimeoutMs)
{
  return Send(ServerName, CommandRefreshPanel, QStringList{Session, Path}, QString(), TimeoutMs);
}
