// Port of WinSCP source/core/FtpFileSystem.cpp (TFTPFileSystem) together with
// the protocol engine of source/filezilla/FtpControlSocket.cpp,
// TransferSocket.cpp, AsyncProxySocketLayer.cpp and AsyncSslSocketLayer.cpp,
// implemented as a blocking FTP/FTPS client on top of QSslSocket
// (QtNetwork + OpenSSL) instead of FileZilla's MFC socket layers.
//
// All methods run on the Terminal worker thread and block (waitFor* with
// SessionData::Timeout); cancellation is polled from FileOperationProgress.
//
// Connection: direct, through a SOCKS4A proxy (own implementation of the
// FileZilla CAsyncProxySocketLayer handshake), SOCKS5 or HTTP CONNECT proxy
// (QNetworkProxy), or through an FTP proxy with one of the 8 FileZilla logon
// sequences (FtpProxyLogonType: SITE %host, USER %proxyuser USER %user@%host,
// OPEN %host, ...). Optional HOST command (FtpHost), CLNT/CSID client
// identification and post login commands (PostLoginCommands, sent before the
// working directory is read, as TFTPFileSystem::DoStartup).
//
// TLS: trusted CAs of the system store plus the hidden option CertificateStorage
// (a PEM bundle, WinSCP's replacement for the Windows store); protocol
// versions MinTlsVersion..MaxTlsVersion (OpenSSL MinProtocol/
// MaxProtocol; TLS 1.0/1.1 need OpenSSL security level 0, as WinSCP's
// SetupSsl), client certificate TlsCertificateFile (PKCS#12 or PEM with the key,
// optionally with a .crt/.cer next to it; passphrase prompted, as
// TTerminal::LoadTlsCertificate). The server certificate is verified against
// the system CA store (/etc/ssl/certs, Linux replacement for the Windows
// certificate store) plus host name; certificates that do not verify (or any
// certificate when connecting to an IP address, as WinSCP does) are checked
// against the session's fingerprint (SshHostKey, "*" = any) and against
// the WinSCP compatible cache [FtpsCertificates] "host:port=sha256;failures"
// (TTerminal::VerifyCertificate). Otherwise they are confirmed with
// UserInterface::QueryUser using WinSCP's VERIFY_CERT_PROMPT3 text and
// CERT_TEXT2 details: Yes = store in the cache (nixscp.ini, shared with
// background queue connections), No = connect once (remembered for reconnects
// of this file system object), Cancel = abort.
//
// Data connections reuse the TLS session of the control connection
// (QSslConfiguration::sessionTicket with SslOptionDisableSessionPersistence
// off, i.e. SSL_set_session), which servers like vsftpd (require_ssl_reuse)
// and FileZilla Server demand. Active mode announces the external IP address
// and listens in the port range of Preferences > Network
// (Configuration::ExternalIpAddress, LocalPortNumberMin/Max).
//
// The raw command console (fcAnyCommand) is available through
// AnyCommandFileSystem: the reply lines are the output.
#pragma once

#include "core/FileSystem.h"
#include "core/FtpListing.h"
#include "core/SecureShell.h"
#include "core/SessionData.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QMap>
#include <QSet>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslError>
#include <QSslKey>
#include <QStringList>

#include <memory>

class QSslSocket;
class QTcpServer;
class QTimer;

class FtpFileSystem : public FileSystem, public AnyCommandFileSystem
{
public:
  FtpFileSystem(const SessionData & Data, UserInterface * UI);
  ~FtpFileSystem() override;

  // TFTPFileSystem::IsCapable: the answers that depend on the server
  // (FEAT, SYST, welcome message); Default is the static table of Terminal
  bool IsCapable(FSCapability Capability, bool Default) const override;

  void Open() override;
  void Close() override;
  bool IsOpened() const override;
  QString ProtocolName() const override;
  QString ServerInfo() const override;

  QString CanonicalizePath(const QString & Path) override;
  QString HomeDirectory() override;
  RemoteFileList ReadDirectory(const QString & Path) override;
  RemoteFile ReadFile(const QString & Path) override;
  bool FileExists(const QString & Path, RemoteFile * File) override;
  void CreateDirectory(const QString & Path, int Mode) override;
  void DeleteFile(const QString & Path) override;
  void RemoveDirectory(const QString & Path) override;
  // Replaces an existing target file, like POSIX rename (on IIS, which refuses
  // to, the target is deleted first: !fcMoveOverExistingFile)
  void RenameFile(const QString & From, const QString & To) override;
  void ChangeMode(const QString & Path, unsigned int Mode) override;
  void SetModificationTime(const QString & Path, const QDateTime & Time) override;
  void CreateLink(const QString & Path, const QString & Target) override;

  void Download(const QString & RemotePath, const QString & LocalPath, FileOperationProgress & Progress) override;
  void Upload(const QString & LocalPath, const QString & RemotePath, int Mode, FileOperationProgress & Progress) override;
  // TransferParams::Append uploads with APPE (fcAppendUpload).
  // Text mode (TYPE A, CR/LF on the wire converted from/to the local LF) is
  // used when TransferParams has an AsciiTransfer member set to true (see
  // FtpAsciiTransfer in FtpFileSystem.cpp), or after SetAsciiTransfer(true).
  void Download(const QString & RemotePath, const QString & LocalPath, const TransferParams & Params, FileOperationProgress & Progress) override;
  void Upload(const QString & LocalPath, const QString & RemotePath, int Mode, const TransferParams & Params, FileOperationProgress & Progress) override;

  // AnyCommandFileSystem (TFTPFileSystem::AnyCommand, fcAnyCommand): sends a
  // raw command in Directory; each reply line ("250 OK") goes to Output.
  // Returns 0, throws on a 4xx/5xx reply. *NewDirectory gets the working
  // directory after the command (PWD).
  int AnyCommand(const QString & Command, const QString & Directory, OutputEvent Output,
    FileOperationProgress & Progress, QString * NewDirectory) override;
  // Sends a raw command and returns the reply lines with codes
  QStringList AnyCommand(const QString & Command);

  // Features listed in FEAT reply
  QStringList Features() const { return FFeatures; }
  bool UsingMlsd() const;
  // Keepalive (TFTPFileSystem::Idle + CFtpControlSocket::OnTimer); called from
  // a timer on the worker thread
  void Idle();

  // Text mode for the following transfers (for callers that cannot pass
  // TransferParams::AsciiTransfer)
  void SetAsciiTransfer(bool Ascii) { FAsciiTransfer = Ascii; }

  // ---- Server side operations (TFTPFileSystem; IsCapable) ----
  // fcMoveOverExistingFile (false for IIS)
  bool CanMoveOverExistingFile() const { return !FIIS; }
  // fcRemoteCopy: SITE CPFR/CPTO (server announces "SITE COPY")
  bool CanRemoteCopy() const { return SupportsSiteCommand(QStringLiteral("COPY")); }
  // TFTPFileSystem::CopyFile
  void CopyFile(const QString & From, const QString & To) override;
  // fcCalculatingChecksum and TFTPFileSystem::GetSupportedChecksumAlgs:
  // algorithms of the HASH command plus those of XSHA1/XSHA256/XSHA512/XMD5/
  // MD5/XCRC (WinSCP names: "sha-1", "sha-256", "sha-512", "md5", "crc32")
  bool CanCalculateChecksum() const { return FSupportsAnyChecksumFeature; }
  QStringList ChecksumAlgorithms() const override;
  // TFTPFileSystem::CalculateFilesChecksumInitialize + DoCalculateFileChecksum;
  // returns the lower case hex checksum
  QString CalculateFileChecksum(const QString & Path, const QString & Alg) override;
  // TFTPFileSystem::SpaceAvailable (fcCheckingSpaceAvailable): XQUOTA (WS_FTP) or AVBL (Serv-U)
  SpaceAvailableInfo SpaceAvailable(const QString & Path) override;

  // ---- Server / Protocol Information ----
  // CERT_TEXT2 details of the server certificate (Full certificate)
  QString CertificateText() const { return FCertificateText; }
  QString CertificateFingerprintSHA256() const { return FCertificateFingerprintSHA256; }
  QString CertificateFingerprintSHA1() const { return FCertificateFingerprintSHA1; }

  // CheckCertificate (Common.cpp): validates a client certificate file
  // (TlsCertificateFile) as Advanced Site Settings do on OK. An encrypted file
  // is accepted (its passphrase is asked when connecting). Throws ExtException.
  static void CheckCertificate(const QString & Path);

  // TTerminal::VerifyCertificate / CacheCertificate on the
  // [FtpsCertificates] storage key (SiteKey = "host:port", value
  // "sha256-fingerprint;XX", XX = OpenSSL verification error in hex)
  static bool IsCertificateCached(const QString & SiteKey, const QString & FingerprintSHA1,
    const QString & FingerprintSHA256, int Failures);
  static void CacheCertificate(const QString & SiteKey, const QString & FingerprintSHA256, int Failures);

private:
  struct Reply
  {
    int Code = 0;
    QStringList Lines;   // text without the code prefix (FLastResponse)
    QStringList RawLines; // lines as received (FOnCaptureOutput)
    int Class() const { return Code / 100; }
  };
  class OperationGuard;

  SessionData FData;
  UserInterface * FUI;
  std::unique_ptr<QSslSocket> FControl;
  std::unique_ptr<QTimer> FKeepaliveTimer;
  int FBusy = 0;
  bool FLoggedIn = false;
  bool FTls = false;
  bool FProtP = false;
  bool FUtf8 = true;
  bool FPasswordFailed = false;
  bool FAnyPassword = false;
  bool FAwaitingWelcome = false;
  bool FAsciiTransfer = false;
  // reply parser state
  QString FMultiLineCode;
  Reply FPartialReply;
  QString FLastCommand;
  // server
  QString FWelcomeMessage;
  QString FSystem;
  QString FServerID;            // CSID reply
  QStringList FFeatures;
  QSet<QString> FSupportedCommands;
  QSet<QString> FSupportedSiteCommands;
  QStringList FHashAlgs;        // HASH command algorithms (lower case)
  QString FHashAlg;             // algorithm selected with OPTS HASH
  bool FSupportsAnyChecksumFeature = false;
  bool FHasClntCommand = false;
  int FServerType = 0;          // FtpListParser::ServerTypeFlag
  bool FMVS = false;
  bool FVMS = false;
  bool FIIS = false;
  bool FFileZilla = false;
  TAutoSwitch FListAll = asAuto;
  TAutoSwitch FWorkFromCwd = asAuto;
  QString FHomeDirectory;
  QString FCurrentDirectory;    // actual working directory of the server
  QChar FTransferType;          // last TYPE sent
  bool FAnyTransferSucceeded = false;
  QElapsedTimer FLastActivity;  // last command (dummy command keepalive)
  QElapsedTimer FLastDataSent;  // last directory listing (directory keepalive)
  // timestamps
  bool FDetectTimeDifference = true;
  qint64 FTimeDifference = 0;   // seconds added to listing times (server local time)
  QMap<QString, QDateTime> FUploadedTimes;
  // TLS
  QSslCertificate FCertificate;
  QSslConfiguration FDataTlsConfiguration;
  QString FCertificateFingerprintSHA256;
  QString FCertificateFingerprintSHA1;
  QString FCertificateText;
  QSet<QString> FAcceptedOnce;
  QString FTlsVersion;
  QString FCipher;
  bool FClientCertificateLoaded = false;
  QSslCertificate FClientCertificate;
  QSslKey FClientKey;
  // Configuration snapshot (Preferences > Network, hidden options)
  QString FExternalIpAddress;
  int FLocalPortNumberMin = 0;
  int FLocalPortNumberMax = 0;
  bool FShowFtpWelcomeMessage = false;
  QList<QSslCertificate> FCaCertificates;  // CertificateStorage
  // parent directory listing cache used by ReadFile without MLST
  // (TFTPFileSystem::FFileListCache)
  QString FFileListCacheDir;
  RemoteFileList FFileListCache;
  QSet<QString> FFileListCacheDirty;
  QElapsedTimer FFileListCacheTime;

  int TimeoutMs() const;
  void LoadConfiguration();
  void Connect();
  void ConnectSocket(QSslSocket * Socket, const QString & Host, quint16 Port, FileOperationProgress * Progress, bool Control);
  void Socks4Handshake(QSslSocket * Socket, const QString & Host, quint16 Port, FileOperationProgress * Progress);
  bool UseProxy() const;
  void ApplySocketOptions(QSslSocket * Socket, bool DataConnection) const;
  void Login(QString & Password);
  bool LoadTlsCertificate();
  void StartTls(QSslSocket * Socket, bool Control);
  void VerifyCertificate(const QList<QSslError> & Errors);
  bool VerifyCertificateAgainstSessionData() const;
  void ConfigureSocket(QSslSocket * Socket, bool SetProxy) const;
  void DoClose();
  void Startup();
  void ProcessFeatures(const Reply & FeatReply);
  void ProcessWelcome(const Reply & R);
  void ReadCurrentDirectory();

  QByteArray Encode(const QString & Str) const;
  QString Decode(const QByteArray & Bytes) const;
  void SendCommand(const QString & Command, bool DrainStale = true);
  bool PollReply(Reply & Result);
  Reply ReadReply(FileOperationProgress * Progress = nullptr);
  Reply Command(const QString & Command);
  void CheckReply(const Reply & R, const QString & Message, bool Allow3xx = false);
  [[noreturn]] void ReplyError(const Reply & R, const QString & Message);
  [[noreturn]] void ConnectionLost(const QString & Details);
  void CheckConnection();
  void WriteAll(QSslSocket * Socket, const QByteArray & Data, FileOperationProgress * Progress);

  bool SupportsCommand(const QString & Command) const { return FSupportedCommands.contains(Command.toUpper()); }
  bool SupportsSiteCommand(const QString & Command) const { return FSupportedSiteCommands.contains(Command.toUpper()); }
  QString AbsolutePath(const QString & Path) const;
  void ChangeWorkingDirectory(const QString & Directory);
  // EnsureLocationWhenWorkFromCwd: with FtpWorkFromCwd, enters the directory
  // of Abs and returns the name only; otherwise returns Abs
  QString FileCommandPath(const QString & Abs);
  void EnsureType(QChar Type);
  void InvalidateCache(const QString & Path);

  // Data connection (CFtpControlSocket LIST/FILETRANSFER states + CTransferSocket)
  struct DataConnection;
  std::unique_ptr<DataConnection> OpenDataConnection(const QString & Command, qint64 * Rest, FileOperationProgress * Progress, const QString & Error);
  bool ListenForDataConnection(QTcpServer * Server, const QHostAddress & Local, QString & Error);
  QString FormatPortCommand(const QHostAddress & Local, quint16 Port) const;
  qint64 ReadData(DataConnection & Data, char * Buffer, qint64 MaxSize, FileOperationProgress * Progress, const QString & Error);
  void FinishDataConnection(DataConnection & Data, FileOperationProgress * Progress, const QString & Error);
  void AbortDataConnection(std::unique_ptr<DataConnection> & Data);
  bool ParsePasvReply(const Reply & R, QString & Host, quint16 & Port) const;
  bool ParseEpsvReply(const Reply & R, quint16 & Port) const;

  RemoteFileList DoReadDirectory(const QString & Directory, bool ListAll);
  RemoteFileList ListDirectory(const QString & Directory);
  void ResolveSymlinks(const QString & Directory, RemoteFileList & Files);
  RemoteFile FileFromEntry(const FtpListEntry & Entry, const QString & Directory);
  void AutoDetectTimeDifference(const QString & Directory, const QList<FtpListEntry> & Entries);
  void ApplyTimeDifference(const QString & FullPath, const FtpListEntry & Entry, RemoteFile & File);
  bool ReadFileInternal(const QString & Path, RemoteFile & File, QString & Error);
  bool ListFile(const QString & Abs, RemoteFile & File, QString & Error);
  bool SupportsReadingFile() const;
  QDateTime ParseMdtm(const Reply & R) const;
  bool Unquote(QString & Str) const;
};
