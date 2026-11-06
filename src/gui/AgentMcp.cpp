#include "gui/AgentMcp.h"

#include "core/AgentAccess.h"
#include "core/CopyParam.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QList>
#include <QTimer>

#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

using namespace AgentAccess;

namespace
{

constexpr int ChunkSize = 64 * 1024;
constexpr qint64 DefaultMaxOutput = 100 * 1024;
constexpr qint64 LimitMaxOutput = 2 * 1024 * 1024;
// the MCP protocol versions this server speaks (newest first)
const QStringList ProtocolVersions{QStringLiteral("2025-06-18"), QStringLiteral("2025-03-26"), QStringLiteral("2024-11-05")};

// One request to the broker, collecting the result (the --connect/--get/--put
// client streams to the terminal instead)
struct Outcome
{
  int Code = ErrorExitCode;
  QString Error;          // NixSCP error (frError), empty when the request ran
  QByteArray Stdout;
  QByteArray Stderr;
  bool Truncated = false;
  QByteArray List;        // frList
};

// Set while a request runs: aborting the socket ends it, and the broker then
// stops the work on the server
QLocalSocket * GRunningSocket = nullptr;

Outcome Request(const QJsonObject & Req, const QByteArray & Stdin, const QString & LocalFile, qint64 MaxOutput)
{
  Outcome Result;
  QLocalSocket Socket;
  Socket.connectToServer(SocketName());
  if (!Socket.waitForConnected(3000))
  {
    Result.Error = QStringLiteral("NixSCP is not running. The user needs to start NixSCP to use its sites.");
    return Result;
  }
  if (!PeerIsSameUser(Socket.socketDescriptor()))
  {
    Result.Error = QStringLiteral("Refusing the agent socket \"%1\": it belongs to another user.").arg(SocketName());
    return Result;
  }
  QString Op = Req.value(QStringLiteral("op")).toString();
  std::unique_ptr<DownloadFile> Output;
  std::unique_ptr<QFile> Input;
  if (Op == QStringLiteral("put"))
  {
    Input.reset(new QFile(LocalFile));
    if (!Input->open(QIODevice::ReadOnly))
    {
      Result.Error = QStringLiteral("Cannot read \"%1\".").arg(LocalFile);
      return Result;
    }
  }
  QEventLoop Loop;
  FrameReader Reader;
  bool Done = false;
  bool Downloaded = false;
  bool Uploading = false;
  bool Feeding = false;
  qint64 Sent = 0, Acked = 0;
  auto Finish = [&]() { Done = true; Loop.quit(); };
  auto Keep = [&](QByteArray & Buffer, const QByteArray & Data)
  {
    qint64 Room = MaxOutput - Buffer.size();
    Buffer.append(Data.left(qMax<qint64>(Room, 0)));
    Result.Truncated = Result.Truncated || (Data.size() > Room);
  };
  auto SendUpload = [&]()
  {
    while (Uploading && (Sent - Acked < InputWindow))
    {
      QByteArray Chunk = Input->read(ChunkSize);
      if (Chunk.isEmpty())
      {
        Socket.write(Frame(frDataEnd));
        Uploading = false;
        break;
      }
      Socket.write(Frame(frData, Chunk));
      Sent += Chunk.size();
    }
  };
  // in frames the broker takes, and only as far as the flow control allows
  auto SendStdin = [&]()
  {
    while (Feeding && (Sent - Acked < InputWindow))
    {
      if (Sent >= Stdin.size())
      {
        Socket.write(Frame(frStdinEof));
        Feeding = false;
        break;
      }
      QByteArray Chunk = Stdin.mid(Sent, ChunkSize);
      Socket.write(Frame(frStdin, Chunk));
      Sent += Chunk.size();
    }
  };
  auto Read = [&]()
  {
    Reader.Append(Socket.readAll());
    quint8 Type = 0;
    QByteArray Payload;
    bool Error = false;
    while (!Done && Reader.Next(Type, Payload, Error))
    {
      switch (Type)
      {
        case frStdout: Keep(Result.Stdout, Payload); break;
        case frStderr: Keep(Result.Stderr, Payload); break;
        case frData:
          if (Output && !Output->Write(Payload))
          {
            Result.Error = QStringLiteral("Cannot write \"%1\": %2").arg(LocalFile, Output->ErrorString());
            Finish();
          }
          break;
        case frDataEnd:
          Downloaded = true;
          break;
        case frInfo:
          if (Op == QStringLiteral("get"))
          {
            // not before the broker accepted the request and found the file
            Output.reset(new DownloadFile(LocalFile));
            if (!Output->Open())
            {
              Result.Error = QStringLiteral("Cannot write \"%1\": %2").arg(LocalFile, Output->ErrorString());
              Finish();
            }
          }
          else if (Op == QStringLiteral("put"))
          {
            Uploading = true;
            SendUpload();
          }
          break;
        case frAck:
          Acked = Payload.toLongLong();
          SendUpload();
          SendStdin();
          break;
        case frList:
          Result.List = Payload;
          break;
        case frExit:
          Result.Code = QJsonDocument::fromJson(Payload).object().value(QStringLiteral("code")).toInt(0);
          if ((Op == QStringLiteral("get")) && (Result.Code == 0) && (!Output || !Downloaded || !Output->Commit()))
          {
            Result.Error = QStringLiteral("Cannot write \"%1\": %2").arg(LocalFile, Output ? Output->ErrorString() : QString());
          }
          Finish();
          break;
        case frError:
          Result.Error = QString::fromUtf8(Payload);
          Finish();
          break;
        default:
          break;
      }
    }
    if (Error && !Done)
    {
      Result.Error = QStringLiteral("Protocol error.");
      Finish();
    }
  };
  QObject::connect(&Socket, &QLocalSocket::readyRead, &Loop, Read);
  QObject::connect(&Socket, &QLocalSocket::disconnected, &Loop, [&]()
  {
    Read();
    if (!Done)
    {
      Result.Error = QStringLiteral("NixSCP closed the connection.");
      Finish();
    }
  });
  Socket.write(Frame(frRequest, QJsonDocument(Req).toJson(QJsonDocument::Compact)));
  if (Op == QStringLiteral("exec"))
  {
    Feeding = true;
    SendStdin();
  }
  if (!Done)
  {
    GRunningSocket = &Socket;
    Loop.exec();
    GRunningSocket = nullptr;
  }
  return Result;
}

QJsonObject Schema(const QJsonObject & Properties, const QStringList & Required)
{
  return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}, {QStringLiteral("properties"), Properties},
    {QStringLiteral("required"), QJsonArray::fromStringList(Required)}};
}

QJsonObject Prop(const QString & Type, const QString & Description)
{
  return QJsonObject{{QStringLiteral("type"), Type}, {QStringLiteral("description"), Description}};
}

QJsonArray Tools()
{
  QJsonObject Site = Prop(QStringLiteral("string"), QStringLiteral("Site name exactly as returned by list_sites"));
  return QJsonArray{
    QJsonObject{{QStringLiteral("name"), QStringLiteral("list_sites")},
      {QStringLiteral("description"), QStringLiteral("Lists the SSH servers (sites) you may use through NixSCP: name, host, "
        "what is permitted and whether the user approves each request. Call this first. Passwords and keys are never revealed.")},
      {QStringLiteral("inputSchema"), Schema(QJsonObject(), {})}},
    QJsonObject{{QStringLiteral("name"), QStringLiteral("run_command")},
      {QStringLiteral("description"), QStringLiteral("Runs a shell command on a site (the user's login shell on the server, "
        "no terminal: avoid interactive programs, use non-interactive flags such as --no-pager or -y). Returns the exit code, "
        "standard output and standard error. Sites may restrict commands or ask the user to approve first.")},
      {QStringLiteral("inputSchema"), Schema(QJsonObject{
        {QStringLiteral("site"), Site},
        {QStringLiteral("command"), Prop(QStringLiteral("string"), QStringLiteral("The command line, e.g. \"systemctl status nginx --no-pager\""))},
        {QStringLiteral("stdin"), Prop(QStringLiteral("string"), QStringLiteral("Optional text passed to the command's standard input"))},
        {QStringLiteral("max_output_bytes"), Prop(QStringLiteral("integer"), QStringLiteral("Output kept per stream (default 102400, at most 2097152)"))},
      }, {QStringLiteral("site"), QStringLiteral("command")})}},
    QJsonObject{{QStringLiteral("name"), QStringLiteral("download_file")},
      {QStringLiteral("description"), QStringLiteral("Downloads one file from a site to this computer. For a folder, create an "
        "archive first with run_command (e.g. \"tar czf /tmp/site.tar.gz /var/www/site\") and download that.")},
      {QStringLiteral("inputSchema"), Schema(QJsonObject{
        {QStringLiteral("site"), Site},
        {QStringLiteral("remote_path"), Prop(QStringLiteral("string"), QStringLiteral("File on the server (relative paths are in the home folder)"))},
        {QStringLiteral("local_path"), Prop(QStringLiteral("string"), QStringLiteral("Where to save it on this computer (a file or an existing folder)"))},
      }, {QStringLiteral("site"), QStringLiteral("remote_path"), QStringLiteral("local_path")})}},
    QJsonObject{{QStringLiteral("name"), QStringLiteral("upload_file")},
      {QStringLiteral("description"), QStringLiteral("Uploads one file from this computer to a site.")},
      {QStringLiteral("inputSchema"), Schema(QJsonObject{
        {QStringLiteral("site"), Site},
        {QStringLiteral("local_path"), Prop(QStringLiteral("string"), QStringLiteral("File on this computer"))},
        {QStringLiteral("remote_path"), Prop(QStringLiteral("string"), QStringLiteral("File or folder on the server (default: the home folder)"))},
      }, {QStringLiteral("site"), QStringLiteral("local_path")})}},
  };
}

QJsonObject TextResult(const QString & Text, bool IsError)
{
  return QJsonObject{{QStringLiteral("content"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
    {QStringLiteral("text"), Text}}}}, {QStringLiteral("isError"), IsError}};
}

QJsonObject CallTool(const QString & Name, const QJsonObject & Args)
{
  auto Str = [&Args](const char * Key) { return Args.value(QLatin1String(Key)).toString(); };
  if (Name == QStringLiteral("list_sites"))
  {
    Outcome R = Request(QJsonObject{{QStringLiteral("op"), QStringLiteral("list")}}, QByteArray(), QString(), 0);
    if (!R.Error.isEmpty())
    {
      return TextResult(R.Error, true);
    }
    QJsonArray Sites = QJsonDocument::fromJson(R.List).array();
    if (Sites.isEmpty())
    {
      return TextResult(QStringLiteral("No sites are available to agents. The user enables them in NixSCP (site settings: Agent Access...)."), false);
    }
    QJsonObject Result = TextResult(QString::fromUtf8(QJsonDocument(Sites).toJson(QJsonDocument::Indented)), false);
    Result.insert(QStringLiteral("structuredContent"), QJsonObject{{QStringLiteral("sites"), Sites}});
    return Result;
  }
  if (Name == QStringLiteral("run_command"))
  {
    if (Str("site").isEmpty() || Str("command").trimmed().isEmpty())
    {
      return TextResult(QStringLiteral("Give \"site\" and \"command\"."), true);
    }
    qint64 Max = qBound<qint64>(1024, Args.value(QStringLiteral("max_output_bytes")).toInteger(DefaultMaxOutput), LimitMaxOutput);
    Outcome R = Request(QJsonObject{{QStringLiteral("op"), QStringLiteral("exec")}, {QStringLiteral("site"), Str("site")},
      {QStringLiteral("command"), Str("command")}}, Str("stdin").toUtf8(), QString(), Max);
    if (!R.Error.isEmpty())
    {
      return TextResult(R.Error, true);
    }
    QString Text = QStringLiteral("exit code: %1\n").arg(R.Code);
    Text += QStringLiteral("--- stdout ---\n") + QString::fromUtf8(R.Stdout);
    if (!R.Stderr.isEmpty())
    {
      Text += QStringLiteral("\n--- stderr ---\n") + QString::fromUtf8(R.Stderr);
    }
    if (R.Truncated)
    {
      Text += QStringLiteral("\n[output truncated; raise max_output_bytes or filter the output]");
    }
    QJsonObject Result = TextResult(Text, false);
    Result.insert(QStringLiteral("structuredContent"), QJsonObject{{QStringLiteral("exit_code"), R.Code},
      {QStringLiteral("stdout"), QString::fromUtf8(R.Stdout)}, {QStringLiteral("stderr"), QString::fromUtf8(R.Stderr)},
      {QStringLiteral("truncated"), R.Truncated}});
    return Result;
  }
  if (Name == QStringLiteral("download_file"))
  {
    QString Remote = Str("remote_path");
    QString Local = Str("local_path");
    QString RemoteName = Remote.section(QLatin1Char('/'), -1);
    if (Str("site").isEmpty() || RemoteName.isEmpty() || Local.isEmpty())
    {
      return TextResult(QStringLiteral("Give \"site\", a remote file \"remote_path\" and \"local_path\"."), true);
    }
    if (QFileInfo(Local).isDir())
    {
      Local = QDir(Local).filePath(CopyParam::ValidLocalFileName(RemoteName));
    }
    Outcome R = Request(QJsonObject{{QStringLiteral("op"), QStringLiteral("get")}, {QStringLiteral("site"), Str("site")},
      {QStringLiteral("remote"), Remote}}, QByteArray(), Local, 0);
    if (!R.Error.isEmpty())
    {
      return TextResult(R.Error, true);
    }
    return TextResult(QStringLiteral("Downloaded %1 to %2 (%3 bytes).").arg(Remote, QFileInfo(Local).absoluteFilePath())
      .arg(QFileInfo(Local).size()), false);
  }
  if (Name == QStringLiteral("upload_file"))
  {
    QFileInfo Info(Str("local_path"));
    if (Str("site").isEmpty() || !Info.isFile())
    {
      return TextResult(QStringLiteral("Give \"site\" and an existing file \"local_path\"."), true);
    }
    Outcome R = Request(QJsonObject{{QStringLiteral("op"), QStringLiteral("put")}, {QStringLiteral("site"), Str("site")},
      {QStringLiteral("remote"), Str("remote_path")}, {QStringLiteral("name"), Info.fileName()}, {QStringLiteral("size"), Info.size()}},
      QByteArray(), Info.absoluteFilePath(), 0);
    if (!R.Error.isEmpty())
    {
      return TextResult(R.Error, true);
    }
    return TextResult(QStringLiteral("Uploaded %1 (%2 bytes).").arg(Info.fileName()).arg(Info.size()), false);
  }
  return QJsonObject();
}

QJsonObject ResultMessage(const QJsonValue & Id, const QJsonObject & Result)
{
  return QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("id"), Id}, {QStringLiteral("result"), Result}};
}

QJsonObject ErrorMessage(const QJsonValue & Id, int Code, const QString & Message)
{
  return QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("id"), Id},
    {QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), Code}, {QStringLiteral("message"), Message}}}};
}

void Print(const QByteArray & Json)
{
  std::fputs(Json.constData(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

// The replies of a JSON-RPC batch go out together, as one array
struct Batch
{
  QJsonArray Replies;
  int Waiting = 0;      // tool calls still to finish
  bool Read = false;    // all its requests were looked at
};

// A tool call waiting for its turn
struct Call
{
  QJsonValue Id;
  QString Tool;
  QJsonObject Arguments;
  std::shared_ptr<Batch> InBatch;
};

// Standard input is read by a thread, so messages also arrive while a tool call
// runs (in the event loop of Request): ping is answered and a call cancelled at
// once. Tool calls themselves run one at a time, in the order they came.
class Server : public QObject
{
public:
  int Run()
  {
    std::thread([this]()
    {
      std::string Line;
      while (std::getline(std::cin, Line))
      {
        QByteArray Message = QByteArray::fromStdString(Line);
        QMetaObject::invokeMethod(this, [this, Message]() { Receive(Message); }, Qt::QueuedConnection);
      }
      QMetaObject::invokeMethod(this, [this]()
      {
        FInputEnded = true;
        ExitWhenDone();
      }, Qt::QueuedConnection);
    }).detach();
    return QCoreApplication::exec();
  }

private:
  QList<Call> FCalls;
  bool FBusy = false;
  bool FCancelled = false;
  QJsonValue FRunningId;
  bool FInputEnded = false;

  void ExitWhenDone()
  {
    if (FInputEnded && !FBusy && FCalls.isEmpty())
    {
      QCoreApplication::exit(0);
    }
  }

  void Receive(const QByteArray & Line)
  {
    if (Line.trimmed().isEmpty())
    {
      return;
    }
    QJsonParseError ParseError;
    QJsonDocument Doc = QJsonDocument::fromJson(Line, &ParseError);
    if (ParseError.error != QJsonParseError::NoError)
    {
      Print(QJsonDocument(ErrorMessage(QJsonValue(), -32700, QStringLiteral("Parse error"))).toJson(QJsonDocument::Compact));
    }
    else if (Doc.isObject())
    {
      Handle(Doc.object(), nullptr);
    }
    else if (Doc.isArray() && !Doc.array().isEmpty())
    {
      auto InBatch = std::make_shared<Batch>();
      for (const QJsonValue & Element : Doc.array())
      {
        if (Element.isObject())
        {
          Handle(Element.toObject(), InBatch);
        }
        else
        {
          InBatch->Replies.append(ErrorMessage(QJsonValue(), -32600, QStringLiteral("Invalid request")));
        }
      }
      InBatch->Read = true;
      FlushBatch(InBatch);
    }
    else
    {
      Print(QJsonDocument(ErrorMessage(QJsonValue(), -32600, QStringLiteral("Invalid request"))).toJson(QJsonDocument::Compact));
    }
    RunCalls();
  }

  void Reply(const QJsonObject & Message, const std::shared_ptr<Batch> & InBatch)
  {
    if (InBatch != nullptr)
    {
      InBatch->Replies.append(Message);
    }
    else
    {
      Print(QJsonDocument(Message).toJson(QJsonDocument::Compact));
    }
  }

  void FlushBatch(const std::shared_ptr<Batch> & InBatch)
  {
    if (InBatch->Read && (InBatch->Waiting == 0) && !InBatch->Replies.isEmpty())
    {
      Print(QJsonDocument(InBatch->Replies).toJson(QJsonDocument::Compact));
      InBatch->Replies = QJsonArray();
    }
  }

  void Cancel(const QJsonValue & Id)
  {
    for (qsizetype I = 0; I < FCalls.size(); I++)
    {
      if (FCalls.at(I).Id == Id)
      {
        std::shared_ptr<Batch> InBatch = FCalls.at(I).InBatch;
        FCalls.removeAt(I);
        if (InBatch != nullptr)
        {
          InBatch->Waiting--;
          FlushBatch(InBatch);
        }
        return;
      }
    }
    if (FBusy && (FRunningId == Id))
    {
      // a cancelled request gets no reply
      FCancelled = true;
      if (GRunningSocket != nullptr)
      {
        GRunningSocket->abort();
      }
    }
  }

  void Handle(const QJsonObject & Message, const std::shared_ptr<Batch> & InBatch)
  {
    QString Method = Message.value(QStringLiteral("method")).toString();
    QJsonValue Id = Message.value(QStringLiteral("id"));
    QJsonObject Params = Message.value(QStringLiteral("params")).toObject();
    if (Method == QStringLiteral("notifications/cancelled"))
    {
      Cancel(Params.value(QStringLiteral("requestId")));
      return;
    }
    if (!Message.contains(QStringLiteral("id")) || Method.isEmpty())
    {
      // other notifications (initialized) and responses need no answer
      return;
    }
    if (Method == QStringLiteral("initialize"))
    {
      QString Wanted = Params.value(QStringLiteral("protocolVersion")).toString();
      Reply(ResultMessage(Id, QJsonObject{
        {QStringLiteral("protocolVersion"), ProtocolVersions.contains(Wanted) ? Wanted : ProtocolVersions.first()},
        {QStringLiteral("capabilities"), QJsonObject{{QStringLiteral("tools"), QJsonObject{{QStringLiteral("listChanged"), false}}}}},
        {QStringLiteral("serverInfo"), QJsonObject{{QStringLiteral("name"), QStringLiteral("nixscp")},
          {QStringLiteral("version"), QCoreApplication::applicationVersion()}}},
        {QStringLiteral("instructions"), QStringLiteral("NixSCP gives you the user's SSH servers without their passwords or keys. "
          "Call list_sites first and use site names exactly as listed. Some sites restrict commands or need the user's "
          "approval for each request (the call then waits).")},
      }), InBatch);
    }
    else if (Method == QStringLiteral("ping"))
    {
      Reply(ResultMessage(Id, QJsonObject()), InBatch);
    }
    else if (Method == QStringLiteral("tools/list"))
    {
      Reply(ResultMessage(Id, QJsonObject{{QStringLiteral("tools"), Tools()}}), InBatch);
    }
    else if (Method == QStringLiteral("tools/call"))
    {
      FCalls.append(Call{Id, Params.value(QStringLiteral("name")).toString(), Params.value(QStringLiteral("arguments")).toObject(), InBatch});
      if (InBatch != nullptr)
      {
        InBatch->Waiting++;
      }
    }
    else
    {
      Reply(ErrorMessage(Id, -32601, QStringLiteral("Method not found: %1").arg(Method)), InBatch);
    }
  }

  void RunCalls()
  {
    if (FBusy)
    {
      // the running call goes on to the next one
      return;
    }
    FBusy = true;
    while (!FCalls.isEmpty())
    {
      Call Next = FCalls.takeFirst();
      FRunningId = Next.Id;
      FCancelled = false;
      QJsonObject Result = CallTool(Next.Tool, Next.Arguments);
      if (!FCancelled)
      {
        Reply(Result.isEmpty() ? ErrorMessage(Next.Id, -32602, QStringLiteral("Unknown tool: %1").arg(Next.Tool)) : ResultMessage(Next.Id, Result),
          Next.InBatch);
      }
      if (Next.InBatch != nullptr)
      {
        Next.InBatch->Waiting--;
        FlushBatch(Next.InBatch);
      }
    }
    FBusy = false;
    ExitWhenDone();
  }
};

}

namespace AgentMcp
{

int Serve()
{
  Server Instance;
  return Instance.Run();
}

}
