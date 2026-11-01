// Tests of core/S3FileSystem.
//
// Always run (no network):
//  - AWS Signature Version 4 against the published test vectors of the AWS
//    sig-v4 test suite (aws-c-auth / aws-sig-v4-test-suite: get-vanilla,
//    get-vanilla-query-order-key-case, get-vanilla-utf8-query, get-utf8,
//    get-space-unnormalized, get-header-value-trim, post-vanilla,
//    post-x-www-form-urlencoded) and the examples of the Amazon S3 API
//    reference ("Signature Calculations for the Authorization Header:
//    Transferring Payload in a Single Chunk");
//  - parsing of ListAllMyBucketsResult, ListBucketResult (V1/V2,
//    encoding-type=url) and Error responses;
//  - AWS credentials from the environment and the AWS configuration files.
//
// Server part, skipped unless NIXSCP_TEST_S3_ENDPOINT is set. Environment:
//   NIXSCP_TEST_S3_ENDPOINT     host:port of an S3 server (MinIO or similar)
//   NIXSCP_TEST_S3_ACCESS_KEY   default "minioadmin"
//   NIXSCP_TEST_S3_SECRET_KEY   default "minioadmin"
//   NIXSCP_TEST_S3_TLS          1 = HTTPS (certificate is accepted when asked), default 0
//   NIXSCP_TEST_S3_URL_STYLE    "path" (default) or "virtual"
//   NIXSCP_TEST_S3_REGION       default region (S3DefaultRegion), default empty
//   NIXSCP_TEST_S3_MANY         set to 0 to skip the listing pagination test (1001 objects)
//   NIXSCP_TEST_S3_REGION_BUCKET  optional "bucket:region" of an existing bucket in a region
//                               other than the default one (region detection test)
// The test creates and deletes a bucket named nixscp-test-<random>.
#include "core/S3FileSystem.h"
#include "core/Terminal.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QTimer>
#include <QVariant>
#include <QElapsedTimer>

#include <QThread>

#include <cstdio>
#include <cstdlib>
#include <thread>

static int Failures = 0;

#define CHECK(Cond) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); std::fflush(stdout); Failures++; } } while (0)
#define CHECK_MSG(Cond, Msg) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, #Cond, qPrintable(Msg)); std::fflush(stdout); Failures++; } } while (0)
#define CHECK_EQ(A, B) \
  do { auto VA = (A); auto VB = (B); if (!(VA == VB)) { std::printf("FAIL %s:%d: %s == %s\n  got:      %s\n  expected: %s\n", __FILE__, __LINE__, #A, #B, \
    qPrintable(QVariant::fromValue(VA).toString()), qPrintable(QVariant::fromValue(VB).toString())); std::fflush(stdout); Failures++; } } while (0)

static void Log(const QString & Message)
{
  std::printf("  %s\n", qPrintable(Message));
  std::fflush(stdout);
}

//---------------------------------------------------------------------------
// Signature Version 4
//---------------------------------------------------------------------------

struct SigV4Vector
{
  const char * Name;
  const char * Request;            // request.txt
  const char * CanonicalRequest;   // header-canonical-request.txt
  const char * StringToSign;       // header-string-to-sign.txt
  const char * Signature;          // header-signature.txt
  bool SignBody;                   // context.json "sign_body"
};

// AWS sig-v4 test suite (credentials AKIDEXAMPLE / wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY,
// region us-east-1, service "service", 2015-08-30T12:36:00Z)
static const SigV4Vector SuiteVectors[] =
{
  {
    "get-vanilla",
    "GET / HTTP/1.1\nHost:example.amazonaws.com\n",
    "GET\n/\n\nhost:example.amazonaws.com\nx-amz-date:20150830T123600Z\n\nhost;x-amz-date\n"
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
    "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\n"
    "bb579772317eb040ac9ed261061d46c1f17a8133879d6129b6e1c25292927e63",
    "5fa00fa31553b73ebf1942676e86291e8372ff2a2260956d9b8aae1d763fbf31",
    false
  },
  {
    "get-vanilla-query-order-key-case",
    "GET /?Param2=value2&Param1=value1 HTTP/1.1\nHost:example.amazonaws.com\n",
    "GET\n/\nParam1=value1&Param2=value2\nhost:example.amazonaws.com\nx-amz-date:20150830T123600Z\n\nhost;x-amz-date\n"
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
    "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\n"
    "816cd5b414d056048ba4f7c5386d6e0533120fb1fcfa93762cf0fc39e2cf19e0",
    "b97d918cfa904a5beff61c982a1b6f458b799221646efd99d3219ec94cdf2500",
    false
  },
  {
    "get-vanilla-utf8-query",
    "GET /?\xE1\x88\xB4=bar HTTP/1.1\nHost:example.amazonaws.com\n",
    "GET\n/\n%E1%88%B4=bar\nhost:example.amazonaws.com\nx-amz-date:20150830T123600Z\n\nhost;x-amz-date\n"
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
    "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\n"
    "eb30c5bed55734080471a834cc727ae56beb50e5f39d1bff6d0d38cb192a7073",
    "2cdec8eed098649ff3a119c94853b13c643bcf08f8b0a1d91e12c9027818dd04",
    false
  },
  {
    "get-utf8",
    "GET /\xE1\x88\xB4 HTTP/1.1\nHost:example.amazonaws.com\n",
    "GET\n/%E1%88%B4\n\nhost:example.amazonaws.com\nx-amz-date:20150830T123600Z\n\nhost;x-amz-date\n"
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
    "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\n"
    "2a0a97d02205e45ce2e994789806b19270cfbbb0921b278ccf58f5249ac42102",
    "8318018e0b0f223aa2bbf98705b62bb787dc9c0e678f255a891fd03141be5d85",
    false
  },
  {
    "get-space-unnormalized",
    "GET /example space/ HTTP/1.1\nHost:example.amazonaws.com\n",
    "GET\n/example%20space/\n\nhost:example.amazonaws.com\nx-amz-date:20150830T123600Z\n\nhost;x-amz-date\n"
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
    "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\n"
    "63ee75631ed7234ae61b5f736dfc7754cdccfedbff4b5128a915706ee9390d86",
    "652487583200325589f1fba4c7e578f72c47cb61beeca81406b39ddec1366741",
    false
  },
  {
    "get-header-value-trim",
    "GET / HTTP/1.1\nHost:example.amazonaws.com\nMy-Header1: value1\nMy-Header2: \"a   b   c\"\n",
    "GET\n/\n\nhost:example.amazonaws.com\nmy-header1:value1\nmy-header2:\"a b c\"\nx-amz-date:20150830T123600Z\n\n"
    "host;my-header1;my-header2;x-amz-date\n"
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
    "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\n"
    "a726db9b0df21c14f559d0a978e563112acb1b9e05476f0a6a1c7d68f28605c7",
    "acc3ed3afb60bb290fc8d2dd0098b9911fcaa05412b367055dee359757a9c736",
    false
  },
  {
    "post-vanilla",
    "POST / HTTP/1.1\nHost:example.amazonaws.com\n",
    "POST\n/\n\nhost:example.amazonaws.com\nx-amz-date:20150830T123600Z\n\nhost;x-amz-date\n"
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
    "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\n"
    "553f88c9e4d10fc9e109e2aeb65f030801b70c2f6468faca261d401ae622fc87",
    "5da7c1a2acd57cee7505fc6676e4e544621c30862966e37dddb68e92efbe5d6b",
    false
  },
  {
    "post-x-www-form-urlencoded",
    "POST / HTTP/1.1\nContent-Type:application/x-www-form-urlencoded\nHost:example.amazonaws.com\nContent-Length:13\n\nParam1=value1",
    "POST\n/\n\ncontent-length:13\ncontent-type:application/x-www-form-urlencoded\nhost:example.amazonaws.com\n"
    "x-amz-content-sha256:9095672bbd1f56dfc5b65f3e153adc8731a4a654192329106275f4c7b24d0b6e\nx-amz-date:20150830T123600Z\n\n"
    "content-length;content-type;host;x-amz-content-sha256;x-amz-date\n"
    "9095672bbd1f56dfc5b65f3e153adc8731a4a654192329106275f4c7b24d0b6e",
    "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\n"
    "b1edd1d03544c25390e32085d55b57acc9a3961bb59415ff86c45c3d89d16cfb",
    "d3875051da38690788ef43de4db0d8f280229d82040bfac253562e56c3f20e0b",
    true
  },
};

static S3NameValueList ParseQueryString(const QByteArray & Query)
{
  S3NameValueList Result;
  for (const QByteArray & Part : Query.split('&'))
  {
    if (Part.isEmpty())
    {
      continue;
    }
    qsizetype Equals = Part.indexOf('=');
    QByteArray Name = (Equals >= 0) ? Part.left(Equals) : Part;
    QByteArray Value = (Equals >= 0) ? Part.mid(Equals + 1) : QByteArray();
    Result.append(qMakePair(QByteArray::fromPercentEncoding(Name), QByteArray::fromPercentEncoding(Value)));
  }
  return Result;
}

static void TestSuiteVector(const SigV4Vector & Vector)
{
  std::printf("[sigv4 %s]\n", Vector.Name);
  QByteArray Request(Vector.Request);
  qsizetype BodyStart = Request.indexOf("\n\n");
  QByteArray Body = (BodyStart >= 0) ? Request.mid(BodyStart + 2) : QByteArray();
  QList<QByteArray> Lines = ((BodyStart >= 0) ? Request.left(BodyStart) : Request).split('\n');
  // "GET /path?query HTTP/1.1"; the path may contain spaces
  QByteArray RequestLine = Lines.takeFirst();
  qsizetype MethodEnd = RequestLine.indexOf(' ');
  qsizetype TargetEnd = RequestLine.lastIndexOf(' ');
  QByteArray Target = RequestLine.mid(MethodEnd + 1, TargetEnd - MethodEnd - 1);
  qsizetype QueryStart = Target.indexOf('?');

  S3SignatureV4 Signature;
  Signature.Method = RequestLine.left(MethodEnd);
  Signature.CanonicalUri = S3SignatureV4::UriEncode((QueryStart >= 0) ? Target.left(QueryStart) : Target, false);
  if (QueryStart >= 0)
  {
    Signature.Query = ParseQueryString(Target.mid(QueryStart + 1));
  }
  for (const QByteArray & Line : Lines)
  {
    if (Line.isEmpty())
    {
      continue;
    }
    qsizetype Colon = Line.indexOf(':');
    Signature.Headers.append(qMakePair(Line.left(Colon), Line.mid(Colon + 1)));
  }
  Signature.Time = QDateTime(QDate(2015, 8, 30), QTime(12, 36, 0), QTimeZone::UTC);
  Signature.Headers.append(qMakePair(QByteArray("X-Amz-Date"), S3SignatureV4::AmzDate(Signature.Time)));
  Signature.PayloadHash = S3SignatureV4::Sha256Hex(Body);
  if (Vector.SignBody)
  {
    Signature.Headers.append(qMakePair(QByteArray("X-Amz-Content-Sha256"), Signature.PayloadHash));
  }
  Signature.AccessKeyId = "AKIDEXAMPLE";
  Signature.SecretAccessKey = "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY";
  Signature.Region = "us-east-1";
  Signature.Service = "service";
  Signature.Sign();
  CHECK_EQ(Signature.CanonicalRequest, QByteArray(Vector.CanonicalRequest));
  CHECK_EQ(Signature.StringToSign, QByteArray(Vector.StringToSign));
  CHECK_EQ(Signature.Signature, QByteArray(Vector.Signature));
  CHECK(Signature.Authorization.startsWith("AWS4-HMAC-SHA256 Credential=AKIDEXAMPLE/20150830/us-east-1/service/aws4_request, SignedHeaders="));
  CHECK(Signature.Authorization.endsWith(", Signature=" + QByteArray(Vector.Signature)));
}

// Amazon S3 API reference examples (AKIAIOSFODNN7EXAMPLE, 2013-05-24, us-east-1, examplebucket)
static void TestS3Example(const char * Name, const QByteArray & Method, const QByteArray & Path, const QByteArray & Query,
  const S3NameValueList & Headers, const QByteArray & PayloadHash, const QByteArray & CanonicalRequestHash, const QByteArray & ExpectedSignature)
{
  std::printf("[sigv4 s3 example %s]\n", Name);
  S3SignatureV4 Signature;
  Signature.Method = Method;
  Signature.CanonicalUri = S3SignatureV4::UriEncode(Path, false);
  Signature.Query = ParseQueryString(Query);
  Signature.Time = QDateTime(QDate(2013, 5, 24), QTime(0, 0, 0), QTimeZone::UTC);
  Signature.Headers = Headers;
  Signature.Headers.append(qMakePair(QByteArray("Host"), QByteArray("examplebucket.s3.amazonaws.com")));
  Signature.Headers.append(qMakePair(QByteArray("x-amz-date"), S3SignatureV4::AmzDate(Signature.Time)));
  Signature.Headers.append(qMakePair(QByteArray("x-amz-content-sha256"), PayloadHash));
  Signature.PayloadHash = PayloadHash;
  Signature.AccessKeyId = "AKIAIOSFODNN7EXAMPLE";
  Signature.SecretAccessKey = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";
  Signature.Region = "us-east-1";
  Signature.Service = "s3";
  Signature.Sign();
  CHECK_EQ(Signature.StringToSign.split('\n').value(3), CanonicalRequestHash);
  CHECK_EQ(Signature.Signature, ExpectedSignature);
  CHECK(Signature.Authorization.startsWith("AWS4-HMAC-SHA256 Credential=AKIAIOSFODNN7EXAMPLE/20130524/us-east-1/s3/aws4_request, SignedHeaders="));
}

static void TestSignature()
{
  for (const SigV4Vector & Vector : SuiteVectors)
  {
    TestSuiteVector(Vector);
  }

  QByteArray Empty = S3SignatureV4::EmptyPayloadHash();
  CHECK_EQ(S3SignatureV4::Sha256Hex(QByteArray()), Empty);
  TestS3Example("GET Object", "GET", "/test.txt", QByteArray(),
    {qMakePair(QByteArray("Range"), QByteArray("bytes=0-9"))}, Empty,
    "7344ae5b7ee6c3e7e6b0fe0640412a37625d1fbfff95c48bbb2dc43964946972",
    "f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41");
  QByteArray PutPayload = S3SignatureV4::Sha256Hex("Welcome to Amazon S3.");
  CHECK_EQ(PutPayload, QByteArray("44ce7dd67c959e0d3524ffac1771dfbba87d2b6b4b4e99e42034a8b803f8b072"));
  TestS3Example("PUT Object", "PUT", "/test$file.text", QByteArray(),
    {qMakePair(QByteArray("Date"), QByteArray("Fri, 24 May 2013 00:00:00 GMT")),
     qMakePair(QByteArray("x-amz-storage-class"), QByteArray("REDUCED_REDUNDANCY"))}, PutPayload,
    "9e0e90d9c76de8fa5b200d8c849cd5b8dc7a3be3951ddb7f6a76b4158342019d",
    "98ad721746da40c64f1a55b78f14c238d841ea1380cd77a1b5971af0ece108bd");
  TestS3Example("GET Bucket Lifecycle", "GET", "/", "lifecycle", {}, Empty,
    "9766c798316ff2757b517bc739a67f6213b4ab36dd5da2f94eaebf79c77395ca",
    "fea454ca298b7da1c68078a5d1bdbfbbe0d65c699e0f91ac7a200a0136783543");
  TestS3Example("Get Bucket (List Objects)", "GET", "/", "max-keys=2&prefix=J", {}, Empty,
    "df57d21db20da04d7fa30298dd4488ba3a2b47ca3a489c74750e0f1e7df1b9b7",
    "34b48302e7b5fa45bde8084f4b7868a86f0a534bc59db6670ed5711ef69dc6f7");

  std::printf("[sigv4 encoding]\n");
  CHECK_EQ(S3SignatureV4::UriEncode("/a b/c+d&e=f~g_h-i.j/", false), QByteArray("/a%20b/c%2Bd%26e%3Df~g_h-i.j/"));
  CHECK_EQ(S3SignatureV4::UriEncode("a/b", true), QByteArray("a%2Fb"));
  CHECK_EQ(S3SignatureV4::UriEncode("\xC3\xA9*!'()", false), QByteArray("%C3%A9%2A%21%27%28%29"));
  CHECK_EQ(S3SignatureV4::AmzDate(QDateTime(QDate(2015, 8, 30), QTime(12, 36, 0), QTimeZone::UTC)), QByteArray("20150830T123600Z"));
  // Query values are sorted after encoding, empty values keep "="
  S3SignatureV4 Signature;
  Signature.Method = "GET";
  Signature.CanonicalUri = "/";
  Signature.Query = {qMakePair(QByteArray("uploads"), QByteArray()), qMakePair(QByteArray("prefix"), QByteArray("a/b c")),
    qMakePair(QByteArray("delimiter"), QByteArray("/"))};
  Signature.Time = QDateTime::currentDateTimeUtc();
  Signature.Headers = {qMakePair(QByteArray("host"), QByteArray("h")), qMakePair(QByteArray("x-amz-date"), S3SignatureV4::AmzDate(Signature.Time))};
  Signature.PayloadHash = Empty;
  Signature.AccessKeyId = "A";
  Signature.SecretAccessKey = "S";
  Signature.Region = "r";
  Signature.Sign();
  CHECK_EQ(Signature.CanonicalQueryString, QByteArray("delimiter=%2F&prefix=a%2Fb%20c&uploads="));
}

//---------------------------------------------------------------------------
// XML
//---------------------------------------------------------------------------

static void TestXml()
{
  std::printf("[xml ListAllMyBucketsResult]\n");
  {
    QByteArray Xml =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<ListAllMyBucketsResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">\n"
      "  <Owner>\n"
      "    <ID>bcaf1ffd86f461ca5fb16fd081034f</ID>\n"
      "    <DisplayName>webfile</DisplayName>\n"
      "  </Owner>\n"
      "  <Buckets>\n"
      "    <Bucket>\n"
      "      <Name>quotes</Name>\n"
      "      <CreationDate>2006-02-03T16:45:09.000Z</CreationDate>\n"
      "    </Bucket>\n"
      "    <Bucket>\n"
      "      <Name>samples</Name>\n"
      "      <CreationDate>2006-02-03T16:41:58.000Z</CreationDate>\n"
      "      <BucketRegion>eu-west-1</BucketRegion>\n"
      "    </Bucket>\n"
      "  </Buckets>\n"
      "</ListAllMyBucketsResult>";
    S3ListAllMyBucketsResult Result;
    QString Error;
    CHECK_MSG(S3FileSystem::ParseListAllMyBucketsResult(Xml, Result, &Error), Error);
    CHECK_EQ(Result.OwnerId, QStringLiteral("bcaf1ffd86f461ca5fb16fd081034f"));
    CHECK_EQ(Result.OwnerDisplayName, QStringLiteral("webfile"));
    CHECK_EQ(Result.Buckets.size(), 2);
    CHECK_EQ(Result.Buckets.value(0).Name, QStringLiteral("quotes"));
    CHECK_EQ(Result.Buckets.value(0).CreationDate, QDateTime(QDate(2006, 2, 3), QTime(16, 45, 9), QTimeZone::UTC));
    CHECK_EQ(Result.Buckets.value(1).Name, QStringLiteral("samples"));
    CHECK_EQ(Result.Buckets.value(1).Region, QStringLiteral("eu-west-1"));
    CHECK(Result.ContinuationToken.isEmpty());
    // a different document is rejected
    CHECK(!S3FileSystem::ParseListAllMyBucketsResult("<ListBucketResult/>", Result, &Error));
    CHECK(!S3FileSystem::ParseListAllMyBucketsResult("<ListAllMyBucketsResult><Buckets>", Result, &Error));
  }

  std::printf("[xml ListBucketResult V2]\n");
  {
    QByteArray Xml =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<ListBucketResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">\n"
      "  <Name>example-bucket</Name>\n"
      "  <Prefix>photos/2006/</Prefix>\n"
      "  <KeyCount>3</KeyCount>\n"
      "  <MaxKeys>1000</MaxKeys>\n"
      "  <Delimiter>/</Delimiter>\n"
      "  <IsTruncated>true</IsTruncated>\n"
      "  <NextContinuationToken>1ueGcxLPRx1Tr/XYExHnhbYLgveDs2J/wm36Hy4vbOwM=</NextContinuationToken>\n"
      "  <Contents>\n"
      "    <Key>photos/2006/</Key>\n"
      "    <LastModified>2009-10-12T17:50:30.000Z</LastModified>\n"
      "    <ETag>&quot;d41d8cd98f00b204e9800998ecf8427e&quot;</ETag>\n"
      "    <Size>0</Size>\n"
      "    <StorageClass>STANDARD</StorageClass>\n"
      "  </Contents>\n"
      "  <Contents>\n"
      "    <Key>photos/2006/index &amp; more.html</Key>\n"
      "    <LastModified>2009-10-12T17:50:30.789Z</LastModified>\n"
      "    <ETag>\"fba9dede5f27731c9771645a39863328\"</ETag>\n"
      "    <Size>434234</Size>\n"
      "    <Owner><ID>75aa57f09aa0c8caeab4f8c24e99d10f8e7faeebf76c078efc7c6caea54ba06a</ID><DisplayName>mtd</DisplayName></Owner>\n"
      "    <StorageClass>STANDARD</StorageClass>\n"
      "  </Contents>\n"
      "  <CommonPrefixes>\n"
      "    <Prefix>photos/2006/February/</Prefix>\n"
      "  </CommonPrefixes>\n"
      "  <CommonPrefixes>\n"
      "    <Prefix>photos/2006/January/</Prefix>\n"
      "  </CommonPrefixes>\n"
      "</ListBucketResult>";
    S3ListBucketResult Result;
    QString Error;
    CHECK_MSG(S3FileSystem::ParseListBucketResult(Xml, Result, &Error), Error);
    CHECK_EQ(Result.Name, QStringLiteral("example-bucket"));
    CHECK_EQ(Result.Prefix, QStringLiteral("photos/2006/"));
    CHECK(Result.IsTruncated);
    CHECK(!Result.UrlEncoded);
    CHECK_EQ(Result.NextContinuationToken, QStringLiteral("1ueGcxLPRx1Tr/XYExHnhbYLgveDs2J/wm36Hy4vbOwM="));
    CHECK_EQ(Result.Contents.size(), 2);
    CHECK_EQ(Result.Contents.value(0).Key, QStringLiteral("photos/2006/"));
    CHECK_EQ(Result.Contents.value(0).Size, Q_INT64_C(0));
    CHECK_EQ(Result.Contents.value(0).ETag, QStringLiteral("\"d41d8cd98f00b204e9800998ecf8427e\""));
    CHECK_EQ(Result.Contents.value(1).Key, QStringLiteral("photos/2006/index & more.html"));
    CHECK_EQ(Result.Contents.value(1).Size, Q_INT64_C(434234));
    // second precision (as WinSCP)
    CHECK_EQ(Result.Contents.value(1).LastModified, QDateTime(QDate(2009, 10, 12), QTime(17, 50, 30), QTimeZone::UTC));
    CHECK_EQ(Result.Contents.value(1).OwnerDisplayName, QStringLiteral("mtd"));
    CHECK_EQ(Result.Contents.value(1).OwnerId, QStringLiteral("75aa57f09aa0c8caeab4f8c24e99d10f8e7faeebf76c078efc7c6caea54ba06a"));
    CHECK_EQ(Result.CommonPrefixes, QStringList({QStringLiteral("photos/2006/February/"), QStringLiteral("photos/2006/January/")}));
  }

  std::printf("[xml ListBucketResult V1, encoding-type=url]\n");
  {
    QByteArray Xml =
      "<ListBucketResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">"
      "<Name>b</Name><Prefix>dir%2F</Prefix><Marker></Marker><NextMarker>dir%2Fz+z</NextMarker>"
      "<MaxKeys>2</MaxKeys><Delimiter>%2F</Delimiter><IsTruncated>true</IsTruncated>"
      "<Contents><Key>dir%2Fa+b%2Bc%26d%C3%A9%01.txt</Key><LastModified>2024-01-02T03:04:05.000Z</LastModified>"
      "<ETag>\"x\"</ETag><Size>5</Size><StorageClass>STANDARD</StorageClass></Contents>"
      "<CommonPrefixes><Prefix>dir%2Fsub+dir%2F</Prefix></CommonPrefixes>"
      "<EncodingType>url</EncodingType></ListBucketResult>";
    S3ListBucketResult Result;
    QString Error;
    CHECK_MSG(S3FileSystem::ParseListBucketResult(Xml, Result, &Error), Error);
    CHECK(Result.UrlEncoded);
    CHECK(Result.IsTruncated);
    CHECK(Result.NextContinuationToken.isEmpty());
    CHECK_EQ(Result.Prefix, QStringLiteral("dir/"));
    CHECK_EQ(Result.NextMarker, QStringLiteral("dir/z z"));
    CHECK_EQ(Result.Contents.value(0).Key, QString::fromUtf8("dir/a b+c&d\xC3\xA9\x01.txt"));
    CHECK_EQ(Result.CommonPrefixes.value(0), QStringLiteral("dir/sub dir/"));
    // empty listing
    CHECK(S3FileSystem::ParseListBucketResult("<ListBucketResult><Name>b</Name><IsTruncated>false</IsTruncated></ListBucketResult>", Result, &Error));
    CHECK(Result.Contents.isEmpty() && Result.CommonPrefixes.isEmpty() && !Result.IsTruncated);
    CHECK(!S3FileSystem::ParseListBucketResult("not xml", Result, &Error));
  }

  std::printf("[xml Error]\n");
  {
    S3ErrorResult Error;
    CHECK(S3FileSystem::ParseError(
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<Error><Code>AuthorizationHeaderMalformed</Code>"
      "<Message>The authorization header is malformed; the region 'us-east-1' is wrong; expecting 'eu-west-1'</Message>"
      "<Region>eu-west-1</Region><RequestId>656c76696e6727732072657175657374</RequestId>"
      "<HostId>Uuag1LuByRx9e6j5Onimru9pO4ZVKnJ2Qz7/C1NPcfTWAtRPfTaOFg==</HostId></Error>", Error));
    CHECK_EQ(Error.Code, QStringLiteral("AuthorizationHeaderMalformed"));
    CHECK_EQ(Error.Region, QStringLiteral("eu-west-1"));
    CHECK(Error.Message.startsWith(QStringLiteral("The authorization header is malformed")));
    CHECK_EQ(Error.ExtraDetails.size(), 3);
    CHECK_EQ(Error.ExtraDetails.value(1).first, QStringLiteral("RequestId"));
    CHECK(S3FileSystem::ParseError(
      "<Error><Code>PermanentRedirect</Code><Message>The bucket you are attempting to access must be addressed using the specified endpoint.</Message>"
      "<Endpoint>bucket.s3.eu-west-1.amazonaws.com</Endpoint><Bucket>bucket</Bucket><Resource>/bucket/key</Resource></Error>", Error));
    CHECK_EQ(Error.Code, QStringLiteral("PermanentRedirect"));
    CHECK_EQ(Error.Endpoint, QStringLiteral("bucket.s3.eu-west-1.amazonaws.com"));
    CHECK_EQ(Error.Resource, QStringLiteral("/bucket/key"));
    CHECK(!S3FileSystem::ParseError(QByteArray(), Error));
    CHECK(!S3FileSystem::ParseError("<ListBucketResult/>", Error));
  }

  std::printf("[paths]\n");
  {
    QString Bucket;
    QString Key;
    S3FileSystem::ParsePath(QStringLiteral("/bucket"), Bucket, Key);
    CHECK((Bucket == QStringLiteral("bucket")) && Key.isEmpty());
    S3FileSystem::ParsePath(QStringLiteral("/bucket/a/b c"), Bucket, Key);
    CHECK((Bucket == QStringLiteral("bucket")) && (Key == QStringLiteral("a/b c")));
    S3FileSystem::ParsePath(QStringLiteral("/"), Bucket, Key);
    CHECK(Bucket.isEmpty() && Key.isEmpty());
    SessionData Data;
    Data.FSProtocol = fsS3;
    S3FileSystem Fs(Data, nullptr);
    CHECK_EQ(Fs.CanonicalizePath(QString()), QStringLiteral("/"));
    CHECK_EQ(Fs.CanonicalizePath(QStringLiteral("/b/x/../y/./z/")), QStringLiteral("/b/y/z"));
    CHECK_EQ(Fs.HomeDirectory(), QStringLiteral("/"));
    CHECK_EQ(Fs.ProtocolName(), QStringLiteral("S3"));
    CHECK(!Fs.IsOpened());
    bool Thrown = false;
    try
    {
      Fs.ChangeMode(QStringLiteral("/b/x"), 0644);
    }
    catch (const ExtException &)
    {
      Thrown = true;
    }
    CHECK(Thrown);
  }
}

//---------------------------------------------------------------------------
// AWS credentials
//---------------------------------------------------------------------------

static void WriteFile(const QString & FileName, const QByteArray & Contents)
{
  QFile File(FileName);
  CHECK(File.open(QIODevice::WriteOnly | QIODevice::Truncate));
  File.write(Contents);
}

static void TestCredentials(const QString & Scratch)
{
  std::printf("[aws credentials]\n");
  QString CredentialsFile = Scratch + QStringLiteral("/credentials");
  QString ConfigFile = Scratch + QStringLiteral("/config");
  WriteFile(CredentialsFile,
    "# comment\n"
    "[default]\n"
    "aws_access_key_id = AKIDDEFAULT\n"
    "aws_secret_access_key=secretdefault\n"
    "\n"
    "[Dev]\n"
    "AWS_ACCESS_KEY_ID = AKIDDEV\n"
    "aws_secret_access_key = secretdev\n"
    "aws_session_token = tokendev\n"
    "[partial]\n"
    "aws_access_key_id = AKIDPARTIAL\n");
  WriteFile(ConfigFile,
    "[default]\n"
    "region = eu-west-1\n"
    "[profile chained]\n"
    "source_profile = dev\n"
    "role_arn = arn:aws:iam::123456789012:role/test\n"
    "[profile fromenv]\n"
    "credential_source = Environment\n"
    "role_arn = arn:aws:iam::123456789012:role/env\n"
    "[notaprofile]\n"
    "aws_access_key_id = X\n"
    "aws_secret_access_key = Y\n");
  qputenv("AWS_SHARED_CREDENTIALS_FILE", QFile::encodeName(CredentialsFile));
  qputenv("AWS_CONFIG_FILE", QFile::encodeName(ConfigFile));
  qunsetenv("AWS_PROFILE");
  qunsetenv("AWS_ACCESS_KEY_ID");
  qunsetenv("AWS_SECRET_ACCESS_KEY");
  qunsetenv("AWS_SESSION_TOKEN");
  qunsetenv("AWS_ROLE_ARN");

  QString Source;
  CHECK_EQ(S3FileSystem::EnvUserName(QString(), &Source), QStringLiteral("AKIDDEFAULT"));
  CHECK_EQ(Source, QStringLiteral("credentials/default"));
  CHECK_EQ(S3FileSystem::EnvPassword(QString()), QStringLiteral("secretdefault"));
  CHECK(S3FileSystem::EnvSessionToken(QString()).isEmpty());
  // case insensitive section and key names (TMemIniFile)
  CHECK_EQ(S3FileSystem::EnvUserName(QStringLiteral("dev")), QStringLiteral("AKIDDEV"));
  CHECK_EQ(S3FileSystem::EnvSessionToken(QStringLiteral("dev")), QStringLiteral("tokendev"));
  // source_profile
  CHECK_EQ(S3FileSystem::EnvUserName(QStringLiteral("chained"), &Source), QStringLiteral("AKIDDEV"));
  CHECK_EQ(Source, QStringLiteral("config/chained=>credentials/dev"));
  CHECK_EQ(S3FileSystem::EnvRoleArn(QStringLiteral("chained")), QStringLiteral("arn:aws:iam::123456789012:role/test"));
  // AWS_PROFILE selects the profile when none is given
  qputenv("AWS_PROFILE", "dev");
  CHECK_EQ(S3FileSystem::EnvUserName(QString()), QStringLiteral("AKIDDEV"));
  qunsetenv("AWS_PROFILE");
  // environment variables win when no profile is given
  qputenv("AWS_ACCESS_KEY_ID", "AKIDENV");
  CHECK_EQ(S3FileSystem::EnvUserName(QString(), &Source), QStringLiteral("AKIDENV"));
  CHECK_EQ(Source, QStringLiteral("$AWS_ACCESS_KEY_ID"));
  CHECK_EQ(S3FileSystem::EnvUserName(QStringLiteral("dev")), QStringLiteral("AKIDDEV"));
  // credential_source = Environment
  CHECK_EQ(S3FileSystem::EnvUserName(QStringLiteral("fromenv"), &Source), QStringLiteral("AKIDENV"));
  CHECK_EQ(Source, QStringLiteral("config/fromenv=>$AWS_ACCESS_KEY_ID"));
  qunsetenv("AWS_ACCESS_KEY_ID");
  CHECK(S3FileSystem::EnvUserName(QStringLiteral("missing")).isEmpty());

  QStringList Profiles = S3FileSystem::Profiles();
  CHECK_MSG(Profiles == QStringList({QStringLiteral("chained"), QStringLiteral("default"), QStringLiteral("Dev"), QStringLiteral("fromenv")}),
    Profiles.join(QLatin1Char(',')));
}

//---------------------------------------------------------------------------
// Server
//---------------------------------------------------------------------------

class TestUserInterface : public UserInterface
{
public:
  QMutex Mutex;
  QStringList PromptAnswers;
  QList<PromptKind> PromptKinds;
  int Queries = 0;

  HostKeyAnswer VerifyHostKey(const QString &, int, const QString &, const QString &, const QString &, HostKeyState) override
  {
    return HostKeyCancel;
  }

  bool PromptUser(PromptKind Kind, const QString &, const QString &, QList<Prompt> & Prompts) override
  {
    QMutexLocker Lock(&Mutex);
    PromptKinds.append(Kind);
    for (Prompt & P : Prompts)
    {
      if (PromptAnswers.isEmpty())
      {
        return false;
      }
      P.Result = PromptAnswers.takeFirst();
    }
    return true;
  }

  void DisplayBanner(const QString &, const QString &) override
  {
  }

  unsigned int QueryUser(const QString & Query, const QString &, unsigned int) override
  {
    QMutexLocker Lock(&Mutex);
    Queries++;
    Log(QStringLiteral("QueryUser \"%1...\" -> Yes").arg(Query.left(60).replace(QLatin1Char('\n'), QLatin1Char(' '))));
    return qaYes;
  }
};

struct Waiter
{
  QEventLoop Loop;
  OperationResult Result;
  QString Directory;
  RemoteFileList Files;
  bool Called = false;

  Terminal::Callback Cb()
  {
    return [this](const OperationResult & R)
    {
      Result = R;
      Called = true;
      Loop.quit();
    };
  }
  Terminal::ListCallback ListCb()
  {
    return [this](const OperationResult & R, const QString & Dir, const RemoteFileList & List)
    {
      Result = R;
      Directory = Dir;
      Files = List;
      Called = true;
      Loop.quit();
    };
  }
  OperationResult Wait(int TimeoutMs = 300000)
  {
    if (!Called)
    {
      QTimer::singleShot(TimeoutMs, &Loop, [this]() { std::printf("FAIL: operation timed out\n"); Failures++; Loop.quit(); });
      Loop.exec();
    }
    return Result;
  }
};

static QString Describe(const OperationResult & R)
{
  return QStringLiteral("ok=%1 aborted=%2 fatal=%3 msg=\"%4\" more=\"%5\"").arg(R.Ok).arg(R.Aborted).arg(R.Fatal).arg(R.Message, R.MoreMessages);
}

static const RemoteFile * Find(const RemoteFileList & Files, const QString & Name)
{
  for (const RemoteFile & F : Files)
  {
    if (F.FileName == Name)
    {
      return &F;
    }
  }
  return nullptr;
}

static QByteArray RandomData(qint64 Size)
{
  QByteArray Result(static_cast<qsizetype>(Size), Qt::Uninitialized);
  QRandomGenerator Generator(static_cast<quint32>(Size));
  for (qsizetype Index = 0; Index < Result.size(); Index++)
  {
    Result[Index] = static_cast<char>(Generator.bounded(256));
  }
  return Result;
}

static QByteArray ReadAll(const QString & FileName)
{
  QFile File(FileName);
  return File.open(QIODevice::ReadOnly) ? File.readAll() : QByteArray();
}

// Runs Body and returns the error message (empty = no exception)
template<typename F>
static QString Error(F Body)
{
  try
  {
    Body();
  }
  catch (const ExtException & E)
  {
    return E.Message() + QStringLiteral(" | ") + E.MoreMessages();
  }
  return QString();
}

static void TestServer(const SessionData & Data, const QString & Scratch)
{
  TestUserInterface UI;
  QString Bucket = QStringLiteral("nixscp-test-%1").arg(QRandomGenerator::global()->bounded(100000000));
  QString Root = QLatin1Char('/') + Bucket;

  std::printf("[server wrong secret]\n");
  {
    SessionData Wrong = Data;
    Wrong.Password = QStringLiteral("wrong secret");
    S3FileSystem Fs(Wrong, &UI);
    bool Fatal = false;
    try
    {
      Fs.Open();
    }
    catch (const FatalException & E)
    {
      Fatal = true;
      Log(E.Message() + QStringLiteral(" | ") + E.MoreMessages());
      CHECK(E.Message() == QStringLiteral("Connection failed."));
      CHECK_MSG(E.MoreMessages().contains(QStringLiteral("Authentication failed.")) || E.MoreMessages().contains(QStringLiteral("Access denied.")),
        E.MoreMessages());
    }
    CHECK(Fatal);
    CHECK(!Fs.IsOpened());
  }

  std::printf("[server prompt for secret]\n");
  {
    SessionData NoSecret = Data;
    NoSecret.Password.clear();
    UI.PromptAnswers = QStringList{Data.Password};
    UI.PromptKinds.clear();
    S3FileSystem Fs(NoSecret, &UI);
    QString E = Error([&]() { Fs.Open(); });
    CHECK_MSG(E.isEmpty(), E);
    CHECK(Fs.IsOpened());
    CHECK((UI.PromptKinds.size() == 1) && (UI.PromptKinds.value(0) == UserInterface::pkPassword));
    Log(Fs.ServerInfo().replace(QLatin1Char('\n'), QStringLiteral("; ")));
  }

  S3FileSystem Fs(Data, &UI);
  QString E = Error([&]() { Fs.Open(); });
  CHECK_MSG(E.isEmpty(), E);
  if (!E.isEmpty())
  {
    return;
  }
  Fs.SetMultipartThreshold(6 * 1024 * 1024, 5 * 1024 * 1024);

  std::printf("[server create bucket %s]\n", qPrintable(Bucket));
  E = Error([&]() { Fs.CreateDirectory(Root, 0755); });
  CHECK_MSG(E.isEmpty(), E);
  {
    RemoteFileList Files;
    E = Error([&]() { Files = Fs.ReadDirectory(QStringLiteral("/")); });
    CHECK_MSG(E.isEmpty(), E);
    const RemoteFile * File = Find(Files, Bucket);
    CHECK(File && File->IsDirectory);
    RemoteFile BucketFile;
    CHECK(Fs.FileExists(Root, &BucketFile) && BucketFile.IsDirectory);
    CHECK(!Fs.FileExists(QStringLiteral("/nixscp-missing-bucket-%1").arg(QRandomGenerator::global()->bounded(1000000)), nullptr));
    E = Error([&]() { Files = Fs.ReadDirectory(Root); });
    CHECK_MSG(E.isEmpty() && Files.isEmpty(), E);
    Log(QStringLiteral("bucket region: %1").arg(Fs.BucketRegion(Bucket)));
  }

  QString RegionBucket = qEnvironmentVariable("NIXSCP_TEST_S3_REGION_BUCKET");
  if (!RegionBucket.isEmpty())
  {
    // An existing bucket in a region other than the default one: detected
    // from the AuthorizationHeaderMalformed error (TS3FileSystem::GetBucketContext)
    std::printf("[server region detection]\n");
    QString Name = RegionBucket.section(QLatin1Char(':'), 0, 0);
    QString Region = RegionBucket.section(QLatin1Char(':'), 1);
    S3FileSystem RegionFs(Data, &UI);
    E = Error([&]() { RegionFs.Open(); });
    CHECK_MSG(E.isEmpty(), E);
    CHECK(RegionFs.BucketRegion(Name).isEmpty());
    E = Error([&]() { RegionFs.ReadDirectory(QLatin1Char('/') + Name); });
    CHECK_MSG(E.isEmpty(), E);
    CHECK_EQ(RegionFs.BucketRegion(Name), Region);
    RemoteFile File;
    CHECK(RegionFs.FileExists(QLatin1Char('/') + Name, &File) && File.IsDirectory);
  }

  std::printf("[server folders]\n");
  {
    E = Error([&]() { Fs.CreateDirectory(Root + QStringLiteral("/dir"), 0755); });
    CHECK_MSG(E.isEmpty(), E);
    E = Error([&]() { Fs.CreateDirectory(Root + QStringLiteral("/dir/sub dir"), 0755); });
    CHECK_MSG(E.isEmpty(), E);
    RemoteFileList Files;
    E = Error([&]() { Files = Fs.ReadDirectory(Root); });
    CHECK_MSG(E.isEmpty(), E);
    CHECK((Files.size() == 1) && Files.value(0).IsDirectory && (Files.value(0).FileName == QStringLiteral("dir")));
    E = Error([&]() { Files = Fs.ReadDirectory(Root + QStringLiteral("/dir")); });
    CHECK_MSG(E.isEmpty(), E);
    CHECK((Files.size() == 1) && Files.value(0).IsDirectory && (Files.value(0).FileName == QStringLiteral("sub dir")));
    RemoteFile File;
    CHECK(Fs.FileExists(Root + QStringLiteral("/dir/sub dir"), &File) && File.IsDirectory);
    E = Error([&]() { Files = Fs.ReadDirectory(Root + QStringLiteral("/dir/sub dir")); });
    CHECK_MSG(E.isEmpty() && Files.isEmpty(), E);
    E = Error([&]() { Fs.ReadDirectory(Root + QStringLiteral("/nonexistent")); });
    CHECK_MSG(E.startsWith(QStringLiteral("Error listing directory")), E);
    CHECK(!Fs.FileExists(Root + QStringLiteral("/nonexistent"), nullptr));
    E = Error([&]() { Fs.ReadFile(Root + QStringLiteral("/nonexistent")); });
    CHECK_MSG(E.contains(QStringLiteral("does not exist")), E);
  }

  QString SmallName = QString::fromUtf8("a b+c&d=e%f\xC3\xA9 (1).txt");
  QString SmallLocal = Scratch + QStringLiteral("/small.txt");
  QString SmallRemote = Root + QStringLiteral("/dir/") + SmallName;
  QByteArray SmallData = RandomData(300000);
  std::printf("[server upload/download]\n");
  {
    WriteFile(SmallLocal, SmallData);
    FileOperationProgress Progress;
    Progress.Start(FileOperationProgress::Copy, FileOperationProgress::ToRemote, 1);
    E = Error([&]() { Fs.Upload(SmallLocal, SmallRemote, 0644, Progress); });
    CHECK_MSG(E.isEmpty(), E);
    CHECK_EQ(Progress.Get().TotalTransferred, qint64(SmallData.size()));
    RemoteFile File;
    E = Error([&]() { File = Fs.ReadFile(SmallRemote); });
    CHECK_MSG(E.isEmpty(), E);
    CHECK_EQ(File.FileName, SmallName);
    CHECK_EQ(File.Size, qint64(SmallData.size()));
    CHECK(!File.IsDirectory);
    CHECK(File.Modification.isValid() && (qAbs(File.Modification.secsTo(QDateTime::currentDateTime())) < 600));
    RemoteFileList Files;
    E = Error([&]() { Files = Fs.ReadDirectory(Root + QStringLiteral("/dir")); });
    CHECK_MSG(E.isEmpty() && Find(Files, SmallName) && (Files.size() == 2), E);

    QString Downloaded = Scratch + QStringLiteral("/small.down");
    Progress.Start(FileOperationProgress::Copy, FileOperationProgress::ToLocal, 1);
    E = Error([&]() { Fs.Download(SmallRemote, Downloaded, Progress); });
    CHECK_MSG(E.isEmpty(), E);
    CHECK(ReadAll(Downloaded) == SmallData);
    CHECK_EQ(Progress.Get().TotalTransferred, qint64(SmallData.size()));

    // resume
    WriteFile(Downloaded, SmallData.left(100000));
    TransferParams Params;
    Params.ResumeOffset = 100000;
    Params.KeepPartial = true;
    Progress.Start(FileOperationProgress::Copy, FileOperationProgress::ToLocal, 1);
    Progress.AddResumed(Params.ResumeOffset);
    E = Error([&]() { Fs.Download(SmallRemote, Downloaded, Params, Progress); });
    CHECK_MSG(E.isEmpty(), E);
    CHECK(ReadAll(Downloaded) == SmallData);
    CHECK_EQ(Progress.Get().TotalTransferred, qint64(SmallData.size()));
    // resume of a complete file
    Params.ResumeOffset = SmallData.size();
    E = Error([&]() { Fs.Download(SmallRemote, Downloaded, Params, Progress); });
    CHECK_MSG(E.isEmpty(), E);
    CHECK(ReadAll(Downloaded) == SmallData);

    // missing source: the local file is not created
    QString Missing = Scratch + QStringLiteral("/missing.down");
    E = Error([&]() { Fs.Download(Root + QStringLiteral("/dir/missing"), Missing, Progress); });
    CHECK_MSG(E.startsWith(QStringLiteral("Error transferring file")), E);
    CHECK(!QFileInfo::exists(Missing));

    // empty file
    QString EmptyLocal = Scratch + QStringLiteral("/empty");
    WriteFile(EmptyLocal, QByteArray());
    E = Error([&]() { Fs.Upload(EmptyLocal, Root + QStringLiteral("/empty"), 0644, Progress); });
    CHECK_MSG(E.isEmpty(), E);
    WriteFile(Scratch + QStringLiteral("/empty.down"), "garbage");
    E = Error([&]() { Fs.Download(Root + QStringLiteral("/empty"), Scratch + QStringLiteral("/empty.down"), Progress); });
    CHECK_MSG(E.isEmpty(), E);
    CHECK(QFileInfo::exists(Scratch + QStringLiteral("/empty.down")) && (QFileInfo(Scratch + QStringLiteral("/empty.down")).size() == 0));

    // cancelled download
    Progress.Start(FileOperationProgress::Copy, FileOperationProgress::ToLocal, 1);
    Progress.Cancel();
    bool Aborted = false;
    try
    {
      Fs.Download(SmallRemote, Scratch + QStringLiteral("/cancelled"), Progress);
    }
    catch (const AbortException &)
    {
      Aborted = true;
    }
    catch (const ExtException & Ex)
    {
      Log(Ex.Message());
    }
    CHECK(Aborted);
    // the connection is usable afterwards
    CHECK(Fs.FileExists(SmallRemote, nullptr));

    // upload to the root is refused
    E = Error([&]() { Fs.Upload(SmallLocal, QStringLiteral("/file"), 0644, Progress); });
    CHECK_MSG(E.contains(QStringLiteral("Specify target bucket.")), E);
  }

  std::printf("[server multipart]\n");
  {
    QByteArray BigData = RandomData(13 * 1024 * 1024 + 12345);
    QString BigLocal = Scratch + QStringLiteral("/big.bin");
    WriteFile(BigLocal, BigData);
    FileOperationProgress Progress;
    Progress.Start(FileOperationProgress::Copy, FileOperationProgress::ToRemote, 1);
    E = Error([&]() { Fs.Upload(BigLocal, Root + QStringLiteral("/big.bin"), 0644, Progress); });
    CHECK_MSG(E.isEmpty(), E);
    CHECK_EQ(Progress.Get().TotalTransferred, qint64(BigData.size()));
    RemoteFile File;
    E = Error([&]() { File = Fs.ReadFile(Root + QStringLiteral("/big.bin")); });
    CHECK_MSG(E.isEmpty() && (File.Size == BigData.size()), E);
    Progress.Start(FileOperationProgress::Copy, FileOperationProgress::ToLocal, 1);
    E = Error([&]() { Fs.Download(Root + QStringLiteral("/big.bin"), Scratch + QStringLiteral("/big.down"), Progress); });
    CHECK_MSG(E.isEmpty(), E);
    CHECK(ReadAll(Scratch + QStringLiteral("/big.down")) == BigData);
    // speed limit
    Progress.Start(FileOperationProgress::Copy, FileOperationProgress::ToLocal, 1);
    Progress.SetCPSLimit(4 * 1024 * 1024);
    QElapsedTimer Timer;
    Timer.start();
    E = Error([&]() { Fs.Download(Root + QStringLiteral("/big.bin"), Scratch + QStringLiteral("/big.down"), Progress); });
    CHECK_MSG(E.isEmpty(), E);
    Log(QStringLiteral("13 MB at 4 MB/s limit took %1 ms").arg(Timer.elapsed()));
    CHECK(Timer.elapsed() > 2000);
    CHECK(ReadAll(Scratch + QStringLiteral("/big.down")) == BigData);

    // cancelled multipart upload: the upload is aborted, no object is created
    Progress.Start(FileOperationProgress::Copy, FileOperationProgress::ToRemote, 1);
    Progress.SetCPSLimit(2 * 1024 * 1024);
    std::thread Canceller([&Progress]() { QThread::msleep(700); Progress.Cancel(); });
    bool Aborted = false;
    try
    {
      Fs.Upload(BigLocal, Root + QStringLiteral("/cancelled.bin"), 0644, Progress);
    }
    catch (const AbortException &)
    {
      Aborted = true;
    }
    catch (const ExtException & Ex)
    {
      Log(Ex.Message() + QStringLiteral(" | ") + Ex.MoreMessages());
    }
    Canceller.join();
    CHECK(Aborted);
    CHECK(!Fs.FileExists(Root + QStringLiteral("/cancelled.bin"), nullptr));
  }

  std::printf("[server rename/copy]\n");
  {
    QString Renamed = Root + QStringLiteral("/renamed & moved.txt");
    E = Error([&]() { Fs.RenameFile(SmallRemote, Renamed); });
    CHECK_MSG(E.isEmpty(), E);
    CHECK(!Fs.FileExists(SmallRemote, nullptr));
    RemoteFile File;
    CHECK(Fs.FileExists(Renamed, &File) && (File.Size == SmallData.size()));
    E = Error([&]() { Fs.CopyFile(Renamed, SmallRemote); });
    CHECK_MSG(E.isEmpty(), E);
    FileOperationProgress Progress;
    Progress.Start(FileOperationProgress::Copy, FileOperationProgress::ToLocal, 1);
    E = Error([&]() { Fs.Download(SmallRemote, Scratch + QStringLiteral("/copy.down"), Progress); });
    CHECK_MSG(E.isEmpty(), E);
    CHECK(ReadAll(Scratch + QStringLiteral("/copy.down")) == SmallData);
    E = Error([&]() { Fs.RenameFile(Root + QStringLiteral("/dir"), Root + QStringLiteral("/dir2")); });
    CHECK_MSG(E.contains(QStringLiteral("Operation not supported.")), E);
    E = Error([&]() { Fs.ChangeMode(Renamed, 0600); });
    CHECK(!E.isEmpty());
    E = Error([&]() { Fs.SetModificationTime(Renamed, QDateTime::currentDateTime()); });
    CHECK(E.isEmpty());
    E = Error([&]() { Fs.CreateLink(Root + QStringLiteral("/link"), Renamed); });
    CHECK(!E.isEmpty());
    E = Error([&]() { Fs.DeleteFile(Renamed); });
    CHECK_MSG(E.isEmpty(), E);
    CHECK(!Fs.FileExists(Renamed, nullptr));
  }

  if (qEnvironmentVariable("NIXSCP_TEST_S3_MANY") != QStringLiteral("0"))
  {
    std::printf("[server listing pagination]\n");
    QString EmptyLocal = Scratch + QStringLiteral("/empty");
    FileOperationProgress Progress;
    Progress.Start(FileOperationProgress::Copy, FileOperationProgress::ToRemote, 1);
    const int Count = 1001;
    for (int Index = 0; (Index < Count) && E.isEmpty(); Index++)
    {
      E = Error([&]() { Fs.Upload(EmptyLocal, Root + QStringLiteral("/many/f%1").arg(Index, 4, 10, QLatin1Char('0')), 0644, Progress); });
    }
    CHECK_MSG(E.isEmpty(), E);
    E = Error([&]() { Fs.CreateDirectory(Root + QStringLiteral("/many/zz"), 0755); });
    RemoteFileList Files;
    E = Error([&]() { Files = Fs.ReadDirectory(Root + QStringLiteral("/many")); });
    CHECK_MSG(E.isEmpty(), E);
    CHECK_EQ(Files.size(), Count + 1);
    for (int Index = 0; (Index < Count) && E.isEmpty(); Index++)
    {
      E = Error([&]() { Fs.DeleteFile(Root + QStringLiteral("/many/f%1").arg(Index, 4, 10, QLatin1Char('0'))); });
    }
    CHECK_MSG(E.isEmpty(), E);
    E = Error([&]() { Fs.RemoveDirectory(Root + QStringLiteral("/many/zz")); });
    CHECK_MSG(E.isEmpty(), E);
    CHECK(!Fs.FileExists(Root + QStringLiteral("/many"), nullptr));
  }

  std::printf("[server terminal]\n");
  {
    // Through Terminal: default transfer settings (resumable ".filepart"
    // uploads renamed by copy + delete, timestamps "preserved")
    Terminal T(Data, &UI);
    Waiter W1;
    T.Open(W1.Cb());
    CHECK_MSG(W1.Wait().Ok, Describe(W1.Result));
    QString UpDir = Scratch + QStringLiteral("/up");
    QDir().mkpath(UpDir + QStringLiteral("/sub"));
    WriteFile(UpDir + QStringLiteral("/one.bin"), SmallData);
    WriteFile(UpDir + QStringLiteral("/sub/two.txt"), "two");
    CopyParam Param;
    Waiter W2;
    T.CopyToRemote({UpDir}, Root, Param, false, W2.Cb());
    CHECK_MSG(W2.Wait().Ok, Describe(W2.Result));
    Waiter W3;
    T.ReadDirectory(Root + QStringLiteral("/up"), W3.ListCb());
    CHECK_MSG(W3.Wait().Ok, Describe(W3.Result));
    const RemoteFile * One = Find(W3.Files, QStringLiteral("one.bin"));
    CHECK(One && (One->Size == SmallData.size()));
    CHECK(Find(W3.Files, QStringLiteral("sub")) && Find(W3.Files, QStringLiteral("sub"))->IsDirectory);
    CHECK(!Find(W3.Files, QStringLiteral("one.bin.filepart")));
    QString DownDir = Scratch + QStringLiteral("/down");
    QDir().mkpath(DownDir);
    Waiter W4;
    T.CopyToLocal({Root + QStringLiteral("/up")}, DownDir, Param, false, W4.Cb());
    CHECK_MSG(W4.Wait().Ok, Describe(W4.Result));
    CHECK(ReadAll(DownDir + QStringLiteral("/up/one.bin")) == SmallData);
    CHECK(ReadAll(DownDir + QStringLiteral("/up/sub/two.txt")) == QByteArray("two"));
    Waiter W5;
    T.DeleteFiles({Root + QStringLiteral("/up")}, W5.Cb());
    CHECK_MSG(W5.Wait().Ok, Describe(W5.Result));
  }

  std::printf("[server cleanup]\n");
  {
    for (const QString & Path : {SmallRemote, Root + QStringLiteral("/big.bin"), Root + QStringLiteral("/empty")})
    {
      E = Error([&]() { Fs.DeleteFile(Path); });
      CHECK_MSG(E.isEmpty(), E);
    }
    E = Error([&]() { Fs.RemoveDirectory(Root + QStringLiteral("/dir/sub dir")); });
    CHECK_MSG(E.isEmpty(), E);
    E = Error([&]() { Fs.RemoveDirectory(Root + QStringLiteral("/dir")); });
    CHECK_MSG(E.isEmpty(), E);
    RemoteFileList Files;
    E = Error([&]() { Files = Fs.ReadDirectory(Root); });
    CHECK_MSG(E.isEmpty() && Files.isEmpty(), E);
    for (const RemoteFile & File : Files)
    {
      Log(QStringLiteral("left over: %1").arg(File.FileName));
    }
    E = Error([&]() { Fs.RemoveDirectory(Root); });
    CHECK_MSG(E.isEmpty(), E);
    CHECK(!Fs.FileExists(Root, nullptr));
    Fs.Close();
    CHECK(!Fs.IsOpened());
  }
}

int main(int argc, char ** argv)
{
  QCoreApplication App(argc, argv);
  QTemporaryDir Scratch;
  CHECK(Scratch.isValid());

  TestSignature();
  TestXml();
  TestCredentials(Scratch.path());

  QString Endpoint = qEnvironmentVariable("NIXSCP_TEST_S3_ENDPOINT");
  if (Endpoint.isEmpty())
  {
    std::printf("test_s3: server part SKIPPED (set NIXSCP_TEST_S3_ENDPOINT=host:port [, NIXSCP_TEST_S3_ACCESS_KEY, NIXSCP_TEST_S3_SECRET_KEY, "
      "NIXSCP_TEST_S3_TLS, NIXSCP_TEST_S3_URL_STYLE, NIXSCP_TEST_S3_REGION])\n");
  }
  else
  {
    SessionData Data;
    Data.FSProtocol = fsS3;
    qsizetype Colon = Endpoint.lastIndexOf(QLatin1Char(':'));
    bool Tls = (qEnvironmentVariable("NIXSCP_TEST_S3_TLS") == QStringLiteral("1"));
    Data.Ftps = Tls ? ftpsImplicit : ftpsNone;
    Data.HostName = (Colon > 0) ? Endpoint.left(Colon) : Endpoint;
    Data.PortNumber = (Colon > 0) ? Endpoint.mid(Colon + 1).toInt() : SessionData::DefaultPort(fsS3, Data.Ftps);
    Data.UserName = qEnvironmentVariable("NIXSCP_TEST_S3_ACCESS_KEY", QStringLiteral("minioadmin"));
    Data.Password = qEnvironmentVariable("NIXSCP_TEST_S3_SECRET_KEY", QStringLiteral("minioadmin"));
    Data.S3UrlStyle = (qEnvironmentVariable("NIXSCP_TEST_S3_URL_STYLE") == QStringLiteral("virtual")) ? s3usVirtualHost : s3usPath;
    Data.S3DefaultRegion = qEnvironmentVariable("NIXSCP_TEST_S3_REGION");
    Data.Timeout = 15;
    TestServer(Data, Scratch.path());
  }

  if (Failures > 0)
  {
    std::printf("test_s3: %d FAILURE(S)\n", Failures);
    return 1;
  }
  std::printf("test_s3: all tests passed\n");
  return 0;
}
