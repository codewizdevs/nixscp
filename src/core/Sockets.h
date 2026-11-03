// BSD sockets on Linux and Winsock on Windows behind the same small API, for
// the code that needs raw sockets (SSH proxy negotiation and connection
// timeouts, closing FTP data connections). Note that a Windows SOCKET is
// unsigned: compare with Net::Invalid, never with < 0.
#pragma once

#include <QString>
#include <QtGlobal>

#ifdef Q_OS_WIN
// winsock2.h / ws2tcpip.h come first, from WinPrelude.h
#else
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace Net
{
#ifdef Q_OS_WIN
  using Socket = SOCKET;
  using PollFd = WSAPOLLFD;
  inline constexpr Socket Invalid = INVALID_SOCKET;
  inline constexpr int NoSignal = 0;      // Windows raises no SIGPIPE
  inline constexpr int ShutWrite = SD_SEND;

  // Winsock must be started once per process (libssh and Qt do too; the calls nest)
  inline void Startup()
  {
    static const bool Started = []
    {
      WSADATA Data;
      return WSAStartup(MAKEWORD(2, 2), &Data) == 0;
    }();
    Q_UNUSED(Started);
  }

  // Not inherited by child processes (as SOCK_CLOEXEC)
  inline Socket Open(int Family, int Type, int Protocol)
  {
    Startup();
    return WSASocketW(Family, Type, Protocol, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
  }
  inline int Close(Socket S) { return closesocket(S); }
  inline int LastError() { return WSAGetLastError(); }
  inline bool Interrupted(int Error) { return Error == WSAEINTR; }
  inline bool WouldBlock(int Error) { return (Error == WSAEWOULDBLOCK) || (Error == WSAEINPROGRESS); }
  inline int Poll(PollFd * Fds, unsigned long Count, int TimeoutMs) { return WSAPoll(Fds, Count, TimeoutMs); }
  inline bool SetBlocking(Socket S, bool Blocking)
  {
    u_long NonBlocking = Blocking ? 0 : 1;
    return ioctlsocket(S, FIONBIO, &NonBlocking) == 0;
  }
  inline qint64 Send(Socket S, const char * Data, qint64 Size, int Flags)
  {
    return ::send(S, Data, static_cast<int>(qMin<qint64>(Size, 0x40000000)), Flags);
  }
  inline qint64 Receive(Socket S, char * Data, qint64 Size, int Flags)
  {
    return ::recv(S, Data, static_cast<int>(qMin<qint64>(Size, 0x40000000)), Flags);
  }
  inline QString ErrorText(int Error)
  {
    wchar_t * Buffer = nullptr;
    DWORD Length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, static_cast<DWORD>(Error), 0, reinterpret_cast<wchar_t *>(&Buffer), 0, nullptr);
    QString Result = (Length > 0) ? QString::fromWCharArray(Buffer, static_cast<int>(Length)).trimmed() : QStringLiteral("Socket error %1").arg(Error);
    LocalFree(Buffer);
    return Result;
  }
  // A second handle to the same connection (used to finish closing FTP uploads)
  inline Socket Duplicate(Socket S)
  {
    WSAPROTOCOL_INFOW Info;
    if (WSADuplicateSocketW(S, GetCurrentProcessId(), &Info) != 0)
    {
      return Invalid;
    }
    return WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, &Info, 0, WSA_FLAG_NO_HANDLE_INHERIT);
  }
  inline int GetError(Socket S)
  {
    int Error = 0;
    int Length = sizeof(Error);
    getsockopt(S, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&Error), &Length);
    return Error;
  }
  inline constexpr int TimedOut = WSAETIMEDOUT;
#else
  using Socket = int;
  using PollFd = pollfd;
  inline constexpr Socket Invalid = -1;
#ifdef Q_OS_MACOS
  inline constexpr int NoSignal = 0;      // SO_NOSIGPIPE, set by Open; SIGPIPE is ignored too
#else
  inline constexpr int NoSignal = MSG_NOSIGNAL;
#endif
  inline constexpr int ShutWrite = SHUT_WR;

  inline void Startup()
  {
  }
#ifdef Q_OS_MACOS
  inline Socket Open(int Family, int Type, int Protocol)
  {
    Socket S = ::socket(Family, Type, Protocol);
    if (S >= 0)
    {
      fcntl(S, F_SETFD, FD_CLOEXEC);
      int On = 1;
      setsockopt(S, SOL_SOCKET, SO_NOSIGPIPE, &On, sizeof(On));
    }
    return S;
  }
#else
  inline Socket Open(int Family, int Type, int Protocol) { return ::socket(Family, Type | SOCK_CLOEXEC, Protocol); }
#endif
  inline int Close(Socket S) { return ::close(S); }
  inline int LastError() { return errno; }
  inline bool Interrupted(int Error) { return Error == EINTR; }
  inline bool WouldBlock(int Error) { return (Error == EINPROGRESS) || (Error == EAGAIN) || (Error == EWOULDBLOCK); }
  inline int Poll(PollFd * Fds, unsigned long Count, int TimeoutMs) { return ::poll(Fds, static_cast<nfds_t>(Count), TimeoutMs); }
  inline bool SetBlocking(Socket S, bool Blocking)
  {
    int Flags = fcntl(S, F_GETFL, 0);
    return fcntl(S, F_SETFL, Blocking ? (Flags & ~O_NONBLOCK) : (Flags | O_NONBLOCK)) == 0;
  }
  inline qint64 Send(Socket S, const char * Data, qint64 Size, int Flags)
  {
    return ::send(S, Data, static_cast<size_t>(Size), Flags);
  }
  inline qint64 Receive(Socket S, char * Data, qint64 Size, int Flags)
  {
    return ::recv(S, Data, static_cast<size_t>(Size), Flags);
  }
  inline QString ErrorText(int Error) { return QString::fromLocal8Bit(strerror(Error)); }
  inline Socket Duplicate(Socket S) { return ::dup(S); }
  inline int GetError(Socket S)
  {
    int Error = 0;
    socklen_t Length = sizeof(Error);
    getsockopt(S, SOL_SOCKET, SO_ERROR, &Error, &Length);
    return Error;
  }
  inline constexpr int TimedOut = ETIMEDOUT;
#endif
}
