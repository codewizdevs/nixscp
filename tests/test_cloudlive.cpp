// NixSCP Pro sync against a real NixSCP cloud server (core/CloudSync): two
// computers (separate configuration folders, separate processes) sign in to
// the same account, set up and unlock the sync password, sync sites both
// ways, hand over the open sessions, and hit a conflict that is resolved
// with the cloud settings.
//
// Skipped unless NIXSCP_TEST_CLOUD_COOKIE is the website session cookie
// (nx_session) of a test account with an active license; the server is
// NIXSCP_CLOUD_URL (default https://nixscp.com). The account's synced data
// should be empty before the run.
#include "core/CloudCrypto.h"
#include "core/CloudSync.h"
#include "core/ConfigVault.h"
#include "core/Configuration.h"
#include "core/SessionData.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrlQuery>
#include <cstdio>
#include <memory>

static int Failures = 0;

#define CHECK(Cond) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); std::fflush(stdout); Failures++; } } while (0)

static const QString SyncPassword = QStringLiteral("live test sync password");
static const QByteArray SessionPayload = "[Sessions\\NixSCP%20Cloud%20Session/0000]\nHostName=tabs.example.com\nIsWorkspace=1\n";

static bool WaitFor(const std::function<bool()> & Condition, int TimeoutMs = 30000)
{
  QElapsedTimer Timer;
  Timer.start();
  while (!Condition() && (Timer.elapsed() < TimeoutMs))
  {
    QEventLoop Loop;
    QTimer::singleShot(50, &Loop, &QEventLoop::quit);
    Loop.exec();
  }
  return Condition();
}

static void Trace(const char * Where)
{
  CloudSync & Sync = CloudSync::Instance();
  std::printf("  [%s] state=%d status=%s error=%s\n", Where, int(Sync.State()), qPrintable(Sync.StatusText()), qPrintable(Sync.LastError()));
  std::fflush(stdout);
}

static bool Idle()
{
  CloudSync::TState State = CloudSync::Instance().State();
  return (State != CloudSync::csSyncing);
}

// What the browser does: the website session approves this computer
static bool SignIn()
{
  CloudSync & Sync = CloudSync::Instance();
  CloudSync::SignInRequest Request = Sync.NewSignInRequest(53682);
  QUrlQuery Query(QUrl(Request.Url));
  QJsonObject Body
  {
    {QStringLiteral("client_id"), QStringLiteral("nixscp-desktop")},
    {QStringLiteral("redirect_uri"), Request.RedirectUri},
    {QStringLiteral("code_challenge"), Query.queryItemValue(QStringLiteral("code_challenge"))},
    {QStringLiteral("code_challenge_method"), QStringLiteral("S256")},
    {QStringLiteral("state"), Request.State},
    {QStringLiteral("device_name"), QStringLiteral("live test")},
  };
  QNetworkAccessManager Manager;
  QNetworkRequest Http(QUrl(Sync.Client().BaseUrl() + QStringLiteral("/v1/oauth/authorize")));
  Http.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
  Http.setRawHeader("Cookie", "nx_session=" + qgetenv("NIXSCP_TEST_CLOUD_COOKIE"));
  QNetworkReply * Reply = Manager.post(Http, QJsonDocument(Body).toJson());
  WaitFor([Reply]() { return Reply->isFinished(); });
  QUrl Redirect(QJsonDocument::fromJson(Reply->readAll()).object().value(QStringLiteral("redirect_url")).toString());
  delete Reply;
  QString Code = QUrlQuery(Redirect).queryItemValue(QStringLiteral("code"));
  if (Code.isEmpty())
  {
    return false;
  }
  auto Done = std::make_shared<int>(0);
  Sync.CompleteSignIn(Request, Code, [Done](bool Ok, const QString &) { *Done = Ok ? 1 : 2; });
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

// ---- Steps, each run in its own process (one computer) ----

static void StepFirst()
{
  CloudSync & Sync = CloudSync::Instance();
  CHECK(SignIn());
  Trace("after sign-in");
  CHECK(Sync.State() == CloudSync::csNeedKey);
  auto Probe = std::make_shared<int>(-1);
  Sync.ProbeCloud([Probe](bool Ok, bool HasData, const QString &) { *Probe = Ok ? (HasData ? 1 : 0) : 2; });
  CHECK(WaitFor([Probe]() { return *Probe >= 0; }) && (*Probe == 0));
  AddSite(QStringLiteral("Live/A1"));
  auto Created = std::make_shared<int>(0);
  Sync.CreateKey(SyncPassword, true, [Created](bool Ok, const QString & Recovery) { *Created = (Ok && !Recovery.isEmpty()) ? 1 : 2; });
  CHECK(*Created == 1);
  CHECK(WaitFor(Idle));
  Trace("after first sync");
  CHECK(Sync.State() == CloudSync::csIdle);
  // The open tabs for the next computer
  Sync.FlushSession(SessionPayload, 15000);
}

static void StepSecond()
{
  CloudSync & Sync = CloudSync::Instance();
  CHECK(SignIn());
  auto Result = std::make_shared<int>(0);
  Sync.UnlockKey(QStringLiteral("wrong password"), false, [Result](bool Ok, const QString &) { *Result = Ok ? 1 : 2; });
  CHECK(WaitFor([Result]() { return *Result != 0; }) && (*Result == 2));
  CHECK(!Sync.HasKey());
  *Result = 0;
  Sync.UnlockKey(SyncPassword, false, [Result](bool Ok, const QString &) { *Result = Ok ? 1 : 2; });
  CHECK(WaitFor([Result]() { return *Result != 0; }) && (*Result == 1));
  CHECK(WaitFor([]() { return Idle() && HasSite(QStringLiteral("Live/A1")); }));
  CHECK(Sync.State() == CloudSync::csIdle);
  // The tabs closed on the first computer
  auto Session = std::make_shared<QByteArray>();
  auto Fetched = std::make_shared<bool>(false);
  Sync.FetchSession(15000, [Session, Fetched](bool Found, const QByteArray & Payload) { *Fetched = true; if (Found) *Session = Payload; });
  CHECK(WaitFor([Fetched]() { return *Fetched; }));
  CHECK(Session->contains("tabs.example.com"));
  // A change of this computer
  AddSite(QStringLiteral("Live/B1"));
  Sync.SyncNow();
  CHECK(WaitFor([]() { return Idle() && (CloudSync::Instance().State() == CloudSync::csIdle); }));
}

static void StepConflict()
{
  // The first computer again (still unlocked), changed while "offline"
  CloudSync & Sync = CloudSync::Instance();
  CHECK(Sync.HasKey());
  auto Conflict = std::make_shared<bool>(false);
  QObject::connect(&Sync, &CloudSync::ConflictDetected, [Conflict]() { *Conflict = true; });
  AddSite(QStringLiteral("Live/A2"));
  Sync.SyncNow();
  CHECK(WaitFor([Conflict]() { return *Conflict; }));
  CHECK(Sync.State() == CloudSync::csConflict);
  // Never resolved automatically; the user takes the cloud settings
  Sync.ResolveConflict(CloudSync::crUseCloud);
  CHECK(WaitFor([]() { return Idle() && (CloudSync::Instance().State() == CloudSync::csIdle); }));
  CHECK(HasSite(QStringLiteral("Live/A1")));
  CHECK(HasSite(QStringLiteral("Live/B1")));
  CHECK(!HasSite(QStringLiteral("Live/A2")));
  // The replaced local settings were backed up
  QDir Backups(QFileInfo(Configuration::Instance().IniFileName()).absolutePath() + QStringLiteral("/backups"));
  CHECK(!Backups.entryList({QStringLiteral("nixscp-*.ini")}, QDir::Files).isEmpty());
}

// What an attacker who got into the account (but not the sync password) can do:
// sign in a computer and upload a snapshot with a key of its own on top
static bool UploadJunk(const QString & Slot)
{
  CloudClient & Client = CloudSync::Instance().Client();
  auto Call = [&Client](const QByteArray & Method, const QString & Path, const QJsonObject & Body)
  {
    auto Result = std::make_shared<CloudClient::Reply>();
    auto Done = std::make_shared<bool>(false);
    Client.Api(Method, Path, Body, [Result, Done](const CloudClient::Reply & R) { *Result = R; *Done = true; });
    WaitFor([Done]() { return *Done; });
    return *Result;
  };
  CloudClient::Reply Me = Call("GET", QStringLiteral("/v1/me"), QJsonObject());
  CloudClient::Reply Head = Call("GET", QStringLiteral("/v1/sync/head?slot=%1").arg(Slot), QJsonObject());
  qint64 Base = Head.Json.value(QStringLiteral("version")).toInteger();
  CloudCrypto::SnapshotHeader Header;
  Header.Slot = Slot;
  Header.User = Me.Json.value(QStringLiteral("user")).toObject().value(QStringLiteral("uuid")).toString();
  Header.Version = Base + 1;
  Header.SavedAt = QDateTime::fromString(QStringLiteral("2099-01-01T00:00:00Z"), Qt::ISODate);
  Header.Device = QStringLiteral("attacker");
  Header.Kdf.MemoryKib = 256;
  Header.Kdf.Iterations = 1;
  QByteArray Dek = CloudCrypto::RandomBytes(CloudCrypto::KeyLength);
  CloudCrypto::WrapDek(Header, Dek, QStringLiteral("not the sync password"), QByteArray());
  QByteArray Snapshot = CloudCrypto::EncodeSnapshot(Header, Dek, "[Sessions\\Evil]\nHostName=evil.example.com\n");
  CloudClient::Reply Up = Call("POST", QStringLiteral("/v1/sync/upload-url"),
    {{QStringLiteral("slot"), Slot}, {QStringLiteral("base_version"), Base}, {QStringLiteral("size"), Snapshot.size()}});
  if (!Up.Ok()) return false;
  auto PutDone = std::make_shared<int>(0);
  Client.Transfer("PUT", QUrl(Up.Json.value(QStringLiteral("url")).toString()), Snapshot, {{"Content-Type", "application/octet-stream"}},
    [PutDone](const CloudClient::Reply & R) { *PutDone = R.Ok() ? 1 : 2; });
  WaitFor([PutDone]() { return *PutDone != 0; });
  CloudClient::Reply Commit = Call("POST", QStringLiteral("/v1/sync/commit"),
    {{QStringLiteral("slot"), Slot}, {QStringLiteral("base_version"), Base}, {QStringLiteral("object_key"), Up.Json.value(QStringLiteral("object_key"))},
     {QStringLiteral("size"), Snapshot.size()},
     {QStringLiteral("sha256"), QString::fromLatin1(QCryptographicHash::hash(Snapshot, QCryptographicHash::Sha256).toHex())}});
  return Commit.Ok();
}

static void StepJunk()
{
  // An intruder's computer: signed in, never unlocked
  CHECK(SignIn());
  CHECK(UploadJunk(QStringLiteral("settings")));
  CHECK(UploadJunk(QStringLiteral("session")));
}

static void StepRepair()
{
  // Computer A: ignores the foreign snapshot, warns, and uploads valid settings on top
  CloudSync & Sync = CloudSync::Instance();
  auto Warned = std::make_shared<QString>();
  QObject::connect(&Sync, &CloudSync::ForeignSnapshotIgnored, [Warned](const QString & Device) { *Warned = Device; });
  Sync.SyncNow();
  CHECK(WaitFor([Warned]() { return !Warned->isEmpty() && Idle(); }));
  Trace("after repair");
  CHECK(*Warned == QStringLiteral("attacker"));
  CHECK(Sync.HasKey());
  CHECK(WaitFor([]() { return Idle() && (CloudSync::Instance().State() == CloudSync::csIdle); }));
  CHECK(HasSite(QStringLiteral("Live/A1")));
  CHECK(!HasSite(QStringLiteral("Evil")));
  // The session slot: its forged "saved in 2099" does not stop uploads
  Sync.FlushSession("[Sessions\\NixSCP%20Cloud%20Session/0000]\nHostName=after-repair.example.com\nIsWorkspace=1\n", 15000);
  auto Session = std::make_shared<QByteArray>();
  auto Fetched = std::make_shared<bool>(false);
  Sync.FetchSession(15000, [Session, Fetched](bool Found, const QByteArray & Payload) { *Fetched = true; if (Found) *Session = Payload; });
  CHECK(WaitFor([Fetched]() { return *Fetched; }));
  CHECK(Session->contains("after-repair.example.com"));
}

static void StepUnlockPastJunk()
{
  // A new computer while the newest snapshot is foreign: the right password still unlocks
  CloudSync & Sync = CloudSync::Instance();
  CHECK(SignIn());
  auto Result = std::make_shared<int>(0);
  auto Message = std::make_shared<QString>();
  Sync.UnlockKey(SyncPassword, false, [Result, Message](bool Ok, const QString & Error) { *Result = Ok ? 1 : 2; *Message = Error; });
  CHECK(WaitFor([Result]() { return *Result != 0; }, 60000));
  if (*Result != 1) std::printf("  unlock error: %s\n", qPrintable(*Message));
  CHECK(*Result == 1);
  CHECK(WaitFor([]() { return Idle() && HasSite(QStringLiteral("Live/A1")); }, 60000));
  CHECK(!HasSite(QStringLiteral("Evil")));
}

static void StepRemoved()
{
  // Computer A is removed on the website: its next sync signs it out, with the reason
  CloudSync & Sync = CloudSync::Instance();
  auto Account = std::make_shared<QJsonObject>();
  auto Loaded = std::make_shared<bool>(false);
  Sync.FetchAccount([Account, Loaded](bool, const QJsonObject & Me) { *Account = Me; *Loaded = true; });
  CHECK(WaitFor([Loaded]() { return *Loaded; }));
  QString DeviceUuid = Account->value(QStringLiteral("device")).toObject().value(QStringLiteral("uuid")).toString();
  CHECK(!DeviceUuid.isEmpty());
  QNetworkAccessManager Manager;
  QNetworkRequest Http(QUrl(Sync.Client().BaseUrl() + QStringLiteral("/v1/devices/") + DeviceUuid));
  Http.setRawHeader("Cookie", "nx_session=" + qgetenv("NIXSCP_TEST_CLOUD_COOKIE"));
  Http.setRawHeader("Origin", Sync.Client().BaseUrl().toLatin1());
  QNetworkReply * Reply = Manager.deleteResource(Http);
  WaitFor([Reply]() { return Reply->isFinished(); });
  CHECK(Reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200);
  delete Reply;
  auto Message = std::make_shared<QString>();
  QObject::connect(&Sync, &CloudSync::SignedOutRemotely, [Message](const QString & Text) { *Message = Text; });
  Sync.SyncNow();
  CHECK(WaitFor([Message]() { return !Message->isEmpty(); }));
  Trace("after removal");
  CHECK(Message->contains(QStringLiteral("removed from your NixSCP account")));
  CHECK(Sync.State() == CloudSync::csSignedOut);
  // The reason stays visible (status bar indicator, Cloud Sync dialog)
  CHECK(WaitFor([]() { return CloudSync::Instance().LastError().contains(QStringLiteral("removed")); }, 5000));
  CHECK(Sync.HasKey());
}

static int RunStep(const QString & Dir, const QString & Step)
{
  Configuration & Config = Configuration::Instance();
  Config.SetIniFileName(Dir + QStringLiteral("/nixscp.ini"));
  // as the application at startup: a computer that set up sync before has its
  // configuration encrypted with the sync (NixSCP) password
  bool Encrypted = ConfigVault::IsSealedFile(Config.IniFileName());
  if (Encrypted)
  {
    CHECK(!ConfigVault::Unlock(Config.IniFileName(), QStringLiteral("wrong password"), false));
    CHECK(ConfigVault::Unlock(Config.IniFileName(), SyncPassword, false));
  }
  Config.Load();
  CHECK(!Config.IsLocked());
  CloudSync::Instance().Initialize(Config);
  if (Step == QStringLiteral("first")) StepFirst();
  else if (Step == QStringLiteral("second")) StepSecond();
  else if (Step == QStringLiteral("conflict")) StepConflict();
  else if (Step == QStringLiteral("junk")) StepJunk();
  else if (Step == QStringLiteral("repair")) StepRepair();
  else if (Step == QStringLiteral("unlock-past-junk")) StepUnlockPastJunk();
  else if (Step == QStringLiteral("removed")) StepRemoved();
  // with the key, the configuration is (now) encrypted on disk
  if (CloudSync::Instance().HasKey())
  {
    CHECK(ConfigVault::IsSealedFile(Config.IniFileName()));
  }
  std::printf("step %s: %s\n", qPrintable(Step), (Failures == 0) ? "ok" : "FAILED");
  return (Failures == 0) ? 0 : 1;
}

int main(int argc, char ** argv)
{
  QCoreApplication App(argc, argv);
  QStringList Args = App.arguments();
  if (Args.size() == 3)
  {
    return RunStep(Args[1], Args[2]);
  }
  if (qEnvironmentVariableIsEmpty("NIXSCP_TEST_CLOUD_COOKIE"))
  {
    std::printf("SKIP: set NIXSCP_TEST_CLOUD_COOKIE (and optionally NIXSCP_CLOUD_URL) to test against a cloud server\n");
    return 0;
  }
  QTemporaryDir ComputerA, ComputerB, Intruder, Intruder2, ComputerE;
  const QList<QPair<QString, QString>> Steps
  {
    {ComputerA.path(), QStringLiteral("first")},
    {ComputerB.path(), QStringLiteral("second")},
    {ComputerA.path(), QStringLiteral("conflict")},
    {Intruder.path(), QStringLiteral("junk")},
    {ComputerA.path(), QStringLiteral("repair")},
    {Intruder2.path(), QStringLiteral("junk")},
    {ComputerE.path(), QStringLiteral("unlock-past-junk")},
    {ComputerA.path(), QStringLiteral("removed")},
  };
  for (const auto & Step : Steps)
  {
    QProcess Process;
    Process.setProcessChannelMode(QProcess::ForwardedChannels);
    Process.start(App.applicationFilePath(), {Step.first, Step.second});
    Process.waitForFinished(120000);
    CHECK((Process.exitStatus() == QProcess::NormalExit) && (Process.exitCode() == 0));
    if (Failures > 0)
    {
      break;
    }
  }
  if (Failures == 0)
  {
    std::printf("All live cloud sync tests passed\n");
  }
  return (Failures == 0) ? 0 : 1;
}
