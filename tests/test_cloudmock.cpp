// NixSCP Pro sync (core/CloudSync) against a server this test controls: one that
// fails, lies, rolls back or forks. Every computer is a separate process with its
// own configuration folder, as in test_cloudlive, and the mock runs in the parent.
//
//  - changing and replacing the keys, and computers left behind by that,
//  - rollback and forks of the settings and session slots,
//  - the sealed sync state, and a plain state file planted by someone else,
//  - sign-in tokens, conflicts, failures and sign-out during a sync,
//  - replacing the data key with the process killed at every step, and with
//    lost replies: the user keeps working keys and gets the recovery key,
//  - cloud data of an old format, and how it is left behind.
#include "core/CloudClient.h"
#include "core/CloudCrypto.h"
#include "core/CloudSettings.h"
#include "core/CloudSync.h"
#include "core/ConfigVault.h"
#include "core/Configuration.h"
#include "core/IniStorage.h"
#include "core/SessionData.h"
#include "core/TeamCrypto.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QPointer>
#include <QProcess>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <openssl/evp.h>

#include <csignal>
#include <cstdio>
#include <memory>
#include <utility>

static int Failures = 0;

#define CHECK(Cond) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); std::fflush(stdout); Failures++; } } while (0)

static const QString User = QStringLiteral("11111111-2222-3333-4444-555555555555");
static const QString Email = QStringLiteral("someone@nixscp.test");
static const QString Password1 = QStringLiteral("first mock sync password");
static const QString Password2 = QStringLiteral("second mock sync password");
static const QString Password3 = QStringLiteral("third mock sync password");
static const QByteArray Tabs1 = "[Sessions\\NixSCP%20Cloud%20Session/0000]\nHostName=tabs-one.example.com\nIsWorkspace=1\n";
static const QByteArray Tabs2 = "[Sessions\\NixSCP%20Cloud%20Session/0000]\nHostName=tabs-two.example.com\nIsWorkspace=1\n";

static bool WaitFor(const std::function<bool()> & Condition, int TimeoutMs = 20000)
{
  QElapsedTimer Timer;
  Timer.start();
  while (!Condition() && (Timer.elapsed() < TimeoutMs))
  {
    QEventLoop Loop;
    QTimer::singleShot(20, &Loop, &QEventLoop::quit);
    Loop.exec();
  }
  return Condition();
}

static QByteArray ReadRaw(const QString & FileName)
{
  QFile File(FileName);
  return File.open(QIODevice::ReadOnly) ? File.readAll() : QByteArray();
}

static void WriteRaw(const QString & FileName, const QByteArray & Data)
{
  QFile File(FileName);
  CHECK(File.open(QIODevice::WriteOnly | QIODevice::Truncate));
  File.write(Data);
}

//---------------------------------------------------------------------------
// The server

class MockCloud : public QObject
{
public:
  QMap<QString, QMap<qint64, QByteArray>> History;
  QHash<QString, qint64> Heads;
  // What is told about the head, instead of the truth: rollback
  QHash<QString, qint64> ServedHead;
  // Status for requests with a path containing the key
  QHash<QString, int> Fail;
  QHash<QString, int> FailPut;
  // Milliseconds before an answer
  QHash<QString, int> Delay;
  // A download announced as this size (0 = the truth), and larger than announced
  qint64 AnnouncedSize = 0;
  QJsonObject Keys;
  int Commits = 0;
  int Replays = 0;
  int KeyPuts = 0;
  int Requests = 0;
  bool AskIfNoneMatch = false;
  bool SawIfNoneMatch = false;
  // The next commit: done, or not, and the connection drops either way
  bool LoseCommitReply = false;
  bool DropCommit = false;
  // ...and the server is gone from then on
  bool GoneAfterCommit = false;
  // Someone else's version arrives just before the next commit
  QByteArray BeatCommit;

  bool Listen()
  {
    connect(&FServer, &QTcpServer::newConnection, this, [this]() { Accept(); });
    return FServer.listen(QHostAddress::LocalHost, 0);
  }

  QString Url() const { return QStringLiteral("http://127.0.0.1:%1").arg(FServer.serverPort()); }

  qint64 Head(const QString & Slot = QStringLiteral("settings")) const { return Heads.value(Slot); }

  QByteArray HeadData(const QString & Slot = QStringLiteral("settings")) const
  {
    return History.value(Slot).value(Head(Slot));
  }

  void Inject(const QString & Slot, const QByteArray & Data)
  {
    qint64 Version = Heads.value(Slot) + 1;
    Heads[Slot] = Version;
    History[Slot][Version] = Data;
  }

  void Reset()
  {
    History.clear();
    Heads.clear();
    ServedHead.clear();
    Fail.clear();
    FailPut.clear();
    Delay.clear();
    AnnouncedSize = 0;
    Keys = QJsonObject();
    Commits = Replays = KeyPuts = Requests = 0;
    AskIfNoneMatch = SawIfNoneMatch = false;
    LoseCommitReply = DropCommit = GoneAfterCommit = false;
    BeatCommit.clear();
    FLive.clear();
    FUsed.clear();
    FAccess.clear();
    FObjects.clear();
  }

private:
  QTcpServer FServer;
  QHash<QTcpSocket *, QByteArray> FBuffers;
  QSet<QByteArray> FLive;
  QSet<QByteArray> FUsed;
  QSet<QByteArray> FAccess;
  QHash<QString, QByteArray> FObjects;
  int FCounter = 0;
  QString FVanishedCommit;

  void Accept()
  {
    while (QTcpSocket * Socket = FServer.nextPendingConnection())
    {
      connect(Socket, &QTcpSocket::readyRead, this, [this, Socket]() { Read(Socket); });
      connect(Socket, &QTcpSocket::disconnected, Socket, &QObject::deleteLater);
      connect(Socket, &QObject::destroyed, this, [this, Socket]() { FBuffers.remove(Socket); });
    }
  }

  void Read(QTcpSocket * Socket)
  {
    QByteArray & Buffer = FBuffers[Socket];
    Buffer += Socket->readAll();
    qsizetype End = Buffer.indexOf("\r\n\r\n");
    if (End < 0)
    {
      return;
    }
    const QByteArray Head = Buffer.left(End);
    qint64 Length = 0;
    bool IfNoneMatch = false;
    const QList<QByteArray> Lines = Head.split('\n');
    for (const QByteArray & Line : Lines)
    {
      QByteArray Lower = Line.trimmed().toLower();
      if (Lower.startsWith("content-length:"))
      {
        Length = Lower.mid(15).trimmed().toLongLong();
      }
      IfNoneMatch = IfNoneMatch || Lower.startsWith("if-none-match:");
    }
    if (Buffer.size() - End - 4 < Length)
    {
      return;
    }
    QByteArray Body = Buffer.mid(End + 4, Length);
    QList<QByteArray> Request = Lines.value(0).trimmed().split(' ');
    QByteArray Auth;
    for (const QByteArray & Line : Lines)
    {
      if (Line.toLower().startsWith("authorization:"))
      {
        Auth = Line.mid(14).trimmed();
      }
    }
    Buffer.clear();
    Handle(Socket, Request.value(0), QString::fromLatin1(Request.value(1)), Auth, Body, IfNoneMatch);
  }

  void Send(QTcpSocket * Socket, int Status, const QByteArray & Data, const QString & Path, const QByteArray & Type = "application/json")
  {
    QByteArray Response = "HTTP/1.1 " + QByteArray::number(Status) + " X\r\nContent-Type: " + Type +
      "\r\nConnection: close\r\nContent-Length: " + QByteArray::number(Data.size()) + "\r\n\r\n" + Data;
    int Wait = 0;
    for (auto It = Delay.constBegin(); It != Delay.constEnd(); ++It)
    {
      if (Path.contains(It.key()))
      {
        Wait = It.value();
      }
    }
    QPointer<QTcpSocket> Guard(Socket);
    QTimer::singleShot(Wait, this, [Guard, Response]()
    {
      if (Guard)
      {
        Guard->write(Response);
        Guard->disconnectFromHost();
      }
    });
  }

  void SendJson(QTcpSocket * Socket, int Status, const QJsonObject & Json, const QString & Path)
  {
    Send(Socket, Status, QJsonDocument(Json).toJson(QJsonDocument::Compact), Path);
  }

  void Vanish(QTcpSocket * Socket, const QJsonObject & Commit)
  {
    FVanishedCommit = Commit.value(QStringLiteral("object_key")).toString();
    if (GoneAfterCommit)
    {
      Fail[QStringLiteral("/v1/")] = 503;
    }
    Socket->abort();
  }

  bool ValidReplacement(const QJsonObject & Json) const
  {
    auto Text = [&Json](const char * Name) { return Json.value(QString::fromLatin1(Name)).toString(); };
    if ((Text("public_key") != Keys.value(QStringLiteral("public_key")).toString()) ||
        (Text("identity_key") != Keys.value(QStringLiteral("identity_key")).toString()))
    {
      return false;
    }
    QByteArray Signed = (Text("public_key") + QLatin1Char('|') + Text("identity_key") + QLatin1Char('|') + Text("wrapped_private_key")).toUtf8();
    QByteArray Message = "NXT2-KEYS-REPLACE|" + User.toUtf8() + '|' + CloudSettings::Hash(Signed).toLatin1();
    QByteArray Public = QByteArray::fromBase64(Text("identity_key").toLatin1());
    QByteArray Signature = QByteArray::fromBase64(Text("replace_signature").toLatin1());
    EVP_PKEY * Key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
      reinterpret_cast<const unsigned char *>(Public.constData()), static_cast<size_t>(Public.size()));
    EVP_MD_CTX * Ctx = EVP_MD_CTX_new();
    bool Ok = (Key != nullptr) && (Ctx != nullptr) && (EVP_DigestVerifyInit(Ctx, nullptr, nullptr, nullptr, Key) == 1) &&
      (EVP_DigestVerify(Ctx, reinterpret_cast<const unsigned char *>(Signature.constData()), static_cast<size_t>(Signature.size()),
        reinterpret_cast<const unsigned char *>(Message.constData()), static_cast<size_t>(Message.size())) == 1);
    EVP_MD_CTX_free(Ctx);
    EVP_PKEY_free(Key);
    return Ok;
  }

  QJsonObject IssueTokens()
  {
    QByteArray Access = "access-" + QByteArray::number(++FCounter);
    QByteArray Refresh = "refresh-" + QByteArray::number(++FCounter);
    FAccess.insert(Access);
    FLive.insert(Refresh);
    return QJsonObject{{QStringLiteral("access_token"), QString::fromLatin1(Access)}, {QStringLiteral("refresh_token"), QString::fromLatin1(Refresh)}};
  }

  QJsonObject HeadJson(const QString & Slot) const
  {
    qint64 Version = ServedHead.contains(Slot) ? ServedHead.value(Slot) : Head(Slot);
    return QJsonObject{{QStringLiteral("version"), Version}, {QStringLiteral("device"), QStringLiteral("mock")},
      {QStringLiteral("updated_at"), QStringLiteral("2026-11-05T10:00:00.000Z")}};
  }

  void Handle(QTcpSocket * Socket, const QByteArray & Method, const QString & Target, const QByteArray & Auth, const QByteArray & Body, bool IfNoneMatch)
  {
    Requests++;
    QUrl Url(Target);
    QString Path = Url.path();
    QUrlQuery Query(Url);
    QJsonObject Json = QJsonDocument::fromJson(Body).object();
    QString Slot = Json.contains(QStringLiteral("slot")) ? Json.value(QStringLiteral("slot")).toString() : Query.queryItemValue(QStringLiteral("slot"));
    for (auto It = Fail.constBegin(); It != Fail.constEnd(); ++It)
    {
      if (Path.contains(It.key()))
      {
        Send(Socket, It.value(), "{}", Path);
        return;
      }
    }
    if ((Method == "PUT") && FailPut.contains(Path))
    {
      Send(Socket, FailPut.value(Path), "{}", Path);
      return;
    }
    if (Path.startsWith(QStringLiteral("/__store/")))
    {
      QString Key = Path.mid(9);
      if (Method == "PUT")
      {
        SawIfNoneMatch = SawIfNoneMatch || IfNoneMatch;
        FObjects[Key] = Body;
        Send(Socket, 200, "{}", Path);
        return;
      }
      QStringList Parts = Key.split(QLatin1Char('-'));
      QByteArray Data = History.value(Parts.value(0)).value(Parts.value(1).toLongLong());
      Send(Socket, 200, (AnnouncedSize > 0) ? Data + QByteArray(1024 * 1024, 'x') : Data, Path, "application/octet-stream");
      return;
    }
    if (Path == QStringLiteral("/v1/oauth/token"))
    {
      if (Json.value(QStringLiteral("grant_type")).toString() == QStringLiteral("authorization_code"))
      {
        SendJson(Socket, 200, IssueTokens(), Path);
        return;
      }
      QByteArray Token = Json.value(QStringLiteral("refresh_token")).toString().toLatin1();
      if (FUsed.contains(Token))
      {
        Replays++;
        SendJson(Socket, 400, {{QStringLiteral("error"), QStringLiteral("invalid_grant")}, {QStringLiteral("reason"), QStringLiteral("token_reused")}}, Path);
        return;
      }
      if (!FLive.remove(Token))
      {
        SendJson(Socket, 400, {{QStringLiteral("error"), QStringLiteral("invalid_grant")}}, Path);
        return;
      }
      FUsed.insert(Token);
      SendJson(Socket, 200, IssueTokens(), Path);
      return;
    }
    if (!Auth.startsWith("Bearer ") || !FAccess.contains(Auth.mid(7)))
    {
      SendJson(Socket, 401, {{QStringLiteral("error"), QStringLiteral("invalid_token")}}, Path);
      return;
    }
    if (Path == QStringLiteral("/v1/me"))
    {
      SendJson(Socket, 200, {{QStringLiteral("user"), QJsonObject{{QStringLiteral("uuid"), User}, {QStringLiteral("email"), Email}}},
        {QStringLiteral("license"), QJsonObject{{QStringLiteral("active"), true}, {QStringLiteral("plan"), QStringLiteral("lifetime")}}}}, Path);
    }
    else if (Path == QStringLiteral("/v1/oauth/revoke"))
    {
      SendJson(Socket, 200, {{QStringLiteral("ok"), true}}, Path);
    }
    else if (Path == QStringLiteral("/v1/sync/head"))
    {
      SendJson(Socket, 200, HeadJson(Slot), Path);
    }
    else if (Path == QStringLiteral("/v1/sync/history"))
    {
      QJsonArray List;
      const QList<qint64> Versions = History.value(Slot).keys();
      for (qint64 Version : Versions)
      {
        List.append(QJsonObject{{QStringLiteral("version"), Version}});
      }
      Send(Socket, 200, QJsonDocument(List).toJson(QJsonDocument::Compact), Path);
    }
    else if (Path == QStringLiteral("/v1/sync/download-url"))
    {
      qint64 Version = Query.queryItemValue(QStringLiteral("version")).toLongLong();
      if (!History.value(Slot).contains(Version))
      {
        SendJson(Socket, 404, {{QStringLiteral("error"), QStringLiteral("not_found")}}, Path);
        return;
      }
      QByteArray Data = History.value(Slot).value(Version);
      SendJson(Socket, 200, {{QStringLiteral("version"), Version}, {QStringLiteral("size"), (AnnouncedSize > 0) ? AnnouncedSize : Data.size()},
        {QStringLiteral("sha256"), CloudSettings::Hash(Data)},
        {QStringLiteral("url"), QStringLiteral("%1/__store/%2-%3").arg(this->Url(), Slot).arg(Version)}}, Path);
    }
    else if (Path == QStringLiteral("/v1/sync/upload-url"))
    {
      if (Json.value(QStringLiteral("base_version")).toInteger() != Head(Slot))
      {
        SendJson(Socket, 409, {{QStringLiteral("error"), QStringLiteral("version_conflict")}, {QStringLiteral("head"), HeadJson(Slot)}}, Path);
        return;
      }
      QString Key = QStringLiteral("up_%1").arg(++FCounter);
      QJsonObject Reply{{QStringLiteral("object_key"), Key}, {QStringLiteral("url"), QStringLiteral("%1/__store/%2").arg(this->Url(), Key)}};
      if (AskIfNoneMatch)
      {
        Reply.insert(QStringLiteral("headers"), QJsonObject{{QStringLiteral("If-None-Match"), QStringLiteral("*")}});
      }
      SendJson(Socket, 200, Reply, Path);
    }
    else if (Path == QStringLiteral("/v1/sync/commit"))
    {
      if (!BeatCommit.isEmpty())
      {
        Inject(Slot, std::exchange(BeatCommit, QByteArray()));
      }
      // Qt before 6.10 sends a POST again by itself when the connection drops: the
      // connection is as bad for that one, and the client learns what it would learn on a newer Qt
      if (DropCommit || (Json.value(QStringLiteral("object_key")).toString() == FVanishedCommit))
      {
        DropCommit = false;
        Vanish(Socket, Json);
        return;
      }
      if (Json.value(QStringLiteral("base_version")).toInteger() != Head(Slot))
      {
        SendJson(Socket, 409, {{QStringLiteral("error"), QStringLiteral("version_conflict")}, {QStringLiteral("head"), HeadJson(Slot)}}, Path);
        return;
      }
      Commits++;
      Inject(Slot, FObjects.value(Json.value(QStringLiteral("object_key")).toString()));
      if (LoseCommitReply)
      {
        LoseCommitReply = false;
        Vanish(Socket, Json);
        return;
      }
      SendJson(Socket, 200, {{QStringLiteral("version"), Head(Slot)}}, Path);
    }
    else if ((Path == QStringLiteral("/v1/keys/me")) && (Method == "GET"))
    {
      if (Keys.isEmpty())
      {
        SendJson(Socket, 404, {{QStringLiteral("error"), QStringLiteral("no_keys")}}, Path);
      }
      else
      {
        SendJson(Socket, 200, Keys, Path);
      }
    }
    else if ((Path == QStringLiteral("/v1/keys/me")) && (Method == "PUT"))
    {
      // As the real server: a session alone cannot replace stored keys
      if (!Keys.isEmpty() && !ValidReplacement(Json))
      {
        SendJson(Socket, 403, {{QStringLiteral("error"), QStringLiteral("bad_signature")}}, Path);
        return;
      }
      KeyPuts += Keys.isEmpty() ? 0 : 1;
      Keys = Json;
      Keys.remove(QStringLiteral("replace_signature"));
      SendJson(Socket, 200, {{QStringLiteral("ok"), true}}, Path);
    }
    else
    {
      SendJson(Socket, 404, {{QStringLiteral("error"), QStringLiteral("not_found")}}, Path);
    }
  }
};

//---------------------------------------------------------------------------
// One computer, in a process of its own

// Seen since the start of this computer
static bool ConflictSeen = false;
static bool ForeignSeen = false;
static int KeyChangesSettled = 0;
static bool KeyChangeReplaced = false;
static QString KeyChangeMessage;
// The step of a key change that kills this process
static QByteArray KillAt;

static void Trace(const char * Where)
{
  CloudSync & Sync = CloudSync::Instance();
  std::printf("  [%s] state=%d status=%s\n", Where, int(Sync.State()), qPrintable(Sync.StatusText()));
  std::fflush(stdout);
}

static bool NotSyncing()
{
  return CloudSync::Instance().State() != CloudSync::csSyncing;
}

// What happens by itself at the start is over
static void Settle()
{
  QEventLoop Loop;
  QTimer::singleShot(60, &Loop, &QEventLoop::quit);
  Loop.exec();
  CHECK(WaitFor(NotSyncing));
}

static bool Synced()
{
  CloudSync::Instance().SyncNow();
  bool Done = WaitFor(NotSyncing);
  if (CloudSync::Instance().State() != CloudSync::csIdle)
  {
    Trace("not idle");
  }
  return Done && (CloudSync::Instance().State() == CloudSync::csIdle);
}

static bool SignIn()
{
  CloudSync & Sync = CloudSync::Instance();
  CloudSync::SignInRequest Request = Sync.NewSignInRequest(53682);
  auto Done = std::make_shared<int>(0);
  Sync.CompleteSignIn(Request, QStringLiteral("a code"), [Done](bool Ok, const QString &) { *Done = Ok ? 1 : 2; });
  WaitFor([Done]() { return *Done != 0; });
  return *Done == 1;
}

static void AddSite(const QString & Name)
{
  SessionData Site;
  Site.Name = Name;
  Site.HostName = Name.toLower().replace(QLatin1Char('/'), QLatin1Char('-')) + QStringLiteral(".example.com");
  Site.FSProtocol = fsSFTP;
  Configuration::Instance().StoredSessions().Add(std::make_shared<SessionData>(Site));
  CHECK(Configuration::Instance().Save());
}

static bool HasSite(const QString & Name)
{
  return Configuration::Instance().StoredSessions().FindByName(Name) != nullptr;
}

static QString ConfigDir()
{
  return QFileInfo(Configuration::Instance().IniFileName()).absolutePath();
}

static QByteArray SealedConfig()
{
  return ReadRaw(Configuration::Instance().IniFileName());
}

// Returns (ok, result)
static QPair<bool, QString> ChangeKeyAndWait(const CloudSync::KeyChange & Change)
{
  auto Result = std::make_shared<QPair<bool, QString>>(false, QString());
  auto Done = std::make_shared<bool>(false);
  CloudSync::Instance().ChangeKey(Change, [Result, Done](bool Ok, const QString & Text)
  {
    *Result = qMakePair(Ok, Text);
    *Done = true;
  });
  CHECK(WaitFor([Done]() { return *Done; }));
  return *Result;
}

static bool Unlock(const QString & Secret, bool IsRecoveryKey = false)
{
  auto Result = std::make_shared<int>(0);
  CloudSync::Instance().UnlockKey(Secret, IsRecoveryKey, [Result](bool Ok, const QString &) { *Result = Ok ? 1 : 2; });
  WaitFor([Result]() { return *Result != 0; });
  return *Result == 1;
}

static bool ConfigOpensWith(const QString & Secret)
{
  QByteArray Plain;
  return ConfigVault::OpenWithSecret(SealedConfig(), Secret, false, Plain);
}

static void StepFirst()
{
  CloudSync & Sync = CloudSync::Instance();
  CHECK(SignIn());
  CHECK(Sync.State() == CloudSync::csNeedKey);
  AddSite(QStringLiteral("Mock/A1"));
  auto Recovery = std::make_shared<QString>();
  Sync.CreateKey(Password1, true, [Recovery](bool Ok, const QString & Result) { CHECK(Ok); *Recovery = Result; });
  CHECK(WaitFor(NotSyncing));
  CHECK(Sync.State() == CloudSync::csIdle);
  CHECK(!Recovery->isEmpty());
  WriteRaw(ConfigDir() + QStringLiteral("/recovery.txt"), Recovery->toLatin1());
  CHECK(Sync.PendingRecoveryKey() == *Recovery);
  Sync.RecoveryKeySaved();
  Sync.FlushSession(Tabs1, 5000);
}

static void StepSecond()
{
  CHECK(SignIn());
  CHECK(Unlock(Password1));
  CHECK(WaitFor([]() { return NotSyncing() && HasSite(QStringLiteral("Mock/A1")); }));
  CHECK(CloudSync::Instance().State() == CloudSync::csIdle);
}

static void StepTeamKey()
{
  // The private key of Teams on the server, under the data key
  CloudSync & Sync = CloudSync::Instance();
  TeamCrypto::KeyPair Keys = TeamCrypto::NewKeyPair();
  QByteArray Identity = CloudCrypto::RandomBytes(32);
  QByteArray Private = Keys.Private + Identity;
  auto Done = std::make_shared<bool>(false);
  Sync.Client().Api("PUT", QStringLiteral("/v1/keys/me"),
    {{QStringLiteral("public_key"), QString::fromLatin1(Keys.Public.toBase64())},
     {QStringLiteral("identity_key"), QString::fromLatin1(CloudCrypto::Ed25519Public(Identity).toBase64())},
     {QStringLiteral("key_signature"), QStringLiteral("c2lnbmF0dXJl")},
     {QStringLiteral("wrapped_private_key"), QString::fromLatin1(CloudCrypto::Seal(Sync.DataKey(), Private, QByteArray()).toBase64())}},
    [Done](const CloudClient::Reply & Reply) { CHECK(Reply.Ok()); *Done = true; });
  CHECK(WaitFor([Done]() { return *Done; }));
  WriteRaw(ConfigDir() + QStringLiteral("/team-private.bin"), Private);
}

static void StepCheckTeamKey()
{
  CloudSync & Sync = CloudSync::Instance();
  QByteArray Private = ReadRaw(ConfigDir() + QStringLiteral("/team-private.bin"));
  auto Done = std::make_shared<bool>(false);
  Sync.Client().Api("GET", QStringLiteral("/v1/keys/me"), [&Sync, Private, Done](const CloudClient::Reply & Reply)
  {
    CHECK(Reply.Ok());
    QByteArray Wrapped = QByteArray::fromBase64(Reply.Json.value(QStringLiteral("wrapped_private_key")).toString().toLatin1());
    QByteArray Opened;
    CHECK(CloudCrypto::Open(Sync.DataKey(), Wrapped, QByteArray(), Opened) && (Opened == Private));
    *Done = true;
  });
  CHECK(WaitFor([Done]() { return *Done; }));
}

static void StepAdd(const QString & Name)
{
  AddSite(Name);
  CHECK(Synced());
}

static void StepHasSite(const QString & Name)
{
  CHECK(Synced());
  CHECK(HasSite(Name));
}

static void StepChangePassword()
{
  CloudSync & Sync = CloudSync::Instance();
  QByteArray Key = Sync.DataKey();
  // Without the current secret nothing happens: an unlocked session is not enough
  CloudSync::KeyChange Wrong;
  Wrong.Current = QStringLiteral("not the password");
  Wrong.NewPassword = Password2;
  QPair<bool, QString> Refused = ChangeKeyAndWait(Wrong);
  CHECK(!Refused.first && Refused.second.contains(QStringLiteral("not correct")));
  CHECK(ConfigOpensWith(Password1) && !ConfigOpensWith(Password2));
  // Too weak is refused here as well as in the interface
  CloudSync::KeyChange Weak;
  Weak.Current = Password1;
  Weak.NewPassword = QStringLiteral("password");
  CHECK(!ChangeKeyAndWait(Weak).first);
  CloudSync::KeyChange Change;
  Change.Current = Password1;
  Change.NewPassword = Password2;
  QPair<bool, QString> Result = ChangeKeyAndWait(Change);
  CHECK(Result.first && Result.second.isEmpty());
  CHECK(WaitFor(NotSyncing));
  CHECK(Sync.State() == CloudSync::csIdle);
  CHECK(Sync.DataKey() == Key);
  CHECK(ConfigOpensWith(Password2) && !ConfigOpensWith(Password1));
}

static void StepKeepLocal()
{
  // An old computer that missed the new password: edited, conflict, "keep mine"
  CloudSync & Sync = CloudSync::Instance();
  CHECK(ConflictSeen);
  Sync.ResolveConflict(CloudSync::crUseLocal);
  CHECK(WaitFor([&Sync]() { return Sync.State() == CloudSync::csIdle; }));
  // It took the new password over, here as well
  CHECK(ConfigOpensWith(Password2) && !ConfigOpensWith(Password1));
}

static void WaitForJournal();

static void StepRotate()
{
  CloudSync & Sync = CloudSync::Instance();
  QByteArray OldKey = Sync.DataKey();
  QByteArray OldConfig = SealedConfig();
  CloudSync::KeyChange Change;
  Change.Current = Password1;
  Change.NewPassword = Password2;
  Change.RotateDataKey = true;
  QPair<bool, QString> Result = ChangeKeyAndWait(Change);
  CHECK(Result.first);
  CHECK(!Result.second.isEmpty());  // the old recovery key cannot open a new key
  WriteRaw(ConfigDir() + QStringLiteral("/recovery2.txt"), Result.second.toLatin1());
  CHECK(Sync.HasKey());
  CHECK(Sync.DataKey() != OldKey);
  CHECK(ConfigVault::Key() == Sync.DataKey());
  CHECK(Sync.State() == CloudSync::csIdle);
  // The local files followed
  CHECK(ConfigOpensWith(Password2) && !ConfigOpensWith(Password1));
  QByteArray Plain;
  CHECK(ConfigVault::OpenWithSecret(SealedConfig(), Result.second, true, Plain));
  CHECK(!ConfigVault::OpenWithSecret(SealedConfig(), QString::fromLatin1(ReadRaw(ConfigDir() + QStringLiteral("/recovery.txt"))), true, Plain));
  CHECK(!ConfigVault::Open(OldConfig, Plain));
  QByteArray Sealed = ReadRaw(ConfigDir() + QStringLiteral("/nixscp-cloud.ini"));
  CHECK(ConfigVault::OpenFile(ConfigDir() + QStringLiteral("/nixscp-cloud.ini"), Sealed, Plain));
  QByteArray TabsFile = ReadRaw(ConfigDir() + QStringLiteral("/nixscp-cloud-session.ini"));
  CHECK(ConfigVault::OpenFile(ConfigDir() + QStringLiteral("/nixscp-cloud-session.ini"), TabsFile, Plain) && (Plain == Tabs1));
  // The open tabs go up under the new key too
  QEventLoop Loop;
  QTimer::singleShot(500, &Loop, &QEventLoop::quit);
  Loop.exec();
}

static CloudSync::KeyChange RotationToSecond()
{
  CloudSync::KeyChange Change;
  Change.Current = Password1;
  Change.NewPassword = Password2;
  Change.RotateDataKey = true;
  return Change;
}

static void StepRotateKilled(const QString & Where)
{
  // Dies on the way, as a power cut would
  KillAt = Where.toLatin1();
  ConfigVault::KeyChangeStep = [](const char * Step)
  {
    if (KillAt == Step)
    {
      std::raise(SIGKILL);
    }
  };
  ChangeKeyAndWait(RotationToSecond());
  CHECK(false);
}

// What a user sees of a replacement whose reply was lost
static void StepRotateLost(const QString & Expected)
{
  CloudSync & Sync = CloudSync::Instance();
  QByteArray Key = Sync.DataKey();
  QPair<bool, QString> Result = ChangeKeyAndWait(RotationToSecond());
  std::printf("  ChangeKey: %d %s\n", Result.first, qPrintable(Result.second));
  CHECK(WaitFor(NotSyncing));
  if (Expected == QStringLiteral("replaced"))
  {
    CHECK(Result.first && !Result.second.isEmpty());
    CHECK(Sync.DataKey() != Key);
    CHECK(Sync.PendingRecoveryKey() == Result.second);
    CHECK(ConfigOpensWith(Password2) && !ConfigOpensWith(Password1));
    CHECK(Sync.State() == CloudSync::csIdle);
    return;
  }
  CHECK(!Result.first && !Result.second.isEmpty());
  CHECK(Sync.PendingRecoveryKey().isEmpty());
  if (Expected == QStringLiteral("refused"))
  {
    CHECK(Result.second.contains(QStringLiteral("not replaced")));
    CHECK(Sync.DataKey() == Key);
    CHECK(ConfigOpensWith(Password1));
  }
  else if (Expected == QStringLiteral("taken-over"))
  {
    CHECK(Result.second.contains(QStringLiteral("not replaced")));
    CHECK(!Sync.HasKey());
    CHECK(Sync.State() == CloudSync::csNeedKey);
  }
  else
  {
    // Undecided: nothing moved here, and nothing else may change meanwhile
    CHECK(Result.second.contains(QStringLiteral("finds out")));
    CHECK(Sync.DataKey() == Key);
    CHECK(ConfigOpensWith(Password1));
    QPair<bool, QString> Second = ChangeKeyAndWait(RotationToSecond());
    CHECK(!Second.first && Second.second.contains(QStringLiteral("not finished")));
  }
}

// The journal has nothing left: what was on its way to the server arrived
static void WaitForJournal()
{
  CloudSync::Instance().RecoveryKeySaved();
  CHECK(WaitFor([]() { return !QFile::exists(ConfigDir() + QStringLiteral("/nixscp-cloud-keys.ini")); }));
}

// A restart after a replacement that did not get to its end
static void StepSettled(const QString & Outcome)
{
  CloudSync & Sync = CloudSync::Instance();
  CHECK(WaitFor(NotSyncing));
  Trace("settled");
  CHECK(Sync.HasKey());
  CHECK(ConfigVault::Key() == Sync.DataKey());
  CHECK(ConfigOpensWith(qEnvironmentVariable("STEP_SECRET")) || (Outcome == QStringLiteral("landed")));
  CHECK(Sync.State() == CloudSync::csIdle);
  QByteArray Plain;
  if (Outcome == QStringLiteral("refused"))
  {
    CHECK(Sync.PendingRecoveryKey().isEmpty());
    CHECK(ConfigOpensWith(Password1) && !ConfigOpensWith(Password2));
    return;
  }
  // The new password and the new recovery key are what works now
  CHECK(ConfigOpensWith(Password2) && !ConfigOpensWith(Password1));
  QString Recovery = Sync.PendingRecoveryKey();
  CHECK(!Recovery.isEmpty());
  CHECK(ConfigVault::OpenWithSecret(SealedConfig(), Recovery, true, Plain));
  WriteRaw(ConfigDir() + QStringLiteral("/recovery2.txt"), Recovery.toLatin1());
  // Everything of this computer opens with the key in use, the state for the next start
  for (const char * Name : {"nixscp-cloud.ini", "nixscp-cloud-session.ini"})
  {
    QString File = ConfigDir() + QLatin1Char('/') + QLatin1String(Name);
    CHECK(ConfigVault::OpenFile(File, ReadRaw(File), Plain));
  }
  CHECK(!QFile::exists(ConfigDir() + QStringLiteral("/nixscp-cloud-bridge.ini")));
  Sync.RecoveryKeySaved();
  CHECK(Sync.PendingRecoveryKey().isEmpty());
  // The team keys are on their way; with them the journal is done
  WaitForJournal();
}

static void StepToldLater(const QString & Outcome)
{
  // The user was told an answer would come
  CHECK(KeyChangesSettled == 1);
  CHECK(KeyChangeReplaced == (Outcome == QStringLiteral("landed")));
  CHECK(KeyChangeMessage.contains(KeyChangeReplaced ? QStringLiteral("replaced after all") : QStringLiteral("not replaced")));
  StepSettled(Outcome);
}

static void StepNothingWaits()
{
  CloudSync & Sync = CloudSync::Instance();
  CHECK(Synced());
  CHECK(Sync.PendingRecoveryKey().isEmpty());
  CHECK(!QFile::exists(ConfigDir() + QStringLiteral("/nixscp-cloud-keys.ini")));
}

static void StepSignOut()
{
  CloudSync & Sync = CloudSync::Instance();
  Sync.SignOut();
  CHECK(WaitFor([&Sync]() { return !Sync.SignedIn(); }));
  CHECK(!Sync.HasKey());
}

static void StepBackIn(const QString & Password)
{
  CHECK(SignIn());
  CHECK(Unlock(Password));
  CHECK(WaitFor(NotSyncing));
  CHECK(CloudSync::Instance().State() == CloudSync::csIdle);
  WaitForJournal();
}

// The configuration cannot be saved, the disk is as good as full: a state sealed
// next to it would read as a replaced configuration at the next start
static void StepConfigNotWritten()
{
  CloudSync & Sync = CloudSync::Instance();
  QString Ini = Configuration::Instance().IniFileName();
  QString State = ConfigDir() + QStringLiteral("/nixscp-cloud.ini");
  CHECK(SignIn());
  CHECK(QDir().mkpath(Ini));
  auto Done = std::make_shared<bool>(false);
  Sync.CreateKey(Password1, true, [Done](bool, const QString &) { *Done = true; });
  CHECK(WaitFor([Done]() { return *Done; }));
  CHECK(WaitFor(NotSyncing));
  CHECK(!ConfigVault::IsSealedFile(State));
  CHECK(!ConfigVault::RefusesConfig(Ini));
  // Space again: the next save brings both
  CHECK(QDir().rmdir(Ini));
  Sync.SetRestoreSessions(true);
  CHECK(ConfigVault::IsSealedFile(Ini));
  CHECK(ConfigVault::IsSealedFile(State));
  CHECK(!ConfigVault::RefusesConfig(Ini));
}

// The same for a new password: nothing changes, and the user is told
static void StepPasswordNotWritten()
{
  CloudSync & Sync = CloudSync::Instance();
  QString Ini = Configuration::Instance().IniFileName();
  QString Aside = Ini + QStringLiteral(".aside");
  CHECK(QFile::rename(Ini, Aside));
  CHECK(QDir().mkpath(Ini));
  CloudSync::KeyChange Change;
  Change.Current = Password1;
  Change.NewPassword = Password2;
  QPair<bool, QString> Result = ChangeKeyAndWait(Change);
  CHECK(!Result.first && Result.second.contains(QStringLiteral("nothing was changed")));
  CHECK(WaitFor(NotSyncing));
  CHECK(QDir().rmdir(Ini));
  CHECK(QFile::rename(Aside, Ini));
  CHECK(ConfigOpensWith(Password1));
  CHECK(Sync.PendingRecoveryKey().isEmpty());
  // And the password is still the one it was
  CloudSync::KeyChange Again;
  Again.Current = Password1;
  Again.NewPassword = Password2;
  CHECK(ChangeKeyAndWait(Again).first);
  CHECK(WaitFor(NotSyncing));
  CHECK(ConfigOpensWith(Password2));
}

static void StepNoAccount()
{
  // The account did not come: no half sign-in that a key could be made for
  CloudSync & Sync = CloudSync::Instance();
  CHECK(!SignIn());
  CHECK(!Sync.SignedIn());
  CHECK(Sync.State() == CloudSync::csSignedOut);
  CHECK(Sync.UserUuid().isEmpty());
}

static QPair<int, int> Probed()
{
  auto Result = std::make_shared<QPair<int, int>>(-1, -1);
  CloudSync::Instance().ProbeCloud([Result](bool Ok, bool HasData, bool OldFormat, const QString &)
  {
    *Result = Ok ? qMakePair(int(HasData), int(OldFormat)) : qMakePair(-2, -2);
  });
  CHECK(WaitFor([Result]() { return Result->first != -1; }));
  return *Result;
}

// What an account with data of NixSCP 1.2.3 meets on a new computer
static void StepOldCloud()
{
  CloudSync & Sync = CloudSync::Instance();
  CHECK(SignIn());
  CHECK(Probed() == qMakePair(1, 1));
  // The old way in is shut, and it says nothing happened by itself
  CHECK(!Unlock(Password1));
  CHECK(!Sync.HasKey());
  CHECK(Sync.State() == CloudSync::csNeedKey);
  AddSite(QStringLiteral("Mock/Fresh"));
  auto Result = std::make_shared<QPair<int, QString>>(0, QString());
  Sync.ReplaceOldCloud(Password1, true, [Result](bool Ok, const QString & Text) { *Result = qMakePair(Ok ? 1 : 2, Text); });
  CHECK(WaitFor([Result]() { return Result->first != 0; }));
  CHECK(Result->first == 1);
  CHECK(!Result->second.isEmpty());
  CHECK(WaitFor(NotSyncing));
  Trace("replaced");
  CHECK(Sync.State() == CloudSync::csIdle);
  CHECK(Sync.HasKey());
  CHECK(Sync.PendingRecoveryKey() == Result->second);
  WriteRaw(ConfigDir() + QStringLiteral("/recovery.txt"), Result->second.toLatin1());
  CHECK(Probed() == qMakePair(1, 0));
}

static void StepNotOldCloud()
{
  // A new computer of an ordinary account: nothing to replace, whatever is asked
  CloudSync & Sync = CloudSync::Instance();
  CHECK(SignIn());
  CHECK(Probed() == qMakePair(1, 0));
  auto Result = std::make_shared<int>(0);
  Sync.ReplaceOldCloud(Password2, false, [Result](bool Ok, const QString &) { *Result = Ok ? 1 : 2; });
  CHECK(WaitFor([Result]() { return *Result != 0; }));
  CHECK(*Result == 2);
  CHECK(!Sync.HasKey());
  CHECK(Unlock(Password1));
  CHECK(WaitFor(NotSyncing));
}

static void StepKeepsOldHeadOut()
{
  // An established computer: an old-format head from the server changes nothing here
  CloudSync & Sync = CloudSync::Instance();
  QByteArray Key = Sync.DataKey();
  Sync.SyncNow();
  CHECK(WaitFor(NotSyncing));
  Trace("old head");
  CHECK(!Sync.HasKey() || (Sync.DataKey() == Key));
  CHECK(ConfigOpensWith(Password1));
}

static void StepRotateAgain()
{
  CloudSync::KeyChange Change;
  Change.Current = Password2;
  Change.NewPassword = Password3;
  Change.RotateDataKey = true;
  CHECK(ChangeKeyAndWait(Change).first);
  CHECK(ConfigOpensWith(Password3));
  WaitForJournal();
}

static void StepRotateRefused()
{
  // The cloud moved on: nothing may change, here or there
  CloudSync & Sync = CloudSync::Instance();
  QByteArray Key = Sync.DataKey();
  QByteArray Config = SealedConfig();
  CloudSync::KeyChange Change;
  Change.Current = Password1;
  Change.NewPassword = Password2;
  Change.RotateDataKey = true;
  QPair<bool, QString> Result = ChangeKeyAndWait(Change);
  CHECK(!Result.first && !Result.second.isEmpty());
  CHECK(Sync.DataKey() == Key);
  CHECK(SealedConfig() == Config);
  // A recovery key cannot wrap a new data key, so a password is needed for that
  QString Recovery = QString::fromLatin1(ReadRaw(ConfigDir() + QStringLiteral("/recovery.txt")));
  CloudSync::KeyChange NoPassword;
  NoPassword.Current = Recovery;
  NoPassword.CurrentIsRecoveryKey = true;
  NoPassword.RotateDataKey = true;
  QPair<bool, QString> Refused = ChangeKeyAndWait(NoPassword);
  CHECK(!Refused.first && Refused.second.contains(QStringLiteral("password")));
}

static void StepLeftBehind()
{
  // Its key was replaced elsewhere: it stops and asks, it does not "repair"
  CloudSync & Sync = CloudSync::Instance();
  CHECK(WaitFor([&Sync]() { return Sync.State() == CloudSync::csNeedKey; }));
  Trace("left behind");
  CHECK(!Sync.HasKey());
  CHECK(!ForeignSeen);
  CHECK(Sync.StatusText().contains(QStringLiteral("replaced")));
}

static void StepUnlockNewKey(const QString & Password)
{
  CloudSync & Sync = CloudSync::Instance();
  Sync.SyncNow();
  CHECK(WaitFor([&Sync]() { return Sync.State() == CloudSync::csNeedKey; }));
  // The old password still opens old versions in the history, and leads nowhere
  CHECK(!Unlock(QStringLiteral("not any password of this account")));
  CHECK(Unlock(Password));
  CHECK(WaitFor(NotSyncing));
  CHECK(Sync.State() == CloudSync::csIdle);
  CHECK(Sync.HasKey());
  CHECK(ConfigVault::Key() == Sync.DataKey());
  CHECK(ConfigOpensWith(Password));
  WaitForJournal();
}

static void StepNewRecovery()
{
  CloudSync & Sync = CloudSync::Instance();
  CloudSync::KeyChange Change;
  Change.Current = Password1;
  Change.NewRecoveryKey = true;
  QPair<bool, QString> Result = ChangeKeyAndWait(Change);
  CHECK(Result.first && !Result.second.isEmpty());
  CHECK(WaitFor(NotSyncing));
  CHECK(Sync.State() == CloudSync::csIdle);
  WriteRaw(ConfigDir() + QStringLiteral("/recovery2.txt"), Result.second.toLatin1());
  CHECK(ConfigOpensWith(Password1));
  // The recovery key works as the current secret too
  CloudSync::KeyChange WithRecovery;
  WithRecovery.Current = Result.second;
  WithRecovery.CurrentIsRecoveryKey = true;
  WithRecovery.NewPassword = Password2;
  CHECK(ChangeKeyAndWait(WithRecovery).first);
  CHECK(WaitFor(NotSyncing));
  CHECK(ConfigOpensWith(Password2));
  // ...and stays valid when only the password changes
  QByteArray Plain;
  CHECK(ConfigVault::OpenWithSecret(SealedConfig(), Result.second, true, Plain));
}

static void StepExpectError(const QString & Text)
{
  CloudSync & Sync = CloudSync::Instance();
  Sync.SyncNow();
  CHECK(WaitFor(NotSyncing));
  Trace("expected error");
  CHECK(Sync.State() == CloudSync::csError);
  CHECK(Sync.LastError().contains(Text));
  CHECK(Sync.HasKey());
}

static void StepForge(const QString & Kind)
{
  // A key holder made these, as a server that kept a fork of the history would
  // have them: valid, but not on top of what the first computer saw.
  CloudSync & Sync = CloudSync::Instance();
  QByteArray Head = ReadRaw(ConfigDir() + QStringLiteral("/head.bin"));
  CloudCrypto::SnapshotHeader Header;
  CHECK(CloudCrypto::ReadHeader(Head, Header));
  QByteArray Dek = Sync.DataKey();
  IniFile Site;
  Site.WriteRaw(QStringLiteral("Sessions\\Forged"), QStringLiteral("HostName"), QStringLiteral("forged.example.com"));
  QByteArray Plaintext = CloudSettings::Canonical(Site);
  qint64 Head1 = Header.Version;
  CloudCrypto::SnapshotHeader One = Header;
  One.Version = Head1 + 1;
  One.PrevVersion = Head1;
  One.PrevHash = QString(64, QLatin1Char('0'));
  QByteArray First = CloudCrypto::EncodeSnapshot(One, Dek, Plaintext);
  WriteRaw(ConfigDir() + QStringLiteral("/forged1.bin"), First);
  if (Kind == QStringLiteral("gap"))
  {
    CloudCrypto::SnapshotHeader Two = One;
    Two.Version = Head1 + 2;
    Two.PrevVersion = Head1 + 1;
    Two.PrevHash = CloudSettings::Hash(First);
    WriteRaw(ConfigDir() + QStringLiteral("/forged2.bin"), CloudCrypto::EncodeSnapshot(Two, Dek, Plaintext));
  }
}

static void StepSessionPush(const QByteArray & Payload)
{
  CloudSync::Instance().FlushSession(Payload, 5000);
}

static QByteArray FetchedSession()
{
  auto Session = std::make_shared<QByteArray>();
  auto Done = std::make_shared<bool>(false);
  CloudSync::Instance().FetchSession(5000, [Session, Done](bool, const QByteArray & Payload) { *Session = Payload; *Done = true; });
  CHECK(WaitFor([Done]() { return *Done; }));
  return *Session;
}

static void StepPlanted()
{
  // The state file was replaced by a plain one: everything in it is ignored
  CloudSync & Sync = CloudSync::Instance();
  CHECK(!Sync.SignedIn());
  CHECK(!Sync.HasKey());
  CHECK(!Sync.Client().BaseUrl().contains(QStringLiteral("evil")));
  CHECK(Sync.Client().BaseUrl() == CloudClient::DefaultBaseUrl());
  CHECK(Sync.Email().isEmpty() && Sync.UserUuid().isEmpty());
  // The key of the files is the one the password opened
  CHECK(ConfigVault::Key() == ReadRaw(ConfigDir() + QStringLiteral("/dek.bin")));
  CHECK(ConfigOpensWith(Password1));
  Sync.SyncNow();
  QEventLoop Loop;
  QTimer::singleShot(300, &Loop, &QEventLoop::quit);
  Loop.exec();
}

static void StepKeepDek()
{
  WriteRaw(ConfigDir() + QStringLiteral("/dek.bin"), ConfigVault::Key());
}

static void StepSealedState()
{
  CloudSync & Sync = CloudSync::Instance();
  QString Name = ConfigDir() + QStringLiteral("/nixscp-cloud.ini");
  QByteArray Raw = ReadRaw(Name);
  CHECK(ConfigVault::IsSealed(Raw));
  CHECK(!Raw.contains(Sync.Client().RefreshToken().toLatin1()));
  CHECK(!Raw.contains("BaseUrl") && !Raw.contains("127.0.0.1") && !Raw.contains(Email.toLatin1()) && !Raw.contains("RefreshToken"));
  // Restarted, it still knows everything
  CHECK(Sync.SignedIn() && Sync.HasKey());
  CHECK(Sync.Email() == Email);
  CHECK(Sync.UserUuid() == User);
}

static void StepPreKey()
{
  // Signed in, no key yet: nothing to seal with, so nothing reaches the disk
  CloudSync & Sync = CloudSync::Instance();
  CHECK(SignIn());
  CHECK(Sync.SignedIn() && !Sync.HasKey());
  Sync.SetSyncSessions(false);
  QByteArray Raw = ReadRaw(ConfigDir() + QStringLiteral("/nixscp-cloud.ini"));
  CHECK(!Raw.isEmpty() && Raw.contains("SyncSessions=0"));
  CHECK(!Raw.contains("refresh-") && !Raw.contains("RefreshToken") && !Raw.contains("BaseUrl") && !Raw.contains(Email.toLatin1()));
}

static void StepPreKeyRestarted()
{
  // Asked to sign in again; the option stayed
  CloudSync & Sync = CloudSync::Instance();
  CHECK(!Sync.SignedIn());
  CHECK(!Sync.SyncSessions());
}

static void StepKeyMismatch()
{
  // Whatever brought this about: settings never leave under another key
  CloudSync & Sync = CloudSync::Instance();
  CloudCrypto::SnapshotHeader Wrap = ConfigVault::Wrap();
  ConfigVault::SetKey(CloudCrypto::RandomBytes(CloudCrypto::KeyLength), Wrap);
  Sync.SyncNow();
  CHECK(Sync.State() == CloudSync::csError);
  CHECK(Sync.LastError().contains(QStringLiteral("does not match")));
  Sync.PushSession(Tabs2);
  QEventLoop Loop;
  QTimer::singleShot(200, &Loop, &QEventLoop::quit);
  Loop.exec();
}

static void StepNoConflictDialog()
{
  // Changed here and in the cloud, to the same settings
  CHECK(!ConflictSeen);
  CHECK(CloudSync::Instance().State() == CloudSync::csIdle);
}

static void StepKeepsKey()
{
  // The history could not be read: that says nothing about the key
  CloudSync & Sync = CloudSync::Instance();
  Sync.SyncNow();
  CHECK(WaitFor(NotSyncing));
  CHECK(Sync.HasKey());
  CHECK(Sync.State() == CloudSync::csError);
}

static void StepRepairs()
{
  CHECK(ForeignSeen);
  CHECK(CloudSync::Instance().HasKey());
  CHECK(CloudSync::Instance().State() == CloudSync::csIdle);
}

static void StepPause()
{
  CloudSync & Sync = CloudSync::Instance();
  CHECK(ConflictSeen);
  Sync.ResolveConflict(CloudSync::crLater);
  CHECK(Sync.State() == CloudSync::csPaused);
}

static void StepResolveAfterRestart(bool UseCloud)
{
  CloudSync & Sync = CloudSync::Instance();
  CHECK(Sync.State() == CloudSync::csPaused);
  Sync.ResolveConflict(UseCloud ? CloudSync::crUseCloud : CloudSync::crUseLocal);
  CHECK(WaitFor(NotSyncing));
  Trace("resolved after restart");
  CHECK(Sync.State() == CloudSync::csIdle);
  CHECK(HasSite(QStringLiteral("Mock/Paused")) != UseCloud);
}

static void StepUnauthorized()
{
  // The API refuses a call although the sign-in is fine: not stuck on "syncing"
  CloudSync & Sync = CloudSync::Instance();
  Sync.SyncNow();
  CHECK(WaitFor(NotSyncing, 5000));
  CHECK(Sync.State() == CloudSync::csError);
  CHECK(Sync.SignedIn());
}

static void StepSignOutDuringSync()
{
  CloudSync & Sync = CloudSync::Instance();
  AddSite(QStringLiteral("Mock/LastMinute"));
  Sync.SyncNow();
  CHECK(Sync.State() == CloudSync::csSyncing);
  Sync.SignOut();
  CHECK(WaitFor([&Sync]() { return !Sync.SignedIn(); }));
  // The answers arrive after the account is gone
  QEventLoop Loop;
  QTimer::singleShot(900, &Loop, &QEventLoop::quit);
  Loop.exec();
  Trace("signed out during a sync");
  CHECK(Sync.State() == CloudSync::csSignedOut);
  CHECK(!Sync.HasKey());
}

static void StepRefreshOnly()
{
  CloudSync & Sync = CloudSync::Instance();
  auto Done = std::make_shared<bool>(false);
  Sync.Client().Api("GET", QStringLiteral("/v1/me"), [Done](const CloudClient::Reply & Reply) { CHECK(Reply.Ok()); *Done = true; });
  CHECK(WaitFor([Done]() { return *Done; }));
}

static int RunChild(const QString & Dir, const QString & Step, const QString & Secret);

static void StepTwoInstances(const QString & Dir)
{
  // Another NixSCP on this computer rotates the token this one holds: using the
  // old one would sign the computer out as if it had been stolen
  CloudSync & Sync = CloudSync::Instance();
  CHECK(RunChild(Dir, QStringLiteral("refresh-only"), Password1) == 0);
  // The access token ran out: the next call has to refresh
  Sync.Client().SetRefreshToken(Sync.Client().RefreshToken());
  auto Done = std::make_shared<bool>(false);
  Sync.Client().Api("GET", QStringLiteral("/v1/me"), [Done](const CloudClient::Reply & Reply) { CHECK(Reply.Ok()); *Done = true; });
  CHECK(WaitFor([Done]() { return *Done; }));
  CHECK(Sync.SignedIn());
}

static void StepRefreshFails()
{
  // A 400 that is not the server's "invalid_grant" (a proxy, a bad gateway) does
  // not cost the sign-in
  CloudSync & Sync = CloudSync::Instance();
  Sync.SyncNow();
  CHECK(WaitFor(NotSyncing));
  CHECK(Sync.SignedIn());
  CHECK(Sync.State() == CloudSync::csError);
}

static void StepSecondInstanceWraps(const QString & Dir)
{
  // Another NixSCP changes the password while this one runs with the old wraps
  CloudSync & Sync = CloudSync::Instance();
  Configuration & Config = Configuration::Instance();
  CHECK(RunChild(Dir, QStringLiteral("change-password"), Password1) == 0);
  CHECK(ConfigOpensWith(Password2));
  // Reading the file brings its wraps; the state of this NixSCP must follow them
  Config.Load();
  CHECK(!Config.IsLocked());
  Sync.SetRestoreSessions(false);
  Sync.SetRestoreSessions(true);
  CHECK(ConfigOpensWith(Password2));
  CHECK(!ConfigOpensWith(Password1));
  // Not only the file: what this NixSCP uploads next carries them as well
  IniFile State;
  CHECK(State.Load(ConfigDir() + QStringLiteral("/nixscp-cloud.ini")));
  CHECK(State.ReadRaw(QStringLiteral("Key"), QStringLiteral("WrapEpoch")) == QStringLiteral("2"));
  AddSite(QStringLiteral("Mock/AfterSecondInstance"));
  CHECK(ConfigOpensWith(Password2));
  CHECK(!ConfigOpensWith(Password1));
}

static void StepCheckUploadHeader()
{
  AddSite(QStringLiteral("Mock/Header"));
  CHECK(Synced());
}

static int RunStep(const QString & Dir, const QString & Step, const QString & Param)
{
  Configuration & Config = Configuration::Instance();
  Config.SetIniFileName(Dir + QStringLiteral("/nixscp.ini"));
  if (ConfigVault::IsSealedFile(Config.IniFileName()))
  {
    CHECK(ConfigVault::Unlock(Config.IniFileName(), qEnvironmentVariable("STEP_SECRET"), false));
  }
  Config.Load();
  CHECK(!Config.IsLocked());
  QObject::connect(&CloudSync::Instance(), &CloudSync::ConflictDetected, []() { ConflictSeen = true; });
  QObject::connect(&CloudSync::Instance(), &CloudSync::ForeignSnapshotIgnored, []() { ForeignSeen = true; });
  QObject::connect(&CloudSync::Instance(), &CloudSync::KeyChangeSettled, [](bool Replaced, const QString & Message)
  {
    KeyChangesSettled++;
    KeyChangeReplaced = Replaced;
    KeyChangeMessage = Message;
  });
  CloudSync::Instance().Initialize(Config);
  // A change made while this computer was off: the first sync meets it
  if (Step == QStringLiteral("keep-local")) AddSite(QStringLiteral("Mock/LocalEdit"));
  if (Step == QStringLiteral("pause")) AddSite(QStringLiteral("Mock/Paused"));
  if (Step == QStringLiteral("no-conflict-dialog")) AddSite(Param);
  Settle();
  if (Step == QStringLiteral("first")) StepFirst();
  else if (Step == QStringLiteral("second")) StepSecond();
  else if (Step == QStringLiteral("team-key")) StepTeamKey();
  else if (Step == QStringLiteral("check-team-key")) StepCheckTeamKey();
  else if (Step == QStringLiteral("add")) StepAdd(Param);
  else if (Step == QStringLiteral("has-site")) StepHasSite(Param);
  else if (Step == QStringLiteral("sync")) CHECK(Synced());
  else if (Step == QStringLiteral("sync-keys"))
  {
    CHECK(Synced());
    WaitForJournal();
  }
  else if (Step == QStringLiteral("change-password")) StepChangePassword();
  else if (Step == QStringLiteral("keep-local")) StepKeepLocal();
  else if (Step == QStringLiteral("rotate")) StepRotate();
  else if (Step == QStringLiteral("rotate-again")) StepRotateAgain();
  else if (Step == QStringLiteral("rotate-plain")) CHECK(ChangeKeyAndWait(RotationToSecond()).first);
  else if (Step == QStringLiteral("change-password-killed"))
  {
    KillAt = "wraps-in-config";
    ConfigVault::KeyChangeStep = [](const char * Reached)
    {
      if (KillAt == Reached)
      {
        std::raise(SIGKILL);
      }
    };
    CloudSync::KeyChange Change;
    Change.Current = Password1;
    Change.NewPassword = Password2;
    ChangeKeyAndWait(Change);
    CHECK(false);
  }
  else if (Step == QStringLiteral("rotate-killed")) StepRotateKilled(Param);
  else if (Step == QStringLiteral("rotate-lost")) StepRotateLost(Param);
  else if (Step == QStringLiteral("settled")) StepSettled(Param);
  else if (Step == QStringLiteral("told-later")) StepToldLater(Param);
  else if (Step == QStringLiteral("nothing-waits")) StepNothingWaits();
  else if (Step == QStringLiteral("sign-out")) StepSignOut();
  else if (Step == QStringLiteral("back-in")) StepBackIn(Param);
  else if (Step == QStringLiteral("config-not-written")) StepConfigNotWritten();
  else if (Step == QStringLiteral("password-not-written")) StepPasswordNotWritten();
  else if (Step == QStringLiteral("no-account")) StepNoAccount();
  else if (Step == QStringLiteral("old-cloud")) StepOldCloud();
  else if (Step == QStringLiteral("not-old-cloud")) StepNotOldCloud();
  else if (Step == QStringLiteral("keeps-old-head-out")) StepKeepsOldHeadOut();
  else if (Step == QStringLiteral("second-sees"))
  {
    CHECK(SignIn());
    CHECK(Unlock(Password1));
    CHECK(WaitFor([Param]() { return NotSyncing() && HasSite(Param); }));
  }
  else if (Step == QStringLiteral("rotate-refused")) StepRotateRefused();
  else if (Step == QStringLiteral("left-behind")) StepLeftBehind();
  else if (Step == QStringLiteral("unlock-new-key")) StepUnlockNewKey(Param);
  else if (Step == QStringLiteral("new-recovery")) StepNewRecovery();
  else if (Step == QStringLiteral("expect-error")) StepExpectError(Param);
  else if (Step == QStringLiteral("forge")) StepForge(Param);
  else if (Step == QStringLiteral("session-push")) StepSessionPush(Param == QStringLiteral("2") ? Tabs2 : Tabs1);
  else if (Step == QStringLiteral("session-is")) CHECK(FetchedSession() == (Param == QStringLiteral("2") ? Tabs2 : Tabs1));
  else if (Step == QStringLiteral("planted")) StepPlanted();
  else if (Step == QStringLiteral("keep-dek")) StepKeepDek();
  else if (Step == QStringLiteral("sealed-state")) StepSealedState();
  else if (Step == QStringLiteral("pre-key")) StepPreKey();
  else if (Step == QStringLiteral("pre-key-restarted")) StepPreKeyRestarted();
  else if (Step == QStringLiteral("key-mismatch")) StepKeyMismatch();
  else if (Step == QStringLiteral("no-conflict-dialog")) StepNoConflictDialog();
  else if (Step == QStringLiteral("keeps-key")) StepKeepsKey();
  else if (Step == QStringLiteral("repairs")) StepRepairs();
  else if (Step == QStringLiteral("pause")) StepPause();
  else if (Step == QStringLiteral("resolve-cloud")) StepResolveAfterRestart(true);
  else if (Step == QStringLiteral("resolve-local")) StepResolveAfterRestart(false);
  else if (Step == QStringLiteral("unauthorized")) StepUnauthorized();
  else if (Step == QStringLiteral("sign-out-during-sync")) StepSignOutDuringSync();
  else if (Step == QStringLiteral("refresh-only")) StepRefreshOnly();
  else if (Step == QStringLiteral("two-instances")) StepTwoInstances(Dir);
  else if (Step == QStringLiteral("refresh-fails")) StepRefreshFails();
  else if (Step == QStringLiteral("second-instance-wraps")) StepSecondInstanceWraps(Dir);
  else if (Step == QStringLiteral("check-upload-header")) StepCheckUploadHeader();
  else CHECK(false);
  std::printf("step %s %s: %s\n", qPrintable(Step), qPrintable(Param), (Failures == 0) ? "ok" : "FAILED");
  std::fflush(stdout);
  return (Failures == 0) ? 0 : 1;
}

// Runs a step in its own process; this one keeps serving meanwhile
static int RunChild(const QString & Dir, const QString & Step, const QString & Secret)
{
  QProcess Process;
  Process.setProcessChannelMode(QProcess::ForwardedChannels);
  QProcessEnvironment Env = QProcessEnvironment::systemEnvironment();
  Env.insert(QStringLiteral("STEP_SECRET"), Secret);
  Process.setProcessEnvironment(Env);
  QEventLoop Loop;
  QObject::connect(&Process, &QProcess::finished, &Loop, &QEventLoop::quit);
  QTimer::singleShot(60000, &Loop, &QEventLoop::quit);
  QStringList Parts = Step.split(QLatin1Char(':'));
  Process.start(QCoreApplication::applicationFilePath(), {Dir, Parts.value(0), Parts.value(1)});
  Loop.exec();
  return ((Process.state() == QProcess::NotRunning) && (Process.exitStatus() == QProcess::NormalExit)) ? Process.exitCode() : 99;
}

//---------------------------------------------------------------------------
// Scenarios, in the parent

static void Step(const QString & Dir, const QString & Name, const QString & Secret = Password1)
{
  CHECK(RunChild(Dir, Name, Secret) == 0);
}

static CloudCrypto::SnapshotHeader HeaderOf(const QByteArray & Snapshot)
{
  CloudCrypto::SnapshotHeader Header;
  CHECK(CloudCrypto::ReadHeader(Snapshot, Header));
  return Header;
}

static void ScenarioStaleWraps(MockCloud & Mock)
{
  std::printf("-- a stale computer does not bring the old password back\n");
  Mock.Reset();
  QTemporaryDir A, B;
  Step(A.path(), QStringLiteral("first"));
  Step(B.path(), QStringLiteral("second"));
  Step(B.path(), QStringLiteral("change-password"));
  CHECK(HeaderOf(Mock.HeadData()).WrapEpoch == 2);
  Step(A.path(), QStringLiteral("keep-local"));
  // Pushed over the password change, and the cloud still has the new password
  CloudCrypto::SnapshotHeader Head = HeaderOf(Mock.HeadData());
  CHECK(Head.WrapEpoch == 2);
  CHECK(CloudCrypto::UnwrapDekWithPassword(Head, Password1).isEmpty());
  CHECK(!CloudCrypto::UnwrapDekWithPassword(Head, Password2).isEmpty());
  CHECK(HeaderOf(Mock.History.value(QStringLiteral("settings")).value(1)).WrapEpoch == 1);

  // The conflict was postponed before the password changed: nothing read the new
  // head yet when "keep mine" is chosen, after a restart
  std::printf("-- ...nor does a conflict postponed before the change\n");
  Mock.Reset();
  QTemporaryDir C, D;
  Step(C.path(), QStringLiteral("first"));
  Step(D.path(), QStringLiteral("second"));
  Step(D.path(), QStringLiteral("add:Mock/FromD"));
  Step(C.path(), QStringLiteral("pause"));
  Step(D.path(), QStringLiteral("change-password"));
  Step(C.path(), QStringLiteral("resolve-local"));
  Head = HeaderOf(Mock.HeadData());
  CHECK(Head.WrapEpoch == 2);
  CHECK(CloudCrypto::UnwrapDekWithPassword(Head, Password1).isEmpty());
  CHECK(!CloudCrypto::UnwrapDekWithPassword(Head, Password2).isEmpty());
}

static void ScenarioRotation(MockCloud & Mock)
{
  std::printf("-- replacing the data key\n");
  Mock.Reset();
  QTemporaryDir A, B, C, D;
  Step(A.path(), QStringLiteral("first"));
  Step(A.path(), QStringLiteral("team-key"));
  Step(B.path(), QStringLiteral("second"));
  Step(C.path(), QStringLiteral("second"));
  QByteArray OldHead = Mock.HeadData();
  QString Recovery1 = QString::fromLatin1(ReadRaw(A.path() + QStringLiteral("/recovery.txt")));
  QByteArray OldKey = CloudCrypto::UnwrapDekWithPassword(HeaderOf(OldHead), Password1);
  CHECK(!OldKey.isEmpty());

  Step(A.path(), QStringLiteral("rotate"));
  QByteArray NewHead = Mock.HeadData();
  CloudCrypto::SnapshotHeader Head = HeaderOf(NewHead);
  CHECK(Head.KeyEpoch == 2);
  // The old password and recovery key open nothing new, from old headers either
  CHECK(CloudCrypto::UnwrapDekWithPassword(Head, Password1).isEmpty());
  QByteArray Recovered = CloudCrypto::UnwrapDekWithRecoveryKey(Head, CloudCrypto::ParseRecoveryKey(Recovery1));
  CHECK(Recovered.isEmpty());
  QByteArray NewKey = CloudCrypto::UnwrapDekWithPassword(Head, Password2);
  CHECK(!NewKey.isEmpty() && (NewKey != OldKey));
  CloudCrypto::SnapshotHeader Decoded;
  QByteArray Plain;
  CHECK(!CloudCrypto::DecodeSnapshot(NewHead, OldKey, Decoded, Plain));
  CHECK(CloudCrypto::DecodeSnapshot(NewHead, NewKey, Decoded, Plain) && Plain.contains("Mock/A1"));
  CHECK(CloudCrypto::ProvesRotation(Head, OldKey));
  QString Recovery2 = QString::fromLatin1(ReadRaw(A.path() + QStringLiteral("/recovery2.txt")));
  CHECK(CloudCrypto::UnwrapDekWithRecoveryKey(Head, CloudCrypto::ParseRecoveryKey(Recovery2)) == NewKey);
  // The tabs too
  CHECK(CloudCrypto::DecodeSnapshot(Mock.HeadData(QStringLiteral("session")), NewKey, Decoded, Plain) && (Plain == Tabs1));
  // user_keys moved, signed by the identity key as the server demands
  CHECK(Mock.KeyPuts == 1);
  CHECK(!CloudCrypto::Open(OldKey, QByteArray::fromBase64(Mock.Keys.value(QStringLiteral("wrapped_private_key")).toString().toLatin1()), QByteArray(), Plain));
  Step(A.path(), QStringLiteral("check-team-key"), Password2);

  // A computer left behind stops: nothing goes up under the old key
  int Commits = Mock.Commits;
  Step(B.path(), QStringLiteral("left-behind"), Password1);
  CHECK(Mock.Commits == Commits);
  CHECK(Mock.HeadData() == NewHead);
  // It joins with the current password, and its files follow
  Step(B.path(), QStringLiteral("unlock-new-key:") + Password2, Password1);
  Step(B.path(), QStringLiteral("add:Mock/AfterRotation"), Password2);
  Step(A.path(), QStringLiteral("has-site:Mock/AfterRotation"), Password2);
  CHECK(HeaderOf(Mock.HeadData()).KeyEpoch == 2);
  // A new computer gets the new key
  Step(D.path(), QStringLiteral("sync-new-computer:") + Password2, QString());

  // Twice: the proof in the head is for a key C never had, but it passes the
  // version that replaced its key on the way back
  Step(A.path(), QStringLiteral("rotate-again"), Password2);
  CHECK(HeaderOf(Mock.HeadData()).KeyEpoch == 3);
  Commits = Mock.Commits;
  Step(C.path(), QStringLiteral("left-behind"), Password1);
  CHECK(Mock.Commits == Commits);
  Step(C.path(), QStringLiteral("unlock-new-key:") + Password3, Password1);

  // The server is unreachable right after: the wraps wait in the sealed state
  std::printf("-- the keys on the server follow later\n");
  Mock.Reset();
  QTemporaryDir E;
  Step(E.path(), QStringLiteral("first"));
  Step(E.path(), QStringLiteral("team-key"));
  Mock.FailPut[QStringLiteral("/v1/keys/me")] = 500;
  Step(E.path(), QStringLiteral("rotate"));
  CHECK(Mock.KeyPuts == 0);
  Mock.FailPut.clear();
  Step(E.path(), QStringLiteral("sync-keys"), Password2);
  CHECK(Mock.KeyPuts == 1);
  Step(E.path(), QStringLiteral("check-team-key"), Password2);

  // They wait for a sync that shows the key is still the account's: a stale wrap
  // must not go over what a later replacement put on the server
  std::printf("-- ...but not over a newer key\n");
  Mock.Reset();
  QTemporaryDir Late, Other;
  Step(Late.path(), QStringLiteral("first"));
  Step(Late.path(), QStringLiteral("team-key"));
  WriteRaw(Other.path() + QStringLiteral("/team-private.bin"), ReadRaw(Late.path() + QStringLiteral("/team-private.bin")));
  Step(Other.path(), QStringLiteral("second"));
  Mock.FailPut[QStringLiteral("/v1/keys/me")] = 500;
  Step(Late.path(), QStringLiteral("rotate"));
  Mock.FailPut.clear();
  Step(Other.path(), QStringLiteral("left-behind"), Password1);
  Step(Other.path(), QStringLiteral("unlock-new-key:") + Password2, Password1);
  CHECK(Mock.KeyPuts == 1);
  Step(Other.path(), QStringLiteral("rotate-again"), Password2);
  CHECK(Mock.KeyPuts == 2);
  QJsonObject Newest = Mock.Keys;
  Step(Late.path(), QStringLiteral("left-behind"), Password2);
  CHECK(Mock.Keys == Newest);
  Step(Late.path(), QStringLiteral("unlock-new-key:") + Password3, Password2);
  Step(Late.path(), QStringLiteral("check-team-key"), Password3);
  Step(Other.path(), QStringLiteral("check-team-key"), Password3);

  std::printf("-- no replacement while the cloud has something newer\n");
  Mock.Reset();
  QTemporaryDir F, G;
  Step(F.path(), QStringLiteral("first"));
  Step(G.path(), QStringLiteral("second"));
  Step(G.path(), QStringLiteral("add:Mock/FromG"));
  qint64 Head1 = Mock.Head();
  // Only the sync at the start would catch up, so it must not get there first
  Mock.Fail[QStringLiteral("/v1/sync/download-url")] = 503;
  Step(F.path(), QStringLiteral("rotate-refused"));
  CHECK(Mock.Head() == Head1);
  CHECK(HeaderOf(Mock.HeadData()).KeyEpoch == 1);
}

// A first computer with a team key, about to replace its data key
static void RotationStart(MockCloud & Mock, const QString & Dir)
{
  Mock.Reset();
  Step(Dir, QStringLiteral("first"));
  Step(Dir, QStringLiteral("team-key"));
}

static void CheckReplaced(MockCloud & Mock, const QString & Dir, const QByteArray & OldKey, const QString & Old)
{
  CloudCrypto::SnapshotHeader Head = HeaderOf(Mock.HeadData());
  CHECK(Head.KeyEpoch == 2);
  QByteArray NewKey = CloudCrypto::UnwrapDekWithPassword(Head, Password2);
  CHECK(!NewKey.isEmpty() && (NewKey != OldKey));
  CHECK(CloudCrypto::UnwrapDekWithPassword(Head, Password1).isEmpty());
  CHECK(CloudCrypto::UnwrapDekWithRecoveryKey(Head, CloudCrypto::ParseRecoveryKey(Old)).isEmpty());
  // The recovery key the user was given opens what the cloud has
  QString Recovery = QString::fromLatin1(ReadRaw(Dir + QStringLiteral("/recovery2.txt")));
  CHECK(CloudCrypto::UnwrapDekWithRecoveryKey(Head, CloudCrypto::ParseRecoveryKey(Recovery)) == NewKey);
  // Teams: under the new key, signed by the identity key, not lost
  QByteArray Wrapped = QByteArray::fromBase64(Mock.Keys.value(QStringLiteral("wrapped_private_key")).toString().toLatin1());
  QByteArray Plain;
  CHECK(CloudCrypto::Open(NewKey, Wrapped, QByteArray(), Plain));
  CHECK(!CloudCrypto::Open(OldKey, Wrapped, QByteArray(), Plain));
  Step(Dir, QStringLiteral("check-team-key"), Password2);
  Step(Dir, QStringLiteral("nothing-waits"), Password2);
  Step(Dir, QStringLiteral("add:Mock/AfterAll"), Password2);
}

static void CheckNotReplaced(MockCloud & Mock, const QString & Dir, const QByteArray & OldKey)
{
  CloudCrypto::SnapshotHeader Head = HeaderOf(Mock.HeadData());
  CHECK(Head.KeyEpoch == 1);
  CHECK(CloudCrypto::UnwrapDekWithPassword(Head, Password1) == OldKey);
  CHECK(CloudCrypto::UnwrapDekWithPassword(Head, Password2).isEmpty());
  QByteArray Wrapped = QByteArray::fromBase64(Mock.Keys.value(QStringLiteral("wrapped_private_key")).toString().toLatin1());
  QByteArray Plain;
  CHECK(CloudCrypto::Open(OldKey, Wrapped, QByteArray(), Plain));
  Step(Dir, QStringLiteral("check-team-key"), Password1);
  Step(Dir, QStringLiteral("nothing-waits"), Password1);
  Step(Dir, QStringLiteral("add:Mock/AfterAll"), Password1);
}

static void ScenarioRotationKilled(MockCloud & Mock)
{
  struct
  {
    const char * Where;
    bool Landed;      // the new head is in the cloud
    bool NewSecret;   // the configuration asks for the new password already
  } const Points[] =
  {
    {"journal-written", false, false},
    {"uploaded", false, false},
    {"committed", true, false},
    {"bridge-written", true, false},
    {"config-resealed", true, true},
    {"files-resealed", true, true},
    {"key-switched", true, true},
    {"config-rewritten", true, true},
    {"state-saved", true, true},
    {"journal-cleared", true, true},
  };
  for (const auto & Point : Points)
  {
    std::printf("-- replacing the data key, killed at %s\n", Point.Where);
    QTemporaryDir A;
    RotationStart(Mock, A.path());
    QByteArray OldKey = CloudCrypto::UnwrapDekWithPassword(HeaderOf(Mock.HeadData()), Password1);
    QString Old = QString::fromLatin1(ReadRaw(A.path() + QStringLiteral("/recovery.txt")));
    qint64 Base = Mock.Head();
    int Commits = Mock.Commits;
    CHECK(RunChild(A.path(), QStringLiteral("rotate-killed:%1").arg(QLatin1String(Point.Where)), Password1) == 99);
    CHECK(Mock.Commits == Commits + (Point.Landed ? 1 : 0));
    CHECK(Mock.Head() == Base + (Point.Landed ? 1 : 0));
    // The next start: nothing is left to the user but the password the file asks for
    Step(A.path(), Point.Landed ? QStringLiteral("settled:landed") : QStringLiteral("settled:refused"), Point.NewSecret ? Password2 : Password1);
    if (Point.Landed)
    {
      CHECK(Mock.KeyPuts == 1);
      CheckReplaced(Mock, A.path(), OldKey, Old);
    }
    else
    {
      // Called off with a version that shuts the base out
      CHECK(Mock.Head() == Base + 1);
      CHECK(Mock.KeyPuts == 0);
      CheckNotReplaced(Mock, A.path(), OldKey);
    }
  }

  std::printf("-- the computer that replaced the key never comes back\n");
  {
    QTemporaryDir A, B;
    RotationStart(Mock, A.path());
    Step(B.path(), QStringLiteral("second"));
    WriteRaw(B.path() + QStringLiteral("/team-private.bin"), ReadRaw(A.path() + QStringLiteral("/team-private.bin")));
    CHECK(RunChild(A.path(), QStringLiteral("rotate-killed:committed"), Password1) == 99);
    CHECK(Mock.KeyPuts == 0);
    Step(B.path(), QStringLiteral("left-behind"), Password1);
    // Entering the new password: the old key opens the team keys one last time
    Step(B.path(), QStringLiteral("unlock-new-key:") + Password2, Password1);
    CHECK(Mock.KeyPuts == 1);
    Step(B.path(), QStringLiteral("check-team-key"), Password2);
    Step(B.path(), QStringLiteral("nothing-waits"), Password2);
    // ...and when it does come back, it finds the work done and keeps the recovery key
    Step(A.path(), QStringLiteral("settled:landed"), Password1);
    Step(A.path(), QStringLiteral("check-team-key"), Password2);
  }
}

static void ScenarioRotationLost(MockCloud & Mock)
{
  std::printf("-- the commit went through, the reply did not\n");
  {
    QTemporaryDir A;
    RotationStart(Mock, A.path());
    QByteArray OldKey = CloudCrypto::UnwrapDekWithPassword(HeaderOf(Mock.HeadData()), Password1);
    QString Old = QString::fromLatin1(ReadRaw(A.path() + QStringLiteral("/recovery.txt")));
    Mock.LoseCommitReply = true;
    Step(A.path(), QStringLiteral("rotate-lost:replaced"));
    CHECK(HeaderOf(Mock.HeadData()).KeyEpoch == 2);
    // The test keeps what the user saw
    Step(A.path(), QStringLiteral("settled:landed"), Password2);
    // Once: the process of the step above may end before its request is out,
    // and then the next start sends it
    CHECK(Mock.KeyPuts == 1);
    CheckReplaced(Mock, A.path(), OldKey, Old);
  }

  std::printf("-- the commit did not go through, and nobody said so\n");
  {
    QTemporaryDir A;
    RotationStart(Mock, A.path());
    QByteArray OldKey = CloudCrypto::UnwrapDekWithPassword(HeaderOf(Mock.HeadData()), Password1);
    qint64 Base = Mock.Head();
    Mock.DropCommit = true;
    Step(A.path(), QStringLiteral("rotate-lost:refused"));
    CHECK(Mock.Head() == Base + 1);
    CheckNotReplaced(Mock, A.path(), OldKey);
  }

  std::printf("-- the server is gone after the commit: decided at the next start\n");
  {
    QTemporaryDir A;
    RotationStart(Mock, A.path());
    QByteArray OldKey = CloudCrypto::UnwrapDekWithPassword(HeaderOf(Mock.HeadData()), Password1);
    QString Old = QString::fromLatin1(ReadRaw(A.path() + QStringLiteral("/recovery.txt")));
    Mock.LoseCommitReply = Mock.GoneAfterCommit = true;
    Step(A.path(), QStringLiteral("rotate-lost:undecided"));
    CHECK(Mock.Head() == 2);
    Mock.Fail.clear();
    Mock.GoneAfterCommit = false;
    Step(A.path(), QStringLiteral("told-later:landed"), Password1);
    CheckReplaced(Mock, A.path(), OldKey, Old);
  }
  {
    QTemporaryDir A;
    RotationStart(Mock, A.path());
    QByteArray OldKey = CloudCrypto::UnwrapDekWithPassword(HeaderOf(Mock.HeadData()), Password1);
    qint64 Base = Mock.Head();
    Mock.DropCommit = Mock.GoneAfterCommit = true;
    Step(A.path(), QStringLiteral("rotate-lost:undecided"));
    CHECK(Mock.Head() == Base);
    Mock.Fail.clear();
    Mock.GoneAfterCommit = false;
    Step(A.path(), QStringLiteral("told-later:refused"), Password1);
    CheckNotReplaced(Mock, A.path(), OldKey);
  }

  std::printf("-- two computers replace the key at the same moment\n");
  {
    QTemporaryDir A, B;
    Mock.Reset();
    Step(A.path(), QStringLiteral("first"));
    Step(B.path(), QStringLiteral("second"));
    // What B will upload, kept back
    Step(B.path(), QStringLiteral("rotate-plain"));
    QByteArray Theirs = Mock.HeadData();
    Mock.History[QStringLiteral("settings")].remove(Mock.Head());
    Mock.Heads[QStringLiteral("settings")] = Mock.Head() - 1;
    Mock.BeatCommit = Theirs;
    Step(A.path(), QStringLiteral("rotate-lost:taken-over"));
    CHECK(Mock.HeadData() == Theirs);
    Step(A.path(), QStringLiteral("unlock-new-key:") + Password2, Password1);
  }

  std::printf("-- signing out does not lose the team keys on their way\n");
  {
    QTemporaryDir A;
    RotationStart(Mock, A.path());
    Mock.FailPut[QStringLiteral("/v1/keys/me")] = 500;
    Step(A.path(), QStringLiteral("rotate"));
    CHECK(Mock.KeyPuts == 0);
    Step(A.path(), QStringLiteral("sign-out"), Password2);
    Mock.FailPut.clear();
    Step(A.path(), QStringLiteral("back-in:") + Password2, Password2);
    CHECK(Mock.KeyPuts == 1);
    Step(A.path(), QStringLiteral("check-team-key"), Password2);
  }

  std::printf("-- a new password survives a crash between the configuration and the state\n");
  {
    QTemporaryDir A;
    Mock.Reset();
    Step(A.path(), QStringLiteral("first"));
    CHECK(RunChild(A.path(), QStringLiteral("change-password-killed"), Password1) == 99);
    CHECK(HeaderOf(Mock.HeadData()).WrapEpoch == 1);
    Step(A.path(), QStringLiteral("sync"), Password2);
    CHECK(HeaderOf(Mock.HeadData()).WrapEpoch == 2);
    CHECK(!CloudCrypto::UnwrapDekWithPassword(HeaderOf(Mock.HeadData()), Password2).isEmpty());
  }

  std::printf("-- a state is sealed only next to an encrypted configuration\n");
  {
    QTemporaryDir A, B;
    Mock.Reset();
    Step(A.path(), QStringLiteral("config-not-written"));
    Mock.Reset();
    Step(B.path(), QStringLiteral("first"));
    Step(B.path(), QStringLiteral("password-not-written"));
    // Only the attempt that reached the disk went up
    CHECK(Mock.Head() == 2);
    CHECK(HeaderOf(Mock.HeadData()).WrapEpoch == 2);
  }

  std::printf("-- the reply about the account gets lost at sign-in\n");
  {
    QTemporaryDir A;
    Mock.Reset();
    Mock.Fail[QStringLiteral("/v1/me")] = 503;
    Step(A.path(), QStringLiteral("no-account"));
    Mock.Fail.clear();
    Step(A.path(), QStringLiteral("first"));
  }
}

static void ScenarioOldFormat(MockCloud & Mock)
{
  std::printf("-- cloud data of NixSCP 1.2.3: a confirmed way out\n");
  Mock.Reset();
  QTemporaryDir A, B, C, D;
  Step(A.path(), QStringLiteral("first"));
  QByteArray Old = Mock.HeadData();
  QByteArray OldTabs = Mock.HeadData(QStringLiteral("session"));
  CHECK(Old.contains("\"fmt\":2"));
  Mock.Reset();
  Mock.Inject(QStringLiteral("settings"), QByteArray(Old).replace("\"fmt\":2", "\"fmt\":1"));
  Mock.Inject(QStringLiteral("settings"), QByteArray(Old).replace("\"fmt\":2", "\"fmt\":1"));
  Mock.Inject(QStringLiteral("session"), QByteArray(OldTabs).replace("\"fmt\":2", "\"fmt\":1"));
  Step(B.path(), QStringLiteral("old-cloud"), QString());
  CHECK(Mock.Head() == 3);
  CHECK(Mock.History.value(QStringLiteral("settings")).contains(2));
  CloudCrypto::SnapshotHeader Head = HeaderOf(Mock.HeadData());
  CHECK(Head.KeyEpoch == 1);
  CHECK(!CloudCrypto::UnwrapDekWithPassword(Head, Password1).isEmpty());
  // Other computers join the new start
  Step(C.path(), QStringLiteral("second-sees:Mock/Fresh"), QString());

  std::printf("-- ...which an ordinary cloud does not have\n");
  int Commits = Mock.Commits;
  Step(D.path(), QStringLiteral("not-old-cloud"), QString());
  CHECK(Mock.Commits == Commits);

  std::printf("-- ...and an established computer does not take by itself\n");
  Commits = Mock.Commits;
  Mock.Inject(QStringLiteral("settings"), QByteArray(Old).replace("\"fmt\":2", "\"fmt\":1"));
  QByteArray Planted = Mock.HeadData();
  Step(B.path(), QStringLiteral("keeps-old-head-out"), Password1);
  CHECK(Mock.History.value(QStringLiteral("settings")).value(Mock.Head() - 1) == Planted || (Mock.HeadData() == Planted));
}

static void ScenarioRecovery(MockCloud & Mock)
{
  std::printf("-- a new recovery key\n");
  Mock.Reset();
  QTemporaryDir A;
  Step(A.path(), QStringLiteral("first"));
  QString Old = QString::fromLatin1(ReadRaw(A.path() + QStringLiteral("/recovery.txt")));
  Step(A.path(), QStringLiteral("new-recovery"));
  CloudCrypto::SnapshotHeader Head = HeaderOf(Mock.HeadData());
  QString New = QString::fromLatin1(ReadRaw(A.path() + QStringLiteral("/recovery2.txt")));
  CHECK(CloudCrypto::UnwrapDekWithRecoveryKey(Head, CloudCrypto::ParseRecoveryKey(Old)).isEmpty());
  CHECK(!CloudCrypto::UnwrapDekWithRecoveryKey(Head, CloudCrypto::ParseRecoveryKey(New)).isEmpty());
}

static void ScenarioRollback(MockCloud & Mock)
{
  std::printf("-- rollback of the settings\n");
  Mock.Reset();
  QTemporaryDir A, B, New;
  Step(A.path(), QStringLiteral("first"));
  Step(A.path(), QStringLiteral("add:Mock/A2"));
  Step(A.path(), QStringLiteral("add:Mock/A3"));
  qint64 Newest = Mock.Head();
  CHECK(Newest == 3);
  // Each step is a restart: what was seen comes from the sealed state
  Mock.ServedHead[QStringLiteral("settings")] = 1;
  int Commits = Mock.Commits;
  Step(A.path(), QStringLiteral("expect-error:older"));
  CHECK(Mock.Commits == Commits);
  // A new computer has seen nothing, and believes the old version: not detectable
  Step(New.path(), QStringLiteral("second"));
  Mock.ServedHead.clear();
  Step(A.path(), QStringLiteral("sync"));

  std::printf("-- rollback of the open tabs\n");
  Step(A.path(), QStringLiteral("session-push:2"));
  CHECK(Mock.Head(QStringLiteral("session")) == 2);
  Mock.ServedHead[QStringLiteral("session")] = 1;
  Step(A.path(), QStringLiteral("session-is:2"));
  // ...and when the cloud claims to have none at all
  Mock.ServedHead[QStringLiteral("session")] = 0;
  Step(A.path(), QStringLiteral("session-is:2"));
  Mock.ServedHead.clear();

  std::printf("-- a fork of the settings\n");
  Step(B.path(), QStringLiteral("second"));
  WriteRaw(B.path() + QStringLiteral("/head.bin"), Mock.HeadData());
  Step(B.path(), QStringLiteral("forge:direct"));
  QByteArray Good = Mock.HeadData();
  Commits = Mock.Commits;
  Mock.Inject(QStringLiteral("settings"), ReadRaw(B.path() + QStringLiteral("/forged1.bin")));
  Step(A.path(), QStringLiteral("expect-error:does not continue"));
  CHECK(Mock.Commits == Commits);
  // ...with a version in between that this computer never saw
  Mock.History[QStringLiteral("settings")].remove(Mock.Head());
  Mock.Heads[QStringLiteral("settings")] = Mock.Head() - 1;
  CHECK(Mock.HeadData() == Good);
  Step(B.path(), QStringLiteral("forge:gap"));
  Mock.Inject(QStringLiteral("settings"), ReadRaw(B.path() + QStringLiteral("/forged1.bin")));
  Mock.Inject(QStringLiteral("settings"), ReadRaw(B.path() + QStringLiteral("/forged2.bin")));
  Step(A.path(), QStringLiteral("expect-error:does not continue"));
  CHECK(Mock.Commits == Commits);
  Mock.History[QStringLiteral("settings")].remove(Mock.Head());
  Mock.Heads[QStringLiteral("settings")] = Mock.Head() - 1;
  Mock.History[QStringLiteral("settings")].remove(Mock.Head());
  Mock.Heads[QStringLiteral("settings")] = Mock.Head() - 1;

  // Honest versions this computer missed pass the same check
  Step(B.path(), QStringLiteral("add:Mock/B1"));
  Step(B.path(), QStringLiteral("add:Mock/B2"));
  Step(B.path(), QStringLiteral("add:Mock/B3"));
  Step(A.path(), QStringLiteral("has-site:Mock/B3"));
  // ...also when the oldest of them is no longer kept
  Step(B.path(), QStringLiteral("add:Mock/B4"));
  Step(B.path(), QStringLiteral("add:Mock/B5"));
  Mock.History[QStringLiteral("settings")].remove(Mock.Head() - 1);
  Step(A.path(), QStringLiteral("has-site:Mock/B5"));
}

static void ScenarioState(MockCloud & Mock)
{
  std::printf("-- the state file\n");
  Mock.Reset();
  QTemporaryDir A, Fresh;
  Step(A.path(), QStringLiteral("first"));
  Step(A.path(), QStringLiteral("sealed-state"));
  Step(A.path(), QStringLiteral("keep-dek"));
  Step(Fresh.path(), QStringLiteral("pre-key"));
  Step(Fresh.path(), QStringLiteral("pre-key-restarted"));

  // The attack of the audit: one offline write, to send everything elsewhere
  std::printf("-- a planted plain state file\n");
  MockCloud Evil;
  CHECK(Evil.Listen());
  QByteArray EvilKey = CloudCrypto::RandomBytes(CloudCrypto::KeyLength);
  QByteArray Planted = "[Account]\nBaseUrl=" + Evil.Url().toLatin1() + "\nEmail=" + Email.toLatin1() + "\nUser=" + User.toLatin1() +
    "\nRefreshToken=refresh-planted\n[Key]\nDek=" + EvilKey.toBase64() + "\nDekPassword=\nDekRecovery=\nSalt=\n[Settings]\nBaseVersion=0\nBaseHash=planted\n";
  QByteArray Sealed = ReadRaw(A.path() + QStringLiteral("/nixscp-cloud.ini"));
  WriteRaw(A.path() + QStringLiteral("/nixscp-cloud.ini"), Planted);
  int Commits = Mock.Commits;
  Step(A.path(), QStringLiteral("planted"));
  CHECK(Evil.Requests == 0);
  CHECK(Mock.Commits == Commits);
  // Its contents do not survive either
  CHECK(!ReadRaw(A.path() + QStringLiteral("/nixscp-cloud.ini")).contains("refresh-planted"));

  std::printf("-- the keys of the sync and the configuration differ\n");
  WriteRaw(A.path() + QStringLiteral("/nixscp-cloud.ini"), Sealed);
  Commits = Mock.Commits;
  qint64 Tabs = Mock.Head(QStringLiteral("session"));
  Step(A.path(), QStringLiteral("key-mismatch"));
  CHECK(Mock.Commits == Commits);
  CHECK(Mock.Head(QStringLiteral("session")) == Tabs);
}

static void ScenarioConflicts(MockCloud & Mock)
{
  std::printf("-- the same change on two computers is no conflict\n");
  Mock.Reset();
  QTemporaryDir A, B;
  Step(A.path(), QStringLiteral("first"));
  Step(B.path(), QStringLiteral("second"));
  Step(B.path(), QStringLiteral("add:Mock/Same"));
  Step(A.path(), QStringLiteral("no-conflict-dialog:Mock/Same"));

  std::printf("-- a postponed conflict after a restart\n");
  Step(B.path(), QStringLiteral("add:Mock/Other1"));
  Step(A.path(), QStringLiteral("pause"));
  Step(A.path(), QStringLiteral("resolve-cloud"));
  Step(A.path(), QStringLiteral("has-site:Mock/Other1"));
  Step(B.path(), QStringLiteral("add:Mock/Other2"));
  Step(A.path(), QStringLiteral("pause"));
  Step(A.path(), QStringLiteral("resolve-local"));
  CHECK(Mock.Head() > 0);
  Step(B.path(), QStringLiteral("has-site:Mock/Paused"));
}

static void ScenarioFailures(MockCloud & Mock)
{
  std::printf("-- a failed history request is not a wrong key\n");
  Mock.Reset();
  QTemporaryDir A;
  Step(A.path(), QStringLiteral("first"));
  Step(A.path(), QStringLiteral("add:Mock/A2"));
  // Someone in the account uploaded with a key of their own
  CloudCrypto::SnapshotHeader Foreign = HeaderOf(Mock.HeadData());
  Foreign.Version = Mock.Head() + 1;
  Foreign.Device = QStringLiteral("stranger");
  QByteArray Junk = CloudCrypto::EncodeSnapshot(Foreign, CloudCrypto::RandomBytes(CloudCrypto::KeyLength), "[X]\n");
  Mock.Inject(QStringLiteral("settings"), Junk);
  Mock.Fail[QStringLiteral("/v1/sync/history")] = 500;
  Step(A.path(), QStringLiteral("keeps-key"));
  Mock.Fail.clear();
  Mock.Fail[QStringLiteral("/__store/settings-1")] = 503;
  Mock.Fail[QStringLiteral("/__store/settings-2")] = 503;
  Step(A.path(), QStringLiteral("keeps-key"));
  Mock.Fail.clear();
  // ...and with the server back it is repaired as usual
  qint64 Junked = Mock.Head();
  Step(A.path(), QStringLiteral("repairs"));
  CHECK(Mock.Head() == Junked + 1);
  CHECK(HeaderOf(Mock.HeadData()).Device != QStringLiteral("stranger"));

  std::printf("-- a refused call does not leave the state on syncing\n");
  Mock.Fail[QStringLiteral("/v1/sync/head")] = 401;
  Step(A.path(), QStringLiteral("unauthorized"));
  Mock.Fail.clear();

  std::printf("-- sign-out during a sync\n");
  Mock.Delay[QStringLiteral("/v1/sync/head")] = 300;
  int Commits = Mock.Commits;
  Step(A.path(), QStringLiteral("sign-out-during-sync"));
  Mock.Delay.clear();
  CHECK(Mock.Commits == Commits);
  CHECK(!ReadRaw(A.path() + QStringLiteral("/nixscp-cloud.ini")).isEmpty());

  std::printf("-- oversized downloads\n");
  Mock.Reset();
  QTemporaryDir B, C;
  Step(B.path(), QStringLiteral("first"));
  Step(C.path(), QStringLiteral("second"));
  Step(C.path(), QStringLiteral("add:Mock/Big"));
  Mock.AnnouncedSize = 10;
  Step(B.path(), QStringLiteral("expect-error:more data"));
  Mock.AnnouncedSize = CloudCrypto::MaxSnapshotLength + 1;
  Step(B.path(), QStringLiteral("expect-error:impossible size"));
  Mock.AnnouncedSize = 0;
  Step(B.path(), QStringLiteral("has-site:Mock/Big"));
}

static void ScenarioTokens(MockCloud & Mock)
{
  std::printf("-- two NixSCP on one computer share the refresh token\n");
  Mock.Reset();
  QTemporaryDir A;
  Step(A.path(), QStringLiteral("first"));
  Step(A.path(), QStringLiteral("two-instances"));
  CHECK(Mock.Replays == 0);

  std::printf("-- a failing token endpoint keeps the sign-in\n");
  Mock.Fail[QStringLiteral("/v1/oauth/token")] = 400;
  Step(A.path(), QStringLiteral("refresh-fails"));
  Mock.Fail.clear();
  Step(A.path(), QStringLiteral("sync"));

  std::printf("-- two NixSCP on one computer and a password change\n");
  Mock.Reset();
  QTemporaryDir B;
  Step(B.path(), QStringLiteral("first"));
  Step(B.path(), QStringLiteral("second-instance-wraps"));
  CHECK(Mock.Replays == 0);
  CHECK(HeaderOf(Mock.HeadData()).WrapEpoch == 2);
  CHECK(CloudCrypto::UnwrapDekWithPassword(HeaderOf(Mock.HeadData()), Password1).isEmpty());

  std::printf("-- the signed upload header reaches the store\n");
  Mock.Reset();
  QTemporaryDir C;
  Step(C.path(), QStringLiteral("first"));
  CHECK(!Mock.SawIfNoneMatch);
  Mock.AskIfNoneMatch = true;
  Step(C.path(), QStringLiteral("check-upload-header"));
  CHECK(Mock.SawIfNoneMatch);
}

int main(int argc, char ** argv)
{
  QCoreApplication App(argc, argv);
  QStringList Args = App.arguments();
  if (Args.size() >= 3)
  {
    // A computer that signs in for the first time with this password
    if (Args[2] == QStringLiteral("sync-new-computer"))
    {
      Configuration & Config = Configuration::Instance();
      Config.SetIniFileName(Args[1] + QStringLiteral("/nixscp.ini"));
      Config.Load();
      CloudSync::Instance().Initialize(Config);
      CHECK(SignIn());
      CHECK(Unlock(Args.value(3)));
      CHECK(WaitFor([]() { return NotSyncing() && HasSite(QStringLiteral("Mock/A1")); }));
      CHECK(CloudSync::Instance().State() == CloudSync::csIdle);
      return (Failures == 0) ? 0 : 1;
    }
    return RunStep(Args[1], Args[2], Args.value(3));
  }
  MockCloud Mock;
  CHECK(Mock.Listen());
  qputenv("NIXSCP_CLOUD_URL", Mock.Url().toLatin1());
  // MOCK_ONLY: the one scenario being worked on
  const QList<QPair<QString, void (*)(MockCloud &)>> Scenarios
  {
    {QStringLiteral("stale"), ScenarioStaleWraps},
    {QStringLiteral("rotation"), ScenarioRotation},
    {QStringLiteral("killed"), ScenarioRotationKilled},
    {QStringLiteral("lost"), ScenarioRotationLost},
    {QStringLiteral("old-format"), ScenarioOldFormat},
    {QStringLiteral("recovery"), ScenarioRecovery},
    {QStringLiteral("rollback"), ScenarioRollback},
    {QStringLiteral("state"), ScenarioState},
    {QStringLiteral("conflicts"), ScenarioConflicts},
    {QStringLiteral("failures"), ScenarioFailures},
    {QStringLiteral("tokens"), ScenarioTokens},
  };
  QString Only = qEnvironmentVariable("MOCK_ONLY");
  for (const auto & Scenario : Scenarios)
  {
    if (Only.isEmpty() || (Only == Scenario.first))
    {
      Scenario.second(Mock);
    }
  }
  if (Failures == 0)
  {
    std::printf("All mock cloud sync tests passed\n");
  }
  return (Failures == 0) ? 0 : 1;
}
