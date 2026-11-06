#include "core/TeamCrypto.h"
#include "core/CloudCrypto.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QtEndian>

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/params.h>

#include <memory>

namespace TeamCrypto
{

namespace
{

const char Alphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
const QByteArray SnapshotMagic = QByteArrayLiteral("NXT2");
const QByteArray SnapshotDomain = QByteArrayLiteral("NXT2-SNAPSHOT|");
constexpr quint32 MaxSnapshotHeader = 4096;
constexpr int NonceAndTag = 12 + 16;

QString Base32(const QByteArray & Data)
{
  // big-endian bit order, no padding
  QString Result;
  quint32 Buffer = 0;
  int Bits = 0;
  for (char C : Data)
  {
    Buffer = (Buffer << 8) | static_cast<quint8>(C);
    Bits += 8;
    while (Bits >= 5)
    {
      Result += QLatin1Char(Alphabet[(Buffer >> (Bits - 5)) & 0x1F]);
      Bits -= 5;
    }
  }
  if (Bits > 0)
  {
    Result += QLatin1Char(Alphabet[(Buffer << (5 - Bits)) & 0x1F]);
  }
  return Result;
}

// Normalized Crockford digits (uppercase, O=0, I/L=1); empty when invalid
QString Normalize(const QString & Text)
{
  QString Result;
  for (QChar C : Text.toUpper())
  {
    if (C.isSpace())
    {
      continue;
    }
    if (C == QLatin1Char('O')) C = QLatin1Char('0');
    if ((C == QLatin1Char('I')) || (C == QLatin1Char('L'))) C = QLatin1Char('1');
    if ((C.unicode() >= 128) || !QByteArray(Alphabet).contains(C.toLatin1()))
    {
      return QString();
    }
    Result += C;
  }
  return Result;
}

QByteArray FromBase32(const QString & Digits, int Bytes)
{
  QByteArray Result;
  quint32 Buffer = 0;
  int Bits = 0;
  for (QChar C : Digits)
  {
    Buffer = (Buffer << 5) | static_cast<quint32>(QByteArray(Alphabet).indexOf(C.toLatin1()));
    Bits += 5;
    if (Bits >= 8)
    {
      Result.append(static_cast<char>((Buffer >> (Bits - 8)) & 0xFF));
      Bits -= 8;
    }
  }
  return (Result.size() == Bytes) ? Result : QByteArray();
}

QByteArray Info(const char * Prefix, const QString & TeamUuid, qint64 KeyVersion)
{
  return QByteArray(Prefix) + TeamUuid.toLower().toUtf8() + '|' + QByteArray::number(KeyVersion);
}

using PKey = std::unique_ptr<EVP_PKEY, void (*)(EVP_PKEY *)>;

QByteArray X25519(const QByteArray & Private, const QByteArray & PeerPublic)
{
  PKey Mine(EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, reinterpret_cast<const unsigned char *>(Private.constData()),
    static_cast<size_t>(Private.size())), EVP_PKEY_free);
  PKey Peer(EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, reinterpret_cast<const unsigned char *>(PeerPublic.constData()),
    static_cast<size_t>(PeerPublic.size())), EVP_PKEY_free);
  if (!Mine || !Peer)
  {
    return QByteArray();
  }
  std::unique_ptr<EVP_PKEY_CTX, void (*)(EVP_PKEY_CTX *)> Ctx(EVP_PKEY_CTX_new(Mine.get(), nullptr), EVP_PKEY_CTX_free);
  size_t Length = 32;
  QByteArray Shared(32, '\0');
  if (!Ctx || (EVP_PKEY_derive_init(Ctx.get()) != 1) || (EVP_PKEY_derive_set_peer(Ctx.get(), Peer.get()) != 1) ||
      (EVP_PKEY_derive(Ctx.get(), reinterpret_cast<unsigned char *>(Shared.data()), &Length) != 1) || (Length != 32))
  {
    return QByteArray();
  }
  return Shared;
}

}

QByteArray HkdfSha256(const QByteArray & Ikm, const QByteArray & Salt, const QByteArray & InfoBytes, int Length)
{
  EVP_KDF * Kdf = EVP_KDF_fetch(nullptr, "HKDF", nullptr);
  if (Kdf == nullptr)
  {
    return QByteArray();
  }
  EVP_KDF_CTX * Ctx = EVP_KDF_CTX_new(Kdf);
  EVP_KDF_free(Kdf);
  if (Ctx == nullptr)
  {
    return QByteArray();
  }
  char Digest[] = "SHA256";
  OSSL_PARAM Params[5];
  int N = 0;
  Params[N++] = OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST, Digest, 0);
  Params[N++] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_KEY, const_cast<char *>(Ikm.constData()), static_cast<size_t>(Ikm.size()));
  if (!Salt.isEmpty())
  {
    Params[N++] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT, const_cast<char *>(Salt.constData()), static_cast<size_t>(Salt.size()));
  }
  Params[N++] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_INFO, const_cast<char *>(InfoBytes.constData()), static_cast<size_t>(InfoBytes.size()));
  Params[N] = OSSL_PARAM_construct_end();
  QByteArray Result(Length, '\0');
  bool Ok = (EVP_KDF_derive(Ctx, reinterpret_cast<unsigned char *>(Result.data()), static_cast<size_t>(Length), Params) == 1);
  EVP_KDF_CTX_free(Ctx);
  return Ok ? Result : QByteArray();
}

KeyPair NewKeyPair()
{
  KeyPair Result;
  EVP_PKEY * Key = EVP_PKEY_Q_keygen(nullptr, nullptr, "X25519");
  if (Key == nullptr)
  {
    return Result;
  }
  size_t Length = 32;
  Result.Private = QByteArray(32, '\0');
  Result.Public = QByteArray(32, '\0');
  if ((EVP_PKEY_get_raw_private_key(Key, reinterpret_cast<unsigned char *>(Result.Private.data()), &Length) != 1) ||
      (EVP_PKEY_get_raw_public_key(Key, reinterpret_cast<unsigned char *>(Result.Public.data()), &Length) != 1))
  {
    Result = KeyPair();
  }
  EVP_PKEY_free(Key);
  return Result;
}

QByteArray PublicKey(const QByteArray & Private)
{
  PKey Key(EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, reinterpret_cast<const unsigned char *>(Private.constData()),
    static_cast<size_t>(Private.size())), EVP_PKEY_free);
  QByteArray Result(32, '\0');
  size_t Length = 32;
  if (!Key || (EVP_PKEY_get_raw_public_key(Key.get(), reinterpret_cast<unsigned char *>(Result.data()), &Length) != 1))
  {
    return QByteArray();
  }
  return Result;
}

bool IsUsableBoxKey(const QByteArray & Public)
{
  // X25519 fails on the low-order points whatever the private key is
  static const KeyPair Probe = NewKeyPair();
  return !X25519(Probe.Private, Public).isEmpty();
}

QByteArray WrapPrivateKey(const QByteArray & Dek, const QByteArray & Blob)
{
  return CloudCrypto::Seal(Dek, Blob, QByteArray());
}

QByteArray UnwrapPrivateKey(const QByteArray & Dek, const QByteArray & Wrapped)
{
  QByteArray Blob;
  return (CloudCrypto::Open(Dek, Wrapped, QByteArray(), Blob) && (Blob.size() == 64)) ? Blob : QByteArray();
}

QByteArray KeyBlob(const KeyPair & Box, const SignKey & Identity)
{
  return Box.Private + Identity.Seed;
}

bool SplitKeyBlob(const QByteArray & Blob, KeyPair & Box, SignKey & Identity)
{
  if (Blob.size() != 64)
  {
    return false;
  }
  Box.Private = Blob.left(32);
  Box.Public = PublicKey(Box.Private);
  Identity = SignKeyFromSeed(Blob.mid(32));
  return !Box.Public.isEmpty() && Identity.IsValid();
}

SignKey NewSignKey()
{
  return SignKeyFromSeed(CloudCrypto::RandomBytes(32));
}

SignKey SignKeyFromSeed(const QByteArray & Seed)
{
  SignKey Result;
  if (Seed.size() != 32)
  {
    return Result;
  }
  PKey Key(EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, reinterpret_cast<const unsigned char *>(Seed.constData()),
    static_cast<size_t>(Seed.size())), EVP_PKEY_free);
  QByteArray Public(32, '\0');
  size_t Length = 32;
  if (!Key || (EVP_PKEY_get_raw_public_key(Key.get(), reinterpret_cast<unsigned char *>(Public.data()), &Length) != 1) || (Length != 32))
  {
    return Result;
  }
  Result.Public = Public;
  Result.Seed = Seed;
  return Result;
}

QByteArray Sign(const SignKey & Key, const QByteArray & Message)
{
  PKey Private(EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, reinterpret_cast<const unsigned char *>(Key.Seed.constData()),
    static_cast<size_t>(Key.Seed.size())), EVP_PKEY_free);
  std::unique_ptr<EVP_MD_CTX, void (*)(EVP_MD_CTX *)> Ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  QByteArray Signature(SignatureLength, '\0');
  size_t Length = SignatureLength;
  if (!Private || !Ctx || (EVP_DigestSignInit(Ctx.get(), nullptr, nullptr, nullptr, Private.get()) != 1) ||
      (EVP_DigestSign(Ctx.get(), reinterpret_cast<unsigned char *>(Signature.data()), &Length,
        reinterpret_cast<const unsigned char *>(Message.constData()), static_cast<size_t>(Message.size())) != 1) ||
      (Length != SignatureLength))
  {
    return QByteArray();
  }
  return Signature;
}

bool Verify(const QByteArray & Public, const QByteArray & Message, const QByteArray & Signature)
{
  if ((Public.size() != 32) || (Signature.size() != SignatureLength))
  {
    return false;
  }
  PKey Key(EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, reinterpret_cast<const unsigned char *>(Public.constData()),
    static_cast<size_t>(Public.size())), EVP_PKEY_free);
  std::unique_ptr<EVP_MD_CTX, void (*)(EVP_MD_CTX *)> Ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  return Key && Ctx && (EVP_DigestVerifyInit(Ctx.get(), nullptr, nullptr, nullptr, Key.get()) == 1) &&
    (EVP_DigestVerify(Ctx.get(), reinterpret_cast<const unsigned char *>(Signature.constData()), static_cast<size_t>(Signature.size()),
      reinterpret_cast<const unsigned char *>(Message.constData()), static_cast<size_t>(Message.size())) == 1);
}

QString Fingerprint(const QByteArray & IdentityPublic)
{
  // 120 bits, as 24 digits in groups of four
  QString Digits = Base32(QCryptographicHash::hash(IdentityPublic, QCryptographicHash::Sha256).left(15));
  QStringList Groups;
  for (qsizetype I = 0; I < Digits.size(); I += 4)
  {
    Groups.append(Digits.mid(I, 4));
  }
  return Groups.join(QLatin1Char('-'));
}

QByteArray BoxKeyMessage(const QString & UserUuid, const QByteArray & BoxPublic)
{
  return QByteArrayLiteral("NXT2-BOX-KEY|") + UserUuid.toLower().toUtf8() + '|' + BoxPublic;
}

QByteArray NewTeamKey()
{
  return CloudCrypto::RandomBytes(KeyLength);
}

QByteArray WrapForMember(const QByteArray & MemberPublic, const QByteArray & TeamKey, const QString & TeamUuid, qint64 KeyVersion)
{
  KeyPair Ephemeral = NewKeyPair();
  QByteArray Shared = X25519(Ephemeral.Private, MemberPublic);
  if (Shared.isEmpty())
  {
    return QByteArray();
  }
  QByteArray InfoBytes = Info("NXT1-WRAP|", TeamUuid, KeyVersion);
  QByteArray Key = HkdfSha256(Shared, Ephemeral.Public + MemberPublic, InfoBytes);
  return Ephemeral.Public + CloudCrypto::Seal(Key, TeamKey, InfoBytes);
}

QByteArray UnwrapForMember(const KeyPair & Mine, const QByteArray & Wrap, const QString & TeamUuid, qint64 KeyVersion)
{
  if (Wrap.size() != 32 + 12 + KeyLength + 16)
  {
    return QByteArray();
  }
  QByteArray EphemeralPublic = Wrap.left(32);
  QByteArray Shared = X25519(Mine.Private, EphemeralPublic);
  if (Shared.isEmpty())
  {
    return QByteArray();
  }
  QByteArray InfoBytes = Info("NXT1-WRAP|", TeamUuid, KeyVersion);
  QByteArray Key = HkdfSha256(Shared, EphemeralPublic + Mine.Public, InfoBytes);
  QByteArray TeamKey;
  return (CloudCrypto::Open(Key, Wrap.mid(32), InfoBytes, TeamKey) && (TeamKey.size() == KeyLength)) ? TeamKey : QByteArray();
}

QString Invite::Code() const
{
  return Id + QLatin1Char('-') + Base32(Secret);
}

Invite NewInvite()
{
  Invite Result;
  // 16 characters = 80 random bits
  Result.Id = Base32(CloudCrypto::RandomBytes(10));
  Result.Secret = CloudCrypto::RandomBytes(SecretLength);
  return Result;
}

bool ParseInviteCode(const QString & Text, Invite & Result)
{
  QString T = Text.trimmed();
  qsizetype Dash = T.indexOf(QLatin1Char('-'));
  if (Dash < 0)
  {
    return false;
  }
  QString Id = Normalize(T.left(Dash));
  QString Secret = Normalize(T.mid(Dash + 1).remove(QLatin1Char('-')));
  if ((Id.size() != 16) || (Secret.size() != 32))
  {
    return false;
  }
  Result.Id = Id;
  Result.Secret = FromBase32(Secret, SecretLength);
  return !Result.Secret.isEmpty();
}

QByteArray WrapForInvite(const Invite & Code, const InviteContent & Content, const QString & TeamUuid, qint64 KeyVersion)
{
  QByteArray InfoBytes = Info("NXT2-INVITE|", TeamUuid, KeyVersion);
  QByteArray Key = HkdfSha256(Code.Secret, Code.Id.toLatin1(), InfoBytes);
  QJsonObject Plain{{QStringLiteral("key"), QString::fromLatin1(Content.TeamKey.toBase64())}, {QStringLiteral("name"), Content.TeamName},
    {QStringLiteral("seq"), Content.RosterSeq}, {QStringLiteral("hash"), Content.RosterHash}};
  return CloudCrypto::Seal(Key, QJsonDocument(Plain).toJson(QJsonDocument::Compact), InfoBytes);
}

bool UnwrapForInvite(const Invite & Code, const QByteArray & Wrap, const QString & TeamUuid, qint64 KeyVersion, InviteContent & Content)
{
  QByteArray InfoBytes = Info("NXT2-INVITE|", TeamUuid, KeyVersion);
  QByteArray Key = HkdfSha256(Code.Secret, Code.Id.toLatin1(), InfoBytes);
  QByteArray Plain;
  if (!CloudCrypto::Open(Key, Wrap, InfoBytes, Plain))
  {
    return false;
  }
  QJsonObject Json = QJsonDocument::fromJson(Plain).object();
  Content.TeamKey = QByteArray::fromBase64(Json.value(QStringLiteral("key")).toString().toLatin1());
  Content.TeamName = Json.value(QStringLiteral("name")).toString();
  Content.RosterSeq = Json.value(QStringLiteral("seq")).toInteger();
  Content.RosterHash = Json.value(QStringLiteral("hash")).toString();
  return (Content.TeamKey.size() == KeyLength) && (Content.RosterSeq > 0) && (Content.RosterHash.size() == 64);
}

QByteArray InviteVerifier(const Invite & Code)
{
  return HkdfSha256(Code.Secret, QByteArray(), QByteArray("NXT1-INVITE-VERIFY|") + Code.Id.toLatin1());
}

QString InviteVerifierHash(const Invite & Code)
{
  return QString::fromLatin1(QCryptographicHash::hash(InviteVerifier(Code), QCryptographicHash::Sha256).toHex());
}

SignKey InviteSignKey(const Invite & Code)
{
  return SignKeyFromSeed(HkdfSha256(Code.Secret, Code.Id.toLatin1(), QByteArray("NXT2-INVITE-SIGN|") + Code.Id.toLatin1()));
}

//---------------------------------------------------------------------------
// Team snapshots

QByteArray EncodeSnapshot(const Snapshot & Header, const QByteArray & TeamKey, const SignKey & Author, const QByteArray & Plaintext)
{
  QJsonObject Json{
    {QStringLiteral("fmt"), 2},
    {QStringLiteral("team"), Header.Team},
    {QStringLiteral("version"), Header.Version},
    {QStringLiteral("key_version"), Header.KeyVersion},
    {QStringLiteral("author"), Header.Author},
    {QStringLiteral("roster_seq"), Header.RosterSeq},
    {QStringLiteral("prev"), Header.Previous},
    {QStringLiteral("saved_at"), Header.SavedAt.toUTC().toString(Qt::ISODateWithMs)},
  };
  QByteArray Text = QJsonDocument(Json).toJson(QJsonDocument::Compact);
  QByteArray Prefix = SnapshotMagic;
  char Length[4];
  qToLittleEndian<quint32>(static_cast<quint32>(Text.size()), Length);
  Prefix.append(Length, 4);
  Prefix.append(Text);
  QByteArray Signed = Prefix + CloudCrypto::Seal(TeamKey, CloudCrypto::Pack(Plaintext), Prefix);
  return Signed + Sign(Author, SnapshotDomain + Signed);
}

// Splits a snapshot; Prefix = magic || length || header (the additional data)
static bool SplitSnapshot(const QByteArray & Data, QByteArray & Prefix, QByteArray & Sealed)
{
  if ((Data.size() < 8) || !Data.startsWith(SnapshotMagic))
  {
    return false;
  }
  quint32 Length = qFromLittleEndian<quint32>(Data.constData() + 4);
  if ((Length == 0) || (Length > MaxSnapshotHeader) ||
      (Data.size() < 8 + static_cast<qsizetype>(Length) + NonceAndTag + SignatureLength))
  {
    return false;
  }
  Prefix = Data.left(8 + Length);
  Sealed = Data.mid(8 + Length, Data.size() - 8 - Length - SignatureLength);
  return true;
}

bool ReadSnapshot(const QByteArray & Data, Snapshot & Header, QString * Error)
{
  QByteArray Prefix, Sealed;
  QJsonObject Json;
  if (SplitSnapshot(Data, Prefix, Sealed))
  {
    Json = QJsonDocument::fromJson(Prefix.mid(8)).object();
  }
  if (Json.value(QStringLiteral("fmt")).toInt() != 2)
  {
    if (Error != nullptr)
    {
      *Error = QStringLiteral("This is not a team snapshot of this version of NixSCP.");
    }
    return false;
  }
  Header = Snapshot();
  Header.Team = Json.value(QStringLiteral("team")).toString();
  Header.Version = Json.value(QStringLiteral("version")).toInteger();
  Header.KeyVersion = Json.value(QStringLiteral("key_version")).toInteger();
  Header.Author = Json.value(QStringLiteral("author")).toString();
  Header.RosterSeq = Json.value(QStringLiteral("roster_seq")).toInteger();
  Header.Previous = Json.value(QStringLiteral("prev")).toString();
  Header.SavedAt = QDateTime::fromString(Json.value(QStringLiteral("saved_at")).toString(), Qt::ISODateWithMs).toUTC();
  return true;
}

bool VerifySnapshot(const QByteArray & Data, const QByteArray & AuthorPublic)
{
  if (Data.size() <= SignatureLength)
  {
    return false;
  }
  return Verify(AuthorPublic, SnapshotDomain + Data.left(Data.size() - SignatureLength), Data.right(SignatureLength));
}

bool DecodeSnapshot(const QByteArray & Data, const QByteArray & TeamKey, QByteArray & Plaintext, QString * Error)
{
  auto Fail = [Error](const QString & Message)
  {
    if (Error != nullptr)
    {
      *Error = Message;
    }
    return false;
  };
  QByteArray Prefix, Sealed, Packed;
  if (!SplitSnapshot(Data, Prefix, Sealed))
  {
    return Fail(QStringLiteral("The team snapshot is damaged."));
  }
  if (!CloudCrypto::Open(TeamKey, Sealed, Prefix, Packed))
  {
    return Fail(QStringLiteral("The team snapshot cannot be decrypted with the team key."));
  }
  if (!CloudCrypto::Unpack(Packed, Plaintext))
  {
    Plaintext.clear();
    return Fail(QStringLiteral("The team snapshot is damaged or too large."));
  }
  return true;
}

}
