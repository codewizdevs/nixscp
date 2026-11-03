#include "gui/AgentMcp.h"

#include "core/AgentAccess.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QTimer>

#include <cstdio>
#include <iostream>
#include <memory>
#include <string>

using namespace AgentAccess;

namespace
{

constexpr int ChunkSize = 64 * 1024;
constexpr qint64 UploadWindow = 4 * 1024 * 1024;
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
  QString Op = Req.value(QStringLiteral("op")).toString();
  std::unique_ptr<QFile> Output, Input;
  if (Op == QStringLiteral("get"))
  {
    Output.reset(new QFile(LocalFile));
    if (!Output->open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
      Result.Error = QStringLiteral("Cannot write \"%1\".").arg(LocalFile);
      return Result;
    }
  }
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
  bool Uploading = false;
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
    while (Uploading && (Sent - Acked < UploadWindow))
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
          if (Output) Output->write(Payload);
          break;
        case frDataEnd:
          if (Output) Output->close();
          break;
        case frInfo:
          if (Op == QStringLiteral("put"))
          {
            Uploading = true;
            SendUpload();
          }
          break;
        case frAck:
          Acked = Payload.toLongLong();
          SendUpload();
          break;
        case frList:
          Result.List = Payload;
          break;
        case frExit:
          Result.Code = QJsonDocument::fromJson(Payload).object().value(QStringLiteral("code")).toInt(0);
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
    if (!Stdin.isEmpty())
    {
      Socket.write(Frame(frStdin, Stdin));
    }
    Socket.write(Frame(frStdinEof));
  }
  if (!Done)
  {
    Loop.exec();
  }
  if (!Result.Error.isEmpty() && Output)
  {
    Output->close();
    Output->remove();
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
      Local = QDir(Local).filePath(RemoteName);
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

void Reply(const QJsonValue & Id, const QJsonObject & Result)
{
  QJsonObject Message{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("id"), Id}, {QStringLiteral("result"), Result}};
  std::fputs(QJsonDocument(Message).toJson(QJsonDocument::Compact).constData(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

void ReplyError(const QJsonValue & Id, int Code, const QString & Message)
{
  QJsonObject Out{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("id"), Id},
    {QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), Code}, {QStringLiteral("message"), Message}}}};
  std::fputs(QJsonDocument(Out).toJson(QJsonDocument::Compact).constData(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

}

namespace AgentMcp
{

int Serve()
{
  std::string Line;
  while (std::getline(std::cin, Line))
  {
    if (Line.find_first_not_of(" \t\r") == std::string::npos)
    {
      continue;
    }
    QJsonParseError ParseError;
    QJsonDocument Doc = QJsonDocument::fromJson(QByteArray::fromStdString(Line), &ParseError);
    if (ParseError.error != QJsonParseError::NoError || !Doc.isObject())
    {
      ReplyError(QJsonValue(), -32700, QStringLiteral("Parse error"));
      continue;
    }
    QJsonObject Message = Doc.object();
    QString Method = Message.value(QStringLiteral("method")).toString();
    QJsonValue Id = Message.value(QStringLiteral("id"));
    bool IsRequest = Message.contains(QStringLiteral("id")) && !Method.isEmpty();
    if (!IsRequest)
    {
      // notifications (initialized, cancelled) and responses need no answer
      continue;
    }
    QJsonObject Params = Message.value(QStringLiteral("params")).toObject();
    if (Method == QStringLiteral("initialize"))
    {
      QString Wanted = Params.value(QStringLiteral("protocolVersion")).toString();
      Reply(Id, QJsonObject{
        {QStringLiteral("protocolVersion"), ProtocolVersions.contains(Wanted) ? Wanted : ProtocolVersions.first()},
        {QStringLiteral("capabilities"), QJsonObject{{QStringLiteral("tools"), QJsonObject{{QStringLiteral("listChanged"), false}}}}},
        {QStringLiteral("serverInfo"), QJsonObject{{QStringLiteral("name"), QStringLiteral("nixscp")},
          {QStringLiteral("version"), QCoreApplication::applicationVersion()}}},
        {QStringLiteral("instructions"), QStringLiteral("NixSCP gives you the user's SSH servers without their passwords or keys. "
          "Call list_sites first and use site names exactly as listed. Some sites restrict commands or need the user's "
          "approval for each request (the call then waits).")},
      });
    }
    else if (Method == QStringLiteral("ping"))
    {
      Reply(Id, QJsonObject());
    }
    else if (Method == QStringLiteral("tools/list"))
    {
      Reply(Id, QJsonObject{{QStringLiteral("tools"), Tools()}});
    }
    else if (Method == QStringLiteral("tools/call"))
    {
      QJsonObject Result = CallTool(Params.value(QStringLiteral("name")).toString(), Params.value(QStringLiteral("arguments")).toObject());
      if (Result.isEmpty())
      {
        ReplyError(Id, -32602, QStringLiteral("Unknown tool: %1").arg(Params.value(QStringLiteral("name")).toString()));
      }
      else
      {
        Reply(Id, Result);
      }
    }
    else
    {
      ReplyError(Id, -32601, QStringLiteral("Method not found: %1").arg(Method));
    }
  }
  return 0;
}

}
