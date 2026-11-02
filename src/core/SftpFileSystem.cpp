// Port of WinSCP source/core/SftpFileSystem.cpp + source/core/SecureShell.cpp (libssh based)
#include "core/SftpFileSystem.h"
#include "core/CopyParam.h"

#include "core/FtpListing.h"
#include "core/ScpFileSystem.h"
#include "core/SessionLog.h"
#include "core/Terminal.h"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QScopeGuard>
#include <QUtf8StringView>
#include <deque>
#include <optional>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>

// File type bits of SFTP attributes (POSIX values, as sent by the server: they
// describe remote files, whatever the local operating system)
static constexpr unsigned SftpTypeMask = 0170000;
static constexpr unsigned SftpTypeDirectory = 0040000;
static constexpr unsigned SftpTypeSymlink = 0120000;
static constexpr unsigned SftpTypeFile = 0100000;

namespace
{

QString SftpStatusText(int Code)
{
  // TSFTPFileSystem::GotStatusPacket messages
  switch (Code)
  {
    case SSH_FX_OK: return QStringLiteral("Unexpected OK response.");
    case SSH_FX_EOF: return QStringLiteral("Unexpected EOF response.");
    case SSH_FX_NO_SUCH_FILE: return QStringLiteral("No such file or directory.");
    case SSH_FX_PERMISSION_DENIED: return QStringLiteral("Permission denied.");
    case SSH_FX_FAILURE: return QStringLiteral("General failure (server should provide error description).");
    case SSH_FX_BAD_MESSAGE: return QStringLiteral("Bad message (badly formatted packet or protocol incompatibility).");
    case SSH_FX_NO_CONNECTION: return QStringLiteral("No connection.");
    case SSH_FX_CONNECTION_LOST: return QStringLiteral("Connection lost.");
    case SSH_FX_OP_UNSUPPORTED: return QStringLiteral("The server does not support the operation.");
    case SSH_FX_INVALID_HANDLE: return QStringLiteral("Invalid handle.");
    case SSH_FX_NO_SUCH_PATH: return QStringLiteral("The file path does not exist or is invalid.");
    case SSH_FX_FILE_ALREADY_EXISTS: return QStringLiteral("File already exists.");
    case SSH_FX_WRITE_PROTECT: return QStringLiteral("The file is on read-only media, or the media is write protected.");
    case SSH_FX_NO_MEDIA: return QStringLiteral("There is no media available in the drive.");
    default: return QStringLiteral("Unknown status code.");
  }
}

QString SftpErrorDetails(int Code, const QString & ServerMessage)
{
  // SFTP_ERROR_FORMAT3: "%s\nError code: %d\nError message from server%s: %s"
  QString More = QStringLiteral("%1\nError code: %2\nError message from server: %3")
    .arg(SftpStatusText(Code)).arg(Code).arg(ServerMessage);
  if (Code == SSH_FX_FAILURE)
  {
    More += QStringLiteral("\n\nCommon reasons for the Error code 4 are:\n- Renaming a file to a name of already existing file.\n- Creating a directory that already exists.\n- Moving a remote file to a different filesystem (HDD).\n- Uploading a file to a full filesystem (HDD).\n- Exceeding a user disk quota.");
  }
  return More;
}

// SFTP v3 packet types (draft-ietf-secsh-filexfer-02)
enum : quint8
{
  FXP_CLOSE = 4, FXP_LSTAT = 7, FXP_SETSTAT = 9, FXP_OPENDIR = 11, FXP_READDIR = 12,
  FXP_STAT = 17, FXP_RENAME = 18, FXP_READLINK = 19, FXP_SYMLINK = 20,
  FXP_STATUS = 101, FXP_HANDLE = 102, FXP_DATA = 103, FXP_NAME = 104, FXP_ATTRS = 105,
  FXP_EXTENDED = 200, FXP_EXTENDED_REPLY = 201
};

const quint32 ATTR_SIZE = 0x00000001;
const quint32 ATTR_UIDGID = 0x00000002;
const quint32 ATTR_PERMISSIONS = 0x00000004;
const quint32 ATTR_ACMODTIME = 0x00000008;
const quint32 ATTR_EXTENDED = 0x80000000;

// READLINK + STAT pairs in flight while resolving symlinks
const int SymlinkWindow = 64;
// READDIR requests in flight against OpenSSH, when SFTPListingQueue has its default
const int OpenSshListingQueue = 8;
// Largest packet accepted from the server (WinSCP SFTP_MAX_PACKET_LEN)
const quint32 MaxPacketLength = 1024 * 1024 * 16;

const char * const PosixRenameExtension = "posix-rename@openssh.com";

void AddCardinal(QByteArray & Data, quint32 Value)
{
  char Buf[4] = {static_cast<char>(Value >> 24), static_cast<char>(Value >> 16), static_cast<char>(Value >> 8), static_cast<char>(Value)};
  Data.append(Buf, 4);
}

QByteArray SftpString(const QByteArray & Str)
{
  QByteArray Result;
  Result.reserve(4 + Str.size());
  AddCardinal(Result, static_cast<quint32>(Str.size()));
  Result.append(Str);
  return Result;
}

// TSFTPPacket reading
class PacketReader
{
public:
  PacketReader(const QByteArray & Data, int Pos = 0) : FData(Data), FPos(Pos) {}
  int Pos() const { return FPos; }
  bool AtEnd() const { return FPos >= FData.size(); }
  quint32 Cardinal()
  {
    Need(4);
    const uchar * P = reinterpret_cast<const uchar *>(FData.constData()) + FPos;
    FPos += 4;
    return (quint32(P[0]) << 24) | (quint32(P[1]) << 16) | (quint32(P[2]) << 8) | quint32(P[3]);
  }
  quint64 Int64()
  {
    quint64 High = Cardinal();
    return (High << 32) | Cardinal();
  }
  // Pointer into the packet data and length
  std::pair<const char *, int> RawString()
  {
    quint32 Len = Cardinal();
    Need(static_cast<qint64>(Len));
    std::pair<const char *, int> Result(FData.constData() + FPos, static_cast<int>(Len));
    FPos += static_cast<int>(Len);
    return Result;
  }
  QByteArray String()
  {
    auto [Data, Len] = RawString();
    return QByteArray(Data, Len);
  }

private:
  const QByteArray & FData;
  int FPos;
  void Need(qint64 Len) const
  {
    if (FData.size() - FPos < Len)
    {
      throw FatalException(QStringLiteral("Bad message (badly formatted packet or protocol incompatibility)."),
        QStringLiteral("Truncated SFTP packet (%1 B missing).").arg(Len - (FData.size() - FPos)));
    }
  }
};

struct RawAttrs
{
  quint32 Flags = 0;
  quint64 Size = 0;
  quint32 Uid = 0;
  quint32 Gid = 0;
  quint32 Permissions = 0;
  quint32 MTime = 0;
};

RawAttrs ReadAttrs(PacketReader & Reader)
{
  // SFTP v3 ATTRS
  RawAttrs A;
  A.Flags = Reader.Cardinal();
  if (A.Flags & ATTR_SIZE)
  {
    A.Size = Reader.Int64();
  }
  if (A.Flags & ATTR_UIDGID)
  {
    A.Uid = Reader.Cardinal();
    A.Gid = Reader.Cardinal();
  }
  if (A.Flags & ATTR_PERMISSIONS)
  {
    A.Permissions = Reader.Cardinal();
  }
  if (A.Flags & ATTR_ACMODTIME)
  {
    Reader.Cardinal(); // atime
    A.MTime = Reader.Cardinal();
  }
  if (A.Flags & ATTR_EXTENDED)
  {
    quint32 Count = Reader.Cardinal();
    for (quint32 Index = 0; Index < Count; Index++)
    {
      Reader.RawString();
      Reader.RawString();
    }
  }
  return A;
}

}

SftpFileSystem::SftpFileSystem(const SessionData & Data, UserInterface * UI) :
  FData(Data),
  FUI(UI),
  FShell(std::make_unique<SecureShell>(Data, UI))
{
}

SftpFileSystem::~SftpFileSystem()
{
  Close();
}

[[noreturn]] void SftpFileSystem::SshError(const QString & Message)
{
  FShell->SshError(Message);
}

[[noreturn]] void SftpFileSystem::SftpError(const QString & Message)
{
  CheckConnection();
  int Code = sftp_get_error(FSftp);
  QString ServerMessage = QString::fromUtf8(ssh_get_error(FShell->Session()));
  // libssh reports status messages as "SFTP server: <message>"
  const QString ServerPrefix = QStringLiteral("SFTP server: ");
  if (ServerMessage.startsWith(ServerPrefix))
  {
    ServerMessage = ServerMessage.mid(ServerPrefix.size());
  }
  throw ExtException(Message, SftpErrorDetails(Code, ServerMessage));
}

static bool SftpChannelClosed(sftp_session Sftp)
{
  // The SSH connection may survive the sftp-server process (killed, crashed,
  // exited); WinSCP treats the closed channel as a lost connection too.
  return (Sftp != nullptr) &&
    ((Sftp->channel == nullptr) || !ssh_channel_is_open(Sftp->channel) || ssh_channel_is_eof(Sftp->channel));
}

void SftpFileSystem::CheckConnection()
{
  if (!FShell->Active() || SftpChannelClosed(FSftp))
  {
    throw FatalException(QStringLiteral("Connection has been unexpectedly closed."),
      (FShell->Session() != nullptr) ? QString::fromUtf8(ssh_get_error(FShell->Session())) : QString());
  }
}

bool SftpFileSystem::IsOpened() const
{
  if (FFallback)
  {
    return FFallback->IsOpened();
  }
  return (FSftp != nullptr) && FShell->Active() && !SftpChannelClosed(FSftp);
}

bool SftpFileSystem::IsCapable(FSCapability Capability, bool Default) const
{
  if (FFallback)
  {
    // Default is the SFTP answer of the session's protocol (fsSFTP)
    return FFallback->IsCapable(Capability, Terminal::ProtocolCapability(fsSCPonly, Capability));
  }
  // TSFTPFileSystem::IsCapable, the parts depending on the server
  int Version = (FSftp != nullptr) ? sftp_server_version(FSftp) : 3;
  switch (Capability)
  {
    case fcOwnerChanging:
    case fcGroupChanging:
      return (FSftp != nullptr) && (Version <= 3);
    case fcGroupOwnerChangingByID:
      return (Version <= 3);
    case fcRemoteCopy:
      // OpenSSH (since 9.0), ProFTPD/mod_sftp
      return RawCapable() && (SupportsExtension("copy-data") || SupportsExtension("copy-file"));
    case fcHardLink:
      return SupportsExtension("hardlink@openssh.com");
    case fcCheckingSpaceAvailable:
      return SupportsExtension("statvfs@openssh.com") || SupportsExtension("space-available");
    case fcCalculatingChecksum:
      return RawCapable() && (SupportsExtension("check-file") || SupportsExtension("check-file-name"));
    default:
      return Default;
  }
}

QString SftpFileSystem::ProtocolName() const
{
  if (FFallback)
  {
    return FFallback->ProtocolName();
  }
  return QStringLiteral("SFTP-%1").arg((FSftp != nullptr) ? sftp_server_version(FSftp) : 3);
}

QString SftpFileSystem::ServerInfo() const
{
  if (FFallback)
  {
    return FFallback->ServerInfo();
  }
  if (FShell->Session() == nullptr)
  {
    return QString();
  }
  QStringList Lines = FShell->SessionInfo();
  Lines.insert(std::min<qsizetype>(2, Lines.size()),
    QStringLiteral("SFTP protocol version: %1").arg((FSftp != nullptr) ? sftp_server_version(FSftp) : 0));
  return Lines.join(QLatin1Char('\n'));
}

QByteArray SftpFileSystem::Path(const QString & APath) const
{
  // TSFTPPacket::AddPathString with FUtfStrings
  return (FUtfStrings == asOff) ? FtpEncodeLegacy(APath) : APath.toUtf8();
}

QString SftpFileSystem::DecodeString(const char * Data, qsizetype Len)
{
  // TSFTPPacket::GetPathString: with asAuto, UTF-8 is used until the
  // server sends a string that is not valid UTF-8
  QByteArray Bytes = QByteArray::fromRawData(Data, Len);
  if (FUtfStrings == asAuto)
  {
    if (QUtf8StringView(Data, Len).isValidUtf8())
    {
      return QString::fromUtf8(Data, Len);
    }
    FUtfStrings = asOff;
  }
  return (FUtfStrings == asOff) ? FtpDecodeLegacy(Bytes) : QString::fromUtf8(Data, Len);
}

QDateTime SftpFileSystem::ModificationFromUnix(quint32 Time, quint64 Time64) const
{
  if (Time64 != 0)
  {
    return QDateTime::fromSecsSinceEpoch(static_cast<qint64>(Time64));
  }
  // SFTPSignedTSBug: the server sends signed 32-bit timestamps
  qint64 Seconds = FSignedTS ? static_cast<qint64>(static_cast<qint32>(Time)) : static_cast<qint64>(Time);
  return QDateTime::fromSecsSinceEpoch(Seconds);
}

void SftpFileSystem::DirectoryModified(const QString & APath, bool SubDirs, bool ClearChanges)
{
  if (FCache)
  {
    FCache->FileModified(APath, SubDirs, ClearChanges);
  }
}

void SftpFileSystem::Open()
{
  // Pipelined requests stall on delayed ACKs with Nagle's algorithm on
  FShell->SetTcpNoDelay(true);
  FShell->Open();
  FData.UserName = FShell->UserName();
  ssh_session Session = FShell->Session();
  SessionLog::LogEvent(QStringLiteral("Using SFTP protocol."));

  if (FShell->Data().SftpServer.isEmpty())
  {
    FSftp = sftp_new(Session);
  }
  else
  {
    // Environment > SFTP > SFTP server: run the server binary instead of the subsystem
    ssh_channel Channel = ssh_channel_new(Session);
    QByteArray Server = FShell->Data().SftpServer.toUtf8();
    if ((Channel != nullptr) &&
        (ssh_channel_open_session(Channel) == SSH_OK) &&
        (ssh_channel_request_exec(Channel, Server.constData()) == SSH_OK))
    {
      FSftp = sftp_new_channel(Session, Channel);
    }
    if ((FSftp == nullptr) && (Channel != nullptr))
    {
      ssh_channel_free(Channel);
    }
  }
  if ((FSftp == nullptr) || (sftp_init(FSftp) != SSH_OK))
  {
    QString More = QString::fromUtf8(ssh_get_error(Session));
    if (FSftp != nullptr)
    {
      sftp_free(FSftp);
      FSftp = nullptr;
    }
    // "Allow SCP fallback" (fsSFTP, TTerminal::DoOpen with SshFallbackCmd)
    if ((FData.FSProtocol == fsSFTP) && FShell->Active() && OpenScpFallback(More))
    {
      return;
    }
    throw FatalException(QStringLiteral("Cannot initialize SFTP protocol. Is the host running an SFTP server?"), More);
  }

  int Version = sftp_server_version(FSftp);
  SessionLog::LogEvent(QStringLiteral("SFTP version %1 negotiated.").arg(Version));
  // TSFTPFileSystem::DoStartup
  FSignedTS = (Version < 4) && (FData.SFTPBug[sbSignedTS] != asOff);
  switch (FData.NotUtf)
  {
    case asOff:
      FUtfStrings = asOn;
      break;
    case asOn:
      FUtfStrings = asOff;
      break;
    default:
      FUtfStrings = (Version >= 4) ? asOn : asAuto;
      break;
  }
  // SFTPListingQueue (WinSCP keeps 2 READDIRs in flight); OpenSSH processes
  // more READDIRs on one handle in order, so more are used with the default
  FListingQueue = std::max(1, FData.SFTPListingQueue);
  if ((FData.SFTPListingQueue == 2) && (ssh_get_openssh_version(Session) != 0))
  {
    FListingQueue = OpenSshListingQueue;
  }
  if (!FCache && (FData.CacheDirectories || FData.CacheDirectoryChanges))
  {
    FCache = SessionDirectoryCache::Acquire(FData);
  }
  FHomeDirectory = CanonicalizePath(QStringLiteral("."));
}

bool SftpFileSystem::OpenScpFallback(const QString & /*Reason*/)
{
  // The SCP shell runs on the same SSH connection
  FFallback = std::make_unique<ScpFileSystem>(FData, FUI, FShell.get());
  try
  {
    FFallback->Open();
  }
  catch (...)
  {
    FFallback.reset();
    throw;
  }
  FHomeDirectory = FFallback->HomeDirectory();
  return true;
}

void SftpFileSystem::Close()
{
  FCommandChannel.reset();
  if (FFallback)
  {
    FFallback->Close();
    FFallback.reset();
  }
  if (FSftp != nullptr)
  {
    sftp_free(FSftp);
    FSftp = nullptr;
  }
  FRawOut.clear();
  FRawStash.clear();
  FShell->Close();
}

SessionDirectoryCache * SftpFileSystem::DirectoryCache() const
{
  return FFallback ? FFallback->DirectoryCache() : FCache.get();
}

int SftpFileSystem::AnyCommand(const QString & Command, const QString & Directory, OutputEvent Output,
  FileOperationProgress & Progress, QString * NewDirectory)
{
  if (FFallback)
  {
    return FFallback->AnyCommand(Command, Directory, Output, Progress, NewDirectory);
  }
  // WinSCP opens a secondary SCP shell for the console of an SFTP session;
  // here each command runs in its own exec channel of the same connection,
  // in Directory, and reports the working directory it ended in.
  CheckConnection();
  // A command may change anything (TTerminal::AnyCommand: DirectoryModified)
  auto Modified = qScopeGuard([this]() { DirectoryModified(QStringLiteral("/"), true); });
  static const QString Marker = QStringLiteral("NixSCP: this is end-of-file:");
  QString Script;
  if (!Directory.isEmpty())
  {
    Script = QStringLiteral("cd %1 && ").arg(ShellQuoteStr(Directory));
  }
  Script += QStringLiteral("{\n%1\n} ; __nixscp_rc=$? ; printf '\\n%2%s\\n' \"$(pwd)\" ; exit $__nixscp_rc")
    .arg(Command, Marker);
  FCommandChannel = std::make_unique<SshChannel>(*FShell);
  SshChannel & Channel = *FCommandChannel;
  Channel.SetCancelProgress(&Progress);
  Channel.SetStdErrorCallback([&Output](const QString & Line) { if (Output) Output(Line, true); });
  int ExitCode = -1;
  try
  {
    Channel.OpenExec(Script);
    Channel.SendEof();
    QString Line;
    // An empty line is held back: the one directly before the marker comes
    // from the marker's leading newline
    bool HeldEmpty = false;
    while (Channel.ReceiveLineOrEof(Line))
    {
      if (Line.startsWith(Marker))
      {
        if (NewDirectory != nullptr)
        {
          *NewDirectory = Line.mid(Marker.size());
        }
        HeldEmpty = false;
        continue;
      }
      if (HeldEmpty && Output)
      {
        Output(QString(), false);
      }
      HeldEmpty = Line.isEmpty();
      if (!HeldEmpty && Output)
      {
        Output(Line, false);
      }
    }
    if (HeldEmpty && Output)
    {
      Output(QString(), false);
    }
    Channel.FlushStdErrorLine();
    ExitCode = Channel.WaitForExitStatus();
  }
  catch (...)
  {
    FCommandChannel.reset();
    throw;
  }
  FCommandChannel.reset();
  return ExitCode;
}

QString SftpFileSystem::CanonicalizePath(const QString & APath)
{
  if (FFallback)
  {
    return FFallback->CanonicalizePath(APath);
  }
  CheckConnection();
  SessionLog::LogEvent(1, QStringLiteral("Type: SSH_FXP_REALPATH, Path: %1").arg(APath));
  char * Result = sftp_canonicalize_path(FSftp, Path(APath).constData());
  if (Result == nullptr)
  {
    SftpError(QStringLiteral("Cannot get real path for '%1'.").arg(APath));
  }
  QString S = DecodeString(Result, static_cast<qsizetype>(strlen(Result)));
  ssh_string_free_char(Result);
  return S;
}

QString SftpFileSystem::HomeDirectory()
{
  return FHomeDirectory;
}

RemoteFile SftpFileSystem::FileFromAttributes(sftp_attributes Attrs, const QString & /*Directory*/)
{
  RemoteFile File;
  File.FileName = DecodeString(Attrs->name, static_cast<qsizetype>(strlen(Attrs->name)));
  File.Size = static_cast<qint64>(Attrs->size);
  File.Permissions = Attrs->permissions & 07777;
  File.Modification = ModificationFromUnix(Attrs->mtime, Attrs->mtime64);
  File.IsHidden = File.FileName.startsWith(QLatin1Char('.'));
  switch (Attrs->type)
  {
    case SSH_FILEXFER_TYPE_DIRECTORY: File.FileType = RemoteFile::Directory; break;
    case SSH_FILEXFER_TYPE_SYMLINK: File.FileType = RemoteFile::Symlink; break;
    case SSH_FILEXFER_TYPE_REGULAR: File.FileType = RemoteFile::File; break;
    default: File.FileType = RemoteFile::Other; break;
  }
  File.IsDirectory = (File.FileType == RemoteFile::Directory);
  File.IsSymLink = (File.FileType == RemoteFile::Symlink);

  if ((Attrs->owner != nullptr) && (Attrs->group != nullptr))
  {
    File.Owner = QString::fromUtf8(Attrs->owner);
    File.Group = QString::fromUtf8(Attrs->group);
  }
  else if (Attrs->longname != nullptr)
  {
    // OpenSSH "ls -l" style: perms links owner group size ...
    QStringList Parts = QString::fromUtf8(Attrs->longname).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (Parts.size() >= 4)
    {
      File.Owner = Parts[2];
      File.Group = Parts[3];
    }
  }
  if (File.Owner.isEmpty() && (Attrs->flags & SSH_FILEXFER_ATTR_UIDGID))
  {
    File.Owner = QString::number(Attrs->uid);
    File.Group = QString::number(Attrs->gid);
  }
  RememberTokens(File.Owner, File.Group, (Attrs->flags & SSH_FILEXFER_ATTR_UIDGID) != 0, Attrs->uid, Attrs->gid);
  return File;
}

//---------------------------------------------------------------------------
// Raw SFTP v3 requests on the libssh channel (pipelining)

bool SftpFileSystem::RawCapable() const
{
  // Only the v3 packet format is implemented; libssh negotiates v3 anyway
  return (FSftp != nullptr) && (FSftp->version == 3) && (FSftp->channel != nullptr);
}

quint32 SftpFileSystem::RawSend(quint8 Type, const QByteArray & Body)
{
  // The id comes from libssh's counter, so it never collides with libssh's own requests
  quint32 Id = ++FSftp->id_counter;
  SessionLog::LogEvent(2, QStringLiteral("Type: %1, Size: %2, Number: %3").arg(Type).arg(5 + Body.size()).arg(Id));
  AddCardinal(FRawOut, static_cast<quint32>(5 + Body.size()));
  FRawOut.append(static_cast<char>(Type));
  AddCardinal(FRawOut, Id);
  FRawOut.append(Body);
  return Id;
}

void SftpFileSystem::RawFlush()
{
  qsizetype Offset = 0;
  while (Offset < FRawOut.size())
  {
    int Written = ssh_channel_write(FSftp->channel, FRawOut.constData() + Offset,
      static_cast<uint32_t>(std::min<qsizetype>(FRawOut.size() - Offset, 1024 * 1024)));
    if (Written <= 0)
    {
      FRawOut.clear();
      CheckConnection();
      throw FatalException(QStringLiteral("Connection has been unexpectedly closed."),
        QString::fromUtf8(ssh_get_error(FShell->Session())));
    }
    Offset += Written;
  }
  FRawOut.clear();
}

void SftpFileSystem::RawRead(char * Buffer, qsizetype Len)
{
  qsizetype Got = 0;
  while (Got < Len)
  {
    int Read = ssh_channel_read(FSftp->channel, Buffer + Got, static_cast<uint32_t>(Len - Got), 0);
    if (Read < 0)
    {
      throw FatalException(QStringLiteral("Connection has been unexpectedly closed."),
        QString::fromUtf8(ssh_get_error(FShell->Session())));
    }
    if (Read == 0)
    {
      // timeout of the blocking read, or the channel was closed
      CheckConnection();
      continue;
    }
    Got += Read;
  }
}

SftpFileSystem::RawResponse SftpFileSystem::RawReceive(quint32 Id)
{
  auto Stashed = FRawStash.find(Id);
  if (Stashed != FRawStash.end())
  {
    RawResponse Result = Stashed.value();
    FRawStash.erase(Stashed);
    return Result;
  }
  for (;;)
  {
    char LenBuf[4];
    RawRead(LenBuf, 4);
    quint32 Len = (quint32(uchar(LenBuf[0])) << 24) | (quint32(uchar(LenBuf[1])) << 16) |
      (quint32(uchar(LenBuf[2])) << 8) | quint32(uchar(LenBuf[3]));
    if ((Len < 5) || (Len > MaxPacketLength))
    {
      throw FatalException(QStringLiteral("Received too large (%1 B) SFTP packet. Max supported packet size is %2 B.")
        .arg(Len).arg(MaxPacketLength));
    }
    QByteArray Packet(static_cast<qsizetype>(Len), Qt::Uninitialized);
    RawRead(Packet.data(), Len);
    PacketReader Reader(Packet, 1);
    quint32 ResponseId = Reader.Cardinal();
    RawResponse Response{static_cast<quint8>(Packet[0]), Packet.mid(5)};
    if (ResponseId == Id)
    {
      return Response;
    }
    // reply to another of our pipelined requests
    FRawStash.insert(ResponseId, Response);
  }
}

[[noreturn]] void SftpFileSystem::RawStatusError(const RawResponse & Response, const QString & Message)
{
  if (Response.Type != FXP_STATUS)
  {
    // INVALID_RESPONSE
    throw FatalException(Message, QStringLiteral("Received unexpected SFTP packet type %1.").arg(Response.Type));
  }
  PacketReader Reader(Response.Payload);
  int Code = static_cast<int>(Reader.Cardinal());
  QString ServerMessage;
  if (!Reader.AtEnd())
  {
    auto [Data, Len] = Reader.RawString();
    ServerMessage = QString::fromUtf8(Data, Len);
  }
  throw ExtException(Message, SftpErrorDetails(Code, ServerMessage));
}

void SftpFileSystem::RawExpectStatus(quint32 Id, const QString & Message)
{
  RawResponse Response = RawReceive(Id);
  if (Response.Type == FXP_STATUS)
  {
    PacketReader Reader(Response.Payload);
    if (Reader.Cardinal() == SSH_FX_OK)
    {
      return;
    }
  }
  RawStatusError(Response, Message);
}

RemoteFile SftpFileSystem::RawFileFromName(const QByteArray & Payload, int & Pos, const QString & /*Directory*/, bool & Valid)
{
  // One entry of SSH_FXP_NAME (v3: filename, longname, attrs);
  // the same result as FileFromAttributes for libssh's attributes
  PacketReader Reader(Payload, Pos);
  auto [NameData, NameLen] = Reader.RawString();
  auto [LongData, LongLen] = Reader.RawString();
  RawAttrs A = ReadAttrs(Reader);
  Pos = Reader.Pos();
  RemoteFile File;
  Valid = !(((NameLen == 1) && (NameData[0] == '.')) || ((NameLen == 2) && (NameData[0] == '.') && (NameData[1] == '.')));
  if (!Valid)
  {
    return File;
  }
  File.FileName = DecodeString(NameData, NameLen);
  File.Size = static_cast<qint64>(A.Size);
  File.Permissions = A.Permissions & 07777;
  File.Modification = ModificationFromUnix(A.MTime, 0);
  File.IsHidden = (NameData[0] == '.');
  if (A.Flags & ATTR_PERMISSIONS)
  {
    switch (A.Permissions & SftpTypeMask)
    {
      case SftpTypeDirectory: File.FileType = RemoteFile::Directory; break;
      case SftpTypeSymlink: File.FileType = RemoteFile::Symlink; break;
      case SftpTypeFile: File.FileType = RemoteFile::File; break;
      default: File.FileType = RemoteFile::Other; break;
    }
  }
  else
  {
    File.FileType = RemoteFile::Other;
  }
  File.IsDirectory = (File.FileType == RemoteFile::Directory);
  File.IsSymLink = (File.FileType == RemoteFile::Symlink);
  // OpenSSH "ls -l" style longname: perms links owner group size ...
  const char * P = LongData;
  const char * End = LongData + LongLen;
  int Field = 0;
  while ((P < End) && (Field < 4))
  {
    while ((P < End) && (*P == ' '))
    {
      P++;
    }
    const char * Start = P;
    while ((P < End) && (*P != ' '))
    {
      P++;
    }
    if (P > Start)
    {
      if (Field == 2)
      {
        File.Owner = QString::fromUtf8(Start, P - Start);
      }
      else if (Field == 3)
      {
        File.Group = QString::fromUtf8(Start, P - Start);
      }
      Field++;
    }
  }
  if (Field < 4)
  {
    File.Owner.clear();
    File.Group.clear();
  }
  if (File.Owner.isEmpty() && (A.Flags & ATTR_UIDGID))
  {
    File.Owner = QString::number(A.Uid);
    File.Group = QString::number(A.Gid);
  }
  RememberTokens(File.Owner, File.Group, (A.Flags & ATTR_UIDGID) != 0, A.Uid, A.Gid);
  return File;
}

RemoteFileList SftpFileSystem::RawReadDirectory(const QString & Directory)
{
  // TSFTPFileSystem::ReadDirectory, with SFTPListingQueue READDIRs in flight
  const QString ErrorMessage = QStringLiteral("Error listing directory '%1'.").arg(Directory);
  SessionLog::LogEvent(QStringLiteral("Listing directory \"%1\".").arg(Directory));
  SessionLog::LogEvent(1, QStringLiteral("Type: SSH_FXP_OPENDIR, Path: %1").arg(Directory));
  quint32 OpenId = RawSend(FXP_OPENDIR, SftpString(Path(Directory)));
  RawFlush();
  RawResponse Opened = RawReceive(OpenId);
  if (Opened.Type != FXP_HANDLE)
  {
    RawStatusError(Opened, ErrorMessage);
  }
  QByteArray Handle = SftpString(PacketReader(Opened.Payload).String());

  RemoteFileList Result;
  QList<qsizetype> Links;
  std::deque<quint32> Pending;
  std::optional<RawResponse> Error;
  bool Eof = false;
  while (!Eof || !Pending.empty())
  {
    while (!Eof && (Pending.size() < static_cast<size_t>(FListingQueue)))
    {
      Pending.push_back(RawSend(FXP_READDIR, Handle));
    }
    RawFlush();
    RawResponse Response = RawReceive(Pending.front());
    Pending.pop_front();
    if (Response.Type == FXP_NAME)
    {
      if (Eof)
      {
        // a READDIR sent before the end was seen; the server listed it in order
        continue;
      }
      PacketReader Reader(Response.Payload);
      quint32 Count = Reader.Cardinal();
      int Pos = Reader.Pos();
      for (quint32 Index = 0; Index < Count; Index++)
      {
        bool Valid = false;
        RemoteFile File = RawFileFromName(Response.Payload, Pos, Directory, Valid);
        if (Valid)
        {
          if (File.IsSymLink)
          {
            Links.append(Result.size());
          }
          Result.append(std::move(File));
        }
      }
    }
    else
    {
      Eof = true;
      bool IsEof = false;
      if (Response.Type == FXP_STATUS)
      {
        PacketReader Reader(Response.Payload);
        IsEof = (Reader.Cardinal() == SSH_FX_EOF);
      }
      if (!IsEof && !Error)
      {
        Error = Response;
      }
    }
  }
  // CLOSE goes in the same burst as the symlink requests
  quint32 CloseId = RawSend(FXP_CLOSE, Handle);
  if (!Error)
  {
    ResolveSymlinks(Directory, Result, Links);
  }
  RawFlush();
  RawReceive(CloseId);
  if (Error)
  {
    RawStatusError(*Error, ErrorMessage);
  }
  return Result;
}

void SftpFileSystem::ResolveSymlinks(const QString & Directory, RemoteFileList & Files, const QList<qsizetype> & Links)
{
  // TTerminal::ReadSymlink / TSFTPFileSystem::ReadSymlink: READLINK for
  // LinkTo and STAT for the linked file, for all links at once
  if (!FData.ResolveSymlinks || Links.isEmpty())
  {
    return;
  }
  if (!RawCapable())
  {
    for (qsizetype Index : Links)
    {
      RemoteFile & File = Files[Index];
      QByteArray FullPath = Path(UnixPath::Combine(Directory, File.FileName));
      if (char * Target = sftp_readlink(FSftp, FullPath.constData()))
      {
        File.LinkTo = DecodeString(Target, static_cast<qsizetype>(strlen(Target)));
        ssh_string_free_char(Target);
      }
      if (sftp_attributes Linked = sftp_stat(FSftp, FullPath.constData()))
      {
        File.IsDirectory = (Linked->type == SSH_FILEXFER_TYPE_DIRECTORY);
        if (!File.IsDirectory)
        {
          File.Size = static_cast<qint64>(Linked->size);
        }
        sftp_attributes_free(Linked);
      }
    }
    return;
  }
  struct Request
  {
    qsizetype Index;
    quint32 ReadLinkId;
    quint32 StatId;
  };
  std::deque<Request> Pending;
  qsizetype Next = 0;
  while ((Next < Links.size()) || !Pending.empty())
  {
    while ((Next < Links.size()) && (Pending.size() < static_cast<size_t>(SymlinkWindow)))
    {
      qsizetype Index = Links[Next++];
      QByteArray FullPath = SftpString(Path(UnixPath::Combine(Directory, Files[Index].FileName)));
      quint32 ReadLinkId = RawSend(FXP_READLINK, FullPath);
      quint32 StatId = RawSend(FXP_STAT, FullPath);
      Pending.push_back(Request{Index, ReadLinkId, StatId});
    }
    RawFlush();
    Request Req = Pending.front();
    Pending.pop_front();
    RemoteFile & File = Files[Req.Index];
    RawResponse LinkResponse = RawReceive(Req.ReadLinkId);
    if (LinkResponse.Type == FXP_NAME)
    {
      PacketReader Reader(LinkResponse.Payload);
      if (Reader.Cardinal() >= 1)
      {
        auto [Data, Len] = Reader.RawString();
        File.LinkTo = DecodeString(Data, Len);
      }
    }
    RawResponse StatResponse = RawReceive(Req.StatId);
    if (StatResponse.Type == FXP_ATTRS)
    {
      PacketReader Reader(StatResponse.Payload);
      RawAttrs A = ReadAttrs(Reader);
      // broken links stay links to nothing
      File.IsDirectory = (A.Flags & ATTR_PERMISSIONS) && ((A.Permissions & SftpTypeMask) == SftpTypeDirectory);
      if (!File.IsDirectory)
      {
        File.Size = static_cast<qint64>(A.Size);
      }
    }
  }
}

RemoteFileList SftpFileSystem::ReadDirectory(const QString & Directory)
{
  if (FFallback)
  {
    return FFallback->ReadDirectory(Directory);
  }
  CheckConnection();
  if (RawCapable())
  {
    return RawReadDirectory(Directory);
  }
  SessionLog::LogEvent(QStringLiteral("Listing directory \"%1\".").arg(Directory));
  SessionLog::LogEvent(1, QStringLiteral("Type: SSH_FXP_OPENDIR, Path: %1").arg(Directory));
  sftp_dir Dir = sftp_opendir(FSftp, Path(Directory).constData());
  if (Dir == nullptr)
  {
    SftpError(QStringLiteral("Error listing directory '%1'.").arg(Directory));
  }
  RemoteFileList Result;
  QList<qsizetype> Links;
  while (sftp_attributes Attrs = sftp_readdir(FSftp, Dir))
  {
    if ((qstrcmp(Attrs->name, ".") == 0) || (qstrcmp(Attrs->name, "..") == 0))
    {
      // skip
    }
    else if (!IsValidListingName(QString::fromUtf8(Attrs->name)))
    {
      SessionLog::LogEvent(QStringLiteral("Discarding entry \"%1\" with invalid name.").arg(QString::fromUtf8(Attrs->name)));
    }
    else
    {
      Result.append(FileFromAttributes(Attrs, Directory));
      if (Result.last().IsSymLink)
      {
        Links.append(Result.size() - 1);
      }
    }
    sftp_attributes_free(Attrs);
  }
  bool Eof = sftp_dir_eof(Dir);
  sftp_closedir(Dir);
  if (!Eof)
  {
    SftpError(QStringLiteral("Error listing directory '%1'.").arg(Directory));
  }
  ResolveSymlinks(Directory, Result, Links);
  return Result;
}

RemoteFile SftpFileSystem::ReadFile(const QString & APath)
{
  if (FFallback)
  {
    return FFallback->ReadFile(APath);
  }
  CheckConnection();
  SessionLog::LogEvent(1, QStringLiteral("Type: SSH_FXP_LSTAT, Path: %1").arg(APath));
  sftp_attributes Attrs = sftp_lstat(FSftp, Path(APath).constData());
  if (Attrs == nullptr)
  {
    SftpError(QStringLiteral("Cannot get attributes of file '%1'.").arg(APath));
  }
  if (Attrs->name == nullptr)
  {
    // lstat does not return name
    QByteArray Name = Path(UnixPath::ExtractFileName(APath));
    Attrs->name = strdup(Name.constData());
  }
  RemoteFileList Files{FileFromAttributes(Attrs, UnixPath::ExtractFileDir(APath))};
  sftp_attributes_free(Attrs);
  if (Files[0].IsSymLink)
  {
    ResolveSymlinks(UnixPath::ExtractFileDir(APath), Files, {0});
  }
  return Files[0];
}

bool SftpFileSystem::FileExists(const QString & APath, RemoteFile * File)
{
  if (FFallback)
  {
    return FFallback->FileExists(APath, File);
  }
  CheckConnection();
  SessionLog::LogEvent(1, QStringLiteral("Type: SSH_FXP_LSTAT, Path: %1").arg(APath));
  sftp_attributes Attrs = sftp_lstat(FSftp, Path(APath).constData());
  if (Attrs == nullptr)
  {
    int Code = sftp_get_error(FSftp);
    if ((Code == SSH_FX_NO_SUCH_FILE) || (Code == SSH_FX_NO_SUCH_PATH))
    {
      return false;
    }
    SftpError(QStringLiteral("Cannot get attributes of file '%1'.").arg(APath));
  }
  if (File != nullptr)
  {
    if (Attrs->name == nullptr)
    {
      QByteArray Name = Path(UnixPath::ExtractFileName(APath));
      Attrs->name = strdup(Name.constData());
    }
    RemoteFileList Files{FileFromAttributes(Attrs, UnixPath::ExtractFileDir(APath))};
    if (Files[0].IsSymLink)
    {
      ResolveSymlinks(UnixPath::ExtractFileDir(APath), Files, {0});
    }
    *File = Files[0];
  }
  sftp_attributes_free(Attrs);
  return true;
}

void SftpFileSystem::CreateDirectory(const QString & APath, int Mode)
{
  if (FFallback)
  {
    FFallback->CreateDirectory(APath, Mode);
    return;
  }
  CheckConnection();
  auto Modified = qScopeGuard([&]() { DirectoryModified(APath, false); });
  SessionLog::LogEvent(1, QStringLiteral("Type: SSH_FXP_MKDIR, Path: %1").arg(APath));
  if (sftp_mkdir(FSftp, Path(APath).constData(), static_cast<mode_t>(Mode)) != SSH_OK)
  {
    SftpError(QStringLiteral("Error creating folder '%1'.").arg(APath));
  }
}

void SftpFileSystem::DeleteFile(const QString & APath)
{
  if (FFallback)
  {
    FFallback->DeleteFile(APath);
    return;
  }
  CheckConnection();
  auto Modified = qScopeGuard([&]() { DirectoryModified(APath, false, true); });
  SessionLog::LogEvent(1, QStringLiteral("Type: SSH_FXP_REMOVE, Path: %1").arg(APath));
  if (sftp_unlink(FSftp, Path(APath).constData()) != SSH_OK)
  {
    SftpError(QStringLiteral("Error deleting file '%1'.").arg(APath));
  }
}

void SftpFileSystem::RemoveDirectory(const QString & APath)
{
  if (FFallback)
  {
    FFallback->RemoveDirectory(APath);
    return;
  }
  CheckConnection();
  auto Modified = qScopeGuard([&]() { DirectoryModified(APath, true, true); });
  SessionLog::LogEvent(1, QStringLiteral("Type: SSH_FXP_RMDIR, Path: %1").arg(APath));
  if (sftp_rmdir(FSftp, Path(APath).constData()) != SSH_OK)
  {
    SftpError(QStringLiteral("Error deleting file '%1'.").arg(APath));
  }
}

void SftpFileSystem::RenameFile(const QString & From, const QString & To)
{
  if (FFallback)
  {
    FFallback->RenameFile(From, To);
    return;
  }
  CheckConnection();
  auto Modified = qScopeGuard([&]()
  {
    DirectoryModified(From, true, true);
    DirectoryModified(To, true, true);
  });
  QString Message = QStringLiteral("Error renaming file '%1' to '%2'.").arg(UnixPath::ExtractFileName(From), UnixPath::ExtractFileName(To));
  if (!RawCapable())
  {
    SessionLog::LogEvent(1, QStringLiteral("Type: SSH_FXP_RENAME, Path: %1, Target: %2").arg(From, To));
    if (sftp_rename(FSftp, Path(From).constData(), Path(To).constData()) != SSH_OK)
    {
      SftpError(Message);
    }
    return;
  }
  // TSFTPFileSystem::RenameFile: "Use POSIX rename" sends the OpenSSH
  // extension (replaces an existing target), otherwise SSH_FXP_RENAME
  // (fails on an existing target); falls back to SSH_FXP_RENAME when the
  // server does not announce the extension
  bool UsePosixRename = FData.UsePosixRename && (sftp_extension_supported(FSftp, PosixRenameExtension, "1") != 0);
  QByteArray Body;
  if (UsePosixRename)
  {
    Body = SftpString(QByteArray(PosixRenameExtension));
  }
  Body += SftpString(Path(From)) + SftpString(Path(To));
  quint32 Id = RawSend(UsePosixRename ? FXP_EXTENDED : FXP_RENAME, Body);
  RawFlush();
  RawExpectStatus(Id, Message);
}

void SftpFileSystem::ChangeMode(const QString & APath, unsigned int Mode)
{
  if (FFallback)
  {
    FFallback->ChangeMode(APath, Mode);
    return;
  }
  CheckConnection();
  auto Modified = qScopeGuard([&]() { DirectoryModified(APath, false); });
  SessionLog::LogEvent(1, QStringLiteral("Type: SSH_FXP_SETSTAT, Path: %1").arg(APath));
  if (sftp_chmod(FSftp, Path(APath).constData(), static_cast<mode_t>(Mode)) != SSH_OK)
  {
    SftpError(QStringLiteral("Can't change properties of file '%1'.").arg(APath));
  }
}

void SftpFileSystem::SetModificationTime(const QString & APath, const QDateTime & Time)
{
  SetFileProperties(APath, -1, Time);
}

void SftpFileSystem::SetFileProperties(const QString & APath, int Mode, const QDateTime & Time)
{
  if (FFallback)
  {
    if (Mode >= 0)
    {
      FFallback->ChangeMode(APath, static_cast<unsigned int>(Mode));
    }
    if (Time.isValid())
    {
      FFallback->SetModificationTime(APath, Time);
    }
    return;
  }
  if ((Mode < 0) && !Time.isValid())
  {
    return;
  }
  CheckConnection();
  auto Modified = qScopeGuard([&]() { DirectoryModified(APath, false); });
  // One SETSTAT with permissions and times (TSFTPFileSystem::SFTPSource)
  struct sftp_attributes_struct Attr = {};
  if (Mode >= 0)
  {
    Attr.flags |= SSH_FILEXFER_ATTR_PERMISSIONS;
    Attr.permissions = static_cast<uint32_t>(Mode);
  }
  if (Time.isValid())
  {
    Attr.flags |= SSH_FILEXFER_ATTR_ACMODTIME;
    // a negative time is sent as signed 32-bit value (SFTPSignedTSBug)
    Attr.atime = Attr.mtime = static_cast<uint32_t>(Time.toSecsSinceEpoch());
    Attr.atime64 = Attr.mtime64 = static_cast<uint64_t>(Time.toSecsSinceEpoch());
  }
  SessionLog::LogEvent(1, QStringLiteral("Type: SSH_FXP_SETSTAT, Path: %1").arg(APath));
  if (sftp_setstat(FSftp, Path(APath).constData(), &Attr) != SSH_OK)
  {
    if (Time.isValid())
    {
      SftpError(QStringLiteral("Upload of file '%1' was successful, but error occurred while setting the permissions and/or timestamp.").arg(UnixPath::ExtractFileName(APath)));
    }
    SftpError(QStringLiteral("Can't change properties of file '%1'.").arg(APath));
  }
}

void SftpFileSystem::CreateLink(const QString & APath, const QString & Target)
{
  if (FFallback)
  {
    FFallback->CreateLink(APath, Target);
    return;
  }
  CheckConnection();
  auto Modified = qScopeGuard([&]() { DirectoryModified(APath, false); });
  QString Message = QStringLiteral("Error creating link '%1'.").arg(APath);
  TAutoSwitch Bug = FData.SFTPBug[sbSymlink];
  if ((Bug == asAuto) || !RawCapable())
  {
    // libssh detects OpenSSH, which has the arguments reversed
    SessionLog::LogEvent(1, QStringLiteral("Type: SSH_FXP_SYMLINK, Path: %1").arg(APath));
    if (sftp_symlink(FSftp, Path(Target).constData(), Path(APath).constData()) != SSH_OK)
    {
      SftpError(Message);
    }
    return;
  }
  // SFTPSymlinkBug forced on or off (TSFTPFileSystem::CreateLink)
  QByteArray Body = (Bug == asOn) ?
    (SftpString(Path(Target)) + SftpString(Path(APath))) :
    (SftpString(Path(APath)) + SftpString(Path(Target)));
  quint32 Id = RawSend(FXP_SYMLINK, Body);
  RawFlush();
  RawExpectStatus(Id, Message);
}

void SftpFileSystem::Download(const QString & RemotePath, const QString & LocalPath, FileOperationProgress & Progress)
{
  // Plain download; partial file cleanup is up to the caller
  TransferParams Params;
  Params.KeepPartial = true;
  Download(RemotePath, LocalPath, Params, Progress);
}

void SftpFileSystem::Download(const QString & RemotePath, const QString & LocalPath, const TransferParams & Params, FileOperationProgress & Progress)
{
  if (FFallback)
  {
    FFallback->Download(RemotePath, LocalPath, Params, Progress);
    return;
  }
  CheckConnection();
  // As TSFTPFileSystem::Sink: first open source file, not to lose the
  // destination file, if we cannot open the source one in the first place
  SessionLog::LogEvent(1, QStringLiteral("Type: SSH_FXP_OPEN, Path: %1").arg(RemotePath));
  sftp_file File = sftp_open(FSftp, Path(RemotePath).constData(), O_RDONLY, 0);
  if (File == nullptr)
  {
    SftpError(QStringLiteral("Cannot open remote file '%1'.").arg(RemotePath));
  }
  quint64 Offset = static_cast<quint64>(std::max<qint64>(Params.ResumeOffset, 0));
  QFile Local(LocalPath);
  QIODevice::OpenMode OpenMode = (Offset > 0) ? (QIODevice::WriteOnly | QIODevice::Append) : (QIODevice::WriteOnly | QIODevice::Truncate);
  if (!Local.open(OpenMode))
  {
    sftp_close(File);
    throw ExtException(QStringLiteral("Can't create file '%1'.").arg(LocalPath), Local.errorString());
  }
  if (Offset > 0)
  {
    // Partial file must end exactly at the resume offset
    if (!Local.resize(static_cast<qint64>(Offset)) || !Local.seek(static_cast<qint64>(Offset)) ||
        (sftp_seek64(File, Offset) != 0))
    {
      Local.close();
      sftp_close(File);
      throw ExtException(QStringLiteral("Can't write to file '%1'.").arg(LocalPath), Local.errorString());
    }
  }

  sftp_limits_t Limits = sftp_limits(FSftp);
  size_t Chunk = (Limits != nullptr) ? static_cast<size_t>(Limits->max_read_length) : 32768;
  if (Limits != nullptr) sftp_limits_free(Limits);
  Chunk = std::min<size_t>(Chunk, 256 * 1024);
  if (FData.SFTPMaxPacketSize > 0)
  {
    // SFTPMaxPacketSize: the whole DATA packet (length, type, id, data length) must fit
    Chunk = std::min<size_t>(Chunk, static_cast<size_t>(std::max(512, FData.SFTPMaxPacketSize - 13)));
  }
  // SFTPDownloadQueue: read requests in flight
  size_t QueueLength = static_cast<size_t>(std::max(1, FData.SFTPDownloadQueue));

  sftp_attributes Attrs = sftp_fstat(File);
  quint64 Size = (Attrs != nullptr) ? Attrs->size : 0;
  if (Attrs != nullptr) sftp_attributes_free(Attrs);

  struct Request
  {
    sftp_aio Aio;
    size_t Len;
  };
  std::deque<Request> InFlight;
  QByteArray Buffer(static_cast<qsizetype>(Chunk), Qt::Uninitialized);
  // Text mode: EOL of the server to the local one (TSFTPFileSystem::SFTPSink,
  // SFTP 3 has no "newline" extension, the session's EOLType applies)
  TextEOLConverter Converter(EOLToStr(FData.EOLType), LocalEOL);
  bool Convert = Params.AsciiTransfer && !Converter.Identity();
  quint64 Requested = Offset;
  quint64 Received = Offset;
  bool Eof = false;
  auto Drain = [&]()
  {
    while (!InFlight.empty())
    {
      Request Req = InFlight.front();
      InFlight.pop_front();
      sftp_aio_wait_read(&Req.Aio, Buffer.data(), Req.Len);
      sftp_aio_free(Req.Aio);
    }
  };
  try
  {
    while (!Eof)
    {
      Progress.CheckCancel();
      // Fill the pipeline; the request reaching beyond the known size goes
      // in the same burst, so its EOF arrives without an extra round trip;
      // beyond that keep only one request outstanding (the file may have grown).
      // With a speed limit, request only what the current second's budget allows
      // (block for the budget only when nothing is outstanding).
      while ((InFlight.size() < QueueLength) &&
             ((Requested <= Size) || InFlight.empty()))
      {
        size_t Len = Chunk;
        if (Requested < Size)
        {
          // the EOF probe beyond the known size is not counted to the speed limit
          Len = std::min<quint64>(Chunk, Size - Requested);
          // a cancel makes the speed limit pass everything: stop filling the pipeline
          Progress.CheckCancel();
          Len = Progress.AdjustToCPSLimit(static_cast<unsigned long>(Len), InFlight.empty());
          if (Len == 0)
          {
            break;
          }
        }
        sftp_aio Aio = nullptr;
        if (sftp_aio_begin_read(File, Len, &Aio) == SSH_ERROR)
        {
          SftpError(QStringLiteral("Cannot read remote file '%1'.").arg(RemotePath));
        }
        InFlight.push_back(Request{Aio, Len});
        Requested += Len;
      }
      Request Req = InFlight.front();
      InFlight.pop_front();
      ssize_t Read = sftp_aio_wait_read(&Req.Aio, Buffer.data(), Req.Len);
      if (Read == SSH_ERROR)
      {
        SftpError(QStringLiteral("Cannot read remote file '%1'.").arg(RemotePath));
      }
      if (Read == 0)
      {
        Eof = true;
        break;
      }
      if (Convert)
      {
        QByteArray Converted = Converter.Convert(QByteArray::fromRawData(Buffer.constData(), Read));
        if (Local.write(Converted) != Converted.size())
        {
          throw ExtException(QStringLiteral("Can't write to file '%1'.").arg(LocalPath), Local.errorString());
        }
      }
      else if (Local.write(Buffer.constData(), Read) != Read)
      {
        throw ExtException(QStringLiteral("Can't write to file '%1'.").arg(LocalPath), Local.errorString());
      }
      Received += static_cast<quint64>(Read);
      Progress.AddTransferred(Read);
      if (static_cast<size_t>(Read) < Req.Len)
      {
        // Short read: requests already queued assumed full chunks, restart pipeline at current offset
        Drain();
        if (sftp_seek64(File, Received) != 0)
        {
          SftpError(QStringLiteral("Cannot read remote file '%1'.").arg(RemotePath));
        }
        Requested = Received;
      }
    }
    Drain();
    if (Convert)
    {
      QByteArray Rest = Converter.Finish();
      if (Local.write(Rest) != Rest.size())
      {
        throw ExtException(QStringLiteral("Can't write to file '%1'.").arg(LocalPath), Local.errorString());
      }
    }
  }
  catch (...)
  {
    try { Drain(); } catch (...) {}
    sftp_close(File);
    Local.close();
    // As TSFTPFileSystem::Sink: delete the incomplete file, unless the transfer
    // is resumable and something was transferred
    if (!Params.KeepPartial || (Local.size() == 0))
    {
      Local.remove();
    }
    throw;
  }
  sftp_close(File);
  if (!Local.flush())
  {
    Local.close();
    if (!Params.KeepPartial)
    {
      Local.remove();
    }
    throw ExtException(QStringLiteral("Can't write to file '%1'.").arg(LocalPath), Local.errorString());
  }
  Local.close();
}

void SftpFileSystem::Upload(const QString & LocalPath, const QString & RemotePath, int Mode, FileOperationProgress & Progress)
{
  TransferParams Params;
  Upload(LocalPath, RemotePath, Mode, Params, Progress);
}

void SftpFileSystem::Upload(const QString & LocalPath, const QString & RemotePath, int Mode, const TransferParams & Params, FileOperationProgress & Progress)
{
  if (FFallback)
  {
    FFallback->Upload(LocalPath, RemotePath, Mode, Params, Progress);
    return;
  }
  CheckConnection();
  QFile Local(LocalPath);
  if (!Local.open(QIODevice::ReadOnly))
  {
    throw ExtException(QStringLiteral("Can't open file '%1'.").arg(LocalPath), Local.errorString());
  }
  qint64 RemoteOffset = std::max<qint64>(Params.ResumeOffset, 0);
  qint64 LocalPos = RemoteOffset;
  qint64 LocalEnd = Local.size();
  // Text mode conversions (TFileBuffer::Convert with cpRemoveBOM / cpRemoveCtrlZ
  // and the EOL of the server, SessionData::EOLType with SFTP 3)
  if (Params.RemoveBOM && (LocalPos == 0) && (Local.peek(3) == QByteArray("\xEF\xBB\xBF")))
  {
    LocalPos = 3;
  }
  if (Params.RemoveCtrlZ && (LocalEnd > LocalPos))
  {
    char Last = 0;
    if (Local.seek(LocalEnd - 1) && (Local.read(&Last, 1) == 1) && (Last == '\x1A'))
    {
      LocalEnd--;
    }
  }
  if (!Local.seek(LocalPos))
  {
    throw ExtException(QStringLiteral("Can't read file '%1'.").arg(LocalPath), Local.errorString());
  }
  // The removed BOM / ^Z still count to the progress of the local file
  const qint64 Removed = (LocalPos - RemoteOffset) + (Local.size() - LocalEnd);
  auto Modified = qScopeGuard([&]() { DirectoryModified(RemotePath, false); });
  int Flags = O_WRONLY | O_CREAT | ((RemoteOffset > 0) ? 0 : O_TRUNC);
  SessionLog::LogEvent(1, QStringLiteral("Type: SSH_FXP_OPEN, Path: %1").arg(RemotePath));
  sftp_file File = sftp_open(FSftp, Path(RemotePath).constData(), Flags, static_cast<mode_t>(Mode));
  if (File == nullptr)
  {
    SftpError(QStringLiteral("Cannot create remote file '%1'.").arg(RemotePath));
  }
  if ((RemoteOffset > 0) && (sftp_seek64(File, static_cast<uint64_t>(RemoteOffset)) != 0))
  {
    sftp_close(File);
    SftpError(QStringLiteral("Cannot write to remote file '%1'.").arg(RemotePath));
  }

  sftp_limits_t Limits = sftp_limits(FSftp);
  size_t Chunk = (Limits != nullptr) ? static_cast<size_t>(Limits->max_write_length) : 32768;
  if (Limits != nullptr) sftp_limits_free(Limits);
  Chunk = std::min<size_t>(Chunk, 256 * 1024);
  if (FData.SFTPMaxPacketSize > 0)
  {
    // SFTPMaxPacketSize: the whole WRITE packet (length, type, id, handle,
    // offset, data length) must fit (TSFTPFileSystem::UploadBlockSize)
    Chunk = std::min<size_t>(Chunk, static_cast<size_t>(std::max(512, FData.SFTPMaxPacketSize - 300)));
  }
  // SFTPUploadQueue (WinSCP default 64): write requests in flight
  size_t QueueLength = static_cast<size_t>(std::max(1, FData.SFTPUploadQueue));

  // Text mode: local EOL to the server's one; the progress counts the local bytes
  TextEOLConverter Converter(LocalEOL, EOLToStr(FData.EOLType));
  bool Convert = Params.AsciiTransfer && (RemoteOffset == 0) && !Converter.Identity();
  std::deque<std::pair<sftp_aio, qint64>> InFlight;
  QByteArray Buffer(static_cast<qsizetype>(Chunk), Qt::Uninitialized);
  auto BeginWrite = [&](const char * Data, size_t Len, qint64 Counted)
  {
    sftp_aio Aio = nullptr;
    if (sftp_aio_begin_write(File, Data, Len, &Aio) == SSH_ERROR)
    {
      SftpError(QStringLiteral("Cannot write to remote file '%1'.").arg(RemotePath));
    }
    InFlight.push_back({Aio, Counted});
  };
  auto WaitOne = [&]()
  {
    auto [Aio, Len] = InFlight.front();
    InFlight.pop_front();
    if (sftp_aio_wait_write(&Aio) == SSH_ERROR)
    {
      SftpError(QStringLiteral("Cannot write to remote file '%1'.").arg(RemotePath));
    }
    Progress.AddTransferred(Len);
  };
  try
  {
    bool Eof = false;
    while (!Eof || !InFlight.empty())
    {
      Progress.CheckCancel();
      while (!Eof && (InFlight.size() < QueueLength))
      {
        qint64 Want = std::min<qint64>(static_cast<qint64>(Chunk), LocalEnd - LocalPos);
        if (Want <= 0)
        {
          Eof = true;
          if (Convert)
          {
            QByteArray Rest = Converter.Finish();
            if (!Rest.isEmpty())
            {
              BeginWrite(Rest.constData(), static_cast<size_t>(Rest.size()), 0);
            }
          }
          break;
        }
        if (Convert)
        {
          // the converted chunk (each EOL may double, plus a kept back
          // byte) must still fit into one WRITE
          Want = std::min<qint64>(Want, std::max<qint64>(1, (static_cast<qint64>(Chunk) - 2) / 2));
        }
        // a cancel makes the speed limit pass everything: stop filling the pipeline
        Progress.CheckCancel();
        Want = Progress.AdjustToCPSLimit(static_cast<unsigned long>(Want), InFlight.empty());
        if (Want == 0)
        {
          break;
        }
        qint64 Read = Local.read(Buffer.data(), Want);
        if (Read < 0)
        {
          throw ExtException(QStringLiteral("Can't read file '%1'.").arg(LocalPath), Local.errorString());
        }
        if (Read == 0)
        {
          Eof = true;
          break;
        }
        LocalPos += Read;
        if (Convert)
        {
          QByteArray Converted = Converter.Convert(QByteArray::fromRawData(Buffer.constData(), Read));
          if (Converted.isEmpty())
          {
            // the only byte was kept back (first byte of a two byte EOL)
            Progress.AddTransferred(Read);
            continue;
          }
          BeginWrite(Converted.constData(), static_cast<size_t>(Converted.size()), Read);
        }
        else
        {
          BeginWrite(Buffer.constData(), static_cast<size_t>(Read), Read);
        }
      }
      if (!InFlight.empty())
      {
        WaitOne();
      }
    }
  }
  catch (...)
  {
    // Collect outstanding responses, so they do not confuse later requests
    for (auto & [Aio, Len] : InFlight)
    {
      sftp_aio_wait_write(&Aio);
      sftp_aio_free(Aio);
    }
    sftp_close(File);
    // As TSFTPFileSystem::SFTPSource: delete file if transfer was not completed,
    // unless the transfer is resumable (temporary ".filepart" file is kept)
    if (!Params.KeepPartial && IsOpened())
    {
      sftp_unlink(FSftp, Path(RemotePath).constData());
    }
    throw;
  }
  if (sftp_close(File) != SSH_OK)
  {
    SftpError(QStringLiteral("Cannot close remote file '%1'.").arg(RemotePath));
  }
  Progress.AddTransferred(Removed);
}

//---------------------------------------------------------------------------
// Server side operations (TSFTPFileSystem::ChangeFileProperties, CopyFile,
// CreateLink with hard links, SpaceAvailable, CalculateFilesChecksum)

namespace
{

const quint8 FXP_OPEN_PACKET = 3;
const quint32 FXF_READ = 0x00000001;
const quint32 FXF_WRITE = 0x00000002;
const quint32 FXF_CREAT = 0x00000008;
const quint32 FXF_EXCL = 0x00000020;

void AddInt64(QByteArray & Data, quint64 Value)
{
  AddCardinal(Data, static_cast<quint32>(Value >> 32));
  AddCardinal(Data, static_cast<quint32>(Value));
}

// TSFTPFileSystem::RegisterChecksumAlg
const char * const SftpChecksumAlgs[][2] = {
  {"sha-1", "sha1"}, {"sha-224", "sha224"}, {"sha-256", "sha256"}, {"sha-384", "sha384"},
  {"sha-512", "sha512"}, {"md5", "md5"}, {"crc32", "crc32"}};

}

bool SftpFileSystem::SupportsExtension(const char * Name) const
{
  if (FSftp == nullptr)
  {
    return false;
  }
  unsigned int Count = sftp_extensions_get_count(FSftp);
  for (unsigned int Index = 0; Index < Count; Index++)
  {
    const char * ExtName = sftp_extensions_get_name(FSftp, Index);
    if ((ExtName != nullptr) && (strcmp(ExtName, Name) == 0))
    {
      return true;
    }
  }
  return false;
}

void SftpFileSystem::RememberTokens(const QString & Owner, const QString & Group, bool IdsValid, quint32 Uid, quint32 Gid)
{
  if (!IdsValid)
  {
    return;
  }
  if (!Owner.isEmpty() && !FUserIds.contains(Owner))
  {
    FUserIds.insert(Owner, Uid);
  }
  if (!Group.isEmpty() && !FGroupIds.contains(Group))
  {
    FGroupIds.insert(Group, Gid);
  }
}

bool SftpFileSystem::ResolveToken(const QString & AText, const QHash<QString, quint32> & Ids, quint32 & Id) const
{
  // TPropertiesDialog::StoreRemoteToken with poUserGroupByID:
  // "name [id]", "[id]", a known name or a number
  QString Text = AText.trimmed();
  bool Ok = false;
  qsizetype IDStart = Text.lastIndexOf(QLatin1Char('['));
  if ((IDStart >= 0) && Text.endsWith(QLatin1Char(']')))
  {
    Id = Text.mid(IDStart + 1, Text.size() - IDStart - 2).toUInt(&Ok);
    return Ok;
  }
  auto It = Ids.constFind(Text);
  if (It != Ids.constEnd())
  {
    Id = It.value();
    return true;
  }
  Id = Text.toUInt(&Ok);
  return Ok;
}

void SftpFileSystem::ChangeFileProperties(const QString & APath, const RemoteFile & File, const RemoteProperties & Properties)
{
  if (FFallback)
  {
    FFallback->ChangeFileProperties(APath, File, Properties);
    return;
  }
  CheckConnection();
  auto Modified = qScopeGuard([&]() { DirectoryModified(APath, false); });
  const QString Message = QStringLiteral("Can't change properties of file '%1'.").arg(APath);
  struct sftp_attributes_struct Attr = {};
  if (!Properties.Owner.isEmpty() || !Properties.Group.isEmpty())
  {
    // SFTP v3 changes ownership by ID, both IDs at once
    quint32 Uid = 0, Gid = 0;
    if (Properties.Owner.isEmpty() || Properties.Group.isEmpty())
    {
      sftp_attributes Current = sftp_stat(FSftp, Path(APath).constData());
      if (Current == nullptr)
      {
        SftpError(Message);
      }
      Uid = Current->uid;
      Gid = Current->gid;
      sftp_attributes_free(Current);
    }
    // PROPERTIES_UNKNOWN_TOKEN
    const QString UnknownToken = QStringLiteral("Current session allows changing ownership by UID only. "
      "It was not possible to resolve UID from account name \"%1\". Specify UID explicitly instead.");
    if (!Properties.Owner.isEmpty() && !ResolveToken(Properties.Owner, FUserIds, Uid))
    {
      throw ExtException(UnknownToken.arg(Properties.Owner.trimmed()));
    }
    if (!Properties.Group.isEmpty() && !ResolveToken(Properties.Group, FGroupIds, Gid))
    {
      throw ExtException(UnknownToken.arg(Properties.Group.trimmed()));
    }
    Attr.flags |= SSH_FILEXFER_ATTR_UIDGID;
    Attr.uid = Uid;
    Attr.gid = Gid;
  }
  if (Properties.ValidRights)
  {
    Attr.flags |= SSH_FILEXFER_ATTR_PERMISSIONS;
    Attr.permissions = Properties.CombineRights(File.Permissions, File.IsDirectory);
  }
  if (Attr.flags == 0)
  {
    return;
  }
  SessionLog::LogEvent(1, QStringLiteral("Type: SSH_FXP_SETSTAT, Path: %1").arg(APath));
  if (sftp_setstat(FSftp, Path(APath).constData(), &Attr) != SSH_OK)
  {
    SftpError(Message);
  }
}

bool SftpFileSystem::ChangesPropertiesRecursively() const
{
  return FFallback ? FFallback->ChangesPropertiesRecursively() : false;
}

void SftpFileSystem::ChangeOwnerGroup(const QString & APath, const QString & Owner, const QString & Group)
{
  RemoteProperties Properties;
  Properties.Owner = Owner;
  Properties.Group = Group;
  ChangeFileProperties(APath, RemoteFile(), Properties);
}

QByteArray SftpFileSystem::RawOpenHandle(const QString & APath, quint32 Flags, const QString & Message)
{
  QByteArray Body = SftpString(Path(APath));
  AddCardinal(Body, Flags);
  AddCardinal(Body, 0); // no attributes
  quint32 Id = RawSend(FXP_OPEN_PACKET, Body);
  RawFlush();
  RawResponse Response = RawReceive(Id);
  if (Response.Type != FXP_HANDLE)
  {
    RawStatusError(Response, Message);
  }
  PacketReader Reader(Response.Payload);
  return Reader.String();
}

void SftpFileSystem::RawCloseHandle(const QByteArray & Handle)
{
  // called from scope guards: never throws
  try
  {
    if (IsOpened())
    {
      quint32 Id = RawSend(FXP_CLOSE, SftpString(Handle));
      RawFlush();
      RawReceive(Id);
    }
  }
  catch (...)
  {
  }
}

void SftpFileSystem::ShellCopy(const QString & From, const QString & To)
{
  // TSCPFileSystem::CopyFile in the secondary shell: "cp -p -r -f -T",
  // without the GNU -T switch when it fails
  QStringList Output;
  OutputEvent Event = [&Output](const QString & Line, bool) { Output.append(Line); };
  FileOperationProgress Progress;
  QString Args = ShellQuoteStr(From) + QLatin1Char(' ') + ShellQuoteStr(To);
  int ExitCode = AnyCommand(QStringLiteral("cp -p -r -f -T ") + Args, QString(), Event, Progress, nullptr);
  if (ExitCode != 0)
  {
    Output.clear();
    ExitCode = AnyCommand(QStringLiteral("cp -p -r -f ") + Args, QString(), Event, Progress, nullptr);
  }
  if (ExitCode != 0)
  {
    // COPY_FILE_ERROR
    throw ExtException(QStringLiteral("Error copying file '%1' to '%2'.").arg(From, To), Output.join(QLatin1Char('\n')));
  }
}

void SftpFileSystem::CopyFile(const QString & From, const QString & To)
{
  if (FFallback)
  {
    FFallback->CopyFile(From, To);
    return;
  }
  CheckConnection();
  RemoteFile Source = ReadFile(From);
  bool CopyFileExt = SupportsExtension("copy-file");
  bool CopyDataExt = SupportsExtension("copy-data");
  if (Source.IsDirectory || !RawCapable() || (!CopyFileExt && !CopyDataExt))
  {
    ShellCopy(From, To);
    return;
  }
  auto Modified = qScopeGuard([&]() { DirectoryModified(To, false); });
  const QString Message = QStringLiteral("Error copying file '%1' to '%2'.").arg(From, To);
  if (CopyFileExt)
  {
    // draft-ietf-secsh-filexfer-extensions-00 "copy-file" (no overwrite)
    QByteArray Body = SftpString(QByteArrayLiteral("copy-file")) + SftpString(Path(From)) + SftpString(Path(To));
    Body.append('\0');
    quint32 Id = RawSend(FXP_EXTENDED, Body);
    RawFlush();
    RawExpectStatus(Id, Message);
    return;
  }
  // "copy-data" (OpenSSH): source and target handles; the target is created
  // exclusively, as copy-file never overwrites either
  QByteArray SourceHandle = RawOpenHandle(From, FXF_READ, Message);
  auto CloseSource = qScopeGuard([&]() { RawCloseHandle(SourceHandle); });
  QByteArray TargetHandle = RawOpenHandle(To, FXF_WRITE | FXF_CREAT | FXF_EXCL, Message);
  auto CloseTarget = qScopeGuard([&]() { RawCloseHandle(TargetHandle); });
  QByteArray Body = SftpString(QByteArrayLiteral("copy-data")) + SftpString(SourceHandle);
  AddInt64(Body, 0);
  AddInt64(Body, 0); // until EOF
  Body += SftpString(TargetHandle);
  AddInt64(Body, 0);
  quint32 Id = RawSend(FXP_EXTENDED, Body);
  RawFlush();
  RawExpectStatus(Id, Message);
  // permissions and modification time of the source
  QByteArray Attrs;
  AddCardinal(Attrs, ATTR_PERMISSIONS | ATTR_ACMODTIME);
  AddCardinal(Attrs, Source.Permissions & 07777);
  quint32 Time = static_cast<quint32>(Source.Modification.toSecsSinceEpoch());
  AddCardinal(Attrs, Time);
  AddCardinal(Attrs, Time);
  quint32 StatId = RawSend(FXP_SETSTAT, SftpString(Path(To)) + Attrs);
  RawFlush();
  RawExpectStatus(StatId, Message);
}

void SftpFileSystem::CreateHardLink(const QString & APath, const QString & Target)
{
  if (FFallback)
  {
    FFallback->CreateHardLink(APath, Target);
    return;
  }
  CheckConnection();
  auto Modified = qScopeGuard([&]() { DirectoryModified(APath, false); });
  // a relative target is relative to the link's directory, as with "ln"
  QString AbsoluteTarget = Target.startsWith(QLatin1Char('/')) ? Target :
    UnixPath::Combine(UnixPath::ExtractFileDir(APath), Target);
  SessionLog::LogEvent(1, QStringLiteral("Type: SSH_FXP_EXTENDED (hardlink@openssh.com), Path: %1").arg(APath));
  if (sftp_hardlink(FSftp, Path(AbsoluteTarget).constData(), Path(APath).constData()) != SSH_OK)
  {
    SftpError(QStringLiteral("Error creating link '%1'.").arg(APath));
  }
}

SpaceAvailableInfo SftpFileSystem::SpaceAvailable(const QString & APath)
{
  if (FFallback)
  {
    return FFallback->SpaceAvailable(APath);
  }
  CheckConnection();
  SpaceAvailableInfo Result;
  if (SupportsExtension("statvfs@openssh.com"))
  {
    SessionLog::LogEvent(1, QStringLiteral("Type: SSH_FXP_EXTENDED (statvfs@openssh.com), Path: %1").arg(APath));
    sftp_statvfs_t Stat = sftp_statvfs(FSftp, Path(APath).constData());
    if (Stat == nullptr)
    {
      SftpError(QStringLiteral("Error checking space available for path '%1'.").arg(APath));
    }
    qint64 BlockSize = static_cast<qint64>(Stat->f_bsize);
    Result.BytesOnDevice = BlockSize * static_cast<qint64>(Stat->f_blocks);
    Result.UnusedBytesOnDevice = BlockSize * static_cast<qint64>(Stat->f_bfree);
    Result.BytesAvailableToUser = 0;
    Result.UnusedBytesAvailableToUser = BlockSize * static_cast<qint64>(Stat->f_bavail);
    Result.BytesPerAllocationUnit = BlockSize;
    sftp_statvfs_free(Stat);
    return Result;
  }
  if (!RawCapable() || !SupportsExtension("space-available"))
  {
    throw ExtException(QStringLiteral("The server does not support the operation."));
  }
  quint32 Id = RawSend(FXP_EXTENDED, SftpString(QByteArrayLiteral("space-available")) + SftpString(Path(APath)));
  RawFlush();
  RawResponse Response = RawReceive(Id);
  if (Response.Type != FXP_EXTENDED_REPLY)
  {
    RawStatusError(Response, QStringLiteral("Error checking space available for path '%1'.").arg(APath));
  }
  PacketReader Reader(Response.Payload);
  Result.BytesOnDevice = static_cast<qint64>(Reader.Int64());
  Result.UnusedBytesOnDevice = static_cast<qint64>(Reader.Int64());
  Result.BytesAvailableToUser = static_cast<qint64>(Reader.Int64());
  Result.UnusedBytesAvailableToUser = static_cast<qint64>(Reader.Int64());
  if (!Reader.AtEnd())
  {
    Result.BytesPerAllocationUnit = Reader.Cardinal();
  }
  return Result;
}

QStringList SftpFileSystem::ChecksumAlgorithms() const
{
  if (FFallback || !IsCapable(fcCalculatingChecksum, false))
  {
    return QStringList();
  }
  QStringList Result;
  for (const auto & Alg : SftpChecksumAlgs)
  {
    Result << QString::fromLatin1(Alg[0]);
  }
  return Result;
}

QString SftpFileSystem::CalculateFileChecksum(const QString & APath, const QString & Alg)
{
  if (FFallback)
  {
    return FFallback->CalculateFileChecksum(APath, Alg);
  }
  CheckConnection();
  QByteArray SftpAlg = Alg.toLatin1();
  for (const auto & Def : SftpChecksumAlgs)
  {
    if (Alg.compare(QLatin1String(Def[0]), Qt::CaseInsensitive) == 0)
    {
      SftpAlg = QByteArray(Def[1]);
    }
  }
  // TSFTPCalculateFilesChecksumQueue: "check-file-name"
  QByteArray Body = SftpString(QByteArrayLiteral("check-file-name")) + SftpString(Path(APath)) + SftpString(SftpAlg);
  AddInt64(Body, 0); // offset
  AddInt64(Body, 0); // length (0 = till end)
  AddCardinal(Body, 0); // block size (0 = no blocks or "one block")
  quint32 Id = RawSend(FXP_EXTENDED, Body);
  RawFlush();
  RawResponse Response = RawReceive(Id);
  if (Response.Type != FXP_EXTENDED_REPLY)
  {
    RawStatusError(Response, QStringLiteral("Error calculating checksum for file '%1'.").arg(APath));
  }
  PacketReader Reader(Response.Payload);
  Reader.RawString(); // algorithm used
  return Response.Payload.mid(Reader.Pos()).toHex();
}

void SftpFileSystem::LookupUsersGroups(QStringList & Users, QStringList & Groups)
{
  if (FFallback)
  {
    FFallback->LookupUsersGroups(Users, Groups);
    return;
  }
  // SFTP v3 has no user/group listing (owner-group-query@generic-extensions
  // is not supported by OpenSSH): the names seen in listings
  auto Names = [](const QHash<QString, quint32> & Ids)
  {
    QStringList Result;
    for (auto It = Ids.constBegin(); It != Ids.constEnd(); ++It)
    {
      bool Numeric = false;
      It.key().toUInt(&Numeric);
      if (!Numeric)
      {
        Result << It.key();
      }
    }
    Result.sort(Qt::CaseInsensitive);
    return Result;
  };
  Users = Names(FUserIds);
  Groups = Names(FGroupIds);
}
