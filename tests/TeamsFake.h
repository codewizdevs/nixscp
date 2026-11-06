// A fake of the NixSCP cloud API for team tests: the real CloudSync and TeamSync
// code talks to it on 127.0.0.1, and it can lie in every answer. Its other people
// ("peers") are built with the same crypto. Include after defining CHECK and
// CHECK_MSG. One client per process: the client classes are singletons.
#pragma once

#include "core/CloudCrypto.h"
#include "core/CloudSettings.h"
#include "core/CloudSync.h"
#include "core/ConfigVault.h"
#include "core/Configuration.h"
#include "core/IniStorage.h"
#include "core/SecureShell.h"
#include "core/SessionData.h"
#include "core/TeamContent.h"
#include "core/TeamCrypto.h"
#include "core/TeamRoster.h"
#include "core/TeamSync.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>

#include <cstdio>
#include <functional>
#include <memory>
#include <unistd.h>


static QString B64(const QByteArray & D) { return QString::fromLatin1(D.toBase64()); }
static QByteArray UnB64(const QJsonValue & V) { return QByteArray::fromBase64(V.toString().toLatin1()); }
static QString Sha(const QByteArray & D) { return QString::fromLatin1(QCryptographicHash::hash(D, QCryptographicHash::Sha256).toHex()); }
static QString NewUuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces).toLower(); }

static const QString SyncPassword = QStringLiteral("correct horse battery staple");
static const QString VictimUuid = QStringLiteral("aaaaaaaa-0000-4000-8000-000000000001");
static const QString VictimEmail = QStringLiteral("victim@nixscp.test");

// Someone else in a team, with their keys
struct Peer
{
  QString Uuid;
  QString Email;
  TeamCrypto::KeyPair Box;
  TeamCrypto::SignKey Identity;
};

static Peer NewPeer(const QString & Name)
{
  return Peer{NewUuid(), Name + QStringLiteral("@nixscp.test"), TeamCrypto::NewKeyPair(), TeamCrypto::NewSignKey()};
}

struct FakeTeam
{
  QString Uuid;
  QJsonArray Entries;
  TeamRoster::State Roster;
  qint64 Head = 0;
  QHash<qint64, QByteArray> Snapshots;
  QHash<qint64, qint64> KeyVersions;
  QHash<QString, QByteArray> Wraps;
  QByteArray Key;                // the team key, when a peer made it
  // what a lying server says instead of the truth
  QByteArray ServeWrap;
  qint64 ServeKeyVersion = 0;
  QJsonArray ServeEntries;
  bool HasServeEntries = false;
  qint64 ServeHead = -1;
  bool Hidden = false;
  bool SayNeedsRotation = false;
  QJsonArray ServeMembers;
};

struct Fake : QObject
{
  QTcpServer Server;
  QString Base;
  QString User = VictimUuid;
  QString Email = VictimEmail;
  QHash<QString, QJsonObject> Keys;      // user uuid -> the stored keys
  QHash<QString, QByteArray> Objects;
  QHash<QString, FakeTeam> Teams;
  struct Invite { QString Team; QString Role; QByteArray Wrap; qint64 KeyVersion; QString VerifierHash; bool Used = false; };
  QHash<QString, Invite> Invites;
  QHash<QString, qint64> SyncHead;
  QHash<QString, QHash<qint64, QString>> SyncObjects;
  QStringList Log;
  int Puts = 0;
  int Commits = 0;
  QByteArray LastPut;
  QByteArray Headers;
  QByteArray LastHeaders;
  QByteArray TeamsRaw;                   // replaces the answer of GET /v1/teams
  bool CheckHead = true;                 // refuses a stale head, as the real server does
  QHash<QString, int> FailStatus;        // request text part -> status
  QHash<QString, int> FailCount;         // how many times; absent: always
  std::function<void(const QString &)> OnRequest;
  int Counter = 0;

  void Start()
  {
    Server.listen(QHostAddress::LocalHost, 0);
    Base = QStringLiteral("http://127.0.0.1:%1").arg(Server.serverPort());
    connect(&Server, &QTcpServer::newConnection, this, [this]()
    {
      while (QTcpSocket * S = Server.nextPendingConnection())
      {
        Serve(S);
      }
    });
  }

  void Serve(QTcpSocket * S)
  {
    auto Buf = std::make_shared<QByteArray>();
    connect(S, &QTcpSocket::readyRead, this, [this, S, Buf]()
    {
      Buf->append(S->readAll());
      qsizetype End = Buf->indexOf("\r\n\r\n");
      if (End < 0) return;
      QByteArray Head = Buf->left(End);
      qsizetype Length = 0;
      for (const QByteArray & Line : Head.split('\n'))
      {
        if (Line.toLower().startsWith("content-length:")) Length = Line.mid(15).trimmed().toLongLong();
      }
      if (Buf->size() < End + 4 + Length) return;
      QByteArray Body = Buf->mid(End + 4, Length);
      Buf->clear();
      QList<QByteArray> First = Head.left(Head.indexOf("\r\n")).split(' ');
      int Code = 200;
      Headers = Head;
      QByteArray Out = Handle(QString::fromLatin1(First[0]), QString::fromLatin1(First[1]), Body, Code);
      S->write("HTTP/1.1 " + QByteArray::number(Code) + " X\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " +
        QByteArray::number(Out.size()) + "\r\n\r\n" + Out);
      S->disconnectFromHost();
    });
    connect(S, &QTcpSocket::disconnected, S, &QObject::deleteLater);
  }

  static QByteArray J(const QJsonObject & O) { return QJsonDocument(O).toJson(QJsonDocument::Compact); }
  static QByteArray Error(int & Code, int Status, const QString & Text, const QJsonObject & More = QJsonObject())
  {
    QJsonObject O = More;
    O.insert(QStringLiteral("error"), Text);
    Code = Status;
    return J(O);
  }
  QString NewObject(const QByteArray & Data = QByteArray())
  {
    QString Key = QStringLiteral("o%1").arg(++Counter);
    Objects.insert(Key, Data);
    return Key;
  }

  void Publish(FakeTeam & T, qint64 Version, const QByteArray & Blob, qint64 KeyVersion)
  {
    T.Head = Version;
    T.Snapshots.insert(Version, Blob);
    T.KeyVersions.insert(Version, KeyVersion);
  }

  QJsonObject RosterHead(const FakeTeam & T) const
  {
    if (T.HasServeEntries)
    {
      TeamRoster::Entry Last;
      TeamRoster::Parse(T.ServeEntries.last().toObject(), Last);
      return QJsonObject{{"seq", Last.Seq}, {"hash", Last.Hash}};
    }
    return QJsonObject{{"seq", T.Roster.Seq}, {"hash", T.Roster.Hash}};
  }

  QByteArray ListTeams()
  {
    QJsonArray List;
    for (const FakeTeam & T : Teams)
    {
      const TeamRoster::Member * Me = T.Roster.Find(User);
      if (T.Hidden || ((Me == nullptr) && !T.HasServeEntries))
      {
        continue;
      }
      qint64 Head = (T.ServeHead >= 0) ? T.ServeHead : T.Head;
      QByteArray Wrap = !T.ServeWrap.isEmpty() ? T.ServeWrap : T.Wraps.value(User);
      List.append(QJsonObject{{"uuid", T.Uuid}, {"role", "owner"}, {"member_count", 99}, {"key_version", T.Roster.KeyVersion},
        {"my_key_version", T.ServeKeyVersion ? T.ServeKeyVersion : T.Roster.KeyVersion}, {"wrapped_team_key", B64(Wrap)}, {"roster", RosterHead(T)},
        {"needs_rotation", T.SayNeedsRotation},
        {"head", QJsonObject{{"version", Head}, {"sha256", Sha(T.Snapshots.value(Head))}, {"key_version", T.KeyVersions.value(Head)}}}});
    }
    return QJsonDocument(List).toJson(QJsonDocument::Compact);
  }

  QByteArray Handle(const QString & Method, const QString & Target, const QByteArray & Body, int & Code)
  {
    QUrl Url(Target);
    QString P = Url.path();
    QUrlQuery Q(Url);
    QJsonObject In = QJsonDocument::fromJson(Body).object();
    QString Request = Method + QLatin1Char(' ') + P;
    Log.append(Request);
    if (OnRequest)
    {
      auto Hook = OnRequest;   // it may clear itself
      Hook(Request);
    }
    for (auto It = FailStatus.begin(); It != FailStatus.end(); ++It)
    {
      if (Request.contains(It.key()) && (!FailCount.contains(It.key()) || (FailCount[It.key()] > 0)))
      {
        if (FailCount.contains(It.key())) FailCount[It.key()]--;
        return Error(Code, It.value(), QStringLiteral("injected"));
      }
    }
    if (P == "/v1/oauth/token") return J({{"access_token", "accessaccessaccessaccess"}, {"refresh_token", QStringLiteral("refresh%1").arg(++Counter)}, {"device_id", 1}});
    if (P == "/v1/me") return J({{"user", QJsonObject{{"uuid", User}, {"email", Email}}}, {"license", QJsonObject{{"active", true}}}});
    if (P == "/v1/keys/me")
    {
      if (Method == "PUT")
      {
        if (Keys.contains(User) && !In.contains("replace_signature")) return Error(Code, 409, "keys_exist");
        Keys.insert(User, In);
        return J({{"ok", true}});
      }
      if (!Keys.contains(User)) return Error(Code, 404, "no_keys");
      return J(Keys.value(User));
    }
    if (P.startsWith("/blob/"))
    {
      QString K = P.mid(6);
      if (Method == "PUT")
      {
        // A team upload URL creates an object, it does not rewrite one
        if (!Objects.value(K).isEmpty()) return Error(Code, 412, "precondition");
        LastHeaders = Headers;
        Objects[K] = Body;
        LastPut = Body;
        Puts++;
        return QByteArray();
      }
      return Objects.value(K);
    }
    if (P == "/v1/sync/head") return J({{"version", SyncHead.value(Q.queryItemValue("slot"))}});
    if (P == "/v1/sync/upload-url") { QString K = NewObject(); return J({{"object_key", K}, {"url", Base + "/blob/" + K}}); }
    if (P == "/v1/sync/commit")
    {
      QString Slot = In["slot"].toString();
      qint64 V = In["base_version"].toInteger() + 1;
      SyncHead[Slot] = V;
      SyncObjects[Slot][V] = In["object_key"].toString();
      Puts--;   // not a team upload
      return J({{"version", V}});
    }
    if (P == "/v1/sync/download-url")
    {
      QString K = SyncObjects[Q.queryItemValue("slot")].value(Q.queryItemValue("version").toLongLong());
      return J({{"sha256", Sha(Objects.value(K))}, {"url", Base + "/blob/" + K}});
    }
    if (P == "/v1/sync/history") return "[]";
    if (P == "/v1/teams")
    {
      if (Method == "GET") return TeamsRaw.isEmpty() ? ListTeams() : TeamsRaw;
      TeamRoster::Entry Item;
      FakeTeam T;
      T.Uuid = In["uuid"].toString();
      if (!TeamRoster::Parse(In["entry"].toObject(), Item) || !TeamRoster::Apply(T.Roster, Item)) return Error(Code, 400, "bad_entry");
      T.Entries.append(In["entry"]);
      T.Wraps.insert(User, UnB64(In["wrapped_team_key"]));
      Teams.insert(T.Uuid, T);
      Code = 201;
      return J({{"uuid", T.Uuid}, {"role", "owner"}, {"key_version", 1}, {"roster", QJsonObject{{"seq", 1}, {"hash", Item.Hash}}}});
    }
    QStringList Parts = P.split(QLatin1Char('/'));
    if ((Parts.size() >= 5) && (Parts[2] == "invites"))
    {
      QString Id = Parts[3];
      auto It = Invites.find(Id);
      bool Valid = (It != Invites.end()) && !It->Used && (Sha(UnB64(In["verifier"])) == It->VerifierHash);
      if (!Valid) return Error(Code, 404, "invalid_invite");
      FakeTeam & T = Teams[It->Team];
      if (Parts[4] == "open")
      {
        return J({{"team", QJsonObject{{"uuid", T.Uuid}}}, {"role", It->Role}, {"wrapped_team_key", B64(It->Wrap)}, {"key_version", It->KeyVersion},
          {"entries", T.HasServeEntries ? T.ServeEntries : T.Entries}});
      }
      TeamRoster::Entry Item;
      if (!TeamRoster::Parse(In["entry"].toObject(), Item) || !TeamRoster::Apply(T.Roster, Item)) return Error(Code, 409, "roster_conflict");
      T.Entries.append(In["entry"]);
      T.Wraps.insert(User, UnB64(In["wrapped_team_key"]));
      It->Used = true;
      return J({{"team", QJsonObject{{"uuid", T.Uuid}}}, {"role", It->Role}, {"key_version", It->KeyVersion}, {"roster", RosterHead(T)}});
    }
    if ((Parts.size() >= 4) && (Parts[2] == "teams"))
    {
      auto TeamIt = Teams.find(Parts[3]);
      if (TeamIt == Teams.end() || (!TeamIt->Roster.Find(User) && !TeamIt->HasServeEntries)) return Error(Code, 404, "not_found");
      FakeTeam & T = *TeamIt;
      QString Rest = Parts.mid(4).join(QLatin1Char('/'));
      if (Rest == "roster")
      {
        if (Method == "GET")
        {
          QJsonArray Entries = T.HasServeEntries ? T.ServeEntries : T.Entries;
          QJsonObject Head = RosterHead(T);
          return J({{"seq", Head["seq"]}, {"hash", Head["hash"]}, {"entries", Entries}});
        }
        TeamRoster::Entry Item;
        if (!TeamRoster::Parse(In["entry"].toObject(), Item)) return Error(Code, 400, "bad_entry");
        if (Item.Type == "delete")
        {
          QJsonObject Head = RosterHead(T);
          if ((Item.Seq != Head["seq"].toInteger() + 1) || (Item.Prev != Head["hash"].toString())) return Error(Code, 409, "roster_conflict");
          if (Item.Signer != T.Roster.Owner) return Error(Code, 403, "role_required");
          Teams.remove(Parts[3]);
          return J({{"ok", true}});
        }
        // What ends someone's admin rights names the committed head
        const TeamRoster::Member * Subject = T.Roster.Find((Item.Type == "leave") ? Item.Signer : Item.Data["user"].toString());
        bool EndsAdmin = (Item.Type == "leave") || (Item.Type == "remove") || ((Item.Type == "role") && (Item.Data["role"].toString() == "member"));
        if (CheckHead && EndsAdmin && (Subject != nullptr) && (Subject->Role == "admin") && (Item.Data["head"].toInteger() != T.Head))
        {
          return Error(Code, 409, "head_conflict", {{"head", QJsonObject{{"version", T.Head}}}});
        }
        if (!TeamRoster::Apply(T.Roster, Item)) return Error(Code, 409, "roster_conflict", {{"roster", RosterHead(T)}});
        T.Entries.append(In["entry"]);
        return J({{"ok", true}, {"roster", RosterHead(T)}});
      }
      if (Rest == "members")
      {
        if (!T.ServeMembers.isEmpty()) return QJsonDocument(T.ServeMembers).toJson(QJsonDocument::Compact);
        QJsonArray List;
        for (const TeamRoster::Member & M : T.Roster.Members) List.append(QJsonObject{{"user_uuid", M.Uuid}, {"email", M.Email}, {"role", M.Role}});
        return QJsonDocument(List).toJson(QJsonDocument::Compact);
      }
      if (Rest == "rotate")
      {
        TeamRoster::Entry Item;
        QJsonObject HeadBody = In["head"].toObject();
        if (!TeamRoster::Parse(In["entry"].toObject(), Item)) return Error(Code, 400, "bad_entry");
        if ((T.Head > 0) && HeadBody.isEmpty()) return Error(Code, 409, "head_required");
        if ((T.Head > 0) && (HeadBody["base_version"].toInteger() != T.Head)) return Error(Code, 409, "version_conflict", {{"head", QJsonObject{{"version", T.Head}}}});
        if (!TeamRoster::Apply(T.Roster, Item)) return Error(Code, 409, "roster_conflict");
        T.Entries.append(In["entry"]);
        for (const QJsonValue & W : In["wraps"].toArray()) T.Wraps.insert(W.toObject()["user_uuid"].toString(), UnB64(W.toObject()["wrapped_team_key"]));
        if (T.Head > 0) Publish(T, T.Head + 1, Objects.value(HeadBody["object_key"].toString()), T.Roster.KeyVersion);
        return J({{"ok", true}, {"key_version", T.Roster.KeyVersion}, {"roster", RosterHead(T)}});
      }
      if (Rest == "invites")
      {
        if (Method == "GET") return "[]";
        TeamRoster::Entry Item;
        if (!TeamRoster::Parse(In["entry"].toObject(), Item) || !TeamRoster::Apply(T.Roster, Item)) return Error(Code, 409, "roster_conflict");
        T.Entries.append(In["entry"]);
        Invites.insert(In["id"].toString(), Invite{T.Uuid, Item.Data["role"].toString(), UnB64(In["wrapped_team_key"]), In["key_version"].toInteger(), In["verifier_hash"].toString()});
        Code = 201;
        return J({{"id", In["id"]}, {"roster", RosterHead(T)}});
      }
      if (Rest == "sync/head") return J({{"version", T.Head}, {"sha256", Sha(T.Snapshots.value(T.Head))}});
      if (Rest == "sync/history")
      {
        QJsonArray List;
        for (qint64 V = T.Head; V >= 1; V--) List.append(QJsonObject{{"version", V}, {"sha256", Sha(T.Snapshots.value(V))}, {"key_version", T.KeyVersions.value(V)}});
        return QJsonDocument(List).toJson(QJsonDocument::Compact);
      }
      if (Rest == "sync/download-url")
      {
        qint64 V = Q.queryItemValue("version").toLongLong();
        if (!T.Snapshots.contains(V)) return Error(Code, 404, "not_found");
        QString K = NewObject(T.Snapshots.value(V));
        return J({{"version", V}, {"sha256", Sha(Objects.value(K))}, {"url", Base + "/blob/" + K}});
      }
      if (Rest == "sync/upload-url")
      {
        if (In["base_version"].toInteger() != T.Head) return Error(Code, 409, "version_conflict", {{"head", QJsonObject{{"version", T.Head}, {"sha256", Sha(T.Snapshots.value(T.Head))}}}});
        QString K = NewObject();
        return J({{"object_key", K}, {"url", Base + "/blob/" + K}, {"headers", QJsonObject{{"If-None-Match", "*"}}}});
      }
      if (Rest == "sync/commit")
      {
        if (T.Roster.NeedsRotation) return Error(Code, 409, "rotation_required");
        if (In["key_version"].toInteger() != T.Roster.KeyVersion) return Error(Code, 409, "key_version_mismatch");
        if (In["base_version"].toInteger() != T.Head) return Error(Code, 409, "version_conflict", {{"head", QJsonObject{{"version", T.Head}, {"sha256", Sha(T.Snapshots.value(T.Head))}}}});
        Publish(T, T.Head + 1, Objects.value(In["object_key"].toString()), In["key_version"].toInteger());
        Commits++;
        return J({{"version", T.Head}});
      }
    }
    return Error(Code, 404, "not_found");
  }

  // ---- what the other people do ----

  void Append(FakeTeam & T, const TeamRoster::Entry & Item)
  {
    QString Why;
    bool Ok = TeamRoster::Apply(T.Roster, Item, &Why);
    CHECK_MSG(Ok, Why);
    T.Entries.append(TeamRoster::ToWire(Item));
  }

  void AddKeys(const Peer & P)
  {
    Keys.insert(P.Uuid, QJsonObject{{"public_key", B64(P.Box.Public)}, {"identity_key", B64(P.Identity.Public)}});
  }

  // A team a peer made: the victim gets into it with PeerInvite and JoinWithCode
  QString PeerCreate(const Peer & Owner)
  {
    FakeTeam T;
    T.Uuid = NewUuid();
    T.Key = TeamCrypto::NewTeamKey();
    QByteArray Wrap = TeamCrypto::WrapForMember(Owner.Box.Public, T.Key, T.Uuid, 1);
    Append(T, TeamRoster::Make(T.Roster, T.Uuid, "create", Owner.Uuid,
      TeamRoster::CreateData(Owner.Email, Owner.Identity, Owner.Uuid, Owner.Box.Public, Wrap), Owner.Identity));
    T.Wraps.insert(Owner.Uuid, Wrap);
    Teams.insert(T.Uuid, T);
    AddKeys(Owner);
    return T.Uuid;
  }

  QString PeerInvite(const Peer & Admin, const QString & Uuid, const QString & Role, const QString & Name)
  {
    FakeTeam & T = Teams[Uuid];
    TeamCrypto::Invite Code = TeamCrypto::NewInvite();
    QString Expires = QDateTime::currentDateTimeUtc().addDays(3).toString("yyyy-MM-ddTHH:mm:ss.zzzZ");
    TeamRoster::Entry Item = TeamRoster::Make(T.Roster, Uuid, "invite", Admin.Uuid, QJsonObject{{"id", Code.Id}, {"role", Role}, {"expires", Expires},
      {"public", B64(TeamCrypto::InviteSignKey(Code).Public)}, {"key_version", T.Roster.KeyVersion}}, Admin.Identity);
    Append(T, Item);
    TeamCrypto::InviteContent Content;
    Content.TeamKey = T.Key;
    Content.TeamName = Name;
    Content.RosterSeq = Item.Seq;
    Content.RosterHash = Item.Hash;
    Invites.insert(Code.Id, Invite{Uuid, Role, TeamCrypto::WrapForInvite(Code, Content, Uuid, T.Roster.KeyVersion), T.Roster.KeyVersion, TeamCrypto::InviteVerifierHash(Code)});
    return Code.Code();
  }

  void PeerJoin(const Peer & P, const QString & Text)
  {
    TeamCrypto::Invite Code;
    CHECK(TeamCrypto::ParseInviteCode(Text, Code));
    Invite & I = Invites[Code.Id];
    FakeTeam & T = Teams[I.Team];
    TeamCrypto::InviteContent Content;
    CHECK(TeamCrypto::UnwrapForInvite(Code, I.Wrap, T.Uuid, I.KeyVersion, Content));
    T.Key = Content.TeamKey;
    QByteArray Wrap = TeamCrypto::WrapForMember(P.Box.Public, T.Key, T.Uuid, I.KeyVersion);
    TeamCrypto::SignKey InviteKey = TeamCrypto::InviteSignKey(Code);
    Append(T, TeamRoster::Make(T.Roster, T.Uuid, "join", P.Uuid, TeamRoster::JoinData(Code.Id, P.Email, P.Identity, P.Uuid, P.Box.Public, Wrap), P.Identity, &InviteKey));
    T.Wraps.insert(P.Uuid, Wrap);
    I.Used = true;
    AddKeys(P);
  }

  QByteArray PeerSnapshot(const Peer & Author, FakeTeam & T, qint64 Version, const QByteArray & Content, const QByteArray & Key = QByteArray(), qint64 RosterSeq = 0, qint64 KeyVersion = 0)
  {
    TeamCrypto::Snapshot H;
    H.Team = T.Uuid;
    H.Version = Version;
    H.KeyVersion = KeyVersion ? KeyVersion : T.Roster.KeyVersion;
    H.Author = Author.Uuid;
    H.RosterSeq = RosterSeq ? RosterSeq : T.Roster.Seq;
    H.Previous = T.Snapshots.contains(Version - 1) ? Sha(T.Snapshots.value(Version - 1)) : QString();
    H.SavedAt = QDateTime::currentDateTimeUtc();
    return TeamCrypto::EncodeSnapshot(H, Key.isEmpty() ? T.Key : Key, Author.Identity, Content);
  }

  void PeerPush(const Peer & Author, const QString & Uuid, const QByteArray & Content)
  {
    FakeTeam & T = Teams[Uuid];
    Publish(T, T.Head + 1, PeerSnapshot(Author, T, T.Head + 1, Content), T.Roster.KeyVersion);
  }

  void PeerRemove(const Peer & Admin, const QString & Uuid, const QString & User)
  {
    FakeTeam & T = Teams[Uuid];
    Append(T, TeamRoster::Make(T.Roster, Uuid, "remove", Admin.Uuid, QJsonObject{{"user", User}, {"head", T.Head}}, Admin.Identity));
    T.Wraps.remove(User);
  }

  void PeerRole(const Peer & Owner, const QString & Uuid, const QString & User, const QString & Role)
  {
    FakeTeam & T = Teams[Uuid];
    Append(T, TeamRoster::Make(T.Roster, Uuid, "role", Owner.Uuid, QJsonObject{{"user", User}, {"role", Role}, {"head", T.Head}}, Owner.Identity));
  }

  // A peer renews the key: wraps for the members, the head under the new key
  void PeerRotate(const Peer & Admin, const QString & Uuid)
  {
    FakeTeam & T = Teams[Uuid];
    QByteArray NewKey = TeamCrypto::NewTeamKey();
    qint64 Version = T.Roster.KeyVersion + 1;
    QJsonObject Hashes;
    for (const TeamRoster::Member & M : T.Roster.Members)
    {
      QByteArray Wrap = TeamCrypto::WrapForMember(M.Box, NewKey, Uuid, Version);
      T.Wraps.insert(M.Uuid, Wrap);
      Hashes.insert(M.Uuid, TeamRoster::WrapHash(Wrap));
    }
    QByteArray Plain;
    if (T.Head > 0) CHECK(TeamCrypto::DecodeSnapshot(T.Snapshots.value(T.Head), T.Key, Plain));
    Append(T, TeamRoster::Make(T.Roster, Uuid, "rotate", Admin.Uuid, QJsonObject{{"key_version", Version}, {"wraps", Hashes}}, Admin.Identity));
    T.Key = NewKey;
    if (T.Head > 0) Publish(T, T.Head + 1, PeerSnapshot(Admin, T, T.Head + 1, Plain), Version);
  }
};

static Fake * Srv;
static QString Dir;
static QStringList Notices;
static int Received = 0;
static QList<QList<int>> Counts;

static void Spin(const std::function<bool()> & Until, int Ms = 20000)
{
  QElapsedTimer Timer;
  Timer.start();
  while (!Until() && (Timer.elapsed() < Ms))
  {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
  }
}

static bool Wait(const std::function<void(const TeamSync::Done &)> & Operation, QString * Error = nullptr)
{
  auto Result = std::make_shared<int>(0);
  auto Message = std::make_shared<QString>();
  Operation([Result, Message](bool Ok, const QString & E) { *Result = Ok ? 1 : 2; *Message = E; });
  Spin([Result]() { return *Result != 0; });
  if (Error != nullptr) *Error = *Message;
  return *Result == 1;
}

static bool SyncAll(QString * Error = nullptr)
{
  return Wait([](const auto & D) { TeamSync::Instance().SyncAll(D); }, Error);
}

static TeamSync & Teams() { return TeamSync::Instance(); }
static StoredSessionList & Sites() { return Configuration::Instance().StoredSessions(); }
static std::shared_ptr<SessionData> Site(const QString & Name) { return Sites().FindByName(Name); }

static void AddSite(const QString & Name, const QString & Host = QStringLiteral("db.internal"), const QString & Password = QStringLiteral("S3cret-password"))
{
  SessionData S;
  S.Name = Name;
  S.HostName = Host;
  S.UserName = QStringLiteral("deploy");
  S.Password = Password;
  S.FSProtocol = fsSFTP;
  Sites().Add(std::make_shared<SessionData>(S));
  Configuration::Instance().Save();
}

static void SignIn()
{
  CloudSync & Sync = CloudSync::Instance();
  CloudSync::SignInRequest Request = Sync.NewSignInRequest(53685);
  auto Done = std::make_shared<int>(0);
  Sync.CompleteSignIn(Request, QStringLiteral("code"), [Done](bool Ok, const QString &) { *Done = Ok ? 1 : 2; });
  Spin([Done]() { return *Done != 0; });
  CHECK(*Done == 1);
}

static void MakeKey()
{
  auto Done = std::make_shared<int>(0);
  CloudSync::Instance().CreateKey(SyncPassword, true, [Done](bool Ok, const QString &) { *Done = Ok ? 1 : 2; });
  Spin([Done]() { return *Done != 0; });
  CHECK(*Done == 1);
  Spin([]() { return CloudSync::Instance().State() == CloudSync::csIdle; }, 5000);
  CHECK(Teams().Available());
}

static void Boot()
{
  Configuration & Config = Configuration::Instance();
  Config.SetIniFileName(Dir + QStringLiteral("/nixscp.ini"));
  Config.Load();
  CloudSync::Instance().Initialize(Config);
  Teams().Initialize(Config, CloudSync::Instance());
  QObject::connect(&Teams(), &TeamSync::Notice, &Teams(), [](const QString & M) { Notices.append(M); });
  QObject::connect(&Teams(), &TeamSync::SitesReceived, &Teams(), [](const QString &, int A, int R, int C) { Received++; Counts.append({A, R, C}); });
  SignIn();
  MakeKey();
}

static QString CreateTeam(const QString & Name)
{
  QSet<QString> Before;
  for (const TeamSync::Team & T : Teams().Teams()) Before.insert(T.Uuid);
  QString Error;
  bool Ok = Wait([&](const auto & D) { Teams().CreateTeam(Name, D); }, &Error);
  CHECK_MSG(Ok, Error);
  for (const TeamSync::Team & T : Teams().Teams())
  {
    if (!Before.contains(T.Uuid)) return T.Uuid;
  }
  return QString();
}

static QString FolderOf(const QString & Uuid)
{
  const TeamSync::Team * T = Teams().Find(Uuid);
  return (T != nullptr) ? T->Folder : QString();
}

// The victim invites a peer to its team, with a role
static QString InviteCode(const QString & Uuid, const QString & Role)
{
  auto Code = std::make_shared<QString>();
  auto Done = std::make_shared<int>(0);
  Teams().CreateInvite(Uuid, Role, 3, [Code, Done](bool Ok, const QString &, const QString & C) { *Code = C; *Done = Ok ? 1 : 2; });
  Spin([Done]() { return *Done != 0; });
  CHECK(*Done == 1);
  return *Code;
}

// Content of a snapshot: sites as (name, host, password)
static QByteArray Content(const QString & TeamName, const QList<QStringList> & List)
{
  IniFile Ini;
  for (const QStringList & Item : List)
  {
    SessionData S;
    S.HostName = Item.value(1);
    S.UserName = QStringLiteral("deploy");
    S.Password = Item.value(2);
    S.FSProtocol = fsSFTP;
    CHECK(TeamContent::AddSite(Ini, Item.value(0), S));
  }
  return TeamContent::Encode(TeamName, Ini);
}

// What the server received as the team's head, read with the team's key
static bool Read(const QByteArray & Blob, const QByteArray & Key, QString & Name, QStringList & Names)
{
  QByteArray Plain;
  IniFile Ini;
  if (!TeamCrypto::DecodeSnapshot(Blob, Key, Plain) || !TeamContent::Decode(Plain, Name, Ini)) return false;
  Names = TeamContent::SiteHashes(Ini).keys();
  Names.sort();
  return true;
}

// A plain team of the victim as owner, with one shared site and Bob as a member
struct Setup
{
  QString Uuid;
  QString Folder;
  Peer Bob;
  QByteArray Key;
};

static Setup OwnedTeam(const QString & Role = QStringLiteral("member"))
{
  Setup S;
  S.Uuid = CreateTeam(QStringLiteral("Prod"));
  S.Folder = FolderOf(S.Uuid);
  CHECK(S.Folder == QStringLiteral("Team: Prod"));
  AddSite(S.Folder + QStringLiteral("/db"));
  CHECK(SyncAll());
  S.Bob = NewPeer(QStringLiteral("bob"));
  Srv->PeerJoin(S.Bob, InviteCode(S.Uuid, Role));
  CHECK(SyncAll());
  S.Key = Teams().Find(S.Uuid)->Key.Copy();
  return S;
}

// A team of a peer the victim joins with a code
struct Joined
{
  QString Uuid;
  QString Folder;
  Peer Owner;
};

static Joined MemberOf(const QString & Name, const QList<QStringList> & List, const QString & Role = QStringLiteral("member"))
{
  Joined J;
  J.Owner = NewPeer(QStringLiteral("owner"));
  J.Uuid = Srv->PeerCreate(J.Owner);
  Srv->PeerPush(J.Owner, J.Uuid, Content(Name, List));
  QString Code = Srv->PeerInvite(J.Owner, J.Uuid, Role, Name);
  QString Error;
  bool Ok = Wait([&](const auto & D) { Teams().JoinWithCode(Code, D); }, &Error);
  CHECK_MSG(Ok, Error);
  J.Folder = FolderOf(J.Uuid);
  return J;
}

static bool Rejected(const QString & Uuid)
{
  const TeamSync::Team * T = Teams().Find(Uuid);
  return (T != nullptr) && !T->Error.isEmpty();
}

