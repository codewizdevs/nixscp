// Port of WinSCP source/core/WebDAVFileSystem.cpp (TWebDAVFileSystem) and of the
// neon request handling it uses, as a blocking QSslSocket based HTTP/1.1
// client (see WebDavFileSystem.h).
#include "core/WebDavFileSystem.h"
#include "core/SessionLog.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QLocale>
#include <QMimeDatabase>
#include <QMutex>
#include <QNetworkProxy>
#include <QRandomGenerator>
#include <QSslCipher>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QTimeZone>
#include <QUrl>
#include <QXmlStreamReader>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace
{

// TextsCore1.rc
const char * const CONNECTION_FAILED = "Connection failed.";
const char * const AUTHENTICATION_FAILED = "Authentication failed.";
const char * const PROXY_AUTHENTICATION_FAILED = "Proxy authentication failed.";
const char * const FTP_ACCESS_DENIED = "Access denied.";
const char * const NET_TRANSL_HOST_NOT_EXIST2 = "Host \"%1\" does not exist.";
const char * const NET_TRANSL_TIMEOUT2 = "Network error: Connection to \"%1\" timed out.";
const char * const REQUEST_REDIRECTED = "WebDAV resource moved to '%1'.";
const char * const UNENCRYPTED_REDIRECT = "The server redirected to an unencrypted URL. NixSCP does not follow it, as the password of this site would be sent unencrypted.";
const char * const WEBDAV_CROSS_DOMAIN_REDIR = "The server redirected to other host %1. NixSCP does not follow it, as the password of this site would go there. If you trust that host, connect to it directly.";
const char * const TOO_MANY_REDIRECTS = "Too many redirects.";
const char * const REDIRECT_LOOP = "Redirect loop detected.";
const char * const INVALID_URL = "Invalid URL \"%1\".";
const char * const NOTSUPPORTED = "Operation not supported.";
const char * const FTPS_IMPLICIT = "TLS/SSL Implicit encryption";
const char * const WEBDAV_EXTENSION_INFO = "The server supports these WebDAV extensions:";
const char * const FILE_NOT_EXISTS = "File or folder '%1' does not exist.";
const char * const TRANSFER_ERROR = "Error transferring file '%1'.";
const char * const LIST_DIR_ERROR = "Error listing directory '%1'.";
const char * const CHANGE_DIR_ERROR = "Error changing directory to '%1'.";
const char * const CANT_GET_ATTRS = "Can't get attributes of file '%1'.";
const char * const CREATE_DIR_ERROR = "Error creating folder '%1'.";
const char * const DELETE_FILE_ERROR = "Error deleting file '%1'.";
const char * const RENAME_FILE_ERROR = "Error renaming file '%1' to '%2'.";
const char * const CANT_SET_ATTRS = "Can't set attributes of file '%1'.";
const char * const CREATE_LINK_ERROR = "Cannot create link '%1'.";
const char * const PRESERVE_TIME_PERM_ERROR3 = "Upload of file '%1' was successful, but error occurred while setting the permissions and/or timestamp.";
// Certificates (as FtpFileSystem; summary as NeonCertificateFailuresErrorStr)
const char * const VERIFY_CERT_PROMPT3 =
  "**The server's certificate is not known. You have no guarantee that the server is the computer you think it is.**\n\n"
  "Server's certificate details follow:\n\n%1\n\n"
  "If you trust this certificate, press Yes. To connect without storing certificate, press No. To abandon the connection press Cancel.\n\n"
  "Continue connecting and store the certificate?";
const char * const CERT_TEXT2 = "Issuer:\n%1\nSubject:\n%2\nValid: %3 – %4\n\nFingerprints:\n- SHA-256: %5\n- SHA-1: %6\n\nSummary: %7";
const char * const CERT_OK = "Certificate is valid.";
const char * const CERT_ERR_CERT_NOT_YET_VALID = "Certificate is not yet valid.";
const char * const CERT_ERR_CERT_HAS_EXPIRED = "Certificate has expired.";
const char * const CERT_NAME_MISMATCH = "Certificate was not issued for this server. You might be connecting to a server that is pretending to be \"%1\".";
const char * const CERT_ERR_CERT_UNTRUSTED = "Certificate not trusted.";
const char * const CERT_ERR_BAD_CHAIN = "Some certificates in certificate chain are invalid.";
// neon (ne_request.c, ne_socket.c, ne_openssl.c)
const char * const NE_COULD_NOT_CONNECT = "Could not connect to server: %1";
const char * const NE_COULD_NOT_CONNECT_PROXY = "Could not connect to proxy server: %1";
const char * const NE_COULD_NOT_RESOLVE = "Could not resolve hostname `%1': %2";
const char * const NE_SSL_HANDSHAKE_FAILED = "SSL handshake failed: %1";
const char * const NE_VERIFY_FAILED = "Server certificate verification failed: %1";
const char * const NE_VERIFY_ERROR = "Certificate verification error: %1";
const char * const NE_READ_STATUS_LINE = "Could not read status line";
const char * const NE_READ_HEADERS = "Error reading response headers";
const char * const NE_READ_BODY = "Could not read response body";
const char * const NE_READ_CHUNK_SIZE = "Could not read chunk size";
const char * const NE_READ_CHUNK_DELIMITER = "Could not read chunk delimiter";
const char * const NE_SEND_REQUEST = "Could not send request";
const char * const NE_SEND_BODY = "Could not send request body";

QString Tr(const char * Text)
{
  return QString::fromUtf8(Text);
}

const qint64 TransferChunk = 64 * 1024;
const qint64 UploadHighWater = 256 * 1024;
const qint64 ReadBufferSize = 1024 * 1024;
const qint64 MaxLineLength = 64 * 1024;
const qint64 MaxBodySize = 512 * 1024 * 1024;  // non-streamed bodies (listings)
const qsizetype MaxHeaderFields = 1000;
const int MaxRedirectAttempts = 5;             // MAX_REDIRECT_ATTEMPTS
const QString DavNamespace = QStringLiteral("DAV:");

bool LogEnabled()
{
  static const bool Enabled = !qEnvironmentVariableIsEmpty("NIXSCP_DAV_LOG");
  return Enabled;
}

void LogLine(const QString & Line)
{
  SessionLog::LogProtocolLine(Line, 0);
  if (LogEnabled())
  {
    std::fprintf(stderr, "[dav] %s\n", qPrintable(Line));
  }
}

// "host:port=fingerprint" entries for this server
bool HasAcceptedCertificate(const QSet<QString> & Accepted, const QString & Host, int Port)
{
  QString Prefix = QStringLiteral("%1:%2=").arg(Host).arg(Port);
  return std::any_of(Accepted.cbegin(), Accepted.cend(), [&Prefix](const QString & Key) { return Key.startsWith(Prefix); });
}

// TTerminal::VerifyCertificateAgainstSessionData: fingerprint(s) configured
// for the site (-certificate / SshHostKey, ";" separated, "*" = any certificate)
bool CertificateMatchesSessionData(const QString & Configured, const QString & FingerprintSHA1, const QString & FingerprintSHA256)
{
  auto Same = [](QString A, QString B)
  {
    A.replace(QLatin1Char(':'), QLatin1Char('-'));
    B.replace(QLatin1Char(':'), QLatin1Char('-'));
    return (A.compare(B, Qt::CaseInsensitive) == 0);
  };
  for (const QString & ExpectedKey : Configured.split(QLatin1Char(';'), Qt::SkipEmptyParts))
  {
    QString Expected = ExpectedKey.trimmed();
    if (Expected == QStringLiteral("*"))
    {
      LogLine(QStringLiteral("WARNING! Giving up security and accepting any certificate as configured!"));
      return true;
    }
    if (Same(Expected, FingerprintSHA1) || Same(Expected, FingerprintSHA256))
    {
      LogLine(QStringLiteral("Certificate matches configured fingerprint"));
      return true;
    }
  }
  return false;
}

// Certificates accepted with "Yes" (process wide, per host:port); WinSCP keeps
// HTTPS certificates separate from FTPS ones (HttpsCertificateStorageKey)
QMutex AcceptedCertificatesMutex;
QSet<QString> AcceptedCertificates;

// neon certificate failure bits (NE_SSL_*)
enum
{
  SslNotYetValid = 0x01,
  SslExpired = 0x02,
  SslIdMismatch = 0x04,
  SslUntrusted = 0x08,
  SslBadChain = 0x10,
};

QString NormalizePath(const QString & Path)
{
  QStringList Result;
  for (const QString & Part : Path.split(QLatin1Char('/'), Qt::SkipEmptyParts))
  {
    if (Part == QStringLiteral("."))
    {
      continue;
    }
    if (Part == QStringLiteral(".."))
    {
      if (!Result.isEmpty())
      {
        Result.removeLast();
      }
      continue;
    }
    Result.append(Part);
  }
  return QStringLiteral("/") + Result.join(QLatin1Char('/'));
}

QString ExcludeTrailingSlash(const QString & Path)
{
  QString Result = Path;
  while ((Result.size() > 1) && Result.endsWith(QLatin1Char('/')))
  {
    Result.chop(1);
  }
  return Result;
}

QString JoinMessages(const QString & Message, const QString & More)
{
  return More.isEmpty() ? Message : (Message + QLatin1Char('\n') + More);
}

// Error of an operation with the protocol error as details (as TTerminal's
// FILE_OPERATION_LOOP / ExtException(E, Message) do)
template <typename F>
void WithErrorMessage(const QString & Message, F && Func)
{
  try
  {
    Func();
  }
  catch (const AbortException &)
  {
    throw;
  }
  catch (const FatalException &)
  {
    throw;
  }
  catch (const ExtException & E)
  {
    throw ExtException(Message, JoinMessages(E.Message(), E.MoreMessages()));
  }
}

QString FormatContactList(const QString & Entry1, const QString & Entry2)
{
  if (!Entry1.isEmpty() && !Entry2.isEmpty())
  {
    return QStringLiteral("%1, %2").arg(Entry1, Entry2);
  }
  return Entry1 + Entry2;
}

// FormatContact (VERIFY_CERT_CONTACT), as FtpFileSystem
QString FormatContact(const QSslCertificate & Certificate, bool Issuer)
{
  auto Info = [&](QSslCertificate::SubjectInfo What) -> QString
  {
    return (Issuer ? Certificate.issuerInfo(What) : Certificate.subjectInfo(What)).join(QStringLiteral(", "));
  };
  QString Result = QStringLiteral("- Organization: %1\n").arg(
    FormatContactList(FormatContactList(FormatContactList(
      Info(QSslCertificate::Organization), Info(QSslCertificate::OrganizationalUnitName)),
      Info(QSslCertificate::CommonName)), Info(QSslCertificate::EmailAddress)));
  QString Location = FormatContactList(FormatContactList(
    Info(QSslCertificate::CountryName), Info(QSslCertificate::StateOrProvinceName)), Info(QSslCertificate::LocalityName));
  if (!Location.isEmpty())
  {
    Result += QStringLiteral("- Location: %1\n").arg(Location);
  }
  return Result;
}

// NeonCertificateFailuresErrorStr
QString CertificateFailuresErrorStr(int Failures, const QString & HostName)
{
  QStringList Result;
  if (Failures & SslNotYetValid)
  {
    Result << Tr(CERT_ERR_CERT_NOT_YET_VALID);
  }
  if (Failures & SslExpired)
  {
    Result << Tr(CERT_ERR_CERT_HAS_EXPIRED);
  }
  if (Failures & SslIdMismatch)
  {
    Result << Tr(CERT_NAME_MISMATCH).arg(HostName);
  }
  if (Failures & SslUntrusted)
  {
    Result << Tr(CERT_ERR_CERT_UNTRUSTED);
  }
  if (Failures & SslBadChain)
  {
    Result << Tr(CERT_ERR_BAD_CHAIN);
  }
  return Result.join(QLatin1Char(' '));
}

// ne__sock_set_verify_err
QString VerifyFailureReasons(int Failures)
{
  QStringList Result;
  if (Failures & SslNotYetValid) Result << QStringLiteral("certificate is not yet valid");
  if (Failures & SslExpired) Result << QStringLiteral("certificate has expired");
  if (Failures & SslIdMismatch) Result << QStringLiteral("certificate issued for a different hostname");
  if (Failures & SslUntrusted) Result << QStringLiteral("issuer is not trusted");
  if (Failures & SslBadChain) Result << QStringLiteral("bad certificate chain");
  return Result.join(QStringLiteral(", "));
}

// "HTTP/1.1 200 OK" -> 200 (0 when missing or malformed)
int StatusCode(const QString & StatusLine)
{
  QStringList Parts = StatusLine.simplified().split(QLatin1Char(' '));
  if ((Parts.size() < 2) || !Parts[0].startsWith(QStringLiteral("HTTP/"), Qt::CaseInsensitive))
  {
    return 0;
  }
  return Parts[1].toInt();
}

// RFC 1123 date as parsed by TWebDAVFileSystem::ParsePropResultSet
// ("%3s, %02d %3s %4d %02d:%02d:%02d GMT", at least a complete date)
QDateTime ParseHttpDate(const QString & Str)
{
  static const char * const Months[] = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};
  QStringList Parts = Str.simplified().split(QLatin1Char(' '));
  if ((Parts.size() < 4) || (Parts[0].size() != 4) || !Parts[0].endsWith(QLatin1Char(',')))
  {
    return QDateTime();
  }
  bool DayOk = false, YearOk = false;
  int Day = Parts[1].toInt(&DayOk);
  int Year = Parts[3].toInt(&YearOk);
  int Month = 0;
  for (int Index = 0; Index < 12; Index++)
  {
    if (Parts[2].compare(QLatin1String(Months[Index]), Qt::CaseInsensitive) == 0)
    {
      Month = Index + 1;
    }
  }
  QDate Date(Year, Month, Day);
  if (!DayOk || !YearOk || (Month == 0) || !Date.isValid())
  {
    return QDateTime();
  }
  QTime Time(0, 0, 0);
  if (Parts.size() >= 5)
  {
    QStringList TimeParts = Parts[4].split(QLatin1Char(':'));
    QTime Parsed(TimeParts.value(0).toInt(), TimeParts.value(1).toInt(), TimeParts.value(2).toInt());
    if (Parsed.isValid())
    {
      Time = Parsed;
    }
  }
  return QDateTime(Date, Time, QTimeZone::UTC).toLocalTime();
}

// Path of an href of a multistatus response: absolute URIs are reduced to
// their path, relative references resolved against the request path.
// LiberalEscaping (WebDavLiberalEscaping, NE_SESSFLAG_LIBERAL_ESCAPING): the
// server does not encode names, so '?' and '#' are characters of the name.
QString HrefPathOf(const QString & Href, const QString & RequestPath, bool LiberalEscaping)
{
  QByteArray Raw = Href.trimmed().toUtf8();
  qsizetype SchemeEnd = Raw.indexOf("://");
  if ((SchemeEnd > 0) && !Raw.left(SchemeEnd).contains('/'))
  {
    qsizetype PathStart = Raw.indexOf('/', SchemeEnd + 3);
    Raw = (PathStart < 0) ? QByteArray("/") : Raw.mid(PathStart);
  }
  qsizetype End = LiberalEscaping ? -1 : Raw.indexOf('?');
  qsizetype Fragment = LiberalEscaping ? -1 : Raw.indexOf('#');
  if ((Fragment >= 0) && ((End < 0) || (Fragment < End)))
  {
    End = Fragment;
  }
  if (End >= 0)
  {
    Raw.truncate(End);
  }
  QString Result = WebDavFileSystem::PathUnescape(Raw);
  if (!Result.startsWith(QLatin1Char('/')))
  {
    QString Base = RequestPath.endsWith(QLatin1Char('/')) ? RequestPath : (UnixPath::ExtractFileDir(RequestPath) + QLatin1Char('/'));
    if (!Base.startsWith(QLatin1Char('/')))
    {
      Base.prepend(QLatin1Char('/'));
    }
    Result = Base + Result;
  }
  return Result;
}

// Value of an auth-param: token or quoted-string
QByteArray QuoteHeaderValue(const QByteArray & Value)
{
  QByteArray Result = Value;
  Result.replace('\\', "\\\\");
  Result.replace('"', "\\\"");
  return '"' + Result + '"';
}

QByteArray RandomHex(int Bytes)
{
  QByteArray Data(Bytes, Qt::Uninitialized);
  for (int Index = 0; Index < Bytes; Index++)
  {
    Data[Index] = static_cast<char>(QRandomGenerator::global()->bounded(256));
  }
  return Data.toHex();
}

}

//---------------------------------------------------------------------------

QByteArray WebDavFileSystem::Response::Header(const QByteArray & Name) const
{
  for (const auto & Field : Headers)
  {
    if (Field.first == Name)
    {
      return Field.second;
    }
  }
  return QByteArray();
}

QList<QByteArray> WebDavFileSystem::Response::AllHeaders(const QByteArray & Name) const
{
  QList<QByteArray> Result;
  for (const auto & Field : Headers)
  {
    if (Field.first == Name)
    {
      Result << Field.second;
    }
  }
  return Result;
}

QString WebDavFileSystem::Response::StatusText() const
{
  return QStringLiteral("%1 %2").arg(Status).arg(QString::fromLatin1(Reason));
}

//---------------------------------------------------------------------------

WebDavFileSystem::WebDavFileSystem(const SessionData & Data, UserInterface * UI) :
  FData(Data),
  FUI(UI)
{
  FTls = (FData.Ftps != ftpsNone);
  FHost = FData.HostName;
  FPort = (FData.PortNumber > 0) ? FData.PortNumber : DefaultPort();
}

WebDavFileSystem::~WebDavFileSystem()
{
  Close();
}

int WebDavFileSystem::TimeoutMs() const
{
  return std::max(FData.Timeout, 1) * 1000;
}

bool WebDavFileSystem::IsOpened() const
{
  return FOpened;
}

QString WebDavFileSystem::ProtocolName() const
{
  // CONST_WEBDAV_PROTOCOL_BASE_NAME
  return QStringLiteral("WebDAV");
}

QString WebDavFileSystem::ServerInfo() const
{
  QStringList Lines;
  if (!FRemoteSystem.isEmpty())
  {
    Lines << QStringLiteral("Server: %1").arg(FRemoteSystem);
  }
  if (FTls)
  {
    QString Security = Tr(FTPS_IMPLICIT);
    if (!FTlsVersion.isEmpty())
    {
      Security += QStringLiteral(", ") + FTlsVersion;
    }
    Lines << QStringLiteral("Encryption: %1").arg(Security);
    if (!FCipher.isEmpty())
    {
      Lines << QStringLiteral("Cipher: %1").arg(FCipher);
    }
    if (!FCertificateFingerprintSHA256.isEmpty())
    {
      Lines << QStringLiteral("Certificate fingerprint (SHA-256): %1").arg(FCertificateFingerprintSHA256);
    }
  }
  if (!FCapabilities.isEmpty())
  {
    // WEBDAV_EXTENSION_INFO in TFileSystemInfo::AdditionalInfo
    Lines << Tr(WEBDAV_EXTENSION_INFO) << (QStringLiteral("  ") + FCapabilities.join(QStringLiteral(", ")));
  }
  return Lines.join(QLatin1Char('\n'));
}

QString WebDavFileSystem::HrefPath(const QString & Href, const QString & RequestPath, bool LiberalEscaping)
{
  return HrefPathOf(Href, RequestPath, LiberalEscaping);
}

QByteArray WebDavFileSystem::PathEscape(const QString & Path)
{
  // ne_path_escape: escape all but URI_SEGCHAR (unreserved, sub-delims, ':', '@', '/')
  static const char Allowed[] = "-._~!$&'()*+,;=:@/";
  QByteArray Utf8 = Path.toUtf8();
  QByteArray Result;
  Result.reserve(Utf8.size());
  for (char C : std::as_const(Utf8))
  {
    unsigned char U = static_cast<unsigned char>(C);
    if (((U >= 'a') && (U <= 'z')) || ((U >= 'A') && (U <= 'Z')) || ((U >= '0') && (U <= '9')) ||
        ((U != 0) && (std::strchr(Allowed, C) != nullptr)))
    {
      Result += C;
    }
    else
    {
      static const char Hex[] = "0123456789abcdef";
      Result += '%';
      Result += Hex[U >> 4];
      Result += Hex[U & 0x0F];
    }
  }
  return Result;
}

QString WebDavFileSystem::PathUnescape(const QByteArray & Path)
{
  // ne_path_unescape; malformed escapes: take the path as is (PathUnescape in WinSCP)
  QByteArray Result;
  Result.reserve(Path.size());
  for (qsizetype Index = 0; Index < Path.size(); Index++)
  {
    char C = Path[Index];
    if (C == '%')
    {
      bool Ok = false;
      int Value = (Index + 2 < Path.size()) ? Path.mid(Index + 1, 2).toInt(&Ok, 16) : 0;
      if (!Ok || !std::isxdigit(static_cast<unsigned char>(Path[Index + 1])) || !std::isxdigit(static_cast<unsigned char>(Path[Index + 2])))
      {
        return QString::fromUtf8(Path);
      }
      Result += static_cast<char>(Value);
      Index += 2;
    }
    else
    {
      Result += C;
    }
  }
  return QString::fromUtf8(Result);
}

QByteArray WebDavFileSystem::HostPort() const
{
  // ne_get_server_hostport
  QByteArray Host = QUrl::toAce(FHost);
  if (Host.isEmpty())
  {
    Host = FHost.toUtf8();
  }
  if (Host.contains(':'))
  {
    Host = '[' + Host + ']';
  }
  if (FPort != DefaultPort())
  {
    Host += ':' + QByteArray::number(FPort);
  }
  return Host;
}

QByteArray WebDavFileSystem::UrlBytes(const QString & Path) const
{
  return (FTls ? QByteArray("https://") : QByteArray("http://")) + HostPort() + PathEscape(Path);
}

QString WebDavFileSystem::AbsolutePath(const QString & Path) const
{
  if (Path.startsWith(QLatin1Char('/')))
  {
    return NormalizePath(Path);
  }
  QString Base = !FCurrentDirectory.isEmpty() ? FCurrentDirectory : HomeDirectoryOrRoot();
  return NormalizePath(Base + QLatin1Char('/') + Path);
}

QString WebDavFileSystem::DirectoryPath(const QString & Path)
{
  // TWebDAVFileSystem::DirectoryPath (FHasTrailingSlash): the trailing slash
  // really matters for some servers (mod_dav, nginx)
  return Path.endsWith(QLatin1Char('/')) ? Path : (Path + QLatin1Char('/'));
}

//---------------------------------------------------------------------------
// Connection

void WebDavFileSystem::ConfigureSocket(QSslSocket * Socket)
{
  // InitNeonSession (proxy) + InitSslSession (TLS versions)
  QNetworkProxy Proxy(QNetworkProxy::NoProxy);
  FViaHttpProxy = false;
  bool Local = (FHost == QStringLiteral("localhost")) || QHostAddress(FHost).isLoopback();
  if (!Local || FData.ProxyLocalhost)
  {
    switch (FData.ProxyMethod)
    {
      case pmSocks5:
        Proxy = QNetworkProxy(QNetworkProxy::Socks5Proxy, FData.ProxyHost, static_cast<quint16>(FData.ProxyPort),
          FData.ProxyUsername, FData.ProxyPassword);
        break;
      case pmHTTP:
        if (FTls)
        {
          // CONNECT tunnel
          Proxy = QNetworkProxy(QNetworkProxy::HttpProxy, FData.ProxyHost, static_cast<quint16>(FData.ProxyPort),
            FData.ProxyUsername, FData.ProxyPassword);
        }
        else
        {
          // requests with absolute URIs sent to the proxy (ne_session_proxy)
          FViaHttpProxy = true;
        }
        break;
      default:
        // SOCKS4, Telnet and local proxies are not supported by QtNetwork
        break;
    }
  }
  Socket->setProxy(Proxy);
  if (FData.TcpNoDelay)
  {
    Socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
  }
  // bounds memory use while a download is throttled
  Socket->setReadBufferSize(ReadBufferSize);
  if (FTls)
  {
    QSslConfiguration Config = QSslConfiguration::defaultConfiguration();
    if (FData.MinTlsVersion >= tls13)
    {
      Config.setProtocol(QSsl::TlsV1_3OrLater);
    }
    else if (FData.MaxTlsVersion <= tls12)
    {
      Config.setProtocol(QSsl::TlsV1_2);
    }
    else
    {
      Config.setProtocol(QSsl::SecureProtocols);
    }
    Socket->setSslConfiguration(Config);
    Socket->setPeerVerifyName(FHost);
    // Errors are evaluated after the handshake (VerifyCertificate); the
    // handshake runs within waitFor* on this thread, so the slot is called directly
    QObject::connect(Socket, &QSslSocket::sslErrors, Socket, [Socket](const QList<QSslError> &)
    {
      Socket->ignoreSslErrors();
    });
  }
}

void WebDavFileSystem::Connect()
{
  Disconnect();
  FSocket = std::make_unique<QSslSocket>();
  ConfigureSocket(FSocket.get());
  FCloseAfterResponse = false;
  QAbstractSocket::NetworkLayerProtocol Family =
    (FData.AddressFamily == afIPv4) ? QAbstractSocket::IPv4Protocol :
    ((FData.AddressFamily == afIPv6) ? QAbstractSocket::IPv6Protocol : QAbstractSocket::AnyIPProtocol);
  QString ConnectHost = FViaHttpProxy ? FData.ProxyHost : FHost;
  quint16 ConnectPort = static_cast<quint16>(FViaHttpProxy ? FData.ProxyPort : FPort);
  LogLine(QStringLiteral("Connecting to %1:%2 ...").arg(ConnectHost).arg(ConnectPort));
  if (FTls)
  {
    FSocket->connectToHostEncrypted(FHost, ConnectPort, FHost, QIODevice::ReadWrite, Family);
  }
  else
  {
    FSocket->connectToHost(ConnectHost, ConnectPort, QIODevice::ReadWrite, Family);
  }
  if (!FSocket->waitForConnected(TimeoutMs()))
  {
    QAbstractSocket::SocketError Error = FSocket->error();
    QString ErrorString = FSocket->errorString();
    bool Proxied = FViaHttpProxy || (FSocket->proxy().type() != QNetworkProxy::NoProxy);
    Disconnect();
    // CheckNeonStatus: NE_LOOKUP, NE_TIMEOUT, NE_PROXYAUTH, NE_CONNECT
    switch (Error)
    {
      case QAbstractSocket::HostNotFoundError:
        throw ExtException(Tr(NET_TRANSL_HOST_NOT_EXIST2).arg(ConnectHost), Tr(NE_COULD_NOT_RESOLVE).arg(ConnectHost, ErrorString));
      case QAbstractSocket::SocketTimeoutError:
        throw ExtException(Tr(NET_TRANSL_TIMEOUT2).arg(ConnectHost),
          Tr(Proxied ? NE_COULD_NOT_CONNECT_PROXY : NE_COULD_NOT_CONNECT).arg(QStringLiteral("connection timed out")));
      case QAbstractSocket::ProxyAuthenticationRequiredError:
        throw ExtException(Tr(PROXY_AUTHENTICATION_FAILED), ErrorString);
      default:
        throw ExtException(Tr(CONNECTION_FAILED), Tr(Proxied ? NE_COULD_NOT_CONNECT_PROXY : NE_COULD_NOT_CONNECT).arg(ErrorString));
    }
  }
  if (FTls)
  {
    if (!FSocket->waitForEncrypted(TimeoutMs()))
    {
      QString ErrorString = FSocket->errorString();
      Disconnect();
      throw ExtException(Tr(NE_SSL_HANDSHAKE_FAILED).arg(ErrorString));
    }
    VerifyCertificate(FSocket->sslHandshakeErrors());
    switch (FSocket->sessionProtocol())
    {
      case QSsl::TlsV1_2: FTlsVersion = QStringLiteral("TLSv1.2"); break;
      case QSsl::TlsV1_3: FTlsVersion = QStringLiteral("TLSv1.3"); break;
      default: FTlsVersion.clear(); break;
    }
    FCipher = FSocket->sessionCipher().name();
  }
}

void WebDavFileSystem::Disconnect()
{
  if (FSocket)
  {
    FSocket->abort();
    FSocket.reset();
  }
  FCloseAfterResponse = false;
}

bool WebDavFileSystem::ConnectionUsable()
{
  if (!FSocket || (FSocket->state() != QAbstractSocket::ConnectedState) || (FSocket->bytesAvailable() > 0))
  {
    return false;
  }
  // notices a connection closed by the server in the meantime (keep-alive timeout)
  FSocket->waitForReadyRead(0);
  return (FSocket->state() == QAbstractSocket::ConnectedState) && (FSocket->bytesAvailable() == 0);
}

void WebDavFileSystem::VerifyCertificate(const QList<QSslError> & Errors)
{
  // TWebDAVFileSystem::NeonServerSSLCallback -> TTerminal::VerifyOrConfirmHttpCertificate;
  // the failures are mapped as neon's verify_callback does
  FCertificate = FSocket->peerCertificate();
  FCertificateFingerprintSHA256 = QString::fromLatin1(FCertificate.digest(QCryptographicHash::Sha256).toHex(':'));
  FCertificateFingerprintSHA1 = QString::fromLatin1(FCertificate.digest(QCryptographicHash::Sha1).toHex(':'));
  QList<QSslCertificate> Chain = FSocket->peerCertificateChain();
  int Failures = 0;
  for (const QSslError & Error : Errors)
  {
    qsizetype Depth = Error.certificate().isNull() ? 0 : Chain.indexOf(Error.certificate());
    switch (Error.error())
    {
      case QSslError::UnableToGetIssuerCertificate:
      case QSslError::UnableToGetLocalIssuerCertificate:
      case QSslError::SelfSignedCertificateInChain:
      case QSslError::SelfSignedCertificate:
      case QSslError::CertificateUntrusted:
      case QSslError::UnableToVerifyFirstCertificate:
        Failures |= SslUntrusted;
        break;
      case QSslError::CertificateNotYetValid:
        Failures |= (Depth > 0) ? SslBadChain : SslNotYetValid;
        break;
      case QSslError::CertificateExpired:
        Failures |= (Depth > 0) ? SslBadChain : SslExpired;
        break;
      case QSslError::HostNameMismatch:
        Failures |= SslIdMismatch;
        break;
      default:
      {
        // A failure that cannot be presented to the user is a hard failure
        QString ErrorString = Error.errorString();
        Disconnect();
        throw ExtException(Tr(NE_SSL_HANDSHAKE_FAILED).arg(Tr(NE_VERIFY_ERROR).arg(ErrorString)));
      }
    }
  }
  if (Failures == 0)
  {
    return;
  }
  QString Key = QStringLiteral("%1:%2=%3").arg(FHost).arg(FPort).arg(FCertificateFingerprintSHA256);
  bool OtherAccepted = false;
  {
    QMutexLocker Lock(&AcceptedCertificatesMutex);
    if (AcceptedCertificates.contains(Key))
    {
      return;
    }
    OtherAccepted = HasAcceptedCertificate(AcceptedCertificates, FHost, FPort);
  }
  if (FAcceptedOnce.contains(Key))
  {
    return;
  }
  // As with an SSH host key: a fingerprint in a URL does not outrank the
  // certificate accepted for this server before, a different one is a question
  OtherAccepted = OtherAccepted || HasAcceptedCertificate(FAcceptedOnce, FHost, FPort);
  if (!(FData.HostKeyFromUrl && OtherAccepted) &&
      CertificateMatchesSessionData(FData.TrustedHostKey(), FCertificateFingerprintSHA1, FCertificateFingerprintSHA256))
  {
    return;
  }
  // CertificateSummary + TTerminal::ConfirmCertificate
  QString Details = Tr(CERT_TEXT2).arg(
    FormatContact(FCertificate, true), FormatContact(FCertificate, false),
    QLocale().toString(FCertificate.effectiveDate().toLocalTime(), QLocale::ShortFormat),
    QLocale().toString(FCertificate.expiryDate().toLocalTime(), QLocale::ShortFormat),
    FCertificateFingerprintSHA256, FCertificateFingerprintSHA1,
    (Failures == 0) ? Tr(CERT_OK) : CertificateFailuresErrorStr(Failures, FHost));
  unsigned int Answer = FUI->QueryUser(Tr(VERIFY_CERT_PROMPT3).arg(Details), QString(), qaYes | qaNo | qaCancel);
  if (Answer == qaYes)
  {
    QMutexLocker Lock(&AcceptedCertificatesMutex);
    AcceptedCertificates.insert(Key);
  }
  else if (Answer == qaNo)
  {
    FAcceptedOnce.insert(Key);
  }
  else
  {
    Disconnect();
    throw ExtException(Tr(NE_VERIFY_FAILED).arg(VerifyFailureReasons(Failures)));
  }
}

bool WebDavFileSystem::WaitForData(FileOperationProgress * Progress, const char * Doing, qint64 Have)
{
  // Waits until more than Have bytes can be read; false when the connection was closed
  QElapsedTimer Idle;
  Idle.start();
  while (FSocket->bytesAvailable() <= Have)
  {
    if (Progress != nullptr)
    {
      Progress->CheckCancel();
    }
    if (FSocket->state() != QAbstractSocket::ConnectedState)
    {
      return false;
    }
    if (!FSocket->waitForReadyRead(100) && (FSocket->state() == QAbstractSocket::ConnectedState) &&
        (Idle.elapsed() >= TimeoutMs()))
    {
      Disconnect();
      // NE_TIMEOUT
      throw ExtException(Tr(NET_TRANSL_TIMEOUT2).arg(FHost), QStringLiteral("%1: connection timed out").arg(QString::fromLatin1(Doing)));
    }
  }
  return true;
}

[[noreturn]] void WebDavFileSystem::ReadError(const char * Doing)
{
  // aborted(): NE_SOCK_CLOSED / socket error
  QString Reason = QStringLiteral("connection was closed by server");
  if (FSocket && (FSocket->error() != QAbstractSocket::RemoteHostClosedError) &&
      (FSocket->error() != QAbstractSocket::UnknownSocketError))
  {
    Reason = FSocket->errorString();
  }
  Disconnect();
  throw ExtException(QStringLiteral("%1: %2").arg(QString::fromLatin1(Doing), Reason));
}

void WebDavFileSystem::WriteAll(const QByteArray & Data, FileOperationProgress * Progress)
{
  const char * Doing = (Progress != nullptr) ? NE_SEND_BODY : NE_SEND_REQUEST;
  if (!Data.isEmpty() && (FSocket->write(Data) != Data.size()))
  {
    throw ExtException(QStringLiteral("%1: %2").arg(QString::fromLatin1(Doing), FSocket->errorString()));
  }
  QElapsedTimer Idle;
  Idle.start();
  qint64 Pending = FSocket->bytesToWrite() + FSocket->encryptedBytesToWrite();
  while (Pending > ((Progress != nullptr) ? UploadHighWater : 0))
  {
    if (Progress != nullptr)
    {
      Progress->CheckCancel();
      if (FSocket->bytesAvailable() > 0)
      {
        // early response, see SendRequest
        return;
      }
    }
    if (FSocket->state() != QAbstractSocket::ConnectedState)
    {
      throw ExtException(QStringLiteral("%1: %2").arg(QString::fromLatin1(Doing), FSocket->errorString()));
    }
    FSocket->waitForBytesWritten(100);
    qint64 Now = FSocket->bytesToWrite() + FSocket->encryptedBytesToWrite();
    if (Now < Pending)
    {
      Idle.restart();
    }
    else if (Idle.elapsed() > TimeoutMs())
    {
      throw ExtException(Tr(NET_TRANSL_TIMEOUT2).arg(FHost), QStringLiteral("%1: connection timed out").arg(QString::fromLatin1(Doing)));
    }
    Pending = Now;
  }
}

QByteArray WebDavFileSystem::ReadLine(FileOperationProgress * Progress, const char * Doing, bool StatusLine)
{
  for (;;)
  {
    if (FSocket->canReadLine())
    {
      QByteArray Line = FSocket->readLine();
      if (Line.size() > MaxLineLength)
      {
        throw ExtException(QStringLiteral("Response header too long"));
      }
      while (Line.endsWith('\n') || Line.endsWith('\r'))
      {
        Line.chop(1);
      }
      return Line;
    }
    qint64 Have = FSocket->bytesAvailable();
    if (Have >= MaxLineLength)
    {
      throw ExtException(QStringLiteral("Response header too long"));
    }
    if (StatusLine && (Have > 0))
    {
      FResponseStarted = true;
    }
    if (!WaitForData(Progress, Doing, Have))
    {
      ReadError(Doing);
    }
    if (StatusLine)
    {
      FResponseStarted = true;
    }
  }
}

//---------------------------------------------------------------------------
// HTTP requests

WebDavFileSystem::Request WebDavFileSystem::MakeRequest(const QByteArray & Method, const QString & Path)
{
  Request Result;
  Result.Method = Method;
  Result.Path = Path;
  return Result;
}

QByteArray WebDavFileSystem::RequestTarget(const Request & Req) const
{
  QByteArray Result = PathEscape(Req.Path);
  if (!Req.Query.isEmpty())
  {
    Result += '?' + Req.Query;
  }
  if (FViaHttpProxy)
  {
    Result.prepend("http://" + HostPort());
  }
  return Result;
}

WebDavFileSystem::Response WebDavFileSystem::Dispatch(Request & Req)
{
  // ne_request_dispatch with the auth hooks: retried while the server asks
  // for (other) credentials and the user provides them
  for (;;)
  {
    bool CredentialsSent = (FAuth.Scheme != Authentication::None);
    Response Resp = Exchange(Req);
    if ((Resp.Status != 401) || !Authenticate(Resp, CredentialsSent))
    {
      return Resp;
    }
  }
}

WebDavFileSystem::Response WebDavFileSystem::Exchange(Request & Req)
{
  for (int Attempt = 1; ; Attempt++)
  {
    bool Reused = ConnectionUsable();
    if (!Reused)
    {
      Connect();
    }
    FResponseStarted = false;
    Response Resp;
    try
    {
      SendRequest(Req);
      ReadResponse(Req, Resp);
      return Resp;
    }
    catch (const AbortException &)
    {
      Disconnect();
      throw;
    }
    catch (const ExtException & E)
    {
      Disconnect();
      // A persistent connection closed by the server before it answered:
      // retry once on a new connection (as neon's NE_RETRY)
      if (Reused && !FResponseStarted && (Attempt == 1))
      {
        LogLine(QStringLiteral("Persistent connection was closed (%1), retrying").arg(E.Message()));
        continue;
      }
      throw;
    }
  }
}

void WebDavFileSystem::SendRequest(Request & Req)
{
  if ((Req.Progress != nullptr) && (Req.Counted != 0))
  {
    // the body is sent again (authentication, new connection)
    Req.Progress->AddTransferred(-Req.Counted);
    Req.Counted = 0;
  }
  QByteArray Head = Req.Method + ' ' + RequestTarget(Req) + " HTTP/1.1\r\n";
  Head += "Host: " + HostPort() + "\r\n";
  QString Version = QCoreApplication::applicationVersion();
  Head += "User-Agent: NixSCP" + (Version.isEmpty() ? QByteArray() : ('/' + Version.toLatin1())) + "\r\n";
  if (FAuth.Scheme != Authentication::None)
  {
    Head += "Authorization: " + AuthorizationHeader(Req) + "\r\n";
  }
  if (FViaHttpProxy && !FData.ProxyUsername.isEmpty())
  {
    Head += "Proxy-Authorization: Basic " + (FData.ProxyUsername + QLatin1Char(':') + FData.ProxyPassword).toUtf8().toBase64() + "\r\n";
  }
  for (const auto & Field : std::as_const(Req.Headers))
  {
    Head += Field.first + ": " + Field.second + "\r\n";
  }
  qint64 Length = (Req.File != nullptr) ? Req.FileLength : Req.Body.size();
  if ((Length > 0) || (Req.File != nullptr))
  {
    Head += "Content-Length: " + QByteArray::number(Length) + "\r\n";
  }
  Head += "\r\n";
  LogLine(QStringLiteral("> %1 %2").arg(QString::fromLatin1(Req.Method), QString::fromLatin1(RequestTarget(Req))));

  if (Req.File == nullptr)
  {
    WriteAll(Head + Req.Body, nullptr);
    return;
  }

  FileOperationProgress & Progress = *Req.Progress;
  WriteAll(Head, &Progress);
  if (!Req.File->seek(Req.FileOffset))
  {
    throw ExtException(QStringLiteral("Can't read file '%1'.").arg(Req.File->fileName()), Req.File->errorString());
  }
  qint64 Sent = 0;
  while (Sent < Req.FileLength)
  {
    // lets the socket notice incoming data without blocking
    FSocket->waitForReadyRead(0);
    if (FSocket->bytesAvailable() > 0)
    {
      // The server answered before getting the whole body (an error or an
      // authentication request): read the answer, the connection cannot be reused
      LogLine(QStringLiteral("Response received before the request body was sent completely"));
      FCloseAfterResponse = true;
      return;
    }
    Progress.CheckCancel();
    qint64 Want = std::min(TransferChunk, Req.FileLength - Sent);
    Want = static_cast<qint64>(Progress.AdjustToCPSLimit(static_cast<unsigned long>(Want)));
    Progress.CheckCancel();
    // a new buffer each time: QIODevice::write(QByteArray) may keep the
    // (implicitly shared) array itself instead of copying it
    QByteArray Buffer = Req.File->read(Want);
    if (Buffer.isEmpty())
    {
      throw ExtException(QStringLiteral("Can't read file '%1'.").arg(Req.File->fileName()),
        Req.File->atEnd() ? QStringLiteral("Premature EOF in request body file") : Req.File->errorString());
    }
    try
    {
      WriteAll(Buffer, &Progress);
    }
    catch (const AbortException &)
    {
      throw;
    }
    catch (const ExtException &)
    {
      if (FSocket && (FSocket->bytesAvailable() > 0))
      {
        FCloseAfterResponse = true;
        return;
      }
      throw;
    }
    qint64 Before = Sent;
    Sent += Buffer.size();
    qint64 Count = Sent - std::max(Before, Req.Uncounted);
    if (Count > 0)
    {
      Progress.AddTransferred(Count);
      Req.Counted += Count;
    }
  }
  WriteAll(QByteArray(), nullptr);
}

void WebDavFileSystem::ReadResponse(Request & Req, Response & Resp)
{
  bool Http11 = true;
  for (;;)
  {
    // read_status_line
    QByteArray Line = ReadLine(Req.Progress, NE_READ_STATUS_LINE, true);
    qsizetype Space = Line.indexOf(' ');
    if (!Line.startsWith("HTTP/") || (Space < 0))
    {
      throw ExtException(QStringLiteral("Could not parse response status line"));
    }
    QByteArray Version = Line.mid(5, Space - 5);
    if (!Version.startsWith("1."))
    {
      throw ExtException(QStringLiteral("Incompatible HTTP version"));
    }
    Http11 = (Version != "1.0");
    QByteArray Rest = Line.mid(Space + 1);
    bool Ok = false;
    Resp.Status = Rest.left(3).toInt(&Ok);
    if (!Ok || (Resp.Status < 100) || (Resp.Status > 999))
    {
      throw ExtException(QStringLiteral("Could not parse response status line"));
    }
    Resp.Reason = Rest.mid(4).trimmed();
    Resp.Headers.clear();
    for (;;)
    {
      QByteArray Field = ReadLine(Req.Progress, NE_READ_HEADERS);
      if (Field.isEmpty())
      {
        break;
      }
      if (((Field[0] == ' ') || (Field[0] == '\t')) && !Resp.Headers.isEmpty())
      {
        Resp.Headers.last().second += ' ' + Field.trimmed();
        continue;
      }
      qsizetype Colon = Field.indexOf(':');
      if (Colon <= 0)
      {
        continue;
      }
      if (Resp.Headers.size() >= MaxHeaderFields)
      {
        throw ExtException(QStringLiteral("Response exceeded maximum number of header fields"));
      }
      Resp.Headers.append(qMakePair(Field.left(Colon).trimmed().toLower(), Field.mid(Colon + 1).trimmed()));
    }
    // interim responses (100 Continue)
    if ((Resp.Status >= 200) || (Resp.Status == 101))
    {
      break;
    }
  }
  LogLine(QStringLiteral("< %1").arg(Resp.StatusText()));

  if (FInitialHandshake)
  {
    // NeonBodyAccepter: RemoteSystem
    QStringList RemoteSystem;
    for (const char * Name : {"x-engine", "x-dav-powered-by", "server", "x-powered-by"})
    {
      QByteArray Value = Resp.Header(Name);
      if (!Value.isEmpty())
      {
        RemoteSystem << QString::fromUtf8(Value);
      }
    }
    FRemoteSystem = RemoteSystem.join(QStringLiteral("; "));
  }

  QByteArray Connection = Resp.Header("connection").toLower();
  bool Persist = Http11 ? !Connection.contains("close") : Connection.contains("keep-alive");
  bool Stream = Req.OnHeaders && Req.OnHeaders(Resp);
  if ((Req.Method == "HEAD") || (Resp.Status == 204) || (Resp.Status == 304))
  {
    // no body
  }
  else if (Resp.Header("transfer-encoding").toLower().contains("chunked"))
  {
    ReadBody(Req, Resp, -1, true, Stream);
  }
  else if (!Resp.Header("content-length").isEmpty())
  {
    bool Ok = false;
    qint64 Length = Resp.Header("content-length").trimmed().toLongLong(&Ok);
    if (!Ok || (Length < 0))
    {
      throw ExtException(QStringLiteral("Invalid Content-Length in response"));
    }
    ReadBody(Req, Resp, Length, false, Stream);
  }
  else
  {
    // body delimited by the end of the connection
    Persist = false;
    ReadBody(Req, Resp, -1, false, Stream);
  }
  if (!Persist || FCloseAfterResponse)
  {
    Disconnect();
  }
}

void WebDavFileSystem::ReadBody(Request & Req, Response & Resp, qint64 Length, bool Chunked, bool Stream)
{
  QByteArray Buffer(static_cast<qsizetype>(TransferChunk), Qt::Uninitialized);
  if (Chunked)
  {
    for (;;)
    {
      QByteArray SizeLine = ReadLine(Req.Progress, NE_READ_CHUNK_SIZE);
      qsizetype Extension = SizeLine.indexOf(';');
      if (Extension >= 0)
      {
        SizeLine.truncate(Extension);
      }
      bool Ok = false;
      qint64 ChunkSize = SizeLine.trimmed().toLongLong(&Ok, 16);
      if (!Ok || (ChunkSize < 0))
      {
        throw ExtException(QStringLiteral("Could not parse chunk size"));
      }
      if (ChunkSize == 0)
      {
        // trailer fields
        while (!ReadLine(Req.Progress, NE_READ_HEADERS).isEmpty())
        {
        }
        break;
      }
      ReadBodyPart(Req, Resp, ChunkSize, Stream, Buffer);
      if (!ReadLine(Req.Progress, NE_READ_CHUNK_DELIMITER).isEmpty())
      {
        throw ExtException(QStringLiteral("Chunk delimiter was invalid"));
      }
    }
  }
  else
  {
    ReadBodyPart(Req, Resp, Length, Stream, Buffer);
  }
}

void WebDavFileSystem::ReadBodyPart(Request & Req, Response & Resp, qint64 Length, bool Stream, QByteArray & Buffer)
{
  // Length < 0: until the connection is closed
  qint64 Remaining = Length;
  while ((Length < 0) || (Remaining > 0))
  {
    if (!WaitForData(Req.Progress, NE_READ_BODY))
    {
      if (Length < 0)
      {
        break;
      }
      ReadError(NE_READ_BODY);
    }
    qint64 Want = std::min(TransferChunk, FSocket->bytesAvailable());
    if (Length >= 0)
    {
      Want = std::min(Want, Remaining);
    }
    if (Stream && (Req.Progress != nullptr))
    {
      // only what can be read now is taken from the speed limit budget
      Want = static_cast<qint64>(Req.Progress->AdjustToCPSLimit(static_cast<unsigned long>(Want)));
      Req.Progress->CheckCancel();
    }
    qint64 Read = FSocket->read(Buffer.data(), Want);
    if (Read <= 0)
    {
      ReadError(NE_READ_BODY);
    }
    if (Stream)
    {
      Req.OnData(Buffer.constData(), Read);
    }
    else
    {
      if (Resp.Body.size() + Read > MaxBodySize)
      {
        throw ExtException(QStringLiteral("Response too large"));
      }
      Resp.Body.append(Buffer.constData(), Read);
    }
    Remaining -= Read;
  }
}

//---------------------------------------------------------------------------
// Authentication

QList<WebDavFileSystem::Challenge> WebDavFileSystem::ParseChallenges(const QByteArray & Header)
{
  // WWW-Authenticate: one or more "scheme param=value, param="value"" challenges
  QList<Challenge> Result;
  qsizetype Pos = 0;
  qsizetype Size = Header.size();
  auto SkipSpace = [&]()
  {
    while ((Pos < Size) && ((Header[Pos] == ' ') || (Header[Pos] == '\t')))
    {
      Pos++;
    }
  };
  while (Pos < Size)
  {
    SkipSpace();
    while ((Pos < Size) && (Header[Pos] == ','))
    {
      Pos++;
      SkipSpace();
    }
    if (Pos >= Size)
    {
      break;
    }
    qsizetype Start = Pos;
    while ((Pos < Size) && !std::strchr(" \t,=\"", Header[Pos]))
    {
      Pos++;
    }
    QByteArray Token = Header.mid(Start, Pos - Start);
    SkipSpace();
    if ((Pos < Size) && (Header[Pos] == '=') && !Result.isEmpty() && !Token.isEmpty())
    {
      Pos++;
      SkipSpace();
      QByteArray Value;
      if ((Pos < Size) && (Header[Pos] == '"'))
      {
        Pos++;
        while ((Pos < Size) && (Header[Pos] != '"'))
        {
          if ((Header[Pos] == '\\') && (Pos + 1 < Size))
          {
            Pos++;
          }
          Value += Header[Pos];
          Pos++;
        }
        Pos++;
      }
      else
      {
        qsizetype ValueStart = Pos;
        while ((Pos < Size) && (Header[Pos] != ','))
        {
          Pos++;
        }
        Value = Header.mid(ValueStart, Pos - ValueStart).trimmed();
      }
      Result.last().Params.insert(Token.toLower(), Value);
    }
    else if ((Pos < Size) && (Header[Pos] == '='))
    {
      // token68 or a parameter without a challenge: skip
      while ((Pos < Size) && (Header[Pos] != ','))
      {
        Pos++;
      }
    }
    else if (!Token.isEmpty())
    {
      Challenge New;
      New.Scheme = Token.toLower();
      Result.append(New);
    }
    else
    {
      Pos++;
    }
  }
  return Result;
}

bool WebDavFileSystem::Authenticate(const Response & Resp, bool CredentialsSent)
{
  // ne_auth challenge handling + TWebDAVFileSystem::NeonRequestAuth
  QList<Challenge> Challenges;
  for (const QByteArray & Header : Resp.AllHeaders("www-authenticate"))
  {
    Challenges += ParseChallenges(Header);
  }
  const Challenge * Digest = nullptr;
  const Challenge * Basic = nullptr;
  for (const Challenge & C : std::as_const(Challenges))
  {
    if ((C.Scheme == "digest") && (Digest == nullptr))
    {
      QByteArray Algorithm = C.Params.value("algorithm", "MD5").toUpper();
      bool AlgorithmOk =
        (Algorithm == "MD5") || (Algorithm == "MD5-SESS") || (Algorithm == "SHA-256") || (Algorithm == "SHA-256-SESS");
      bool HasQop = C.Params.contains("qop");
      bool QopAuth = false;
      for (const QByteArray & Qop : C.Params.value("qop").split(','))
      {
        QopAuth = QopAuth || (Qop.trimmed().toLower() == "auth");
      }
      if (AlgorithmOk && C.Params.contains("nonce") && C.Params.contains("realm") &&
          (QopAuth || (!HasQop && FData.WebDavAuthLegacy)))
      {
        Digest = &C;
      }
      else
      {
        LogLine(QStringLiteral("Ignoring unsupported Digest challenge"));
      }
    }
    else if ((C.Scheme == "basic") && (Basic == nullptr) && C.Params.contains("realm"))
    {
      Basic = &C;
    }
  }
  const Challenge * Chosen = (Digest != nullptr) ? Digest : Basic;
  if (Chosen == nullptr)
  {
    LogLine(QStringLiteral("No supported authentication challenge"));
    return false;
  }

  auto InitDigest = [this](const Challenge & C)
  {
    FAuth.Scheme = Authentication::Digest;
    FAuth.Realm = C.Params.value("realm");
    FAuth.Nonce = C.Params.value("nonce");
    FAuth.Opaque = C.Params.value("opaque");
    FAuth.Algorithm = C.Params.value("algorithm", "MD5");
    QByteArray Upper = FAuth.Algorithm.toUpper();
    FAuth.Sha256 = Upper.startsWith("SHA-256");
    FAuth.Session = Upper.endsWith("-SESS");
    FAuth.Qop = C.Params.contains("qop");
    FAuth.CNonce = RandomHex(16);
    FAuth.NonceCount = 0;
    if (FAuth.Session)
    {
      QByteArray HA1 = DigestHash(FUserName.toUtf8() + ':' + FAuth.Realm + ':' + FPassword.toUtf8());
      FAuth.HA1 = DigestHash(HA1 + ':' + FAuth.Nonce + ':' + FAuth.CNonce);
    }
  };

  if (CredentialsSent && (Chosen == Digest) && (FAuth.Scheme == Authentication::Digest) &&
      (Digest->Params.value("stale").toLower() == "true"))
  {
    // the nonce expired, the password itself was accepted
    LogLine(QStringLiteral("Digest nonce is stale, retrying with a new one"));
    InitDigest(*Digest);
    return true;
  }
  if (CredentialsSent)
  {
    // NeonBodyAccepter: password authentication failed
    FPassword.clear();
    FPasswordKnown = false;
    FPasswordFailed = true;
  }
  FAuth = Authentication();

  // will ask for username only once
  if (FUserName.isEmpty())
  {
    if (!FData.UserName.isEmpty())
    {
      FUserName = FData.UserName;
    }
    else
    {
      QList<Prompt> Prompts{Prompt{QStringLiteral("&Username:"), true, QString()}};
      if (!FUI->PromptUser(UserInterface::pkUserName, FData.SessionName(), QString(), Prompts))
      {
        return false;
      }
      FUserName = Prompts[0].Result;
    }
  }
  if (!FPasswordKnown)
  {
    if (!FData.Password.isEmpty() && !FStoredPasswordTried)
    {
      FPassword = FData.Password;
      FStoredPasswordTried = true;
    }
    else
    {
      QList<Prompt> Prompts{Prompt{QStringLiteral("&Password: "), false, QString()}};
      if (!FUI->PromptUser(UserInterface::pkPassword, FData.SessionName(),
            FPasswordFailed ? Tr(FTP_ACCESS_DENIED) : QString(), Prompts))
      {
        return false;
      }
      FPassword = Prompts[0].Result;
    }
    FPasswordKnown = true;
  }

  if (Chosen == Digest)
  {
    InitDigest(*Digest);
  }
  else
  {
    FAuth.Scheme = Authentication::Basic;
    FAuth.Realm = Basic->Params.value("realm");
  }
  LogLine(QStringLiteral("Authenticating with %1").arg((Chosen == Digest) ? QStringLiteral("Digest") : QStringLiteral("Basic")));
  return true;
}

QByteArray WebDavFileSystem::DigestHash(const QByteArray & Data) const
{
  return QCryptographicHash::hash(Data, FAuth.Sha256 ? QCryptographicHash::Sha256 : QCryptographicHash::Md5).toHex();
}

QByteArray WebDavFileSystem::AuthorizationHeader(const Request & Req)
{
  QByteArray UserName = FUserName.toUtf8();
  if (FAuth.Scheme == Authentication::Basic)
  {
    return "Basic " + (UserName + ':' + FPassword.toUtf8()).toBase64();
  }
  // RFC 7616 / 2617 (2069 without qop)
  FAuth.NonceCount++;
  QByteArray Uri = RequestTarget(Req);
  QByteArray NonceCount = QByteArray::number(FAuth.NonceCount, 16).rightJustified(8, '0');
  QByteArray HA1 = FAuth.Session ? FAuth.HA1 : DigestHash(UserName + ':' + FAuth.Realm + ':' + FPassword.toUtf8());
  QByteArray HA2 = DigestHash(Req.Method + ':' + Uri);
  QByteArray Digest = FAuth.Qop ?
    DigestHash(HA1 + ':' + FAuth.Nonce + ':' + NonceCount + ':' + FAuth.CNonce + ":auth:" + HA2) :
    DigestHash(HA1 + ':' + FAuth.Nonce + ':' + HA2);
  QByteArray Result = "Digest username=" + QuoteHeaderValue(UserName) +
    ", realm=" + QuoteHeaderValue(FAuth.Realm) +
    ", nonce=" + QuoteHeaderValue(FAuth.Nonce) +
    ", uri=" + QuoteHeaderValue(Uri) +
    ", response=\"" + Digest + "\"" +
    ", algorithm=" + FAuth.Algorithm;
  if (!FAuth.Opaque.isEmpty())
  {
    Result += ", opaque=" + QuoteHeaderValue(FAuth.Opaque);
  }
  if (FAuth.Qop)
  {
    Result += ", qop=auth, nc=" + NonceCount + ", cnonce=\"" + FAuth.CNonce + "\"";
  }
  return Result;
}

//---------------------------------------------------------------------------
// WebDAV helpers

bool WebDavFileSystem::IsRedirect(const Response & Resp)
{
  // ne_redirect
  return ((Resp.Status == 301) || (Resp.Status == 302) || (Resp.Status == 303) ||
          (Resp.Status == 307) || (Resp.Status == 308)) &&
    !Resp.Header("location").isEmpty();
}

QString WebDavFileSystem::RedirectPath(const Response & Resp, const Request & Req, QByteArray * Query)
{
  // TWebDAVFileSystem::GetRedirectUrl
  QByteArray Location = Resp.Header("location").trimmed();
  QUrl Base = QUrl::fromEncoded(UrlBytes(Req.Path));
  QUrl Target = Base.resolved(QUrl::fromEncoded(Location, QUrl::TolerantMode));
  LogLine(QStringLiteral("Redirected to \"%1\".").arg(Target.toString()));
  if (!Target.isValid() || Target.host().isEmpty())
  {
    throw ExtException(Tr(INVALID_URL).arg(QString::fromUtf8(Location)));
  }
  bool TargetTls = (Target.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0);
  if (FTls && !TargetTls)
  {
    throw ExtException(Tr(UNENCRYPTED_REDIRECT));
  }
  int TargetDefaultPort = TargetTls ? HTTPSPortNumber : HTTPPortNumber;
  int TargetPort = Target.port(TargetDefaultPort);
  if ((Target.host().compare(QUrl(Base).host(), Qt::CaseInsensitive) != 0) || (TargetPort != FPort) || (TargetTls != FTls))
  {
    QString OtherHost = Target.host();
    if (TargetPort != TargetDefaultPort)
    {
      OtherHost += QStringLiteral(":%1").arg(TargetPort);
    }
    throw ExtException(Tr(WEBDAV_CROSS_DOMAIN_REDIR).arg(OtherHost));
  }
  if (Query != nullptr)
  {
    *Query = Target.query(QUrl::FullyEncoded).toLatin1();
  }
  QString Path = PathUnescape(Target.path(QUrl::FullyEncoded).toLatin1());
  return Path.isEmpty() ? QStringLiteral("/") : Path;
}

void WebDavFileSystem::CheckStatus(const Response & Resp, bool Multistatus)
{
  // CheckNeonStatus (NE_AUTH, NE_PROXYAUTH, NE_REDIRECT, NE_ERROR) and the
  // 207 handling of ne_simple_request
  if (Resp.Status == 401)
  {
    throw ExtException(Tr(AUTHENTICATION_FAILED), Resp.StatusText());
  }
  if (Resp.Status == 407)
  {
    throw ExtException(Tr(PROXY_AUTHENTICATION_FAILED), Resp.StatusText());
  }
  if (IsRedirect(Resp))
  {
    throw ExtException(Tr(REQUEST_REDIRECTED).arg(QString::fromUtf8(Resp.Header("location"))));
  }
  if (Multistatus && (Resp.Status == 207))
  {
    QStringList Errors;
    ParseMultistatus(Resp.Body, QStringLiteral("/"), &Errors);
    if (!Errors.isEmpty())
    {
      throw ExtException(Errors.join(QStringLiteral(", ")));
    }
  }
  if (Resp.Class() != 2)
  {
    throw ExtException(Resp.StatusText());
  }
}

QList<WebDavFileSystem::Resource> WebDavFileSystem::ParseMultistatus(const QByteArray & Xml, const QString & RequestPath, QStringList * Errors,
  bool LiberalEscaping)
{
  // ne_207.c + ne_props.c: properties of 2xx propstat elements only
  QList<Resource> Result;
  QXmlStreamReader Reader(Xml);
  Resource Current;
  bool InResponse = false;
  bool InPropstat = false;
  QString ResponseStatus;
  QString ResponseDescription;
  QString PropstatStatus;
  QString PropstatDescription;
  QMap<QString, QString> PropstatProps;
  bool PropstatCollection = false;
  auto AddError = [&](const QString & Status, const QString & Description)
  {
    int Code = StatusCode(Status);
    if ((Errors == nullptr) || (Code == 0) || ((Code / 100) == 2) || (Code == 424))
    {
      return;
    }
    QString Text = QStringLiteral("%1: %2").arg(Current.Path, Status.simplified().section(QLatin1Char(' '), 1));
    if (!Description.isEmpty())
    {
      Text += QStringLiteral(" (%1)").arg(Description.trimmed());
    }
    Errors->append(Text);
  };
  while (!Reader.atEnd())
  {
    Reader.readNext();
    if (Reader.isStartElement())
    {
      bool Dav = (Reader.namespaceUri() == DavNamespace);
      QStringView Name = Reader.name();
      if (Dav && (Name == u"response"))
      {
        InResponse = true;
        InPropstat = false;
        Current = Resource();
        ResponseStatus.clear();
        ResponseDescription.clear();
      }
      else if (!InResponse)
      {
        continue;
      }
      else if (Dav && (Name == u"href") && !InPropstat)
      {
        Current.Path = HrefPath(Reader.readElementText(QXmlStreamReader::IncludeChildElements), RequestPath, LiberalEscaping);
      }
      else if (Dav && (Name == u"propstat"))
      {
        InPropstat = true;
        PropstatStatus.clear();
        PropstatDescription.clear();
        PropstatProps.clear();
        PropstatCollection = false;
      }
      else if (Dav && (Name == u"status"))
      {
        (InPropstat ? PropstatStatus : ResponseStatus) = Reader.readElementText(QXmlStreamReader::IncludeChildElements);
      }
      else if (Dav && (Name == u"responsedescription"))
      {
        (InPropstat ? PropstatDescription : ResponseDescription) = Reader.readElementText(QXmlStreamReader::IncludeChildElements);
      }
      else if (Dav && (Name == u"prop") && InPropstat)
      {
        while (Reader.readNextStartElement())
        {
          QString Key = Reader.namespaceUri().toString() + Reader.name().toString();
          if (Key == DavNamespace + QStringLiteral("resourcetype"))
          {
            while (Reader.readNextStartElement())
            {
              if ((Reader.namespaceUri() == DavNamespace) && (Reader.name() == u"collection"))
              {
                PropstatCollection = true;
              }
              Reader.skipCurrentElement();
            }
            PropstatProps.insert(Key, QString());
          }
          else
          {
            PropstatProps.insert(Key, Reader.readElementText(QXmlStreamReader::IncludeChildElements));
          }
        }
      }
    }
    else if (Reader.isEndElement() && InResponse && (Reader.namespaceUri() == DavNamespace))
    {
      if (Reader.name() == u"propstat")
      {
        int Code = StatusCode(PropstatStatus);
        if ((Code == 0) || ((Code / 100) == 2))
        {
          for (auto It = PropstatProps.cbegin(); It != PropstatProps.cend(); ++It)
          {
            Current.Props.insert(It.key(), It.value());
          }
          Current.Collection = Current.Collection || PropstatCollection;
        }
        else
        {
          AddError(PropstatStatus, PropstatDescription);
        }
        InPropstat = false;
      }
      else if (Reader.name() == u"response")
      {
        int Code = StatusCode(ResponseStatus);
        if ((Code != 0) && ((Code / 100) != 2))
        {
          Current.Failed = true;
          AddError(ResponseStatus, ResponseDescription);
        }
        Result.append(Current);
        InResponse = false;
      }
    }
  }
  if (Reader.hasError())
  {
    throw ExtException(QStringLiteral("XML parse error at line %1: %2").arg(Reader.lineNumber()).arg(Reader.errorString()));
  }
  return Result;
}

RemoteFile WebDavFileSystem::FileFromResource(const Resource & Res, const QString & Name) const
{
  // TWebDAVFileSystem::ParsePropResultSet
  RemoteFile File;
  File.FileName = Name;
  // Some servers do not use DAV:collection tag, but indicate the folder by trailing slash only.
  bool Collection = ((Res.Path.size() > 1) && Res.Path.endsWith(QLatin1Char('/'))) || Res.Collection;
  File.FileType = Collection ? RemoteFile::Directory : RemoteFile::File;
  File.IsDirectory = Collection;
  // some servers, for example iFiles, do not provide "getcontentlength" for folders
  File.Size = Res.Props.value(DavNamespace + QStringLiteral("getcontentlength")).trimmed().toLongLong();
  // We've seen a server that does not set "getlastmodified" for the "this" folder entry.
  File.Modification = ParseHttpDate(Res.Props.value(DavNamespace + QStringLiteral("getlastmodified")));
  // MS extension (draft-hopmann-collection-props-00); otherwise TRemoteFile::GetIsHidden
  auto Hidden = Res.Props.constFind(DavNamespace + QStringLiteral("ishidden"));
  if (Hidden != Res.Props.cend())
  {
    File.IsHidden = (Hidden->trimmed().toInt() != 0);
  }
  else
  {
    File.IsHidden = Name.startsWith(QLatin1Char('.'));
  }
  File.Owner = Res.Props.value(DavNamespace + QStringLiteral("owner")).trimmed();
  return File;
}

WebDavFileSystem::Response WebDavFileSystem::Propfind(QString & Path, int Depth)
{
  // ne_propfind_allprop / ne_simple_propfind with NULL properties
  Request Req = MakeRequest("PROPFIND", Path);
  Req.Headers << qMakePair(QByteArray("Depth"), QByteArray::number(Depth))
    << qMakePair(QByteArray("Content-Type"), QByteArray("application/xml"));
  Req.Body = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n<propfind xmlns=\"DAV:\"><allprop/></propfind>\r\n";
  Response Resp = Dispatch(Req);
  if (IsRedirect(Resp))
  {
    // IsValidRedirect: one step only, mainly for "folder" => "folder/" of Apache/mod_dav
    QString Redirected = RedirectPath(Resp, Req, nullptr);
    if (!Redirected.isEmpty() && (Redirected != Path))
    {
      Path = Redirected;
      Req.Path = Path;
      Resp = Dispatch(Req);
    }
  }
  return Resp;
}

bool WebDavFileSystem::ReadFileInternal(const QString & Abs, RemoteFile & File, Response * Failure)
{
  // TWebDAVFileSystem::CustomReadFileInternal
  QString Path = Abs;
  Response Resp = Propfind(Path, 0);
  if (Resp.Class() != 2)
  {
    if (Failure != nullptr)
    {
      *Failure = Resp;
    }
    return false;
  }
  QList<Resource> Resources = ParseMultistatus(Resp.Body, Path, nullptr, FData.WebDavLiberalEscaping);
  const Resource * Found = nullptr;
  for (const Resource & Res : std::as_const(Resources))
  {
    if (Res.Failed)
    {
      continue;
    }
    if (ExcludeTrailingSlash(Res.Path) == ExcludeTrailingSlash(Path))
    {
      Found = &Res;
      break;
    }
    if (Found == nullptr)
    {
      Found = &Res;
    }
  }
  if (Found == nullptr)
  {
    if (Failure != nullptr)
    {
      *Failure = Resp;
    }
    return false;
  }
  QString Name = (Abs == QStringLiteral("/")) ? Abs : UnixPath::ExtractFileName(ExcludeTrailingSlash(Abs));
  File = FileFromResource(*Found, Name);
  return true;
}

void WebDavFileSystem::ParseCapabilities(const Response & Resp)
{
  // ExchangeCapabilities: DAV header of the OPTIONS response (parse_dav_header)
  static const char * const Known[] = {
    "1", "2", "3", "<http://apache.org/dav/propset/fs/1>", "access-control", "version-control",
    "checkout-in-place", "version-history", "workspace", "update", "label", "working-resource",
    "merge", "baseline", "version-controlled-collection", "extended-mkcol"};
  FCapabilities.clear();
  for (const QByteArray & Header : Resp.AllHeaders("dav"))
  {
    for (const QByteArray & Token : Header.split(','))
    {
      QString Capability = QString::fromLatin1(Token.trimmed());
      for (const char * Name : Known)
      {
        if ((Capability == QLatin1String(Name)) && !FCapabilities.contains(Capability))
        {
          FCapabilities << Capability;
        }
      }
    }
  }
  if (!FCapabilities.isEmpty())
  {
    LogLine(QStringLiteral("Server capabilities: %1").arg(FCapabilities.join(QStringLiteral(", "))));
  }
}

//---------------------------------------------------------------------------
// Session

void WebDavFileSystem::Open()
{
  // TWebDAVFileSystem::Open + NeonClientOpenSessionInternal
  Disconnect();
  FOpened = false;
  FTls = (FData.Ftps != ftpsNone);
  FHost = FData.HostName;
  FPort = (FData.PortNumber > 0) ? FData.PortNumber : DefaultPort();
  FHomeDirectory.clear();
  FCurrentDirectory.clear();
  FCapabilities.clear();
  FRemoteSystem.clear();
  FUserName.clear();
  FPassword.clear();
  FPasswordKnown = false;
  FStoredPasswordTried = false;
  FPasswordFailed = false;
  FAuth = Authentication();
  FTlsVersion.clear();
  FCipher.clear();

  QString Path = FData.RemoteDirectory.trimmed();
  if (!Path.startsWith(QLatin1Char('/')))
  {
    Path.prepend(QLatin1Char('/'));
  }
  try
  {
    QList<QByteArray> AttemptedUrls{UrlBytes(Path)};
    for (;;)
    {
      Request Req = MakeRequest("OPTIONS", Path);
      Response Resp;
      FInitialHandshake = true;
      try
      {
        Resp = Dispatch(Req);
      }
      catch (...)
      {
        FInitialHandshake = false;
        throw;
      }
      FInitialHandshake = false;
      if (IsRedirect(Resp))
      {
        QString Redirected = RedirectPath(Resp, Req, nullptr);
        // CheckRedirectLoop
        QByteArray RedirectUrl = UrlBytes(Redirected);
        if (AttemptedUrls.size() > MaxRedirectAttempts)
        {
          throw ExtException(Tr(TOO_MANY_REDIRECTS));
        }
        if (AttemptedUrls.contains(RedirectUrl))
        {
          throw ExtException(Tr(REDIRECT_LOOP));
        }
        AttemptedUrls << RedirectUrl;
        Path = Redirected;
        continue;
      }
      CheckStatus(Resp, false);
      ParseCapabilities(Resp);
      break;
    }
  }
  catch (const AbortException &)
  {
    Disconnect();
    throw;
  }
  catch (const ExtException & E)
  {
    // FTerminal->FatalError(&E, LoadStr(CONNECTION_FAILED))
    Disconnect();
    QString More = (E.Message() == Tr(CONNECTION_FAILED)) ? E.MoreMessages() : JoinMessages(E.Message(), E.MoreMessages());
    throw FatalException(Tr(CONNECTION_FAILED), More);
  }
  // The (redirected) URL path is the initial directory (ReadCurrentDirectory).
  // With a remote directory in the site settings, the session starts there
  // and falls back to the root (TWebDAVFileSystem::HomeDirectory) when that
  // cannot be opened.
  FCurrentDirectory = NormalizePath(Path);
  FHomeDirectory = FData.RemoteDirectory.trimmed().isEmpty() ? FCurrentDirectory : QStringLiteral("/");
  FOpened = true;
}

void WebDavFileSystem::Close()
{
  Disconnect();
  FOpened = false;
}

QString WebDavFileSystem::HomeDirectoryOrRoot() const
{
  return FHomeDirectory.isEmpty() ? QStringLiteral("/") : FHomeDirectory;
}

QString WebDavFileSystem::HomeDirectory()
{
  return HomeDirectoryOrRoot();
}

QString WebDavFileSystem::CanonicalizePath(const QString & APath)
{
  QString Abs = AbsolutePath(APath);
  QString Result;
  WithErrorMessage(Tr(CHANGE_DIR_ERROR).arg(APath), [&]()
  {
    // TWebDAVFileSystem::ChangeDirectory: to verify existence of directory try to open it
    QString Path = DirectoryPath(Abs);
    Response Resp = Propfind(Path, 0);
    if (Resp.Class() == 2)
    {
      Result = NormalizePath(Path);
      FCurrentDirectory = Result;
      return;
    }
    if ((Resp.Status == 404) && (Abs != QStringLiteral("/")))
    {
      // not a directory, maybe a file
      QString FilePath = Abs;
      Response FileResp = Propfind(FilePath, 0);
      if (FileResp.Class() == 2)
      {
        Result = NormalizePath(FilePath);
        return;
      }
    }
    CheckStatus(Resp, false);
  });
  return Result;
}

RemoteFileList WebDavFileSystem::ReadDirectory(const QString & Directory)
{
  QString Abs = AbsolutePath(Directory);
  RemoteFileList Result;
  WithErrorMessage(Tr(LIST_DIR_ERROR).arg(Directory), [&]()
  {
    // TWebDAVFileSystem::ReadDirectory + NeonPropsResult
    QString Path = DirectoryPath(Abs);
    Response Resp = Propfind(Path, 1);
    CheckStatus(Resp, false);
    QString Base = DirectoryPath(Path);
    for (const Resource & Res : ParseMultistatus(Resp.Body, Path, nullptr, FData.WebDavLiberalEscaping))
    {
      if (Res.Failed)
      {
        continue;
      }
      if (DirectoryPath(Res.Path) == Base)
      {
        // the directory itself (PARENTDIRECTORY in WinSCP)
        continue;
      }
      if (!Res.Path.startsWith(Base))
      {
        LogLine(QStringLiteral("Discarding entry \"%1\" because it is not descendant of directory \"%2\".").arg(Res.Path, Base));
        continue;
      }
      QString Name = ExcludeTrailingSlash(Res.Path).mid(Base.size());
      if (!IsValidListingName(Name))
      {
        LogLine(QStringLiteral("Discarding entry \"%1\" because it is not direct child of directory \"%2\".").arg(Res.Path, Base));
        continue;
      }
      Result.append(FileFromResource(Res, Name));
    }
  });
  return Result;
}

RemoteFile WebDavFileSystem::ReadFile(const QString & Path)
{
  QString Abs = AbsolutePath(Path);
  RemoteFile File;
  WithErrorMessage(Tr(CANT_GET_ATTRS).arg(Path), [&]()
  {
    Response Failure;
    if (!ReadFileInternal(Abs, File, &Failure))
    {
      CheckStatus(Failure, false);
      throw ExtException(Tr(FILE_NOT_EXISTS).arg(Path));
    }
  });
  return File;
}

bool WebDavFileSystem::FileExists(const QString & Path, RemoteFile * File)
{
  // As TWebDAVFileSystem::Source: errors of the check are ignored
  QString Abs = AbsolutePath(Path);
  RemoteFile Found;
  bool Result = false;
  try
  {
    Result = ReadFileInternal(Abs, Found, nullptr);
  }
  catch (const AbortException &)
  {
    throw;
  }
  catch (const FatalException &)
  {
    throw;
  }
  catch (const ExtException & E)
  {
    LogLine(QStringLiteral("Cannot check existence of \"%1\": %2").arg(Abs, JoinMessages(E.Message(), E.MoreMessages())));
    Result = false;
  }
  if (Result && (File != nullptr))
  {
    *File = Found;
  }
  return Result;
}

//---------------------------------------------------------------------------
// File operations

void WebDavFileSystem::CreateDirectory(const QString & Path, int /*Mode*/)
{
  QString Abs = AbsolutePath(Path);
  WithErrorMessage(Tr(CREATE_DIR_ERROR).arg(Path), [&]()
  {
    // ne_mkcol: collections are created with a trailing slash
    Request Req = MakeRequest("MKCOL", DirectoryPath(Abs));
    CheckStatus(Dispatch(Req), true);
  });
}

void WebDavFileSystem::DeleteFile(const QString & Path)
{
  QString Abs = AbsolutePath(Path);
  WithErrorMessage(Tr(DELETE_FILE_ERROR).arg(Path), [&]()
  {
    Request Req = MakeRequest("DELETE", Abs);
    CheckStatus(Dispatch(Req), true);
  });
}

void WebDavFileSystem::RemoveDirectory(const QString & Path)
{
  // DELETE of a collection is always recursive (RFC 4918, 9.6.1)
  QString Abs = AbsolutePath(Path);
  WithErrorMessage(Tr(DELETE_FILE_ERROR).arg(Path), [&]()
  {
    Request Req = MakeRequest("DELETE", DirectoryPath(Abs));
    CheckStatus(Dispatch(Req), true);
  });
}

void WebDavFileSystem::RenameFile(const QString & From, const QString & To)
{
  QString AbsFrom = AbsolutePath(From);
  QString AbsTo = AbsolutePath(To);
  WithErrorMessage(Tr(RENAME_FILE_ERROR).arg(UnixPath::ExtractFileName(From), UnixPath::ExtractFileName(To)), [&]()
  {
    // ne_move without overwriting (an existing target is an error, as with SFTP)
    auto Move = [this](const QString & Source, const QString & Target)
    {
      Request Req = MakeRequest("MOVE", Source);
      Req.Headers << qMakePair(QByteArray("Destination"), UrlBytes(Target))
        << qMakePair(QByteArray("Overwrite"), QByteArray("F"));
      return Dispatch(Req);
    };
    QString Source = AbsFrom;
    Response Resp = Move(Source, AbsTo);
    if (IsRedirect(Resp))
    {
      Request Original = MakeRequest("MOVE", Source);
      QString Redirected = RedirectPath(Resp, Original, nullptr);
      if (!Redirected.isEmpty() && (Redirected != Source))
      {
        Source = Redirected;
        Resp = Move(Source, AbsTo);
      }
    }
    if ((Resp.Status == 409) && !Source.endsWith(QLatin1Char('/')))
    {
      // nginx refuses to move a collection addressed without the trailing slash
      Response Retry = Move(DirectoryPath(Source), DirectoryPath(AbsTo));
      if ((Retry.Status == 201) || (Retry.Status == 204))
      {
        Resp = Retry;
      }
    }
    CheckStatus(Resp, true);
  });
  if (FCurrentDirectory.startsWith(AbsFrom))
  {
    FCurrentDirectory.clear();
  }
}

void WebDavFileSystem::ChangeMode(const QString & Path, unsigned int /*Mode*/)
{
  // fcModeChanging is not supported by WebDAV
  throw ExtException(Tr(CANT_SET_ATTRS).arg(Path), Tr(NOTSUPPORTED));
}

void WebDavFileSystem::SetModificationTime(const QString & Path, const QDateTime & Time)
{
  QString Abs = AbsolutePath(Path);
  WithErrorMessage(Tr(PRESERVE_TIME_PERM_ERROR3).arg(UnixPath::ExtractFileName(Path)), [&]()
  {
    // TWebDAVFileSystem::Source: PROPPATCH of DAV:getlastmodified
    QString LastModified = QLocale::c().toString(Time.toUTC(), QStringLiteral("ddd, d MMM yyyy hh:mm:ss 'GMT'"));
    Request Req = MakeRequest("PROPPATCH", Abs);
    Req.Headers << qMakePair(QByteArray("Content-Type"), QByteArray("application/xml"));
    Req.Body =
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
      "<D:propertyupdate xmlns:D=\"DAV:\"><D:set><D:prop><D:getlastmodified>" + LastModified.toLatin1() +
      "</D:getlastmodified></D:prop></D:set></D:propertyupdate>\r\n";
    Response Resp = Dispatch(Req);
    // NE_AUTH, NE_REDIRECT (and network errors) fail
    if ((Resp.Status == 401) || (Resp.Status == 407) || IsRedirect(Resp))
    {
      CheckStatus(Resp, true);
    }
    // NE_ERROR (HTTP error, failed propstat) is ignored, as major WebDAV
    // servers (like IIS) do not support changing getlastmodified
    try
    {
      CheckStatus(Resp, true);
    }
    catch (const ExtException & E)
    {
      LogLine(QStringLiteral("Preserving timestamp failed, ignoring: %1").arg(JoinMessages(E.Message(), E.MoreMessages())));
    }
  });
}

void WebDavFileSystem::CreateLink(const QString & Path, const QString & /*Target*/)
{
  // fcSymbolicLink / fcHardLink are not supported by WebDAV
  throw ExtException(Tr(CREATE_LINK_ERROR).arg(Path), Tr(NOTSUPPORTED));
}

//---------------------------------------------------------------------------
// Transfers

void WebDavFileSystem::Download(const QString & RemotePath, const QString & LocalPath, FileOperationProgress & Progress)
{
  // Plain download; partial file cleanup is up to the caller
  TransferParams Params;
  Params.KeepPartial = true;
  Download(RemotePath, LocalPath, Params, Progress);
}

void WebDavFileSystem::Download(const QString & RemotePath, const QString & LocalPath, const TransferParams & Params, FileOperationProgress & Progress)
{
  // TWebDAVFileSystem::Sink (ne_get), resumed with a Range request
  QString Abs = AbsolutePath(RemotePath);
  qint64 Offset = std::max<qint64>(Params.ResumeOffset, 0);
  QFile Local(LocalPath);
  bool LocalOpened = false;
  qint64 Uncounted = 0;
  Request Req = MakeRequest("GET", Abs);
  // Needed by IIS to download source code, not code output, and files with
  // unregistered extensions
  Req.Headers << qMakePair(QByteArray("Translate"), QByteArray("f"));
  if (Offset > 0)
  {
    Req.Headers << qMakePair(QByteArray("Range"), "bytes=" + QByteArray::number(Offset) + '-');
  }
  Req.Progress = &Progress;
  // the destination is opened only once the source is known to exist
  Req.OnHeaders = [&](const Response & Resp) -> bool
  {
    if (Resp.Class() != 2)
    {
      return false;
    }
    bool Append = false;
    if ((Resp.Status == 206) && (Offset > 0))
    {
      // "bytes <first>-<last>/<size>"
      QByteArray Range = Resp.Header("content-range").trimmed();
      if (Range.startsWith("bytes ") && (Range.mid(6, Range.indexOf('-') - 6).trimmed().toLongLong() == Offset))
      {
        Append = true;
      }
      else
      {
        throw ExtException(QStringLiteral("Invalid Content-Range in response"), QString::fromLatin1(Range));
      }
    }
    // Without a partial answer, the whole file is transferred again: the part
    // counted already by the caller (AddResumed) is not counted twice
    Uncounted = Append ? 0 : Offset;
    if (LocalOpened)
    {
      Local.close();
      LocalOpened = false;
    }
    if (!Local.open(Append ? (QIODevice::WriteOnly | QIODevice::Append) : (QIODevice::WriteOnly | QIODevice::Truncate)))
    {
      throw ExtException(QStringLiteral("Can't create file '%1'.").arg(LocalPath), Local.errorString());
    }
    LocalOpened = true;
    return true;
  };
  Req.OnData = [&](const char * Data, qint64 Size)
  {
    if (Local.write(Data, Size) != Size)
    {
      throw ExtException(QStringLiteral("Can't write to file '%1'.").arg(LocalPath), Local.errorString());
    }
    qint64 Count = Size;
    if (Uncounted > 0)
    {
      qint64 Skip = std::min(Uncounted, Count);
      Uncounted -= Skip;
      Count -= Skip;
    }
    if (Count > 0)
    {
      Progress.AddTransferred(Count);
    }
  };

  try
  {
    WithErrorMessage(Tr(TRANSFER_ERROR).arg(RemotePath), [&]()
    {
      Response Resp = Dispatch(Req);
      if (IsRedirect(Resp))
      {
        // Contrary to other actions, for "GET" any redirect is followed
        QByteArray Query;
        QString Redirected = RedirectPath(Resp, Req, &Query);
        Req.Path = Redirected;
        Req.Query = Query;
        Resp = Dispatch(Req);
      }
      if ((Resp.Status == 416) && (Offset > 0))
      {
        // Range not satisfiable: the partial file is complete, or the file changed
        if (Resp.Header("content-range").trimmed() == "bytes */" + QByteArray::number(Offset))
        {
          return;
        }
        Req.Headers.removeIf([](const QPair<QByteArray, QByteArray> & Field) { return Field.first == "Range"; });
        Resp = Dispatch(Req);
      }
      CheckStatus(Resp, false);
      if (LocalOpened && !Local.flush())
      {
        throw ExtException(QStringLiteral("Can't write to file '%1'.").arg(LocalPath), Local.errorString());
      }
    });
    if (LocalOpened)
    {
      Local.close();
    }
  }
  catch (...)
  {
    if (LocalOpened)
    {
      Local.close();
      // As TSFTPFileSystem::Sink: delete the incomplete file, unless the transfer
      // is resumable and something was transferred
      if (!Params.KeepPartial || (Local.size() == 0))
      {
        Local.remove();
      }
    }
    throw;
  }
}

void WebDavFileSystem::Upload(const QString & LocalPath, const QString & RemotePath, int Mode, FileOperationProgress & Progress)
{
  TransferParams Params;
  Upload(LocalPath, RemotePath, Mode, Params, Progress);
}

void WebDavFileSystem::Upload(const QString & LocalPath, const QString & RemotePath, int /*Mode*/, const TransferParams & Params, FileOperationProgress & Progress)
{
  // TWebDAVFileSystem::Source (ne_put); permissions cannot be set (fcModeChangingUpload)
  QFile Local(LocalPath);
  if (!Local.open(QIODevice::ReadOnly))
  {
    throw ExtException(QStringLiteral("Can't open file '%1'.").arg(LocalPath), Local.errorString());
  }
  qint64 Start = 0;
  qint64 End = Local.size();
  // Text mode conversions (TFileBuffer::Convert with cpRemoveBOM / cpRemoveCtrlZ)
  if (Params.RemoveBOM && (Local.peek(3) == QByteArray("\xEF\xBB\xBF")))
  {
    Start = 3;
  }
  if (Params.RemoveCtrlZ && (End > Start))
  {
    char Last = 0;
    if (Local.seek(End - 1) && (Local.read(&Last, 1) == 1) && (Last == '\x1A'))
    {
      End--;
    }
  }
  QString Abs = AbsolutePath(RemotePath);
  WithErrorMessage(Tr(TRANSFER_ERROR).arg(LocalPath), [&]()
  {
    Request Req = MakeRequest("PUT", Abs);
    Req.File = &Local;
    Req.FileOffset = Start;
    Req.FileLength = End - Start;
    // A PUT cannot append: on resume the whole file is uploaded again, the
    // part counted already by the caller (AddResumed) is not counted twice
    Req.Uncounted = std::max<qint64>(Params.ResumeOffset, 0);
    Req.Progress = &Progress;
    // FUploadMimeType (Configuration->GetFileMimeType: only known types)
    QMimeType Mime = QMimeDatabase().mimeTypeForFile(UnixPath::ExtractFileName(Abs), QMimeDatabase::MatchExtension);
    if (Mime.isValid() && !Mime.isDefault())
    {
      Req.Headers << qMakePair(QByteArray("Content-Type"), Mime.name().toLatin1());
    }
    CheckStatus(Dispatch(Req), false);
  });
}
