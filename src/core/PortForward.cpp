#include "core/PortForward.h"
#include "core/FileSystem.h"
#include "core/SecureShell.h"
#include "core/Sockets.h"

#include <QStringList>

#include <libssh/libssh.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

#ifndef Q_OS_WIN
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

//---------------------------------------------------------------------------
// PortForwardSpec

bool PortForwardSpec::IsValid() const
{
  return (LocalPort > 0) && (LocalPort < 65536) && !Host.trimmed().isEmpty() && (Port > 0) && (Port < 65536);
}

QString PortForwardSpec::Text() const
{
  return QStringLiteral("%1 -> %2:%3").arg(LocalPort).arg(Host).arg(Port);
}

QString PortForwardSpec::Save() const
{
  QString SafeName = Name;
  SafeName.remove(QLatin1Char('|')).remove(QLatin1Char('\n'));
  return QStringLiteral("%1|%2|%3|%4").arg(SafeName).arg(LocalPort).arg(Host.trimmed()).arg(Port);
}

PortForwardSpec PortForwardSpec::Load(const QString & Line)
{
  PortForwardSpec Spec;
  QStringList Parts = Line.split(QLatin1Char('|'));
  if (Parts.size() == 4)
  {
    Spec.Name = Parts.at(0).trimmed();
    Spec.LocalPort = Parts.at(1).toInt();
    Spec.Host = Parts.at(2).trimmed();
    Spec.Port = Parts.at(3).toInt();
  }
  return Spec;
}

bool PortForwardSpec::Parse(const QString & Text, PortForwardSpec & Spec, QString * Error)
{
  // <local port>:<host>:<port>; an IPv6 host in brackets
  QString T = Text.trimmed();
  qsizetype First = T.indexOf(QLatin1Char(':'));
  qsizetype Last = T.lastIndexOf(QLatin1Char(':'));
  if ((First < 0) || (Last == First))
  {
    if (Error != nullptr) *Error = QStringLiteral("Use <local port>:<host>:<port>, e.g. 5433:localhost:5432.");
    return false;
  }
  Spec.LocalPort = T.left(First).toInt();
  Spec.Host = T.mid(First + 1, Last - First - 1);
  if (Spec.Host.startsWith(QLatin1Char('[')) && Spec.Host.endsWith(QLatin1Char(']')))
  {
    Spec.Host = Spec.Host.mid(1, Spec.Host.size() - 2);
  }
  Spec.Port = T.mid(Last + 1).toInt();
  if (!Spec.IsValid())
  {
    if (Error != nullptr) *Error = QStringLiteral("Invalid port forward \"%1\". Use <local port>:<host>:<port>, e.g. 5433:localhost:5432.").arg(Text);
    return false;
  }
  return true;
}

QList<PortForwardSpec> PortForwardSpec::LoadAll(const QString & Lines)
{
  QList<PortForwardSpec> Result;
  for (const QString & Line : Lines.split(QLatin1Char('\n'), Qt::SkipEmptyParts))
  {
    PortForwardSpec Spec = Load(Line);
    if (Spec.IsValid())
    {
      Result.append(Spec);
    }
  }
  return Result;
}

QString PortForwardSpec::SaveAll(const QList<PortForwardSpec> & Specs)
{
  QStringList Lines;
  for (const PortForwardSpec & Spec : Specs)
  {
    Lines.append(Spec.Save());
  }
  return Lines.join(QLatin1Char('\n'));
}

//---------------------------------------------------------------------------
// PortForward

PortForward::PortForward(const SessionData & Data, const PortForwardSpec & Spec, UserInterface * UI, QObject * Parent) :
  QObject(Parent), FData(Data), FSpec(Spec), FUI(UI)
{
}

PortForward::~PortForward()
{
  Stop();
  if (FThread != nullptr)
  {
    FThread->wait();
    delete FThread;
  }
}

void PortForward::Start()
{
  if (FThread != nullptr)
  {
    return;
  }
  FStop = false;
  FThread = QThread::create([this]() { Run(); });
  FThread->setObjectName(QStringLiteral("Tunnel %1").arg(FSpec.LocalPort));
  FThread->start();
}

void PortForward::Stop()
{
  FStop = true;
}

namespace
{

struct Pipe
{
  Net::Socket Local = Net::Invalid;
  ssh_channel Channel = nullptr;
  bool LocalEof = false;
};

void ClosePipe(Pipe & P)
{
  if (P.Channel != nullptr)
  {
    ssh_channel_close(P.Channel);
    ssh_channel_free(P.Channel);
    P.Channel = nullptr;
  }
  if (P.Local != Net::Invalid)
  {
    Net::Close(P.Local);
    P.Local = Net::Invalid;
  }
}

}

void PortForward::Run()
{
  QString Error;
  Net::Socket Listener = Net::Invalid;
  std::vector<Pipe> Pipes;
  try
  {
    SecureShell Shell(FData, FUI);
    Shell.Open();
    ssh_session Session = Shell.Session();

    // only this computer can use the tunnel (127.0.0.1)
    Net::Startup();
    Listener = Net::Open(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (Listener == Net::Invalid)
    {
      throw ExtException(QStringLiteral("Cannot create a socket: %1").arg(Net::ErrorText(Net::LastError())));
    }
#ifndef Q_OS_WIN
    int Reuse = 1;
    setsockopt(Listener, SOL_SOCKET, SO_REUSEADDR, &Reuse, sizeof(Reuse));
#endif
    sockaddr_in Address{};
    Address.sin_family = AF_INET;
    Address.sin_port = htons(static_cast<quint16>(FSpec.LocalPort));
    Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if ((::bind(Listener, reinterpret_cast<sockaddr *>(&Address), sizeof(Address)) != 0) || (::listen(Listener, 16) != 0))
    {
      throw ExtException(QStringLiteral("Cannot listen on 127.0.0.1:%1: %2").arg(FSpec.LocalPort).arg(Net::ErrorText(Net::LastError())));
    }
    FRunning = true;
    emit Started(true, QString());

    std::vector<char> Buffer(32 * 1024);
    while (!FStop)
    {
      std::vector<Net::PollFd> Fds(1 + Pipes.size());
      Fds[0].fd = Listener;
      Fds[0].events = POLLIN;
      for (size_t I = 0; I < Pipes.size(); I++)
      {
        Fds[1 + I].fd = Pipes[I].Local;
        Fds[1 + I].events = Pipes[I].LocalEof ? 0 : POLLIN;
      }
      Net::Poll(Fds.data(), static_cast<unsigned long>(Fds.size()), 20);
      if (Fds[0].revents & POLLIN)
      {
        sockaddr_in From{};
        socklen_t FromLength = sizeof(From);
        Net::Socket Client = ::accept(Listener, reinterpret_cast<sockaddr *>(&From), &FromLength);
        if (Client != Net::Invalid)
        {
          Pipe P;
          P.Local = Client;
          P.Channel = ssh_channel_new(Session);
          if ((P.Channel == nullptr) ||
              (ssh_channel_open_forward(P.Channel, FSpec.Host.toUtf8().constData(), FSpec.Port, "127.0.0.1", ntohs(From.sin_port)) != SSH_OK))
          {
            emit ConnectionFailed(QStringLiteral("The server refused to forward the connection to %1:%2: %3")
              .arg(FSpec.Host).arg(FSpec.Port).arg(QString::fromUtf8(ssh_get_error(Session))));
            ClosePipe(P);
          }
          else
          {
            Pipes.push_back(P);
            FConnections = static_cast<int>(Pipes.size());
            emit ConnectionsChanged(FConnections);
          }
        }
      }
      bool Changed = false;
      for (size_t I = 0; I < Pipes.size(); I++)
      {
        Pipe & P = Pipes[I];
        bool Closed = false;
        // local -> server (only when the socket has data: the read does not block)
        if (!P.LocalEof && (Fds[1 + I].revents & (POLLIN | POLLHUP | POLLERR)))
        {
          qint64 N = Net::Receive(P.Local, Buffer.data(), static_cast<qint64>(Buffer.size()), 0);
          if (N > 0)
          {
            if (ssh_channel_write(P.Channel, Buffer.data(), static_cast<uint32_t>(N)) == SSH_ERROR)
            {
              Closed = true;
            }
            FBytes += N;
          }
          else
          {
            P.LocalEof = true;
            ssh_channel_send_eof(P.Channel);
          }
        }
        // server -> local
        for (int Stream = 0; !Closed && (Stream <= 1); Stream++)
        {
          int N = ssh_channel_read_nonblocking(P.Channel, Buffer.data(), static_cast<uint32_t>(Buffer.size()), Stream);
          if (N == SSH_ERROR)
          {
            Closed = true;
          }
          else if ((N > 0) && (Net::Send(P.Local, Buffer.data(), N, Net::NoSignal) != N))
          {
            Closed = true;
          }
          else if (N > 0)
          {
            FBytes += N;
          }
        }
        if (!Closed && (ssh_channel_is_eof(P.Channel) || ssh_channel_is_closed(P.Channel)))
        {
          // the server finished: deliver what is still buffered, then close
          int N = 0;
          while ((N = ssh_channel_read_nonblocking(P.Channel, Buffer.data(), static_cast<uint32_t>(Buffer.size()), 0)) > 0)
          {
            if (Net::Send(P.Local, Buffer.data(), N, Net::NoSignal) != N)
            {
              break;
            }
            FBytes += N;
          }
          Closed = true;
        }
        if (Closed)
        {
          ClosePipe(P);
          Changed = true;
        }
      }
      if (Changed)
      {
        Pipes.erase(std::remove_if(Pipes.begin(), Pipes.end(), [](const Pipe & P) { return P.Channel == nullptr; }), Pipes.end());
        FConnections = static_cast<int>(Pipes.size());
        emit ConnectionsChanged(FConnections);
      }
      if (!ssh_is_connected(Session))
      {
        throw ExtException(QStringLiteral("The connection to the server was lost."));
      }
    }
    for (Pipe & P : Pipes)
    {
      ClosePipe(P);
    }
    Pipes.clear();
    Shell.Close();
  }
  catch (const ExtException & E)
  {
    Error = E.Message();
    if (!E.MoreMessages().trimmed().isEmpty())
    {
      Error += QLatin1Char(' ') + E.MoreMessages().trimmed();
    }
    for (Pipe & P : Pipes)
    {
      ClosePipe(P);
    }
  }
  if (Listener != Net::Invalid)
  {
    Net::Close(Listener);
  }
  bool WasRunning = FRunning;
  FRunning = false;
  FConnections = 0;
  if (!WasRunning)
  {
    emit Started(false, Error);
  }
  emit Stopped(Error);
}
