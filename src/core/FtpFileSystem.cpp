// Port of WinSCP source/core/FtpFileSystem.cpp (TFTPFileSystem) and the
// protocol logic of source/filezilla/FtpControlSocket.cpp / TransferSocket.cpp /
// AsyncProxySocketLayer.cpp as a blocking QSslSocket based client (see
// FtpFileSystem.h); client certificates as TTerminal::LoadTlsCertificate and
// ParseCertificate (source/core/Common.cpp), the certificate cache as
// TTerminal::VerifyCertificate / CacheCertificate (source/core/Terminal.cpp).
#include "core/FtpFileSystem.h"
#include "core/SessionLog.h"
#include "core/Configuration.h"
#include "core/IniStorage.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostInfo>
#include <QLocale>
#include <QLockFile>
#include <QMutex>
#include <QNetworkProxy>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSslCipher>
#include <QSslSocket>
#include <QStringDecoder>
#include <QTcpServer>
#include <QThread>
#include <QTimeZone>
#include <QTimer>
#include <QUrl>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/pkcs12.h>
#include <openssl/proverr.h>
#include <openssl/x509.h>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <type_traits>

namespace
{

// TextsCore1.rc / TextsFileZilla.rc
const char * const CONNECTION_FAILED = "Connection failed.";
const char * const USER_TERMINATED = "Terminated by user.";
const char * const LOST_CONNECTION = "Lost connection.";
const char * const FILE_NOT_EXISTS = "File or folder '%1' does not exist.";
const char * const AUTHENTICATION_FAILED = "Authentication failed.";
const char * const NOTSUPPORTED = "Operation not supported.";
const char * const FTP_ACCESS_DENIED = "Access denied.";
const char * const FTP_RESPONSE_ERROR = "Invalid response to %1 command '%2'.";
const char * const CREDENTIALS_NOT_SPECIFIED = "Credentials were not specified.";
const char * const SFTP_AS_FTP_ERROR = "You cannot connect to an SFTP server using an FTP protocol. Please select the correct protocol.";
const char * const FTP_CANNOT_OPEN_ACTIVE_CONNECTION2 = "The server cannot open connection in active mode. If you are behind a NAT router, you may need to specify an external IP address. Alternatively, consider switching to passive mode.";
const char * const IDS_ERRORMSG_TIMEOUT = "Timeout detected.";
const char * const IDS_ERRORMSG_CANTOPENTRANSFERCHANNEL = "Transfer channel can't be opened. Reason: %1";
const char * const IDS_ERRORMSG_CANTESTABLISHSSLCONNECTION = "Can't establish TLS connection.";
const char * const IDS_ERRORMSG_CERTREJECTED = "Peer certificate rejected.";
const char * const FTPS_IMPLICIT = "TLS/SSL Implicit encryption";
const char * const FTPS_EXPLICIT = "TLS/SSL Explicit encryption";
const char * const VERIFY_CERT_PROMPT3 =
  "**The server's certificate is not known. You have no guarantee that the server is the computer you think it is.**\n\n"
  "Server's certificate details follow:\n\n%1\n\n"
  "If you trust this certificate, press Yes. To connect without storing certificate, press No. To abandon the connection press Cancel.\n\n"
  "Continue connecting and store the certificate?";
const char * const CERT_TEXT2 = "Issuer:\n%1\nSubject:\n%2\nValid: %3 – %4\n\nFingerprints:\n- SHA-256: %5\n- SHA-1: %6\n\nSummary: %7";
const char * const CERT_ERRDEPTH = "The error occurred at a depth of %1 in the certificate chain.";
const char * const CERT_NAME_MISMATCH = "Certificate was not issued for this server. You might be connecting to a server that is pretending to be \"%1\".";
const char * const CERT_IP_CANNOT_VERIFY = "When connecting using an IP address, it is not possible to verify if the certificate was issued for the server. Use a hostname instead of the IP address.";
const char * const CERT_OK = "Certificate is valid.";
// Terminal level messages (same as SftpFileSystem)
const char * const LIST_DIR_ERROR = "Error listing directory '%1'.";
const char * const CREATE_DIR_ERROR = "Error creating folder '%1'.";
const char * const DELETE_FILE_ERROR = "Error deleting file '%1'.";
const char * const RENAME_FILE_ERROR = "Error renaming file '%1' to '%2'.";
const char * const CANT_SET_ATTRS = "Can't change properties of file '%1'.";
const char * const CREATE_LINK_ERROR = "Error creating link '%1'.";
const char * const PRESERVE_TIME_PERM_ERROR3 = "Upload of file '%1' was successful, but error occurred while setting the permissions and/or timestamp.";
const char * const CANT_OPEN_REMOTE = "Cannot open remote file '%1'.";
const char * const CANT_CREATE_REMOTE = "Cannot create remote file '%1'.";
const char * const CANT_GET_REAL_PATH = "Cannot get real path for '%1'.";
const char * const COPY_FILE_ERROR = "Error copying file '%1' to '%2'.";
const char * const CHECKSUM_ERROR = "Error calculating checksum for file '%1'.";
const char * const UNKNOWN_CHECKSUM = "Checksum algorithm '%1' is not supported.";
const char * const ANY_CERTIFICATE = "WARNING! Giving up security and accepting any certificate as configured!";
// Client certificate (TextsCore1.rc)
const char * const CERTIFICATE_OPEN_ERROR = "Cannot open certificate \"%1\".";
const char * const CERTIFICATE_READ_ERROR = "Cannot read certificate \"%1\".";
const char * const CERTIFICATE_DECODE_ERROR_INFO = "Error decoding certificate.";
const char * const CERTIFICATE_PUBLIC_KEY_NOT_FOUND = "Certificate file \"%1\" does not contain a public key and no corresponding .crt/.cer file was found.";
const char * const NO_PRIVATE_KEY = "No private key found in %1 file.";
const char * const CERTIFICATE_PASSPHRASE_PROMPT = "&Passphrase for client certificate:";
// Proxies (TextsFileZilla.rc)
const char * const IDS_ERRORMSG_PROXY_NOCONN = "Can't connect to proxy server.";
const char * const IDS_ERRORMSG_PROXY_REQUESTFAILED = "Proxy request failed, can't connect through proxy server.";
const char * const IDS_ERRORMSG_CANTCREATEDUETOPORTRANGE = "Could not create socket in the specified port range.";

QString Tr(const char * Text)
{
  return QString::fromUtf8(Text);
}

// TFTPFileSystem::RegisterChecksumAlgCommand: algorithm specific commands
struct FtpChecksumCommand
{
  const char * Alg;
  const char * Command;
};
const FtpChecksumCommand FtpChecksumCommands[] = {
  {"sha-1", "XSHA1"},      // e.g. Cerberos FTP
  {"sha-256", "XSHA256"},  // e.g. Cerberos FTP
  {"sha-512", "XSHA512"},  // e.g. Cerberos FTP
  {"md5", "XMD5"},         // e.g. Cerberos FTP
  {"md5", "MD5"},          // e.g. Apache FTP
  {"crc32", "XCRC"},       // e.g. Cerberos FTP
};

// SameIdent / FindIdent (Common.cpp): case insensitive, dashes ignored
bool SameIdent(const QString & Ident1, const QString & Ident2)
{
  QString A = Ident1;
  QString B = Ident2;
  return (A.remove(QLatin1Char('-')).compare(B.remove(QLatin1Char('-')), Qt::CaseInsensitive) == 0);
}

QString FindIdent(const QString & Ident, const QStringList & Idents)
{
  for (const QString & Candidate : Idents)
  {
    if (SameIdent(Ident, Candidate))
    {
      return Candidate;
    }
  }
  return Ident;
}

// WinSCP's storage key of accepted FTPS certificates (Configuration.cpp)
const QString FtpsCertificateStorageKey = QStringLiteral("FtpsCertificates");
// Hidden options in Configuration\Interface (TConfiguration::ShowFtpWelcomeMessage,
// TConfiguration::CertificateStorage)
const QString ShowFtpWelcomeMessageKey = QStringLiteral("ShowFtpWelcomeMessage");
const QString CertificateStorageKey = QStringLiteral("CertificateStorage");

// TransferParams::AsciiTransfer, when the member exists (it is added by the
// owner of core/FileSystem.h; until then text mode is set with SetAsciiTransfer)
template <typename T>
auto FtpAsciiTransfer(const T & Params, int) -> decltype(static_cast<bool>(Params.AsciiTransfer))
{
  return static_cast<bool>(Params.AsciiTransfer);
}
template <typename T>
bool FtpAsciiTransfer(const T &, long)
{
  return false;
}

// Runs Func on the application (GUI) thread, where Configuration is used.
// The worker thread waits; safe because the GUI never blocks on a worker
// without processing events (see Terminal::~Terminal and GuiUserInterface).
void RunInMainThread(const std::function<void()> & Func)
{
  QCoreApplication * App = QCoreApplication::instance();
  if ((App == nullptr) || (QThread::currentThread() == App->thread()))
  {
    Func();
  }
  else
  {
    QMetaObject::invokeMethod(App, Func, Qt::BlockingQueuedConnection);
  }
}

// MD5ToUrlSafe + SameChecksum(.., false) (Common.cpp)
bool SameChecksum(const QString & Checksum1, const QString & Checksum2)
{
  QString A = Checksum1;
  QString B = Checksum2;
  A.replace(QLatin1Char(':'), QLatin1Char('-'));
  B.replace(QLatin1Char(':'), QLatin1Char('-'));
  return (A.compare(B, Qt::CaseInsensitive) == 0);
}

// FormatCertificateData (Terminal.cpp)
QString FormatCertificateData(const QString & Fingerprint, int Failures)
{
  return QStringLiteral("%1;%2").arg(Fingerprint, QString::number(Failures, 16).toUpper().rightJustified(2, QLatin1Char('0')));
}

// TlsVersionToOpenssl (NeonIntf.cpp) as OpenSSL configuration command value
QByteArray TlsVersionName(TTlsVersion Version)
{
  switch (Version)
  {
    case tls10: return QByteArrayLiteral("TLSv1");
    case tls11: return QByteArrayLiteral("TLSv1.1");
    case tls12: return QByteArrayLiteral("TLSv1.2");
    case tls13: return QByteArrayLiteral("TLSv1.3");
    default: return QByteArray();
  }
}

// X509_V_ERR_* code of a verification error (the value WinSCP keeps in the
// certificate cache, from FileZilla's verify_callback)
int OpenSslVerifyError(const QSslError & Error)
{
  switch (Error.error())
  {
    case QSslError::UnableToGetIssuerCertificate: return X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT;
    case QSslError::UnableToDecryptCertificateSignature: return X509_V_ERR_UNABLE_TO_DECRYPT_CERT_SIGNATURE;
    case QSslError::UnableToDecodeIssuerPublicKey: return X509_V_ERR_UNABLE_TO_DECODE_ISSUER_PUBLIC_KEY;
    case QSslError::CertificateSignatureFailed: return X509_V_ERR_CERT_SIGNATURE_FAILURE;
    case QSslError::CertificateNotYetValid: return X509_V_ERR_CERT_NOT_YET_VALID;
    case QSslError::CertificateExpired: return X509_V_ERR_CERT_HAS_EXPIRED;
    case QSslError::InvalidNotBeforeField: return X509_V_ERR_ERROR_IN_CERT_NOT_BEFORE_FIELD;
    case QSslError::InvalidNotAfterField: return X509_V_ERR_ERROR_IN_CERT_NOT_AFTER_FIELD;
    case QSslError::SelfSignedCertificate: return X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT;
    case QSslError::SelfSignedCertificateInChain: return X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN;
    case QSslError::UnableToGetLocalIssuerCertificate: return X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY;
    case QSslError::UnableToVerifyFirstCertificate: return X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE;
    case QSslError::CertificateRevoked: return X509_V_ERR_CERT_REVOKED;
    case QSslError::InvalidCaCertificate: return X509_V_ERR_INVALID_CA;
    case QSslError::PathLengthExceeded: return X509_V_ERR_PATH_LENGTH_EXCEEDED;
    case QSslError::InvalidPurpose: return X509_V_ERR_INVALID_PURPOSE;
    case QSslError::CertificateUntrusted: return X509_V_ERR_CERT_UNTRUSTED;
    case QSslError::CertificateRejected: return X509_V_ERR_CERT_REJECTED;
    case QSslError::SubjectIssuerMismatch: return X509_V_ERR_SUBJECT_ISSUER_MISMATCH;
    case QSslError::AuthorityIssuerSerialNumberMismatch: return X509_V_ERR_AKID_ISSUER_SERIAL_MISMATCH;
    default: return X509_V_ERR_UNSPECIFIED;
  }
}

// GetTlsErrorStr (Common.cpp)
QString TlsErrorStr(unsigned long Err)
{
  char Buffer[512];
  ERR_error_string_n(Err, Buffer, sizeof(Buffer));
  QString S = QString::fromUtf8(Buffer);
  for (int Index = 0; Index < 4; Index++)
  {
    qsizetype P = S.indexOf(QLatin1Char(':'));
    S = (P >= 0) ? S.mid(P + 1) : QString();
  }
  return QStringLiteral("OpenSSL %1: %2").arg(QString::number(static_cast<qulonglong>(Err), 16).toUpper(), S.trimmed());
}

// IsTlsPassphraseError (Common.cpp). With OpenSSL 3 the decryption of PKCS#8
// keys fails in the provider (PROV_R_BAD_DECRYPT), also for an empty
// passphrase, and the queue may hold several errors: any of them counts.
bool IsTlsPassphraseError(unsigned long Error, bool HasPassphrase)
{
  int ErrorLib = ERR_GET_LIB(Error);
  int ErrorReason = ERR_GET_REASON(Error);
  return
    ((ErrorLib == ERR_LIB_PKCS12) && (ErrorReason == PKCS12_R_MAC_VERIFY_FAILURE)) ||
    ((ErrorLib == ERR_LIB_PEM) && (ErrorReason == PEM_R_BAD_PASSWORD_READ)) ||
    ((ErrorLib == ERR_LIB_PROV) && (ErrorReason == PROV_R_BAD_DECRYPT)) ||
    ((ErrorLib == ERR_LIB_EVP) && (ErrorReason == EVP_R_BAD_DECRYPT)) ||
    (HasPassphrase && ((ErrorReason == PEM_R_BAD_DECRYPT) || (ErrorReason == PEM_R_BAD_BASE64_DECODE)));
}

// Takes the whole error queue: returns its first error, Passphrase tells
// whether any of them is a passphrase error
unsigned long TakeTlsErrors(bool HasPassphrase, bool & Passphrase)
{
  Passphrase = false;
  unsigned long First = 0;
  unsigned long Error;
  while ((Error = ERR_get_error()) != 0)
  {
    if (First == 0)
    {
      First = Error;
    }
    Passphrase |= IsTlsPassphraseError(Error, HasPassphrase);
  }
  return First;
}

void ThrowTlsCertificateErrorIgnorePassphraseErrors(const QString & Path, bool HasPassphrase)
{
  bool Passphrase = false;
  unsigned long Error = TakeTlsErrors(HasPassphrase, Passphrase);
  if (!Passphrase)
  {
    throw ExtException(Tr(CERTIFICATE_READ_ERROR).arg(Path), TlsErrorStr(Error));
  }
}

FILE * OpenCertificate(const QString & Path)
{
  FILE * Result = std::fopen(QFile::encodeName(Path).constData(), "rb");
  if (Result == nullptr)
  {
    throw ExtException(Tr(CERTIFICATE_OPEN_ERROR).arg(Path), QString::fromLocal8Bit(std::strerror(errno)));
  }
  return Result;
}

int PemPasswordCallback(char * Buf, int Size, int /*RWFlag*/, void * UserData)
{
  const QByteArray & Passphrase = *static_cast<const QByteArray *>(UserData);
  if (Size <= 0)
  {
    return 0;
  }
  int Length = static_cast<int>(std::min<qsizetype>(Passphrase.size(), Size - 1));
  std::memcpy(Buf, Passphrase.constData(), static_cast<size_t>(Length));
  Buf[Length] = '\0';
  return Length;
}

// ParseCertificate (Common.cpp): PKCS#12 (.pfx/.p12), or PEM with the private
// key (encrypted or not) and the certificate in the same file or in a .cer/.crt
// file next to it (PEM or DER). Certificate/PrivateKey are left null with
// WrongPassphrase set when the passphrase is missing or wrong.
void ParseCertificate(const QString & Path, const QString & Passphrase,
  QSslCertificate & Certificate, QSslKey & PrivateKey, bool & WrongPassphrase)
{
  Certificate = QSslCertificate();
  PrivateKey = QSslKey();
  WrongPassphrase = false;
  bool HasPassphrase = !Passphrase.isEmpty();
  QByteArray PassphraseUtf = Passphrase.toUtf8();
  X509 * X509Certificate = nullptr;
  EVP_PKEY * Key = nullptr;
  auto Cleanup = [&]()
  {
    if (Key != nullptr)
    {
      EVP_PKEY_free(Key);
      Key = nullptr;
    }
    if (X509Certificate != nullptr)
    {
      X509_free(X509Certificate);
      X509Certificate = nullptr;
    }
  };
  try
  {
    ERR_clear_error();
    FILE * File = OpenCertificate(Path);
    PKCS12 * Pkcs12 = d2i_PKCS12_fp(File, nullptr);
    std::fclose(File);
    if (Pkcs12 != nullptr)
    {
      bool Result = (PKCS12_parse(Pkcs12, PassphraseUtf.constData(), &Key, &X509Certificate, nullptr) == 1);
      PKCS12_free(Pkcs12);
      if (!Result)
      {
        ThrowTlsCertificateErrorIgnorePassphraseErrors(Path, HasPassphrase);
        WrongPassphrase = true;
      }
      if (!WrongPassphrase && (Key == nullptr))
      {
        throw ExtException(Tr(NO_PRIVATE_KEY).arg(Path));
      }
    }
    else
    {
      ERR_clear_error();
      File = OpenCertificate(Path);
      Key = PEM_read_PrivateKey(File, nullptr, PemPasswordCallback, &PassphraseUtf);
      std::fclose(File);
      if (Key == nullptr)
      {
        ThrowTlsCertificateErrorIgnorePassphraseErrors(Path, HasPassphrase);
        WrongPassphrase = true;
      }

      File = OpenCertificate(Path);
      X509Certificate = PEM_read_X509(File, nullptr, PemPasswordCallback, &PassphraseUtf);
      std::fclose(File);
      if (X509Certificate == nullptr)
      {
        bool Passphrase = false;
        TakeTlsErrors(HasPassphrase, Passphrase);
        if (Passphrase)
        {
          WrongPassphrase = true;
        }
        else
        {
          QFileInfo Info(Path);
          QString Base = Info.path() + QLatin1Char('/') + Info.completeBaseName();
          QString CertificatePath = Base + QStringLiteral(".cer");
          if (!QFileInfo::exists(CertificatePath))
          {
            CertificatePath = Base + QStringLiteral(".crt");
          }
          if (!QFileInfo::exists(CertificatePath))
          {
            throw ExtException(Tr(CERTIFICATE_PUBLIC_KEY_NOT_FOUND).arg(Path));
          }
          ERR_clear_error();
          File = OpenCertificate(CertificatePath);
          X509Certificate = PEM_read_X509(File, nullptr, PemPasswordCallback, &PassphraseUtf);
          std::fclose(File);
          if (X509Certificate == nullptr)
          {
            unsigned long Base64Error = ERR_get_error();
            File = OpenCertificate(CertificatePath);
            X509Certificate = d2i_X509_fp(File, nullptr);
            std::fclose(File);
            if (X509Certificate == nullptr)
            {
              unsigned long DERError = ERR_get_error();
              throw ExtException(Tr(CERTIFICATE_READ_ERROR).arg(CertificatePath),
                QStringLiteral("Base64: %1\nDER: %2").arg(TlsErrorStr(Base64Error), TlsErrorStr(DERError)));
            }
          }
        }
      }
    }
  }
  catch (...)
  {
    Cleanup();
    ERR_clear_error();
    throw;
  }
  if (WrongPassphrase)
  {
    Cleanup();
  }
  else
  {
    int Length = i2d_X509(X509Certificate, nullptr);
    QByteArray Der(Length, Qt::Uninitialized);
    unsigned char * Out = reinterpret_cast<unsigned char *>(Der.data());
    i2d_X509(X509Certificate, &Out);
    Certificate = QSslCertificate(Der, QSsl::Der);
    // QSslKey takes over the key (opaque key of any algorithm)
    PrivateKey = QSslKey(reinterpret_cast<Qt::HANDLE>(Key), QSsl::PrivateKey);
    Key = nullptr;
    Cleanup();
  }
  ERR_clear_error();
}

const QString AnonymousUserName = QStringLiteral("anonymous");
const QString AnonymousPassword = QStringLiteral("anonymous@example.com");

const qint64 TransferChunk = 64 * 1024;
const qint64 UploadHighWater = 256 * 1024;
const int FileListCacheMs = 15000;

bool LogEnabled()
{
  static const bool Enabled = !qEnvironmentVariableIsEmpty("NIXSCP_FTP_LOG");
  return Enabled;
}

void LogLine(const QString & Line)
{
  SessionLog::LogProtocolLine(Line, -1);
  if (LogEnabled())
  {
    std::fprintf(stderr, "[ftp] %s\n", qPrintable(Line));
  }
}

// CFtpControlSocket::IsRoutableAddress
bool IsRoutableAddress(const QString & Host)
{
  if (Host.startsWith(QStringLiteral("127")) ||
      Host.startsWith(QStringLiteral("10.")) ||
      Host.startsWith(QStringLiteral("192.168")) ||
      Host.startsWith(QStringLiteral("169.254")) ||
      Host.startsWith(QStringLiteral("0.")))
  {
    return false;
  }
  else if (Host.startsWith(QStringLiteral("172")))
  {
    QString Middle = Host.mid(4);
    int Part = Middle.left(Middle.indexOf(QLatin1Char('.'))).toInt();
    if ((Part >= 16) && (Part <= 31))
    {
      return false;
    }
  }
  return true;
}

QString AddressString(const QHostAddress & Address)
{
  bool Ok = false;
  quint32 V4 = Address.toIPv4Address(&Ok);
  if (Ok)
  {
    return QHostAddress(V4).toString();
  }
  return Address.toString();
}

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

QString FormatContactList(const QString & Entry1, const QString & Entry2)
{
  if (!Entry1.isEmpty() && !Entry2.isEmpty())
  {
    return QStringLiteral("%1, %2").arg(Entry1, Entry2);
  }
  return Entry1 + Entry2;
}

// FormatContact (VERIFY_CERT_CONTACT)
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

QString CertificateErrorText(const QSslError & Error)
{
  // TFTPFileSystem::HandleAsynchRequestVerifyCertificate (CERT_ERR_*)
  switch (Error.error())
  {
    case QSslError::UnableToGetIssuerCertificate: return QStringLiteral("Unable to get issuer certificate.");
    case QSslError::UnableToDecryptCertificateSignature: return QStringLiteral("Unable to decrypt certificate signature.");
    case QSslError::UnableToDecodeIssuerPublicKey: return QStringLiteral("Unable to decode issuer public key.");
    case QSslError::CertificateSignatureFailed: return QStringLiteral("Certificate signature failure.");
    case QSslError::CertificateNotYetValid: return QStringLiteral("Certificate is not yet valid.");
    case QSslError::CertificateExpired: return QStringLiteral("Certificate has expired.");
    case QSslError::InvalidNotBeforeField: return QStringLiteral("Format error in certificate's valid from field.");
    case QSslError::InvalidNotAfterField: return QStringLiteral("Format error in certificate's valid until field.");
    case QSslError::SelfSignedCertificate: return QStringLiteral("Self-signed certificate.");
    case QSslError::SelfSignedCertificateInChain: return QStringLiteral("Self-signed certificate in certificate chain.");
    case QSslError::UnableToGetLocalIssuerCertificate: return QStringLiteral("Unable to get local issuer certificate.");
    case QSslError::UnableToVerifyFirstCertificate: return QStringLiteral("Unable to verify the first certificate.");
    case QSslError::InvalidCaCertificate: return QStringLiteral("Invalid CA certificate.");
    case QSslError::PathLengthExceeded: return QStringLiteral("Path length constraint exceeded.");
    case QSslError::InvalidPurpose: return QStringLiteral("Unsupported certificate purpose.");
    case QSslError::CertificateUntrusted: return QStringLiteral("Certificate not trusted.");
    case QSslError::CertificateRejected: return QStringLiteral("Certificate rejected.");
    default: return QStringLiteral("Unknown certificate verification error. (%1)").arg(Error.errorString());
  }
}

// FileZilla's CAsyncProxySocketLayer: blocking helpers on an unencrypted socket
bool WaitForBytes(QSslSocket * Socket, qint64 Count, int TimeoutMs, FileOperationProgress * Progress)
{
  QElapsedTimer Timer;
  Timer.start();
  while (Socket->bytesAvailable() < Count)
  {
    if (Progress != nullptr)
    {
      Progress->CheckCancel();
    }
    if (Socket->state() != QAbstractSocket::ConnectedState)
    {
      return (Socket->bytesAvailable() >= Count);
    }
    if (!Socket->waitForReadyRead(100) && (Timer.elapsed() > TimeoutMs))
    {
      return false;
    }
  }
  return true;
}

// Server socket for active mode data connections, accepted connections are QSslSockets
class SslServer : public QTcpServer
{
public:
  QSslSocket * TakeConnection()
  {
    if (FPending.isEmpty())
    {
      return nullptr;
    }
    return FPending.takeFirst();
  }
  ~SslServer() override
  {
    qDeleteAll(FPending);
  }

protected:
  void incomingConnection(qintptr Descriptor) override
  {
    QSslSocket * Socket = new QSslSocket();
    if (Socket->setSocketDescriptor(Descriptor))
    {
      FPending.append(Socket);
    }
    else
    {
      delete Socket;
    }
  }

private:
  QList<QSslSocket *> FPending;
};

}

struct FtpFileSystem::DataConnection
{
  std::unique_ptr<QSslSocket> Socket;
  std::unique_ptr<SslServer> Server;
  Reply FinalReply;
  bool HaveFinalReply = false;
  // TLS close_notify received; servers like vsftpd keep the TCP connection
  // open until the client answers with its own close_notify
  bool PeerClosed = false;

  void WatchSocket()
  {
    QObject::connect(Socket.get(), &QSslSocket::alertReceived, Socket.get(),
      [this](QSsl::AlertLevel, QSsl::AlertType Type, const QString &)
      {
        if (Type == QSsl::AlertType::CloseNotify)
        {
          PeerClosed = true;
        }
      });
  }
};

class FtpFileSystem::OperationGuard
{
public:
  explicit OperationGuard(FtpFileSystem * Owner) : FOwner(Owner) { FOwner->FBusy++; }
  ~OperationGuard() { FOwner->FBusy--; }
private:
  FtpFileSystem * FOwner;
};

FtpFileSystem::FtpFileSystem(const SessionData & Data, UserInterface * UI) :
  FData(Data),
  FUI(UI)
{
  FListAll = FData.FtpListAll;
  FWorkFromCwd = FData.FtpWorkFromCwd;
  FKeepaliveTimer = std::make_unique<QTimer>();
  FKeepaliveTimer->setInterval(1000);
  QObject::connect(FKeepaliveTimer.get(), &QTimer::timeout, [this]() { Idle(); });
}

FtpFileSystem::~FtpFileSystem()
{
  Close();
}

int FtpFileSystem::TimeoutMs() const
{
  return std::max(FData.Timeout, 1) * 1000;
}

bool FtpFileSystem::IsOpened() const
{
  return FLoggedIn && FControl && (FControl->state() == QAbstractSocket::ConnectedState);
}

bool FtpFileSystem::IsCapable(FSCapability Capability, bool Default) const
{
  // TFTPFileSystem::IsCapable
  switch (Capability)
  {
    case fcAnyCommand:
      // raw FTP commands, but not shell commands (fcShellAnyCommand)
      return true;
    case fcShellAnyCommand:
    case fcSecondaryShell:
      return false;
    case fcAppendUpload:
      // APPE
      return true;
    case fcMoveOverExistingFile:
      return !FIIS;
    default:
      break;
  }
  if (!FLoggedIn)
  {
    return Default;
  }
  switch (Capability)
  {
    case fcRemoteCopy:
      return CanRemoteCopy();
    case fcSymbolicLink:
      return SupportsSiteCommand(QStringLiteral("SYMLINK"));
    case fcCalculatingChecksum:
      return FSupportsAnyChecksumFeature;
    case fcCheckingSpaceAvailable:
      return SupportsCommand(QStringLiteral("XQUOTA")) || SupportsCommand(QStringLiteral("AVBL"));
    case fcPreservingTimestampUpload:
      // MFMT, or MFMT-like MDTM (CFtpControlSocket::TransferFinished), or SITE UTIME
      return SupportsCommand(QStringLiteral("MFMT")) || SupportsCommand(QStringLiteral("MDTM")) ||
        SupportsSiteCommand(QStringLiteral("UTIME"));
    default:
      return Default;
  }
}

bool FtpFileSystem::UsingMlsd() const
{
  // CFtpControlSocket::UsingMlsd
  return (FData.FtpUseMlsd == asOn) ||
    ((FData.FtpUseMlsd != asOff) &&
     // MLST feature implies MLSD support (RFC 3659), FileZilla checks both
     (SupportsCommand(QStringLiteral("MLSD")) || SupportsCommand(QStringLiteral("MLST"))));
}

QString FtpFileSystem::ProtocolName() const
{
  return FTls ? QStringLiteral("FTPS") : QStringLiteral("FTP");
}

QString FtpFileSystem::ServerInfo() const
{
  QStringList Lines;
  if (!FSystem.isEmpty())
  {
    Lines << QStringLiteral("Server: %1").arg(FSystem);
  }
  if (!FServerID.isEmpty())
  {
    // CSID (TFTPFileSystem::DoStartup)
    Lines << QStringLiteral("Server software: %1").arg(FServerID);
  }
  if (FTls)
  {
    QString Security = Tr((FData.Ftps == ftpsImplicit) ? FTPS_IMPLICIT : FTPS_EXPLICIT);
    if (!FTlsVersion.isEmpty())
    {
      Security += QStringLiteral(", ") + FTlsVersion;
    }
    Lines << QStringLiteral("Encryption: %1").arg(Security);
    if (!FCipher.isEmpty())
    {
      Lines << QStringLiteral("Cipher: %1").arg(FCipher);
    }
    Lines << QStringLiteral("Certificate fingerprint (SHA-256): %1").arg(FCertificateFingerprintSHA256);
    Lines << QStringLiteral("Certificate fingerprint (SHA-1): %1").arg(FCertificateFingerprintSHA1);
    Lines << QStringLiteral("Data connection encryption: %1").arg(FProtP ? QStringLiteral("Yes") : QStringLiteral("No"));
    if (FClientCertificateLoaded)
    {
      Lines << QStringLiteral("Client certificate: %1").arg(FClientCertificate.subjectDisplayName());
    }
  }
  Lines << QStringLiteral("Transfer mode: %1").arg(FData.FtpPasvMode ? QStringLiteral("Passive") : QStringLiteral("Active"));
  Lines << QStringLiteral("Directory listing: %1").arg(UsingMlsd() ? QStringLiteral("MLSD") : QStringLiteral("LIST"));
  if (!FFeatures.isEmpty())
  {
    Lines << QStringLiteral("Features: %1").arg(FFeatures.join(QStringLiteral(", ")));
  }
  if (!FWelcomeMessage.isEmpty())
  {
    Lines << QStringLiteral("Welcome message:") << FWelcomeMessage;
  }
  if (!FCertificateText.isEmpty())
  {
    // Full certificate (FileSystemInfo dialog)
    Lines << QStringLiteral("Certificate:") << FCertificateText;
  }
  return Lines.join(QLatin1Char('\n'));
}

//---------------------------------------------------------------------------
// Control connection

QByteArray FtpFileSystem::Encode(const QString & Str) const
{
  return FUtf8 ? Str.toUtf8() : FtpEncodeLegacy(Str);
}

QString FtpFileSystem::Decode(const QByteArray & Bytes) const
{
  // CFtpControlSocket::DecodeString
  if (FUtf8)
  {
    QStringDecoder Decoder(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    QString Result = Decoder.decode(Bytes);
    if (!Decoder.hasError())
    {
      return Result;
    }
  }
  return FtpDecodeLegacy(Bytes);
}

[[noreturn]] void FtpFileSystem::ConnectionLost(const QString & Details)
{
  DoClose();
  throw FatalException(Tr(LOST_CONNECTION), Details);
}

void FtpFileSystem::CheckConnection()
{
  if (!IsOpened())
  {
    QString Details = FControl ? FControl->errorString() : QString();
    DoClose();
    throw FatalException(Tr(LOST_CONNECTION), Details);
  }
}

namespace
{
// Closes an upload data connection without a reset. With TLS 1.3 the
// server's session tickets may arrive only when the server starts reading,
// after the client has written the whole file. Closing a socket with unread
// received data makes the kernel reset the connection (RST) instead of
// closing it, and the server loses the end of the file still in its receive
// buffer (a truncated upload reported as complete). So the socket is kept
// open (a duplicate descriptor) after QSslSocket sent close_notify and closed
// its descriptor; it is then half-closed and read until the server closes it.
void CloseUploadConnection(QSslSocket * Socket, int TimeoutMs, FileOperationProgress * Progress)
{
  qintptr Fd = Socket->socketDescriptor();
  int Dup = (Fd >= 0) ? dup(static_cast<int>(Fd)) : -1;
  Socket->disconnectFromHost();
  if (Socket->state() != QAbstractSocket::UnconnectedState)
  {
    Socket->waitForDisconnected(TimeoutMs);
  }
  if (Dup < 0)
  {
    return;
  }
  shutdown(Dup, SHUT_WR);
  QElapsedTimer Timer;
  Timer.start();
  char Buffer[4096];
  while (Timer.elapsed() < TimeoutMs)
  {
    if ((Progress != nullptr) && Progress->Cancelled())
    {
      break;
    }
    pollfd P{Dup, POLLIN, 0};
    int Ready = poll(&P, 1, 100);
    if (Ready < 0)
    {
      break;
    }
    if (Ready == 0)
    {
      continue;
    }
    ssize_t Read = recv(Dup, Buffer, sizeof(Buffer), 0);
    if (Read <= 0)
    {
      break;
    }
  }
  close(Dup);
}
}

void FtpFileSystem::WriteAll(QSslSocket * Socket, const QByteArray & Data, FileOperationProgress * Progress)
{
  if (Socket->write(Data) != Data.size())
  {
    throw ExtException(Tr(LOST_CONNECTION), Socket->errorString());
  }
  QElapsedTimer Idle;
  Idle.start();
  qint64 Pending = Socket->bytesToWrite() + Socket->encryptedBytesToWrite();
  while (Pending > ((Progress != nullptr) ? UploadHighWater : 0))
  {
    if (Progress != nullptr)
    {
      Progress->CheckCancel();
    }
    if (Socket->state() != QAbstractSocket::ConnectedState)
    {
      throw ExtException(Tr(LOST_CONNECTION), Socket->errorString());
    }
    Socket->waitForBytesWritten(100);
    qint64 Now = Socket->bytesToWrite() + Socket->encryptedBytesToWrite();
    if (Now < Pending)
    {
      Idle.restart();
    }
    else if (Idle.elapsed() > TimeoutMs())
    {
      throw ExtException(Tr(LOST_CONNECTION), Tr(IDS_ERRORMSG_TIMEOUT));
    }
    Pending = Now;
  }
}

void FtpFileSystem::SendCommand(const QString & Command, bool DrainStale)
{
  // A CR/LF in a file name would split the command (FTP command injection)
  if (Command.contains(QLatin1Char('\r')) || Command.contains(QLatin1Char('\n')))
  {
    throw ExtException(QStringLiteral("Invalid character in file name or command."));
  }
  if (!FControl || (FControl->state() != QAbstractSocket::ConnectedState))
  {
    ConnectionLost(FControl ? FControl->errorString() : QString());
  }
  if (DrainStale)
  {
    // Replies that arrived while idle (typically "421 Timeout")
    Reply Stale;
    while (FControl->canReadLine() && PollReply(Stale))
    {
      LogLine(QStringLiteral("Discarding unexpected reply %1").arg(Stale.Code));
      if (Stale.Code == 421)
      {
        ConnectionLost(Stale.Lines.join(QLatin1Char('\n')));
      }
      Stale = Reply();
    }
  }
  FLastCommand = Command.section(QLatin1Char(' '), 0, 0).toUpper();
  LogLine(QStringLiteral("> %1").arg((FLastCommand == QStringLiteral("PASS")) ? SessionLog::MaskCommand(Command, Command.mid(5)) : Command));
  FLastActivity.start();
  try
  {
    WriteAll(FControl.get(), Encode(Command) + "\r\n", nullptr);
  }
  catch (const ExtException & E)
  {
    ConnectionLost(E.MoreMessages());
  }
}

bool FtpFileSystem::PollReply(Reply & Result)
{
  // CFtpControlSocket::OnReceive + TFTPFileSystem::HandleReplyStatus
  while (FControl && FControl->canReadLine())
  {
    QByteArray Raw = FControl->readLine();
    while (Raw.endsWith('\n') || Raw.endsWith('\r'))
    {
      Raw.chop(1);
    }
    QString Line = Decode(Raw);
    LogLine(QStringLiteral("< %1").arg(Line));
    bool HasCode =
      (Line.size() >= 3) && Line[0].isDigit() && Line[1].isDigit() && Line[2].isDigit() &&
      ((Line.size() == 3) || (Line[3] == QLatin1Char(' ')) || (Line[3] == QLatin1Char('-')));
    if (FMultiLineCode.isEmpty())
    {
      if (!HasCode)
      {
        // TFTPFileSystem::HandleReplyStatus: SSH server banner instead of
        // the FTP welcome message
        if (FAwaitingWelcome && Line.startsWith(QStringLiteral("SSH")))
        {
          DoClose();
          throw FatalException(Tr(CONNECTION_FAILED), Tr(SFTP_AS_FTP_ERROR));
        }
        // garbage outside of a reply
        continue;
      }
      FPartialReply = Reply();
      FPartialReply.Code = Line.left(3).toInt();
      FPartialReply.Lines.append(Line.mid(4));
      FPartialReply.RawLines.append(Line);
      if ((Line.size() >= 4) && (Line[3] == QLatin1Char('-')))
      {
        FMultiLineCode = Line.left(3);
        continue;
      }
    }
    else
    {
      FPartialReply.RawLines.append(Line);
      if (HasCode && Line.startsWith(FMultiLineCode))
      {
        FPartialReply.Lines.append(Line.mid(4));
        if ((Line.size() == 3) || (Line[3] == QLatin1Char(' ')))
        {
          FMultiLineCode.clear();
        }
        else
        {
          continue;
        }
      }
      else
      {
        FPartialReply.Lines.append(Line.startsWith(QLatin1Char(' ')) ? Line.mid(1) : Line);
        continue;
      }
    }
    Result = FPartialReply;
    FPartialReply = Reply();
    return true;
  }
  return false;
}

FtpFileSystem::Reply FtpFileSystem::ReadReply(FileOperationProgress * Progress)
{
  Reply Result;
  QElapsedTimer Idle;
  Idle.start();
  while (!PollReply(Result))
  {
    if (Progress != nullptr)
    {
      Progress->CheckCancel();
    }
    if (!FControl || (FControl->state() != QAbstractSocket::ConnectedState))
    {
      if (FControl && FControl->canReadLine())
      {
        continue;
      }
      ConnectionLost(FControl ? FControl->errorString() : QString());
    }
    if (FControl->waitForReadyRead(100))
    {
      Idle.restart();
    }
    else if (Idle.elapsed() > TimeoutMs())
    {
      ConnectionLost(Tr(IDS_ERRORMSG_TIMEOUT));
    }
  }
  FLastActivity.start();
  return Result;
}

FtpFileSystem::Reply FtpFileSystem::Command(const QString & ACommand)
{
  SendCommand(ACommand);
  return ReadReply();
}

[[noreturn]] void FtpFileSystem::ReplyError(const Reply & R, const QString & Message)
{
  // TFTPFileSystem::GotReply
  QStringList More;
  bool Lost = (R.Code == 421);
  if (Lost)
  {
    More << Tr(LOST_CONNECTION);
  }
  if (R.Code == 530)
  {
    More << Tr(AUTHENTICATION_FAILED);
  }
  if (((R.Code == 425) || (R.Code == 426)) && !FData.FtpPasvMode && !FAnyTransferSucceeded)
  {
    More << Tr(FTP_CANNOT_OPEN_ACTIVE_CONNECTION2);
  }
  More << R.Lines;
  QString Msg = Message;
  if (Msg.isEmpty() && !More.isEmpty())
  {
    Msg = More.takeFirst();
  }
  if (Lost)
  {
    DoClose();
    throw FatalException(Msg, More.join(QLatin1Char('\n')));
  }
  throw ExtException(Msg, More.join(QLatin1Char('\n')));
}

void FtpFileSystem::CheckReply(const Reply & R, const QString & Message, bool Allow3xx)
{
  if ((R.Class() != 2) && (!Allow3xx || (R.Class() != 3)))
  {
    ReplyError(R, Message);
  }
}

QStringList FtpFileSystem::AnyCommand(const QString & ACommand)
{
  QStringList Result;
  FileOperationProgress Progress;
  AnyCommand(ACommand, QString(), [&Result](const QString & Line, bool) { Result << Line; }, Progress, nullptr);
  return Result;
}

int FtpFileSystem::AnyCommand(const QString & ACommand, const QString & Directory, OutputEvent Output,
  FileOperationProgress & Progress, QString * NewDirectory)
{
  // TFTPFileSystem::AnyCommand
  OperationGuard Guard(this);
  CheckConnection();
  // end-user has right to expect that client current directory is really
  // current directory for the server (EnsureLocation)
  if (!Directory.isEmpty())
  {
    ChangeWorkingDirectory(AbsolutePath(Directory));
  }
  SendCommand(ACommand);
  Reply R = ReadReply(&Progress);
  // Preliminary replies (e.g. to a raw transfer command) are followed by the final one
  while (R.Class() == 1)
  {
    if (Output)
    {
      for (const QString & Line : std::as_const(R.RawLines))
      {
        Output(Line, false);
      }
    }
    R = ReadReply(&Progress);
  }
  // the command may change the working directory or transfer type
  FCurrentDirectory.clear();
  FTransferType = QChar();
  FFileListCacheDir.clear();
  if (Output)
  {
    for (const QString & Line : std::as_const(R.RawLines))
    {
      Output(Line, false);
    }
  }
  CheckReply(R, QString(), true);
  if (NewDirectory != nullptr)
  {
    try
    {
      ReadCurrentDirectory();
      *NewDirectory = FCurrentDirectory;
    }
    catch (const FatalException &)
    {
      throw;
    }
    catch (const ExtException &)
    {
      // keep the previous directory
    }
  }
  return 0;
}

//---------------------------------------------------------------------------
// Connection

void FtpFileSystem::ApplySocketOptions(QSslSocket * Socket, bool DataConnection) const
{
  // Options of a connected socket (QSslSocket ignores them before)
  if (FData.TcpNoDelay)
  {
    Socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
  }
  if (DataConnection && (FData.SendBuf > 0))
  {
    // OPTION_MPEXT_SNDBUF (CTransferSocket::OnConnect)
    Socket->setSocketOption(QAbstractSocket::SendBufferSizeSocketOption, FData.SendBuf);
  }
}

bool FtpFileSystem::UseProxy() const
{
  // TFTPFileSystem::GetOptionVal(OPTION_PROXYTYPE): SOCKS4 (as SOCKS4A),
  // SOCKS5, HTTP; telnet and local proxies are SSH only
  bool Local =
    (FData.HostName == QStringLiteral("localhost")) || QHostAddress(FData.HostName).isLoopback();
  return
    ((FData.ProxyMethod == pmSocks4) || (FData.ProxyMethod == pmSocks5) || (FData.ProxyMethod == pmHTTP)) &&
    (!Local || FData.ProxyLocalhost);
}

void FtpFileSystem::ConfigureSocket(QSslSocket * Socket, bool SetProxy) const
{
  QNetworkProxy Proxy(QNetworkProxy::NoProxy);
  if (UseProxy())
  {
    switch (FData.ProxyMethod)
    {
      case pmSocks5:
        Proxy = QNetworkProxy(QNetworkProxy::Socks5Proxy, FData.ProxyHost, static_cast<quint16>(FData.ProxyPort),
          FData.ProxyUsername, FData.ProxyPassword);
        break;
      case pmHTTP:
        Proxy = QNetworkProxy(QNetworkProxy::HttpProxy, FData.ProxyHost, static_cast<quint16>(FData.ProxyPort),
          FData.ProxyUsername, FData.ProxyPassword);
        break;
      default:
        // SOCKS4A: handshake of our own in ConnectSocket
        break;
    }
  }
  if (SetProxy)
  {
    Socket->setProxy(Proxy);
  }
  QSslConfiguration Config = QSslConfiguration::defaultConfiguration();
  // SetupSsl (NeonIntf.cpp, shared with FTP): SSL_set_min/max_proto_version;
  // since OpenSSL 3, TLS 1.0 and 1.1 are enabled on security level 0 only
  Config.setProtocol(QSsl::AnyProtocol);
  QByteArray MinVersion = TlsVersionName(FData.MinTlsVersion);
  QByteArray MaxVersion = TlsVersionName(FData.MaxTlsVersion);
  if (!MinVersion.isEmpty())
  {
    Config.setBackendConfigurationOption(QByteArrayLiteral("MinProtocol"), MinVersion);
  }
  if (!MaxVersion.isEmpty())
  {
    Config.setBackendConfigurationOption(QByteArrayLiteral("MaxProtocol"), MaxVersion);
  }
  if ((FData.MinTlsVersion != 0) && (FData.MinTlsVersion <= tls11))
  {
    Config.setBackendConfigurationOption(QByteArrayLiteral("CipherString"), QByteArrayLiteral("DEFAULT:@SECLEVEL=0"));
  }
  if (!FCaCertificates.isEmpty())
  {
    Config.addCaCertificates(FCaCertificates);
  }
  if (FClientCertificateLoaded)
  {
    // CAsyncSslSocketLayer::ProvideClientCert
    Config.setLocalCertificate(FClientCertificate);
    Config.setPrivateKey(FClientKey);
  }
  // Keep the session, so that data connections can resume it
  Config.setSslOption(QSsl::SslOptionDisableSessionPersistence, !FData.SslSessionReuse);
  Config.setSslOption(QSsl::SslOptionDisableSessionTickets, false);
  Socket->setSslConfiguration(Config);
  Socket->setPeerVerifyName(FData.HostName);
  // Errors are evaluated after the handshake (VerifyCertificate, OpenDataConnection);
  // the handshake runs within waitFor* on this thread, so the slot is called directly
  QObject::connect(Socket, &QSslSocket::sslErrors, Socket, [Socket](const QList<QSslError> &)
  {
    Socket->ignoreSslErrors();
  });
}

void FtpFileSystem::LoadConfiguration()
{
  // Preferences > Network and hidden options of TConfiguration used by FTP
  // (OPTION_TRANSFERIP, OPTION_PORTRANGELOW/HIGH, ShowFtpWelcomeMessage)
  QString ExternalIpAddress;
  int LocalPortNumberMin = 0;
  int LocalPortNumberMax = 0;
  bool ShowFtpWelcomeMessage = false;
  QString CertificateStorage;
  RunInMainThread([&]()
  {
    Configuration & Config = Configuration::Instance();
    ExternalIpAddress = Config.ExternalIpAddress;
    LocalPortNumberMin = Config.LocalPortNumberMin;
    LocalPortNumberMax = Config.LocalPortNumberMax;
    IniStorage Storage(&Config.Ini());
    if (Storage.OpenSubKey(QStringLiteral("Configuration"), false) &&
        Storage.OpenSubKey(QStringLiteral("Interface"), false))
    {
      ShowFtpWelcomeMessage = Storage.ReadBool(ShowFtpWelcomeMessageKey, false);
      CertificateStorage = Storage.ReadString(CertificateStorageKey, QString());
    }
  });
  // TConfiguration::CertificateStorageExpanded: a CA bundle trusted in
  // addition to the system store (FileZilla's SSL_CTX_load_verify_locations)
  FCaCertificates.clear();
  CertificateStorage = QDir::fromNativeSeparators(CertificateStorage.trimmed());
  if (CertificateStorage.startsWith(QStringLiteral("~/")))
  {
    CertificateStorage = QDir::homePath() + CertificateStorage.mid(1);
  }
  if (!CertificateStorage.isEmpty() && QFileInfo(CertificateStorage).isFile())
  {
    FCaCertificates = QSslCertificate::fromPath(CertificateStorage, QSsl::Pem);
    LogLine(QStringLiteral("Using certificate store \"%1\" (%2 certificates)").arg(CertificateStorage).arg(FCaCertificates.size()));
  }
  FExternalIpAddress = ExternalIpAddress.trimmed();
  FLocalPortNumberMin = LocalPortNumberMin;
  FLocalPortNumberMax = LocalPortNumberMax;
  FShowFtpWelcomeMessage = ShowFtpWelcomeMessage;
}

void FtpFileSystem::Open()
{
  OperationGuard Guard(this);
  // TFTPFileSystem::Open
  LoadConfiguration();
  FPasswordFailed = false;
  FAnyPassword = !FData.Password.isEmpty();
  QString Password = FData.Password;
  FTls = (FData.Ftps != ftpsNone);
  FIIS = false;
  FFileZilla = false;
  do
  {
    FUtf8 = (FData.NotUtf != asOn);
    FWorkFromCwd = FData.FtpWorkFromCwd;
    FDetectTimeDifference = FData.TimeDifferenceAuto;
    FTimeDifference = FData.TimeDifferenceAuto ? 0 : static_cast<qint64>(FData.TimeDifference * 86400.0);
    FFeatures.clear();
    FSupportedCommands.clear();
    FSupportedSiteCommands.clear();
    FHashAlgs.clear();
    FHashAlg.clear();
    FSupportsAnyChecksumFeature = false;
    FHasClntCommand = false;
    FSystem.clear();
    FServerID.clear();
    FWelcomeMessage.clear();
    FServerType = 0;
    FMVS = FVMS = false;

    // ask for username if it was not specified in advance, even on retry
    if (FData.UserName.isEmpty())
    {
      QList<Prompt> Prompts{Prompt{QStringLiteral("&Username:"), true, QString()}};
      if (!FUI->PromptUser(UserInterface::pkUserName, FData.SessionName(), QString(), Prompts))
      {
        throw FatalException(Tr(CREDENTIALS_NOT_SPECIFIED));
      }
      FData.UserName = Prompts[0].Result;
    }

    // On retry ask for password (so that a stored password that is no longer
    // valid is not tried in a loop)
    if (FPasswordFailed)
    {
      Password.clear();
      QList<Prompt> Prompts{Prompt{QStringLiteral("&Password: "), false, QString()}};
      if (!FUI->PromptUser(UserInterface::pkPassword, FData.SessionName(), Tr(FTP_ACCESS_DENIED), Prompts))
      {
        throw FatalException(Tr(FAnyPassword ? AUTHENTICATION_FAILED : CREDENTIALS_NOT_SPECIFIED));
      }
      Password = Prompts[0].Result;
      if (!Password.isEmpty())
      {
        FAnyPassword = true;
      }
    }
    if (FTls && !FClientCertificateLoaded)
    {
      LoadTlsCertificate();
    }

    FPasswordFailed = false;

    Connect();
    try
    {
      Login(Password);
    }
    catch (...)
    {
      DoClose();
      throw;
    }
    if (FPasswordFailed)
    {
      DoClose();
    }
  }
  while (FPasswordFailed);

  Startup();
  FLoggedIn = true;
  FLastDataSent.start();
  if ((FData.FtpPingType != fptOff) && (QThread::currentThread()->eventDispatcher() != nullptr))
  {
    FKeepaliveTimer->start();
  }
}

bool FtpFileSystem::LoadTlsCertificate()
{
  // TTerminal::LoadTlsCertificate (inspired by neon's ne_ssl_clicert_read)
  if (FData.TlsCertificateFile.isEmpty())
  {
    return false;
  }
  QString Path = QDir::fromNativeSeparators(FData.TlsCertificateFile);
  if (Path.startsWith(QStringLiteral("~/")))
  {
    Path = QDir::homePath() + Path.mid(1);
  }
  // (WinSCP starts with the passphrase given on the command line; NixSCP
  // sessions have none)
  QString Passphrase;
  bool Result = true;
  bool Retry;
  do
  {
    Retry = false;
    bool WrongPassphrase = false;
    ParseCertificate(Path, Passphrase, FClientCertificate, FClientKey, WrongPassphrase);
    if (WrongPassphrase)
    {
      LogLine(Passphrase.isEmpty() ? QStringLiteral("Certificate is encrypted, need passphrase") : Tr(CERTIFICATE_DECODE_ERROR_INFO));
      QString Instructions = Passphrase.isEmpty() ? QString() : Tr(CERTIFICATE_DECODE_ERROR_INFO);
      QList<Prompt> Prompts{Prompt{Tr(CERTIFICATE_PASSPHRASE_PROMPT), false, QString()}};
      if (FUI->PromptUser(UserInterface::pkPassphrase, FData.SessionName(), Instructions, Prompts))
      {
        Passphrase = Prompts[0].Result;
        Retry = true;
      }
      else
      {
        // connect without the certificate
        Result = false;
      }
    }
  }
  while (Retry);
  FClientCertificateLoaded = Result;
  if (Result)
  {
    LogLine(QStringLiteral("Using client certificate \"%1\"").arg(FClientCertificate.subjectDisplayName()));
  }
  return Result;
}

void FtpFileSystem::Socks4Handshake(QSslSocket * Socket, const QString & Host, quint16 Port, FileOperationProgress * Progress)
{
  // CAsyncProxySocketLayer::OnConnect/OnReceive, PROXYTYPE_SOCKS4A (WinSCP
  // uses SOCKS4A for the SOCKS4 proxy method): the host name is always sent,
  // the proxy resolves it; empty user ID
  QByteArray Request;
  Request.append(char(4));
  Request.append(char(1)); // CONNECT
  Request.append(char(Port >> 8));
  Request.append(char(Port & 0xFF));
  Request.append(char(0));
  Request.append(char(0));
  Request.append(char(0));
  Request.append(char(1)); // 0.0.0.x, x nonzero
  Request.append(char(0)); // user ID
  Request.append(QUrl::toAce(Host));
  Request.append(char(0));
  if (Socket->write(Request) != Request.size())
  {
    throw ExtException(Tr(IDS_ERRORMSG_PROXY_REQUESTFAILED), Socket->errorString());
  }
  Socket->flush();
  if (!WaitForBytes(Socket, 8, TimeoutMs(), Progress))
  {
    throw ExtException(Tr(IDS_ERRORMSG_PROXY_REQUESTFAILED),
      (Socket->state() == QAbstractSocket::ConnectedState) ? Tr(IDS_ERRORMSG_TIMEOUT) : Socket->errorString());
  }
  QByteArray Response = Socket->read(8);
  if ((Response[0] != 0) || (static_cast<unsigned char>(Response[1]) != 90))
  {
    throw ExtException(Tr(IDS_ERRORMSG_PROXY_REQUESTFAILED),
      QStringLiteral("SOCKS4 reply code %1").arg(static_cast<unsigned char>(Response[1])));
  }
  LogLine(QStringLiteral("Connected through SOCKS4A proxy to %1:%2").arg(Host).arg(Port));
}

void FtpFileSystem::ConnectSocket(QSslSocket * Socket, const QString & Host, quint16 Port, FileOperationProgress * Progress, bool Control)
{
  // Throws ExtException (the caller decides whether it is fatal)
  QAbstractSocket::NetworkLayerProtocol Family =
    (FData.AddressFamily == afIPv4) ? QAbstractSocket::IPv4Protocol :
    ((FData.AddressFamily == afIPv6) ? QAbstractSocket::IPv6Protocol : QAbstractSocket::AnyIPProtocol);
  bool Socks4 = UseProxy() && (FData.ProxyMethod == pmSocks4);
  QString ConnectHost = Socks4 ? FData.ProxyHost : Host;
  quint16 ConnectPort = Socks4 ? static_cast<quint16>(FData.ProxyPort) : Port;
  LogLine(QStringLiteral("Connecting to %1:%2 ...").arg(ConnectHost).arg(ConnectPort));
  if (Socks4 && (FData.AddressFamily == afAuto))
  {
    Socket->connectToHost(ConnectHost, ConnectPort);
  }
  else
  {
    Socket->connectToHost(ConnectHost, ConnectPort, QIODevice::ReadWrite, Family);
  }
  QElapsedTimer Timer;
  Timer.start();
  while (!Socket->waitForConnected(100))
  {
    if (Progress != nullptr)
    {
      Progress->CheckCancel();
    }
    if (Socket->state() == QAbstractSocket::UnconnectedState)
    {
      QString Error = Socket->errorString();
      if (Socks4)
      {
        throw ExtException(Tr(IDS_ERRORMSG_PROXY_NOCONN), Error);
      }
      throw ExtException(Error);
    }
    if (Timer.elapsed() > TimeoutMs())
    {
      QString Error = Control ?
        QStringLiteral("Network error: Connection to \"%1\" timed out.").arg(ConnectHost) : Tr(IDS_ERRORMSG_TIMEOUT);
      throw ExtException(Error);
    }
  }
  if (Socks4)
  {
    Socks4Handshake(Socket, Host, Port, Progress);
  }
  ApplySocketOptions(Socket, !Control);
}

void FtpFileSystem::Connect()
{
  DoClose();
  FControl = std::make_unique<QSslSocket>();
  ConfigureSocket(FControl.get(), true);
  // With an FTP proxy (logon type), connect to the proxy (OPTION_FWHOST/FWPORT)
  QString Host = FData.HostName;
  quint16 Port = static_cast<quint16>(FData.PortNumber);
  if (FData.FtpProxyLogonType != 0)
  {
    Host = FData.ProxyHost;
    Port = static_cast<quint16>(FData.ProxyPort);
  }
  try
  {
    ConnectSocket(FControl.get(), Host, Port, nullptr, true);
  }
  catch (const ExtException & E)
  {
    DoClose();
    QString Details = E.MoreMessages().isEmpty() ? E.Message() : (E.Message() + QLatin1Char('\n') + E.MoreMessages());
    throw FatalException(Tr(CONNECTION_FAILED), Details);
  }
  if (FData.Ftps == ftpsImplicit)
  {
    StartTls(FControl.get(), true);
  }
}

void FtpFileSystem::StartTls(QSslSocket * Socket, bool Control)
{
  if (!Socket->isEncrypted())
  {
    if (Socket->mode() == QSslSocket::UnencryptedMode)
    {
      Socket->startClientEncryption();
    }
    if (!Socket->waitForEncrypted(TimeoutMs()))
    {
      QString Error = Socket->errorString();
      if (Control)
      {
        DoClose();
        throw FatalException(Tr(IDS_ERRORMSG_CANTESTABLISHSSLCONNECTION), Error);
      }
      throw ExtException(Tr(IDS_ERRORMSG_CANTESTABLISHSSLCONNECTION), Error);
    }
  }
  if (Control)
  {
    VerifyCertificate(Socket->sslHandshakeErrors());
    // CAsyncSslSocketLayer::GetTlsVersionStr (SSL_get_version)
    QT_WARNING_PUSH
    QT_WARNING_DISABLE_DEPRECATED
    switch (Socket->sessionProtocol())
    {
      case QSsl::TlsV1_0: FTlsVersion = QStringLiteral("TLSv1"); break;
      case QSsl::TlsV1_1: FTlsVersion = QStringLiteral("TLSv1.1"); break;
      case QSsl::TlsV1_2: FTlsVersion = QStringLiteral("TLSv1.2"); break;
      case QSsl::TlsV1_3: FTlsVersion = QStringLiteral("TLSv1.3"); break;
      default: FTlsVersion.clear(); break;
    }
    QT_WARNING_POP
    FCipher = Socket->sessionCipher().name();
  }
}

bool FtpFileSystem::VerifyCertificateAgainstSessionData() const
{
  // TTerminal::VerifyCertificateAgainstSessionData: fingerprint(s) configured
  // for the site (SshHostKey, ";" separated, "*" = any certificate)
  for (const QString & ExpectedKey : FData.HostKey.split(QLatin1Char(';'), Qt::SkipEmptyParts))
  {
    QString Expected = ExpectedKey.trimmed();
    if (Expected == QStringLiteral("*"))
    {
      LogLine(Tr(ANY_CERTIFICATE));
      return true;
    }
    if (SameChecksum(Expected, FCertificateFingerprintSHA1) || SameChecksum(Expected, FCertificateFingerprintSHA256))
    {
      LogLine(QStringLiteral("Certificate for \"%1\" matches configured fingerprint").arg(FCertificate.subjectDisplayName()));
      return true;
    }
  }
  return false;
}

bool FtpFileSystem::IsCertificateCached(const QString & SiteKey, const QString & FingerprintSHA1,
  const QString & FingerprintSHA256, int Failures)
{
  // TTerminal::VerifyCertificate (cache part)
  QString CertificateDataSHA1 = FormatCertificateData(FingerprintSHA1, Failures);
  QString CertificateDataSHA256 = FormatCertificateData(FingerprintSHA256, Failures);
  bool Result = false;
  RunInMainThread([&]()
  {
    IniStorage Storage(&Configuration::Instance().Ini());
    if (Storage.OpenSubKey(FtpsCertificateStorageKey, false))
    {
      if (Storage.ValueExists(SiteKey))
      {
        QString CachedCertificateData = Storage.ReadString(SiteKey, QString());
        Result =
          SameChecksum(CertificateDataSHA1, CachedCertificateData) ||
          SameChecksum(CertificateDataSHA256, CachedCertificateData);
      }
      else
      {
        // legacy cache format (fingerprint as the value name)
        Result = Storage.ValueExists(FingerprintSHA1) || Storage.ValueExists(FingerprintSHA256);
      }
    }
  });
  return Result;
}

void FtpFileSystem::CacheCertificate(const QString & SiteKey, const QString & FingerprintSHA256, int Failures)
{
  // TTerminal::CacheCertificate: stored immediately, both into the
  // configuration in memory (saved with it later) and into the file
  QString CertificateData = FormatCertificateData(FingerprintSHA256, Failures);
  RunInMainThread([&]()
  {
    Configuration & Config = Configuration::Instance();
    IniStorage Storage(&Config.Ini());
    if (Storage.OpenSubKey(FtpsCertificateStorageKey, true))
    {
      Storage.WriteString(SiteKey, CertificateData);
    }
    QString FileName = Config.IniFileName();
    // (not with /ini=nul; serialized with Configuration::WriteDisk by its lock
    // file; an existing file that cannot be read is never overwritten)
    if (!FileName.isEmpty() && !Config.IsNulStorage())
    {
      QDir().mkpath(QFileInfo(FileName).absolutePath());
      QLockFile Lock(FileName + QStringLiteral(".lock"));
      Lock.setStaleLockTime(30 * 1000);
      bool Locked = Lock.tryLock(10 * 1000);
      IniFile Disk;
      bool Loaded = !QFileInfo::exists(FileName) || Disk.Load(FileName);
      IniStorage DiskStorage(&Disk);
      if (Loaded && DiskStorage.OpenSubKey(FtpsCertificateStorageKey, true))
      {
        DiskStorage.WriteString(SiteKey, CertificateData);
        Disk.Save(FileName);
      }
      if (Locked)
      {
        Lock.unlock();
      }
    }
  });
}

void FtpFileSystem::VerifyCertificate(const QList<QSslError> & Errors)
{
  // TFTPFileSystem::HandleAsynchRequestVerifyCertificate
  FCertificate = FControl->peerCertificate();
  FCertificateFingerprintSHA256 = QString::fromLatin1(FCertificate.digest(QCryptographicHash::Sha256).toHex(':'));
  FCertificateFingerprintSHA1 = QString::fromLatin1(FCertificate.digest(QCryptographicHash::Sha1).toHex(':'));
  QList<QSslCertificate> Chain = FControl->peerCertificateChain();

  // The first verification error (FileZilla's verify_callback keeps the
  // first one) is what WinSCP shows and caches; the host name is checked
  // separately (VerifyCertificateHostName)
  bool Trusted = true;
  bool NameMismatch = false;
  int VerificationResult = X509_V_OK;
  QString VerificationResultStr;
  qsizetype VerificationDepth = 0;
  for (const QSslError & Error : Errors)
  {
    if (Error.error() == QSslError::HostNameMismatch)
    {
      NameMismatch = true;
      continue;
    }
    if (Trusted)
    {
      Trusted = false;
      VerificationResult = OpenSslVerifyError(Error);
      VerificationResultStr = CertificateErrorText(Error);
      VerificationDepth = std::max<qsizetype>(Chain.indexOf(Error.certificate()), 0);
    }
  }
  LogLine(QStringLiteral("Verifying certificate for \"%1\" with fingerprint %2 and %3 failures")
    .arg(FCertificate.subjectInfo(QSslCertificate::Organization).join(QStringLiteral(", ")), FCertificateFingerprintSHA256)
    .arg(VerificationResult));
  bool IsHostNameIPAddress = !QHostAddress(FData.HostName).isNull();
  bool CertificateHostNameVerified = !IsHostNameIPAddress && !NameMismatch;
  bool Verified = Trusted && CertificateHostNameVerified;
  // TSessionData::GetSiteKey
  QString SiteKey = QStringLiteral("%1:%2").arg(FData.HostName).arg(FData.PortNumber);
  if (!Verified)
  {
    if (IsCertificateCached(SiteKey, FCertificateFingerprintSHA1, FCertificateFingerprintSHA256, VerificationResult))
    {
      LogLine(QStringLiteral("Certificate matches cached fingerprint and failures"));
      Verified = true;
    }
    else if (VerifyCertificateAgainstSessionData())
    {
      Verified = true;
    }
  }

  const QString SummarySeparator = QStringLiteral("\n\n");
  QStringList Summary;
  if (!Trusted)
  {
    Summary << (VerificationResultStr + QLatin1Char(' ') + Tr(CERT_ERRDEPTH).arg(VerificationDepth + 1));
  }
  if (IsHostNameIPAddress)
  {
    Summary << Tr(CERT_IP_CANNOT_VERIFY);
  }
  else if (!CertificateHostNameVerified)
  {
    Summary << Tr(CERT_NAME_MISMATCH).arg(FData.HostName);
  }
  if (Summary.isEmpty())
  {
    Summary << Tr(CERT_OK);
  }
  // FormatValidityTime: short date and long time
  QLocale Locale;
  QString DateTimeFormat = Locale.dateFormat(QLocale::ShortFormat) + QLatin1Char(' ') + Locale.timeFormat(QLocale::LongFormat);
  FCertificateText = Tr(CERT_TEXT2).arg(
    FormatContact(FCertificate, true), FormatContact(FCertificate, false),
    Locale.toString(FCertificate.effectiveDate().toLocalTime(), DateTimeFormat),
    Locale.toString(FCertificate.expiryDate().toLocalTime(), DateTimeFormat),
    FCertificateFingerprintSHA256, FCertificateFingerprintSHA1, Summary.join(SummarySeparator));

  if (Verified)
  {
    return;
  }
  QString OnceKey = SiteKey + QLatin1Char('=') + FCertificateFingerprintSHA256;
  if (FAcceptedOnce.contains(OnceKey))
  {
    return;
  }
  // TTerminal::ConfirmCertificate
  unsigned int Answer = FUI->QueryUser(Tr(VERIFY_CERT_PROMPT3).arg(FCertificateText), QString(), qaYes | qaNo | qaCancel);
  if (Answer == qaYes)
  {
    CacheCertificate(SiteKey, FCertificateFingerprintSHA256, VerificationResult);
  }
  else if (Answer == qaNo)
  {
    FAcceptedOnce.insert(OnceKey);
  }
  else
  {
    DoClose();
    throw FatalException(Tr(IDS_ERRORMSG_CERTREJECTED));
  }
}

void FtpFileSystem::ProcessWelcome(const Reply & R)
{
  // TFTPFileSystem::HandleReplyStatus (FLastCode == 220): the HOST command
  // also replies 220, only the initial message is kept
  if (!FWelcomeMessage.isEmpty())
  {
    return;
  }
  FWelcomeMessage = R.Lines.join(QLatin1Char('\n'));
  if (FShowFtpWelcomeMessage)
  {
    FUI->DisplayBanner(FData.SessionName(), FWelcomeMessage);
  }
  if ((FData.FtpTransferActiveImmediately == asAuto) && FWelcomeMessage.contains(QStringLiteral("Idea FTP Server")))
  {
    // (the TLS handshake on the data connection always runs while waiting
    // for the 1yz reply here, so nothing else is needed)
    LogLine(QStringLiteral("The server requires TLS/SSL handshake on transfer connection before responding 1yz to STOR/APPE"));
  }
  if (FWelcomeMessage.contains(QStringLiteral("Microsoft FTP Service"), Qt::CaseInsensitive) && !FIIS)
  {
    LogLine(QStringLiteral("IIS detected."));
    FIIS = true;
  }
}

void FtpFileSystem::Login(QString & Password)
{
  // CFtpControlSocket::LogOnToServer
  FAwaitingWelcome = true;
  Reply R;
  try
  {
    R = ReadReply();
  }
  catch (...)
  {
    FAwaitingWelcome = false;
    throw;
  }
  FAwaitingWelcome = false;
  if (R.Class() != 2)
  {
    if (R.Lines.value(0).startsWith(QStringLiteral("SSH")))
    {
      throw FatalException(Tr(CONNECTION_FAILED), Tr(SFTP_AS_FTP_ERROR));
    }
    QStringList More = R.Lines;
    throw FatalException(Tr(CONNECTION_FAILED), More.join(QLatin1Char('\n')));
  }
  ProcessWelcome(R);

  if (FData.FtpHost == asOn)
  {
    // CONNECT_HOST (RFC 7151 virtual hosts); the reply is not checked
    Command(QStringLiteral("HOST ") + FData.HostName);
  }

  if ((FData.Ftps == ftpsExplicitTls) || (FData.Ftps == ftpsExplicitSsl))
  {
    QStringList Auths;
    if (FData.Ftps == ftpsExplicitTls)
    {
      // Try to fall back to AUTH SSL
      Auths << QStringLiteral("AUTH TLS") << QStringLiteral("AUTH SSL");
    }
    else
    {
      Auths << QStringLiteral("AUTH SSL");
    }
    bool Ok = false;
    for (const QString & Auth : Auths)
    {
      R = Command(Auth);
      if ((R.Class() == 2) || (R.Class() == 3))
      {
        Ok = true;
        break;
      }
    }
    if (!Ok)
    {
      throw FatalException(Tr(CONNECTION_FAILED), R.Lines.join(QLatin1Char('\n')));
    }
    StartTls(FControl.get(), true);
  }

  // Logon sequences for the various firewalls (FtpProxyLogonType):
  // blocks of 3 numbers: command to send, next point in the sequence when a
  // 2xx reply is received, next point when a 3xx reply is received
  const int LO = -2;
  const int ER = -1;
  static const int LogonSeq[9][20] = {
    {0,LO,3, 1,LO,6, 12,LO,ER}, // no firewall
    {3,6,3, 4,6,ER, 5,9,9, 0,LO,12, 1,LO,15, 12,LO,ER}, // SITE hostname
    {3,6,3, 4,6,ER, 6,LO,9, 1,LO,12, 12,LO,ER}, // USER after logon
    {7,3,3, 0,LO,6, 1,LO,9, 12,LO,ER}, // proxy OPEN
    {3,6,3, 4,6,ER, 0,LO,9, 1,LO,12, 12,LO,ER}, // Transparent
    {6,LO,3, 1,LO,6, 12,LO,ER}, // USER remoteID@remotehost
    {8,6,3, 4,6,ER, 0,LO,9, 1,LO,12, 12,LO,ER}, // USER fireID@remotehost
    {9,ER,3, 1,LO,6, 2,LO,ER}, // USER remoteID@remotehost fireID
    {10,LO,3, 11,LO,6, 2,LO,ER} // USER remoteID@fireID@remotehost
  };
  int LogonType = FData.FtpProxyLogonType;
  if ((LogonType < 0) || (LogonType >= 9))
  {
    LogonType = 0;
  }
  QString HostName = FData.HostName;
  if (FData.PortNumber != FtpPortNumber)
  {
    // add port to hostname (only if port is not 21)
    HostName += QStringLiteral(":%1").arg(FData.PortNumber);
  }
  if (LogonType != 0)
  {
    LogLine(QStringLiteral("Trying to access %1 through ftp proxy...").arg(HostName));
  }
  const QString UserName = FData.UserName;
  const QString FwUser = FData.ProxyUsername;
  const QString FwPass = FData.ProxyPassword;
  auto ObtainPassword = [&]()
  {
    if (!Password.isEmpty())
    {
      return;
    }
    if (UserName.compare(AnonymousUserName, Qt::CaseInsensitive) == 0)
    {
      Password = AnonymousPassword;
    }
    else if (FClientCertificateLoaded)
    {
      // TFTPFileSystem::HandleAsynchRequestNeedPass
      LogLine(QStringLiteral("Server asked for password, but we are using certificate, and no password was specified upfront, using fake password"));
      Password = QStringLiteral("USINGCERT");
    }
    else
    {
      QList<Prompt> Prompts{Prompt{QStringLiteral("&Password: "), false, QString()}};
      if (!FUI->PromptUser(UserInterface::pkPassword, FData.SessionName(), QString(), Prompts))
      {
        throw FatalException(Tr(CONNECTION_FAILED), Tr(USER_TERMINATED));
      }
      Password = Prompts[0].Result;
      if (!Password.isEmpty())
      {
        FAnyPassword = true;
      }
    }
  };
  auto IsAscii = [](const QString & Str)
  {
    for (QChar C : Str)
    {
      if (C.unicode() > 127)
      {
        return false;
      }
    }
    return true;
  };
  bool CharsetFallback = false;
  int State = 0;
  for (;;)
  {
    QString CommandText;
    bool IsPass = false;
    switch (LogonSeq[LogonType][State])
    {
      case 0:
        CommandText = QStringLiteral("USER ") + UserName;
        break;
      case 1:
        ObtainPassword();
        CommandText = QStringLiteral("PASS ") + Password;
        IsPass = true;
        break;
      case 2:
        CommandText = QStringLiteral("ACCT ") + FwPass;
        break;
      case 3:
        CommandText = QStringLiteral("USER ") + FwUser;
        break;
      case 4:
        CommandText = QStringLiteral("PASS ") + FwPass;
        IsPass = true;
        break;
      case 5:
        CommandText = QStringLiteral("SITE ") + HostName;
        break;
      case 6:
        CommandText = QStringLiteral("USER ") + UserName + QLatin1Char('@') + HostName;
        break;
      case 7:
        CommandText = QStringLiteral("OPEN ") + HostName;
        break;
      case 8:
        CommandText = QStringLiteral("USER ") + FwUser + QLatin1Char('@') + HostName;
        break;
      case 9:
        CommandText = QStringLiteral("USER ") + UserName + QLatin1Char('@') + HostName + QLatin1Char(' ') + FwUser;
        break;
      case 10:
        CommandText = QStringLiteral("USER ") + UserName + QLatin1Char('@') + FwUser + QLatin1Char('@') + HostName;
        break;
      case 11:
        ObtainPassword();
        CommandText = QStringLiteral("PASS ") + Password + QLatin1Char('@') + FwPass;
        IsPass = true;
        break;
      case 12:
        CommandText = QStringLiteral("ACCT ") + (FData.FtpAccount.isEmpty() ? QStringLiteral("default") : FData.FtpAccount);
        break;
    }
    R = Command(CommandText);
    // 530 = "Not logged in.", 501 = "Login incorrect." (ProFTPD empty password code)
    if (IsPass && ((R.Code == 530) || (R.Code == 501)))
    {
      FPasswordFailed = true;
      return;
    }
    int Class = R.Class();
    if ((Class != 2) && (Class != 3))
    {
      // Fall back to local charset for the case that the server might not
      // support UTF8 and the login data contains non-ascii characters
      if (FUtf8 && (FData.NotUtf == asAuto) && !CharsetFallback &&
          (!IsAscii(UserName) || !IsAscii(Password) || !IsAscii(FData.FtpAccount)))
      {
        LogLine(QStringLiteral("Login data contains non-ascii characters and server might not be UTF-8 aware. Trying local charset."));
        FUtf8 = false;
        CharsetFallback = true;
        State = 0;
        continue;
      }
      QStringList More;
      if (R.Code == 530)
      {
        More << Tr(AUTHENTICATION_FAILED);
      }
      More << R.Lines;
      throw FatalException(Tr(CONNECTION_FAILED), More.join(QLatin1Char('\n')));
    }
    State = LogonSeq[LogonType][State + Class - 1];
    if (State == LO)
    {
      break;
    }
    if (State == ER)
    {
      throw FatalException(Tr(CONNECTION_FAILED), R.Lines.join(QLatin1Char('\n')));
    }
  }
}

void FtpFileSystem::ProcessFeatures(const Reply & FeatReply)
{
  // TFTPFileSystem::HandleFeatReply + ProcessFeatures
  FFeatures.clear();
  FSupportedCommands.clear();
  FSupportedSiteCommands.clear();
  FHashAlgs.clear();
  FSupportsAnyChecksumFeature = false;
  FHasClntCommand = false;
  if ((FeatReply.Code == 211) && (FeatReply.Lines.size() > 2))
  {
    for (qsizetype Index = 1; Index < FeatReply.Lines.size() - 1; Index++)
    {
      QString Feature = FeatReply.Lines[Index].trimmed();
      if (Feature.isEmpty())
      {
        continue;
      }
      FFeatures << Feature;
      QString Command = Feature.section(QLatin1Char(' '), 0, 0).toUpper();
      QString Args = Feature.section(QLatin1Char(' '), 1);
      FSupportedCommands.insert(Command);
      if (Command == QStringLiteral("SITE"))
      {
        // Serv-U lists all SITE commands in one line, ProFTPD separately
        for (const QString & Arg : Args.split(QLatin1Char(';'), Qt::SkipEmptyParts))
        {
          FSupportedSiteCommands.insert(Arg.trimmed().toUpper());
        }
      }
      else if (Command == QStringLiteral("HASH"))
      {
        for (QString Alg : Args.split(QLatin1Char(';'), Qt::SkipEmptyParts))
        {
          Alg = Alg.trimmed();
          if (Alg.endsWith(QLatin1Char('*')))
          {
            Alg.chop(1);
          }
          // FTP HASH alg names follow IANA as we do, but using uppercase
          FHashAlgs << Alg.toLower();
          FSupportsAnyChecksumFeature = true;
        }
      }
      else if (Command == QStringLiteral("CLNT"))
      {
        // CFtpControlSocket::DiscardLine (m_hasClntCmd)
        FHasClntCommand = true;
      }
      for (const auto & Entry : FtpChecksumCommands)
      {
        if (Command == QLatin1String(Entry.Command))
        {
          FSupportsAnyChecksumFeature = true;
        }
      }
    }
  }
}

void FtpFileSystem::Startup()
{
  // SYST (CONNECT_SYST + TFTPFileSystem::HandleReplyStatus)
  Reply R = Command(QStringLiteral("SYST"));
  if (R.Class() == 2)
  {
    FSystem = R.Lines.join(QLatin1Char('\n')).trimmed();
    FMVS = FSystem.startsWith(QStringLiteral("MVS"));
    if (FMVS)
    {
      LogLine(QStringLiteral("MVS system detected."));
      FServerType |= FtpListParser::stMVS;
    }
    if (FSystem.startsWith(QStringLiteral("BS-2000")))
    {
      FServerType |= FtpListParser::stBS2000;
    }
    // The welcome message usually contains "Microsoft FTP Service" but can be empty
    if (FSystem.contains(QStringLiteral("Windows_NT"), Qt::CaseInsensitive) && !FIIS)
    {
      LogLine(QStringLiteral("IIS detected."));
      FIIS = true;
    }
    // VMS system type. VMS V5.5-2.
    FVMS = FSystem.startsWith(QStringLiteral("VMS "));
    if (FVMS)
    {
      LogLine(QStringLiteral("VMS system detected."));
      FServerType |= FtpListParser::stVMS;
    }
    if ((FListAll == asAuto) && (FSystem.contains(QStringLiteral("Personal FTP Server")) || FMVS || FVMS))
    {
      LogLine(QStringLiteral("Server is known not to support LIST -a"));
      FListAll = asOff;
    }
    if ((FWorkFromCwd == asAuto) && FVMS)
    {
      LogLine(QStringLiteral("Server is known to require use of relative paths"));
      FWorkFromCwd = asOn;
    }
    if (FSystem.contains(QStringLiteral("FileZilla")))
    {
      LogLine(QStringLiteral("FileZilla server detected."));
      FFileZilla = true;
    }
  }

  // FEAT
  R = Command(QStringLiteral("FEAT"));
  ProcessFeatures(R);

  // UTF-8: on Linux the local charset is UTF-8 as well, so with "auto" UTF-8
  // is used unless the server sends names in invalid UTF-8 (FtpListParser::Utf8)
  bool Utf8Announced = (FData.NotUtf == asOff) || ((FData.NotUtf == asAuto) && SupportsCommand(QStringLiteral("UTF8")));
  if (FHasClntCommand)
  {
    // Some servers refuse to enable UTF8 if client does not send CLNT command
    // (CONNECT_CLNT is followed by OPTS UTF8 ON unconditionally)
    QString Version = QCoreApplication::applicationVersion();
    Command(QStringLiteral("CLNT NixSCP-release-%1").arg(Version.isEmpty() ? QStringLiteral("0") : Version));
    Command(QStringLiteral("OPTS UTF8 ON"));
  }
  else if (Utf8Announced && !FFileZilla)
  {
    // Handle servers that disobey RFC 2640 that have UTF8 in the FEAT
    // response but do not use UTF8 unless OPTS UTF8 ON gets send.
    Command(QStringLiteral("OPTS UTF8 ON"));
  }

  if (FTls)
  {
    Command(QStringLiteral("PBSZ 0"));
    R = Command(QStringLiteral("PROT P"));
    FProtP = (R.Class() == 2) || (R.Class() == 3);
  }

  // Enable facts the parser understands but the server does not send by default
  // (FileZilla 2 never got to send OPTS MLST, see CFtpControlSocket::DiscardLine)
  if (UsingMlsd())
  {
    QString Facts;
    for (const QString & Feature : std::as_const(FFeatures))
    {
      if (Feature.startsWith(QStringLiteral("MLST "), Qt::CaseInsensitive))
      {
        Facts = Feature.mid(5);
      }
    }
    static const QStringList Wanted{
      QStringLiteral("type"), QStringLiteral("size"), QStringLiteral("modify"), QStringLiteral("create"),
      QStringLiteral("perm"), QStringLiteral("unix.mode"), QStringLiteral("unix.owner"), QStringLiteral("unix.user"),
      QStringLiteral("unix.group"), QStringLiteral("unix.uid"), QStringLiteral("unix.gid")};
    QString OptsFacts;
    bool HadUnset = false;
    for (QString Fact : Facts.split(QLatin1Char(';'), Qt::SkipEmptyParts))
    {
      bool Enabled = Fact.endsWith(QLatin1Char('*'));
      if (Enabled)
      {
        Fact.chop(1);
      }
      if (Wanted.contains(Fact.toLower()))
      {
        HadUnset |= !Enabled;
        OptsFacts += Fact + QLatin1Char(';');
      }
    }
    if (HadUnset)
    {
      Command(QStringLiteral("OPTS MLST ") + OptsFacts);
    }
  }

  // TFTPFileSystem::DoStartup: post login commands
  static const QRegularExpression LineBreak(QStringLiteral("\r\n|\r|\n"));
  for (const QString & PostLoginCommand : FData.PostLoginCommands.split(LineBreak, Qt::SkipEmptyParts))
  {
    if (!PostLoginCommand.isEmpty())
    {
      R = Command(PostLoginCommand);
      CheckReply(R, QString(), true);
    }
  }
  // the commands may change the working directory or transfer type
  FCurrentDirectory.clear();
  FTransferType = QChar();

  // CSID client/server identification
  if (SupportsCommand(QStringLiteral("CSID")))
  {
    QString Version = QCoreApplication::applicationVersion();
    R = Command(QStringLiteral("CSID Name=NixSCP;Version=%1;").arg(Version.isEmpty() ? QStringLiteral("0") : Version));
    if ((R.Class() == 2) && (R.Lines.size() == 1))
    {
      QString Name;
      QString ServerVersion;
      for (const QString & Token : R.Lines[0].split(QLatin1Char(';'), Qt::SkipEmptyParts))
      {
        QString Fact = Token.section(QLatin1Char('='), 0, 0).trimmed();
        QString Value = Token.section(QLatin1Char('='), 1);
        if (Fact.compare(QStringLiteral("Name"), Qt::CaseInsensitive) == 0)
        {
          Name = Value;
        }
        else if (Fact.compare(QStringLiteral("Version"), Qt::CaseInsensitive) == 0)
        {
          ServerVersion = Value;
        }
      }
      if (!Name.isEmpty())
      {
        FServerID = ServerVersion.isEmpty() ? Name : (Name + QLatin1Char(' ') + ServerVersion);
        LogLine(QStringLiteral("Server: %1").arg(FServerID));
      }
    }
    else
    {
      LogLine(QStringLiteral("CSID command failed"));
    }
  }

  // retrieve initialize working directory to save it as home directory
  ReadCurrentDirectory();
  FHomeDirectory = FCurrentDirectory;
}

void FtpFileSystem::ReadCurrentDirectory()
{
  // TFTPFileSystem::ReadCurrentDirectory
  Reply R = Command(QStringLiteral("PWD"));
  bool Result = false;
  QString Path;
  if ((R.Class() == 2) && ((R.Code == 257) || FData.FtpAnyCodeForPwd) && (R.Lines.size() == 1))
  {
    Path = R.Lines[0];
    qsizetype P = Path.indexOf(QLatin1Char('"'));
    if (P < 0)
    {
      // some systems use single quotes, be tolerant
      P = Path.indexOf(QLatin1Char('\''));
    }
    if (P >= 0)
    {
      Path = Path.mid(P);
      Result = Unquote(Path);
    }
    else
    {
      Path = Path.section(QLatin1Char(' '), 0, 0);
      Result = true;
    }
  }
  if (!Result)
  {
    throw ExtException(Tr(FTP_RESPONSE_ERROR).arg(QStringLiteral("PWD"), R.Lines.join(QLatin1Char('\n'))));
  }
  if (Path.isEmpty() || !Path.startsWith(QLatin1Char('/')))
  {
    Path = QStringLiteral("/") + Path;
  }
  while ((Path.size() > 1) && Path.endsWith(QLatin1Char('/')))
  {
    Path.chop(1);
  }
  FCurrentDirectory = Path;
}

bool FtpFileSystem::Unquote(QString & Str) const
{
  // TFTPFileSystem::Unquote
  enum { INIT, QUOTE, QUOTED, DONE } State = INIT;
  qsizetype Index = 0;
  QChar Quote;
  while (Index < Str.size())
  {
    switch (State)
    {
      case INIT:
        if ((Str[Index] == QLatin1Char('"')) || (Str[Index] == QLatin1Char('\'')))
        {
          Quote = Str[Index];
          State = QUOTED;
          Str.remove(Index, 1);
        }
        else
        {
          Str.clear();
        }
        break;

      case QUOTED:
        if (Str[Index] == Quote)
        {
          State = QUOTE;
          Str.remove(Index, 1);
        }
        else
        {
          Index++;
        }
        break;

      case QUOTE:
        if (Str[Index] == Quote)
        {
          Index++;
          State = QUOTED;
        }
        else
        {
          // end of quoted string, trim the rest
          Str.truncate(Index);
          State = DONE;
        }
        break;

      case DONE:
        break;
    }
  }
  return (State == DONE) || (State == QUOTE);
}

void FtpFileSystem::DoClose()
{
  if (FKeepaliveTimer)
  {
    FKeepaliveTimer->stop();
  }
  if (FControl)
  {
    FControl->abort();
    FControl.reset();
  }
  FLoggedIn = false;
  FProtP = false;
  FCurrentDirectory.clear();
  FTransferType = QChar();
  FMultiLineCode.clear();
  FPartialReply = Reply();
  FFileListCacheDir.clear();
  FFileListCache.clear();
}

void FtpFileSystem::Close()
{
  if (FControl && (FControl->state() == QAbstractSocket::ConnectedState))
  {
    try
    {
      SendCommand(QStringLiteral("QUIT"), false);
      Reply R;
      QElapsedTimer Timer;
      Timer.start();
      while (!PollReply(R) && (Timer.elapsed() < 2000) && (FControl->state() == QAbstractSocket::ConnectedState))
      {
        FControl->waitForReadyRead(100);
      }
    }
    catch (...)
    {
    }
  }
  DoClose();
}

void FtpFileSystem::Idle()
{
  if ((FBusy > 0) || !IsOpened() || (FData.FtpPingType == fptOff))
  {
    return;
  }
  qint64 IntervalMs = std::max(FData.FtpPingInterval, 1) * 1000LL;
  OperationGuard Guard(this);
  try
  {
    // TFTPFileSystem::Idle: keep session alive by reading the directory
    if ((FData.FtpPingType == fptDirectoryListing) && FLastDataSent.isValid() &&
        (FLastDataSent.elapsed() > IntervalMs * 4))
    {
      LogLine(QStringLiteral("Dummy directory read to keep session alive."));
      FLastDataSent.start();
      QString Directory = FCurrentDirectory.isEmpty() ? FHomeDirectory : FCurrentDirectory;
      try
      {
        DoReadDirectory(Directory, !UsingMlsd() && (FListAll == asOn));
      }
      catch (const FatalException &)
      {
        throw;
      }
      catch (const ExtException &)
      {
        // ignore non-fatal errors (i.e. current directory may not exist anymore)
      }
      return;
    }
    // CFtpControlSocket::OnTimer + SendKeepAliveCommand: a random dummy command
    if (FLastActivity.isValid() && (FLastActivity.elapsed() < IntervalMs))
    {
      return;
    }
    LogLine(QStringLiteral("Sending dummy command to keep session alive."));
    static const char * const Commands[4] = {"PWD", "REST 0", "TYPE A", "TYPE I"};
    QString DummyCommand = QString::fromLatin1(Commands[QRandomGenerator::global()->bounded(4)]);
    Reply R = Command(DummyCommand);
    if (DummyCommand.startsWith(QStringLiteral("TYPE ")))
    {
      FTransferType = (R.Class() == 2) ? DummyCommand.back() : QChar();
    }
    else if ((R.Code == 421) || (FControl == nullptr))
    {
      DoClose();
    }
  }
  catch (...)
  {
    // the next operation reports the lost connection
    LogLine(QStringLiteral("Keepalive failed"));
    DoClose();
  }
}

//---------------------------------------------------------------------------
// Paths

QString FtpFileSystem::AbsolutePath(const QString & Path) const
{
  if (Path.startsWith(QLatin1Char('/')))
  {
    return NormalizePath(Path);
  }
  QString Base = FCurrentDirectory.isEmpty() ? FHomeDirectory : FCurrentDirectory;
  if (Path.isEmpty())
  {
    return NormalizePath(Base);
  }
  return NormalizePath(Base + QLatin1Char('/') + Path);
}

void FtpFileSystem::ChangeWorkingDirectory(const QString & Directory)
{
  if (Directory == FCurrentDirectory)
  {
    return;
  }
  if ((FWorkFromCwd == asOn) && !FCurrentDirectory.isEmpty())
  {
    // TFTPFileSystem::DoChangeDirectory: one level at a time, with relative
    // names (CWD subdirectory / CDUP)
    QString Target = (Directory == QStringLiteral("/")) ? Directory : (Directory + QLatin1Char('/'));
    while (FCurrentDirectory != Directory)
    {
      QString Actual = (FCurrentDirectory == QStringLiteral("/")) ? FCurrentDirectory : (FCurrentDirectory + QLatin1Char('/'));
      Reply R;
      QString Next;
      if (Target.startsWith(Actual))
      {
        QString SubDirectory = Target.mid(Actual.size()).section(QLatin1Char('/'), 0, 0);
        R = Command(QStringLiteral("CWD ") + SubDirectory);
        Next = UnixPath::Combine(FCurrentDirectory, SubDirectory);
      }
      else
      {
        R = Command(QStringLiteral("CDUP"));
        Next = UnixPath::ExtractFileDir(FCurrentDirectory);
      }
      if (R.Class() != 2)
      {
        FCurrentDirectory.clear();
        ReplyError(R, Tr(LIST_DIR_ERROR).arg(Directory));
      }
      FCurrentDirectory = Next.isEmpty() ? QStringLiteral("/") : Next;
    }
    return;
  }
  Reply R = Command(QStringLiteral("CWD ") + Directory);
  if (R.Class() != 2)
  {
    FCurrentDirectory.clear();
    ReplyError(R, Tr(LIST_DIR_ERROR).arg(Directory));
  }
  FCurrentDirectory = Directory;
}

QString FtpFileSystem::FileCommandPath(const QString & Abs)
{
  // TFTPFileSystem::EnsureLocationWhenWorkFromCwd
  if ((FWorkFromCwd == asOn) && (Abs != QStringLiteral("/")))
  {
    ChangeWorkingDirectory(UnixPath::ExtractFileDir(Abs));
    return UnixPath::ExtractFileName(Abs);
  }
  return Abs;
}

void FtpFileSystem::EnsureType(QChar Type)
{
  if (FTransferType == Type)
  {
    return;
  }
  Reply R = Command(QStringLiteral("TYPE ") + Type);
  // Do not really care if TYPE succeeded or not (as FileZilla's LISTFILE_TYPE)
  if (R.Class() == 2)
  {
    FTransferType = Type;
  }
}

void FtpFileSystem::InvalidateCache(const QString & Path)
{
  if (UnixPath::ExtractFileDir(Path) == FFileListCacheDir)
  {
    FFileListCacheDirty.insert(UnixPath::ExtractFileName(Path));
  }
  if (Path == FFileListCacheDir)
  {
    FFileListCacheDir.clear();
  }
}

QString FtpFileSystem::CanonicalizePath(const QString & Path)
{
  OperationGuard Guard(this);
  CheckConnection();
  QString Abs = AbsolutePath(Path);
  Reply R = Command(QStringLiteral("CWD ") + Abs);
  if (R.Class() == 2)
  {
    FCurrentDirectory = Abs;
    // the server's idea of the path (TFTPFileSystem::DoChangeDirectory + ReadCurrentDirectory)
    Reply Pwd = Command(QStringLiteral("PWD"));
    QString Result = Abs;
    if ((Pwd.Class() == 2) && (Pwd.Lines.size() == 1))
    {
      QString Text = Pwd.Lines[0];
      qsizetype P = Text.indexOf(QLatin1Char('"'));
      if (P >= 0)
      {
        Text = Text.mid(P);
        if (Unquote(Text) && Text.startsWith(QLatin1Char('/')))
        {
          Result = NormalizePath(Text);
        }
      }
    }
    FCurrentDirectory = Result;
    return Result;
  }
  FCurrentDirectory.clear();
  // Not a directory, maybe a file
  RemoteFile File;
  QString Error;
  if (R.Class() == 5)
  {
    try
    {
      if (ReadFileInternal(Abs, File, Error) && !File.IsDirectory)
      {
        return Abs;
      }
    }
    catch (const FatalException &)
    {
      throw;
    }
    catch (const ExtException &)
    {
    }
  }
  ReplyError(R, Tr(CANT_GET_REAL_PATH).arg(Path));
}

QString FtpFileSystem::HomeDirectory()
{
  return FHomeDirectory;
}

//---------------------------------------------------------------------------
// Data connections

bool FtpFileSystem::ParsePasvReply(const Reply & R, QString & Host, quint16 & Port) const
{
  static const QRegularExpression Pasv(QStringLiteral("(\\d+),(\\d+),(\\d+),(\\d+),(\\d+),(\\d+)"));
  QRegularExpressionMatch Match = Pasv.match(R.Lines.join(QLatin1Char(' ')));
  if (!Match.hasMatch())
  {
    return false;
  }
  int Values[6];
  for (int I = 0; I < 6; I++)
  {
    Values[I] = Match.captured(I + 1).toInt();
    if ((Values[I] < 0) || (Values[I] > 255))
    {
      return false;
    }
  }
  Host = QStringLiteral("%1.%2.%3.%4").arg(Values[0]).arg(Values[1]).arg(Values[2]).arg(Values[3]);
  Port = static_cast<quint16>((Values[4] << 8) | Values[5]);
  return (Port != 0);
}

bool FtpFileSystem::ParseEpsvReply(const Reply & R, quint16 & Port) const
{
  static const QRegularExpression Epsv(QStringLiteral("\\((.)\\1\\1(\\d+)\\1\\)"));
  QRegularExpressionMatch Match = Epsv.match(R.Lines.join(QLatin1Char(' ')));
  if (!Match.hasMatch())
  {
    return false;
  }
  int Value = Match.captured(2).toInt();
  if ((Value <= 0) || (Value > 65535))
  {
    return false;
  }
  Port = static_cast<quint16>(Value);
  return true;
}

bool FtpFileSystem::ListenForDataConnection(QTcpServer * Server, const QHostAddress & Local, QString & Error)
{
  // CTransferSocket::Create: listening port within Preferences > Network >
  // "Limit listening ports" (TConfiguration::HasLocalPortNumberLimits),
  // starting at a random port of the range
  bool HasLimits = (FLocalPortNumberMin > 0) && (FLocalPortNumberMax >= FLocalPortNumberMin);
  if (!HasLimits)
  {
    if (!Server->listen(Local, 0))
    {
      Error = Server->errorString();
      return false;
    }
    return true;
  }
  int Min = FLocalPortNumberMin;
  int Max = std::min(FLocalPortNumberMax, 65535);
  int StartPort = Min + static_cast<int>(QRandomGenerator::global()->bounded(static_cast<quint32>(Max - Min + 1)));
  int Port = StartPort;
  while (!Server->listen(Local, static_cast<quint16>(Port)))
  {
    Port++;
    if (Port > Max)
    {
      Port = Min;
    }
    if (Port == StartPort)
    {
      Error = Tr(IDS_ERRORMSG_CANTCREATEDUETOPORTRANGE);
      return false;
    }
  }
  return true;
}

QString FtpFileSystem::FormatPortCommand(const QHostAddress & Local, quint16 Port) const
{
  // CFtpControlSocket::FormatPortCmd: the external IP address of
  // Preferences > Network (OPTION_TRANSFERIP, IP or host name), otherwise
  // the address of the control connection (from the operating system)
  bool IsV4 = false;
  quint32 V4 = Local.toIPv4Address(&IsV4);
  if (IsV4)
  {
    if (!FExternalIpAddress.isEmpty())
    {
      QHostAddress External;
      if (External.setAddress(FExternalIpAddress) && (External.protocol() == QAbstractSocket::IPv4Protocol))
      {
        V4 = External.toIPv4Address();
      }
      else
      {
        const QList<QHostAddress> Addresses = QHostInfo::fromName(FExternalIpAddress).addresses();
        for (const QHostAddress & Address : Addresses)
        {
          if (Address.protocol() == QAbstractSocket::IPv4Protocol)
          {
            V4 = Address.toIPv4Address();
            break;
          }
        }
      }
    }
    return QStringLiteral("PORT %1,%2,%3,%4,%5,%6")
      .arg((V4 >> 24) & 0xFF).arg((V4 >> 16) & 0xFF).arg((V4 >> 8) & 0xFF).arg(V4 & 0xFF)
      .arg(Port >> 8).arg(Port & 0xFF);
  }
  // (OPTION_TRANSFERIP6 is always empty in WinSCP)
  return QStringLiteral("EPRT |2|%1|%2|").arg(Local.toString()).arg(Port);
}

std::unique_ptr<FtpFileSystem::DataConnection> FtpFileSystem::OpenDataConnection(
  const QString & ACommand, qint64 * Rest, FileOperationProgress * Progress, const QString & Error)
{
  auto Data = std::make_unique<DataConnection>();
  bool Passive = FData.FtpPasvMode;
  QHostAddress Peer = FControl->peerAddress();
  bool IPv6 = (Peer.protocol() == QAbstractSocket::IPv6Protocol) && (AddressString(Peer).contains(QLatin1Char(':')));
  auto CantOpen = [&](const QString & Reason) -> ExtException
  {
    return ExtException(Error, Tr(IDS_ERRORMSG_CANTOPENTRANSFERCHANNEL).arg(Reason));
  };

  QString Host;
  quint16 Port = 0;
  if (Passive)
  {
    // PASV for IPv4, EPSV for IPv6 (as FileZilla), the other one as a fallback
    QStringList Commands = IPv6 ?
      QStringList{QStringLiteral("EPSV"), QStringLiteral("PASV")} :
      QStringList{QStringLiteral("PASV"), QStringLiteral("EPSV")};
    Reply R;
    bool Ok = false;
    for (const QString & Cmd : Commands)
    {
      R = Command(Cmd);
      if (R.Class() != 2)
      {
        continue;
      }
      if (Cmd == QStringLiteral("PASV"))
      {
        Ok = ParsePasvReply(R, Host, Port);
        if (Ok)
        {
          // CFtpControlSocket::CheckForcePasvIp. Through a SOCKS/HTTP proxy
          // the server address is known only when the host is an IP address
          // (CAsyncProxySocketLayer::GetPeerName)
          QString PeerHost;
          bool PeerKnown = true;
          if (UseProxy())
          {
            QHostAddress HostAddress(FData.HostName);
            PeerKnown = !HostAddress.isNull();
            PeerHost = PeerKnown ? AddressString(HostAddress) : QString();
          }
          else
          {
            PeerHost = AddressString(Peer);
          }
          switch (FData.FtpForcePasvIp)
          {
            case asOn:
              if (!PeerKnown && (FData.FtpProxyLogonType == 0))
              {
                // this is a hostname, not an IP, but it should not be a problem
                PeerHost = FData.HostName;
                PeerKnown = true;
              }
              if (PeerKnown && (PeerHost != Host))
              {
                LogLine(QStringLiteral("Using host address %1 instead of the one suggested by the server: %2").arg(PeerHost, Host));
                Host = PeerHost;
              }
              break;
            case asOff:
              break;
            default:
              if (!PeerKnown)
              {
                LogLine(QStringLiteral("Error retrieving server address, cannot test if address is routable"));
              }
              else if (!IsRoutableAddress(Host) && IsRoutableAddress(PeerHost))
              {
                LogLine(QStringLiteral("Server sent passive reply with unroutable address %1, using host address instead.").arg(Host));
                Host = PeerHost;
              }
              break;
          }
        }
      }
      else
      {
        Ok = ParseEpsvReply(R, Port);
        // the same server as the control connection
        Host = UseProxy() ? FData.HostName : AddressString(Peer);
      }
      if (Ok)
      {
        break;
      }
    }
    if (!Ok)
    {
      if (R.Class() == 2)
      {
        throw ExtException(Error, Tr(FTP_RESPONSE_ERROR).arg(FLastCommand, R.Lines.join(QLatin1Char('\n'))));
      }
      ReplyError(R, Error);
    }

    Data->Socket = std::make_unique<QSslSocket>();
    ConfigureSocket(Data->Socket.get(), true);
    Data->WatchSocket();
    try
    {
      ConnectSocket(Data->Socket.get(), Host, Port, Progress, false);
    }
    catch (const AbortException &)
    {
      throw;
    }
    catch (const ExtException & E)
    {
      throw CantOpen(E.MoreMessages().isEmpty() ? E.Message() : (E.Message() + QLatin1Char(' ') + E.MoreMessages()));
    }
  }
  else
  {
    Data->Server = std::make_unique<SslServer>();
    QHostAddress Local = FControl->localAddress();
    QString ListenError;
    if (!ListenForDataConnection(Data->Server.get(), Local, ListenError))
    {
      throw CantOpen(ListenError);
    }
    QString Cmd = FormatPortCommand(Local, Data->Server->serverPort());
    Reply R = Command(Cmd);
    CheckReply(R, Error);
  }

  if ((Rest != nullptr) && (*Rest > 0))
  {
    Reply R = Command(QStringLiteral("REST %1").arg(*Rest));
    if (R.Class() != 3)
    {
      // IDS_ERRORMSG_CANTRESUME
      LogLine(QStringLiteral("Resume command not supported by server, overwriting file."));
      *Rest = 0;
    }
  }

  bool NeedTls = FTls && FProtP;
  auto StartDataTls = [&]()
  {
    // Resume the control connection's session
    QSslConfiguration Config = Data->Socket->sslConfiguration();
    QSslConfiguration ControlConfig = FControl->sslConfiguration();
    if (FData.SslSessionReuse)
    {
      Config.setSessionTicket(ControlConfig.sessionTicket());
    }
    Data->Socket->setSslConfiguration(Config);
    Data->Socket->setPeerVerifyName(FData.HostName);
    Data->Socket->startClientEncryption();
  };
  if (Passive && NeedTls)
  {
    StartDataTls();
  }

  SendCommand(ACommand);

  bool GotPrelim = false;
  bool ReplyConsumed = false;
  try
  {
  bool Ready = Passive && !NeedTls;
  QElapsedTimer Idle;
  Idle.start();
  while (!GotPrelim || !Ready)
  {
    if (Progress != nullptr)
    {
      Progress->CheckCancel();
    }
    Reply R;
    if (!GotPrelim && PollReply(R))
    {
      Idle.restart();
      if (R.Class() == 1)
      {
        GotPrelim = true;
      }
      else if (R.Class() == 2)
      {
        // Transfer finished already (empty listing without data connection)
        GotPrelim = true;
        Data->FinalReply = R;
        Data->HaveFinalReply = true;
        if (!Passive && !Data->Socket)
        {
          Ready = true;
        }
      }
      else
      {
        Data->Socket.reset();
        Data->Server.reset();
        ReplyConsumed = true;
        ReplyError(R, Error);
      }
      continue;
    }
    if (!FControl || (FControl->state() != QAbstractSocket::ConnectedState))
    {
      ConnectionLost(FControl ? FControl->errorString() : QString());
    }
    if (!Ready)
    {
      if (!Data->Socket)
      {
        if (Data->Server->waitForNewConnection(10))
        {
          Data->Socket.reset(Data->Server->TakeConnection());
          if (Data->Socket)
          {
            Idle.restart();
            ConfigureSocket(Data->Socket.get(), false);
            ApplySocketOptions(Data->Socket.get(), true);
            Data->WatchSocket();
            if (NeedTls)
            {
              StartDataTls();
            }
            else
            {
              Ready = true;
            }
          }
        }
      }
      else
      {
        if (Data->Socket->isEncrypted() || Data->Socket->waitForEncrypted(10))
        {
          Ready = true;
          Idle.restart();
        }
        else if ((Data->Socket->state() == QAbstractSocket::UnconnectedState) ||
                 ((Data->Socket->error() != QAbstractSocket::SocketTimeoutError) &&
                  (Data->Socket->error() != QAbstractSocket::UnknownSocketError)))
        {
          throw CantOpen(Data->Socket->errorString());
        }
      }
      if (!GotPrelim)
      {
        if (FControl->waitForReadyRead(1))
        {
          Idle.restart();
        }
      }
    }
    else if (FControl->waitForReadyRead(100))
    {
      Idle.restart();
    }
    if (Idle.elapsed() > TimeoutMs())
    {
      if (!Data->Socket && !Passive && GotPrelim)
      {
        throw CantOpen(Tr(IDS_ERRORMSG_TIMEOUT));
      }
      ConnectionLost(Tr(IDS_ERRORMSG_TIMEOUT));
    }
  }
  if (NeedTls && Data->Socket && (Data->Socket->peerCertificate() != FCertificate))
  {
    // the data connection must come from the same server
    throw CantOpen(Tr(IDS_ERRORMSG_CERTREJECTED));
  }
  }
  catch (const FatalException &)
  {
    throw;
  }
  catch (...)
  {
    if (!ReplyConsumed)
    {
      // the transfer command is pending, abort it
      AbortDataConnection(Data);
    }
    throw;
  }
  Data->Server.reset();
  return Data;
}

qint64 FtpFileSystem::ReadData(DataConnection & Data, char * Buffer, qint64 MaxSize, FileOperationProgress * Progress, const QString & Error)
{
  // CTransferSocket::OnReceive/OnClose. Returns 0 at the end of data.
  QSslSocket * Socket = Data.Socket.get();
  if (Socket == nullptr)
  {
    return 0;
  }
  QElapsedTimer Idle;
  Idle.start();
  for (;;)
  {
    if (Progress != nullptr)
    {
      Progress->CheckCancel();
    }
    if (Socket->bytesAvailable() > 0)
    {
      qint64 Read = Socket->read(Buffer, MaxSize);
      if (Read < 0)
      {
        throw ExtException(Error, Socket->errorString());
      }
      return Read;
    }
    if (Data.PeerClosed)
    {
      return 0;
    }
    if (Socket->state() != QAbstractSocket::ConnectedState)
    {
      QAbstractSocket::SocketError SocketError = Socket->error();
      if ((SocketError != QAbstractSocket::RemoteHostClosedError) &&
          (SocketError != QAbstractSocket::SocketTimeoutError) &&
          (SocketError != QAbstractSocket::UnknownSocketError))
      {
        throw ExtException(Error, Socket->errorString());
      }
      return 0;
    }
    if (Socket->waitForReadyRead(100))
    {
      Idle.restart();
      continue;
    }
    if ((Socket->bytesAvailable() == 0) && (Socket->state() == QAbstractSocket::ConnectedState))
    {
      // Some servers (vsftpd over TLS) send close_notify and the final reply,
      // but do not close the connection until the client answers the shutdown:
      // once the final reply is in and no data arrive, the transfer is complete
      if (!Data.HaveFinalReply)
      {
        Reply R;
        if (FControl && (FControl->canReadLine() || FControl->waitForReadyRead(1)) && PollReply(R))
        {
          if (R.Class() != 1)
          {
            Data.FinalReply = R;
            Data.HaveFinalReply = true;
          }
          Idle.restart();
        }
      }
      else if (Idle.elapsed() >= 1000)
      {
        return 0;
      }
    }
    if (Idle.elapsed() > TimeoutMs())
    {
      throw ExtException(Error, Tr(IDS_ERRORMSG_TIMEOUT));
    }
  }
}

void FtpFileSystem::FinishDataConnection(DataConnection & Data, FileOperationProgress * Progress, const QString & Error)
{
  if (Data.Socket && (Data.Socket->state() == QAbstractSocket::ConnectedState))
  {
    // Answer the server's TLS shutdown (or close a plain connection)
    Data.Socket->disconnectFromHost();
    if (Data.Socket->state() != QAbstractSocket::UnconnectedState)
    {
      Data.Socket->waitForDisconnected(std::min(TimeoutMs(), 5000));
    }
  }
  Data.Socket.reset();
  Data.Server.reset();
  Reply R;
  if (Data.HaveFinalReply)
  {
    R = Data.FinalReply;
  }
  else
  {
    R = ReadReply(Progress);
    // Some non-rfc959 compatible servers send more than one code 1yz reply
    while (R.Class() == 1)
    {
      R = ReadReply(Progress);
    }
  }
  CheckReply(R, Error);
  FAnyTransferSucceeded = true;
}

void FtpFileSystem::AbortDataConnection(std::unique_ptr<DataConnection> & Data)
{
  bool HadFinal = Data && Data->HaveFinalReply;
  if (Data)
  {
    if (Data->Socket)
    {
      Data->Socket->abort();
    }
    Data.reset();
  }
  if (!FControl || (FControl->state() != QAbstractSocket::ConnectedState))
  {
    return;
  }
  // Abort the transfer and resynchronize the control connection:
  // replies to the transfer command and to ABOR are discarded until
  // the reply to NOOP arrives
  try
  {
    if (!HadFinal)
    {
      SendCommand(QStringLiteral("ABOR"), false);
    }
    SendCommand(QStringLiteral("NOOP"), false);
    for (int Count = 0; Count < 10; Count++)
    {
      Reply R = ReadReply();
      if (R.Code == 200)
      {
        return;
      }
      if (R.Code == 421)
      {
        break;
      }
    }
    DoClose();
  }
  catch (...)
  {
    DoClose();
  }
}

//---------------------------------------------------------------------------
// Directory listing

RemoteFile FtpFileSystem::FileFromEntry(const FtpListEntry & Entry, const QString & Directory)
{
  RemoteFile File = FtpListEntryToRemoteFile(Entry);
  ApplyTimeDifference(UnixPath::Combine(Directory, Entry.Name), Entry, File);
  return File;
}

void FtpFileSystem::ApplyTimeDifference(const QString & FullPath, const FtpListEntry & Entry, RemoteFile & File)
{
  // TFTPFileSystem::ApplyTimeDifference (+ LookupUploadModificationTime)
  if (!File.Modification.isValid() || Entry.Date.Utc)
  {
    return;
  }
  FtpTimePrecision Precision = FtpListDatePrecision(Entry.Date);
  if ((Precision != ftpTimeDay) && (FTimeDifference != 0))
  {
    File.Modification = File.Modification.addSecs(FTimeDifference);
  }
  if (Precision == ftpTimeSecond)
  {
    return;
  }
  auto It = FUploadedTimes.find(FullPath);
  if (It != FUploadedTimes.end())
  {
    QDateTime Reduced = It.value().toLocalTime();
    if (Precision == ftpTimeDay)
    {
      Reduced = QDateTime(Reduced.date(), QTime(0, 0));
    }
    else
    {
      Reduced = QDateTime(Reduced.date(), QTime(Reduced.time().hour(), Reduced.time().minute()));
    }
    if (Reduced == File.Modification)
    {
      File.Modification = It.value();
    }
    else
    {
      FUploadedTimes.erase(It);
    }
  }
}

QDateTime FtpFileSystem::ParseMdtm(const Reply & R) const
{
  // CFtpControlSocket::HandleMdtm
  if ((R.Code != 213) || R.Lines.isEmpty())
  {
    return QDateTime();
  }
  QString Value = R.Lines[0].trimmed();
  if (Value.size() < 14)
  {
    return QDateTime();
  }
  QDate Date(Value.mid(0, 4).toInt(), Value.mid(4, 2).toInt(), Value.mid(6, 2).toInt());
  QTime Time(Value.mid(8, 2).toInt(), Value.mid(10, 2).toInt(), Value.mid(12, 2).toInt());
  if (!Date.isValid() || !Time.isValid() || (Date.year() < 1971))
  {
    return QDateTime();
  }
  return QDateTime(Date, Time, QTimeZone::UTC);
}

void FtpFileSystem::AutoDetectTimeDifference(const QString & Directory, const QList<FtpListEntry> & Entries)
{
  // TFTPFileSystem::AutoDetectTimeDifference
  if (!FDetectTimeDifference || UsingMlsd() ||
      !SupportsCommand(QStringLiteral("MDTM")) || !SupportsCommand(QStringLiteral("SIZE")))
  {
    return;
  }
  for (const FtpListEntry & Entry : Entries)
  {
    if (Entry.Dir || Entry.Link || !Entry.Date.HasDate || !Entry.Date.HasTime || Entry.Date.Utc)
    {
      continue;
    }
    Reply R = Command(QStringLiteral("MDTM ") + UnixPath::Combine(Directory, Entry.Name));
    if (R.Class() != 2)
    {
      FDetectTimeDifference = false;
      LogLine(QStringLiteral("Failed to retrieve file attributes to detect timezone difference"));
      return;
    }
    QDateTime Utc = ParseMdtm(R);
    if (!Utc.isValid() || (Utc > QDateTime::currentDateTimeUtc()))
    {
      continue;
    }
    FDetectTimeDifference = false;
    QDateTime Listing = FtpListDateToDateTime(Entry.Date);
    // MDTM returns seconds, trim those
    QDateTime Local = Utc.toLocalTime();
    if (!Entry.Date.HasSeconds)
    {
      Local = QDateTime(Local.date(), QTime(Local.time().hour(), Local.time().minute()));
    }
    FTimeDifference = Listing.secsTo(Local);
    if (std::abs(FTimeDifference) >= 48 * 3600)
    {
      // Seen with "GamingDeluxe FTP Server", which returns "213 00010101000000"
      FTimeDifference = 0;
    }
    LogLine(QStringLiteral("Timezone difference of %1 s detected").arg(FTimeDifference));
    return;
  }
}

void FtpFileSystem::ResolveSymlinks(const QString & Directory, RemoteFileList & Files)
{
  // FileZilla treats all symlinks as directories; resolve them with SIZE
  // (succeeds for files only) where the server supports it
  if (!FData.ResolveSymlinks || !SupportsCommand(QStringLiteral("SIZE")))
  {
    return;
  }
  bool TypeSet = false;
  for (RemoteFile & File : Files)
  {
    if (!File.IsSymLink)
    {
      continue;
    }
    if (!TypeSet)
    {
      // SIZE returns the size as transferred over the stream
      // (ProFTPD does not even support SIZE in ASCII mode)
      EnsureType(QLatin1Char('I'));
      TypeSet = true;
    }
    Reply R = Command(QStringLiteral("SIZE ") + UnixPath::Combine(Directory, File.FileName));
    if (R.Code == 213)
    {
      File.IsDirectory = false;
      File.Size = R.Lines.value(0).trimmed().toLongLong();
    }
  }
}

RemoteFileList FtpFileSystem::DoReadDirectory(const QString & Directory, bool ListAll)
{
  // TFTPFileSystem::DoReadDirectory + CFtpControlSocket::List
  QString Error = Tr(LIST_DIR_ERROR).arg(Directory);
  FLastDataSent.start();
  ChangeWorkingDirectory(Directory);
  QString ListCommand;
  if (UsingMlsd())
  {
    ListCommand = QStringLiteral("MLSD");
  }
  else
  {
    ListCommand = ListAll ? QStringLiteral("LIST -a") : QStringLiteral("LIST");
  }
  EnsureType(QLatin1Char('A'));
  FtpListParser Parser(false, FUtf8, FServerType, FData.VMSAllRevisions);
  std::unique_ptr<DataConnection> Data;
  bool Empty = false;
  try
  {
    Data = OpenDataConnection(ListCommand, nullptr, nullptr, Error);
    QByteArray Buffer(static_cast<qsizetype>(TransferChunk), Qt::Uninitialized);
    qint64 Read;
    while ((Read = ReadData(*Data, Buffer.data(), Buffer.size(), nullptr, Error)) > 0)
    {
      Parser.AddData(Buffer.constData(), Read);
    }
    FinishDataConnection(*Data, nullptr, Error);
  }
  catch (const ExtException & E)
  {
    if (dynamic_cast<const FatalException *>(&E) != nullptr)
    {
      throw;
    }
    // CFtpControlSocket::IsMisleadingListResponse
    static const QStringList Misleading{
      QStringLiteral("No members found."), QStringLiteral("No data sets found."), QStringLiteral("No files found.")};
    if (Misleading.contains(E.MoreMessages().trimmed(), Qt::CaseInsensitive))
    {
      Empty = true;
    }
    else
    {
      if (Data && Data->Socket)
      {
        AbortDataConnection(Data);
      }
      throw;
    }
  }
  QList<FtpListEntry> Entries = Empty ? QList<FtpListEntry>() : Parser.GetList();
  if (!Parser.Utf8() && (FData.NotUtf == asAuto))
  {
    FUtf8 = false;
  }
  FServerType |= (Parser.ServerType() & FtpListParser::stVMS);
  for (const QString & Line : Parser.UnparsedLines())
  {
    LogLine(QStringLiteral("Cannot parse line: %1").arg(Line));
  }
  AutoDetectTimeDifference(Directory, Entries);
  RemoteFileList Files;
  for (const FtpListEntry & Entry : std::as_const(Entries))
  {
    if (Entry.Name.isEmpty() || (Entry.Name == QStringLiteral(".")) || (Entry.Name == QStringLiteral("..")))
    {
      continue;
    }
    if (!IsValidListingName(Entry.Name))
    {
      LogLine(QStringLiteral("Discarding entry \"%1\" with invalid name.").arg(Entry.Name));
      continue;
    }
    Files.append(FileFromEntry(Entry, Directory));
  }
  ResolveSymlinks(Directory, Files);
  return Files;
}

RemoteFileList FtpFileSystem::ListDirectory(const QString & ADirectory)
{
  // TFTPFileSystem::ReadDirectory: "LIST -a" auto detection
  QString Directory = AbsolutePath(ADirectory);
  RemoteFileList Files;
  if (UsingMlsd())
  {
    Files = DoReadDirectory(Directory, false);
  }
  else
  {
    bool GotNoFilesForAll = false;
    bool Repeat;
    do
    {
      Repeat = false;
      try
      {
        Files = DoReadDirectory(Directory, (FListAll == asAuto) || (FListAll == asOn));
        if (GotNoFilesForAll && Files.isEmpty())
        {
          // It was not "-a"'s problem, revert to auto and let it decide the next time
          FListAll = asAuto;
        }
        else if (FListAll == asAuto)
        {
          // some servers take "-a" as a mask and return empty directory listing
          if (Files.isEmpty())
          {
            Repeat = true;
            FListAll = asOff;
            GotNoFilesForAll = true;
            LogLine(QStringLiteral("LIST with -a switch returned empty directory listing, will try pure LIST"));
          }
          else
          {
            FListAll = asOn;
          }
        }
      }
      catch (const FatalException &)
      {
        throw;
      }
      catch (const AbortException &)
      {
        throw;
      }
      catch (const ExtException &)
      {
        // reading the first directory has failed,
        // further try without "-a" only as the server may not support it
        if ((FListAll == asAuto) && (FCurrentDirectory == Directory))
        {
          LogLine(QStringLiteral("LIST with -a failed, will try pure LIST"));
          FListAll = asOff;
          Repeat = true;
        }
        else
        {
          throw;
        }
      }
    }
    while (Repeat);
  }
  FFileListCacheDir = Directory;
  FFileListCache = Files;
  FFileListCacheDirty.clear();
  FFileListCacheTime.start();
  return Files;
}

RemoteFileList FtpFileSystem::ReadDirectory(const QString & Directory)
{
  OperationGuard Guard(this);
  CheckConnection();
  return ListDirectory(Directory);
}

bool FtpFileSystem::ReadFileInternal(const QString & Path, RemoteFile & File, QString & Error)
{
  QString Abs = AbsolutePath(Path);
  if (Abs == QStringLiteral("/"))
  {
    File = RemoteFile();
    File.FileName = QStringLiteral("/");
    File.FileType = RemoteFile::Directory;
    File.IsDirectory = true;
    return true;
  }
  QString Directory = UnixPath::ExtractFileDir(Abs);
  QString Name = UnixPath::ExtractFileName(Abs);
  if (UsingMlsd())
  {
    // CFtpControlSocket::ListFile (LISTFILE_MLST)
    Reply R = Command(QStringLiteral("MLST ") + Abs);
    if (R.Class() != 2)
    {
      Error = R.Lines.join(QLatin1Char('\n'));
      if (R.Code == 421)
      {
        ReplyError(R, QString());
      }
      return false;
    }
    FtpListParser Parser(true, FUtf8, FServerType, FData.VMSAllRevisions);
    for (qsizetype Index = 1; Index < R.Lines.size() - 1; Index++)
    {
      Parser.AddData(Encode(R.Lines[Index]) + "\n");
    }
    QList<FtpListEntry> Entries = Parser.GetList();
    if (Entries.isEmpty())
    {
      throw ExtException(Tr(FTP_RESPONSE_ERROR).arg(QStringLiteral("MLST"), R.Lines.join(QLatin1Char('\n'))));
    }
    FtpListEntry Entry = Entries[0];
    Entry.Name = Name;
    File = FileFromEntry(Entry, Directory);
    if (File.IsSymLink)
    {
      RemoteFileList List{File};
      ResolveSymlinks(Directory, List);
      File = List[0];
    }
    return true;
  }

  // FZAPI does not have efficient way to read properties of one file.
  // In case we need properties of set of files from the same directory,
  // cache the file list for future (TFTPFileSystem::ReadFile)
  bool CacheValid =
    (FFileListCacheDir == Directory) && FFileListCacheTime.isValid() &&
    (FFileListCacheTime.elapsed() < FileListCacheMs) && !FFileListCacheDirty.contains(Name);
  if (!CacheValid && SupportsReadingFile())
  {
    // TFTPFileSystem::DoReadFile (without MLST: CWD, SIZE and MDTM)
    return ListFile(Abs, File, Error);
  }
  if (!CacheValid)
  {
    try
    {
      ListDirectory(Directory);
    }
    catch (const FatalException &)
    {
      throw;
    }
    catch (const AbortException &)
    {
      throw;
    }
    catch (const ExtException & E)
    {
      if (FCurrentDirectory == Directory)
      {
        // the directory exists, but cannot be listed
        throw;
      }
      // parent directory does not exist
      Error = E.MoreMessages();
      return false;
    }
  }
  for (const RemoteFile & Entry : std::as_const(FFileListCache))
  {
    if (Entry.FileName == Name)
    {
      File = Entry;
      return true;
    }
  }
  return false;
}

bool FtpFileSystem::SupportsReadingFile() const
{
  // TFTPFileSystem::SupportsReadingFile
  return UsingMlsd() || (SupportsCommand(QStringLiteral("MDTM")) && SupportsCommand(QStringLiteral("SIZE")));
}

bool FtpFileSystem::ListFile(const QString & Abs, RemoteFile & File, QString & Error)
{
  // CFtpControlSocket::ListFile without MLST: a successful CWD tells a
  // directory (the working directory is tracked, so it need not be restored);
  // otherwise SIZE and MDTM of the file
  FLastDataSent.start();
  QString Directory = UnixPath::ExtractFileDir(Abs);
  FtpListEntry Entry;
  Entry.Name = UnixPath::ExtractFileName(Abs);
  Reply R = Command(QStringLiteral("CWD ") + Abs);
  if (R.Code == 421)
  {
    ReplyError(R, QString());
  }
  if (R.Class() == 2)
  {
    FCurrentDirectory = Abs;
    Entry.Dir = true;
    File = FileFromEntry(Entry, Directory);
    return true;
  }
  // (a failed CWD keeps the working directory, RFC 959)
  // SIZE returns the size as transferred over the stream (RFC 3659), and
  // ProFTPD does not even support SIZE in ASCII mode
  EnsureType(QLatin1Char('I'));
  R = Command(QStringLiteral("SIZE ") + Abs);
  if ((R.Code != 213) || R.Lines.isEmpty())
  {
    if (R.Code == 421)
    {
      ReplyError(R, QString());
    }
    Error = R.Lines.join(QLatin1Char('\n'));
    return false;
  }
  Entry.Size = R.Lines.value(0).trimmed().toLongLong();
  R = Command(QStringLiteral("MDTM ") + Abs);
  QDateTime Utc = ParseMdtm(R);
  if (!Utc.isValid())
  {
    if (R.Code == 421)
    {
      ReplyError(R, QString());
    }
    Error = R.Lines.join(QLatin1Char('\n'));
    return false;
  }
  Entry.Date.Year = Utc.date().year();
  Entry.Date.Month = Utc.date().month();
  Entry.Date.Day = Utc.date().day();
  Entry.Date.Hour = Utc.time().hour();
  Entry.Date.Minute = Utc.time().minute();
  Entry.Date.Second = Utc.time().second();
  Entry.Date.HasDate = true;
  Entry.Date.HasTime = true;
  Entry.Date.HasYear = true;
  Entry.Date.HasSeconds = true;
  Entry.Date.Utc = true;
  File = FileFromEntry(Entry, Directory);
  return true;
}

RemoteFile FtpFileSystem::ReadFile(const QString & Path)
{
  OperationGuard Guard(this);
  CheckConnection();
  RemoteFile File;
  QString Error;
  if (!ReadFileInternal(Path, File, Error))
  {
    throw ExtException(Tr(FILE_NOT_EXISTS).arg(Path), Error);
  }
  return File;
}

bool FtpFileSystem::FileExists(const QString & Path, RemoteFile * File)
{
  OperationGuard Guard(this);
  CheckConnection();
  RemoteFile Found;
  QString Error;
  bool Result = ReadFileInternal(Path, Found, Error);
  if (Result && (File != nullptr))
  {
    *File = Found;
  }
  return Result;
}

//---------------------------------------------------------------------------
// File operations

void FtpFileSystem::CreateDirectory(const QString & Path, int /*Mode*/)
{
  OperationGuard Guard(this);
  CheckConnection();
  QString Abs = AbsolutePath(Path);
  Reply R = Command(QStringLiteral("MKD ") + Abs);
  InvalidateCache(Abs);
  CheckReply(R, Tr(CREATE_DIR_ERROR).arg(Path));
}

void FtpFileSystem::DeleteFile(const QString & Path)
{
  OperationGuard Guard(this);
  CheckConnection();
  QString Abs = AbsolutePath(Path);
  Reply R = Command(QStringLiteral("DELE ") + FileCommandPath(Abs));
  InvalidateCache(Abs);
  CheckReply(R, Tr(DELETE_FILE_ERROR).arg(Path));
  FUploadedTimes.remove(Abs);
}

void FtpFileSystem::RemoveDirectory(const QString & Path)
{
  OperationGuard Guard(this);
  CheckConnection();
  QString Abs = AbsolutePath(Path);
  // If current remote directory is in the directory being removed,
  // some servers may refuse to delete it
  if (!FCurrentDirectory.isEmpty() &&
      ((FCurrentDirectory == Abs) || FCurrentDirectory.startsWith(Abs + QLatin1Char('/'))))
  {
    QString Parent = UnixPath::ExtractFileDir(Abs);
    Reply Cwd = Command(QStringLiteral("CWD ") + Parent);
    FCurrentDirectory = (Cwd.Class() == 2) ? Parent : QString();
  }
  Reply R = Command(QStringLiteral("RMD ") + Abs);
  InvalidateCache(Abs);
  CheckReply(R, Tr(DELETE_FILE_ERROR).arg(Path));
}

void FtpFileSystem::RenameFile(const QString & From, const QString & To)
{
  OperationGuard Guard(this);
  CheckConnection();
  QString AbsFrom = AbsolutePath(From);
  QString AbsTo = AbsolutePath(To);
  QString Error = Tr(RENAME_FILE_ERROR).arg(UnixPath::ExtractFileName(From), UnixPath::ExtractFileName(To));
  InvalidateCache(AbsFrom);
  InvalidateCache(AbsTo);
  if (FIIS && (AbsFrom != AbsTo))
  {
    // !fcMoveOverExistingFile (TTerminal::MoveFile): IIS refuses to rename
    // over an existing file, delete it first
    RemoteFile Existing;
    QString ExistingError;
    if (ReadFileInternal(AbsTo, Existing, ExistingError) && !Existing.IsDirectory)
    {
      CheckReply(Command(QStringLiteral("DELE ") + AbsTo), Error);
      FUploadedTimes.remove(AbsTo);
      InvalidateCache(AbsTo);
    }
  }
  Reply R = Command(QStringLiteral("RNFR ") + AbsFrom);
  if (R.Class() != 3)
  {
    ReplyError(R, Error);
  }
  R = Command(QStringLiteral("RNTO ") + AbsTo);
  CheckReply(R, Error);
  if (FCurrentDirectory.startsWith(AbsFrom))
  {
    FCurrentDirectory.clear();
  }
  auto It = FUploadedTimes.find(AbsFrom);
  if (It != FUploadedTimes.end())
  {
    QDateTime Time = It.value();
    FUploadedTimes.erase(It);
    FUploadedTimes.insert(AbsTo, Time);
  }
}

void FtpFileSystem::ChangeMode(const QString & Path, unsigned int Mode)
{
  OperationGuard Guard(this);
  CheckConnection();
  QString Abs = AbsolutePath(Path);
  // CFtpControlSocket::Chmod: "SITE CHMOD %03d" of octal number represented as decadic
  QString Octal = QString::number(Mode & 07777, 8).rightJustified(3, QLatin1Char('0'));
  Reply R = Command(QStringLiteral("SITE CHMOD %1 %2").arg(Octal, Abs));
  InvalidateCache(Abs);
  CheckReply(R, Tr(CANT_SET_ATTRS).arg(Path));
}

void FtpFileSystem::SetModificationTime(const QString & Path, const QDateTime & Time)
{
  OperationGuard Guard(this);
  CheckConnection();
  QString Abs = AbsolutePath(Path);
  QString Error = Tr(PRESERVE_TIME_PERM_ERROR3).arg(UnixPath::ExtractFileName(Path));
  QString Stamp = Time.toUTC().toString(QStringLiteral("yyyyMMddHHmmss"));
  QString Cmd;
  // CFtpControlSocket::TransferFinished (MFMT, or MFMT-like call to MDTM),
  // ProFTPD's SITE UTIME
  if (SupportsCommand(QStringLiteral("MFMT")))
  {
    Cmd = QStringLiteral("MFMT %1 %2").arg(Stamp, Abs);
  }
  else if (SupportsCommand(QStringLiteral("MDTM")))
  {
    Cmd = QStringLiteral("MDTM %1 %2").arg(Stamp, Abs);
  }
  else if (SupportsSiteCommand(QStringLiteral("UTIME")))
  {
    Cmd = QStringLiteral("SITE UTIME %1 %2").arg(Stamp, Abs);
  }
  else
  {
    throw ExtException(Error, Tr(NOTSUPPORTED));
  }
  Reply R = Command(Cmd);
  InvalidateCache(Abs);
  CheckReply(R, Error);
  // Listing shows minutes only, remember the full time (FUploadedTimes)
  FUploadedTimes.insert(Abs, Time);
}

void FtpFileSystem::CreateLink(const QString & Path, const QString & Target)
{
  OperationGuard Guard(this);
  CheckConnection();
  QString Abs = AbsolutePath(Path);
  QString Error = Tr(CREATE_LINK_ERROR).arg(Path);
  if (!SupportsSiteCommand(QStringLiteral("SYMLINK")))
  {
    throw ExtException(Error, Tr(NOTSUPPORTED));
  }
  Reply R = Command(QStringLiteral("SITE SYMLINK %1 %2").arg(Target, Abs));
  InvalidateCache(Abs);
  CheckReply(R, Error);
}

//---------------------------------------------------------------------------
// Transfers

void FtpFileSystem::Download(const QString & RemotePath, const QString & LocalPath, FileOperationProgress & Progress)
{
  // Plain download; partial file cleanup is up to the caller
  TransferParams Params;
  Params.KeepPartial = true;
  Download(RemotePath, LocalPath, Params, Progress);
}

void FtpFileSystem::Download(const QString & RemotePath, const QString & LocalPath, const TransferParams & Params, FileOperationProgress & Progress)
{
  OperationGuard Guard(this);
  CheckConnection();
  QString Abs = AbsolutePath(RemotePath);
  QString Error = Tr(CANT_OPEN_REMOTE).arg(RemotePath);
  // Text mode: TYPE A, the CR/LF of the wire is converted to the local LF
  // (FileZilla on Windows keeps CR/LF, which is the local convention there)
  bool Ascii = FAsciiTransfer || FtpAsciiTransfer(Params, 0);
  qint64 Offset = Ascii ? 0 : std::max<qint64>(Params.ResumeOffset, 0);
  if (Offset != std::max<qint64>(Params.ResumeOffset, 0))
  {
    Progress.AddTransferred(-Params.ResumeOffset);
  }
  QString RemoteName = FileCommandPath(Abs);
  EnsureType(Ascii ? QLatin1Char('A') : QLatin1Char('I'));
  FLastDataSent.start();

  std::unique_ptr<DataConnection> Data;
  QFile Local(LocalPath);
  bool LocalOpened = false;
  bool PendingCR = false;
  try
  {
    // the destination is opened only once the source is known to exist
    // (as TFTPFileSystem::Sink does not lose the destination file)
    qint64 Rest = Offset;
    Data = OpenDataConnection(QStringLiteral("RETR ") + RemoteName, &Rest, &Progress, Error);
    if (Rest != Offset)
    {
      // Resume command not supported by server, overwriting file
      Progress.AddTransferred(-Offset);
      Offset = 0;
    }
    QIODevice::OpenMode OpenMode = (Offset > 0) ? (QIODevice::WriteOnly | QIODevice::Append) : (QIODevice::WriteOnly | QIODevice::Truncate);
    if (!Local.open(OpenMode))
    {
      throw ExtException(QStringLiteral("Can't create file '%1'.").arg(LocalPath), Local.errorString());
    }
    LocalOpened = true;
    if ((Offset > 0) && (!Local.resize(Offset) || !Local.seek(Offset)))
    {
      throw ExtException(QStringLiteral("Can't write to file '%1'.").arg(LocalPath), Local.errorString());
    }

    QByteArray Buffer(static_cast<qsizetype>(TransferChunk), Qt::Uninitialized);
    for (;;)
    {
      Progress.CheckCancel();
      qint64 Want = static_cast<qint64>(Progress.AdjustToCPSLimit(static_cast<unsigned long>(TransferChunk)));
      qint64 Read = ReadData(*Data, Buffer.data(), Want, &Progress, Error);
      if (Read == 0)
      {
        break;
      }
      const char * Out = Buffer.constData();
      qint64 OutSize = Read;
      QByteArray Converted;
      if (Ascii)
      {
        Converted.reserve(Read + 1);
        for (qint64 Index = 0; Index < Read; Index++)
        {
          char C = Buffer.at(Index);
          if (PendingCR)
          {
            PendingCR = false;
            if (C != '\n')
            {
              Converted.append('\r');
            }
          }
          if (C == '\r')
          {
            PendingCR = true;
          }
          else
          {
            Converted.append(C);
          }
        }
        Out = Converted.constData();
        OutSize = Converted.size();
      }
      if (Local.write(Out, OutSize) != OutSize)
      {
        throw ExtException(QStringLiteral("Can't write to file '%1'.").arg(LocalPath), Local.errorString());
      }
      Progress.AddTransferred(Read);
    }
    if (PendingCR && (Local.write("\r", 1) != 1))
    {
      throw ExtException(QStringLiteral("Can't write to file '%1'.").arg(LocalPath), Local.errorString());
    }
    FinishDataConnection(*Data, &Progress, Error);
    if (!Local.flush())
    {
      throw ExtException(QStringLiteral("Can't write to file '%1'.").arg(LocalPath), Local.errorString());
    }
    Local.close();
  }
  catch (const FatalException &)
  {
    Data.reset();
    if (LocalOpened)
    {
      Local.close();
      if (!Params.KeepPartial || (Local.size() == 0))
      {
        Local.remove();
      }
    }
    throw;
  }
  catch (...)
  {
    if (Data)
    {
      AbortDataConnection(Data);
    }
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

void FtpFileSystem::Upload(const QString & LocalPath, const QString & RemotePath, int Mode, FileOperationProgress & Progress)
{
  TransferParams Params;
  Upload(LocalPath, RemotePath, Mode, Params, Progress);
}

void FtpFileSystem::Upload(const QString & LocalPath, const QString & RemotePath, int /*Mode*/, const TransferParams & Params, FileOperationProgress & Progress)
{
  // (permissions are set by the caller with ChangeMode, as WinSCP does for FTP)
  OperationGuard Guard(this);
  CheckConnection();
  QFile Local(LocalPath);
  if (!Local.open(QIODevice::ReadOnly))
  {
    throw ExtException(QStringLiteral("Can't open file '%1'.").arg(LocalPath), Local.errorString());
  }
  qint64 RemoteOffset = std::max<qint64>(Params.ResumeOffset, 0);
  qint64 LocalPos = RemoteOffset;
  qint64 LocalEnd = Local.size();
  // Text mode conversions (TFileBuffer::Convert with cpRemoveBOM / cpRemoveCtrlZ)
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

  QString Abs = AbsolutePath(RemotePath);
  QString Error = Tr(CANT_CREATE_REMOTE).arg(RemotePath);
  // Text mode: TYPE A, local LF line ends go as CR/LF to the wire
  bool Ascii = FAsciiTransfer || FtpAsciiTransfer(Params, 0);
  InvalidateCache(Abs);
  FUploadedTimes.remove(Abs);
  QString RemoteName = FileCommandPath(Abs);
  EnsureType(Ascii ? QLatin1Char('A') : QLatin1Char('I'));
  FLastDataSent.start();
  std::unique_ptr<DataConnection> Data;
  bool Started = false;
  char PreviousByte = '\0';
  try
  {
    // Resume by appending (CFtpControlSocket bResumeAppend)
    // (Append: the whole file after the existing contents, omAppend)
    bool Append = (RemoteOffset > 0) || Params.Append;
    Data = OpenDataConnection((Append ? QStringLiteral("APPE ") : QStringLiteral("STOR ")) + RemoteName, nullptr, &Progress, Error);
    Started = true;
    QSslSocket * Socket = Data->Socket.get();
    while ((Socket != nullptr) && (LocalPos < LocalEnd))
    {
      Progress.CheckCancel();
      qint64 Want = std::min<qint64>(TransferChunk, LocalEnd - LocalPos);
      Want = static_cast<qint64>(Progress.AdjustToCPSLimit(static_cast<unsigned long>(Want)));
      Progress.CheckCancel();
      // a new buffer each time: QIODevice::write(QByteArray) may queue the
      // (implicitly shared) array itself instead of copying it
      QByteArray Buffer = Local.read(Want);
      qint64 Read = Buffer.size();
      if ((Read == 0) && !Local.atEnd())
      {
        throw ExtException(QStringLiteral("Can't read file '%1'.").arg(LocalPath), Local.errorString());
      }
      if (Read == 0)
      {
        break;
      }
      LocalPos += Read;
      if (Ascii)
      {
        QByteArray Converted;
        Converted.reserve(Read + Read / 16 + 1);
        for (char C : std::as_const(Buffer))
        {
          if ((C == '\n') && (PreviousByte != '\r'))
          {
            Converted.append('\r');
          }
          Converted.append(C);
          PreviousByte = C;
        }
        Buffer = Converted;
      }
      try
      {
        WriteAll(Socket, Buffer, &Progress);
      }
      catch (const AbortException &)
      {
        throw;
      }
      catch (const ExtException & E)
      {
        // The server closed the data connection, its reply tells why
        Socket->abort();
        Data->Socket.reset();
        Socket = nullptr;
        Reply R = ReadReply(&Progress);
        while (R.Class() == 1)
        {
          R = ReadReply(&Progress);
        }
        Data->FinalReply = R;
        Data->HaveFinalReply = true;
        if (R.Class() != 2)
        {
          ReplyError(R, Error);
        }
        throw ExtException(Error, E.MoreMessages());
      }
      Progress.AddTransferred(Read);
    }
    if (Socket != nullptr)
    {
      // flush and close (TLS close_notify) to signal the end of the file
      WriteAll(Socket, QByteArray(), nullptr);
      CloseUploadConnection(Socket, TimeoutMs(), &Progress);
    }
    FinishDataConnection(*Data, &Progress, Error);
  }
  catch (const FatalException &)
  {
    throw;
  }
  catch (...)
  {
    if (Data)
    {
      AbortDataConnection(Data);
    }
    // As TSFTPFileSystem::SFTPSource: delete file if transfer was not completed,
    // unless the transfer is resumable (temporary ".filepart" file is kept)
    if (Started && !Params.KeepPartial && !Params.Append && IsOpened())
    {
      try
      {
        Command(QStringLiteral("DELE ") + Abs);
      }
      catch (...)
      {
      }
    }
    throw;
  }
}

//---------------------------------------------------------------------------
// Server side operations

void FtpFileSystem::CopyFile(const QString & From, const QString & To)
{
  // TFTPFileSystem::CopyFile: SITE CPFR/CPTO (fcRemoteCopy)
  OperationGuard Guard(this);
  CheckConnection();
  QString AbsFrom = AbsolutePath(From);
  QString AbsTo = AbsolutePath(To);
  QString Error = Tr(COPY_FILE_ERROR).arg(From, To);
  if (!CanRemoteCopy())
  {
    throw ExtException(Error, Tr(NOTSUPPORTED));
  }
  Reply R = Command(QStringLiteral("SITE CPFR ") + AbsFrom);
  if (R.Class() != 3)
  {
    ReplyError(R, Error);
  }
  R = Command(QStringLiteral("SITE CPTO ") + AbsTo);
  InvalidateCache(AbsTo);
  CheckReply(R, Error);
  FUploadedTimes.remove(AbsTo);
}

QStringList FtpFileSystem::ChecksumAlgorithms() const
{
  // TFTPFileSystem::GetSupportedChecksumAlgs
  QStringList Result = FHashAlgs;
  for (const auto & Entry : FtpChecksumCommands)
  {
    QString Alg = QString::fromLatin1(Entry.Alg);
    if (SupportsCommand(QString::fromLatin1(Entry.Command)) && !Result.contains(Alg))
    {
      Result << Alg;
    }
  }
  return Result;
}

QString FtpFileSystem::CalculateFileChecksum(const QString & Path, const QString & Alg)
{
  OperationGuard Guard(this);
  CheckConnection();
  QString Abs = AbsolutePath(Path);
  QString Error = Tr(CHECKSUM_ERROR).arg(Abs);

  // TFTPFileSystem::CalculateFilesChecksumInitialize
  QStringList ChecksumAlgs;
  for (const auto & Entry : FtpChecksumCommands)
  {
    ChecksumAlgs << QString::fromLatin1(Entry.Alg);
  }
  QString NormalizedAlg = FindIdent(FindIdent(Alg, FHashAlgs), ChecksumAlgs);
  bool UsingHashCommand = FHashAlgs.contains(NormalizedAlg);
  QString CommandName;
  if (UsingHashCommand)
  {
    CommandName = QStringLiteral("HASH");
    if (FHashAlg != NormalizedAlg)
    {
      // The server should understand lowercase alg name by spec,
      // but we should use uppercase anyway
      CheckReply(Command(QStringLiteral("OPTS HASH ") + NormalizedAlg.toUpper()), Error);
      FHashAlg = NormalizedAlg;
    }
  }
  else
  {
    for (const auto & Entry : FtpChecksumCommands)
    {
      if (NormalizedAlg == QLatin1String(Entry.Alg))
      {
        // the first command the server announces (WinSCP takes the first registered one)
        if (CommandName.isEmpty() || (!SupportsCommand(CommandName) && SupportsCommand(QString::fromLatin1(Entry.Command))))
        {
          CommandName = QString::fromLatin1(Entry.Command);
        }
      }
    }
    if (CommandName.isEmpty())
    {
      throw ExtException(Tr(UNKNOWN_CHECKSUM).arg(Alg));
    }
  }

  // TFTPFileSystem::DoCalculateFileChecksum. FTP way is not to quote, but
  // as Serv-U, GlobalSCAPE and possibly others allow additional parameters
  // (SP ER range), they need to quote file name
  QString FileName = Abs;
  if (FileName.contains(QLatin1Char(' ')))
  {
    FileName = QStringLiteral("\"%1\"").arg(FileName);
  }
  Reply R = Command(CommandName + QLatin1Char(' ') + FileName);
  CheckReply(R, Error);
  // ProFTPD response has this format:
  // 213-Computing MD5 digest
  // 213 MD5 0-687104 b359479cfda703b7fd473c7e09f39049 filename
  QString ResponseText = R.Lines.isEmpty() ? QString() : R.Lines.last();
  QString Hash;
  if (UsingHashCommand)
  {
    // ("213" SP) hashname SP start-point "-" end-point SP filehash SP <pathname>;
    // older drafts (FileZilla Server, Cerberus) have no range
    QStringList Tokens = ResponseText.trimmed().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    QString Range = Tokens.value(1);
    Hash = Range.contains(QLatin1Char('-')) ? Tokens.value(2) : Range;
  }
  else
  {
    // MD5 response (draft-twine-ftpmd5-00) includes a file name, X<hash>
    // commands return the hash only; WS_FTP uses code 220
    ResponseText = ResponseText.trimmed();
    qsizetype P = ResponseText.lastIndexOf(QLatin1Char(' '));
    Hash = (P >= 0) ? ResponseText.mid(P + 1) : ResponseText;
  }
  if (Hash.isEmpty())
  {
    throw ExtException(Error, Tr(FTP_RESPONSE_ERROR).arg(CommandName, ResponseText));
  }
  return Hash.toLower();
}

void FtpFileSystem::CheckCertificate(const QString & Path)
{
  // CheckCertificate (Common.cpp)
  QSslCertificate Certificate;
  QSslKey PrivateKey;
  bool WrongPassphrase = false;
  QString FileName = QDir::fromNativeSeparators(Path);
  if (FileName.startsWith(QStringLiteral("~/")))
  {
    FileName = QDir::homePath() + FileName.mid(1);
  }
  ParseCertificate(FileName, QString(), Certificate, PrivateKey, WrongPassphrase);
}

SpaceAvailableInfo FtpFileSystem::SpaceAvailable(const QString & Path)
{
  // TFTPFileSystem::SpaceAvailable
  OperationGuard Guard(this);
  CheckConnection();
  QString Abs = AbsolutePath(Path);
  // SPACE_AVAILABLE_ERROR
  QString Error = QStringLiteral("Error checking space available for path '%1'.").arg(Abs);
  SpaceAvailableInfo Result;
  if (SupportsCommand(QStringLiteral("XQUOTA")))
  {
    // WS_FTP: "213-File and disk usage" / "Disk usage: N" / "Disk limit: N"; global, not path-specific
    Reply R = Command(QStringLiteral("XQUOTA"));
    CheckReply(R, Error);
    qint64 UsedBytes = -1;
    for (const QString & ALine : R.Lines)
    {
      QString Line = ALine.trimmed();
      qsizetype Colon = Line.indexOf(QLatin1Char(':'));
      if (Colon < 0)
      {
        continue;
      }
      QString Label = Line.left(Colon).trimmed();
      QString Value = Line.mid(Colon + 1).trimmed();
      if (Label.compare(QLatin1String("Disk usage"), Qt::CaseInsensitive) == 0)
      {
        UsedBytes = Value.toLongLong();
      }
      else if ((Label.compare(QLatin1String("Disk limit"), Qt::CaseInsensitive) == 0) &&
               (Value.compare(QLatin1String("unlimited"), Qt::CaseInsensitive) != 0))
      {
        Result.BytesAvailableToUser = Value.toLongLong();
      }
    }
    if ((UsedBytes >= 0) && (Result.BytesAvailableToUser > 0))
    {
      Result.UnusedBytesAvailableToUser = Result.BytesAvailableToUser - UsedBytes;
    }
  }
  else if (SupportsCommand(QStringLiteral("AVBL")))
  {
    // draft-peterson-streamlined-ftp-command-extensions-10 (Serv-U)
    Reply R = Command(QStringLiteral("AVBL ") + Abs);
    CheckReply(R, Error);
    QString Response = R.Lines.isEmpty() ? QString() : R.Lines.last().trimmed();
    bool Ok = false;
    Result.UnusedBytesAvailableToUser = Response.toLongLong(&Ok);
    if (!Ok)
    {
      throw ExtException(Error, Response);
    }
  }
  else
  {
    throw ExtException(QStringLiteral("The server does not support the operation."));
  }
  return Result;
}
