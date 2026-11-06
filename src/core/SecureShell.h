// Port of WinSCP source/core/SecureShell.{h,cpp}: the SSH connection
// (connect, proxies, host key verification, banner, authentication, agent
// forwarding, keepalives) and
// the channel I/O used by the shell based protocols, implemented on top of
// libssh instead of WinSCP's PuTTY fork. Shared by SftpFileSystem and
// ScpFileSystem.
#pragma once

#include "core/Sockets.h"
#include "core/FileSystem.h"
#include "core/SessionData.h"

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <functional>
#include <memory>

#include <libssh/libssh.h>

class QTimer;

// Server host key as presented in the key exchange (Server and protocol
// information dialog, /fingerprintscan)
struct SshHostKeyInfo
{
  QString KeyType;                  // e.g. "ssh-ed25519"
  QString SHA256;                   // "SHA256:<base64>"
  QString MD5;                      // "xx:xx:..."
};

class SecureShell
{
public:
  SecureShell(const SessionData & Data, UserInterface * UI);
  ~SecureShell();
  SecureShell(const SecureShell &) = delete;
  SecureShell & operator=(const SecureShell &) = delete;

  // Connects, verifies the host key and authenticates (TSecureShell::Open)
  void Open();
  // Connects and returns the server host key, without verifying it and
  // without authentication (winscp.com /fingerprintscan); closes afterwards
  SshHostKeyInfo ScanHostKey();
  void Close();
  bool Active() const;
  // Forces TCP_NODELAY regardless of SessionData::TcpNoDelay (call before Open)
  void SetTcpNoDelay(bool NoDelay) { FForceNoDelay = NoDelay; }

  ssh_session Session() const { return FSession; }
  // Session settings, with the user name filled in when it was prompted for
  const SessionData & Data() const { return FData; }
  UserInterface * UI() const { return FUI; }
  QString UserName() const { return FData.UserName; }
  // "Server: ...", "SSH protocol version: ...", algorithms, compression,
  // host key algorithm and fingerprints (TSessionInfo / TFileSystemInfo)
  QStringList SessionInfo() const;
  // Host key of the open connection
  const SshHostKeyInfo & HostKey() const { return FHostKey; }

  // Keepalives (TSessionData::PingType/PingInterval). The SSH layer sends
  // SSH_MSG_IGNORE; for ptDummyCommand the file system may install a callback
  // executing a dummy protocol command instead. Runs from a timer on the
  // thread that called Open(), so only between operations of the worker.
  void SetDummyCommandCallback(std::function<void()> Callback) { FDummyCommand = std::move(Callback); }
  void KeepAlive();

  // AgentFwd: asks for agent forwarding on a new session channel (no-op
  // when disabled or no ssh-agent is running); called by SshChannel
  void RequestAgentForwarding(ssh_channel Channel);
  // Number of agent channels the server opened so far (diagnostics, tests)
  int AgentChannelsOpened() const;

  // Install Public Key into Server (TTerminal::UploadPublicKey): appends
  // the key's line to ~/.ssh/authorized_keys over SFTP, creating ~/.ssh
  // (0700) and the file (0600) when missing. Information receives the
  // progress messages. Returns the PUBLIC_KEY_UPLOADED message (with
  // WinSCP's **main instruction** markup). Throws on failure.
  QString InstallPublicKey(const QString & KeyFileName, const std::function<void(const QString &)> & Information = {});
  // Same over a new connection for Data (Login/Site Advanced dialog Tools
  // menu); call from a worker thread, UI prompts as for any connection
  static QString InstallPublicKeyToServer(const SessionData & Data, UserInterface * UI, const QString & KeyFileName,
    const std::function<void(const QString &)> & Information = {});
  // "<type> <base64> <comment>" for a .ppk/OpenSSH key file (Display Public
  // Key, authorized_keys); throws ExtException for unknown files
  static QString PublicKeyLine(const QString & KeyFileName, QString * Comment = nullptr);
  // AddMatchingKeyCertificate: "<key>-cert.pub" / "<key>.pub-aadcert.pub"
  // next to the key file (".ppk" removed), empty when none exists
  static QString MatchingCertificateFile(const QString & KeyFileName);
  // Proxy "Autodetect": environment (all_proxy/https_proxy/http_proxy) or
  // the system proxy; false when none is configured
  static bool AutodetectProxy(TProxyMethod & Method, QString & Host, int & Port);

  // TSecureShell::TimeoutPrompt: no response for the session timeout. Returns
  // when the user wants to wait on, throws FatalException otherwise, on its
  // own after SessionReopenAutoStall (the connection is then reopened).
  void ConfirmStalled();

  [[noreturn]] void SshError(const QString & Message);
  // Throws FatalException when the connection is lost
  void CheckConnection();

  // libssh option values for the algorithm lists of the SSH page
  // (HostKeyOption is empty for the default list: libssh's order is kept)
  static QString CipherOption(const SessionData & Data);
  static QString KexOption(const SessionData & Data);
  static QString HostKeyOption(const SessionData & Data);
  // Command of a "Local" proxy (ProxyLocalCommand with patterns expanded);
  // empty for other proxy types, which are negotiated internally (SOCKS 4/4A,
  // SOCKS 5, HTTP CONNECT, Telnet), and for excluded localhost connections
  static QString ProxyCommandOption(const SessionData & Data);

private:
  SessionData FData;
  UserInterface * FUI;
  ssh_session FSession = nullptr;
  bool FPasswordUsed = false;
  bool FForceNoDelay = false;
  QTimer * FPingTimer = nullptr;
  std::function<void()> FDummyCommand;
  struct JumpData;
  std::unique_ptr<JumpData> FJump;
  struct AgentForwarding;
  std::unique_ptr<AgentForwarding> FAgent;
  SshHostKeyInfo FHostKey;
  // Name/port the host key is looked up under (LogicalHostName)
  QString FKnownHostName;
  int FKnownHostPort = SshPortNumber;

  // "Local" proxy command, run with its stdin and stdout on the returned socket
  // (Windows: left to libssh); stopped with a bounded wait when the session closes
#ifdef Q_OS_WIN
  void StopProxyCommand() {}
#else
  pid_t FProxyCommandPid = 0;
  bool FProxyCommandGroup = false;
  socket_t StartProxyCommand(const QString & Command);
  void StopProxyCommand();
#endif

  void Connect();
  bool TryFtp();
  void ApplyOptions();
  // A socket connected to Target.HostName through the proxy of Target
  Net::Socket OpenProxySocket(SessionData & Target);
  bool PromptProxyCredentials(SessionData & Data);
  void VerifyHostKey();
  void Authenticate();
  void StartKeepAlive();
};

// One SSH channel with WinSCP-like line oriented I/O (TSecureShell::SendLine,
// ReceiveLine, Receive, stderr collection). Used for the SCP shell and for
// commands executed by the console on an SFTP connection.
class SshChannel
{
public:
  explicit SshChannel(SecureShell & Shell);
  ~SshChannel();
  SshChannel(const SshChannel &) = delete;
  SshChannel & operator=(const SshChannel &) = delete;

  // Shell empty: interactive shell without terminal (as WinSCP for SCP),
  // otherwise the command is executed instead of the shell
  void OpenShell(const QString & Shell);
  void OpenExec(const QString & Command);
  void Close();
  bool IsOpen() const;

  void Send(const char * Data, qsizetype Len);
  void Send(const QByteArray & Data) { Send(Data.constData(), Data.size()); }
  void SendLine(const QString & Line);
  void SendNull();
  void SendEof();

  // Blocks until a full line arrives ("\n", "\r\n" stripped). Throws
  // FatalException when the channel closes, AbortException when the
  // cancel progress (if set) is cancelled.
  QString ReceiveLine();
  // Like ReceiveLine, but returns false at the end of the output instead of
  // throwing (an unterminated last line is returned first)
  bool ReceiveLineOrEof(QString & Line);
  // Exactly Len bytes
  void Receive(char * Buffer, qsizetype Len);
  // Waits for the channel to close; returns exit status (-1 unknown)
  int WaitForExitStatus();

  QString StdError() const { return QString::fromUtf8(FStdError); }
  void ClearStdError() { FStdError.clear(); }
  // stderr lines as they arrive (TSecureShell::OnCaptureOutput with cotError)
  void SetStdErrorCallback(std::function<void(const QString &)> Callback) { FOnStdError = std::move(Callback); }
  void FlushStdErrorLine();
  // Collects stderr data arriving within Ms milliseconds
  void WaitForStdError(int Ms);
  // Progress whose cancellation interrupts waiting for data (nullptr = none)
  void SetCancelProgress(const FileOperationProgress * Progress) { FCancelProgress = Progress; }
  // Encoding of commands and lines: UTF-8 (default) or the local 8-bit
  // encoding (SessionData::NotUtf / detected by the SCP shell)
  void SetUtf(bool Utf) { FUtf = Utf; }
  bool Utf() const { return FUtf; }

private:
  SecureShell & FShell;
  ssh_channel FChannel = nullptr;
  QByteArray FInput;
  // Start of the unconsumed part of FInput
  qsizetype FInputPos = 0;
  QByteArray FStdError;
  QByteArray FStdErrorLine;
  std::function<void(const QString &)> FOnStdError;
  const FileOperationProgress * FCancelProgress = nullptr;
  bool FUtf = true;

  void Open();
  void ConsumeInput(qsizetype Count);
  QByteArray Encode(const QString & Str) const;
  QString Decode(const QByteArray & Str) const;
  // Reads more stdout data into FInput; false on EOF
  bool Fill();
  void DrainStdError();
  [[noreturn]] void ChannelClosed();
};

// Implemented by file systems that can execute arbitrary shell commands
// (fcAnyCommand); used by Terminal::AnyCommand for the console.
class AnyCommandFileSystem
{
public:
  virtual ~AnyCommandFileSystem() = default;
  using OutputEvent = std::function<void(const QString & Line, bool StdError)>;
  // Runs Command in Directory (empty = keep); Output is called on the worker
  // thread for each line. Returns the exit code; *NewDirectory receives the
  // working directory after the command (commands may "cd").
  virtual int AnyCommand(const QString & Command, const QString & Directory, OutputEvent Output,
    FileOperationProgress & Progress, QString * NewDirectory) = 0;
};

// Shell quoting as WinSCP DelimitStr/ShellQuoteStr ("..." with $ \ ` " escaped)
QString ShellDelimitStr(const QString & Str);
QString ShellQuoteStr(const QString & Str);
