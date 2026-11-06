#include "core/TeamRoster.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>

namespace TeamRoster
{

namespace
{

const QByteArray Domain = QByteArrayLiteral("NXT2-ROSTER|");
const QString Genesis = QString(64, QLatin1Char('0'));
constexpr int MaxBody = 12 * 1024;

const QString RoleOwner = QStringLiteral("owner");
const QString RoleAdmin = QStringLiteral("admin");
const QString RoleMember = QStringLiteral("member");

bool Fail(QString * Error, const QString & Message)
{
  if (Error != nullptr)
  {
    *Error = Message;
  }
  return false;
}

QByteArray UnB64(const QJsonValue & Value)
{
  return QByteArray::fromBase64(Value.toString().toLatin1());
}

bool IsHash(const QString & Text)
{
  static const QRegularExpression Pattern(QStringLiteral("^[0-9a-f]{64}$"));
  return Pattern.match(Text).hasMatch();
}

bool IsTime(const QString & Text)
{
  static const QRegularExpression Pattern(QStringLiteral("^\\d{4}-\\d{2}-\\d{2}T\\d{2}:\\d{2}:\\d{2}\\.\\d{3}Z$"));
  return Pattern.match(Text).hasMatch();
}

bool IsInviteRole(const QString & Role)
{
  return (Role == RoleAdmin) || (Role == RoleMember);
}

bool SignedBy(const QByteArray & Public, const Entry & Item, const QByteArray & Signature)
{
  return TeamCrypto::Verify(Public, Domain + Item.Body, Signature);
}

// create and join: the member's keys, with the identity's statement on the box key
bool ReadKeys(const Entry & Item, Member & Result, QString * Error)
{
  Result.Uuid = Item.Signer;
  Result.Email = Item.Data.value(QStringLiteral("email")).toString();
  Result.Identity = UnB64(Item.Data.value(QStringLiteral("identity")));
  Result.Box = UnB64(Item.Data.value(QStringLiteral("box")));
  Result.Usable = TeamCrypto::IsUsableBoxKey(Result.Box);
  Result.WrapHash = Item.Data.value(QStringLiteral("wrap_hash")).toString();
  Result.JoinedSeq = Item.Seq;
  Result.Joined = Item.Time;
  QByteArray BoxSig = UnB64(Item.Data.value(QStringLiteral("box_sig")));
  if ((Result.Identity.size() != 32) || (Result.Box.size() != 32) || !IsHash(Result.WrapHash))
  {
    return Fail(Error, QStringLiteral("a member's keys are malformed"));
  }
  if (!TeamCrypto::Verify(Result.Identity, TeamCrypto::BoxKeyMessage(Item.Signer, Result.Box), BoxSig))
  {
    return Fail(Error, QStringLiteral("a member's box key is not signed by their identity key"));
  }
  if (!SignedBy(Result.Identity, Item, Item.Sig))
  {
    return Fail(Error, QStringLiteral("an entry is not signed by the new member"));
  }
  return true;
}

void NoteRole(State & S, const QString & Uuid, qint64 Seq, const QString & Role, const QByteArray & Identity, qint64 Head)
{
  S.Roles[Uuid].append(RoleChange{Seq, Role, Identity, Head});
}

qint64 HeadOf(const Entry & Item, bool & Valid)
{
  QJsonValue Value = Item.Data.value(QStringLiteral("head"));
  Valid = Value.isDouble() && (Value.toInteger(-1) >= 0);
  return Value.toInteger(-1);
}

}

const Member * State::Find(const QString & Uuid) const
{
  auto It = Members.constFind(Uuid);
  return (It == Members.cend()) ? nullptr : &It.value();
}

bool State::CanEdit(const QString & Uuid) const
{
  const Member * M = Find(Uuid);
  return (M != nullptr) && ((M->Role == RoleOwner) || (M->Role == RoleAdmin));
}

qint64 State::KeyVersionAt(qint64 Position) const
{
  qint64 Result = 0;
  for (const auto & Key : Keys)
  {
    if (Key.first > Position)
    {
      break;
    }
    Result = Key.second;
  }
  return Result;
}

bool State::AuthorAllowed(const QString & Uuid, qint64 Position, qint64 Version, QByteArray * Identity) const
{
  auto It = Roles.constFind(Uuid);
  if ((It == Roles.cend()) || (Position < 1) || (Position > Seq))
  {
    return false;
  }
  const RoleChange * AtPosition = nullptr;
  for (const RoleChange & Change : *It)
  {
    if (Change.Seq <= Position)
    {
      AtPosition = &Change;
    }
    else if ((Change.Role != RoleOwner) && (Change.Role != RoleAdmin) && (Version > Change.Head))
    {
      // written after they stopped being an admin, whatever position it claims
      return false;
    }
  }
  if ((AtPosition == nullptr) || ((AtPosition->Role != RoleOwner) && (AtPosition->Role != RoleAdmin)))
  {
    return false;
  }
  if (Identity != nullptr)
  {
    *Identity = AtPosition->Identity;
  }
  return true;
}

bool IsUuid(const QString & Text)
{
  static const QRegularExpression Pattern(QStringLiteral("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"));
  return Pattern.match(Text).hasMatch();
}

QString WrapHash(const QByteArray & Wrap)
{
  return QString::fromLatin1(QCryptographicHash::hash(Wrap, QCryptographicHash::Sha256).toHex());
}

QString NowText()
{
  return QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss.zzzZ"));
}

bool Parse(const QJsonObject & Wire, Entry & Result, QString * Error)
{
  QString BodyText = Wire.value(QStringLiteral("body")).toString();
  if (BodyText.isEmpty() || (BodyText.size() > MaxBody * 2))
  {
    return Fail(Error, QStringLiteral("an entry has no body"));
  }
  Result = Entry();
  Result.Body = QByteArray::fromBase64(BodyText.toLatin1());
  Result.Sig = UnB64(Wire.value(QStringLiteral("sig")));
  Result.Sig2 = UnB64(Wire.value(QStringLiteral("sig2")));
  QJsonParseError Parsed;
  QJsonDocument Document = QJsonDocument::fromJson(Result.Body, &Parsed);
  if ((Parsed.error != QJsonParseError::NoError) || !Document.isObject())
  {
    return Fail(Error, QStringLiteral("an entry cannot be read"));
  }
  QJsonObject Json = Document.object();
  Result.Team = Json.value(QStringLiteral("team")).toString();
  Result.Seq = Json.value(QStringLiteral("seq")).toInteger();
  Result.Prev = Json.value(QStringLiteral("prev")).toString();
  Result.Type = Json.value(QStringLiteral("type")).toString();
  Result.Signer = Json.value(QStringLiteral("signer")).toString();
  Result.Time = Json.value(QStringLiteral("time")).toString();
  Result.Data = Json.value(QStringLiteral("data")).toObject();
  Result.Hash = QString::fromLatin1(QCryptographicHash::hash(Result.Body, QCryptographicHash::Sha256).toHex());
  if ((Json.value(QStringLiteral("v")).toInt() != 2) || !IsUuid(Result.Team) || (Result.Seq < 1) || !IsHash(Result.Prev) ||
      !IsUuid(Result.Signer) || !IsTime(Result.Time) || !Json.value(QStringLiteral("data")).isObject())
  {
    return Fail(Error, QStringLiteral("an entry is malformed"));
  }
  return true;
}

QJsonObject ToWire(const Entry & Item)
{
  QJsonObject Wire{{QStringLiteral("body"), QString::fromLatin1(Item.Body.toBase64())},
    {QStringLiteral("sig"), QString::fromLatin1(Item.Sig.toBase64())}};
  if (!Item.Sig2.isEmpty())
  {
    Wire.insert(QStringLiteral("sig2"), QString::fromLatin1(Item.Sig2.toBase64()));
  }
  return Wire;
}

Entry Make(const State & S, const QString & Team, const QString & Type, const QString & Signer, const QJsonObject & Data,
  const TeamCrypto::SignKey & Key, const TeamCrypto::SignKey * Second)
{
  QJsonObject Json{
    {QStringLiteral("v"), 2},
    {QStringLiteral("team"), Team},
    {QStringLiteral("seq"), S.Seq + 1},
    {QStringLiteral("prev"), (S.Seq == 0) ? Genesis : S.Hash},
    {QStringLiteral("type"), Type},
    {QStringLiteral("signer"), Signer},
    {QStringLiteral("time"), NowText()},
    {QStringLiteral("data"), Data},
  };
  Entry Item;
  Item.Body = QJsonDocument(Json).toJson(QJsonDocument::Compact);
  Item.Sig = TeamCrypto::Sign(Key, Domain + Item.Body);
  if (Second != nullptr)
  {
    Item.Sig2 = TeamCrypto::Sign(*Second, Domain + Item.Body);
  }
  QJsonObject Wire = ToWire(Item);
  Parse(Wire, Item);
  return Item;
}

// Every branch checks all it needs before it changes anything: a refused entry
// leaves S as it was, without a copy of a state that grows with the chain
bool Apply(State & S, const Entry & Item, QString * Error)
{
  if ((Item.Seq != S.Seq + 1) || (Item.Prev != ((S.Seq == 0) ? Genesis : S.Hash)))
  {
    return Fail(Error, QStringLiteral("the chain of entries is broken at %1").arg(Item.Seq));
  }
  if ((S.Seq > 0) && (Item.Team != S.Team))
  {
    return Fail(Error, QStringLiteral("an entry belongs to another team"));
  }
  if ((S.Seq == 0) != (Item.Type == QStringLiteral("create")))
  {
    return Fail(Error, QStringLiteral("the chain does not start with the team's creation"));
  }
  const Member * Signer = S.Find(Item.Signer);
  auto Sign = [&]()
  {
    return (Signer != nullptr) && SignedBy(Signer->Identity, Item, Item.Sig);
  };
  auto Editor = [&]()
  {
    return Sign() && S.CanEdit(Item.Signer);
  };
  if (Item.Type == QStringLiteral("create"))
  {
    Member Founder;
    if (!ReadKeys(Item, Founder, Error))
    {
      return false;
    }
    Founder.Role = RoleOwner;
    Founder.WrapVersion = 1;
    S.Team = Item.Team;
    S.Owner = Item.Signer;
    S.KeyVersion = 1;
    S.Keys.append({Item.Seq, 1});
    S.Members.insert(Founder.Uuid, Founder);
    NoteRole(S, Founder.Uuid, Item.Seq, RoleOwner, Founder.Identity, -1);
  }
  else if (Item.Type == QStringLiteral("invite"))
  {
    PendingInvite Invite;
    Invite.Id = Item.Data.value(QStringLiteral("id")).toString();
    Invite.Role = Item.Data.value(QStringLiteral("role")).toString();
    Invite.Expires = Item.Data.value(QStringLiteral("expires")).toString();
    Invite.Public = UnB64(Item.Data.value(QStringLiteral("public")));
    Invite.KeyVersion = Item.Data.value(QStringLiteral("key_version")).toInteger();
    if (!Editor() || (Invite.Id.size() != 16) || !IsInviteRole(Invite.Role) || !IsTime(Invite.Expires) || (Invite.Public.size() != 32) ||
        (Invite.KeyVersion != S.KeyVersion) || S.Invites.contains(Invite.Id))
    {
      return Fail(Error, QStringLiteral("an invite is not signed by an admin of the team"));
    }
    if ((Invite.Role == RoleAdmin) && (Signer->Role != RoleOwner))
    {
      return Fail(Error, QStringLiteral("an admin was invited by someone other than the owner"));
    }
    S.Invites.insert(Invite.Id, Invite);
  }
  else if (Item.Type == QStringLiteral("join"))
  {
    auto Invite = S.Invites.constFind(Item.Data.value(QStringLiteral("invite")).toString());
    if (Invite == S.Invites.cend())
    {
      return Fail(Error, QStringLiteral("a member joined without an invite of the team"));
    }
    Member Joiner;
    if (!ReadKeys(Item, Joiner, Error))
    {
      return false;
    }
    if (!SignedBy(Invite->Public, Item, Item.Sig2))
    {
      return Fail(Error, QStringLiteral("a member joined without proof of the invite code"));
    }
    if (S.Members.contains(Joiner.Uuid) || (Item.Time > Invite->Expires))
    {
      return Fail(Error, QStringLiteral("a member joined with an invite that was not valid"));
    }
    Joiner.Role = Invite->Role;
    Joiner.WrapVersion = Invite->KeyVersion;
    QString Id = Invite->Id;
    NoteRole(S, Joiner.Uuid, Item.Seq, Joiner.Role, Joiner.Identity, -1);
    S.Members.insert(Joiner.Uuid, Joiner);
    S.Invites.remove(Id);
  }
  else if (Item.Type == QStringLiteral("revoke"))
  {
    QString Id = Item.Data.value(QStringLiteral("invite")).toString();
    if (!Editor() || !S.Invites.contains(Id))
    {
      return Fail(Error, QStringLiteral("an invite was revoked by someone who may not"));
    }
    S.Invites.remove(Id);
  }
  else if ((Item.Type == QStringLiteral("remove")) || (Item.Type == QStringLiteral("leave")))
  {
    bool Leave = (Item.Type == QStringLiteral("leave"));
    QString Uuid = Leave ? Item.Signer : Item.Data.value(QStringLiteral("user")).toString();
    const Member * Target = S.Find(Uuid);
    bool ValidHead = false;
    qint64 Head = HeadOf(Item, ValidHead);
    if (!Sign() || (Target == nullptr) || !ValidHead || (Target->Role == RoleOwner))
    {
      return Fail(Error, QStringLiteral("a removal is not valid"));
    }
    if (!Leave && (Signer->Role != RoleOwner) && !((Signer->Role == RoleAdmin) && (Target->Role == RoleMember)))
    {
      return Fail(Error, QStringLiteral("a member was removed by someone who may not"));
    }
    NoteRole(S, Uuid, Item.Seq, QString(), Target->Identity, Head);
    S.Members.remove(Uuid);
    // They know the key and every pending invite
    S.NeedsRotation = true;
    S.Invites.clear();
  }
  else if (Item.Type == QStringLiteral("role"))
  {
    QString Uuid = Item.Data.value(QStringLiteral("user")).toString();
    QString Role = Item.Data.value(QStringLiteral("role")).toString();
    bool ValidHead = false;
    qint64 Head = HeadOf(Item, ValidHead);
    auto Target = S.Members.find(Uuid);
    if (!Sign() || (Signer->Role != RoleOwner) || (Target == S.Members.end()) || (Target->Role == RoleOwner) || !IsInviteRole(Role) || !ValidHead)
    {
      return Fail(Error, QStringLiteral("a role was changed by someone other than the owner"));
    }
    Target->Role = Role;
    NoteRole(S, Uuid, Item.Seq, Role, Target->Identity, Head);
  }
  else if (Item.Type == QStringLiteral("rotate"))
  {
    qint64 KeyVersion = Item.Data.value(QStringLiteral("key_version")).toInteger();
    QJsonObject Wraps = Item.Data.value(QStringLiteral("wraps")).toObject();
    if (!Editor() || (KeyVersion != S.KeyVersion + 1))
    {
      return Fail(Error, QStringLiteral("a key rotation is not signed by an admin"));
    }
    // A key nothing can be wrapped for is left out: it never blocks a rotation
    QStringList Covered = Wraps.keys();
    QSet<QString> Expected;
    for (const Member & M : S.Members)
    {
      if (M.Usable)
      {
        Expected.insert(M.Uuid);
      }
    }
    if (QSet<QString>(Covered.cbegin(), Covered.cend()) != Expected)
    {
      return Fail(Error, QStringLiteral("a key rotation does not cover exactly the members"));
    }
    for (const QString & Uuid : Covered)
    {
      if (!IsHash(Wraps.value(Uuid).toString()))
      {
        return Fail(Error, QStringLiteral("a key rotation is malformed"));
      }
    }
    for (const QString & Uuid : Covered)
    {
      Member & M = S.Members[Uuid];
      M.WrapHash = Wraps.value(Uuid).toString();
      M.WrapVersion = KeyVersion;
    }
    S.KeyVersion = KeyVersion;
    S.Keys.append({Item.Seq, KeyVersion});
    S.NeedsRotation = false;
    S.Invites.clear();
  }
  else
  {
    return Fail(Error, QStringLiteral("an entry has an unknown type"));
  }
  S.Seq = Item.Seq;
  S.Hash = Item.Hash;
  S.Hashes.insert(Item.Seq, Item.Hash);
  return true;
}

bool Replay(const QJsonArray & Wire, const QString & Team, State & Result, QList<Entry> * Items, QString * Error)
{
  if (Wire.size() > MaxEntries)
  {
    return Fail(Error, QStringLiteral("the member list is too long"));
  }
  State S;
  for (const QJsonValue & Value : Wire)
  {
    Entry Item;
    if (!Value.isObject() || !Parse(Value.toObject(), Item, Error))
    {
      return false;
    }
    if (Item.Team != Team)
    {
      return Fail(Error, QStringLiteral("an entry belongs to another team"));
    }
    if (!Apply(S, Item, Error))
    {
      return false;
    }
    if (Items != nullptr)
    {
      Items->append(Item);
    }
  }
  if (S.Seq == 0)
  {
    return Fail(Error, QStringLiteral("the member list is empty"));
  }
  Result = S;
  return true;
}

static QJsonObject KeyData(const QString & Email, const TeamCrypto::SignKey & Identity, const QString & UserUuid,
  const QByteArray & Box, const QByteArray & Wrap)
{
  return QJsonObject{
    {QStringLiteral("email"), Email},
    {QStringLiteral("identity"), QString::fromLatin1(Identity.Public.toBase64())},
    {QStringLiteral("box"), QString::fromLatin1(Box.toBase64())},
    {QStringLiteral("box_sig"), QString::fromLatin1(TeamCrypto::Sign(Identity, TeamCrypto::BoxKeyMessage(UserUuid, Box)).toBase64())},
    {QStringLiteral("wrap_hash"), WrapHash(Wrap)},
  };
}

QJsonObject CreateData(const QString & Email, const TeamCrypto::SignKey & Identity, const QString & UserUuid, const QByteArray & Box, const QByteArray & Wrap)
{
  return KeyData(Email, Identity, UserUuid, Box, Wrap);
}

QJsonObject JoinData(const QString & InviteId, const QString & Email, const TeamCrypto::SignKey & Identity, const QString & UserUuid,
  const QByteArray & Box, const QByteArray & Wrap)
{
  QJsonObject Data = KeyData(Email, Identity, UserUuid, Box, Wrap);
  Data.insert(QStringLiteral("invite"), InviteId);
  return Data;
}

}
