// Port of WinSCP source/core/S3FileSystem.cpp (TS3FileSystem).
//
// WinSCP talks to S3 through libs3 on top of neon. NixSCP implements the
// needed part of the S3 REST API directly:
//  - AWS Signature Version 4 (header based) is computed with OpenSSL
//    (SHA-256, HMAC-SHA256), see S3SignatureV4;
//  - HTTP/1.1 is a small blocking client on top of QSslSocket (keep-alive,
//    Content-Length and chunked bodies, "Expect: 100-continue" for large
//    uploads, proxies via QNetworkProxy), used on the Terminal worker thread
//    with waitFor* calls like FtpFileSystem. QNetworkAccessManager was not
//    used, because its request URL normalization and its own (unsigned)
//    headers make the signed request differ from the sent one, and because
//    streaming bodies with speed limits and cancellation is simpler when the
//    socket is driven directly;
//  - the XML responses are parsed with QXmlStreamReader.
//
// Paths: "/" lists buckets (ListBuckets), "/bucket/prefix" lists objects
// with ListObjectsV2 (delimiter "/", continuation tokens, ListObjects V1
// markers when a server ignores list-type=2). Folders are common prefixes;
// creating a folder creates a zero-byte "prefix/" object (a bucket at the
// root). Uploads above MultipartThreshold() use multipart upload; downloads
// resume with HTTP Range. Rename is CopyObject (x-amz-copy-source, multipart
// UploadPartCopy above 5 GB) + DeleteObject. Permissions, timestamps and
// links cannot be set (ChangeMode/CreateLink throw, SetModificationTime is a
// no-op as WinSCP does not preserve timestamps on S3).
//
// The authentication region of each bucket is detected as WinSCP does
// (TS3FileSystem::GetBucketContext): the bucket is probed with a one-key
// listing, AuthorizationHeaderMalformed errors give the region, 301/307
// redirects the endpoint; the x-amz-bucket-region header is used as well.
// HostName is the endpoint (default s3.amazonaws.com), Ftps != ftpsNone
// means HTTPS. S3UrlStyle selects virtual host or path style URLs; path
// style is used anyway for bucket names that cannot be a host name label
// (or contain a dot with HTTPS, as the wildcard certificate would not match)
// and for IP address endpoints. Untrusted certificates are confirmed with
// UserInterface::QueryUser, as in FtpFileSystem.
//
// Credentials: UserName = access key ID, Password = secret access key
// (prompted for when empty), S3SessionToken, and with S3CredentialsEnv the
// AWS_* environment variables and ~/.aws/credentials + ~/.aws/config
// (S3Profile / AWS_PROFILE, source_profile, credential_source=Environment;
// EC2 instance metadata credentials are not supported).
// S3RoleArn (or role_arn) is assumed with STS AssumeRole for Amazon hosts.
//
// Requests are logged to stderr when NIXSCP_S3_LOG is set.
#pragma once

#include "core/FileSystem.h"
#include "core/SessionData.h"

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QMap>
#include <QPair>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

class QFile;
class QSslError;
class QSslSocket;

using S3NameValueList = QList<QPair<QByteArray, QByteArray>>;

// AWS Signature Version 4, Authorization header variant
// (libs3 request.c compose_auth_header / AWS sig-v4 test suite)
class S3SignatureV4
{
public:
  // Request
  QByteArray Method;
  QByteArray CanonicalUri;        // URI-encoded path, see UriEncode(Path, false)
  S3NameValueList Query;          // decoded (UTF-8) names and values
  S3NameValueList Headers;        // all signed headers, including host and x-amz-date
  QByteArray PayloadHash;         // hex SHA-256 of the body or "UNSIGNED-PAYLOAD"
  QByteArray AccessKeyId;
  QByteArray SecretAccessKey;
  QByteArray Region;
  QByteArray Service = "s3";
  QDateTime Time;                 // must match the x-amz-date header

  // Results of Sign()
  QByteArray CanonicalQueryString;
  QByteArray CanonicalRequest;
  QByteArray SignedHeaders;
  QByteArray StringToSign;
  QByteArray Signature;
  QByteArray Authorization;

  void Sign();

  // RFC 3986 encoding of all but unreserved characters (A-Z a-z 0-9 - _ . ~);
  // "/" is kept unless EncodeSlash
  static QByteArray UriEncode(const QByteArray & Utf8, bool EncodeSlash);
  static QByteArray AmzDate(const QDateTime & Time);     // 20150830T123600Z
  static QByteArray Sha256Hex(const QByteArray & Data);
  static QByteArray HmacSha256(const QByteArray & Key, const QByteArray & Data);
  static QByteArray EmptyPayloadHash();
};

// Parsed S3 XML responses
struct S3Bucket
{
  QString Name;
  QDateTime CreationDate;
  QString Region;                 // BucketRegion (newer AWS responses only)
};

struct S3ListAllMyBucketsResult
{
  QString OwnerId;
  QString OwnerDisplayName;
  QList<S3Bucket> Buckets;
  QString ContinuationToken;
};

struct S3Object
{
  QString Key;
  QDateTime LastModified;
  qint64 Size = 0;
  QString ETag;
  QString OwnerId;
  QString OwnerDisplayName;
};

struct S3ListBucketResult
{
  QString Name;
  QString Prefix;
  bool IsTruncated = false;
  QString NextContinuationToken;  // ListObjectsV2
  QString NextMarker;             // ListObjects (V1)
  bool UrlEncoded = false;        // EncodingType=url (keys were decoded)
  QList<S3Object> Contents;
  QStringList CommonPrefixes;
};

struct S3ErrorResult
{
  QString Code;
  QString Message;
  QString Resource;
  QString Region;
  QString Endpoint;
  QList<QPair<QString, QString>> ExtraDetails;   // all other elements (RequestId, HostId...)
};

class S3FileSystem : public FileSystem
{
public:
  S3FileSystem(const SessionData & Data, UserInterface * UI);
  ~S3FileSystem() override;

  void Open() override;
  void Close() override;
  bool IsOpened() const override;
  QString ProtocolName() const override;
  QString ServerInfo() const override;

  QString CanonicalizePath(const QString & Path) override;
  QString HomeDirectory() override;
  RemoteFileList ReadDirectory(const QString & Path) override;
  RemoteFile ReadFile(const QString & Path) override;
  void CheckDirectory(const QString & Path) override;
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

  // Server side copy of a file (TS3FileSystem::CopyFile)
  void CopyFile(const QString & From, const QString & To) override;
  // Clears the bucket region/endpoint caches (TS3FileSystem::ClearCaches)
  void ClearCaches();

  // Multipart upload is used for files bigger than the threshold; parts are
  // at least ChunkSize (S3 minimum is 5 MB, at most 10000 parts)
  qint64 MultipartThreshold() const { return FMultipartThreshold; }
  void SetMultipartThreshold(qint64 Threshold, qint64 ChunkSize);
  // Authentication region currently used for the bucket (empty = not detected yet)
  QString BucketRegion(const QString & Bucket) const { return FRegions.value(Bucket); }

  static const char * const DefaultHostName;    // S3_DEFAULT_HOSTNAME
  static const char * const DefaultRegion;      // S3_DEFAULT_REGION

  // XML parsers (public for tests)
  static bool ParseListAllMyBucketsResult(const QByteArray & Xml, S3ListAllMyBucketsResult & Result, QString * Error = nullptr);
  static bool ParseListBucketResult(const QByteArray & Xml, S3ListBucketResult & Result, QString * Error = nullptr);
  static bool ParseError(const QByteArray & Xml, S3ErrorResult & Result);
  // "/bucket/key/path" -> "bucket", "key/path" (TS3FileSystem::ParsePath)
  static void ParsePath(const QString & Path, QString & BucketName, QString & Key);

  // AWS credentials from the environment and the AWS configuration files
  // (S3EnvUserName, S3EnvPassword, S3EnvSessionToken, S3EnvRoleArn, GetS3Profiles);
  // Source receives a description where the value came from
  static QString EnvUserName(const QString & Profile, QString * Source = nullptr);
  static QString EnvPassword(const QString & Profile, QString * Source = nullptr);
  static QString EnvSessionToken(const QString & Profile, QString * Source = nullptr);
  static QString EnvRoleArn(const QString & Profile, QString * Source = nullptr);
  static QStringList Profiles();

private:
  struct HttpRequest;
  struct HttpResponse;
  struct BucketContext;
  // Download target: called once response headers of a successful response
  // arrived; returns the device the body is written to
  using BodyTarget = std::function<QFile *(const HttpResponse &)>;

  SessionData FData;
  UserInterface * FUI;
  bool FActive = false;
  bool FTls = true;
  QString FHostName;
  quint16 FPort = 443;
  QString FAuthRegion;
  QByteArray FAccessKeyId;
  QByteArray FSecretAccessKey;
  QByteArray FSessionToken;
  QMap<QString, QString> FRegions;     // bucket -> authentication region
  QMap<QString, QString> FHostNames;   // bucket -> endpoint host name
  qint64 FMultipartThreshold;
  qint64 FMultipartChunkSize;
  // HTTP connection (kept alive between requests)
  std::unique_ptr<QSslSocket> FSocket;
  QString FSocketHost;
  quint16 FSocketPort = 0;
  bool FSocketTls = false;
  QSet<QString> FAcceptedOnce;
  QString FTlsVersion;
  QString FCipher;

  int TimeoutMs() const;
  bool IsAmazon() const;
  bool IsGoogleCloud() const;
  int EffectiveMaxKeys(int MaxKeys) const;
  void SetCredentials(const QString & AccessKeyId, const QString & SecretAccessKey, const QString & SessionToken);
  void AssumeRole(const QString & RoleArn);

  // HTTP
  bool EnsureConnection(const QString & Host, quint16 Port, bool Tls);
  void CloseConnection();
  void VerifyCertificate(const QString & Host, const QList<QSslError> & Errors);
  enum WaitResult { wrData, wrEof, wrTimeout };
  void WriteAll(const QByteArray & Data, FileOperationProgress * Progress);
  // Waits until more than Have bytes can be read
  WaitResult WaitForData(FileOperationProgress * Progress, int Timeout, qint64 Have);
  QByteArray ReadLine(FileOperationProgress * Progress);
  void SendBody(const HttpRequest & Request, FileOperationProgress * Progress, qint64 & Uploaded);
  void ReadResponseHead(HttpResponse & Response, FileOperationProgress * Progress, bool & AnyResponse);
  void ReadResponseBody(const HttpRequest & Request, HttpResponse & Response, FileOperationProgress * Progress);
  HttpResponse DoExecute(HttpRequest & Request, FileOperationProgress * Progress, bool & Reused, bool & AnyResponse, qint64 & Uploaded);
  HttpResponse Execute(HttpRequest & Request, FileOperationProgress * Progress = nullptr);
  [[noreturn]] void ThrowResponseError(const HttpResponse & Response, const QString & Message = QString());
  void CheckResponse(const HttpResponse & Response, const QString & Message = QString());

  // Buckets and keys
  BucketContext MakeBucketContext(const QString & BucketName, const QString & Region) const;
  BucketContext GetBucketContext(const QString & BucketName, const QString & Prefix);
  HttpRequest BucketRequest(const BucketContext & Context, const QByteArray & Method, const QString & Key) const;
  HttpRequest ServiceRequest(const QByteArray & Method) const;
  HttpResponse ListBucketRequest(const BucketContext & Context, const QString & Prefix, int MaxKeys,
    const QString & ContinuationToken, const QString & Marker, bool V1);
  void HandleNonBucketStatus(const HttpResponse & Response, bool & Retry);
  S3ListAllMyBucketsResult ListBuckets();
  RemoteFileList ListPrefix(const QString & Path, int MaxKeys, bool * Exists);
  bool ReadFileInternal(const QString & Path, RemoteFile & File);
  void DeleteObject(const QString & BucketName, const QString & Key);
  void CopyObject(const QString & From, const QString & To, qint64 Size);
  QString InitiateMultipart(const BucketContext & Context, const QString & Key, const QByteArray & ContentType);
  void CompleteMultipart(const BucketContext & Context, const QString & Key, const QString & UploadId, const QList<QByteArray> & ETags);
  void AbortMultipart(const BucketContext & Context, const QString & Key, const QString & UploadId);

  static RemoteFile DirectoryEntry(const QString & Name);
  static QString OwnerName(const QString & Id, const QString & DisplayName);
};
