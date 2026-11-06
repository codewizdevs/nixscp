#include "gui/AgentClient.h"
#include "gui/AgentMcp.h"

#include "core/AgentAccess.h"
#include "core/CopyParam.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <csignal>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

using namespace AgentAccess;

namespace
{

constexpr int ChunkSize = 64 * 1024;

#ifndef Q_OS_WIN
void RestoreTerminal();
#endif

int Usage(const QString & Message)
{
#ifndef Q_OS_WIN
  // an interactive shell put the terminal into raw mode: back to normal first,
  // or the lines of the message would not start at the left edge
  RestoreTerminal();
#endif
  std::fprintf(stderr, "nixscp: %s\nRun \"nixscp --help\" for usage.\n", Message.toLocal8Bit().constData());
  return ErrorExitCode;
}

void WriteOut(FILE * Stream, const QByteArray & Data)
{
  std::fwrite(Data.constData(), 1, static_cast<size_t>(Data.size()), Stream);
  std::fflush(Stream);
}

bool StdinIsTerminal()
{
#ifdef Q_OS_WIN
  return _isatty(_fileno(stdin));
#else
  return isatty(STDIN_FILENO);
#endif
}

void TerminalSize(int & Cols, int & Rows)
{
  Cols = 80;
  Rows = 24;
#ifndef Q_OS_WIN
  struct winsize Size = {};
  if ((ioctl(STDOUT_FILENO, TIOCGWINSZ, &Size) == 0) && (Size.ws_col > 0))
  {
    Cols = Size.ws_col;
    Rows = Size.ws_row;
  }
#endif
}

#ifndef Q_OS_WIN
struct termios GSavedTerminal;
bool GRawTerminal = false;
volatile sig_atomic_t GResized = 0;

void RestoreTerminal()
{
  if (GRawTerminal)
  {
    tcsetattr(STDIN_FILENO, TCSANOW, &GSavedTerminal);
    GRawTerminal = false;
  }
}

void RawTerminal()
{
  if (tcgetattr(STDIN_FILENO, &GSavedTerminal) == 0)
  {
    struct termios Raw = GSavedTerminal;
    cfmakeraw(&Raw);
    tcsetattr(STDIN_FILENO, TCSANOW, &Raw);
    GRawTerminal = true;
    std::atexit(RestoreTerminal);
  }
}
#endif

class Client : public QObject
{
public:
  Client(const QJsonObject & Request, const QString & LocalFile) : FRequest(Request), FLocalFile(LocalFile) {}

  int Exec()
  {
    FOp = FRequest.value(QStringLiteral("op")).toString();
    FSocket.connectToServer(SocketName());
    if (!FSocket.waitForConnected(3000))
    {
      return Usage(QStringLiteral("NixSCP is not running. The user needs to start NixSCP (and unlock it with the NixSCP password) to use agent access."));
    }
    if (!PeerIsSameUser(FSocket.socketDescriptor()))
    {
      return Usage(QStringLiteral("Refusing the agent socket \"%1\": it belongs to another user.").arg(SocketName()));
    }
    if (FOp == QStringLiteral("put"))
    {
      FInput.reset(new QFile(FLocalFile));
      if (!FInput->open(QIODevice::ReadOnly))
      {
        return Usage(QStringLiteral("Cannot read \"%1\".").arg(FLocalFile));
      }
    }
    QObject::connect(&FSocket, &QLocalSocket::readyRead, this, [this]() { Read(); });
    QObject::connect(&FSocket, &QLocalSocket::disconnected, this, [this]()
    {
      // the end of the output and the exit code may still wait in the buffer
      if (FResult < 0)
      {
        Read();
      }
      if (FResult < 0)
      {
        Finish(Usage(QStringLiteral("NixSCP closed the connection.")));
      }
    });
    FSocket.write(Frame(frRequest, QJsonDocument(FRequest).toJson(QJsonDocument::Compact)));
    if ((FOp == QStringLiteral("exec")) || (FOp == QStringLiteral("shell")))
    {
      StartStdin();
    }
#ifndef Q_OS_WIN
    if (FOp == QStringLiteral("shell"))
    {
      auto * Timer = new QTimer(this);
      QObject::connect(Timer, &QTimer::timeout, this, [this]()
      {
        if (GResized)
        {
          GResized = 0;
          int Cols, Rows;
          TerminalSize(Cols, Rows);
          FSocket.write(Frame(frResize, QJsonDocument(QJsonObject{{QStringLiteral("cols"), Cols}, {QStringLiteral("rows"), Rows}}).toJson(QJsonDocument::Compact)));
        }
      });
      Timer->start(200);
    }
#endif
    QCoreApplication::exec();
    return FResult;
  }

private:
  QJsonObject FRequest;
  QString FLocalFile;
  QString FOp;
  QLocalSocket FSocket;
  FrameReader FReader;
  int FResult = -1;
  // a download goes here, and replaces the local file only when it is complete
  std::unique_ptr<DownloadFile> FOutput;
  bool FDownloaded = false;
  std::unique_ptr<QFile> FInput;
  qint64 FSent = 0;
  // stdin (exec, shell) is sent from the main thread, read by another one
  std::atomic<qint64> FStdinRead{0};
  std::atomic<qint64> FAcked{0};
  bool FUploading = false;

  void Finish(int Code)
  {
    if (FResult >= 0)
    {
      return;
    }
    FResult = Code;
    // the process ends with _Exit: an unfinished download must clean up now
    FOutput.reset();
#ifndef Q_OS_WIN
    RestoreTerminal();
#endif
    QCoreApplication::exit(Code);
  }

  // stdin is read by a thread (blocking reads work for terminals, pipes and
  // files on every platform) and forwarded from the main thread
  void StartStdin()
  {
    std::thread([this]()
    {
      char Buffer[ChunkSize];
      for (;;)
      {
        // no further ahead of the server than the flow control allows: a
        // program that reads slowly (or never) stops the reading here
        while (FStdinRead - FAcked >= InputWindow)
        {
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
#ifdef Q_OS_WIN
        int N = _read(_fileno(stdin), Buffer, sizeof(Buffer));
#else
        ssize_t N = read(STDIN_FILENO, Buffer, sizeof(Buffer));
#endif
        FStdinRead += qMax<qint64>(N, 0);
        QByteArray Data = (N > 0) ? QByteArray(Buffer, static_cast<int>(N)) : QByteArray();
        QMetaObject::invokeMethod(this, [this, Data]()
        {
          if (FSocket.state() == QLocalSocket::ConnectedState)
          {
            FSocket.write(Data.isEmpty() ? Frame(frStdinEof) : Frame(frStdin, Data));
          }
        }, Qt::QueuedConnection);
        if (N <= 0)
        {
          break;
        }
      }
    }).detach();
  }

  void SendUpload()
  {
    while (FUploading && (FSent - FAcked < InputWindow))
    {
      QByteArray Chunk = FInput->read(ChunkSize);
      if (Chunk.isEmpty())
      {
        FSocket.write(Frame(frDataEnd));
        FUploading = false;
        break;
      }
      FSocket.write(Frame(frData, Chunk));
      FSent += Chunk.size();
    }
  }

  void Read()
  {
    FReader.Append(FSocket.readAll());
    quint8 Type = 0;
    QByteArray Payload;
    bool Error = false;
    while (FReader.Next(Type, Payload, Error))
    {
      switch (Type)
      {
        case frStdout:
          WriteOut(stdout, Payload);
          break;
        case frStderr:
          WriteOut(stderr, Payload);
          break;
        case frData:
          if (FOutput && !FOutput->Write(Payload))
          {
            Finish(Usage(QStringLiteral("Cannot write \"%1\": %2").arg(FLocalFile, FOutput->ErrorString())));
            return;
          }
          break;
        case frDataEnd:
          FDownloaded = true;
          break;
        case frInfo:
          if (FOp == QStringLiteral("get"))
          {
            // the request was accepted and the file exists: not before that
            FOutput.reset(new DownloadFile(FLocalFile));
            if (!FOutput->Open())
            {
              Finish(Usage(QStringLiteral("Cannot write \"%1\": %2").arg(FLocalFile, FOutput->ErrorString())));
              return;
            }
          }
          else if (FOp == QStringLiteral("put"))
          {
            FUploading = true;
            SendUpload();
          }
          else if (FOp == QStringLiteral("tunnel"))
          {
            QJsonObject Info = QJsonDocument::fromJson(Payload).object();
            WriteOut(stderr, QStringLiteral("Tunnel open: %1 -> %2 (through %3). Press Ctrl+C to close it.\n")
              .arg(Info.value(QStringLiteral("listening")).toString(), Info.value(QStringLiteral("target")).toString(),
                FRequest.value(QStringLiteral("site")).toString()).toUtf8());
          }
          break;
        case frAck:
          FAcked = Payload.toLongLong();
          SendUpload();
          break;
        case frList:
          PrintList(Payload);
          break;
        case frExit:
        {
          int Code = QJsonDocument::fromJson(Payload).object().value(QStringLiteral("code")).toInt(0);
          if ((FOp == QStringLiteral("get")) && (Code == 0) && (!FOutput || !FDownloaded || !FOutput->Commit()))
          {
            Finish(Usage(QStringLiteral("Cannot write \"%1\"%2").arg(FLocalFile,
              FOutput ? QStringLiteral(": ") + FOutput->ErrorString() : QStringLiteral("."))));
            return;
          }
          Finish(Code);
          return;
        }
        case frError:
          // whatever came so far is dropped, the local file stays as it was
          FOutput.reset();
          Finish(Usage(QString::fromUtf8(Payload)));
          return;
        default:
          break;
      }
    }
    if (Error)
    {
      Finish(Usage(QStringLiteral("Protocol error.")));
    }
  }

  void PrintList(const QByteArray & Payload)
  {
    QJsonArray Sites = QJsonDocument::fromJson(Payload).array();
    if (FRequest.value(QStringLiteral("json")).toBool())
    {
      WriteOut(stdout, QJsonDocument(Sites).toJson(QJsonDocument::Indented));
      return;
    }
    if (Sites.isEmpty())
    {
      WriteOut(stdout, "No sites are available to agents. The user enables them in NixSCP: site settings, Agent Access....\n");
      return;
    }
    QByteArray Out = "SITE\tHOST\tPERMITTED\tAPPROVAL\n";
    for (const QJsonValue & V : Sites)
    {
      QJsonObject S = V.toObject();
      QString Host = S.value(QStringLiteral("user")).toString();
      Host = (Host.isEmpty() ? QString() : Host + QLatin1Char('@')) + S.value(QStringLiteral("host")).toString();
      int Port = S.value(QStringLiteral("port")).toInt(22);
      if (Port != 22)
      {
        Host += QStringLiteral(":%1").arg(Port);
      }
      Out += QStringLiteral("%1\t%2\t%3\t%4\n").arg(S.value(QStringLiteral("name")).toString(), Host,
        S.value(QStringLiteral("permissions")).toString(),
        (S.value(QStringLiteral("approval")).toString() == QStringLiteral("ask")) ? QStringLiteral("asks user") : QStringLiteral("automatic")).toUtf8();
    }
    WriteOut(stdout, Out);
  }
};

}

#ifdef Q_OS_WIN
// nixscp.exe is a GUI program: typed in a Command Prompt it has no console, so
// attach to the parent's. Redirected streams (pipes and files, as AI tools
// run it) are kept.
static void AttachParentConsole()
{
  auto Missing = [](DWORD Std) { return GetFileType(GetStdHandle(Std)) == FILE_TYPE_UNKNOWN; };
  bool In = Missing(STD_INPUT_HANDLE), Out = Missing(STD_OUTPUT_HANDLE), Err = Missing(STD_ERROR_HANDLE);
  if ((!In && !Out && !Err) || !AttachConsole(ATTACH_PARENT_PROCESS))
  {
    return;
  }
  if (In) std::freopen("CONIN$", "r", stdin);
  if (Out) std::freopen("CONOUT$", "w", stdout);
  if (Err) std::freopen("CONOUT$", "w", stderr);
}
#endif

namespace AgentClient
{

int Run(int argc, char * argv[], const QStringList & Arguments)
{
  static const QStringList Commands{QStringLiteral("--list"), QStringLiteral("--connect"), QStringLiteral("--get"),
    QStringLiteral("--put"), QStringLiteral("--tunnel"), QStringLiteral("--help"), QStringLiteral("--mcp")};
  if (Arguments.isEmpty() || !Commands.contains(Arguments.first()))
  {
    return -1;
  }
#ifdef Q_OS_WIN
  AttachParentConsole();
#endif
  QString Command = Arguments.first();
  if (Command == QStringLiteral("--help"))
  {
    WriteOut(stdout, HelpText().toUtf8());
    return 0;
  }
#ifdef Q_OS_WIN
  _setmode(_fileno(stdout), _O_BINARY);
  _setmode(_fileno(stdin), _O_BINARY);
#endif
  QCoreApplication App(argc, argv);
  if (Command == QStringLiteral("--mcp"))
  {
    // MCP server for AI tools: JSON-RPC on standard input/output
    QCoreApplication::setApplicationVersion(QStringLiteral(PROJECT_VERSION));
    int Code = AgentMcp::Serve();
    std::fflush(stdout);
    std::_Exit(Code);
  }

  // Options (--json, --cmd <command> / --cmd=<command>) and positional arguments
  QStringList Positional;
  bool Json = false;
  bool HasCmd = false;
  QString Cmd;
  QStringList Args = Arguments.mid(1);
  for (qsizetype I = 0; I < Args.size(); I++)
  {
    const QString & Arg = Args.at(I);
    if ((Command == QStringLiteral("--connect")) && (Arg == QStringLiteral("--cmd")))
    {
      // the rest of the line is the command ("--cmd uname -a" as "--cmd 'uname -a'")
      HasCmd = true;
      Cmd = Args.mid(I + 1).join(QLatin1Char(' '));
      break;
    }
    if ((Command == QStringLiteral("--connect")) && Arg.startsWith(QStringLiteral("--cmd=")))
    {
      HasCmd = true;
      Cmd = Arg.mid(6);
      if (I + 1 < Args.size())
      {
        return Usage(QStringLiteral("Unexpected argument after --cmd=...: \"%1\". Quote the whole command.").arg(Args.at(I + 1)));
      }
      break;
    }
    if ((Command == QStringLiteral("--list")) && (Arg == QStringLiteral("--json")))
    {
      Json = true;
      continue;
    }
    if (Arg.startsWith(QStringLiteral("--")))
    {
      return Usage(QStringLiteral("Unknown option \"%1\".").arg(Arg));
    }
    Positional.append(Arg);
  }

  QJsonObject Request;
  QString LocalFile;
  if (Command == QStringLiteral("--list"))
  {
    if (!Positional.isEmpty())
    {
      return Usage(QStringLiteral("Unexpected argument \"%1\". Usage: nixscp --list [--json]").arg(Positional.first()));
    }
    Request.insert(QStringLiteral("op"), QStringLiteral("list"));
    Request.insert(QStringLiteral("json"), Json);
  }
  else if (Command == QStringLiteral("--connect"))
  {
    if (Positional.isEmpty() || Positional.first().isEmpty())
    {
      return Usage(QStringLiteral("Missing site name. Usage: nixscp --connect <site> [--cmd <command>]"));
    }
    if (Positional.size() > 1)
    {
      return Usage(QStringLiteral("Unexpected argument \"%1\". Give the remote command with --cmd: "
        "nixscp --connect <site> --cmd \"<command>\"").arg(Positional.at(1)));
    }
    Request.insert(QStringLiteral("site"), Positional.first());
    if (HasCmd)
    {
      if (Cmd.trimmed().isEmpty())
      {
        return Usage(QStringLiteral("Missing command after --cmd."));
      }
      Request.insert(QStringLiteral("op"), QStringLiteral("exec"));
      Request.insert(QStringLiteral("command"), Cmd);
    }
    else
    {
      if (!StdinIsTerminal())
      {
        return Usage(QStringLiteral("An interactive shell needs a terminal. Give a command: nixscp --connect <site> --cmd \"<command>\""));
      }
      int Cols, Rows;
      TerminalSize(Cols, Rows);
      Request.insert(QStringLiteral("op"), QStringLiteral("shell"));
      Request.insert(QStringLiteral("cols"), Cols);
      Request.insert(QStringLiteral("rows"), Rows);
      Request.insert(QStringLiteral("term"), qEnvironmentVariable("TERM", QStringLiteral("xterm-256color")));
#ifndef Q_OS_WIN
      std::signal(SIGWINCH, [](int) { GResized = 1; });
      RawTerminal();
#endif
    }
  }
  else if (Command == QStringLiteral("--tunnel"))
  {
    if (Positional.size() != 2)
    {
      return Usage(QStringLiteral("Usage: nixscp --tunnel <site> <local port>:<host>:<port>  (or the name of a saved tunnel)"));
    }
    Request.insert(QStringLiteral("op"), QStringLiteral("tunnel"));
    Request.insert(QStringLiteral("site"), Positional.at(0));
    Request.insert(QStringLiteral("forward"), Positional.at(1));
  }
  else if (Command == QStringLiteral("--get"))
  {
    if ((Positional.size() < 2) || (Positional.size() > 3))
    {
      return Usage(QStringLiteral("Usage: nixscp --get <site> <remote-file> [<local-file-or-folder>]"));
    }
    QString Remote = Positional.at(1);
    QString RemoteName = Remote.section(QLatin1Char('/'), -1);
    if (RemoteName.isEmpty())
    {
      return Usage(QStringLiteral("Give a remote file, not a folder."));
    }
    QString LocalName = CopyParam::ValidLocalFileName(RemoteName);
    LocalFile = (Positional.size() > 2) ? Positional.at(2) : LocalName;
    if (QFileInfo(LocalFile).isDir())
    {
      LocalFile = QDir(LocalFile).filePath(LocalName);
    }
    Request.insert(QStringLiteral("op"), QStringLiteral("get"));
    Request.insert(QStringLiteral("site"), Positional.at(0));
    Request.insert(QStringLiteral("remote"), Remote);
  }
  else
  {
    if ((Positional.size() < 2) || (Positional.size() > 3))
    {
      return Usage(QStringLiteral("Usage: nixscp --put <site> <local-file> [<remote-file-or-folder>]"));
    }
    LocalFile = Positional.at(1);
    QFileInfo Info(LocalFile);
    if (!Info.isFile())
    {
      return Usage(QStringLiteral("\"%1\" is not a file. Upload folders with tar, e.g.: "
        "tar czf - <folder> | nixscp --connect <site> --cmd \"tar xzf -\"").arg(LocalFile));
    }
    Request.insert(QStringLiteral("op"), QStringLiteral("put"));
    Request.insert(QStringLiteral("site"), Positional.at(0));
    Request.insert(QStringLiteral("remote"), (Positional.size() > 2) ? Positional.at(2) : QString());
    Request.insert(QStringLiteral("name"), Info.fileName());
    Request.insert(QStringLiteral("size"), Info.size());
  }
  Client C(Request, LocalFile);
  int Code = C.Exec();
#ifndef Q_OS_WIN
  RestoreTerminal();
#endif
  std::fflush(stdout);
  // the stdin reader thread may still block in read(): leave without joining it
  std::_Exit(Code);
}

}
