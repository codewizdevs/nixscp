#include "gui/AgentBroker.h"
#include "gui/GuiUserInterface.h"

#include "core/AgentAccess.h"
#include "core/CloudSync.h"
#include "core/ConfigVault.h"
#include "core/Configuration.h"
#include "core/FileSystem.h"
#include "core/PortForward.h"
#include "core/SecureShell.h"
#include "core/Security.h"
#include "core/SessionData.h"

#include <QApplication>
#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMessageBox>
#include <QMutex>
#include <QPointer>
#include <QPushButton>
#include <QThread>
#include <QWaitCondition>

#include <libssh/libssh.h>
#include <libssh/sftp.h>

#include <atomic>
#include <fcntl.h>
#include <memory>

#ifndef Q_OS_WIN
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace AgentAccess;

namespace
{

constexpr int MaxJobs = 8;
// Output waiting for the client before the worker stops reading from the server
constexpr qint64 MaxPendingOutput = 8 * 1024 * 1024;
constexpr int ChunkSize = 64 * 1024;


std::atomic<int> GActiveJobs{0};

// Input from the client to a running job (thread-safe)
struct JobInput
{
  QMutex Mutex;
  QWaitCondition Changed;
  QByteArray Data;           // stdin (exec/shell) or file data (put)
  bool End = false;          // frStdinEof / frDataEnd
  bool Cancelled = false;    // client gone
  bool ResizePending = false;
  int Cols = 80;
  int Rows = 24;
  std::atomic<qint64> PendingOutput{0};

  bool IsCancelled()
  {
    QMutexLocker Lock(&Mutex);
    return Cancelled;
  }
};

struct Request
{
  QString Op;                // list, exec, shell, get, put
  QString Site;
  QString Command;
  QString Term;
  QString Remote;            // get: remote file; put: remote file or folder (may be empty)
  QString Name;              // put: local file name
  qint64 Size = -1;          // put
  int Cols = 80;
  int Rows = 24;

  QString Forward;           // tunnel: "<local port>:<host>:<port>" or a saved forward's name

  QString Detail() const
  {
    if (Op == QStringLiteral("exec")) return Command;
    if (Op == QStringLiteral("tunnel")) return Forward;
    if (Op == QStringLiteral("get")) return Remote;
    if (Op == QStringLiteral("put")) return Name + QStringLiteral(" -> ") + (Remote.isEmpty() ? QStringLiteral("~") : Remote);
    return QString();
  }
  int Permission() const
  {
    if (Op == QStringLiteral("tunnel")) return apTunnels;
    return ((Op == QStringLiteral("get")) || (Op == QStringLiteral("put"))) ? apTransfer : apCommands;
  }
};

// A site in the audit log: its name, and the name agents use when different
QString LogName(const SessionData & Data)
{
  QString Agent = AgentSiteName(Data);
  return (Agent == Data.Name) ? Data.Name : QStringLiteral("%1 [agent name: %2]").arg(Data.Name, Agent);
}

// Never asks the person at the computer anything except a key passphrase:
// unknown host keys are refused (verify the server in NixSCP first)
class AgentUserInterface : public UserInterface
{
public:
  explicit AgentUserInterface(UserInterface * Gui) : FGui(Gui) {}

  QString Problem;

  HostKeyAnswer VerifyHostKey(const QString & Host, int, const QString &, const QString & FingerprintSHA256,
    const QString &, HostKeyState State) override
  {
    Problem = (State == HostKeyChanged) ?
      QStringLiteral("The host key of %1 has CHANGED (now %2). Agent access refuses it; connect to the site in NixSCP to check and accept the new key.")
        .arg(Host, FingerprintSHA256) :
      QStringLiteral("The host key of %1 is not known yet. Connect to the site once in NixSCP to verify and accept it.").arg(Host);
    return HostKeyCancel;
  }

  bool PromptUser(PromptKind Kind, const QString & Name, const QString & Instructions, QList<Prompt> & Prompts) override
  {
    if ((Kind == pkPassphrase) && (FGui != nullptr))
    {
      // the user unlocks the key in NixSCP; the passphrase stays in this process
      return FGui->PromptUser(Kind, Name, Instructions, Prompts);
    }
    Problem = (Kind == pkPassword) ?
      QStringLiteral("The site has no stored password, or the stored password was refused. Store the password for the site in NixSCP.") :
      QStringLiteral("The server asks for interactive authentication, which agent access does not support.");
    return false;
  }

  void DisplayBanner(const QString &, const QString &) override {}
  unsigned int QueryUser(const QString &, const QString &, unsigned int Answers) override
  {
    return (Answers & qaCancel) ? qaCancel : ((Answers & qaNo) ? qaNo : qaAbort);
  }

private:
  UserInterface * FGui;
};

// Runs one request on a worker thread
class AgentJob : public QObject
{
  Q_OBJECT

public:
  AgentJob(const SessionData & Data, const Request & Req, std::shared_ptr<JobInput> Input, UserInterface * Gui) :
    FData(Data), FRequest(Req), FInput(std::move(Input)), FUI(Gui)
  {
  }

signals:
  void Output(quint8 Type, const QByteArray & Payload);
  void Finished(int ExitCode, const QString & Error);

public slots:
  void Run()
  {
    GActiveJobs++;
    FStarted = QDateTime::currentDateTimeUtc();
    int Code = 0;
    QString Error;
    try
    {
      SecureShell Shell(FData, &FUI);
      try
      {
        Shell.Open();
      }
      catch (const ExtException & E)
      {
        if (!FUI.Problem.isEmpty())
        {
          throw ExtException(FUI.Problem);
        }
        // with the reason ("Connection refused"...) and where NixSCP connected to
        QString Message = E.Message();
        if (!E.MoreMessages().trimmed().isEmpty())
        {
          Message += QLatin1Char(' ') + E.MoreMessages().trimmed().replace(QLatin1Char('\n'), QLatin1Char(' '));
        }
        QString Target = QStringLiteral("%1:%2").arg(FData.HostName).arg(FData.PortNumber);
        if (FData.Tunnel && !FData.TunnelHostName.isEmpty())
        {
          Target += QStringLiteral(" through %1:%2").arg(FData.TunnelHostName).arg(FData.TunnelPortNumber);
        }
        if (FData.ProxyMethod != pmNone)
        {
          Target += QStringLiteral(" via proxy %1:%2").arg(FData.ProxyHost).arg(FData.ProxyPort);
        }
        throw ExtException(QStringLiteral("%1 (site \"%2\", %3)").arg(Message, FData.Name, Target));
      }
      ssh_session Session = Shell.Session();
      if ((FRequest.Op == QStringLiteral("exec")) || (FRequest.Op == QStringLiteral("shell")))
      {
        Code = RunChannel(Session);
      }
      else if (FRequest.Op == QStringLiteral("get"))
      {
        Download(Session);
      }
      else
      {
        Upload(Session);
      }
      Shell.Close();
    }
    catch (const ExtException & E)
    {
      Code = ErrorExitCode;
      Error = E.Message().isEmpty() ? QStringLiteral("The connection failed.") : E.Message();
    }
    if (FData.AgentRecord)
    {
      SaveRecording(FStarted, LogName(FData), FRequest.Op, FRequest.Detail(), FTranscript, FTruncated, FInputBytes, Code, Error);
    }
    GActiveJobs--;
    emit Finished(Code, Error);
  }

private:
  SessionData FData;
  Request FRequest;
  std::shared_ptr<JobInput> FInput;
  AgentUserInterface FUI;

  // transcript (SessionData::AgentRecord)
  QDateTime FStarted;
  QByteArray FTranscript;
  bool FTruncated = false;
  qint64 FInputBytes = 0;

  void Record(const QByteArray & Data)
  {
    if (!FData.AgentRecord || FTruncated)
    {
      return;
    }
    qint64 Room = MaxRecordingOutput - FTranscript.size();
    FTranscript.append(Data.left(qMax<qint64>(Room, 0)));
    FTruncated = (Data.size() > Room);
  }

  void Send(TFrame Type, const QByteArray & Payload)
  {
    if ((Type == frStdout) || (Type == frStderr))
    {
      Record(Payload);
    }
    FInput->PendingOutput += Payload.size() + 5;
    emit Output(Type, Payload);
  }

  // Waits while the client has not taken the output yet
  void Throttle()
  {
    while ((FInput->PendingOutput > MaxPendingOutput) && !FInput->IsCancelled())
    {
      QThread::msleep(5);
    }
  }

  [[noreturn]] void SessionError(ssh_session Session, const QString & What)
  {
    throw ExtException(QStringLiteral("%1: %2").arg(What, QString::fromUtf8(ssh_get_error(Session))));
  }

  int RunChannel(ssh_session Session)
  {
    ssh_channel Channel = ssh_channel_new(Session);
    if ((Channel == nullptr) || (ssh_channel_open_session(Channel) != SSH_OK))
    {
      SessionError(Session, QStringLiteral("Cannot open a session channel"));
    }
    std::unique_ptr<ssh_channel_struct, void (*)(ssh_channel)> Guard(Channel, [](ssh_channel C) { ssh_channel_close(C); ssh_channel_free(C); });
    bool Shell = (FRequest.Op == QStringLiteral("shell"));
    if (Shell)
    {
      QByteArray Term = FRequest.Term.isEmpty() ? QByteArrayLiteral("xterm-256color") : FRequest.Term.toLatin1();
      if (ssh_channel_request_pty_size(Channel, Term.constData(), FRequest.Cols, FRequest.Rows) != SSH_OK)
      {
        SessionError(Session, QStringLiteral("Cannot open a terminal"));
      }
      if (ssh_channel_request_shell(Channel) != SSH_OK)
      {
        SessionError(Session, QStringLiteral("Cannot start the shell"));
      }
    }
    else if (ssh_channel_request_exec(Channel, FRequest.Command.toUtf8().constData()) != SSH_OK)
    {
      SessionError(Session, QStringLiteral("Cannot run the command"));
    }

    bool EofSent = false;
    char Buffer[ChunkSize];
    while (!FInput->IsCancelled())
    {
      // client -> server
      QByteArray In;
      bool End = false;
      bool Resize = false;
      int Cols = 0, Rows = 0;
      {
        QMutexLocker Lock(&FInput->Mutex);
        In.swap(FInput->Data);
        End = FInput->End;
        Resize = FInput->ResizePending;
        FInput->ResizePending = false;
        Cols = FInput->Cols;
        Rows = FInput->Rows;
      }
      FInputBytes += In.size();
      if (!In.isEmpty() && !EofSent && (ssh_channel_write(Channel, In.constData(), static_cast<uint32_t>(In.size())) == SSH_ERROR))
      {
        SessionError(Session, QStringLiteral("Cannot send input"));
      }
      if (End && !EofSent && In.isEmpty())
      {
        ssh_channel_send_eof(Channel);
        EofSent = true;
      }
      if (Resize && Shell)
      {
        ssh_channel_change_pty_size(Channel, Cols, Rows);
      }

      // server -> client
      Throttle();
      bool Any = false;
      for (int IsStderr = 0; IsStderr <= 1; IsStderr++)
      {
        int N = ssh_channel_read_nonblocking(Channel, Buffer, sizeof(Buffer), IsStderr);
        if (N == SSH_ERROR)
        {
          SessionError(Session, QStringLiteral("Connection lost"));
        }
        if (N > 0)
        {
          Send(IsStderr ? frStderr : frStdout, QByteArray(Buffer, N));
          Any = true;
        }
      }
      if (!Any)
      {
        if (ssh_channel_is_eof(Channel) || ssh_channel_is_closed(Channel))
        {
          break;
        }
        int Polled = ssh_channel_poll_timeout(Channel, 20, 0);
        if (Polled == SSH_ERROR)
        {
          SessionError(Session, QStringLiteral("Connection lost"));
        }
      }
    }
    if (FInput->IsCancelled())
    {
      return ErrorExitCode;
    }
    // the exit status follows the end of the output
    uint32_t ExitCode = 0;
    char * Signal = nullptr;
    int CoreDumped = 0;
    for (int Wait = 0; Wait < 500; Wait++)
    {
      int Rc = ssh_channel_get_exit_state(Channel, &ExitCode, &Signal, &CoreDumped);
      if (Rc == SSH_OK)
      {
        break;
      }
      if ((Rc == SSH_ERROR) || ssh_channel_is_closed(Channel))
      {
        break;
      }
      ssh_channel_poll_timeout(Channel, 20, 0);
    }
    if (Signal != nullptr)
    {
      QString Name = QString::fromUtf8(Signal);
      ssh_string_free_char(Signal);
      throw ExtException(QStringLiteral("The remote command was terminated by signal %1.").arg(Name));
    }
    return static_cast<int>(qMin<uint32_t>(ExitCode, 254));
  }

  static QString RemotePath(const QString & Path)
  {
    // relative paths are relative to the home folder (the SFTP default)
    if (Path == QStringLiteral("~")) return QStringLiteral(".");
    if (Path.startsWith(QStringLiteral("~/"))) return Path.mid(2);
    return Path;
  }

  sftp_session OpenSftp(ssh_session Session)
  {
    sftp_session Sftp = sftp_new(Session);
    if ((Sftp == nullptr) || (sftp_init(Sftp) != SSH_OK))
    {
      if (Sftp != nullptr) sftp_free(Sftp);
      SessionError(Session, QStringLiteral("Cannot start SFTP (the server needs SFTP for file transfers)"));
    }
    return Sftp;
  }

  void Download(ssh_session Session)
  {
    sftp_session Sftp = OpenSftp(Session);
    std::unique_ptr<sftp_session_struct, void (*)(sftp_session)> SftpGuard(Sftp, sftp_free);
    QByteArray Path = RemotePath(FRequest.Remote).toUtf8();
    sftp_attributes Attributes = sftp_stat(Sftp, Path.constData());
    if (Attributes == nullptr)
    {
      throw ExtException(QStringLiteral("Cannot read \"%1\": %2").arg(FRequest.Remote, QString::fromUtf8(ssh_get_error(Session))));
    }
    bool Directory = (Attributes->type == SSH_FILEXFER_TYPE_DIRECTORY);
    quint64 Size = Attributes->size;
    sftp_attributes_free(Attributes);
    if (Directory)
    {
      throw ExtException(QStringLiteral("\"%1\" is a folder. Download folders with tar, e.g.: nixscp --connect <site> --cmd \"tar czf - %1\" > folder.tar.gz").arg(FRequest.Remote));
    }
    sftp_file File = sftp_open(Sftp, Path.constData(), O_RDONLY, 0);
    if (File == nullptr)
    {
      throw ExtException(QStringLiteral("Cannot open \"%1\": %2").arg(FRequest.Remote, QString::fromUtf8(ssh_get_error(Session))));
    }
    std::unique_ptr<sftp_file_struct, int (*)(sftp_file)> FileGuard(File, sftp_close);
    Send(frInfo, QJsonDocument(QJsonObject{{QStringLiteral("size"), static_cast<qint64>(Size)}}).toJson(QJsonDocument::Compact));
    char Buffer[ChunkSize];
    while (!FInput->IsCancelled())
    {
      Throttle();
      ssize_t N = sftp_read(File, Buffer, sizeof(Buffer));
      if (N < 0)
      {
        throw ExtException(QStringLiteral("Reading \"%1\" failed: %2").arg(FRequest.Remote, QString::fromUtf8(ssh_get_error(Session))));
      }
      if (N == 0)
      {
        break;
      }
      Send(frData, QByteArray(Buffer, static_cast<int>(N)));
    }
    Send(frDataEnd, QByteArray());
    Record(QStringLiteral("[downloaded %1 bytes]\n").arg(Size).toUtf8());
  }

  void Upload(ssh_session Session)
  {
    sftp_session Sftp = OpenSftp(Session);
    std::unique_ptr<sftp_session_struct, void (*)(sftp_session)> SftpGuard(Sftp, sftp_free);
    QString Target = RemotePath(FRequest.Remote);
    if (Target.isEmpty())
    {
      Target = FRequest.Name;
    }
    else if (sftp_attributes A = sftp_stat(Sftp, Target.toUtf8().constData()))
    {
      if (A->type == SSH_FILEXFER_TYPE_DIRECTORY)
      {
        Target = (Target.endsWith(QLatin1Char('/')) ? Target : Target + QLatin1Char('/')) + FRequest.Name;
      }
      sftp_attributes_free(A);
    }
    sftp_file File = sftp_open(Sftp, Target.toUtf8().constData(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (File == nullptr)
    {
      throw ExtException(QStringLiteral("Cannot create \"%1\": %2").arg(Target, QString::fromUtf8(ssh_get_error(Session))));
    }
    std::unique_ptr<sftp_file_struct, int (*)(sftp_file)> FileGuard(File, sftp_close);
    // ready: the client starts sending
    Send(frInfo, QJsonDocument(QJsonObject{{QStringLiteral("path"), Target}}).toJson(QJsonDocument::Compact));
    qint64 Written = 0;
    for (;;)
    {
      QByteArray Chunk;
      bool End = false;
      {
        QMutexLocker Lock(&FInput->Mutex);
        while (FInput->Data.isEmpty() && !FInput->End && !FInput->Cancelled)
        {
          FInput->Changed.wait(&FInput->Mutex, 200);
        }
        if (FInput->Cancelled)
        {
          throw ExtException(QStringLiteral("Upload cancelled."));
        }
        Chunk.swap(FInput->Data);
        End = FInput->End;
      }
      for (qsizetype Offset = 0; Offset < Chunk.size(); )
      {
        ssize_t N = sftp_write(File, Chunk.constData() + Offset, static_cast<size_t>(qMin<qsizetype>(Chunk.size() - Offset, ChunkSize)));
        if (N < 0)
        {
          throw ExtException(QStringLiteral("Writing \"%1\" failed: %2").arg(Target, QString::fromUtf8(ssh_get_error(Session))));
        }
        Offset += N;
        Written += N;
      }
      if (!Chunk.isEmpty())
      {
        Send(frAck, QByteArray::number(Written));
      }
      if (End)
      {
        break;
      }
    }
    if ((FRequest.Size >= 0) && (Written != FRequest.Size))
    {
      throw ExtException(QStringLiteral("Upload incomplete: %1 of %2 bytes.").arg(Written).arg(FRequest.Size));
    }
    Record(QStringLiteral("[uploaded %1 bytes to %2]\n").arg(Written).arg(Target).toUtf8());
  }
};

class Broker;

// One client connection
class Connection : public QObject
{
  Q_OBJECT

public:
  Connection(Broker * Owner, QLocalSocket * Socket);
  ~Connection() override;

private:
  Broker * FOwner;
  QLocalSocket * FSocket;
  FrameReader FReader;
  bool FHaveRequest = false;
  bool FDone = false;
  Request FRequest;
  // the site as the user knows it (audit log): its name, and the agent name when different
  QString FLogSite;
  std::shared_ptr<JobInput> FInput = std::make_shared<JobInput>();
  QThread * FThread = nullptr;
  QPointer<QMessageBox> FApproval;
  // tunnel requests: the forward runs while the client stays connected
  PortForwardSpec FTunnelSpec;
  std::unique_ptr<AgentUserInterface> FTunnelUI;
  PortForward * FTunnel = nullptr;
  void StartTunnel(const SessionData & Data);

  void Read();
  void Fail(const QString & Message, const QString & AuditResult = QString());
  void Finish(int Code);
  void Authorize();
  void Authorized(const SessionData & Data);
  void SendList();
  void Start(const SessionData & Data);
  bool CheckAccess(std::shared_ptr<SessionData> & Data, QString & Error);
};

class Broker : public QObject
{
  Q_OBJECT

public:
  Broker(QWidget * Window) : FWindow(Window), FGui(new GuiUserInterface(Window))
  {
    FServer = new QLocalServer(this);
    FServer->setSocketOptions(QLocalServer::UserAccessOption);
    QString Name = SocketName();
    if (!FServer->listen(Name))
    {
      // a stale socket of a crashed instance; another running NixSCP answers
      QLocalSocket Probe;
      Probe.connectToServer(Name);
      if (!Probe.waitForConnected(500))
      {
        QLocalServer::removeServer(Name);
        FServer->listen(Name);
      }
    }
    connect(FServer, &QLocalServer::newConnection, this, [this]()
    {
      while (QLocalSocket * Socket = FServer->nextPendingConnection())
      {
        if (!SameUser(Socket))
        {
          Socket->abort();
          Socket->deleteLater();
          continue;
        }
        new Connection(this, Socket);
      }
    });
  }

  QWidget * Window() const { return FWindow; }
  UserInterface * Gui() const { return FGui; }
  bool AllowedUntilNow(const QString & Site) const
  {
    return FAllowedUntil.value(Site) > QDateTime::currentDateTimeUtc();
  }
  void AllowFor(const QString & Site, qint64 Seconds)
  {
    FAllowedUntil.insert(Site, QDateTime::currentDateTimeUtc().addSecs(Seconds));
  }
  int RunningJobs = 0;

private:
  QWidget * FWindow;
  GuiUserInterface * FGui;
  QLocalServer * FServer;
  QHash<QString, QDateTime> FAllowedUntil;

  static bool SameUser(QLocalSocket * Socket)
  {
#if defined(Q_OS_LINUX)
    struct ucred Credentials = {};
    socklen_t Length = sizeof(Credentials);
    if (getsockopt(static_cast<int>(Socket->socketDescriptor()), SOL_SOCKET, SO_PEERCRED, &Credentials, &Length) != 0)
    {
      return false;
    }
    return Credentials.uid == getuid();
#else
    // Windows: the pipe's ACL (UserAccessOption) admits the current user only
    Q_UNUSED(Socket);
    return true;
#endif
  }
};

Connection::Connection(Broker * Owner, QLocalSocket * Socket) : QObject(Owner), FOwner(Owner), FSocket(Socket)
{
  Socket->setParent(this);
  connect(Socket, &QLocalSocket::readyRead, this, &Connection::Read);
  connect(Socket, &QLocalSocket::bytesWritten, this, [this](qint64 Bytes) { FInput->PendingOutput -= Bytes; });
  connect(Socket, &QLocalSocket::disconnected, this, [this]()
  {
    {
      QMutexLocker Lock(&FInput->Mutex);
      FInput->Cancelled = true;
      FInput->Changed.wakeAll();
    }
    if (FApproval)
    {
      FApproval->reject();
    }
    if (FTunnel != nullptr)
    {
      // the client ended the tunnel (Ctrl+C); Stopped deletes this connection
      FTunnel->Stop();
    }
    else if (FThread == nullptr)
    {
      deleteLater();
    }
  });
}

Connection::~Connection()
{
  // before FTunnelUI: the tunnel's thread may still use it
  delete FTunnel;
  FTunnel = nullptr;
  if (FThread != nullptr)
  {
    FThread->quit();
    FThread->wait();
  }
}

void Connection::Read()
{
  FReader.Append(FSocket->readAll());
  quint8 Type = 0;
  QByteArray Payload;
  bool Error = false;
  while (FReader.Next(Type, Payload, Error))
  {
    if (!FHaveRequest)
    {
      if (Type != frRequest)
      {
        Fail(QStringLiteral("Protocol error."));
        return;
      }
      QJsonObject J = QJsonDocument::fromJson(Payload).object();
      FRequest.Op = J.value(QStringLiteral("op")).toString();
      FRequest.Site = J.value(QStringLiteral("site")).toString();
      FRequest.Command = J.value(QStringLiteral("command")).toString();
      FRequest.Term = J.value(QStringLiteral("term")).toString();
      FRequest.Remote = J.value(QStringLiteral("remote")).toString();
      FRequest.Name = J.value(QStringLiteral("name")).toString();
      FRequest.Size = J.value(QStringLiteral("size")).toInteger(-1);
      FRequest.Cols = qBound(10, J.value(QStringLiteral("cols")).toInt(80), 1000);
      FRequest.Rows = qBound(2, J.value(QStringLiteral("rows")).toInt(24), 1000);
      FRequest.Forward = J.value(QStringLiteral("forward")).toString();
      FHaveRequest = true;
      Authorize();
      continue;
    }
    QMutexLocker Lock(&FInput->Mutex);
    switch (Type)
    {
      case frStdin:
      case frData:
        FInput->Data.append(Payload);
        break;
      case frStdinEof:
      case frDataEnd:
        FInput->End = true;
        break;
      case frResize:
      {
        QJsonObject J = QJsonDocument::fromJson(Payload).object();
        FInput->Cols = qBound(10, J.value(QStringLiteral("cols")).toInt(80), 1000);
        FInput->Rows = qBound(2, J.value(QStringLiteral("rows")).toInt(24), 1000);
        FInput->ResizePending = true;
        break;
      }
      default:
        break;
    }
    FInput->Changed.wakeAll();
  }
  if (Error)
  {
    Fail(QStringLiteral("Protocol error."));
  }
}

void Connection::Fail(const QString & Message, const QString & AuditResult)
{
  if (FDone)
  {
    return;
  }
  if (!AuditResult.isEmpty())
  {
    AppendAudit(FLogSite.isEmpty() ? FRequest.Site : FLogSite, FRequest.Op, FRequest.Detail(), AuditResult);
  }
  FDone = true;
  FSocket->write(Frame(frError, Message.toUtf8()));
  FSocket->flush();
  FSocket->disconnectFromServer();
}

void Connection::Finish(int Code)
{
  if (FDone)
  {
    return;
  }
  FDone = true;
  FSocket->write(Frame(frExit, QJsonDocument(QJsonObject{{QStringLiteral("code"), Code}}).toJson(QJsonDocument::Compact)));
  FSocket->flush();
  FSocket->disconnectFromServer();
}

bool Connection::CheckAccess(std::shared_ptr<SessionData> & Data, QString & Error)
{
  // by the name agents see: the site's agent name, else its name
  Data = FindAgentSite(Configuration::Instance().StoredSessions(), FRequest.Site);
  if (Data == nullptr)
  {
    Error = QStringLiteral("Site \"%1\" is not available to agents. Run \"nixscp --list\" for the sites you may use; "
      "the user enables others in NixSCP (site settings: Agent Access...).").arg(FRequest.Site);
    return false;
  }
  FLogSite = LogName(*Data);
  if (!(Data->AgentPermissions & FRequest.Permission()))
  {
    Error = QStringLiteral("Site \"%1\" does not allow %2 for agents.")
      .arg(FRequest.Site, (FRequest.Permission() == apTransfer) ? QStringLiteral("file transfers") : QStringLiteral("commands"));
    return false;
  }
  if ((FRequest.Op == QStringLiteral("put")) && (Data->AgentPermissions & apDownloadOnly))
  {
    Error = QStringLiteral("Site \"%1\" allows downloads only; uploads are not allowed for agents.").arg(FRequest.Site);
    return false;
  }
  if ((FRequest.Op == QStringLiteral("shell")) && HasCommandRules(*Data))
  {
    Error = QStringLiteral("Site \"%1\" restricts the commands agents may run, so an interactive shell is not available. "
      "Run commands with --cmd.").arg(FRequest.Site);
    return false;
  }
  if (FRequest.Op == QStringLiteral("tunnel"))
  {
    // "<local port>:<host>:<port>", or the name of one of the site's saved forwards
    QString ParseError;
    bool Found = false;
    if (FRequest.Forward.contains(QLatin1Char(':')))
    {
      Found = PortForwardSpec::Parse(FRequest.Forward, FTunnelSpec, &ParseError);
    }
    else
    {
      for (const PortForwardSpec & Spec : PortForwardSpec::LoadAll(Data->PortForwards))
      {
        if (Spec.Name.compare(FRequest.Forward, Qt::CaseInsensitive) == 0)
        {
          FTunnelSpec = Spec;
          Found = true;
        }
      }
      ParseError = QStringLiteral("Site \"%1\" has no saved tunnel \"%2\". Give <local port>:<host>:<port> instead.")
        .arg(FRequest.Site, FRequest.Forward);
    }
    if (!Found)
    {
      Error = ParseError;
      return false;
    }
    if (FTunnelSpec.LocalPort < 1024)
    {
      Error = QStringLiteral("Use a local port from 1024 up.");
      return false;
    }
  }
  QString Reason;
  if ((FRequest.Op == QStringLiteral("exec")) && !CommandAllowed(*Data, FRequest.Command, &Reason))
  {
    Error = QStringLiteral("%1 Run \"nixscp --list\" to see what is permitted.").arg(Reason);
    return false;
  }
  return true;
}

void Connection::Authorize()
{
  static const QStringList Ops{QStringLiteral("list"), QStringLiteral("exec"), QStringLiteral("shell"), QStringLiteral("get"),
    QStringLiteral("put"), QStringLiteral("tunnel")};
  if (!Ops.contains(FRequest.Op))
  {
    Fail(QStringLiteral("Unknown request."));
    return;
  }
  // the saved passwords must be encrypted, so that a program cannot simply
  // read them from the configuration file: the NixSCP password (Pro, the whole
  // file) or a master password (passwords only)
  if (!AgentAccess::PasswordsProtected())
  {
    Fail(QStringLiteral("Agent access needs the saved passwords encrypted. In NixSCP, set a master password "
      "(Preferences > Security), or use your NixSCP Pro password, which encrypts the whole configuration."),
      QStringLiteral("denied: passwords not encrypted"));
    return;
  }
  auto Continue = [Guard = QPointer<Connection>(this)]()
  {
    if (Guard == nullptr)
    {
      return;
    }
    if (Guard->FRequest.Op == QStringLiteral("list"))
    {
      Guard->SendList();
      return;
    }
    std::shared_ptr<SessionData> Data;
    QString Error;
    if (!Guard->CheckAccess(Data, Error))
    {
      Guard->Fail(Error, QStringLiteral("denied: not allowed - ") + Error);
      return;
    }
    if ((Data->AgentAccess == amAsk) && !Guard->FOwner->AllowedUntilNow(Data->Name))
    {
      SessionData Copy = *Data;
      auto * Box = new QMessageBox(QMessageBox::Question, QStringLiteral("NixSCP Agent Access"),
        (AgentSiteName(*Data) == Data->Name) ?
          QStringLiteral("A program wants to use the site \"%1\".").arg(Data->Name) :
          QStringLiteral("A program wants to use the site \"%1\" (agents know it as \"%2\").").arg(Data->Name, AgentSiteName(*Data)),
        QMessageBox::NoButton, nullptr);
      // a window of its own (no parent): answering it does not bring the whole
      // NixSCP window to the front; it stays on top so it is not missed
      Box->setWindowFlag(Qt::WindowStaysOnTopHint, true);
      Box->setWindowModality(Qt::NonModal);
      Box->setWindowIcon(QApplication::windowIcon());
      QString What = (Guard->FRequest.Op == QStringLiteral("exec")) ? QStringLiteral("Run command:\n%1").arg(Guard->FRequest.Command) :
        (Guard->FRequest.Op == QStringLiteral("shell")) ? QStringLiteral("Open an interactive shell.") :
        (Guard->FRequest.Op == QStringLiteral("get")) ? QStringLiteral("Download file:\n%1").arg(Guard->FRequest.Remote) :
        (Guard->FRequest.Op == QStringLiteral("tunnel")) ? QStringLiteral("Open a tunnel:\n127.0.0.1:%1").arg(Guard->FTunnelSpec.Text()) :
        QStringLiteral("Upload file:\n%1").arg(Guard->FRequest.Detail());
      Box->setInformativeText(What);
      Box->setAttribute(Qt::WA_DeleteOnClose);
      QPushButton * Once = Box->addButton(QStringLiteral("Allow once"), QMessageBox::AcceptRole);
      QPushButton * Hour = Box->addButton(QStringLiteral("1 hour"), QMessageBox::AcceptRole);
      QPushButton * Day = Box->addButton(QStringLiteral("24 hours"), QMessageBox::AcceptRole);
      QPushButton * Always = Box->addButton(QStringLiteral("Always"), QMessageBox::AcceptRole);
      Box->addButton(QStringLiteral("Deny"), QMessageBox::RejectRole);
      Once->setToolTip(QStringLiteral("Allow this request only"));
      Hour->setToolTip(QStringLiteral("Allow requests for this site for one hour without asking"));
      Day->setToolTip(QStringLiteral("Allow requests for this site for 24 hours without asking"));
      Always->setToolTip(QStringLiteral("Allow requests for this site without asking from now on "
        "(changeable in the site settings: Agent Access...)"));
      Box->setDefaultButton(Once);
      Guard->FApproval = Box;
      QObject::connect(Box, &QMessageBox::finished, Guard, [Guard, Box, Once, Hour, Day, Always, Copy]()
      {
        QAbstractButton * Clicked = Box->clickedButton();
        if ((Clicked == Once) || (Clicked == Hour) || (Clicked == Day) || (Clicked == Always))
        {
          if (Clicked == Hour)
          {
            Guard->FOwner->AllowFor(Copy.Name, 60 * 60);
          }
          else if (Clicked == Day)
          {
            Guard->FOwner->AllowFor(Copy.Name, 24 * 60 * 60);
          }
          else if (Clicked == Always)
          {
            // stored: the site is allowed without asking from now on
            Configuration & Config = Configuration::Instance();
            if (auto Stored = Config.StoredSessions().FindByName(Copy.Name))
            {
              Stored->AgentAccess = amAllow;
              Config.Save();
            }
            AppendAudit(LogName(Copy), QStringLiteral("settings"), QStringLiteral("approval changed to always allow"), QStringLiteral("by user"));
          }
          Guard->Authorized(Copy);
        }
        else
        {
          Guard->Fail(QStringLiteral("The user denied the request in NixSCP."), QStringLiteral("denied: by user"));
        }
      });
      Box->show();
      Box->raise();
      Box->activateWindow();
      return;
    }
    Guard->Authorized(*Data);
  };
  Continue();
}

void Connection::SendList()
{
  QJsonArray Sites;
  for (const auto & Data : Configuration::Instance().StoredSessions().Sessions())
  {
    if ((Data->AgentAccess == amOff) || !Data->IsSsh() || Data->IsWorkspace)
    {
      continue;
    }
    Sites.append(QJsonObject{
      {QStringLiteral("name"), AgentSiteName(*Data)},
      {QStringLiteral("host"), Data->HostName},
      {QStringLiteral("user"), Data->UserName},
      {QStringLiteral("port"), Data->PortNumber},
      {QStringLiteral("permissions"), PermissionsText(Data->AgentPermissions) +
        (HasCommandRules(*Data) ? QStringLiteral(" (restricted commands)") : QString())},
      {QStringLiteral("approval"), (Data->AgentAccess == amAsk) ? QStringLiteral("ask") : QStringLiteral("none")},
    });
  }
  AppendAudit(QString(), FRequest.Op, QString(), QStringLiteral("allowed"));
  FSocket->write(Frame(frList, QJsonDocument(Sites).toJson(QJsonDocument::Compact)));
  Finish(0);
}

void Connection::Authorized(const SessionData & Data)
{
  if (FDone)
  {
    return;
  }
  if (GActiveJobs >= MaxJobs)
  {
    Fail(QStringLiteral("Too many agent requests are running; try again shortly."), QStringLiteral("denied: busy"));
    return;
  }
  AppendAudit(LogName(Data), FRequest.Op, FRequest.Detail(), QStringLiteral("allowed"));
  Start(Data);
}

void Connection::StartTunnel(const SessionData & Data)
{
  FTunnelUI = std::make_unique<AgentUserInterface>(FOwner->Gui());
  FTunnel = new PortForward(Data, FTunnelSpec, FTunnelUI.get(), this);
  QDateTime Started = QDateTime::currentDateTimeUtc();
  connect(FTunnel, &PortForward::Started, this, [this](bool Ok, const QString &)
  {
    if (Ok && (FSocket->state() == QLocalSocket::ConnectedState))
    {
      FSocket->write(Frame(frInfo, QJsonDocument(QJsonObject{
        {QStringLiteral("listening"), QStringLiteral("127.0.0.1:%1").arg(FTunnelSpec.LocalPort)},
        {QStringLiteral("target"), QStringLiteral("%1:%2").arg(FTunnelSpec.Host).arg(FTunnelSpec.Port)}}).toJson(QJsonDocument::Compact)));
    }
  });
  connect(FTunnel, &PortForward::ConnectionFailed, this, [this](const QString & Error)
  {
    if (FSocket->state() == QLocalSocket::ConnectedState)
    {
      FSocket->write(Frame(frStderr, (QStringLiteral("nixscp: ") + Error + QLatin1Char('\n')).toUtf8()));
    }
  });
  connect(FTunnel, &PortForward::Stopped, this, [this, Data, Started](const QString & AError)
  {
    QString Error = (!AError.isEmpty() && !FTunnelUI->Problem.isEmpty()) ? FTunnelUI->Problem : AError;
    if (Data.AgentRecord)
    {
      SaveRecording(Started, LogName(Data), FRequest.Op, FRequest.Detail(),
        QStringLiteral("[tunnel 127.0.0.1:%1, %2 bytes forwarded]\n").arg(FTunnelSpec.Text()).arg(FTunnel->BytesTransferred()).toUtf8(),
        false, 0, 0, Error);
    }
    if (!Error.isEmpty())
    {
      AppendAudit(LogName(Data), FRequest.Op, FRequest.Detail(), QStringLiteral("failed: ") + Error);
      Fail(Error);
    }
    else
    {
      Finish(0);
    }
    if (FSocket->state() != QLocalSocket::ConnectedState)
    {
      deleteLater();
    }
    else
    {
      connect(FSocket, &QLocalSocket::disconnected, this, &QObject::deleteLater);
    }
  });
  FTunnel->Start();
}

void Connection::Start(const SessionData & Data)
{
  if (FRequest.Op == QStringLiteral("tunnel"))
  {
    StartTunnel(Data);
    return;
  }
  FThread = new QThread(this);
  auto * Job = new AgentJob(Data, FRequest, FInput, FOwner->Gui());
  Job->moveToThread(FThread);
  connect(FThread, &QThread::started, Job, &AgentJob::Run);
  connect(FThread, &QThread::finished, Job, &QObject::deleteLater);
  connect(Job, &AgentJob::Output, this, [this](quint8 Type, const QByteArray & Payload)
  {
    if (FSocket->state() == QLocalSocket::ConnectedState)
    {
      FSocket->write(Frame(static_cast<TFrame>(Type), Payload));
    }
    else
    {
      FInput->PendingOutput -= Payload.size() + 5;
    }
  });
  connect(Job, &AgentJob::Finished, this, [this](int Code, const QString & Error)
  {
    FThread->quit();
    if (!Error.isEmpty())
    {
      AppendAudit(FLogSite.isEmpty() ? FRequest.Site : FLogSite, FRequest.Op, FRequest.Detail(), QStringLiteral("failed: ") + Error);
      Fail(Error);
    }
    else
    {
      Finish(Code);
    }
    if (FSocket->state() != QLocalSocket::ConnectedState)
    {
      deleteLater();
    }
    else
    {
      connect(FSocket, &QLocalSocket::disconnected, this, &QObject::deleteLater);
    }
  });
  FThread->start();
}

QPointer<Broker> GBroker;

}

namespace AgentBroker
{

void Start(QWidget * Window)
{
  if (GBroker == nullptr)
  {
    GBroker = new Broker(Window);
  }
}

int ActiveJobs()
{
  return GActiveJobs;
}

}

#include "AgentBroker.moc"
