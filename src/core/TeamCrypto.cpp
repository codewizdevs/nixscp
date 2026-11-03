#include "core/TeamCrypto.h"
#include "core/CloudCrypto.h"

#include <QCryptographicHash>

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

QByteArray WrapPrivateKey(const QByteArray & Dek, const QByteArray & Private)
{
  return CloudCrypto::Seal(Dek, Private, QByteArray());
}

QByteArray UnwrapPrivateKey(const QByteArray & Dek, const QByteArray & Wrapped)
{
  QByteArray Private;
  return (CloudCrypto::Open(Dek, Wrapped, QByteArray(), Private) && (Private.size() == 32)) ? Private : QByteArray();
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

QByteArray WrapForInvite(const Invite & Code, const QByteArray & TeamKey, const QString & TeamUuid, qint64 KeyVersion)
{
  QByteArray InfoBytes = Info("NXT1-INVITE|", TeamUuid, KeyVersion);
  QByteArray Key = HkdfSha256(Code.Secret, Code.Id.toLatin1(), InfoBytes);
  return CloudCrypto::Seal(Key, TeamKey, InfoBytes);
}

QByteArray UnwrapForInvite(const Invite & Code, const QByteArray & Wrap, const QString & TeamUuid, qint64 KeyVersion)
{
  QByteArray InfoBytes = Info("NXT1-INVITE|", TeamUuid, KeyVersion);
  QByteArray Key = HkdfSha256(Code.Secret, Code.Id.toLatin1(), InfoBytes);
  QByteArray TeamKey;
  return (CloudCrypto::Open(Key, Wrap, InfoBytes, TeamKey) && (TeamKey.size() == KeyLength)) ? TeamKey : QByteArray();
}

QByteArray InviteVerifier(const Invite & Code)
{
  return HkdfSha256(Code.Secret, QByteArray(), QByteArray("NXT1-INVITE-VERIFY|") + Code.Id.toLatin1());
}

QString InviteVerifierHash(const Invite & Code)
{
  return QString::fromLatin1(QCryptographicHash::hash(InviteVerifier(Code), QCryptographicHash::Sha256).toHex());
}

}
