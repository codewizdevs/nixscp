// Port of WinSCP source/core/S3FileSystem.cpp (TS3FileSystem) together with
// the request composition of libs3 (libs/libs3/src/request.c, bucket.c,
// object.c, service.c, multipart.c) as a blocking HTTP/1.1 client on top of
// QSslSocket (see S3FileSystem.h).
#include "core/S3FileSystem.h"
#include "core/Configuration.h"
#include "core/FileMasks.h"
#include "core/SessionLog.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QLocale>
#include <QMimeDatabase>
#include <QMutex>
#include <QNetworkProxy>
#include <QSslCertificate>
#include <QSslCipher>
#include <QSslConfiguration>
#include <QSslError>
#include <QSslSocket>
#include <QUrl>
#include <QXmlStreamReader>

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <algorithm>
#include <cstdio>
#include <vector>

namespace
{

// TConfiguration::GetFileMimeType: MimeTypes "<mask>=<type>,..."; a mask with
// no type means no Content-Type at all
bool ConfiguredMimeType(const QString & FileName, QString & Type)
{
  QString Name = UnixPath::ExtractFileName(FileName);
  const QStringList Tokens = Configuration::Instance().MimeTypes.split(QLatin1Char(','), Qt::SkipEmptyParts);
  for (const QString & Token : Tokens)
  {
    qsizetype P = Token.indexOf(QLatin1Char('='));
    try
    {
      if (FileMasks(Token.left(P).trimmed()).MatchesFileName(Name))
      {
        Type = (P >= 0) ? Token.mid(P + 1).trimmed() : QString();
        return true;
      }
    }
    catch (const FileMasksException &)
    {
      // an invalid mask of the hidden setting matches nothing
    }
  }
  return false;
}

// TextsCore1.rc / TextsFileZilla.rc
const char * const CONNECTION_FAILED = "Connection failed.";
const char * const LOST_CONNECTION = "Lost connection.";
const char * const FILE_NOT_EXISTS = "File or folder '%1' does not exist.";
const char * const AUTHENTICATION_FAILED = "Authentication failed.";
const char * const NOTSUPPORTED = "Operation not supported.";
const char * const TRANSFER_ERROR = "Error transferring file '%1'.";
const char * const NET_TRANSL_HOST_NOT_EXIST2 = "Host \"%HOST%\" does not exist.";
const char * const S3_ERROR_RESOURCE = "Resource: %1";
const char * const S3_ERROR_FURTHER_DETAILS = "Further details: %1";
const char * const S3_ERROR_EXTRA_DETAILS = "Extra Details: ";
const char * const S3_STATUS_ACCESS_DENIED = "Access denied.";
const char * const DUPLICATE_FOLDER_NOT_SUPPORTED = "Direct duplication of folders is not supported. Use a duplication via a local temporary copy.";
const char * const MISSING_TARGET_BUCKET = "Specify target bucket.";
const char * const S3_PROFILE_NOT_EXIST = "S3 profile \"%1\" does not exist or does not contain any relevant options.";
const char * const CREDENTIALS_NOT_SPECIFIED = "Credentials were not specified.";
const char * const S3_ASSUME_ROLE_ERROR = "Error assuming role '%1'.";
const char * const S3_RESPONSE_ERROR = "Unexpected response to AWS request (%1).";
const char * const S3_ACCESS_KEY_ID_PROMPT = "A&ccess key ID:";
const char * const S3_SECRET_ACCESS_KEY_PROMPT = "Secret access &key:";
const char * const IDS_ERRORMSG_TIMEOUT = "Timeout detected.";
const char * const IDS_ERRORMSG_CANTESTABLISHSSLCONNECTION = "Can't establish TLS connection.";
const char * const IDS_ERRORMSG_CERTREJECTED = "Peer certificate rejected.";
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
// Terminal level messages (same as the other file systems)
const char * const LIST_DIR_ERROR = "Error listing directory '%1'.";
const char * const CREATE_DIR_ERROR = "Error creating folder '%1'.";
const char * const DELETE_FILE_ERROR = "Error deleting file '%1'.";
const char * const RENAME_FILE_ERROR = "Error renaming file '%1' to '%2'.";
const char * const CANT_SET_ATTRS = "Can't change properties of file '%1'.";
const char * const CREATE_LINK_ERROR = "Error creating link '%1'.";
const char * const CANT_GET_ATTRS = "Can't get attributes of file '%1'.";
const char * const COPY_FILE_ERROR = "Error copying file '%1' to '%2'.";
const char * const FILE_OPEN_ERROR = "Can't open file '%1'.";
const char * const FILE_READ_ERROR = "Can't read file '%1'.";
const char * const FILE_CREATE_ERROR = "Can't create file '%1'.";
const char * const FILE_WRITE_ERROR = "Can't write to file '%1'.";

QString Tr(const char * Text)
{
  return QString::fromUtf8(Text);
}

// AWS configuration (S3FileSystem.cpp defines)
const char * const AWS_ACCESS_KEY_ID = "AWS_ACCESS_KEY_ID";
const char * const AWS_SECRET_ACCESS_KEY = "AWS_SECRET_ACCESS_KEY";
const char * const AWS_SESSION_TOKEN = "AWS_SESSION_TOKEN";
const char * const AWS_SHARED_CREDENTIALS_FILE = "AWS_SHARED_CREDENTIALS_FILE";
const char * const AWS_CONFIG_FILE = "AWS_CONFIG_FILE";
const char * const AWS_PROFILE = "AWS_PROFILE";
const char * const AWS_PROFILE_DEFAULT = "default";
const char * const AWS_CONFIG_PROFILE_PREFIX = "profile ";
const char * const AWS_SOURCE_PROFILE_KEY = "source_profile";
const char * const AWS_CREDENTIAL_SOURCE_KEY = "credential_source";
const char * const AWS_CREDENTIAL_SOURCE_ENVIRONMENT = "Environment";
const char * const AWS_CREDENTIAL_SOURCE_METADATA = "Ec2InstanceMetadata";
const char * const AWS_ROLE_ARN = "AWS_ROLE_ARN";
const char * const AWS_ROLE_ARN_KEY = "role_arn";

const char * const S3GoogleCloudHostName = "storage.googleapis.com";
const char * const AssumeRoleVersion = "2011-06-15";
const char * const S3XmlNamespace = "http://s3.amazonaws.com/doc/2006-03-01/";
const char * const UnsignedPayload = "UNSIGNED-PAYLOAD";
const char * const UserAgent = "NixSCP";

// TS3FileSystem::S3MinMultiPartChunkSize / S3MaxMultiPartChunks
const qint64 S3MinMultiPartChunkSize = 5 * 1024 * 1024;
const int S3MaxMultiPartChunks = 10000;
const qint64 DefaultMultipartThreshold = 100 * 1024 * 1024;
const qint64 DefaultMultipartChunkSize = 16 * 1024 * 1024;
// PutObject / UploadPart limit
const qint64 S3MaxPutSize = Q_INT64_C(5) * 1024 * 1024 * 1024;
// CopyObject limit; bigger objects are copied with UploadPartCopy
const qint64 S3MaxCopySize = Q_INT64_C(5) * 1024 * 1024 * 1024;
const qint64 S3CopyPartSize = Q_INT64_C(512) * 1024 * 1024;

const qint64 TransferChunk = 64 * 1024;
const qint64 UploadHighWater = 256 * 1024;
// Limit for response bodies kept in memory (listings, errors)
const qint64 ResponseLimit = 64 * 1024 * 1024;
const qint64 MaxLineLength = 64 * 1024;
// "Expect: 100-continue" for bodies of at least this size; how long to wait
// for the interim response before sending the body anyway
const qint64 ExpectContinueMinSize = 1024 * 1024;
const int ContinueTimeoutMs = 3000;
const int MaxPartAttempts = 3;
const int MaxBucketDetectAttempts = 5;

bool LogEnabled()
{
  static const bool Enabled = !qEnvironmentVariableIsEmpty("NIXSCP_S3_LOG");
  return Enabled;
}

void LogLine(const QString & Line)
{
  SessionLog::LogProtocolLine(Line, 0);
  if (LogEnabled())
  {
    std::fprintf(stderr, "[s3] %s\n", qPrintable(Line));
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

// Socket level failure of a request (connection closed, timeout); a request
// sent over a reused keep-alive connection is repeated once on a new one
class NetworkError : public ExtException
{
public:
  using ExtException::ExtException;
};

// Certificates accepted with "Yes" (process wide, per endpoint)
QMutex AcceptedCertificatesMutex;
QSet<QString> AcceptedCertificates;

QString JoinMessages(const QString & Message, const QString & MoreMessages)
{
  if (Message.isEmpty())
  {
    return MoreMessages;
  }
  if (MoreMessages.isEmpty())
  {
    return Message;
  }
  return Message + QLatin1Char('\n') + MoreMessages;
}

QString Details(const ExtException & E)
{
  return JoinMessages(E.Message(), E.MoreMessages());
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

QString ExtractName(const QString & Key)
{
  return Key.mid(Key.lastIndexOf(QLatin1Char('/')) + 1);
}

QString ExcludeTrailingSlash(const QString & Path)
{
  QString Result = Path;
  if (Result.endsWith(QLatin1Char('/')))
  {
    Result.chop(1);
  }
  return Result;
}

QByteArray XmlEscape(const QString & Str)
{
  return Str.toHtmlEscaped().toUtf8();
}

// Decoding of keys listed with encoding-type=url (as unquote_plus)
QString UrlDecode(const QString & Str)
{
  QByteArray Bytes = Str.toUtf8();
  Bytes.replace('+', ' ');
  return QString::fromUtf8(QByteArray::fromPercentEncoding(Bytes));
}

// ISO 8601 times of listings ("2009-10-12T17:50:30.000Z"), with second
// precision as WinSCP parses them
QDateTime ParseIsoTime(const QString & Str)
{
  QString Trimmed = Str.trimmed();
  QDateTime Result = QDateTime::fromString(Trimmed, Qt::ISODateWithMs);
  if (!Result.isValid())
  {
    Result = QDateTime::fromString(Trimmed, Qt::ISODate);
  }
  if (Result.isValid())
  {
    Result = Result.toUTC();
    Result = Result.addMSecs(-Result.time().msec());
  }
  return Result;
}

// Minimal DOM of an XML response (local names, namespaces ignored)
struct XmlNode
{
  QString Name;
  QString Text;
  std::vector<XmlNode> Children;

  const XmlNode * Child(QStringView AName) const
  {
    for (const XmlNode & Node : Children)
    {
      if (Node.Name == AName)
      {
        return &Node;
      }
    }
    return nullptr;
  }

  QString ChildText(QStringView AName) const
  {
    const XmlNode * Node = Child(AName);
    return (Node != nullptr) ? Node->Text : QString();
  }
};

bool ParseXml(const QByteArray & Xml, XmlNode & Root, QString * Error)
{
  QXmlStreamReader Reader(Xml);
  // Pointers to the open elements; only the innermost element gets children,
  // so the pointers to its ancestors stay valid
  std::vector<XmlNode *> Stack;
  bool HaveRoot = false;
  while (!Reader.atEnd())
  {
    QXmlStreamReader::TokenType Token = Reader.readNext();
    if (Token == QXmlStreamReader::StartElement)
    {
      if (Stack.empty())
      {
        Root = XmlNode();
        Root.Name = Reader.name().toString();
        Stack.push_back(&Root);
        HaveRoot = true;
      }
      else
      {
        XmlNode * Parent = Stack.back();
        Parent->Children.emplace_back();
        Parent->Children.back().Name = Reader.name().toString();
        Stack.push_back(&Parent->Children.back());
      }
    }
    else if (Token == QXmlStreamReader::EndElement)
    {
      if (!Stack.empty())
      {
        Stack.pop_back();
      }
    }
    else if ((Token == QXmlStreamReader::Characters) && !Stack.empty())
    {
      Stack.back()->Text += Reader.text();
    }
  }
  if (Reader.hasError())
  {
    if (Error != nullptr)
    {
      *Error = Reader.errorString();
    }
    return false;
  }
  if (!HaveRoot && (Error != nullptr))
  {
    *Error = QStringLiteral("Empty response");
  }
  return HaveRoot;
}

bool IsXmlError(const QByteArray & Body)
{
  // CopyObject and CompleteMultipartUpload can report an error in a 200 response
  XmlNode Root;
  return Body.contains("<Error") && ParseXml(Body, Root, nullptr) && (Root.Name == QStringLiteral("Error"));
}

QString OwnerToken(const QString & Id, const QString & DisplayName)
{
  // TS3FileSystem::MakeRemoteToken
  return DisplayName.isEmpty() ? Id : DisplayName;
}

RemoteFile FileFromObject(const S3Object & Object, const QString & Name)
{
  // TS3FileSystem::LibS3ListBucketCallback
  RemoteFile File;
  File.FileName = Name;
  File.FileType = RemoteFile::File;
  File.Size = Object.Size;
  if (Object.LastModified.isValid())
  {
    File.Modification = Object.LastModified.toLocalTime();
  }
  File.Owner = OwnerToken(Object.OwnerId, Object.OwnerDisplayName);
  File.IsHidden = Name.startsWith(QLatin1Char('.'));
  return File;
}

bool IsVirtualHostCompatible(const QString & BucketName, const QString & HostName, bool Tls)
{
  // Virtual host style needs a DNS compatible bucket name; with TLS a dot in
  // the name would not match the wildcard certificate of the endpoint, and an
  // IP address endpoint cannot have subdomains at all
  if (!QHostAddress(HostName).isNull())
  {
    return false;
  }
  if ((BucketName.length() < 3) || (BucketName.length() > 63))
  {
    return false;
  }
  for (QChar C : BucketName)
  {
    bool Valid = ((C >= QLatin1Char('a')) && (C <= QLatin1Char('z'))) || ((C >= QLatin1Char('0')) && (C <= QLatin1Char('9'))) ||
      (C == QLatin1Char('-')) || (C == QLatin1Char('.'));
    if (!Valid)
    {
      return false;
    }
  }
  if (!BucketName.front().isLetterOrNumber() || !BucketName.back().isLetterOrNumber() ||
      BucketName.contains(QStringLiteral("..")) || BucketName.contains(QStringLiteral(".-")) || BucketName.contains(QStringLiteral("-.")))
  {
    return false;
  }
  if (Tls && BucketName.contains(QLatin1Char('.')))
  {
    return false;
  }
  return true;
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
  // CERT_ERR_* (TFTPFileSystem::HandleAsynchRequestVerifyCertificate)
  switch (Error.error())
  {
    case QSslError::UnableToGetIssuerCertificate: return QStringLiteral("Unable to get issuer certificate.");
    case QSslError::CertificateSignatureFailed: return QStringLiteral("Certificate signature failure.");
    case QSslError::CertificateNotYetValid: return QStringLiteral("Certificate is not yet valid.");
    case QSslError::CertificateExpired: return QStringLiteral("Certificate has expired.");
    case QSslError::SelfSignedCertificate: return QStringLiteral("Self-signed certificate.");
    case QSslError::SelfSignedCertificateInChain: return QStringLiteral("Self-signed certificate in certificate chain.");
    case QSslError::UnableToGetLocalIssuerCertificate: return QStringLiteral("Unable to get local issuer certificate.");
    case QSslError::UnableToVerifyFirstCertificate: return QStringLiteral("Unable to verify the first certificate.");
    case QSslError::InvalidCaCertificate: return QStringLiteral("Invalid CA certificate.");
    case QSslError::PathLengthExceeded: return QStringLiteral("Path length constraint exceeded.");
    case QSslError::InvalidPurpose: return QStringLiteral("Unsupported certificate purpose.");
    case QSslError::CertificateUntrusted: return QStringLiteral("Certificate not trusted.");
    case QSslError::CertificateRejected: return QStringLiteral("Certificate rejected.");
    case QSslError::CertificateRevoked: return QStringLiteral("Certificate revoked.");
    default: return Error.errorString();
  }
}

// ~/.aws/credentials and ~/.aws/config (TMemIniFile in WinSCP: section and
// key names are case insensitive)
struct AwsIniSection
{
  QString Name;
  QMap<QString, QString> Values;   // lower case keys
};
using AwsIniFile = QList<AwsIniSection>;

AwsIniFile ReadAwsIniFile(const QString & FileName)
{
  AwsIniFile Result;
  QFile File(FileName);
  if (FileName.isEmpty() || !File.open(QIODevice::ReadOnly | QIODevice::Text))
  {
    return Result;
  }
  while (!File.atEnd())
  {
    QString Line = QString::fromUtf8(File.readLine()).trimmed();
    if (Line.isEmpty() || Line.startsWith(QLatin1Char('#')) || Line.startsWith(QLatin1Char(';')))
    {
      continue;
    }
    if (Line.startsWith(QLatin1Char('[')) && Line.endsWith(QLatin1Char(']')))
    {
      AwsIniSection Section;
      Section.Name = Line.mid(1, Line.length() - 2).trimmed();
      Result.append(Section);
      continue;
    }
    qsizetype Equals = Line.indexOf(QLatin1Char('='));
    if ((Equals > 0) && !Result.isEmpty())
    {
      QString Name = Line.left(Equals).trimmed().toLower();
      if (!Result.last().Values.contains(Name))
      {
        Result.last().Values.insert(Name, Line.mid(Equals + 1).trimmed());
      }
    }
  }
  return Result;
}

QString ReadAwsIniValue(const AwsIniFile & File, const QString & Section, const QString & Name)
{
  for (const AwsIniSection & ASection : File)
  {
    if (ASection.Name.compare(Section, Qt::CaseInsensitive) == 0)
    {
      return ASection.Values.value(Name.toLower());
    }
  }
  return QString();
}

QString AwsFileName(const char * EnvName, const char * DefaultName)
{
  // NeedS3Config
  QString Result = qEnvironmentVariable(EnvName);
  if (Result.isEmpty())
  {
    QString DefaultFileName = QDir::homePath() + QStringLiteral("/.aws/") + QString::fromLatin1(DefaultName);
    if (QFileInfo::exists(DefaultFileName))
    {
      Result = DefaultFileName;
    }
  }
  return Result;
}

struct AwsConfig
{
  QString CredentialsFileName;
  AwsIniFile Credentials;
  QString ConfigFileName;
  AwsIniFile Config;
  QString Profile;
};

AwsConfig NeedS3Config()
{
  AwsConfig Result;
  Result.Profile = qEnvironmentVariable(AWS_PROFILE);
  if (Result.Profile.isEmpty())
  {
    Result.Profile = QString::fromLatin1(AWS_PROFILE_DEFAULT);
  }
  Result.CredentialsFileName = AwsFileName(AWS_SHARED_CREDENTIALS_FILE, "credentials");
  Result.Credentials = ReadAwsIniFile(Result.CredentialsFileName);
  Result.ConfigFileName = AwsFileName(AWS_CONFIG_FILE, "config");
  Result.Config = ReadAwsIniFile(Result.ConfigFileName);
  return Result;
}

QString GetS3ConfigValue(const AwsIniFile & File, const QString & FileName, const QString & Profile,
  const QString & Name, const QString & Prefix, QString & Source)
{
  QString ConfigSection = Profile;
  if (ConfigSection.compare(QString::fromLatin1(AWS_PROFILE_DEFAULT), Qt::CaseInsensitive) != 0)
  {
    ConfigSection = Prefix + ConfigSection;
  }
  QString Result = ReadAwsIniValue(File, ConfigSection, Name);
  if (!Result.isEmpty())
  {
    Source = QStringLiteral("%1/%2").arg(QFileInfo(FileName).fileName(), Profile);
  }
  return Result;
}

QString GetS3ConfigValue(const AwsConfig & Config, const QString & Profile, const QString & Name, QString & Source)
{
  QString Result = GetS3ConfigValue(Config.Credentials, Config.CredentialsFileName, Profile, Name, QString(), Source);
  if (Result.isEmpty())
  {
    Result = GetS3ConfigValue(Config.Config, Config.ConfigFileName, Profile, Name, QString::fromLatin1(AWS_CONFIG_PROFILE_PREFIX), Source);
  }
  return Result;
}

QString GetS3ConfigValue(const QString & Profile, const char * EnvName, const QString & AConfigName, QString * Source)
{
  QString Result;
  QString ASource;
  if (Profile.isEmpty())
  {
    Result = qEnvironmentVariable(EnvName);
  }
  if (!Result.isEmpty())
  {
    ASource = QStringLiteral("$%1").arg(QString::fromLatin1(EnvName));
  }
  else if (!AConfigName.isEmpty())
  {
    AwsConfig Config = NeedS3Config();
    QString AProfile = Profile.isEmpty() ? Config.Profile : Profile;
    Result = GetS3ConfigValue(Config, AProfile, AConfigName, ASource);
    if (Result.isEmpty())
    {
      QString SourceSource;
      QString SourceProfile = GetS3ConfigValue(Config, AProfile, QString::fromLatin1(AWS_SOURCE_PROFILE_KEY), SourceSource);
      if (!SourceProfile.isEmpty())
      {
        Result = GetS3ConfigValue(Config, SourceProfile, AConfigName, ASource);
      }
      else
      {
        QString CredentialSource = GetS3ConfigValue(Config, AProfile, QString::fromLatin1(AWS_CREDENTIAL_SOURCE_KEY), SourceSource);
        if (CredentialSource.compare(QString::fromLatin1(AWS_CREDENTIAL_SOURCE_ENVIRONMENT), Qt::CaseInsensitive) == 0)
        {
          Result = GetS3ConfigValue(QString(), EnvName, QString(), &ASource);
        }
        // Ec2InstanceMetadata (IMDS) is not supported
      }
      if (!Result.isEmpty())
      {
        ASource = QStringLiteral("%1=>%2").arg(SourceSource, ASource);
      }
    }
  }
  if (Source != nullptr)
  {
    *Source = ASource;
  }
  return Result;
}

void AddS3Profiles(QStringList & Profiles, const AwsIniFile & File, const QString & Prefix)
{
  // GetS3Profiles
  for (const AwsIniSection & Section : File)
  {
    if (Prefix.isEmpty() || Section.Name.startsWith(Prefix, Qt::CaseInsensitive))
    {
      bool Supported = false;
      if (!Section.Values.value(QString::fromLatin1(AWS_ACCESS_KEY_ID).toLower()).isEmpty() &&
          !Section.Values.value(QString::fromLatin1(AWS_SECRET_ACCESS_KEY).toLower()).isEmpty())
      {
        Supported = true;
      }
      else if (!Section.Values.value(QString::fromLatin1(AWS_ROLE_ARN_KEY)).isEmpty())
      {
        if (!Section.Values.value(QString::fromLatin1(AWS_SOURCE_PROFILE_KEY)).isEmpty())
        {
          Supported = true;
        }
        else
        {
          QString CredentialSource = Section.Values.value(QString::fromLatin1(AWS_CREDENTIAL_SOURCE_KEY));
          Supported =
            (CredentialSource.compare(QString::fromLatin1(AWS_CREDENTIAL_SOURCE_ENVIRONMENT), Qt::CaseInsensitive) == 0) ||
            (CredentialSource.compare(QString::fromLatin1(AWS_CREDENTIAL_SOURCE_METADATA), Qt::CaseInsensitive) == 0);
        }
      }
      QString Name = Section.Name.mid(Prefix.length());
      if (Supported && !Profiles.contains(Name, Qt::CaseInsensitive))
      {
        Profiles.append(Name);
      }
    }
  }
}

}

//---------------------------------------------------------------------------
// AWS Signature Version 4
//---------------------------------------------------------------------------

QByteArray S3SignatureV4::UriEncode(const QByteArray & Utf8, bool EncodeSlash)
{
  // libs3 urlEncode
  static const char Hex[] = "0123456789ABCDEF";
  QByteArray Result;
  Result.reserve(Utf8.size() * 3);
  for (char Ch : Utf8)
  {
    unsigned char C = static_cast<unsigned char>(Ch);
    if (((C >= 'A') && (C <= 'Z')) || ((C >= 'a') && (C <= 'z')) || ((C >= '0') && (C <= '9')) ||
        (C == '-') || (C == '_') || (C == '.') || (C == '~') || ((C == '/') && !EncodeSlash))
    {
      Result.append(Ch);
    }
    else
    {
      Result.append('%');
      Result.append(Hex[C >> 4]);
      Result.append(Hex[C & 15]);
    }
  }
  return Result;
}

QByteArray S3SignatureV4::AmzDate(const QDateTime & Time)
{
  return Time.toUTC().toString(QStringLiteral("yyyyMMdd'T'HHmmss'Z'")).toLatin1();
}

QByteArray S3SignatureV4::Sha256Hex(const QByteArray & Data)
{
  unsigned char Digest[EVP_MAX_MD_SIZE];
  unsigned int Length = 0;
  if (EVP_Digest(Data.constData(), static_cast<size_t>(Data.size()), Digest, &Length, EVP_sha256(), nullptr) != 1)
  {
    throw ExtException(QStringLiteral("SHA-256 failed."));
  }
  return QByteArray(reinterpret_cast<const char *>(Digest), static_cast<qsizetype>(Length)).toHex();
}

QByteArray S3SignatureV4::HmacSha256(const QByteArray & Key, const QByteArray & Data)
{
  unsigned char Digest[EVP_MAX_MD_SIZE];
  unsigned int Length = 0;
  if (HMAC(EVP_sha256(), Key.constData(), static_cast<int>(Key.size()),
        reinterpret_cast<const unsigned char *>(Data.constData()), static_cast<size_t>(Data.size()), Digest, &Length) == nullptr)
  {
    throw ExtException(QStringLiteral("HMAC-SHA256 failed."));
  }
  return QByteArray(reinterpret_cast<const char *>(Digest), static_cast<qsizetype>(Length));
}

QByteArray S3SignatureV4::EmptyPayloadHash()
{
  return QByteArrayLiteral("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

void S3SignatureV4::Sign()
{
  // Canonical query string: names and values URI-encoded (including "/"),
  // sorted by name, then value
  S3NameValueList EncodedQuery;
  for (const auto & Param : Query)
  {
    EncodedQuery.append(qMakePair(UriEncode(Param.first, true), UriEncode(Param.second, true)));
  }
  std::sort(EncodedQuery.begin(), EncodedQuery.end());
  QList<QByteArray> QueryParts;
  for (const auto & Param : EncodedQuery)
  {
    QueryParts.append(Param.first + '=' + Param.second);
  }
  CanonicalQueryString = QueryParts.join('&');

  // Canonical headers: lower case names, sorted, values trimmed with
  // sequential spaces collapsed; repeated headers joined with ","
  QMap<QByteArray, QByteArray> CanonicalHeaderMap;
  for (const auto & Header : Headers)
  {
    QByteArray Name = Header.first.trimmed().toLower();
    QByteArray Value = Header.second.simplified();
    auto I = CanonicalHeaderMap.find(Name);
    if (I == CanonicalHeaderMap.end())
    {
      CanonicalHeaderMap.insert(Name, Value);
    }
    else
    {
      I.value() += ',' + Value;
    }
  }
  QByteArray CanonicalHeaders;
  QList<QByteArray> SignedHeaderNames;
  for (auto I = CanonicalHeaderMap.cbegin(); I != CanonicalHeaderMap.cend(); ++I)
  {
    CanonicalHeaders += I.key() + ':' + I.value() + '\n';
    SignedHeaderNames.append(I.key());
  }
  SignedHeaders = SignedHeaderNames.join(';');

  CanonicalRequest =
    Method + '\n' +
    CanonicalUri + '\n' +
    CanonicalQueryString + '\n' +
    CanonicalHeaders + '\n' +
    SignedHeaders + '\n' +
    PayloadHash;

  QByteArray RequestDate = AmzDate(Time);
  QByteArray Date = RequestDate.left(8);
  QByteArray Scope = Date + '/' + Region + '/' + Service + "/aws4_request";
  StringToSign =
    "AWS4-HMAC-SHA256\n" +
    RequestDate + '\n' +
    Scope + '\n' +
    Sha256Hex(CanonicalRequest);

  QByteArray SigningKey = HmacSha256("AWS4" + SecretAccessKey, Date);
  SigningKey = HmacSha256(SigningKey, Region);
  SigningKey = HmacSha256(SigningKey, Service);
  SigningKey = HmacSha256(SigningKey, "aws4_request");
  Signature = HmacSha256(SigningKey, StringToSign).toHex();

  Authorization =
    "AWS4-HMAC-SHA256 Credential=" + AccessKeyId + '/' + Scope +
    ", SignedHeaders=" + SignedHeaders +
    ", Signature=" + Signature;
}

//---------------------------------------------------------------------------
// XML responses
//---------------------------------------------------------------------------

bool S3FileSystem::ParseListAllMyBucketsResult(const QByteArray & Xml, S3ListAllMyBucketsResult & Result, QString * Error)
{
  XmlNode Root;
  if (!ParseXml(Xml, Root, Error))
  {
    return false;
  }
  if (Root.Name != QStringLiteral("ListAllMyBucketsResult"))
  {
    if (Error != nullptr)
    {
      *Error = Tr(S3_RESPONSE_ERROR).arg(Root.Name);
    }
    return false;
  }
  Result = S3ListAllMyBucketsResult();
  if (const XmlNode * Owner = Root.Child(u"Owner"))
  {
    Result.OwnerId = Owner->ChildText(u"ID");
    Result.OwnerDisplayName = Owner->ChildText(u"DisplayName");
  }
  Result.ContinuationToken = Root.ChildText(u"ContinuationToken");
  if (const XmlNode * Buckets = Root.Child(u"Buckets"))
  {
    for (const XmlNode & Node : Buckets->Children)
    {
      if (Node.Name == QStringLiteral("Bucket"))
      {
        S3Bucket Bucket;
        Bucket.Name = Node.ChildText(u"Name");
        Bucket.CreationDate = ParseIsoTime(Node.ChildText(u"CreationDate"));
        Bucket.Region = Node.ChildText(u"BucketRegion");
        Result.Buckets.append(Bucket);
      }
    }
  }
  return true;
}

bool S3FileSystem::ParseListBucketResult(const QByteArray & Xml, S3ListBucketResult & Result, QString * Error)
{
  XmlNode Root;
  if (!ParseXml(Xml, Root, Error))
  {
    return false;
  }
  if (Root.Name != QStringLiteral("ListBucketResult"))
  {
    if (Error != nullptr)
    {
      *Error = Tr(S3_RESPONSE_ERROR).arg(Root.Name);
    }
    return false;
  }
  Result = S3ListBucketResult();
  Result.UrlEncoded = (Root.ChildText(u"EncodingType").trimmed().compare(QStringLiteral("url"), Qt::CaseInsensitive) == 0);
  bool UrlEncoded = Result.UrlEncoded;
  auto Decode = [UrlEncoded](const QString & Str) { return UrlEncoded ? UrlDecode(Str) : Str; };
  Result.Name = Root.ChildText(u"Name");
  Result.Prefix = Decode(Root.ChildText(u"Prefix"));
  Result.IsTruncated = (Root.ChildText(u"IsTruncated").trimmed().compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0);
  Result.NextContinuationToken = Root.ChildText(u"NextContinuationToken");
  Result.NextMarker = Decode(Root.ChildText(u"NextMarker"));
  for (const XmlNode & Node : Root.Children)
  {
    if (Node.Name == QStringLiteral("Contents"))
    {
      S3Object Object;
      Object.Key = Decode(Node.ChildText(u"Key"));
      Object.LastModified = ParseIsoTime(Node.ChildText(u"LastModified"));
      Object.Size = Node.ChildText(u"Size").trimmed().toLongLong();
      Object.ETag = Node.ChildText(u"ETag");
      if (const XmlNode * Owner = Node.Child(u"Owner"))
      {
        Object.OwnerId = Owner->ChildText(u"ID");
        Object.OwnerDisplayName = Owner->ChildText(u"DisplayName");
      }
      Result.Contents.append(Object);
    }
    else if (Node.Name == QStringLiteral("CommonPrefixes"))
    {
      for (const XmlNode & Child : Node.Children)
      {
        if (Child.Name == QStringLiteral("Prefix"))
        {
          Result.CommonPrefixes.append(Decode(Child.Text));
        }
      }
    }
  }
  return true;
}

bool S3FileSystem::ParseError(const QByteArray & Xml, S3ErrorResult & Result)
{
  // libs3 error_parser.c: Code, Message, Resource, FurtherDetails and any
  // other element as "extra details"
  Result = S3ErrorResult();
  XmlNode Root;
  if (Xml.isEmpty() || !ParseXml(Xml, Root, nullptr) || (Root.Name != QStringLiteral("Error")))
  {
    return false;
  }
  for (const XmlNode & Node : Root.Children)
  {
    if (Node.Name == QStringLiteral("Code"))
    {
      Result.Code = Node.Text.trimmed();
    }
    else if (Node.Name == QStringLiteral("Message"))
    {
      Result.Message = Node.Text.trimmed();
    }
    else if (Node.Name == QStringLiteral("Resource"))
    {
      Result.Resource = Node.Text.trimmed();
    }
    else
    {
      if (Node.Name == QStringLiteral("Region"))
      {
        Result.Region = Node.Text.trimmed();
      }
      else if (Node.Name == QStringLiteral("Endpoint"))
      {
        Result.Endpoint = Node.Text.trimmed();
      }
      Result.ExtraDetails.append(qMakePair(Node.Name, Node.Text.trimmed()));
    }
  }
  return true;
}

void S3FileSystem::ParsePath(const QString & APath, QString & BucketName, QString & Key)
{
  QString Path = APath;
  if (Path.startsWith(QLatin1Char('/')))
  {
    Path.remove(0, 1);
  }
  qsizetype P = Path.indexOf(QLatin1Char('/'));
  if (P < 0)
  {
    BucketName = Path;
    Key.clear();
  }
  else
  {
    BucketName = Path.left(P);
    Key = Path.mid(P + 1);
  }
}

//---------------------------------------------------------------------------
// AWS credentials
//---------------------------------------------------------------------------

QString S3FileSystem::EnvUserName(const QString & Profile, QString * Source)
{
  return GetS3ConfigValue(Profile, AWS_ACCESS_KEY_ID, QString::fromLatin1(AWS_ACCESS_KEY_ID), Source);
}

QString S3FileSystem::EnvPassword(const QString & Profile, QString * Source)
{
  return GetS3ConfigValue(Profile, AWS_SECRET_ACCESS_KEY, QString::fromLatin1(AWS_SECRET_ACCESS_KEY), Source);
}

QString S3FileSystem::EnvSessionToken(const QString & Profile, QString * Source)
{
  return GetS3ConfigValue(Profile, AWS_SESSION_TOKEN, QString::fromLatin1(AWS_SESSION_TOKEN), Source);
}

QString S3FileSystem::EnvRoleArn(const QString & Profile, QString * Source)
{
  return GetS3ConfigValue(Profile, AWS_ROLE_ARN, QString::fromLatin1(AWS_ROLE_ARN_KEY), Source);
}

QStringList S3FileSystem::Profiles()
{
  AwsConfig Config = NeedS3Config();
  QStringList Result;
  AddS3Profiles(Result, Config.Credentials, QString());
  AddS3Profiles(Result, Config.Config, QString::fromLatin1(AWS_CONFIG_PROFILE_PREFIX));
  std::sort(Result.begin(), Result.end(), [](const QString & A, const QString & B) { return A.compare(B, Qt::CaseInsensitive) < 0; });
  return Result;
}

//---------------------------------------------------------------------------
// HTTP
//---------------------------------------------------------------------------

struct S3FileSystem::HttpRequest
{
  QByteArray Method;
  QString Host;
  quint16 Port = 0;
  bool Tls = true;
  QString Path = QStringLiteral("/");   // decoded
  S3NameValueList Query;                // decoded
  S3NameValueList Headers;              // signed
  QString Region;
  QByteArray Service = "s3";
  // Body: Data, or FileLength bytes of File from FileOffset (unsigned payload)
  QByteArray Data;
  QFile * File = nullptr;
  qint64 FileOffset = 0;
  qint64 FileLength = 0;
  // Receives the body of a successful response (otherwise HttpResponse::Body)
  BodyTarget Target;
};

struct S3FileSystem::HttpResponse
{
  int Status = 0;
  QByteArray Version;
  QByteArray Reason;
  S3NameValueList Headers;              // lower case names
  QByteArray Body;
  bool CloseConnection = false;

  QByteArray Header(const char * Name) const
  {
    for (const auto & H : Headers)
    {
      if (H.first == Name)
      {
        return H.second;
      }
    }
    return QByteArray();
  }

  bool Ok() const { return (Status >= 200) && (Status < 300); }
};

struct S3FileSystem::BucketContext
{
  QString BucketName;
  QString Host;
  QString PathPrefix;                   // "/bucket" for path style URLs
  QString Region;
};

const char * const S3FileSystem::DefaultHostName = "s3.amazonaws.com";
const char * const S3FileSystem::DefaultRegion = "us-east-1";

S3FileSystem::S3FileSystem(const SessionData & Data, UserInterface * UI) :
  FData(Data),
  FUI(UI),
  FMultipartThreshold(DefaultMultipartThreshold),
  FMultipartChunkSize(DefaultMultipartChunkSize)
{
}

S3FileSystem::~S3FileSystem()
{
  CloseConnection();
}

void S3FileSystem::SetMultipartThreshold(qint64 Threshold, qint64 ChunkSize)
{
  // a single PUT can upload at most 5 GB
  FMultipartThreshold = std::clamp<qint64>(Threshold, 0, S3MaxPutSize);
  FMultipartChunkSize = std::clamp(ChunkSize, S3MinMultiPartChunkSize, S3MaxPutSize);
}

int S3FileSystem::TimeoutMs() const
{
  return std::max(FData.Timeout, 1) * 1000;
}

bool S3FileSystem::IsAmazon() const
{
  // IsAmazonS3SessionData (IsDomainOrSubdomain)
  QString Host = FHostName.toLower();
  QString S3Host = QString::fromLatin1(DefaultHostName);
  return (Host == S3Host) || Host.endsWith(QLatin1Char('.') + S3Host);
}

bool S3FileSystem::IsGoogleCloud() const
{
  return (FHostName.compare(QString::fromLatin1(S3GoogleCloudHostName), Qt::CaseInsensitive) == 0);
}

int S3FileSystem::EffectiveMaxKeys(int MaxKeys) const
{
  if ((FData.S3MaxKeys == asOff) || ((FData.S3MaxKeys == asAuto) && IsGoogleCloud()))
  {
    return 0;
  }
  return MaxKeys;
}

bool S3FileSystem::IsOpened() const
{
  return FActive;
}

QString S3FileSystem::ProtocolName() const
{
  return QStringLiteral("S3");
}

QString S3FileSystem::ServerInfo() const
{
  QStringList Lines;
  QString Endpoint = FHostName;
  if (FPort != static_cast<quint16>(FTls ? HTTPSPortNumber : HTTPPortNumber))
  {
    Endpoint += QStringLiteral(":%1").arg(FPort);
  }
  Lines << QStringLiteral("Endpoint: %1").arg(Endpoint);
  Lines << QStringLiteral("Authentication region: %1").arg(FAuthRegion);
  if (FTls)
  {
    QString Security = QStringLiteral("TLS/SSL");
    if (!FTlsVersion.isEmpty())
    {
      Security += QStringLiteral(", ") + FTlsVersion;
    }
    Lines << QStringLiteral("Encryption: %1").arg(Security);
    if (!FCipher.isEmpty())
    {
      Lines << QStringLiteral("Cipher: %1").arg(FCipher);
    }
  }
  return Lines.join(QLatin1Char('\n'));
}

void S3FileSystem::CloseConnection()
{
  if (FSocket)
  {
    FSocket->abort();
    FSocket.reset();
  }
  FSocketHost.clear();
  FSocketPort = 0;
}

bool S3FileSystem::EnsureConnection(const QString & Host, quint16 Port, bool Tls)
{
  if (FSocket && (FSocketHost == Host) && (FSocketPort == Port) && (FSocketTls == Tls) &&
      (FSocket->state() == QAbstractSocket::ConnectedState))
  {
    // Notice a keep-alive connection the server has closed meanwhile
    FSocket->waitForReadyRead(0);
    if ((FSocket->state() == QAbstractSocket::ConnectedState) && (FSocket->bytesAvailable() == 0))
    {
      return true;
    }
  }
  CloseConnection();

  auto Socket = std::make_unique<QSslSocket>();
  QNetworkProxy Proxy(QNetworkProxy::NoProxy);
  bool Local = (Host == QStringLiteral("localhost")) || QHostAddress(Host).isLoopback();
  if (!Local || FData.ProxyLocalhost)
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
        // SOCKS4, Telnet and local proxies are not supported by QtNetwork
        break;
    }
  }
  Socket->setProxy(Proxy);
  if (FData.TcpNoDelay)
  {
    Socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
  }
  if (Tls)
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
    // Errors are evaluated after the handshake (VerifyCertificate); the
    // handshake runs within waitFor* on this thread, so the slot is called directly
    QSslSocket * ASocket = Socket.get();
    QObject::connect(ASocket, &QSslSocket::sslErrors, ASocket, [ASocket](const QList<QSslError> &)
    {
      ASocket->ignoreSslErrors();
    });
  }
  QAbstractSocket::NetworkLayerProtocol Family =
    (FData.AddressFamily == afIPv4) ? QAbstractSocket::IPv4Protocol :
    ((FData.AddressFamily == afIPv6) ? QAbstractSocket::IPv6Protocol : QAbstractSocket::AnyIPProtocol);
  LogLine(QStringLiteral("Connecting to %1:%2 ...").arg(Host).arg(Port));
  if (Tls)
  {
    Socket->connectToHostEncrypted(Host, Port, Host, QIODevice::ReadWrite, Family);
  }
  else
  {
    Socket->connectToHost(Host, Port, QIODevice::ReadWrite, Family);
  }
  if (!Socket->waitForConnected(TimeoutMs()))
  {
    QString Error = Socket->errorString();
    if (Socket->error() == QAbstractSocket::HostNotFoundError)
    {
      Error = Tr(NET_TRANSL_HOST_NOT_EXIST2).replace(QStringLiteral("%HOST%"), Host);
    }
    else if (Socket->error() == QAbstractSocket::SocketTimeoutError)
    {
      Error = QStringLiteral("Network error: Connection to \"%1\" timed out.").arg(Host);
    }
    throw ExtException(Tr(CONNECTION_FAILED), Error);
  }
  if (Tls)
  {
    if (!Socket->waitForEncrypted(TimeoutMs()))
    {
      throw ExtException(Tr(IDS_ERRORMSG_CANTESTABLISHSSLCONNECTION), Socket->errorString());
    }
    QList<QSslError> Errors = Socket->sslHandshakeErrors();
    FSocket = std::move(Socket);
    FSocketHost = Host;
    FSocketPort = Port;
    FSocketTls = Tls;
    try
    {
      VerifyCertificate(Host, Errors);
    }
    catch (...)
    {
      CloseConnection();
      throw;
    }
    switch (FSocket->sessionProtocol())
    {
      case QSsl::TlsV1_2: FTlsVersion = QStringLiteral("TLSv1.2"); break;
      case QSsl::TlsV1_3: FTlsVersion = QStringLiteral("TLSv1.3"); break;
      default: FTlsVersion.clear(); break;
    }
    FCipher = FSocket->sessionCipher().name();
  }
  else
  {
    FSocket = std::move(Socket);
    FSocketHost = Host;
    FSocketPort = Port;
    FSocketTls = Tls;
  }
  return false;
}

void S3FileSystem::VerifyCertificate(const QString & Host, const QList<QSslError> & Errors)
{
  // TTerminal::VerifyOrConfirmHttpCertificate, with the texts of
  // TFTPFileSystem::HandleAsynchRequestVerifyCertificate
  QSslCertificate Certificate = FSocket->peerCertificate();
  QString FingerprintSHA256 = QString::fromLatin1(Certificate.digest(QCryptographicHash::Sha256).toHex(':'));
  QString FingerprintSHA1 = QString::fromLatin1(Certificate.digest(QCryptographicHash::Sha1).toHex(':'));
  QList<QSslCertificate> Chain = FSocket->peerCertificateChain();

  bool Trusted = true;
  bool NameMismatch = false;
  QStringList Problems;
  for (const QSslError & Error : Errors)
  {
    if (Error.error() == QSslError::HostNameMismatch)
    {
      NameMismatch = true;
      continue;
    }
    Trusted = false;
    qsizetype Depth = Chain.indexOf(Error.certificate());
    QString Text = CertificateErrorText(Error) + QLatin1Char(' ') + Tr(CERT_ERRDEPTH).arg((Depth >= 0) ? (Depth + 1) : 1);
    if (!Problems.contains(Text))
    {
      Problems << Text;
    }
  }
  bool IsHostNameIPAddress = !QHostAddress(Host).isNull();
  if (IsHostNameIPAddress)
  {
    Problems << Tr(CERT_IP_CANNOT_VERIFY);
  }
  else if (NameMismatch)
  {
    Problems << Tr(CERT_NAME_MISMATCH).arg(Host);
  }
  if (Trusted && !IsHostNameIPAddress && !NameMismatch)
  {
    return;
  }
  // Keyed by the endpoint of the site, so that a certificate is confirmed
  // once and not for each bucket host name (virtual host style URLs)
  QString Key = QStringLiteral("%1:%2=%3").arg(FHostName).arg(FPort).arg(FingerprintSHA256);
  bool OtherAccepted = false;
  {
    QMutexLocker Lock(&AcceptedCertificatesMutex);
    if (AcceptedCertificates.contains(Key))
    {
      return;
    }
    OtherAccepted = HasAcceptedCertificate(AcceptedCertificates, FHostName, FPort);
  }
  if (FAcceptedOnce.contains(Key))
  {
    return;
  }
  // As with an SSH host key: a fingerprint in a URL does not outrank the
  // certificate accepted for this server before, a different one is a question
  OtherAccepted = OtherAccepted || HasAcceptedCertificate(FAcceptedOnce, FHostName, FPort);
  if (!(FData.HostKeyFromUrl && OtherAccepted) &&
      CertificateMatchesSessionData(FData.TrustedHostKey(), FingerprintSHA1, FingerprintSHA256))
  {
    return;
  }
  QString Summary = Problems.isEmpty() ? Tr(CERT_OK) : Problems.join(QStringLiteral("\n\n"));
  QString CertificateDetails = Tr(CERT_TEXT2).arg(
    FormatContact(Certificate, true), FormatContact(Certificate, false),
    QLocale().toString(Certificate.effectiveDate().toLocalTime(), QLocale::ShortFormat),
    QLocale().toString(Certificate.expiryDate().toLocalTime(), QLocale::ShortFormat),
    FingerprintSHA256, FingerprintSHA1, Summary);
  unsigned int Answer = FUI->QueryUser(Tr(VERIFY_CERT_PROMPT3).arg(CertificateDetails), QString(), qaYes | qaNo | qaCancel);
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
    throw FatalException(Tr(IDS_ERRORMSG_CERTREJECTED));
  }
}

void S3FileSystem::WriteAll(const QByteArray & Data, FileOperationProgress * Progress)
{
  if (!Data.isEmpty() && (FSocket->write(Data) != Data.size()))
  {
    throw NetworkError(Tr(LOST_CONNECTION), FSocket->errorString());
  }
  QElapsedTimer Idle;
  Idle.start();
  qint64 Pending = FSocket->bytesToWrite() + FSocket->encryptedBytesToWrite();
  while (Pending > ((Progress != nullptr) ? UploadHighWater : 0))
  {
    if (Progress != nullptr)
    {
      Progress->CheckCancel();
    }
    if (FSocket->state() != QAbstractSocket::ConnectedState)
    {
      throw NetworkError(Tr(LOST_CONNECTION), FSocket->errorString());
    }
    FSocket->waitForBytesWritten(100);
    qint64 Now = FSocket->bytesToWrite() + FSocket->encryptedBytesToWrite();
    if (Now < Pending)
    {
      Idle.restart();
    }
    else if (Idle.elapsed() > TimeoutMs())
    {
      throw NetworkError(Tr(LOST_CONNECTION), Tr(IDS_ERRORMSG_TIMEOUT));
    }
    Pending = Now;
  }
}

S3FileSystem::WaitResult S3FileSystem::WaitForData(FileOperationProgress * Progress, int Timeout, qint64 Have)
{
  QElapsedTimer Timer;
  Timer.start();
  while (FSocket->bytesAvailable() <= Have)
  {
    if (Progress != nullptr)
    {
      Progress->CheckCancel();
    }
    if (FSocket->state() != QAbstractSocket::ConnectedState)
    {
      return wrEof;
    }
    qint64 Remaining = Timeout - Timer.elapsed();
    if (Remaining <= 0)
    {
      return wrTimeout;
    }
    if (!FSocket->waitForReadyRead(static_cast<int>(std::min<qint64>(100, Remaining))) &&
        (FSocket->state() != QAbstractSocket::ConnectedState) && (FSocket->bytesAvailable() <= Have))
    {
      return wrEof;
    }
  }
  return wrData;
}

QByteArray S3FileSystem::ReadLine(FileOperationProgress * Progress)
{
  for (;;)
  {
    if (FSocket->canReadLine())
    {
      QByteArray Line = FSocket->readLine(MaxLineLength + 2);
      if (!Line.endsWith('\n'))
      {
        throw NetworkError(Tr(LOST_CONNECTION), QStringLiteral("Response line too long."));
      }
      Line.chop(1);
      if (Line.endsWith('\r'))
      {
        Line.chop(1);
      }
      return Line;
    }
    qint64 Have = FSocket->bytesAvailable();
    if (Have > MaxLineLength)
    {
      throw NetworkError(Tr(LOST_CONNECTION), QStringLiteral("Response line too long."));
    }
    switch (WaitForData(Progress, TimeoutMs(), Have))
    {
      case wrData:
        break;
      case wrTimeout:
        throw NetworkError(Tr(LOST_CONNECTION), Tr(IDS_ERRORMSG_TIMEOUT));
      case wrEof:
        throw NetworkError(Tr(LOST_CONNECTION), FSocket->errorString());
    }
  }
}

void S3FileSystem::SendBody(const HttpRequest & Request, FileOperationProgress * Progress, qint64 & Uploaded)
{
  if (Request.File == nullptr)
  {
    WriteAll(Request.Data, nullptr);
    return;
  }
  QFile * File = Request.File;
  if (!File->seek(Request.FileOffset))
  {
    throw ExtException(Tr(FILE_READ_ERROR).arg(File->fileName()), File->errorString());
  }
  qint64 Remaining = Request.FileLength;
  while (Remaining > 0)
  {
    qint64 Want = std::min(TransferChunk, Remaining);
    if (Progress != nullptr)
    {
      Progress->CheckCancel();
      Want = static_cast<qint64>(Progress->AdjustToCPSLimit(static_cast<unsigned long>(Want)));
      Progress->CheckCancel();
    }
    // a new buffer each time: QIODevice::write(QByteArray) may queue the
    // (implicitly shared) array itself instead of copying it
    QByteArray Buffer = File->read(Want);
    if (Buffer.isEmpty())
    {
      throw ExtException(Tr(FILE_READ_ERROR).arg(File->fileName()), File->errorString());
    }
    WriteAll(Buffer, Progress);
    Remaining -= Buffer.size();
    Uploaded += Buffer.size();
    if (Progress != nullptr)
    {
      Progress->AddTransferred(Buffer.size());
    }
  }
  WriteAll(QByteArray(), nullptr);
}

void S3FileSystem::ReadResponseHead(HttpResponse & Response, FileOperationProgress * Progress, bool & AnyResponse)
{
  if (FSocket->bytesAvailable() == 0)
  {
    switch (WaitForData(Progress, TimeoutMs(), 0))
    {
      case wrData:
        break;
      case wrTimeout:
        throw NetworkError(Tr(LOST_CONNECTION), Tr(IDS_ERRORMSG_TIMEOUT));
      case wrEof:
        throw NetworkError(Tr(LOST_CONNECTION), FSocket->errorString());
    }
  }
  AnyResponse = true;
  QByteArray StatusLine = ReadLine(Progress);
  // "HTTP/1.1 200 OK"
  QList<QByteArray> Parts = StatusLine.split(' ');
  bool Ok = false;
  int Status = (Parts.size() >= 2) ? Parts[1].toInt(&Ok) : 0;
  if (!StatusLine.startsWith("HTTP/") || !Ok || (Status < 100) || (Status > 999))
  {
    throw NetworkError(Tr(LOST_CONNECTION), QStringLiteral("Invalid HTTP response: %1").arg(QString::fromLatin1(StatusLine.left(100))));
  }
  Response = HttpResponse();
  Response.Version = Parts[0];
  Response.Status = Status;
  qsizetype ReasonStart = StatusLine.indexOf(' ', StatusLine.indexOf(' ') + 1);
  Response.Reason = (ReasonStart > 0) ? StatusLine.mid(ReasonStart + 1) : QByteArray();
  for (;;)
  {
    QByteArray Line = ReadLine(Progress);
    if (Line.isEmpty())
    {
      break;
    }
    if (((Line.at(0) == ' ') || (Line.at(0) == '\t')) && !Response.Headers.isEmpty())
    {
      // obsolete line folding
      Response.Headers.last().second += ' ' + Line.trimmed();
      continue;
    }
    qsizetype Colon = Line.indexOf(':');
    if (Colon > 0)
    {
      Response.Headers.append(qMakePair(Line.left(Colon).trimmed().toLower(), Line.mid(Colon + 1).trimmed()));
    }
  }
  LogLine(QStringLiteral("< %1").arg(QString::fromLatin1(StatusLine)));
}

void S3FileSystem::ReadResponseBody(const HttpRequest & Request, HttpResponse & Response, FileOperationProgress * Progress)
{
  QByteArray ConnectionHeader = Response.Header("connection").toLower();
  Response.CloseConnection =
    ConnectionHeader.contains("close") ||
    ((Response.Version == "HTTP/1.0") && !ConnectionHeader.contains("keep-alive"));
  if ((Request.Method == "HEAD") || (Response.Status == 204) || (Response.Status == 304) || ((Response.Status / 100) == 1))
  {
    return;
  }
  QFile * Target = nullptr;
  if (Response.Ok() && Request.Target)
  {
    Target = Request.Target(Response);
  }
  bool Chunked = Response.Header("transfer-encoding").toLower().contains("chunked");
  qint64 Length = -1;
  if (!Chunked)
  {
    QByteArray ContentLength = Response.Header("content-length");
    if (!ContentLength.isEmpty())
    {
      bool Ok = false;
      Length = ContentLength.toLongLong(&Ok);
      if (!Ok || (Length < 0))
      {
        throw NetworkError(Tr(LOST_CONNECTION), QStringLiteral("Invalid Content-Length: %1").arg(QString::fromLatin1(ContentLength)));
      }
    }
    else
    {
      // body ends when the server closes the connection
      Response.CloseConnection = true;
    }
  }

  // Reads Size bytes of the body (-1 = until the connection is closed)
  auto ReadBodyData = [&](qint64 Size)
  {
    qint64 Remaining = Size;
    while ((Size < 0) || (Remaining > 0))
    {
      if (FSocket->bytesAvailable() == 0)
      {
        WaitResult Result = WaitForData(Progress, TimeoutMs(), 0);
        if (Result == wrTimeout)
        {
          throw NetworkError(Tr(LOST_CONNECTION), Tr(IDS_ERRORMSG_TIMEOUT));
        }
        if (Result == wrEof)
        {
          if (Size < 0)
          {
            break;
          }
          throw NetworkError(Tr(LOST_CONNECTION), FSocket->errorString());
        }
      }
      qint64 Want = TransferChunk;
      if (Size >= 0)
      {
        Want = std::min(Want, Remaining);
      }
      if ((Target != nullptr) && (Progress != nullptr))
      {
        Want = static_cast<qint64>(Progress->AdjustToCPSLimit(static_cast<unsigned long>(Want)));
        Progress->CheckCancel();
      }
      QByteArray Data = FSocket->read(Want);
      if (Data.isEmpty())
      {
        continue;
      }
      if (Target != nullptr)
      {
        if (Target->write(Data) != Data.size())
        {
          throw ExtException(Tr(FILE_WRITE_ERROR).arg(Target->fileName()), Target->errorString());
        }
        if (Progress != nullptr)
        {
          Progress->AddTransferred(Data.size());
        }
      }
      else
      {
        if (Response.Body.size() + Data.size() > ResponseLimit)
        {
          throw NetworkError(Tr(LOST_CONNECTION), QStringLiteral("Response too large."));
        }
        Response.Body += Data;
      }
      Remaining -= Data.size();
    }
  };

  if (Chunked)
  {
    for (;;)
    {
      QByteArray Line = ReadLine(Progress);
      qsizetype Semicolon = Line.indexOf(';');
      bool Ok = false;
      qint64 Size = ((Semicolon >= 0) ? Line.left(Semicolon) : Line).trimmed().toLongLong(&Ok, 16);
      if (!Ok || (Size < 0))
      {
        throw NetworkError(Tr(LOST_CONNECTION), QStringLiteral("Invalid chunked transfer encoding."));
      }
      if (Size == 0)
      {
        // trailers
        while (!ReadLine(Progress).isEmpty())
        {
        }
        break;
      }
      ReadBodyData(Size);
      if (!ReadLine(Progress).isEmpty())
      {
        throw NetworkError(Tr(LOST_CONNECTION), QStringLiteral("Invalid chunked transfer encoding."));
      }
    }
  }
  else
  {
    ReadBodyData(Length);
  }
}

S3FileSystem::HttpResponse S3FileSystem::DoExecute(
  HttpRequest & Request, FileOperationProgress * Progress, bool & Reused, bool & AnyResponse, qint64 & Uploaded)
{
  quint16 Port = (Request.Port != 0) ? Request.Port : FPort;
  Reused = EnsureConnection(Request.Host, Port, Request.Tls);

  bool HasBody = (Request.File != nullptr) || !Request.Data.isEmpty() || (Request.Method == "PUT") || (Request.Method == "POST");
  qint64 BodyLength = (Request.File != nullptr) ? Request.FileLength : Request.Data.size();
  QByteArray HostHeader;
  if (QHostAddress(Request.Host).protocol() == QAbstractSocket::IPv6Protocol)
  {
    HostHeader = '[' + Request.Host.toLatin1() + ']';
  }
  else
  {
    HostHeader = QUrl::toAce(Request.Host);
    if (HostHeader.isEmpty())
    {
      HostHeader = Request.Host.toUtf8();
    }
  }
  if (Port != static_cast<quint16>(Request.Tls ? HTTPSPortNumber : HTTPPortNumber))
  {
    HostHeader += ':' + QByteArray::number(Port);
  }

  // libs3 compose_amz_headers + compose_auth_header
  S3SignatureV4 Signature;
  Signature.Method = Request.Method;
  Signature.CanonicalUri = S3SignatureV4::UriEncode(Request.Path.toUtf8(), false);
  Signature.Query = Request.Query;
  Signature.PayloadHash = (Request.File != nullptr) ? QByteArray(UnsignedPayload) : S3SignatureV4::Sha256Hex(Request.Data);
  Signature.Time = QDateTime::currentDateTimeUtc();
  Signature.Headers.append(qMakePair(QByteArray("host"), HostHeader));
  Signature.Headers.append(qMakePair(QByteArray("x-amz-content-sha256"), Signature.PayloadHash));
  Signature.Headers.append(qMakePair(QByteArray("x-amz-date"), S3SignatureV4::AmzDate(Signature.Time)));
  if (!FSessionToken.isEmpty())
  {
    Signature.Headers.append(qMakePair(QByteArray("x-amz-security-token"), FSessionToken));
  }
  if (FData.S3RequesterPays && (Request.Service == "s3"))
  {
    Signature.Headers.append(qMakePair(QByteArray("x-amz-request-payer"), QByteArray("requester")));
  }
  Signature.Headers += Request.Headers;
  Signature.AccessKeyId = FAccessKeyId;
  Signature.SecretAccessKey = FSecretAccessKey;
  Signature.Region = Request.Region.toUtf8();
  Signature.Service = Request.Service;
  Signature.Sign();

  QByteArray Target = Signature.CanonicalUri;
  if (!Signature.CanonicalQueryString.isEmpty())
  {
    Target += '?' + Signature.CanonicalQueryString;
  }
  QByteArray Head = Request.Method + ' ' + Target + " HTTP/1.1\r\n";
  for (const auto & Header : Signature.Headers)
  {
    Head += Header.first + ": " + Header.second + "\r\n";
  }
  Head += "Authorization: " + Signature.Authorization + "\r\n";
  if (HasBody)
  {
    Head += "Content-Length: " + QByteArray::number(BodyLength) + "\r\n";
  }
  bool ExpectContinue = (BodyLength >= ExpectContinueMinSize);
  if (ExpectContinue)
  {
    Head += "Expect: 100-continue\r\n";
  }
  Head += QByteArray("User-Agent: ") + UserAgent + "\r\n\r\n";
  LogLine(QStringLiteral("> %1 %2 (%3, region %4%5)").arg(QString::fromLatin1(Request.Method), QString::fromLatin1(Target),
    QString::fromLatin1(HostHeader), Request.Region, Reused ? QStringLiteral(", reused connection") : QString()));
  WriteAll(Head, Progress);

  HttpResponse Response;
  bool HaveResponse = false;
  bool CloseAfter = false;
  if (ExpectContinue)
  {
    // Wait for "100 Continue"; an early final response (error, redirect) means
    // that the body is not sent at all
    WaitResult Result = WaitForData(Progress, ContinueTimeoutMs, 0);
    if (Result == wrData)
    {
      ReadResponseHead(Response, Progress, AnyResponse);
      if ((Response.Status / 100) == 1)
      {
        // an interim response does not mean the request was processed
        AnyResponse = false;
      }
      else
      {
        HaveResponse = true;
        CloseAfter = true;
      }
    }
    else if (Result == wrEof)
    {
      throw NetworkError(Tr(LOST_CONNECTION), FSocket->errorString());
    }
  }
  if (!HaveResponse)
  {
    if (HasBody)
    {
      try
      {
        SendBody(Request, Progress, Uploaded);
      }
      catch (const NetworkError &)
      {
        // The server may have rejected the request and closed the
        // connection; its response tells why
        if (!FSocket || (FSocket->bytesAvailable() == 0))
        {
          throw;
        }
        CloseAfter = true;
      }
    }
    for (;;)
    {
      ReadResponseHead(Response, Progress, AnyResponse);
      if ((Response.Status / 100) != 1)
      {
        break;
      }
      AnyResponse = false;
    }
  }
  ReadResponseBody(Request, Response, Progress);
  if (Response.CloseConnection || CloseAfter)
  {
    CloseConnection();
  }
  return Response;
}

S3FileSystem::HttpResponse S3FileSystem::Execute(HttpRequest & Request, FileOperationProgress * Progress)
{
  for (int Attempt = 0; ; Attempt++)
  {
    bool Reused = false;
    bool AnyResponse = false;
    qint64 Uploaded = 0;
    try
    {
      return DoExecute(Request, Progress, Reused, AnyResponse, Uploaded);
    }
    catch (const NetworkError & E)
    {
      CloseConnection();
      if ((Progress != nullptr) && (Uploaded > 0))
      {
        Progress->AddTransferred(-Uploaded);
      }
      if (Reused && !AnyResponse && (Attempt == 0))
      {
        LogLine(QStringLiteral("Keep-alive connection was closed (%1), repeating request").arg(E.MoreMessages()));
        continue;
      }
      throw;
    }
    catch (...)
    {
      CloseConnection();
      if ((Progress != nullptr) && (Uploaded > 0))
      {
        Progress->AddTransferred(-Uploaded);
      }
      throw;
    }
  }
}

void S3FileSystem::ThrowResponseError(const HttpResponse & Response, const QString & Message)
{
  // TS3FileSystem::LibS3ResponseCompleteCallback + CheckLibS3Error
  S3ErrorResult Error;
  ParseError(Response.Body, Error);
  QStringList ErrorDetails;
  if (!Error.Resource.isEmpty())
  {
    ErrorDetails << Tr(S3_ERROR_RESOURCE).arg(Error.Resource);
  }
  QStringList Extra;
  for (const auto & Detail : Error.ExtraDetails)
  {
    if (Detail.first == QStringLiteral("FurtherDetails"))
    {
      ErrorDetails << Tr(S3_ERROR_FURTHER_DETAILS).arg(Detail.second);
    }
    else
    {
      Extra << QStringLiteral("%1: %2").arg(Detail.first, Detail.second);
    }
  }
  if (!Extra.isEmpty())
  {
    ErrorDetails << Tr(S3_ERROR_EXTRA_DETAILS) + Extra.join(QStringLiteral(", "));
  }
  QString ErrorText;
  QString DetailsText;
  if (Error.Code == QStringLiteral("AccessDenied"))
  {
    ErrorText = Tr(S3_STATUS_ACCESS_DENIED);
  }
  // While it can mean an implementation fault, it will typically mean a wrong secure key
  else if ((Error.Code == QStringLiteral("SignatureDoesNotMatch")) || (Error.Code == QStringLiteral("InvalidAccessKeyId")))
  {
    ErrorText = Tr(AUTHENTICATION_FAILED);
  }
  if (!ErrorText.isEmpty())
  {
    DetailsText = JoinMessages(Error.Message, ErrorDetails.join(QLatin1Char('\n')));
  }
  else
  {
    if (!Error.Message.isEmpty())
    {
      ErrorText = Error.Message;
    }
    else if (!Error.Code.isEmpty())
    {
      ErrorText = Error.Code;
    }
    else if (Response.Status == 403)
    {
      ErrorText = Tr(S3_STATUS_ACCESS_DENIED);
    }
    else
    {
      ErrorText = QStringLiteral("HTTP error %1 %2").arg(Response.Status).arg(QString::fromLatin1(Response.Reason)).trimmed();
    }
    DetailsText = ErrorDetails.join(QLatin1Char('\n'));
  }
  LogLine(QStringLiteral("Error: %1 (%2)").arg(ErrorText, Error.Code));
  if (Message.isEmpty())
  {
    throw ExtException(ErrorText, DetailsText);
  }
  throw ExtException(Message, JoinMessages(ErrorText, DetailsText));
}

void S3FileSystem::CheckResponse(const HttpResponse & Response, const QString & Message)
{
  if (!Response.Ok() || ((Response.Status == 200) && IsXmlError(Response.Body)))
  {
    ThrowResponseError(Response, Message);
  }
}

//---------------------------------------------------------------------------
// Session
//---------------------------------------------------------------------------

void S3FileSystem::SetCredentials(const QString & AccessKeyId, const QString & SecretAccessKey, const QString & SessionToken)
{
  FAccessKeyId = AccessKeyId.trimmed().toUtf8();
  FSecretAccessKey = SecretAccessKey.toUtf8();
  FSessionToken = SessionToken.trimmed().toUtf8();
}

void S3FileSystem::Open()
{
  // TS3FileSystem::Open
  CloseConnection();
  FActive = false;
  FRegions.clear();
  FHostNames.clear();
  FAuthRegion = FData.S3DefaultRegion.isEmpty() ? QString::fromLatin1(DefaultRegion) : FData.S3DefaultRegion;
  FTls = (FData.Ftps != ftpsNone);
  FHostName = FData.HostName.trimmed();
  if (FHostName.isEmpty())
  {
    FHostName = QString::fromLatin1(DefaultHostName);
  }
  int DefaultPort = FTls ? HTTPSPortNumber : HTTPPortNumber;
  FPort = static_cast<quint16>(((FData.PortNumber > 0) && (FData.PortNumber <= 65535)) ? FData.PortNumber : DefaultPort);

  QString Profile;
  if (FData.S3CredentialsEnv)
  {
    Profile = FData.S3Profile;
  }
  if (!Profile.isEmpty() && !Profiles().contains(Profile, Qt::CaseInsensitive))
  {
    throw FatalException(Tr(S3_PROFILE_NOT_EXIST).arg(Profile));
  }

  QString AccessKeyId = FData.UserName;
  if (AccessKeyId.isEmpty() && FData.S3CredentialsEnv)
  {
    QString Source;
    AccessKeyId = EnvUserName(Profile, &Source);
    if (!AccessKeyId.isEmpty())
    {
      LogLine(QStringLiteral("Username (access key ID) read from %1").arg(Source));
    }
  }
  if (AccessKeyId.isEmpty())
  {
    QList<Prompt> Prompts{Prompt{Tr(S3_ACCESS_KEY_ID_PROMPT), true, QString()}};
    if (!FUI->PromptUser(UserInterface::pkUserName, FData.SessionName(), QString(), Prompts) || Prompts[0].Result.isEmpty())
    {
      throw FatalException(Tr(CREDENTIALS_NOT_SPECIFIED));
    }
    AccessKeyId = Prompts[0].Result;
  }

  QString Password = FData.Password;
  if (Password.isEmpty() && FData.S3CredentialsEnv)
  {
    QString Source;
    Password = EnvPassword(Profile, &Source);
    if (!Password.isEmpty())
    {
      LogLine(QStringLiteral("Password (secret access key) read from %1").arg(Source));
    }
  }
  if (Password.isEmpty())
  {
    QList<Prompt> Prompts{Prompt{Tr(S3_SECRET_ACCESS_KEY_PROMPT), false, QString()}};
    if (!FUI->PromptUser(UserInterface::pkPassword, FData.SessionName(), QString(), Prompts) || Prompts[0].Result.isEmpty())
    {
      throw FatalException(Tr(CREDENTIALS_NOT_SPECIFIED));
    }
    Password = Prompts[0].Result;
  }

  QString SessionToken = FData.S3SessionToken;
  if (SessionToken.isEmpty() && FData.S3CredentialsEnv)
  {
    QString Source;
    SessionToken = EnvSessionToken(Profile, &Source);
    if (!SessionToken.isEmpty())
    {
      LogLine(QStringLiteral("Session token read from %1").arg(Source));
    }
  }

  SetCredentials(AccessKeyId, Password, SessionToken);

  try
  {
    if (IsAmazon())
    {
      QString RoleArn = FData.S3RoleArn;
      if (RoleArn.isEmpty())
      {
        QString Source;
        RoleArn = EnvRoleArn(Profile, &Source);
        if (!RoleArn.isEmpty())
        {
          LogLine(QStringLiteral("Role ARN read from %1").arg(Source));
        }
      }
      if (!RoleArn.isEmpty())
      {
        AssumeRole(RoleArn);
      }
    }

    // TryOpenDirectory: the first request verifies the credentials
    QString Path = CanonicalizePath(FData.RemoteDirectory);
    LogLine(QStringLiteral("Trying to open directory \"%1\".").arg(Path));
    if (Path == QStringLiteral("/"))
    {
      ListBuckets();
    }
    else
    {
      bool Exists = false;
      ListPrefix(Path, 1, &Exists);
      if (!Exists)
      {
        throw ExtException(Tr(FILE_NOT_EXISTS).arg(Path));
      }
    }
  }
  catch (const FatalException &)
  {
    CloseConnection();
    throw;
  }
  catch (const ExtException & E)
  {
    CloseConnection();
    throw FatalException(Tr(CONNECTION_FAILED), Details(E));
  }
  FActive = true;
}

void S3FileSystem::Close()
{
  CloseConnection();
  FActive = false;
}

void S3FileSystem::AssumeRole(const QString & RoleArn)
{
  // TS3FileSystem::AssumeRole; according to AWS cli, AWS_ROLE_SESSION_NAME does not apply here
  QString RoleSessionName = FData.S3RoleSessionName.isEmpty() ? QStringLiteral("NixSCP") : FData.S3RoleSessionName;
  try
  {
    HttpRequest Request;
    Request.Method = "GET";
    QString StsHostName = QString::fromLatin1(DefaultHostName);
    StsHostName.replace(0, 3, QStringLiteral("sts."));
    Request.Host = StsHostName;
    Request.Port = static_cast<quint16>(HTTPSPortNumber);
    Request.Tls = true;
    Request.Query.append(qMakePair(QByteArray("Version"), QByteArray(AssumeRoleVersion)));
    Request.Query.append(qMakePair(QByteArray("Action"), QByteArray("AssumeRole")));
    Request.Query.append(qMakePair(QByteArray("RoleSessionName"), RoleSessionName.toUtf8()));
    Request.Query.append(qMakePair(QByteArray("RoleArn"), RoleArn.toUtf8()));
    Request.Region = FAuthRegion;
    Request.Service = "sts";
    HttpResponse Response = Execute(Request);
    CheckResponse(Response);

    XmlNode Root;
    QString Error;
    if (!ParseXml(Response.Body, Root, &Error))
    {
      throw ExtException(Tr(S3_RESPONSE_ERROR).arg(QStringLiteral("AssumeRoleResponse")), Error);
    }
    auto NeedNode = [](const XmlNode * Parent, const char * Name) -> const XmlNode *
    {
      const XmlNode * Result = (Parent != nullptr) ? Parent->Child(QString::fromLatin1(Name)) : nullptr;
      if (Result == nullptr)
      {
        throw ExtException(Tr(S3_RESPONSE_ERROR).arg(QString::fromLatin1(Name)));
      }
      return Result;
    };
    if (Root.Name != QStringLiteral("AssumeRoleResponse"))
    {
      throw ExtException(Tr(S3_RESPONSE_ERROR).arg(QStringLiteral("AssumeRoleResponse")));
    }
    const XmlNode * Credentials = NeedNode(NeedNode(&Root, "AssumeRoleResult"), "Credentials");
    QString AccessKeyId = NeedNode(Credentials, "AccessKeyId")->Text;
    QString SecretAccessKey = NeedNode(Credentials, "SecretAccessKey")->Text;
    QString SessionToken = NeedNode(Credentials, "SessionToken")->Text;
    QString Expiration = NeedNode(Credentials, "Expiration")->Text;
    LogLine(QStringLiteral("Assumed role \"%1\", new access key is %2, credentials expire %3").arg(RoleArn, AccessKeyId, Expiration));
    SetCredentials(AccessKeyId, SecretAccessKey, SessionToken);
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
    throw ExtException(Tr(S3_ASSUME_ROLE_ERROR).arg(RoleArn), Details(E));
  }
}

void S3FileSystem::ClearCaches()
{
  FRegions.clear();
  FHostNames.clear();
}

//---------------------------------------------------------------------------
// Buckets
//---------------------------------------------------------------------------

S3FileSystem::BucketContext S3FileSystem::MakeBucketContext(const QString & BucketName, const QString & Region) const
{
  BucketContext Result;
  Result.BucketName = BucketName;
  Result.Region = Region;
  bool PathStyle = (FData.S3UrlStyle == s3usPath);
  QString HostName = FHostName;
  auto I = FHostNames.constFind(BucketName);
  if (I != FHostNames.constEnd())
  {
    HostName = I.value();
    QString BucketPrefix = BucketName + QLatin1Char('.');
    if (HostName.startsWith(BucketPrefix, Qt::CaseInsensitive))
    {
      HostName.remove(0, BucketPrefix.length());
      // Even when using path-style URL Amazon seems to redirect us to bucket hostname and
      // we need to switch to virtual host style URL (without bucket name in the path)
      PathStyle = false;
    }
  }
  if (!PathStyle && !IsVirtualHostCompatible(BucketName, HostName, FTls))
  {
    PathStyle = true;
  }
  if (PathStyle)
  {
    Result.Host = HostName;
    Result.PathPrefix = QLatin1Char('/') + BucketName;
  }
  else
  {
    Result.Host = BucketName + QLatin1Char('.') + HostName;
  }
  return Result;
}

S3FileSystem::BucketContext S3FileSystem::GetBucketContext(const QString & BucketName, const QString & Prefix)
{
  // TS3FileSystem::GetBucketContext
  auto I = FRegions.constFind(BucketName);
  if (I != FRegions.constEnd())
  {
    return MakeBucketContext(BucketName, I.value());
  }
  LogLine(QStringLiteral("Unknown bucket \"%1\", will detect its region (and service endpoint)").arg(BucketName));
  QString Region = FAuthRegion;
  for (int Attempt = 1; ; Attempt++)
  {
    BucketContext Context = MakeBucketContext(BucketName, Region);
    if (Attempt > MaxBucketDetectAttempts)
    {
      return Context;
    }
    // Using prefix for which we need the bucket, as the account may have access to that prefix only (using "Condition" in policy)
    HttpResponse Response = ListBucketRequest(Context, Prefix, 1, QString(), QString(), false);
    if (Response.Ok())
    {
      LogLine(QStringLiteral("Will use region \"%1\" for bucket \"%2\" from now on.").arg(Region, BucketName));
      FRegions.insert(BucketName, Region);
      return Context;
    }
    S3ErrorResult Error;
    ParseError(Response.Body, Error);
    QString HeaderRegion = QString::fromLatin1(Response.Header("x-amz-bucket-region")).trimmed();
    bool Redirect = (Response.Status == 301) || (Response.Status == 302) || (Response.Status == 307);
    bool Retry = false;
    if ((Error.Code == QStringLiteral("AuthorizationHeaderMalformed")) && !Error.Region.isEmpty() && (Error.Region != Region))
    {
      LogLine(QStringLiteral("Bucket \"%1\" is in region \"%2\".").arg(BucketName, Error.Region));
      Region = Error.Region;
      Retry = true;
    }
    // happens with newly created buckets (and happens before the region redirect)
    else if (Redirect)
    {
      if (!HeaderRegion.isEmpty() && (HeaderRegion != Region))
      {
        Region = HeaderRegion;
        Retry = true;
      }
      QString Endpoint = Error.Endpoint;
      if (Endpoint.isEmpty() && !HeaderRegion.isEmpty() && FHostName.endsWith(QStringLiteral(".amazonaws.com"), Qt::CaseInsensitive))
      {
        Endpoint = QStringLiteral("s3.%1.amazonaws.com").arg(HeaderRegion);
      }
      if (!Endpoint.isEmpty() && (Endpoint != FHostNames.value(BucketName, FHostName)))
      {
        LogLine(QStringLiteral("Will use endpoint \"%1\" for bucket \"%2\" from now on.").arg(Endpoint, BucketName));
        FHostNames.insert(BucketName, Endpoint);
        Retry = true;
      }
    }
    else if (!HeaderRegion.isEmpty() && (HeaderRegion != Region))
    {
      Region = HeaderRegion;
      Retry = true;
    }
    if (!Retry)
    {
      // The error itself is reported by the actual request
      if (Region != FAuthRegion)
      {
        FRegions.insert(BucketName, Region);
      }
      return Context;
    }
  }
}

S3FileSystem::HttpRequest S3FileSystem::ServiceRequest(const QByteArray & Method) const
{
  HttpRequest Result;
  Result.Method = Method;
  Result.Host = FHostName;
  Result.Port = FPort;
  Result.Tls = FTls;
  Result.Path = QStringLiteral("/");
  Result.Region = FAuthRegion;
  return Result;
}

S3FileSystem::HttpRequest S3FileSystem::BucketRequest(const BucketContext & Context, const QByteArray & Method, const QString & Key) const
{
  HttpRequest Result;
  Result.Method = Method;
  Result.Host = Context.Host;
  Result.Port = FPort;
  Result.Tls = FTls;
  Result.Path = Context.PathPrefix + QLatin1Char('/') + Key;
  Result.Region = Context.Region;
  return Result;
}

S3FileSystem::HttpResponse S3FileSystem::ListBucketRequest(const BucketContext & Context, const QString & Prefix, int MaxKeys,
  const QString & ContinuationToken, const QString & Marker, bool V1)
{
  // TS3FileSystem::DoListBucket (ListObjectsV2 instead of libs3's ListObjects)
  HttpRequest Request = BucketRequest(Context, "GET", QString());
  if (!V1)
  {
    Request.Query.append(qMakePair(QByteArray("list-type"), QByteArray("2")));
    Request.Query.append(qMakePair(QByteArray("fetch-owner"), QByteArray("true")));
  }
  Request.Query.append(qMakePair(QByteArray("delimiter"), QByteArray("/")));
  Request.Query.append(qMakePair(QByteArray("encoding-type"), QByteArray("url")));
  if (!Prefix.isEmpty())
  {
    Request.Query.append(qMakePair(QByteArray("prefix"), Prefix.toUtf8()));
  }
  int Keys = EffectiveMaxKeys(MaxKeys);
  if (Keys > 0)
  {
    Request.Query.append(qMakePair(QByteArray("max-keys"), QByteArray::number(Keys)));
  }
  if (!ContinuationToken.isEmpty())
  {
    Request.Query.append(qMakePair(QByteArray("continuation-token"), ContinuationToken.toUtf8()));
  }
  if (!Marker.isEmpty())
  {
    Request.Query.append(qMakePair(QByteArray("marker"), Marker.toUtf8()));
  }
  return Execute(Request);
}

void S3FileSystem::HandleNonBucketStatus(const HttpResponse & Response, bool & Retry)
{
  // TS3FileSystem::HandleNonBucketStatus
  S3ErrorResult Error;
  if (!Response.Ok() && ParseError(Response.Body, Error) &&
      (Error.Code == QStringLiteral("AuthorizationHeaderMalformed")) && !Error.Region.isEmpty() && (Error.Region != FAuthRegion))
  {
    LogLine(QStringLiteral("Will use authentication region \"%1\" from now on.").arg(Error.Region));
    FAuthRegion = Error.Region;
    Retry = true;
  }
}

S3ListAllMyBucketsResult S3FileSystem::ListBuckets()
{
  // TS3FileSystem::ReadDirectoryInternal for the root (S3_list_service)
  S3ListAllMyBucketsResult Result;
  QString Token;
  for (int Page = 0; ; Page++)
  {
    HttpResponse Response;
    bool Retry;
    int Attempts = 0;
    do
    {
      HttpRequest Request = ServiceRequest("GET");
      if (!Token.isEmpty())
      {
        Request.Query.append(qMakePair(QByteArray("continuation-token"), Token.toUtf8()));
      }
      Response = Execute(Request);
      Retry = false;
      HandleNonBucketStatus(Response, Retry);
    }
    while (Retry && (++Attempts < MaxBucketDetectAttempts));
    CheckResponse(Response);
    S3ListAllMyBucketsResult List;
    QString Error;
    if (!ParseListAllMyBucketsResult(Response.Body, List, &Error))
    {
      throw ExtException(Tr(S3_RESPONSE_ERROR).arg(QStringLiteral("ListBuckets")), Error);
    }
    if (Page == 0)
    {
      Result.OwnerId = List.OwnerId;
      Result.OwnerDisplayName = List.OwnerDisplayName;
    }
    Result.Buckets += List.Buckets;
    if (List.ContinuationToken.isEmpty() || (List.ContinuationToken == Token))
    {
      break;
    }
    Token = List.ContinuationToken;
  }
  return Result;
}

RemoteFileList S3FileSystem::ListPrefix(const QString & Path, int MaxKeys, bool * Exists)
{
  // TS3FileSystem::ReadDirectoryInternal for buckets and folders; MaxKeys
  // > 0 lists the first page only (existence check)
  QString BucketName;
  QString Key;
  ParsePath(Path, BucketName, Key);
  QString Prefix = Key.isEmpty() ? QString() : (Key + QLatin1Char('/'));
  BucketContext Context = GetBucketContext(BucketName, Prefix);

  RemoteFileList Result;
  bool Any = false;
  QString Token;
  QString Marker;
  bool V1 = false;
  for (;;)
  {
    HttpResponse Response = ListBucketRequest(Context, Prefix, MaxKeys, Token, Marker, V1);
    CheckResponse(Response);
    S3ListBucketResult List;
    QString Error;
    if (!ParseListBucketResult(Response.Body, List, &Error))
    {
      throw ExtException(Tr(S3_RESPONSE_ERROR).arg(QStringLiteral("ListObjects")), Error);
    }
    QString LastKey;
    for (const S3Object & Object : List.Contents)
    {
      Any = true;
      LastKey = std::max(LastKey, Object.Key);
      // "prefix/" itself (folder object) has an empty name
      QString Name = ExtractName(Object.Key);
      if (IsValidListingName(Name))
      {
        Result.append(FileFromObject(Object, Name));
      }
    }
    for (const QString & CommonPrefix : List.CommonPrefixes)
    {
      Any = true;
      LastKey = std::max(LastKey, CommonPrefix);
      // Have seen prefixes like "/" or "path/subpath//"
      QString Name = ExtractName(ExcludeTrailingSlash(CommonPrefix));
      if (IsValidListingName(Name))
      {
        Result.append(DirectoryEntry(Name));
      }
    }
    if (!List.IsTruncated || (MaxKeys > 0))
    {
      break;
    }
    if (!V1 && !List.NextContinuationToken.isEmpty())
    {
      Token = List.NextContinuationToken;
    }
    else
    {
      // The server does not know ListObjectsV2 (list-type ignored): continue
      // with ListObjects markers
      QString NextMarker = !List.NextMarker.isEmpty() ? List.NextMarker : LastKey;
      if (NextMarker.isEmpty() || (NextMarker == Marker))
      {
        break;
      }
      V1 = true;
      Token.clear();
      Marker = NextMarker;
    }
  }

  // Listing bucket root directory will report an error if the bucket does not exist.
  // But when listing a prefix, we do not get any error, when the "prefix" does not exist.
  // But when the prefix does exist, there's at least the prefix/ entry. If there's none, it means that the path does not exist.
  // Even an empty-named entry/subprefix (which are ignored for other purposes) still indicate that the prefix exists.
  if (Exists != nullptr)
  {
    *Exists = Prefix.isEmpty() || Any;
  }
  return Result;
}

RemoteFile S3FileSystem::DirectoryEntry(const QString & Name)
{
  RemoteFile File;
  File.FileName = Name;
  File.FileType = RemoteFile::Directory;
  File.IsDirectory = true;
  File.IsHidden = Name.startsWith(QLatin1Char('.'));
  return File;
}

QString S3FileSystem::OwnerName(const QString & Id, const QString & DisplayName)
{
  return OwnerToken(Id, DisplayName);
}

//---------------------------------------------------------------------------
// FileSystem
//---------------------------------------------------------------------------

QString S3FileSystem::CanonicalizePath(const QString & Path)
{
  // TS3FileSystem::AbsolutePath: no server side canonicalization
  return NormalizePath(Path);
}

QString S3FileSystem::HomeDirectory()
{
  return QStringLiteral("/");
}

RemoteFileList S3FileSystem::ReadDirectory(const QString & APath)
{
  QString Path = CanonicalizePath(APath);
  try
  {
    if (Path == QStringLiteral("/"))
    {
      S3ListAllMyBucketsResult Buckets = ListBuckets();
      RemoteFileList Result;
      QString Owner = OwnerName(Buckets.OwnerId, Buckets.OwnerDisplayName);
      for (const S3Bucket & Bucket : Buckets.Buckets)
      {
        RemoteFile File = DirectoryEntry(Bucket.Name);
        File.Owner = Owner;
        Result.append(File);
      }
      return Result;
    }
    bool Exists = false;
    RemoteFileList Result = ListPrefix(Path, 0, &Exists);
    if (!Exists)
    {
      throw ExtException(Tr(FILE_NOT_EXISTS).arg(Path));
    }
    return Result;
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
    throw ExtException(Tr(LIST_DIR_ERROR).arg(Path), Details(E));
  }
}

bool S3FileSystem::ReadFileInternal(const QString & APath, RemoteFile & File)
{
  // TS3FileSystem::DoReadFile
  QString Path = CanonicalizePath(APath);
  if (Path == QStringLiteral("/"))
  {
    File = DirectoryEntry(Path);
    return true;
  }
  QString BucketName;
  QString Key;
  ParsePath(Path, BucketName, Key);
  if (Key.isEmpty())
  {
    BucketContext Context = GetBucketContext(BucketName, QString());
    HttpResponse Response = ListBucketRequest(Context, QString(), 1, QString(), QString(), false);
    if (Response.Status == 404)
    {
      return false;
    }
    CheckResponse(Response);
    File = DirectoryEntry(BucketName);
    return true;
  }

  // The key itself sorts first among the keys with the prefix; the folder
  // ("key/" common prefix) can be preceded by keys like "key-1" or "key.txt"
  BucketContext Context = GetBucketContext(BucketName, Key);
  HttpResponse Response = ListBucketRequest(Context, Key, 2, QString(), QString(), false);
  if (Response.Status == 404)
  {
    return false;
  }
  CheckResponse(Response);
  S3ListBucketResult List;
  QString Error;
  if (!ParseListBucketResult(Response.Body, List, &Error))
  {
    throw ExtException(Tr(S3_RESPONSE_ERROR).arg(QStringLiteral("ListObjects")), Error);
  }
  QString Name = ExtractName(Key);
  QString FolderKey = Key + QLatin1Char('/');
  for (const S3Object & Object : List.Contents)
  {
    if (Object.Key == Key)
    {
      File = FileFromObject(Object, Name);
      return true;
    }
  }
  if (List.CommonPrefixes.contains(FolderKey))
  {
    File = DirectoryEntry(Name);
    return true;
  }
  if (List.IsTruncated)
  {
    Response = ListBucketRequest(Context, FolderKey, 1, QString(), QString(), false);
    CheckResponse(Response);
    if (!ParseListBucketResult(Response.Body, List, &Error))
    {
      throw ExtException(Tr(S3_RESPONSE_ERROR).arg(QStringLiteral("ListObjects")), Error);
    }
    if (!List.Contents.isEmpty() || !List.CommonPrefixes.isEmpty())
    {
      File = DirectoryEntry(Name);
      return true;
    }
  }
  return false;
}

RemoteFile S3FileSystem::ReadFile(const QString & Path)
{
  RemoteFile File;
  bool Found = false;
  try
  {
    Found = ReadFileInternal(Path, File);
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
    throw ExtException(Tr(CANT_GET_ATTRS).arg(Path), Details(E));
  }
  if (!Found)
  {
    throw ExtException(Tr(FILE_NOT_EXISTS).arg(Path));
  }
  return File;
}

bool S3FileSystem::FileExists(const QString & Path, RemoteFile * File)
{
  // As TS3FileSystem::Source: errors (like missing list permission) are
  // treated as a non-existing file
  RemoteFile AFile;
  bool Result = false;
  try
  {
    Result = ReadFileInternal(Path, AFile);
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
    LogLine(QStringLiteral("Error checking existence of \"%1\": %2").arg(Path, Details(E)));
    Result = false;
  }
  if (Result && (File != nullptr))
  {
    *File = AFile;
  }
  return Result;
}

void S3FileSystem::CreateDirectory(const QString & APath, int /*Mode*/)
{
  // TS3FileSystem::CreateDirectory
  QString Path = CanonicalizePath(APath);
  try
  {
    QString BucketName;
    QString Key;
    ParsePath(Path, BucketName, Key);
    if (BucketName.isEmpty())
    {
      throw ExtException(Tr(NOTSUPPORTED));
    }
    if (Key.isEmpty())
    {
      QByteArray Body;
      if (!FData.S3DefaultRegion.isEmpty() && (FData.S3DefaultRegion != QString::fromLatin1(DefaultRegion)))
      {
        Body =
          QByteArray("<CreateBucketConfiguration xmlns=\"") + S3XmlNamespace + "\"><LocationConstraint>" +
          XmlEscape(FData.S3DefaultRegion) + "</LocationConstraint></CreateBucketConfiguration>";
      }
      HttpResponse Response;
      bool Retry;
      int Attempts = 0;
      do
      {
        // Not using GetBucketContext here, as the bucket does not exist
        HttpRequest Request = BucketRequest(MakeBucketContext(BucketName, FAuthRegion), "PUT", QString());
        Request.Data = Body;
        if (!Body.isEmpty())
        {
          Request.Headers.append(qMakePair(QByteArray("content-type"), QByteArray("application/xml")));
        }
        Response = Execute(Request);
        Retry = false;
        HandleNonBucketStatus(Response, Retry);
      }
      while (Retry && (++Attempts < MaxBucketDetectAttempts));
      CheckResponse(Response);
      FRegions.remove(BucketName);
      FHostNames.remove(BucketName);
    }
    else
    {
      QString FolderKey = Key + QLatin1Char('/');
      BucketContext Context = GetBucketContext(BucketName, FolderKey);
      HttpRequest Request = BucketRequest(Context, "PUT", FolderKey);
      HttpResponse Response = Execute(Request);
      CheckResponse(Response);
    }
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
    throw ExtException(Tr(CREATE_DIR_ERROR).arg(Path), Details(E));
  }
}

void S3FileSystem::DeleteObject(const QString & BucketName, const QString & Key)
{
  BucketContext Context = GetBucketContext(BucketName, Key);
  HttpRequest Request = BucketRequest(Context, "DELETE", Key);
  HttpResponse Response = Execute(Request);
  CheckResponse(Response);
}

void S3FileSystem::DeleteFile(const QString & APath)
{
  // TS3FileSystem::DeleteFile (files)
  QString Path = CanonicalizePath(APath);
  QString BucketName;
  QString Key;
  ParsePath(Path, BucketName, Key);
  if (Key.isEmpty())
  {
    RemoveDirectory(Path);
    return;
  }
  try
  {
    DeleteObject(BucketName, Key);
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
    throw ExtException(Tr(DELETE_FILE_ERROR).arg(Path), Details(E));
  }
}

void S3FileSystem::RemoveDirectory(const QString & APath)
{
  // TS3FileSystem::DeleteFile (folders and buckets); the contents were deleted by the caller
  QString Path = CanonicalizePath(APath);
  try
  {
    QString BucketName;
    QString Key;
    ParsePath(Path, BucketName, Key);
    if (BucketName.isEmpty())
    {
      throw ExtException(Tr(NOTSUPPORTED));
    }
    if (Key.isEmpty())
    {
      BucketContext Context = GetBucketContext(BucketName, QString());
      HttpRequest Request = BucketRequest(Context, "DELETE", QString());
      HttpResponse Response = Execute(Request);
      CheckResponse(Response);
      FRegions.remove(BucketName);
      FHostNames.remove(BucketName);
    }
    else
    {
      try
      {
        DeleteObject(BucketName, Key + QLatin1Char('/'));
      }
      catch (const AbortException &)
      {
        throw;
      }
      catch (const FatalException &)
      {
        throw;
      }
      catch (const ExtException &)
      {
        if (!FileExists(Path, nullptr))
        {
          // Amazon silently ignores attempts to delete non existing folders,
          // But Google Cloud fails that.
          LogLine(QStringLiteral("Folder does not exist anymore, it was probably only virtual"));
        }
        else
        {
          throw;
        }
      }
    }
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
    throw ExtException(Tr(DELETE_FILE_ERROR).arg(Path), Details(E));
  }
}

void S3FileSystem::CopyObject(const QString & From, const QString & To, qint64 Size)
{
  // TS3FileSystem::CopyFile (S3_copy_object)
  QString SourceBucketName;
  QString SourceKey;
  ParsePath(From, SourceBucketName, SourceKey);
  QString DestBucketName;
  QString DestKey;
  ParsePath(To, DestBucketName, DestKey);
  if (DestKey.isEmpty())
  {
    throw ExtException(Tr(MISSING_TARGET_BUCKET));
  }
  if (SourceKey.isEmpty())
  {
    throw ExtException(Tr(NOTSUPPORTED));
  }
  QByteArray CopySource =
    '/' + S3SignatureV4::UriEncode(SourceBucketName.toUtf8(), true) + '/' + S3SignatureV4::UriEncode(SourceKey.toUtf8(), false);
  BucketContext Context = GetBucketContext(DestBucketName, DestKey);
  if (Size <= S3MaxCopySize)
  {
    HttpRequest Request = BucketRequest(Context, "PUT", DestKey);
    Request.Headers.append(qMakePair(QByteArray("x-amz-copy-source"), CopySource));
    HttpResponse Response = Execute(Request);
    CheckResponse(Response);
    return;
  }

  // Objects over 5 GB: UploadPartCopy; the content type is taken over from the source
  QByteArray ContentType;
  {
    BucketContext SourceContext = GetBucketContext(SourceBucketName, SourceKey);
    HttpRequest Request = BucketRequest(SourceContext, "HEAD", SourceKey);
    HttpResponse Response = Execute(Request);
    CheckResponse(Response);
    ContentType = Response.Header("content-type");
  }
  qint64 PartSize = std::max(S3CopyPartSize, (Size + S3MaxMultiPartChunks - 1) / S3MaxMultiPartChunks);
  int Parts = static_cast<int>((Size + PartSize - 1) / PartSize);
  QString UploadId = InitiateMultipart(Context, DestKey, ContentType);
  try
  {
    QList<QByteArray> ETags;
    for (int Part = 1; Part <= Parts; Part++)
    {
      qint64 Start = (Part - 1) * PartSize;
      qint64 End = std::min(Size, Start + PartSize) - 1;
      LogLine(QStringLiteral("Copying part %1 [%2-%3]").arg(Part).arg(Start).arg(End));
      HttpRequest Request = BucketRequest(Context, "PUT", DestKey);
      Request.Query.append(qMakePair(QByteArray("partNumber"), QByteArray::number(Part)));
      Request.Query.append(qMakePair(QByteArray("uploadId"), UploadId.toUtf8()));
      Request.Headers.append(qMakePair(QByteArray("x-amz-copy-source"), CopySource));
      Request.Headers.append(qMakePair(QByteArray("x-amz-copy-source-range"),
        "bytes=" + QByteArray::number(Start) + '-' + QByteArray::number(End)));
      HttpResponse Response = Execute(Request);
      CheckResponse(Response);
      XmlNode Root;
      QString Error;
      if (!ParseXml(Response.Body, Root, &Error) || Root.ChildText(u"ETag").isEmpty())
      {
        throw ExtException(Tr(S3_RESPONSE_ERROR).arg(QStringLiteral("CopyPartResult")), Error);
      }
      ETags.append(Root.ChildText(u"ETag").toUtf8());
    }
    CompleteMultipart(Context, DestKey, UploadId, ETags);
  }
  catch (...)
  {
    AbortMultipart(Context, DestKey, UploadId);
    throw;
  }
}

void S3FileSystem::CopyFile(const QString & AFrom, const QString & ATo)
{
  // TS3FileSystem::CopyFile
  QString From = CanonicalizePath(AFrom);
  QString To = CanonicalizePath(ATo);
  try
  {
    RemoteFile File = ReadFile(From);
    if (File.IsDirectory)
    {
      throw ExtException(Tr(DUPLICATE_FOLDER_NOT_SUPPORTED));
    }
    CopyObject(From, To, File.Size);
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
    throw ExtException(Tr(COPY_FILE_ERROR).arg(From, To), Details(E));
  }
}

void S3FileSystem::RenameFile(const QString & AFrom, const QString & ATo)
{
  // TS3FileSystem::RenameFile: copy + delete, folders are not supported
  QString From = CanonicalizePath(AFrom);
  QString To = CanonicalizePath(ATo);
  try
  {
    RemoteFile File = ReadFile(From);
    if (File.IsDirectory)
    {
      throw ExtException(Tr(NOTSUPPORTED));
    }
    if (From == To)
    {
      return;
    }
    CopyObject(From, To, File.Size);
    QString BucketName;
    QString Key;
    ParsePath(From, BucketName, Key);
    DeleteObject(BucketName, Key);
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
    throw ExtException(Tr(RENAME_FILE_ERROR).arg(From, To), Details(E));
  }
}

void S3FileSystem::ChangeMode(const QString & Path, unsigned int /*Mode*/)
{
  // fcModeChanging is not supported (ACLs are not mapped to permissions)
  throw ExtException(Tr(CANT_SET_ATTRS).arg(Path), Tr(NOTSUPPORTED));
}

void S3FileSystem::CheckDirectory(const QString & Path)
{
  // TS3FileSystem::ChangeDirectory: to verify existence of directory try to open it
  if (!UnixPath::IsRoot(Path) && !ReadFile(Path).IsDirectory)
  {
    throw ExtException(QStringLiteral("Error changing directory to '%1'.").arg(Path));
  }
}

void S3FileSystem::SetModificationTime(const QString & /*Path*/, const QDateTime & /*Time*/)
{
  // fcPreservingTimestampUpload is not supported: WinSCP does not even try
  // (TTerminal checks the capability); the caller here always calls, so
  // this is silently ignored
}

void S3FileSystem::CreateLink(const QString & Path, const QString & /*Target*/)
{
  throw ExtException(Tr(CREATE_LINK_ERROR).arg(Path), Tr(NOTSUPPORTED));
}

//---------------------------------------------------------------------------
// Transfers
//---------------------------------------------------------------------------

QString S3FileSystem::InitiateMultipart(const BucketContext & Context, const QString & Key, const QByteArray & ContentType)
{
  HttpRequest Request = BucketRequest(Context, "POST", Key);
  Request.Query.append(qMakePair(QByteArray("uploads"), QByteArray()));
  if (!ContentType.isEmpty())
  {
    Request.Headers.append(qMakePair(QByteArray("content-type"), ContentType));
  }
  HttpResponse Response = Execute(Request);
  CheckResponse(Response);
  XmlNode Root;
  QString Error;
  QString UploadId;
  if (ParseXml(Response.Body, Root, &Error))
  {
    UploadId = Root.ChildText(u"UploadId").trimmed();
  }
  if (UploadId.isEmpty())
  {
    throw ExtException(Tr(S3_RESPONSE_ERROR).arg(QStringLiteral("InitiateMultipartUploadResult")), Error);
  }
  LogLine(QStringLiteral("Initiated multipart upload (%1)").arg(UploadId));
  return UploadId;
}

void S3FileSystem::CompleteMultipart(const BucketContext & Context, const QString & Key, const QString & UploadId, const QList<QByteArray> & ETags)
{
  QByteArray Body = QByteArray("<CompleteMultipartUpload xmlns=\"") + S3XmlNamespace + "\">\n";
  for (int Index = 0; Index < ETags.size(); Index++)
  {
    Body += "  <Part><PartNumber>" + QByteArray::number(Index + 1) + "</PartNumber><ETag>" +
      XmlEscape(QString::fromUtf8(ETags[Index])) + "</ETag></Part>\n";
  }
  Body += "</CompleteMultipartUpload>\n";
  LogLine(QStringLiteral("Committing multipart upload (%1 - %2 parts)").arg(UploadId).arg(ETags.size()));
  HttpRequest Request = BucketRequest(Context, "POST", Key);
  Request.Query.append(qMakePair(QByteArray("uploadId"), UploadId.toUtf8()));
  Request.Headers.append(qMakePair(QByteArray("content-type"), QByteArray("application/xml")));
  Request.Data = Body;
  HttpResponse Response = Execute(Request);
  CheckResponse(Response);
}

void S3FileSystem::AbortMultipart(const BucketContext & Context, const QString & Key, const QString & UploadId)
{
  LogLine(QStringLiteral("Aborting multipart upload (%1)").arg(UploadId));
  try
  {
    HttpRequest Request = BucketRequest(Context, "DELETE", Key);
    Request.Query.append(qMakePair(QByteArray("uploadId"), UploadId.toUtf8()));
    Execute(Request);
  }
  catch (...)
  {
    // swallow
  }
}

void S3FileSystem::Upload(const QString & LocalPath, const QString & RemotePath, int Mode, FileOperationProgress & Progress)
{
  TransferParams Params;
  Upload(LocalPath, RemotePath, Mode, Params, Progress);
}

void S3FileSystem::Upload(const QString & LocalPath, const QString & RemotePath, int /*Mode*/, const TransferParams & Params, FileOperationProgress & Progress)
{
  // TS3FileSystem::Source
  QString Path = CanonicalizePath(RemotePath);
  QFile Local(LocalPath);
  try
  {
    if (!Local.open(QIODevice::ReadOnly))
    {
      throw ExtException(Tr(FILE_OPEN_ERROR).arg(LocalPath), Local.errorString());
    }
    if (Params.ResumeOffset > 0)
    {
      // S3 objects cannot be appended to, the whole file is uploaded again
      Progress.AddTransferred(-Params.ResumeOffset);
    }
    qint64 LocalPos = 0;
    qint64 LocalEnd = Local.size();
    // Text mode conversions (TFileBuffer::Convert with cpRemoveBOM / cpRemoveCtrlZ)
    if (Params.RemoveBOM && (Local.peek(3) == QByteArray("\xEF\xBB\xBF")))
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

    QString BucketName;
    QString Key;
    ParsePath(Path, BucketName, Key);
    if (Key.isEmpty())
    {
      throw ExtException(Tr(MISSING_TARGET_BUCKET));
    }
    BucketContext Context = GetBucketContext(BucketName, Key);

    QByteArray ContentType;
    QString ConfiguredType;
    if (ConfiguredMimeType(RemotePath, ConfiguredType))
    {
      ContentType = ConfiguredType.toUtf8();
    }
    else
    {
      QMimeType MimeType = QMimeDatabase().mimeTypeForFile(LocalPath, QMimeDatabase::MatchExtension);
      if (MimeType.isValid() && !MimeType.isDefault())
      {
        ContentType = MimeType.name().toUtf8();
      }
    }

    qint64 Size = LocalEnd - LocalPos;
    if (Size <= FMultipartThreshold)
    {
      HttpRequest Request = BucketRequest(Context, "PUT", Key);
      if (!ContentType.isEmpty())
      {
        Request.Headers.append(qMakePair(QByteArray("content-type"), ContentType));
      }
      Request.File = &Local;
      Request.FileOffset = LocalPos;
      Request.FileLength = Size;
      HttpResponse Response = Execute(Request, &Progress);
      CheckResponse(Response);
      return;
    }

    int Parts = static_cast<int>(std::min<qint64>(S3MaxMultiPartChunks, std::max<qint64>(1, (Size + FMultipartChunkSize - 1) / FMultipartChunkSize)));
    qint64 ChunkSize = std::max(FMultipartChunkSize, (Size + Parts - 1) / Parts);
    LogLine(QStringLiteral("Initiating multipart upload (%1 parts - chunk size %2)").arg(Parts).arg(ChunkSize));
    QString UploadId = InitiateMultipart(Context, Key, ContentType);
    try
    {
      QList<QByteArray> ETags;
      qint64 Position = LocalPos;
      for (int Part = 1; Position < LocalEnd; Part++)
      {
        qint64 PartLength = std::min(ChunkSize, LocalEnd - Position);
        LogLine(QStringLiteral("Uploading part %1 [%2]").arg(Part).arg(PartLength));
        HttpResponse Response;
        for (int Attempt = 1; ; Attempt++)
        {
          HttpRequest Request = BucketRequest(Context, "PUT", Key);
          Request.Query.append(qMakePair(QByteArray("partNumber"), QByteArray::number(Part)));
          Request.Query.append(qMakePair(QByteArray("uploadId"), UploadId.toUtf8()));
          Request.File = &Local;
          Request.FileOffset = Position;
          Request.FileLength = PartLength;
          try
          {
            Response = Execute(Request, &Progress);
            break;
          }
          catch (const NetworkError & E)
          {
            if (Attempt >= MaxPartAttempts)
            {
              throw;
            }
            LogLine(QStringLiteral("Uploading part %1 failed (%2), retrying").arg(Part).arg(E.MoreMessages()));
          }
        }
        CheckResponse(Response);
        QByteArray ETag = Response.Header("etag");
        if (ETag.isEmpty())
        {
          throw ExtException(Tr(S3_RESPONSE_ERROR).arg(QStringLiteral("ETag")));
        }
        ETags.append(ETag);
        Position += PartLength;
      }
      CompleteMultipart(Context, Key, UploadId, ETags);
    }
    catch (...)
    {
      AbortMultipart(Context, Key, UploadId);
      throw;
    }
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
    throw ExtException(Tr(TRANSFER_ERROR).arg(LocalPath), Details(E));
  }
}

void S3FileSystem::Download(const QString & RemotePath, const QString & LocalPath, FileOperationProgress & Progress)
{
  // Plain download; partial file cleanup is up to the caller
  TransferParams Params;
  Params.KeepPartial = true;
  Download(RemotePath, LocalPath, Params, Progress);
}

void S3FileSystem::Download(const QString & RemotePath, const QString & LocalPath, const TransferParams & Params, FileOperationProgress & Progress)
{
  // TS3FileSystem::Sink, resuming with a Range request
  QString Path = CanonicalizePath(RemotePath);
  qint64 Offset = std::max<qint64>(Params.ResumeOffset, 0);
  QFile Local(LocalPath);
  bool LocalOpened = false;
  try
  {
    QString BucketName;
    QString Key;
    ParsePath(Path, BucketName, Key);
    if (Key.isEmpty())
    {
      throw ExtException(Tr(NOTSUPPORTED));
    }
    BucketContext Context = GetBucketContext(BucketName, Key);
    HttpRequest Request = BucketRequest(Context, "GET", Key);
    if (Offset > 0)
    {
      Request.Headers.append(qMakePair(QByteArray("range"), "bytes=" + QByteArray::number(Offset) + '-'));
    }
    // the destination is opened only once the source is known to exist
    Request.Target = [&](const HttpResponse & Response) -> QFile *
    {
      if (Offset > 0)
      {
        // "Content-Range: bytes 100-199/200"
        QByteArray ContentRange = Response.Header("content-range");
        qint64 Start = -1;
        if ((Response.Status == 206) && ContentRange.startsWith("bytes "))
        {
          Start = ContentRange.mid(6, ContentRange.indexOf('-') - 6).trimmed().toLongLong();
        }
        if (Start != Offset)
        {
          // Range not supported by the server, overwriting file
          Progress.AddTransferred(-Offset);
          Offset = 0;
        }
      }
      QIODevice::OpenMode OpenMode = (Offset > 0) ? (QIODevice::WriteOnly | QIODevice::Append) : (QIODevice::WriteOnly | QIODevice::Truncate);
      if (!Local.open(OpenMode))
      {
        throw ExtException(Tr(FILE_CREATE_ERROR).arg(LocalPath), Local.errorString());
      }
      LocalOpened = true;
      if ((Offset > 0) && (!Local.resize(Offset) || !Local.seek(Offset)))
      {
        throw ExtException(Tr(FILE_WRITE_ERROR).arg(LocalPath), Local.errorString());
      }
      return &Local;
    };
    HttpResponse Response = Execute(Request, &Progress);
    if ((Response.Status == 416) && (Offset > 0))
    {
      // "Content-Range: bytes */200": the partial file is complete already
      QByteArray ContentRange = Response.Header("content-range");
      qsizetype Slash = ContentRange.indexOf('/');
      if ((Slash >= 0) && (ContentRange.mid(Slash + 1).trimmed().toLongLong() == Offset))
      {
        LogLine(QStringLiteral("Partial file \"%1\" is complete already").arg(LocalPath));
        return;
      }
    }
    CheckResponse(Response);
    if (!LocalOpened)
    {
      throw ExtException(Tr(S3_RESPONSE_ERROR).arg(QStringLiteral("GetObject")));
    }
    if (!Local.flush())
    {
      throw ExtException(Tr(FILE_WRITE_ERROR).arg(LocalPath), Local.errorString());
    }
    Local.close();
  }
  catch (...)
  {
    if (LocalOpened)
    {
      Local.close();
      // As TS3FileSystem::Sink: delete the incomplete file, unless the transfer
      // is resumable and something was transferred
      if (!Params.KeepPartial || (Local.size() == 0))
      {
        Local.remove();
      }
    }
    try
    {
      throw;
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
      throw ExtException(Tr(TRANSFER_ERROR).arg(Path), Details(E));
    }
  }
}
