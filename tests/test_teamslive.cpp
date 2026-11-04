// NixSCP Teams against a real NixSCP cloud server (core/TeamSync): two
// accounts on separate "computers" (configuration folders, processes): Alice
// creates a team and shares a site folder, invites Bob with a code, Bob joins
// and gets the sites (members' local changes are reverted), Alice adds a site
// and shares single sites (copied and moved), stops sharing them again,
// removes Bob (the key is renewed, Bob loses the sites), and deletes the team.
//
// Skipped unless NIXSCP_TEST_CLOUD_COOKIE and NIXSCP_TEST_CLOUD_COOKIE2 are
// website session cookies of two fresh test accounts (beta or a license).
#include "core/CloudSync.h"
#include "core/ConfigVault.h"
#include "core/Configuration.h"
#include "core/SessionData.h"
#include "core/TeamSync.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QTemporaryDir>
#include <QUrlQuery>

#include <cstdio>
#include <functional>
#include <memory>

static int Failures = 0;

#define CHECK(Cond) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); std::fflush(stdout); Failures++; } } while (0)
#define CHECK_MSG(Cond, Msg) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n  %s\n", __FILE__, __LINE__, #Cond, qPrintable(Msg)); std::fflush(stdout); Failures++; } } while (0)

static const QString SyncPassword = QStringLiteral("teams live test sync password");
static const QString TeamFolder = QStringLiteral("Team: Live Team");

static bool WaitFor(const std::function<bool()> & Condition, int TimeoutMs = 60000)
{
  QElapsedTimer Timer;
  Timer.start();
  while (!Condition())
  {
    if (Timer.elapsed() > TimeoutMs) return false;
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  }
  return true;
}

static bool SignIn(const QByteArray & Cookie)
{
  CloudSync & Sync = CloudSync::Instance();
  CloudSync::SignInRequest Request = Sync.NewSignInRequest(53684);
  QUrlQuery Query(QUrl(Request.Url));
  QJsonObject Body{{QStringLiteral("client_id"), QStringLiteral("nixscp-desktop")}, {QStringLiteral("redirect_uri"), Request.RedirectUri},
    {QStringLiteral("code_challenge"), Query.queryItemValue(QStringLiteral("code_challenge"))},
    {QStringLiteral("code_challenge_method"), QStringLiteral("S256")}, {QStringLiteral("state"), Request.State},
    {QStringLiteral("device_name"), QStringLiteral("teams live test")}};
  QNetworkAccessManager Manager;
  QNetworkRequest Http(QUrl(Sync.Client().BaseUrl() + QStringLiteral("/v1/oauth/authorize")));
  Http.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
  Http.setRawHeader("Cookie", "nx_session=" + Cookie);
  QNetworkReply * Reply = Manager.post(Http, QJsonDocument(Body).toJson());
  WaitFor([Reply]() { return Reply->isFinished(); });
  QString Code = QUrlQuery(QUrl(QJsonDocument::fromJson(Reply->readAll()).object().value(QStringLiteral("redirect_url")).toString()))
    .queryItemValue(QStringLiteral("code"));
  delete Reply;
  if (Code.isEmpty()) return false;
  auto Done = std::make_shared<int>(0);
  Sync.CompleteSignIn(Request, Code, [Done](bool Ok, const QString &) { *Done = Ok ? 1 : 2; });
  WaitFor([Done]() { return *Done != 0; });
  return *Done == 1;
}

// Runs an asynchronous TeamSync operation to its end
static bool Wait(const std::function<void(const TeamSync::Done &)> & Operation, QString * Error = nullptr)
{
  auto Result = std::make_shared<int>(0);
  auto Message = std::make_shared<QString>();
  Operation([Result, Message](bool Ok, const QString & E) { *Result = Ok ? 1 : 2; *Message = E; });
  WaitFor([Result]() { return *Result != 0; }, 120000);
  if (Error != nullptr) *Error = *Message;
  if (*Result != 1) std::printf("  operation failed: %s\n", qPrintable(*Message));
  return *Result == 1;
}

static std::shared_ptr<SessionData> Site(const QString & Name)
{
  return Configuration::Instance().StoredSessions().FindByName(Name);
}

static void AddSite(const QString & Name, const QString & Password)
{
  SessionData S;
  S.Name = Name;
  S.HostName = Name.section(QLatin1Char('/'), -1) + QStringLiteral(".example.com");
  S.UserName = QStringLiteral("deploy");
  S.Password = Password;
  S.FSProtocol = fsSFTP;
  Configuration::Instance().StoredSessions().Add(std::make_shared<SessionData>(S));
  Configuration::Instance().Save();
}

static QString TeamUuid()
{
  for (const TeamSync::Team & T : TeamSync::Instance().Teams())
  {
    if (T.Name == QStringLiteral("Live Team")) return T.Uuid;
  }
  return QString();
}

static void Write(const QString & File, const QByteArray & Data)
{
  QFile F(File);
  if (F.open(QIODevice::WriteOnly)) F.write(Data);
}

static QByteArray Read(const QString & File)
{
  QFile F(File);
  return F.open(QIODevice::ReadOnly) ? F.readAll().trimmed() : QByteArray();
}

static int RunStep(const QString & Dir, const QString & Step, const QString & Shared)
{
  Configuration & Config = Configuration::Instance();
  Config.SetIniFileName(Dir + QStringLiteral("/nixscp.ini"));
  if (ConfigVault::IsSealedFile(Config.IniFileName()))
  {
    CHECK(ConfigVault::Unlock(Config.IniFileName(), SyncPassword, false));
  }
  Config.Load();
  CloudSync & Sync = CloudSync::Instance();
  Sync.Initialize(Config);
  TeamSync & Teams = TeamSync::Instance();
  Teams.Initialize(Config, Sync);
  QStringList Notices;
  QObject::connect(&Teams, &TeamSync::Notice, &Teams, [&Notices](const QString & M) { Notices.append(M); });
  QByteArray Cookie = qgetenv(Step.startsWith(QStringLiteral("alice")) ? "NIXSCP_TEST_CLOUD_COOKIE" : "NIXSCP_TEST_CLOUD_COOKIE2");

  if (Step.endsWith(QStringLiteral("-setup")))
  {
    CHECK(SignIn(Cookie));
    auto Done = std::make_shared<int>(0);
    Sync.CreateKey(SyncPassword, true, [Done](bool Ok, const QString &) { *Done = Ok ? 1 : 2; });
    WaitFor([Done]() { return *Done != 0; });
    CHECK(*Done == 1);
    CHECK(Teams.Available());
  }
  if (Step == QStringLiteral("alice-setup"))
  {
    AddSite(QStringLiteral("Shared/web1"), QStringLiteral("web1-password"));
    AddSite(QStringLiteral("Private/mine"), QStringLiteral("mine-password"));
    CHECK(Wait([&](const auto & D) { Teams.CreateTeam(QStringLiteral("Live Team"), D); }));
    QString Uuid = TeamUuid();
    CHECK(!Uuid.isEmpty());
    const TeamSync::Team * T = Teams.Find(Uuid);
    CHECK((T != nullptr) && (T->Role == QStringLiteral("owner")) && !T->Key.isEmpty());
    CHECK(Wait([&](const auto & D) { Teams.ShareFolder(Uuid, QStringLiteral("Shared"), D); }));
    CHECK(Site(TeamFolder + QStringLiteral("/Shared/web1")) != nullptr);
    CHECK(Site(QStringLiteral("Shared/web1")) == nullptr);
    CHECK(Teams.Find(Uuid)->HeadVersion >= 1);
    // the invite code goes to Bob "privately" (a file)
    auto Code = std::make_shared<QString>();
    auto Ok = std::make_shared<int>(0);
    Teams.CreateInvite(Uuid, QStringLiteral("member"), 1, [Code, Ok](bool Success, const QString &, const QString & C) { *Code = C; *Ok = Success ? 1 : 2; });
    WaitFor([Ok]() { return *Ok != 0; });
    CHECK((*Ok == 1) && (Code->size() == 49));
    Write(Shared + QStringLiteral("/code"), Code->toLatin1());
    // a tampered code does not get in, and does not use up the invite
    QString Bad = *Code;
    Bad[20] = (Bad[20] == QLatin1Char('A')) ? QLatin1Char('B') : QLatin1Char('A');
    Write(Shared + QStringLiteral("/badcode"), Bad.toLatin1());
  }
  else if (Step == QStringLiteral("bob-setup"))
  {
    QString Error;
    CHECK(!Wait([&](const auto & D) { Teams.JoinWithCode(QString::fromLatin1(Read(Shared + QStringLiteral("/badcode"))), D); }, &Error));
    CHECK(Error.contains(QStringLiteral("not valid")));
    CHECK(Wait([&](const auto & D) { Teams.JoinWithCode(QString::fromLatin1(Read(Shared + QStringLiteral("/code"))), D); }));
    QString Uuid = TeamUuid();
    const TeamSync::Team * T = Teams.Find(Uuid);
    CHECK((T != nullptr) && (T->Role == QStringLiteral("member")) && !T->Key.isEmpty());
    auto Web1 = Site(TeamFolder + QStringLiteral("/Shared/web1"));
    CHECK((Web1 != nullptr) && (Web1->Password == QStringLiteral("web1-password")) && (Web1->HostName == QStringLiteral("web1.example.com")));
    CHECK(Site(TeamFolder + QStringLiteral("/Private/mine")) == nullptr);
    // a member's change is reverted on the next sync
    Web1->HostName = QStringLiteral("changed.example.com");
    Config.Save();
    CHECK(Wait([&](const auto & D) { Teams.SyncAll(D); }));
    CHECK(Site(TeamFolder + QStringLiteral("/Shared/web1"))->HostName == QStringLiteral("web1.example.com"));
    CHECK(!Notices.isEmpty() && Notices.last().contains(QStringLiteral("reverted")));
    // agent access stays this person's own: set here, kept over syncs, never pushed
    Site(TeamFolder + QStringLiteral("/Shared/web1"))->AgentAccess = 1;
    Config.Save();
    CHECK(Wait([&](const auto & D) { Teams.SyncAll(D); }));
    CHECK(Site(TeamFolder + QStringLiteral("/Shared/web1"))->AgentAccess == 1);
    // the team's sites are not in the personal sync
    CHECK(!Config.ExportData().SaveToData().isEmpty());
  }
  else if (Step == QStringLiteral("alice-add"))
  {
    CHECK(Wait([&](const auto & D) { Teams.SyncAll(D); }));
    QString Uuid = TeamUuid();
    CHECK(Teams.Find(Uuid)->MemberCount == 2);
    AddSite(TeamFolder + QStringLiteral("/Shared/db1"), QStringLiteral("db1-password"));
    CHECK(Wait([&](const auto & D) { Teams.SyncAll(D); }));
    // a single site, copied into a subfolder: the personal one stays
    CHECK(Wait([&](const auto & D) { Teams.ShareSites(Uuid, {QStringLiteral("Private/mine")}, QStringLiteral("Ops"), false, D); }));
    CHECK(Site(TeamFolder + QStringLiteral("/Ops/mine")) != nullptr);
    CHECK(Site(QStringLiteral("Private/mine")) != nullptr);
    // a single site, moved to the team's top level
    AddSite(QStringLiteral("Temp/gone"), QStringLiteral("gone-password"));
    CHECK(Wait([&](const auto & D) { Teams.ShareSites(Uuid, {QStringLiteral("Temp/gone")}, QString(), true, D); }));
    CHECK(Site(TeamFolder + QStringLiteral("/gone")) != nullptr);
    CHECK(Site(QStringLiteral("Temp/gone")) == nullptr);
    CHECK(Teams.SharedSites(Uuid) == QStringList({QStringLiteral("gone"), QStringLiteral("Ops/mine"), QStringLiteral("Shared/db1"), QStringLiteral("Shared/web1")}));
    // Bob's agent access setting did not come to Alice
    CHECK(Site(TeamFolder + QStringLiteral("/Shared/web1"))->AgentAccess == 0);
  }
  else if (Step == QStringLiteral("bob-pull"))
  {
    // the open site lists (Login dialog) are told to reload
    int Reloads = 0;
    QObject::connect(&Configuration::Instance(), &Configuration::StoredSessionsChanged, [&Reloads]() { Reloads++; });
    int Added = 0;
    QObject::connect(&Teams, &TeamSync::SitesReceived, [&Added](const QString &, int A, int, int) { Added += A; });
    CHECK(Wait([&](const auto & D) { Teams.SyncAll(D); }));
    CHECK(Reloads > 0);
    CHECK(Added == 3);
    CHECK(Site(TeamFolder + QStringLiteral("/Ops/mine")) != nullptr);
    CHECK(Site(TeamFolder + QStringLiteral("/gone")) != nullptr);
    // members cannot stop sharing
    CHECK(!Wait([&](const auto & D) { Teams.Unshare(TeamUuid(), {QStringLiteral("gone")}, true, D); }));
    auto Db1 = Site(TeamFolder + QStringLiteral("/Shared/db1"));
    CHECK((Db1 != nullptr) && (Db1->Password == QStringLiteral("db1-password")));
    CHECK(Site(TeamFolder + QStringLiteral("/Shared/web1"))->AgentAccess == 1);
  }
  else if (Step == QStringLiteral("alice-unshare"))
  {
    CHECK(Wait([&](const auto & D) { Teams.SyncAll(D); }));
    QString Uuid = TeamUuid();
    // stop sharing, keeping a personal copy
    CHECK(Wait([&](const auto & D) { Teams.Unshare(Uuid, {QStringLiteral("gone")}, true, D); }));
    CHECK(Site(TeamFolder + QStringLiteral("/gone")) == nullptr);
    CHECK((Site(QStringLiteral("gone")) != nullptr) && (Site(QStringLiteral("gone"))->Password == QStringLiteral("gone-password")));
    // a whole subfolder, deleted
    CHECK(Wait([&](const auto & D) { Teams.Unshare(Uuid, {QStringLiteral("Ops/")}, false, D); }));
    CHECK(Site(TeamFolder + QStringLiteral("/Ops/mine")) == nullptr);
    CHECK(Site(QStringLiteral("Ops/mine")) == nullptr);
    CHECK(Teams.SharedSites(Uuid) == QStringList({QStringLiteral("Shared/db1"), QStringLiteral("Shared/web1")}));
  }
  else if (Step == QStringLiteral("bob-unshared"))
  {
    int Removed = 0;
    QObject::connect(&Teams, &TeamSync::SitesReceived, [&Removed](const QString &, int, int R, int) { Removed += R; });
    CHECK(Wait([&](const auto & D) { Teams.SyncAll(D); }));
    CHECK(Removed == 2);
    CHECK(Site(TeamFolder + QStringLiteral("/gone")) == nullptr);
    CHECK(Site(TeamFolder + QStringLiteral("/Ops/mine")) == nullptr);
    CHECK(Site(QStringLiteral("gone")) == nullptr);
    CHECK(Site(TeamFolder + QStringLiteral("/Shared/web1")) != nullptr);
  }
  else if (Step == QStringLiteral("alice-remove"))
  {
    CHECK(Wait([&](const auto & D) { Teams.SyncAll(D); }));
    QString Uuid = TeamUuid();
    qint64 OldVersion = Teams.Find(Uuid)->KeyVersion;
    auto Members = std::make_shared<QJsonArray>();
    auto Ok = std::make_shared<int>(0);
    Teams.Members(Uuid, [Members, Ok](bool S, const QString &, const QJsonArray & L) { *Members = L; *Ok = S ? 1 : 2; });
    WaitFor([Ok]() { return *Ok != 0; });
    QString Bob;
    for (const QJsonValue & V : *Members)
    {
      if (V.toObject().value(QStringLiteral("role")).toString() == QStringLiteral("member")) Bob = V.toObject().value(QStringLiteral("user_uuid")).toString();
    }
    CHECK(!Bob.isEmpty());
    CHECK(Wait([&](const auto & D) { Teams.RemoveMember(Uuid, Bob, D); }));
    const TeamSync::Team * T = Teams.Find(Uuid);
    CHECK((T != nullptr) && (T->KeyVersion == OldVersion + 1) && !T->NeedsRotation && (T->MemberCount == 1));
    CHECK(Site(TeamFolder + QStringLiteral("/Shared/db1")) != nullptr);
  }
  else if (Step == QStringLiteral("bob-removed"))
  {
    CHECK(Wait([&](const auto & D) { Teams.SyncAll(D); }));
    CHECK(TeamUuid().isEmpty());
    CHECK(Site(TeamFolder + QStringLiteral("/Shared/web1")) == nullptr);
    CHECK(Site(TeamFolder + QStringLiteral("/Shared/db1")) == nullptr);
    CHECK(!Notices.isEmpty() && Notices.last().contains(QStringLiteral("no longer a member")));
  }
  else if (Step == QStringLiteral("alice-delete"))
  {
    CHECK(Wait([&](const auto & D) { Teams.SyncAll(D); }));
    QString Uuid = TeamUuid();
    CHECK(Wait([&](const auto & D) { Teams.DeleteTeam(Uuid, D); }));
    CHECK(TeamUuid().isEmpty());
    CHECK(Site(TeamFolder + QStringLiteral("/Shared/db1")) == nullptr);
    CHECK(Site(QStringLiteral("Private/mine")) != nullptr);
  }
  std::printf("step %s: %s\n", qPrintable(Step), (Failures == 0) ? "ok" : "FAILED");
  return (Failures == 0) ? 0 : 1;
}

int main(int argc, char ** argv)
{
  QCoreApplication App(argc, argv);
  QStringList Args = App.arguments();
  if (Args.size() == 4)
  {
    return RunStep(Args[1], Args[2], Args[3]);
  }
  if (qEnvironmentVariableIsEmpty("NIXSCP_TEST_CLOUD_COOKIE") || qEnvironmentVariableIsEmpty("NIXSCP_TEST_CLOUD_COOKIE2"))
  {
    std::printf("SKIP: set NIXSCP_TEST_CLOUD_COOKIE and NIXSCP_TEST_CLOUD_COOKIE2 (two fresh test accounts)\n");
    return 0;
  }
  QTemporaryDir Alice, Bob, Shared;
  const QList<QPair<QString, QString>> Steps{
    {Alice.path(), QStringLiteral("alice-setup")}, {Bob.path(), QStringLiteral("bob-setup")},
    {Alice.path(), QStringLiteral("alice-add")}, {Bob.path(), QStringLiteral("bob-pull")},
    {Alice.path(), QStringLiteral("alice-unshare")}, {Bob.path(), QStringLiteral("bob-unshared")},
    {Alice.path(), QStringLiteral("alice-remove")}, {Bob.path(), QStringLiteral("bob-removed")},
    {Alice.path(), QStringLiteral("alice-delete")}};
  for (const auto & Step : Steps)
  {
    QProcess Process;
    Process.setProcessChannelMode(QProcess::ForwardedChannels);
    Process.start(App.applicationFilePath(), {Step.first, Step.second, Shared.path()});
    Process.waitForFinished(300000);
    CHECK((Process.exitStatus() == QProcess::NormalExit) && (Process.exitCode() == 0));
    if (Failures > 0) break;
  }
  if (Failures == 0)
  {
    std::printf("All live teams tests passed\n");
  }
  return (Failures == 0) ? 0 : 1;
}
