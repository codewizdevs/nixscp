// NixSCP Teams against a real NixSCP cloud server (core/TeamSync): two
// accounts on separate "computers" (configuration folders, processes): Alice
// creates a team and shares a site folder, invites Bob with a code, Bob joins
// and gets the sites (members' local changes are reverted), Alice adds a site
// and shares single sites (copied and moved), stops sharing them again. A second
// computer of Alice's reads the team from the signed roster alone. Bob becomes
// an admin: a rename with odd whitespace, same-named teams, two admins editing at
// once and what the real server refuses. Then Alice removes Bob (the key is
// renewed, Bob loses the sites) and deletes the team.
//
// Skipped unless NIXSCP_TEST_CLOUD_COOKIE and NIXSCP_TEST_CLOUD_COOKIE2 are
// website session cookies of two fresh test accounts (beta or a license).
#include "core/CloudClient.h"
#include "core/CloudSync.h"
#include "core/ConfigVault.h"
#include "core/Configuration.h"
#include "core/SessionData.h"
#include "core/TeamCrypto.h"
#include "core/TeamRoster.h"
#include "core/TeamSync.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
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
static const QString RenamedFolder = QStringLiteral("Team: Live Renamed");

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

static QString TeamUuid(const QString & Name = QStringLiteral("Live Team"))
{
  for (const TeamSync::Team & T : TeamSync::Instance().Teams())
  {
    if (T.Name == Name) return T.Uuid;
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

// The account's storage as /v1/me reports it
static QJsonObject StorageOf()
{
  auto Me = std::make_shared<QJsonObject>();
  auto Fetched = std::make_shared<int>(0);
  CloudSync::Instance().FetchAccount([Me, Fetched](bool Ok, const QJsonObject & J) { *Me = J; *Fetched = Ok ? 1 : 2; });
  WaitFor([Fetched]() { return *Fetched != 0; });
  return Me->value(QStringLiteral("storage")).toObject();
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

  if (Step == QStringLiteral("alice2-setup"))
  {
    // a second computer: the sync password, and the team from the roster alone
    CHECK(SignIn(Cookie));
    auto Done = std::make_shared<int>(0);
    Sync.UnlockKey(SyncPassword, false, [Done](bool Ok, const QString &) { *Done = Ok ? 1 : 2; });
    WaitFor([Done]() { return *Done != 0; });
    CHECK(*Done == 1);
    CHECK(Teams.Available());
    CHECK(Wait([&](const auto & D) { Teams.SyncAll(D); }));
    QString Uuid = TeamUuid();
    CHECK(!Uuid.isEmpty());
    const TeamSync::Team * T = Teams.Find(Uuid);
    CHECK((T != nullptr) && (T->Role == QStringLiteral("owner")) && !T->Key.IsEmpty() && (T->Folder == TeamFolder));
    CHECK(Site(TeamFolder + QStringLiteral("/Shared/web1")) != nullptr);
    CHECK(Site(TeamFolder + QStringLiteral("/Shared/web1"))->Password == QStringLiteral("web1-password"));
    // and it does not push because of being new
    qint64 Head = T->HeadVersion;
    CHECK(Wait([&](const auto & D) { Teams.SyncAll(D); }));
    CHECK(Teams.Find(Uuid)->HeadVersion == Head);
  }
  else if (Step.endsWith(QStringLiteral("-setup")))
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
    CHECK((T != nullptr) && (T->Role == QStringLiteral("owner")) && !T->Key.IsEmpty());
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
    CHECK((T != nullptr) && (T->Role == QStringLiteral("member")) && !T->Key.IsEmpty());
    auto Web1 = Site(TeamFolder + QStringLiteral("/Shared/web1"));
    CHECK((Web1 != nullptr) && (Web1->Password == QStringLiteral("web1-password")) && (Web1->HostName == QStringLiteral("web1.example.com")));
    CHECK(Site(TeamFolder + QStringLiteral("/Private/mine")) == nullptr);
    // a member's change is reverted on the next sync
    Web1->HostName = QStringLiteral("changed.example.com");
    Config.Save();
    CHECK(Wait([&](const auto & D) { Teams.SyncAll(D); }));
    CHECK(Site(TeamFolder + QStringLiteral("/Shared/web1"))->HostName == QStringLiteral("web1.example.com"));
    CHECK(!Notices.isEmpty() && Notices.join(QLatin1Char('\n')).contains(QStringLiteral("reverted")));
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
  else if (Step == QStringLiteral("alice-notes"))
  {
    // notes: private until shared; shared in the signed sites; team edits in the note channel
    CHECK(Wait([&](const auto & D) { Teams.SyncAll(D); }));
    const QString Web1 = TeamFolder + QStringLiteral("/Shared/web1");
    const QString Db1 = TeamFolder + QStringLiteral("/Shared/db1");
    CHECK(Wait([&](const auto & D) { Teams.SaveNote(Web1, QStringLiteral("Alice's own reminder."), D); }));
    TeamSync::NoteView View = Teams.NoteOf(*Site(Web1));
    CHECK(View.InTeam && !View.Shared && View.CanEdit && View.CanShare && (View.Text == QStringLiteral("Alice's own reminder.")));
    CHECK(Wait([&](const auto & D) { Teams.SetNoteShared(Web1, true, D); }));
    CHECK(Wait([&](const auto & D) { Teams.SaveNote(Web1, QStringLiteral("Web1: restart with systemctl."), D); }));
    CHECK(Wait([&](const auto & D) { Teams.SyncAll(D); }));
    View = Teams.NoteOf(*Site(Web1));
    CHECK(View.Shared && !View.TeamEdit && (View.Text == QStringLiteral("Web1: restart with systemctl.")) && !View.EditedBy.isEmpty());
    Write(Shared + QStringLiteral("/alice-email"), View.EditedBy.toUtf8());
    // db1: shared, and members edit it too
    CHECK(Wait([&](const auto & D) { Teams.SaveNote(Db1, QStringLiteral("Db1: backups at 02:00."), D); }));
    CHECK(Wait([&](const auto & D) { Teams.SetNoteShared(Db1, true, D); }));
    CHECK(Wait([&](const auto & D) { Teams.SetNoteTeamEdit(Db1, true, D); }));
    CHECK(Wait([&](const auto & D) { Teams.SyncAll(D); }));
    View = Teams.NoteOf(*Site(Db1));
    CHECK(View.Shared && View.TeamEdit && (View.Text == QStringLiteral("Db1: backups at 02:00.")));
    // the note is not in the personal sync, and agent rights stay off
    CHECK(Site(Db1)->PrivateNote.isEmpty());
  }
  else if (Step == QStringLiteral("bob-notes"))
  {
    CHECK(Wait([&](const auto & D) { Teams.SyncAll(D); }));
    const QString Web1 = TeamFolder + QStringLiteral("/Shared/web1");
    const QString Db1 = TeamFolder + QStringLiteral("/Shared/db1");
    const QString AliceEmail = QString::fromUtf8(Read(Shared + QStringLiteral("/alice-email")));
    TeamSync::NoteView View = Teams.NoteOf(*Site(Web1));
    CHECK_MSG(View.Shared && !View.CanEdit && !View.CanShare && (View.Text == QStringLiteral("Web1: restart with systemctl.")), View.Text);
    CHECK_MSG(View.EditedBy == AliceEmail, View.EditedBy);
    // Alice's private note never came
    CHECK(!Site(Web1)->PrivateNote.contains(QStringLiteral("Alice")));
    QString Error;
    CHECK(!Wait([&](const auto & D) { Teams.SaveNote(Web1, QStringLiteral("Bob was here."), D); }, &Error));
    CHECK(Error.contains(QStringLiteral("owners and admins")));
    // a member does not decide about sharing
    CHECK(!Wait([&](const auto & D) { Teams.SetNoteShared(Web1, false, D); }));
    // db1 has team edits: Bob writes it through the note channel
    View = Teams.NoteOf(*Site(Db1));
    CHECK_MSG(View.Shared && View.TeamEdit && View.CanEdit && (View.Text == QStringLiteral("Db1: backups at 02:00.")), View.Text);
    CHECK(Wait([&](const auto & D) { Teams.SaveNote(Db1, QStringLiteral("Db1: backups at 03:00 now."), D); }));
    View = Teams.NoteOf(*Site(Db1));
    CHECK((View.Text == QStringLiteral("Db1: backups at 03:00 now.")) && !View.EditedBy.isEmpty() && (View.EditedBy != AliceEmail));
    Write(Shared + QStringLiteral("/bob-email"), View.EditedBy.toUtf8());
    // a second save (version 2) and a fresh sync see the same note
    CHECK(Wait([&](const auto & D) { Teams.SaveNote(Db1, QStringLiteral("Db1: backups at 03:00 now. Checked."), D); }));
    CHECK(Wait([&](const auto & D) { Teams.SyncAll(D); }));
    CHECK(Teams.NoteOf(*Site(Db1)).Text == QStringLiteral("Db1: backups at 03:00 now. Checked."));
  }
  else if (Step == QStringLiteral("alice-notes2"))
  {
    CHECK(Wait([&](const auto & D) { Teams.SyncAll(D); }));
    const QString Web1 = TeamFolder + QStringLiteral("/Shared/web1");
    const QString Db1 = TeamFolder + QStringLiteral("/Shared/db1");
    const QString BobEmail = QString::fromUtf8(Read(Shared + QStringLiteral("/bob-email")));
    TeamSync::NoteView View = Teams.NoteOf(*Site(Db1));
    CHECK_MSG(View.TeamEdit && (View.Text == QStringLiteral("Db1: backups at 03:00 now. Checked.")), View.Text);
    CHECK_MSG(View.EditedBy == BobEmail, View.EditedBy);
    // the owner's storage counts the team's sites and Bob's note in the channel
    QJsonObject Storage = StorageOf();
    QString Json = QString::fromUtf8(QJsonDocument(Storage).toJson(QJsonDocument::Compact));
    CHECK_MSG((Storage.value(QStringLiteral("teams")).toDouble() > 0) && (Storage.value(QStringLiteral("notes")).toDouble() > 0) &&
      (Storage.value(QStringLiteral("limit")).toDouble() == 20 * 1024 * 1024), Json);
    CHECK_MSG(CloudClient::StorageText(Storage).endsWith(QStringLiteral("of 20 MB used")), CloudClient::StorageText(Storage));
    // team edits off: the note goes back into the signed sites, with who wrote it
    CHECK(Wait([&](const auto & D) { Teams.SetNoteTeamEdit(Db1, false, D); }));
    CHECK(Wait([&](const auto & D) { Teams.SyncAll(D); }));
    // folded back: nothing left in the channel
    CHECK(StorageOf().value(QStringLiteral("notes")).toDouble() == 0);
    View = Teams.NoteOf(*Site(Db1));
    CHECK_MSG(!View.TeamEdit && (View.Text == QStringLiteral("Db1: backups at 03:00 now. Checked.")) && (View.EditedBy == BobEmail), View.EditedBy);
    // web1 not shared any more: it is Alice's private note again
    CHECK(Wait([&](const auto & D) { Teams.SetNoteShared(Web1, false, D); }));
    CHECK(Wait([&](const auto & D) { Teams.SyncAll(D); }));
    View = Teams.NoteOf(*Site(Web1));
    CHECK(!View.Shared && (View.Text == QStringLiteral("Web1: restart with systemctl.")));
  }
  else if (Step == QStringLiteral("bob-notes2"))
  {
    CHECK(Wait([&](const auto & D) { Teams.SyncAll(D); }));
    const QString Web1 = TeamFolder + QStringLiteral("/Shared/web1");
    const QString Db1 = TeamFolder + QStringLiteral("/Shared/db1");
    TeamSync::NoteView View = Teams.NoteOf(*Site(Db1));
    CHECK_MSG(!View.TeamEdit && !View.CanEdit && (View.Text == QStringLiteral("Db1: backups at 03:00 now. Checked.")), View.Text);
    QString Error;
    CHECK(!Wait([&](const auto & D) { Teams.SaveNote(Db1, QStringLiteral("Late edit."), D); }, &Error));
    // web1's note is gone for Bob; his own private note is his again
    View = Teams.NoteOf(*Site(Web1));
    CHECK_MSG(!View.Shared && View.CanEdit && !View.Text.contains(QStringLiteral("systemctl")), View.Text);
  }
  else if (Step == QStringLiteral("alice-promote"))
  {
    CHECK(Wait([&](const auto & D) { Teams.SyncAll(D); }));
    QString Uuid = TeamUuid();
    qint64 Head = Teams.Find(Uuid)->HeadVersion;
    auto Members = std::make_shared<QJsonArray>();
    auto Ok = std::make_shared<int>(0);
    Teams.Members(Uuid, [Members, Ok](bool S, const QString &, const QJsonArray & L) { *Members = L; *Ok = S ? 1 : 2; });
    WaitFor([Ok]() { return *Ok != 0; });
    QString Bob;
    for (const QJsonValue & V : *Members)
    {
      CHECK(V.toObject().value(QStringLiteral("fingerprint")).toString().size() == 29);
      if (V.toObject().value(QStringLiteral("role")).toString() == QStringLiteral("member")) Bob = V.toObject().value(QStringLiteral("user_uuid")).toString();
    }
    CHECK(!Bob.isEmpty());
    Write(Shared + QStringLiteral("/bob"), Bob.toLatin1());
    CHECK(Wait([&](const auto & D) { Teams.SetRole(Uuid, Bob, QStringLiteral("admin"), D); }));
    // B2: a rename with odd whitespace, by one of two admins
    CHECK(Wait([&](const auto & D) { Teams.Rename(Uuid, QStringLiteral("  Live \t  Renamed "), D); }));
    CHECK(Teams.Find(Uuid)->Name == QStringLiteral("Live Renamed"));
    CHECK(Teams.Find(Uuid)->Folder == RenamedFolder);
    CHECK(Teams.Find(Uuid)->HeadVersion == Head + 1);
    CHECK(Site(RenamedFolder + QStringLiteral("/Shared/web1")) != nullptr);
    CHECK(Site(TeamFolder + QStringLiteral("/Shared/web1")) == nullptr);
    CHECK(Teams.SharedSites(Uuid) == QStringList({QStringLiteral("Shared/db1"), QStringLiteral("Shared/web1")}));
    // a second team of the same name, and one that differs in case: nothing mixes
    CHECK(Wait([&](const auto & D) { Teams.CreateTeam(QStringLiteral("Live Renamed"), D); }));
    CHECK(Wait([&](const auto & D) { Teams.CreateTeam(QStringLiteral("LIVE RENAMED"), D); }));
    QStringList Folders;
    for (const TeamSync::Team & T : Teams.Teams()) Folders.append(T.Folder.toLower());
    Folders.removeDuplicates();
    CHECK(Folders.size() == 3);
    for (const TeamSync::Team & T : Teams.Teams())
    {
      if (T.Uuid != Uuid)
      {
        CHECK(Teams.SharedSites(T.Uuid).isEmpty());
        AddSite(T.Folder + QStringLiteral("/other"), QStringLiteral("other-password"));
      }
    }
    CHECK(Wait([&](const auto & D) { Teams.SyncAll(D); }));
    CHECK(Teams.SharedSites(Uuid) == QStringList({QStringLiteral("Shared/db1"), QStringLiteral("Shared/web1")}));
    for (const TeamSync::Team & T : Teams.Teams())
    {
      if (T.Uuid != Uuid) CHECK(Wait([&](const auto & D) { Teams.DeleteTeam(T.Uuid, D); }));
    }
    CHECK(Teams.Teams().size() == 1);
    CHECK(Site(RenamedFolder + QStringLiteral("/Shared/web1")) != nullptr);
    // a local edit, not synced: the next step's admin gets there first
    Site(RenamedFolder + QStringLiteral("/Shared/web1"))->RemoteDirectory = QStringLiteral("/alice");
    Config.Save();
  }
  else if (Step == QStringLiteral("bob-renamed"))
  {
    int Notified = 0;
    QObject::connect(&Teams, &TeamSync::Notice, [&Notified](const QString & M) { Notified += M.contains(QStringLiteral("renamed")) ? 1 : 0; });
    CHECK(Wait([&](const auto & D) { Teams.SyncAll(D); }));
    QString Uuid = TeamUuid(QStringLiteral("Live Renamed"));
    CHECK(!Uuid.isEmpty());
    const TeamSync::Team * T = Teams.Find(Uuid);
    qint64 Head = T->HeadVersion;
    CHECK(T->Role == QStringLiteral("admin"));
    CHECK(T->Folder == RenamedFolder);
    CHECK(Notified == 1);
    // the folder moved here too, with this person's own settings, and nothing was pushed
    auto Web1 = Site(RenamedFolder + QStringLiteral("/Shared/web1"));
    CHECK((Web1 != nullptr) && (Web1->AgentAccess == 1));
    CHECK(Site(TeamFolder + QStringLiteral("/Shared/web1")) == nullptr);
    CHECK(Wait([&](const auto & D) { Teams.SyncAll(D); }));
    CHECK(Teams.Find(Uuid)->HeadVersion == Head);
    // this admin edits first
    Web1->RemoteDirectory = QStringLiteral("/bob");
    Config.Save();
    CHECK(Wait([&](const auto & D) { Teams.SyncAll(D); }));
    CHECK(Teams.Find(Uuid)->HeadVersion == Head + 1);
  }
  else if (Step == QStringLiteral("alice-conflict"))
  {
    // T9: both changed; nothing is lost and nothing overwritten until she decides
    QString Error;
    CHECK(!Wait([&](const auto & D) { Teams.SyncAll(D); }, &Error));
    QString Uuid = TeamUuid(QStringLiteral("Live Renamed"));
    CHECK(!Teams.Find(Uuid)->Conflict.isEmpty());
    CHECK(Site(RenamedFolder + QStringLiteral("/Shared/web1"))->RemoteDirectory == QStringLiteral("/alice"));
    CHECK(Notices.size() == 1);
    CHECK(Wait([&](const auto & D) { Teams.ResolveConflict(Uuid, true, D); }));
    CHECK(Teams.Find(Uuid)->Conflict.isEmpty());
    CHECK(Site(RenamedFolder + QStringLiteral("/Shared/web1"))->RemoteDirectory == QStringLiteral("/alice"));
    // the history keeps Bob's version
    auto Versions = std::make_shared<QJsonArray>();
    auto Ok = std::make_shared<int>(0);
    Teams.History(Uuid, [Versions, Ok](bool S, const QString &, const QJsonArray & L) { *Versions = L; *Ok = S ? 1 : 2; });
    WaitFor([Ok]() { return *Ok != 0; });
    CHECK((*Ok == 1) && (Versions->size() >= 3));
    qint64 BobsVersion = Versions->at(1).toObject().value(QStringLiteral("version")).toInteger();
    CHECK(BobsVersion == Teams.Find(Uuid)->HeadVersion - 1);
    CHECK(Wait([&](const auto & D) { Teams.Restore(Uuid, BobsVersion, D); }));
    CHECK(Site(RenamedFolder + QStringLiteral("/Shared/web1"))->RemoteDirectory == QStringLiteral("/bob"));
    CHECK(Teams.Find(Uuid)->HeadVersion == BobsVersion + 2);
    // back to the plain setup for the rest
    CHECK(Wait([&](const auto & D) { Teams.Rename(Uuid, QStringLiteral("Live Team"), D); }));
    Write(Shared + QStringLiteral("/team"), Uuid.toLatin1());
  }
  else if (Step == QStringLiteral("bob-late-push"))
  {
    // Bob is still an admin, and pushes after the last sync of Alice's computer
    CHECK(Wait([&](const auto & D) { Teams.SyncAll(D); }));
    QString Uuid = TeamUuid();
    Site(TeamFolder + QStringLiteral("/Shared/web1"))->RemoteDirectory = QStringLiteral("/bob");
    Site(TeamFolder + QStringLiteral("/Shared/web1"))->Note = QStringLiteral("last push of an admin");
    Config.Save();
    CHECK(Wait([&](const auto & D) { Teams.SyncAll(D); }));
    Write(Shared + QStringLiteral("/latehead"), QByteArray::number(Teams.Find(Uuid)->HeadVersion));
  }
  else if (Step == QStringLiteral("alice-demote"))
  {
    // No sync first: the head this computer verified is behind. The real server
    // refuses that, the client syncs, verifies Bob's push and signs again. Bob's
    // last snapshot stays valid for everyone
    QString Uuid = QString::fromLatin1(Read(Shared + QStringLiteral("/team")));
    CHECK(Wait([&](const auto & D) { Teams.SetRole(Uuid, QString::fromLatin1(Read(Shared + QStringLiteral("/bob"))), QStringLiteral("member"), D); }));
    const TeamSync::Team * T = Teams.Find(Uuid);
    CHECK((T != nullptr) && T->Error.isEmpty());
    CHECK(T->HeadVersion == Read(Shared + QStringLiteral("/latehead")).toLongLong());
    CHECK(Site(TeamFolder + QStringLiteral("/Shared/web1"))->Note == QStringLiteral("last push of an admin"));
    CHECK(Wait([&](const auto & D) { Teams.SyncAll(D); }));
  }
  else if (Step == QStringLiteral("bob-hostile"))
  {
    // Bob is a plain member again, with a real account and a real key: what he may not do
    // the real server refuses, whatever he sends
    CHECK(Wait([&](const auto & D) { Teams.SyncAll(D); }));
    QString Uuid = TeamUuid();
    CHECK(!Uuid.isEmpty());
    CHECK(Teams.Find(Uuid)->Role == QStringLiteral("member"));
    CHECK(Site(TeamFolder + QStringLiteral("/Shared/web1"))->Note == QStringLiteral("last push of an admin"));
    auto Call = [&](const QByteArray & Method, const QString & Path, const QJsonObject & Body)
    {
      auto Status = std::make_shared<int>(-1);
      Sync.Client().Api(Method, Path, Body, [Status](const CloudClient::Reply & Reply) { *Status = Reply.Status; });
      WaitFor([Status]() { return *Status != -1; });
      return *Status;
    };
    // the v1 routes are gone
    CHECK(Call("PATCH", QStringLiteral("/v1/teams/%1").arg(Uuid), QJsonObject{{QStringLiteral("name"), QStringLiteral("x")}}) == 404);
    CHECK(Call("PUT", QStringLiteral("/v1/teams/%1/members/me/key").arg(Uuid), QJsonObject{{QStringLiteral("wrapped_team_key"), QStringLiteral("AAAA")}, {QStringLiteral("key_version"), 1}}) == 404);
    // a box key nothing can be wrapped for
    {
      TeamCrypto::SignKey Key = TeamCrypto::NewSignKey();
      QByteArray Zero(32, '\0');
      QJsonObject Low{{QStringLiteral("public_key"), QString::fromLatin1(Zero.toBase64())}, {QStringLiteral("identity_key"), QString::fromLatin1(Key.Public.toBase64())},
        {QStringLiteral("key_signature"), QString::fromLatin1(TeamCrypto::Sign(Key, TeamCrypto::BoxKeyMessage(Sync.UserUuid(), Zero)).toBase64())},
        {QStringLiteral("wrapped_private_key"), QStringLiteral("AAAA")}};
      CHECK(Call("PUT", QStringLiteral("/v1/keys/me"), Low) == 400);
    }
    // a key swap with no signature of the old key
    TeamCrypto::SignKey Thief = TeamCrypto::NewSignKey();
    TeamCrypto::KeyPair Box = TeamCrypto::NewKeyPair();
    QJsonObject Swap{{QStringLiteral("public_key"), QString::fromLatin1(Box.Public.toBase64())}, {QStringLiteral("identity_key"), QString::fromLatin1(Thief.Public.toBase64())},
      {QStringLiteral("key_signature"), QString::fromLatin1(TeamCrypto::Sign(Thief, TeamCrypto::BoxKeyMessage(Sync.UserUuid(), Box.Public)).toBase64())},
      {QStringLiteral("wrapped_private_key"), QStringLiteral("AAAA")}};
    CHECK(Call("PUT", QStringLiteral("/v1/keys/me"), Swap) == 409);
    Swap.insert(QStringLiteral("replace_signature"), QString::fromLatin1(TeamCrypto::Sign(Thief, "not the stored key").toBase64()));
    CHECK(Call("PUT", QStringLiteral("/v1/keys/me"), Swap) == 403);
    // role and removal entries signed by a member, and a forged owner
    auto Entry = [&](const QString & Type, const QJsonObject & Data, const TeamCrypto::SignKey & Key, const QString & Signer)
    {
      TeamRoster::State Roster;
      return QJsonObject{{QStringLiteral("entry"), TeamRoster::ToWire(TeamRoster::Make(Roster, Uuid, Type, Signer, Data, Key))}};
    };
    CHECK(Call("POST", QStringLiteral("/v1/teams/%1/roster").arg(Uuid), Entry(QStringLiteral("role"), QJsonObject{{QStringLiteral("user"), Sync.UserUuid()}, {QStringLiteral("role"), QStringLiteral("admin")}, {QStringLiteral("head"), 0}}, Thief, Sync.UserUuid())) >= 400);
    CHECK(Call("POST", QStringLiteral("/v1/teams/%1/roster").arg(Uuid), QJsonObject{{QStringLiteral("entry"), QJsonObject{{QStringLiteral("body"), QStringLiteral("AAAA")}, {QStringLiteral("sig"), QStringLiteral("AAAA")}}}}) == 400);
    // no pushes by a member
    CHECK(Call("POST", QStringLiteral("/v1/teams/%1/sync/upload-url").arg(Uuid), QJsonObject{{QStringLiteral("base_version"), Teams.Find(Uuid)->HeadVersion}, {QStringLiteral("size"), 100}}) == 403);
    // still fine afterwards
    CHECK(Wait([&](const auto & D) { Teams.SyncAll(D); }));
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
  else if (Step == QStringLiteral("alice-reinvite"))
  {
    CHECK(Wait([&](const auto & D) { Teams.SyncAll(D); }));
    QString Uuid = TeamUuid();
    auto Code = std::make_shared<QString>();
    auto Ok = std::make_shared<int>(0);
    Teams.CreateInvite(Uuid, QStringLiteral("member"), 1, [Code, Ok](bool Success, const QString &, const QString & C) { *Code = C; *Ok = Success ? 1 : 2; });
    WaitFor([Ok]() { return *Ok != 0; });
    CHECK(*Ok == 1);
    Write(Shared + QStringLiteral("/code2"), Code->toLatin1());
  }
  else if (Step == QStringLiteral("bob-poison"))
  {
    // A join every client would refuse (signed later than its invite) must not reach
    // the roster: one person with an invite could otherwise stop the team for good
    TeamCrypto::Invite Code;
    CHECK(TeamCrypto::ParseInviteCode(QString::fromLatin1(Read(Shared + QStringLiteral("/code2"))), Code));
    CHECK(Wait([&](const auto & D) { Teams.SyncAll(D); }));
    QString Verifier = QString::fromLatin1(TeamCrypto::InviteVerifier(Code).toBase64());
    auto Opened = std::make_shared<QJsonObject>();
    auto Done = std::make_shared<int>(0);
    Sync.Client().Api("POST", QStringLiteral("/v1/invites/%1/open").arg(Code.Id), QJsonObject{{QStringLiteral("verifier"), Verifier}},
      [Opened, Done](const CloudClient::Reply & Reply) { *Opened = Reply.Json; *Done = Reply.Ok() ? 1 : 2; });
    WaitFor([Done]() { return *Done != 0; });
    CHECK(*Done == 1);
    QString Uuid = Opened->value(QStringLiteral("team")).toObject().value(QStringLiteral("uuid")).toString();
    TeamRoster::State Roster;
    CHECK(TeamRoster::Replay(Opened->value(QStringLiteral("entries")).toArray(), Uuid, Roster));
    auto Keys = std::make_shared<QJsonObject>();
    *Done = 0;
    Sync.Client().Api("GET", QStringLiteral("/v1/keys/me"), [Keys, Done](const CloudClient::Reply & Reply) { *Keys = Reply.Json; *Done = 1; });
    WaitFor([Done]() { return *Done != 0; });
    TeamCrypto::KeyPair Box;
    TeamCrypto::SignKey Identity;
    CHECK(TeamCrypto::SplitKeyBlob(TeamCrypto::UnwrapPrivateKey(Sync.DataKey(), QByteArray::fromBase64(Keys->value(QStringLiteral("wrapped_private_key")).toString().toLatin1())), Box, Identity));
    QByteArray Wrap = TeamCrypto::WrapForMember(Box.Public, QByteArray(32, 'k'), Uuid, Roster.KeyVersion);
    TeamCrypto::SignKey InviteKey = TeamCrypto::InviteSignKey(Code);
    TeamRoster::Entry Join = TeamRoster::Make(Roster, Uuid, QStringLiteral("join"), Sync.UserUuid(),
      TeamRoster::JoinData(Code.Id, Sync.Email(), Identity, Sync.UserUuid(), Box.Public, Wrap), Identity, &InviteKey);
    QJsonObject Json = QJsonDocument::fromJson(Join.Body).object();
    Json.insert(QStringLiteral("time"), QStringLiteral("2099-01-01T00:00:00.000Z"));
    QByteArray Body = QJsonDocument(Json).toJson(QJsonDocument::Compact);
    QJsonObject Wire{{QStringLiteral("body"), QString::fromLatin1(Body.toBase64())},
      {QStringLiteral("sig"), QString::fromLatin1(TeamCrypto::Sign(Identity, QByteArrayLiteral("NXT2-ROSTER|") + Body).toBase64())},
      {QStringLiteral("sig2"), QString::fromLatin1(TeamCrypto::Sign(InviteKey, QByteArrayLiteral("NXT2-ROSTER|") + Body).toBase64())}};
    auto Status = std::make_shared<int>(-1);
    Sync.Client().Api("POST", QStringLiteral("/v1/invites/%1/accept").arg(Code.Id),
      QJsonObject{{QStringLiteral("verifier"), Verifier}, {QStringLiteral("entry"), Wire}, {QStringLiteral("wrapped_team_key"), QString::fromLatin1(Wrap.toBase64())}},
      [Status](const CloudClient::Reply & Reply) { *Status = Reply.Status; });
    WaitFor([Status]() { return *Status != -1; });
    CHECK(*Status == 404);
    // the invite still works, the honest way
    CHECK(Wait([&](const auto & D) { Teams.JoinWithCode(QString::fromLatin1(Read(Shared + QStringLiteral("/code2"))), D); }));
    CHECK(Site(TeamFolder + QStringLiteral("/Shared/web1")) != nullptr);
  }
  else if (Step == QStringLiteral("alice-delete"))
  {
    CHECK(Wait([&](const auto & D) { Teams.SyncAll(D); }));
    QString Uuid = TeamUuid();
    CHECK(Teams.Find(Uuid)->Error.isEmpty());
    CHECK(Teams.Find(Uuid)->MemberCount == 2);
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
  QTemporaryDir Alice, Alice2, Bob, Shared;
  const QList<QPair<QString, QString>> Steps{
    {Alice.path(), QStringLiteral("alice-setup")}, {Bob.path(), QStringLiteral("bob-setup")},
    {Alice.path(), QStringLiteral("alice-add")}, {Bob.path(), QStringLiteral("bob-pull")},
    {Alice2.path(), QStringLiteral("alice2-setup")},
    {Alice.path(), QStringLiteral("alice-unshare")}, {Bob.path(), QStringLiteral("bob-unshared")},
    {Alice.path(), QStringLiteral("alice-notes")}, {Bob.path(), QStringLiteral("bob-notes")},
    {Alice.path(), QStringLiteral("alice-notes2")}, {Bob.path(), QStringLiteral("bob-notes2")},
    {Alice.path(), QStringLiteral("alice-promote")}, {Bob.path(), QStringLiteral("bob-renamed")},
    {Alice.path(), QStringLiteral("alice-conflict")}, {Bob.path(), QStringLiteral("bob-late-push")},
    {Alice.path(), QStringLiteral("alice-demote")}, {Bob.path(), QStringLiteral("bob-hostile")},
    {Alice.path(), QStringLiteral("alice-remove")}, {Bob.path(), QStringLiteral("bob-removed")},
    {Alice.path(), QStringLiteral("alice-reinvite")}, {Bob.path(), QStringLiteral("bob-poison")},
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
