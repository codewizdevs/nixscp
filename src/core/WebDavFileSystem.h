// Port of WinSCP source/core/WebDAVFileSystem.cpp (TWebDAVFileSystem) with the
// parts of source/core/NeonIntf.cpp and of the neon library (libs/neon:
// ne_request.c, ne_basic.c, ne_props.c, ne_207.c, ne_auth.c, ne_redirect.c)
// it relies on.
//
// HTTP client: instead of neon, a small blocking HTTP/1.1 client on top of
// QSslSocket (persistent connections, Content-Length and chunked bodies,
// Basic and Digest authentication, manually followed redirects).
// QNetworkAccessManager was not used: it only makes progress while the
// calling thread runs an event loop, and a local QEventLoop on the Terminal
// worker thread would also dispatch the jobs Terminal queues to that thread
// (Close(), or the destructor's close-and-delete), re-entering this object in
// the middle of a request. QSslSocket's waitFor* functions do not dispatch
// posted events, as in FtpFileSystem.
//
// As with neon, the connection is opened on demand and reopened when the
// server has closed it (keep-alive timeout). Network errors during operations
// are therefore not fatal for the session (as in WinSCP); only Open() throws
// FatalException. A cancelled transfer drops the connection.
//
// Authentication (TWebDAVFileSystem::NeonRequestAuth): the first 401 answer
// is retried with the stored password, then the user is asked (pkPassword)
// until the server accepts the password or the prompt is cancelled. Digest
// (MD5, SHA-256 and their -sess variants, qop=auth; RFC 2069 Digest without
// qop only with WebDavAuthLegacy, as neon's NE_AUTH_LEGACY_DIGEST) is
// preferred to Basic; the credentials are then sent with every request.
//
// TLS certificates that do not verify are confirmed by the user as in
// FtpFileSystem (VERIFY_CERT_PROMPT3 + CERT_TEXT2; Yes = trusted for the rest
// of the process, No = for this file system object only, Cancel = connection
// refused), with the certificate problems summarized as WinSCP does for HTTPS
// (NeonCertificateFailuresErrorStr).
//
// Redirects (TWebDAVFileSystem::GetRedirectUrl): only to the same host and
// port and never from https to http, always: WinSCP's settings to allow more
// (WebDavCrossDomainRedirects / WebDavUnencryptedRedirects) are not ported, as
// they would hand the credentials to whatever the server names; followed by Open
// (OPTIONS, up to 5 with loop detection), by PROPFIND, MOVE (one step) and by
// GET; other requests fail with "WebDAV resource moved to ...".
//
// Unsupported as in WinSCP: permissions (ChangeMode), links. Uploads cannot
// be resumed (the whole file is sent again); downloads are resumed with a
// Range request.
#pragma once

#include "core/FileSystem.h"
#include "core/SessionData.h"

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QMap>
#include <QPair>
#include <QSet>
#include <QSslCertificate>
#include <QSslError>
#include <QStringList>

#include <functional>
#include <memory>

class QFile;
class QSslSocket;

class WebDavFileSystem : public FileSystem
{
public:
  WebDavFileSystem(const SessionData & Data, UserInterface * UI);
  ~WebDavFileSystem() override;

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
  void RenameFile(const QString & From, const QString & To) override;
  void ChangeMode(const QString & Path, unsigned int Mode) override;
  void SetModificationTime(const QString & Path, const QDateTime & Time) override;
  void CreateLink(const QString & Path, const QString & Target) override;

  void Download(const QString & RemotePath, const QString & LocalPath, FileOperationProgress & Progress) override;
  void Upload(const QString & LocalPath, const QString & RemotePath, int Mode, FileOperationProgress & Progress) override;
  void Download(const QString & RemotePath, const QString & LocalPath, const TransferParams & Params, FileOperationProgress & Progress) override;
  void Upload(const QString & LocalPath, const QString & RemotePath, int Mode, const TransferParams & Params, FileOperationProgress & Progress) override;

  // WebDAV compliance classes and extensions announced in the DAV header of
  // the OPTIONS response (FCapabilities), e.g. "1", "2"
  QStringList Capabilities() const { return FCapabilities; }

  // Percent-encoding of paths as neon's ne_path_escape (everything but
  // unreserved characters, sub-delims, ':', '@' and '/'), and the reverse
  // (ne_path_unescape; malformed input is returned as is, as WinSCP does)
  // The path of an href in a multistatus response. LiberalEscaping: WebDavLiberalEscaping
  static QString HrefPath(const QString & Href, const QString & RequestPath, bool LiberalEscaping);
  static QByteArray PathEscape(const QString & Path);
  static QString PathUnescape(const QByteArray & Path);

private:
  using HeaderList = QList<QPair<QByteArray, QByteArray>>;

  struct Response
  {
    int Status = 0;
    QByteArray Reason;
    HeaderList Headers;            // names in lower case
    QByteArray Body;               // unless streamed to Request::OnData
    QByteArray Header(const QByteArray & Name) const;
    QList<QByteArray> AllHeaders(const QByteArray & Name) const;
    int Class() const { return Status / 100; }
    // neon's error text for HTTP failures ("404 Not Found")
    QString StatusText() const;
  };

  struct Request
  {
    QByteArray Method;
    QString Path;                  // absolute, unescaped; a trailing slash is kept
    QByteArray Query;              // escaped query (redirect targets)
    HeaderList Headers;
    QByteArray Body;
    // PUT: body read from File (FileLength bytes from FileOffset);
    // the first Uncounted bytes are not added to Progress
    QFile * File = nullptr;
    qint64 FileOffset = 0;
    qint64 FileLength = 0;
    qint64 Uncounted = 0;
    qint64 Counted = 0;            // added to Progress by the last attempt
    // GET: called with the final response headers; returning true streams
    // the body to OnData (with cancel and speed limit) instead of Body
    std::function<bool(const Response &)> OnHeaders;
    std::function<void(const char *, qint64)> OnData;
    FileOperationProgress * Progress = nullptr;
  };

  struct Challenge
  {
    QByteArray Scheme;             // lower case
    QMap<QByteArray, QByteArray> Params;
  };

  struct Authentication
  {
    enum SchemeType { None, Basic, Digest } Scheme = None;
    QByteArray Realm;
    QByteArray Nonce;
    QByteArray Opaque;
    QByteArray Algorithm;          // as sent by the server ("MD5", "SHA-256-sess")
    bool Qop = false;              // qop=auth
    bool Session = false;          // -sess algorithm
    bool Sha256 = false;
    QByteArray CNonce;
    QByteArray HA1;                // -sess: computed once per challenge
    unsigned int NonceCount = 0;
  };

  // One <D:response> of a multistatus answer (ne_props.c / ne_207.c)
  struct Resource
  {
    QString Path;                  // unescaped path of the href, trailing slash kept
    bool Collection = false;
    QMap<QString, QString> Props;  // "<namespace><name>" -> text, from 2xx propstat only
    bool Failed = false;           // non-2xx response status
  };

  SessionData FData;
  UserInterface * FUI;
  std::unique_ptr<QSslSocket> FSocket;
  bool FOpened = false;
  bool FTls = false;
  QString FHost;
  int FPort = 0;
  bool FViaHttpProxy = false;      // plain HTTP sent to an HTTP proxy (absolute URIs)
  bool FCloseAfterResponse = false;
  bool FInitialHandshake = false;
  bool FResponseStarted = false;   // a byte of the status line arrived
  QString FHomeDirectory;
  QString FCurrentDirectory;
  QStringList FCapabilities;
  QString FRemoteSystem;
  // authentication (TWebDAVFileSystem::NeonRequestAuth)
  QString FUserName;
  QString FPassword;
  bool FPasswordKnown = false;
  bool FStoredPasswordTried = false;
  bool FPasswordFailed = false;
  Authentication FAuth;
  // TLS
  QSslCertificate FCertificate;
  QString FCertificateFingerprintSHA256;
  QString FCertificateFingerprintSHA1;
  QSet<QString> FAcceptedOnce;
  QString FTlsVersion;
  QString FCipher;

  int TimeoutMs() const;
  int DefaultPort() const { return FTls ? HTTPSPortNumber : HTTPPortNumber; }
  QByteArray HostPort() const;

  // connection
  void ConfigureSocket(QSslSocket * Socket);
  void Connect();
  void Disconnect();
  bool ConnectionUsable();
  void VerifyCertificate(const QList<QSslError> & Errors);
  bool WaitForData(FileOperationProgress * Progress, const char * Doing, qint64 Have = 0);
  [[noreturn]] void ReadError(const char * Doing);
  void WriteAll(const QByteArray & Data, FileOperationProgress * Progress);
  QByteArray ReadLine(FileOperationProgress * Progress, const char * Doing, bool StatusLine = false);

  // HTTP
  static Request MakeRequest(const QByteArray & Method, const QString & Path);
  QByteArray UrlBytes(const QString & Path) const;
  QByteArray RequestTarget(const Request & Req) const;
  Response Dispatch(Request & Req);
  Response Exchange(Request & Req);
  void SendRequest(Request & Req);
  void ReadResponse(Request & Req, Response & Resp);
  void ReadBody(Request & Req, Response & Resp, qint64 Length, bool Chunked, bool Stream);
  void ReadBodyPart(Request & Req, Response & Resp, qint64 Length, bool Stream, QByteArray & Buffer);

  // authentication
  static QList<Challenge> ParseChallenges(const QByteArray & Header);
  bool Authenticate(const Response & Resp, bool CredentialsSent);
  QByteArray AuthorizationHeader(const Request & Req);
  QByteArray DigestHash(const QByteArray & Data) const;

  // WebDAV
  QString HomeDirectoryOrRoot() const;
  QString AbsolutePath(const QString & Path) const;
  static QString DirectoryPath(const QString & Path);
  static bool IsRedirect(const Response & Resp);
  QString RedirectPath(const Response & Resp, const Request & Req, QByteArray * Query);
  // CheckStatus/CheckNeonStatus: throws ExtException with neon's error for a
  // failed response (callers add the operation message)
  void CheckStatus(const Response & Resp, bool Multistatus);
  Response Propfind(QString & Path, int Depth);
  static QList<Resource> ParseMultistatus(const QByteArray & Xml, const QString & RequestPath, QStringList * Errors,
    bool LiberalEscaping = false);
  RemoteFile FileFromResource(const Resource & Res, const QString & Name) const;
  bool ReadFileInternal(const QString & Path, RemoteFile & File, Response * Failure);
  void ParseCapabilities(const Response & Resp);
};
