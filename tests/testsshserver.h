// A small SSH server for tests that need what a real server will not do on
// request: go away mid-transfer, stop answering, end a command without an exit
// status. libssh server API, password authentication accepts anything, never
// runs a command. Started as a child: the test program calls itself with
// TestSshServer::ServerArgument and main() calls TestSshServer::RunIfAsked().
// With TESTSSH_PASSWORD in its environment only that password is accepted.
//
// exec "big <n>": n bytes on stdout, exit 0
// exec "cat": echoes standard input, EOF ends it, exit 0
// exec "count <seconds>": reads nothing for that long, then counts standard
//   input; at EOF prints the number, exit 0
// exec "quiet <seconds> <code>": EOF at once, the exit status later
// exec "noexit": EOF and close, no exit status
// exec "bytes ...": prints the command as it arrived, exit 0
//
// direct-tcpip, by the target port:
// 1: "BANNER\r\n", then echo    2: floods the client    3: echo    4: refused
// 5: holds the data for 3 seconds, then counts it; prints the number on the
//    server's standard output at EOF
// 6: the first open is never answered, the others echo
// 7: "HALF\n" and EOF at once, then counts what the client still sends
#pragma once

#include <QFile>
#include <QProcess>
#include <QString>
#include <QStringList>

#include <libssh/callbacks.h>
#include <libssh/libssh.h>
#include <libssh/server.h>

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#ifdef Q_OS_LINUX
#include <sys/prctl.h>
#else
#include <thread>
#endif
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

class TestSshServer
{
public:
  static constexpr const char * ServerArgument = "--test-ssh-server";

  // In the child: serves until killed, and does not return
  static void RunIfAsked(int argc, char ** argv)
  {
    if ((argc == 4) && (std::strcmp(argv[1], ServerArgument) == 0))
    {
      Serve(argv[2], argv[3]);
      std::_Exit(0);
    }
  }

  // Generates the host key in Folder; false when the server does not come up
  bool Start(const QString & Folder, int Port)
  {
    FPort = Port;
    FKnownHosts = Folder + QStringLiteral("/known_hosts");
    QString KeyFile = Folder + QStringLiteral("/host_key");
    if (!QFile::exists(KeyFile))
    {
      ssh_key Key = nullptr;
      if (ssh_pki_generate(SSH_KEYTYPE_ED25519, 0, &Key) != SSH_OK)
      {
        return false;
      }
      char * Base64 = nullptr;
      ssh_pki_export_pubkey_base64(Key, &Base64);
      ssh_pki_export_privkey_file(Key, nullptr, nullptr, nullptr, QFile::encodeName(KeyFile).constData());
      QFile Hosts(FKnownHosts);
      if (Hosts.open(QIODevice::WriteOnly))
      {
        Hosts.write(QStringLiteral("[127.0.0.1]:%1 ssh-ed25519 %2\n").arg(Port).arg(QString::fromLatin1(Base64)).toLatin1());
      }
      ssh_string_free_char(Base64);
      ssh_key_free(Key);
    }
    FProcess.setProcessChannelMode(QProcess::SeparateChannels);
    FProcess.start(QCoreApplication::applicationFilePath(), {QString::fromLatin1(ServerArgument), QString::number(Port), KeyFile});
    if (!FProcess.waitForStarted(5000) || !FProcess.waitForReadyRead(5000))
    {
      return false;
    }
    return FProcess.readLine().startsWith("listening");
  }

  // Everything goes at once, as with a power failure: no TCP goodbye for the
  // sessions is needed, they are killed with the server
  void Kill()
  {
    if (FProcess.processId() > 0)
    {
      kill(-static_cast<pid_t>(FProcess.processId()), SIGKILL);
    }
    FProcess.waitForFinished(5000);
  }
  // Alive for TCP, silent for SSH
  void Freeze() { kill(-static_cast<pid_t>(FProcess.processId()), SIGSTOP); }
  void Thaw() { kill(-static_cast<pid_t>(FProcess.processId()), SIGCONT); }

  QString KnownHosts() const { return FKnownHosts; }
  int Port() const { return FPort; }
  // What the server printed so far
  QByteArray Output()
  {
    FOutput += FProcess.readAllStandardOutput();
    return FOutput;
  }

  ~TestSshServer()
  {
    Kill();
  }

private:
  QProcess FProcess;
  QString FKnownHosts;
  QByteArray FOutput;
  int FPort = 0;

  enum TMode { mdNone, mdBig, mdCat, mdCount, mdQuiet, mdNoExit, mdEcho, mdFlood, mdDelayedSink, mdHalf, mdBytes };

  struct Chan
  {
    ssh_channel C = nullptr;
    TMode Mode = mdNone;
    long long Count = 0;
    std::string Out;
    time_t At = 0;
    int Code = 0;
    bool Eof = false;
    bool Closed = false;
  };

  struct State
  {
    std::vector<Chan *> Chans;
    bool SkippedOpen = false;
  };

  static int OnMessage(ssh_session, ssh_message M, void * User)
  {
    State * St = static_cast<State *>(User);
    int Type = ssh_message_type(M);
    int Sub = ssh_message_subtype(M);
    if (Type == SSH_REQUEST_AUTH)
    {
      const char * Wanted = std::getenv("TESTSSH_PASSWORD");
      if ((Sub == SSH_AUTH_METHOD_PASSWORD) && ((Wanted == nullptr) || (std::strcmp(Wanted, ssh_message_auth_password(M)) == 0)))
      {
        ssh_message_auth_reply_success(M, 0);
        return 0;
      }
      ssh_message_auth_set_methods(M, SSH_AUTH_METHOD_PASSWORD);
      ssh_message_reply_default(M);
      return 0;
    }
    if ((Type == SSH_REQUEST_CHANNEL_OPEN) && (Sub == SSH_CHANNEL_SESSION))
    {
      if (ssh_channel C = ssh_message_channel_request_open_reply_accept(M))
      {
        auto * H = new Chan();
        H->C = C;
        St->Chans.push_back(H);
      }
      return 0;
    }
    if ((Type == SSH_REQUEST_CHANNEL_OPEN) && (Sub == SSH_CHANNEL_DIRECT_TCPIP))
    {
      int Port = ssh_message_channel_request_open_destination_port(M);
      if (Port == 4)
      {
        ssh_message_reply_default(M);
        return 0;
      }
      if ((Port == 6) && !St->SkippedOpen)
      {
        St->SkippedOpen = true;
        return 0;
      }
      if (ssh_channel C = ssh_message_channel_request_open_reply_accept(M))
      {
        auto * H = new Chan();
        H->C = C;
        H->Mode = (Port == 2) ? mdFlood : (Port == 5) ? mdDelayedSink : (Port == 7) ? mdHalf : mdEcho;
        H->Out = (Port == 1) ? "BANNER\r\n" : (Port == 7) ? "HALF\n" : "";
        H->At = time(nullptr) + 3;
        St->Chans.push_back(H);
      }
      return 0;
    }
    if ((Type == SSH_REQUEST_CHANNEL) && (Sub == SSH_CHANNEL_REQUEST_EXEC))
    {
      ssh_channel C = ssh_message_channel_request_channel(M);
      for (Chan * H : St->Chans)
      {
        if (H->C == C)
        {
          long long N = 0;
          int A = 0, B = 0;
          const char * Command = ssh_message_channel_request_command(M);
          if (std::sscanf(Command, "big %lld", &N) == 1)
          {
            H->Mode = mdBig;
            H->Count = N;
          }
          else if (std::sscanf(Command, "count %d", &A) == 1)
          {
            H->Mode = mdCount;
            H->At = time(nullptr) + A;
          }
          else if (std::sscanf(Command, "quiet %d %d", &A, &B) == 2)
          {
            H->Mode = mdQuiet;
            H->At = time(nullptr) + A;
            H->Code = B;
          }
          else if (std::strcmp(Command, "cat") == 0)
          {
            H->Mode = mdCat;
          }
          else if (std::strcmp(Command, "noexit") == 0)
          {
            H->Mode = mdNoExit;
          }
          else if (std::strncmp(Command, "bytes ", 6) == 0)
          {
            H->Mode = mdBytes;
            H->Out = Command;
          }
          else
          {
            H->Mode = mdBig;
          }
          ssh_message_channel_request_reply_success(M);
          return 0;
        }
      }
    }
    return 1;
  }

  static void Finish(Chan * H, int Code)
  {
    ssh_channel_send_eof(H->C);
    if (Code >= 0)
    {
      ssh_channel_request_send_exit_status(H->C, Code);
    }
    ssh_channel_close(H->C);
    H->Closed = true;
  }

  static void Session(ssh_session S)
  {
    // before the key exchange: a fast client's first request comes right behind it
    State St;
    ssh_set_message_callback(S, OnMessage, &St);
    if (ssh_handle_key_exchange(S) != SSH_OK)
    {
      return;
    }
    ssh_set_blocking(S, 0);
    ssh_event Event = ssh_event_new();
    ssh_event_add_session(Event, S);
    static char Fill[32768];
    std::memset(Fill, 'x', sizeof(Fill));
    char In[32768];
    for (;;)
    {
      if ((ssh_event_dopoll(Event, 10) == SSH_ERROR) || !ssh_is_connected(S))
      {
        break;
      }
      // by index: reading a channel runs OnMessage, which may add channels
      for (size_t Index = 0; Index < St.Chans.size(); Index++)
      {
        Chan * H = St.Chans[Index];
        if (H->Closed)
        {
          continue;
        }
        int Window = static_cast<int>(std::min<uint32_t>(ssh_channel_window_size(H->C), sizeof(Fill)));
        switch (H->Mode)
        {
          case mdBig:
            if (H->Count > 0)
            {
              int N = (Window > 0) ? ssh_channel_write(H->C, Fill, static_cast<uint32_t>(std::min<long long>(Window, H->Count))) : 0;
              H->Count -= std::max(N, 0);
            }
            else
            {
              Finish(H, 0);
            }
            break;
          case mdBytes:
            if (!H->Out.empty() && (Window > 0))
            {
              int N = ssh_channel_write(H->C, H->Out.data(), static_cast<uint32_t>(std::min<size_t>(static_cast<size_t>(Window), H->Out.size())));
              H->Out.erase(0, static_cast<size_t>(std::max(N, 0)));
            }
            if (H->Out.empty())
            {
              Finish(H, 0);
            }
            break;
          case mdCat:
          case mdEcho:
          {
            if (H->Out.size() < 65536)
            {
              int N = ssh_channel_read_nonblocking(H->C, In, sizeof(In), 0);
              if (N > 0)
              {
                H->Out.append(In, static_cast<size_t>(N));
              }
            }
            if (!H->Out.empty() && (Window > 0))
            {
              int N = ssh_channel_write(H->C, H->Out.data(), static_cast<uint32_t>(std::min<size_t>(static_cast<size_t>(Window), H->Out.size())));
              if (N > 0)
              {
                H->Out.erase(0, static_cast<size_t>(N));
              }
            }
            if (H->Out.empty() && ssh_channel_is_eof(H->C))
            {
              Finish(H, (H->Mode == mdCat) ? 0 : -1);
            }
            break;
          }
          case mdCount:
          case mdDelayedSink:
          case mdHalf:
            if ((H->Mode == mdHalf) && !H->Eof)
            {
              ssh_channel_write(H->C, H->Out.data(), static_cast<uint32_t>(H->Out.size()));
              ssh_channel_send_eof(H->C);
              H->Eof = true;
            }
            if (time(nullptr) >= H->At)
            {
              int N = 0;
              while ((N = ssh_channel_read_nonblocking(H->C, In, sizeof(In), 0)) > 0)
              {
                H->Count += N;
              }
              if (ssh_channel_is_eof(H->C))
              {
                if (H->Mode == mdCount)
                {
                  std::string Text = std::to_string(H->Count) + "\n";
                  ssh_channel_write(H->C, Text.data(), static_cast<uint32_t>(Text.size()));
                  Finish(H, 0);
                }
                else
                {
                  std::printf("received %lld\n", H->Count);
                  std::fflush(stdout);
                  Finish(H, -1);
                }
              }
            }
            break;
          case mdQuiet:
            if (!H->Eof)
            {
              ssh_channel_send_eof(H->C);
              H->Eof = true;
            }
            if (time(nullptr) >= H->At)
            {
              ssh_channel_request_send_exit_status(H->C, H->Code);
              ssh_channel_close(H->C);
              H->Closed = true;
            }
            break;
          case mdNoExit:
            Finish(H, -1);
            break;
          case mdFlood:
            for (int I = 0; I < 64; I++)
            {
              Window = static_cast<int>(std::min<uint32_t>(ssh_channel_window_size(H->C), sizeof(Fill)));
              if ((Window <= 0) || (ssh_channel_write(H->C, Fill, static_cast<uint32_t>(Window)) <= 0))
              {
                break;
              }
            }
            if (ssh_channel_is_eof(H->C) || ssh_channel_is_closed(H->C))
            {
              ssh_channel_close(H->C);
              H->Closed = true;
            }
            break;
          default:
            break;
        }
      }
    }
    ssh_event_free(Event);
  }

  // Never outlives the process that started this one
  static void EndWithParent()
  {
#ifdef Q_OS_LINUX
    prctl(PR_SET_PDEATHSIG, SIGKILL);
#else
    pid_t Parent = getppid();
    std::thread([Parent]()
    {
      while (getppid() == Parent)
      {
        usleep(200000);
      }
      std::_Exit(1);
    }).detach();
#endif
  }

  static void Serve(const char * Port, const char * KeyFile)
  {
    // its own process group, for Kill and Freeze to reach all the sessions;
    // never outlives a test that was killed
    setpgid(0, 0);
    EndWithParent();
    signal(SIGCHLD, SIG_IGN);
    ssh_bind Bind = ssh_bind_new();
    ssh_bind_options_set(Bind, SSH_BIND_OPTIONS_BINDADDR, "127.0.0.1");
    ssh_bind_options_set(Bind, SSH_BIND_OPTIONS_BINDPORT_STR, Port);
    ssh_bind_options_set(Bind, SSH_BIND_OPTIONS_HOSTKEY, KeyFile);
    if (ssh_bind_listen(Bind) != SSH_OK)
    {
      std::printf("failed: %s\n", ssh_get_error(Bind));
      std::fflush(stdout);
      return;
    }
    std::printf("listening\n");
    std::fflush(stdout);
    for (;;)
    {
      ssh_session S = ssh_new();
      if (ssh_bind_accept(Bind, S) != SSH_OK)
      {
        ssh_free(S);
        continue;
      }
      if (fork() == 0)
      {
        EndWithParent();
        ssh_bind_free(Bind);
        Session(S);
        ssh_disconnect(S);
        ssh_free(S);
        std::_Exit(0);
      }
      ssh_free(S);
    }
  }
};
