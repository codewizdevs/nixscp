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
// (exec, put: input taken so far, flow control: a client keeps at most
// InputWindow bytes unacknowledged), frList (list, JSON) and finally frExit
// (JSON {"code": n}) or frError (UTF-8 message).
#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>

#include <memory>

class QFileDevice;
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
  // Input (exec: stdin, put: file data) a client may have unacknowledged, and
  // the most the broker keeps for a job before it treats the client as broken
  constexpr qint64 InputWindow = 4 * 1024 * 1024;
  constexpr qint64 MaxBufferedInput = 2 * InputWindow;
  // Exit code of nixscp itself when the request fails (as ssh's 255)
  constexpr int ErrorExitCode = 255;

  // QLocalServer name of the broker. A socket path on Linux and macOS, always
  // in a folder only the user can write to, as whoever owns the socket gets
  // what clients send: $XDG_RUNTIME_DIR when it is private, else the home folder.
  // On Windows the name of a pipe, which anyone can try to create first.
  QString SocketName();
  // The other end of a connected local socket runs as this user (both
  // directions: clients also check the broker they reached). On Windows: this
  // user created the pipe, so its ACL is ours and admits this user only.
  bool PeerIsSameUser(qintptr Socket);

  // Audit log of every agent request (allowed or denied). Plain text, one line
  // per request with the site, the request and the first 500 characters of the
  // command line or file name. It never holds stored passwords or keys, but a
  // command is logged as it came: a secret typed into it lands in the log.
  // false: it could not be written (a request must not run unlogged).
  QString AuditLogFileName();
  bool AppendAudit(const QString & Site, const QString & Operation, const QString & Detail, const QString & Result);
  // Text from an agent or a site for logs and prompts: control characters, text
  // direction overrides, invisible format characters and lone surrogates become
  // visible escapes
  QString VisibleText(const QString & Text, bool KeepNewLines);
  // Text of a request is checked, shown and logged as a QString, while the server
  // gets its UTF-8. True when that is the same text: valid UTF-16 (toUtf8() drops
  // a lone surrogate without a word, and no regular expression matches around one)
  // that the bytes give back unchanged (fromUtf8() drops a byte order mark at the
  // start). All other text has to be refused: what runs is not what was checked.
  bool ExactUtf8(const QString & Text);

  QString PermissionsText(int Permissions);

  // Transcripts of agent requests (SessionData::AgentRecord): one file per
  // request in RecordingDirectory(), always encrypted with the NixSCP key
  // (ConfigVault), since output can hold anything; the newest 1000 of the last
  // 90 days are kept. Without that key (a master password only) nothing is
  // recorded: CanRecord() is false and SaveRecording() does nothing.
  constexpr qint64 MaxRecordingOutput = 4 * 1024 * 1024;
  QString RecordingDirectory();
  bool CanRecord();
  bool SaveRecording(const QDateTime & Started, const QString & Site, const QString & Operation, const QString & Detail,
    const QByteArray & Output, bool Truncated, qint64 InputBytes, int ExitCode, const QString & Error);

  // Command rules of a site (SessionData::AgentAllow/AgentDeny; both empty:
  // everything is allowed, except a NUL character and text that is not ExactUtf8).
  // The command is read like a POSIX shell reads it and split into its simple
  // commands (at ; & && || | and new lines); with quoting removed, each must match
  // an allowed pattern (when there are any) and none a denied one. Whatever the
  // checker cannot read with certainty is refused, with allowed or denied patterns
  // alike: NUL and control characters, comments, $variables, $(...) and `...`,
  // $'...', here-documents, line continuations, subshells, braces, shell keywords,
  // VAR=value prefixes, unterminated quotes, wildcards in a program name or an
  // option, a program that runs shell text (sh, su, eval, source, env -S, flock
  // -c...) unless an allowed pattern names it, array subscripts a builtin would
  // evaluate, a line too long or complex for the patterns to give an answer. The
  // only way past this is an allowed pattern
  // without wildcards that equals the whole command line: the user has then read
  // it. With allowed patterns, redirections that write files are refused too. An
  // invalid denied pattern blocks every command, as it cannot be known what it
  // should deny.
  //
  // This is a safety net, NOT a security boundary: it checks text, the server
  // runs programs. It cannot stop:
  //  - an allowed program doing more than its name suggests: options nobody
  //    listed, programs that run others (awk, vim, tar --to-command, less...),
  //    git running the hooks and filters a repository configures;
  //  - a denied program under another name (a copy, a symbolic link, a script,
  //    python3 -c ...): denied patterns are a block list of spellings;
  //  - a * in a pattern matches spaces too: "rm *.tmp" also allows
  //    "rm /etc/passwd x.tmp", and "cat /var/log/*" allows "cat /var/log/../x";
  //  - what a wildcard in an argument expands to on the server;
  //  - a login shell that is not POSIX-like (csh, fish), which reads the same
  //    text differently, and a server locale that is not UTF-8: Shift JIS, GBK
  //    or Big5 can take the second byte of a character for a \\ or a |.
  // A site that must not be changed needs a server account without those
  // rights, not command rules.
  bool HasCommandRules(const SessionData & Data);
  bool CommandAllowed(const SessionData & Data, const QString & Command, QString * Reason = nullptr);
  // The first pattern of Rules (AgentAllow/AgentDeny) that is not valid, empty when all are
  QString InvalidPattern(const QString & Rules);
  // The commands of the "read-only" preset (inspection only)
  QString ReadOnlyAllowPreset();
  QString ReadOnlyDenyPreset();

  // The name agents see and use for a site: its own agent name, else the site name
  QString AgentSiteName(const SessionData & Data);
  // Agent access is on (a known mode), SSH, not a workspace
  bool UsableByAgents(const SessionData & Data);
  // The stored site agents may use under Name; nullptr when there is none, or
  // more than one (Ambiguous): an agent must never reach the wrong server
  std::shared_ptr<SessionData> FindAgentSite(const StoredSessionList & Sites, const QString & Name, bool * Ambiguous = nullptr);

  // The saved passwords are encrypted and available: the configuration is
  // encrypted with the NixSCP password (Pro), or a master password is set and
  // was entered. Agent access needs it (else any program could decode them).
  bool PasswordsProtected();

  // With a master password alone only the passwords are encrypted: nixscp.ini
  // is text that any program of the user can edit, an agent included, to raise
  // its own permissions or point a site with a stored password at another
  // server. So the settings a site runs with must carry a seal only the master
  // password can make, in nixscp-agent-seals: a site without a matching seal
  // (new settings, edited settings, a new master password) is not used until the
  // user accepts them in NixSCP (ConfirmSettings). With the NixSCP password the
  // whole file is encrypted and this does not apply. This cannot detect an old
  // settings file put back together with its old seal: settings the user
  // accepted at some time. The seal covers every saved setting of the site, a
  // new one included, except the passwords and what only the panels use.
  bool SettingsConfirmed(const SessionData & Data);
  bool ConfirmSettings(const SessionData & Data);
  // What the user accepts: server, permissions, rules, and any other setting
  // that is not at its default
  QString SettingsText(const SessionData & Data);

  // Whether a private key file is protected by a passphrase (false for an
  // unprotected key; true when it cannot be read without one or is not readable)
  bool KeyFileHasPassphrase(const QString & FileName);

  // "nixscp --help": instructions for people and AI agents
  QString HelpText();

  QByteArray Frame(TFrame Type, const QByteArray & Payload = QByteArray());

  // The local file of a download. A file already there stays as it is until
  // Commit(): a refused or failed download leaves it untouched. Devices and
  // pipes (/dev/stdout) are written directly, they have nothing to preserve.
  class DownloadFile
  {
  public:
    explicit DownloadFile(const QString & FileName);
    ~DownloadFile();
    bool Open();
    bool Write(const QByteArray & Data);
    bool Commit();
    // leaves the file as it was
    void Cancel();
    QString ErrorString() const;
  private:
    QString FFileName;
    std::unique_ptr<QFileDevice> FFile;
  };

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
