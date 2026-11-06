#include "core/PortForward.h"
#include "core/FileSystem.h"
#include "core/SecureShell.h"
#include "core/Sockets.h"

#include <QElapsedTimer>
#include <QHostAddress>
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

namespace
{

// A host a forward can reach: no spaces or control characters, nothing of the
// "[h" kind a mistyped spec leaves, and a colon only in a real IPv6 address
bool ValidHost(const QString & Host)
{
  // (the server gets its UTF-8, which loses a lone surrogate without a word)
  if (Host.isEmpty() || (Host.size() > 255) || !Host.isValidUtf16())
  {
    return false;
  }
  for (QChar C : Host)
  {
    if (C.isSpace() || (C.unicode() < 0x20) || (C.unicode() == 0x7F) || QStringLiteral("[]|/\\\"'`<>").contains(C))
    {
      return false;
    }
  }
  return !Host.contains(QLatin1Char(':')) || QHostAddress().setAddress(Host);
}

// 1..65535 in digits only (0 otherwise: QString::toInt takes spaces and signs)
int ParsePort(const QString & Text)
{
  if (Text.isEmpty() || (Text.size() > 5) ||
      !std::all_of(Text.begin(), Text.end(), [](QChar C) { return (C >= QLatin1Char('0')) && (C <= QLatin1Char('9')); }))
  {
    return 0;
  }
  return Text.toInt();
}

// The fields of a saved forward are separated by | and the forwards by new lines
QString EscapeField(QString Text)
{
  return Text.replace(QLatin1Char('%'), QStringLiteral("%25")).replace(QLatin1Char('|'), QStringLiteral("%7C"))
    .replace(QLatin1Char('\n'), QStringLiteral("%0A")).replace(QLatin1Char('\r'), QStringLiteral("%0D"));
}

QString UnescapeField(QString Text)
{
  return Text.replace(QStringLiteral("%7C"), QStringLiteral("|")).replace(QStringLiteral("%0A"), QStringLiteral("\n"))
    .replace(QStringLiteral("%0D"), QStringLiteral("\r")).replace(QStringLiteral("%25"), QStringLiteral("%"));
}

}

bool PortForwardSpec::IsValid() const
{
  return (LocalPort > 0) && (LocalPort < 65536) && ValidHost(Host) && (Port > 0) && (Port < 65536);
}

QString PortForwardSpec::Text() const
{
  return QStringLiteral("%1 -> %2:%3").arg(QString::number(LocalPort), Host, QString::number(Port));
}

QString PortForwardSpec::Save() const
{
  return QStringList{EscapeField(Name), QString::number(LocalPort), EscapeField(Host.trimmed()), QString::number(Port)}
    .join(QLatin1Char('|'));
}

PortForwardSpec PortForwardSpec::Load(const QString & Line)
{
  PortForwardSpec Spec;
  QStringList Parts = Line.split(QLatin1Char('|'));
  if (Parts.size() == 4)
  {
    Spec.Name = UnescapeField(Parts.at(0)).trimmed();
    Spec.LocalPort = Parts.at(1).toInt();
    Spec.Host = UnescapeField(Parts.at(2)).trimmed();
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
  PortForwardSpec Parsed;
  Parsed.Name = Spec.Name;
  Parsed.LocalPort = ParsePort(T.left(First));
  Parsed.Host = T.mid(First + 1, Last - First - 1);
  if (Parsed.Host.startsWith(QLatin1Char('[')) && Parsed.Host.endsWith(QLatin1Char(']')))
  {
    Parsed.Host = Parsed.Host.mid(1, Parsed.Host.size() - 2);
  }
  Parsed.Port = ParsePort(T.mid(Last + 1));
  if (!Parsed.IsValid())
  {
    if (Error != nullptr) *Error = QStringLiteral("Invalid port forward \"%1\". Use <local port>:<host>:<port>, e.g. 5433:localhost:5432.").arg(Text);
    return false;
  }
  Spec = Parsed;
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

constexpr int BufferSize = 32 * 1024;
// Local connections of one tunnel; more wait in the listen queue
constexpr size_t MaxPipes = 256;

// One local connection and its channel. What cannot be written yet waits here,
// and nothing more is read from that side meanwhile: a slow peer stalls its own
// connection (the SSH window and the local socket buffer push back), never the
// tunnel, and all sockets stay non-blocking so Stop always works.
struct Pipe
{
  Net::Socket Local = Net::Invalid;
  ssh_channel Channel = nullptr;
  int FromPort = 0;
  bool Opening = true;        // the server has not answered the channel open yet
  QElapsedTimer OpenTimer;
  QByteArray ToServer;
  QByteArray ToLocal;
  bool LocalEof = false;      // the local client finished sending
  bool EofSent = false;       // ... and the server was told
  bool ServerEof = false;     // the server finished sending, and the client was told
};

void ClosePipe(Pipe & P)
{
  if (P.Channel != nullptr)
  {
    // a channel the server refused has no number on its side: closing it would
    // name channel 0, a protocol error that makes OpenSSH drop the connection
    if (ssh_channel_is_open(P.Channel))
    {
      ssh_channel_close(P.Channel);
    }
    ssh_channel_free(P.Channel);
    P.Channel = nullptr;
  }
  if (P.Local != Net::Invalid)
  {
    Net::Close(P.Local);
    P.Local = Net::Invalid;
  }
}

// The channels belong to the session and are freed with it, so this list must
// go before the SecureShell does, also when an exception unwinds the stack:
// declared after it, its destructor runs first
struct PipeList
{
  std::vector<Pipe> Items;
  // the keepalive in progress
  ssh_channel Probe = nullptr;

  ~PipeList()
  {
    CloseAll();
  }
  void CloseProbe()
  {
    if (Probe != nullptr)
    {
      if (ssh_channel_is_open(Probe))
      {
        ssh_channel_close(Probe);
      }
      ssh_channel_free(Probe);
      Probe = nullptr;
    }
  }
  void CloseAll()
  {
    for (Pipe & P : Items)
    {
      ClosePipe(P);
    }
    Items.clear();
    CloseProbe();
  }
};

// Reads the packets of the session, also while no channel is open (nothing
// else would notice that the server went away). Same lifetime rule as PipeList.
class SessionPump
{
public:
  explicit SessionPump(ssh_session Session) : FEvent(ssh_event_new())
  {
    if ((FEvent == nullptr) || (ssh_event_add_session(FEvent, Session) != SSH_OK))
    {
      if (FEvent != nullptr)
      {
        ssh_event_free(FEvent);
      }
      throw ExtException(QStringLiteral("Cannot watch the SSH connection."));
    }
  }
  ~SessionPump()
  {
    // gives the session back to its own poll context
    ssh_event_free(FEvent);
  }
  SessionPump(const SessionPump &) = delete;
  SessionPump & operator=(const SessionPump &) = delete;
  void Run()
  {
    ssh_event_dopoll(FEvent, 0);
  }

private:
  ssh_event FEvent;
};

bool SocketFailed(qint64 Result)
{
  return (Result < 0) && !Net::WouldBlock(Net::LastError()) && !Net::Interrupted(Net::LastError());
}

// Moves what it can in both directions, without blocking; false when the
// connection is finished
bool PumpPipe(Pipe & P, short Events, char * Buffer, std::atomic<qint64> & Bytes)
{
  if (Events & (POLLERR | POLLNVAL))
  {
    return false;
  }
  // local -> server
  if (!P.LocalEof && P.ToServer.isEmpty() && (Events & POLLIN))
  {
    qint64 N = Net::Receive(P.Local, Buffer, BufferSize, 0);
    if (N > 0)
    {
      P.ToServer = QByteArray(Buffer, static_cast<qsizetype>(N));
    }
    else if (N == 0)
    {
      P.LocalEof = true;
    }
    else if (SocketFailed(N))
    {
      return false;
    }
  }
  else if (!P.LocalEof && (Events & POLLHUP) && !(Events & POLLIN))
  {
    P.LocalEof = true;
  }
  if (!P.ToServer.isEmpty())
  {
    // never more than the window: the rest of a write would wait inside libssh
    uint32_t Room = qMin<uint32_t>(ssh_channel_window_size(P.Channel), static_cast<uint32_t>(P.ToServer.size()));
    if (Room > 0)
    {
      int N = ssh_channel_write(P.Channel, P.ToServer.constData(), Room);
      if (N == SSH_ERROR)
      {
        return false;
      }
      P.ToServer.remove(0, N);
      Bytes += N;
    }
  }
  if (P.LocalEof && !P.EofSent && P.ToServer.isEmpty())
  {
    // half-close: the server may still answer
    ssh_channel_send_eof(P.Channel);
    P.EofSent = true;
  }

  // server -> local
  if (P.ToLocal.isEmpty() && !P.ServerEof)
  {
    for (int Stream = 0; Stream <= 1; Stream++)
    {
      int N = ssh_channel_read_nonblocking(P.Channel, Buffer, BufferSize, Stream);
      if (N == SSH_ERROR)
      {
        return false;
      }
      if (N > 0)
      {
        P.ToLocal = QByteArray(Buffer, N);
        break;
      }
    }
  }
  if (!P.ToLocal.isEmpty())
  {
    qint64 N = Net::Send(P.Local, P.ToLocal.constData(), P.ToLocal.size(), Net::NoSignal);
    if (N > 0)
    {
      P.ToLocal.remove(0, static_cast<qsizetype>(N));
      Bytes += N;
    }
    else if (SocketFailed(N))
    {
      return false;
    }
  }
  if (P.ToLocal.isEmpty() && !P.ServerEof && (ssh_channel_poll(P.Channel, 0) <= 0) && (ssh_channel_poll(P.Channel, 1) <= 0) &&
      (ssh_channel_is_eof(P.Channel) || ssh_channel_is_closed(P.Channel)))
  {
    // everything from the server was delivered: the client may still send
    ::shutdown(P.Local, Net::ShutWrite);
    P.ServerEof = true;
  }
  // over when both sides finished, or the channel is gone and there is no way to send
  return !P.ServerEof || (!ssh_channel_is_closed(P.Channel) && !P.EofSent);
}

}

void PortForward::Run()
{
  QString Error;
  Net::Socket Listener = Net::Invalid;
  // the session counts into it until its very end
  ssh_counter_struct Traffic{};
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
#ifdef Q_OS_WIN
    // or a program of another user could bind the same port and get the connections
    int Exclusive = 1;
    setsockopt(Listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&Exclusive), sizeof(Exclusive));
#else
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
    Net::SetBlocking(Listener, false);

    // from here on nothing waits for the server: one thread serves all the
    // connections, and a channel that takes long to open delays only itself
    ssh_set_counters(Session, &Traffic, nullptr);
    ssh_set_blocking(Session, 0);
    // libssh leaves the socket itself blocking: a send that is larger than what
    // the system still takes would stop every connection here until the server
    // reads, and a server that is sending just then may be waiting for us to read
    Net::SetBlocking(ssh_get_fd(Session), false);
    SessionPump Pump(Session);
    PipeList Pipes;
    FRunning = true;
    emit Started(true, QString());

    // Keepalives: this thread has no event loop for the ping timer of
    // SecureShell, and an idle tunnel would never notice a dead link. The
    // SSH_MSG_IGNORE of SecureShell gets no answer, and libssh has the
    // keepalive request only in its server part, which builds can leave out.
    // Opening a session channel works everywhere: the server has to answer,
    // yes or no, and silence means it is gone. Nothing runs in the channel.
    const qint64 PingEvery = ((FData.PingType != ptOff) && (FData.PingInterval > 0)) ? FData.PingInterval * 1000LL : 30000;
    const qint64 ReplyTimeout = qMax(FData.Timeout, 1) * 1000LL;
    QElapsedTimer Silence;
    Silence.start();
    uint64_t LastIn = 0;
    bool PingSent = false;

    std::vector<char> Buffer(BufferSize);
    std::vector<Net::PollFd> Fds;
    const QByteArray Host = FSpec.Host.toUtf8();
    while (!FStop)
    {
      Fds.assign(2 + Pipes.Items.size(), Net::PollFd{});
      Fds[0].fd = Listener;
      Fds[0].events = (Pipes.Items.size() < MaxPipes) ? POLLIN : 0;
      Fds[1].fd = ssh_get_fd(Session);
      Fds[1].events = POLLIN;
      for (size_t I = 0; I < Pipes.Items.size(); I++)
      {
        const Pipe & P = Pipes.Items[I];
        short Wanted = 0;
        if (!P.Opening && !P.LocalEof && P.ToServer.isEmpty())
        {
          Wanted |= POLLIN;
        }
        if (!P.ToLocal.isEmpty())
        {
          Wanted |= POLLOUT;
        }
        // a socket with nothing to wait for stays out: its hang-up would be
        // reported at once on every round
        Fds[2 + I].fd = (Wanted != 0) ? P.Local : Net::Invalid;
        Fds[2 + I].events = Wanted;
      }
      Net::Poll(Fds.data(), static_cast<unsigned long>(Fds.size()), 20);
      Pump.Run();
      bool Changed = false;
      for (size_t I = 0; I < Pipes.Items.size(); I++)
      {
        Pipe & P = Pipes.Items[I];
        bool Alive = true;
        if (P.Opening)
        {
          int Rc = ssh_channel_open_forward(P.Channel, Host.constData(), FSpec.Port, "127.0.0.1", P.FromPort);
          if ((Rc == SSH_ERROR) || ((Rc == SSH_AGAIN) && P.OpenTimer.hasExpired(ReplyTimeout)))
          {
            emit ConnectionFailed(QStringLiteral("The server refused to forward the connection to %1:%2: %3")
              .arg(FSpec.Host, QString::number(FSpec.Port),
                (Rc == SSH_AGAIN) ? QStringLiteral("no answer.") : QString::fromUtf8(ssh_get_error(Session))));
            Alive = false;
          }
          else if (Rc == SSH_OK)
          {
            P.Opening = false;
          }
        }
        else
        {
          Alive = PumpPipe(P, Fds[2 + I].revents, Buffer.data(), FBytes);
        }
        if (!Alive)
        {
          ClosePipe(P);
          Changed = true;
        }
      }
      if (Changed)
      {
        std::erase_if(Pipes.Items, [](const Pipe & P) { return P.Channel == nullptr; });
      }
      // after the pipes: Fds covers only the ones it was built for
      if (Fds[0].revents & POLLIN)
      {
        sockaddr_in From{};
        socklen_t FromLength = sizeof(From);
        Net::Socket Client = ::accept(Listener, reinterpret_cast<sockaddr *>(&From), &FromLength);
        if (Client != Net::Invalid)
        {
          Pipe P;
          P.Local = Client;
          P.FromPort = ntohs(From.sin_port);
          P.OpenTimer.start();
          Net::SetBlocking(Client, false);
          P.Channel = ssh_channel_new(Session);
          if ((P.Channel == nullptr) ||
              (ssh_channel_open_forward(P.Channel, Host.constData(), FSpec.Port, "127.0.0.1", P.FromPort) == SSH_ERROR))
          {
            emit ConnectionFailed(QStringLiteral("The server refused to forward the connection to %1:%2: %3")
              .arg(FSpec.Host, QString::number(FSpec.Port), QString::fromUtf8(ssh_get_error(Session))));
            ClosePipe(P);
          }
          else
          {
            Pipes.Items.push_back(std::move(P));
            Changed = true;
          }
        }
      }
      if (Changed)
      {
        FConnections = static_cast<int>(Pipes.Items.size());
        emit ConnectionsChanged(FConnections);
      }
      if (!ssh_is_connected(Session))
      {
        throw ExtException(QStringLiteral("The connection to the server was lost."));
      }
      if ((Pipes.Probe != nullptr) && (ssh_channel_open_session(Pipes.Probe) != SSH_AGAIN))
      {
        Pipes.CloseProbe();
      }
      if (Traffic.in_bytes != LastIn)
      {
        LastIn = Traffic.in_bytes;
        Silence.restart();
        PingSent = false;
      }
      else if (!PingSent && Silence.hasExpired(PingEvery))
      {
        Pipes.CloseProbe();
        Pipes.Probe = ssh_channel_new(Session);
        if ((Pipes.Probe == nullptr) || (ssh_channel_open_session(Pipes.Probe) == SSH_ERROR))
        {
          throw ExtException(QStringLiteral("The connection to the server was lost."));
        }
        PingSent = true;
        Silence.restart();
      }
      else if (PingSent && Silence.hasExpired(ReplyTimeout))
      {
        throw ExtException(QStringLiteral("The connection to the server was lost: no answer for %1 seconds.").arg(ReplyTimeout / 1000));
      }
    }
    Pipes.CloseAll();
    ssh_set_blocking(Session, 1);
    Net::SetBlocking(ssh_get_fd(Session), true);
    Shell.Close();
  }
  catch (const ExtException & E)
  {
    Error = E.Message();
    if (!E.MoreMessages().trimmed().isEmpty())
    {
      Error += QLatin1Char(' ') + E.MoreMessages().trimmed();
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
