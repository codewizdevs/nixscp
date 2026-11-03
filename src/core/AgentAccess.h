// NixSCP Agent Access (free): lets programs such as AI agents use saved SSH
// sites without ever getting their passwords or keys. The running NixSCP
// application is the broker: it holds the decrypted credentials (master
// password required), checks the per-site settings, asks for approval and
// makes the SSH connection itself; "nixscp --connect <site> ..." (gui/
// AgentClient.cpp) only relays standard input/output over a local socket
// that only the current user can open.
//
// Wire format: frames of [type: 1 byte][length: 4 bytes, big endian][payload].
// The client sends a frRequest (JSON) first; then frStdin/frStdinEof/frResize
// (exec/shell) or frData/frDataEnd (put). The broker answers with frStdout/
// frStderr (exec/shell), frInfo (get: size), frData/frDataEnd (get), frAck
// (put: bytes written, flow control), frList (list, JSON) and finally frExit
// (JSON {"code": n}) or frError (UTF-8 message).
#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>

#include <memory>

class SessionData;
class StoredSessionList;

namespace AgentAccess
{
  // SessionData::AgentAccess
  enum TMode { amOff = 0, amAllow = 1, amAsk = 2 };
  // SessionData::AgentPermissions (bits)
  enum TPermission { apCommands = 0x01, apTransfer = 0x02, apTunnels = 0x04, apAll = 0x07,
    // with apTransfer: downloads only (no uploads)
    apDownloadOnly = 0x10 };

  enum TFrame : quint8
  {
    frRequest = 1, frStdin, frStdinEof, frResize, frStdout, frStderr,
    frData, frDataEnd, frAck, frInfo, frList, frExit, frError,
  };
  constexpr int MaxFrameSize = 1024 * 1024;
  // Exit code of nixscp itself when the request fails (as ssh's 255)
  constexpr int ErrorExitCode = 255;

  // QLocalServer name of the broker (a socket path on Linux, private to the user)
  QString SocketName();
  // Audit log of every agent request (allowed or denied)
  QString AuditLogFileName();
  void AppendAudit(const QString & Site, const QString & Operation, const QString & Detail, const QString & Result);

  QString PermissionsText(int Permissions);

  // Transcripts of agent requests (SessionData::AgentRecord): one file per
  // request in RecordingDirectory(), encrypted like the configuration when it
  // is (ConfigVault); the newest 1000 of the last 90 days are kept
  constexpr qint64 MaxRecordingOutput = 4 * 1024 * 1024;
  QString RecordingDirectory();
  void SaveRecording(const QDateTime & Started, const QString & Site, const QString & Operation, const QString & Detail,
    const QByteArray & Output, bool Truncated, qint64 InputBytes, int ExitCode, const QString & Error);

  // Command rules of a site (SessionData::AgentAllow/AgentDeny; both empty:
  // everything is allowed). The command is split into its simple commands (at
  // ; & && || | and new lines, outside quotes); each must match an allowed
  // pattern (when there are any) and none a denied one. With allowed patterns,
  // command substitution and redirections that write files are refused. Rules
  // are a safety net, not a sandbox: an allowed program may still do more.
  bool HasCommandRules(const SessionData & Data);
  bool CommandAllowed(const SessionData & Data, const QString & Command, QString * Reason = nullptr);
  // The commands of the "read-only" preset (inspection only)
  QString ReadOnlyAllowPreset();
  QString ReadOnlyDenyPreset();

  // The name agents see and use for a site: its own agent name, else the site name
  QString AgentSiteName(const SessionData & Data);
  // The stored site agents may use under Name (agent access on, SSH, not a
  // workspace); nullptr when there is none
  std::shared_ptr<SessionData> FindAgentSite(const StoredSessionList & Sites, const QString & Name);

  // The saved passwords are encrypted and available: the configuration is
  // encrypted with the NixSCP password (Pro), or a master password is set and
  // was entered. Agent access needs it (else any program could decode them).
  bool PasswordsProtected();

  // Whether a private key file is protected by a passphrase (false for an
  // unprotected key; true when it cannot be read without one or is not readable)
  bool KeyFileHasPassphrase(const QString & FileName);

  // "nixscp --help": instructions for people and AI agents
  QString HelpText();

  QByteArray Frame(TFrame Type, const QByteArray & Payload = QByteArray());

  // Splits a byte stream into frames
  class FrameReader
  {
  public:
    void Append(const QByteArray & Data) { FBuffer.append(Data); }
    // false: no complete frame yet; Error: the stream is corrupt
    bool Next(quint8 & Type, QByteArray & Payload, bool & Error);
  private:
    QByteArray FBuffer;
  };
}
